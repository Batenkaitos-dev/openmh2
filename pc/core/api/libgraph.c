/*
 * libgraph, the GS library (its behaviour at its interface), over the
 * graphics front (pc/core/gfx) and the kernel's calls.
 *
 * Its state, as observed in the library's memory: the video mode as
 * sceGsResetGraph got it, the GS's revision, and the VBlank callback with its
 * AddIntcHandler id. sceGsSyncV reads the interlace and the callback;
 * sceGsSyncVCallback and sceGsResetGraph the callback and the id.
 *
 * The busy waits (the polled-wait table's `wait`: sceGsSyncPath, sceGsExecLoadImage and the
 * kernel library's VBlank waits under sceGsSyncV) are library waits
 * (the frame clock's): reaching a loop is an arrival, each turn a
 * step, and sceGsSyncPath's entry is an arrival whenever it is called.
 */
#include "common.h"
#include "dma.h"
#include "pal.h"
#include "gfx/gfx.h"
#include "kernel/kern.h"

s32 AddIntcHandler(s32 cause, s32 (*handler)(s32), s32 next);
s32 RemoveIntcHandler(s32 cause, s32 id);
s32 EnableIntc(s32 cause);
void DisableIntc(s32 cause);
void Gfx_Oddity(const char *tag, u32 key);

#define GS_CSR 0x12001000u
#define GIF_CTRL 0x10003000u
#define GIF_STAT 0x10003020u
#define VIF1_STAT 0x10003C00u
#define VIF1_FBRST 0x10003C10u
#define VIF1_ERR 0x10003C20u
#define VIF1_FIFO 0x10005000u
#define D1_CHCR 0x10009000u
#define D2_CHCR 0x1000A000u
#define D2_MADR 0x1000A010u
#define D2_QWC 0x1000A020u
#define CHCR_STR 0x100u
#define INTC_VBLANK_START 2
#define I_STAT 0x1000F000u

static struct {
    s16 interlace, omode; /* sceGsResetGraph's video mode, as passed */
    s16 ffmode;           /* 1 if its ffmode was nonzero */
    u16 rev;              /* the GS's revision (CSR bits 16-23) at the reset */
    void (*callback)(void);
    s32 handler;          /* its AddIntcHandler id */
} sGs;

/* A loop of a `wait` record: reads reg until (value & mask) == 0. */
static u32 WaitClear(u32 reg, u32 mask)
{
    u32 v = Pal_EeRegRead32(reg);
    if (!(v & mask))
        return v;
    Kern_LibWaitArrive();
    for (;;) {
        v = Pal_EeRegRead32(reg);
        if (!(v & mask))
            return v;
        Kern_LibWaitStep();
    }
}

static void WriteQword(u32 reg, u64 lo, u64 hi)
{
    u64 q[2] __attribute__((aligned(16)));
    q[0] = lo;
    q[1] = hi;
    Pal_EeRegWrite128(reg, q);
}

static void RemoveCallback(void)
{
    DisableIntc(INTC_VBLANK_START);
    RemoveIntcHandler(INTC_VBLANK_START, sGs.handler);
    sGs.callback = NULL;
    sGs.handler = 0;
}

static void NoteMode(s16 interlace, s16 omode, s16 ffmode)
{
    sGs.interlace = interlace;
    sGs.omode = omode;
    sGs.ffmode = ffmode != 0;
    sGs.rev = (u8)(Pal_GsRegRead64(GS_CSR) >> 16);
}

/* The kernel library's VBlank-start wait (0x113FB0, the polled-wait table's `wait` with
 * its loop at 0x113FF0): with
 * the context's interrupts off (the library's DIntr), INTC's VBLANK_START bit
 * cleared, then polled until the VBlank start posts it, and the interrupts
 * back on (the library's EIntr: no delivery point). The bit was just cleared,
 * so the loop is always reached: an arrival, then a step per turn. */
static void WaitVBlankStartBit(void)
{
    Kern_LibDIntr();
    Kern_IntcWrite(I_STAT, 1u << INTC_VBLANK_START);
    Kern_LibWaitArrive();
    while (!(Kern_IntcRead(I_STAT) & (1u << INTC_VBLANK_START)))
        Kern_LibWaitStep();
    Kern_LibEIntr();
}

/* The kernel library's VSync wait (0x114040, its loop at 0x114090), as the
 * reference shows it (2026-10-27, the boot route: its I_STAT accesses and the
 * calls inside it): the syscall SetVSyncFlag on its own slots; with the
 * interrupts off (DIntr), INTC's VBLANK_START bit cleared, then EIntr; the
 * loop runs with the interrupts on (the game's VBlank callback runs inside
 * the wait), reading I_STAT at each turn, and ends when the VBlank start has
 * set the flag (the bit itself is cleared again by then: the last read sees
 * it clear); then DIntr, the bit cleared again, EIntr. It returns the CSR
 * slot's word, which that VBlank start stored. */
