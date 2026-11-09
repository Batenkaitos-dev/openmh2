#ifndef VU0OPS_H
#define VU0OPS_H

/*
 * VU0's macro-mode operations as C, for the native port: the C forms of the 42 assembly functions call these, one
 * call per VU instruction, over the global state g_vu0.
 *
 * The semantics are those of the original game's VU0 code, in ps2fp's
 * terms: add, subtract and multiply round toward zero, operands with
 * exponent 255 are +/-Fmax and denormals are zeros, results are clamped and
 * flushed, the product is clamped before a MADD adds it to ACC, and the MAC
 * and status flags are those of the original's VU0 code: the overflow flag is never set (a
 * rounded-toward-zero overflow is Fmax), and nor is the underflow flag (the
 * reference runs its host with FTZ and DAZ, so its vout never sees a
 * denormal). VDIV and VRSQRT divide TOWARD ZERO (its host division in round-toward-zero mode), not to
 * nearest like the FPU's div.s (ps2fp0_div). The functions are written with
 * the bit-level functions of ps2fp.h only, so no host rounding mode or
 * denormal mode matters.
 *
 * The instruction forms are numbered as the original's VU0 code decodes them (the enums below).
 * The VU data memory (VLQI, VSQI, VILWR, VISWR) is not provided: nothing the
 * game runs on VU0 uses it. QMTC2 copies a whole 128-bit GPR on the EE; here
 * a GPR is 32 bits, so lanes y, z and w of a QMTC2 target are (sign
 * extension of x, 0, 0): the code that uses QMTC2 reads x only (the C forms'
 * generator checks that).
 *
 * crt_vu0.c defines g_vu0 and vu0_exec_word (the decoder over a raw COP2 word, for
 * the tests against it).
 */

#include "../pc/core/crt/ps2fp.h"

typedef union {
    unsigned int u[4];
    int s[4];
    float f[4];
} Vu0Vec;

typedef struct {
    Vu0Vec vf[32]; /* vf0 is (0, 0, 0, 1.0f) and never written */
    Vu0Vec acc;
    unsigned int vi[32]; /* 0..15 the integer registers; 16 STATUS, 17 MAC, 18 CLIP, 20 R, 21 I, 22 Q */
} Vu0State;

extern Vu0State g_vu0;

enum { VU0_STATUS = 16, VU0_MAC = 17, VU0_CLIP = 18, VU0_R = 20, VU0_I = 21, VU0_Q = 22 };
/* destination masks: x = 8 ... w = 1 */
enum { VU0_X = 8, VU0_Y = 4, VU0_Z = 2, VU0_W = 1, VU0_XYZ = 14, VU0_XYZW = 15 };
enum { VU0_ADD, VU0_SUB, VU0_MUL, VU0_MADD, VU0_MSUB, VU0_MAX, VU0_MIN };
/* the second operand: the vector ft, a field of ft (0..3 = x..w), Q or I */
enum { VU0_BVEC = -1, VU0_BX = 0, VU0_BY = 1, VU0_BZ = 2, VU0_BW = 3, VU0_BQ = 4, VU0_BI = 5 };

#define VU0_INL static inline __attribute__((always_inline))

/* ------------------------------------------------------------------ flags */

/* The MAC bits of one written lane (i = 0..3 for x..w): zero and sign. */
VU0_INL unsigned int vu0_mac_bits(unsigned int v, int i)
{
    unsigned int bit = 8u >> i, m = 0;
    if ((v & 0x7FFFFFFFu) == 0)
        m |= bit;
    if (v & 0x80000000u)
        m |= bit << 4;
    return m;
}

VU0_INL void vu0_set_flags(unsigned int mac)
{
    unsigned int st = 0;
    g_vu0.vi[VU0_MAC] = mac;
    if (mac & 0x000Fu) st |= 1;
    if (mac & 0x00F0u) st |= 2;
    if (mac & 0x0F00u) st |= 4;
    if (mac & 0xF000u) st |= 8;
    g_vu0.vi[VU0_STATUS] = (g_vu0.vi[VU0_STATUS] & 0xFF0u) | st | (st << 6);
}

