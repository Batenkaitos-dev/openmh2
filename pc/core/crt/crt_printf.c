/* The C library's formatted output on PC: sprintf, vsprintf, vsnprintf and
 * printf. Re-hosted from the earlier port's libc_printf.c (Milestone 7,
 *.
 *
 * Written from the C standard (ISO/IEC 9899, 7.19.6) and the conventions
 * of the BSD-derived vfprintf the toolchain's C library (newlib) uses,
 * settled by black-box tests of the original functions
 * and on the game's own
 * calls: flags, width and precision in any order, the conversions of the
 * standard, an unknown conversion character printed as itself, output
 * stored piece by piece at the end of each conversion (what an argument
 * string that overlaps the destination sees), and floating-point digits
 * exact (correctly rounded, half to even) from the double's exact decimal
 * expansion.
 *
 * The arguments: the EE passed every one in an 8-byte slot. Here an integer
 * or pointer argument is the 32-bit int the game's C passes (widened as
 * the EE's register holds it: sign-extended), and a double is the 64 bits
 * of an EE double in a host double that only carries them. */
#include <stdarg.h>

#include "crt_stdout.h"
#include "ps2crt.h"
#include "crt_tap.h"

void *memmove(void *dst, const void *src, u32 n);

/* ------------------------------------------------------------ the output */
/* Pieces of output (bytes of local buffers, or of an argument string),
 * stored in order at the end of each conversion or when eight are waiting,
 * as a stream that writes vectors of pieces does. A piece is moved as
 * memmove would, so an argument string that overlaps the destination is
 * read whole before it is stored. */
#define NPIECE 8
typedef struct Piece {
    const char *p;
    u32 len;
} Piece;

typedef struct Sink {
    int console; /* printf: each piece also goes to stdout's stream (crt_stdout.c) */
    char *dst;   /* the next byte stored (NULL: printf stores none) */
    u32 room;    /* bytes that may still be stored (vsnprintf: size - 1) */
    u32 count;   /* the return value: the fields' sizes, as the C library counts */
    Piece pc[NPIECE];
    int npc;
} Sink;

typedef struct CrtVa {
    va_list *ap;
} CrtVa;

/* An integer or pointer argument, widened as the EE's register holds it. */
static u64 crt_va_int(CrtVa *v)
{
    return (u64)(s64)va_arg(*v->ap, s32);
}

/* A double argument: its 64 bits. */
static u64 crt_va_dbl(CrtVa *v)
{
    double d = va_arg(*v->ap, double);
    u64 u;
    __builtin_memcpy(&u, &d, 8);
    return u;
}

static void sk_store(Sink *s, const char *src, u32 n)
{
    if (n > s->room)
        n = s->room;
    if (!n || !s->dst)
        return;
    memmove(s->dst, src, n);
    s->dst += n;
    s->room -= n;
}

static void sk_flush(Sink *s)
{
    for (int i = 0; i < s->npc; i++) {
        Piece *p = &s->pc[i];
        if (s->console)
            crt_so_piece(p->p, p->len);
        sk_store(s, p->p, p->len);
    }
    s->npc = 0;
}

static void put_host(Sink *s, const char *p, u32 n)
{
    if (!n)
        return;
    s->pc[s->npc].p = p;
    s->pc[s->npc].len = n;
    s->npc++;
    if (s->npc == NPIECE)
        sk_flush(s);
}

#define PADSIZE 16
static const char BLANKS[PADSIZE + 1] = "                ";
static const char ZEROES[PADSIZE + 1] = "0000000000000000";

static void put_pad(Sink *s, int n, const char *with)
{
    while (n > PADSIZE) {
        put_host(s, with, PADSIZE);
        n -= PADSIZE;
    }
    if (n > 0)
        put_host(s, with, (u32)n);
}

/* ------------------------------------------------- floating-point digits */
/* The exact decimal expansion of a finite double v > 0, as dtoa gives it:
 * the digits without leading or trailing zeros, and decpt, with
 * v = 0.DIGITS x 10^decpt. A double is m x 2^e: for e >= 0 the integer
 * m << e; for e < 0, m x 5^-e x 10^e. */
