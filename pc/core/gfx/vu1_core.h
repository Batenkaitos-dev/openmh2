/* VU1 core, shared by the interpreter (gfx_vu1.c) and the microprograms that
 * tools/pc/vu1rec.py compiles ahead of time from the decomp's
 * src/sub_main/vu1_microcode.vsm (pc/core/gfx/sources.cmake).
 *
 * The arithmetic is in pure functions on register values (vu1_fmac_calc()
 * and the like, "operations" below). The interpreter executes one
 * instruction pair with vu1_step(), given its decoded form (a Dec), which
 * keeps the pipeline state in g_vu1. The compiled microprograms call the
 * same functions with constant operation codes, keep the registers in C
 * locals and have their pipeline timing worked out at build time (see
 * vu1rec.py); --vu1-check compares both with the original's plain interpreter.
 *
 * Arithmetic follows the original's VU0 code (PCSX2 clamp mode 3; rounding toward zero, FTZ
 * and DAZ come from the guest thread's MXCSR). Timing is modelled where
 * results depend on it:
 *   - FMAC results are ready 4 cycles after issue; readers stall until then;
 *   - DIV/SQRT/RSQRT update Q after 7/7/13 cycles and the EFU ops update P
 *     after their latency; readers see the old value until then, and
 *     WAITQ/WAITP or a second op on a busy unit stall;
 *   - MAC/status/clip flags reach the flag instructions 4 cycles after the
 *     op that produced them;
 *   - a conditional branch or jump right after an integer op that wrote its
 *     register reads the value from before that write (PCSX2's "branch VI
 *     delay", from hardware tests).
 * Both halves of an instruction pair read their operands before either
 * writes. When both write the same VF register, the lower one is discarded
 * (PCSX2's interpreter does the same). XGKICK sends the packet to PATH1 at
 * once. The vector math runs on SSE2, four lanes at a time. */
#ifndef VU1_CORE_H
#define VU1_CORE_H

#include "gfx_rt.h"

/* The host's sqrtf and fabsf as the original's compiler emits them: SSE's
 * square root (rounded as MXCSR says: the run sets the guest's, toward zero)
 * and the sign bit cleared. */
static inline float vu1_sqrtf(float x) { return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(x))); }
static inline float vu1_fabsf(float x)
{
    union { float f; u32 u; } v;
    v.f = x;
    v.u &= 0x7FFFFFFFu;
    return v.f;
}

#define VU1_CODE_MASK 0x3FFFu   /* 16 KiB micro memory */
#define VU1_DATA_MASK 0x3FFFu   /* 16 KiB data memory */
#define VU1_RUN_CYCLE_LIMIT 50000000ull

#if defined(__GNUC__) || defined(__clang__)
#define VU1_INLINE static inline __attribute__((always_inline))
#else
#define VU1_INLINE static inline
#endif

/* ------------------------------------------------------------ state */

typedef struct Vu1Flag {
    u64 ready;
    u32 mac, zsuo, clip;
    u8 has_mac, has_clip;
} Vu1Flag;

/* One executed pair, for the branch VI delay. */
typedef struct Vu1Hist {
    int reg;        /* VI register the lower op wrote, or -1 */
    int flagread;   /* the writer was a flag instruction (FSAND, FMAND, ...) */
    u32 old;        /* value before the write */
    u32 stall;      /* stall cycles before the pair issued */
} Vu1Hist;

typedef struct Vu1State {
    VF vf[32];
    VF acc;
    u32 vi[16];
    u32 q, p, i, r;
    u32 mac, status, clip;   /* as seen by the flag instructions */
    u32 clip_last;           /* newest CLIP result, shifted by the next CLIP */
    u32 tpc;
    u64 cycle;
    u64 vf_ready[32];        /* [0] stays 0: vf0 is never written */
    u64 q_ready, p_ready;    /* 0 = unit idle */
    u64 due;                 /* earliest pending flag/Q/P update (UINT64_MAX: none) */
    u32 q_next, q_flags, p_next;
    Vu1Flag fq[16];          /* ring, oldest at fq_head; ready times ascend */
    int fq_head, fq_n;
    Vu1Hist hist[4];         /* ring, newest at hist_pos */
    int hist_pos;
    int branch_pending;
    u32 branch_target;
    u64 runs, pairs;
} Vu1State;

extern Vu1State g_vu1;
extern __m128i g_vu1_lanes[16];   /* lane i all-ones where bit i is set */
extern int g_vu1_kick_hashing;    /* --vu1-check: hash GIF output */
extern u64 g_vu1_kick_hash;

/* A run in progress: where it started, whether the E bit was seen, and a
 * number that is different for every run (micro memory cannot change
 * during a run, so compiled code checks it once per run). */
typedef struct Vu1Run {
    u64 start;
    int ending;
    u64 serial;
} Vu1Run;

static inline u8 *vu1_code_mem(void) { return g_vu_mem + 0x8000; }
static inline u8 *vu1_data_mem(void) { return g_vu_mem + 0xC000; }

/* ------------------------------------------------------------ decoded pairs */