/* ------------------------------------------------------------- arithmetic */

/* The result of an FMAC operation. */
VU0_INL unsigned int vu0_alu(int op, unsigned int a, unsigned int b, unsigned int acc)
{
    switch (op) {
    case VU0_ADD: return ps2fp0_add(a, b);
    case VU0_SUB: return ps2fp0_sub(a, b);
    case VU0_MUL: return ps2fp0_mul(a, b);
    case VU0_MADD: return ps2fp0_add(acc, ps2fp0_mul(a, b));
    case VU0_MSUB: return ps2fp0_sub(acc, ps2fp0_mul(a, b));
    case VU0_MAX: return ps2fp0_le(b, a) ? ps2fp0_clean(a) : ps2fp0_clean(b);
    default: return ps2fp0_le(a, b) ? ps2fp0_clean(a) : ps2fp0_clean(b); /* VU0_MIN */
    }
}

/* The generic FMAC operation: fd (or ACC, to_acc) = fs op (ft | ft.field | Q | I) in the lanes of dest. */
VU0_INL void vu0_fmac(int op, int bsrc, int to_acc, unsigned int dest, int fd, int fs, int ft)
{
    Vu0Vec s = g_vu0.vf[fs], t = g_vu0.vf[ft], acc = g_vu0.acc, r;
    unsigned int mac = 0;
    r = to_acc ? g_vu0.acc : g_vu0.vf[fd];
    for (int i = 0; i < 4; i++) {
        if (!(dest & (8u >> i)))
            continue;
        unsigned int b = bsrc < 0 ? t.u[i] : bsrc < 4 ? t.u[bsrc] : bsrc == 4 ? g_vu0.vi[VU0_Q] : g_vu0.vi[VU0_I];
        r.u[i] = vu0_alu(op, s.u[i], b, acc.u[i]);
        if (op != VU0_MAX && op != VU0_MIN)
            mac |= vu0_mac_bits(r.u[i], i);
    }
    if (!to_acc && fd == 0)
        return; /* vf0 is constant */
    if (to_acc)
        g_vu0.acc = r;
    else
        g_vu0.vf[fd] = r;
    if (op != VU0_MAX && op != VU0_MIN)
        vu0_set_flags(mac);
}

/* VOPMULA (msub = 0: ACC.xyz = cross-product terms) and VOPMSUB (msub = 1: fd.xyz = ACC - terms). */
VU0_INL void vu0_opmula(int msub, int fd, int fs, int ft)
{
    static const int a[3] = {1, 2, 0}, b[3] = {2, 0, 1};
    Vu0Vec s = g_vu0.vf[fs], t = g_vu0.vf[ft], acc = g_vu0.acc, r;
    unsigned int mac = 0;
    r = msub ? g_vu0.vf[fd] : g_vu0.acc;
    for (int i = 0; i < 3; i++) {
        unsigned int p = vu0_alu(VU0_MUL, s.u[a[i]], t.u[b[i]], 0);
        r.u[i] = msub ? vu0_alu(VU0_SUB, acc.u[i], p, 0) : p;
        mac |= vu0_mac_bits(r.u[i], i);
    }
    if (msub && fd == 0)
        return;
    if (msub)
        g_vu0.vf[fd] = r;
    else
        g_vu0.acc = r;
    vu0_set_flags(mac);
}

/* ------------------------------------------------------------ Q: the divider */

/* Division toward zero (a nonzero-exponent divisor). */
VU0_INL unsigned int vu0_div_trunc(unsigned int a, unsigned int b)
{
    a = ps2fp0_clean(a);
    b = ps2fp0_clean(b);
    unsigned int sign = (a ^ b) & PS2FP_SIGN;
    unsigned int ea = (a >> 23) & 0xFF, eb = (b >> 23) & 0xFF;
    if (ea == 0)
        return sign;
    unsigned int ma = (a & 0x7FFFFFu) | 0x800000u, mb = (b & 0x7FFFFFu) | 0x800000u;
    int e = (int)ea - (int)eb + 127;
    if (ma < mb) {
        ma <<= 1;
        e--;
    }
    unsigned int r = ma - mb, q = 0;
    for (int i = 0; i < 3; i++) {
        r <<= 8;
        q = (q << 8) | (r / mb);
        r %= mb;
    }
    q |= 1u << 24;
    return ps2fp0_pack(sign, e, q >> 1);
}

