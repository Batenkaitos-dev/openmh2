/*
 * The kernel's log lines: a small formatter (the platform layer's
 * game-facing files include no system headers, so no snprintf), then
 * Host_Log. Conversions: %d %u %x %X %s %c %%, with an optional 0 and width,
 * and the ll length (64-bit).
 */
#include "kernel/klog.h"
#include "host.h"

static void Put(char *buf, int *n, int cap, char c)
{
    if (*n < cap - 1) {
        buf[(*n)++] = c;
    }
}

static void PutNum(char *buf, int *n, int cap, u64 v, int base, int upper, int neg, int width, int zero)
{
    char tmp[24];
    int k = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do {
        tmp[k++] = digits[v % (u64)base];
        v /= (u64)base;
    } while (v);
    if (neg) {
        if (zero) {
            Put(buf, n, cap, '-');
        } else {
            tmp[k++] = '-';
        }
        width--;
    }
    while (width > k) {
        Put(buf, n, cap, zero ? '0' : ' ');
        width--;
    }
    while (k) {
        Put(buf, n, cap, tmp[--k]);
    }
}

void KLogV(char *buf, int cap, const char *fmt, __builtin_va_list ap)
{
    int n = 0;
    while (*fmt) {
        int zero = 0, width = 0, ll = 0;
        char c = *fmt++;
        if (c != '%') {
            Put(buf, &n, cap, c);
            continue;
        }
        if (*fmt == '0') {
            zero = 1;
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt++ - '0');
        }
        while (*fmt == 'l') {
            ll++;
            fmt++;
        }
        c = *fmt ? *fmt++ : 0;
        switch (c) {
        case 'd': {
            s64 v = ll >= 2 ? __builtin_va_arg(ap, s64) : (s64)__builtin_va_arg(ap, s32);
            PutNum(buf, &n, cap, (u64)(v < 0 ? -v : v), 10, 0, v < 0, width, zero);
            break;
        }
        case 'u':
        case 'x':
        case 'X': {
            u64 v = ll >= 2 ? __builtin_va_arg(ap, u64) : (u64)__builtin_va_arg(ap, u32);
            PutNum(buf, &n, cap, v, c == 'u' ? 10 : 16, c == 'X', 0, width, zero);
            break;
        }
        case 's': {
            const char *s = __builtin_va_arg(ap, const char *);
            if (!s) {
                s = "(null)";
            }
            while (*s) {
                Put(buf, &n, cap, *s++);
            }
            break;
        }
        case 'c':
            Put(buf, &n, cap, (char)__builtin_va_arg(ap, int));
            break;
        case '%':
            Put(buf, &n, cap, '%');
            break;
        default:
            break;
        }
    }
    buf[n] = 0;
}

void KFormat(char *buf, int cap, const char *fmt, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    KLogV(buf, cap, fmt, ap);
    __builtin_va_end(ap);
}

void KLog(const char *fmt, ...)
{
    char buf[512];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    KLogV(buf, sizeof(buf), fmt, ap);
    __builtin_va_end(ap);
    Host_Log(buf);
}

void KFatal(const char *fmt, ...)
{
    char buf[512];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    KLogV(buf, sizeof(buf), fmt, ap);
    __builtin_va_end(ap);
    Host_Fatal(buf);
}
