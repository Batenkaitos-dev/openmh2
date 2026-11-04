#include "common.h"
#include "syssettings.h"
#include "em.h"
#include "conv.h"

/* prototypes */
s32 func_002FB000(void);
void Conv_Reset(void);
s32 Dict_LearnEnabled(void);
u8 *Conv_NumberToText(u8 *digits, s32 len, u8 *out, s32 mode);
s32 DictIo_CloseFile(void);
s32 DictFile_WriteHeader(void);
void DictPage_Flush(void);
s32 Slot_Release(s32 i);
void UserDict_Flush(void);
void ConvRecs_Init(void);
void InNode_InitPool(void);
extern s32 gDictFd;
extern s32 D_00404EC8;
extern s32 D_00581B80;
extern s64 gMemFileLeft;
extern u8 *gMemFilePos;
extern s32 gDictLearn;
extern s32 gNumConvMode;
extern s32 D_0059BA48;
/* end of prototypes */

/* A dictionary word (as the kana-kanji conversion's learned words keep it). */

s32 DictEntry_RemoveWord(u8 *entry, Word *w);

/* A hit in the learned words, a dictionary result and its senses. */
typedef struct UserHit {
    s16 unk0;                   /* 0x0 */
    s16 pos;                    /* 0x2 */
    s16 len;                    /* 0x4 */
    s16 unk6;                   /* 0x6 */
    s32 unk8;
    u8 *entry;                  /* 0xC */
} UserHit;

typedef struct MatchItem {
    u8 kind;                    /* 0x0 */
    u8 sub;                     /* 0x1 */
    u8 unk2;                    /* 0x2 */
    u8 unk3;
    u16 unk4;                   /* 0x4 */
} MatchItem;

typedef struct Match {
    s16 len;                    /* 0x00 */
    u64 entry;                  /* 0x08 */
    s16 unk10;                  /* 0x10 */
    s16 unk12;
    s16 count;                  /* 0x14 */
    MatchItem items[1];         /* 0x16 */
} Match;

char *strncpy(char *dst, char *src, u32 n);
s32 Dict_FindEntry(u8 *key, s16 len, UserHit *out);
s32 UserDict_FindExact(u8 *key, s16 len, UserHit *out);
s32 DictEntry_CollectSenses(u8 *entry, MatchItem *items, s32 n, s32 *max);
s32 Slot_Acquire(s32 id, s32 b, s32 c, s32 d);
s32 Dict_ReadingBitClear(u8 *s, s16 single);
/* UserDict_Learn, UserDict_WriteWord and UserDict_AppendWord are defined
 * with an s8 last parameter; the callers here pass an int
 * unconverted, as through an s32 prototype. */
/* unit's own declaration: declared as this file's callers use it; with the definition's s32 (u8 *key, Word *w, s8 arg2), UserDict_AddWordNumbered compiles differently */
s32 UserDict_Learn(u8 *key, Word *w, s32 num);
/* unit's own declaration: declared as this file's callers use it; with the definition's s32 (s32 arg0), Code_ToHalfWidth compiles differently */
s32 Jis_ToHalfByte(u16 c);
extern u16 gHalfKanaJis[];
s32 Str_IsDigits(u8 *key);
void Sjis_ToCode2(u8 *src, u8 *dst);
s32 Sjis_Code2Size(u8 *s);
s32 Str_CompareN(char *a, u8 *b, s32 n);
s32 Dict_FindKey(u8 *key);
void Slot_ClearAll(void);
u32 DictBlock_End(u8 *e);
s32 UserDict_RaiseRecord(u8 *p, u8 *e);
u8 *DictEntry_FindSense(u8 *p, u8 *end);
void Mem_Shift(u8 *start, u8 *end, s32 shift);
s32 DictEntry_MaxUse(u8 *buf);
s32 UserDict_WordSize(Word *w);
s32 UserDict_AppendSize(Word *w);
void UserDict_WriteWord(u8 *buf, s32 size, Word *w, s8 arg3);
void UserDict_AppendWord(u8 *buf, s32 size, Word *w, s32 arg3);
void Dict_SetReadingBit(u8 *s, s16 single);
void DictIo_SetPath(const char *path);
s32 DictIo_OpenFile(void);
s32 DictFile_ReadHeader(void);
s32 Dict_ReadKeys(void);
void Dict_InitCaches(void);
void UserDict_Init(void);
extern s32 gUserDictFlags;

typedef struct EditEnt {
    u16 code;
    u16 pad;
    s32 n;
} EditEnt;
extern u8 gConvEditLine[];
extern u16 gEditLineText[];
u16 Kana_ByteToCode(u16 c);
s32 Kana_TakesDakuten(u16 c);
s32 Kana_TakesHandakuten(u16 c);
u16 Jis_ToSjis(u16 c);
s32 Code_ToHalfWidth(u8 *out, u16 c);

/* One EditEnt walker serves both parts: the counts of the first part and
 * the codes of the second. */
void EditLine_GetText(u8 *dst, s32 start, s32 n, s32 mode)
{
    EditEnt *p;
    EditEnt *end;
    s32 i;
    s32 a;
    s32 b;
    u16 *s;
    u16 c;
    u16 prev;

    if (mode >= 5) {
        p = (EditEnt *)gConvEditLine;
        a = 0;
        for (i = 0; i < start; i++) {
            a += p->n;
            p++;
        }
        b = a;
        for (i = 0; i < n; i++) {
            b += p->n;
            p++;
        }
        s = &gEditLineText[a];
        for (; a < b; a++) {
            if (mode == 5) {
                c = Jis_ToSjis(Kana_ByteToCode(*s++ | 0x100));
                *dst++ = c >> 8;
                *dst++ = c;
            } else if (mode == 6) {
                *dst++ = *s++;
            }
        }
        *dst = 0;
        return;
    }
    p = (EditEnt *)gConvEditLine + start;
    end = p + n;
    prev = 0;
    for (; p < end; p++) {
        c = p->code;
        if (mode == 1) {
            if (!(c & 0xFF00)) {
                if (c == 0xDE && Kana_TakesDakuten(prev)) {
                    dst -= 2;
                    if (prev == 0x2426) {
                        c = 0x2474;
                    } else {
                        c = prev + 1;
                    }
                } else if (c == 0xDF && Kana_TakesHandakuten(prev)) {
                    dst -= 2;
                    c = prev + 2;
                } else {
                    c = Kana_ByteToCode(c);
                }
            }
            if ((c & 0xFF00) == 0x2500) {
                c &= 0x24FF;
            }
        } else if (mode == 2) {
            if (!(c & 0xFF00)) {
                if (c == 0xDE && Kana_TakesDakuten(prev)) {
                    dst -= 2;
                    if (prev == 0x2526) {
                        c = 0x2574;
                    } else {
                        c = prev + 1;
                    }
                } else if (c == 0xDF && Kana_TakesHandakuten(prev)) {
                    dst -= 2;
                    c = prev + 2;
                } else {
                    c = Kana_ByteToCode(c | 0x100);
                }
            }
            if ((c & 0xFF00) == 0x2400) {
                c |= 0x2500;
            }
        } else if (mode == 3) {
            dst += Code_ToHalfWidth(dst, c);
            continue;
        } else if (mode == 4 && !(c & 0xFF00)) {
            c = Kana_ByteToCode(c | 0x100);
        }
        prev = c;
        if (c & 0xFF00) {
            c = Jis_ToSjis(c);
            *dst++ = c >> 8;
        }
        *dst++ = c;
    }
    *dst = 0;
}

