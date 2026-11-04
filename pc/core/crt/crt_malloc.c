/*
  A version of malloc/free/realloc written by Doug Lea and released to the
  public domain.  Send questions/comments/complaints/performance data
  to dl@cs.oswego.edu

* VERSION 2.6.4  Thu Nov 28 07:54:55 1996  Doug Lea  (dl at gee)

  (The notice of newlib 1.8.2's libc/stdlib/mallocr.c, which this file's
  allocator code is taken from.)
*/
/* The C library's allocator on PC: re-hosted from the earlier port's libc_malloc.c
 * (Monster Hunter 2 decomp; listed in
 * THIRD_PARTY.md): _malloc_r, the top's extension, _free_r and malloc_trim. The
 * code is newlib's: the algorithm is newlib 1.8.2's
 * mallocr.c (dlmalloc 2.6.4) as the game's library was configured, pinned
 * by observation: MALLOC_ALIGNMENT 16, SIZE_T_SMALLER_THAN_LONG (size_t 32
 * bits, long 64), no mmap, pages of 4096 bytes. The original game made the
 * lock's and sbrk's calls by guest address; here they are the functions
 * of crt_heap.c, which make the same kernel calls in the same order
 * (crt_kernel.h).
 *
 * The bookkeeping is ps2crt's, outside game RAM, as in the original game:
 * av_ (the bins, 258 words) and the variables after it (trim_threshold and
 * top_pad, sbrk_base, the two maxima, current_mallinfo). The chunks are in
 * game RAM, where Capcom's code can see them, and a free chunk's links
 * point into av_ by the PS2's address of the library's own av_ (AV below):
 * RAM holds the values the original did, and every word access goes
 * through rd() and wr(), which map av_'s address range to ps2crt's copy.
 * The core's RAM is at the PS2's addresses, so a chunk's address is a
 * pointer. */
#include "ps2crt.h"
#include "crt_heap.h"

#define AV        0x00197B18u   /* av_[258]: bin_at(i) = AV + 8 i; av_[1] binblocks, av_[2] the top */
#define AV_N      258u

#define SIZE_SZ     4u
#define MINSIZE     16u
#define ALIGN_MASK  15u
#define PAGESZ      4096ull
#define PREV_INUSE  1u
#define SIZE_BITS   3u
#define MAX_SMALLBIN_SIZE 512u
#define MAX_SMALLBIN 63
#define BINBLOCKWIDTH 4

static struct {
    int loaded;
    u32 av[AV_N];
    u64 trim_threshold, top_pad, max_sbrked_mem, max_total_mem;
    u32 sbrk_base;
    u32 mallinfo[10];
} M;

/* The paths taken, for the tests */
u32 crt_ma_cov[32];
#define COV(n) (crt_ma_cov[n]++)

/* The state the library's start-up leaves (the .data of av_ is dlmalloc's
 * static initialiser, bin_at(i) in both links of every bin; trim_threshold
 * 128 KiB; sbrk_base -1): checked against the original's RAM after its
 * start-up. */
static void ma_load(void)
{
    if (M.loaded)
        return;
    M.av[0] = 0, M.av[1] = 0;
    for (u32 i = 2; i < AV_N; i++)
        M.av[i] = AV + 8u * ((i - 2) / 2);
    M.trim_threshold = 128 * 1024, M.top_pad = 0, M.max_sbrked_mem = 0, M.max_total_mem = 0;
    M.sbrk_base = 0xFFFFFFFFu;
    for (u32 i = 0; i < 10; i++)
        M.mallinfo[i] = 0;
    M.loaded = 1;
}

/* For the tests and the host's snapshots: the allocator's state. */
void crt_malloc_state_reset(void) { M.loaded = 0; }
u32 *crt_malloc_av(void) { ma_load(); return M.av; }

