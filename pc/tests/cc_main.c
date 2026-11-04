/*
 * mh2_cc_test: the calling-convention test.
 * Built twice: "game" (the game's default convention, regcall) must return
 * every pattern unchanged; "control" (the i386 default) is expected to
 * change 0x7F800001 to 0x7FC00001, which shows the test can see the
 * problem. Exit 0 when the build behaves as expected.
 */
#include <stdio.h>

#ifdef CC_CONTROL
unsigned CcControlBits(void);
#else
unsigned CcRun(void) __attribute__((regcall));
#endif

int main(void)
{
#ifdef CC_CONTROL
    unsigned bits = CcControlBits();
    printf("control (default convention): 0x7F800001 comes back as 0x%08X\n", bits);
    return bits == 0x7FC00001u ? 0 : 1;
#else
    unsigned r = CcRun();
    if (r == 0) {
        printf("game convention (regcall): every NaN pattern survives call and return\n");
        return 0;
    }
    printf("FAILED: case %u came back as 0x%06X\n", r >> 24, r & 0xFFFFFF);
    return 1;
#endif
}
