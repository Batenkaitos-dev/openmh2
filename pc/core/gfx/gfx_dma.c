/*
 * The EE's register dispatch (include/pal.h: Pal_EeRegRead32, Pal_EeRegWrite32,
 * Pal_EeRegWrite128, Pal_GsRegRead64, Pal_GsRegWrite64) and the DMAC, from
 * hw.c.
 *
 * A DMA runs to completion at the store to its CHCR that starts it (or, for
 * the GIF's while PATH3 is masked, when the mask is lifted): VIF0 and VIF1
 * data go to gfx_vif.c, GIF data to gfx_gif.c, the scratchpad's channels copy.
 * The chain walker keeps CHCR's TAG field, MADR, TADR and ASR exact: the
 * game's DMA-end handlers read them. Data is handed on in pieces of at most
 * 64 quadwords, as the original game does: the GS stream's records follow them.
 *
 * Addresses: a DMA address on PC is the block's or the image's address
 * (below 0x10000000), or a scratchpad offset with
 * the SPR bit (bit 31; include/dma.h's DMA_TAG_ADDR).
 *
 * The channels' registers are words at the hardware's offsets (sceDmaGetChan
 * returns a pointer into them, include/dma.h's DmaChan): CHCR 0x00, MADR 0x10,
 * QWC 0x20, TADR 0x30, ASR0 0x40, ASR1 0x50, SADR 0x80.
 *
 * The kernel's registers (pc/core/kernel/kern.h): the timers (0x10000000-0x10001FFF,
 * Kern_TimerRead/Write), I_STAT and I_MASK (0x1000F000, 0x1000F010: Kern_IntcRead/Write)
 * and D_STAT (0x1000E010: Kern_DmacStatRead/Write; a finished channel's bit is
 * Kern_RaiseDmac). The SIO's and the SIF's registers (0x1000F100-0x1000F2FF) are
 * reached through the libraries only, never through the dispatch: an access there
 * is an anomaly (the original's SIO TX printed the debug console; its SIF
 * registers were the SIF's).
 *
 * The SPR bit (the n2 review's ruling): a source chain's TADR, or a normal
 * transfer's MADR, with bit 31 set is read from RAM at the offset, as the
 * reference does (its kseg0 mapping), and counted as an anomaly ("dma-spr").
 */
#include "gfx_rt.h"
#include "pal.h"

enum { DMAC_VIF0, DMAC_VIF1, DMAC_GIF, DMAC_FROM_IPU, DMAC_TO_IPU, DMAC_SIF0, DMAC_SIF1, DMAC_SIF2,
       DMAC_FROM_SPR, DMAC_TO_SPR };

#define CHCR 0
#define MADR 4
#define QWC 8
#define TADR 12
#define ASR0 16
#define ASR1 20
#define SADR 32

u32 gGfxDmaRegs[10][0x40] __attribute__((aligned(16)));
static const u32 sDmaBase[10] = {0x10008000, 0x10009000, 0x1000A000, 0x1000B000, 0x1000B400,
                                 0x1000C000, 0x1000C400, 0x1000C800, 0x1000D000, 0x1000D400};

static u32 sDmacCtrl, sDmacPcr, sDmacSqwc, sDmacRbsr, sDmacRbor, sDmacStadr;
static u32 sDmacEnable;
static u32 sMisc[0x10000 / 4]; /* plain storage for the registers without behaviour */
static int sGifDmaWaiting;

#define SPR_SIZE 0x4000u

static const u8 *Mem(u32 addr)
{
    return (const u8 *)addr;
}

static void RaiseDmac(int ch)
{
    Kern_RaiseDmac(ch);
}

/* An address with the SPR bit where the DMAC reads memory (the ruling above). */
static u32 SprBit(int ch, u32 addr)
{
    if (addr & 0x80000000u)
        Gfx_Oddity("dma-spr", (u32)ch);
    return addr;
}

/* ---- the scratchpad's channels ---- */