/* A word of a chunk: av_'s range is ps2crt's. */
static u32 rd(u32 a)
{
    u32 o = a - AV;
    return o < AV_N * 4u ? M.av[o >> 2] : (*(u32 *)(uintptr_t)a);
}
static void wr(u32 a, u32 v)
{
    u32 o = a - AV;
    if (o < AV_N * 4u)
        M.av[o >> 2] = v;
    else
        (*(u32 *)(uintptr_t)a) = v;
}
#define SBRKED_MEM   M.mallinfo[0]   /* current_mallinfo.arena, an int */
#define bin_at(i)    (AV + 8u * (u32)(i))
#define next_bin(b)  ((b) + 8u)
#define prev_bin(b)  ((b) - 8u)
#define TOP          rd(AV + 8u)
#define SET_TOP(v)   wr(AV + 8u, (v))
#define BINBLOCKS    rd(AV + 4u)
#define SET_BINBLOCKS(v) wr(AV + 4u, (v))
#define LAST_REM     bin_at(1)
#define INITIAL_TOP  AV
#define p_size(p)    rd((p) + 4u)
#define p_prev(p)    rd(p)
#define p_fd(p)      rd((p) + 8u)
#define p_bk(p)      rd((p) + 12u)
#define set_size(p, v) wr((p) + 4u, (v))
#define set_fd(p, v) wr((p) + 8u, (v))
#define set_bk(p, v) wr((p) + 12u, (v))
#define chunksize(p) (p_size(p) & ~SIZE_BITS)
#define set_head_size(p, s) set_size(p, (p_size(p) & PREV_INUSE) | (s))
#define set_foot(p, s) wr((p) + (s), (s))
#define inuse_bit_at_offset(p, s) (p_size((p) + (s)) & PREV_INUSE)
#define set_inuse_bit_at_offset(p, s) set_size((p) + (s), p_size((p) + (s)) | PREV_INUSE)

/* request2size: the sum is size_t's (32 bits), the compare long's (64). */
static u32 request2size(u32 req)
{
    u32 t = req + (SIZE_SZ + ALIGN_MASK);
    return (s64)(u64)t < (s64)(MINSIZE + ALIGN_MASK) ? (MINSIZE + ALIGN_MASK) & ~ALIGN_MASK : t & ~ALIGN_MASK;
}
/* long_sub_size_t with SIZE_T_SMALLER_THAN_LONG */
static s64 lsub(u32 x, u32 y)
{
    return x < y ? -(s64)(u32)(y - x) : (s64)(u32)(x - y);
}
static int bin_index(u32 sz)
{
    u32 h = sz >> 9;
    return h == 0 ? (int)(sz >> 3) : h <= 4 ? 56 + (int)(sz >> 6) : h <= 20 ? 91 + (int)(sz >> 9) :
           h <= 84 ? 110 + (int)(sz >> 12) : h <= 340 ? 119 + (int)(sz >> 15) : h <= 1364 ? 124 + (int)(sz >> 18) : 126;
}
static u64 idx2binblock(int ix) { return 1ull << (ix / BINBLOCKWIDTH); }
static void mark_binblock(int ii) { SET_BINBLOCKS((u32)(BINBLOCKS | idx2binblock(ii))); }

static void unlink_chunk(u32 p)
{
    u32 bk = p_bk(p), fd = p_fd(p);
    set_bk(fd, bk);
    set_fd(bk, fd);
}
static void frontlink(u32 p, u32 s)
{
    u32 bk, fd;
    int idx;
    if (s < MAX_SMALLBIN_SIZE) {
        idx = (int)(s >> 3);
        mark_binblock(idx);
        bk = bin_at(idx);
        fd = p_fd(bk);
        set_bk(p, bk);
        set_fd(p, fd);
        set_fd(bk, p);
        set_bk(fd, p);
    } else {
        idx = bin_index(s);
        bk = bin_at(idx);
        fd = p_fd(bk);
        if (fd == bk)
            mark_binblock(idx);
        else {
            while (fd != bk && s < chunksize(fd))
                fd = p_fd(fd);
            bk = p_bk(fd);
        }
        set_bk(p, bk);
        set_fd(p, fd);
        set_fd(bk, p);
        set_bk(fd, p);
    }
}
static void link_last_remainder(u32 p)
{
    set_fd(LAST_REM, p);
    set_bk(LAST_REM, p);
    set_fd(p, LAST_REM);
    set_bk(p, LAST_REM);
}
static void clear_last_remainder(void)
{
    set_fd(LAST_REM, LAST_REM);
    set_bk(LAST_REM, LAST_REM);
}

