/*
 * ====================================================
 * Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
 *
 * Developed at SunPro, a Sun Microsystems, Inc. business.
 * Permission to use, copy, modify, and distribute this
 * software is freely granted, provided that this notice
 * is preserved.
 * ====================================================
 */

/* The C library's math functions on PC: re-hosted from the earlier port's libm.c
 * (Monster Hunter 2 decomp; listed in
 * THIRD_PARTY.md), with the guest registers gone: each function is
 * the C function the game declares (include/libm.h), a float argument and
 * result being the 32 bits of an EE value, and every float operation is
 * ps2fp.h's (the nf_* names of ps2crt.h). The algorithms and the constants
 * are the original's.
 *
 * Taken from newlib 4.6.0's libm/math (fdlibm; the float versions
 * converted by Ian Lance Taylor, Cygnus Support), with newlib 1.8.2's code
 * where the tests settled the game's library has it (expf,
 * atan2f, powf, kf_rem_pio2), and adapted: every float
 * operation goes through the EE's float rules as the game's code has
 * them (mh2native_float.h: operands and results with
 * exponent 255 are +/-Fmax, denormals +/-0, add/sub/mul rounded toward
 * zero, the runtime's division), so a function gives the bits the
 * game's library function gives. A function is
 * replaced only when it matches the game's one bit for bit, on every
 * one of the 2^32 inputs of a one-argument float function.
 * That is how each function here was checked.
 *
 * The EE's calling convention: a float argument in $f12 (a second in
 * $f13), the float result in $f0. */
#include "ps2crt.h"
#include "crt_tap.h"

/* The C library was built for an FPU without infinities, NaNs and
 * denormals: newlib's _FLT_LARGEST_EXPONENT_IS_NORMAL and
 * _FLT_NO_DENORMALS (libm/common/fdlibm.h, newlib 4.6.0; newlib 1.8.2's
 * code has neither). The tests settled it: floorf and ceilf return an
 * input with exponent 255 as it is, and treat a denormal as zero. */
#define FLT_UWORD_IS_FINITE(x) 1
#define FLT_UWORD_IS_ZERO(x) ((x) < 0x00800000u)
#define FLT_UWORD_IS_NAN(x) 0

/* The constants: newlib's decimal literals as the game's toolchain
 * converted them, rounding toward zero (atanf matches
 * on all 2^32 inputs so, and differs on 11,812,078 with the nearest floats,
 * 9.8279368877e-01 among them). a test program prints them; they
 * are spelled here as hex floats, exact in any compiler, with newlib's
 * literal beside each (newlib's own comments give the nearest float's
 * bits, which differ where that program marks it). */
static const float huge = 0x1.93e592p+99f; /* 1.0e30 */

/* floorf (sf_floor.c): x rounded toward -inf to an integral value. */
static u32 m_floorf(u32 x)
{
    s32 i0 = (s32)x;
    u32 ix = x & 0x7fffffffu, i;
    s32 j0 = (s32)(ix >> 23) - 0x7f;
    if (j0 < 23) {
        if (j0 < 0) {   /* raise inexact if x != 0 */
            if (nf_gt(nf_add(nf_val(x), huge), 0.0f)) {   /* return 0*sign(x) if |x|<1 */
                if (i0 >= 0)
                    i0 = 0;
                else if (!FLT_UWORD_IS_ZERO(ix))
                    i0 = (s32)0xbf800000u;
            }
        } else {
            i = 0x007fffffu >> j0;
            if ((i0 & i) == 0)
                return x;   /* x is integral */
            if (nf_gt(nf_add(nf_val(x), huge), 0.0f)) {   /* raise inexact flag */
                if (i0 < 0)
                    i0 += 0x00800000 >> j0;
                i0 &= (s32)~i;
            }
        }
    } else {
        if (!FLT_UWORD_IS_FINITE(ix))
            return nf_bits(nf_add(nf_val(x), nf_val(x)));   /* inf or NaN */
        return x;   /* x is integral */
    }
    return (u32)i0;
}

/* ceilf (sf_ceil.c): x rounded toward +inf to an integral value. */
static u32 m_ceilf(u32 x)
{
    s32 i0 = (s32)x;
    u32 ix = x & 0x7fffffffu, i;
    s32 j0 = (s32)(ix >> 23) - 0x7f;
    if (j0 < 23) {
        if (j0 < 0) {   /* raise inexact if x != 0 */
            if (nf_gt(nf_add(nf_val(x), huge), 0.0f)) {   /* return 0*sign(x) if |x|<1 */
                if (i0 < 0)
                    i0 = (s32)0x80000000u;
                else if (!FLT_UWORD_IS_ZERO(ix))
                    i0 = 0x3f800000;
            }
        } else {
            i = 0x007fffffu >> j0;
            if ((i0 & i) == 0)
                return x;   /* x is integral */
            if (nf_gt(nf_add(nf_val(x), huge), 0.0f)) {   /* raise inexact flag */
                if (i0 > 0)
                    i0 += 0x00800000 >> j0;
                i0 &= (s32)~i;
            }
        }
    } else {
        if (!FLT_UWORD_IS_FINITE(ix))
            return nf_bits(nf_add(nf_val(x), nf_val(x)));   /* inf or NaN */
        return x;   /* x is integral */
    }
    return (u32)i0;
}

/* atanf (sf_atan.c): the arc tangent, in [-pi/2, pi/2]. */
static const float atanhi[] = {
    0x1.dac67p-2f, /* 4.6364760399e-01, atan(0.5)hi 0x3eed6338 */
    0x1.921fb4p-1f, /* 7.8539812565e-01, atan(1.0)hi 0x3f490fda */
    0x1.f730bap-1f, /* 9.8279368877e-01, atan(1.5)hi 0x3f7b985e */
    0x1.921fb4p+0f, /* 1.5707962513e+00, atan(inf)hi 0x3fc90fda */
};
static const float atanlo[] = {
    0x1.586ed2p-28f, /* 5.0121582440e-09, atan(0.5)lo 0x31ac3769 */
    0x1.4442cep-25f, /* 3.7748947079e-08, atan(1.0)lo 0x33222168 */
    0x1.281f68p-25f, /* 3.4473217170e-08, atan(1.5)lo 0x33140fb4 */
    0x1.4442dp-24f, /* 7.5497894159e-08, atan(inf)lo 0x33a22168 */
};
static const float aT[] = {
    0x1.555556p-2f, /* 3.3333334327e-01, 0x3eaaaaaa */
    -0x1.999998p-3f, /* -2.0000000298e-01, 0xbe4ccccd */
    0x1.249248p-3f, /* 1.4285714924e-01, 0x3e124925 */
    -0x1.c71c7p-4f, /* -1.1111110449e-01, 0xbde38e38 */
    0x1.745cdcp-4f, /* 9.0908870101e-02, 0x3dba2e6e */
    -0x1.3b0f2ap-4f, /* -7.6918758452e-02, 0xbd9d8795 */
    0x1.10d668p-4f, /* 6.6610731184e-02, 0x3d886b35 */
    -0x1.dde2d4p-5f, /* -5.8335702866e-02, 0xbd6ef16b */
    0x1.97b4b2p-5f, /* 4.9768779427e-02, 0x3d4bda59 */
    -0x1.2b4442p-5f, /* -3.6531571299e-02, 0xbd15a221 */
    0x1.0ad3aep-6f, /* 1.6285819933e-02, 0x3c8569d7 */
};
static const float one = 1.0f;