#define BIG_LIMBS 96
static int exact_digits(u64 bits, char *out, int *decpt)
{
    u64 m = bits & 0x000FFFFFFFFFFFFFull;
    int ex = (int)((bits >> 52) & 0x7FF);
    if (ex)
        m |= 1ull << 52;
    else
        ex = 1;
    int e = ex - 1075;
    while (!(m & 1)) {
        m >>= 1;
        e++;
    }
    u32 big[BIG_LIMBS] = {(u32)m, (u32)(m >> 32)};
    int n = big[1] ? 2 : 1, dexp = 0;
    if (e > 0) {
        int w = e / 32, b = e % 32;
        for (int i = n - 1; i >= 0; i--) {
            u64 x = (u64)big[i] << b;
            big[i + w + 1] |= (u32)(x >> 32);
            big[i + w] = (u32)x;
            if (w)
                big[i] = 0;
        }
        for (int i = 0; i < w; i++)
            big[i] = 0;
        n = n + w + 1;
    } else if (e < 0) {
        for (int k = -e; k > 0;) {
            int step = k > 13 ? 13 : k;
            u32 mul = 1;
            for (int i = 0; i < step; i++)
                mul *= 5;
            u64 carry = 0;
            for (int i = 0; i < n; i++) {
                u64 x = (u64)big[i] * mul + carry;
                big[i] = (u32)x;
                carry = x >> 32;
            }
            if (carry)
                big[n++] = (u32)carry;
            k -= step;
        }
        dexp = e;
    }
    while (n > 1 && !big[n - 1])
        n--;
    /* to decimal, nine digits at a time from the low end */
    char rev[800];
    int len = 0;
    while (n > 1 || big[0]) {
        u64 rem = 0;
        for (int i = n - 1; i >= 0; i--) {
            u64 x = (rem << 32) | big[i];
            big[i] = (u32)(x / 1000000000u);
            rem = x % 1000000000u;
        }
        while (n > 1 && !big[n - 1])
            n--;
        for (int k = 0; k < 9; k++) {
            rev[len++] = (char)('0' + rem % 10);
            rem /= 10;
        }
    }
    while (len > 0 && rev[len - 1] == '0')
        len--;
    for (int i = 0; i < len; i++)
        out[i] = rev[len - 1 - i];
    *decpt = len + dexp;
    while (len > 0 && out[len - 1] == '0')
        len--;
    out[len] = 0;
    return len;
}

/* The digits of v > 0 as dtoa's modes give them: mode 2, ndigits
 * significant digits (>= 1); mode 3, ndigits after the decimal point.
 * Correctly rounded, an exact half to even; trailing zeros dropped, but
 * for dtoa's "small integer" path (newlib's dtoa.c: an integer value
 * under 10^15 rounded to fewer digits than it has, when its floating-point
 * path gives up: dtoa_quick.c): rounded down, its digits keep their
 * trailing zeros (%g of 8435005.0 is "8.43500e+06", of 1885100160.0
 * "1.8851e+09"). */
int crt_dtoa_quick_fails(u64 bits, int k, int ilim);   /* dtoa_quick.c (newlib's) */

