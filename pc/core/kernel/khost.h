#ifndef MH2_KERNEL_KHOST_H
#define MH2_KERNEL_KHOST_H

/* The kernel's part of the host seam (host_kernel.c; host.h's convention: cdecl). */
#include "host.h"

/* Flushes the output and ends the process with the code. */
HOST void HostK_Exit(int code) __attribute__((noreturn));

/* The host's date and time zone now: seconds since 1970-01-01 UTC, and the
 * local zone's offset from UTC in seconds (east positive), daylight saving
 * included. The kernel reads it once, for play's calendar (sched.c). */
HOST void HostK_LocalDate(long long *epochSeconds, long long *utcOffsetSeconds);

/*
 * A crash (Linux: SIGSEGV, SIGBUS, SIGILL or SIGFPE, on an alternate signal
 * stack, since a fiber's stack may be what overflowed; Windows: a vectored
 * exception handler, for the fatal exceptions only) calls fn once, with the signal or the exception code, the fault
 * address (the access's for SIGSEGV, SIGBUS and an access violation, else
 * the instruction's) and the instruction's address. fn must not return: it
 * ends the process (HostK_Exit). A second fault inside it ends the process
 * plainly. Not installed in a sanitizer build (ASan reports the crash).
 */
typedef void (HOST *HostKCrashFn)(int code, unsigned int faultAddr, unsigned int pc);
HOST void HostK_CatchCrashes(HostKCrashFn fn);
HOST const char *HostK_CrashName(int code);

#endif