static u32 m_atanf(u32 xb)
{
    float x = nf_val(xb), w, s1, s2, z;
    s32 hx = (s32)xb, ix = hx & 0x7fffffff, id;
    if (ix >= 0x50800000) {   /* if |x| >= 2^34 */
        if (FLT_UWORD_IS_NAN(ix))
            return nf_bits(nf_add(x, x));   /* NaN */
        if (hx > 0)
            return nf_bits(nf_add(atanhi[3], atanlo[3]));
        return nf_bits(nf_sub(nf_neg(atanhi[3]), atanlo[3]));
    }
    if (ix < 0x3ee00000) {   /* |x| < 0.4375 */
        if (ix < 0x31000000) {   /* |x| < 2^-29 */
            if (nf_gt(nf_add(x, huge), one))
                return xb;   /* raise inexact */
        }
        id = -1;
    } else {
        x = nf_abs(x);
        if (ix < 0x3f980000) {   /* |x| < 1.1875 */
            if (ix < 0x3f300000) {   /* 7/16 <=|x|<11/16 */
                id = 0;
                x = nf_div(nf_sub(nf_mul(x, 2.0f), one), nf_add(x, 2.0f));
            } else {   /* 11/16<=|x|< 19/16 */
                id = 1;
                x = nf_div(nf_sub(x, one), nf_add(x, one));
            }
        } else {
            if (ix < 0x401c0000) {   /* |x| < 2.4375 */
                id = 2;
                x = nf_div(nf_sub(x, 1.5f), nf_add(nf_mul(x, 1.5f), one));
            } else {   /* 2.4375 <= |x| < 2^66 */
                id = 3;
                x = nf_div(-1.0f, x);
            }
        }
    }
    /* end of argument reduction */
    z = nf_mul(x, x);
    w = nf_mul(z, z);
    /* break sum from i=0 to 10 aT[i]z**(i+1) into odd and even poly */
    /* s1 = z*(aT[0]+w*(aT[2]+w*(aT[4]+w*(aT[6]+w*(aT[8]+w*aT[10]))))),
     * from the inside out */
    s1 = nf_add(nf_mul(w, aT[10]), aT[8]);
    s1 = nf_add(nf_mul(w, s1), aT[6]);
    s1 = nf_add(nf_mul(w, s1), aT[4]);
    s1 = nf_add(nf_mul(w, s1), aT[2]);
    s1 = nf_mul(z, nf_add(nf_mul(w, s1), aT[0]));
    /* s2 = w*(aT[1]+w*(aT[3]+w*(aT[5]+w*(aT[7]+w*aT[9])))) */
    s2 = nf_add(nf_mul(w, aT[9]), aT[7]);
    s2 = nf_add(nf_mul(w, s2), aT[5]);
    s2 = nf_add(nf_mul(w, s2), aT[3]);
    s2 = nf_mul(w, nf_add(nf_mul(w, s2), aT[1]));
    if (id < 0)
        return nf_bits(nf_sub(x, nf_mul(x, nf_add(s1, s2))));
    z = nf_sub(atanhi[id], nf_sub(nf_sub(nf_mul(x, nf_add(s1, s2)), atanlo[id]), x));
    return nf_bits(hx < 0 ? nf_neg(z) : z);
}

/* expf (ef_exp.c, newlib 4.6.0's __OBSOLETE_MATH version; the wrapper
 * returns __ieee754_expf's result), with one change the tests settled: the
 * shortcut 1+x is taken for |x| < 2**-28 (newlib 1.8.2's and fdlibm's
 * bound; 4.6.0 has 2**-23, and differs at x = -0x1.000002p-24, 0xB3800001,
 * the only one of the 2^32 inputs). With no infinities and denormals,
 * fdlibm.h's FLT_UWORD_LOG_MAX is 0x42b2d4fc (log of 2^129) and
 * FLT_UWORD_LOG_MIN 0x42aeac50 (|x| of log of 2^-126); an overflow gives
 * 0x1p97 * 0x1p97 and an underflow 0x1p-95 * 0x1p-95 (math_errf.c, the
 * library's errno is its own memory: decision 1). */
static const float halF[2] = {0.5f, -0.5f};
static const float twom100 = 0x1.fffffep-101f; /* 7.8886090522e-31, 2**-100 */
static const float ln2HI[2] = {
    0x1.62e3p-1f,  /* 6.9313812256e-01 */
    -0x1.62e3p-1f, /* -6.9313812256e-01 */
};
static const float ln2LO[2] = {
    0x1.2fefa2p-17f,  /* 9.0580006145e-06 */
    -0x1.2fefa2p-17f, /* -9.0580006145e-06 */
};
static const float invln2 = 0x1.715474p+0f; /* 1.4426950216e+00 */
static const float P1 = 0x1.555554p-3f;   /* 1.6666667163e-01 */
static const float P2 = -0x1.6c16cp-9f;   /* -2.7777778450e-03 */
static const float P3 = 0x1.1566a8p-14f;  /* 6.6137559770e-05 */
static const float P4 = -0x1.bbd41cp-20f; /* -1.6533901999e-06 */
static const float P5 = 0x1.637696p-25f;  /* 4.1381369442e-08 */
#define FLT_UWORD_LOG_MAX 0x42b2d4fc
#define FLT_UWORD_LOG_MIN 0x42aeac50

static u32 m_expf(u32 xb)
{
    float x = nf_val(xb), y, hi = 0.0f, lo = 0.0f, c, t;
    s32 k = 0, xsb, sx = (s32)xb;
    u32 hx;
    xsb = (sx >> 31) & 1;   /* sign bit of x */
    hx = (u32)sx & 0x7fffffff;   /* high word of |x| */
    /* filter out non-finite argument (none: no NaN, no infinity) */
    if (sx > FLT_UWORD_LOG_MAX)
        return PS2FP_FMAX;   /* overflow: the largest finite float, whatever the model (the library returns the constant) */
    if (sx < 0 && hx > FLT_UWORD_LOG_MIN)
        return nf_bits(nf_mul(0x1p-95f, 0x1p-95f));   /* underflow */
    /* argument reduction */
    if (hx > 0x3eb17218) {   /* if  |x| > 0.5 ln2 */
        if (hx < 0x3F851592) {   /* and |x| < 1.5 ln2 */
            hi = nf_sub(x, ln2HI[xsb]);
            lo = ln2LO[xsb];
            k = 1 - xsb - xsb;
        } else {
            k = nf_f2i(nf_add(nf_mul(x, invln2), halF[xsb]));
            t = nf_i2f(k);
            hi = nf_sub(x, nf_mul(t, ln2HI[0]));   /* t*ln2HI is exact here */
            lo = nf_mul(t, ln2LO[0]);
        }
        x = nf_sub(hi, lo);
    } else if (hx < 0x31800000) {   /* when |x|<2**-28 */
        if (nf_gt(nf_add(x, huge), one))
            return nf_bits(nf_add(x, one));   /* trigger inexact */
    }
    /* x is now in primary range */
    t = nf_mul(x, x);
    /* c = x - t*(P1+t*(P2+t*(P3+t*(P4+t*P5)))) */
    c = nf_add(nf_mul(t, P5), P4);
    c = nf_add(nf_mul(t, c), P3);
    c = nf_add(nf_mul(t, c), P2);
    c = nf_sub(x, nf_mul(t, nf_add(nf_mul(t, c), P1)));
    if (k == 0)   /* one-((x*c)/(c-(float)2.0)-x) */
        return nf_bits(nf_sub(one, nf_sub(nf_div(nf_mul(x, c), nf_sub(c, 2.0f)), x)));
    /* one-((lo-(x*c)/((float)2.0-c))-hi) */
    y = nf_sub(one, nf_sub(nf_sub(lo, nf_div(nf_mul(x, c), nf_sub(2.0f, c))), hi));
    u32 hy = nf_bits(y);
    if (k >= -125)
        return hy + ((u32)k << 23);   /* add k to y's exponent */
    return nf_bits(nf_mul(nf_val(hy + ((u32)(k + 100) << 23)), twom100));
}

