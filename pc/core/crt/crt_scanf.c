/* No user fns here. Pesch 15apr92. */

/*
 * Copyright (c) 1990 The Regents of the University of California.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms are permitted
 * provided that the above copyright notice and this paragraph are
 * duplicated in all such forms and that any documentation,
 * advertising materials, and other materials related to such
 * distribution and use acknowledge that the software was developed
 * by the University of California, Berkeley.  The name of the
 * University may not be used to endorse or promote products derived
 * from this software without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 */

/* The C library's sscanf on PC, re-hosted from the earlier port's libc_scanf.c (Monster
 * Hunter 2 decomp; listed in THIRD_PARTY.md).
 * The scanner below is newlib 1.8.2's (libc/stdio/vfscanf.c: __svfscanf
 * and __sccl, built without MB_CAPABLE), with the stream calls it makes on
 * sscanf's string stream (refill.c's __srefill, ungetc.c's ungetc,
 * fread.c's fread) and the conversions it calls (stdlib's strtol and
 * strtoul), adapted: the stream's state is ps2crt's (sscanf's FILE is on
 * the guest's stack, outside what Capcom's code sees).
 *
 * What the original game's one does, observed: sscanf(str, fmt, ...) takes strlen(str),
 * builds a read-only string stream on its stack and calls the scanner
 * __svfscanf(stream, fmt, ap); nethttp's answer parser is the only caller
 * seen, with "HTTP/%d.%d %d %s". The C library's long is 64 bits (the
 * formatted output's tests), so %l stores 8 bytes and strtol's range is
 * 64-bit.
 *
 * Addresses are the PS2's: the core's memory is at them, so an address
 * is a pointer (RD8 and the stores below). This file is for the 32-bit core.
 *
 * What it leaves out, none of it visible to Capcom's code: the stream's
 * CHECK_INIT and strtol's errno on overflow (the reent's). The
 * floating-point conversions (never passed by the game; each is noted by
 * CRT_UNSEEN) follow 1.8.2 and the C standard, the digits converted with the
 * soft doubles (crt_strtod_bits below: a decimal significand scaled by
 * powers of ten, rounded toward zero, not newlib's correctly rounded
 * strtod). */
#include "ps2crt.h"
#include "ps2dp.h"
#include <stdarg.h>

#define RD8(a) (*(const u8 *)(uintptr_t)(a))
#define WR8(a, v) (*(u8 *)(uintptr_t)(a) = (u8)(v))
#define WR16(a, v) (*(u16 *)(uintptr_t)(a) = (u16)(v))
#define WR32(a, v) (*(u32 *)(uintptr_t)(a) = (u32)(v))
#define WR64(a, v) (*(u64 *)(uintptr_t)(a) = (u64)(v))
#define LG_UNSEEN CRT_UNSEEN

typedef struct ScVa {
    va_list *ap;
} ScVa;
/* Every argument of sscanf after the format is a pointer. */
static u64 sc_next(ScVa *v)
{
    return (u32)(uintptr_t)va_arg(*v->ap, void *);
}


#define SS_UB 0xFFFFFFF0u   /* the stream's pointer inside its 3-byte ungetc buffer: SS_UB + i */
#define SS_BUF 350          /* BUF: MAXEXP + MAXFRACT + 3 (floatio.h: 308 and 39) */

/* sscanf's string stream: the FILE fields the scanner uses. */
typedef struct SsIn {
    u32 p;         /* _p: the next byte (guest), or SS_UB + i */
    s32 r;         /* _r: bytes left */
    u32 base;      /* _bf._base: the string */
    int seof;      /* __SEOF */
    int hasub;     /* HASUB: the ungetc buffer in use */
    u8 ub[3];      /* _ubuf */
    u32 up;        /* _up, _ur: the stream's place under the ungetc buffer */
    s32 ur;
} SsIn;

/* The decimal number in buf (sign, digits, point, exponent) as an EE double's
 * bits: up to 19 significant digits as an integer, scaled by a power of ten
 * with the soft doubles. Never reached by the game. */