static int dtoa_digits(u64 bits, int mode, int ndigits, char *d, int *decpt)
{
    int len = exact_digits(bits, d, decpt);
    int n = mode == 2 ? ndigits : *decpt + ndigits;
    if (n >= len)
        return len;
    int ex = (int)((bits >> 52) & 0x7FF), e = ex - 1075;
    u64 m = (bits & 0x000FFFFFFFFFFFFFull) | (ex ? 1ull << 52 : 0);
    int small_int = *decpt - 1 <= 14 && (e >= 0 || (e > -53 && !(m & ((1ull << -e) - 1))));
    if (n < 0) {
        *decpt = -ndigits;
        d[0] = 0;
        return 0;
    }
    int up;
    if (d[n] > '5') {
        up = 1;
    } else if (d[n] < '5') {
        up = 0;
    } else {
        up = 0;
        for (int i = n + 1; i < len; i++)
            if (d[i] != '0')
                up = 1;
        if (!up)
            up = n > 0 && ((d[n - 1] - '0') & 1);
    }
    len = n;
    if (up) {
        int i = n - 1;
        while (i >= 0 && d[i] == '9')
            d[i--] = '0';
        if (i < 0) {
            d[0] = '1';
            len = 1;
            (*decpt)++;
        } else {
            d[i]++;
        }
    } else if (!n) {
        *decpt = mode == 3 ? -ndigits : *decpt;
    } else if (small_int && mode == 2 && crt_dtoa_quick_fails(bits, *decpt - 1, n)) {
        d[len] = 0;   /* rounded down: the zeros stay */
        return len;
    }
    while (len > 0 && d[len - 1] == '0')
        len--;
    d[len] = 0;
    return len;
}

/* ------------------------------------------------------------ the format */
enum {
    ALT = 0x001, LADJUST = 0x004, LONGDBL = 0x008, LONGINT = 0x010, QUADINT = 0x020,
    SHORTINT = 0x040, ZEROPAD = 0x080, FPT = 0x100, HEXPREFIX = 0x200,
};

/* The C library's long is 64 bits (the tests: %lx prints a slot's 64
 * bits), its int 32; "ll" and "q" take an int (the tests: %llx and %qx
 * print the slot's low 32 bits, as a C library built without long long
 * support does). */
#define LONG_BITS 64

/* Formats the string fmt with the arguments of va into s; returns the
 * number of characters produced. */
