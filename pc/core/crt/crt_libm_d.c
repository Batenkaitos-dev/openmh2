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

/* The C library's double math functions (tan, atan2, sqrt) on PC: re-hosted
 * from the earlier port's libm_d.c (Monster Hunter 2 decomp;
 * listed in THIRD_PARTY.md), with the guest registers gone and the
 * soft doubles in ps2dp.h.
 *
 * Taken from newlib 1.8.2's libm/math and libm/common (fdlibm: s_tan.c,
 * k_tan.c, e_rem_pio2.c, k_rem_pio2.c, s_floor.c, s_scalbn.c,
 * s_copysign.c, e_atan2.c, s_atan.c, e_sqrt.c) and adapted: the EE has no
 * double-precision FPU, so the library's compiled code does every double
 * operation through the C library's soft-double helpers, and so does this
 * file (d_add ... d_div below): add and subtract are libgcc.c's model of
 * dpadd and dpsub, multiply and divide are IEEE arithmetic rounded toward
 * zero with no denormals (the unnamed
 * multiply and divide after dpsub), conversions are litodp's and dptoli's.
 * A function is replaced only when it matches the
 * original one bit for bit on the inputs Capcom's code can pass.
 * That is how each function here was checked.
 *
 * A double argument or result is the 64 bits of an EE double (a host double
 * that only carries them: no operation of the host touches it). */
#include "ps2crt.h"
#include "crt_tap.h"
#include "ps2dp.h"

typedef u64 dbl;   /* a double as its bits */

#define D_SIGN 0x8000000000000000ull
#define D_EXP  0x7FF0000000000000ull
#define D_FRAC 0x000FFFFFFFFFFFFFull
#define D_QNAN 0x7FF8000000000000ull

#define HI(x) ((s32)((x) >> 32))
#define LO(x) ((u32)(x))
#define WORDS(h, l) (((u64)(u32)(h) << 32) | (u32)(l))
#define SET_LO(x, l) ((x) = WORDS(HI(x), (l)))
#define SET_HI(x, h) ((x) = WORDS((h), LO(x)))

/* The constants: newlib's decimal literals as the game's toolchain
 * converted them. DC(toward zero, nearest) (the same program with -d). */
#ifdef LIBM_D_NEAREST
#define DC(rz, rn) (rn##ull)
#else
#define DC(rz, rn) (rz##ull)
#endif

/* ------------------------------------------------------- soft doubles */
/* The C library's double helpers (dpadd, dpsub, litodp, dptoli) and its unnamed
 * multiply and divide: ps2dp.h, re-hosted from the earlier port's libgcc.c and from this
 * file's own model, and tested against both
 * (the ps2fp tests, oracle_dp). */
static inline dbl d_add(dbl a, dbl b) { return ps2dp_add(a, b); }
static inline dbl d_sub(dbl a, dbl b) { return ps2dp_sub(a, b); }
static inline dbl d_neg(dbl a) { return a ^ D_SIGN; }
static inline dbl d_abs(dbl a) { return a & ~D_SIGN; }
static inline dbl d_fromi(s32 v) { return ps2dp_from_si(v); }
static inline s32 d_toi(dbl a) { return ps2dp_to_si(a); }
static inline int d_isnan(dbl a) { return (a & ~D_SIGN) > D_EXP; }
static inline dbl d_mul(dbl a, dbl b) { return ps2dp_mul(a, b); }
static inline dbl d_div(dbl a, dbl b) { return ps2dp_div(a, b); }

/* The compares: zeros (and denormals, as zeros) equal, a NaN unordered. */
static inline s64 d_key(dbl a)
{
    if (!(a & D_EXP))
        return 0;
    return (a & D_SIGN) ? -(s64)(a & ~D_SIGN) : (s64)a;
}
static inline int d_lt(dbl a, dbl b) { return !d_isnan(a) && !d_isnan(b) && d_key(a) < d_key(b); }
static inline int d_le(dbl a, dbl b) { return !d_isnan(a) && !d_isnan(b) && d_key(a) <= d_key(b); }
static inline int d_eq(dbl a, dbl b) { return !d_isnan(a) && !d_isnan(b) && d_key(a) == d_key(b); }
static inline int d_gt(dbl a, dbl b) { return d_lt(b, a); }
static inline int d_ge(dbl a, dbl b) { return d_le(b, a); }

static const dbl zero = 0, half = 0x3FE0000000000000ull, one = 0x3FF0000000000000ull,
                 two = 0x4000000000000000ull, m_one = 0xBFF0000000000000ull;
static const dbl huge = DC(0x7E37E43C8800759B, 0x7E37E43C8800759C);   /* 1.0e300 */
static const dbl tiny = DC(0x01A56E1FC2F8F358, 0x01A56E1FC2F8F359);   /* 1.0e-300 */

/* ------------------------------------------- copysign, scalbn, floor */
static dbl m_copysign(dbl x, dbl y)
{
    return (x & ~D_SIGN) | (y & D_SIGN);
}