static void DmaSpr(int ch)
{
    u32 *c = gGfxDmaRegs[ch];
    u32 mod = (c[CHCR] >> 2) & 3, i;
    if (mod == 1 && ch == DMAC_TO_SPR) { /* source chain */
        int guard;
        for (guard = 0; guard < 0x10000; guard++) {
            u64 tag;
            u32 qwc, id, addr, src;
            int irq;
            memcpy(&tag, Mem(SprBit(ch, c[TADR]) & 0x7FFFFFFFu), 8);
            qwc = (u32)(tag & 0xFFFF);
            id = (u32)(tag >> 28) & 7;
            addr = (u32)(tag >> 32) & 0x7FFFFFF0u;
            src = id == 0 || id == 3 || id == 4 ? addr : c[TADR] + 16; /* refe/ref/refs vs cnt/next/end */
            for (i = 0; i < qwc; i++)
                memcpy(Pal_Spr + ((c[SADR] + i * 16) & (SPR_SIZE - 1)), Mem(src + i * 16), 16);
            c[SADR] = (c[SADR] + qwc * 16) & (SPR_SIZE - 1);
            irq = (int)((tag >> 31) & 1);
            switch (id) {
            case 0: c[TADR] += 16; c[MADR] = addr + qwc * 16; return; /* refe */
            case 1: c[TADR] = src + qwc * 16; break;                   /* cnt */
            case 2: c[TADR] = addr; break;                             /* next */
            case 3: case 4: c[TADR] += 16; break;                      /* ref, refs */
            case 7: c[TADR] = src + qwc * 16; return;                  /* end */
            default:
                rt_log_once("dma", 0x5000 + id, "toSPR chain tag id unsupported");
                return;
            }
            if (irq && (c[CHCR] & 0x80))
                return;
        }
        return;
    }
    if (mod != 0)
        rt_log_once("dma", 0x5100 + ch, "SPR DMA mode not supported");
    c[MADR] = SprBit(ch, c[MADR]) & 0x7FFFFFFFu; /* the original's kseg0: RAM at the offset */
    for (i = 0; i < c[QWC]; i++) {
        u8 *spr = Pal_Spr + ((c[SADR] + i * 16) & (SPR_SIZE - 1));
        if (ch == DMAC_TO_SPR)
            memcpy(spr, Mem(c[MADR] + i * 16), 16);
        else
            memcpy((u8 *)Mem(c[MADR] + i * 16), spr, 16);
    }
    c[MADR] += c[QWC] * 16;
    c[SADR] = (c[SADR] + c[QWC] * 16) & (SPR_SIZE - 1);
    c[QWC] = 0;
}

/* ---- the peripherals' channels ---- */

/* `qwc` quadwords at `addr` (memory, or the scratchpad if `spr`) to the
 * channel's peripheral, 64 at a time (hw.c dma_sink). */
static void DmaSink(int ch, u32 addr, u32 qwc, int spr)
{
    GPR buf[64];
    while (qwc) {
        u32 n = qwc < 64 ? qwc : 64, i;
        const void *p;
        if (spr) {
            for (i = 0; i < n; i++)
                memcpy(&buf[i], Pal_Spr + ((addr + i * 16) & (SPR_SIZE - 1)), 16);
            p = buf;
        } else {
            p = Mem(addr & 0x7FFFFFF0u);
        }
        switch (ch) {
        case DMAC_VIF0: vif_write(0, p, n); break;
        case DMAC_VIF1: vif_write(1, p, n); break;
        case DMAC_GIF: gif_path_write(3, p, n); break;
        default:
            rt_log_once("dma", 0x5400 + ch, "DMA data not processed");
            break;
        }
        addr += n * 16;
        qwc -= n;
    }
}

static void DmaTagSink(int ch, u64 tagHi)
{
    if (ch == DMAC_VIF0 || ch == DMAC_VIF1)
        vif_write_tag(ch, tagHi);
    else
        rt_log_once("dma", 0x5500 + ch, "DMA with TTE not supported");
}

/* VIF1 in the GS-to-EE direction (hw.c dma_from_gs). */
static void DmaFromGs(u32 *c)
{
    GPR buf[64];
    if ((c[CHCR] >> 2) & 3)
        rt_log_once("dma", 0x5600, "VIF1 GS->EE DMA in chain mode not supported");
    while (c[QWC]) {
        u32 n = c[QWC] < 64 ? c[QWC] : 64;
        int real = gs_read_fifo(buf, n);
        memcpy((u8 *)Mem(c[MADR]), buf, n * 16);
        if (real)
            ;
        c[MADR] += n * 16;
        c[QWC] -= n;
    }
}

