/* SPU2, the PS2's sound processor, for the low-level IOP (iop_hw.c routes
 * the register window at 0x1F900000 and IOP DMA channels 4 and 7 here).
 *
 * Two cores, each with 24 ADPCM voices (4-point Gaussian interpolation,
 * ADSR envelopes, volume sweeps, pitch modulation, noise), a reverb unit
 * that works in a region of sound RAM, and a PCM input (MEMIN) that
 * AutoDMA streams into. Core 0's output is core 1's external input; core
 * 1's output is the console's. 2 MiB of sound RAM, in 16-bit words.
 *
 * Registers are 16 bits. 32-bit values are split in two registers: for
 * addresses the first holds bits 16-20 and the second bits 0-15; for voice
 * bitmasks the first holds voices 0-15 and the second voices 16-23. Per
 * core (core 1 at +0x400):
 *   0x000 + 16*v  voice VOLL VOLR PITCH ADSR1 ADSR2 ENVX VOLXL VOLXR
 *   0x180-0x196   PMON, NON, VMIXL, VMIXEL, VMIXR, VMIXER
 *   0x198 MMIX    0x19A ATTR    0x19C IRQA    0x1A0 KON    0x1A4 KOFF
 *   0x1A8 TSA     0x1AC STD (data port)       0x1B0 ADMAS (AutoDMA)
 *   0x1C0 + 12*v  voice SSA, LSAX, NAX
 *   0x2E0 ESA, 0x2E4-0x33A reverb addresses, 0x33C EEA (bits 16-20)
 *   0x340 ENDX    0x344 STATX
 *   0x760 + 0x28*core  MVOL, EVOL, AVOL, BVOL, MVOLX, reverb volumes
 *   0x7C2 IRQINFO (bit 2 + core: that core's IRQ address was hit)
 *
 * Output is 48 kHz stereo generated in step with guest time: a periodic
 * event and every register access bring it up to date
 *. The samples go to the host
 * (SndSys_OutputSamples); Spu2_TakeInterval counts and hashes them.
 *
 * The original's spu2.c, adapted to the core: the module's names;
 * the sound device, the WAV file, the movie's mix and the play-test
 * diagnostics (clips, the last minute) are the host's or left out.
 *
 * DMA: normal transfers copy at once and finish (CHCR busy bit, STATX,
 * IRQ 0x24/0x28) after a delay. AutoDMA transfers are taken 1 KiB at a
 * time (256 samples: 512 bytes left, 512 right) as MEMIN's two halves
 * become free, which is what paces a driver's streaming.
 *
 * Sources: ps2tek (SPU2, AutoDMA), psx-spx (the PS1 SPU's voice, envelope,
 * noise and reverb units, which the SPU2 keeps; the Gaussian table and the
 * reverb resampling filter are listed there), ps2sdk's spu2_mmio_hwport.h
 * and libsd (register layout; how the driver uses STATX, ATTR and ADMAS). */
#include "snd/snd_iop.h"

#define RAM_WORDS 0x100000u   /* 2 MiB */
#define RAM_MASK (RAM_WORDS - 1)
#define RATE 48000u
#define NV 24
#define TICK_NS 2000000ull    /* periodic catch-up */
#define OUT_FRAMES 512

enum { ADSR_OFF, ADSR_ATTACK, ADSR_DECAY, ADSR_SUSTAIN, ADSR_RELEASE };
enum { STATX_DMA_READY = 0x80, STATX_DMA_BUSY = 0x400 };
enum { CHCR_START = 0x01000000u };

/* Reverb address registers, in the order of 0x2E4-0x338 (one hi/lo pair
 * each, word offsets from the current reverb position). */
enum {
    R_APF1_SIZE, R_APF2_SIZE, R_SAME_L_DST, R_SAME_R_DST, R_COMB1_L, R_COMB1_R, R_COMB2_L, R_COMB2_R,
    R_SAME_L_SRC, R_SAME_R_SRC, R_DIFF_L_DST, R_DIFF_R_DST, R_COMB3_L, R_COMB3_R, R_COMB4_L, R_COMB4_R,
    R_DIFF_L_SRC, R_DIFF_R_SRC, R_APF1_L_DST, R_APF1_R_DST, R_APF2_L_DST, R_APF2_R_DST, R_COUNT
};
/* Reverb volumes, 0x774-0x786 relative to core 0 */
enum { V_IIR, V_COMB1, V_COMB2, V_COMB3, V_COMB4, V_WALL, V_APF1, V_APF2, V_IN_L, V_IN_R, V_COUNT };

typedef struct Env {
    s32 level;
    u32 counter;
} Env;

typedef struct Voice {
    u16 voll, volr, pitch, adsr1, adsr2;
    u32 ssa, lsa, nax;       /* word addresses; nax = block being played */
    int lsa_set;             /* LSAX written since key on: loop-start flags don't move it */
    int phase;
    Env env;
    Env vol[2];
    u32 counter;             /* pitch counter, 0x1000 per sample */
    s16 hist[4];             /* last four samples, oldest first */
    s16 block[28];
    int pos;                 /* next sample of block[] */
    u8 flags;                /* loop flags of the current block */
    s32 f1, f2;              /* ADPCM predictor history */
    s32 out;                 /* last output after the envelope (pitch modulation) */
} Voice;

typedef struct Core {
    Voice v[NV];
    u32 pmon, non, vmixl, vmixel, vmixr, vmixer, kon, koff, endx;
    u16 mmix, attr, admas, statx;
    u32 irqa, tsa;
    u32 esa, eea, rev_pos;
    u32 rev[R_COUNT];
    s16 rvol[V_COUNT];
    u16 mvol[2];
    Env mvolx[2];
    s16 evol[2], avol[2], bvol[2];
    s32 noise_timer;
    u16 noise;
    /* reverb resampling (psx-spx: 39-tap FIR at both ends) */
    s16 rin[2][64], rout[2][64];
    u32 rpos;
    /* DMA channel (IOP DMA 4 / 7) */
    u32 madr, bcr, chcr;
    u32 dma_bytes;           /* normal transfer in flight */
    u32 dma_event;
    u32 adma_left;           /* AutoDMA bytes not yet taken */
    u32 in_pos;              /* MEMIN read position, 0-511 */
    u8 in_full[2];
    /* statistics */
    u32 n_keyon, n_dma, n_adma_blocks, n_underruns;
    s32 peak_voices, peak_in, peak_ext, peak_out;
} Core;

