/* ps2dp: MWCC's software doubles, as plain C.
 *
 * The EE's FPU has single precision only, so the C library and the
 * compiler's runtime do every double operation in integer code: results
 * toward zero, denormals in and out are zeros, a float's exponent 255 is a
 * number (fptodp), and a double beyond the floats converts to 0x7F800000
 * (dptofp). A double is its 64 bits.
 *
 * Re-hosted from the earlier port's libgcc.c (dpadd, dpsub, fptodp, dptofp, litodp,
 * dptoli: written from the documented behaviour of GCC's soft-float
 * routines and black-box tests of the executable's; libgcc's source is GPL
 * and was never read) and libm_d.c (the multiply and divide the
 * C library's double functions call, from probes of the executable's). The
 * algorithms are unchanged; the guest registers are gone, and the 128-bit
 * product is done in 32-bit pieces (the core is i686).
 */
#ifndef PS2DP_H
#define PS2DP_H

#include "ps2fp.h"

#define PS2DP_SIGN 0x8000000000000000ull
#define PS2DP_EXP 0x7FF0000000000000ull
#define PS2DP_FRAC 0x000FFFFFFFFFFFFFull
#define PS2DP_QNAN 0x7FF8000000000000ull

/* A finite nonzero value sign x m x 2^(e - 1075 - 3), m with its leading
 * one at bit 55 (three guard bits below the 53 of a double), rounded
 * toward zero and packed; underflow gives a zero. */
PS2FP_INL ps2fp_u64 ps2dp_pack(ps2fp_u64 sign, int e, ps2fp_u64 m)
{
    if (e <= 0)
        return sign;
    m >>= 3;
    if (e >= 0x7FF)
        return sign | PS2DP_EXP;
    return sign | ((ps2fp_u64)e << 52) | (m & PS2DP_FRAC);
}

/* A float as the EE's FPU reads one (no infinities or NaNs; a denormal is a zero). */
PS2FP_INL ps2fp_u64 ps2dp_from_sf(ps2fp_u32 f)
{
    ps2fp_u64 sign = (ps2fp_u64)(f >> 31) << 63;
    int e = (int)((f >> 23) & 0xFF);
    ps2fp_u64 m = f & 0x7FFFFFu;
    if (e == 0)
        return sign;
    return sign | ((ps2fp_u64)(e - 127 + 1023) << 52) | (m << 29);
}

PS2FP_INL ps2fp_u32 ps2dp_to_sf(ps2fp_u64 d)
{
    ps2fp_u32 sign = (ps2fp_u32)(d >> 63) << 31;
    int e = (int)((d >> 52) & 0x7FF);
    ps2fp_u64 m = d & PS2DP_FRAC;
    if (e == 0x7FF)
        return sign | 0x7F800000u; /* an infinity or a NaN */
    if (e == 0)
        return sign; /* a zero or a denormal */
    m |= 1ull << 52;
    int fe = e - 1023 + 127;
    if (fe <= 0)
        return sign; /* below the normal floats: a zero */
    ps2fp_u64 q = m >> 29; /* toward zero */
    if (fe > 0xFF)
        return sign | 0x7F800000u;
    return sign | ((ps2fp_u32)fe << 23) | ((ps2fp_u32)q & 0x7FFFFFu);
}

PS2FP_INL ps2fp_u64 ps2dp_add(ps2fp_u64 a, ps2fp_u64 b)
{
    int ea = (int)((a >> 52) & 0x7FF), eb = (int)((b >> 52) & 0x7FF);
    ps2fp_u64 sa = a & PS2DP_SIGN, sb = b & PS2DP_SIGN;
    ps2fp_u64 ma = a & PS2DP_FRAC, mb = b & PS2DP_FRAC;
    if (ea == 0x7FF || eb == 0x7FF) {
        if (ea == 0x7FF && ma)
            return a | PS2DP_QNAN;
        if (eb == 0x7FF && mb)
            return b | PS2DP_QNAN;
        if (ea == 0x7FF && eb == 0x7FF && sa != sb)
            return PS2DP_QNAN; /* inf - inf: the default NaN */
        return ea == 0x7FF ? a : b;
    }
    /* a denormal is a zero */
    if (!ea) {
        if (!eb)
            return sa & sb; /* +0 unless both are -0 */
        return b;
    }
    if (!eb)
        return a;
    /* 56-bit significands (three guard bits) */
    ma |= 1ull << 52;
    mb |= 1ull << 52;
    ma <<= 3;
    mb <<= 3;
    if (ea < eb || (ea == eb && ma < mb)) { /* |a| >= |b| */
        ps2fp_u64 t = ma;
        ma = mb;
        mb = t;
        int te = ea;
        ea = eb;
        eb = te;
        t = sa;
        sa = sb;
        sb = t;
    }
    /* b aligned to a: its bits below the guard bits are lost; toward zero,
     * a sum drops them and a difference is one less when any was set */
    int d = ea - eb;
    ps2fp_u64 lost = 0;
    if (d > 0) {
        if (d >= 64) { /* too small to count at all (from 64 on) */
            mb = 0;
        } else if (d > 58) {
            lost = 1;
            mb = 0;
        } else {
            lost = (mb & ((1ull << d) - 1)) != 0;
            mb >>= d;
        }
    }
    ps2fp_u64 m;
    int e = ea;
    if (sa == sb) {
        m = ma + mb;
        if (m >> 56) {
            m >>= 1;
            e++;
        }
    } else {
        m = ma - mb - lost;
        if (!m)
            return 0; /* x - x: +0 */
        while (!(m >> 55)) {
            m <<= 1;
            e--;
        }
    }
    return ps2dp_pack(sa, e, m);
}

