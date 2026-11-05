#ifndef MH2_KERNEL_STATE_H
#define MH2_KERNEL_STATE_H

/*
 * Save states: the core's side (state.c).
 *
 * A state is the core's writable memory at a VBlank's start: the image's .data and .bss (the EE's, the IOP's
 * and the SPU2's memory and every module's statics) and the low pool (the fibers' stacks, the arena), all at
 * fixed addresses, plus a few sections that owners contribute. The state is good for the exact build and
 * settings that made it: the header says which, and another build or setting is refused.
 *
 * What stays out of a state is what belongs to the process: the main stack, the C heap, the C library, and the
 * host's handles (files, devices, the connection to the host). The files of the host seam (host_*.c) keep their
 * data in the sections "hostbss" and "hostdat" (hostsec.h), which a state skips; a word of another file that
 * holds such a handle is declared with Kern_StateHostWord, or lives in KERN_HOSTBSS. Memory that a state's
 * pointers reach comes from the arena (Pal_ArenaAlloc), which is part of the state.
 *
 * An owner whose state is not all in that memory (a card image, the renderer's memory) registers a section.
 * The order of a load: every owner's Prepare, the memory, every owner's Load, then (in the restored context)
 * every owner's Resume.
 */
#include "common.h"

/* The section's writer: Put appends bytes. */
typedef struct KStateOut KStateOut;
void Kern_StatePut(KStateOut *out, const void *data, u32 size);

typedef struct KStateOwner {
    const char *tag;                                    /* up to 7 characters, unique */
    void (*save)(KStateOut *out);                       /* the section's bytes (may put nothing) */
    void (*prepare)(void);                              /* a load begins: release what the state replaces */
    int (*load)(const void *data, u32 size);            /* the section's bytes, after the memory: 0 when good */
    void (*resume)(void);                               /* the load is done, in the restored context */
    u32 (*setting)(void);                               /* a value the state is good for only when equal (or NULL) */
    const char *(*accept)(const void *data, u32 size);  /* before a load changes anything: why this section's bytes cannot be
                                                         * taken (NULL: they can; or no such hook) */
    const char *(*busy)(void);                          /* asked at a VBlank's start: why the owner cannot be saved or loaded now
                                                         * (a memory card file is open), or NULL. A request of the command line or
                                                         * a script waits for a VBlank where no owner is busy; the host's is refused
                                                         * with the reason. Optional. */
} KStateOwner;

/* The owner must stay valid for the run. At most 16. */
void Kern_StateRegister(const KStateOwner *owner);

/* A host-owned word (or object) in the image's data: a load leaves it as it is. */
void Kern_StateHostWord(const void *p, u32 size);

/* The host's part of a state (the renderer's memory and registers), filled by the host's side. get returns a
 * block that stays valid until the next call (NULL: none); identity is the part's build, 32 bytes, compared at
 * a load. Both NULL by default. */
typedef struct KStateHost {
    const void *(*get)(u32 *size);
    int (*set)(const void *data, u32 size);             /* 0 when good */
    const char *(*identity)(void);
    const char *(*error)(void);                         /* when get returned NULL: why (NULL: there is no part to keep, the state goes on
                                                         * without it); the save then fails with that reason. Optional. */
} KStateHost;
void Kern_StateSetHost(const KStateHost *host);

/* Asked at a VBlank's start; nonzero while a state cannot be made (a movie is open). Weak: the host side's. */
int Kern_StateBlocked(void);

/* A request, taken at the next VBlank's start (R6): a save writes the file and goes on; a load replaces the
 * game and goes on from the state's VBlank. Returns 0 when taken, else a message. */
const char *Kern_StateRequestSave(const char *path);
const char *Kern_StateRequestLoad(const char *path);
/* Nonzero while a request waits for its VBlank (a save or load held back by a busy owner). */
int Kern_StateRequestPending(void);

/* The result of the last request: NULL when it went through, else why not. */
const char *Kern_StateResult(void);

/* Whether a state embeds the memory cards (the owner's answer is pending): one switch. */
extern int g_kern_state_card;
int Kern_StateCardEmbedded(void);   /* now: the switch, or the development build's choice */

#endif