static const s16 g_gauss[512] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 3, 3,
    3, 4, 4, 5, 5, 6, 7, 7, 8, 9, 9, 10, 11, 12, 13, 14,
    15, 16, 17, 18, 19, 21, 22, 24, 25, 27, 28, 30, 32, 33, 35, 37,
    39, 41, 44, 46, 48, 51, 53, 56, 58, 61, 64, 67, 70, 73, 77, 80,
    84, 87, 91, 95, 99, 103, 107, 111, 116, 120, 125, 130, 135, 140, 145, 150,
    156, 161, 167, 173, 179, 186, 192, 199, 205, 212, 219, 227, 234, 242, 250, 257,
    266, 274, 283, 291, 300, 309, 319, 328, 338, 348, 358, 369, 379, 390, 401, 412,
    424, 436, 448, 460, 473, 485, 498, 512, 525, 539, 553, 567, 582, 597, 612, 627,
    643, 659, 675, 692, 708, 726, 743, 761, 779, 797, 816, 835, 854, 874, 894, 914,
    935, 956, 977, 999, 1020, 1043, 1066, 1089, 1112, 1136, 1160, 1184, 1209, 1234, 1260, 1286,
    1312, 1339, 1366, 1394, 1422, 1450, 1479, 1508, 1537, 1567, 1598, 1628, 1660, 1691, 1723, 1756,
    1789, 1822, 1856, 1890, 1924, 1959, 1995, 2031, 2067, 2104, 2141, 2179, 2217, 2256, 2295, 2334,
    2374, 2415, 2456, 2497, 2539, 2582, 2624, 2668, 2712, 2756, 2801, 2846, 2892, 2938, 2985, 3032,
    3079, 3128, 3176, 3225, 3275, 3325, 3376, 3427, 3479, 3531, 3584, 3637, 3691, 3745, 3799, 3855,
    3910, 3967, 4023, 4081, 4138, 4197, 4255, 4315, 4374, 4435, 4495, 4557, 4619, 4681, 4744, 4807,
    4871, 4935, 5000, 5065, 5131, 5197, 5264, 5332, 5399, 5468, 5536, 5606, 5676, 5746, 5817, 5888,
    5959, 6032, 6104, 6177, 6251, 6325, 6400, 6475, 6550, 6626, 6702, 6779, 6856, 6934, 7012, 7091,
    7170, 7249, 7329, 7409, 7490, 7571, 7653, 7735, 7817, 7900, 7983, 8066, 8150, 8234, 8319, 8404,
    8489, 8575, 8661, 8748, 8834, 8922, 9009, 9097, 9185, 9273, 9362, 9451, 9541, 9630, 9720, 9811,
    9901, 9992, 10083, 10174, 10266, 10358, 10450, 10542, 10635, 10727, 10820, 10913, 11007, 11100, 11194, 11288,
    11382, 11476, 11571, 11665, 11760, 11855, 11950, 12045, 12140, 12236, 12331, 12427, 12522, 12618, 12714, 12809,
    12905, 13001, 13097, 13193, 13289, 13385, 13481, 13577, 13673, 13769, 13865, 13961, 14056, 14152, 14248, 14343,
    14439, 14534, 14630, 14725, 14820, 14915, 15010, 15104, 15199, 15293, 15387, 15481, 15575, 15669, 15762, 15855,
    15948, 16041, 16133, 16226, 16317, 16409, 16500, 16592, 16682, 16773, 16863, 16953, 17042, 17131, 17220, 17308,
    17396, 17484, 17571, 17658, 17744, 17830, 17916, 18001, 18086, 18170, 18254, 18337, 18420, 18502, 18584, 18665,
    18746, 18826, 18905, 18985, 19063, 19141, 19219, 19295, 19372, 19447, 19522, 19597, 19671, 19744, 19816, 19888,
    19959, 20030, 20100, 20169, 20238, 20306, 20373, 20439, 20505, 20570, 20634, 20698, 20760, 20822, 20884, 20944,
    21004, 21063, 21121, 21178, 21235, 21290, 21345, 21399, 21452, 21505, 21556, 21607, 21657, 21706, 21754, 21801,
    21848, 21893, 21938, 21982, 22025, 22066, 22107, 22148, 22187, 22225, 22262, 22299, 22334, 22369, 22402, 22435,
    22467, 22498, 22527, 22556, 22584, 22611, 22637, 22662, 22686, 22709, 22731, 22752, 22772, 22791, 22809, 22826,
    22842, 22857, 22872, 22885, 22897, 22908, 22918, 22927, 22935, 22942, 22948, 22953, 22957, 22960, 22962, 22963,
};

static const s16 g_fir[39] = {
    -1, 0, 2, 0, -10, 0, 35, 0, -103, 0, 266, 0, -616, 0, 1332, 0, -2960, 0, 10246,
    16384,
    10246, 0, -2960, 0, 1332, 0, -616, 0, 266, 0, -103, 0, 35, 0, -10, 0, 2, 0, -1,
};

static const s32 g_adpcm_pos[5] = { 0, 60, 115, 98, 122 };
static const s32 g_adpcm_neg[5] = { 0, 0, -52, -55, -60 };

static u16 g_sram[RAM_WORDS];
static Core g_core[2];
static u16 g_raw[0x400];      /* read-back of registers without behaviour */
static u16 g_irqinfo;
static int g_on;
static u64 g_last_ns, g_rem;
static s16 g_out[OUT_FRAMES * 2];
static u32 g_nout;
static int g_generating;      /* inside advance(): register accesses don't render */
static int g_irq_during_gen;  /* a DMA interrupt was raised while rendering */
/* the interval's sample count and hash (Spu2_TakeInterval) */
#define FNV_INIT 0xcbf29ce484222325ull
static u64 g_seam_h = FNV_INIT, g_seam_n;

static inline s32 clamp16(s32 v) { return v < -0x8000 ? -0x8000 : v > 0x7FFF ? 0x7FFF : v; }
static inline s32 iabs(s32 v) { return v < 0 ? -v : v; }