PS2FP_INL ps2fp_u64 ps2dp_sub(ps2fp_u64 a, ps2fp_u64 b) { return ps2dp_add(a, b ^ PS2DP_SIGN); }

PS2FP_INL ps2fp_u64 ps2dp_from_si(ps2fp_s32 v)
{
    if (!v)
        return 0;
    ps2fp_u64 sign = v < 0 ? PS2DP_SIGN : 0;
    ps2fp_u64 m = v < 0 ? (ps2fp_u64)(-(long long)v) : (ps2fp_u64)v;
    int e = 1023 + 52;
    while (!(m >> 52)) {
        m <<= 1;
        e--;
    }
    return sign | ((ps2fp_u64)e << 52) | (m & PS2DP_FRAC);
}

/* Toward zero; out of range: the int's limit of the value's sign. */
PS2FP_INL ps2fp_s32 ps2dp_to_si(ps2fp_u64 d)
{
    int e = (int)((d >> 52) & 0x7FF);
    ps2fp_u64 m = (d & PS2DP_FRAC) | (1ull << 52);
    int neg = (int)(d >> 63);
    if (e < 1023 || (e == 0x7FF && (d & PS2DP_FRAC))) /* |x| < 1, or a NaN */
        return 0;
    if (e >= 1023 + 31)
        return neg ? (ps2fp_s32)0x80000000u : 0x7FFFFFFF;
    ps2fp_u64 v = m >> (1075 - e);
    return neg ? -(ps2fp_s32)v : (ps2fp_s32)v;
}

/* An infinity or a NaN in a multiply or divide: IEEE's results (Capcom's
 * inputs never lead to one). */
PS2FP_INL ps2fp_u64 ps2dp_special(ps2fp_u64 a, ps2fp_u64 b, int div)
{
    int ea = (int)((a >> 52) & 0x7FF), eb = (int)((b >> 52) & 0x7FF);
    ps2fp_u64 sign = (a ^ b) & PS2DP_SIGN;
    if (ea == 0x7FF && (a & PS2DP_FRAC))
        return a | PS2DP_QNAN;
    if (eb == 0x7FF && (b & PS2DP_FRAC))
        return b | PS2DP_QNAN;
    if (!div)
        return (!ea || !eb) ? PS2DP_QNAN : sign | PS2DP_EXP; /* inf x 0, inf x y */
    if (ea == 0x7FF)
        return eb == 0x7FF ? PS2DP_QNAN : sign | PS2DP_EXP; /* inf / inf, inf / y */
    return sign; /* y / inf */
}

/* Rounded toward zero; a result below the normals is a zero, an overflow an infinity. */
PS2FP_INL ps2fp_u64 ps2dp_pack2(ps2fp_u64 sign, int e, ps2fp_u64 m)
{
    if (e <= 0)
        return sign;
    if (e >= 0x7FF)
        return sign | PS2DP_EXP;
    return sign | ((ps2fp_u64)e << 52) | (m & PS2DP_FRAC);
}