static float CrtImpl_expf(float x)
{
    return nf_val(m_expf(nf_bits(x)));
}

float expf(float x)
{
    return CrtImpl_expf(x);
}

/* The library's __ieee754_sqrtf (ef_sqrt.c, fdlibm's bit-by-bit root).
 * The tests settled that it rounds to nearest: acosf with a root rounded
 * toward zero, as the FPU's sqrt.s gives it, differs on 1,009,910 of the
 * 2^32 inputs, and on none with this. So its "one-tiny" test of the
 * rounding direction finds round-to-nearest (z >= one, one+tiny == one:
 * q += q&1). An exponent of 255 is a number (FLT_UWORD_IS_FINITE), a
 * denormal is a zero and returned as it is (FLT_UWORD_IS_ZERO). */
static u32 m_sqrtf(u32 xb)
{
    s32 ix = (s32)xb, s, q, m, t;
    u32 r, hx = xb & 0x7fffffff;
    if (FLT_UWORD_IS_ZERO(hx))
        return xb;   /* sqrt(+-0) = +-0 */
    if (ix < 0) {   /* sqrt(-ve) = sNaN: (x-x)/(x-x) */
        float x = nf_val(xb);
        return nf_bits(nf_div(nf_sub(x, x), nf_sub(x, x)));
    }
    /* normalize x */
    m = (ix >> 23);
    m -= 127;   /* unbias exponent */
    ix = (ix & 0x007fffff) | 0x00800000;
    if (m & 1)   /* odd m, double x to make it even */
        ix += ix;
    m >>= 1;   /* m = [m/2] */
    /* generate sqrt(x) bit by bit */
    ix += ix;
    q = s = 0;   /* q = sqrt(x) */
    r = 0x01000000;   /* r = moving bit from right to left */
    while (r != 0) {
        t = s + (s32)r;
        if (t <= ix) {
            s = t + (s32)r;
            ix -= t;
            q += (s32)r;
        }
        ix += ix;
        r >>= 1;
    }
    /* the rounding direction: to nearest */
    if (ix != 0)
        q += (q & 1);
    ix = (q >> 1) + 0x3f000000;
    ix += (s32)((u32)m << 23);
    return (u32)ix;
}

/* acosf (ef_acos.c; the wrapper returns __ieee754_acosf's result). */
static const float pi = 0x1.921fb4p+1f;       /* 3.1415925026e+00 */
static const float pio2_hi = 0x1.921fb4p+0f;  /* 1.5707962513e+00 */
static const float pio2_lo = 0x1.4442dp-24f;  /* 7.5497894159e-08 */
static const float pS0 = 0x1.555554p-3f;      /* 1.6666667163e-01 */
static const float pS1 = -0x1.4d611ep-2f;     /* -3.2556581497e-01 */
static const float pS2 = 0x1.9c155p-3f;       /* 2.0121252537e-01 */
static const float pS3 = -0x1.48228cp-5f;     /* -4.0055535734e-02 */
static const float pS4 = 0x1.9efe08p-11f;     /* 7.9153501429e-04 */
static const float pS5 = 0x1.23de1p-15f;      /* 3.4793309169e-05 */
static const float qS1 = -0x1.33a27p+1f;      /* -2.4033949375e+00 */
static const float qS2 = 0x1.02ae58p+1f;      /* 2.0209457874e+00 */
static const float qS3 = -0x1.6066cp-1f;      /* -6.8828397989e-01 */
static const float qS4 = 0x1.3b8c5ap-4f;      /* 7.7038154006e-02 */

/* p = z*(pS0+z*(pS1+z*(pS2+z*(pS3+z*(pS4+z*pS5))))), q = one+z*(qS1+z*(qS2+z*(qS3+z*qS4))): r = p/q */
static float acos_r(float z)
{
    float p = nf_add(nf_mul(z, pS5), pS4);
    p = nf_add(nf_mul(z, p), pS3);
    p = nf_add(nf_mul(z, p), pS2);
    p = nf_add(nf_mul(z, p), pS1);
    p = nf_mul(z, nf_add(nf_mul(z, p), pS0));
    float q = nf_add(nf_mul(z, qS4), qS3);
    q = nf_add(nf_mul(z, q), qS2);
    q = nf_add(nf_mul(z, nf_add(nf_mul(z, q), qS1)), one);
    return nf_div(p, q);
}

static u32 m_acosf(u32 xb)
{
    float x = nf_val(xb), z, r, w, s, c, df;
    s32 hx = (s32)xb, ix = hx & 0x7fffffff;
    if (ix == 0x3f800000) {   /* |x|==1 */
        if (hx > 0)
            return 0;   /* acos(1) = 0  */
        return nf_bits(nf_add(nf_mul(2.0f, pio2_lo), pi));   /* acos(-1)= pi */
    } else if (ix > 0x3f800000) {   /* |x| >= 1 */
        return nf_bits(nf_div(nf_sub(x, x), nf_sub(x, x)));   /* acos(|x|>1) is NaN */
    }
    if (ix < 0x3f000000) {   /* |x| < 0.5 */
        if (ix <= 0x23000000)
            return nf_bits(nf_add(pio2_hi, pio2_lo));   /*if|x|<2**-57*/
        z = nf_mul(x, x);
        r = acos_r(z);
        return nf_bits(nf_sub(pio2_hi, nf_sub(x, nf_sub(pio2_lo, nf_mul(x, r)))));
    } else if (hx < 0) {   /* x < -0.5 */
        z = nf_mul(nf_add(x, one), 0.5f);
        s = nf_val(m_sqrtf(nf_bits(z)));
        r = acos_r(z);
        w = nf_sub(nf_mul(r, s), pio2_lo);
        return nf_bits(nf_sub(pi, nf_mul(nf_add(s, w), 2.0f)));
    }
    /* x > 0.5 */
    z = nf_mul(nf_sub(one, x), 0.5f);
    s = nf_val(m_sqrtf(nf_bits(z)));
    df = nf_val(nf_bits(s) & 0xfffff000);
    c = nf_div(nf_sub(z, nf_mul(df, df)), nf_add(s, df));
    r = acos_r(z);
    w = nf_add(nf_mul(r, s), c);
    return nf_bits(nf_mul(nf_add(df, w), 2.0f));
}

static float CrtImpl_acosf(float x)
{
    return nf_val(m_acosf(nf_bits(x)));
}

float acosf(float x)
{
    return CrtImpl_acosf(x);
}

/* atan2f (ef_atan2.c; the wrapper returns __ieee754_atan2f's result):
 * atan2f(y, x), y in $f12 and x in $f13. pi_lo is newlib 1.8.2's (4.6.0
 * has pi 3.1415927410e+00 and pi_lo -8.7422776573e-08: pi's bits are the
 * same toward zero, and the tests settled pi_lo: atan2f(FLT_MIN, -1) is
 * 0x40490FDA). */
static const float tiny = 0x1.4484bep-100f;   /* 1.0e-30 */
static const float pi_o_2 = 0x1.921fb4p+0f;   /* 1.5707963705e+00 */
static const float pi_lo = 0x1.4442dp-23f;    /* 1.5099578832e-07 */