/* VDIV Q, fs.fsf, ft.ftf (kind 0), VSQRT Q, ft.ftf (kind 1), VRSQRT Q, fs.fsf, ft.ftf (kind 2) */
VU0_INL void vu0_divider(int kind, int fs, int fsf, int ft, int ftf)
{
    unsigned int s = g_vu0.vf[fs].u[fsf], t = g_vu0.vf[ft].u[ftf];
    unsigned int st = g_vu0.vi[VU0_STATUS] & ~0x30u;
    if (kind == 1) {
        if (t & 0x80000000u)
            st |= 0x10 | 0x400;
        g_vu0.vi[VU0_Q] = (t & 0x7F800000u) == 0 ? 0 : ps2fp0_sqrt(t);
    } else {
        if (kind == 2 && (t & 0x80000000u))
            st |= 0x10 | 0x400;
        if ((t & 0x7F800000u) == 0) {
            if ((s & 0x7F800000u) == 0)
                st |= 0x10 | 0x400; /* 0/0: invalid */
            else
                st |= 0x20 | 0x800; /* x/0: divide by zero */
            g_vu0.vi[VU0_Q] = ((s ^ (kind == 0 ? t : 0)) & 0x80000000u) | PS2FP_FMAX;
        } else {
            g_vu0.vi[VU0_Q] = vu0_div_trunc(s, kind == 0 ? t : ps2fp0_sqrt(t));
        }
    }
    g_vu0.vi[VU0_STATUS] = st;
}
#define vu0_vdiv(fs, fsf, ft, ftf) vu0_divider(0, fs, fsf, ft, ftf)
#define vu0_vsqrt(ft, ftf) vu0_divider(1, 0, 0, ft, ftf)
#define vu0_vrsqrt(fs, fsf, ft, ftf) vu0_divider(2, fs, fsf, ft, ftf)

/* ------------------------------------------------------- moves, conversions */

VU0_INL void vu0_write_vf(int reg, const Vu0Vec *v, unsigned int dest)
{
    if (reg == 0)
        return;
    for (int i = 0; i < 4; i++)
        if (dest & (8u >> i))
            g_vu0.vf[reg].u[i] = v->u[i];
}

VU0_INL void vu0_move(unsigned int dest, int ft, int fs)
{
    Vu0Vec s = g_vu0.vf[fs];
    vu0_write_vf(ft, &s, dest);
}

VU0_INL void vu0_mr32(unsigned int dest, int ft, int fs)
{
    Vu0Vec s = g_vu0.vf[fs], r;
    r.u[0] = s.u[1];
    r.u[1] = s.u[2];
    r.u[2] = s.u[3];
    r.u[3] = s.u[0];
    vu0_write_vf(ft, &r, dest);
}

/* VITOFn (n = 0, 4, 12, 15): the integer as a float, toward zero, divided by 2^n. */
VU0_INL void vu0_itof(int n, unsigned int dest, int ft, int fs)
{
    Vu0Vec s = g_vu0.vf[fs], r;
    for (int i = 0; i < 4; i++) {
        unsigned int f = ps2fp_i2f(s.s[i]);
        r.u[i] = f == 0 ? 0 : f - ((unsigned int)n << 23);
    }
    vu0_write_vf(ft, &r, dest);
}

/* VFTOIn: the float times 2^n, truncated, saturating. */
VU0_INL void vu0_ftoi(int n, unsigned int dest, int ft, int fs)
{
    Vu0Vec s = g_vu0.vf[fs], r;
    for (int i = 0; i < 4; i++) {
        unsigned int c = ps2fp0_clean(s.u[i]);
        int e = (int)((c >> 23) & 0xFF);
        if (e == 0)
            r.u[i] = 0;
        else if (e + n >= 158)
            r.u[i] = (c & PS2FP_SIGN) ? 0x80000000u : 0x7FFFFFFFu;
        else
            r.u[i] = (unsigned int)ps2fp_f2i(c + ((unsigned int)n << 23));
    }
    vu0_write_vf(ft, &r, dest);
}

