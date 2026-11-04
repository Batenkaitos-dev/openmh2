/* ps2fp: the EE's arithmetic as plain C, for the code mh2-core compiles.
 *
 * The EE's FPU is not IEEE: operands with exponent 255 are ordinary
 * numbers (+/-Fmax is the largest), denormals are zeros, and results are
 * clamped (overflow is +/-Fmax) and flushed (underflow is +/-0).
 * add, subtract, multiply, square root and the integer-to-float
 * conversions round toward zero; division rounds to nearest, and x / 0 is
 * signed Fmax. Float to integer truncates and saturates. The semantics
 * are those of the original game (fpu.c and the FPU part of
 * the original's runtime header, which tools/pc/ps2fp/tests/oracle.c compiles
 * natively and these functions are compared with, bit for bit).
 *
 * Everything here is integer arithmetic (square root excepted, see
 * ps2fp_sqrt): the results do not depend on the host's rounding mode or its
 * denormal modes, and no float operation of this file can be folded or
 * contracted by the compiler into something else.
 *
 * tools/pc/ps2fp/ps2fp.py rewrites the float, shift and division
 * operations of the game's code to the ps2_* functions at the end of
 * this file, which the compiler then inlines. ps2crt's own code
 * (libm, the soft doubles) calls the bit-level functions directly.
 */
#ifndef PS2FP_H
#define PS2FP_H

/* The pass force-includes this header into the game's files, whose own declarations of the C library's
 * names (atoi, strlen, ...) must not meet the host's: clang's mm_malloc.h would pull in <stdlib.h>. */
#ifndef __MM_MALLOC_H
#define __MM_MALLOC_H
#endif
#include <emmintrin.h>

typedef unsigned int ps2fp_u32;
typedef int ps2fp_s32;
typedef unsigned long long ps2fp_u64;

#define PS2FP_INL static inline __attribute__((always_inline))
/* The functions the rewriter calls are kept in the IR (the rewriter
 * removes the llvm.compiler.used entry again after its rewrite, so the
 * ones nothing calls are dropped). */
#define PS2FP_API static inline __attribute__((always_inline, used))

#define PS2FP_SIGN 0x80000000u
#define PS2FP_EXP 0x7F800000u
#define PS2FP_FMAX 0x7F7FFFFFu

PS2FP_INL ps2fp_u32 ps2fp_bits(float f)
{
    ps2fp_u32 u;
    __builtin_memcpy(&u, &f, 4);
    return u;
}
PS2FP_INL float ps2fp_val(ps2fp_u32 u)
{
    float f;
    __builtin_memcpy(&f, &u, 4);
    return f;
}

/* An operand as the EE sees it, and a result as it gives it: the same
 * function (exponent 255 is +/-Fmax, a zero exponent is +/-0). */
PS2FP_INL ps2fp_u32 ps2fp_clean(ps2fp_u32 u)
{
    ps2fp_u32 e = u & PS2FP_EXP;
    if (e == PS2FP_EXP)
        return (u & PS2FP_SIGN) | PS2FP_FMAX;
    if (e == 0)
        return u & PS2FP_SIGN;
    return u;
}

/* Packs sign, a biased exponent and a 24-bit significand (hidden bit set)
 * as the EE's results: overflow is Fmax, underflow is zero. */
PS2FP_INL ps2fp_u32 ps2fp_pack(ps2fp_u32 sign, ps2fp_s32 e, ps2fp_u32 m)
{
    if (e >= 255)
        return sign | PS2FP_FMAX;
    if (e <= 0)
        return sign;
    return sign | ((ps2fp_u32)e << 23) | (m & 0x7FFFFFu);
}

/* ------------------------------------------------- add, subtract, multiply */

/* Toward zero. The significands carry three guard bits; the bits of the
 * smaller operand shifted out below them only matter as "something was
 * lost", which makes a difference one unit less (the truncation of the
 * exact difference). */
