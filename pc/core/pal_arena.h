#ifndef MH2_CORE_PAL_ARENA_H
#define MH2_CORE_PAL_ARENA_H

/*
 * The arena: memory for buffers that the core's state points to and that the platform layer allocates, at a
 * fixed address inside the low pool, so that a save state (kernel/state.h) holds them and their pointers stay
 * good. 8 bytes of header a block, first fit, free blocks join. Not for the host's handles.
 * Pal_ArenaAlloc returns 0 when the arena is full; sizes are in bytes.
 */
void *Pal_ArenaAlloc(unsigned int size);
void Pal_ArenaFree(void *p);

/* The low pool's two regions in use, for a save state: the bump region (the fibers' stacks) and the arena.
 * Returns the number of regions written (0 before the pool is mapped); lo and hi hold two entries. */
int Pal_LowRegions(unsigned int lo[2], unsigned int hi[2]);

#endif