static u32 m_atan2f(u32 yb, u32 xb)
{
    float x = nf_val(xb), y = nf_val(yb), z;
    s32 k, m, hx = (s32)xb, hy = (s32)yb, ix = hx & 0x7fffffff, iy = hy & 0x7fffffff;
    if (hx == 0x3f800000)
        return m_atanf(yb);   /* x=1.0 */
    m = ((hy >> 31) & 1) | ((hx >> 30) & 2);   /* 2*sign(x)+sign(y) */
    /* when y = 0 */
    if (FLT_UWORD_IS_ZERO((u32)iy)) {
        switch (m) {
        case 0:
        case 1:
            return yb;   /* atan(+-0,+anything)=+-0 */
        case 2:
            return nf_bits(nf_add(pi, tiny));   /* atan(+0,-anything) = pi */
        default:
            return nf_bits(nf_sub(nf_neg(pi), tiny));   /* atan(-0,-anything) =-pi */
        }
    }
    /* when x = 0 */
    if (FLT_UWORD_IS_ZERO((u32)ix))
        return nf_bits(hy < 0 ? nf_sub(nf_neg(pi_o_2), tiny) : nf_add(pi_o_2, tiny));
    /* compute y/x (no infinities) */
    k = (iy - ix) >> 23;
    if (k > 60)
        z = nf_add(nf_mul(0.5f, pi_lo), pi_o_2);   /* |y/x| >  2**60 */
    else if (hx < 0 && k < -60)
        z = 0.0f;   /* |y|/x < -2**60 */
    else
        z = nf_val(m_atanf(nf_bits(nf_abs(nf_div(y, x)))));   /* safe to do y/x */
    switch (m) {
    case 0:
        return nf_bits(z);   /* atan(+,+) */
    case 1:
        return nf_bits(z) ^ 0x80000000u;   /* atan(-,+) */
    case 2:
        return nf_bits(nf_sub(pi, nf_sub(z, pi_lo)));   /* atan(+,-) */
    default:
        return nf_bits(nf_sub(nf_sub(z, pi_lo), pi));   /* atan(-,-) */
    }
}

static float CrtImpl_atan2f(float x, float y)
{
    return nf_val(m_atan2f(nf_bits(x), nf_bits(y)));
}

float atan2f(float x, float y)
{
    return CrtImpl_atan2f(x, y);
}

/* scalbnf (common/sf_scalbn.c, no denormals: FLT_SMALLEST_EXP 1), for
 * powf's results below the normals. */
static u32 m_scalbnf(u32 xb, int n)
{
    s32 ix = (s32)xb, k;
    u32 hx = xb & 0x7fffffff, sign = xb & 0x80000000u;
    k = (s32)(hx >> 23);   /* extract exponent */
    if (FLT_UWORD_IS_ZERO(hx))
        return xb;
    if (n > 50000)   /* in case integer overflow in n+k */
        return nf_bits(nf_mul(nf_val(sign | nf_bits(huge)), huge));   /*overflow*/
    k = k + n;
    if (k > 255)
        return nf_bits(nf_mul(nf_val(sign | nf_bits(huge)), huge));   /* overflow  */
    if (k > 0)   /* normal result */
        return (u32)((ix & (s32)0x807fffff) | (k << 23));
    return nf_bits(nf_mul(nf_val(sign | nf_bits(tiny)), tiny));   /*underflow*/
}

/* powf (ef_pow.c; the wrapper returns __ieee754_powf's result). The tests
 * (2^32 samples plus the edge cases, and probes of each place newlib's
 * versions differ) settled a newlib between 1.8.2 and 4.6.0: newlib
 * 1.8.2's code (x-1 in the |y| > 2^27 path with its bound 0x3f7ffff8, the
 * t_h with its 0x0040000, cp_h and cp_l of 0x3f763800, t masked with
 * 0xfffff000), with fdlibm.h's FLT_UWORD macros of 4.6.0 for the zeros
 * (a denormal x or y is a zero) and the over/underflow bounds
 * (FLT_UWORD_EXP_MAX 0x43010000, FLT_UWORD_EXP_MIN 0x42fc0000). x in
 * $f12, y in $f13. */
static const float bp[] = {1.0f, 1.5f};
static const float dp_h[] = {0.0f, 0x1.2b8p-1f};      /* 5.84960938e-01 */
static const float dp_l[] = {0.0f, 0x1.a39fb8p-20f};  /* 1.56322085e-06 */
static const float L1 = 0x1.333332p-1f;      /* 6.0000002384e-01 */
static const float L2 = 0x1.b6db6ep-2f;      /* 4.2857143283e-01 */
static const float L3 = 0x1.555556p-2f;      /* 3.3333334327e-01 */
static const float L4 = 0x1.174608p-2f;      /* 2.7272811532e-01 */
static const float L5 = 0x1.d864a8p-3f;      /* 2.3066075146e-01 */
static const float L6 = 0x1.a7e284p-3f;      /* 2.0697501302e-01 */
static const float lg2 = 0x1.62e42ep-1f;     /* 6.9314718246e-01 */
static const float lg2_h = 0x1.62e4p-1f;     /* 6.93145752e-01 */
static const float lg2_l = 0x1.7f7d18p-20f;  /* 1.42860654e-06 */
static const float ovt = 0x1.715476p-25f;    /* 4.2995665694e-08 */
static const float cp = 0x1.ec709cp-1f;      /* 9.6179670095e-01 */
static const float cp_h = 0x1.ec7p-1f;       /* 9.6179199219e-01 */
static const float cp_l = 0x1.3b873ep-18f;   /* 4.7017383622e-06 */
static const float ivln2 = 0x1.715474p+0f;   /* 1.4426950216e+00 */
static const float ivln2_h = 0x1.7154p+0f;   /* 1.4426879883e+00 */
static const float ivln2_l = 0x1.d94adep-18f; /* 7.0526075433e-06 */
static const float third = 0x1.555554p-2f;   /* 0.333333333333 */
#define FLT_UWORD_EXP_MAX 0x43010000
#define FLT_UWORD_EXP_MIN 0x42fc0000