enum { UK_NOP, UK_FMAC, UK_OPMULA, UK_OPMSUB, UK_ITOF, UK_FTOI, UK_ABS, UK_CLIP, UK_UNDEF };
enum { OP_ADD, OP_SUB, OP_MUL, OP_MADD, OP_MSUB, OP_MAX, OP_MIN };
enum {
    LK_NONE, LK_UNDEF, LK_LQ, LK_SQ, LK_ILW, LK_ISW, LK_IADDIU, LK_ISUBIU, LK_FCEQ, LK_FCSET, LK_FCAND, LK_FCOR,
    LK_FSEQ, LK_FSSET, LK_FSAND, LK_FSOR, LK_FMEQ, LK_FMAND, LK_FMOR, LK_FCGET, LK_B, LK_BAL, LK_JR, LK_JALR,
    LK_IBEQ, LK_IBNE, LK_IBLTZ, LK_IBGTZ, LK_IBLEZ, LK_IBGEZ, LK_IADD, LK_ISUB, LK_IADDI, LK_IAND, LK_IOR,
    LK_MOVE, LK_MR32, LK_LQI, LK_LQD, LK_SQI, LK_SQD, LK_DIV, LK_SQRT, LK_RSQRT, LK_WAIT, LK_MTIR, LK_MFIR,
    LK_ILWR, LK_ISWR, LK_RNEXT, LK_RGET, LK_RINIT, LK_RXOR, LK_MFP, LK_XTOP, LK_XITOP, LK_XGKICK, LK_EFU,
};

/* The field order is part of the generated code's contract (vu1rec.py
 * writes these initialisers positionally). */
typedef struct Dec {
    u32 lo, up;
    u8 valid;
    u8 ibit, ebit;
    /* upper */
    u8 uk, uop, uacc, udm, ufs, uft, ufd;
    s8 ubsrc;               /* -1 vector ft, 0..3 broadcast field, 4 Q, 5 I */
    u8 ur1, ur2;            /* VF read, for stalls (0 = none) */
    /* lower */
    u8 lk, ldm, lfs, lft, lfd, lis, lit, lid, lfsf, lftf;
    u8 lr1, lr2, lw, lwq, lwp;
    u32 limm;
} Dec;

void vu1_decode(Dec *d, u32 lo, u32 up);

/* Out-of-line parts (gfx_vu1.c): rare or long operations. */
typedef struct UpRes UpRes;
void vu1_opm(const Dec *d, int msub, UpRes *o);   /* OPMULA/OPMSUB, per lane */
void vu1_clip(const Dec *d);
void vu1_vdiv(const Dec *d, int kind);
void vu1_efu(const Dec *d);
void vu1_xgkick(u32 a);
int vu1_lower_undefined(const Dec *d, u32 pc);
void vu1_upper_undefined(const Dec *d);
/* OPMULA (msub 0) / OPMSUB on register values: the xyz result in *out,
 * returns the MAC flags. */
u32 vu1_opm_calc(int msub, const VF *s, const VF *t, const VF *acc, VF *out);
/* EFU operation op (extended opcode 0x70-0x7E) on fs, fsf: the P result. */
u32 vu1_efu_calc(u32 op, const VF *s, u32 fsf);
u32 vu1_efu_latency(u32 op);

/* ------------------------------------------------------------ SSE2 helpers */

typedef __m128i V4;

static const u8 vu1_rev4[16] = {0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15};

static inline V4 vload(const void *p) { return _mm_loadu_si128((const __m128i *)p); }
static inline void vstore(void *p, V4 x) { _mm_storeu_si128((__m128i *)p, x); }
static inline __m128 vf(V4 x) { return _mm_castsi128_ps(x); }
static inline V4 vi(__m128 x) { return _mm_castps_si128(x); }
static inline V4 vsel(V4 m, V4 a, V4 b) { return _mm_or_si128(_mm_and_si128(m, a), _mm_andnot_si128(m, b)); }
static inline u32 vmask(V4 x) { return (u32)_mm_movemask_ps(vf(x)); }   /* bit i = lane i sign */
/* Merge lanes of x selected by dm (bit i = lane i) into *p. */
static inline void vstore_lanes(void *p, V4 x, u32 dm)
{
    if (dm == 15)
        vstore(p, x);
    else
        vstore(p, vsel(g_vu1_lanes[dm], x, vload(p)));
}

/* fin/fout on four lanes: exponent 0 -> +/-0, exponent 255 -> +/-Fmax. */
static inline V4 vclamp(V4 x)
{
    const V4 exp = _mm_set1_epi32(0x7F800000), sgn = _mm_set1_epi32((int)0x80000000u);
    const V4 fmax = _mm_set1_epi32((int)FMAX_BITS);
    V4 e = _mm_and_si128(x, exp), s = _mm_and_si128(x, sgn);
    V4 z = _mm_cmpeq_epi32(e, _mm_setzero_si128()), n = _mm_cmpeq_epi32(e, exp);
    return _mm_or_si128(_mm_andnot_si128(_mm_or_si128(z, n), x), _mm_or_si128(s, _mm_and_si128(n, fmax)));
}

/* vclamp() of the result of an add, subtract or multiply of clamped
 * (finite) operands, in fewer and shorter-latency operations. Such a result
 * is never a NaN, in any rounding mode: only an infinity or a denormal can
 * change. A denormal (exponent 0) keeps its sign bit only; an infinity is
 * s|0x7F800000, and one less is s|0x7F7FFFFF = +/-Fmax. In lanes with
 * other values the result is some value. */