VU0_INL void vu0_abs(unsigned int dest, int ft, int fs)
{
    Vu0Vec s = g_vu0.vf[fs], r;
    for (int i = 0; i < 4; i++)
        r.u[i] = ps2fp0_clean(s.u[i]) & 0x7FFFFFFFu;
    vu0_write_vf(ft, &r, dest);
}

/* VCLIPw.xyz fs, ft */
VU0_INL void vu0_clip(int fs, int ft)
{
    Vu0Vec s = g_vu0.vf[fs];
    unsigned int w = ps2fp0_clean(g_vu0.vf[ft].u[3]) & 0x7FFFFFFFu;
    unsigned int c = (g_vu0.vi[VU0_CLIP] << 6) & 0xFFFFFFu;
    for (int i = 0; i < 3; i++) {
        if (ps2fp0_lt(w, s.u[i]))
            c |= 1u << (2 * i);
        if (ps2fp0_lt(s.u[i], w ^ PS2FP_SIGN))
            c |= 2u << (2 * i);
    }
    g_vu0.vi[VU0_CLIP] = c;
}

/* ------------------------------------------------------ the integer registers */

VU0_INL void vu0_mtir(int it, int fs, int fsf)
{
    if (it)
        g_vu0.vi[it] = g_vu0.vf[fs].u[fsf] & 0xFFFF;
}

VU0_INL void vu0_mfir(unsigned int dest, int ft, int is)
{
    Vu0Vec r;
    for (int i = 0; i < 4; i++)
        r.s[i] = (short)g_vu0.vi[is];
    vu0_write_vf(ft, &r, dest);
}

VU0_INL void vu0_iadd(int id, int is, int it)
{
    if (id)
        g_vu0.vi[id] = (g_vu0.vi[is] + g_vu0.vi[it]) & 0xFFFF;
}
VU0_INL void vu0_isub(int id, int is, int it)
{
    if (id)
        g_vu0.vi[id] = (g_vu0.vi[is] - g_vu0.vi[it]) & 0xFFFF;
}
VU0_INL void vu0_iand(int id, int is, int it)
{
    if (id)
        g_vu0.vi[id] = (g_vu0.vi[is] & g_vu0.vi[it]) & 0xFFFF;
}
VU0_INL void vu0_ior(int id, int is, int it)
{
    if (id)
        g_vu0.vi[id] = (g_vu0.vi[is] | g_vu0.vi[it]) & 0xFFFF;
}
/* VIADDI it, is, imm5 (imm: -16..15) */
VU0_INL void vu0_iaddi(int it, int is, int imm)
{
    if (it)
        g_vu0.vi[it] = (g_vu0.vi[is] + (unsigned int)imm) & 0xFFFF;
}

/* The random-number unit. */
VU0_INL void vu0_rnext(unsigned int dest, int ft)
{
    unsigned int r = g_vu0.vi[VU0_R];
    unsigned int x = (r >> 4) & 1, y = (r >> 22) & 1;
    Vu0Vec v;
    r = (r << 1) ^ x ^ y;
    g_vu0.vi[VU0_R] = (r & 0x7FFFFFu) | 0x3F800000u;
    for (int i = 0; i < 4; i++)
        v.u[i] = g_vu0.vi[VU0_R];
    vu0_write_vf(ft, &v, dest);
}
VU0_INL void vu0_rget(unsigned int dest, int ft)
{
    Vu0Vec v;
    for (int i = 0; i < 4; i++)
        v.u[i] = g_vu0.vi[VU0_R];
    vu0_write_vf(ft, &v, dest);
}
VU0_INL void vu0_rinit(int fs, int fsf) { g_vu0.vi[VU0_R] = 0x3F800000u | (g_vu0.vf[fs].u[fsf] & 0x7FFFFFu); }
VU0_INL void vu0_rxor(int fs, int fsf)
{
    g_vu0.vi[VU0_R] = 0x3F800000u | ((g_vu0.vi[VU0_R] ^ g_vu0.vf[fs].u[fsf]) & 0x7FFFFFu);
}