static u32 m_powf(u32 xb, u32 yb)
{
    float x = nf_val(xb), y = nf_val(yb), z, ax, z_h, z_l, p_h, p_l, y1, t1, t2, r, s, t, u, v, w;
    s32 i, j, k, yisint, n, hx = (s32)xb, hy = (s32)yb, ix = hx & 0x7fffffff, iy = hy & 0x7fffffff;
    /* y==zero: x**0 = 1 */
    if (FLT_UWORD_IS_ZERO((u32)iy))
        return nf_bits(one);
    /* determine if y is an odd int when x < 0
     * yisint = 0 ... y is not an integer
     * yisint = 1 ... y is an odd int
     * yisint = 2 ... y is an even int */
    yisint = 0;
    if (hx < 0) {
        if (iy >= 0x4b800000)
            yisint = 2;   /* even integer y */
        else if (iy >= 0x3f800000) {
            k = (iy >> 23) - 0x7f;   /* exponent */
            j = iy >> (23 - k);
            if ((j << (23 - k)) == iy)
                yisint = 2 - (j & 1);
        }
    }
    /* special value of y (no infinities) */
    if (iy == 0x3f800000) {   /* y is  +-1 */
        if (hy < 0)
            return nf_bits(nf_div(one, x));
        return xb;
    }
    if (hy == 0x40000000)
        return nf_bits(nf_mul(x, x));   /* y is  2 */
    if (hy == 0x3f000000 && hx >= 0)   /* y is  0.5, x >= +0 */
        return m_sqrtf(xb);
    ax = nf_val(xb & 0x7fffffff);   /* fabsf(x) */
    /* special value of x */
    if (FLT_UWORD_IS_ZERO((u32)ix) || ix == 0x3f800000) {
        z = ax;   /*x is +-0,+-1*/
        if (hy < 0)
            z = nf_div(one, z);   /* z = (1/|x|) */
        if (hx < 0) {
            if (((ix - 0x3f800000) | yisint) == 0)
                z = nf_div(nf_sub(z, z), nf_sub(z, z));   /* (-1)**non-int is NaN */
            else if (yisint == 1)
                z = nf_neg(z);   /* (x<0)**odd = -(|x|**odd) */
        }
        return nf_bits(z);
    }
    /* (x<0)**(non-int) is NaN */
    if (((((u32)hx >> 31) - 1) | (u32)yisint) == 0)
        return nf_bits(nf_div(nf_sub(x, x), nf_sub(x, x)));
    /* |y| is huge */
    if (iy > 0x4d000000) {   /* if |y| > 2**27 */
        /* over/underflow if x is not close to one */
        if (ix < 0x3f7ffff8)
            return hy < 0 ? PS2FP_FMAX : nf_bits(nf_mul(tiny, tiny));   /* huge*huge is the library's constant */
        if (ix > 0x3f800007)
            return hy > 0 ? PS2FP_FMAX : nf_bits(nf_mul(tiny, tiny));
        /* now |1-x| is tiny <= 2**-20, suffice to compute
           log(x) by x-x^2/2+x^3/3-x^4/4 */
        t = nf_sub(x, one);   /* t has 20 trailing zeros */
        w = nf_mul(nf_mul(t, t), nf_sub(0.5f, nf_mul(t, nf_sub(third, nf_mul(t, 0.25f)))));
        u = nf_mul(t, ivln2_h);   /* ivln2_h has 16 sig. bits */
        v = nf_sub(nf_mul(t, ivln2_l), nf_mul(w, ivln2));
        t1 = nf_add(u, v);
        t1 = nf_val(nf_bits(t1) & 0xfffff000);
        t2 = nf_sub(v, nf_sub(t1, u));
    } else {
        float s2, s_h, s_l, t_h, t_l;
        n = 0;
        /* (no subnormal x: a denormal is a zero) */
        n += ((ix) >> 23) - 0x7f;
        j = ix & 0x007fffff;
        /* determine interval */
        ix = j | 0x3f800000;   /* normalize ix */
        if (j <= 0x1cc471)
            k = 0;   /* |x|<sqrt(3/2) */
        else if (j < 0x5db3d7)
            k = 1;   /* |x|<sqrt(3)   */
        else {
            k = 0;
            n += 1;
            ix -= 0x00800000;
        }
        ax = nf_val((u32)ix);
        /* compute s = s_h+s_l = (x-1)/(x+1) or (x-1.5)/(x+1.5) */
        u = nf_sub(ax, bp[k]);   /* bp[0]=1.0, bp[1]=1.5 */
        v = nf_div(one, nf_add(ax, bp[k]));
        s = nf_mul(u, v);
        s_h = nf_val(nf_bits(s) & 0xfffff000);
        /* t_h=ax+bp[k] High (newlib 1.8.2's 0x0040000) */
        t_h = nf_val((u32)(((ix >> 1) | 0x20000000) + 0x0040000 + (k << 21)));
        t_l = nf_sub(ax, nf_sub(t_h, bp[k]));
        s_l = nf_mul(v, nf_sub(nf_sub(u, nf_mul(s_h, t_h)), nf_mul(s_h, t_l)));
        /* compute log(ax) */
        s2 = nf_mul(s, s);
        /* r = s2*s2*(L1+s2*(L2+s2*(L3+s2*(L4+s2*(L5+s2*L6))))) */
        r = nf_add(nf_mul(s2, L6), L5);
        r = nf_add(nf_mul(s2, r), L4);
        r = nf_add(nf_mul(s2, r), L3);
        r = nf_add(nf_mul(s2, r), L2);
        r = nf_mul(nf_mul(s2, s2), nf_add(nf_mul(s2, r), L1));
        r = nf_add(r, nf_mul(s_l, nf_add(s_h, s)));
        s2 = nf_mul(s_h, s_h);
        t_h = nf_add(nf_add(s2, 3.0f), r);
        t_h = nf_val(nf_bits(t_h) & 0xfffff000);
        t_l = nf_sub(r, nf_sub(nf_sub(t_h, 3.0f), s2));
        /* u+v = s*(1+...) */
        u = nf_mul(s_h, t_h);
        v = nf_add(nf_mul(s_l, t_h), nf_mul(t_l, s));
        /* 2/(3log2)*(s+...) */
        p_h = nf_add(u, v);
        p_h = nf_val(nf_bits(p_h) & 0xfffff000);
        p_l = nf_sub(v, nf_sub(p_h, u));
        z_h = nf_mul(p_h, cp_h);   /* cp_h+cp_l = 2/(3*log2) */
        z_l = nf_add(nf_add(nf_mul(p_h, cp_l), nf_mul(p_l, cp)), dp_l[k]);
        /* log2(ax) = (s+..)*2/(3*log2) = n + dp_h + z_h + z_l */
        t = nf_i2f(n);
        t1 = nf_add(nf_add(nf_add(z_h, z_l), dp_h[k]), t);
        t1 = nf_val(nf_bits(t1) & 0xfffff000);
        t2 = nf_sub(z_l, nf_sub(nf_sub(nf_sub(t1, t), dp_h[k]), z_h));
    }
    s = one;   /* s (sign of result -ve**odd) = -1 else = 1 */
    if (((((u32)hx >> 31) - 1) | (u32)(yisint - 1)) == 0)
        s = nf_neg(one);   /* (-ve)**(odd int) */
    /* split up y into y1+y2 and compute (y1+y2)*(t1+t2) */
    y1 = nf_val(yb & 0xfffff000);
    p_l = nf_add(nf_mul(nf_sub(y, y1), t1), nf_mul(y, t2));
    p_h = nf_mul(y1, t1);
    z = nf_add(p_l, p_h);
    j = (s32)nf_bits(z);
    i = j & 0x7fffffff;
    if (j > 0) {
        if (i > FLT_UWORD_EXP_MAX)
            return nf_bits(nf_mul(nf_mul(s, huge), huge));   /* overflow */
        else if (i == FLT_UWORD_EXP_MAX && nf_gt(nf_add(p_l, ovt), nf_sub(z, p_h)))
            return nf_bits(nf_mul(nf_mul(s, huge), huge));   /* overflow */
    } else {
        if (i > FLT_UWORD_EXP_MIN)
            return nf_bits(nf_mul(nf_mul(s, tiny), tiny));   /* underflow */
        else if (i == FLT_UWORD_EXP_MIN && nf_le(p_l, nf_sub(z, p_h)))
            return nf_bits(nf_mul(nf_mul(s, tiny), tiny));   /* underflow */
    }
    /* compute 2**(p_h+p_l) */
    k = (i >> 23) - 0x7f;
    n = 0;
    if (i > 0x3f000000) {   /* if |z| > 0.5, set n = [z+0.5] */
        n = j + (0x00800000 >> (k + 1));
        k = ((n & 0x7fffffff) >> 23) - 0x7f;   /* new k for n */
        t = nf_val((u32)(n & ~(0x007fffff >> k)));
        n = ((n & 0x007fffff) | 0x00800000) >> (23 - k);
        if (j < 0)
            n = -n;
        p_h = nf_sub(p_h, t);
    }
    t = nf_add(p_l, p_h);
    t = nf_val(nf_bits(t) & 0xfffff000);
    u = nf_mul(t, lg2_h);
    v = nf_add(nf_mul(nf_sub(p_l, nf_sub(t, p_h)), lg2), nf_mul(t, lg2_l));
    z = nf_add(u, v);
    w = nf_sub(v, nf_sub(z, u));
    t = nf_mul(z, z);
    /* t1 = z - t*(P1+t*(P2+t*(P3+t*(P4+t*P5)))) */
    t1 = nf_add(nf_mul(t, P5), P4);
    t1 = nf_add(nf_mul(t, t1), P3);
    t1 = nf_add(nf_mul(t, t1), P2);
    t1 = nf_sub(z, nf_mul(t, nf_add(nf_mul(t, t1), P1)));
    r = nf_sub(nf_div(nf_mul(z, t1), nf_sub(t1, 2.0f)), nf_add(w, nf_mul(z, w)));
    z = nf_sub(one, nf_sub(r, z));
    j = (s32)nf_bits(z);
    j += (s32)((u32)n << 23);
    if ((j >> 23) <= 0)
        z = nf_val(m_scalbnf(nf_bits(z), n));   /* subnormal output */
    else
        z = nf_val((u32)j);
    return nf_bits(nf_mul(s, z));
}

