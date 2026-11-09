/* gCtypeTable: the class bits of newlib's ctype table, which the PS2 boot executable links and the
 * game reads directly (include/libc_ctype.h: gCtypeTable[1 + c], entry 0 for EOF).
 *
 * Written from newlib's definition of the table (the C locale: libc/ctype/ctype_.c), whose bits are
 * _U 01 (upper case), _L 02 (lower case), _N 04 (digit), _S 010 (space), _P 020 (punctuation),
 * _C 040 (control), _X 0100 (hex digit) and _B 0200 (the blank, which newlib marks on the space
 * only); the bytes 128..255 are in no class. It has the 257 entries of the symbol's data. The
 * bytes were then checked against the original's RAM (the ps2fp tests' ctype check: a
 * verification, not a source). */
#define CU 0x01
#define CL 0x02
#define CN 0x04
#define CS 0x08
#define CP 0x10
#define CC 0x20
#define CX 0x40
#define CB 0x80

/* 16-aligned as on the PS2 (0x1BD420): ctest data_align. */
signed char gCtypeTable[1 + 256] __attribute__((aligned(16))) = {
    [0] = 0,                         /* EOF */
    [1 + 0 ... 1 + 8] = CC,          /* the control characters */
    [1 + 9 ... 1 + 13] = CC | CS,    /* tab, newline, vertical tab, form feed, return */
    [1 + 14 ... 1 + 31] = CC,
    [1 + ' '] = CS | CB,
    [1 + '!' ... 1 + '/'] = CP,
    [1 + '0' ... 1 + '9'] = CN,
    [1 + ':' ... 1 + '@'] = CP,
    [1 + 'A' ... 1 + 'F'] = CU | CX,
    [1 + 'G' ... 1 + 'Z'] = CU,
    [1 + '[' ... 1 + '`'] = CP,
    [1 + 'a' ... 1 + 'f'] = CL | CX,
    [1 + 'g' ... 1 + 'z'] = CL,
    [1 + '{' ... 1 + '~'] = CP,
    [1 + 127] = CC,
};