/* ------------------------------------------------------------ envelopes */

/* One tick of the envelope generator shared by ADSR and volume sweeps
 * (psx-spx, "Envelope Operation"). rate = shift << 2 | step. */
static void env_step(Env *e, u32 rate, int decrease, int exponential, int invert)
{
    u32 shift = (rate >> 2) & 0x1F, stepv = rate & 3;
    s32 step = 7 - (s32)stepv;
    if (decrease ^ invert)
        step = ~step;   /* +7..+4 -> -8..-5 */
    if (shift < 11)
        step *= 1 << (11 - shift);
    u32 inc = shift > 11 ? 0x8000u >> (shift - 11) : 0x8000u;
    if (exponential && !decrease && e->level > 0x6000) {
        if (shift < 10) {
            step >>= 2;
        } else if (shift >= 11) {
            inc >>= 2;
        } else {
            step >>= 1;
            inc >>= 1;
        }
    } else if (exponential && decrease) {
        step = (step * e->level) >> 15;
    }
    if (rate != 0x7F && inc == 0)
        inc = 1;
    e->counter += inc;
    if (!(e->counter & 0x8000))
        return;
    e->counter = 0;
    s32 l = e->level + step;
    if (!decrease)
        l = clamp16(l);
    else if (invert)
        l = l < -0x8000 ? -0x8000 : l > 0 ? 0 : l;
    else if (l < 0)
        l = 0;
    e->level = l;
}

/* A volume register: fixed (bit 15 clear, value/2 in bits 0-14) or a sweep. */
static void vol_tick(Env *e, u16 reg)
{
    if (!(reg & 0x8000))
        e->level = (s16)(u16)(reg << 1);
    else
        env_step(e, reg & 0x7F, (reg >> 13) & 1, (reg >> 14) & 1, (reg >> 12) & 1);
}

static void adsr_tick(Voice *v)
{
    Env *e = &v->env;
    switch (v->phase) {
    case ADSR_ATTACK:
        env_step(e, (v->adsr1 >> 8) & 0x7F, 0, v->adsr1 >> 15, 0);
        if (e->level >= 0x7FFF) {
            v->phase = ADSR_DECAY;
            e->counter = 0;
        }
        break;
    case ADSR_DECAY:
        env_step(e, ((v->adsr1 >> 4) & 0xF) << 2, 1, 1, 0);
        if (e->level <= (s32)((v->adsr1 & 0xF) + 1) * 0x800) {
            v->phase = ADSR_SUSTAIN;
            e->counter = 0;
        }
        break;
    case ADSR_SUSTAIN:
        env_step(e, (v->adsr2 >> 6) & 0x7F, (v->adsr2 >> 14) & 1, v->adsr2 >> 15, 0);
        break;
    case ADSR_RELEASE:
        env_step(e, (u32)(v->adsr2 & 0x1F) << 2, 1, (v->adsr2 >> 5) & 1, 0);
        if (e->level <= 0) {
            e->level = 0;
            v->phase = ADSR_OFF;
        }
        break;
    }
}

/* ------------------------------------------------------------ interrupts */

/* A sound RAM access at [addr, addr+len): cores whose IRQ address lies in
 * it and whose IRQ is enabled (ATTR bit 6) raise IOP interrupt 9. */
static void irq_check(u32 addr, u32 len)
{
    for (int c = 0; c < 2; c++) {
        Core *k = &g_core[c];
        if ((k->attr & 0x40) && ((k->irqa - addr) & RAM_MASK) < len && !(g_irqinfo & (4u << c))) {
            g_irqinfo |= (u16)(4u << c);
            IopHw_RaiseIrq(9);
        }
    }
}

/* ------------------------------------------------------------ voices */

static void decode_block(Voice *v)
{
    u32 a = v->nax & RAM_MASK;
    irq_check(a, 8);
    u16 hdr = g_sram[a];
    int shift = hdr & 0xF, filter = (hdr >> 4) & 7;
    if (shift > 12)
        shift = 9;
    if (filter > 4)
        filter = 4;
    v->flags = (u8)(hdr >> 8);
    if ((v->flags & 4) && !v->lsa_set)
        v->lsa = a;
    s32 f1 = v->f1, f2 = v->f2;
    for (int k = 0; k < 28; k++) {
        u32 nib = (g_sram[(a + 1 + (u32)(k >> 2)) & RAM_MASK] >> ((k & 3) * 4)) & 0xF;
        s32 s = (s32)(s16)(u16)(nib << 12) >> shift;
        s = clamp16(s + ((f1 * g_adpcm_pos[filter] + f2 * g_adpcm_neg[filter] + 32) >> 6));
        f2 = f1;
        f1 = s;
        v->block[k] = (s16)s;
    }
    v->f1 = f1;
    v->f2 = f2;
    v->pos = 0;
}

/* End of the current block: loop or go on to the next one. */
static void block_end(Core *c, Voice *v, int i)
{
    if (v->flags & 1) {
        c->endx |= 1u << i;
        v->nax = v->lsa & RAM_MASK & ~7u;
        if (!(v->flags & 2)) {   /* end without repeat: release, silence */
            v->phase = ADSR_OFF;
            v->env.level = 0;
        }
    } else {
        v->nax = (v->nax + 8) & RAM_MASK;
    }
}

static void key_on(Core *c, int i)
{
    Voice *v = &c->v[i];
    v->nax = v->ssa & ~7u;
    v->lsa_set = 0;
    v->counter = 0;
    v->f1 = v->f2 = 0;
    snd_memset(v->hist, 0, sizeof(v->hist));
    decode_block(v);
    v->env.level = 0;
    v->env.counter = 0;
    v->phase = ADSR_ATTACK;
    c->endx &= ~(1u << i);
    c->n_keyon++;
}

static void key_off(Core *c, int i)
{
    Voice *v = &c->v[i];
    if (v->phase != ADSR_OFF) {
        v->phase = ADSR_RELEASE;
        v->env.counter = 0;
    }
}

