/* The C library's memory and string functions on PC.
 *
 * Re-hosted from the earlier port's libc.c, which is written from the C standard (ISO/IEC 9899, 7.21 and
 * 7.20) and from black-box tests of the original game's functions
 * (the earlier port's tests): where the standard leaves the result open (the value
 * a comparison returns, overlapping copies), the tests settled what the game
 * sees. The behaviour is the original's; the guest memory is native memory.
 *
 * The loops are written out and not turned into calls of each other: the
 * file is compiled without builtins (the core's flags), so the compiler
 * recognises no copy idiom in them.
 */
#include "ps2crt.h"
#include "crt_tap.h"

/* Forward copy, byte by byte in effect: an overlap with dst above src
 * repeats the source's first bytes, as a forward copy does. */
static void *CrtImpl_memcpy(void *dst, const void *src, u32 n)
{
    u8 *d = dst;
    const u8 *s = src;
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memcpy(void *dst, const void *src, u32 n)
{
    return CrtImpl_memcpy(dst, src, n);
}

/* Backward copy when dst is above src and the blocks overlap. */
static void *CrtImpl_memmove(void *dst, const void *src, u32 n)
{
    u8 *d = dst;
    const u8 *s = src;
    if ((u8 *)dst > s && (u32)((u8 *)dst - s) < n) {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    } else {
        while (n--)
            *d++ = *s++;
    }
    return dst;
}

void *memmove(void *dst, const void *src, u32 n)
{
    return CrtImpl_memmove(dst, src, n);
}

static void *CrtImpl_memset(void *p, s32 c, u32 n)
{
    u8 *d = p;
    while (n--)
        *d++ = (u8)c;
    return p;
}

void *memset(void *p, s32 c, u32 n)
{
    return CrtImpl_memset(p, c, n);
}

/* The comparisons return the difference of the first two bytes that
 * differ, as unsigned chars (the tests: not just its sign). */
static s32 CrtImpl_memcmp(const void *a, const void *b, u32 n)
{
    const u8 *x = a, *y = b;
    for (; n; n--, x++, y++) {
        if (*x != *y)
            return (s32)*x - (s32)*y;
    }
    return 0;
}

s32 memcmp(const void *a, const void *b, u32 n)
{
    return CrtImpl_memcmp(a, b, n);
}

static u32 CrtImpl_strlen(const char *s)
{
    u32 n = 0;
    while (s[n])
        n++;
    return n;
}

u32 strlen(const char *s)
{
    return CrtImpl_strlen(s);
}

static char *CrtImpl_strcpy(char *dst, const char *src)
{
    u32 i = 0;
    for (;; i++) {
        u8 c = (u8)src[i];
        dst[i] = (char)c;
        if (!c)
            break;
    }
    return dst;
}

char *strcpy(char *dst, const char *src)
{
    return CrtImpl_strcpy(dst, src);
}

/* At most n bytes; a source shorter than n is padded with zeros to n. */
static char *CrtImpl_strncpy(char *dst, const char *src, u32 n)
{
    u32 i = 0;
    for (; i < n; i++) {
        u8 c = (u8)src[i];
        dst[i] = (char)c;
        if (!c)
            break;
    }
    for (; i < n; i++)
        dst[i] = 0;
    return dst;
}

char *strncpy(char *dst, const char *src, u32 n)
{
    return CrtImpl_strncpy(dst, src, n);
}

static char *CrtImpl_strcat(char *dst, const char *src)
{
    char *e = dst;
    while (*e)
        e++;
    for (u32 i = 0;; i++) {
        u8 c = (u8)src[i];
        e[i] = (char)c;
        if (!c)
            break;
    }
    return dst;
}

char *strcat(char *dst, const char *src)
{
    return CrtImpl_strcat(dst, src);
}

/* At most n bytes of src, then a terminating zero. */
static char *CrtImpl_strncat(char *dst, const char *src, u32 n)
{
    char *e = dst;
    while (*e)
        e++;
    u32 i = 0;
    for (; i < n; i++) {
        u8 c = (u8)src[i];
        if (!c)
            break;
        e[i] = (char)c;
    }
    e[i] = 0;
    return dst;
}

char *strncat(char *dst, const char *src, u32 n)
{
    return CrtImpl_strncat(dst, src, n);
}

static s32 CrtImpl_strcmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        u8 x = (u8)*a, y = (u8)*b;
        if (x != y || !x)
            return (s32)x - (s32)y;
    }
}

s32 strcmp(const char *a, const char *b)
{
    return CrtImpl_strcmp(a, b);
}

static s32 CrtImpl_strncmp(const char *a, const char *b, s32 n)
{
    for (u32 k = (u32)n; k; k--, a++, b++) {
        u8 x = (u8)*a, y = (u8)*b;
        if (x != y || !x)
            return (s32)x - (s32)y;
    }
    return 0;
}

s32 strncmp(const char *a, const char *b, s32 n)
{
    return CrtImpl_strncmp(a, b, n);
}

/* The first c (as a char) in s, its terminator included; 0 if none. */
static char *CrtImpl_strchr(const char *s, s32 c)
{
    u8 ch = (u8)c;
    for (;; s++) {
        u8 x = (u8)*s;
        if (x == ch)
            return (char *)s;
        if (!x)
            break;
    }
    return NULL;
}

char *strchr(const char *s, s32 c)
{
    return CrtImpl_strchr(s, c);
}

