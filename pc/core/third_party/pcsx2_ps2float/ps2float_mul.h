// SPDX-FileCopyrightText: 2002-2024 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The EE FPU's multiplier (mul.s, mula.s, madd.s, msub.s and the other forms
 * whose product goes through the multiplier), as a Booth radix-4 multiplier
 * with a carry-save tree and discarded low columns.
 *
 * This file is a C adaptation of a part of PCSX2's pull request 12001
 * ("[Soft-Float] Initial interpreter implementation of the PS2's floating
 * point unit"), pcsx2/PS2Float.cpp at PCSX2 commit
 * 0392a64e992ac3fa5445dbe484933616cb204674:
 *
 *     PS2Float.cpp   SHA-256 2fa504d2ec7fbad7c3c7a40ea0e106f67e19321d46c2b6c54328327be53f5511
 *
 * Taken from that file: the functions Booth, Add3 and MulMantissa (the
 * multiplier's partial products and tree) and PS2Float::DoMul and the
 * zero/denormal test of PS2Float::Mul (the exponent and the packing of the
 * result). The C++ structures became C structures, the member functions became
 * functions of the raw bit pattern, the 32-bit types became the project's
 * names; the arithmetic is unchanged. The pull request's adder, divider,
 * square roots and flag code are not used here.
 *
 * It is a derived work of PCSX2 and so is distributed under the same licence,
 * the GNU General Public License, version 3 or (at your option) any later
 * version: see COPYING in this directory, and SOURCE.txt. A program that
 * includes this file (the core with the "ps2" float model, and the reference
 * build's model of the same name) is GPL-3.0-or-later as a whole; the rest of
 * the project's files stay CC0 1.0.
 *
 * The operands are IEEE-shaped bit patterns of the EE: the exponent field 255
 * is an ordinary number, a zero exponent field is a zero. The result is
 * what the console's multiplier gives: the exact product's mantissa, minus the
 * difference that the discarded columns make in bit 15 of the 48-bit product,
 * truncated; overflow is the largest value 0x7FFFFFFF, underflow is zero.
 */
#ifndef PS2FLOAT_MUL_H
#define PS2FLOAT_MUL_H

#ifndef PS2FLOAT_U32
#define PS2FLOAT_U32 unsigned int
#define PS2FLOAT_S32 int
#define PS2FLOAT_U64 unsigned long long
#endif

#ifndef PS2FLOAT_INL
#define PS2FLOAT_INL static inline
#endif

/* An attribute for the entry point ps2float_mul alone (the core keeps it out of line). */
#ifndef PS2FLOAT_MUL_ATTR
#define PS2FLOAT_MUL_ATTR
#endif

typedef struct {
    PS2FLOAT_U32 data;
    PS2FLOAT_U32 negate;
} Ps2FloatBoothRecode;

typedef struct {
    PS2FLOAT_U32 lo;
    PS2FLOAT_U32 hi;
} Ps2FloatAddResult;

PS2FLOAT_INL Ps2FloatBoothRecode ps2float_booth(PS2FLOAT_U32 a, PS2FLOAT_U32 b, PS2FLOAT_U32 bit)
{
    PS2FLOAT_U32 test = (bit ? b >> (bit * 2 - 1) : b << 1) & 7;
    a <<= (bit * 2);
    a += (test == 3 || test == 4) ? a : 0;
    PS2FLOAT_U32 neg = (test >= 4 && test <= 6) ? ~0u : 0;
    PS2FLOAT_U32 pos = 1u << (bit * 2);
    a ^= (neg & (0u - pos));
    a &= (test >= 1 && test <= 6) ? ~0u : 0;
    Ps2FloatBoothRecode r = {a, neg & pos};
    return r;
}

PS2FLOAT_INL Ps2FloatAddResult ps2float_add3(PS2FLOAT_U32 a, PS2FLOAT_U32 b, PS2FLOAT_U32 c)
{
    PS2FLOAT_U32 u = a ^ b;
    Ps2FloatAddResult r = {u ^ c, ((u & c) | (a & b)) << 1};
    return r;
}

PS2FLOAT_INL PS2FLOAT_U64 ps2float_mul_mantissa(PS2FLOAT_U32 a, PS2FLOAT_U32 b)
{
    PS2FLOAT_U64 full = (PS2FLOAT_U64)a * (PS2FLOAT_U64)b;
    Ps2FloatBoothRecode b0 = ps2float_booth(a, b, 0);
    Ps2FloatBoothRecode b1 = ps2float_booth(a, b, 1);
    Ps2FloatBoothRecode b2 = ps2float_booth(a, b, 2);
    Ps2FloatBoothRecode b3 = ps2float_booth(a, b, 3);
    Ps2FloatBoothRecode b4 = ps2float_booth(a, b, 4);
    Ps2FloatBoothRecode b5 = ps2float_booth(a, b, 5);
    Ps2FloatBoothRecode b6 = ps2float_booth(a, b, 6);
    Ps2FloatBoothRecode b7 = ps2float_booth(a, b, 7);

    // First cycle
    Ps2FloatAddResult t0 = ps2float_add3(b1.data, b2.data, b3.data);
    Ps2FloatAddResult t1 = ps2float_add3(b4.data & ~0x7ffu, b5.data & ~0xfffu, b6.data);
    // A few adds get skipped, squeeze them back in
    t1.hi |= b6.negate | (b5.data & 0x800);
    b7.data |= (b5.data & 0x400) + b5.negate;

    // Second cycle
    Ps2FloatAddResult t2 = ps2float_add3(b0.data, t0.lo, t0.hi);
    Ps2FloatAddResult t3 = ps2float_add3(b7.data, t1.lo, t1.hi);

    // Third cycle
    Ps2FloatAddResult t4 = ps2float_add3(t2.hi, t3.lo, t3.hi);

    // Fourth cycle
    Ps2FloatAddResult t5 = ps2float_add3(t2.lo, t4.lo, t4.hi);

    // Discard bits and sum
    t5.hi += b7.negate;
    t5.lo &= ~0x7fffu;
    t5.hi &= ~0x7fffu;
    PS2FLOAT_U32 ps2lo = t5.lo + t5.hi;
    return full - ((ps2lo ^ full) & 0x8000);
}

/* PS2Float::Mul and DoMul on raw operands: a zero exponent field (a zero or a
 * denormal) gives a signed zero; otherwise the Booth product. */
PS2FLOAT_INL PS2FLOAT_MUL_ATTR PS2FLOAT_U32 ps2float_mul(PS2FLOAT_U32 raw, PS2FLOAT_U32 other)
{
    PS2FLOAT_U32 sign = (raw ^ other) & 0x80000000u;
    PS2FLOAT_U32 selfExponent = (raw >> 23) & 0xff;
    PS2FLOAT_U32 otherExponent = (other >> 23) & 0xff;
    if (selfExponent == 0 || otherExponent == 0)
        return sign;

    PS2FLOAT_U32 selfMantissa = (raw & 0x7fffff) | 0x800000;
    PS2FLOAT_U32 otherMantissa = (other & 0x7fffff) | 0x800000;

    PS2FLOAT_S32 resExponent = (PS2FLOAT_S32)(selfExponent + otherExponent) - 127;
    PS2FLOAT_U32 resMantissa = (PS2FLOAT_U32)(ps2float_mul_mantissa(selfMantissa, otherMantissa) >> 23);

    if (resMantissa > 0xFFFFFF) {
        resMantissa >>= 1;
        resExponent++;
    }

    if (resExponent > 255)
        return sign | 0x7FFFFFFFu;
    else if (resExponent < 1)
        return sign;

    return sign | ((PS2FLOAT_U32)resExponent << 23) | (resMantissa & 0x7FFFFF);
}

#endif /* PS2FLOAT_MUL_H */
