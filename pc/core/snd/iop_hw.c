/* IOP memory map and the hardware the sound modules touch: the original's
 * iop_hw.c, adapted to the core, with the IOP's RAM and heap (the
 * original's runtime and SIF parts of them).
 *
 * RAM, the SPU2 register window (spu2.c), the DMA channels 4 and 7 that
 * feed the SPU2's two cores, the other I/O registers (plain storage), and
 * the interrupt lines. The native libsd (api/libsd.c) reaches the SPU2, the
 * DMA channels and the SSBUS registers through IopMem_* at their hardware
 * addresses. Interrupt handlers and masks are managed by the intrman
 * handlers in iop_kern.c; this file only records which lines are pending.
 * (The original's 1 KiB scratchpad was the interpreter's alone, and is not
 * here.)
 *
 * Physical map (kseg0/kseg1 mirrors are folded with & 0x1FFFFFFF):
 *   0x00000000  RAM (the HLE IOP's 8 MiB space, IOP_RAM_SIZE)
 *   0x1F801000  I/O registers (DMA 0x1F801080-0x1F8010FF, 0x1F801500-0x1F80157F)
 *   0x1F900000  SPU2 registers
 *   0x1FC00000  BIOS ROM (not present) */
#include "snd/snd_iop.h"

static u8 g_iop_ram[IOP_RAM_SIZE];

#define IO_BASE 0x1F801000u
#define SPU2_BASE 0x1F900000u

static u32 g_io[0x10000 / 4];   /* plain storage for I/O registers without behaviour */
static u64 g_irq_pending;

typedef struct IopDma {
    u32 madr, bcr, chcr;
} IopDma;
static IopDma g_dma[14];

/* ------------------------------------------------------------ interrupts */

void IopHw_RaiseIrq(int irq)
{
    IopKern_Unpark();
    if (irq >= 0 && irq < 64) {
        g_irq_pending |= 1ull << irq;
        SndSys_Post();   /* makes the EE side call the IOP scheduler soon */
    }
}

u64 IopHw_PendingIrqs(void) { return g_irq_pending; }

void IopHw_AckIrq(int irq)
{
    if (irq >= 0 && irq < 64)
        g_irq_pending &= ~(1ull << irq);
}

/* ------------------------------------------------------------ DMA */

static int dma_channel(u32 phys, u32 *reg)
{
    if (phys >= 0x1F801080u && phys < 0x1F801100u && (phys & 0xF) < 0xC) {
        *reg = phys & 0xF;
        return (int)((phys - 0x1F801080u) >> 4);          /* channels 0-6 */
    }
    if (phys >= 0x1F801500u && phys < 0x1F801570u && (phys & 0xF) < 0xC) {
        *reg = phys & 0xF;
        return 7 + (int)((phys - 0x1F801500u) >> 4);      /* channels 7-13 */
    }
    return -1;
}

/* Channels 4 and 7 belong to the SPU2 (spu2.c). No other channel is used
 * by the low-level modules; one that starts is reported and completes
 * after a short delay. */
static void dma_done(u64 now, u32 ch)
{
    (void)now;
    g_dma[ch].chcr &= ~0x01000000u;
    IopHw_RaiseIrq(0x20 + (int)ch);
}

static void dma_start(int ch)
{
    IopDma *d = &g_dma[ch];
    SND_LOG_ONCE("iop", 0x7200 + (u32)ch, "IOP DMA channel %d started (chcr %08X) - not emulated", ch, d->chcr);
    SndSys_EventAdd(SndSys_TimeNs() + 20000, dma_done, (u32)ch);
}

/* ------------------------------------------------------------ access */