/* The last c in s, its terminator included; 0 if none. */
static char *CrtImpl_strrchr(const char *s, s32 c)
{
    const char *last = NULL;
    u8 ch = (u8)c;
    for (;; s++) {
        u8 x = (u8)*s;
        if (x == ch)
            last = s;
        if (!x)
            break;
    }
    return (char *)last;
}

char *strrchr(const char *s, s32 c)
{
    return CrtImpl_strrchr(s, c);
}

/* The first occurrence of the string n in h (h itself for an empty n). */
static char *CrtImpl_strstr(const char *h, const char *n)
{
    for (;; h++) {
        u32 i = 0;
        for (;; i++) {
            u8 y = (u8)n[i];
            if (!y)
                return (char *)h;
            if ((u8)h[i] != y)
                break;
        }
        if (!*h)
            break;
    }
    return NULL;
}

char *strstr(const char *h, const char *n)
{
    return CrtImpl_strstr(h, n);
}

/* In place: the letters a-z to A-Z, every other byte as it is. */
static char *CrtImpl_strupr(char *s)
{
    for (char *p = s;; p++) {
        u8 x = (u8)*p;
        if (!x)
            break;
        if (x >= 'a' && x <= 'z')
            *p = (char)(x - 'a' + 'A');
        else if (x >= 0x80)
            CRT_UNSEEN("strupr", "byte %02X", x); /* the original changes 21 of them */
    }
    return s;
}

char *strupr(char *s)
{
    return CrtImpl_strupr(s);
}

static s32 CrtImpl_abs(s32 x)
{
    return (s32)(x < 0 ? -(u32)x : (u32)x);
}

s32 abs(s32 x)
{
    return CrtImpl_abs(x);
}

/* White space, an optional sign, decimal digits. The tests: the value is
 * taken in 64 bits, held at the 64-bit limits when it overflows
 * (2^63 - 1, -2^63), and returned as its low 32 bits: 2^32 + 5 gives 5, a
 * 20-digit number -1, a 20-digit negative one 0. */
static s32 CrtImpl_atoi(const char *s)
{
    u8 c = (u8)*s;
    while (c == 0x20 || (c >= 0x09 && c <= 0x0D)) /* space, tab, LF, VT, FF, CR */
        c = (u8)*++s;
    int neg = 0;
    if (c == '-' || c == '+') {
        neg = c == '-';
        c = (u8)*++s;
    }
    u64 v = 0;
    int over = 0;
    while (c >= '0' && c <= '9') {
        u32 d = (u32)(c - '0');
        if (v > (0xFFFFFFFFFFFFFFFFull - d) / 10)
            over = 1;
        else
            v = v * 10 + d;
        c = (u8)*++s;
    }
    u64 r;
    if (neg)
        r = over || v > 0x8000000000000000ull ? 0x8000000000000000ull : 0 - v;
    else
        r = over || v > 0x7FFFFFFFFFFFFFFFull ? 0x7FFFFFFFFFFFFFFFull : v;
    if (over || v > 0x7FFFFFFFFFFFFFFFull + (u64)neg)
        CRT_UNSEEN("atoi", "a number out of the 64-bit range (the original writes errno)");
    return (s32)(u32)r;
}

s32 atoi(const char *s)
{
    return CrtImpl_atoi(s);
}

/* ------------------------------------------------- rand and strtok */
/* These two keep a state between calls: the original ones in the C
 * library's reent (its own memory), here in ps2crt.
 * Capcom never calls srand. */

/* rand (7.20.2.1): a 64-bit linear congruential generator from the seed 1,
 * next = next * 6364136223846793005 + 1 (Knuth's multiplier, The Art of
 * Computer Programming, vol. 2, 3rd ed., p. 106), the result bits 32-62
 * of next (RAND_MAX 0x7FFFFFFF): the sequence the tests observe (not the
 * C standard's example generator). */
static u64 g_rand_next = 1;
static s32 CrtImpl_rand(void)
{
    g_rand_next = g_rand_next * 6364136223846793005ull + 1;
    return (s32)((u32)(g_rand_next >> 32) & 0x7FFFFFFFu);
}

s32 rand(void)
{
    return CrtImpl_rand();
}

/* For the harness and for save states of the host: the generator's state. */
u64 crt_rand_state(void) { return g_rand_next; }
void crt_rand_set_state(u64 s) { g_rand_next = s; }

/* strtok (7.21.5.8): the next token of the string (or of the saved
 * position when s is NULL), its end overwritten by a NUL; NULL when no
 * token is left. */
static char *g_strtok_next;
static int strtok_delim(const char *d, u8 c)
{
    for (;; d++) {
        u8 x = (u8)*d;
        if (!x)
            return 0;
        if (x == c)
            return 1;
    }
}

static char *CrtImpl_strtok(char *s, const char *d)
{
    if (!s)
        s = g_strtok_next;
    if (!s)
        return NULL;
    u8 c;
    while ((c = (u8)*s) && strtok_delim(d, c)) /* the delimiters before the token */
        s++;
    if (!c) {
        g_strtok_next = NULL;
        return NULL;
    }
    char *tok = s;
    for (;; s++) {
        c = (u8)*s;
        if (!c) {
            g_strtok_next = NULL;
            break;
        }
        if (strtok_delim(d, c)) {
            *s = 0;
            g_strtok_next = s + 1;
            break;
        }
    }
    return tok;
}

char *strtok(char *s, const char *d)
{
    return CrtImpl_strtok(s, d);
}
