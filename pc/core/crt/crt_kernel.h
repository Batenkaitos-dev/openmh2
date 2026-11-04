/* ps2crt's side of the platform layer: the internal kernel functions the C
 * library calls.
 *
 * ps2crt never calls the game-facing API (the library functions Capcom's
 * code calls: WaitSema, SignalSema, GetThreadId, EndOfHeap, ...). It calls
 * these, which the platform layer (pc/core/kernel) implements with the
 * same effects, in the same order and at the same points on the frame
 * clock as the PS2's: a call that blocks there blocks here (3.6), and the
 * calls ps2crt makes are those of the original game's.
 *
 * tools/pc/ps2fp/tests/crt_stubs.c is an implementation for the tests.
 *
 * What the original game does, observed from outside
 * (a run of the original game with its log at debug level, and the C library's
 * data in RAM):
 *
 *  - The C library's lock semaphore is created by the C library's start-up
 *    (the system set-up __start calls after SetupThread and SetupHeap), as
 *    the first CreateSema of the whole run, before any game code: max 1,
 *    init 1, attr 0 (SA_THFIFO), option 0x001BEB60 (an address in the
 *    library's data). Its id (1) is stored in the library's word at
 *    0x00198790. A second semaphore, id 2 (max 1, init 1, attr 0, option
 *    0x001BEB70), is created straight after, by the same start-up; it is
 *    not the allocator's (SIF's, probably). The game's own semaphores begin
 *    at id 3. The platform layer must create both at that point of its
 *    start-up, so that the game sees the same ids; crt_k_lock_sema returns
 *    the first.
 *  - The heap: sbrk's break starts at 0x01F48000 (the build's map: the
 *    symbol D_1F48000, where __start's clear of the bss ends and SetupHeap
 *    puts the break) and EndOfHeap answers 0x01FA8000 (the stack's bottom
 *    that SetupThread was given: D_1FA8000), 0x60000 bytes in all.
 *  - The first printf takes stdout's 1024-byte buffer from the heap
 *    (_malloc_r(reent, 0x400): the lock, two sbrk calls of 0x420 and 0xBE0
 *    bytes, the unlock), and every line it ends is a write(1, buf, n).
 */
#ifndef CRT_KERNEL_H
#define CRT_KERNEL_H

typedef unsigned int crt_u32;

/* GetThreadId: the running thread's id. */
crt_u32 crt_k_thread_id(void);

/* The id of the C library's lock semaphore (see above). */
crt_u32 crt_k_lock_sema(void);

/* WaitSema and SignalSema on a semaphore, as the kernel's own functions do
 * (WaitSema blocks until the count is positive). */
void crt_k_sema_wait(crt_u32 sema);
void crt_k_sema_signal(crt_u32 sema);

/* The heap newlib's sbrk works in: the first break, and EndOfHeap's answer
 * (the break may not go past it). */
crt_u32 crt_k_heap_start(void);
crt_u32 crt_k_heap_end(void);

/* write(fd, buf, n) as the C library's glue makes it (the fileio write of
 * the IOP: blocks while it runs): the number of bytes written, or a value
 * <= 0 for a failure. Used for stdout (fd 1). */
int crt_k_write(int fd, const void *buf, crt_u32 n);

#endif