/* Source-chain mode (VIF0, VIF1, GIF, toIPU): the tags from TADR (hw.c
 * dma_source_chain). */
static void DmaSourceChain(int ch)
{
    u32 *c = gGfxDmaRegs[ch];
    int tte = (c[CHCR] >> 6) & 1, tie = (c[CHCR] >> 7) & 1, guard;
    u32 asp = (c[CHCR] >> 4) & 3;
    if (c[QWC]) { /* data left from a previous tag (a resumed transfer) */
        DmaSink(ch, SprBit(ch, c[MADR]), c[QWC], 0);
        c[MADR] += c[QWC] * 16;
        c[QWC] = 0;
    }
    for (guard = 0; guard < 100000; guard++) {
        u32 taddr = SprBit(ch, c[TADR]) & 0x7FFFFFF0u;
        GPR t;
        u64 tag;
        u32 qwc, id, irq, addr;
        int spr, end = 0;
        memcpy(&t, Mem(taddr), 16);
        tag = t.d[0];
        qwc = (u32)(tag & 0xFFFF);
        id = (u32)(tag >> 28) & 7;
        irq = (u32)(tag >> 31) & 1;
        addr = (u32)(tag >> 32) & 0x7FFFFFF0u;
        spr = (int)((tag >> 63) & 1);
        c[CHCR] = (c[CHCR] & 0xFFFFu) | ((u32)tag & 0xFFFF0000u); /* the TAG field */
        if (tte)
            DmaTagSink(ch, t.d[1]);
        switch (id) {
        case 0: /* refe */
            c[MADR] = addr;
            DmaSink(ch, addr, qwc, spr);
            c[MADR] = addr + qwc * 16;
            c[TADR] = taddr + 16;
            end = 1;
            break;
        case 1: /* cnt */
            c[MADR] = taddr + 16;
            DmaSink(ch, c[MADR], qwc, 0);
            c[MADR] += qwc * 16;
            c[TADR] = c[MADR];
            break;
        case 2: /* next */
            c[MADR] = taddr + 16;
            DmaSink(ch, c[MADR], qwc, 0);
            c[MADR] += qwc * 16;
            c[TADR] = addr;
            break;
        case 3: case 4: /* ref, refs */
            c[MADR] = addr;
            DmaSink(ch, addr, qwc, spr);
            c[MADR] = addr + qwc * 16;
            c[TADR] = taddr + 16;
            break;
        case 5: /* call */
            c[MADR] = taddr + 16;
            DmaSink(ch, c[MADR], qwc, 0);
            c[MADR] += qwc * 16;
            if (asp < 2) {
                c[asp == 0 ? ASR0 : ASR1] = c[MADR];
                asp++;
            } else {
                rt_log_once("dma", 0x5200 + ch, "DMA call stack overflow");
                end = 1;
            }
            c[TADR] = addr;
            break;
        case 6: /* ret */
            c[MADR] = taddr + 16;
            DmaSink(ch, c[MADR], qwc, 0);
            c[MADR] += qwc * 16;
            if (asp > 0) {
                asp--;
                c[TADR] = asp == 0 ? c[ASR0] : c[ASR1];
            } else {
                end = 1;
            }
            break;
        case 7: /* end */
            c[MADR] = taddr + 16;
            DmaSink(ch, c[MADR], qwc, 0);
            c[MADR] += qwc * 16;
            c[TADR] = taddr; /* stays on the end tag */
            end = 1;
            break;
        }
        c[CHCR] = (c[CHCR] & ~0x30u) | (asp << 4);
        if (end || (irq && tie))
            return;
    }
    rt_log_once("dma", 0x5300 + ch, "DMA chain did not terminate");
}