static u64 crt_strtod_bits(const char *b)
{
    u64 m = 0;
    int neg = 0, exp10 = 0, nd = 0, seen_dot = 0;
    if (*b == '-' || *b == '+')
        neg = *b++ == '-';
    for (; *b; b++) {
        if (*b >= '0' && *b <= '9') {
            if (nd < 19) {
                m = m * 10 + (u64)(*b - '0');
                if (m)
                    nd++;
                if (seen_dot)
                    exp10--;
            } else if (!seen_dot) {
                exp10++;
            }
        } else if (*b == '.' && !seen_dot) {
            seen_dot = 1;
        } else {
            break;
        }
    }
    if (*b == 'e' || *b == 'E') {
        int es = 1, ev = 0;
        b++;
        if (*b == '-' || *b == '+')
            es = *b++ == '-' ? -1 : 1;
        for (; *b >= '0' && *b <= '9'; b++)
            ev = ev < 100000 ? ev * 10 + (*b - '0') : ev;
        exp10 += es * ev;
    }
    u64 d = ps2dp_add(ps2dp_mul(ps2dp_from_si((s32)(m >> 32)), 0x41F0000000000000ull), ps2dp_from_si(0));
    /* m = hi * 2^32 + lo: the low half is added as an unsigned 32-bit value */
    {
        u32 lo = (u32)m;
        u64 l = ps2dp_from_si((s32)(lo >> 1));
        l = ps2dp_add(l, l);
        if (lo & 1)
            l = ps2dp_add(l, 0x3FF0000000000000ull);
        d = ps2dp_add(d, l);
    }
    u64 ten = 0x4024000000000000ull;
    for (int i = 0; i < (exp10 < 0 ? -exp10 : exp10) && i < 400; i++)
        d = exp10 < 0 ? ps2dp_div(d, ten) : ps2dp_mul(d, ten);
    return neg ? d ^ PS2DP_SIGN : d;
}

static int ss_sp(int c)   /* isspace in the C locale (the C library's _ctype_) */
{
    return c == ' ' || (c >= 9 && c <= 13);
}

static u8 ss_at(const SsIn *f)
{
    return f->p >= SS_UB ? f->ub[f->p - SS_UB] : RD8(f->p);
}

/* __srefill on the string stream: its read answers 0 (the end). */
static int ss_refill(SsIn *f)
{
    f->r = 0;
    if (f->seof)
        return -1;
    if (f->hasub) {   /* FREEUB: the stream's own three bytes */
        f->hasub = 0;
        if ((f->r = f->ur) != 0) {
            f->p = f->up;
            return 0;
        }
    }
    f->p = f->base;
    f->r = 0;
    f->seof = 1;
    return -1;
}

#define SS_EMPTY(f) ((f)->r <= 0 && ss_refill(f))

static void ss_ungetc(SsIn *f, int c)
{
    if (c == -1)
        return;
    f->seof = 0;
    c &= 0xFF;
    if (f->hasub) {
        if (f->r >= 3) {   /* __submore (the C library's heap): the scanner never pushes back so many */
            LG_UNSEEN("sscanf", "a fourth byte pushed back");
            return;
        }
        f->p--;
        f->ub[f->p - SS_UB] = (u8)c;
        f->r++;
        return;
    }
    if (f->p > f->base && RD8(f->p - 1) == (u8)c) {
        f->p--;
        f->r++;
        return;
    }
    f->ur = f->r;
    f->up = f->p;
    f->hasub = 1;
    f->ub[2] = (u8)c;
    f->p = SS_UB + 2;
    f->r = 1;
}

/* fread(dst, 1, n, stream): the bytes stored in order. */
static u32 ss_fread(SsIn *f, u32 dst, u32 n)
{
    u32 resid = n;
    if (!resid)
        return 0;
    if (f->r < 0)
        f->r = 0;
    while (resid > (u32)f->r) {
        for (s32 i = 0; i < f->r; i++)
            WR8(dst++, ss_at(f)), f->p++;
        resid -= (u32)f->r;
        if (ss_refill(f))
            return n - resid;
    }
    for (u32 i = 0; i < resid; i++)
        WR8(dst++, ss_at(f)), f->p++;
    f->r -= (s32)resid;
    return n;
}

/* strtol and strtoul (C standard 7.20.1.4) at the C library's 64-bit
 * long, over the scanner's digits: *over set on overflow (the C library
 * writes ERANGE to the reent's errno then). */