PS2FP_INL ps2fp_u64 ps2dp_mul(ps2fp_u64 a, ps2fp_u64 b)
{
    int ea = (int)((a >> 52) & 0x7FF), eb = (int)((b >> 52) & 0x7FF);
    ps2fp_u64 sign = (a ^ b) & PS2DP_SIGN;
    if (ea == 0x7FF || eb == 0x7FF)
        return ps2dp_special(a, b, 0);
    if (!ea || !eb)
        return sign;
    ps2fp_u64 x = (a & PS2DP_FRAC) | (1ull << 52), y = (b & PS2DP_FRAC) | (1ull << 52);
    /* the 106-bit product in 32-bit pieces: hi:lo */
    ps2fp_u64 x0 = (ps2fp_u32)x, x1 = x >> 32, y0 = (ps2fp_u32)y, y1 = y >> 32;
    ps2fp_u64 p00 = x0 * y0, p01 = x0 * y1, p10 = x1 * y0, p11 = x1 * y1;
    ps2fp_u64 mid = (p00 >> 32) + (ps2fp_u32)p01 + (ps2fp_u32)p10;
    ps2fp_u64 lo = (mid << 32) | (ps2fp_u32)p00;
    ps2fp_u64 hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
    int e = ea + eb - 1023;
    ps2fp_u64 m;
    if (hi >> 41) { /* bit 105 of the product */
        m = (hi << 11) | (lo >> 53);
        e++;
    } else {
        m = (hi << 12) | (lo >> 52);
    }
    return ps2dp_pack2(sign, e, m);
}

PS2FP_INL ps2fp_u64 ps2dp_div(ps2fp_u64 a, ps2fp_u64 b)
{
    int ea = (int)((a >> 52) & 0x7FF), eb = (int)((b >> 52) & 0x7FF);
    ps2fp_u64 sign = (a ^ b) & PS2DP_SIGN;
    if (ea == 0x7FF || eb == 0x7FF)
        return ps2dp_special(a, b, 1);
    if (!eb)
        return ea ? sign | PS2DP_EXP : PS2DP_QNAN;
    if (!ea)
        return sign;
    ps2fp_u64 ma = (a & PS2DP_FRAC) | (1ull << 52), mb = (b & PS2DP_FRAC) | (1ull << 52);
    ps2fp_u64 q = 0, r = ma; /* q = floor(ma x 2^53 / mb), restoring division */
    for (int k = 0; k <= 53; k++) {
        q <<= 1;
        if (r >= mb) {
            r -= mb;
            q |= 1;
        }
        r <<= 1;
    }
    int e = ea - eb + 1022;
    if (q >> 53) {
        q >>= 1;
        e++;
    }
    return ps2dp_pack2(sign, e, q);
}

/* The compares: zeros (and denormals, as zeros) equal, a NaN unordered. */
PS2FP_INL int ps2dp_isnan(ps2fp_u64 a) { return (a & ~PS2DP_SIGN) > PS2DP_EXP; }
PS2FP_INL long long ps2dp_key(ps2fp_u64 a)
{
    if (!(a & PS2DP_EXP))
        return 0;
    return (a & PS2DP_SIGN) ? -(long long)(a & ~PS2DP_SIGN) : (long long)a;
}
PS2FP_INL int ps2dp_lt(ps2fp_u64 a, ps2fp_u64 b) { return !ps2dp_isnan(a) && !ps2dp_isnan(b) && ps2dp_key(a) < ps2dp_key(b); }
PS2FP_INL int ps2dp_le(ps2fp_u64 a, ps2fp_u64 b) { return !ps2dp_isnan(a) && !ps2dp_isnan(b) && ps2dp_key(a) <= ps2dp_key(b); }
PS2FP_INL int ps2dp_eq(ps2fp_u64 a, ps2fp_u64 b) { return !ps2dp_isnan(a) && !ps2dp_isnan(b) && ps2dp_key(a) == ps2dp_key(b); }

/* ------------------------------------------------ the rewriter's targets */

PS2FP_API unsigned long long ps2_dpadd(unsigned long long a, unsigned long long b) { return ps2dp_add(a, b); }
PS2FP_API unsigned long long ps2_dpsub(unsigned long long a, unsigned long long b) { return ps2dp_sub(a, b); }
PS2FP_API unsigned long long ps2_dpmul(unsigned long long a, unsigned long long b) { return ps2dp_mul(a, b); }
PS2FP_API unsigned long long ps2_dpdiv(unsigned long long a, unsigned long long b) { return ps2dp_div(a, b); }
PS2FP_API unsigned long long ps2_litodp(int v) { return ps2dp_from_si(v); }
PS2FP_API int ps2_dptoli(unsigned long long d) { return ps2dp_to_si(d); }
PS2FP_API unsigned long long ps2_fptodp(float f) { return ps2dp_from_sf(ps2fp_bits(f)); }
PS2FP_API float ps2_dptofp(unsigned long long d) { return ps2fp_val(ps2dp_to_sf(d)); }

#endif /* PS2DP_H */
