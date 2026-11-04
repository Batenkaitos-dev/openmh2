/* Structs whose layout depends on the host ABI's rules, not on the game's data: tools/pc/layout.py
 * compares them with MWCC's layout like the game's own records (pc/README.md, "Layout").
 * The game's bit-fields are whole 32-bit words today (dma.h, model.h, sprite_queue.h, kbd.c); this
 * probe is what would notice a mixed one, and a target whose bit-field rules differ (mingw's
 * default is the Microsoft layout, which the Windows build turns off with -mno-ms-bitfields). */
#ifndef PC_LAYOUT_PROBE_H
#define PC_LAYOUT_PROBE_H

typedef struct ProbeBitsMixed_s {
    unsigned char  a : 4;
    unsigned short b : 4;
    int            c : 8;
} ProbeBitsMixed;

typedef struct ProbeBitsSpill_s {
    unsigned char  a : 3;
    unsigned int   b : 30;
    unsigned char  c : 3;
    unsigned char  d;
} ProbeBitsSpill;

typedef struct ProbeBitsInterleaved_s {
    unsigned int   a : 5;
    unsigned short b;
    unsigned char  c : 1;
    unsigned int   d : 20;
} ProbeBitsInterleaved;

typedef struct ProbeEightByte_s {
    char               a;
    double             d;
    long long          l;
    char               b;
    unsigned long long u;
} ProbeEightByte;

typedef struct ProbeUnionBits_s {
    char  a;
    union {
        int   i;
        float f;
        short s[3];
    } u;
    unsigned char b : 2;
    unsigned char c : 6;
} ProbeUnionBits;

#endif