static s32 voice_tick(Core *c, int i)
{
    Voice *v = &c->v[i];
    s32 s;
    if (c->non & (1u << i)) {
        s = (s16)c->noise;
    } else {
        u32 k = (v->counter >> 4) & 0xFF;
        s = (g_gauss[0xFF - k] * v->hist[0]) >> 15;
        s += (g_gauss[0x1FF - k] * v->hist[1]) >> 15;
        s += (g_gauss[0x100 + k] * v->hist[2]) >> 15;
        s += (g_gauss[k] * v->hist[3]) >> 15;
    }
    adsr_tick(v);
    s = (s * v->env.level) >> 15;
    v->out = s;

    u32 step = v->pitch;
    if (i > 0 && (c->pmon & (1u << i))) {
        s32 f = c->v[i - 1].out + 0x8000;
        step = (u32)(((s32)(s16)step * f) >> 15) & 0xFFFF;
    }
    if (step > 0x3FFF)
        step = 0x4000;
    v->counter += step;
    while (v->counter >= 0x1000) {
        v->counter -= 0x1000;
        if (v->pos >= 28) {
            block_end(c, v, i);
            decode_block(v);
        }
        v->hist[0] = v->hist[1];
        v->hist[1] = v->hist[2];
        v->hist[2] = v->hist[3];
        v->hist[3] = v->block[v->pos++];
    }
    return s;
}

static void noise_tick(Core *c)
{
    u32 shift = (c->attr >> 10) & 0xF, step = ((c->attr >> 8) & 3) + 4;
    c->noise_timer -= (s32)step;
    if (c->noise_timer < 0) {
        u32 n = c->noise;
        u32 parity = ((n >> 15) ^ (n >> 12) ^ (n >> 11) ^ (n >> 10) ^ 1) & 1;
        c->noise = (u16)((n << 1) | parity);
        c->noise_timer += (s32)(0x20000u >> shift);
        if (c->noise_timer < 0)
            c->noise_timer += (s32)(0x20000u >> shift);
    }
}

/* ------------------------------------------------------------ reverb */

static u32 rev_addr(const Core *c, s32 off)
{
    u32 start = c->esa & RAM_MASK, end = c->eea & RAM_MASK;
    if (end <= start)
        return start;
    s64 size = (s64)(end - start) + 1;
    s64 rel = ((s64)c->rev_pos - (s64)start + off) % size;
    if (rel < 0)
        rel += size;
    return start + (u32)rel;
}

static s32 rev_rd(const Core *c, s32 off) { return (s16)g_sram[rev_addr(c, off)]; }
static void rev_wr(const Core *c, s32 off, s32 v) { g_sram[rev_addr(c, off)] = (u16)clamp16(v); }

/* One 24 kHz step of the reverb (psx-spx "Reverb Formula"; SPU2 offsets
 * are in words, so "[x-2]" there is x-1 here). */
static void reverb_step(Core *c, s32 in_l, s32 in_r, s32 *out_l, s32 *out_r)
{
    const s32 *a = (const s32 *)c->rev;
    const s16 *vv = c->rvol;
    s32 lin = (in_l * vv[V_IN_L]) >> 15, rin = (in_r * vv[V_IN_R]) >> 15;
    s32 iir = vv[V_IIR], wall = vv[V_WALL];
    s32 p;

    p = rev_rd(c, a[R_SAME_L_DST] - 1);
    rev_wr(c, a[R_SAME_L_DST], (((clamp16(lin + ((rev_rd(c, a[R_SAME_L_SRC]) * wall) >> 15)) - p) * iir) >> 15) + p);
    p = rev_rd(c, a[R_SAME_R_DST] - 1);
    rev_wr(c, a[R_SAME_R_DST], (((clamp16(rin + ((rev_rd(c, a[R_SAME_R_SRC]) * wall) >> 15)) - p) * iir) >> 15) + p);
    p = rev_rd(c, a[R_DIFF_L_DST] - 1);
    rev_wr(c, a[R_DIFF_L_DST], (((clamp16(lin + ((rev_rd(c, a[R_DIFF_R_SRC]) * wall) >> 15)) - p) * iir) >> 15) + p);
    p = rev_rd(c, a[R_DIFF_R_DST] - 1);
    rev_wr(c, a[R_DIFF_R_DST], (((clamp16(rin + ((rev_rd(c, a[R_DIFF_L_SRC]) * wall) >> 15)) - p) * iir) >> 15) + p);

    s32 l = clamp16((rev_rd(c, a[R_COMB1_L]) * vv[V_COMB1] + rev_rd(c, a[R_COMB2_L]) * vv[V_COMB2] +
                     rev_rd(c, a[R_COMB3_L]) * vv[V_COMB3] + rev_rd(c, a[R_COMB4_L]) * vv[V_COMB4]) >> 15);
    s32 r = clamp16((rev_rd(c, a[R_COMB1_R]) * vv[V_COMB1] + rev_rd(c, a[R_COMB2_R]) * vv[V_COMB2] +
                     rev_rd(c, a[R_COMB3_R]) * vv[V_COMB3] + rev_rd(c, a[R_COMB4_R]) * vv[V_COMB4]) >> 15);

    p = rev_rd(c, a[R_APF1_L_DST] - a[R_APF1_SIZE]);
    l = clamp16(l - ((p * vv[V_APF1]) >> 15));
    rev_wr(c, a[R_APF1_L_DST], l);
    l = clamp16(((l * vv[V_APF1]) >> 15) + p);
    p = rev_rd(c, a[R_APF1_R_DST] - a[R_APF1_SIZE]);
    r = clamp16(r - ((p * vv[V_APF1]) >> 15));
    rev_wr(c, a[R_APF1_R_DST], r);
    r = clamp16(((r * vv[V_APF1]) >> 15) + p);

    p = rev_rd(c, a[R_APF2_L_DST] - a[R_APF2_SIZE]);
    l = clamp16(l - ((p * vv[V_APF2]) >> 15));
    rev_wr(c, a[R_APF2_L_DST], l);
    l = clamp16(((l * vv[V_APF2]) >> 15) + p);
    p = rev_rd(c, a[R_APF2_R_DST] - a[R_APF2_SIZE]);
    r = clamp16(r - ((p * vv[V_APF2]) >> 15));
    rev_wr(c, a[R_APF2_R_DST], r);
    r = clamp16(((r * vv[V_APF2]) >> 15) + p);

    *out_l = l;
    *out_r = r;
    c->rev_pos = c->rev_pos + 1 > (c->eea & RAM_MASK) ? c->esa & RAM_MASK : c->rev_pos + 1;
}