void Conv_Init(s32 arg0)
{
    s32 n;
    s32 half;

    D_0059BA48 = Dict_LearnEnabled();
    InNode_InitPool();
    ConvRecs_Init();
    Conv_Reset();
    if (arg0 == 1) {
        n = func_002FB000() - 1;
        half = n >> 1;
        if (n < 0) {
            half = (s32) (n + 1) >> 1;
        }
        D_00404EC8 = half;
        D_00581B80 = 1;
    }
}

extern u8 gConvEditLine[];
extern u8 *gConvEditEnd;
extern u8 *gConvEditCaret;
extern u16 *gEditLineCursor;
extern u16 *gEditLineConvStart;
extern u16 *gEditLineConvEnd;
s32 Text_CharToCells(u16 *dst, u16 c, s32 n);
u16 Kana_ByteToCode(u16 c);

/*
 * Lays out entries start .. start + n of the edit line (gConvEditLine, 8 bytes each) as text
 * cells into dst (Text_CharToCells), with the conversion buffer gEditLineConvStart .. gEditLineConvEnd
 * inserted at the caret entry gConvEditCaret; with cursor set, the character after the caret
 * (or gEditLineCursor in the buffer, or a closing space) is drawn highlighted (8). Returns the
 * number of cells. The flag's initializer at its declaration puts it in a1, as in the
 * game (`shown = 0;` after `p = dst;` gives v1).
 */
s32 EditLine_ToCells(s32 start, s32 n, u16 *dst, s32 cursor)
{
    u8 *e;
    u16 *p;
    u8 *end;
    u16 *c;
    s32 shown = 0;
    u16 code;
    s32 k;

    e = &gConvEditLine[start * 8];
    end = &gConvEditLine[(start + n) * 8];
    p = dst;
    for (; e < end; e += 8, shown = 0) {
        if (e == gConvEditCaret) {
            for (c = gEditLineConvStart; c < gEditLineConvEnd; c++) {
                if (*c & 0x4000) {
                    code = Kana_ByteToCode(*c);
                } else {
                    code = *c & 0xFF;
                }
                if (cursor && c == gEditLineCursor) {
                    k = Text_CharToCells(p, code, 8);
                    cursor = 0;
                } else {
                    k = Text_CharToCells(p, code, 4);
                }
                p += k;
            }
            shown = 1;
        }
        if (e >= gConvEditEnd) {
            break;
        }
        if (cursor && shown) {
            k = Text_CharToCells(p, *(u16 *)e, 8);
            cursor = 0;
        } else {
            k = Text_CharToCells(p, *(u16 *)e, 4);
        }
        p += k;
    }
    if (cursor) {
        p += Text_CharToCells(p, ' ', 8);
    }
    return p - dst;
}

s32 MemFile_Open(char *path, s32 mode)
{
    gMemFileLeft = (s64) (u32) gSysSettings.memFileSize;
    gMemFilePos = (*(u8 * *)&gSysSettings.memFile);
    return 1;
}

s32 MemFile_Close(s32 fd)
{
    return 0;
}

void *memcpy(void *dst, const void *src, u32 n);

s32 MemFile_Read(s32 unused, void *dst, s32 size)
{
    s32 n = gMemFileLeft;

    if (n >= size) {
        n = size;
    }
    memcpy(dst, (void *)gMemFilePos, n);
    gMemFileLeft -= n;
    gMemFilePos += n;
    return n;
}

void *memcpy(void *dst, const void *src, u32 n);

s32 MemFile_Write(s32 unused, void *src, s32 size)
{
    s32 n = gMemFileLeft;

    if (size < n) {
        n = size;
    }
    memcpy((void *)gMemFilePos, src, size);
    gMemFileLeft -= n;
    gMemFilePos += n;
    return n;
}

/* lseek on the memory file ((*(u8 * *)&gSysSettings.memFile), gSysSettings.memFileSize bytes; gMemFilePos the
 * position, gMemFileLeft what is left): the new offset, or -1. */
s64 MemFile_Seek(s32 fd, s64 pos, s32 whence)
{
    s64 r;

    switch (whence) {
    case 0:
        if (pos < 0) {
            return -1;
        }
        gMemFileLeft = (u32)gSysSettings.memFileSize - pos;
        gMemFilePos = (*(u8 * *)&gSysSettings.memFile) + pos;
        break;
    case 1:
        r = gMemFileLeft - pos;
        if (r < 0 || (u32)gSysSettings.memFileSize < r) {
            return -1;
        }
        gMemFileLeft = r;
        gMemFilePos += pos;
        break;
    case 2:
        if (pos > 0) {
            return -1;
        }
        gMemFileLeft = -pos;
        gMemFilePos = (*(u8 * *)&gSysSettings.memFile) + gSysSettings.memFileSize + pos - 1;
        break;
    }
    return gMemFilePos - (*(u8 * *)&gSysSettings.memFile);
}

extern u16 gKanaByteCodes[];

u16 Kana_ByteToCode(u16 c)
{
    u16 v = gKanaByteCodes[c & 0xFF];

    if ((v & 0xFF00) == 0x2500 && !(c & 0x100)) {
        return (v & 0xFF) | 0x2400;
    }
    return v;
}

/* Keys (0-terminated) and the codes they map to. */
extern u8 gKanaKeyChars[];
extern u16 gKanaKeyCodes[];

