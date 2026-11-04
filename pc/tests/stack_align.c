/* Local arrays of 16 bytes or more are 16-aligned in code built with the game's flags
 * (tools/pc/stackalign.py; the AMD64 psABI's rule, which the original game has). Built with the game's flags
 * (regcall), through the launcher as the core's code is; main is cdecl for the C runtime.
 * The probes run at several stack depths: on Windows the stack is only 4-aligned at entry, so a frame
 * that doesn't realign would show a misaligned buffer at some depth. (No VLA here: on Windows its frame
 * would need a base pointer, which bpcheck.py refuses in every object of the build.) */
#include <stdint.h>
#include <stdio.h>

static unsigned s_bad;
static unsigned s_runs;

__attribute__((noinline)) static void sa_note(const char *what, const void *p)
{
    s_runs++;
    if ((uintptr_t)p & 15) {
        s_bad++;
        printf("stack_align: %s at %p (%u mod 16)\n", what, p, (unsigned)((uintptr_t)p & 15));
    }
}

/* the shape of Gs_ClearVram (src/main/gs_setup.c): a 96-byte buffer after a smaller local */
__attribute__((noinline)) static int sa_buf96(int seed)
{
    volatile int small = seed;
    unsigned char buf[96];
    for (int i = 0; i < 96; i++)
        buf[i] = (unsigned char)(i + small);
    sa_note("u8 buf[96]", buf);
    return buf[seed & 63] + small;
}

typedef struct { char c[3]; } Three;

__attribute__((noinline)) static int sa_arrays(int seed)
{
    char c4;
    Three t6[6];        /* 18 bytes: aligned */
    short s8[8];        /* 16 bytes: aligned */
    float m[4][4];
    c4 = (char)seed;
    t6[seed & 3].c[0] = c4;
    s8[seed & 7] = (short)seed;
    m[seed & 3][0] = (float)seed;
    sa_note("Three[6]", t6);
    sa_note("short[8]", s8);
    sa_note("float[4][4]", m);
    return t6[seed & 3].c[0] + s8[seed & 7] + (int)m[seed & 3][0];
}

/* a recursion whose frames change the stack's offset, so the probes run at several of them */
static int sa_depth(int d, int seed);
static int (*volatile s_depth)(int, int) = sa_depth;   /* no tail-recursion elimination */

__attribute__((noinline)) static int sa_depth(int d, int seed)
{
    volatile char pad[1];
    pad[0] = (char)d;
    if (d > 0)
        return s_depth(d - 1, seed + pad[0]) + 1;
    return sa_buf96(seed) + sa_arrays(seed);
}

__attribute__((cdecl)) int main(void)
{
    int sum = 0;
    for (int d = 0; d < 8; d++)
        sum += sa_depth(d, d);
    if (s_bad) {
        printf("stack_align: FAILED, %u of %u buffers misaligned\n", s_bad, s_runs);
        return 1;
    }
    printf("stack_align: OK, %u buffers 16-aligned (%d)\n", s_runs, sum & 1);
    return 0;
}