static float CrtImpl_powf(float x, float y)
{
    return nf_val(m_powf(nf_bits(x), nf_bits(y)));
}

float powf(float x, float y)
{
    return CrtImpl_powf(x, y);
}

/* sinf and cosf (sf_sin.c, sf_cos.c, with kf_sin.c, kf_cos.c,
 * ef_rem_pio2.c and kf_rem_pio2.c; tables from ef_rem_pio2.c). */
#define REM_PIO2_IH_SHIFT 8   /* kf_rem_pio2.c when q0 == 0: ih = iq[jz-1]>>8 (newlib 1.8.2; 4.6.0 has >>7) */
static const float S1 = -0x1.555554p-3f;   /* -1.6666667163e-01 */
static const float S2 = 0x1.111112p-7f;    /* 8.3333337680e-03 */
static const float S3 = -0x1.a01a02p-13f;  /* -1.9841270114e-04 */
static const float S4 = 0x1.71de34p-19f;   /* 2.7557314297e-06 */
static const float S5 = -0x1.ae5e68p-26f;  /* -2.5050759689e-08 */
static const float S6 = 0x1.5d93a4p-33f;   /* 1.5896910177e-10 */
static const float C1 = 0x1.555554p-5f;    /* 4.1666667908e-02 */
static const float C2 = -0x1.6c16cp-10f;   /* -1.3888889225e-03 */
static const float C3 = 0x1.a01ap-16f;     /* 2.4801587642e-05 */
static const float C4 = -0x1.27e4f6p-22f;  /* -2.7557314297e-07 */
static const float C5 = 0x1.1ee9eap-29f;   /* 2.0875723372e-09 */
static const float C6 = -0x1.8fae9cp-37f;  /* -1.1359647598e-11 */

static u32 k_sinf(u32 xb, float y, int iy)
{
    float x = nf_val(xb), z, r, v;
    u32 ix = xb & 0x7fffffff;   /* high word of x */
    if (ix < 0x32000000)   /* |x| < 2**-27 */
        return xb;   /* (int)x == 0: generate inexact */
    z = nf_mul(x, x);
    v = nf_mul(z, x);
    /* r = S2+z*(S3+z*(S4+z*(S5+z*S6))) */
    r = nf_add(nf_mul(z, S6), S5);
    r = nf_add(nf_mul(z, r), S4);
    r = nf_add(nf_mul(z, r), S3);
    r = nf_add(nf_mul(z, r), S2);
    if (iy == 0)
        return nf_bits(nf_add(x, nf_mul(v, nf_add(nf_mul(z, r), S1))));
    /* x-((z*(half*y-v*r)-y)-v*S1) */
    return nf_bits(nf_sub(x, nf_sub(nf_sub(nf_mul(z, nf_sub(nf_mul(y, 0.5f), nf_mul(v, r))), y), nf_mul(v, S1))));
}

static u32 k_cosf(u32 xb, float y)
{
    float x = nf_val(xb), a, hz, z, r, qx;
    u32 ix = xb & 0x7fffffff;   /* ix = |x|'s high word*/
    if (ix < 0x32000000)   /* if x < 2**27 */
        return nf_bits(one);   /* (int)x == 0: generate inexact */
    z = nf_mul(x, x);
    /* r = z*(C1+z*(C2+z*(C3+z*(C4+z*(C5+z*C6))))) */
    r = nf_add(nf_mul(z, C6), C5);
    r = nf_add(nf_mul(z, r), C4);
    r = nf_add(nf_mul(z, r), C3);
    r = nf_add(nf_mul(z, r), C2);
    r = nf_mul(z, nf_add(nf_mul(z, r), C1));
    if (ix < 0x3e99999a)   /* if |x| < 0.3 */
        return nf_bits(nf_sub(one, nf_sub(nf_mul(z, 0.5f), nf_sub(nf_mul(z, r), nf_mul(x, y)))));
    if (ix > 0x3f480000)   /* x > 0.78125 */
        qx = 0.28125f;
    else
        qx = nf_val(ix - 0x01000000);   /* x/4 */
    hz = nf_sub(nf_mul(z, 0.5f), qx);
    a = nf_sub(one, qx);
    return nf_bits(nf_sub(a, nf_sub(hz, nf_sub(nf_mul(z, r), nf_mul(x, y)))));
}

static const s32 two_over_pi[] = {
    0xA2, 0xF9, 0x83, 0x6E, 0x4E, 0x44, 0x15, 0x29, 0xFC, 0x27, 0x57, 0xD1, 0xF5, 0x34, 0xDD, 0xC0, 0xDB, 0x62,
    0x95, 0x99, 0x3C, 0x43, 0x90, 0x41, 0xFE, 0x51, 0x63, 0xAB, 0xDE, 0xBB, 0xC5, 0x61, 0xB7, 0x24, 0x6E, 0x3A,
    0x42, 0x4D, 0xD2, 0xE0, 0x06, 0x49, 0x2E, 0xEA, 0x09, 0xD1, 0x92, 0x1C, 0xFE, 0x1D, 0xEB, 0x1C, 0xB1, 0x29,
    0xA7, 0x3E, 0xE8, 0x82, 0x35, 0xF5, 0x2E, 0xBB, 0x44, 0x84, 0xE9, 0x9C, 0x70, 0x26, 0xB4, 0x5F, 0x7E, 0x41,
    0x39, 0x91, 0xD6, 0x39, 0x83, 0x53, 0x39, 0xF4, 0x9C, 0x84, 0x5F, 0x8B, 0xBD, 0xF9, 0x28, 0x3B, 0x1F, 0xF8,
    0x97, 0xFF, 0xDE, 0x05, 0x98, 0x0F, 0xEF, 0x2F, 0x11, 0x8B, 0x5A, 0x0A, 0x6D, 0x1F, 0x6D, 0x36, 0x7E, 0xCF,
    0x27, 0xCB, 0x09, 0xB7, 0x4F, 0x46, 0x3F, 0x66, 0x9E, 0x5F, 0xEA, 0x2D, 0x75, 0x27, 0xBA, 0xC7, 0xEB, 0xE5,
    0xF1, 0x7B, 0x3D, 0x07, 0x39, 0xF7, 0x8A, 0x52, 0x92, 0xEA, 0x6B, 0xFB, 0x5F, 0xB1, 0x1F, 0x8D, 0x5D, 0x08,
    0x56, 0x03, 0x30, 0x46, 0xFC, 0x7B, 0x6B, 0xAB, 0xF0, 0xCF, 0xBC, 0x20, 0x9A, 0xF4, 0x36, 0x1D, 0xA9, 0xE3,
    0x91, 0x61, 0x5E, 0xE6, 0x1B, 0x08, 0x65, 0x99, 0x85, 0x5F, 0x14, 0xA0, 0x68, 0x40, 0x8D, 0xFF, 0xD8, 0x80,
    0x4D, 0x73, 0x27, 0x31, 0x06, 0x06, 0x15, 0x56, 0xCA, 0x73, 0xA8, 0xC9, 0x60, 0xE2, 0x7B, 0xC0, 0x8C, 0x6B,
};
static const s32 npio2_hw[] = {
    0x3fc90f00, 0x40490f00, 0x4096cb00, 0x40c90f00, 0x40fb5300, 0x4116cb00, 0x412fed00, 0x41490f00,
    0x41623100, 0x417b5300, 0x418a3a00, 0x4196cb00, 0x41a35c00, 0x41afed00, 0x41bc7e00, 0x41c90f00,
    0x41d5a000, 0x41e23100, 0x41eec200, 0x41fb5300, 0x4203f200, 0x420a3a00, 0x42108300, 0x4216cb00,
    0x421d1400, 0x42235c00, 0x4229a500, 0x422fed00, 0x42363600, 0x423c7e00, 0x4242c700, 0x42490f00,
};
static const float PIo2[] = {
    0x1.92p+0f,       /* 1.5703125000e+00 */
    0x1.ep-12f,       /* 4.5776367188e-04 */
    0x1.b3fffep-16f,  /* 2.5987625122e-05 */
    0x1.44p-24f,      /* 7.5437128544e-08 */
    0x1.07fffep-34f,  /* 6.0026650317e-11 */
    0x1.9ffffep-41f,  /* 7.3896444519e-13 */
    0x1.83fffep-48f,  /* 5.3845816694e-15 */
    0x1.9ffffep-58f,  /* 5.6378512969e-18 */
    0x1.88p-64f,      /* 8.3009228831e-20 */
    0x1.8bfffep-72f,  /* 3.2756352257e-22 */
    0x1.88p-81f,      /* 6.3331015649e-25 */
};
static const float two8 = 256.0f, twon8 = 0.00390625f;
static const float invpio2 = 0x1.45f308p-1f;  /* 6.3661980629e-01 */
static const float pio2_1 = 0x1.921fp+0f;     /* 1.5707855225e+00 */
static const float pio2_1t = 0x1.6a8884p-17f; /* 1.0804334124e-05 */
static const float pio2_2 = 0x1.6a87fep-17f;  /* 1.0804273188e-05 */
static const float pio2_2t = 0x1.0b461p-34f; /* 6.0770999344e-11 */
static const float pio2_3 = 0x1.0b46p-34f;    /* 6.0770943833e-11 */
static const float pio2_3t = 0x1.1a6262p-54f; /* 6.1232342629e-17 */