/* The code for key byte c (0 if none); a 0x25xx code becomes 0x24xx
 * unless c has bit 8 set. The redundant (u8) on k gives the re-mask. */
u16 Kana_CodeForKey(u16 c)
{
    u8 *key;
    u16 *val;
    u8 k;

    key = gKanaKeyChars;
    val = gKanaKeyCodes;
    for (k = *key; k != 0; k = *++key, val++) {
        if ((u8)k == (u8)c) {
            if ((*val & 0xFF00) == 0x2500 && !(c & 0x100)) {
                return (*val & 0xFF) | 0x2400;
            }
            return *val;
        }
    }
    return 0;
}

u16 Kana_ToJis(u8 a, u16 b)
{
    return a | ((b & 0x100) + 0x2400);
}

/* Code c as half-width bytes at out: rows 0x00 and 0x23 as is, rows 0x21,
 * 0x24 and 0x25 (0x25 read as 0x24) through Jis_ToHalfByte and the kana
 * table gHalfKanaJis, adding 0xDE or 0xDF for a voiced mark; anything
 * else a space. Returns the byte count. */
s32 Code_ToHalfWidth(u8 *out, u16 c)
{
    s32 k = c;
    u8 v;
    u16 t;

    switch (k & 0xFF00) {
    case 0x2300:
    case 0:
        *out = k;
        return 1;
    case 0x2500:
        c &= 0x24FF;
    case 0x2400:
    case 0x2100:
        v = Jis_ToHalfByte(c);
        if (v < 0x98 || v >= 0xF7) {
            *out = v;
            return 1;
        }
        t = gHalfKanaJis[v];
        *out = t;
        if (t & 0x8000) {
            out[1] = 0xDE;
            return 2;
        }
        if (t & 0x4000) {
            out[1] = 0xDF;
            return 2;
        }
        return 1;
    default:
        *out = 0x20;
        return 1;
    }
}

s32 Sjis_IsCode(u16 c);
u16 Sjis_ToJis(u16 c);
s32 Code_ToHalfWidth(u8 *out, u16 c);
extern u8 gKbdCharClass[];

s32 Kana_TakesDakuten(u16 c)
{
    u8 buf[4];

    if (Sjis_IsCode(c)) {
        c = Sjis_ToJis(c);
    }
    if (Code_ToHalfWidth(buf, c) != 1) {
        return 0;
    }
    return (gKbdCharClass[buf[0]] & 0xF) == 0xA;
}

s32 Kana_TakesHandakuten(u16 c)
{
    u8 buf[4];

    if (Sjis_IsCode(c)) {
        c = Sjis_ToJis(c);
    }
    if (Code_ToHalfWidth(buf, c) != 1) {
        return 0;
    }
    return (gKbdCharClass[buf[0]] & 0xF) == 0xB;
}

extern u8 gJisHalfSymbols[];
extern u8 gJisHalfSymbolsEnd[];

s32 Jis_LookupHalfSymbol(u16 c)
{
    u8 *p;

    for (p = gJisHalfSymbols; p < gJisHalfSymbolsEnd; p += 4) {
        if (*(u16 *)p == c) {
            return p[2];
        }
        if (*(u16 *)p > c) {
            break;
        }
    }
    return 0;
}

extern u8 gConvConnBits[];
extern u8 gConvBitMasks[];

extern u8 gConvConnCols[];
extern u8 gConvConnRows[];

s32 Conv_TestConnBit(s16 bit)
{
    return gConvConnBits[bit >> 3] & gConvBitMasks[bit & 7];
}

/* The kana code with its dakuten form for c in 1..13 (c + 0x7F), else 0. */
static u8 Kana_ToDakuten(u8 c)
{
    if (c > 0 && c < 0xE) {
        return c + 0x7F;
    }
    return 0;
}

/* Whether kana c (dakuten form with mark) is set in the bit table for kind (gConvConnCols index, gConvConnRows rows). base is compared as s32 (a (s16) cast there changes where it is extended). */
s32 Kana_TestBit(u8 c, u8 mark, u8 kind)
{
    s32 base;
    s16 *e;

    if (kind < 0x80) {
        return 0;
    }
    if (c < 0x2D && mark) {
        if ((c = Kana_ToDakuten(c)) == 0) {
            return 0;
        }
    }
    base = (s16)gConvConnCols[c];
    if (base == 0xFF) {
        return 0;
    }
    if (kind >= 0xC0) {
        e = (s16 *)(gConvConnRows + (kind - 0xC0) * 4);
    } else {
        e = (s16 *)(gConvConnRows + (kind - 0x41) * 4);
    }
    if (c < 0x80 || c >= 0xC0) {
        return Conv_TestConnBit(e[0] + (s16)base);
    }
    return Conv_TestConnBit(mark + (e[1] + (s16)base) - 1);
}

/* Whether a dictionary sense of this kind (part of speech) takes sub-kind sub (a
 * conjugation class, 0 none): kinds 0xC0.. always; below 0x80, 0xD, 0x13..0x34 and 0x38, or
 * 1..13 with a sub-kind through their dakuten codes (Kana_ToDakuten); the rest by groups.
 * (sub - 7) <= 1 as unsigned is the game's compare for 7 or 8. */
s32 Dict_SenseTakesSub(u8 kind, u8 sub)
{
    if (kind >= 0xC0) {
        return 1;
    }
    if (kind < 0x80) {
        if (kind < 0x2D && sub != 0) {
            if ((kind = Kana_ToDakuten(kind)) == 0) {
                return 0;
            }
        } else {
            if (kind == 0xD || (kind >= 0x13 && kind <= 0x34) || kind == 0x38) {
                return 1;
            }
            return 0;
        }
    }
    switch (kind) {
    case 0x8B:
    case 0x8D:
    case 0x93:
    case 0x96:
    case 0x99:
    case 0x9A:
    case 0x9B:
    case 0x9E:
    case 0xA1:
    case 0xA2:
    case 0xA7:
    case 0xA8:
    case 0xB1:
        if ((sub >= 5 && sub <= 8) || sub >= 10) {
            return 1;
        }
        break;
    case 0x97:
        if (sub == 8) {
            return 0;
        }
    case 0x8E:
    case 0x92:
    case 0x94:
    case 0x95:
    case 0x98:
    case 0xA0:
        if (sub >= 5) {
            return 1;
        }
        break;
    default:
        if (sub == 4 || (u32)(sub - 7) <= 1 || sub >= 10) {
            return 1;
        }
        break;
    }
    return 0;
}