/* scalbn (s_scalbn.c): x * 2^n by its exponent. */
static const dbl two54 = 0x4350000000000000ull, twom54 = 0x3C90000000000000ull;
static dbl m_scalbn(dbl x, int n)
{
    s32 k, hx = HI(x), lx = (s32)LO(x);
    k = (hx & 0x7ff00000) >> 20;   /* extract exponent */
    if (k == 0) {                   /* 0 or subnormal x */
        if ((lx | (hx & 0x7fffffff)) == 0)
            return x;   /* +-0 */
        x = d_mul(x, two54);
        hx = HI(x);
        k = ((hx & 0x7ff00000) >> 20) - 54;
        if (n < -50000)
            return d_mul(tiny, x);   /* underflow */
    }
    if (k == 0x7ff)
        return d_add(x, x);   /* NaN or Inf */
    k = k + n;
    if (k > 0x7fe)
        return d_mul(huge, m_copysign(huge, x));   /* overflow */
    if (k > 0) {   /* normal result */
        SET_HI(x, (hx & 0x800fffff) | (k << 20));
        return x;
    }
    if (k <= -54) {
        if (n > 50000)   /* in case integer overflow in n+k */
            return d_mul(huge, m_copysign(huge, x));   /* overflow */
        return d_mul(tiny, m_copysign(tiny, x));   /* underflow */
    }
    k += 54;   /* subnormal result */
    SET_HI(x, (hx & 0x800fffff) | (k << 20));
    return d_mul(x, twom54);
}

/* floor (s_floor.c): x rounded toward -inf to an integral value. */
static dbl m_floor(dbl x)
{
    s32 i0 = HI(x), i1 = (s32)LO(x), j0;
    u32 i, j;
    j0 = ((i0 >> 20) & 0x7ff) - 0x3ff;
    if (j0 < 20) {
        if (j0 < 0) {   /* raise inexact if x != 0 */
            if (d_gt(d_add(huge, x), zero)) {   /* return 0*sign(x) if |x|<1 */
                if (i0 >= 0) {
                    i0 = i1 = 0;
                } else if (((i0 & 0x7fffffff) | i1) != 0) {
                    i0 = (s32)0xbff00000u;
                    i1 = 0;
                }
            }
        } else {
            i = (0x000fffffu) >> j0;
            if (((i0 & (s32)i) | i1) == 0)
                return x;   /* x is integral */
            if (d_gt(d_add(huge, x), zero)) {   /* raise inexact flag */
                if (i0 < 0)
                    i0 += (0x00100000) >> j0;
                i0 &= (s32)(~i);
                i1 = 0;
            }
        }
    } else if (j0 > 51) {
        if (j0 == 0x400)
            return d_add(x, x);   /* inf or NaN */
        return x;   /* x is integral */
    } else {
        i = 0xffffffffu >> (j0 - 20);
        if ((i1 & (s32)i) == 0)
            return x;   /* x is integral */
        if (d_gt(d_add(huge, x), zero)) {   /* raise inexact flag */
            if (i0 < 0) {
                if (j0 == 20) {
                    i0 += 1;
                } else {
                    j = (u32)i1 + (1u << (52 - j0));
                    if (j < (u32)i1)
                        i0 += 1;   /* got a carry */
                    i1 = (s32)j;
                }
            }
            i1 &= (s32)(~i);
        }
    }
    return WORDS(i0, i1);
}

/* ------------------------------------------------------------------ tan */
/* __kernel_tan (k_tan.c, newlib 1.8.2): tan(x + y) on [-pi/4, pi/4], y
 * the tail of x; iy 1 for tan, -1 for -1/tan. */
static const dbl pio4 = DC(0x3FE921FB54442D17, 0x3FE921FB54442D18);     /* 7.85398163397448278999e-01 */
static const dbl pio4lo = DC(0x3C81A62633145C06, 0x3C81A62633145C07);   /* 3.06161699786838301793e-17 */
static const dbl T[] = {
    0x3FD5555555555563ull,                             /* 3.33333333333334091986e-01 */
    0x3FC111111110FE7Aull,                             /* 1.33333333333201242699e-01 */
    0x3FABA1BA1BB341FEull,                             /* 5.39682539762260521377e-02 */
    0x3F9664F48406D637ull,                             /* 2.18694882948595424599e-02 */
    0x3F8226E3E96E8493ull,                             /* 8.86323982359930005737e-03 */
    DC(0x3F6D6D22C9560327, 0x3F6D6D22C9560328),        /* 3.59207910759131235356e-03 */
    DC(0x3F57DBC8FEE08314, 0x3F57DBC8FEE08315),        /* 1.45620945432529025516e-03 */
    0x3F4344D8F2F26501ull,                             /* 5.88041240820264096874e-04 */
    0x3F3026F71A8D1068ull,                             /* 2.46463134818469906812e-04 */
    DC(0x3F147E88A03792A5, 0x3F147E88A03792A6),        /* 7.81794442939557092300e-05 */
    DC(0x3F12B80F32F0A7E8, 0x3F12B80F32F0A7E9),        /* 7.14072491382608190305e-05 */
    0xBEF375CBDB605373ull,                             /* -1.85586374855275456654e-05 */
    DC(0x3EFB2A7074BF7AD3, 0x3EFB2A7074BF7AD4),        /* 2.59073051863633712884e-05 */
};

