/*
 * Character code helpers (Shift-JIS and JIS X 0208 tests and
 * conversions) and a pool of 0x18-byte nodes for the text input.
 */
#include "common.h"
#include "em.h"
#include "conv.h"

/* prototypes */
void CandLine_Select(s32, s32);
ConvOut *Out_Nth(ConvOut *o, s32 n);
void CandLine_Build(void);
s32 CandLine_CountFit(ConvOut *);
s32 Out_ChainWidth(ConvOut *o);
ConvOut *Out_NextChain(ConvOut *o);
extern s32 gCandLineEnd;
extern s32 gCandPageSel;
extern s32 gCandPageCount;
extern ConvOut *gCandPageFirst;
/* end of prototypes */

typedef struct Node18 {
    struct Node18 *next;        /* 0x00: free list */
    u8 pad4[0x18 - 0x4];
} Node18;

/* The same nodes chained through +0x14 (InNode_FreeChain). */
typedef struct EntryNode {
    u8 pad0[8];
    u64 entry;              /* 0x08 */
    u8 pad10[4];
    struct EntryNode *next; /* 0x14 */
} EntryNode;

/* The link fields of the conversion's nodes (Suffix, Cand and
 * Choice, Out) as the free functions see them. */
typedef struct SuffixLink {
    u8 pad0[8];
    struct SuffixLink *next;    /* 0x08 */
} SuffixLink;

typedef struct CandLink {
    u8 pad0[0xC];
    struct CandLink *next;      /* 0x0C: Cand and Choice */
} CandLink;

typedef struct OutLink {
    u8 pad0[0x10];
    struct OutLink *next;       /* 0x10 */
} OutLink;

extern Node18 *gInNodeFree;
extern Node18 gInNodePool[3000];
extern s32 D_00404DB0;

/* unit's own declaration: declared as this file's callers use it; with the definition's s32 (u16 c), Jis_ToHalfByte compiles differently */
s32 Jis_LookupHalfSymbol(s32 c);
s32 Sjis_IsLead(s32 c);
s32 Sjis_CellWidth(u16 c);
u16 Jis_ToSjis(u16 c);
#if PLATFORM_PC
/* the definition's s32: the caller passes the entry's low word (as in unk_2FF510.c: Slot_Release tests the
 * EE's whole register, and an entry is Slot_Acquire's s32 sign-extended, or -1) */
s32 Conv_ReleaseSlot(s32 i);
#define Conv_ReleaseSlot(entry) Conv_ReleaseSlot((s32)(entry))
#else
/* unit's own declaration: declared as this file's callers use it; with the definition's s32 (s32 arg0), InNode_FreeChain compiles differently */
s32 Conv_ReleaseSlot(u64 entry);
#endif
s32 Jis_IsKatakana(s32 c, s32 a);
Node18 *InNode_Alloc(void);
void InNode_Free(Node18 *n);

/* How many chained entries from first (at most 9, at least 1) fit in
 * gCandLineEnd - 10, each Out_ChainWidth wide plus 4. */
s32 CandLine_CountFit(ConvOut *first)
{
    s32 count;
    s32 total;
    s32 limit;
    ConvOut *o;

    if (first == NULL) {
        return 0;
    }
    o = first;
    total = 0;
    count = 0;
    limit = gCandLineEnd - 10;
    while (count < 9 && o != NULL) {
        total += Out_ChainWidth(o) + 4;
        if (total > limit) {
            break;
        }
        count++;
        o = Out_NextChain(o);
    }
    if (count == 0) {
        return 1;
    }
    return count;
}

s32 CandLine_NextPage(s32 arg0, s32 arg1)
{
    s32 n;
    ConvOut *o;

    o = gCandPageFirst;
    gCandPageFirst = Out_Nth(o, gCandPageCount);
    n = CandLine_CountFit(gCandPageFirst);
    if (n == 0) {
        if (arg1 == 0) {
            gCandPageFirst = o;
            return 0;
        }
        CandLine_Select(0, 0);
        goto block_7;
    }
    gCandPageCount = n;
block_7:
    gCandPageSel = 0;
    if (arg0 == 1) {
        CandLine_Build();
    }
    return 1;
}

s32 CandLine_PrevPage(s32 mode, s32 keep)
{
    ConvOut *cur = gCandPageFirst;
    s32 pos = gCandPageCount;

    CandLine_Select(0, 0);
    if (cur == gCandPageFirst) {
        if (keep == 0) {
            gCandPageFirst = cur;
            gCandPageCount = pos;
            gCandPageSel = 0;
            return 0;
        }
        cur = NULL;
    }
    for (;;) {
        if (Out_Nth(gCandPageFirst, gCandPageCount) == cur) {
            break;
        }
        CandLine_NextPage(0, 1);
    }
    if (mode == 1) {
        CandLine_Build();
    }
    return 1;
}

