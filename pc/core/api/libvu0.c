/*
 * libvu0's sceVpu0Reset (its behaviour at its interface):
 * resets VIF0 (MARK, ERR, FBRST.RST) and VU0 (COP2's FBRST: the
 * original's VU0 takes the write as nothing, vu0.c CTC2), and sends VIF0 its
 * initial settings (STCYCL 4/4, STMASK 0, STMOD 0, ITOP 0).
 */
#include "common.h"
#include "pal.h"

#define VIF0_FBRST 0x10003810u
#define VIF0_ERR 0x10003820u
#define VIF0_MARK 0x10003830u
#define VIF0_FIFO 0x10004000u

static void sceVpu0ResetWork(void)
{
    u64 q[2] __attribute__((aligned(16)));
    Pal_EeRegWrite32(VIF0_MARK, 0);
    Pal_EeRegWrite32(VIF0_ERR, 0);
    Pal_EeRegWrite32(VIF0_FBRST, 1);
    q[0] = 0x2000000001000404ull;
    q[1] = 0x0500000000000000ull;
    Pal_EeRegWrite128(VIF0_FIFO, q);
    q[0] = 0x0000000004000000ull;
    q[1] = 0;
    Pal_EeRegWrite128(VIF0_FIFO, q);
}

void sceVpu0Reset(void)
{
    sceVpu0ResetWork();
}