static s32 fir(const s16 *ring, u32 newest)
{
    s32 acc = 0;
    for (u32 k = 0; k < 39; k++)
        acc += g_fir[k] * ring[(newest - k) & 63];
    return acc;
}

/* The reverb runs at 24 kHz between two 39-tap resampling filters. */
static void reverb_tick(Core *c, s32 in_l, s32 in_r, s32 *out_l, s32 *out_r)
{
    u32 p = c->rpos++;
    c->rin[0][p & 63] = (s16)in_l;
    c->rin[1][p & 63] = (s16)in_r;
    s32 yl = 0, yr = 0;
    if (p & 1) {
        reverb_step(c, clamp16(fir(c->rin[0], p) >> 15), clamp16(fir(c->rin[1], p) >> 15), &yl, &yr);
    }
    c->rout[0][p & 63] = (s16)yl;
    c->rout[1][p & 63] = (s16)yr;
    *out_l = clamp16(fir(c->rout[0], p) >> 14);
    *out_r = clamp16(fir(c->rout[1], p) >> 14);
}

/* ------------------------------------------------------------ DMA */

static void dma_irq(Core *c, int core)
{
    SND_LOGT("spu2", "core %d DMA done", core);
    c->chcr &= ~CHCR_START;
    IopHw_RaiseIrq(core ? 0x28 : 0x24);
    if (g_generating)
        g_irq_during_gen = 1;
}

/* Feed MEMIN from an AutoDMA transfer while one of its halves is free. */
static void adma_feed(Core *c, int core)
{
    while (c->adma_left && (c->chcr & CHCR_START) && (c->admas & (1u << core))) {
        u32 cur = c->in_pos >> 8;
        u32 h;
        if ((c->in_pos & 255) == 0 && !c->in_full[cur])
            h = cur;          /* at a boundary: the half about to play */
        else if (!c->in_full[cur ^ 1])
            h = cur ^ 1;
        else
            break;
        u32 n = c->adma_left < 1024 ? c->adma_left : 1024;
        u32 base = 0x2000u + (u32)core * 0x400u + h * 256u;
        for (u32 b = 0; b < n; b += 2) {
            u32 dst = b < 512 ? base + b / 2 : base + 0x200u + (b - 512) / 2;
            g_sram[dst] = IopMem_Read16(c->madr + b);
        }
        c->madr += n;
        c->adma_left -= n;
        c->in_full[h] = 1;
        c->n_adma_blocks++;
        if (!c->adma_left)
            dma_irq(c, core);
    }
}

static void adma_read(Core *c, int core, s32 *l, s32 *r)
{
    u32 half = c->in_pos >> 8, base = 0x2000u + (u32)core * 0x400u;
    if (c->in_full[half]) {
        *l = (s16)g_sram[base + c->in_pos];
        *r = (s16)g_sram[base + 0x200u + c->in_pos];
    } else {
        *l = *r = 0;
    }
    c->in_pos = (c->in_pos + 1) & 511;
    if ((c->in_pos & 255) == 0) {
        if (!c->in_full[half] && c->n_underruns++ < 10)
            SND_LOGD("spu2", "core %d: MEMIN underrun (AutoDMA %s)", core, c->chcr & CHCR_START ? "late" : "stopped");
        c->in_full[half] = 0;
        adma_feed(c, core);
    }
}

static void dma_done(u64 now, u32 core)
{
    (void)now;
    Core *c = &g_core[core];
    c->dma_event = 0;
    if (!(c->chcr & CHCR_START))
        return;   /* stopped meanwhile */
    c->madr += c->dma_bytes;
    SND_LOGT("spu2", "core %u DMA of 0x%X bytes done", core, c->dma_bytes);
    c->dma_bytes = 0;
    c->statx = (u16)((c->statx | STATX_DMA_READY) & ~STATX_DMA_BUSY);
    dma_irq(c, (int)core);
}

static void dma_start(Core *c, int core)
{
    u32 bs = c->bcr & 0xFFFF, bc = c->bcr >> 16;
    u32 words = ((c->chcr >> 9) & 3) == 0 ? (bs ? bs : 0x10000u) : bs * (bc ? bc : 1);
    u32 bytes = words * 4;
    c->n_dma++;
    if (c->admas & (1u << core)) {
        SND_LOGT("spu2", "core %d AutoDMA 0x%X bytes from 0x%06X", core, bytes, c->madr);
        c->adma_left = bytes;
        adma_feed(c, core);
        return;
    }
    if (c->admas & 4) {
        SND_LOG_ONCE("spu2", 0x100u + (u32)core, "SPU2 core %d: AutoDMA read (not emulated)", core);
        c->dma_bytes = 0;
        c->dma_event = SndSys_EventAdd(SndSys_TimeNs() + 20000, dma_done, (u32)core);
        return;
    }
    u32 n = bytes / 2, tsa = c->tsa;
    if (c->chcr & 1) {
        for (u32 i = 0; i < n; i++)
            g_sram[(tsa + i) & RAM_MASK] = IopMem_Read16(c->madr + i * 2);
        irq_check(tsa, n);
    } else {
        for (u32 i = 0; i < n; i++)
            IopMem_Write16(c->madr + i * 2, g_sram[(tsa + i) & RAM_MASK]);
    }
    c->tsa = (tsa + n) & RAM_MASK;
    c->dma_bytes = bytes;
    c->statx = (u16)((c->statx & ~STATX_DMA_READY) | STATX_DMA_BUSY);
    u64 ns = (u64)bytes * 60;
    c->dma_event = SndSys_EventAdd(SndSys_TimeNs() + (ns < 20000 ? 20000 : ns), dma_done, (u32)core);
    SND_LOGT("spu2", "core %d DMA %s 0x%X bytes, IOP 0x%06X, SPU word 0x%05X", core, (c->chcr & 1) ? "write" : "read", bytes,
         c->madr, tsa);
}