PS2FP_INL ps2fp_u32 ps2fp_add(ps2fp_u32 a, ps2fp_u32 b)
{
    a = ps2fp_clean(a);
    b = ps2fp_clean(b);
    ps2fp_u32 sa = a & PS2FP_SIGN, sb = b & PS2FP_SIGN;
    ps2fp_u32 ma = a & ~PS2FP_SIGN, mb = b & ~PS2FP_SIGN;
    if (ma < mb) {
        ps2fp_u32 t = ma;
        ma = mb;
        mb = t;
        t = sa;
        sa = sb;
        sb = t;
    }
    if (mb == 0)
        return ma == 0 ? (sa & sb) : (sa | ma); /* +0 unless both are -0 */
    ps2fp_s32 e = (ps2fp_s32)(ma >> 23);
    ps2fp_u32 d = (ma >> 23) - (mb >> 23);
    ps2fp_u32 fa = ((ma & 0x7FFFFFu) | 0x800000u) << 3;
    ps2fp_u32 fb = ((mb & 0x7FFFFFu) | 0x800000u) << 3;
    ps2fp_u32 lost = 0;
    if (d > 27) {
        lost = 1;
        fb = 0;
    } else if (d) {
        lost = (fb & ((1u << d) - 1)) != 0;
        fb >>= d;
    }
    ps2fp_u32 m;
    if (sa == sb) {
        m = fa + fb;
        if (m >> 27) {
            m >>= 1;
            e++;
        }
    } else {
        m = fa - fb - lost;
        if (m == 0)
            return 0; /* x - x is +0 */
        int sh = __builtin_clz(m) - 5;
        m <<= sh;
        e -= sh;
    }
    return ps2fp_pack(sa, e, m >> 3);
}

PS2FP_INL ps2fp_u32 ps2fp_sub(ps2fp_u32 a, ps2fp_u32 b)
{
    return ps2fp_add(a, b ^ PS2FP_SIGN);
}

PS2FP_INL ps2fp_u32 ps2fp_mul(ps2fp_u32 a, ps2fp_u32 b)
{
    a = ps2fp_clean(a);
    b = ps2fp_clean(b);
    ps2fp_u32 sign = (a ^ b) & PS2FP_SIGN;
    ps2fp_u32 ea = (a >> 23) & 0xFF, eb = (b >> 23) & 0xFF;
    if (ea == 0 || eb == 0)
        return sign;
    ps2fp_u64 p = (ps2fp_u64)((a & 0x7FFFFFu) | 0x800000u) * ((b & 0x7FFFFFu) | 0x800000u);
    ps2fp_s32 e = (ps2fp_s32)(ea + eb) - 127;
    ps2fp_u32 m;
    if (p >> 47) {
        m = (ps2fp_u32)(p >> 24);
        e++;
    } else {
        m = (ps2fp_u32)(p >> 23);
    }
    return ps2fp_pack(sign, e, m);
}

/* -------------------------------------------------------- divide and sqrt */

/* To nearest even; x / 0 (a zero or denormal divisor) is signed Fmax. */
PS2FP_INL ps2fp_u32 ps2fp_div(ps2fp_u32 a, ps2fp_u32 b)
{
    if ((b & PS2FP_EXP) == 0)
        return ((a ^ b) & PS2FP_SIGN) | PS2FP_FMAX;
    a = ps2fp_clean(a);
    b = ps2fp_clean(b);
    ps2fp_u32 sign = (a ^ b) & PS2FP_SIGN;
    ps2fp_u32 ea = (a >> 23) & 0xFF, eb = (b >> 23) & 0xFF;
    if (ea == 0)
        return sign;
    ps2fp_u32 ma = (a & 0x7FFFFFu) | 0x800000u, mb = (b & 0x7FFFFFu) | 0x800000u;
    ps2fp_s32 e = (ps2fp_s32)ea - (ps2fp_s32)eb + 127;
    if (ma < mb) { /* the quotient is in [0.5, 1): make it [1, 2) */
        ma <<= 1;
        e--;
    }
    /* ma / mb = 1.xxx: 24 fractional bits, 8 at a time (every partial
     * dividend is below 2^32 because the remainder is below mb < 2^24) */
    ps2fp_u32 r = ma - mb;
    ps2fp_u32 q = 0;
    for (int i = 0; i < 3; i++) {
        r <<= 8;
        q = (q << 8) | (r / mb);
        r %= mb;
    }
    q |= 1u << 24; /* 25 bits: 24 + the guard bit */
    ps2fp_u32 m = q >> 1;
    if ((q & 1) && (r != 0 || (m & 1)))
        m++;
    if (m >> 24) { /* rounded up to 2.0 */
        m >>= 1;
        e++;
    }
    return ps2fp_pack(sign, e, m);
}

/* Toward zero; the sign of the operand does not count (sqrt(-x) is
 * sqrt(x)), and a zero or denormal is a (signed) zero. The root is taken
 * in double precision and truncated to 24 bits: for a float operand the
 * double result never lies above a 24-bit value the exact root is below
 * (the roots of 24-bit significands are at least 2^-48 away, relative,
 * from the squares of 24-bit numbers), so the truncation is the exact
 * root's. This holds in every rounding mode of the host's SSE unit. */