static dbl k_tan(dbl x, dbl y, int iy)
{
    dbl z, r, v, w, s;
    s32 ix, hx = HI(x);
    ix = hx & 0x7fffffff;   /* high word of |x| */
    if (ix < 0x3e300000) {   /* x < 2**-28 */
        if (d_toi(x) == 0) {   /* generate inexact */
            u32 low = LO(x);
            if (((ix | (s32)low) | (iy + 1)) == 0)
                return d_div(one, d_abs(x));
            return (iy == 1) ? x : d_div(m_one, x);
        }
    }
    if (ix >= 0x3FE59428) {   /* |x|>=0.6744 */
        if (hx < 0) {
            x = d_neg(x);
            y = d_neg(y);
        }
        z = d_sub(pio4, x);
        w = d_sub(pio4lo, y);
        x = d_add(z, w);
        y = zero;
    }
    z = d_mul(x, x);
    w = d_mul(z, z);
    /* Break x^5*(T[1]+x^2*T[2]+...) into
     *    x^5(T[1]+x^4*T[3]+...+x^20*T[11]) +
     *    x^5(x^2*(T[2]+x^4*T[4]+...+x^22*[T12])) */
    r = d_add(T[1], d_mul(w, d_add(T[3], d_mul(w, d_add(T[5], d_mul(w, d_add(T[7], d_mul(w, d_add(T[9], d_mul(w, T[11]))))))))));
    v = d_mul(z, d_add(T[2], d_mul(w, d_add(T[4], d_mul(w, d_add(T[6], d_mul(w, d_add(T[8], d_mul(w, d_add(T[10], d_mul(w, T[12])))))))))));
    s = d_mul(z, x);
    r = d_add(y, d_mul(z, d_add(d_mul(s, d_add(r, v)), y)));
    r = d_add(r, d_mul(T[0], s));
    w = d_add(x, r);
    if (ix >= 0x3FE59428) {
        v = d_fromi(iy);
        return d_mul(d_fromi(1 - ((hx >> 30) & 2)),
                     d_sub(v, d_mul(two, d_sub(x, d_sub(d_div(d_mul(w, w), d_add(w, v)), r)))));
    }
    if (iy == 1)
        return w;
    /* compute -1.0/(x+r) accurately */
    {
        dbl a, t;
        z = w;
        SET_LO(z, 0);
        v = d_sub(r, d_sub(z, x));   /* z+v = r+x */
        t = a = d_div(m_one, w);     /* a = -1.0/w */
        SET_LO(t, 0);
        s = d_add(one, d_mul(t, z));
        return d_add(t, d_mul(a, d_add(s, d_mul(t, v))));
    }
}

/* __kernel_rem_pio2 (k_rem_pio2.c, newlib 1.8.2), for prec 2 (the only
 * one __ieee754_rem_pio2 asks for): x[0..nx-1], 24-bit pieces of the
 * argument scaled by 2^-e0, reduced by pi/2 into y[0] + y[1]; returns the
 * quadrant (n & 7). */
static const dbl PIo2[] = {
    DC(0x3FF921FB3FFFFFFF, 0x3FF921FB40000000), /* 1.57079625129699707031e+00 */
    DC(0x3E74442CFFFFFFFF, 0x3E74442D00000000), /* 7.54978941586159635335e-08 */
    DC(0x3CF846987FFFFFFF, 0x3CF8469880000000), /* 5.39030252995776476554e-15 */
    DC(0x3B78CC515FFFFFFF, 0x3B78CC5160000000), /* 3.28200341580791294123e-22 */
    DC(0x39F01B837FFFFFFF, 0x39F01B8380000000), /* 1.27065575308067607349e-29 */
    DC(0x387A25203FFFFFFF, 0x387A252040000000), /* 1.22933308981111328932e-36 */
    0x36E3822280000000ull,                      /* 2.73370053816464559624e-44 */
    0x3569F31D00000000ull,                      /* 2.16741683877804819444e-51 */
};
static const dbl two24 = 0x4170000000000000ull, twon24 = 0x3E70000000000000ull,
                 eight = 0x4020000000000000ull, eighth = 0x3FC0000000000000ull;

