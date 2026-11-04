#ifndef MH2_KERNEL_KLOG_H
#define MH2_KERNEL_KLOG_H

#include "common.h"

/* Log lines with numbers (klog.c): %d %u %x %X %s %c, 0 and a width, ll for 64 bits. Variadic, so cdecl. */
void KLog(const char *fmt, ...);
void KFatal(const char *fmt, ...) __attribute__((noreturn));
void KFormat(char *buf, int cap, const char *fmt, ...);
void KLogV(char *buf, int cap, const char *fmt, __builtin_va_list ap);

#endif