static inline V4 vclamp_res(V4 x)
{
    const V4 exp = _mm_set1_epi32(0x7F800000);
    V4 e = _mm_and_si128(x, exp);
    V4 keep = _mm_or_si128(_mm_cmpgt_epi32(e, _mm_setzero_si128()), _mm_set1_epi32((int)0x80000000u));
    return _mm_add_epi32(_mm_and_si128(x, keep), _mm_cmpeq_epi32(e, exp));
}

/* MAC flags of four results (lanes in dm): zero, sign, underflow, overflow. */
static inline u32 vmac(V4 r, V4 under, u32 dm)
{
    const V4 absm = _mm_set1_epi32(0x7FFFFFFF), fmax = _mm_set1_epi32((int)FMAX_BITS);
    V4 a = _mm_and_si128(r, absm);
    u32 z = vmask(_mm_cmpeq_epi32(a, _mm_setzero_si128())) & dm;
    u32 s = vmask(r) & dm;
    u32 u = vmask(under) & dm;
    u32 o = vmask(_mm_cmpeq_epi32(a, fmax)) & dm;
    return vu1_rev4[z] | (u32)vu1_rev4[s] << 4 | (u32)vu1_rev4[u] << 8 | (u32)vu1_rev4[o] << 12;
}

/* Status bits 0-3 (zero, sign, underflow, overflow) of MAC flags. */
static inline u32 vu1_zsuo(u32 mac)
{
    return (u32)((mac & 0x000F) != 0) | (u32)((mac & 0x00F0) != 0) << 1 | (u32)((mac & 0x0F00) != 0) << 2 |
           (u32)((mac & 0xF000) != 0) << 3;
}

/* ------------------------------------------------------------ operations */

/* FMAC arithmetic on clamped operands (vclamp: the fin of every lane): the
 * clamped result, and in *under the lanes that underflowed. cacc, the
 * clamped ACC, is only read by MADD/MSUB. MAX/MIN return one of their
 * operands, so their result needs no clamp; they set no flags and leave
 * *under alone. Lanes never mix: compiled code only clamps the operand
 * lanes whose results it keeps. */
VU1_INLINE V4 vu1_fmac_calc(int op, V4 cs, V4 ct, V4 cacc, V4 *under)
{
    const __m128 zero = _mm_setzero_ps(), sgn = _mm_castsi128_ps(_mm_set1_epi32((int)0x80000000u));
    __m128 fs = vf(cs), ft = vf(ct), r;
    switch (op) {
    case OP_ADD:
        r = _mm_add_ps(fs, ft);
        *under = vi(_mm_and_ps(_mm_cmpeq_ps(r, zero), _mm_cmpneq_ps(fs, _mm_xor_ps(ft, sgn))));
        break;
    case OP_SUB:
        r = _mm_sub_ps(fs, ft);
        *under = vi(_mm_and_ps(_mm_cmpeq_ps(r, zero), _mm_cmpneq_ps(fs, ft)));
        break;
    case OP_MUL:
        r = _mm_mul_ps(fs, ft);
        *under = vi(_mm_and_ps(_mm_cmpeq_ps(r, zero), _mm_and_ps(_mm_cmpneq_ps(fs, zero), _mm_cmpneq_ps(ft, zero))));
        break;
    case OP_MADD:
    case OP_MSUB: {
        __m128 p = vf(vclamp_res(vi(_mm_mul_ps(fs, ft))));
        __m128 pu = _mm_and_ps(_mm_cmpeq_ps(p, zero), _mm_and_ps(_mm_cmpneq_ps(fs, zero), _mm_cmpneq_ps(ft, zero)));
        __m128 fc = vf(cacc);
        if (op == OP_MADD) {
            r = _mm_add_ps(fc, p);
            *under = vi(_mm_or_ps(pu, _mm_and_ps(_mm_cmpeq_ps(r, zero), _mm_cmpneq_ps(fc, _mm_xor_ps(p, sgn)))));
        } else {
            r = _mm_sub_ps(fc, p);
            *under = vi(_mm_or_ps(pu, _mm_and_ps(_mm_cmpeq_ps(r, zero), _mm_cmpneq_ps(fc, p))));
        }
        break;
    }
    case OP_MAX:   /* fs >= ft ? fs : ft (max_ps returns its second operand on ties) */
        return vi(_mm_max_ps(ft, fs));
    default:       /* OP_MIN: fs <= ft ? fs : ft */
        return vi(_mm_min_ps(ft, fs));
    }
    return vclamp_res(vi(r));
}

/* ITOF0/4/12/15 (k = 0..3) of raw integers. */
VU1_INLINE V4 vu1_itof_calc(int k, V4 s)
{
    static const float sc[4] = {1.0f, 1.0f / 16, 1.0f / 4096, 1.0f / 32768};
    return vclamp(vi(_mm_mul_ps(_mm_cvtepi32_ps(s), _mm_set1_ps(sc[k]))));
}

/* FTOI0/4/12/15 (k = 0..3) of clamped floats. */
VU1_INLINE V4 vu1_ftoi_calc(int k, V4 cs)
{
    static const float sc[4] = {1.0f, 16.0f, 4096.0f, 32768.0f};
    __m128 v = _mm_mul_ps(vf(cs), _mm_set1_ps(sc[k]));
    V4 r = _mm_cvttps_epi32(v);   /* out of range: 0x80000000 */
    V4 big = vi(_mm_cmpge_ps(v, _mm_set1_ps(2147483648.0f)));
    return vsel(big, _mm_set1_epi32(0x7FFFFFFF), r);
}

