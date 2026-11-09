/* ps2fp: the EE's arithmetic as plain C, for the code mh2-core compiles.
 *
 * The EE's FPU is not IEEE: operands with exponent 255 are ordinary
 * numbers (+/-Fmax is the largest), denormals are zeros, and results are
 * clamped (overflow is +/-Fmax) and flushed (underflow is +/-0).
 * add, subtract, multiply, square root and the integer-to-float
 * conversions round toward zero; division rounds to nearest, and x / 0 is
 * signed Fmax. Float to integer truncates and saturates. The semantics
 * are those of the original game (its FPU code and the FPU part of
 * the original's runtime header, which the ps2fp tests' oracle compiles
 * natively and these functions are compared with, bit for bit).
 *
 * Two float models, chosen at run time by ps2fp_model (a saved session names it,
 * "fpu_model" in its header). The model above is the "legacy" one: ps2fp0_*
 * below, which VU0's operations (include/vu0ops.h) use whatever the model.
 * The "ps2" model (ps2fp2_*, further down) has the console's results where
 * known: exponent 255 is an ordinary number (1.m x 2^128) and the largest
 * value is 0x7FFFFFFF, x / 0 is signed 0x7FFFFFFF, abs and neg are bit
 * operations, the square root and the quotient round to nearest, sqrt(-0) is
 * +0, the product is the Booth multiplier's (ps2float_mul) and the sum and
 * difference are the guard-bit adder's (ps2float_add); both are derived works
 * of PCSX2, GPL-3.0-or-later, and a core built with this header is
 * GPL-3.0-or-later as a whole. The ps2fp_*
 * functions, which the rewriter and the library call, dispatch on the model.
 *
 * Everything here is integer arithmetic (the legacy square root excepted, see
 * ps2fp0_sqrt): the results do not depend on the host's rounding mode or its
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

/* The float model: 0 legacy, 1 ps2. Set before the game's first instruction runs (pc/core/pal_fpmodel.c); the strong
 * definition is crt_fpmodel.c's. The weak one here keeps every program that includes this header linkable (the tests
 * of the layers that compile game code), in the legacy model. */
extern int ps2fp_model;
__attribute__((weak)) int ps2fp_model;

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
PS2FP_INL ps2fp_u32 ps2fp0_clean(ps2fp_u32 u)
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
PS2FP_INL ps2fp_u32 ps2fp0_pack(ps2fp_u32 sign, ps2fp_s32 e, ps2fp_u32 m)
{
    if (e >= 255)
        return sign | PS2FP_FMAX;
    if (e <= 0)
        return sign;
    return sign | ((ps2fp_u32)e << 23) | (m & 0x7FFFFFu);
}

/* ------------------------------------- the legacy model: add, subtract, multiply */

/* Toward zero. The significands carry three guard bits; the bits of the
 * smaller operand shifted out below them only matter as "something was
 * lost", which makes a difference one unit less (the truncation of the
 * exact difference). */
PS2FP_INL ps2fp_u32 ps2fp0_add(ps2fp_u32 a, ps2fp_u32 b)
{
    a = ps2fp0_clean(a);
    b = ps2fp0_clean(b);
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
    return ps2fp0_pack(sa, e, m >> 3);
}

PS2FP_INL ps2fp_u32 ps2fp0_sub(ps2fp_u32 a, ps2fp_u32 b)
{
    return ps2fp0_add(a, b ^ PS2FP_SIGN);
}

PS2FP_INL ps2fp_u32 ps2fp0_mul(ps2fp_u32 a, ps2fp_u32 b)
{
    a = ps2fp0_clean(a);
    b = ps2fp0_clean(b);
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
    return ps2fp0_pack(sign, e, m);
}

/* -------------------------------------------------------- divide and sqrt */

/* To nearest even; x / 0 (a zero or denormal divisor) is signed Fmax. */
PS2FP_INL ps2fp_u32 ps2fp0_div(ps2fp_u32 a, ps2fp_u32 b)
{
    if ((b & PS2FP_EXP) == 0)
        return ((a ^ b) & PS2FP_SIGN) | PS2FP_FMAX;
    a = ps2fp0_clean(a);
    b = ps2fp0_clean(b);
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
    return ps2fp0_pack(sign, e, m);
}

/* Toward zero; the sign of the operand does not count (sqrt(-x) is
 * sqrt(x)), and a zero or denormal is a (signed) zero. The root is taken
 * in double precision and truncated to 24 bits: for a float operand the
 * double result never lies above a 24-bit value the exact root is below
 * (the roots of 24-bit significands are at least 2^-48 away, relative,
 * from the squares of 24-bit numbers), so the truncation is the exact
 * root's. This holds in every rounding mode of the host's SSE unit. */