/* __kernel_rem_pio2f with prec 2 (jk 9) */
static int k_rem_pio2f(const float *x, float *y, int e0, int nx)
{
    s32 jz, jx, jv, jp, jk, carry, n, iq[20], i, j, k, m, q0, ih;
    float z, fw, f[20], fq[20], q[20];
    jk = 9;
    jp = jk;
    /* determine jx,jv,q0, note that 3>q0 */
    jx = nx - 1;
    jv = (e0 - 3) / 8;
    if (jv < 0)
        jv = 0;
    q0 = e0 - 8 * (jv + 1);
    /* set up f[0] to f[jx+jk] where f[jx+jk] = ipio2[jv+jk] */
    j = jv - jx;
    m = jx + jk;
    for (i = 0; i <= m; i++, j++)
        f[i] = (j < 0) ? 0.0f : nf_i2f(two_over_pi[j]);
    /* compute q[0],q[1],...q[jk] */
    for (i = 0; i <= jk; i++) {
        for (j = 0, fw = 0.0f; j <= jx; j++)
            fw = nf_add(fw, nf_mul(x[j], f[jx + i - j]));
        q[i] = fw;
    }
    jz = jk;
recompute:
    /* distill q[] into iq[] reversingly */
    for (i = 0, j = jz, z = q[jz]; j > 0; i++, j--) {
        fw = nf_i2f(nf_f2i(nf_mul(z, twon8)));
        iq[i] = nf_f2i(nf_sub(z, nf_mul(fw, two8)));
        z = nf_add(q[j - 1], fw);
    }
    /* compute n */
    z = nf_val(m_scalbnf(nf_bits(z), (int)q0));   /* actual value of z */
    z = nf_sub(z, nf_mul(nf_val(m_floorf(nf_bits(nf_mul(z, 0.125f)))), 8.0f));   /* trim off integer >= 8 */
    n = nf_f2i(z);
    z = nf_sub(z, nf_i2f(n));
    ih = 0;
    if (q0 > 0) {   /* need iq[jz-1] to determine n */
        i = (iq[jz - 1] >> (8 - q0));
        n += i;
        iq[jz - 1] -= i << (8 - q0);
        ih = iq[jz - 1] >> (7 - q0);
    } else if (q0 == 0)
        ih = iq[jz - 1] >> REM_PIO2_IH_SHIFT;
    else if (nf_ge(z, 0.5f))
        ih = 2;
    if (ih > 0) {   /* q > 0.5 */
        n += 1;
        carry = 0;
        for (i = 0; i < jz; i++) {   /* compute 1-q */
            j = iq[i];
            if (carry == 0) {
                if (j != 0) {
                    carry = 1;
                    iq[i] = 0x100 - j;
                }
            } else
                iq[i] = 0xff - j;
        }
        if (q0 > 0) {   /* rare case: chance is 1 in 12 */
            switch (q0) {
            case 1:
                iq[jz - 1] &= 0x7f;
                break;
            case 2:
                iq[jz - 1] &= 0x3f;
                break;
            }
        }
        if (ih == 2) {
            z = nf_sub(one, z);
            if (carry != 0)
                z = nf_sub(z, nf_val(m_scalbnf(nf_bits(one), (int)q0)));
        }
    }
    /* check if recomputation is needed */
    if (nf_eq(z, 0.0f)) {
        j = 0;
        for (i = jz - 1; i >= jk; i--)
            j |= iq[i];
        if (j == 0) {   /* need recomputation */
            for (k = 1; iq[jk - k] == 0; k++)
                ;   /* k = no. of terms needed */
            for (i = jz + 1; i <= jz + k; i++) {   /* add q[jz+1] to q[jz+k] */
                f[jx + i] = nf_i2f(two_over_pi[jv + i]);
                for (j = 0, fw = 0.0f; j <= jx; j++)
                    fw = nf_add(fw, nf_mul(x[j], f[jx + i - j]));
                q[i] = fw;
            }
            jz += k;
            goto recompute;
        }
    }
    /* chop off zero terms */
    if (nf_eq(z, 0.0f)) {
        jz -= 1;
        q0 -= 8;
        while (iq[jz] == 0) {
            jz--;
            q0 -= 8;
        }
    } else {   /* break z into 8-bit if necessary */
        z = nf_val(m_scalbnf(nf_bits(z), -(int)q0));
        if (nf_ge(z, two8)) {
            fw = nf_i2f(nf_f2i(nf_mul(z, twon8)));
            iq[jz] = nf_f2i(nf_sub(z, nf_mul(fw, two8)));
            jz += 1;
            q0 += 8;
            iq[jz] = nf_f2i(fw);
        } else
            iq[jz] = nf_f2i(z);
    }
    /* convert integer "bit" chunk to floating-point value */
    fw = nf_val(m_scalbnf(nf_bits(one), (int)q0));
    for (i = jz; i >= 0; i--) {
        q[i] = nf_mul(fw, nf_i2f(iq[i]));
        fw = nf_mul(fw, twon8);
    }
    /* compute PIo2[0,...,jp]*q[jz,...,0] */
    for (i = jz; i >= 0; i--) {
        for (fw = 0.0f, k = 0; k <= jp && k <= jz - i; k++)
            fw = nf_add(fw, nf_mul(PIo2[k], q[i + k]));
        fq[jz - i] = fw;
    }
    /* compress fq[] into y[] (prec 2) */
    fw = 0.0f;
    for (i = jz; i >= 0; i--)
        fw = nf_add(fw, fq[i]);
    y[0] = (ih == 0) ? fw : nf_neg(fw);
    fw = nf_sub(fq[0], fw);
    for (i = 1; i <= jz; i++)
        fw = nf_add(fw, fq[i]);
    y[1] = (ih == 0) ? fw : nf_neg(fw);
    return n & 7;
}