/* CLIP fs.xyz, ft.w on clamped operands: the new clip flags, the previous
 * ones (clip_last) shifted up by 6. Lane i sets bit 2i when above +|w| and
 * bit 2i+1 when below -|w|. */
VU1_INLINE u32 vu1_clip_calc(V4 cs, V4 ct, u32 clip_last)
{
    const __m128 absm = _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF));
    const __m128 sgn = _mm_castsi128_ps(_mm_set1_epi32((int)0x80000000u));
    __m128 w = _mm_and_ps(vf(_mm_shuffle_epi32(ct, 0xFF)), absm);
    u32 gt = (u32)_mm_movemask_ps(_mm_cmpgt_ps(vf(cs), w)) & 7;
    u32 lt = (u32)_mm_movemask_ps(_mm_cmplt_ps(vf(cs), _mm_xor_ps(w, sgn))) & 7;
    u32 bits = (gt & 1) | (gt & 2) << 1 | (gt & 4) << 2;
    bits |= ((lt & 1) | (lt & 2) << 1 | (lt & 4) << 2) << 1;
    return ((clip_last << 6) & 0xFFFFFFu) | bits;
}

/* DIV (kind 0), SQRT (1), RSQRT (2) of fs.fsf = s, ft.ftf = t: the new Q
 * and the status bits it sets (0x10 invalid, 0x20 divide by zero). */
static inline u32 vu1_div_calc(int kind, u32 s, u32 t, u32 *flags)
{
    u32 fl = 0, q;
    if (kind == 1) {   /* SQRT */
        if (t & 0x80000000u)
            fl |= 0x10;
        q = (t & 0x7F800000u) == 0 ? 0 : fout(vu1_sqrtf(vu1_fabsf(fin(t))));
    } else {
        float den = kind == 0 ? fin(t) : vu1_sqrtf(vu1_fabsf(fin(t)));
        if (kind == 2 && (t & 0x80000000u))
            fl |= 0x10;
        if ((t & 0x7F800000u) == 0) {
            fl |= (s & 0x7F800000u) == 0 ? 0x10 : 0x20;   /* 0/0 invalid, x/0 divide by zero */
            q = ((s ^ (kind == 0 ? t : 0)) & 0x80000000u) | FMAX_BITS;
        } else {
            q = fout(fin(s) / den);
        }
    }
    *flags = fl;
    return q;
}

/* ------------------------------------------------------------ pipeline */

void vu1_advance_slow(u64 to);

/* Bring the clock to `to`, applying flag/Q/P results that are due. */
static inline void vu1_advance(u64 to)
{
    g_vu1.cycle = to;
    if (to >= g_vu1.due)
        vu1_advance_slow(to);
}

static inline void vu1_push_flags(u32 mac, int has_mac, u32 clip, int has_clip)
{
    Vu1State *v = &g_vu1;
    if (v->fq_n == 16)
        vu1_advance_slow(v->fq[v->fq_head].ready);   /* cannot happen with 4-cycle latency; keep order anyway */
    Vu1Flag *e = &v->fq[(v->fq_head + v->fq_n) & 15];
    v->fq_n++;
    e->ready = v->cycle + 4;
    e->mac = mac;
    e->zsuo = vu1_zsuo(mac);
    e->clip = clip;
    e->has_mac = (u8)has_mac;
    e->has_clip = (u8)has_clip;
    if (e->ready < v->due)
        v->due = e->ready;
}

/* ------------------------------------------------------------ upper */

/* Pending upper result, written after the lower op of the pair. */
struct UpRes {
    int reg;        /* VF register, 32 = ACC, -1 = none */
    u32 dm;         /* lanes (bit i = lane i) */
    V4 v;
};

VU1_INLINE void vu1_fmac(const Dec *d, UpRes *o)
{
    Vu1State *vu = &g_vu1;
    V4 s = vclamp(vload(&vu->vf[d->ufs]));
    V4 t;
    switch (d->ubsrc) {
    case -1: t = vload(&vu->vf[d->uft]); break;
    case 4: t = _mm_set1_epi32((int)vu->q); break;
    case 5: t = _mm_set1_epi32((int)vu->i); break;
    default: t = _mm_set1_epi32((int)vu->vf[d->uft].u[d->ubsrc]); break;
    }
    t = vclamp(t);
    int acc = d->uop == OP_MADD || d->uop == OP_MSUB;
    V4 under;
    o->reg = d->uacc ? 32 : d->ufd;
    o->dm = d->udm;
    o->v = vu1_fmac_calc(d->uop, s, t, acc ? vclamp(vload(&vu->acc)) : _mm_setzero_si128(), &under);
    if (d->uop != OP_MAX && d->uop != OP_MIN)
        vu1_push_flags(vmac(o->v, under, d->udm), 1, 0, 0);
}

