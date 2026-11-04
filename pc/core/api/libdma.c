/*
 * libdma, the EE's DMA library (its behaviour at its interface). The
 * channels are the graphics front's (pc/core/gfx): sceDmaGetChan hands out a
 * pointer to a channel's registers, which the game writes (CHCR's TTE and TIE)
 * before sceDmaSend.
 *
 * Observed: sceDmaSend and sceDmaRecvN wait for STR, read TADR (MADR) before
 * writing it, write QWC, then CHCR with MOD and DIR set (chain, from memory;
 * normal, to memory) and its other bits as read; a scratchpad address becomes
 * SPR | its offset; they return CHCR. sceDmaSync's mode 1 says whether STR is
 * set, any other mode waits. sceDmaReset clears the channels' registers but the
 * SIF ones, D_STAT's status and masks (SIF's masks kept), D_CTRL's settings but
 * DMAE, D_PCR, D_SQWC, D_RBOR and D_RBSR, then sets DMAE if asked; it returns
 * the old DMAE.
 */
#include "common.h"
#include "dma.h"
#include "pal.h"
#include "gfx/gfx.h"
#include "kernel/kern.h"

#define CHCR 0x00u
#define MADR 0x10u
#define QWC 0x20u
#define TADR 0x30u
#define ASR0 0x40u
#define ASR1 0x50u
#define SADR 0x80u
#define CHCR_STR 0x100u
#define D_CTRL 0x1000E000u
#define D_STAT 0x1000E010u
#define D_PCR 0x1000E020u
#define D_SQWC 0x1000E030u
#define D_RBSR 0x1000E040u
#define D_RBOR 0x1000E050u

static const u32 sChanAddr[10] = {0x10008000u, 0x10009000u, 0x1000A000u, 0x1000B000u, 0x1000B400u,
                                  0x1000C000u, 0x1000C400u, 0x1000C800u, 0x1000D000u, 0x1000D400u};

extern u32 gGfxDmaRegs[10][0x40];
void Gfx_Oddity(const char *tag, u32 key);

/* The register block's PS2 address for a channel pointer the game holds. */
static u32 ChanAddr(DmaChan *chan)
{
    return Gfx_DmaChanPs2Addr((u32)chan);
}

/* An address for MADR or TADR: a scratchpad one as SPR | its offset. */
static u32 DmaAddr(u32 a)
{
    if (DMA_ADDR_IS_SPR(a))
        return (a - PAL_SPR(0)) | 0x80000000u;
    return a;
}

/* Reads CHCR until STR is clear. In the core a channel is busy only while a
 * GIF transfer waits for PATH3's mask; libdma's own loops are no `wait` of
 * polls.txt (the original game never finds them busy), so a turn here is counted
 * as an anomaly and taken as a library wait's step. */
static u32 WaitIdle(u32 chan)
{
    u32 v = Pal_EeRegRead32(chan + CHCR);
    if (!(v & CHCR_STR))
        return v;
    Gfx_Oddity("libdma", chan);
    Kern_LibWaitArrive();
    for (;;) {
        v = Pal_EeRegRead32(chan + CHCR);
        if (!(v & CHCR_STR))
            return v;
        Kern_LibWaitStep();
    }
}


static DmaChan * sceDmaGetChanWork(s32 id)
{
    if (id < 0 || id > 9) {
        Gfx_Oddity("sceDmaGetChan", (u32)id);
        return NULL;
    }
    return (DmaChan *)gGfxDmaRegs[id];
}

DmaChan * sceDmaGetChan(s32 id)
{
    return sceDmaGetChanWork(id);
}

static s32 sceDmaResetWork(s32 enable)
{
    u32 old = Pal_EeRegRead32(D_CTRL), st, ctrl;
    int ch;
    for (ch = 0; ch < 10; ch++) {
        if (ch < 5 || ch > 7) {
            u32 c = sChanAddr[ch];
            Pal_EeRegWrite32(c + SADR, 0);
            Pal_EeRegWrite32(c + CHCR, 0);
            Pal_EeRegWrite32(c + TADR, 0);
            Pal_EeRegWrite32(c + MADR, 0);
            Pal_EeRegWrite32(c + ASR1, 0);
            Pal_EeRegWrite32(c + ASR0, 0);
        }
    }
    Pal_EeRegWrite32(D_STAT, 0xFF1Fu);
    st = Pal_EeRegRead32(D_STAT);
    Pal_EeRegWrite32(D_STAT, st & 0xFF1F0000u);
    ctrl = Pal_EeRegRead32(D_CTRL);
    Pal_EeRegWrite32(D_CTRL, ctrl & ~0xFEu);
    Pal_EeRegWrite32(D_PCR, 0);
    Pal_EeRegWrite32(D_SQWC, 0);
    Pal_EeRegWrite32(D_RBOR, 0);
    Pal_EeRegWrite32(D_RBSR, 0);
    if (enable) {
        ctrl = Pal_EeRegRead32(D_CTRL);
        Pal_EeRegWrite32(D_CTRL, ctrl | 1u);
    }
    return (s32)(old & 1u);
}

s32 sceDmaReset(s32 enable)
{
    s32 r = sceDmaResetWork(enable);
    return r;
}

static s32 sceDmaSendWork(DmaChan *chanp, void *tag)
{
    u32 chan = ChanAddr(chanp), c;
    WaitIdle(chan);
    Pal_EeRegWrite32(chan + TADR, DmaAddr((u32)tag));
    c = Pal_EeRegRead32(chan + CHCR);
    Pal_EeRegWrite32(chan + QWC, 0);
    c = (c & ~0x0Cu) | 0x105u;
    Pal_EeRegWrite32(chan + CHCR, c);
    return (s32)c;
}

s32 sceDmaSend(DmaChan *chanp, void *tag)
{
    return sceDmaSendWork(chanp, tag);
}

static s32 sceDmaRecvNWork(DmaChan *chanp, void *addr, s32 qwc)
{
    u32 chan = ChanAddr(chanp), c;
    WaitIdle(chan);
    Pal_EeRegWrite32(chan + MADR, DmaAddr((u32)addr));
    c = Pal_EeRegRead32(chan + CHCR);
    Pal_EeRegWrite32(chan + QWC, (u32)qwc);
    c = (c & ~0x0Du) | 0x100u;
    Pal_EeRegWrite32(chan + CHCR, c);
    return (s32)c;
}

s32 sceDmaRecvN(DmaChan *chanp, void *addr, s32 qwc)
{
    return sceDmaRecvNWork(chanp, addr, qwc);
}

/* polls.txt: `ret main 00103890 sceDmaSync v0 != 0 && a1 == 1`: the game's
 * busy answer in mode 1 is a "not ready". */
static s32 sceDmaSyncWork(DmaChan *chanp, s32 mode, s32 timeout)
{
    u32 chan = ChanAddr(chanp);
    if (mode == 1) {
        if (Pal_EeRegRead32(chan + CHCR) & CHCR_STR) {
            Pal_Poll();
            return 1;
        }
        return 0;
    }
    if (timeout)
        Gfx_Oddity("sceDmaSync", (u32)timeout);
    WaitIdle(chan);
    return 0;
}

s32 sceDmaSync(DmaChan *chanp, s32 mode, s32 timeout)
{
    return sceDmaSyncWork(chanp, mode, timeout);
}