static inline s32 Jis_IsByte(u8 c)
{
    s32 r = 0;

    if (c >= 0x21 && c <= 0x7E) {
        r = 1;
    }
    return r;
}

/* A JIS code: both bytes in 0x21..0x7E. */
s32 Jis_IsCode(u16 c)
{
    u8 hi = c >> 8;
    u8 lo = c;
    s32 r = 0;

    if (Jis_IsByte(hi) && Jis_IsByte(lo)) {
        r = 1;
    }
    return r;
}

/* A Shift-JIS lead byte. */
s32 Sjis_IsLead(s32 c)
{
    c &= 0xFF;
    if (c < 0x81 || c >= 0xFD || (c >= 0xA0 && c < 0xE0)) {
        return 0;
    }
    return 1;
}

/* A two-byte Shift-JIS character. */
s32 Sjis_IsCode(u16 c)
{
    u8 lo = c;

    if (Sjis_IsLead(c >> 8 & 0xFF) == 0) {
        return 0;
    }
    if (lo < 0x40 || lo >= 0xFD || lo == 0x7F) {
        return 0;
    }
    return 1;
}

/* Puts c into dst as font cells with palette n (top four bits): two
 * cells (return 2) for a Shift-JIS or JIS code, else one. */
s32 Text_CharToCells(u16 *dst, u16 c, s32 n)
{
    u16 attr = (u16)n << 12;

    if (Sjis_IsCode(c)) {
        if (Sjis_CellWidth(c) == 1) {
            attr |= 0x300;
        } else {
            attr |= 0x700;
        }
    } else if (Jis_IsCode(c)) {
        attr |= 0x500;
        if (c == 0x2474) {
            c = 0x2574;
        }
        if (c == 0x2475) {
            c = 0x2575;
        }
        if (c == 0x2476) {
            c = 0x2576;
        }
    } else {
        dst[0] = attr | (c & 0xFF);
        return 1;
    }
    dst[0] = attr | (c >> 8);
    dst[1] = (attr + 0x100) | (c & 0xFF);
    return 2;
}

void Cells_SetPalette(u16 *p, s32 n, u16 v)
{
    u16 hi = v << 12;

    while (n-- != 0) {
        *p = (*p & 0xFFF) | hi;
        p++;
    }
}

s32 Sjis_CellWidth(u16 c)
{
    switch (c & 0xFF00) {
    case 0x8000:
    case 0x8500:
        return 1;
    case 0x8600:
        if ((c & 0xFF) < 0x9E) {
            return 1;
        }
    default:
        return 2;
    }
}

/* Converts a Shift-JIS string to font cells; returns the cell count. */
s32 Text_SjisToCells(u16 *dst, u8 *src, s32 n)
{
    u16 *p = dst;

    while (*src != 0) {
        u8 c = *src;

        if (c < 0x80 || (c >= 0xA0 && c < 0xE0)) {
            p += Text_CharToCells(p, *src++, n);
        } else {
            p += Text_CharToCells(p, (c << 8) | src[1], n);
            src += 2;
        }
    }
    return p - dst;
}

s32 Jis_ToHalfByte(s32 code)
{
    s32 c;
    s32 hi;

    c = code & 0xFFFF;
    if ((c > 32) && (c < 0x7F)) {
        return 0;
    }
    hi = c & 0xFF00;
    switch (hi) {
    case 0x2300:
        return c & 0x7F;
    case 0x2400:
        return ((c & 0x7F) | 0x80) & 0xFF;
    case 0x2500:
        return 0;
    default:
        return Jis_LookupHalfSymbol(code);
    }
}

s32 Jis_IsKatakana(s32 c, s32 a)
{
    if (a != 0 && (u16)c == 0x213C) {
        return 1;
    }
    if (((u16)c & 0xFF00) == 0x2500) {
        return 1;
    }
    return 0;
}

s32 Jis_IsKanji(u16 c)
{
    return c >= 0x3020;
}

s32 Sjis_CellWidth(u16 c);
s32 Jis_IsKatakana(s32 c, s32 a);

s32 Jis_IsNonKanjiNotKatakana(s32 code)
{
    s32 c;

    c = code & 0xFFFF;
    if ((c >= 0x2120) && (c < 0x3020)) {
        return !Jis_IsKatakana(code, 0);
    }
    return 0;
}

/* Font cells back to a Shift-JIS string (the inverse of Text_SjisToCells);
 * returns its length. */
