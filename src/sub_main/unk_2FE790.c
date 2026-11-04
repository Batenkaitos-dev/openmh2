/*
 * A dictionary file with a page cache (probably the text input's
 * conversion dictionary): 4-byte keys at gDictKeyBuf searched by
 * bisection, 0x400-byte pages at 0x3400 + page * 0x400 cached in ten
 * blocks with move-to-front, and per-row bitmaps of SJIS codes at
 * *gDictReadingBits.
 */
#include "common.h"

typedef struct CacheBlk {
    s32 id;                     /* 0x000: page, -1 for none */
    s32 dirty;                  /* 0x004 */
    struct CacheBlk *next;      /* 0x008 */
    u8 data[0x400];             /* 0x00C */
} CacheBlk;

/* A reference-counted entry of the 0x80-slot table at gDictSlots. */
typedef struct Slot8 {
    u8 refs;                    /* 0x0: references, 0 for free */
    s8 use;                     /* 0x1: the use number (Dict_MarkUsed) */
    s16 unk2;
    s16 unk4;
    s16 unk6;
} Slot8;

typedef struct Node8 {
    s32 unk0;
    struct Node8 *next;
} Node8;

extern Node8 *gUserNodeFree;
extern Node8 gUserNodePool[512];
extern s32 gUserDictHash[0x50];
extern u8 gUserDictDirty;
extern u8 *gUserDictPageEnd;
extern u8 *gUserDictFree;
extern u8 gUserDictPages[8][0x400];
extern s32 gUserDictPage;

void UserNode_InitPool(void);
void UserDict_ClearBuckets(void);
void UserDict_ClearPages(void);
s32 UserDict_Load(void);
s32 UserDict_Save(void);
void UserDict_RecyclePage(void);

extern s32 gDictFd;
extern u8 *gDictReadingBits;
extern u8 gDictKeyBuf[];
extern s32 gDictKeyLo;
extern s32 gDictKeyHi;
s32 Str_CompareN(char *a, u8 *b, s32 n);
extern s32 gDictReadingsDirty;
extern CacheBlk *gDictPageHead;
extern CacheBlk gDictPages[10];
extern Slot8 gDictSlots[0x80];

s32 DictIo_Read(s32 fd, void *buf, s32 n);
s32 DictIo_Write(s32 fd, void *buf, s32 n);
#if PLATFORM_PC
/* the definition's s32: the callers pass the position's low word. The EE's DictIo_SeekTo hands its whole
 * register on to MemFile_Seek (s64 pos); the positions are page offsets that fit in an s32, so the register is
 * the low word sign-extended, as the PC's conversion back to s64 makes it */
s32 DictIo_SeekTo(s32 pos);
#define DictIo_SeekTo(pos) DictIo_SeekTo((s32)(pos))
#else
/* unit's own declaration: declared as this file's callers use it; with the definition's s32 (s32 pos), DictPage_Read and DictPage_Write compile differently */
s32 DictIo_SeekTo(s64 pos);
#endif
s32 Str_CommonPrefix(u8 *a, u8 *b, s32 n);
s32 DictPage_Write(CacheBlk *b);
s32 DictPage_Read(CacheBlk *b);

s32 Dict_ReadKeys(void)
{
    s32 r;

    if (DictIo_SeekTo(0x400) == -1) {
        return -1;
    }
    r = DictIo_Read(gDictFd, gDictKeyBuf, 0x1000);
    return -(r != 0x1000);
}

s32 Dict_ReadingBitClear(u8 *s, s16 single)
{
    s32 c;
    s32 row;
    s32 col;
    s32 i;

    c = s[0];
    if (c <= 0xA0) {
        return 0;
    }
    row = c - 0xA1;
    if (single == 1) {
        col = 0;
    } else {
        c = s[1];
        if (c <= 0xA0) {
            return 0;
        }
        col = c - 0xA0;
    }
    i = row * 11 + (col >> 3);
    if (gDictReadingBits[i] & (1 << (col & 7))) {
        return 0;
    }
    return 1;
}

/* Sets the bit of the reading s (one or two bytes from 0xA1) in the table
 * at *gDictReadingBits that Dict_ReadingBitClear tests; marks it changed. */
void Dict_SetReadingBit(u8 *s, s16 single)
{
    s32 c;
    s32 row;
    s32 col;
    s32 i;

    c = s[0];
    if (c < 0xA1) {
        return;
    }
    row = c - 0xA1;
    if (single == 1) {
        col = 0;
    } else {
        c = s[1];
        if (c < 0xA1) {
            return;
        }
        col = c - 0xA0;
    }
    i = row * 11 + (col >> 3);
    gDictReadingBits[i] |= 1 << (col & 7);
    gDictReadingsDirty = 1;
}