PS2FP_INL ps2fp_u32 ps2fp0_sqrt(ps2fp_u32 a)
{
    if ((a & PS2FP_EXP) == 0)
        return a & PS2FP_SIGN;
    float f = ps2fp_val(ps2fp0_clean(a) & ~PS2FP_SIGN);
    __m128d v = _mm_set_sd((double)f);
    v = _mm_sqrt_sd(v, v);
    ps2fp_u64 d;
    double dd = _mm_cvtsd_f64(v);
    __builtin_memcpy(&d, &dd, 8);
    return (ps2fp_u32)(d >> 29) - (896u << 23);
}

/* rsqrt.s: fs / sqrt(|ft|) (the root toward zero, the division to nearest) */
PS2FP_INL ps2fp_u32 ps2fp0_rsqrt(ps2fp_u32 a, ps2fp_u32 b)
{
    if ((b & PS2FP_EXP) == 0)
        return ((a ^ b) & PS2FP_SIGN) | PS2FP_FMAX;
    return ps2fp0_div(a, ps2fp0_sqrt(b));
}

PS2FP_INL ps2fp_u32 ps2fp0_neg(ps2fp_u32 a) { return ps2fp0_clean(a) ^ PS2FP_SIGN; }
PS2FP_INL ps2fp_u32 ps2fp0_abs(ps2fp_u32 a) { return ps2fp0_clean(a) & ~PS2FP_SIGN; }

/* ----------------------------------------------------------- comparisons */

/* No NaNs after cleaning: the exact order of the cleaned values, zeros equal. */
PS2FP_INL ps2fp_s32 ps2fp0_key(ps2fp_u32 u)
{
    u = ps2fp0_clean(u);
    ps2fp_s32 m = (ps2fp_s32)(u & ~PS2FP_SIGN);
    return (u & PS2FP_SIGN) ? -m : m;
}
PS2FP_INL int ps2fp0_eq(ps2fp_u32 a, ps2fp_u32 b) { return ps2fp0_key(a) == ps2fp0_key(b); }
PS2FP_INL int ps2fp0_lt(ps2fp_u32 a, ps2fp_u32 b) { return ps2fp0_key(a) < ps2fp0_key(b); }
PS2FP_INL int ps2fp0_le(ps2fp_u32 a, ps2fp_u32 b) { return ps2fp0_key(a) <= ps2fp0_key(b); }

/* ------------------------------------------------------------ the ps2 model */

/* The Booth multiplier (GPL-3.0-or-later; SOURCE.txt next to it). Its entry point stays a call: it runs for every
 * product under the ps2 model, and the game has thousands of them. */
#define PS2FLOAT_U32 ps2fp_u32
#define PS2FLOAT_S32 ps2fp_s32
#define PS2FLOAT_U64 ps2fp_u64
#define PS2FLOAT_MUL_ATTR __attribute__((noinline))
#include "../third_party/pcsx2_ps2float/ps2float_mul.h"
#define PS2FLOAT_ADD_ATTR __attribute__((noinline))
#include "../third_party/pcsx2_ps2float/ps2float_add.h"

#define PS2FP2 static inline __attribute__((noinline))   /* the ps2 model's bigger functions: one copy per file, called */
#define PS2FP2_MAX 0x7FFFFFFFu

/* Exponent 255 is a number; a zero exponent field is a zero. */
PS2FP_INL ps2fp_u32 ps2fp2_clean(ps2fp_u32 u) { return (u & PS2FP_EXP) == 0 ? u & PS2FP_SIGN : u; }

/* Overflow (past exponent 255) is the largest value, 0x7FFFFFFF; underflow is zero. */
PS2FP_INL ps2fp_u32 ps2fp2_pack(ps2fp_u32 sign, ps2fp_s32 e, ps2fp_u32 m)
{
    if (e >= 256)
        return sign | PS2FP2_MAX;
    if (e <= 0)
        return sign;
    return sign | ((ps2fp_u32)e << 23) | (m & 0x7FFFFFu);
}

/* The adder: the guard-bit adder (ps2float_add.h, GPL-3.0-or-later; SOURCE.txt). Exponent 255 is a number. */
PS2FP_INL ps2fp_u32 ps2fp2_add(ps2fp_u32 a, ps2fp_u32 b) { return ps2float_add(a, b); }
PS2FP_INL ps2fp_u32 ps2fp2_sub(ps2fp_u32 a, ps2fp_u32 b) { return ps2float_sub(a, b); }

