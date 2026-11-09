/*
 * The product trace (the trace build, MH2_FPTRACE). The ps2 model's multiplier
 * is not commutative (and the adder's guard bits may not be either), so the order of the operands is part of the
 * result, and MWCC's order is not always the C source's. Each float add and multiply of the game's C carries a site
 * id (tools/pc/ps2fp/ps2fp.py); this logs, for the operations whose result changes when the operands are exchanged,
 * one record of five 32-bit words: VBlank, site, a, b, result. The same record is written by the other build
 * (with the guest function in the place of the site), so the two logs compare site by site.
 *
 *   MH2_FPTRACE=PATH            the log (the trace is off without it)
 *   MH2_FPTRACE_FRAMES=LO:HI    only the VBlanks LO to HI (default: all)
 *
 * A record's high bit of the site word is the operation: add 0, multiply 1 (the site id keeps 31 bits).
 */
#define MH2_FPTRACE_IMPL 1
#include "pal_core.h"


#include "crt/ps2fp.h"
#include "host.h"
#include "kernel/kern.h"

/* host_fptrace.c (the system's stdio) */
__attribute__((cdecl)) int Host_FpTraceOpen(const char *path);
__attribute__((cdecl)) void Host_FpTraceWrite(const void *rec, unsigned size);

static int sOpen;
static int sInit;
static unsigned long long sLo, sHi = ~0ULL;

static unsigned long long Number(const char **s)
{
    unsigned long long n = 0;
    while (**s >= '0' && **s <= '9')
        n = n * 10 + (unsigned long long)(*(*s)++ - '0');
    return n;
}

static void Init(void)
{
    const char *p = Host_Getenv("MH2_FPTRACE"), *f = Host_Getenv("MH2_FPTRACE_FRAMES");
    sInit = 1;
    if (f) {
        sLo = Number(&f);
        if (*f == ':') {
            f++;
            sHi = Number(&f);
        }
    }
    if (p)
        sOpen = Host_FpTraceOpen(p);
}

void mh2_fptrace(int op, ps2fp_u32 a, ps2fp_u32 b, ps2fp_u32 r, unsigned site)
{
    ps2fp_u32 r2;
    unsigned long long v;
    if (!sInit)
        Init();
    if (!sOpen || !ps2fp_model)
        return;
    v = Kern_VblankCount();
    if (v < sLo || v > sHi)
        return;
    r2 = op == 2 ? ps2fp_mul(b, a) : ps2fp_add(b, a);
    if (r2 == r)
        return;
    {
        ps2fp_u32 rec[5] = {(ps2fp_u32)v, (site & 0x7FFFFFFFu) | (op == 2 ? 0x80000000u : 0), a, b, r};
        Host_FpTraceWrite(rec, sizeof rec);
    }
}
