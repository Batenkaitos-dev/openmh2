/*
 * The compiler's runtime helpers that the game calls by name:
 * MWCC's software doubles fptodp, dptofp, dpadd, dpsub, litodp and dptoli,
 * and the 64-bit remainder __umoddi3.
 *
 * The EE's FPU has single precision only, so the PS2's doubles are integer code: results toward zero,
 * denormals in and out are zeros, a float's exponent 255 is a number (fptodp), a double beyond the
 * floats is 0x7F800000 (dptofp), a conversion to an int saturates and a NaN gives 0 (dptoli). Each
 * function here is its ps2_* helper of pc/core/crt/ps2dp.h, the same arithmetic the ps2fp pass gives
 * the game's own double operations, tested against a model of the original's (checked
 * against the executable's with 4,000,000 inputs each) by tools/pc/ps2fp/tests/run.sh.
 *
 * A double crosses the interface as its 64 bits: a host double only carries them (no host
 * operation touches it). The prototypes are the game's (include/libgcc.h) for fptodp and dptofp, and
 * GCC's soft-float ones for the others, which no game C calls (the pass inlines their arithmetic);
 * they are defined so that no name of the library inventory stays an aborting stub.
 *
 * The seam table (config/pc/seam_calls.txt) marks all seven untraced: the original game logs no C line for
 * them, so neither does the core.
 */
#include "common.h"
#include "libgcc.h"
#include "crt/ps2dp.h"

void crt_unseen(const char *func, const char *fmt, ...);   /* pc/core/crt/crt_diag.c (ps2crt.h) */

f64 dpadd(f64 a, f64 b);
f64 dpsub(f64 a, f64 b);
f64 litodp(s32 v);
s32 dptoli(f64 d);
u64 __umoddi3(u64 a, u64 b);

static inline u64 Gcc_Bits(f64 x)
{
    u64 u;
    __builtin_memcpy(&u, &x, 8);
    return u;
}

static inline f64 Gcc_Double(u64 u)
{
    f64 x;
    __builtin_memcpy(&x, &u, 8);
    return x;
}

f64 fptodp(f32 x)
{
    return Gcc_Double(ps2_fptodp(x));
}

f32 dptofp(f64 x)
{
    return ps2_dptofp(Gcc_Bits(x));
}

f64 dpadd(f64 a, f64 b)
{
    return Gcc_Double(ps2_dpadd(Gcc_Bits(a), Gcc_Bits(b)));
}

f64 dpsub(f64 a, f64 b)
{
    return Gcc_Double(ps2_dpsub(Gcc_Bits(a), Gcc_Bits(b)));
}

f64 litodp(s32 v)
{
    return Gcc_Double(ps2_litodp(v));
}

s32 dptoli(f64 d)
{
    return ps2_dptoli(Gcc_Bits(d));
}

/* An exact remainder. A zero divisor was never seen in the game (the original's model returns the
 * dividend and notes it): the same here, noted as the C library's unseen inputs are. */
u64 __umoddi3(u64 a, u64 b)
{
    if (!b) {
        crt_unseen("__umoddi3", "a division by zero");
        return a;
    }
    return a % b;
}
