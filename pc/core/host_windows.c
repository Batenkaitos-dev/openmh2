/* The host seam on Windows (llvm-mingw, i686): see host.h. */
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

#include "host.h"

extern IMAGE_DOS_HEADER __ImageBase;

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
    DWORD old;
    return VirtualProtect(p, size, readWrite ? PAGE_READWRITE : PAGE_NOACCESS, &old) ? 0 : -1;
}

HOST void Host_ImageRange(unsigned int *lo, unsigned int *hi)
{
    const BYTE *base = (const BYTE *)&__ImageBase;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + __ImageBase.e_lfanew);
    *lo = (unsigned int)(size_t)base;
    *hi = (unsigned int)(size_t)base + nt->OptionalHeader.SizeOfImage;
}

HOST const char *Host_Getenv(const char *name)
{
    return getenv(name);
}

HOST unsigned int Host_PageSize(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwPageSize;
}

HOST void *Host_LowMap(unsigned int addr, unsigned int size)
{
    return VirtualAlloc((void *)(size_t)addr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}