/* sbrked_mem's maxima: the int widened to unsigned long (sign-extended) */
static void ma_maxima(void)
{
    u64 s = (u64)(s64)(s32)SBRKED_MEM;
    if (s > M.max_sbrked_mem)
        M.max_sbrked_mem = s;
    if (s > M.max_total_mem)
        M.max_total_mem = s;
}

/* malloc_extend_top(reent, nb). */
void crt_malloc_extend_top(u32 reent, u32 nb)
{
    ma_load();
    u32 old_top = TOP, old_top_size = chunksize(old_top), old_end = old_top + old_top_size;
    u32 sbrk_size = nb + (u32)M.top_pad + MINSIZE;
    if (M.sbrk_base != 0xFFFFFFFFu)
        sbrk_size = (sbrk_size + (u32)(PAGESZ - 1)) & ~(u32)(PAGESZ - 1);
    u32 brk = crt_sbrk_r(reent, sbrk_size);
    if (brk == 0xFFFFFFFFu || (brk < old_end && old_top != INITIAL_TOP)) {
        COV(0);
        return;
    }
    SBRKED_MEM += sbrk_size;
    if (brk == old_end) {
        u32 top_size = sbrk_size + old_top_size;
        set_size(TOP, top_size | PREV_INUSE);
        COV(1);
    } else {
        if (M.sbrk_base == 0xFFFFFFFFu)
            M.sbrk_base = brk;
        else
            SBRKED_MEM += brk - old_end;
        u32 front_misalign = (brk + 2 * SIZE_SZ) & ALIGN_MASK, correction;
        if (front_misalign > 0) {
            correction = (ALIGN_MASK + 1) - front_misalign;
            brk += correction;
        } else
            correction = 0;
        correction += (u32)PAGESZ - ((brk + sbrk_size) & (u32)(PAGESZ - 1));
        u32 new_brk = crt_sbrk_r(reent, correction);
        if (new_brk == 0xFFFFFFFFu) {
            COV(2);
            return;
        }
        SBRKED_MEM += correction;
        SET_TOP(brk);
        u32 top_size = new_brk - brk + correction;
        set_size(brk, top_size | PREV_INUSE);
        COV(3);
        if (old_top != INITIAL_TOP) {
            if (old_top_size < MINSIZE) {
                set_size(TOP, PREV_INUSE);
                COV(4);
                return;
            }
            old_top_size = (old_top_size - 3 * SIZE_SZ) & ~ALIGN_MASK;
            set_head_size(old_top, old_top_size);
            set_size(old_top + old_top_size, SIZE_SZ | PREV_INUSE);
            set_size(old_top + old_top_size + SIZE_SZ, SIZE_SZ | PREV_INUSE);
            if (old_top_size >= MINSIZE)
                COV(5), crt_free_r(reent, old_top + 2 * SIZE_SZ);
            else
                COV(6);
        }
    }
    ma_maxima();
}

