/*
 * mh2ipc_os.h on Windows: the block is a named file mapping backed by the
 * paging file (MH2_IPC holds its name, Local\mh2ipc-PID-N), and each event
 * of mh2ipc.h is a named auto-reset event, the mapping's name plus "-eN".
 * A wake with nobody waiting leaves its event set, so the next wait
 * returns at once and its caller checks again: no wake is lost.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "ipc/mh2ipc.h"
#include "ipc/mh2ipc_os.h"

/* os[0] the mapping, os[1..4] the events, os[5] the watched process, os[6] its pid */
static int open_events(Mh2IpcMap *m)
{
    for (int i = 0; i < MH2IPC_EV_COUNT; i++) {
        char n[MH2IPC_NAME_MAX + 8];
        snprintf(n, sizeof(n), "%s-e%d", m->name, i);
        m->os[1 + i] = CreateEventA(NULL, FALSE, FALSE, n);
        if (!m->os[1 + i])
            return -1;
    }
    return 0;
}

int mh2ipc_create(Mh2IpcMap *m, unsigned int bytes)
{
    static LONG serial;
    memset(m, 0, sizeof(*m));
    snprintf(m->name, sizeof(m->name), "Local\\mh2ipc-%lu-%ld", (unsigned long)GetCurrentProcessId(),
             InterlockedIncrement(&serial));
    HANDLE h = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, bytes, m->name);
    if (!h || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (h)
            CloseHandle(h);
        return -1;
    }
    m->os[0] = h;
    m->base = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, bytes);
    m->bytes = bytes;
    if (!m->base || open_events(m) != 0) {
        mh2ipc_close(m);
        return -1;
    }
    return 0;
}

int mh2ipc_open(Mh2IpcMap *m, const char *name)
{
    memset(m, 0, sizeof(*m));
    snprintf(m->name, sizeof(m->name), "%s", name);
    HANDLE h = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!h)
        return -1;
    m->os[0] = h;
    m->base = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    MEMORY_BASIC_INFORMATION mbi;
    if (!m->base || !VirtualQuery(m->base, &mbi, sizeof(mbi)) || open_events(m) != 0) {
        mh2ipc_close(m);
        return -1;
    }
    m->bytes = (unsigned int)mbi.RegionSize;
    return 0;
}

void mh2ipc_close(Mh2IpcMap *m)
{
    if (m->base)
        UnmapViewOfFile(m->base);
    m->base = NULL;
    for (int i = 0; i < 6; i++) {
        if (m->os[i])
            CloseHandle(m->os[i]);
        m->os[i] = NULL;
    }
}

void mh2ipc_wait(Mh2IpcMap *m, int ev, const unsigned int *word, unsigned int old, unsigned int timeout_ms)
{
    if (__atomic_load_n(word, __ATOMIC_ACQUIRE) != old)
        return;
    WaitForSingleObject(m->os[1 + ev], timeout_ms);
}

void mh2ipc_wake(Mh2IpcMap *m, int ev, const unsigned int *word)
{
    (void)word;
    SetEvent(m->os[1 + ev]);
}

unsigned int mh2ipc_pid(void)
{
    return (unsigned int)GetCurrentProcessId();
}

int mh2ipc_alive(Mh2IpcMap *m, unsigned int pid)
{
    if (pid == (unsigned int)GetCurrentProcessId())
        return 1;
    if (!m->os[5] || (unsigned int)(UINT_PTR)m->os[6] != pid) {
        if (m->os[5])
            CloseHandle(m->os[5]);
        m->os[5] = OpenProcess(SYNCHRONIZE, FALSE, pid);
        m->os[6] = (void *)(UINT_PTR)pid;
        if (!m->os[5])
            return 0;
    }
    return WaitForSingleObject(m->os[5], 0) == WAIT_TIMEOUT;
}

unsigned long long mh2ipc_time_ns(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    unsigned long long f = (unsigned long long)freq.QuadPart, t = (unsigned long long)c.QuadPart;
    return t / f * 1000000000ull + t % f * 1000000000ull / f;
}

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

void mh2ipc_wait_until_ns(unsigned long long due)
{
    static HANDLE timer;
    static int tried;
    if (!tried) {
        tried = 1;
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    }
    unsigned long long now = mh2ipc_time_ns();
    if (now + 1500000ull < due) {
        unsigned long long ns = due - now - 1000000ull;
        if (timer) {
            LARGE_INTEGER rel;
            rel.QuadPart = -(LONGLONG)(ns / 100ull);   /* relative, in 100 ns units */
            if (SetWaitableTimer(timer, &rel, 0, NULL, NULL, FALSE))
                WaitForSingleObject(timer, INFINITE);
        } else if (ns >= 2000000ull) {
            Sleep((DWORD)(ns / 1000000ull) - 1);   /* the system's tick may be coarse */
        }
    }
    while (mh2ipc_time_ns() < due)
        YieldProcessor();
}