/* Opens the dictionary file at path and reads it: 3, or -1 (no path),
 * -7/-8 (open failed), -2 (read failed), -6 (flag 0x8000 in gUserDictFlags). */
s32 DictFile_Open(char *path)
{
    s32 r;

    if (*path == 0) {
        return -1;
    }
    DictIo_SetPath(path);
    r = DictIo_OpenFile();
    if (r != 0) {
        return r == -1 ? -7 : -8;
    }
    if (DictFile_ReadHeader() == -1) {
        DictIo_CloseFile();
        return -2;
    }
    if (Dict_ReadKeys() == -1) {
        DictIo_CloseFile();
        return -2;
    }
    Dict_InitCaches();
    UserDict_Init();
    return gUserDictFlags == 0x8000 ? -6 : 3;
}

s32 DictFile_Close(void)
{
    if (gDictFd == -1) {
        return -3;
    }
    DictFile_WriteHeader();
    UserDict_Flush();
    DictPage_Flush();
    if (DictIo_CloseFile() == -1) {
        return -2;
    }
    return 3;
}

s32 Dict_FindPrefix(u8 *key, s16 len, UserHit *out);
s32 UserDict_FindPrefix(u8 *key, s16 len, UserHit *out);

/* Looks the longest prefix of the reading s (len bytes) up in the dictionary and the
 * learned words into m: 1, 0 if in neither, 2 (either lookup's 2), -3 without a dictionary. */
s32 Dict_LookupPrefix(u8 *s, s16 len, Match *m)
{
    s32 max;
    u8 key[0x5C];
    UserHit ha;
    UserHit hu;
    s32 fa;
    s32 fu;
    s16 a;
    s16 b;
    s16 lu;
    s16 la;

    if (gDictFd == -1) {
        return -3;
    }
    strncpy((char *)key, (char *)s, len);
    key[len] = 0;
    if ((fa = Dict_FindPrefix(key, len, &ha)) == 2) {
        return 2;
    }
    strncpy((char *)key, (char *)s, len);
    key[len] = 0;
    fu = UserDict_FindPrefix(key, len, &hu);
    if (fu == 2) {
        return 2;
    }
    if (fa == 0 && fu == 0) {
        return 0;
    }
    m->unk12 = 1;
    max = 0;
    a = hu.unk6;
    b = ha.unk6;
    m->unk10 = a < b ? b : a;
    lu = hu.len;
    la = ha.len;
    if (lu < la) {
        m->len = la;
        m->count = DictEntry_CollectSenses(ha.entry, m->items, 0, &max);
        hu.pos = -1;
    } else if (la < lu) {
        m->len = lu;
        m->count = DictEntry_CollectSenses(hu.entry, m->items, 0, &max);
        ha.pos = -1;
    } else {
        m->len = la;
        m->count = DictEntry_CollectSenses(hu.entry, m->items, DictEntry_CollectSenses(ha.entry, m->items, 0, &max), &max);
    }
    return (m->entry = Slot_Acquire(ha.unk0, ha.pos, hu.pos, max)) != -1;
}

s32 func_002FEA30(s32 i, u8 *s);
s32 DictKey_MatchLen(s32 i, u8 *s);
s32 Str_CommonPrefix(u8 *a, u8 *b, s32 n);
u8 *DictPage_Get(s32 k);

/* Looks for the longest dictionary word that is a prefix of key, shortening key a byte at a
 * time while nothing is found: 2 when key itself is there (first pass), 1 with the hit in out,
 * 0 (out->pos -1) for none. */
s32 Dict_FindPrefix(u8 *key, s16 len, UserHit *out)
{
    s16 n;
    u8 *base;
    u8 *found;
    s16 blen;
    s32 k;
    s16 klen;
    s16 maxpre;
    s32 first;
    u8 *e;
    s16 pre;
    s32 d;

    first = 1;
    out->unk6 = 0;
    out->len = 0;
    found = NULL;
    k = Dict_FindKey(key);
    if (func_002FEA30(k, key) == 0) {
        return 2;
    }
    maxpre = 0;
    for (;;) {
        e = base = DictPage_Get(k);
        n = DictKey_MatchLen(k, key);
        blen = 0;
        if (!(u16)(e[0] + (e[1] << 8)) == 0) {
            do {
                klen = e[2];
                pre = Str_CommonPrefix(e + 3, key, klen);
                if (pre >= maxpre) {
                    maxpre = pre + 1;
                }
                d = Str_CompareN((char *) (e + 3), key, klen);
                if (d == 0) {
                    if (klen == len && first) {
                        return 2;
                    }
                    blen = klen;
                    found = e;
                } else if (d > 0) {
                    if (Str_CompareN((char *) (e + 3), key, len) == 0 && first) {
                        return 2;
                    }
                    break;
                }
                e += (u16)(e[0] + (e[1] << 8));
            } while ((u16)(e[0] + (e[1] << 8)) != 0);
        }
        if (blen > 0) {
            break;
        }
        if (n < 2) {
            out->pos = -1;
            return 0;
        }
        len = n - 1;
        key[len] = 0;
        k = Dict_FindKey(key);
        first = 0;
    }
    out->unk0 = k;
    out->pos = found - base;
    out->len = blen;
    out->unk6 = maxpre;
    out->entry = found;
    return 1;
}

/* Reads a dictionary entry's senses (kind, frequency, optional sub-kind below 12) into
 * items[n..], skipping those UserDict_RaiseRecord finds already listed, up to 32; *max keeps the
 * highest frequency. Returns the new count. */
s32 DictEntry_CollectSenses(u8 *entry, MatchItem *items, s32 n, s32 *max)
{
    MatchItem *first;
    u8 *end;
    s32 v;

    first = items;
    end = entry + (u16)(entry[0] + (entry[1] << 8));
    entry = entry + entry[2] + 3;
    items += n;
    while (entry < end) {
        items->kind = *entry++;
        if ((items->unk4 = *entry++) > *max) {
            *max = items->unk4;
        }
        v = *entry;
        if (v < 0xC) {
            items->sub = v;
            entry++;
        } else {
            items->sub = 0;
        }
        items->unk2 = 0;
        if (UserDict_RaiseRecord((u8 *)first, (u8 *)items) == 0) {
            n++;
            items++;
            if (n >= 32) {
                break;
            }
        }
        entry = DictEntry_FindSense(entry, end);
    }
    return n;
}