/* ------------------------------------------------------- COP2 moves and memory */

/* QMTC2 vf, gpr: the GPR is 32 bits here (see the top). */
VU0_INL void vu0_qmtc2(int vf, unsigned int v)
{
    if (vf) {
        g_vu0.vf[vf].u[0] = v;
        g_vu0.vf[vf].u[1] = (unsigned int)((int)v >> 31);
        g_vu0.vf[vf].u[2] = 0;
        g_vu0.vf[vf].u[3] = 0;
    }
}
/* QMFC2 gpr, vf: the low 32 bits. */
VU0_INL unsigned int vu0_qmfc2(int vf) { return g_vu0.vf[vf].u[0]; }

VU0_INL unsigned int vu0_cfc2(int reg)
{
    if (reg < 16)
        return g_vu0.vi[reg] & 0xFFFF;
    if (reg == 29)
        return 0; /* VPU_STAT: VU0 and VU1 idle */
    return g_vu0.vi[reg];
}
VU0_INL void vu0_ctc2(int reg, unsigned int v)
{
    if (reg == 0)
        return;
    if (reg < 16)
        g_vu0.vi[reg] = v & 0xFFFF;
    else if (reg == VU0_STATUS)
        g_vu0.vi[reg] = (v & 0xFC0u) | (g_vu0.vi[reg] & 0x3Fu);
    else if (reg == VU0_CLIP)
        g_vu0.vi[reg] = v & 0xFFFFFFu;
    else if (reg == 28 || reg == 29)
        return; /* VU resets: nothing to do in macro mode */
    else
        g_vu0.vi[reg] = v;
}

VU0_INL void vu0_lq(int vf, unsigned int addr)
{
    if (vf)
        __builtin_memcpy(&g_vu0.vf[vf], (const void *)(__SIZE_TYPE__)addr, 16);
}
VU0_INL void vu0_sq(int vf, unsigned int addr)
{
    __builtin_memcpy((void *)(__SIZE_TYPE__)addr, &g_vu0.vf[vf], 16);
}

/* ------------------------------------------- what the C forms of the assembly use */

/* Memory (guest addresses are host addresses in mh2-core), the FPU registers as bits, the 128-bit GPR forms. */
#define PC_U2F(u) ({ unsigned int u_ = (u); float f_; __builtin_memcpy(&f_, &u_, 4); f_; })
#define PC_F2U(f) ({ float f_ = (f); unsigned int u_; __builtin_memcpy(&u_, &f_, 4); u_; })
#define PC_LW(a) (*(unsigned int *)(__SIZE_TYPE__)(a))
#define PC_SW(a, v) (*(unsigned int *)(__SIZE_TYPE__)(a) = (v))
#define PC_LD(a) (*(unsigned long long *)(__SIZE_TYPE__)(a))
#define PC_SD(a, v) (*(unsigned long long *)(__SIZE_TYPE__)(a) = (unsigned long long)(unsigned int)(v))
#define PC_LWC1(a) (*(float *)(__SIZE_TYPE__)(a))
#define PC_SWC1(a, f) (*(float *)(__SIZE_TYPE__)(a) = (f))
#define PC_LQ(q, a) __builtin_memcpy((q), (const void *)(__SIZE_TYPE__)(a), 16)
#define PC_SQ(a, q) __builtin_memcpy((void *)(__SIZE_TYPE__)(a), (q), 16)
/* a register spilled to the stack by SQ (only its low 32 bits come back) */
#define PC_SQ32(a, v) do { unsigned int q_[4] = {(v), (unsigned int)((int)(v) >> 31), 0, 0}; PC_SQ((a), q_); } while (0)

/* One raw COP2 instruction word (the original's vu0_exec_word: the tests' reference decoder). */
void vu0_exec_word(unsigned int raw);

#endif /* VU0OPS_H */