static u32 crt_vformat(Sink *s, const u8 *fmt, CrtVa *va)
{
    const u8 *p = fmt;
    for (;;) {
        const u8 *lit = p;
        while ((*p) && (*p) != '%')
            p++;
        put_host(s, (const char *)lit, (u32)(p - lit));
        s->count += p - lit;
        if (!(*p))
            break;
        p++;   /* the '%' */

        int flags = 0, width = 0, prec = -1, dprec = 0;
        char sign = 0;
        u8 ch;
        char buf[40];          /* the digits of an integer, a character */
        const char *cp = NULL; /* host bytes to print */
        const char *gcp = NULL; /* or the argument string (%s) */
        int size = 0, gstr = 0;
        char ox[2];
        /* floating point */
        char dig[800], expstr[8];
        int expt = 0, ndig = 0, expsize = 0, isfloat = 0;
        u64 dbits = 0, fmag = 0;
        const char *xdigs = "0123456789abcdef";
        u64 uq = 0;
        int base = 10;

    rflag:
        ch = (*p++);
    reswitch:
        switch (ch) {
        case ' ':
            if (!sign)
                sign = ' ';
            goto rflag;
        case '#':
            flags |= ALT;
            goto rflag;
        case '*':
            width = (s32)(u32)crt_va_int(va);
            if (width >= 0)
                goto rflag;
            width = -width;
            /* fall through */
        case '-':
            flags |= LADJUST;
            goto rflag;
        case '+':
            sign = '+';
            goto rflag;
        case '.':
            if ((ch = (*p++)) == '*') {
                int n = (s32)(u32)crt_va_int(va);
                prec = n < 0 ? -1 : n;
                goto rflag;
            } else {
                int n = 0;
                while (ch >= '0' && ch <= '9') {
                    n = 10 * n + (ch - '0');
                    ch = (*p++);
                }
                prec = n < 0 ? -1 : n;
                goto reswitch;
            }
        case '0':
            flags |= ZEROPAD;
            goto rflag;
        case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9': {
            int n = 0;
            do {
                n = 10 * n + (ch - '0');
                ch = (*p++);
            } while (ch >= '0' && ch <= '9');
            width = n;
            goto reswitch;
        }
        case 'h':
            flags |= SHORTINT;
            goto rflag;
        case 'l':
            if ((*p) == 'l') {
                p++;
                flags |= QUADINT;
            } else {
                flags |= LONGINT;
            }
            goto rflag;
        case 'q':
            flags |= QUADINT;
            goto rflag;
        case 'L':
            flags |= LONGDBL;
            goto rflag;
        case 'c':
            buf[0] = (char)(u8)crt_va_int(va);
            cp = buf;
            size = 1;
            sign = 0;
            break;
        case 'D':
            flags |= LONGINT;
            /* fall through */
        case 'd':
        case 'i': {
            u64 x = crt_va_int(va);
            s64 v;
            if (flags & LONGINT)
                v = LONG_BITS == 64 ? (s64)x : (s64)(s32)(u32)x;
            else if (flags & SHORTINT)
                v = (s16)(u16)x;
            else
                v = (s32)(u32)x;
            uq = (u64)v;
            if (v < 0) {
                uq = -uq;
                sign = '-';
            }
            base = 10;
            goto number;
        }
        case 'e':
        case 'E':
        case 'f':
        case 'g':
        case 'G': {
            dbits = crt_va_dbl(va);
            int ex = (int)((dbits >> 52) & 0x7FF);
            u64 frac = dbits & 0x000FFFFFFFFFFFFFull;
            if (ex == 0x7FF && !frac) {
                if (dbits >> 63)
                    sign = '-';
                cp = "Inf";
                size = 3;
                break;
            }
            if (ex == 0x7FF) {
                cp = "NaN";
                size = 3;
                break;
            }
            flags |= FPT;
            isfloat = 1;
            if (prec == -1)
                prec = 6;
            else if ((ch == 'g' || ch == 'G') && prec == 0)
                prec = 1;
            /* cvt */
            int neg = (int)(dbits >> 63);   /* the sign bit: -0 prints "-0" (the tests) */
            u64 mag = dbits & 0x7FFFFFFFFFFFFFFFull;
            if (mag < 0x0010000000000000ull)
                mag = 0;   /* a denormal prints as a zero (the tests: %G of -4.9e-324 is "-0") */
            fmag = mag;
            int mode, nd = prec;
            if (ch == 'f') {
                mode = 3;
            } else {
                if (ch == 'e' || ch == 'E')
                    nd++;
                mode = 2;
            }
            int len;
            if (!mag) {
                dig[0] = '0';
                dig[1] = 0;
                len = 1;
                expt = 1;
            } else {
                len = dtoa_digits(mag, mode, nd, dig, &expt);
            }
            if ((ch != 'g' && ch != 'G') || (flags & ALT)) {
                /* trailing zeros */
                int bp = nd;
                if (ch == 'f') {
                    if (dig[0] == '0' && mag)
                        expt = -nd + 1;
                    bp += expt;
                }
                if (!mag)
                    len = bp;
                while (len < bp)
                    dig[len++] = '0';
            }
            ndig = len;
            if (neg)
                sign = '-';
            if (ch == 'g' || ch == 'G') {
                if (expt <= -4 || expt > prec)
                    ch = ch == 'g' ? 'e' : 'E';
                else
                    ch = 'g';
            }
            if (ch <= 'e') {   /* 'e' or 'E' */
                int x = --expt;
                char *q = expstr;
                *q++ = (char)ch;
                if (x < 0) {
                    x = -x;
                    *q++ = '-';
                } else {
                    *q++ = '+';
                }
                if (x > 9) {
                    char t[8];
                    int k = 0;
                    do {
                        t[k++] = (char)('0' + x % 10);
                        x /= 10;
                    } while (x);
                    while (k)
                        *q++ = t[--k];
                } else {
                    *q++ = '0';
                    *q++ = (char)('0' + x);
                }
                expsize = (int)(q - expstr);
                size = expsize + ndig;
                if (ndig > 1 || (flags & ALT))
                    ++size;
            } else if (ch == 'f') {
                if (expt > 0) {
                    size = expt;
                    if (prec || (flags & ALT))
                        size += prec + 1;
                } else {
                    /* "0.X": two more than the precision, also when only
                     * "0" is printed (precision 0): the count and the
                     * padding then take one character more than is
                     * stored (the tests) */
                    size = prec + 2;
                }
            } else if (expt >= ndig) {
                size = expt;
                if (flags & ALT)
                    ++size;
            } else {
                size = ndig + (expt > 0 ? 1 : 2 - expt);
            }
            break;
        }
        case 'n': {
            CRT_UNSEEN("printf", "conversion %%n");
            void *a = (void *)(uintptr_t)(u32)crt_va_int(va);
            u32 c = s->count;
            if ((flags & LONGINT) && LONG_BITS == 64) {
                *(u64 *)a = c;
            } else if (flags & SHORTINT) {
                *(u16 *)a = (u16)c;
            } else {
                *(u32 *)a = c;
            }
            continue;
        }
        case 'O':
            flags |= LONGINT;
            /* fall through */
        case 'o':
            base = 8;
            goto unsign;
        case 'p':
            uq = (u64)(s64)(s32)(u32)crt_va_int(va);   /* a pointer widened as the EE does: signed */
            base = 16;
            xdigs = "0123456789abcdef";
            flags |= HEXPREFIX;
            ch = 'x';
            goto nosign;
        case 's':
            gcp = (const char *)(uintptr_t)(u32)crt_va_int(va);
            sign = 0;
            if (!gcp) {
                cp = "(null)";
                size = 6;
                if (prec >= 0 && prec < size)
                    size = prec;
                break;
            }
            gstr = 1;
            size = 0;
            if (prec >= 0) {
                while (size < prec && gcp[size])
                    size++;
            } else {
                while (gcp[size])
                    size++;
            }
            break;
        case 'U':
            flags |= LONGINT;
            /* fall through */
        case 'u':
            base = 10;
            goto unsign;
        case 'X':
            xdigs = "0123456789ABCDEF";
            goto hex;
        case 'x':
            xdigs = "0123456789abcdef";
        hex:
            base = 16;
        unsign: {
            u64 x = crt_va_int(va);
            if (flags & LONGINT)
                uq = LONG_BITS == 64 ? x : (u32)x;
            else if (flags & SHORTINT)
                uq = (u16)x;
            else
                uq = (u32)x;
            if (base == 16 && (flags & ALT) && uq != 0)
                flags |= HEXPREFIX;
        }
        nosign:
            sign = 0;
        number:
            if ((dprec = prec) >= 0)
                flags &= ~ZEROPAD;
            {
                char *e = buf + sizeof(buf), *q = e;
                if (uq != 0 || prec != 0) {
                    do {
                        *--q = xdigs[uq % (u64)base];
                        uq /= (u64)base;
                    } while (uq);
                    if (base == 8 && (flags & ALT) && *q != '0')
                        *--q = '0';
                }
                cp = q;
                size = (int)(e - q);
            }
            break;
        default:
            if (ch == 0) {
                p--;   /* the end of the format */
                goto done;
            }
            buf[0] = (char)ch;
            cp = buf;
            size = 1;
            sign = 0;
            break;
        }

        {
            int realsz = dprec > size ? dprec : size;
            if (sign)
                realsz++;
            else if (flags & HEXPREFIX)
                realsz += 2;
            if ((flags & (LADJUST | ZEROPAD)) == 0)
                put_pad(s, width - realsz, BLANKS);
            if (sign) {
                put_host(s, &sign, 1);
            } else if (flags & HEXPREFIX) {
                ox[0] = '0';
                ox[1] = (char)ch;
                put_host(s, ox, 2);
            }
            if ((flags & (LADJUST | ZEROPAD)) == ZEROPAD)
                put_pad(s, width - realsz, ZEROES);
            put_pad(s, dprec - size, ZEROES);
            if (!isfloat) {
                if (gstr)
                    put_host(s, gcp, (u32)size);
                else
                    put_host(s, cp, (u32)size);
            } else if (ch >= 'f') {   /* 'f' or 'g' */
                if (!fmag) {
                    put_host(s, "0", 1);
                    if (expt < ndig || (flags & ALT)) {
                        put_host(s, ".", 1);
                        put_pad(s, ndig - 1, ZEROES);
                    }
                } else if (expt <= 0) {
                    put_host(s, "0", 1);
                    if (expt || ndig) {   /* a value that rounds to 0 at precision 0: "0" */
                        put_host(s, ".", 1);
                        put_pad(s, -expt, ZEROES);
                        put_host(s, dig, (u32)ndig);
                    }
                } else if (expt >= ndig) {
                    put_host(s, dig, (u32)ndig);
                    put_pad(s, expt - ndig, ZEROES);
                    if (flags & ALT)
                        put_host(s, ".", 1);
                } else {
                    put_host(s, dig, (u32)expt);
                    put_host(s, ".", 1);
                    put_host(s, dig + expt, (u32)(ndig - expt));
                }
            } else {   /* 'e' or 'E' */
                if (ndig > 1 || (flags & ALT)) {
                    ox[0] = dig[0];
                    ox[1] = '.';
                    put_host(s, ox, 2);
                    if (fmag)
                        put_host(s, dig + 1, (u32)(ndig - 1));
                    else
                        put_pad(s, ndig - 1, ZEROES);
                } else {
                    put_host(s, dig, 1);
                }
                put_host(s, expstr, (u32)expsize);
            }
            if (flags & LADJUST)
                put_pad(s, width - realsz, BLANKS);
            /* the count: the field's width or its computed size, as the
             * C library counts (not what was stored: "0.X" above) */
            s->count += (u32)(width > realsz ? width : realsz);
            sk_flush(s);   /* the conversion's pieces, before the next one */
        }
    }