static u64 ss_strtox(const char *s, int base, int sgn, int *over)
{
    int neg = 0, c = (u8)*s++;
    while (ss_sp(c))
        c = (u8)*s++;
    if (c == '-')
        neg = 1, c = (u8)*s++;
    else if (c == '+')
        c = (u8)*s++;
    if ((base == 0 || base == 16) && c == '0' && (*s == 'x' || *s == 'X')) {
        c = (u8)s[1];
        s += 2;
        base = 16;
    }
    if (base == 0)
        base = c == '0' ? 8 : 10;
    u64 lim = sgn ? (neg ? 0x8000000000000000ull : 0x7FFFFFFFFFFFFFFFull) : ~0ull;
    u64 cutoff = lim / (u64)base, cutlim = lim % (u64)base, acc = 0;
    int any = 0;
    for (;; c = (u8)*s++) {
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'Z' ? c - 'A' + 10 : c >= 'a' && c <= 'z' ? c - 'a' + 10 : 99;
        if (d >= base)
            break;
        if (any < 0 || acc > cutoff || (acc == cutoff && (u64)d > cutlim))
            any = -1;
        else
            any = 1, acc = acc * (u64)base + (u64)d;
    }
    if (any < 0) {
        *over = 1;
        return lim;
    }
    return neg ? 0 - acc : acc;
}

/* __sccl: the scanset after '[' into tab; returns the format's address
 * past the closing ']' (or at its end). */
static u32 ss_sccl(u8 *tab, u32 fmt)
{
    int c = (s8)RD8(fmt++), n, v;
    if (c == '^') {
        v = 1;
        c = (s8)RD8(fmt++);
    } else {
        v = 0;
    }
    __builtin_memset(tab, v, 256);
    if (c == 0)
        return fmt - 1;
    v = 1 - v;
    for (;;) {
        if (c < 0)   /* the C library's format bytes are signed chars: tab[c] would lie outside the table */
            LG_UNSEEN("sscanf", "a scanset byte over 0x7F");
        tab[(u8)c] = (u8)v;
    doswitch:
        n = (s8)RD8(fmt++);
        switch (n) {
        case 0:
            return fmt - 1;
        case '-':
            n = (s8)RD8(fmt);
            if (n == ']' || n < c) {
                c = '-';
                break;
            }
            fmt++;
            do {
                tab[(u8)++c] = (u8)v;
            } while (c < n);
            goto doswitch;
        case ']':
            return fmt;
        default:
            c = n;
            break;
        }
    }
}

enum {
    LONG = 0x01, LONGDBL = 0x02, SHORT = 0x04, SUPPRESS = 0x08, POINTER = 0x10, NOSKIP = 0x20,
    SIGNOK = 0x40, NDIGITS = 0x80, DPTOK = 0x100, EXPOK = 0x200, PFXOK = 0x100, NZDIGITS = 0x200,
};
enum { CT_CHAR, CT_CCL, CT_STRING, CT_INT, CT_FLOAT };