/* The product: the Booth multiplier's (a zero exponent field gives a signed zero). */
PS2FP_INL ps2fp_u32 ps2fp2_mul(ps2fp_u32 a, ps2fp_u32 b) { return ps2float_mul(a, b); }

/* To nearest even; x / 0 (a zero or denormal divisor) is signed 0x7FFFFFFF. */
PS2FP2 ps2fp_u32 ps2fp2_div(ps2fp_u32 a, ps2fp_u32 b)
{
    if ((b & PS2FP_EXP) == 0)
        return ((a ^ b) & PS2FP_SIGN) | PS2FP2_MAX;
    a = ps2fp2_clean(a);
    b = ps2fp2_clean(b);
    ps2fp_u32 sign = (a ^ b) & PS2FP_SIGN;
    ps2fp_u32 ea = (a >> 23) & 0xFF, eb = (b >> 23) & 0xFF;
    if (ea == 0)
        return sign;
    ps2fp_u32 ma = (a & 0x7FFFFFu) | 0x800000u, mb = (b & 0x7FFFFFu) | 0x800000u;
    ps2fp_s32 e = (ps2fp_s32)ea - (ps2fp_s32)eb + 127;
    if (ma < mb) {
        ma <<= 1;
        e--;
    }
    ps2fp_u32 r = ma - mb;
    ps2fp_u32 q = 0;
    for (int i = 0; i < 3; i++) {
        r <<= 8;
        q = (q << 8) | (r / mb);
        r %= mb;
    }
    q |= 1u << 24;
    ps2fp_u32 m = q >> 1;
    if ((q & 1) && (r != 0 || (m & 1)))
        m++;
    if (m >> 24) {
        m >>= 1;
        e++;
    }
    return ps2fp2_pack(sign, e, m);
}

/* To nearest even, from the exact integer root (24 bits of significand from a 31 or 32-bit root, and its
 * remainder). A zero exponent field is +0, even for -0; the sign of the operand does not count. */
