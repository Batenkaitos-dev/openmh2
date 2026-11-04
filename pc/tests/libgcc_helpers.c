/* The compiler's runtime helpers the game calls by name (pc/core/api/libgcc.c): each against PS2
 * results, through the game's calling convention (this file is built with the game's flags, regcall,
 * and calls them through the game's prototypes, include/libgcc.h, as Capcom's code does).
 *
 * The expected values are the original's model of the executable's helpers (libgcc.c,
 * checked against the executable's own with 4,000,000 inputs each): results toward zero (0.1 + 0.2 is
 * 0x3FD3333333333333, IEEE's nearest ...334), no denormals, a float's exponent 255 a number, a double
 * beyond the floats 0x7F800000, dptoli saturating with a NaN 0. A signalling NaN's bits pass the
 * interface unchanged (no x87 load quiets them). main is cdecl for the C runtime. */
#include <stdio.h>

#include "common.h"
#include "libgcc.h"

f64 dpadd(f64 a, f64 b);
f64 dpsub(f64 a, f64 b);
f64 litodp(s32 v);
s32 dptoli(f64 d);
u64 __umoddi3(u64 a, u64 b);

static unsigned s_unseen, s_bad, s_n;

/* pc/core/crt/crt_diag.c's, counted here */
void crt_unseen(const char *func, const char *fmt, ...)
{
    (void)func;
    (void)fmt;
    s_unseen++;
}

static u64 Bits(f64 x)
{
    u64 u;
    __builtin_memcpy(&u, &x, 8);
    return u;
}

static f64 Dbl(u64 u)
{
    f64 x;
    __builtin_memcpy(&x, &u, 8);
    return x;
}

static u32 FBits(f32 x)
{
    u32 u;
    __builtin_memcpy(&u, &x, 4);
    return u;
}

static f32 Flt(u32 u)
{
    f32 x;
    __builtin_memcpy(&x, &u, 4);
    return x;
}

static void Check(const char *what, u64 in1, u64 in2, u64 got, u64 want)
{
    s_n++;
    if (got != want) {
        s_bad++;
        printf("libgcc_helpers: %s(0x%016llX, 0x%016llX) = 0x%016llX, the PS2's 0x%016llX\n", what, in1, in2,
               got, want);
    }
}

static const struct { u32 in; u64 out; } kFptodp[] = {
    {0x00000000u, 0x0000000000000000ull}, {0x80000000u, 0x8000000000000000ull},
    {0x3F800000u, 0x3FF0000000000000ull}, {0xBFC00000u, 0xBFF8000000000000ull},
    {0x00400000u, 0x0000000000000000ull}, {0x80400000u, 0x8000000000000000ull},
    {0x7F800000u, 0x47F0000000000000ull}, {0x7FFFFFFFu, 0x47FFFFFFE0000000ull},
    {0xFF800001u, 0xC7F0000020000000ull}, {0x3DCCCCCDu, 0x3FB99999A0000000ull},
    {0x00800000u, 0x3810000000000000ull}, {0x4B7FFFFFu, 0x416FFFFFE0000000ull},
};

static const struct { u64 in; u32 fp; s32 li; } kFromDouble[] = {
    {0x3FF0000000000000ull, 0x3F800000u, 1},           {0x3FB999999999999Aull, 0x3DCCCCCCu, 0},
    {0x7FF0000000000000ull, 0x7F800000u, 2147483647},  {0x7FF8000000000000ull, 0x7F800000u, 0},
    {0x47F0000000000000ull, 0x7F800000u, 2147483647},  {0x4800000000000000ull, 0x7F800000u, 2147483647},
    {0x3800000000000000ull, 0x00000000u, 0},           {0x0000000000000001ull, 0x00000000u, 0},
    {0xC00921FB54442D18ull, 0xC0490FDAu, -3},          {0x3810000000000000ull, 0x00800000u, 0},
    {0x380FFFFFFFFFFFFFull, 0x00000000u, 0},           {0xBFEFFFFFFFFFFFFFull, 0xBF7FFFFFu, 0},
    {0x41E0000000000000ull, 0x4F000000u, 2147483647},  {0xC1E0000000000000ull, 0xCF000000u, -2147483647 - 1},
    {0x41EFFFFFFFE00000ull, 0x4F7FFFFFu, 2147483647},  {0xC1E0000000200000ull, 0xCF000000u, -2147483647 - 1},
    {0x3FFE666666666666ull, 0x3FF33333u, 1},           {0x8000000000000000ull, 0x80000000u, 0},
};

static const struct { s32 in; u64 out; } kLitodp[] = {
    {0, 0x0000000000000000ull},           {1, 0x3FF0000000000000ull},
    {-1, 0xBFF0000000000000ull},          {-2147483647 - 1, 0xC1E0000000000000ull},
    {2147483647, 0x41DFFFFFFFC00000ull},  {123456789, 0x419D6F3454000000ull},
    {-1000, 0xC08F400000000000ull},
};

