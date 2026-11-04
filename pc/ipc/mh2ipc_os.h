/*
 * The operating system's part of the protocol (mh2ipc.h): the shared
 * block, the waits and wakes, and whether the other process lives.
 * mh2ipc_os_linux.c (memfd, futexes) and mh2ipc_os_windows.c (a named
 * file mapping, named events). Both processes link the same file: the
 * host (64-bit) and mh2-core (32-bit; pc/core/ipc/sources.cmake puts it
 * into the core's host seam, compiled with the host's calling convention).
 *
 * No system headers here.
 */
#ifndef MH2IPC_OS_H
#define MH2IPC_OS_H

#define MH2IPC_NAME_MAX 96

/* A process's view of a block. */
typedef struct Mh2IpcMap {
    void *base;
    unsigned int bytes;
    char name[MH2IPC_NAME_MAX];   /* what MH2_IPC holds */
    void *os[8];                  /* the system's handles */
} Mh2IpcMap;

/* The host: a new zeroed block of `bytes`, whose name a child process can
 * open (on Linux the descriptor stays open across exec). 0 = done. */
int mh2ipc_create(Mh2IpcMap *m, unsigned int bytes);
/* The core: the block named `name`. 0 = done. */
int mh2ipc_open(Mh2IpcMap *m, const char *name);
void mh2ipc_close(Mh2IpcMap *m);

/* Waits until *word != old, a wake, or timeout_ms (may return early). The
 * caller checks its condition again. */
void mh2ipc_wait(Mh2IpcMap *m, int ev, const unsigned int *word, unsigned int old, unsigned int timeout_ms);
/* Wakes the process waiting on `ev` (word: the one it waits on). */
void mh2ipc_wake(Mh2IpcMap *m, int ev, const unsigned int *word);

unsigned int mh2ipc_pid(void);
/* Whether process `pid` still runs: the core asks for its host (on Linux,
 * whether it is still the parent). */
int mh2ipc_alive(Mh2IpcMap *m, unsigned int pid);

/* A monotonic clock in nanoseconds, and a wait until it reaches `due`
 * (it sleeps most of the time and spins the last millisecond). Real-time
 * pacing only: nothing the game can observe reads them. */
unsigned long long mh2ipc_time_ns(void);
void mh2ipc_wait_until_ns(unsigned long long due);

#endif
