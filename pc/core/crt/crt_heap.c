/* The C library's heap underneath malloc on PC: the heap's lock and unlock,
 * sbrk and _sbrk_r, malloc and free.
 *
 * Re-hosted from the earlier port's libc_heap.c, which was written from observed
 * behaviour only (the state poked and the syscalls' answers chosen, and the
 * calls with their return addresses). The original game made the kernel calls by
 * guest address; here they are crt_kernel.h's, in the same order.
 *
 * The state is ps2crt's (the owner's decision 1): the lock's owner and
 * count and sbrk's break. The original's places for them in game RAM
 * (0x197F70, 0x197F74, 0x198054) held, after the library's start-up, owner
 * -1, count 0 and the heap's start.
 */
#include "crt_heap.h"
#include "crt_kernel.h"

#define ENOMEM 12

static struct {
    int loaded;
    u32 owner, count, brk;
} H;

static void hp_load(void)
{
    if (H.loaded)
        return;
    H.owner = 0xFFFFFFFFu;
    H.count = 0;
    H.brk = crt_k_heap_start();
    H.loaded = 1;
}

void crt_heap_state_reset(void)
{
    H.loaded = 0;
}

/* The heap's lock (func_00107F88(reent)), recursive: the thread's id
 * (GetThreadId); if it holds the lock already (the owner is that id), the
 * count goes up by one; else WaitSema on the lock's semaphore, then the
 * owner is that id and the count goes up by one. Returns the count. */
u32 crt_heap_lock(u32 reent)
{
    (void)reent;
    hp_load();
    u32 id = crt_k_thread_id();
    if (H.owner == id) {
        H.count++;
    } else {
        crt_k_sema_wait(crt_k_lock_sema());
        H.owner = id;
        H.count++;
    }
    return H.count;
}

/* The unlock (func_00108008(reent)): the count down by one; at 0 the owner
 * is -1 and the semaphore is signalled. Returns the count. */
u32 crt_heap_unlock(u32 reent)
{
    (void)reent;
    hp_load();
    H.count--;
    if (H.count)
        return H.count;
    H.owner = 0xFFFFFFFFu;
    crt_k_sema_signal(crt_k_lock_sema());
    return 0;
}

/* sbrk(incr) (func_00114220): the heap's end (EndOfHeap); if the break +
 * incr (32 bits, wrapping) is past it (unsigned), ENOMEM is stored where
 * __errno() points (word 0 of the reent) and the result is -1; else the
 * break, moved by incr. */
u32 crt_sbrk(u32 incr)
{
    hp_load();
    u32 end = crt_k_heap_end();
    if (H.brk + incr > end) {
        *(u32 *)(uintptr_t)CRT_REENT = ENOMEM;
        return 0xFFFFFFFFu;
    }
    u32 r = H.brk;
    H.brk += incr;
    return r;
}

/* _sbrk_r(reent, incr): sbrk's result, nothing else written. */
u32 crt_sbrk_r(u32 reent, u32 incr)
{
    (void)reent;
    return crt_sbrk(incr);
}

/* malloc(n) (the function at 0x107140's behaviour in nethttp's wrapper: the
 * lock, _malloc_r and the unlock). */
void *malloc(u32 n)
{
    crt_heap_lock(CRT_REENT);
    u32 p = crt_malloc_r(CRT_REENT, n);
    crt_heap_unlock(CRT_REENT);
    return (void *)(uintptr_t)p;
}

/* free(p): the lock, _free_r(reent, p), and the unlock last (_free_r takes the
 * lock itself too, so the count is 2 inside). */
void free(void *p)
{
    crt_heap_lock(CRT_REENT);
    crt_free_r(CRT_REENT, (u32)(uintptr_t)p);
    crt_heap_unlock(CRT_REENT);
}

/* The heap's state in the library's places in game RAM (the original's
 * hp_state_export): for the tests and for a comparison of the RAM image. */
void crt_heap_state_export(void)
{
    hp_load();
    *(u32 *)(uintptr_t)0x00197F70u = H.owner;
    *(u32 *)(uintptr_t)0x00197F74u = H.count;
    *(u32 *)(uintptr_t)0x00198054u = H.brk;
}