/* Looks for the 6-byte record with e's two id bytes before e; raises its u16 at +4 to e's. */
s32 UserDict_RaiseRecord(u8 *p, u8 *e)
{
    for (; p < e; p += 6) {
        if (p[0] == e[0] && p[1] == e[1]) {
            if (*(u16 *)(p + 4) < *(u16 *)(e + 4)) {
                *(u16 *)(p + 4) = *(u16 *)(e + 4);
            }
            return 1;
        }
    }
    return 0;
}

u8 *DictEntry_FindSense(u8 *p, u8 *end)
{
    for (; p < end; p += 2) {
        if (*p <= 0x38) {
            break;
        }
    }
    return p;
}

/* A word reference is a u64 (Slot_Get uses its low word). */
#if PLATFORM_PC
/* the definition's s32: the callers pass the original's low word. The EE's Slot_Get tests the whole register
 * (sltiu a0, 0x80) and indexes with its low word; every reference is Slot_Acquire's s32 sign-extended, or -1,
 * so the low word gives the same result */
s32 Slot_Get(s32 i, s32 *a, s32 *b, s32 *c);
#define Slot_Get(r, a, b, c) Slot_Get((s32)(r), a, b, c)
#else
/* unit's own declaration: declared as this file's callers use it; with the definition's s32 (s32 i, s32 *a, s32 *b, s32 *c), Dict_ClearUse and Dict_FindBest compile differently */
s32 Slot_Get(u64 r, s32 *a, s32 *b, s32 *c);
#endif
u8 *DictPage_Get(s32 k);
void DictPage_MarkDirty(void);
u8 *UserDict_EntryAt(s32 pos);
void UserDict_SetDirty(void);

/* Looks the reading s (len bytes) up in the dictionary and the learned
 * words into m: 1, 0 if in neither, -3 without a dictionary. */
s32 Dict_LookupReading(u8 *s, s16 len, Match *m)
{
    s32 max;
    u8 key[0x5C];
    UserHit ha;
    UserHit hu;
    s32 fa;
    s32 fu;

    if (gDictFd == -1) {
        return -3;
    }
    strncpy((char *)key, (char *)s, len);
    key[len] = 0;
    fa = Dict_FindEntry(key, len, &ha);
    fu = UserDict_FindExact(key, len, &hu);
    if (fa == 0 && fu == 0) {
        return 0;
    }
    m->len = len;
    m->unk10 = len;
    m->unk12 = 1;
    m->count = 0;
    max = 0;
    if (fa == 1) {
        m->count = DictEntry_CollectSenses(ha.entry, m->items, 0, &max);
    }
    if (fu == 1) {
        m->count = DictEntry_CollectSenses(hu.entry, m->items, m->count, &max);
    }
    if ((m->entry = Slot_Acquire(ha.unk0, ha.pos, hu.pos, max)) == -1) {
        return 0;
    }
    return 1;
}

/* Finds the dictionary entry whose reading is key (len bytes): 1 with
 * its block, offset and pointer in out, else 0 (out->pos -1). */
s32 Dict_FindEntry(u8 *key, s16 len, UserHit *out)
{
    s32 k;
    u8 *base;
    u8 *e;
    s16 kl;
    s32 r;

    if (len < 3 && Dict_ReadingBitClear(key, len)) {
        out->pos = -1;
        return 0;
    }
    k = Dict_FindKey(key);
    e = base = DictPage_Get(k);
    if ((u16)(e[0] + (e[1] << 8)) != 0) {
        do {
            kl = e[2];
            r = Str_CompareN((char *) (e + 3), key, kl);
            if (r == 0) {
                if (kl == len) {
                    break;
                }
            } else if (r > 0) {
                out->pos = -1;
                return 0;
            }
            e += (u16)(e[0] + (e[1] << 8));
        } while ((u16)(e[0] + (e[1] << 8)) != 0);
    }
    if ((u16)(e[0] + (e[1] << 8)) == 0) {
        out->pos = -1;
        return 0;
    }
    out->unk0 = k;
    out->pos = e - base;
    out->entry = e;
    return 1;
}

void DictEntry_FindBest(u8 *entry, s32 kind, s32 sub, s32 *found, u8 *out, s32 flags);
void Cand_FromDictRec(u8 *out, u8 *in, u8 *end, s32 code);

/* Looks word r up (dictionary, then user dictionary) for its record of
 * kind and sub with the highest value: 1 found (into out), 0 not, -3
 * without a dictionary. */
s32 Dict_FindBest(u64 r, u8 kind, u8 sub, u8 *out)
{
    s32 a;
    s32 b;
    s32 c;
    s32 found;
    s32 k;

    if (gDictFd == -1) {
        return -3;
    }
    k = Slot_Get(r, &a, &b, &c);
    if (k == -1) {
        return 0;
    }
    found = -1;
    if (a != -1) {
        DictEntry_FindBest(DictPage_Get(k) + a, kind, sub, &found, out, 0);
    }
    if (b != -1) {
        DictEntry_FindBest(UserDict_EntryAt(b), kind, sub, &found, out, 0x8000);
    }
    if (found == -1) {
        return 0;
    }
    return 1;
}

/* Of the entry's records of kind and sub, the one whose value is above
 * *found (and the highest) goes to out. */
void DictEntry_FindBest(u8 *entry, s32 kind, s32 sub, s32 *found, u8 *out, s32 flags)
{
    u8 *p;
    u8 *rec;
    s32 v;
    s32 attr;
    u8 *end;
    u8 *hit;
    s32 best;

    end = entry + (u16)(entry[0] + (entry[1] << 8));
    best = *found;
    p = entry + entry[2] + 3;
    hit = NULL;
    while (p < end) {
        rec = p;
        attr = p[1];
        p += 2;
        v = *p;
        if (v < 0xC) {
            p++;
        } else {
            v = 0;
        }
        if (kind == rec[0] && sub == v && *found < attr) {
            *found = attr;
            hit = rec;
        }
        p = DictEntry_FindSense(p, end);
    }
    if (best < *found) {
        Cand_FromDictRec(out, hit, end, flags | (hit - entry));
    }
}

void Code2_ToSjis(u8 *dst, u8 *src, u8 *end);

void Cand_FromDictRec(u8 *out, u8 *in, u8 *end, s32 code)
{
    *(s16 *)out = code;
    *(s16 *)(out + 2) = in[1];
    in += 2;
    out[4] = 0;
    if (*in < 0xC) {
        in++;
    }
    Code2_ToSjis(out + 5, in, end);
}

void DictEntry_ListAll(u8 *entry, s32 kind, s32 sub, s32 *n, s32 *m, u8 *out, s32 flags);