done:
    sk_flush(s);
    return s->count;
}

/* -------------------------------------------------------- the functions */
static s32 fmt_to_buf(char *buf, u32 size, const char *fmt, va_list ap)
{
    Sink s;
    __builtin_memset(&s, 0, sizeof(s));
    s.dst = buf;
    s.room = size ? size - 1 : 0;
    va_list ap2;
    va_copy(ap2, ap);
    CrtVa va = {&ap2};
    u32 n = crt_vformat(&s, (const u8 *)fmt, &va);
    va_end(ap2);
    if (size) {
        s.room = 1;
        sk_store(&s, "", 1); /* the NUL */
    }
    return (s32)n;
}

/* The taps (crt_tap.h): the variable arguments are not described (the
 * format string is, and the text written). */
s32 ps2_sprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    s32 n = fmt_to_buf(buf, 0xFFFFFFFFu, fmt, ap);
    va_end(ap);
    return n;
}

s32 vsprintf(char *buf, const char *fmt, va_list ap)
{
    return fmt_to_buf(buf, 0xFFFFFFFFu, fmt, ap);
}

s32 vsnprintf(char *buf, s32 size, const char *fmt, va_list ap)
{
    return fmt_to_buf(buf, (u32)size, fmt, ap);
}

/* printf: the text formatted as sprintf's, written to stdout's stream
 * (crt_stdout.c): the allocator at the first call and the write at each
 * newline, as the C library's printf, so the waits on the IOP underneath
 * happen as before. Returns the count, or EOF once a write has failed (on
 * the original game the console's file doesn't open, so every write fails).
 * Capcom's code never reads the value (the boot's "Setup ..." lines, the
 * hunt's debugging lines). */
static s32 Printf(const char *fmt, va_list *ap)
{
    Sink s;
    __builtin_memset(&s, 0, sizeof(s));
    s.console = 1;
    s.room = 0xFFFFFFFEu;
    crt_so_begin();
    CrtVa va = {ap};
    u32 n = crt_vformat(&s, (const u8 *)fmt, &va);
    return crt_so_end() ? -1 : (s32)n;
}

s32 ps2_printf(const char *fmt, ...)
{
    va_list ap;
    s32 n;
    va_start(ap, fmt);
    n = Printf(fmt, &ap);
    va_end(ap);
    return n;
}