s32 Text_CellsToSjis(u16 *src, u8 *dst, s32 n)
{
    u16 c;
    s32 i = 0;
    u8 *p = dst;

    while (i < n) {
        u16 v = *src;
        s32 pal = (v >> 8) & 0xF;

        if (pal == 0) {
            *p = v;
            src++;
            p++;
            i++;
        } else if (pal & 1) {
            s32 hi = (u16)(v << 8);
            u16 w = *++src;

            i++;
            if (((w >> 8) & 0xF) == pal + 1) {
                src++;
                i++;
                c = hi | (w & 0xFF);
                if (Jis_IsCode(c)) {
                    c = Jis_ToSjis(c);
                }
                p[0] = c >> 8;
                p[1] = c;
                p += 2;
            }
        }
    }
    *p = 0;
    return p - dst;
}

/* Shift-JIS to JIS X 0208 (0 when the trail byte is not valid). */
u16 Sjis_ToJis(u16 c)
{
    u8 hi = c >> 8;
    u8 lo = c;

    if (lo < 0x40 || lo >= 0xFD || lo == 0x7F) {
        return 0;
    }
    if (hi >= 0xE0) {
        hi -= 0x40;
    }
    hi = (hi - 0x81) * 2 + 0x21;
    if (lo >= 0x9F) {
        hi++;
        lo -= 0x7E;
    } else if (lo >= 0x80) {
        lo -= 0x20;
    } else {
        lo -= 0x1F;
    }
    return (hi << 8) | lo;
}

/* JIS X 0208 to Shift-JIS (0 outside 0x2121..0x7E7E). */
u16 Jis_ToSjis(u16 c)
{
    u32 hi = (u8)(c >> 8);
    u8 lo = c;
    u8 h;

    if (c < 0x2121 || c > 0x7E7E) {
        return 0;
    }
    if (lo < 0x21 || lo >= 0x7F) {
        return 0;
    }
    lo += 0x1F;
    if ((u8)hi & 1) {
        h = hi >> 1;
    } else {
        h = hi >> 1;
        lo += 0x5E;
        h--;
    }
    if (lo >= 0x7F) {
        lo++;
    }
    if (h < 0x2F) {
        h += 0x71;
    } else {
        h += 0xB1;
    }
    return (h << 8) | lo;
}

/* Links the node pool into the free list. */
void InNode_InitPool(void)
{
    Node18 *n;

    gInNodeFree = gInNodePool;
    for (n = gInNodePool; n < &gInNodePool[3000 - 1]; n++) {
        n->next = n + 1;
    }
    n->next = NULL;
    D_00404DB0 = 0;
}

Node18 *InNode_Alloc(void)
{
    Node18 *n = gInNodeFree;

    if (n == NULL) {
        return NULL;
    }
    gInNodeFree = n->next;
    return n;
}

void InNode_Free(Node18 *n)
{
    if (n != NULL) {
        n->next = gInNodeFree;
        gInNodeFree = n;
    }
}

Node18 *Seg_Alloc(void)
{
    Node18 *n = InNode_Alloc();

    if (n != NULL) {
        return n;
    }
    return NULL;
}

Node18 *Cand_Alloc(void)
{
    Node18 *n = InNode_Alloc();

    if (n != NULL) {
        return n;
    }
    return NULL;
}

Node18 *Suffix_Alloc(void)
{
    Node18 *n = InNode_Alloc();

    if (n != NULL) {
        return n;
    }
    return NULL;
}

Node18 *Out_Alloc(void)
{
    Node18 *n = InNode_Alloc();

    if (n != NULL) {
        return n;
    }
    return NULL;
}

Node18 *Choice_Alloc(void)
{
    Node18 *n = InNode_Alloc();

    if (n != NULL) {
        return n;
    }
    return NULL;
}

void Suffix_FreeChain(SuffixLink *n)
{
    SuffixLink *next;

    while (n != NULL) {
        next = n->next;
        InNode_Free((Node18 *)n);
        n = next;
    }
}

/* Frees a chain of nodes, calling Conv_ReleaseSlot once per run of equal
 * entries (not for -1). */
void InNode_FreeChain(EntryNode *n)
{
    EntryNode *next;
    u64 prev = -1;

    while (n != NULL) {
        next = n->next;
        if (prev == -1 || n->entry != prev) {
            if (n->entry != -1) {
                Conv_ReleaseSlot(n->entry);
            }
        }
        prev = n->entry;
        InNode_Free((Node18 *)n);
        n = next;
    }
}

void Cand_FreeChain(CandLink *n)
{
    CandLink *next;

    while (n != NULL) {
        next = n->next;
        InNode_Free((Node18 *)n);
        n = next;
    }
}

void Out_FreeChain(OutLink *n)
{
    OutLink *next;

    while (n != NULL) {
        next = n->next;
        InNode_Free((Node18 *)n);
        n = next;
    }
}

void Choice_FreeChain(CandLink *n)
{
    CandLink *next;

    while (n != NULL) {
        next = n->next;
        InNode_Free((Node18 *)n);
        n = next;
    }
}

/* This file's initialised data. */
s32 D_00404DB0 = {
#include "sub_main/00404DB0.inc"
};