PS2FP_INL ps2fp_u32 ps2fp_sqrt(ps2fp_u32 a)
{
    if ((a & PS2FP_EXP) == 0)
        return a & PS2FP_SIGN;
    float f = ps2fp_val(ps2fp_clean(a) & ~PS2FP_SIGN);
    __m128d v = _mm_set_sd((double)f);
    v = _mm_sqrt_sd(v, v);
    ps2fp_u64 d;
    double dd = _mm_cvtsd_f64(v);
    __builtin_memcpy(&d, &dd, 8);
    return (ps2fp_u32)(d >> 29) - (896u << 23);
}

/* rsqrt.s: fs / sqrt(|ft|) (the root toward zero, the division to nearest) */
PS2FP_INL ps2fp_u32 ps2fp_rsqrt(ps2fp_u32 a, ps2fp_u32 b)
{
    if ((b & PS2FP_EXP) == 0)
        return ((a ^ b) & PS2FP_SIGN) | PS2FP_FMAX;
    return ps2fp_div(a, ps2fp_sqrt(b));
}

PS2FP_INL ps2fp_u32 ps2fp_neg(ps2fp_u32 a) { return ps2fp_clean(a) ^ PS2FP_SIGN; }
PS2FP_INL ps2fp_u32 ps2fp_abs(ps2fp_u32 a) { return ps2fp_clean(a) & ~PS2FP_SIGN; }

/* ----------------------------------------------------------- comparisons */

/* No NaNs after cleaning: the exact order of the cleaned values, zeros equal. */
PS2FP_INL ps2fp_s32 ps2fp_key(ps2fp_u32 u)
{
    u = ps2fp_clean(u);
    ps2fp_s32 m = (ps2fp_s32)(u & ~PS2FP_SIGN);
    return (u & PS2FP_SIGN) ? -m : m;
}
PS2FP_INL int ps2fp_eq(ps2fp_u32 a, ps2fp_u32 b) { return ps2fp_key(a) == ps2fp_key(b); }
PS2FP_INL int ps2fp_lt(ps2fp_u32 a, ps2fp_u32 b) { return ps2fp_key(a) < ps2fp_key(b); }
PS2FP_INL int ps2fp_le(ps2fp_u32 a, ps2fp_u32 b) { return ps2fp_key(a) <= ps2fp_key(b); }

/* ----------------------------------------------------------- conversions */

/* cvt.s.w: toward zero */
PS2FP_INL ps2fp_u32 ps2fp_i2f(ps2fp_s32 w)
{
    if (w == 0)
        return 0;
    ps2fp_u32 sign = w < 0 ? PS2FP_SIGN : 0;
    ps2fp_u32 mag = w < 0 ? 0u - (ps2fp_u32)w : (ps2fp_u32)w;
    int n = 31 - __builtin_clz(mag);
    ps2fp_u32 m = n <= 23 ? mag << (23 - n) : mag >> (n - 23);
    return sign | ((ps2fp_u32)(127 + n) << 23) | (m & 0x7FFFFFu);
}

/* cvt.w.s: truncates; the values beyond the int saturate */
PS2FP_INL ps2fp_s32 ps2fp_f2i(ps2fp_u32 a)
{
    ps2fp_u32 e = (a >> 23) & 0xFF;
    if (e >= 158)
        return (a & PS2FP_SIGN) ? (ps2fp_s32)0x80000000u : 0x7FFFFFFF;
    if (e < 127)
        return 0;
    ps2fp_u32 m = (a & 0x7FFFFFu) | 0x800000u;
    ps2fp_u32 v = e >= 150 ? m << (e - 150) : m >> (150 - e);
    return (a & PS2FP_SIGN) ? -(ps2fp_s32)v : (ps2fp_s32)v;
}

/* MWCC's unsigned conversions, as it compiles them (the EE has no
 * instruction for them): u32 -> float: x < 2^31 ? cvt.s.w(x) :
 * 2 * cvt.s.w(x >> 1 | x & 1); float -> u32: 2^31 <= f ?
 * cvt.w.s(f - 2^31) | 2^31 : cvt.w.s(f). */
PS2FP_INL ps2fp_u32 ps2fp_u2f(ps2fp_u32 x)
{
    if ((ps2fp_s32)x >= 0)
        return ps2fp_i2f((ps2fp_s32)x);
    ps2fp_u32 h = ps2fp_i2f((ps2fp_s32)((x >> 1) | (x & 1)));
    return ps2fp_add(h, h);
}
PS2FP_INL ps2fp_u32 ps2fp_f2u(ps2fp_u32 f)
{
    if (ps2fp_le(0x4F000000u, f)) /* 2^31 */
        return (ps2fp_u32)ps2fp_f2i(ps2fp_sub(f, 0x4F000000u)) | 0x80000000u;
    return (ps2fp_u32)ps2fp_f2i(f);
}