/* __ieee754_rem_pio2f: x - n*pi/2 in y[0] + y[1], returns n */
static s32 rem_pio2f(u32 xb, float *y)
{
    float x = nf_val(xb), z, w, t, r, fn, tx[3];
    s32 i, j, n, ix, hx = (s32)xb;
    int e0, nx;
    ix = hx & 0x7fffffff;
    if (ix <= 0x3f490fd8) {   /* |x| ~<= pi/4 , no need for reduction */
        y[0] = x;
        y[1] = 0;
        return 0;
    }
    if (ix < 0x4016cbe4) {   /* |x| < 3pi/4, special case with n=+-1 */
        if (hx > 0) {
            z = nf_sub(x, pio2_1);
            if ((ix & 0xfffffff0) != 0x3fc90fd0) {   /* 24+24 bit pi OK */
                y[0] = nf_sub(z, pio2_1t);
                y[1] = nf_sub(nf_sub(z, y[0]), pio2_1t);
            } else {   /* near pi/2, use 24+24+24 bit pi */
                z = nf_sub(z, pio2_2);
                y[0] = nf_sub(z, pio2_2t);
                y[1] = nf_sub(nf_sub(z, y[0]), pio2_2t);
            }
            return 1;
        }
        /* negative x */
        z = nf_add(x, pio2_1);
        if ((ix & 0xfffffff0) != 0x3fc90fd0) {   /* 24+24 bit pi OK */
            y[0] = nf_add(z, pio2_1t);
            y[1] = nf_add(nf_sub(z, y[0]), pio2_1t);
        } else {   /* near pi/2, use 24+24+24 bit pi */
            z = nf_add(z, pio2_2);
            y[0] = nf_add(z, pio2_2t);
            y[1] = nf_add(nf_sub(z, y[0]), pio2_2t);
        }
        return -1;
    }
    if (ix <= 0x43490f80) {   /* |x| ~<= 2^7*(pi/2), medium size */
        t = nf_abs(x);
        n = nf_f2i(nf_add(nf_mul(t, invpio2), 0.5f));
        fn = nf_i2f(n);
        r = nf_sub(t, nf_mul(fn, pio2_1));
        w = nf_mul(fn, pio2_1t);   /* 1st round good to 40 bit */
        if (n < 32 && (ix & (s32)0xffffff00) != npio2_hw[n - 1]) {
            y[0] = nf_sub(r, w);   /* quick check no cancellation */
        } else {
            u32 high;
            j = ix >> 23;
            y[0] = nf_sub(r, w);
            high = nf_bits(y[0]);
            i = j - (s32)((high >> 23) & 0xff);
            if (i > 8) {   /* 2nd iteration needed, good to 57 */
                t = r;
                w = nf_mul(fn, pio2_2);
                r = nf_sub(t, w);
                w = nf_sub(nf_mul(fn, pio2_2t), nf_sub(nf_sub(t, r), w));
                y[0] = nf_sub(r, w);
                high = nf_bits(y[0]);
                i = j - (s32)((high >> 23) & 0xff);
                if (i > 25) {   /* 3rd iteration need, 74 bits acc */
                    t = r;   /* will cover all possible cases */
                    w = nf_mul(fn, pio2_3);
                    r = nf_sub(t, w);
                    w = nf_sub(nf_mul(fn, pio2_3t), nf_sub(nf_sub(t, r), w));
                    y[0] = nf_sub(r, w);
                }
            }
        }
        y[1] = nf_sub(nf_sub(r, y[0]), w);
        if (hx < 0) {
            y[0] = nf_neg(y[0]);
            y[1] = nf_neg(y[1]);
            return -n;
        }
        return n;
    }
    /* all other (large) arguments (an exponent of 255 is a number) */
    /* set z = scalbn(|x|,ilogb(x)-7) */
    e0 = (int)((ix >> 23) - 134);   /* e0 = ilogb(z)-7; */
    z = nf_val((u32)(ix - ((s32)e0 << 23)));
    for (i = 0; i < 2; i++) {
        tx[i] = nf_i2f(nf_f2i(z));
        z = nf_mul(nf_sub(z, tx[i]), two8);
    }
    tx[2] = z;
    nx = 3;
    while (nf_eq(tx[nx - 1], 0.0f))
        nx--;   /* skip zero term */
    n = k_rem_pio2f(tx, y, e0, nx);
    if (hx < 0) {
        y[0] = nf_neg(y[0]);
        y[1] = nf_neg(y[1]);
        return -n;
    }
    return n;
}

static u32 m_sinf(u32 xb)
{
    float y[2];
    s32 n;
    u32 ix = xb & 0x7fffffff;
    /* |x| ~< pi/4 */
    if (ix <= 0x3f490fd8)
        return k_sinf(xb, 0.0f, 0);
    /* argument reduction needed (no Inf or NaN) */
    n = rem_pio2f(xb, y);
    switch (n & 3) {
    case 0:
        return k_sinf(nf_bits(y[0]), y[1], 1);
    case 1:
        return k_cosf(nf_bits(y[0]), y[1]);
    case 2:
        return nf_bits(nf_neg(nf_val(k_sinf(nf_bits(y[0]), y[1], 1))));
    default:
        return nf_bits(nf_neg(nf_val(k_cosf(nf_bits(y[0]), y[1]))));
    }
}

static u32 m_cosf(u32 xb)
{
    float y[2];
    s32 n;
    u32 ix = xb & 0x7fffffff;
    /* |x| ~< pi/4 */
    if (ix <= 0x3f490fd8)
        return k_cosf(xb, 0.0f);
    /* argument reduction needed (no Inf or NaN) */
    n = rem_pio2f(xb, y);
    switch (n & 3) {
    case 0:
        return k_cosf(nf_bits(y[0]), y[1]);
    case 1:
        return nf_bits(nf_neg(nf_val(k_sinf(nf_bits(y[0]), y[1], 1))));
    case 2:
        return nf_bits(nf_neg(nf_val(k_cosf(nf_bits(y[0]), y[1]))));
    default:
        return k_sinf(nf_bits(y[0]), y[1], 1);
    }
}

static float CrtImpl_sinf(float x)
{
    return nf_val(m_sinf(nf_bits(x)));
}

float sinf(float x)
{
    return CrtImpl_sinf(x);
}

static float CrtImpl_cosf(float x)
{
    return nf_val(m_cosf(nf_bits(x)));
}

float cosf(float x)
{
    return CrtImpl_cosf(x);
}

static float CrtImpl_atanf(float x)
{
    return nf_val(m_atanf(nf_bits(x)));
}

float atanf(float x)
{
    return CrtImpl_atanf(x);
}

static float CrtImpl_floorf(float x)
{
    return nf_val(m_floorf(nf_bits(x)));
}

float floorf(float x)
{
    return CrtImpl_floorf(x);
}

static float CrtImpl_ceilf(float x)
{
    return nf_val(m_ceilf(nf_bits(x)));
}

float ceilf(float x)
{
    return CrtImpl_ceilf(x);
}