PS2FP2 ps2fp_u32 ps2fp2_sqrt(ps2fp_u32 a)
{
    if ((a & PS2FP_EXP) == 0)
        return 0;
    ps2fp_s32 k = (ps2fp_s32)((a >> 23) & 0xFF) - 127 - 23; /* the value is m * 2^k, m 24 bits */
    ps2fp_u64 x = (a & 0x7FFFFFu) | 0x800000u;
    if (k & 1) {
        x <<= 1;
        k--;
    }
    x <<= 38; /* x in [2^61, 2^63), k - 38 even; the root is x^(1/2) * 2^((k - 38) / 2) */
    ps2fp_u64 r = 0, bit = 1ull << 62, n = x;
    while (bit > n)
        bit >>= 2;
    while (bit) {
        if (n >= r + bit) {
            n -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    /* r = floor(sqrt(x)), n = x - r * r */
    int sh = (r >> 31) ? 8 : 7;
    ps2fp_u32 q = (ps2fp_u32)(r >> sh);
    ps2fp_u32 frac = (ps2fp_u32)r & ((1u << sh) - 1), half = 1u << (sh - 1);
    if (frac > half || (frac == half && (n != 0 || (q & 1))))
        q++;
    ps2fp_s32 e = 23 + sh + (k - 38) / 2 + 127; /* q in [2^23, 2^24] */
    if (q >> 24) {
        q >>= 1;
        e++;
    }
    return ps2fp2_pack(0, e, q);
}

/* rsqrt.s: fs / sqrt(|ft|), composed of this model's root and quotient (the game has no rsqrt.s) */
PS2FP2 ps2fp_u32 ps2fp2_rsqrt(ps2fp_u32 a, ps2fp_u32 b)
{
    if ((b & PS2FP_EXP) == 0)
        return (a & PS2FP_SIGN) | PS2FP2_MAX;
    return ps2fp2_div(a, ps2fp2_sqrt(b));
}

PS2FP_INL ps2fp_u32 ps2fp2_neg(ps2fp_u32 a) { return a ^ PS2FP_SIGN; }
PS2FP_INL ps2fp_u32 ps2fp2_abs(ps2fp_u32 a) { return a & ~PS2FP_SIGN; }

/* No NaNs: the exact order of the values (exponent 255 included), zeros equal. */
PS2FP_INL ps2fp_s32 ps2fp2_key(ps2fp_u32 u)
{
    u = ps2fp2_clean(u);
    ps2fp_s32 m = (ps2fp_s32)(u & ~PS2FP_SIGN);
    return (u & PS2FP_SIGN) ? -m : m;
}
PS2FP_INL int ps2fp2_eq(ps2fp_u32 a, ps2fp_u32 b) { return ps2fp2_key(a) == ps2fp2_key(b); }
PS2FP_INL int ps2fp2_lt(ps2fp_u32 a, ps2fp_u32 b) { return ps2fp2_key(a) < ps2fp2_key(b); }
PS2FP_INL int ps2fp2_le(ps2fp_u32 a, ps2fp_u32 b) { return ps2fp2_key(a) <= ps2fp2_key(b); }

/* ------------------------------------------------ the model in force: dispatch */

PS2FP_INL ps2fp_u32 ps2fp_add(ps2fp_u32 a, ps2fp_u32 b) { return ps2fp_model ? ps2fp2_add(a, b) : ps2fp0_add(a, b); }
PS2FP_INL ps2fp_u32 ps2fp_sub(ps2fp_u32 a, ps2fp_u32 b) { return ps2fp_model ? ps2fp2_sub(a, b) : ps2fp0_sub(a, b); }
PS2FP_INL ps2fp_u32 ps2fp_mul(ps2fp_u32 a, ps2fp_u32 b) { return ps2fp_model ? ps2fp2_mul(a, b) : ps2fp0_mul(a, b); }
PS2FP_INL ps2fp_u32 ps2fp_div(ps2fp_u32 a, ps2fp_u32 b) { return ps2fp_model ? ps2fp2_div(a, b) : ps2fp0_div(a, b); }
PS2FP_INL ps2fp_u32 ps2fp_sqrt(ps2fp_u32 a) { return ps2fp_model ? ps2fp2_sqrt(a) : ps2fp0_sqrt(a); }
PS2FP_INL ps2fp_u32 ps2fp_rsqrt(ps2fp_u32 a, ps2fp_u32 b) { return ps2fp_model ? ps2fp2_rsqrt(a, b) : ps2fp0_rsqrt(a, b); }
PS2FP_INL ps2fp_u32 ps2fp_neg(ps2fp_u32 a) { return ps2fp_model ? ps2fp2_neg(a) : ps2fp0_neg(a); }
PS2FP_INL ps2fp_u32 ps2fp_abs(ps2fp_u32 a) { return ps2fp_model ? ps2fp2_abs(a) : ps2fp0_abs(a); }
PS2FP_INL int ps2fp_eq(ps2fp_u32 a, ps2fp_u32 b) { return ps2fp_model ? ps2fp2_eq(a, b) : ps2fp0_eq(a, b); }
PS2FP_INL int ps2fp_lt(ps2fp_u32 a, ps2fp_u32 b) { return ps2fp_model ? ps2fp2_lt(a, b) : ps2fp0_lt(a, b); }
PS2FP_INL int ps2fp_le(ps2fp_u32 a, ps2fp_u32 b) { return ps2fp_model ? ps2fp2_le(a, b) : ps2fp0_le(a, b); }

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
#ifdef MH2_FPTRACE
/* The trace build (the product trace): the pass gives every float add and multiply a site id; the
 * tracer (pal_fptrace.c) logs the products whose result depends on the order of the operands. */
__attribute__((cdecl)) void mh2_fptrace(int op, ps2fp_u32 a, ps2fp_u32 b, ps2fp_u32 r, unsigned site);
#ifndef MH2_FPTRACE_IMPL   /* a program that links the game's objects without the tracer (the tests): no trace */
__attribute__((cdecl, weak)) void mh2_fptrace(int op, ps2fp_u32 a, ps2fp_u32 b, ps2fp_u32 r, unsigned site)
{
    (void)op; (void)a; (void)b; (void)r; (void)site;
}
#endif
PS2FP_API float ps2_fadd_t(float a, float b, int site)
{
    ps2fp_u32 r = ps2fp_add(ps2fp_bits(a), ps2fp_bits(b));
    mh2_fptrace(0, ps2fp_bits(a), ps2fp_bits(b), r, (unsigned)site);
    return ps2fp_val(r);
}
PS2FP_API float ps2_fmul_t(float a, float b, int site)
{
    ps2fp_u32 r = ps2fp_mul(ps2fp_bits(a), ps2fp_bits(b));
    mh2_fptrace(2, ps2fp_bits(a), ps2fp_bits(b), r, (unsigned)site);
    return ps2fp_val(r);
}
#endif
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