VU1_INLINE void vu1_upper(const Dec *d, UpRes *o)
{
    Vu1State *vu = &g_vu1;
    o->reg = -1;
    switch (d->uk) {
    case UK_FMAC: vu1_fmac(d, o); return;
    case UK_OPMULA: vu1_opm(d, 0, o); return;
    case UK_OPMSUB: vu1_opm(d, 1, o); return;
    case UK_ITOF:
        o->v = vu1_itof_calc(d->uop, vload(&vu->vf[d->ufs]));
        o->reg = d->uft;
        o->dm = d->udm;
        return;
    case UK_FTOI:
        o->v = vu1_ftoi_calc(d->uop, vclamp(vload(&vu->vf[d->ufs])));
        o->reg = d->uft;
        o->dm = d->udm;
        return;
    case UK_ABS:
        o->v = _mm_and_si128(vclamp(vload(&vu->vf[d->ufs])), _mm_set1_epi32(0x7FFFFFFF));
        o->reg = d->uft;
        o->dm = d->udm;
        return;
    case UK_CLIP: vu1_clip(d); return;
    case UK_NOP: return;
    default: vu1_upper_undefined(d); return;
    }
}

/* ------------------------------------------------------------ lower */

static inline u8 *vu1_qword(u32 a) { return vu1_data_mem() + ((a * 16) & VU1_DATA_MASK); }
static inline Vu1Hist *vu1_hist_at(int i) { return &g_vu1.hist[(g_vu1.hist_pos - i) & 3]; }

static inline void vu1_set_vi(int reg, u32 v, Vu1Hist *h, int flagread)
{
    if (!reg)
        return;
    h->reg = reg;
    h->old = g_vu1.vi[reg];
    h->flagread = flagread;
    g_vu1.vi[reg] = v & 0xFFFF;
}

/* Value a branch sees for VI[reg] (see the header comment). */
static inline u32 vu1_branch_vi(int reg, u32 own_stall)
{
    u32 cur = g_vu1.vi[reg];
    if (!reg || own_stall)
        return cur;
    if (vu1_hist_at(0)->reg != reg || vu1_hist_at(0)->flagread)
        return cur;
    int j = 0;
    u32 cyc = 0;
    for (int i = 0; i < 4 && cyc < 4; i++) {
        Vu1Hist *h = vu1_hist_at(i);
        if (h->reg == reg) {
            if (h->flagread)
                break;
            j = i;
        }
        cyc += h->stall + 1;
    }
    return vu1_hist_at(j)->old;
}