static void dma_stop(Core *c)
{
    SND_LOGD("spu2", "core %d DMA stopped (%s, 0x%X bytes left)", (int)(c - g_core), c->adma_left ? "AutoDMA" : "normal",
         c->adma_left ? c->adma_left : c->dma_bytes);
    if (c->dma_event)
        SndSys_EventCancel(c->dma_event);
    c->dma_event = 0;
    c->dma_bytes = 0;
    c->adma_left = 0;
    c->statx = (u16)(c->statx & ~STATX_DMA_BUSY);
}

u32 Spu2_DmaRead(int core, u32 reg)
{
    Spu2_Advance(SndSys_TimeNs());
    Core *c = &g_core[core & 1];
    return reg == 0 ? c->madr : reg == 4 ? c->bcr : c->chcr;
}

void Spu2_DmaWrite(int core, u32 reg, u32 v, u32 mask)
{
    Spu2_Advance(SndSys_TimeNs());
    Core *c = &g_core[core & 1];
    if (reg == 0) {
        c->madr = ((c->madr & ~mask) | (v & mask)) & 0x00FFFFFFu;
    } else if (reg == 4) {
        c->bcr = (c->bcr & ~mask) | (v & mask);
    } else {
        u32 old = c->chcr;
        c->chcr = (c->chcr & ~mask) | (v & mask);
        if ((c->chcr & CHCR_START) && !(old & CHCR_START))
            dma_start(c, core & 1);
        else if (!(c->chcr & CHCR_START) && (old & CHCR_START))
            dma_stop(c);
    }
}

/* ------------------------------------------------------------ mixing */

static void core_tick(Core *c, int core, s32 ext_l, s32 ext_r, s32 *out_l, s32 *out_r)
{
    s32 dl = 0, dr = 0, wl = 0, wr = 0;
    if (c->non)
        noise_tick(c);
    for (int i = 0; i < NV; i++) {
        Voice *v = &c->v[i];
        if (v->phase == ADSR_OFF) {
            v->out = 0;
            continue;
        }
        s32 s = voice_tick(c, i);
        vol_tick(&v->vol[0], v->voll);
        vol_tick(&v->vol[1], v->volr);
        s32 l = (s * v->vol[0].level) >> 15, r = (s * v->vol[1].level) >> 15;
        u32 bit = 1u << i;
        if (c->vmixl & bit)
            dl += l;
        if (c->vmixr & bit)
            dr += r;
        if (c->vmixel & bit)
            wl += l;
        if (c->vmixer & bit)
            wr += r;
    }
    s32 il = 0, ir = 0;
    if (iabs(dl) > c->peak_voices)
        c->peak_voices = iabs(dl);
    if (c->admas & (1u << core)) {
        adma_read(c, core, &il, &ir);
        if (iabs(il) > c->peak_in)
            c->peak_in = iabs(il);
        il = (il * c->bvol[0]) >> 15;
        ir = (ir * c->bvol[1]) >> 15;
    }
    if (iabs(ext_l) > c->peak_ext)
        c->peak_ext = iabs(ext_l);
    ext_l = (ext_l * c->avol[0]) >> 15;
    ext_r = (ext_r * c->avol[1]) >> 15;
    u16 m = c->mmix;
    s32 tdl = ((m & 0x800) ? dl : 0) + ((m & 0x80) ? il : 0) + ((m & 0x8) ? ext_l : 0);
    s32 tdr = ((m & 0x400) ? dr : 0) + ((m & 0x40) ? ir : 0) + ((m & 0x4) ? ext_r : 0);
    s32 rl = 0, rr = 0;
    if (c->attr & 0x80) {
        s32 twl = ((m & 0x200) ? wl : 0) + ((m & 0x20) ? il : 0) + ((m & 0x2) ? ext_l : 0);
        s32 twr = ((m & 0x100) ? wr : 0) + ((m & 0x10) ? ir : 0) + ((m & 0x1) ? ext_r : 0);
        reverb_tick(c, clamp16(twl), clamp16(twr), &rl, &rr);
        rl = (rl * c->evol[0]) >> 15;
        rr = (rr * c->evol[1]) >> 15;
    }
    vol_tick(&c->mvolx[0], c->mvol[0]);
    vol_tick(&c->mvolx[1], c->mvol[1]);
    *out_l = (clamp16(tdl + rl) * c->mvolx[0].level) >> 15;
    *out_r = (clamp16(tdr + rr) * c->mvolx[1].level) >> 15;
    if (iabs(*out_l) > c->peak_out)
        c->peak_out = iabs(*out_l);
}

static void flush_out(void)
{
    if (!g_nout)
        return;
    SndSys_OutputSamples(g_out, g_nout);
    g_nout = 0;
}

static void gen_frame(void)
{
    s32 l0, r0, l1, r1;
    core_tick(&g_core[0], 0, 0, 0, &l0, &r0);
    core_tick(&g_core[1], 1, l0, r0, &l1, &r1);
    g_out[g_nout * 2] = (s16)clamp16(l1);
    g_out[g_nout * 2 + 1] = (s16)clamp16(r1);
    s16 seam[2] = {(s16)clamp16(l1), (s16)clamp16(r1)};
    const u8 *b = (const u8 *)seam;
    for (int i = 0; i < 4; i++)
        g_seam_h = (g_seam_h ^ b[i]) * 0x100000001b3ull;
    g_seam_n++;
    if (++g_nout == OUT_FRAMES)
        flush_out();
}

/* Render the samples between the last call and `now`. The IOP only runs
 * when the EE lets it, so the periodic tick may render several blocks at
 * once; with may_run_iop it stops at each DMA interrupt and lets the IOP
 * answer (a driver restarting AutoDMA) before going on, as it would have
 * on the console. Register accesses from the IOP meanwhile don't render. */
static void advance(u64 now_ns, int may_run_iop)
{
    if (!g_on || g_generating || now_ns <= g_last_ns)
        return;
    u64 d = now_ns - g_last_ns;
    g_last_ns = now_ns;
    if (d > NS_PER_SEC)
        d = NS_PER_SEC;   /* after a long stall, don't render the backlog */
    u64 acc = d * RATE + g_rem;
    u64 n = acc / NS_PER_SEC;
    g_rem = acc % NS_PER_SEC;
    g_generating = 1;
    while (n--) {
        gen_frame();
        if (may_run_iop && g_irq_during_gen) {
            g_irq_during_gen = 0;
            IopKern_Run();
        }
    }
    g_irq_during_gen = 0;
    g_generating = 0;
    flush_out();
}

