// SPDX-FileCopyrightText: 2002-2024 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The EE FPU's adder (add.s, sub.s, adda.s, suba.s, and the add step of madd.s, msub.s, madda.s, msuba.s), as a
 * guard-bit adder: the smaller operand is aligned with six extra bits and no sticky bit, so a bit shifted far
 * enough out has no effect.
 *
 * This file is a C adaptation of a part of PCSX2's pull request 12001
 * ("[Soft-Float] Initial interpreter implementation of the PS2's floating
 * point unit"), pcsx2/PS2Float.cpp at PCSX2 commit
 * 0392a64e992ac3fa5445dbe484933616cb204674:
 *
 *     PS2Float.cpp   SHA-256 2fa504d2ec7fbad7c3c7a40ea0e106f67e19321d46c2b6c54328327be53f5511
 *
 * Taken from that file: PS2Float::DoAdd, and the zero/denormal tests, the exponent-difference masking and the
 * call of DoAdd in PS2Float::Add and PS2Float::Sub (with the sign rules of DetermineAdditionOperationSign and
 * DetermineSubtractionOperationSign for zero or denormal operands). The same file's Common::normalizeAmounts,
 * CountLeadingSignBits and BitScanReverse (common/BitUtils.h at that commit, SHA-256
 * 6b669c934089c13d7ceb8df54f12e12651dcfc5495ef0165a52215ce3d44192e) became one table and two compiler
 * builtins. The C++ structures became functions of the raw bit pattern, the 32-bit types became the project's
 * names; the arithmetic is unchanged. The overflow and underflow flags and the MAC exception code
 * (DetermineMacException) of the pull request are not used: the port keeps no FPU flags.
 *
 * It is a derived work of PCSX2 and so is distributed under the same licence,
 * the GNU General Public License, version 3 or (at your option) any later
 * version: see COPYING in this directory, and SOURCE.txt. A program that
 * includes this file (the core with the "ps2" float model, and the reference
 * build's model of the same name) is GPL-3.0-or-later as a whole; the rest of
 * the project's files stay CC0 1.0.
 *
 * Agreement with other evidence: PCSX2 master's own adder (FPU_CORRECT_ADD_SUB in iFPU.cpp), written apart from this
 * pull request, gives the same words in the game's boot table gRandGauss; the console's recorded cases
 * add 0xFFFFFFFF + 1.0 = 0xFFFFFFFF and sub 0x7FFFFFFF - 1.0 = 0x7FFFFFFF (ps2autotests) hold. docs/port-fpu.md.
 *
 * The operands are IEEE-shaped bit patterns of the EE: the exponent field 255
 * is an ordinary number, a zero exponent field is a zero (a denormal reads
 * as a signed zero). Overflow is +/-0x7FFFFFFF (0xFFFFFFFF for a negative one),
 * underflow a signed zero, an exact cancellation +0.
 */
#ifndef PS2FLOAT_ADD_H
#define PS2FLOAT_ADD_H

#ifndef PS2FLOAT_U32
#define PS2FLOAT_U32 unsigned int
#define PS2FLOAT_S32 int
#define PS2FLOAT_U64 unsigned long long
#endif

#ifndef PS2FLOAT_INL
#define PS2FLOAT_INL static inline
#endif

/* An attribute for the entry points ps2float_add and ps2float_sub (the core keeps them out of line). */
#ifndef PS2FLOAT_ADD_ATTR
#define PS2FLOAT_ADD_ATTR
#endif

/* PS2Float::DoAdd: both operands have a non-zero exponent field. */
PS2FLOAT_INL PS2FLOAT_U32 ps2float_do_add(PS2FLOAT_U32 raw, PS2FLOAT_U32 other)
{
    PS2FLOAT_U32 selfExponent = (raw >> 23) & 0xff;
    PS2FLOAT_S32 resExponent = (PS2FLOAT_S32)selfExponent - (PS2FLOAT_S32)((other >> 23) & 0xff);

    if (resExponent < 0) {   // the larger exponent first (the pull request recurses with the operands swapped)
        PS2FLOAT_U32 t = raw;
        raw = other;
        other = t;
        selfExponent = (raw >> 23) & 0xff;
        resExponent = -resExponent;
    }
    if (resExponent >= 25)
        return raw;

    static const PS2FLOAT_S32 normalizeAmounts[32] = {
        0, 0, 0, 0, 0, 0, 0, 0, 0, 8, 8, 8, 8, 8, 8, 8, 8, 16, 16, 16, 16, 16, 16, 16, 16, 24, 24, 24, 24, 24, 24, 24};

    // http://graphics.stanford.edu/~seander/bithacks.html#ConditionalNegate
    PS2FLOAT_U32 sign1 = (PS2FLOAT_U32)((PS2FLOAT_S32)raw >> 31);
    PS2FLOAT_S32 selfMantissa = (PS2FLOAT_S32)((((raw & 0x7fffff) | 0x800000) ^ sign1) - sign1);
    PS2FLOAT_U32 sign2 = (PS2FLOAT_U32)((PS2FLOAT_S32)other >> 31);
    PS2FLOAT_S32 otherMantissa = (PS2FLOAT_S32)((((other & 0x7fffff) | 0x800000) ^ sign2) - sign2);

    // roundingMultiplier = 6 guard bits
    PS2FLOAT_S32 man = (PS2FLOAT_S32)((PS2FLOAT_U32)selfMantissa << 6) +
                       (((PS2FLOAT_S32)((PS2FLOAT_U32)otherMantissa << 6)) >> resExponent);
    PS2FLOAT_S32 absMan = man < 0 ? -man : man;
    if (absMan == 0)
        return 0;

    PS2FLOAT_S32 rawExp = (PS2FLOAT_S32)selfExponent - 6;

    // CountLeadingSignBits of a positive number: its leading zero count (32 for 0, not reached)
    PS2FLOAT_S32 amount = normalizeAmounts[__builtin_clz((PS2FLOAT_U32)absMan)];
    rawExp -= amount;
    absMan = (PS2FLOAT_S32)((PS2FLOAT_U32)absMan << amount);

    // BitScanReverse(absMan >> 23): the index of the top bit (the normalisation above keeps it at 23 or more)
    PS2FLOAT_S32 msbIndex = 31 - __builtin_clz((PS2FLOAT_U32)absMan >> 23);
    rawExp += msbIndex;
    absMan >>= msbIndex;

    if (rawExp > 255)
        return man < 0 ? 0xFFFFFFFFu : 0x7FFFFFFFu;
    else if (rawExp < 1)
        return man < 0 ? 0x80000000u : 0;

    return ((PS2FLOAT_U32)man & 0x80000000u) | ((PS2FLOAT_U32)rawExp << 23) | ((PS2FLOAT_U32)absMan & 0x7FFFFF);
}

/* PS2Float::Add on raw operands. A zero or denormal operand gives the other (a zero when both are, negative only
 * when both are); otherwise the smaller operand's bits below the exponent difference are cleared, and the
 * guard-bit sum is taken. */
PS2FLOAT_INL PS2FLOAT_ADD_ATTR PS2FLOAT_U32 ps2float_add(PS2FLOAT_U32 a, PS2FLOAT_U32 b)
{
    PS2FLOAT_U32 ea = (a >> 23) & 0xff, eb = (b >> 23) & 0xff;
    if (ea == 0 || eb == 0) {
        if (ea != 0)
            return a;
        if (eb != 0)
            return b;
        return (a & b) & 0x80000000u;   /* both zero or denormal: -0 only if both are negative */
    }

    PS2FLOAT_S32 d = (PS2FLOAT_S32)ea - (PS2FLOAT_S32)eb;
    if (d > 0 && d < 25) {
        d -= 1;
        b = (0xFFFFFFFFu << d) & b;
    } else if (d < 0 && d > -25) {
        d = -d;
        d -= 1;
        a = a & (0xFFFFFFFFu << d);
    }
    return ps2float_do_add(a, b);
}

/* PS2Float::Sub: the sum with the subtrahend's sign flipped (the pull request's sign rules for zero and denormal
 * operands are those of the sum with the flipped sign). */
PS2FLOAT_INL PS2FLOAT_ADD_ATTR PS2FLOAT_U32 ps2float_sub(PS2FLOAT_U32 a, PS2FLOAT_U32 b)
{
    return ps2float_add(a, b ^ 0x80000000u);
}

#endif /* PS2FLOAT_ADD_H */