/* Execute a lower op. Returns 1 to stop the program (invalid op). */
VU1_INLINE int vu1_lower(const Dec *d, u32 pc, u32 own_stall, Vu1Hist *h)
{
    Vu1State *vu = &g_vu1;
    const int is = d->lis, it = d->lit;
    switch (d->lk) {
    case LK_LQ:
        if (d->lft)
            vstore_lanes(&vu->vf[d->lft], vload(vu1_qword(vu->vi[is] + d->limm)), d->ldm);
        return 0;
    case LK_SQ:
        vstore_lanes(vu1_qword(vu->vi[it] + d->limm), vload(&vu->vf[d->lfs]), d->ldm);
        return 0;
    case LK_ILW:
    case LK_ILWR: {
        const u8 *p = vu1_qword(vu->vi[is] + (d->lk == LK_ILW ? d->limm : 0));
        for (int i = 0; i < 4; i++)
            if (d->ldm & (1u << i)) {
                u32 v;
                memcpy(&v, p + 4 * i, 4);
                vu1_set_vi(it, v, h, 0);
                break;
            }
        return 0;
    }
    case LK_ISW: vstore_lanes(vu1_qword(vu->vi[is] + d->limm), _mm_set1_epi32((int)vu->vi[it]), d->ldm); return 0;
    case LK_ISWR: vstore_lanes(vu1_qword(vu->vi[is]), _mm_set1_epi32((int)vu->vi[it]), d->ldm); return 0;
    case LK_IADDIU: vu1_set_vi(it, vu->vi[is] + d->limm, h, 0); return 0;
    case LK_ISUBIU: vu1_set_vi(it, vu->vi[is] - d->limm, h, 0); return 0;
    case LK_FCEQ: vu1_set_vi(1, (vu->clip & 0xFFFFFF) == (d->lo & 0xFFFFFF), h, 1); return 0;
    case LK_FCSET: vu->clip = vu->clip_last = d->lo & 0xFFFFFF; return 0;
    case LK_FCAND: vu1_set_vi(1, (vu->clip & d->lo & 0xFFFFFF) != 0, h, 1); return 0;
    case LK_FCOR: vu1_set_vi(1, ((vu->clip | d->lo) & 0xFFFFFF) == 0xFFFFFF, h, 1); return 0;
    case LK_FSEQ: vu1_set_vi(it, (vu->status & 0xFFF) == d->limm, h, 1); return 0;
    case LK_FSSET: vu->status = (vu->status & 0x3F) | (d->limm & 0xFC0); return 0;
    case LK_FSAND: vu1_set_vi(it, vu->status & d->limm, h, 1); return 0;
    case LK_FSOR: vu1_set_vi(it, (vu->status | d->limm) & 0xFFF, h, 1); return 0;
    case LK_FMEQ: vu1_set_vi(it, (vu->mac & 0xFFFF) == (vu->vi[is] & 0xFFFF), h, 1); return 0;
    case LK_FMAND: vu1_set_vi(it, vu->mac & vu->vi[is], h, 1); return 0;
    case LK_FMOR: vu1_set_vi(it, (vu->mac | vu->vi[is]) & 0xFFFF, h, 1); return 0;
    case LK_FCGET: vu1_set_vi(it, vu->clip & 0xFFF, h, 1); return 0;
    case LK_B:
    case LK_BAL:
        if (d->lk == LK_BAL)
            vu1_set_vi(it, (pc + 16) / 8, h, 0);
        vu->branch_pending = 1;
        vu->branch_target = (pc + 8 + d->limm * 8) & VU1_CODE_MASK;
        return 0;
    case LK_JR:
    case LK_JALR: {
        u32 target = (vu1_branch_vi(is, own_stall) * 8) & VU1_CODE_MASK;
        if (d->lk == LK_JALR)
            vu1_set_vi(it, (pc + 16) / 8, h, 0);
        vu->branch_pending = 1;
        vu->branch_target = target;
        return 0;
    }
    case LK_IBEQ: case LK_IBNE: case LK_IBLTZ: case LK_IBGTZ: case LK_IBLEZ: case LK_IBGEZ: {
        s16 a = (s16)vu1_branch_vi(is, own_stall);
        s16 b = d->lk <= LK_IBNE ? (s16)vu1_branch_vi(it, own_stall) : 0;
        int take;
        switch (d->lk) {
        case LK_IBEQ: take = a == b; break;
        case LK_IBNE: take = a != b; break;
        case LK_IBLTZ: take = a < 0; break;
        case LK_IBGTZ: take = a > 0; break;
        case LK_IBLEZ: take = a <= 0; break;
        default: take = a >= 0; break;
        }
        if (take) {
            vu->branch_pending = 1;
            vu->branch_target = (pc + 8 + d->limm * 8) & VU1_CODE_MASK;
        }
        return 0;
    }
    case LK_IADD: vu1_set_vi(d->lid, vu->vi[is] + vu->vi[it], h, 0); return 0;
    case LK_ISUB: vu1_set_vi(d->lid, vu->vi[is] - vu->vi[it], h, 0); return 0;
    case LK_IADDI: vu1_set_vi(it, vu->vi[is] + d->limm, h, 0); return 0;
    case LK_IAND: vu1_set_vi(d->lid, vu->vi[is] & vu->vi[it], h, 0); return 0;
    case LK_IOR: vu1_set_vi(d->lid, vu->vi[is] | vu->vi[it], h, 0); return 0;
    case LK_MOVE:
        if (d->lft)
            vstore_lanes(&vu->vf[d->lft], vload(&vu->vf[d->lfs]), d->ldm);
        return 0;
    case LK_MR32:
        if (d->lft)
            vstore_lanes(&vu->vf[d->lft], _mm_shuffle_epi32(vload(&vu->vf[d->lfs]), _MM_SHUFFLE(0, 3, 2, 1)), d->ldm);
        return 0;
    case LK_LQI:
    case LK_LQD:
        if (d->lk == LK_LQD)
            vu1_set_vi(is, vu->vi[is] - 1, h, 0);
        if (d->lft)
            vstore_lanes(&vu->vf[d->lft], vload(vu1_qword(vu->vi[is])), d->ldm);
        if (d->lk == LK_LQI)
            vu1_set_vi(is, vu->vi[is] + 1, h, 0);
        return 0;
    case LK_SQI:
    case LK_SQD:
        if (d->lk == LK_SQD)
            vu1_set_vi(it, vu->vi[it] - 1, h, 0);
        vstore_lanes(vu1_qword(vu->vi[it]), vload(&vu->vf[d->lfs]), d->ldm);
        if (d->lk == LK_SQI)
            vu1_set_vi(it, vu->vi[it] + 1, h, 0);
        return 0;
    case LK_DIV: vu1_vdiv(d, 0); return 0;
    case LK_SQRT: vu1_vdiv(d, 1); return 0;
    case LK_RSQRT: vu1_vdiv(d, 2); return 0;
    case LK_WAIT: return 0;   /* WAITQ/WAITP: stalled before issue */
    case LK_MTIR: vu1_set_vi(it, vu->vf[d->lfs].u[d->lfsf], h, 0); return 0;
    case LK_MFIR:
        if (d->lft)
            vstore_lanes(&vu->vf[d->lft], _mm_set1_epi32((s32)(s16)vu->vi[is]), d->ldm);
        return 0;
    case LK_RNEXT:
    case LK_RGET:
        if (d->lk == LK_RNEXT) {
            u32 r = vu->r;
            u32 b = ((r >> 4) & 1) ^ ((r >> 22) & 1);
            vu->r = (((r << 1) | b) & 0x7FFFFFu) | 0x3F800000u;
        }
        if (d->lft)
            vstore_lanes(&vu->vf[d->lft], _mm_set1_epi32((int)vu->r), d->ldm);
        return 0;
    case LK_RINIT: vu->r = 0x3F800000u | (vu->vf[d->lfs].u[d->lfsf] & 0x7FFFFFu); return 0;
    case LK_RXOR: vu->r = 0x3F800000u | ((vu->r ^ vu->vf[d->lfs].u[d->lfsf]) & 0x7FFFFFu); return 0;
    case LK_MFP:
        if (d->lft)
            vstore_lanes(&vu->vf[d->lft], _mm_set1_epi32((int)vu->p), d->ldm);
        return 0;
    case LK_XTOP: vu1_set_vi(it, vif1_top(), h, 0); return 0;
    case LK_XITOP: vu1_set_vi(it, vif1_itop(), h, 0); return 0;
    case LK_XGKICK: vu1_xgkick(vu->vi[is]); return 0;
    case LK_EFU: vu1_efu(d); return 0;
    case LK_NONE: return 0;
    default: return vu1_lower_undefined(d, pc);
    }
}