/* __svfscanf on the string stream. */
static int ss_scan(SsIn *f, u32 fmt, ScVa *va)
{
    static const short basefix[17] = {10, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    int nassigned = 0, nread = 0, base = 0, sgn = 0, c, flags;
    u32 width;
    u8 ccltab[256];
    char buf[SS_BUF], *p;
    for (;;) {
        int wc = (s8)RD8(fmt++);
        if (wc == 0)
            return nassigned;
        if (wc < 0)   /* isspace of a negative char reads outside the C library's table */
            LG_UNSEEN("sscanf", "a format byte over 0x7F");
        if (ss_sp(wc)) {
            for (;;) {
                if (SS_EMPTY(f))
                    return nassigned;
                if (!ss_sp(ss_at(f)))
                    break;
                nread++, f->r--, f->p++;
            }
            continue;
        }
        if (wc != '%')
            goto literal;
        width = 0;
        flags = 0;
    again:
        c = (s8)RD8(fmt++);
        switch (c) {
        case '%':
        literal:
            if (SS_EMPTY(f))
                goto input_failure;
            if (ss_at(f) != (u32)(s32)(s8)RD8(fmt - 1))   /* an unsigned byte against a signed char */
                goto match_failure;
            f->r--, f->p++;
            nread++;
            continue;
        case '*':
            flags |= SUPPRESS;
            goto again;
        case 'l':
            flags |= LONG;
            goto again;
        case 'L':
            flags |= LONGDBL;
            goto again;
        case 'h':
            flags |= SHORT;
            goto again;
        case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9':
            width = width * 10 + (u32)(c - '0');
            goto again;
        case 'D':
            flags |= LONG;
            /* fall through */
        case 'd':
            c = CT_INT, sgn = 1, base = 10;
            break;
        case 'i':
            c = CT_INT, sgn = 1, base = 0;
            break;
        case 'O':
            flags |= LONG;
            /* fall through */
        case 'o':
            c = CT_INT, sgn = 0, base = 8;
            break;
        case 'u':
            c = CT_INT, sgn = 0, base = 10;
            break;
        case 'X':
        case 'x':
            flags |= PFXOK;
            c = CT_INT, sgn = 0, base = 16;
            break;
        case 'E': case 'G': case 'e': case 'f': case 'g':
            c = CT_FLOAT;
            break;
        case 's':
            c = CT_STRING;
            break;
        case '[':
            fmt = ss_sccl(ccltab, fmt);
            flags |= NOSKIP;
            c = CT_CCL;
            break;
        case 'c':
            flags |= NOSKIP;
            c = CT_CHAR;
            break;
        case 'p':
            flags |= POINTER | PFXOK;
            c = CT_INT, sgn = 0, base = 16;
            break;
        case 'n': {
            if (flags & SUPPRESS)
                continue;
            u32 a = (u32)sc_next(va);
            if (flags & SHORT)
                WR16(a, (u16)nread);
            else if (flags & LONG)
                WR64(a, (u64)(s64)nread);
            else
                WR32(a, (u32)nread);
            continue;
        }
        case '\0':
            return -1;
        default:
            if (c < 0)
                LG_UNSEEN("sscanf", "a conversion byte over 0x7F");
            if (c >= 'A' && c <= 'Z')
                flags |= LONG;
            c = CT_INT, sgn = 1, base = 10;
            break;
        }

        if (SS_EMPTY(f))
            goto input_failure;
        if (!(flags & NOSKIP)) {
            while (ss_sp(ss_at(f))) {
                nread++;
                if (--f->r > 0)
                    f->p++;
                else if (ss_refill(f))
                    goto input_failure;
            }
        }

        switch (c) {
        case CT_CHAR:
            if (width == 0)
                width = 1;
            if (flags & SUPPRESS) {
                u32 sum = 0;
                for (;;) {
                    u32 n = (u32)f->r;
                    if (n < width) {
                        sum += n;
                        width -= n;
                        f->p += n;
                        if (ss_refill(f)) {
                            if (sum == 0)
                                goto input_failure;
                            break;
                        }
                    } else {
                        sum += width;
                        f->r -= (s32)width;
                        f->p += width;
                        break;
                    }
                }
                nread += (int)sum;
            } else {
                u32 r = ss_fread(f, (u32)sc_next(va), width);
                if (r == 0)
                    goto input_failure;
                nread += (int)r;
                nassigned++;
            }
            break;
        case CT_CCL: {
            int n = 0;
            if (width == 0)
                width = ~0u;
            if (flags & SUPPRESS) {
                while (ccltab[ss_at(f)]) {
                    n++, f->r--, f->p++;
                    if (--width == 0)
                        break;
                    if (SS_EMPTY(f)) {
                        if (n == 0)
                            goto input_failure;
                        break;
                    }
                }
                if (n == 0)
                    goto match_failure;
            } else {
                u32 d0 = (u32)sc_next(va), d = d0;
                while (ccltab[ss_at(f)]) {
                    f->r--;
                    WR8(d++, ss_at(f)), f->p++;
                    if (--width == 0)
                        break;
                    if (SS_EMPTY(f)) {
                        if (d == d0)
                            goto input_failure;
                        break;
                    }
                }
                n = (int)(d - d0);
                if (n == 0)
                    goto match_failure;
                WR8(d, 0);
                nassigned++;
            }
            nread += n;
            break;
        }
        case CT_STRING:
            if (width == 0)
                width = ~0u;
            if (flags & SUPPRESS) {
                int n = 0;
                while (!ss_sp(ss_at(f))) {
                    n++, f->r--, f->p++;
                    if (--width == 0)
                        break;
                    if (SS_EMPTY(f))
                        break;
                }
                nread += n;
            } else {
                u32 d0 = (u32)sc_next(va), d = d0;
                while (!ss_sp(ss_at(f))) {
                    f->r--;
                    WR8(d++, ss_at(f)), f->p++;
                    if (--width == 0)
                        break;
                    if (SS_EMPTY(f))
                        break;
                }
                WR8(d, 0);
                nread += (int)(d - d0);
                nassigned++;
            }
            continue;
        case CT_INT:
            if (--width > SS_BUF - 2)
                width = SS_BUF - 2;
            width++;
            flags |= SIGNOK | NDIGITS | NZDIGITS;
            for (p = buf; width; width--) {
                c = ss_at(f);
                switch (c) {
                case '0':
                    if (base == 0) {
                        base = 8;
                        flags |= PFXOK;
                    }
                    if (flags & NZDIGITS)
                        flags &= ~(SIGNOK | NZDIGITS | NDIGITS);
                    else
                        flags &= ~(SIGNOK | PFXOK | NDIGITS);
                    goto ok;
                case '1': case '2': case '3': case '4': case '5': case '6': case '7':
                    base = basefix[base];
                    flags &= ~(SIGNOK | PFXOK | NDIGITS);
                    goto ok;
                case '8': case '9':
                    base = basefix[base];
                    if (base <= 8)
                        break;
                    flags &= ~(SIGNOK | PFXOK | NDIGITS);
                    goto ok;
                case 'A': case 'B': case 'C': case 'D': case 'E': case 'F':
                case 'a': case 'b': case 'c': case 'd': case 'e': case 'f':
                    if (base <= 10)
                        break;
                    flags &= ~(SIGNOK | PFXOK | NDIGITS);
                    goto ok;
                case '+': case '-':
                    if (flags & SIGNOK) {
                        flags &= ~SIGNOK;
                        goto ok;
                    }
                    break;
                case 'x': case 'X':
                    if ((flags & PFXOK) && p == buf + 1) {
                        base = 16;
                        flags &= ~PFXOK;
                        goto ok;
                    }
                    break;
                }
                break;
            ok:
                *p++ = (char)c;
                if (--f->r > 0)
                    f->p++;
                else if (ss_refill(f))
                    break;
            }
            if (flags & NDIGITS) {
                if (p > buf)
                    ss_ungetc(f, (s8) * --p);
                goto match_failure;
            }
            c = (s8)p[-1];
            if (c == 'x' || c == 'X') {
                --p;
                ss_ungetc(f, c);
            }
            if (!(flags & SUPPRESS)) {
                int over = 0;
                *p = 0;
                u64 res = ss_strtox(buf, base, sgn, &over);
                if (over)
                    LG_UNSEEN("sscanf", "a number out of the 64-bit range (the C library writes errno)");
                u32 a = (u32)sc_next(va);
                if (flags & POINTER)
                    WR32(a, (u32)res);
                else if (flags & SHORT)
                    WR16(a, (u16)res);
                else if (flags & LONG)
                    WR64(a, res);
                else
                    WR32(a, (u32)res);
                nassigned++;
            }
            nread += (int)(p - buf);
            break;
        case CT_FLOAT:
            LG_UNSEEN("sscanf", "a floating-point conversion");
            if (--width > SS_BUF - 2)
                width = SS_BUF - 2;
            width++;
            flags |= SIGNOK | NDIGITS | DPTOK | EXPOK;
            for (p = buf; width; width--) {
                c = ss_at(f);
                switch (c) {
                case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9':
                    flags &= ~(SIGNOK | NDIGITS);
                    goto fok;
                case '+': case '-':
                    if (flags & SIGNOK) {
                        flags &= ~SIGNOK;
                        goto fok;
                    }
                    break;
                case '.':
                    if (flags & DPTOK) {
                        flags &= ~(SIGNOK | DPTOK);
                        goto fok;
                    }
                    break;
                case 'e': case 'E':
                    if ((flags & (NDIGITS | EXPOK)) == EXPOK) {
                        flags = (flags & ~(EXPOK | DPTOK)) | SIGNOK | NDIGITS;
                        goto fok;
                    }
                    break;
                }
                break;
            fok:
                *p++ = (char)c;
                if (--f->r > 0)
                    f->p++;
                else if (ss_refill(f))
                    break;
            }
            if (flags & NDIGITS) {
                if (flags & EXPOK) {
                    while (p > buf)
                        ss_ungetc(f, (s8) * --p);
                    goto match_failure;
                }
                c = (s8) * --p;
                if (c != 'e' && c != 'E') {
                    ss_ungetc(f, c);
                    c = (s8) * --p;
                }
                ss_ungetc(f, c);
            }
            if (!(flags & SUPPRESS)) {
                *p = 0;
                u64 bits = crt_strtod_bits(buf);
                u32 a = (u32)sc_next(va);
                if (flags & (LONG | LONGDBL))
                    WR64(a, bits);
                else
                    WR32(a, ps2dp_to_sf(bits));
                nassigned++;
            }
            nread += (int)(p - buf);
            break;
        }
    }
input_failure:
    return nassigned ? nassigned : -1;
match_failure:
    return nassigned;
}

/* sscanf(str, fmt, ...) (0x10A968). */
s32 sscanf(const char *str, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    ScVa va = {&ap};
    SsIn f;
    __builtin_memset(&f, 0, sizeof(f));
    f.p = f.base = (u32)(uintptr_t)str;
    while (RD8(f.base + (u32)f.r)) /* strlen */
        f.r++;
    s32 r = ss_scan(&f, (u32)(uintptr_t)fmt, &va);
    va_end(ap);
    return r;
}