static int k_rem_pio2(const dbl *x, dbl *y, int e0, int nx, const s32 *ipio2)
{
    s32 jz, jx, jv, jp, jk, carry, n, iq[20], i, j, k, m, q0, ih;
    dbl z, fw, f[20], fq[20], q[20];

    /* initialize jk */
    jk = 4;   /* init_jk[2] */
    jp = jk;
    /* determine jx,jv,q0, note that 3>q0 */
    jx = nx - 1;
    jv = (e0 - 3) / 24;
    if (jv < 0)
        jv = 0;
    q0 = e0 - 24 * (jv + 1);
    /* set up f[0] to f[jx+jk] where f[jx+jk] = ipio2[jv+jk] */
    j = jv - jx;
    m = jx + jk;
    for (i = 0; i <= m; i++, j++)
        f[i] = (j < 0) ? zero : d_fromi(ipio2[j]);
    /* compute q[0],q[1],...q[jk] */
    for (i = 0; i <= jk; i++) {
        for (j = 0, fw = zero; j <= jx; j++)
            fw = d_add(fw, d_mul(x[j], f[jx + i - j]));
        q[i] = fw;
    }
    jz = jk;
recompute:
    /* distill q[] into iq[] reversingly */
    for (i = 0, j = jz, z = q[jz]; j > 0; i++, j--) {
        fw = d_fromi(d_toi(d_mul(twon24, z)));
        iq[i] = d_toi(d_sub(z, d_mul(two24, fw)));
        z = d_add(q[j - 1], fw);
    }
    /* compute n */
    z = m_scalbn(z, (int)q0);                         /* actual value of z */
    z = d_sub(z, d_mul(eight, m_floor(d_mul(z, eighth))));   /* trim off integer >= 8 */
    n = d_toi(z);
    z = d_sub(z, d_fromi(n));
    ih = 0;
    if (q0 > 0) {   /* need iq[jz-1] to determine n */
        i = (iq[jz - 1] >> (24 - q0));
        n += i;
        iq[jz - 1] -= i << (24 - q0);
        ih = iq[jz - 1] >> (23 - q0);
    } else if (q0 == 0) {
        ih = iq[jz - 1] >> 23;
    } else if (d_ge(z, half)) {
        ih = 2;
    }
    if (ih > 0) {   /* q > 0.5 */
        n += 1;
        carry = 0;
        for (i = 0; i < jz; i++) {   /* compute 1-q */
            j = iq[i];
            if (carry == 0) {
                if (j != 0) {
                    carry = 1;
                    iq[i] = 0x1000000 - j;
                }
            } else {
                iq[i] = 0xffffff - j;
            }
        }
        if (q0 > 0) {   /* rare case: chance is 1 in 12 */
            switch (q0) {
            case 1:
                iq[jz - 1] &= 0x7fffff;
                break;
            case 2:
                iq[jz - 1] &= 0x3fffff;
                break;
            }
        }
        if (ih == 2) {
            z = d_sub(one, z);
            if (carry != 0)
                z = d_sub(z, m_scalbn(one, (int)q0));
        }
    }
    /* check if recomputation is needed */
    if (d_eq(z, zero)) {
        j = 0;
        for (i = jz - 1; i >= jk; i--)
            j |= iq[i];
        if (j == 0) {   /* need recomputation */
            for (k = 1; iq[jk - k] == 0; k++)
                ;   /* k = no. of terms needed */
            for (i = jz + 1; i <= jz + k; i++) {   /* add q[jz+1] to q[jz+k] */
                f[jx + i] = d_fromi(ipio2[jv + i]);
                for (j = 0, fw = zero; j <= jx; j++)
                    fw = d_add(fw, d_mul(x[j], f[jx + i - j]));
                q[i] = fw;
            }
            jz += k;
            goto recompute;
        }
    }
    /* chop off zero terms */
    if (d_eq(z, zero)) {
        jz -= 1;
        q0 -= 24;
        while (iq[jz] == 0) {
            jz--;
            q0 -= 24;
        }
    } else {   /* break z into 24-bit if necessary */
        z = m_scalbn(z, -(int)q0);
        if (d_ge(z, two24)) {
            fw = d_fromi(d_toi(d_mul(twon24, z)));
            iq[jz] = d_toi(d_sub(z, d_mul(two24, fw)));
            jz += 1;
            q0 += 24;
            iq[jz] = d_toi(fw);
        } else {
            iq[jz] = d_toi(z);
        }
    }
    /* convert integer "bit" chunk to floating-point value */
    fw = m_scalbn(one, (int)q0);
    for (i = jz; i >= 0; i--) {
        q[i] = d_mul(fw, d_fromi(iq[i]));
        fw = d_mul(fw, twon24);
    }
    /* compute PIo2[0,...,jp]*q[jz,...,0] */
    for (i = jz; i >= 0; i--) {
        for (fw = zero, k = 0; k <= jp && k <= jz - i; k++)
            fw = d_add(fw, d_mul(PIo2[k], q[i + k]));
        fq[jz - i] = fw;
    }
    /* compress fq[] into y[] (prec 2) */
    fw = zero;
    for (i = jz; i >= 0; i--)
        fw = d_add(fw, fq[i]);
    y[0] = (ih == 0) ? fw : d_neg(fw);
    fw = d_sub(fq[0], fw);
    for (i = 1; i <= jz; i++)
        fw = d_add(fw, fq[i]);
    y[1] = (ih == 0) ? fw : d_neg(fw);
    return n & 7;
}

/* __ieee754_rem_pio2 (e_rem_pio2.c, newlib 1.8.2): x reduced by pi/2 into
 * y[0] + y[1]; returns the quadrant. */
