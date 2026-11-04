/* The C library's heap and allocator: what crt_heap.c and crt_malloc.c
 * share (newlib 1.8.2's malloc, _malloc_r and friends; the original game
 * calls them by their addresses 0x107438, 0x105B38, 0x105E30, 0x1071E0,
 * 0x107F88, 0x108008, 0x10A6E8 and 0x114220). The game's memory is at the
 * PS2's addresses, so a chunk's address is a pointer and the arguments
 * named reent are the PS2 address of the C library's reent. */
#ifndef CRT_HEAP_H
#define CRT_HEAP_H

#include "ps2crt.h"

/* The C library's reent (what _impure_ptr points at): errno is its word 0. */
#define CRT_REENT 0x00197818u

u32 crt_malloc_r(u32 reent, u32 bytes);
void crt_free_r(u32 reent, u32 mem);
u32 crt_malloc_trim(u32 reent, u32 pad);
void crt_malloc_extend_top(u32 reent, u32 nb);

/* The heap's lock (recursive, on the library's semaphore), and sbrk. */
u32 crt_heap_lock(u32 reent);
u32 crt_heap_unlock(u32 reent);
u32 crt_sbrk(u32 incr);
u32 crt_sbrk_r(u32 reent, u32 incr);

/* ps2crt's state outside game RAM, reset to the library's start-up state (the
 * tests; the host's snapshots). */
void crt_heap_state_reset(void);
void crt_malloc_state_reset(void);
void crt_heap_state_export(void);
void crt_malloc_state_export(void);

#endif
