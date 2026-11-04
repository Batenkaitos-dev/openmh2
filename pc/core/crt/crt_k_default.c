/* Default implementations of crt_kernel.h, for a build whose platform layer does
 * not provide the kernel functions yet. All are weak: the kernel defines
 * the real ones, with the same names, and these drop out.
 *
 * What they do is what a program with one thread and no contention sees on the
 * original game: the thread id is a constant, the lock semaphore is id 1 and its
 * waits and signals have no effect (nothing else takes it), the heap is the
 * range the original's map and EndOfHeap give, and the console's write fails
 * as the original's does (its console file never opens). They do not model
 * the time a blocking call takes: that is the kernel's. */
#include "crt_kernel.h"

#define WEAK __attribute__((weak))

WEAK crt_u32 crt_k_thread_id(void) { return 1; }
WEAK crt_u32 crt_k_lock_sema(void) { return 1; }
WEAK void crt_k_sema_wait(crt_u32 sema) { (void)sema; }
WEAK void crt_k_sema_signal(crt_u32 sema) { (void)sema; }
WEAK crt_u32 crt_k_heap_start(void) { return 0x01F48000u; }
WEAK crt_u32 crt_k_heap_end(void) { return 0x01FA8000u; }
WEAK int crt_k_write(int fd, const void *buf, crt_u32 n)
{
    (void)fd;
    (void)buf;
    (void)n;
    return -1;
}