/* Table of constants for 2/pi, 396 Hex digits (476 decimal) of 2/pi */
static const s32 two_over_pi[] = {
    0xA2F983, 0x6E4E44, 0x1529FC, 0x2757D1, 0xF534DD, 0xC0DB62,
    0x95993C, 0x439041, 0xFE5163, 0xABDEBB, 0xC561B7, 0x246E3A,
    0x424DD2, 0xE00649, 0x2EEA09, 0xD1921C, 0xFE1DEB, 0x1CB129,
    0xA73EE8, 0x8235F5, 0x2EBB44, 0x84E99C, 0x7026B4, 0x5F7E41,
    0x3991D6, 0x398353, 0x39F49C, 0x845F8B, 0xBDF928, 0x3B1FF8,
    0x97FFDE, 0x05980F, 0xEF2F11, 0x8B5A0A, 0x6D1F6D, 0x367ECF,
    0x27CB09, 0xB74F46, 0x3F669E, 0x5FEA2D, 0x7527BA, 0xC7EBE5,
    0xF17B3D, 0x0739F7, 0x8A5292, 0xEA6BFB, 0x5FB11F, 0x8D5D08,
    0x560330, 0x46FC7B, 0x6BABF0, 0xCFBC20, 0x9AF436, 0x1DA9E3,
    0x91615E, 0xE61B08, 0x659985, 0x5F14A0, 0x68408D, 0xFFD880,
    0x4D7327, 0x310606, 0x1556CA, 0x73A8C9, 0x60E27B, 0xC08C6B,
};
static const s32 npio2_hw[] = {
    0x3FF921FB, 0x400921FB, 0x4012D97C, 0x401921FB, 0x401F6A7A, 0x4022D97C,
    0x4025FDBB, 0x402921FB, 0x402C463A, 0x402F6A7A, 0x4031475C, 0x4032D97C,
    0x40346B9C, 0x4035FDBB, 0x40378FDB, 0x403921FB, 0x403AB41B, 0x403C463A,
    0x403DD85A, 0x403F6A7A, 0x40407E4C, 0x4041475C, 0x4042106C, 0x4042D97C,
    0x4043A28C, 0x40446B9C, 0x404534AC, 0x4045FDBB, 0x4046C6CB, 0x40478FDB,
    0x404858EB, 0x404921FB,
};
static const dbl invpio2 = 0x3FE45F306DC9C883ull;                        /* 6.36619772367581382433e-01 */
static const dbl pio2_1 = 0x3FF921FB54400000ull;                         /* 1.57079632673412561417e+00 */
static const dbl pio2_1t = 0x3DD0B4611A626331ull;                        /* 6.07710050650619224932e-11 */
static const dbl pio2_2 = 0x3DD0B4611A600000ull;                         /* 6.07710050630396597660e-11 */
static const dbl pio2_2t = DC(0x3BA3198A2E037072, 0x3BA3198A2E037073);   /* 2.02226624879595063154e-21 */
static const dbl pio2_3 = 0x3BA3198A2E000000ull;                         /* 2.02226624871116645580e-21 */
static const dbl pio2_3t = DC(0x397B839A252049C0, 0x397B839A252049C1);   /* 8.47842766036889956997e-32 */

static s32 rem_pio2(dbl x, dbl *y)
{
    dbl z = 0, w, t, r, fn;
    dbl tx[3];
    s32 i, j, n, ix, hx;
    int e0, nx;
    u32 low;

    hx = HI(x);   /* high word of x */
    ix = hx & 0x7fffffff;
    if (ix <= 0x3fe921fb) {   /* |x| ~<= pi/4 , no need for reduction */
        y[0] = x;
        y[1] = zero;
        return 0;
    }
    if (ix < 0x4002d97c) {   /* |x| < 3pi/4, special case with n=+-1 */
        if (hx > 0) {
            z = d_sub(x, pio2_1);
            if (ix != 0x3ff921fb) {   /* 33+53 bit pi is good enough */
                y[0] = d_sub(z, pio2_1t);
                y[1] = d_sub(d_sub(z, y[0]), pio2_1t);
            } else {   /* near pi/2, use 33+33+53 bit pi */
                z = d_sub(z, pio2_2);
                y[0] = d_sub(z, pio2_2t);
                y[1] = d_sub(d_sub(z, y[0]), pio2_2t);
            }
            return 1;
        } else {   /* negative x */
            z = d_add(x, pio2_1);
            if (ix != 0x3ff921fb) {   /* 33+53 bit pi is good enough */
                y[0] = d_add(z, pio2_1t);
                y[1] = d_add(d_sub(z, y[0]), pio2_1t);
            } else {   /* near pi/2, use 33+33+53 bit pi */
                z = d_add(z, pio2_2);
                y[0] = d_add(z, pio2_2t);
                y[1] = d_add(d_sub(z, y[0]), pio2_2t);
            }
            return -1;
        }
    }
    if (ix <= 0x413921fb) {   /* |x| ~<= 2^19*(pi/2), medium size */
        t = d_abs(x);
        n = d_toi(d_add(d_mul(t, invpio2), half));
        fn = d_fromi(n);
        r = d_sub(t, d_mul(fn, pio2_1));
        w = d_mul(fn, pio2_1t);   /* 1st round good to 85 bit */
        if (n < 32 && ix != npio2_hw[n - 1]) {
            y[0] = d_sub(r, w);   /* quick check no cancellation */
        } else {
            u32 high;
            j = ix >> 20;
            y[0] = d_sub(r, w);
            high = (u32)HI(y[0]);
            i = j - ((high >> 20) & 0x7ff);
            if (i > 16) {   /* 2nd iteration needed, good to 118 */
                t = r;
                w = d_mul(fn, pio2_2);
                r = d_sub(t, w);
                w = d_sub(d_mul(fn, pio2_2t), d_sub(d_sub(t, r), w));
                y[0] = d_sub(r, w);
                high = (u32)HI(y[0]);
                i = j - ((high >> 20) & 0x7ff);
                if (i > 49) {   /* 3rd iteration need, 151 bits acc */
                    t = r;      /* will cover all possible cases */
                    w = d_mul(fn, pio2_3);
                    r = d_sub(t, w);
                    w = d_sub(d_mul(fn, pio2_3t), d_sub(d_sub(t, r), w));
                    y[0] = d_sub(r, w);
                }
            }
        }
        y[1] = d_sub(d_sub(r, y[0]), w);
        if (hx < 0) {
            y[0] = d_neg(y[0]);
            y[1] = d_neg(y[1]);
            return -n;
        }
        return n;
    }
    /* all other (large) arguments */
    if (ix >= 0x7ff00000) {   /* x is inf or NaN */
        y[0] = y[1] = d_sub(x, x);
        return 0;
    }
    /* set z = scalbn(|x|,ilogb(x)-23) */
    low = LO(x);
    SET_LO(z, low);
    e0 = (int)((ix >> 20) - 1046);   /* e0 = ilogb(z)-23; */
    SET_HI(z, ix - ((s32)e0 << 20));
    for (i = 0; i < 2; i++) {
        tx[i] = d_fromi(d_toi(z));
        z = d_mul(d_sub(z, tx[i]), two24);
    }
    tx[2] = z;
    nx = 3;
    while (d_eq(tx[nx - 1], zero))
        nx--;   /* skip zero term */
    n = k_rem_pio2(tx, y, e0, nx, two_over_pi);
    if (hx < 0) {
        y[0] = d_neg(y[0]);
        y[1] = d_neg(y[1]);
        return -n;
    }
    return n;
}