/* ------------------------------------------------------------ one pair */

/* Execute the pair at pc (decoded as d). Sets TPC to the next pair and
 * returns 1 when the program has ended (E bit + delay slot, an invalid op,
 * or the run limit). */
VU1_INLINE int vu1_step(const Dec *d, u32 pc, Vu1Run *rc)
{
    Vu1State *vu = &g_vu1;
    /* Stalls: operands of both halves, and busy DIV/EFU units (vf_ready[0] is 0). */
    u64 at = vu->cycle;
    if (d->ur1 && vu->vf_ready[d->ur1] > at) at = vu->vf_ready[d->ur1];
    if (d->ur2 && vu->vf_ready[d->ur2] > at) at = vu->vf_ready[d->ur2];
    if (d->lr1 && vu->vf_ready[d->lr1] > at) at = vu->vf_ready[d->lr1];
    if (d->lr2 && vu->vf_ready[d->lr2] > at) at = vu->vf_ready[d->lr2];
    if (d->lwq && vu->q_ready > at) at = vu->q_ready;
    if (d->lwp && vu->p_ready > at) at = vu->p_ready;
    u32 stall = (u32)(at - vu->cycle);
    vu1_advance(at);

    Vu1Hist h = {-1, 0, 0, stall};
    int take = vu->branch_pending;
    u32 target = vu->branch_target;
    vu->branch_pending = 0;

    UpRes ures;
    vu1_upper(d, &ures);
    int stop = 0;
    if (d->ibit)
        vu->i = d->lo;
    else if (!(ures.reg > 0 && ures.reg < 32 && ures.reg == d->lw))
        stop = vu1_lower(d, pc, stall, &h);
    if (ures.reg == 32)
        vstore_lanes(&vu->acc, ures.v, ures.dm);
    else if (ures.reg > 0)
        vstore_lanes(&vu->vf[ures.reg], ures.v, ures.dm);
    if (ures.reg > 0 && ures.reg < 32)
        vu->vf_ready[ures.reg] = vu->cycle + 4;
    if (d->lw > 0)
        vu->vf_ready[d->lw] = vu->cycle + 4;

    vu->hist_pos = (vu->hist_pos + 1) & 3;
    vu->hist[vu->hist_pos] = h;
    vu->pairs++;
    vu->cycle++;
    vu->tpc = take ? target : (pc + 8) & VU1_CODE_MASK;

    if (rc->ending || stop)
        return 1;
    if (d->ebit)
        rc->ending = 1;
    if (vu->cycle - rc->start > VU1_RUN_CYCLE_LIMIT) {
        LOGW("vu1", "microprogram did not end within %llu cycles (TPC %04X); stopping it",
             (unsigned long long)VU1_RUN_CYCLE_LIMIT, vu->tpc);
        return 1;
    }
    return 0;
}

/* Compiled microprograms (the generated vu1_gen.c): run from TPC while the
 * code in micro memory is code it knows. Returns 1 when the program ended,
 * 0 at a pair it has no code for (TPC points there). */
int vu1_gen_exec(Vu1Run *rc);
/* Check the generated decodes against vu1_decode (--vu1-check). */
int vu1_gen_selfcheck(void);
/* Times compiled code handed a run over to the interpreter midway. */
extern u64 g_vu1_gen_bails;

/* ------------------------------------------------------------ compiled code
 * Helpers for the generated vu1_gen.c, which keeps the registers in locals
 * (V4 vf1..vf31, acc; u32 vi1..vi15). dm is always a constant there. */

#define VU1_VF0 _mm_set_epi32(0x3F800000, 0, 0, 0)
#define VU1_BC(x, lane) _mm_shuffle_epi32((x), (lane) * 0x55)
#define VU1_LANE(x, lane) ((u32)_mm_cvtsi128_si32(_mm_shuffle_epi32((x), (lane) * 0x55)))

static inline V4 vu1_mask(u32 dm)
{
    return _mm_set_epi32((dm & 8) ? -1 : 0, (dm & 4) ? -1 : 0, (dm & 2) ? -1 : 0, (dm & 1) ? -1 : 0);
}
static inline V4 vu1_lanes(V4 x, V4 old, u32 dm) { return dm == 15 ? x : vsel(vu1_mask(dm), x, old); }
static inline V4 vu1_ld(const u8 *mem, u32 a, V4 old, u32 dm) { return vu1_lanes(vload(mem + ((a * 16) & VU1_DATA_MASK)), old, dm); }
static inline void vu1_st(u8 *mem, u32 a, V4 x, u32 dm)
{
    u8 *p = mem + ((a * 16) & VU1_DATA_MASK);
    vstore(p, vu1_lanes(x, dm == 15 ? x : vload(p), dm));
}
static inline u32 vu1_lw(const u8 *mem, u32 a, int lane)
{
    u32 v;
    memcpy(&v, mem + ((a * 16) & VU1_DATA_MASK) + 4 * lane, 4);
    return v;
}