static void DmaRun(int ch)
{
    u32 *c = gGfxDmaRegs[ch];
    u32 mod = (c[CHCR] >> 2) & 3;
    switch (ch) {
    case DMAC_FROM_SPR:
    case DMAC_TO_SPR:
        DmaSpr(ch);
        break;
    case DMAC_SIF0: case DMAC_SIF1: case DMAC_SIF2:
        break; /* the SIF's syscalls drive them */
    default:
        if (ch == DMAC_VIF1 && !(c[CHCR] & 1)) {
            DmaFromGs(c);
        } else if (mod == 1 && ch != DMAC_FROM_IPU) {
            DmaSourceChain(ch);
        } else {
            DmaSink(ch, SprBit(ch, c[MADR]), c[QWC], 0);
            c[MADR] += c[QWC] * 16;
            c[QWC] = 0;
        }
        break;
    }
    c[CHCR] &= ~0x100u; /* STR off: the transfer is done */
    if (ch != DMAC_SIF0 && ch != DMAC_SIF1 && ch != DMAC_SIF2)
        RaiseDmac(ch);
}

static void DmaStart(int ch)
{
    if (ch == DMAC_GIF && gif_path3_masked()) {
        sGifDmaWaiting = 1; /* it runs when PATH3 is unmasked */
        gif_note_deferred();
        return;
    }
    DmaRun(ch);
}

void hw_gif_dma_resume(void)
{
    if (!sGifDmaWaiting)
        return;
    sGifDmaWaiting = 0;
    if (gGfxDmaRegs[DMAC_GIF][CHCR] & 0x100)
        DmaRun(DMAC_GIF);
}

static u32 *DmaReg(u32 addr, int *ch)
{
    int i;
    for (i = 0; i < 10; i++) {
        if (addr >= sDmaBase[i] && addr < sDmaBase[i] + 0x100) {
            u32 off = (addr - sDmaBase[i]) & 0xF0;
            *ch = i;
            switch (off) {
            case 0x00: case 0x10: case 0x20: case 0x30: case 0x40: case 0x50: case 0x80:
                return &gGfxDmaRegs[i][off / 4];
            }
            return NULL;
        }
    }
    *ch = -1;
    return NULL;
}

u32 Gfx_DmaChanPs2Addr(u32 addr)
{
    u32 lo = (u32)&gGfxDmaRegs[0][0];
    if (addr - lo >= sizeof(gGfxDmaRegs))
        return 0;
    return sDmaBase[(addr - lo) / 0x100] + (addr - lo) % 0x100;
}

/* ---- the dispatch (hw.c hw_read32, hw_write32, hw_write64, hw_write128) ---- */

static int IsIntc(u32 addr)
{
    return addr == 0x1000F000u || addr == 0x1000F010u;
}

/* The SIO's and the SIF's registers: the libraries' (the file's comment). */
static int IsSioSif(u32 addr)
{
    return addr >= 0x1000F100u && addr < 0x1000F300u;
}

u32 Pal_EeRegRead32(u32 addr)
{
    int ch;
    u32 *r;
    if (addr >= 0x12000000)
        return (u32)(gs_read(addr) >> ((addr & 4) * 8));
    if (addr < 0x10002000u)
        return Kern_TimerRead(addr);
    if (IsIntc(addr))
        return Kern_IntcRead(addr);
    if (IsSioSif(addr))
        Gfx_Oddity("hw-sio-sif", addr);
    r = DmaReg(addr, &ch);
    if (ch >= 0)
        return r ? *r : 0;
    if (addr >= 0x10003000 && addr < 0x10003100)
        return gif_read_reg(addr);
    if (addr >= 0x10003800 && addr < 0x10003980)
        return vif_read_reg(0, addr - 0x10003800);
    if (addr >= 0x10003C00 && addr < 0x10003D80)
        return vif_read_reg(1, addr - 0x10003C00);
    switch (addr) {
    case 0x10002000: case 0x10002010: case 0x10002020: return 0; /* IPU_CMD, IPU_CTRL (not busy), IPU_BP */
    case 0x1000E000: return sDmacCtrl;
    case 0x1000E010: return Kern_DmacStatRead();
    case 0x1000E020: return sDmacPcr;
    case 0x1000E030: return sDmacSqwc;
    case 0x1000E040: return sDmacRbsr;
    case 0x1000E050: return sDmacRbor;
    case 0x1000E060: return sDmacStadr;
    case 0x1000F520: return sDmacEnable;
    }
    rt_log_once("hw", addr, "read32 from an unhandled register");
    return sMisc[(addr & 0xFFFF) / 4];
}

