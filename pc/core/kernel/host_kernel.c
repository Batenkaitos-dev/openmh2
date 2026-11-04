/* The kernel's host functions (khost.h): system headers, the host's calling convention. */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE     /* ucontext's REG_EIP */
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <signal.h>
#include <ucontext.h>
#endif

#include "kernel/khost.h"

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define HOSTK_SANITIZER 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) && !defined(HOSTK_SANITIZER)
#define HOSTK_SANITIZER 1
#endif

#ifndef HOSTK_SANITIZER
/* Under ASan the sanitizer keeps its own handlers: the crash handler, its function and its table are left out
 * (HostK_CatchCrashes does nothing). */
static HostKCrashFn sCrashFn;
#endif

#ifdef _WIN32
/*
 * A vectored handler, not an unhandled-exception filter: the kernel's fibers
 * start with an empty exception list (fiber.c), so on a fiber no frame-based
 * handler, and so no filter, ever runs; the system ends the process. A
 * vectored handler sees every exception first, wherever it happens: it takes
 * only the fatal ones and passes the others on.
 */
#ifndef HOSTK_SANITIZER
static int IsFatal(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_INT_OVERFLOW:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
    case EXCEPTION_IN_PAGE_ERROR:
        return 1;
    default:
        return 0;
    }
}

static LONG WINAPI OnException(EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *r = ep->ExceptionRecord;
    unsigned int pc = (unsigned int)(uintptr_t)r->ExceptionAddress;
    unsigned int addr = pc;
    HostKCrashFn fn = sCrashFn;
    if (!fn || !IsFatal(r->ExceptionCode)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
        addr = (unsigned int)r->ExceptionInformation[1];
    }
    sCrashFn = 0;                       /* a second fault: the system's handling */
    fn((int)r->ExceptionCode, addr, pc);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

void HostK_CatchCrashes(HostKCrashFn fn)
{
#ifndef HOSTK_SANITIZER
    static int installed;
    ULONG guarantee = 64 * 1024;        /* room for the handler after a stack overflow */
    sCrashFn = fn;
    if (installed) {
        return;
    }
    installed = 1;
    SetThreadStackGuarantee(&guarantee);
    AddVectoredExceptionHandler(1, OnException);
#else
    (void)fn;
#endif
}

const char *HostK_CrashName(int code)
{
    switch ((DWORD)code) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case EXCEPTION_INT_OVERFLOW: return "integer overflow";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "misaligned access";
    case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
    default: return "exception";
    }
}
#else
#ifndef HOSTK_SANITIZER
static const int sCrashSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE};

static void OnSignal(int sig, siginfo_t *si, void *ucv)
{
    unsigned int pc = 0;
    unsigned int addr;
    HostKCrashFn fn = sCrashFn;
#if defined(__i386__)
    pc = (unsigned int)((ucontext_t *)ucv)->uc_mcontext.gregs[REG_EIP];
#else
    (void)ucv;
#endif
    addr = (sig == SIGSEGV || sig == SIGBUS) ? (unsigned int)(uintptr_t)si->si_addr : pc;
    sCrashFn = 0;
    if (fn) {
        fn(sig, addr, pc);
    }
    _Exit(128 + sig);
}
#endif

void HostK_CatchCrashes(HostKCrashFn fn)
{
#ifndef HOSTK_SANITIZER
    static int installed;
    stack_t ss;
    struct sigaction sa;
    unsigned int i;
    sCrashFn = fn;
    if (installed) {
        return;
    }
    installed = 1;
    ss.ss_sp = malloc(256 * 1024);      /* verify's finish writes files: room for it */
    ss.ss_size = 256 * 1024;
    ss.ss_flags = 0;
    if (ss.ss_sp && sigaltstack(&ss, 0) != 0) {
        free(ss.ss_sp);
    }
    for (i = 0; i < sizeof(sCrashSignals) / sizeof(sCrashSignals[0]); i++) {
        sigemptyset(&sa.sa_mask);
        sa.sa_sigaction = OnSignal;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;   /* a second fault: the default action */
        sigaction(sCrashSignals[i], &sa, 0);
    }
#else
    (void)fn;
#endif
}

const char *HostK_CrashName(int code)
{
    switch (code) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGILL: return "SIGILL";
    case SIGFPE: return "SIGFPE";
    default: return "signal";
    }
}
#endif

void HostK_LocalDate(long long *epochSeconds, long long *utcOffsetSeconds)
{
    struct tm lt;
#ifdef _WIN32
    __time64_t now = _time64(NULL);
    _localtime64_s(&lt, &now);
    __time64_t asUtc = _mkgmtime64(&lt);   /* the local fields read as UTC */
#else
    time_t now = time(NULL);
    localtime_r(&now, &lt);
    time_t asUtc = timegm(&lt);
#endif
    *epochSeconds = (long long)now;
    *utcOffsetSeconds = (long long)asUtc - (long long)now;
}

void HostK_Exit(int code)
{
    Host_ExitHook(code);
    fflush(stdout);
    fflush(stderr);
    exit(code);
}