/* A MAC flag result travels as a payload of two vectors until it reaches
 * the flag registers: the result with the lanes outside dm set to 1.0 (a
 * value with no flags), and the underflow lanes within dm. vmac() of the
 * payload with all lanes gives the MAC flags; the sticky status bits
 * accumulate over payloads with a few vector operations (VU1_STICKY), and
 * the MAC flags are only computed where something reads them. */
static inline V4 vu1_flag_r(V4 r, u32 dm) { return dm == 15 ? r : vsel(vu1_mask(dm), r, _mm_set1_epi32(0x3F800000)); }
static inline V4 vu1_flag_u(V4 u, u32 dm) { return dm == 15 ? u : _mm_and_si128(u, vu1_mask(dm)); }
static inline u32 vu1_mac_of(V4 r, V4 u) { return vmac(r, u, 15); }

/* The payload of existing MAC flags (a lane never has both Z and O). */
static inline void vu1_mac_payload(u32 mac, V4 *r, V4 *u)
{
    VF rv, uv;
    for (int i = 0; i < 4; i++) {
        u32 b = 3 - (u32)i;   /* lane i is bit 3-i of each group */
        u32 mag = ((mac >> b) & 1) ? 0 : ((mac >> (12 + b)) & 1) ? FMAX_BITS : 0x3F800000u;
        rv.u[i] = mag | ((mac >> (4 + b)) & 1) << 31;
        uv.u[i] = ((mac >> (8 + b)) & 1) ? 0xFFFFFFFFu : 0;
    }
    *r = vload(&rv);
    *u = vload(&uv);
}

/* Sticky accumulators: sk0 the least |result|, sk1 the largest, sk2 the OR
 * of the results (sign), sk3 the OR of the underflow lanes. */
#define VU1_STICKY(r, u)                                                              \
    do {                                                                              \
        V4 a_ = _mm_and_si128((r), _mm_set1_epi32(0x7FFFFFFF));                       \
        sk0 = vi(_mm_min_ps(vf(sk0), vf(a_)));                                        \
        sk1 = vi(_mm_max_ps(vf(sk1), vf(a_)));                                        \
        sk2 = _mm_or_si128(sk2, (r));                                                 \
        sk3 = _mm_or_si128(sk3, (u));                                                 \
    } while (0)
#define VU1_STICKY_RESET()                                                            \
    do {                                                                              \
        sk0 = _mm_set1_epi32(0x3F800000);                                             \
        sk1 = sk2 = sk3 = _mm_setzero_si128();                                        \
    } while (0)

/* The branch VI history (Vu1Hist) as compiled code keeps it: the last 4
 * pairs in 12 bits each, newest lowest (VI register + 1 or 0, bit 5 the
 * flag-instruction mark, bits 6-11 the stall, saturated: any stall over 3
 * closes the window anyway), and their old values in o0-o3. */
static inline u64 vu1_gen_hist_get(u32 *o0, u32 *o1, u32 *o2, u32 *o3)
{
    u32 *o[4] = {o0, o1, o2, o3};
    u64 h = 0;
    for (int i = 3; i >= 0; i--) {
        const Vu1Hist *e = vu1_hist_at(i);
        h = h << 12 | (u32)((e->reg + 1) & 31) | (u32)(e->flagread != 0) << 5 | (e->stall < 63 ? e->stall : 63) << 6;
        *o[i] = e->old;
    }
    return h;
}

static inline void vu1_gen_hist_put(u64 h, u32 o0, u32 o1, u32 o2, u32 o3)
{
    const u32 o[4] = {o0, o1, o2, o3};
    g_vu1.hist_pos = 0;
    for (int i = 0; i < 4; i++) {
        u32 e = (u32)(h >> (12 * i)) & 0xFFF;
        g_vu1.hist[(-i) & 3] = (Vu1Hist){(int)(e & 31) - 1, (int)((e >> 5) & 1), o[i], e >> 6};
    }
}

/* vu1_branch_vi() for a branch that did not stall, on that history. */
static inline u32 vu1_gen_branch_vi(u32 reg, u32 cur, u64 h, u32 o0, u32 o1, u32 o2, u32 o3)
{
    if (((u32)h & 31) != reg + 1 || ((h >> 5) & 1))
        return cur;
    int j = 0;
    u32 cyc = 0;
    for (int i = 0; i < 4 && cyc < 4; i++) {
        u32 e = (u32)(h >> (12 * i)) & 0xFFF;
        if ((e & 31) == reg + 1) {
            if ((e >> 5) & 1)
                break;
            j = i;
        }
        cyc += (e >> 6) + 1;
    }
    return j == 0 ? o0 : j == 1 ? o1 : j == 2 ? o2 : o3;
}

/* Status bits 0-3 accumulated in sk0..sk3 (shifted to 6-9 when sticky). */
static inline u32 vu1_sticky(V4 mn, V4 mx, V4 s, V4 u)
{
    u32 z = vmask(_mm_cmpeq_epi32(mn, _mm_setzero_si128())) != 0;
    u32 sg = vmask(s) != 0;
    u32 un = vmask(u) != 0;
    u32 o = vmask(_mm_cmpeq_epi32(mx, _mm_set1_epi32((int)FMAX_BITS))) != 0;
    return z | sg << 1 | un << 2 | o << 3;
}

#endif