static const struct { u64 a, b, add, sub; } kAddSub[] = {
    {0x3FF0000000000000ull, 0x4000000000000000ull, 0x4008000000000000ull, 0xBFF0000000000000ull},
    {0x3FF0000000000000ull, 0x3C30000000000000ull, 0x3FF0000000000000ull, 0x3FEFFFFFFFFFFFFFull},
    {0x3FF0000000000000ull, 0xBFF0000000000000ull, 0x0000000000000000ull, 0x4000000000000000ull},
    {0x3FB999999999999Aull, 0x3FC999999999999Aull, 0x3FD3333333333333ull, 0xBFB999999999999Aull},
    {0x7FF0000000000000ull, 0xFFF0000000000000ull, 0x7FF8000000000000ull, 0x7FF0000000000000ull},
    {0x7FEFFFFFFFFFFFFFull, 0x7FEFFFFFFFFFFFFFull, 0x7FF0000000000000ull, 0x0000000000000000ull},
    {0x0000000000000001ull, 0x3FF0000000000000ull, 0x3FF0000000000000ull, 0xBFF0000000000000ull},
    {0x0010000000000000ull, 0x8018000000000000ull, 0x8000000000000000ull, 0x0024000000000000ull},
    {0x7FF0000000000001ull, 0x3FF0000000000000ull, 0x7FF8000000000001ull, 0x7FF8000000000001ull},
    {0xC00921FB54442D18ull, 0x3CA1A62633145C07ull, 0xC00921FB54442D17ull, 0xC00921FB54442D18ull},
    {0x3FF0000000000000ull, 0xBC90000000000000ull, 0x3FEFFFFFFFFFFFFFull, 0x3FF0000000000000ull},
    {0x4340000000000000ull, 0x3FF8000000000000ull, 0x4340000000000000ull, 0x433FFFFFFFFFFFFEull},
};

/* an exact remainder; a zero divisor gives the dividend (the original's model) and is noted */
static const struct { u64 a, b, r; } kUmod[] = {
    {10, 3, 1},
    {5, 7, 5},
    {0xFFFFFFFFFFFFFFFFull, 0x100000001ull, 0},
    {0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFEull, 1},
    {0x123456789ABCDEF0ull, 0xFEDCBA98ull, 0x51451440ull},
    {0x8000000000000000ull, 0x7FFFFFFFFFFFFFFFull, 1},
    {0x123456789ABCDEF0ull, 0x1000000000000ull, 0x56789ABCDEF0ull},
    {0xDEADBEEFull, 0, 0xDEADBEEFull},
};

__attribute__((cdecl)) int main(void)
{
    unsigned i;
    for (i = 0; i < sizeof kFptodp / sizeof *kFptodp; i++)
        Check("fptodp", kFptodp[i].in, 0, Bits(fptodp(Flt(kFptodp[i].in))), kFptodp[i].out);
    for (i = 0; i < sizeof kFromDouble / sizeof *kFromDouble; i++) {
        Check("dptofp", kFromDouble[i].in, 0, FBits(dptofp(Dbl(kFromDouble[i].in))), kFromDouble[i].fp);
        Check("dptoli", kFromDouble[i].in, 0, (u32)dptoli(Dbl(kFromDouble[i].in)), (u32)kFromDouble[i].li);
    }
    for (i = 0; i < sizeof kLitodp / sizeof *kLitodp; i++)
        Check("litodp", (u32)kLitodp[i].in, 0, Bits(litodp(kLitodp[i].in)), kLitodp[i].out);
    for (i = 0; i < sizeof kAddSub / sizeof *kAddSub; i++) {
        Check("dpadd", kAddSub[i].a, kAddSub[i].b, Bits(dpadd(Dbl(kAddSub[i].a), Dbl(kAddSub[i].b))), kAddSub[i].add);
        Check("dpsub", kAddSub[i].a, kAddSub[i].b, Bits(dpsub(Dbl(kAddSub[i].a), Dbl(kAddSub[i].b))), kAddSub[i].sub);
    }
    for (i = 0; i < sizeof kUmod / sizeof *kUmod; i++)
        Check("__umoddi3", kUmod[i].a, kUmod[i].b, __umoddi3(kUmod[i].a, kUmod[i].b), kUmod[i].r);
    if (s_unseen != 1) {
        printf("libgcc_helpers: %u unseen notes, 1 expected (the zero divisor)\n", s_unseen);
        s_bad++;
    }
    if (s_bad) {
        printf("libgcc_helpers: FAILED, %u of %u\n", s_bad, s_n);
        return 1;
    }
    printf("libgcc_helpers: OK, %u cases\n", s_n);
    return 0;
}