/* Binary search of the 4-byte keys at gDictKeyBuf (gDictKeyLo .. gDictKeyHi): the index
 * of key, or of the last key before it. */
s32 Dict_FindKey(u8 *key)
{
    s32 mid;
    s32 c;
    s32 hi;
    s32 lo;

    hi = gDictKeyHi;
    lo = gDictKeyLo;
    while (lo + 1 < hi) {
        mid = (hi + lo) / 2;
        c = Str_CompareN((char *) key, &gDictKeyBuf[mid * 4], 4);
        if (c == 0) {
            return mid;
        }
        if (c > 0) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    if (Str_CompareN((char *) key, &gDictKeyBuf[lo * 4], 4) < 0) {
        lo--;
    }
    return lo;
}

s32 func_002FEA30(s32 i, u8 *s)
{
    return s[Str_CommonPrefix(&gDictKeyBuf[(i + 1) * 4], s, 4)] != 0;
}

s32 DictKey_MatchLen(s32 i, u8 *s)
{
    s32 n = Str_CommonPrefix(&gDictKeyBuf[i * 4], s, 4);

    if (n >= 4) {
        return n;
    }
    return s[n] == 0 ? n : n + 1;
}

/* The length of the common prefix of a and b, at most n. */
s32 Str_CommonPrefix(u8 *a, u8 *b, s32 n)
{
    s32 i;

    for (i = 0; i < n; i++) {
        if (*a != *b || *a == 0) {
            break;
        }
        a++;
        b++;
    }
    return i;
}

void DictPage_Init(void)
{
    CacheBlk *b;

    gDictPageHead = gDictPages;
    for (b = gDictPages; b < &gDictPages[9]; b++) {
        b->id = -1;
        b->dirty = 0;
        b->next = b + 1;
    }
    b->id = -1;
    b->dirty = 0;
    b->next = NULL;
}

s32 DictPage_Write(CacheBlk *b)
{
    s32 r;

    if (DictIo_SeekTo(((s64)b->id << 10) + 0x3400) == -1) {
        return -1;
    }
    r = DictIo_Write(gDictFd, b->data, 0x400);
    return -(r != 0x400);
}

s32 DictPage_Read(CacheBlk *b)
{
    if (DictIo_SeekTo(((s64)b->id << 10) + 0x3400) == -1) {
        return -1;
    }
    DictIo_Read(gDictFd, b->data, 0x400);
    return 0;
}

/* The cached page, moved to the front of the list; loads it into the
 * last block (writing that back first if it was changed) on a miss. */
u8 *DictPage_Get(s32 id)
{
    CacheBlk *prev = NULL;
    CacheBlk *b = gDictPageHead;

    for (;;) {
        if (b->id == id) {
            if (prev != NULL) {
                prev->next = b->next;
                b->next = gDictPageHead;
                gDictPageHead = b;
            }
            return b->data;
        }
        if (b->next == NULL) {
            break;
        }
        prev = b;
        b = b->next;
    }
    prev->next = NULL;
    b->next = gDictPageHead;
    gDictPageHead = b;
    if (b->dirty == 1) {
        DictPage_Write(b);
    }
    b->id = id;
    b->dirty = 0;
    DictPage_Read(b);
    return b->data;
}

void DictPage_MarkDirty(void)
{
    gDictPageHead->dirty = 1;
}

void DictPage_Flush(void)
{
    CacheBlk *b;

    for (b = gDictPageHead; b != NULL; b = b->next) {
        if (b->dirty == 1) {
            DictPage_Write(b);
        }
    }
}

void Slot_ClearAll(void)
{
    s32 i;

    for (i = 0; i < 0x80; i++) {
        gDictSlots[i].refs = 0;
    }
}

/* The slot of gDictSlots holding (id, b, c), with one more reference, or
 * the first free one set to it (d at +1) with one: its index, or -1 when
 * the table is full. */
s32 Slot_Acquire(s32 id, s32 b, s32 c, s32 d)
{
    Slot8 *s = gDictSlots;
    s32 free = -1;
    s32 i;

    for (i = 0; i < 0x80; i++, s++) {
        if (s->refs == 0) {
            if (free == -1) {
                free = i;
            }
        } else if ((u16)s->unk2 == id && s->unk4 == b && s->unk6 == c) {
            s->refs++;
            return i;
        }
    }
    if (free == -1) {
        return -1;
    }
    s = &gDictSlots[free];
    s->refs = 1;
    s->unk2 = id;
    s->unk4 = b;
    s->unk6 = c;
    s->use = d;
    return free;
}

/* The three values of slot i of gDictSlots into a, b, c; returns its +2, or -1 when i is out of range or the slot is free. */
s32 Slot_Get(s32 i, s32 *a, s32 *b, s32 *c)
{
    Slot8 *s;

    if ((u32)i >= 0x80) {
        return -1;
    }
    s = gDictSlots + (MWCC_LONG)i;
    if (s->refs == 0) {
        return -1;
    }
    *a = s->unk4;
    *b = s->unk6;
    *c = (u8)s->use;
    return (u16)s->unk2;
}

s32 Slot_MaxUse(s32 *ids, s32 n)
{
    s32 max;
    s32 i;
    s32 id;
    Slot8 *s;

    max = 0;
    for (i = 0; i < n; i++, ids += 2) {
        id = *ids;
        if (id >= 0 && id < 0x80) {
            s = &gDictSlots[id];
            if (s->refs > 0 && max < (u8)s->use) {
                max = (u8)s->use;
            }
        }
    }
    return max;
}

s32 Slot_SetUse(s32 *ids, s32 n, s8 v)
{
    s32 i;
    s32 id;
    Slot8 *s;

    for (i = 0; i < n; i++, ids += 2) {
        id = *ids;
        if (id >= 0 && id < 0x80) {
            s = &gDictSlots[id];
            if (s->refs > 0) {
                s->use = v;
            }
        }
    }
    return 0;
}

/* Drops a reference to slot i of gDictSlots: 0, or -1 when i is out of
 * range or the slot is free. */
s32 Slot_Release(s32 i)
{
    Slot8 *s;

    if ((u32)i >= 0x80) {
        return -1;
    }
    s = gDictSlots + (MWCC_LONG)i;
    if (s->refs == 0) {
        return -1;
    }
    s->refs--;
    return 0;
}

void DictSlots_ClearGroup(s32 v)
{
    s32 i;
    Slot8 *e = gDictSlots;

    for (i = 0; i < 0x80; i++, e++) {
        if (v == (s16)(e->unk6 >> 12)) {
            e->unk6 = -1;
        }
    }
}

void DictSlots_ClearId(s32 v)
{
    s32 i;
    Slot8 *e = gDictSlots;

    for (i = 0; i < 0x80; i++, e++) {
        if (v == e->unk6) {
            e->unk6 = -1;
        }
    }
}

void UserDict_Init(void)
{
    UserNode_InitPool();
    UserDict_ClearBuckets();
    if (UserDict_Load() == -1) {
        UserDict_ClearPages();
    }
    gUserDictDirty = 0;
}

void UserDict_Flush(void)
{
    if (gUserDictDirty != 0) {
        UserDict_Save();
    }
}

/* Its end address &gUserNodePool[511] has a %hi one above the start's. */
void UserNode_InitPool(void)
{
    Node8 *n;

    for (n = gUserNodePool; n < &gUserNodePool[511]; n++) {
        n->next = n + 1;
    }
    n->next = NULL;
    gUserNodeFree = gUserNodePool;
}

void UserDict_ClearBuckets(void)
{
    s32 i;

    for (i = 0; i < 0x50; i++) {
        gUserDictHash[i] = 0;
    }
}

/* Empties the eight pages of learned words; the pool starts again at page 0. */
void UserDict_ClearPages(void)
{
    s32 i;
    u8 *p;

    gUserDictPages[0][1] = 0;
    gUserDictPages[0][0] = 0;
    for (i = 1; i < 8; i++) {
        p = gUserDictPages[i];
        p[1] = 0;
        p[0] = 0;
    }
    gUserDictFree = gUserDictPages[0];
    gUserDictPageEnd = gUserDictPages[1];
    gUserDictPage = 0;
}

s32 UserDict_Bucket(u8 *s)
{
    if (*s >= 0xA0 && *s < 0xF0) {
        return *s - 0xA0;
    }
    return 0;
}

Node8 *UserNode_Alloc(void)
{
    Node8 *n;

    if (gUserNodeFree == NULL) {
        UserDict_RecyclePage();
    }
    n = gUserNodeFree;
    gUserNodeFree = n->next;
    return n;
}

void UserNode_Free(Node8 *n)
{
    n->next = gUserNodeFree;
    gUserNodeFree = n;
}

void UserDict_RecyclePage(void);

/* n bytes from the pool at gUserDictFree (UserDict_RecyclePage first when they would pass gUserDictPageEnd - 2);
 * the two bytes after them are cleared. */
u8 *UserDict_AllocBytes(s32 n)
{
    u8 *p;

    if (gUserDictFree > gUserDictPageEnd - n - 2) {
        UserDict_RecyclePage();
    }
    p = gUserDictFree;
    gUserDictFree = p + n;
    gUserDictFree[0] = 0;
    gUserDictFree[1] = 0;
    return p;
}
