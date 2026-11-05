#ifndef MH2_CORE_HOST_H
#define MH2_CORE_HOST_H

/*
 * The seam between the platform layer's game-facing files (the pal_ files and the api files:
 * compiled with the game's calling convention, regcall) and the host's C
 * library and operating system. Every function here uses the host's own
 * convention (cdecl), whatever the default is, and is implemented in
 * host_linux.c or host_windows.c, which include the system headers. A file that
 * includes game headers includes this and not <stdio.h> and the like: the
 * game's headers declare libc names of their own (include/libc.h), in the
 * game's convention. See pc/README.md, "Calling convention".
 */
#define HOST __attribute__((cdecl))

/* Prints "mh2-core: MSG" to stderr and aborts (a failed rule, a stub). */
HOST void Host_Fatal(const char *msg) __attribute__((noreturn));
/* The same for a stub: "mh2-core: library function NAME was called, and is not written yet". */
HOST void Host_StubFatal(const char *name) __attribute__((noreturn));
/* One line to stderr. */
HOST void Host_Log(const char *msg);
/* Zeroed memory from the host's allocator; not game-visible. NULL if out. */
HOST void *Host_Alloc(unsigned int size);
HOST void Host_Free(void *p);
/* Page protection over whole pages: Host_Protect(p, n, 0) = no access, 1 = read/write. 0 = done. */
HOST int Host_Protect(void *p, unsigned int size, int readWrite);
/* The executable image: its first byte and one past the last (code, data and .bss). */
HOST void Host_ImageRange(unsigned int *lo, unsigned int *hi);
/* An environment variable, or NULL. A value is UTF-8 (on Windows it is read as UTF-16 and converted: the value stays valid). */
HOST const char *Host_Getenv(const char *name);
/* The command line as UTF-8 (main.c, first). On Windows the C library's argv is in the ANSI code page and has already
 * lost every letter outside it, so the program's own UTF-16 command line is read and converted; on Linux nothing changes. */
HOST void Host_Args(int *argc, char ***argv);
/* The page size. */
HOST unsigned int Host_PageSize(void);
/* Zeroed read/write memory at exactly addr (size bytes, whole pages), or NULL
 * when that range is not free. */
HOST void *Host_LowMap(unsigned int addr, unsigned int size);
/* What the host's IPC is told at the end (main.c sets them: Ipc_Fatal and
 * Ipc_Exit, pc/core/ipc/core_ipc.h): Host_Fatal and Host_StubFatal call
 * fatal(msg) before aborting, and Host_ExitHook(code) calls onExit(code). NULL:
 * nothing (the tests). */
typedef void (HOST *HostFatalHook)(const char *msg);
typedef void (HOST *HostExitHook)(int code);
HOST void Host_SetEndHooks(HostFatalHook fatal, HostExitHook onExit);
HOST void Host_ExitHook(int code);

#endif