/* _malloc_r(reent, bytes). */
u32 crt_malloc_r(u32 reent, u32 bytes)
{
    ma_load();
    u32 victim, victim_size, bin, remainder, q;
    s64 remainder_size;
    int idx, startidx;
    u64 block;
    u32 nb = request2size(bytes);
    crt_heap_lock(reent);
    if (nb < MAX_SMALLBIN_SIZE - 8u) {
        idx = (int)(nb >> 3);
        q = bin_at(idx);
        victim = p_bk(q);
        if (victim != q) {
            victim_size = chunksize(victim);
            unlink_chunk(victim);
            set_inuse_bit_at_offset(victim, victim_size);
            COV(8);
            goto done;
        }
        idx += 2;
    } else {
        idx = bin_index(nb);
        bin = bin_at(idx);
        for (victim = p_bk(bin); victim != bin; victim = p_bk(victim)) {
            victim_size = chunksize(victim);
            remainder_size = lsub(victim_size, nb);
            if (remainder_size >= (s64)MINSIZE) {
                --idx;
                break;
            } else if (remainder_size >= 0) {
                unlink_chunk(victim);
                set_inuse_bit_at_offset(victim, victim_size);
                COV(9);
                goto done;
            }
        }
        ++idx;
    }
    if ((victim = p_fd(LAST_REM)) != LAST_REM) {
        victim_size = chunksize(victim);
        remainder_size = lsub(victim_size, nb);
        if (remainder_size >= (s64)MINSIZE) {
            remainder = victim + nb;
            set_size(victim, nb | PREV_INUSE);
            link_last_remainder(remainder);
            set_size(remainder, (u32)remainder_size | PREV_INUSE);
            set_foot(remainder, (u32)remainder_size);
            COV(10);
            goto done;
        }
        clear_last_remainder();
        if (remainder_size >= 0) {
            set_inuse_bit_at_offset(victim, victim_size);
            COV(11);
            goto done;
        }
        frontlink(victim, victim_size);
        COV(12);
    }
    if ((block = idx2binblock(idx)) <= BINBLOCKS) {
        if ((block & BINBLOCKS) == 0) {
            idx = (idx & ~(BINBLOCKWIDTH - 1)) + BINBLOCKWIDTH;
            block <<= 1;
            while ((block & BINBLOCKS) == 0) {
                idx += BINBLOCKWIDTH;
                block <<= 1;
            }
        }
        for (;;) {
            startidx = idx;
            q = bin = bin_at(idx);
            do {
                for (victim = p_bk(bin); victim != bin; victim = p_bk(victim)) {
                    victim_size = chunksize(victim);
                    remainder_size = lsub(victim_size, nb);
                    if (remainder_size >= (s64)MINSIZE) {
                        remainder = victim + nb;
                        set_size(victim, nb | PREV_INUSE);
                        unlink_chunk(victim);
                        link_last_remainder(remainder);
                        set_size(remainder, (u32)remainder_size | PREV_INUSE);
                        set_foot(remainder, (u32)remainder_size);
                        COV(13);
                        goto done;
                    } else if (remainder_size >= 0) {
                        set_inuse_bit_at_offset(victim, victim_size);
                        unlink_chunk(victim);
                        COV(14);
                        goto done;
                    }
                }
                bin = next_bin(bin);
                if (idx < MAX_SMALLBIN) {   /* MALLOC_ALIGN == 16 */
                    bin = next_bin(bin);
                    ++idx;
                }
            } while ((++idx & (BINBLOCKWIDTH - 1)) != 0);
            do {
                if ((startidx & (BINBLOCKWIDTH - 1)) == 0) {
                    SET_BINBLOCKS((u32)(BINBLOCKS & ~block));
                    break;
                }
                --startidx;
                q = prev_bin(q);
            } while (p_fd(q) == q);
            if ((block <<= 1) <= BINBLOCKS && block != 0) {
                while ((block & BINBLOCKS) == 0) {
                    idx += BINBLOCKWIDTH;
                    block <<= 1;
                }
            } else
                break;
        }
    }
    remainder_size = lsub(chunksize(TOP), nb);
    if (chunksize(TOP) < nb || remainder_size < (s64)MINSIZE) {
        crt_malloc_extend_top(reent, nb);
        COV(16);
        remainder_size = lsub(chunksize(TOP), nb);
        if (chunksize(TOP) < nb || remainder_size < (s64)MINSIZE) {
            COV(17);
            crt_heap_unlock(reent);
            return 0;
        }
    }
    COV(15);
    victim = TOP;
    set_size(victim, nb | PREV_INUSE);
    SET_TOP(victim + nb);
    set_size(victim + nb, (u32)remainder_size | PREV_INUSE);
done:
    crt_heap_unlock(reent);
    return victim + 2 * SIZE_SZ;
}