/* ------------------------------------------------------------- integers */

/* The EE's div/divu: by zero the quotient is -1 (1 for a negative
 * dividend; all ones unsigned) and the remainder the dividend; INT_MIN / -1
 * is INT_MIN with remainder 0. x86 traps on both. */
PS2FP_INL ps2fp_s32 ps2fp_sdiv(ps2fp_s32 n, ps2fp_s32 d)
{
    if (d == 0)
        return n < 0 ? 1 : -1;
    if (n == (ps2fp_s32)0x80000000u && d == -1)
        return n;
    return n / d;
}
PS2FP_INL ps2fp_s32 ps2fp_srem(ps2fp_s32 n, ps2fp_s32 d)
{
    if (d == 0)
        return n;
    if (n == (ps2fp_s32)0x80000000u && d == -1)
        return 0;
    return n % d;
}
PS2FP_INL ps2fp_u32 ps2fp_udiv(ps2fp_u32 n, ps2fp_u32 d) { return d == 0 ? 0xFFFFFFFFu : n / d; }
PS2FP_INL ps2fp_u32 ps2fp_urem(ps2fp_u32 n, ps2fp_u32 d) { return d == 0 ? n : n % d; }

/* ------------------------------------------------ the rewriter's targets */

PS2FP_API float ps2_fadd(float a, float b) { return ps2fp_val(ps2fp_add(ps2fp_bits(a), ps2fp_bits(b))); }
PS2FP_API float ps2_fsub(float a, float b) { return ps2fp_val(ps2fp_sub(ps2fp_bits(a), ps2fp_bits(b))); }
PS2FP_API float ps2_fmul(float a, float b) { return ps2fp_val(ps2fp_mul(ps2fp_bits(a), ps2fp_bits(b))); }
PS2FP_API float ps2_fdiv(float a, float b) { return ps2fp_val(ps2fp_div(ps2fp_bits(a), ps2fp_bits(b))); }
PS2FP_API float ps2_fneg(float a) { return ps2fp_val(ps2fp_neg(ps2fp_bits(a))); }
PS2FP_API float ps2_fabs(float a) { return ps2fp_val(ps2fp_abs(ps2fp_bits(a))); }
PS2FP_API float ps2_fsqrt(float a) { return ps2fp_val(ps2fp_sqrt(ps2fp_bits(a))); }
PS2FP_API int ps2_feq(float a, float b) { return ps2fp_eq(ps2fp_bits(a), ps2fp_bits(b)); }
PS2FP_API int ps2_flt(float a, float b) { return ps2fp_lt(ps2fp_bits(a), ps2fp_bits(b)); }
PS2FP_API int ps2_fle(float a, float b) { return ps2fp_le(ps2fp_bits(a), ps2fp_bits(b)); }
PS2FP_API float ps2_i2f(int w) { return ps2fp_val(ps2fp_i2f(w)); }
PS2FP_API float ps2_u2f(unsigned int x) { return ps2fp_val(ps2fp_u2f(x)); }
PS2FP_API int ps2_f2i(float a) { return ps2fp_f2i(ps2fp_bits(a)); }
PS2FP_API unsigned int ps2_f2u(float a) { return ps2fp_f2u(ps2fp_bits(a)); }

PS2FP_API int ps2_sdiv(int n, int d) { return ps2fp_sdiv(n, d); }
PS2FP_API int ps2_srem(int n, int d) { return ps2fp_srem(n, d); }
PS2FP_API unsigned int ps2_udiv(unsigned int n, unsigned int d) { return ps2fp_udiv(n, d); }
PS2FP_API unsigned int ps2_urem(unsigned int n, unsigned int d) { return ps2fp_urem(n, d); }

/* Variable shifts: sllv/srlv/srav use the low 5 bits of the count, dsllv
 * and the others the low 6. */
PS2FP_API unsigned int ps2_shl32(unsigned int x, unsigned int s) { return x << (s & 31); }
PS2FP_API unsigned int ps2_lshr32(unsigned int x, unsigned int s) { return x >> (s & 31); }
PS2FP_API int ps2_ashr32(int x, unsigned int s) { return x >> (s & 31); }
PS2FP_API unsigned long long ps2_shl64(unsigned long long x, unsigned int s) { return x << (s & 63); }
PS2FP_API unsigned long long ps2_lshr64(unsigned long long x, unsigned int s) { return x >> (s & 63); }
PS2FP_API long long ps2_ashr64(long long x, unsigned int s) { return x >> (s & 63); }

/* the software doubles (MWCC's), whose ps2_* functions the rewriter also calls */
#include "ps2dp.h"

#endif /* PS2FP_H */