static u32 io_read(u32 phys, int size)
{
    if (phys >= SPU2_BASE && phys < SPU2_BASE + 0x800) {
        u32 v = Spu2_Read(phys - SPU2_BASE);
        if (size == 4)
            v |= (u32)Spu2_Read(phys - SPU2_BASE + 2) << 16;
        return v;
    }
    u32 reg;
    int ch = dma_channel(phys, &reg);
    if (ch >= 0) {
        u32 w = ch == 4 || ch == 7 ? Spu2_DmaRead(ch == 7, reg & ~3u)
                : (reg & ~3u) == 0 ? g_dma[ch].madr : (reg & ~3u) == 4 ? g_dma[ch].bcr : g_dma[ch].chcr;
        return w >> ((reg & 3) * 8);
    }
    if (phys >= IO_BASE && phys < IO_BASE + 0x10000) {
        u32 w = g_io[(phys - IO_BASE) >> 2];
        return w >> ((phys & 3) * 8);
    }
    SND_LOG_ONCE("iop", 0x7300 + (phys >> 12), "IOP read from unmapped 0x%08X", phys);
    return 0;
}

static void io_write(u32 phys, u32 v, int size)
{
    if (phys >= SPU2_BASE && phys < SPU2_BASE + 0x800) {
        Spu2_Write(phys - SPU2_BASE, (u16)v);
        if (size == 4)
            Spu2_Write(phys - SPU2_BASE + 2, (u16)(v >> 16));
        return;
    }
    u32 reg;
    int ch = dma_channel(phys, &reg);
    if (ch >= 0) {
        u32 sh = (reg & 3) * 8, mask = (size == 4 ? 0xFFFFFFFFu : size == 2 ? 0xFFFFu : 0xFFu) << sh;
        u32 val = v << sh;
        reg &= ~3u;
        if (ch == 4 || ch == 7) {
            Spu2_DmaWrite(ch == 7, reg, val, mask);
        } else if (reg == 0) {
            g_dma[ch].madr = ((g_dma[ch].madr & ~mask) | (val & mask)) & 0x00FFFFFF;
        } else if (reg == 4) {
            g_dma[ch].bcr = (g_dma[ch].bcr & ~mask) | (val & mask);
        } else {
            u32 old = g_dma[ch].chcr;
            g_dma[ch].chcr = (old & ~mask) | (val & mask);
            if ((g_dma[ch].chcr & 0x01000000u) && !(old & 0x01000000u))
                dma_start(ch);
        }
        return;
    }
    if (phys >= IO_BASE && phys < IO_BASE + 0x10000) {
        u32 *w = &g_io[(phys - IO_BASE) >> 2];
        if (size == 4) {
            *w = v;
        } else {
            u32 sh = (phys & 3) * 8, mask = (size == 2 ? 0xFFFFu : 0xFFu) << sh;
            *w = (*w & ~mask) | ((v << sh) & mask);
        }
        return;
    }
    if (phys == 0x1FFE0130u)
        return;   /* cache control (KSEG2 0xFFFE0130) */
    static int reported;
    if (reported++ < 8)
        SND_LOGW("iop", "IOP write to unmapped 0x%08X = 0x%X", phys, v);
}

static inline u8 *ram(u32 phys, u32 size)
{
    if (phys + size <= IOP_RAM_SIZE)
        return g_iop_ram + phys;
    return NULL;
}

u8 IopMem_Read8(u32 addr)
{
    u32 p = iop_phys(addr);
    u8 *m = ram(p, 1);
    return m ? *m : (u8)io_read(p, 1);
}

u16 IopMem_Read16(u32 addr)
{
    u32 p = iop_phys(addr);
    u8 *m = ram(p, 2);
    if (m) {
        u16 v;
        snd_memcpy(&v, m, 2);
        return v;
    }
    return (u16)io_read(p, 2);
}

u32 IopMem_Read32(u32 addr)
{
    u32 p = iop_phys(addr);
    u8 *m = ram(p, 4);
    if (m) {
        u32 v;
        snd_memcpy(&v, m, 4);
        return v;
    }
    return io_read(p, 4);
}

void IopMem_Write8(u32 addr, u8 v)
{
    u32 p = iop_phys(addr);
    u8 *m = ram(p, 1);
    if (m)
        *m = v;
    else
        io_write(p, v, 1);
}