/* Like Dict_FindBest, but lists every record of kind and sub; *count
 * gets how many. */
s32 Dict_ListAll(u64 r, u8 kind, u8 sub, u8 *out, s32 *count)
{
    s32 a;
    s32 b;
    s32 n;
    s32 m;
    s32 k;

    if (gDictFd == -1) {
        return -3;
    }
    k = Slot_Get(r, &a, &b, &n);
    if (k == -1) {
        return 0;
    }
    n = 0;
    m = 0;
    if (a != -1) {
        DictEntry_ListAll(DictPage_Get(k) + a, kind, sub, &n, &m, out, 0);
    }
    if (b != -1) {
        DictEntry_ListAll(UserDict_EntryAt(b), kind, sub, &n, &m, out, 0x8000);
    }
    if ((*count = n) == 0) {
        return 0;
    }
    return 1;
}

s32 Code2_SjisWidth(u8 *p, u8 *end);
u8 *CandList_Insert(u8 *out, s32 attr, s32 len, s32 pos);

/* Appends each of the entry's records of kind and sub to the list at
 * out (CandList_Insert places it; *m is the list's size, *n the count). */
void DictEntry_ListAll(u8 *entry, s32 kind, s32 sub, s32 *n, s32 *m, u8 *out, s32 flags)
{
    s32 attr;
    u8 *p;
    u8 *end;
    u8 *rec;
    s32 len;
    s32 v;

    end = entry + (u16)(entry[0] + (entry[1] << 8));
    p = entry + entry[2] + 3;
    while (p < end) {
        rec = p;
        attr = p[1];
        p += 2;
        v = *p;
        if (v < 0xC) {
            p++;
        } else {
            v = 0;
        }
        if (kind == rec[0] && sub == v) {
            len = Code2_SjisWidth(p, end) + 6;
            if (len & 1) {
                len++;
            }
            Cand_FromDictRec(CandList_Insert(out, attr, len, *m), rec, end, flags | (rec - entry));
            *m += len;
            (*n)++;
        }
        p = DictEntry_FindSense(p, end);
    }
}

/* Finds where a record of value attr goes in the list (records sorted
 * by the u16 at +2, descending; a record is 5 bytes and a string, 2-byte
 * aligned) and opens len bytes there (kept for the match: one pointer p
 * serves both loops, as in the original). */
u8 *CandList_Insert(u8 *list, s32 attr, s32 len, s32 size)
{
    u8 *end = list + size;
    u8 *p;

    while (list < end) {
        p = list + 2;
        if (*(u16 *)p < attr) {
            break;
        }
        list += 5;
        while (*list++ != 0) {
        }
        if ((u32)list & 1) {
            list++;
        }
    }
    if (list < end) {
        for (p = end - 1; p >= list; p--) {
            p[len] = *p;
        }
    }
    return list;
}

void Dict_ClearUse(u64 *refs, s32 n);
s32 Slot_MaxUse(s32 *ids, s32 n);
s32 Slot_SetUse(u64 *refs, s32 n, s32 v); /* unit's own declaration: declared as this file's callers use it; with the definition's s32 (s32 *ids, s32 n, s8 v), UserDict_AddWordNumbered compiles differently */
void UserDict_Touch(s32 pos);

/* Marks word r's record at sel (bit 15: in the user dictionary) with the
 * next use number after Slot_MaxUse(refs) (at 0xFF Dict_ClearUse clears
 * the refs' numbers and it restarts at 1). r == -1 with sel 0..3 sets
 * gNumConvMode instead. */
s32 Dict_MarkUsed(u64 r, u16 sel, u64 *refs, s32 n)
{
    s32 a;
    s32 b;
    s32 c;
    s32 k;
    u8 *entry;
    u8 *p;

    if (gDictLearn == 0) {
        return 3;
    }
    if (gDictFd == -1) {
        return -3;
    }
    if (r == -1 && sel >= 0 && sel < 4) {
        gNumConvMode = sel;
        Dict_ClearUse(refs, n);
        Slot_SetUse(refs, n, 0);
        return 3;
    }
    k = Slot_Get(r, &a, &b, &c);
    if (k == -1) {
        return 0;
    }
    if (sel & 0x8000) {
        if (b == -1) {
            return 0;
        }
        entry = UserDict_EntryAt(b);
    } else {
        if (a == -1) {
            return 0;
        }
        entry = DictPage_Get(k) + a;
    }
    c = Slot_MaxUse((s32 *) refs, n);
    p = entry + (sel & ~0x8000) + 1;
    if (*p && *p == c) {
        if (sel & 0x8000) {
            UserDict_Touch(b);
            UserDict_SetDirty();
        }
        return 3;
    }
    if (++c == 0xFF) {
        Dict_ClearUse(refs, n);
        c = 1;
    }
    *p = c;
    if (sel & 0x8000) {
        UserDict_Touch(b);
        UserDict_SetDirty();
    } else {
        DictPage_MarkDirty();
    }
    Slot_SetUse(refs, n, c);
    return 3;
}

void DictEntry_ClearUse(u8 *buf)
{
    u8 *end;
    u8 *p;

    end = buf + (u16)(buf[0] + (buf[1] << 8));
    p = buf + buf[2] + 3;
    while (p < end) {
        p[1] = 0;
        p += 2;
        if (*p <= 11) {
            p++;
        }
        p = DictEntry_FindSense(p, end);
    }
}

/* For each of the n references: DictEntry_ClearUse on its dictionary entry and on its learned word, marking them changed. */
void Dict_ClearUse(u64 *refs, s32 n)
{
    s32 a;
    s32 b;
    s32 c;
    s32 k;
    s32 i;

    for (i = 0; i < n; i++) {
        if ((k = Slot_Get(refs[i], &a, &b, &c)) != -1) {
            if (a != -1) {
                DictEntry_ClearUse(DictPage_Get(k) + a);
                DictPage_MarkDirty();
            }
            if (b != -1) {
                DictEntry_ClearUse(UserDict_EntryAt(b));
                UserDict_SetDirty();
            }
        }
    }
}

/* The largest +1 byte among the records of buf (walked as DictEntry_ClearUse does). */
s32 DictEntry_MaxUse(u8 *buf)
{
    u8 *end;
    u8 *p;
    s32 max;

    end = buf + (u16)(buf[0] + (buf[1] << 8));
    p = buf + buf[2] + 3;
    max = 0;
    while (p < end) {
        if (p[1] > max) {
            max = p[1];
        }
        p += 2;
        if (*p <= 11) {
            p++;
        }
        p = DictEntry_FindSense(p, end);
    }
    return max;
}