void Spu2_Advance(u64 now_ns) { advance(now_ns, 0); }

static void tick(u64 now, u32 param)
{
    (void)param;
    advance(SndSys_TimeNs(), 1);
    SndSys_EventAdd(now + TICK_NS, tick, 0);
}

/* ------------------------------------------------------------ registers */

static u16 lo16(u32 v) { return (u16)v; }
static u16 hi16(u32 v) { return (u16)(v >> 16); }
static void set_lo(u32 *r, u16 v) { *r = (*r & 0xFFFF0000u) | v; }
static void set_hi(u32 *r, u16 v) { *r = (*r & 0x0000FFFFu) | ((u32)v << 16); }
/* 32-bit registers: addresses keep bits 16-20 at the lower register;
 * voice masks keep voices 0-15 there. */
static void set_addr(u32 *r, u32 which, u16 v)
{
    if (which == 0)
        *r = ((*r & 0xFFFFu) | ((u32)(v & 0x1F) << 16)) & RAM_MASK;
    else
        *r = (*r & 0xFFFF0000u) | v;
}
static u16 get_addr(u32 r, u32 which) { return which == 0 ? (u16)((r >> 16) & 0x1F) : (u16)r; }
static void set_mask(u32 *r, u32 which, u16 v)
{
    if (which == 0)
        set_lo(r, v);
    else
        set_hi(r, (u16)(v & 0xFF));
}
static u16 get_mask(u32 r, u32 which) { return which == 0 ? lo16(r) : (u16)(hi16(r) & 0xFF); }

static u16 core_read(Core *c, int core, u32 r)
{
    if (r < 0x180) {
        Voice *v = &c->v[r >> 4];
        switch ((r >> 1) & 7) {
        case 0: return v->voll;
        case 1: return v->volr;
        case 2: return v->pitch;
        case 3: return v->adsr1;
        case 4: return v->adsr2;
        case 5: return (u16)v->env.level;
        case 6: return (u16)v->vol[0].level;
        default: return (u16)v->vol[1].level;
        }
    }
    if (r >= 0x1C0 && r < 0x2E0) {
        u32 i = r - 0x1C0;
        Voice *v = &c->v[i / 12];
        switch ((i % 12) >> 1) {
        case 0: return get_addr(v->ssa, 0);
        case 1: return get_addr(v->ssa, 1);
        case 2: return get_addr(v->lsa, 0);
        case 3: return get_addr(v->lsa, 1);
        case 4: return get_addr(v->nax, 0);
        default: return get_addr(v->nax, 1);
        }
    }
    if (r >= 0x2E4 && r < 0x33C)
        return get_addr(c->rev[(r - 0x2E4) >> 2], (r >> 1) & 1);
    switch (r) {
    case 0x180: case 0x182: return get_mask(c->pmon, (r >> 1) & 1);
    case 0x184: case 0x186: return get_mask(c->non, (r >> 1) & 1);
    case 0x188: case 0x18A: return get_mask(c->vmixl, (r >> 1) & 1);
    case 0x18C: case 0x18E: return get_mask(c->vmixel, (r >> 1) & 1);
    case 0x190: case 0x192: return get_mask(c->vmixr, (r >> 1) & 1);
    case 0x194: case 0x196: return get_mask(c->vmixer, (r >> 1) & 1);
    case 0x198: return c->mmix;
    case 0x19A: return c->attr;
    case 0x19C: case 0x19E: return get_addr(c->irqa, (r >> 1) & 1);
    case 0x1A0: case 0x1A2: return get_mask(c->kon, (r >> 1) & 1);
    case 0x1A4: case 0x1A6: return get_mask(c->koff, (r >> 1) & 1);
    case 0x1A8: case 0x1AA: return get_addr(c->tsa, (r >> 1) & 1);
    case 0x1B0: return c->admas;
    case 0x2E0: case 0x2E2: return get_addr(c->esa, (r >> 1) & 1);
    case 0x33C: return (u16)((c->eea >> 16) & 0x1F);
    case 0x340: case 0x342: return get_mask(c->endx, (r >> 1) & 1);
    case 0x344: return c->statx;
    default: return g_raw[((u32)core * 0x400 + r) >> 1];
    }
}