void Pal_EeRegWrite32(u32 addr, u32 v)
{
    int ch;
    u32 *r;
    if (addr >= 0x12000000) {
        gs_write(addr, v, 0);
        return;
    }
    if (addr < 0x10002000u) {
        Kern_TimerWrite(addr, v);
        return;
    }
    if (IsIntc(addr)) {
        Kern_IntcWrite(addr, v);
        return;
    }
    if (IsSioSif(addr))
        Gfx_Oddity("hw-sio-sif", addr);
    r = DmaReg(addr, &ch);
    if (ch >= 0) {
        if (!r)
            return;
        *r = v;
        if (r == &gGfxDmaRegs[ch][CHCR] && (v & 0x100))
            DmaStart(ch);
        return;
    }
    if (addr >= 0x10003000 && addr < 0x10003100) {
        gif_write_reg(addr, v);
        return;
    }
    if (addr >= 0x10003800 && addr < 0x10003980) {
        vif_write_reg(0, addr - 0x10003800, v);
        return;
    }
    if (addr >= 0x10003C00 && addr < 0x10003D80) {
        vif_write_reg(1, addr - 0x10003C00, v);
        return;
    }
    switch (addr) {
    case 0x1000E000:
        if ((v ^ sDmacCtrl) & 0xFE)
            rt_log_once("dma", 0x5700 + (v & 0xFE), "D_CTRL: MFIFO and stall control not emulated");
        sDmacCtrl = v;
        return;
    case 0x1000E010:
        Kern_DmacStatWrite(v); /* CIS: a 1 clears; CIM: a 1 toggles */
        return;
    case 0x1000E020: sDmacPcr = v; return;
    case 0x1000E030: sDmacSqwc = v; return;
    case 0x1000E040: sDmacRbsr = v; return;
    case 0x1000E050: sDmacRbor = v; return;
    case 0x1000E060: sDmacStadr = v; return;
    case 0x1000F590: sDmacEnable = v; return;
    }
    if (addr >= 0x10004000 && addr < 0x10008000) {
        rt_log_once("hw", addr & ~0xFFFu, "32-bit FIFO write ignored");
        return;
    }
    rt_log_once("hw", addr, "write32 to an unhandled register");
    sMisc[(addr & 0xFFFF) / 4] = v;
}

void Pal_EeRegWrite128(u32 addr, const void *qword)
{
    u64 lo;
    switch (addr & ~0xFFFu) {
    case 0x10004000: vif_write(0, qword, 1); return;
    case 0x10005000: vif_write(1, qword, 1); return;
    case 0x10006000: gif_path_write(3, qword, 1); return;
    case 0x10007000:
        rt_log_once("hw", 0x10007000, "IPU FIFO write ignored");
        return;
    }
    memcpy(&lo, qword, 8);
    if (addr >= 0x12000000) {
        gs_write(addr, lo, 1);
        return;
    }
    Pal_EeRegWrite32(addr, (u32)lo); /* the EE's registers are 32-bit: the upper half is ignored */
}

u64 Pal_GsRegRead64(u32 addr)
{
    if (addr >= 0x12000000)
        return gs_read(addr);
    return Pal_EeRegRead32(addr) | ((u64)Pal_EeRegRead32(addr + 4) << 32);
}

void Pal_GsRegWrite64(u32 addr, u64 v)
{
    if (addr >= 0x12000000) {
        gs_write(addr, v, 1);
        return;
    }
    Pal_EeRegWrite32(addr, (u32)v);
}

void dma_init(void)
{
    memset(gGfxDmaRegs, 0, sizeof(gGfxDmaRegs));
    sDmacCtrl = sDmacPcr = sDmacSqwc = sDmacRbsr = sDmacRbor = sDmacStadr = 0;
    sDmacEnable = 0x1201;
    sGifDmaWaiting = 0;
}

void Gfx_Init(void)
{
    dma_init();
    gs_init();
    gif_reset_all();
    vif_init();
}