/* Adds word w to the dictionary: a new entry for its reading, or its
 * record appended to the reading's entry. 3, -5 when the block has no
 * room (0x3FE bytes), -3 without a dictionary, -6 when gUserDictFlags is
 * 0x8000. */
s32 Dict_AddWord(Word *w)
{
    u8 key[0x50];
    s32 r;
    u8 *start;
    u8 *e;
    u8 *end;
    s32 missing = 1;
    s16 kl;
    s32 size;

    if (gDictFd == -1) {
        return -3;
    }
    if (gUserDictFlags == 0x8000) {
        return -6;
    }
    strncpy((char *)key, (char *)w->reading, w->len);
    key[w->len] = 0;
    start = DictPage_Get(Dict_FindKey(key));
    end = (u8 *)DictBlock_End(start);
    for (e = start; e < end; e += (u16)(e[0] + (e[1] << 8))) {
        kl = e[2];
        r = Str_CompareN((char *) (e + 3), key, kl);
        if (r == 0) {
            if (kl == w->len) {
                missing = 0;
                break;
            }
        } else if (r > 0) {
            break;
        }
    }
    if (missing) {
        size = UserDict_WordSize(w);
    } else {
        size = UserDict_AppendSize(w);
    }
    if (start + 0x3FE - size < end) {
        return -5;
    }
    if (missing) {
        Mem_Shift(e, end + 2, size);
        UserDict_WriteWord(e, size, w, 0);
        if (w->len < 3) {
            Dict_SetReadingBit(key, w->len);
        }
    } else {
        Mem_Shift(e + (u16)(e[0] + (e[1] << 8)), end + 2, size);
        UserDict_AppendWord(e, size, w, DictEntry_MaxUse(e) + 1);
    }
    Slot_ClearAll();
    DictPage_MarkDirty();
    return 3;
}

u32 DictBlock_End(u8 *e)
{
    s32 len;
    u8 *p;

    p = e;
    len = (*(u8 *)((u8 *)p + 0) + (*(u8 *)((u8 *)p + 1) << 8)) & 0xFFFF;
    if (len != 0) {
        do {
            p = &p[len];
            len = (*(u8 *)((u8 *)p + 0) + (*(u8 *)((u8 *)p + 1) << 8)) & 0xFFFF;
        } while (len != 0);
    }
    return (u32) p;
}

/* Moves the bytes from start to end by shift (either way, overlap-safe). */
void Mem_Shift(u8 *start, u8 *end, s32 shift)
{
    if (shift > 0) {
        for (end--; end >= start; end--) {
            end[shift] = *end;
        }
    } else if (shift < 0) {
        for (; start < end; start++) {
            start[shift] = *start;
        }
    }
}

/* Deletes word w from the dictionary: finds the entry of its reading and
 * removes the record (DictEntry_RemoveWord), or the whole entry when it was the
 * only one. 3, 0 if absent, -3 without a dictionary, -6 when gUserDictFlags
 * is 0x8000. */
s32 Dict_DeleteWord(Word *w)
{
    u8 key[0x50];
    s32 r;
    s32 size;
    s16 kl;
    u8 *e;
    u8 *end;
    s32 missing = 1;

    if (gDictFd == -1) {
        return -3;
    }
    if (gUserDictFlags == 0x8000) {
        return -6;
    }
    strncpy((char *)key, (char *)w->reading, w->len);
    key[w->len] = 0;
    e = DictPage_Get(Dict_FindKey(key));
    end = (u8 *)DictBlock_End(e);
    for (; e < end; e += (u16)(e[0] + (e[1] << 8))) {
        kl = e[2];
        r = Str_CompareN((char *) (e + 3), key, kl);
        if (r == 0) {
            if (kl == w->len) {
                missing = 0;
                break;
            }
        } else if (r > 0) {
            break;
        }
    }
    if (missing) {
        return 0;
    }
    r = DictEntry_RemoveWord(e, w);
    if (r == 0) {
        return 0;
    }
    if (r == -1) {
        size = (u16)(e[0] + (e[1] << 8));
        Mem_Shift(e + size, end + 2, -size);
    } else {
        Mem_Shift(e + (u16)(e[0] + (e[1] << 8)) + r, end + 2, -r);
    }
    Slot_ClearAll();
    DictPage_MarkDirty();
    return 3;
}

/* Removes word w's record from a dictionary entry (the entry's size in
 * its first two bytes shrinks): the record size, 0 if none, -1 when it is
 * the entry's only record. */
s32 DictEntry_RemoveWord(u8 *entry, Word *w)
{
    u8 code[0x80];
    s32 n;
    u8 *top;
    s32 size;
    u8 *end;
    u8 *text;
    u8 *rec;
    s32 len;

    Sjis_ToCode2(w->text, code);
    len = Sjis_Code2Size(w->text);
    code[len] = 0;
    top = entry;
    size = (u16)(entry[0] + (entry[1] << 8));
    end = entry + size;
    entry = entry + entry[2] + 3;
    for (rec = entry; entry < end; rec = entry) {
        entry += 2;
        if (*entry < 0xC) {
            entry++;
        }
        text = entry;
        entry = DictEntry_FindSense(entry, end);
        n = entry - text;
        if (rec[0] == w->kind && n == len && Str_CompareN((char *) text, code, n) == 0) {
            break;
        }
    }
    len = entry - rec;
    if (len > 0) {
        if (rec == top + top[2] + 3 && entry == end) {
            return -1;
        }
        Mem_Shift(entry, end, -len);
        size -= len;
        top[0] = size % 256;
        top[1] = size / 256;
    }
    return len;
}

/* Adds word w to the learned words (UserDict_Learn), its reading as the
 * key; its +7 byte 0x28 or 0x29 becomes 0x27 first. */
s32 UserDict_AddWord(Word *w)
{
    u8 key[0x50];

    if (gDictLearn == 0) {
        return 3;
    }
    if (gDictFd == -1) {
        return -3;
    }
    strncpy((char *)key, (char *)w->reading, w->len);
    if (w->kind == 0x28 || w->kind == 0x29) {
        w->kind = 0x27;
    }
    key[w->len] = 0;
    UserDict_Learn(key, w, 1);
    return 3;
}