void IopMem_Write16(u32 addr, u16 v)
{
    u32 p = iop_phys(addr);
    u8 *m = ram(p, 2);
    if (m)
        snd_memcpy(m, &v, 2);
    else
        io_write(p, v, 2);
}

void IopMem_Write32(u32 addr, u32 v)
{
    u32 p = iop_phys(addr);
    u8 *m = ram(p, 4);
    if (m)
        snd_memcpy(m, &v, 4);
    else
        io_write(p, v, 4);
}

void IopHw_Init(void)
{
    snd_memset(g_dma, 0, sizeof(g_dma));
    snd_memset(g_io, 0, sizeof(g_io));
    g_irq_pending = 0;
    Spu2_Init();
}

/* ------------------------------------------------------------ RAM and the heap (snd.h) */

u8 *IopMem_Ptr(u32 iop_addr) { return g_iop_ram + (iop_addr & (IOP_RAM_SIZE - 1)); }

void IopMem_Write(u32 iop_addr, const void *src, u32 size)
{
    u32 a = iop_addr & (IOP_RAM_SIZE - 1);
    if (size > IOP_RAM_SIZE - a)
        size = IOP_RAM_SIZE - a;
    snd_memcpy(g_iop_ram + a, src, size);
}

void IopMem_Read(u32 iop_addr, void *dst, u32 size)
{
    u32 a = iop_addr & (IOP_RAM_SIZE - 1);
    if (size > IOP_RAM_SIZE - a)
        size = IOP_RAM_SIZE - a;
    snd_memcpy(dst, g_iop_ram + a, size);
}

/* The original's IOP heap: first fit over a list of
 * blocks in address order, sizes rounded up to 256 bytes; a free merges
 * with free neighbours. */
typedef struct HeapBlock {
    u32 addr, size;
    int used;
} HeapBlock;
static HeapBlock g_heap[512];
static int g_nheap;

static void heap_reset(void)
{
    g_nheap = 1;
    g_heap[0] = (HeapBlock){IOP_HEAP_BASE, IOP_HEAP_END - IOP_HEAP_BASE, 0};
}

u32 IopKern_HeapAlloc(u32 size)
{
    if (!g_nheap)
        heap_reset();
    size = (size + 255) & ~255u;
    for (int i = 0; i < g_nheap; i++) {
        HeapBlock *b = &g_heap[i];
        if (b->used || b->size < size)
            continue;
        if (b->size > size && g_nheap < (int)(sizeof(g_heap) / sizeof(g_heap[0]))) {
            snd_memmove(&g_heap[i + 2], &g_heap[i + 1], (u32)(g_nheap - i - 1) * sizeof(HeapBlock));
            g_heap[i + 1] = (HeapBlock){b->addr + size, b->size - size, 0};
            g_nheap++;
            b->size = size;
        }
        b->used = 1;
        return b->addr;
    }
    SND_LOGE("iop", "IOP heap exhausted (want 0x%X)", size);
    return 0;
}

void IopKern_HeapFree(u32 addr)
{
    for (int i = 0; i < g_nheap; i++) {
        if (g_heap[i].addr == addr && g_heap[i].used) {
            g_heap[i].used = 0;
            if (i + 1 < g_nheap && !g_heap[i + 1].used) {
                g_heap[i].size += g_heap[i + 1].size;
                snd_memmove(&g_heap[i + 1], &g_heap[i + 2], (u32)(g_nheap - i - 2) * sizeof(HeapBlock));
                g_nheap--;
            }
            if (i > 0 && !g_heap[i - 1].used) {
                g_heap[i - 1].size += g_heap[i].size;
                snd_memmove(&g_heap[i], &g_heap[i + 1], (u32)(g_nheap - i - 1) * sizeof(HeapBlock));
                g_nheap--;
            }
            return;
        }
    }
    SND_LOGW("iop", "free of unknown IOP heap block 0x%08X", addr);
}

void IopKern_Reset(void)
{
    heap_reset();
}