static u64 WaitVSync(void)
{
    Gfx_SetVSyncFlag();
    Kern_LibDIntr();
    Kern_IntcWrite(I_STAT, 1u << INTC_VBLANK_START);
    Kern_LibEIntr();
    Kern_LibWaitArrive();
    while (!(Kern_IntcRead(I_STAT) & (1u << INTC_VBLANK_START)) && !Gfx_VSyncFlag())
        Kern_LibWaitStep();
    Kern_LibDIntr();
    Kern_IntcWrite(I_STAT, 1u << INTC_VBLANK_START);
    Kern_LibEIntr();
    return Gfx_VSyncSlot();
}

/* Mode 0 (the game's): resets the GS (CSR.RESET), notes the video mode and the
 * revision, GsPutIMR(0xFF00), removes a VBlank callback, and SetGsCrt(interlace
 * & 1, omode & 0xFF, ffmode & 1). Mode 1 flushes (CSR.FLUSH); mode 5 notes the
 * mode and sets it; the others do nothing. The game passes 0 only. */
static void sceGsResetGraphWork(s16 mode, s16 interlace, s16 omode, s16 ffmode)
{
    if (mode != 0)
        Gfx_Oddity("sceGsResetGraph", (u32)mode);
    switch (mode) {
    case 0:
        Pal_GsRegWrite64(GS_CSR, 0x200u);
        NoteMode(interlace, omode, ffmode);
        Gfx_GsPutImr(0xFF00u);
        if (sGs.callback)
            RemoveCallback();
        break;
    case 1:
        Pal_GsRegWrite64(GS_CSR, 0x100u);
        return;
    case 5:
        NoteMode(interlace, omode, ffmode);
        break;
    default:
        return;
    }
    Gfx_SetGsCrt((u32)(interlace & 1), (u32)(omode & 0xFF), (u32)(ffmode & 1));
}

void sceGsResetGraph(s16 mode, s16 interlace, s16 omode, s16 ffmode)
{
    sceGsResetGraphWork(mode, interlace, omode, ffmode);
}

/* Resets VIF1 (FBRST.RST, ERR.ME0) and VU1 (COP2's FBRST: the original's VU0
 * takes the write as nothing, CTC2), sends VIF1 its initial settings
 * (STCYCL 4/4, STMASK 0, STMOD 0, MSKPATH3 0, BASE 0, OFFSET 0, ITOP 0) and
 * resets the GIF. Returns what the original game left in v0. */
static s32 sceGsResetPathWork(void)
{
    Pal_EeRegWrite32(VIF1_FBRST, 1);
    Pal_EeRegWrite32(VIF1_ERR, 2);
    WriteQword(VIF1_FIFO, 0x2000000001000404ull, 0x0500000000000000ull);
    WriteQword(VIF1_FIFO, 0x0300000006000000ull, 0x0400000002000000ull);
    Pal_EeRegWrite32(GIF_CTRL, 1);
    return 0x06000000;
}

s32 sceGsResetPath(void)
{
    return sceGsResetPathWork();
}

/* Waits for the next VBlank; returns the field it starts (CSR's FIELD) when
 * sceGsResetGraph's interlace was 1, else 1. With a callback set the kernel
 * library's VSync wait does it and gives the CSR word (Gfx_GsCsrForVSyncFlag);
 * without one, its poll of INTC's VBLANK_START, then CSR is read. */
static s32 sceGsSyncVWork(s32 mode)
{
    (void)mode;
    if (sGs.callback) {
        u64 csr = WaitVSync();
        return sGs.interlace == 1 ? (s32)((csr >> 13) & 1u) : 1;
    }
    WaitVBlankStartBit();
    if (sGs.interlace != 1)
        return 1;
    return (s32)((Pal_GsRegRead64(GS_CSR) >> 13) & 1u);
}

s32 sceGsSyncV(s32 mode)
{
    return sceGsSyncVWork(mode);
}

/* Mode 0 (the game's): an arrival at entry, then waits until VIF1's and the
 * GIF's DMA, VIF1 (FQC, VPS), VU1 and the GIF (APATH) are idle; returns 0.
 * Another mode returns which are busy (bits 0-4, in that order). VU1 runs at
 * the request in the core: never busy. */
static s32 sceGsSyncPathWork(s32 mode, u16 timeout)
{
    u32 busy = 0;
    (void)timeout;
    if (mode == 0) {
        Kern_LibWaitArrive();
        WaitClear(D1_CHCR, CHCR_STR);
        WaitClear(D2_CHCR, CHCR_STR);
        WaitClear(VIF1_STAT, 0x1F000003u);
        WaitClear(GIF_STAT, 0xC00u);
        return 0;
    }
    if (Pal_EeRegRead32(D1_CHCR) & CHCR_STR)
        busy |= 1;
    if (Pal_EeRegRead32(D2_CHCR) & CHCR_STR)
        busy |= 2;
    if (Pal_EeRegRead32(VIF1_STAT) & 0x1F000003u)
        busy |= 4;
    if (Pal_EeRegRead32(GIF_STAT) & 0xC00u)
        busy |= 16;
    return (s32)busy;
}