/* Adds word w to the learned words with the next use number after
 * Slot_MaxUse(refs) (as Dict_MarkUsed does); +7 becomes 0x1F when
 * Str_IsDigits accepts the reading. */
s32 UserDict_AddWordNumbered(Word *w, u64 *refs, s32 n)
{
    u8 key[0x50];
    s32 num;

    if (gDictLearn == 0) {
        return 3;
    }
    if (gDictFd == -1) {
        return -3;
    }
    strncpy((char *)key, (char *)w->reading, w->len);
    key[w->len] = 0;
    if (Str_IsDigits(key)) {
        w->kind = 0x1F;
    }
    num = Slot_MaxUse((s32 *) refs, n);
    if (++num == 0xFF) {
        Dict_ClearUse(refs, n);
        num = 1;
    }
    UserDict_Learn(key, w, num);
    Slot_SetUse(refs, n, num);
    return 3;
}

s32 Str_IsDigits(u8 *key)
{
    s32 temp_v1;
    u8 *p;
    u8 c;

    p = key;
    c = *p;
    if (c != 0) {
loop_2:
        temp_v1 = c & 0xFF;
        if ((temp_v1 < 0x30) || (temp_v1 > 57)) {
            return 0;
        }
        p += 1;
        c = *p;
        if (c == 0) {
            goto block_6;
        }
        goto loop_2;
    }
block_6:
    return 1;
}

s32 Conv_ReleaseSlot(s32 i)
{
    Slot_Release((u32) i);
    return 3;
}

s32 Dict_LearnEnabled(void)
{
    return gDictLearn;
}

s32 Conv_NumberText(u8 *digits, s32 len, void *out)
{
    if (gDictFd == -1) {
        return -3;
    }
    if (Conv_NumberToText((u8 *)digits, len, out, gNumConvMode) == NULL) {
        Conv_NumberToText((u8 *)digits, len, out, 0);
    }
    return 1;
}

extern u8 *gNumberDigitGlyphs[];        /* digit glyph tables (Shift-JIS pairs) by style */
extern u8 *gNumberPlaceGlyphs;          /* place-name glyphs (ten, hundred, thousand, 10^4..) */

/*
 * Writes the digit string digits (len characters) as a numeral candidate in style mode:
 * a header (s16 mode, s16 0, a zero byte), then Shift-JIS pairs: styles 0 and 1 map each
 * digit (gNumberDigitGlyphs[mode]); styles 2 and 3 write kanji numerals with the place names of
 * gNumberPlaceGlyphs (ten, hundred, thousand; 10^4 and up per group of four, skipped for empty
 * groups; no "one" before ten or hundred in style 2), at most 13 digits. Returns the end
 * past the terminating zero, or NULL when nothing was written.
 */
u8 *Conv_NumberToText(u8 *digits, s32 len, u8 *out, s32 mode)
{
    u8 *p;
    u8 *mark;
    s32 i;
    s32 rem;
    s32 d;
    s32 pos;
    s32 k;
    u8 **t;                     /* the style table slot, read again after each store */

    if (mode >= 2 && (s16)len > 13) {
        return NULL;
    }
    ((s16 *)out)[0] = mode;
    ((s16 *)out)[1] = 0;
    out[4] = 0;
    p = out + 5;
    mark = p;
    for (i = 0; i < (s16)len; i++) {
        d = digits[i] - '0';
        if (mode < 2) {
            p[0] = gNumberDigitGlyphs[mode][d * 2];
            p[1] = gNumberDigitGlyphs[mode][d * 2 + 1];
            p += 2;
            continue;
        }
        rem = (s16)len - i - 1;
        pos = rem % 4;
        if (d != 0 && !(mode == 2 && d == 1 && (u32)(pos - 1) <= 1)) {
            k = (d > 0 && d <= 3) ? mode - 1 : 1;
            t = &gNumberDigitGlyphs[k];
            p[0] = (*t)[d * 2];
            p[1] = (*t)[d * 2 + 1];
            p += 2;
        }
        if (rem == 0) {
            continue;
        }
        if (pos == 0 && mark == p) {
            continue;
        }
        if (pos != 0 && d == 0) {
            continue;
        }
        if (pos == 0) {
            k = rem / 4 + 6;
        } else if (pos == 1) {
            k = (mode == 2) ? 4 : 0;
        } else {
            k = pos + 3;
        }
        p[0] = gNumberPlaceGlyphs[k * 2];
        p[1] = gNumberPlaceGlyphs[k * 2 + 1];
        p += 2;
        if (pos == 0) {
            mark = p;
        }
    }
    if (p == out + 5) {
        return NULL;
    }
    *p = 0;
    return p + 1;
}

/* This file's initialised data. */
extern u32 gNumberDigitGlyphs_0[];
extern u32 gNumberDigitGlyphs_1[];
extern u32 D_005859AE[];
u16 gKanaByteCodes[120] = {
#include "sub_main/00403320.inc"
};
u16 gHalfKanaJis[136] = {
#include "sub_main/00403410.inc"
};
u8 gKanaKeyChars[16] = {
#include "sub_main/00403520.inc"
};
u16 gKanaKeyCodes[104] = {
#include "sub_main/00403530.inc"
};
u8 gJisHalfSymbols[47] = {
#include "sub_main/00403600.inc"
};
u32 D_00403630[36] = {
#include "sub_main/00403630.inc"
};
u8 gConvConnBits[992] = {
#include "sub_main/004036C0.inc"
};
u8 gConvConnRows[464] = {
#include "sub_main/00403AA0.inc"
};
u8 gConvConnCols[256] = {
#include "sub_main/00403C70.inc"
};
u8 gConvBitMasks[8] = {
#include "sub_main/00403D70.inc"
};
s32 gDictFd = {
#include "sub_main/00403D78.inc"
};
s32 gUserDictFlags = {
#include "sub_main/00403D80.inc"
};
u8 D_00403D90[24] = {
#include "sub_main/00403D90.inc"
};
u8 *gNumberDigitGlyphs[2] = {
    (u8 *)gNumberDigitGlyphs_0, (u8 *)gNumberDigitGlyphs_1,
};
u8 *gNumberPlaceGlyphs = D_00403D90;
u32 gDictReadingBits[2] = {
    (u32)D_005859AE, 0,
};

/* This file's read-only data. */
#pragma rodata on
u32 gNumberDigitGlyphs_0[8] = {
#include "sub_main/0046A3C0.inc"
};
u32 gNumberDigitGlyphs_1[8] = {
#include "sub_main/0046A3E0.inc"
};
#pragma rodata off