/* tan (s_tan.c). */
static dbl m_tan(dbl x)
{
    dbl y[2], z = zero;
    s32 n, ix = HI(x) & 0x7fffffff;
    if (ix <= 0x3fe921fb)   /* |x| ~< pi/4 */
        return k_tan(x, z, 1);
    if (ix >= 0x7ff00000)   /* tan(Inf or NaN) is NaN */
        return d_sub(x, x);
    n = rem_pio2(x, y);   /* argument reduction needed */
    return k_tan(y[0], y[1], 1 - ((n & 1) << 1));   /* 1 -- n even, -1 -- n odd */
}

/* ---------------------------------------------------------- atan, atan2 */
/* atan (s_atan.c, newlib 1.8.2). */
static const dbl atanhi[] = {
    DC(0x3FDDAC670561BB4E, 0x3FDDAC670561BB4F), /* 4.63647609000806093515e-01 atan(0.5)hi */
    DC(0x3FE921FB54442D17, 0x3FE921FB54442D18), /* 7.85398163397448278999e-01 atan(1.0)hi */
    DC(0x3FEF730BD281F69A, 0x3FEF730BD281F69B), /* 9.82793723247329054082e-01 atan(1.5)hi */
    0x3FF921FB54442D18ull,                      /* 1.57079632679489655800e+00 atan(inf)hi */
};
static const dbl atanlo[] = {
    DC(0x3C7A2B7F222F65E1, 0x3C7A2B7F222F65E2), /* 2.26987774529616870924e-17 atan(0.5)lo */
    DC(0x3C81A62633145C06, 0x3C81A62633145C07), /* 3.06161699786838301793e-17 atan(1.0)lo */
    0x3C7007887AF0CBBDull,                      /* 1.39033110312309984516e-17 atan(1.5)lo */
    0x3C91A62633145C07ull,                      /* 6.12323399573676603587e-17 atan(inf)lo */
};
static const dbl aT[] = {
    0x3FD555555555550Dull,                      /* 3.33333333333329318027e-01 */
    DC(0xBFC999999998EBC3, 0xBFC999999998EBC4), /* -1.99999999998764832476e-01 */
    0x3FC24924920083FFull,                      /* 1.42857142725034663711e-01 */
    0xBFBC71C6FE231671ull,                      /* -1.11111104054623557880e-01 */
    0x3FB745CDC54C206Eull,                      /* 9.09088713343650656196e-02 */
    DC(0xBFB3B0F2AF749A6C, 0xBFB3B0F2AF749A6D), /* -7.69187620504482999495e-02 */
    0x3FB10D66A0D03D51ull,                      /* 6.66107313738753120669e-02 */
    DC(0xBFADDE2D52DEFD99, 0xBFADDE2D52DEFD9A), /* -5.83357013379057348645e-02 */
    0x3FA97B4B24760DEBull,                      /* 4.97687799461593236017e-02 */
    DC(0xBFA2B4442C6A6C2E, 0xBFA2B4442C6A6C2F), /* -3.65315727442169155270e-02 */
    DC(0x3F90AD3AE322DA10, 0x3F90AD3AE322DA11), /* 1.62858201153657823623e-02 */
};
static const dbl one_half = 0x3FF8000000000000ull;   /* 1.5 */

