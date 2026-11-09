/* The twelve block copy, clear and fill loops of main (src/main/mem_loops.s, 0x182450-0x182640) as C
 * for mh2-core. Each is the assembly routine's one counted loop, do { ... } while (--n): the loop
 * runs first and tests the count after, so n = 0 runs 2^32 times (the game never passes it). The loads
 * and stores are in the routine's own order (all loads of a pass, then all its stores, and a
 * word loop one word at a time), so an overlapping source and destination give the EE's result.
 * They are volatile accesses, so the compiler can neither merge the loop into a memcpy nor reorder
 * it. lq/sq ignore the low four bits of the address (the quadword below a misaligned pointer is
 * used); ld/sd and lw/sw do the plain access (the EE raises an address error on a misaligned one,
 * which the game never causes).
 *
 * Tested against the original game's routines by the ps2fp tests. The
 * parameters are the game's: (src, dst, n) for the copies, (dst, n) and (dst, n, value) for
 * the clears and fills; Mem_FillQwords takes its 128-bit value as four words (a u128 argument
 * is one 128-bit general register on the EE; no C caller passes one on PC).
 */
#include "ps2crt.h"

#define RD32(p) (*(volatile u32 *)(__SIZE_TYPE__)(p))
#define WR32(p, v) (*(volatile u32 *)(__SIZE_TYPE__)(p) = (v))
#define RD64(p) (*(volatile u64 *)(__SIZE_TYPE__)(p))
#define WR64(p, v) (*(volatile u64 *)(__SIZE_TYPE__)(p) = (v))
#define QW(p) ((__SIZE_TYPE__)(p) & ~(__SIZE_TYPE__)15)

void Mem_CopyWords(const u32 *src, u32 *dst, s32 n)
{
    do {
        u32 w = RD32(src);
        src += 1;
        n--;
        WR32(dst, w);
        dst += 1;
    } while (n != 0);
}

void Mem_Copy16ByWords(const void *src, void *dst, s32 n)
{
    u32 s = (u32)(__SIZE_TYPE__)src, d = (u32)(__SIZE_TYPE__)dst;
    do {
        u32 v0 = RD32(s + 0), v1 = RD32(s + 4), t0 = RD32(s + 8), t1 = RD32(s + 12);
        WR32(d + 0, v0);
        WR32(d + 4, v1);
        WR32(d + 8, t0);
        WR32(d + 12, t1);
        n--;
        s += 16;
        d += 16;
    } while (n != 0);
}

void Mem_Copy16ByDwords(const void *src, void *dst, s32 n)
{
    u32 s = (u32)(__SIZE_TYPE__)src, d = (u32)(__SIZE_TYPE__)dst;
    do {
        u64 v0 = RD64(s + 0), v1 = RD64(s + 8);
        WR64(d + 0, v0);
        WR64(d + 8, v1);
        n--;
        s += 16;
        d += 16;
    } while (n != 0);
}

void Mem_CopyQwords(const void *src, void *dst, s32 n)
{
    u32 s = (u32)(__SIZE_TYPE__)src, d = (u32)(__SIZE_TYPE__)dst;
    do {
        u32 sq = (u32)QW(s), dq = (u32)QW(d);
        u64 lo = RD64(sq), hi = RD64(sq + 8);
        n--;
        s += 16;
        WR64(dq, lo);
        WR64(dq + 8, hi);
        d += 16;
    } while (n != 0);
}

void Mem_Copy32ByWords(const void *src, void *dst, s32 n)
{
    u32 s = (u32)(__SIZE_TYPE__)src, d = (u32)(__SIZE_TYPE__)dst;
    do {
        u32 v0 = RD32(s + 0), v1 = RD32(s + 4), t0 = RD32(s + 8), t1 = RD32(s + 12);
        u32 t2 = RD32(s + 16), t3 = RD32(s + 20), t4 = RD32(s + 24), t5 = RD32(s + 28);
        WR32(d + 0, v0);
        WR32(d + 4, v1);
        WR32(d + 8, t0);
        WR32(d + 12, t1);
        WR32(d + 16, t2);
        WR32(d + 20, t3);
        WR32(d + 24, t4);
        WR32(d + 28, t5);
        n--;
        s += 32;
        d += 32;
    } while (n != 0);
}

void Mem_Copy32ByDwords(const void *src, void *dst, s32 n)
{
    u32 s = (u32)(__SIZE_TYPE__)src, d = (u32)(__SIZE_TYPE__)dst;
    do {
        u64 v0 = RD64(s + 0), v1 = RD64(s + 8), t0 = RD64(s + 16), t1 = RD64(s + 24);
        WR64(d + 0, v0);
        WR64(d + 8, v1);
        WR64(d + 16, t0);
        WR64(d + 24, t1);
        n--;
        s += 32;
        d += 32;
    } while (n != 0);
}

void Mem_Copy32ByQwords(const void *src, void *dst, s32 n)
{
    u32 s = (u32)(__SIZE_TYPE__)src, d = (u32)(__SIZE_TYPE__)dst;
    do {
        u32 a = (u32)QW(s), b = (u32)QW(s + 16), c = (u32)QW(d), e = (u32)QW(d + 16);
        u64 v0l = RD64(a), v0h = RD64(a + 8), v1l = RD64(b), v1h = RD64(b + 8);
        WR64(c, v0l);
        WR64(c + 8, v0h);
        WR64(e, v1l);
        WR64(e + 8, v1h);
        n--;
        s += 32;
        d += 32;
    } while (n != 0);
}

void Mem_Copy64ByQwords(const void *src, void *dst, s32 n)
{
    u32 s = (u32)(__SIZE_TYPE__)src, d = (u32)(__SIZE_TYPE__)dst;
    do {
        u64 q[8];
        for (int i = 0; i < 4; i++) {
            u32 a = (u32)QW(s + 16 * i);
            q[2 * i] = RD64(a);
            q[2 * i + 1] = RD64(a + 8);
        }
        for (int i = 0; i < 4; i++) {
            u32 a = (u32)QW(d + 16 * i);
            WR64(a, q[2 * i]);
            WR64(a + 8, q[2 * i + 1]);
        }
        n--;
        s += 64;
        d += 64;
    } while (n != 0);
}

void Mem_ClearWords(u32 *dst, s32 n)
{
    do {
        n--;
        WR32(dst, 0);
        dst += 1;
    } while (n != 0);
}

void Mem_ClearQwords(void *dst, s32 n)
{
    u32 d = (u32)(__SIZE_TYPE__)dst;
    do {
        u32 dq = (u32)QW(d);
        n--;
        WR64(dq, 0);
        WR64(dq + 8, 0);
        d += 16;
    } while (n != 0);
}

void Mem_FillWords(u32 *dst, s32 n, u32 value)
{
    do {
        n--;
        WR32(dst, value);
        dst += 1;
    } while (n != 0);
}

void Mem_FillQwords(void *dst, s32 n, u32 w0, u32 w1, u32 w2, u32 w3)
{
    u32 d = (u32)(__SIZE_TYPE__)dst;
    u64 lo = ((u64)w1 << 32) | w0, hi = ((u64)w3 << 32) | w2;
    do {
        u32 dq = (u32)QW(d);
        n--;
        WR64(dq, lo);
        WR64(dq + 8, hi);
        d += 16;
    } while (n != 0);
}