/* _free_r(reent, mem); the unlock last. */
void crt_free_r(u32 reent, u32 mem)
{
    ma_load();
    if (mem == 0) {
        COV(18);
        return;
    }
    crt_heap_lock(reent);
    u32 p = mem - 2 * SIZE_SZ, hd = p_size(p);
    u32 sz = hd & ~PREV_INUSE, next = p + sz, nextsz = chunksize(next);
    if (next == TOP) {
        sz += nextsz;
        if (!(hd & PREV_INUSE)) {
            u32 prevsz = p_prev(p);
            p -= prevsz;
            sz += prevsz;
            unlink_chunk(p);
            COV(21);
        }
        set_size(p, sz | PREV_INUSE);
        SET_TOP(p);
        COV(19);
        if ((u64)sz >= M.trim_threshold)
            COV(20), crt_malloc_trim(reent, (u32)M.top_pad);
        crt_heap_unlock(reent);
        return;
    }
    set_size(next, nextsz);
    int islr = 0;
    if (!(hd & PREV_INUSE)) {
        u32 prevsz = p_prev(p);
        p -= prevsz;
        sz += prevsz;
        if (p_fd(p) == LAST_REM)
            islr = 1, COV(23);
        else
            unlink_chunk(p);
    }
    if (!inuse_bit_at_offset(next, nextsz)) {
        sz += nextsz;
        if (!islr && p_fd(next) == LAST_REM) {
            islr = 1;
            link_last_remainder(p);
            COV(24);
        } else
            unlink_chunk(next), COV(25);
    }
    set_size(p, sz | PREV_INUSE);
    set_foot(p, sz);
    if (!islr)
        frontlink(p, sz), COV(22);
    crt_heap_unlock(reent);
}

/* malloc_trim(reent, pad). */
u32 crt_malloc_trim(u32 reent, u32 pad)
{
    ma_load();
    crt_heap_lock(reent);
    s64 top_size = (s64)(u64)chunksize(TOP);
    s64 extra = (s64)((((u64)(top_size - (s64)(u64)pad - (s64)MINSIZE) + (PAGESZ - 1)) / PAGESZ - 1) * PAGESZ);
    u32 r = 0;
    COV(26);
    if (extra >= (s64)PAGESZ) {
        u32 top = TOP;
        u32 current_brk = crt_sbrk_r(reent, 0);
        COV(27);
        if (current_brk == top + (u32)top_size) {
            u32 new_brk = crt_sbrk_r(reent, (u32)-extra);
            if (new_brk == 0xFFFFFFFFu) {
                COV(28);
                current_brk = crt_sbrk_r(reent, 0);
                top_size = (s64)(s32)(current_brk - top);
                if (top_size >= (s64)MINSIZE) {
                    SBRKED_MEM = current_brk - M.sbrk_base;
                    COV(29);
                    set_size(top, (u32)top_size | PREV_INUSE);
                }
            } else {
                set_size(top, (u32)(top_size - extra) | PREV_INUSE);
                SBRKED_MEM = (u32)((s64)(s32)SBRKED_MEM - extra);
                r = 1;
                COV(30);
            }
        }
    }
    crt_heap_unlock(reent);
    return r;
}

/* The allocator's state as the library's places in game RAM hold it (what the
 * original's ma_state_export writes): for the tests, and for a comparison of
 * the RAM image with the original's. */
void crt_malloc_state_export(void)
{
    ma_load();
    for (u32 i = 0; i < AV_N; i++)
        *(u32 *)(uintptr_t)(AV + 4 * i) = M.av[i];
    *(u32 *)(uintptr_t)0x00197F20u = (u32)M.trim_threshold, *(u32 *)(uintptr_t)0x00197F24u = (u32)(M.trim_threshold >> 32);
    *(u32 *)(uintptr_t)0x00197F28u = (u32)M.top_pad, *(u32 *)(uintptr_t)0x00197F2Cu = (u32)(M.top_pad >> 32);
    *(u32 *)(uintptr_t)0x00197F30u = M.sbrk_base;
    *(u32 *)(uintptr_t)0x00197F38u = (u32)M.max_sbrked_mem, *(u32 *)(uintptr_t)0x00197F3Cu = (u32)(M.max_sbrked_mem >> 32);
    *(u32 *)(uintptr_t)0x00197F40u = (u32)M.max_total_mem, *(u32 *)(uintptr_t)0x00197F44u = (u32)(M.max_total_mem >> 32);
    for (u32 i = 0; i < 10; i++)
        *(u32 *)(uintptr_t)(0x00197F48u + 4 * i) = M.mallinfo[i];
}