s32 sceGsSyncPath(s32 mode, u16 timeout)
{
    return sceGsSyncPathWork(mode, timeout);
}

/* The image's quadwords as the library computes them: w x h shifted by the
 * format's size (floor), 24-bit formats as w x h x 3 bytes; 0 for a format it
 * doesn't know (4,000 random argument sets fitted). */
static s32 ImageQwc(s32 psm, s32 w, s32 h)
{
    s32 p = w * h;
    switch (psm) {
    case 0x00: case 0x30: return p >> 2;
    case 0x01: case 0x31: return (s32)((u32)p * 3u) >> 4;
    case 0x02: case 0x0A: case 0x32: case 0x3A: return p >> 3;
    case 0x13: case 0x1B: return p >> 4;
    case 0x14: case 0x24: case 0x2C: return p >> 5;
    }
    return 0;
}

/* The packet that loads an image: a GIFtag (A+D, 4 registers), BITBLTBUF,
 * TRXPOS, TRXREG, TRXDIR (host to local), then the IMAGE GIFtag of the data's
 * quadwords; 6 quadwords. An image over 0x7FFF quadwords writes nothing and
 * returns 0. The halfword arguments go into their fields sign-extended. */
static s32 sceGsSetDefLoadImageWork(void *lpv, s16 dbp, s16 dbw, s16 dpsm, s16 x, s16 y, s16 w, s16 h)
{
    u64 *lp = lpv;
    s32 qwc = ImageQwc(dpsm, w, h);
    if (qwc > 0x7FFF)
        return 0;
    lp[0] = 0x1000000000000004ull;
    lp[1] = 0x0Eull;
    lp[2] = ((u64)(s64)dbp << 32) | ((u64)(s64)dbw << 48) | ((u64)(s64)dpsm << 56);
    lp[3] = 0x50ull;
    lp[4] = ((u64)(s64)x << 32) | ((u64)(s64)y << 48);
    lp[5] = 0x51ull;
    lp[6] = (u64)(s64)w | ((u64)(s64)h << 32);
    lp[7] = 0x52ull;
    lp[8] = 0;
    lp[9] = 0x53ull;
    lp[10] = (u64)((u32)qwc & 0x7FFFu) | 0x8000u | (2ull << 58);
    lp[11] = 0;
    return 6;
}

s32 sceGsSetDefLoadImage(void *lpv, s16 dbp, s16 dbw, s16 dpsm, s16 x, s16 y, s16 w, s16 h)
{
    s32 r = sceGsSetDefLoadImageWork(lpv, dbp, dbw, dpsm, x, y, w, h);
    return r;
}

/* MADR for the GIF's channel: a scratchpad address as SPR | its offset. */
static u32 GifAddr(u32 a)
{
    if (DMA_ADDR_IS_SPR(a))
        return (a - PAL_SPR(0)) | 0x80000000u;
    return a & 0x0FFFFFFFu;
}

/* Sends the packet, then the image's quadwords (the IMAGE tag's NLOOP), through
 * the GIF's channel in normal mode, each once the channel is idle; returns 0. */
static s32 sceGsExecLoadImageWork(void *lp, void *data)
{
    WaitClear(D2_CHCR, CHCR_STR);
    Pal_EeRegWrite32(D2_QWC, 6);
    Pal_EeRegWrite32(D2_MADR, GifAddr((u32)lp));
    Pal_EeRegWrite32(D2_CHCR, 0x101u);
    WaitClear(D2_CHCR, CHCR_STR);
    Pal_EeRegWrite32(D2_QWC, (u32)((const u64 *)lp)[10] & 0x7FFFu);
    Pal_EeRegWrite32(D2_MADR, GifAddr((u32)data));
    Pal_EeRegWrite32(D2_CHCR, 0x101u);
    return 0;
}

s32 sceGsExecLoadImage(void *lp, void *data)
{
    return sceGsExecLoadImageWork(lp, data);
}

/* Sets the function the VBlank-start interrupt calls (INTC's VBLANK_START
 * handler, enabled), or with 0 removes it; returns the previous one. */
static void *sceGsSyncVCallbackWork(void (*func)(void), s32 arg)
{
    void (*old)(void) = sGs.callback;
    (void)arg;
    if (old || !func)
        RemoveCallback();
    if (func) {
        sGs.handler = AddIntcHandler(INTC_VBLANK_START, (s32(*)(s32))func, -1);
        EnableIntc(INTC_VBLANK_START);
    }
    sGs.callback = func;
    return (void *)old;
}

void *sceGsSyncVCallback(void (*func)(void), s32 arg)
{
    return sceGsSyncVCallbackWork(func, arg);
}
