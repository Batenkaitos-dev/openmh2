/* The 128-bit float (IEEE binary128, __float128) routines that glibc's static code for the 32-bit x86 target calls and that compiler-rt's
   builtins do not provide there (they need a 128-bit integer type, which a 32-bit target lacks; GCC's libgcc has them).

   The callers are in glibc's printf and strtof128 code for a _Float128 argument or result, which the game never passes: its formats
   are the usual ones. So:
   - the comparisons (__unordtf2, __eqtf2 and the others) are written out in full, with libgcc's results: a comparison with a NaN
     is "unordered" (__unordtf2 true; the others return the value their callers treat as false: 1 for < and <=, -1 for > and >=,
     nonzero for ==, and for != the other way round);
   - the arithmetic (__addtf3, __multf3) stops the program: a call would be a bug, and a wrong number must not go on silently.
   Public domain (CC0). */
#include <stdint.h>

typedef struct {
    uint32_t sign;
    uint32_t mag[4];   /* the magnitude, most significant word first */
    int nan;
    int zero;
} f128;

static f128 unpack(__float128 x)
{
    uint32_t w[4];
    f128 r;
    __builtin_memcpy(w, &x, 16);   /* little endian: w[3] holds the sign, the exponent and the top of the fraction */
    r.sign = w[3] >> 31;
    r.mag[0] = w[3] & 0x7fffffffu;
    r.mag[1] = w[2];
    r.mag[2] = w[1];
    r.mag[3] = w[0];
    r.nan = ((w[3] >> 16) & 0x7fff) == 0x7fff && ((w[3] & 0xffff) | w[2] | w[1] | w[0]) != 0;
    r.zero = (r.mag[0] | r.mag[1] | r.mag[2] | r.mag[3]) == 0;
    return r;
}

/* -1, 0 or 1 for a < b, a == b, a > b; 2 if either is a NaN */
static int cmp(__float128 a, __float128 b)
{
    f128 x = unpack(a), y = unpack(b);
    int i, c = 0;
    if (x.nan || y.nan)
        return 2;
    if (x.zero && y.zero)
        return 0;
    if (x.sign != y.sign)
        return x.sign ? -1 : 1;
    for (i = 0; i < 4 && !c; i++)
        c = x.mag[i] < y.mag[i] ? -1 : x.mag[i] > y.mag[i] ? 1 : 0;
    return x.sign ? -c : c;
}

int __unordtf2(__float128 a, __float128 b) { return cmp(a, b) == 2; }
int __eqtf2(__float128 a, __float128 b) { int c = cmp(a, b); return c == 2 ? 1 : c; }
int __netf2(__float128 a, __float128 b) { int c = cmp(a, b); return c == 2 ? 1 : c; }
int __lttf2(__float128 a, __float128 b) { int c = cmp(a, b); return c == 2 ? 1 : c; }
int __letf2(__float128 a, __float128 b) { int c = cmp(a, b); return c == 2 ? 1 : c; }
int __gttf2(__float128 a, __float128 b) { int c = cmp(a, b); return c == 2 ? -1 : c; }
int __getf2(__float128 a, __float128 b) { int c = cmp(a, b); return c == 2 ? -1 : c; }

extern void abort(void);
__float128 __addtf3(__float128 a, __float128 b) { (void)a; (void)b; abort(); }
__float128 __multf3(__float128 a, __float128 b) { (void)a; (void)b; abort(); }
