/****************************************************************
 *
 * The author of this software is David M. Gay.
 *
 * Copyright (c) 1991 by AT&T.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose without fee is hereby granted, provided that this entire notice
 * is included in all copies of any software which is or includes a copy
 * or modification of this software and in all copies of the supporting
 * documentation for such software.
 *
 * THIS SOFTWARE IS BEING PROVIDED "AS IS", WITHOUT ANY EXPRESS OR IMPLIED
 * WARRANTY.  IN PARTICULAR, NEITHER THE AUTHOR NOR AT&T MAKES ANY
 * REPRESENTATION OR WARRANTY OF ANY KIND CONCERNING THE MERCHANTABILITY
 * OF THIS SOFTWARE OR ITS FITNESS FOR ANY PARTICULAR PURPOSE.
 *
 ***************************************************************/

/* Taken from newlib 1.8.2's libc/stdlib/dtoa.c (_dtoa_r, the start of its
 * "Try to get by with floating-point arithmetic" path) and adapted for the
 * port (Monster Hunter 2 decomp; listed in
 * THIRD_PARTY.md); re-hosted for PC from the earlier port's dtoa_quick.c.
 *
 * The port's own formatted output (crt_printf.c) computes exact digits.
 * The C library's dtoa gives the same digits but one way leaves trailing
 * zeros in them, which %g prints: an integer value under 10^15 rounded to
 * fewer digits than it has (mode 2) goes through the "small integer" path
 * when this floating-point path gives up, and that path keeps the zeros
 * when it rounds down. This function says whether the floating-point path
 * gives up, for those values only: mode 2, 1 <= v < 10^15 an integer, so
 * k = floor(log10(v)) is between 0 and 14 (ieps is 2, ds is 10^k exactly
 * and k needs no check), 1 <= ilim <= 14 (Quick_max).
 *
 * The arithmetic is the EE's: the C library's soft doubles (ps2dp.h:
 * rounded toward zero), not the host's. */
#include "ps2crt.h"
#include "ps2dp.h"

int crt_dtoa_quick_fails(u64 bits, int k, int ilim)
{
    if (k < 0 || k > 14 || ilim < 1 || ilim > 14)
        return 0;
    const u64 one = 0x3FF0000000000000ull, two = 0x4000000000000000ull, ten = 0x4024000000000000ull,
              seven = 0x401C000000000000ull, half = 0x3FE0000000000000ull;
    u64 tens[15];
    tens[0] = one;
    for (int i = 1; i < 15; i++)
        tens[i] = ps2dp_mul(tens[i - 1], ten); /* exact: 10^14 < 2^53 */
    u64 d = bits & 0x7FFFFFFFFFFFFFFFull;
    d = ps2dp_div(d, tens[k]);
    u64 eps = ps2dp_mul(two, d); /* ieps * d + 7 */
    eps = ps2dp_add(eps, seven);
    eps -= (u64)(52 * 0x100000u) << 32; /* word0(eps) -= (P - 1) * Exp_msk1 */
    eps = ps2dp_mul(eps, tens[ilim - 1]);
    int fails = 0;
    /* Generate ilim digits, then fix them up. */
    for (int i = 1;; i++) {
        s32 L = ps2dp_to_si(d);
        d = ps2dp_sub(d, ps2dp_from_si(L));
        if (i == ilim) {
            u64 half_up = ps2dp_add(half, eps);
            u64 half_dn = ps2dp_sub(half, eps);
            if (ps2dp_lt(half_up, d))
                fails = 0; /* bump_up */
            else if (ps2dp_lt(d, half_dn))
                fails = 0; /* the digits, trailing zeros dropped */
            else
                fails = 1; /* fast_failed */
            break;
        }
        d = ps2dp_mul(d, ten);
    }
    return fails;
}