static void core_write(Core *c, int core, u32 r, u16 v)
{
    if (r < 0x180) {
        Voice *vo = &c->v[r >> 4];
        switch ((r >> 1) & 7) {
        case 0: vo->voll = v; break;
        case 1: vo->volr = v; break;
        case 2: vo->pitch = v; break;
        case 3: vo->adsr1 = v; break;
        case 4: vo->adsr2 = v; break;
        case 5: vo->env.level = (s16)v; break;
        default: break;   /* VOLXL/VOLXR: current volume, read-only */
        }
        return;
    }
    if (r >= 0x1C0 && r < 0x2E0) {
        u32 i = r - 0x1C0;
        Voice *vo = &c->v[i / 12];
        u32 which = (i >> 1) & 1;
        switch ((i % 12) >> 1) {
        case 0: case 1: set_addr(&vo->ssa, which, v); break;
        case 2: case 3: set_addr(&vo->lsa, which, v); vo->lsa_set = 1; break;
        default: set_addr(&vo->nax, which, v); break;
        }
        return;
    }
    if (r >= 0x2E4 && r < 0x33C) {
        set_addr(&c->rev[(r - 0x2E4) >> 2], (r >> 1) & 1, v);
        return;
    }
    u32 which = (r >> 1) & 1;
    switch (r) {
    case 0x180: case 0x182: set_mask(&c->pmon, which, v); break;
    case 0x184: case 0x186: set_mask(&c->non, which, v); break;
    case 0x188: case 0x18A: set_mask(&c->vmixl, which, v); break;
    case 0x18C: case 0x18E: set_mask(&c->vmixel, which, v); break;
    case 0x190: case 0x192: set_mask(&c->vmixr, which, v); break;
    case 0x194: case 0x196: set_mask(&c->vmixer, which, v); break;
    case 0x198: c->mmix = v; break;
    case 0x19A: {
        u16 old = c->attr;
        c->attr = v;
        if (!(v & 0x8000))
            c->statx = 0;   /* core reset */
        if ((old & 0x40) && !(v & 0x40))
            g_irqinfo = (u16)(g_irqinfo & ~(4u << core));
        if ((v & 0x30) == 0x10)
            c->statx = (u16)(c->statx & ~STATX_DMA_BUSY);   /* manual transfer: STD data is already in RAM */
        break;
    }
    case 0x19C: case 0x19E: set_addr(&c->irqa, which, v); break;
    case 0x1A0: case 0x1A2:
        set_mask(&c->kon, which, v);
        for (int i = 0; i < 16; i++)
            if (v & (1u << i) && (which == 0 || i < 8))
                key_on(c, which ? 16 + i : i);
        break;
    case 0x1A4: case 0x1A6:
        set_mask(&c->koff, which, v);
        for (int i = 0; i < 16; i++)
            if (v & (1u << i) && (which == 0 || i < 8))
                key_off(c, which ? 16 + i : i);
        break;
    case 0x1A8: case 0x1AA: set_addr(&c->tsa, which, v); break;
    case 0x1AC:   /* STD: manual transfer, one word at TSA */
        irq_check(c->tsa, 1);
        g_sram[c->tsa] = v;
        c->tsa = (c->tsa + 1) & RAM_MASK;
        break;
    case 0x1B0: {
        u16 old = c->admas;
        c->admas = v;
        if (v != old)
            SND_LOGD("spu2", "core %d ADMAS %X -> %X", core, old, v);
        if ((v & (1u << core)) && !(old & (1u << core))) {
            c->in_pos = 0;   /* AutoDMA starts on an empty MEMIN */
            c->in_full[0] = c->in_full[1] = 0;
        }
        break;
    }
    case 0x2E0: case 0x2E2:
        set_addr(&c->esa, which, v);
        c->rev_pos = c->esa;
        break;
    case 0x33C: c->eea = ((u32)(v & 0x1F) << 16) | 0xFFFFu; break;
    case 0x340: case 0x342: set_mask(&c->endx, which, v); break;
    case 0x344: break;
    default: g_raw[((u32)core * 0x400 + r) >> 1] = v; break;
    }
}

/* 0x760-0x7AF: per-core volumes (0x28 bytes each) */
static u16 *vol_reg(u32 off, Core **cp)
{
    Core *c = &g_core[(off - 0x760) / 0x28];
    *cp = c;
    u32 r = (off - 0x760) % 0x28;
    switch (r) {
    case 0x00: return &c->mvol[0];
    case 0x02: return &c->mvol[1];
    case 0x04: return (u16 *)&c->evol[0];
    case 0x06: return (u16 *)&c->evol[1];
    case 0x08: return (u16 *)&c->avol[0];
    case 0x0A: return (u16 *)&c->avol[1];
    case 0x0C: return (u16 *)&c->bvol[0];
    case 0x0E: return (u16 *)&c->bvol[1];
    case 0x10: case 0x12: return NULL;   /* MVOLX: current master volume */
    default: return (u16 *)&c->rvol[(r - 0x14) >> 1];
    }
}

u16 Spu2_Read(u32 off)
{
    Spu2_Advance(SndSys_TimeNs());
    off &= 0x7FE;
    if (off < 0x760)
        return core_read(&g_core[off >> 10], (int)(off >> 10), off & 0x3FF);
    if (off < 0x7B0) {
        Core *c;
        u16 *p = vol_reg(off, &c);
        if (p)
            return *p;
        return (u16)c->mvolx[((off - 0x760) % 0x28) == 0x12].level;
    }
    if (off == 0x7C2)
        return g_irqinfo;
    return g_raw[off >> 1];
}

void Spu2_Write(u32 off, u16 v)
{
    Spu2_Advance(SndSys_TimeNs());
    off &= 0x7FE;
    if (off < 0x760) {
        core_write(&g_core[off >> 10], (int)(off >> 10), off & 0x3FF, v);
        return;
    }
    if (off < 0x7B0) {
        Core *c;
        u16 *p = vol_reg(off, &c);
        if (p)
            *p = v;
        return;
    }
    if (off == 0x7C2) {
        g_irqinfo = v;
        return;
    }
    g_raw[off >> 1] = v;
}

/* ------------------------------------------------------------ setup */

void Spu2_Init(void)
{
    snd_memset(g_core, 0, sizeof(g_core));
    snd_memset(g_raw, 0, sizeof(g_raw));
    g_irqinfo = 0;
    for (int c = 0; c < 2; c++) {
        g_core[c].eea = RAM_MASK;
        g_core[c].noise = 1;
    }
    if (g_on)
        return;
    g_on = 1;
    g_last_ns = SndSys_TimeNs();
    g_rem = 0;
    SndSys_EventAdd(SndSys_TimeNs() + TICK_NS, tick, 0);
    SND_LOGI("spu2", "SPU2 on: 48 kHz output");
}

void Spu2_TakeInterval(u64 now_ns, u64 *samples, u64 *hash)
{
    Spu2_Advance(now_ns);
    *samples = g_seam_n;
    *hash = g_seam_h;
    g_seam_h = FNV_INIT;
    g_seam_n = 0;
}

void Spu2_Stats(void)
{
    if (!g_on)
        return;
    for (int c = 0; c < 2; c++) {
        Core *k = &g_core[c];
        int active = 0;
        for (int i = 0; i < NV; i++)
            active += k->v[i].phase != ADSR_OFF;
        SND_LOGI("spu2", "core %d: %u key-ons, %d voices sounding, %u DMAs, %u AutoDMA blocks (%u MEMIN underruns); "
                 "ATTR %04X MMIX %03X MVOL %04X/%04X EVOL %04X AVOL %04X BVOL %04X; peaks: voices %d, MEMIN %d, ext %d, out %d",
                 c, k->n_keyon, active, k->n_dma, k->n_adma_blocks, k->n_underruns, k->attr, k->mmix, k->mvol[0], k->mvol[1],
                 (u16)k->evol[0], (u16)k->avol[0], (u16)k->bvol[0], k->peak_voices, k->peak_in, k->peak_ext, k->peak_out);
    }
}
