/* The host seam on Linux (and other POSIX systems): see host.h. */
#define _FILE_OFFSET_BITS 64   /* HostFile_Open is the files' fopen: on i686 the large-file one, as the callers had it (files over 2 GB) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "host.h"
#include "host_file.h"

extern char __executable_start;
extern char _end;

static HostFatalHook sFatalHook;
static HostExitHook sExitHook;

HOST void Host_SetEndHooks(HostFatalHook fatal, HostExitHook onExit)
{
    sFatalHook = fatal;
    sExitHook = onExit;
}

HOST void Host_ExitHook(int code)
{
    HostExitHook h = sExitHook;
    sExitHook = 0;
    if (h)
        h(code);
}

/* Once: a fatal error inside the hook aborts plainly. */
static void FatalHook(const char *msg)
{
    HostFatalHook h = sFatalHook;
    sFatalHook = 0;
    if (h)
        h(msg);
}

HOST void Host_Fatal(const char *msg)
{
    fprintf(stderr, "mh2-core: %s\n", msg);
    fflush(stderr);
    FatalHook(msg);
    abort();
}

HOST void Host_StubFatal(const char *name)
{
    fprintf(stderr, "mh2-core: library function %s was called, and is not written yet\n", name);
    fflush(stderr);
    FatalHook("a library function was called that is not written yet");
    abort();
}

HOST void Host_Log(const char *msg)
{
    fprintf(stderr, "mh2-core: %s\n", msg);
}

HOST void *Host_Alloc(unsigned int size)
{
    return calloc(1, size);
}

HOST void Host_Free(void *p)
{
    free(p);
}

HOST int Host_Protect(void *p, unsigned int size, int readWrite)
{
    return mprotect(p, size, readWrite ? PROT_READ | PROT_WRITE : PROT_NONE);
}

HOST void Host_ImageRange(unsigned int *lo, unsigned int *hi)
{
    *lo = (unsigned int)(size_t)&__executable_start;
    *hi = (unsigned int)(size_t)&_end;
}

HOST const char *Host_Getenv(const char *name)
{
    return getenv(name);
}

HOST void Host_Args(int *argc, char ***argv)
{
    (void)argc;
    (void)argv;   /* the paths are UTF-8 already */
}

HOST FILE *HostFile_Open(const char *path, const char *mode)
{
    return fopen(path, mode);
}

HOST int HostFile_Remove(const char *path)
{
    return remove(path);
}

HOST int HostFile_Rename(const char *from, const char *to)
{
    return rename(from, to);
}

HOST int HostFile_Replace(const char *from, const char *to)
{
    return rename(from, to);
}

HOST unsigned int Host_PageSize(void)
{
    return (unsigned int)sysconf(_SC_PAGESIZE);
}

HOST void *Host_LowMap(unsigned int addr, unsigned int size)
{
    void *p = mmap((void *)(size_t)addr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                   -1, 0);
    if (p == MAP_FAILED)
        return NULL;
    if (p != (void *)(size_t)addr) {   /* a kernel without MAP_FIXED_NOREPLACE takes it as a hint */
        munmap(p, size);
        return NULL;
    }
    return p;
}