static dbl m_atan(dbl x)
{
    dbl w, s1, s2, z;
    s32 ix, hx = HI(x), id;
    ix = hx & 0x7fffffff;
    if (ix >= 0x44100000) {   /* if |x| >= 2^66 */
        u32 low = LO(x);
        if (ix > 0x7ff00000 || (ix == 0x7ff00000 && (low != 0)))
            return d_add(x, x);   /* NaN */
        if (hx > 0)
            return d_add(atanhi[3], atanlo[3]);
        return d_sub(d_neg(atanhi[3]), atanlo[3]);
    }
    if (ix < 0x3fdc0000) {   /* |x| < 0.4375 */
        if (ix < 0x3e200000) {   /* |x| < 2^-29 */
            if (d_gt(d_add(huge, x), one))
                return x;   /* raise inexact */
        }
        id = -1;
    } else {
        x = d_abs(x);
        if (ix < 0x3ff30000) {   /* |x| < 1.1875 */
            if (ix < 0x3fe60000) {   /* 7/16 <=|x|<11/16 */
                id = 0;
                x = d_div(d_sub(d_mul(two, x), one), d_add(two, x));
            } else {   /* 11/16<=|x|< 19/16 */
                id = 1;
                x = d_div(d_sub(x, one), d_add(x, one));
            }
        } else {
            if (ix < 0x40038000) {   /* |x| < 2.4375 */
                id = 2;
                x = d_div(d_sub(x, one_half), d_add(one, d_mul(one_half, x)));
            } else {   /* 2.4375 <= |x| < 2^66 */
                id = 3;
                x = d_div(m_one, x);
            }
        }
    }
    /* end of argument reduction */
    z = d_mul(x, x);
    w = d_mul(z, z);
    /* break sum from i=0 to 10 aT[i]z**(i+1) into odd and even poly */
    s1 = d_mul(z, d_add(aT[0], d_mul(w, d_add(aT[2], d_mul(w, d_add(aT[4], d_mul(w, d_add(aT[6], d_mul(w, d_add(aT[8], d_mul(w, aT[10])))))))))));
    s2 = d_mul(w, d_add(aT[1], d_mul(w, d_add(aT[3], d_mul(w, d_add(aT[5], d_mul(w, d_add(aT[7], d_mul(w, aT[9])))))))));
    if (id < 0)
        return d_sub(x, d_mul(x, d_add(s1, s2)));
    z = d_sub(atanhi[id], d_sub(d_sub(d_mul(x, d_add(s1, s2)), atanlo[id]), x));
    return (hx < 0) ? d_neg(z) : z;
}

/* __ieee754_atan2 (e_atan2.c, newlib 1.8.2). */
static const dbl pi_o_4 = 0x3FE921FB54442D18ull;                       /* 7.8539816339744827900E-01 */
static const dbl pi_o_2 = 0x3FF921FB54442D18ull;                       /* 1.5707963267948965580E+00 */
static const dbl pi = 0x400921FB54442D18ull;                           /* 3.1415926535897931160E+00 */
static const dbl pi_lo = DC(0x3CA1A62633145C06, 0x3CA1A62633145C07);   /* 1.2246467991473531772E-16 */
static const dbl three = 0x4008000000000000ull;

static dbl m_atan2(dbl y, dbl x)
{
    dbl z;
    s32 k, m, hx, hy, ix, iy;
    u32 lx, ly;

    hx = HI(x), lx = LO(x);
    ix = hx & 0x7fffffff;
    hy = HI(y), ly = LO(y);
    iy = hy & 0x7fffffff;
    if (((u32)ix | ((lx | -lx) >> 31)) > 0x7ff00000u || ((u32)iy | ((ly | -ly) >> 31)) > 0x7ff00000u)   /* x or y is NaN */
        return d_add(x, y);
    if (((hx - 0x3ff00000) | (s32)lx) == 0)
        return m_atan(y);   /* x=1.0 */
    m = ((hy >> 31) & 1) | ((hx >> 30) & 2);   /* 2*sign(x)+sign(y) */

    /* when y = 0 */
    if ((iy | (s32)ly) == 0) {
        switch (m) {
        case 0:
        case 1:
            return y;   /* atan(+-0,+anything)=+-0 */
        case 2:
            return d_add(pi, tiny);   /* atan(+0,-anything) = pi */
        case 3:
            return d_sub(d_neg(pi), tiny);   /* atan(-0,-anything) =-pi */
        }
    }
    /* when x = 0 */
    if ((ix | (s32)lx) == 0)
        return (hy < 0) ? d_sub(d_neg(pi_o_2), tiny) : d_add(pi_o_2, tiny);

    /* when x is INF */
    if (ix == 0x7ff00000) {
        if (iy == 0x7ff00000) {
            switch (m) {
            case 0:
                return d_add(pi_o_4, tiny);   /* atan(+INF,+INF) */
            case 1:
                return d_sub(d_neg(pi_o_4), tiny);   /* atan(-INF,+INF) */
            case 2:
                return d_add(d_mul(three, pi_o_4), tiny);   /*atan(+INF,-INF)*/
            default:
                return d_sub(d_mul(d_neg(three), pi_o_4), tiny);   /*atan(-INF,-INF)*/
            }
        } else {
            switch (m) {
            case 0:
                return zero;   /* atan(+...,+INF) */
            case 1:
                return d_neg(zero);   /* atan(-...,+INF) */
            case 2:
                return d_add(pi, tiny);   /* atan(+...,-INF) */
            default:
                return d_sub(d_neg(pi), tiny);   /* atan(-...,-INF) */
            }
        }
    }
    /* when y is INF */
    if (iy == 0x7ff00000)
        return (hy < 0) ? d_sub(d_neg(pi_o_2), tiny) : d_add(pi_o_2, tiny);

    /* compute y/x */
    k = (iy - ix) >> 20;
    if (k > 60)
        z = d_add(pi_o_2, d_mul(half, pi_lo));   /* |y/x| >  2**60 */
    else if (hx < 0 && k < -60)
        z = zero;   /* |y|/x < -2**60 */
    else
        z = m_atan(d_abs(d_div(y, x)));   /* safe to do y/x */
    switch (m) {
    case 0:
        return z;   /* atan(+,+) */
    case 1:
        SET_HI(z, (u32)HI(z) ^ 0x80000000u);
        return z;   /* atan(-,+) */
    case 2:
        return d_sub(pi, d_sub(z, pi_lo));   /* atan(+,-) */
    default:   /* case 3 */
        return d_sub(d_sub(z, pi_lo), pi);   /* atan(-,-) */
    }
}

