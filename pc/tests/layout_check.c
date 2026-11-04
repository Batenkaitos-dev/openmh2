/* The probe structs of layout_probe.h, as the compiler of this build (with the flags the game is built
 * with) lays them out, against what MWCC lays out (the numbers tools/pc/layout.py reads from its debug
 * information: bit positions counted from bit 0 of the struct's first byte, little-endian). Run by ctest
 * (layout_probe). It stops a flag that changes the bit-field rules, on the target that has such a flag. */
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "layout_probe.h"

static int failures;

static void check_bits(const char *what, const void *p, size_t n, int lo, int width)
{
    const unsigned char *b = (const unsigned char *)p;
    for (size_t i = 0; i < n * 8; i++) {
        int set = (b[i / 8] >> (i % 8)) & 1;
        int want = (int)i >= lo && (int)i < lo + width;
        if (set != want) {
            printf("layout_check: %s: bit %zu is %d, MWCC's layout has bits %d..%d\n", what, i, set, lo, lo + width - 1);
            failures++;
            return;
        }
    }
}

#define SIZE(T, N) do { if (sizeof(T) != (N)) { printf("layout_check: sizeof(" #T ") = %zu, MWCC's is " #N "\n", sizeof(T)); failures++; } } while (0)
#define OFF(T, F, N) do { if (offsetof(T, F) != (N)) { printf("layout_check: offsetof(" #T ", " #F ") = %zu, MWCC's is " #N "\n", offsetof(T, F)); failures++; } } while (0)
#define BITS(T, F, LO, W) do { T t_; volatile int ones = -1; memset(&t_, 0, sizeof t_); t_.F = ones; check_bits(#T "." #F, &t_, sizeof t_, LO, W); } while (0)

int main(void)
{
    SIZE(ProbeBitsMixed, 4);
    BITS(ProbeBitsMixed, a, 0, 4);
    BITS(ProbeBitsMixed, b, 4, 4);
    BITS(ProbeBitsMixed, c, 8, 8);

    SIZE(ProbeBitsSpill, 12);
    BITS(ProbeBitsSpill, a, 0, 3);
    BITS(ProbeBitsSpill, b, 32, 30);
    BITS(ProbeBitsSpill, c, 64, 3);
    OFF(ProbeBitsSpill, d, 9);

    SIZE(ProbeBitsInterleaved, 8);
    BITS(ProbeBitsInterleaved, a, 0, 5);
    OFF(ProbeBitsInterleaved, b, 2);
    BITS(ProbeBitsInterleaved, c, 32, 1);
    BITS(ProbeBitsInterleaved, d, 33, 20);

    SIZE(ProbeEightByte, 40);
    OFF(ProbeEightByte, d, 8);
    OFF(ProbeEightByte, l, 16);
    OFF(ProbeEightByte, b, 24);
    OFF(ProbeEightByte, u, 32);

    SIZE(ProbeUnionBits, 16);
    OFF(ProbeUnionBits, u, 4);
    BITS(ProbeUnionBits, b, 96, 2);
    BITS(ProbeUnionBits, c, 98, 6);

    if (failures) {
        printf("layout_check: %d differences from MWCC's layout\n", failures);
        return 1;
    }
    printf("layout_check: ok\n");
    return 0;
}
