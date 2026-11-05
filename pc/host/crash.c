/*
 * Crash dumps. On Windows an unhandled exception in mh2.exe writes SESSION.mh2-crash.dmp into the sessions
 * folder (dbghelp's dump function, Windows' own, loaded when needed), and mh2 gives its core
 * MH2_CRASH_DUMP=SESSION.core-crash.dmp in the environment, which the core writes the same way on a fatal
 * exception. A dump holds threads, stacks, modules and the exception; not the game's memory. The files are
 * named after the session; they are never committed. On Linux the system's own core handling applies
 * (no dump is written).
 */
#include "mh2host.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>

typedef BOOL(WINAPI *MiniDumpWriteDumpFn)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

static char host_dump[1500], core_dump[1500];
static wchar_t host_dump_w[1500];   /* made in crash_setup: the paths are UTF-8 (hostfs.h), the handler allocates nothing */

static LONG WINAPI on_crash(EXCEPTION_POINTERS *ep)
{
    static volatile LONG once;
    if (InterlockedExchange(&once, 1))
        return EXCEPTION_CONTINUE_SEARCH;
    HMODULE lib = LoadLibraryA("dbghelp.dll");
    MiniDumpWriteDumpFn write = lib ? (MiniDumpWriteDumpFn)(void *)GetProcAddress(lib, "MiniDumpWriteDump") : NULL;
    HANDLE file = CreateFileW(host_dump_w, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (write && file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei;
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = ep;
        mei.ClientPointers = FALSE;
        write(GetCurrentProcess(), GetCurrentProcessId(), file, MiniDumpNormal, &mei, NULL, NULL);
    }
    if (file != INVALID_HANDLE_VALUE)
        CloseHandle(file);
    return EXCEPTION_CONTINUE_SEARCH;
}

void crash_setup(const char *dir, const char *session)
{
    if (!dir || !dir[0] || !session || !session[0])
        return;
    snprintf(host_dump, sizeof(host_dump), "%s\\%s.mh2-crash.dmp", dir, session);
    snprintf(core_dump, sizeof(core_dump), "%s\\%s.core-crash.dmp", dir, session);
    wchar_t *wh = hfs_wide(host_dump), *wc = hfs_wide(core_dump);
    if (wh && wc && wcslen(wh) < sizeof(host_dump_w) / sizeof(host_dump_w[0])) {
        wcscpy(host_dump_w, wh);
        SetEnvironmentVariableW(L"MH2_CRASH_DUMP", wc);   /* the core inherits it */
    }
    free(wh);
    free(wc);
    SetUnhandledExceptionFilter(on_crash);
    host_log("crash dumps: %s and %s (written only on a crash)", host_dump, core_dump);
}
#else
void crash_setup(const char *dir, const char *session)
{
    (void)dir;
    (void)session;
}
#endif