/* ----------------------------------------------------------------- sqrt */
/* __ieee754_sqrt (e_sqrt.c, newlib 1.8.2): the root bit by bit, its last
 * bit by a floating add (with the library's add, one - tiny is one, so it
 * rounds to nearest). */
static dbl m_sqrt(dbl x)
{
    dbl z;
    s32 sign = (s32)0x80000000u;
    u32 r, t1, s1, ix1, q1;
    s32 ix0, s0, q, m, t, i;

    ix0 = HI(x), ix1 = LO(x);
    /* take care of Inf and NaN */
    if ((ix0 & 0x7ff00000) == 0x7ff00000)
        return d_add(d_mul(x, x), x);   /* sqrt(NaN)=NaN, sqrt(+inf)=+inf, sqrt(-inf)=sNaN */
    /* take care of zero */
    if (ix0 <= 0) {
        if (((ix0 & (~sign)) | (s32)ix1) == 0)
            return x;   /* sqrt(+-0) = +-0 */
        else if (ix0 < 0) {
            CRT_UNSEEN("sqrt", "a negative argument");
            return d_div(d_sub(x, x), d_sub(x, x));   /* sqrt(-ve) = sNaN */
        }
    }
    /* normalize x */
    m = (ix0 >> 20);
    if (m == 0) {   /* subnormal x */
        while (ix0 == 0) {
            m -= 21;
            ix0 |= (s32)(ix1 >> 11);
            ix1 <<= 21;
        }
        for (i = 0; (ix0 & 0x00100000) == 0; i++)
            ix0 <<= 1;
        m -= i - 1;
        ix0 |= (s32)(ix1 >> ((32 - i) & 31));   /* the EE shifts by the low 5 bits */
        ix1 <<= i;
    }
    m -= 1023;   /* unbias exponent */
    ix0 = (ix0 & 0x000fffff) | 0x00100000;
    if (m & 1) {   /* odd m, double x to make it even */
        ix0 += ix0 + (s32)((ix1 & (u32)sign) >> 31);
        ix1 += ix1;
    }
    m >>= 1;   /* m = [m/2] */

    /* generate sqrt(x) bit by bit */
    ix0 += ix0 + (s32)((ix1 & (u32)sign) >> 31);
    ix1 += ix1;
    q = s0 = 0;
    q1 = s1 = 0;   /* [q,q1] = sqrt(x) */
    r = 0x00200000;   /* r = moving bit from right to left */

    while (r != 0) {
        t = s0 + (s32)r;
        if (t <= ix0) {
            s0 = t + (s32)r;
            ix0 -= t;
            q += (s32)r;
        }
        ix0 += ix0 + (s32)((ix1 & (u32)sign) >> 31);
        ix1 += ix1;
        r >>= 1;
    }

    r = (u32)sign;
    while (r != 0) {
        t1 = s1 + r;
        t = s0;
        if ((t < ix0) || ((t == ix0) && (t1 <= ix1))) {
            s1 = t1 + r;
            if (((t1 & (u32)sign) == (u32)sign) && (s1 & (u32)sign) == 0)
                s0 += 1;
            ix0 -= t;
            if (ix1 < t1)
                ix0 -= 1;
            ix1 -= t1;
            q1 += r;
        }
        ix0 += ix0 + (s32)((ix1 & (u32)sign) >> 31);
        ix1 += ix1;
        r >>= 1;
    }

    /* use floating add to find out rounding direction */
    if ((ix0 | (s32)ix1) != 0) {
        z = d_sub(one, tiny);   /* trigger inexact flag */
        if (d_ge(z, one)) {
            z = d_add(one, tiny);
            if (q1 == 0xffffffffu) {
                q1 = 0;
                q += 1;
            } else if (d_gt(z, one)) {
                if (q1 == 0xfffffffeu)
                    q += 1;
                q1 += 2;
            } else {
                q1 += (q1 & 1);
            }
        }
    }
    ix0 = (q >> 1) + 0x3fe00000;
    ix1 = q1 >> 1;
    if ((q & 1) == 1)
        ix1 |= (u32)sign;
    ix0 += (m << 20);
    return WORDS(ix0, ix1);
}

/* -------------------------------------------------------- the functions */
static inline dbl dbits(double x)
{
    dbl u;
    __builtin_memcpy(&u, &x, 8);
    return u;
}
static inline double dval(dbl u)
{
    double x;
    __builtin_memcpy(&x, &u, 8);
    return x;
}

static double CrtImpl_tan(double x)
{
    return dval(m_tan(dbits(x)));
}

double tan(double x)
{
    return CrtImpl_tan(x);
}

static double CrtImpl_atan2(double y, double x)
{
    return dval(m_atan2(dbits(y), dbits(x)));
}

double atan2(double y, double x)
{
    return CrtImpl_atan2(y, x);
}

static double CrtImpl_sqrt(double x)
{
    return dval(m_sqrt(dbits(x)));
}

double sqrt(double x)
{
    return CrtImpl_sqrt(x);
}
