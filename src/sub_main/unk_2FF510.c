/*
 * The kana-kanji conversion of the online text input (with the
 * dictionary's units and the node pools):
 * - the learned words: eight 0x400-byte pages at gUserDictPages (file offset
 *   0x1400) hashed by their key's first character into gUserDictHash;
 * - the per-position records at gConvRecs: for each input position its
 *   entry in the built-in table, its dictionary matches (ConvSeg), its
 *   conversion candidates (ConvCand) and their texts (ConvOut chains);
 * - the candidate line: the texts of the current segment, numbered, at
 *   gCandLine.
 */
#include "common.h"
#include "conv.h"

/* A node of the hash of the learned words (the dictionary's Node8). The
 * words are kept in the pages as a 2-byte little-endian size, the key
 * length, the key and the word (UserDict_WriteWord). */
typedef struct Node8 {
    u8 *entry;                  /* 0x0 */
    struct Node8 *next;         /* 0x4 */
} Node8;

/* A hit in the learned words. */
typedef struct UserHit {
    s16 unk0;                   /* 0x0 */
    s16 pos;                    /* 0x2: UserDict_EntryPos of the entry, -1 for none */
    s16 len;                    /* 0x4: matched length */
    s16 unk6;                   /* 0x6: longest common prefix + 1 */
    s32 unk8;
    u8 *entry;                  /* 0xC */
} UserHit;



/* One sense of a dictionary result (6 bytes). */
typedef struct MatchItem {
    u8 kind;                    /* 0x0 */
    u8 sub;                     /* 0x1 */
    u8 unk2;                    /* 0x2 */
    u8 unk3;
    u16 unk4;                   /* 0x4 */
} MatchItem;

/* A dictionary result: the matched length, the entry and its senses. */
typedef struct Match {
    s16 len;                    /* 0x00 */
    u64 entry;                  /* 0x08 */
    s16 unk10;                  /* 0x10 */
    s16 unk12;
    s16 count;                  /* 0x14 */
    MatchItem items[1];         /* 0x16 */
} Match;

/* A suffix that may follow a word (Conv_FindSuffixes; pool nodes linked at +8). */
typedef struct Suffix {
    s16 len;                    /* 0x0 */
    u8 unk2;                    /* 0x2: kind of the first table entry */
    u8 unk3;                    /* 0x3 */
    u8 kind;                    /* 0x4: kind of the last table entry */
    u8 sub;                     /* 0x5 */
    struct Suffix *next;        /* 0x8 */
} Suffix;



/* The source of a text: two codes, a kind and the text (gTextList holds
 * a list of them). */
typedef struct OutSrc {
    u16 unk0;                   /* 0x0 */
    u16 unk2;                   /* 0x2 */
    u8 kind;                    /* 0x4 */
    u8 text[1];                 /* 0x5 */
} OutSrc;

/* A candidate with its texts and their priority (0x10 bytes). */
typedef struct Choice {
    ConvCand *cand;                 /* 0x0 */
    u16 prio;                   /* 0x4 */
    ConvOut *outs;                  /* 0x8 */
    struct Choice *next;        /* 0xC */
} Choice;


/* The learned words */
extern u8 gUserDictPages[][0x400];
extern u8 gUserDictDirty;
extern u8 *gUserDictPageEnd;
extern u8 *gUserDictFree;
extern s32 gUserDictPage;
extern Node8 *gUserDictHash[0x50];
extern s32 gDictFd;

/* The conversion */
extern s32 gConvState;
extern s32 D_00581B80;
extern s32 gConvMode;
extern s32 D_00588ED8;
extern Match gConvMatch;
extern ConvRec gConvRecs[80];
extern u8 *gTextListEnd;
extern s32 gCandPageSel;
extern s32 gCandPageCount;
extern ConvOut *gCandPageFirst;
extern s32 gCandLineEnd;
extern u8 gTextList[];
extern u8 gConvSjis[];
extern u8 gCandLine[];
extern u8 gConvInput[];
extern s32 gConvInputLen;
extern u16 gConvEditLine[][4];
extern s32 gConvSegLen;
extern s32 gConvSegPos;
extern ConvSeg D_00404D80;
extern u8 D_00404D98[];
extern u8 D_00404DA0[];
extern u8 D_00404DA5[];
extern u8 gConvSuffixTails[];
extern u8 gConvSuffixes[][4];
extern s16 gConvSuffixIndex[];

s32 strlen(u8 *s);
char *strncpy(char *dst, char *src, u32 n);
s32 func_002FAFF0(void);
void EditLine_GetText(u8 *dst, s32 a, s32 b, s32 c);
s32 EditLine_ToCells(s32 start, s32 n, u16 *dst, s32 cursor);
s32 Kana_TestBit(u8 kind, u8 sub, u8 k);
s32 Dict_SenseTakesSub(u8 kind, u8 sub);
s32 Dict_LookupPrefix(u8 *s, s16 len, Match *m);
s32 Dict_LookupReading(u8 *s, s16 len, Match *m);
s32 Dict_FindBest(u64 r, u8 kind, u8 sub, u8 *out);
s32 Dict_ListAll(u64 r, u8 kind, u8 sub, u8 *out, s32 *count);
#if PLATFORM_PC
/* the definition's s32: the caller passes the entry's low word. The EE's Conv_ReleaseSlot hands its whole
 * register to Slot_Release, which tests it (sltiu a0, 0x80) and indexes with its low word; an entry is
 * Slot_Acquire's s32 sign-extended, or -1, so the low word gives the same result */
s32 Conv_ReleaseSlot(s32 i);
#define Conv_ReleaseSlot(entry) Conv_ReleaseSlot((s32)(entry))
#else
/* unit's own declaration: declared as this file's callers use it; with the definition's s32 (s32 arg0), Conv_FindMatches compiles differently */
s32 Conv_ReleaseSlot(u64 entry);
#endif
s32 Conv_NumberText(u8 *s, s32 len, void *out);
s32 Dict_CollectAll(u8 *s, s32 b, void *out, s32 *n);
s32 Str_CompareN(char *a, u8 *b, s32 n);
s32 DictIo_Read(s32 fd, void *buf, s32 n);
s32 DictIo_Write(s32 fd, void *buf, s32 n);
s32 DictIo_SeekTo(s32 pos);
s32 Str_CommonPrefix(u8 *a, u8 *b, s32 n);
void DictSlots_ClearGroup(s32 page);
void DictSlots_ClearId(s32 pos);
s32 UserDict_Bucket(u8 *s);
Node8 *UserNode_Alloc(void);
void UserNode_Free(Node8 *n);
u8 *UserDict_AllocBytes(s32 n);
s32 CandLine_CountFit(ConvOut *first);
s32 Sjis_IsLead(s32 c);
void Cells_SetPalette(u16 *p, s32 n, u16 v);
s32 Text_SjisToCells(u16 *dst, u8 *src, s32 n);
s32 Jis_IsKatakana(s32 c, s32 a);
s32 Jis_IsKanji(u16 c);
s32 Jis_IsNonKanjiNotKatakana(s32 c);
s32 Text_CellsToSjis(u16 *src, u8 *dst, s32 n);
struct Node18;
void InNode_Free(struct Node18 *n);
struct Node18 *Seg_Alloc(void);
struct Node18 *Cand_Alloc(void);
struct Node18 *Suffix_Alloc(void);
struct Node18 *Out_Alloc(void);
struct Node18 *Choice_Alloc(void);
struct SuffixLink;
void Suffix_FreeChain(struct SuffixLink *list);
struct EntryNode;
void InNode_FreeChain(struct EntryNode *list);
struct CandLink;
void Cand_FreeChain(struct CandLink *list);
struct OutLink;
void Out_FreeChain(struct OutLink *list);
void Choice_FreeChain(struct CandLink *list);
s32 Conv_PickBest(s32 pos, s32 to, s32 len);
void ConvRec_InitExtraCosts(s32 i);
u16 Cand_Prio(ConvCand *c, s32 code);
s32 KbdChar_ClassC0(s32 c);
s32 KbdChar_Class80(s32 c);
s32 KbdChar_Class40(s32 c);
s32 KbdChar_Class20(s32 c);

s32 UserDict_WordSize(Word *w);
void Sjis_ToCode2(u8 *src, u8 *dst);
s32 Sjis_Code2Size(u8 *s);
s32 Sjis_IsLeadByte(u8 c);
void ConvRec_Init(ConvRec *r);
void ConvRec_Clear(ConvRec *r, s32 keep);
s32 Conv_PlainRunLen(s32 from, s32 to);
void Conv_LookupTable(s32 from, s32 to);
s32 Conv_FindMatches(s32 pos, s32 to);
ConvSeg *Match_ToSegs(s32 unused, Match *m);
void ConvRec_AddSeg(s32 i, ConvSeg *n);
void ConvRec_AddRun(s32 i, s32 len, u16 kind);
s32 ConvRec_MakeCands(s32 i, s32 end);
ConvCand *Seg_MakeCands(s32 pos, s32 end, ConvSeg *seg);
void ConvRec_MergeCands(s32 i, ConvCand *list);
void ConvRec_KeepLen(s32 i, s32 len);
u8 *ConvTable_Lookup(u8 *s, s32 n, s32 *len);
Suffix *Conv_FindSuffixes(s32 pos, s32 end, u8 kind, u8 sub);
s32 Kana_IsSmall(u8 c);
s32 Char_IsPunct(u8 c);
void ConvRec_MakeText(s32 i, s32 len);
void Conv_MakeChoices(void);
ConvOut *Cand_MakeTexts(ConvCand *c);
void Choices_FreeFirstOuts(Choice *list);
ConvOut *Out_NewRaw(s32 len);
ConvOut *Out_NewChain(OutSrc *src, ConvSeg *seg, s32 len, ConvOut **head, ConvOut **tail);
s32 Sjis_CopyN(u8 *dst, u8 *src, s32 n);
ConvOut *Conv_MakeFormText(s32 a, s32 b, s32 c);
void Conv_AddTexts(s32 i, Choice *list);
ConvOut *Choice_TakeBest(Choice *list);
void TextList_Build(s32 i, ConvOut *o);
void Conv_AddText(s32 i, ConvOut **head, ConvOut **tail, ConvOut *chain);
s32 Conv_HasText(u8 *key, s32 len);
ConvOut *Out_Cut(ConvOut *o);
ConvOut *Out_NextChain(ConvOut *o);
ConvOut *Out_Last(ConvOut *o);
s32 Out_Count(ConvOut *o);
void CandLine_Build(void);
void Format3Digits(s32 n, u8 *buf);
s32 Out_ToCode2(s32 from, s32 n, ConvOut *o, u8 *buf);
s32 Out_TextToCode2(ConvOut *o, u8 *buf);

/* Looks key (len bytes) up in its hash bucket (sorted by key): *found gets
 * its node or NULL, the result is the link that points (or would point) to it. */
Node8 **UserDict_Find(u8 *key, s16 len, Node8 **found)
{
    s16 klen;
    Node8 *n;
    Node8 *prev;
    s32 h;
    s32 d;

    prev = NULL;
    for (n = gUserDictHash[h = UserDict_Bucket(key)]; n != NULL; n = n->next) {
        klen = n->entry[2];
        d = Str_CompareN((char *) (n->entry + 3), key, klen);
        if (d == 0) {
            if (len == klen) {
                break;
            }
        } else if (d > 0) {
            break;
        }
        prev = n;
    }
    *found = n;
    if (prev == NULL) {
        return &gUserDictHash[h];
    }
    return &prev->next;
}

/* Ends the current page of learned words and recycles the next one: its
 * words leave the hash and it becomes the page written to. */
void UserDict_RecyclePage(void)
{
    u8 *p;
    s16 klen;
    Node8 *found;
    u8 key[0x50];
    Node8 **link;

    *gUserDictFree = 0;
    gUserDictFree++;
    *gUserDictFree = 0;
    gUserDictPage = (gUserDictPage + 1) % 8;
    gUserDictFree = gUserDictPages[gUserDictPage];
    p = gUserDictFree;
    gUserDictPageEnd = p + 0x400;
    while ((u16)(p[0] + (p[1] << 8)) != 0) {
        klen = p[2];
        if (klen != 0) {
            strncpy((char *)key, (char *)p + 3, klen);
            key[(s32)klen] = 0;
            link = UserDict_Find(key, klen, &found);
            if (found->entry == p) {
                *link = found->next;
                UserNode_Free(found);
            }
        }
        p += (u16)(p[0] + (p[1] << 8));
    }
    DictSlots_ClearGroup(gUserDictPage);
}

/* The position of the learned word at p: page << 12 | offset. */
s32 UserDict_EntryPos(u8 *p)
{
    s32 d = p - (u8 *)gUserDictPages;

    return (d / 0x400) << 12 | (d % 0x400);
}

/* The learned word at position x. */
u8 *UserDict_EntryAt(s32 x)
{
    return &gUserDictPages[(s16)(x >> 12)][(s16)(x & 0xFFF)];
}

/* Loads the eight pages of learned words and hashes their words; the
 * current page's end becomes the write position. */
s32 UserDict_Load(void)
{
    Node8 **link;
    s16 klen;
    u8 *p;
    s32 i;
    u8 *page;
    Node8 *found;
    u8 key[0x50];
    Node8 *n;

    if (DictIo_SeekTo(0x1400) == -1) {
        return -1;
    }
    if (DictIo_Read(gDictFd, gUserDictPages, 0x2000) != 0x2000) {
        return -1;
    }
    for (i = 0, page = gUserDictPages[0]; i < 8; i++, page += 0x400) {
        for (p = page; (u16)(p[0] + (p[1] << 8)) != 0; p += (u16)(p[0] + (p[1] << 8))) {
            klen = p[2];
            if (klen != 0) {
                strncpy((char *)key, (char *)p + 3, klen);
                key[(s32)klen] = 0;
                link = UserDict_Find(key, klen, &found);
                n = UserNode_Alloc();
                n->entry = p;
                n->next = found;
                *link = n;
            }
        }
        if (gUserDictPage == i) {
            gUserDictFree = p;
            gUserDictPageEnd = page + 0x400;
        }
    }
    return 0;
}

/* Writes the eight pages of learned words back. */
s32 UserDict_Save(void)
{
    if (DictIo_SeekTo(0x1400) == -1) {
        return -1;
    }
    if (DictIo_Write(gDictFd, (void *)&gUserDictPages, 0x2000) != 0x2000) {
        return -1;
    }
    return 0;
}

/* The size of w as a learned word (UserDict_WriteWord). */
s32 UserDict_WordSize(Word *w)
{
    s32 n;

    if (w->unk8 != 0 || w->kind >= 0x2D) {
        n = 3;
    } else {
        n = 2;
    }
    return w->len + 3 + n + Sjis_Code2Size(w->text);
}

/* The size of the part of w that UserDict_AppendWord appends. */
s32 UserDict_AppendSize(Word *w)
{
    s32 n;

    if (w->unk8 != 0 || w->kind >= 0x2D) {
        n = 3;
    } else {
        n = 2;
    }
    return n + Sjis_Code2Size(w->text);
}

/* Writes w as a learned word of size bytes to buf: the size, the reading,
 * two or three bytes of attributes and the text in 2-byte codes. */
void UserDict_WriteWord(u8 *buf, s32 size, Word *w, s8 num)
{
    *buf++ = size % 256;
    *buf++ = size / 256;
    *buf++ = w->len;
    strncpy((char *)buf, (char *)w->reading, w->len);
    buf += w->len;
    *buf++ = w->kind;
    *buf++ = num;
    if (w->unk8 != 0 || w->kind >= 0x2D) {
        *buf++ = w->unk8;
    }
    Sjis_ToCode2(w->text, buf);
}

/* Appends w's attributes and text (size bytes) to the learned word at buf. */
void UserDict_AppendWord(u8 *buf, s32 size, Word *w, s32 arg3)
{
    s32 off = (u16)(buf[0] + (buf[1] << 8));

    size += off;
    buf[0] = size % 256;
    buf[1] = size / 256;
    buf += off;
    buf[0] = w->kind;
    buf[1] = arg3;
    buf += 2;
    if (w->unk8 != 0 || w->kind >= 0x2D) {
        *buf++ = w->unk8;
    }
    Sjis_ToCode2(w->text, buf);
}

/* Learns word w under key: its old copy (if any) is dropped and a new one
 * is written to the current page and hashed. */
s32 UserDict_Learn(u8 *key, Word *w, s8 num)
{
    Node8 **link;
    Node8 *found;
    u8 *entry;
    Node8 *n;
    s32 size;
    u8 *p;

    gUserDictDirty = 1;
    link = UserDict_Find(key, w->len, &found);
    entry = found->entry;
    if (entry[2] == w->len && Str_CompareN((char *) key, entry + 3, w->len) == 0) {
        entry[2] = 0;
        *link = found->next;
        DictSlots_ClearId(UserDict_EntryPos(found->entry));
        UserNode_Free(found);
    }
    n = UserNode_Alloc();
    p = UserDict_AllocBytes(size = UserDict_WordSize(w));
    UserDict_WriteWord(p, size, w, num);
    link = UserDict_Find(key, w->len, &found);
    n->entry = p;
    n->next = found;
    *link = n;
    return 0;
}

/* Looks for the longest learned word that is a prefix of key: 2 when key
 * itself is there, 1 with the hit in out, 0 (out->pos -1) for none. */
s32 UserDict_FindPrefix(u8 *key, s16 len, UserHit *out)
{
    u8 *best;
    Node8 *n;
    u8 *entry;
    s16 maxpre;
    s16 blen;
    s16 klen;
    s16 pre;
    s32 d;

    best = NULL;
    maxpre = out->unk6 = 0;
    blen = 0;
    out->len = 0;
    for (n = gUserDictHash[UserDict_Bucket(key)]; n != NULL; n = n->next) {
        entry = n->entry;
        klen = entry[2];
        pre = Str_CommonPrefix(entry + 3, key, klen);
        if (pre >= maxpre) {
            maxpre = pre + 1;
        }
        d = Str_CompareN((char *) (entry + 3), key, klen);
        if (d == 0) {
            blen = klen;
            if (blen == len) {
                return 2;
            }
            best = entry;
        } else if (d > 0) {
            if (Str_CompareN((char *) (entry + 3), key, len) == 0) {
                return 2;
            }
            break;
        }
    }
    if (blen == 0) {
        out->pos = -1;
        return 0;
    }
    out->unk0 = 0;
    out->pos = UserDict_EntryPos(best);
    out->len = blen;
    out->unk6 = maxpre;
    out->entry = best;
    return 1;
}

/* Looks key (len bytes) up in the learned words: 1 with the hit in out,
 * 0 (out->pos -1) when it isn't there. */
s32 UserDict_FindExact(u8 *key, s16 len, UserHit *out)
{
    Node8 *n;
    u8 *entry;
    s16 klen;
    s32 d;

    entry = NULL;
    for (n = gUserDictHash[UserDict_Bucket(key)]; n != NULL; n = n->next) {
        entry = n->entry;
        klen = entry[2];
        d = Str_CompareN((char *) (entry + 3), key, klen);
        if (d == 0) {
            if (klen == len) {
                break;
            }
        } else if (d > 0) {
            out->pos = -1;
            return 0;
        }
    }
    if (n == NULL) {
        out->pos = -1;
        return 0;
    }
    out->unk0 = 0;
    out->pos = UserDict_EntryPos(entry);
    out->entry = entry;
    return 1;
}

/* Moves the learned word at pos to the current page (unless it is there). */
void UserDict_Touch(s32 pos)
{
    s16 klen;
    s32 size;
    u8 *src;
    u8 *dst;
    u8 *entry;
    s32 page;
    Node8 *found;
    u8 key[0x50];

    if (gUserDictPage == (page = (s16)(pos >> 12))) {
        return;
    }
    entry = src = UserDict_EntryAt(pos);
    size = (u16)(src[0] + (src[1] << 8));
    dst = UserDict_AllocBytes(size);
    if (page == gUserDictPage) {
        return;
    }
    klen = src[2];
    strncpy((char *)key, (char *)src + 3, klen);
    key[(s32)klen] = 0;
    UserDict_Find(key, klen, &found);
    if (found->entry != src) {
        return;
    }
    found->entry = dst;
    while (size-- != 0) {
        *dst++ = *src++;
    }
    entry[2] = 0;
    DictSlots_ClearId(pos);
}

/* Marks the learned words as changed. */
void UserDict_SetDirty(void)
{
    gUserDictDirty = 1;
}

/* The size of the Shift-JIS string s in 2-byte codes. */
s32 Sjis_Code2Size(u8 *s)
{
    s32 n = 0;
    u8 c = *s;

    while (c != 0) {
        if (Sjis_IsLeadByte(c) != 0) {
            s += 2;
        } else {
            s += 1;
        }
        c = *s;
        n += 2;
    }
    return n;
}

/* Converts the Shift-JIS string src to 2-byte codes (0xFF before a
 * one-byte character). */
void Sjis_ToCode2(u8 *src, u8 *dst)
{
    while (*src != 0) {
        if (Sjis_IsLeadByte(*src)) {
            *dst++ = *src++;
        } else {
            *dst++ = 0xFF;
        }
        *dst++ = *src++;
    }
}

/* The width of the 2-byte codes from p while they are >= 0x39, up to end: 1 for 0xFF, else 2. */
s32 Code2_SjisWidth(u8 *p, u8 *end)
{
    s32 n = 0;

    while (p < end && *p > 0x38) {
        if (*p == 0xFF) {
            n += 1;
        } else {
            n += 2;
        }
        p += 2;
    }
    return n;
}

/* Copies the 2-byte codes from src while they are >= 0x39 (up to end), dropping 0xFF lead bytes. */
void Code2_ToSjis(u8 *dst, u8 *src, u8 *end)
{
    while (src < end && *src > 0x38) {
        if (*src == 0xFF) {
            src++;
        } else {
            *dst++ = *src++;
        }
        *dst++ = *src++;
    }
    *dst = 0;
}

s32 Sjis_IsLeadByte(u8 c)
{
    s32 v = c;

    if ((v >= 0x80 && v <= 0x9F) || (v >= 0xE0 && v < 0xFD)) {
        return 1;
    }
    return 0;
}

/* Clears all the records. */
void ConvRecs_Init(void)
{
    ConvRec *r;

    for (r = gConvRecs; r < &gConvRecs[80]; r++) {
        ConvRec_Init(r);
    }
}

void ConvRec_Init(ConvRec *r)
{
    r->tableEntry = (u8 *)-1;
    r->segs = NULL;
    r->cands = NULL;
    r->outs = NULL;
    r->unk10 = 0;
    r->fixed = 0;
    r->len = 0;
    r->fixedLen = 0;
    r->unk17 = -1;
    r->tableLen = -1;
    r->unk19 = -1;
}

/* Calls ConvRec_Clear on the records from to to - 1. */
void ConvRecs_Clear(s32 from, s32 to, s32 keep)
{
    ConvRec *r;
    ConvRec *end;

    end = &gConvRecs[to];
    r = &gConvRecs[from];
    for (; r < end; r++) {
        ConvRec_Clear(r, keep);
    }
}

/* Frees record r's candidates and texts (and with keep 0 its lookups and
 * matches too). */
void ConvRec_Clear(ConvRec *r, s32 keep)
{
    if (keep == 0) {
        r->tableEntry = (u8 *)-1;
        r->tableLen = -1;
        if (r->segs != (ConvSeg *)-1) {
            InNode_FreeChain((struct EntryNode *)r->segs);
        }
        r->segs = NULL;
        r->unk17 = -1;
        r->unk19 = -1;
        r->fixedLen = 0;
    }
    if (r->cands != NULL && r->cands != (ConvCand *)-1) {
        Cand_FreeChain((struct CandLink *)r->cands);
    }
    r->cands = NULL;
    if (r->outs != NULL) {
        Out_FreeChain((struct OutLink *)r->outs);
    }
    r->outs = NULL;
    r->unk10 = 0;
    r->fixed = 0;
    r->len = 0;
}

/* Converts the input from from to to in the given mode: looks the
 * positions up and makes their candidates, picks each segment's length
 * (len0 for the first one in modes 1 and 2) and text, then merges runs of
 * plain segments into one. */
void Conv_Convert(s32 from, s32 to, s32 mode, s32 len0)
{
    s32 k;
    ConvRec *s;
    ConvRec *r;
    s32 pos;
    s32 next;
    ConvRec *r2;
    ConvCand *c;
    s32 n;
    ConvRec *t;

    gConvMode = mode;
    Conv_LookupTable(from, to);
    if (gConvMode == 3 && D_00588ED8 != 0) {
        return;
    }
    pos = from;
    while (pos < to) {
        r = &gConvRecs[pos];
        if (r->len > 0) {
            pos += r->len;
            continue;
        }
        if (r->segs == NULL && Conv_FindMatches(pos, to) == 0) {
            break;
        }
        if (r->cands == NULL && ConvRec_MakeCands(pos, to) == 0) {
            break;
        }
        for (c = r->cands; c != NULL; c = c->next) {
            next = pos + c->len;
            if (next >= to) {
                if (gConvMode < 3) {
                    continue;
                }
                if (c->kind != 0xFF) {
                    break;
                }
                gConvMode = 0;
                continue;
            }
            r2 = &gConvRecs[next];
            if (r2->segs == NULL && Conv_FindMatches(next, to) == 0) {
                break;
            }
            if (r2->cands == NULL && ConvRec_MakeCands(next, to) == 0) {
                break;
            }
        }
        if (pos == from) {
            if (gConvMode == 1) {
                Conv_PickBest(pos, to, len0);
                n = len0;
                gConvRecs[from].fixed = 1;
                goto found;
            }
            if (gConvMode == 2) {
                gConvRecs[from].fixed = 1;
                n = len0;
                goto found;
            }
        } else if (pos == from + len0 && gConvMode == 2) {
            ConvRec_InitExtraCosts(pos);
        }
        if ((n = Conv_PickBest(pos, to, -1)) == -1) {
            break;
        }
found:
        ConvRec_KeepLen(pos, n);
        ConvRec_MakeText(pos, n);
        r->len = n;
        pos += n;
    }
    if (from < to) {
        do {
            s = &gConvRecs[from];
            k = s->len;
            t = s;
            if (s->fixed != 0) {
                n = k;
            } else {
                if ((n = Conv_PlainRunLen(from, to)) < 0) {
                    return;
                }
                if (n == 0) {
                    n = k;
                }
                if (n == k) {
                    s->fixed = 1;
                } else {
                    while (k < n) {
                        if (t->cands != (ConvCand *)-1) {
                            Cand_FreeChain((struct CandLink *)t->cands);
                        }
                        t->cands = NULL;
                        t->len = 0;
                        Out_FreeChain((struct OutLink *)t->outs);
                        t->outs = NULL;
                        t = &gConvRecs[from + k];
                        k += t->len;
                    }
                    if (t->cands != (ConvCand *)-1) {
                        Cand_FreeChain((struct CandLink *)t->cands);
                    }
                    t->cands = NULL;
                    t->len = 0;
                    Out_FreeChain((struct OutLink *)t->outs);
                    t->outs = NULL;
                    s->len = n;
                    if ((c = (ConvCand *)Cand_Alloc()) != NULL) {
                        c->len = n;
                        c->kind = 0x28;
                        c->sub = 0;
                        c->seg = NULL;
                        c->prio = 0;
                        c->extra = 0;
                        c->next = NULL;
                        s->cands = c;
                        s->fixed = 1;
                        ConvRec_MakeText(from, n);
                    }
                }
            }
            from += n;
        } while (from < to);
    }
}

/* The length of the run of segments from from (before to) whose texts are
 * plain (plus the next segment's when it ends early); -1 when a position
 * has no segment. */
s32 Conv_PlainRunLen(s32 from, s32 to)
{
    ConvRec *r;
    s32 step;
    s32 n;

    step = 0;
    r = &gConvRecs[from];
    n = 0;
    while (from < to) {
        step = r->len;
        if (step == 0) {
            return -1;
        }
        if (r->cands != NULL && r->cands != (ConvCand *)-1 && r->cands->kind != 0x28) {
            break;
        }
        from += step;
        n += step;
        r += step;
    }
    if (n == 0 || from >= to) {
        return n;
    }
    return n + step;
}

/* The length from start of the input up to the next character that can
 * start a word (before end). */
s32 Conv_UnitLen(s32 start, s32 end)
{
    s32 i;

    for (i = start + 1; i < end; i++) {
        if (Kana_IsSmall(gConvInput[i]) == 0) {
            break;
        }
    }
    return i - start;
}

/* Looks the positions from from to to up in the built-in table (those not
 * looked up yet). */
void Conv_LookupTable(s32 from, s32 to)
{
    ConvRec *r;
    s32 n;
    u8 *s;
    s32 len;
    u8 *id;

    n = to - from;
    r = &gConvRecs[from];
    s = &gConvInput[from];
    for (; n > 0; n--, s++, r++) {
        if (r->tableEntry == (u8 *)-1) {
            id = ConvTable_Lookup(s, n, &len);
            if (id == (u8 *)-1) {
                break;
            }
            r->tableLen = len;
            r->tableEntry = id;
        }
    }
}

/* Finds the matches at input position pos (before to): a run of digits,
 * letters, symbols or JIS codes of one kind, or the dictionary words the
 * input there starts with; 1 when some were added, 0 when the input ends
 * too early, -1 when there is none. */
s32 Conv_FindMatches(s32 pos, s32 to)
{
    u16 kind;
    ConvRec *r;
    u32 n;
    u8 *p;
    s32 k;
    s32 i;
    u32 m;
    u16 c;
    s8 c0;
    u8 *q;

    n = to - pos;
    if (n == 0) {
        return 0;
    }
    p = &gConvInput[pos];
    r = &gConvRecs[pos];
    kind = 0;
    if (KbdChar_Class80(p[0])) {
        kind = 0x1F;
        for (k = 1; k < n; k++) {
            if (KbdChar_Class80(p[k]) == 0) {
                break;
            }
        }
        if (gConvMode >= 3 && k >= n) {
            return 0;
        }
        goto lookup;
    }
    if (KbdChar_Class40(p[0])) {
        kind = 0x19;
        for (k = 1; k < n; k++) {
            if (KbdChar_ClassC0(p[k]) == 0) {
                break;
            }
        }
        if (gConvMode >= 3 && k >= n) {
            return 0;
        }
        goto lookup;
    }
    if (KbdChar_Class20(p[0])) {
        if (gConvMode >= 3 && n < 2) {
            return 0;
        }
        m = 1;
        k = m;
        kind = 0x19;
        goto loop;
    }
    if ((c0 = *(s8 *)p) != 0 && p[0] < 0xA0) {
        if (gConvMode >= 3 && n < 2) {
            return 0;
        }
        m = 1;
        k = m;
        kind = 0x29;
        goto loop;
    }
    if (c0 == 0 && (gConvEditLine[pos][0] & 0xFF00) == 0) {
        for (i = pos + 1; i < to; i++) {
            if (gConvEditLine[i][0] & 0xFF00) {
                break;
            }
        }
        c = 0x19;
        goto add;
    }
    if (c0 == 0 && Jis_IsKatakana(gConvEditLine[pos][0], 0) != 0) {
        for (i = pos + 1; i < to; i++) {
            if (Jis_IsKatakana(gConvEditLine[i][0], 1) == 0) {
                break;
            }
        }
        c = 0x19;
add:
        if (gConvMode >= 3 && i == to) {
            return 0;
        }
        ConvRec_AddRun(pos, i - pos, c);
        return 1;
    }
    if (*(s8 *)p == 0) {
        for (i = pos + 1, p++; i < to; i++, p++) {
            if (*(s8 *)p != 0) {
                break;
            }
        }
        if (Jis_IsKanji(gConvEditLine[i - 1][0])) {
            c = 0x28;
        } else if (Jis_IsNonKanjiNotKatakana(gConvEditLine[i - 1][0])) {
            c = 0x29;
        } else {
            c = 0x19;
        }
        goto add;
    }
    k = 1;
lookup:
    q = p;
    for (m = 0; m < n; m++) {
        if (*(s8 *)q++ == 0) {
            break;
        }
    }
    switch (Dict_LookupPrefix(p, m, &gConvMatch)) {
    case 2:
        if (gConvMode >= 3) {
            return 0;
        }
        goto loop;
    case 1:
        if (kind && gConvMatch.len <= k) {
            Conv_ReleaseSlot(gConvMatch.entry);
            m = k;
            r->unk17 = gConvMatch.unk10;
        } else {
            ConvRec_AddSeg(pos, Match_ToSegs(pos, &gConvMatch));
            r->unk17 = gConvMatch.unk10;
            m = gConvMatch.len - 1;
        }
        goto loop;
    case -3:
    case -2:
    case -1:
    case 0:
    default:
        if (kind) {
            m = k;
            goto loop;
        }
        r->segs = (ConvSeg *)-1;
        return -1;
    }
loop:
    while (m >= k) {
        if (kind && m == k) {
            ConvRec_AddRun(pos, k, kind);
        }
        if (Dict_LookupReading(p, m, &gConvMatch) == 1) {
            if (r->unk17 == -1) {
                r->unk17 = gConvMatch.unk10;
            }
            ConvRec_AddSeg(pos, Match_ToSegs(pos, &gConvMatch));
        }
        m--;
    }
    return 1;
}

/* The matches of a dictionary result, one per sense. */
ConvSeg *Match_ToSegs(s32 unused, Match *m)
{
    s16 len;
    u64 entry;
    s32 n;
    MatchItem *it;
    ConvSeg *head;
    ConvSeg *prev;
    ConvSeg *s;

    len = m->len;
    entry = m->entry;
    n = m->count;
    it = m->items;
    head = NULL;
    prev = NULL;
    while (n-- != 0) {
        if ((s = (ConvSeg *)Seg_Alloc()) == NULL) {
            break;
        }
        if (head == NULL) {
            head = s;
        }
        s->len = len;
        s->kind = it->kind;
        s->sub = it->sub;
        s->entry = entry;
        s->unk10 = it->unk4;
        s->next = NULL;
        if (prev != NULL) {
            prev->next = s;
        }
        prev = s;
        it++;
    }
    return head;
}

/* Appends n to record i's matches. */
void ConvRec_AddSeg(s32 i, ConvSeg *n)
{
    ConvSeg **link;
    ConvSeg *p;

    link = &gConvRecs[i].segs;
    for (p = *link; p != NULL; p = *link) {
        link = &p->next;
    }
    *link = n;
}

/* Adds a match of length len and this kind (with no dictionary entry) to
 * record i. */
void ConvRec_AddRun(s32 i, s32 len, u16 kind)
{
    ConvSeg *s;

    if ((s = (ConvSeg *)Seg_Alloc()) != NULL) {
        s->len = len;
        s->kind = kind;
        s->sub = 0;
        s->entry = -1;
        s->unk10 = 0;
        s->next = NULL;
        gConvRecs[i].unk17 = len + 1;
        ConvRec_AddSeg(i, s);
    }
}

/* Makes record i's candidates from its matches (and a default match): 1
 * when it has some (a plain one is added when none was found), 0 when
 * making them failed, -1 when the pool ran out. */
s32 ConvRec_MakeCands(s32 i, s32 end)
{
    ConvRec *r = &gConvRecs[i];
    ConvSeg *s;
    ConvCand *list;

    s = r->segs;
    if (s != (ConvSeg *)-1) {
        for (; s != NULL; s = s->next) {
            list = Seg_MakeCands(i, end, s);
            if (list == (ConvCand *)-1) {
                if (r->cands != NULL) {
                    Cand_FreeChain((struct CandLink *)r->cands);
                    r->cands = NULL;
                }
                return 0;
            }
            if (list != NULL) {
                ConvRec_MergeCands(i, list);
            }
        }
    }
    list = Seg_MakeCands(i, end, &D_00404D80);
    if (list == (ConvCand *)-1) {
        if (r->cands != NULL) {
            Cand_FreeChain((struct CandLink *)r->cands);
            r->cands = NULL;
        }
        return 0;
    }
    if (list != NULL) {
        ConvRec_MergeCands(i, list);
    }
    if (r->cands == NULL) {
        if ((list = (ConvCand *)Cand_Alloc()) == NULL) {
            return -1;
        }
        list->len = Conv_UnitLen(i, end);
        list->kind = 0x28;
        list->sub = 0;
        list->seg = NULL;
        list->prio = 0;
        list->extra = 0;
        list->next = NULL;
        r->cands = list;
        return 1;
    }
    return 1;
}

/* The candidates for match seg at pos: the word with each suffix that can
 * follow it, then the word alone when it can end there (-1 when
 * Conv_FindSuffixes fails). */
ConvCand *Seg_MakeCands(s32 pos, s32 end, ConvSeg *seg)
{
    s32 next;
    s32 len;
    Suffix *sfx;
    Suffix *list;
    ConvCand *head;
    ConvCand *tail;
    ConvCand *c;

    head = NULL;
    tail = NULL;
    len = seg->len;
    next = pos + len;
    sfx = Conv_FindSuffixes(next, end, seg->kind, seg->sub);
    if (sfx == (Suffix *)-1) {
        return (ConvCand *)-1;
    }
    list = sfx;
    for (; sfx != NULL; sfx = sfx->next) {
        if ((c = (ConvCand *)Cand_Alloc()) == NULL) {
            break;
        }
        if (head == NULL) {
            head = c;
        }
        c->len = len + sfx->len;
        c->kind = sfx->kind;
        c->sub = sfx->sub;
        c->seg = seg;
        c->prio = 0;
        c->extra = 0;
        c->next = NULL;
        if (tail != NULL) {
            tail->next = c;
        }
        tail = c;
    }
    Suffix_FreeChain((struct SuffixLink *)list);
    if (len > 0 && Dict_SenseTakesSub(seg->kind, seg->sub) != 0) {
        if (next >= end || Kana_IsSmall(gConvInput[next]) == 0) {
            if ((c = (ConvCand *)Cand_Alloc()) != NULL) {
                if (head == NULL) {
                    head = c;
                }
                c->len = len;
                c->kind = seg->kind;
                c->sub = seg->sub;
                c->seg = seg;
                c->prio = 0;
                c->extra = 0;
                c->next = NULL;
                if (tail != NULL) {
                    tail->next = c;
                }
            }
        }
    }
    return head;
}

/* Inserts c into the list at head (longest first); returns the new head. */
static ConvCand *Cand_InsertSorted(ConvCand *head, ConvCand *c)
{
    ConvCand *p;
    ConvCand *prev;
    s32 len = c->len;

    if (head == NULL || head->len < len) {
        c->next = head;
        return c;
    }
    prev = head;
    for (p = head->next; p != NULL; p = p->next) {
        if (p->len < len) {
            break;
        }
        prev = p;
    }
    prev->next = c;
    c->next = p;
    return head;
}

/* Inserts the candidates of list into record i's sorted list. */
void ConvRec_MergeCands(s32 i, ConvCand *list)
{
    ConvCand *head = gConvRecs[i].cands;
    ConvCand *next;

    while (list != NULL) {
        next = list->next;
        head = Cand_InsertSorted(head, list);
        list = next;
    }
    gConvRecs[i].cands = head;
}

/* Frees record i's candidates whose length isn't len. */
void ConvRec_KeepLen(s32 i, s32 len)
{
    ConvCand **link = &gConvRecs[i].cands;
    ConvCand *c;

    for (c = *link; c != NULL; c = *link) {
        if (c->len == len) {
            link = &c->next;
        } else {
            *link = c->next;
            InNode_Free((struct Node18 *)c);
        }
    }
}

/* The length of the segment at i (0 past the input, and in mode 2 while
 * it isn't fixed). */
s32 ConvRec_SegLen(s32 i)
{
    ConvRec *r;

    if (i >= gConvInputLen) {
        return 0;
    }
    r = &gConvRecs[i];
    if (D_00581B80 == 2 && r->fixed == 0) {
        return 0;
    }
    return r->len;
}

/*
 * Keeps the length of the segment at i (ConvRec.fixedLen) when it is first fixed.
 */
void ConvRec_NoteFixedLen(s32 i)
{
    ConvRec *r = &gConvRecs[i];

    if (r->fixedLen == 0 && r->fixed != 0) {
        r->fixedLen = r->len;
    }
}

/* Looks the start of s (n bytes) up in the built-in table: the entry of
 * the longest string that s starts with (0 for none, -1 when s ends inside
 * one in modes 3 and up); *len gets the length matched. */
u8 *ConvTable_Lookup(u8 *s, s32 n, s32 *len)
{
    s32 best;
    u8 *e;
    u8 *q;
    u8 *p;
    s32 m;
    s32 left;
    u8 *first;
    s32 k;
    s32 c;
    u8 i;

    best = 1;
    c = *s;
    if (c < 0xA0) {
        *len = 1;
        return NULL;
    }
    i = (u8)c - 0xA0;
    e = gConvSuffixes[gConvSuffixIndex[i + 1]] - 4;
    first = gConvSuffixes[gConvSuffixIndex[i]];
    for (; e >= first; e -= 4) {
        p = s + 1;
        left = n - 1;
        k = 1;
        q = &gConvSuffixTails[e[2]];
        m = *q++ - 1;
        for (;;) {
            if (m == 0) {
                if (best < k) {
                    best = k;
                }
                *len = best;
                return e;
            }
            if (left == 0) {
                if (gConvMode < 3) {
                    break;
                }
                return (u8 *)-1;
            }
            k++;
            if (*q != *p) {
                if (best < k) {
                    best = k;
                }
                break;
            }
            p++;
            q++;
            left--;
            m--;
        }
    }
    *len = best;
    return NULL;
}

/* The suffixes that can follow a word of this kind at pos (before end),
 * from the table entries matched there, recursively; -1 when the input
 * ends inside one (modes 3 and up) or a position has no lookup yet. */
Suffix *Conv_FindSuffixes(s32 pos, s32 end, u8 kind, u8 sub)
{
    ConvRec *r;
    u8 *e;
    Suffix *list;
    s32 n;
    Suffix *s;
    Suffix *t;

    list = NULL;
    if (pos >= end) {
        return gConvMode < 3 ? NULL : (Suffix *)-1;
    }
    r = &gConvRecs[pos];
    e = r->tableEntry;
    if (e == (u8 *)-1) {
        return (Suffix *)-1;
    }
    if (e == NULL && Char_IsPunct(gConvInput[pos]) != 0) {
        if (kind == 0 || Dict_SenseTakesSub(kind, sub) != 0) {
            if ((s = (Suffix *)Suffix_Alloc()) != NULL) {
                s->len = 1;
                s->kind = 0xFF;
                s->unk2 = 0xFF;
                s->sub = 0;
                s->unk3 = 0;
                s->next = NULL;
            }
            return s;
        }
    }
    while (e != NULL) {
        if (Dict_SenseTakesSub(e[0], e[1]) != 0 && (kind == 0 || Kana_TestBit(kind, sub, e[0]) != 0)) {
            n = gConvSuffixTails[e[2]];
            if (pos + n >= end || Kana_IsSmall(gConvInput[pos + n]) == 0) {
                if ((s = (Suffix *)Suffix_Alloc()) != NULL) {
                    s->len = n;
                    s->unk2 = s->kind = e[0];
                    s->unk3 = s->sub = e[1];
                    s->next = list;
                    list = s;
                }
            }
        }
        if ((s8)e[3] == 0) {
            break;
        }
        e -= (s8)e[3] * 4;
    }
    for (e = r->tableEntry; e != NULL; e -= (s8)e[3] * 4) {
        if (kind == 0 || Kana_TestBit(kind, sub, e[0]) != 0) {
            n = gConvSuffixTails[e[2]];
            s = Conv_FindSuffixes(pos + n, end, e[0], e[1]);
            if (s == (Suffix *)-1) {
                Suffix_FreeChain((struct SuffixLink *)list);
                return (Suffix *)-1;
            }
            if (s != NULL) {
                for (t = s; ; t = t->next) {
                    t->len = t->len + n;
                    t->unk2 = e[0];
                    t->unk3 = e[1];
                    if (t->next == NULL) {
                        break;
                    }
                }
                t->next = list;
                list = s;
            }
        }
        if ((s8)e[3] == 0) {
            break;
        }
    }
    return list;
}

/* 1 for the small kana codes (which can't start a word); always 0 in modes 1 and 2. */
s32 Kana_IsSmall(u8 c)
{
    if (gConvMode == 1 || gConvMode == 2) {
        return 0;
    }
    switch (c) {
    case 0x9D:
    case 0xA1:
    case 0xA3:
    case 0xA5:
    case 0xA7:
    case 0xA9:
    case 0xC3:
    case 0xE3:
    case 0xE5:
    case 0xE7:
    case 0xEE:
    case 0xF2:
    case 0xF3:
        return 1;
    }
    return 0;
}

/* 1 for the punctuation codes (space ! , . : ; ? and 0x98, 0x9B, 0x9C). */
s32 Char_IsPunct(u8 c)
{
    if (c >= 0xA0) {
        return 0;
    }
    switch (c) {
    case 0x20:
    case 0x21:
    case 0x2C:
    case 0x2E:
    case 0x3A:
    case 0x3B:
    case 0x3F:
    case 0x98:
    case 0x9B:
    case 0x9C:
        return 1;
    }
    return 0;
}

/* Gives record i a text for its best candidate of length len when it has
 * none (a raw node when there is no candidate or no conversion). */
void ConvRec_MakeText(s32 i, s32 len)
{
    ConvRec *r;
    ConvCand *c;
    ConvCand *best;
    u16 bprio;
    u16 prio;
    ConvSeg *s;
    ConvOut *h;
    ConvOut *t;

    best = NULL;
    bprio = 0;
    r = &gConvRecs[i];
    if (r->outs != NULL) {
        return;
    }
    c = r->cands;
    if (c == (ConvCand *)-1) {
raw:
        r->outs = Out_NewRaw(len);
        return;
    }
    for (; c != NULL; c = c->next) {
        if (c->len == len) {
            prio = c->seg == NULL ? 0 : Cand_Prio(c, c->seg->unk10);
            if (prio > bprio || best == NULL) {
                bprio = prio;
                best = c;
            }
        }
    }
    if (best == NULL) {
        goto raw;
    }
    s = best->seg;
    if (s == NULL || s->len == 0) {
        goto raw;
    }
    if (s->entry == -1) {
        if (s->kind != 0x1F || Conv_NumberText(&gConvInput[i], s->len, gTextList) <= 0) {
            goto raw;
        }
    } else if (Dict_FindBest(s->entry, s->kind, s->sub, gTextList) <= 0) {
        goto raw;
    }
    r->outs = Out_NewChain((OutSrc *)gTextList, s, s->len, &h, &t);
}

/* Selects candidate pos of the current segment: finds the page of the
 * candidate line that holds it (gCandPageFirst its first text, gCandPageCount its
 * size, gCandPageSel the index in it); mode 1 rebuilds the texts first and
 * redraws the line. */
void CandLine_Select(s32 pos, s32 mode)
{
    ConvOut *o;
    s32 n;

    if (mode == 1) {
        Conv_MakeChoices();
    }
    o = gConvRecs[gConvSegPos].outs;
    if (gConvState > 0) {
        gCandLineEnd = 0x50;
    } else {
        gCandLineEnd = func_002FAFF0();
    }
    n = CandLine_CountFit(o);
    while (pos >= n) {
        pos -= n;
        while (n-- != 0) {
            o = Out_NextChain(o);
        }
        if ((n = CandLine_CountFit(o)) == 0) {
            CandLine_Select(0, 0);
            goto done;
        }
    }
    gCandPageFirst = o;
    gCandPageSel = pos;
    gCandPageCount = n;
done:
    if (mode == 1 && gConvState == 0) {
        CandLine_Build();
    }
}

/* Makes the choices for the candidates of the current segment that have
 * the current length and adds their texts to its list. */
void Conv_MakeChoices(void)
{
    ConvRec *r = &gConvRecs[gConvSegPos];
    Choice *ch;
    Choice *head;
    Choice *tail;
    ConvCand *c = r->cands;
    ConvOut *o;

    if (c == (ConvCand *)-1) {
        return;
    }
    head = NULL;
    tail = NULL;
    for (; c != NULL; c = c->next) {
        if (c->len == gConvSegLen) {
            if ((ch = (Choice *)Choice_Alloc()) == NULL) {
                Choices_FreeFirstOuts(head);
                head = NULL;
                break;
            }
            o = Cand_MakeTexts(c);
            ch->outs = o;
            ch->cand = c;
            ch->prio = o == NULL ? 0 : Cand_Prio(c, o->unkE);
            ch->next = NULL;
            if (head == NULL) {
                tail = ch;
                head = ch;
            } else {
                tail->next = ch;
                tail = ch;
            }
        }
    }
    Conv_AddTexts(gConvSegPos, head);
    Choice_FreeChain((struct CandLink *)head);
}

/* Builds the texts of candidate c from its dictionary match (a raw node
 * when there is none). */
ConvOut *Cand_MakeTexts(ConvCand *c)
{
    s32 count;
    ConvOut *h;
    ConvOut *t;
    ConvSeg *s;
    ConvOut *tail;
    u8 *p;
    ConvOut *head;

    s = c->seg;
    if (s == NULL || s->len == 0) {
raw:
        return Out_NewRaw(c->len);
    }
    if (s->entry == -1) {
        if (s->kind != 0x1F || Dict_CollectAll(&gConvInput[gConvSegPos], s->len, gTextList, &count) <= 0) {
            goto raw;
        }
    } else if (Dict_ListAll(s->entry, s->kind, s->sub, gTextList, &count) <= 0) {
        goto raw;
    }
    tail = NULL;
    head = NULL;
    p = gTextList;
    while (count > 0) {
        if (Out_NewChain((OutSrc *)p, s, s->len, &h, &t) == NULL) {
            Out_FreeChain((struct OutLink *)head);
            return NULL;
        }
        if (head == NULL) {
            head = h;
            tail = t;
        } else {
            tail->next = h;
            tail = t;
        }
        p += 5;
        while (*p++ != 0) {
        }
        if ((u32)p & 1) {
            p++;
        }
        count--;
    }
    return head;
}

/* Frees the first node of each choice's texts. */
void Choices_FreeFirstOuts(Choice *list)
{
    while (list != NULL) {
        InNode_Free((struct Node18 *)list->outs);
        list = list->next;
    }
}

/* A raw node (flags 0x80, no text) standing for len input characters. */
ConvOut *Out_NewRaw(s32 len)
{
    ConvOut *n = (void *)Out_Alloc();

    if (n != NULL) {
        n->flags = 0x80;
        n->text[0] = 0;
        n->len = len;
        n->kind = 0;
        n->seg = NULL;
        n->unkC = 0xFFFF;
        n->next = NULL;
    }
    return n;
}

/* Builds a chain holding src's text: the first node keeps the codes, seg
 * and len; returns it (NULL when the pool ran out). */
ConvOut *Out_NewChain(OutSrc *src, ConvSeg *seg, s32 len, ConvOut **head, ConvOut **tail)
{
    u8 *s = src->text;
    s32 size = strlen(s);
    ConvOut *n;
    ConvOut *m;
    s32 k;

    if ((n = (void *)Out_Alloc()) == NULL) {
        return NULL;
    }
    n->flags = 0;
    k = Sjis_CopyN(n->text, s, 4);
    n->len = len;
    size -= k;
    s += k;
    n->kind = src->kind;
    n->seg = seg;
    n->unkC = src->unk0;
    n->unkE = src->unk2;
    n->next = NULL;
    *head = n;
    while (size > 0) {
        if ((m = (void *)Out_Alloc()) == NULL) {
            Out_FreeChain((struct OutLink *)*head);
            return NULL;
        }
        n->flags |= 1;
        m->flags = 2;
        k = Sjis_CopyN(m->text, s, 0xE);
        m->next = NULL;
        size -= k;
        n->next = m;
        s += k;
        n = m;
    }
    *tail = n;
    return *head;
}

/* Copies up to n bytes of the Shift-JIS string src to dst (not splitting a
 * character) and ends it; returns the bytes copied. */
s32 Sjis_CopyN(u8 *dst, u8 *src, s32 n)
{
    s32 n0 = n;
    s32 c;

    while ((c = *src) && n > 0) {
        if (Sjis_IsLead(c)) {
            if (n <= 1) {
                break;
            }
            *dst++ = *src++;
            n--;
        }
        *dst++ = *src++;
        n--;
    }
    *dst = 0;
    return n0 - n;
}

/* A chain holding the text EditLine_GetText makes of a, b and c. */
ConvOut *Conv_MakeFormText(s32 a, s32 b, s32 c)
{
    ConvOut *head;
    ConvOut *tail;
    OutSrc *src = (OutSrc *)gTextList;

    src->unk0 = 0xFFFF;
    src->unk2 = 0;
    src->kind = 0;
    EditLine_GetText(src->text, a, b, c);
    return Out_NewChain(src, NULL, b, &head, &tail);
}

/* Replaces the current segment's texts with the one Conv_MakeFormText makes. */
void Conv_SetSegmentForm(s32 c)
{
    ConvRec *r = &gConvRecs[gConvSegPos];

    Out_FreeChain((struct OutLink *)r->outs);
    r->outs = Conv_MakeFormText(gConvSegPos, gConvSegLen, c);
}

/* Adds the texts of the choices in list (best first, without duplicates)
 * and a raw one to segment i's texts. */
void Conv_AddTexts(s32 i, Choice *list)
{
    ConvRec *r = &gConvRecs[i];
    ConvOut *head = r->outs;
    ConvOut *tail;
    ConvOut *o;

    tail = Out_Last(head);
    TextList_Build(i, head);
    while ((o = Choice_TakeBest(list)) != NULL) {
        Conv_AddText(i, &head, &tail, o);
    }
    if ((o = Out_NewRaw(gConvSegLen)) != NULL) {
        Conv_AddText(i, &head, &tail, o);
    }
    r->outs = head;
}

/* Takes the first text of the choice with the highest priority out of list. */
ConvOut *Choice_TakeBest(Choice *list)
{
    ConvOut *o;
    Choice *found = NULL;
    u16 best = 0;

    for (; list != NULL; list = list->next) {
        if (list->outs != NULL) {
            if (found == NULL || list->prio > best) {
                best = list->prio;
                found = list;
            }
        }
    }
    if (found == NULL) {
        return NULL;
    }
    o = found->outs;
    found->outs = Out_Cut(o);
    found->prio = found->outs == NULL ? 0 : Cand_Prio(found->cand, found->outs->unkE);
    return o;
}

/* Rebuilds the list of the texts from o at gTextList (each a length and
 * the bytes). */
void TextList_Build(s32 i, ConvOut *o)
{
    s32 n;
    s32 len;

    gTextListEnd = gTextList;
    while (o != NULL) {
        n = Out_ToCode2(i, gConvSegLen, o, gCandLine);
        len = Text_CellsToSjis((u16 *) gCandLine, gTextListEnd + 1, n);
        *gTextListEnd = len;
        gTextListEnd++;
        gTextListEnd += len;
        o = Out_NextChain(o);
    }
}

/* Appends chain to the list head..tail unless its text is already there
 * (then the chain is freed); new texts are also kept at gTextList. */
void Conv_AddText(s32 i, ConvOut **head, ConvOut **tail, ConvOut *chain)
{
    s32 len;

    len = Text_CellsToSjis((u16 *) gCandLine, gConvSjis, Out_ToCode2(i, gConvSegLen, chain, gCandLine));
    if (Conv_HasText(gConvSjis, len)) {
        Out_FreeChain((struct OutLink *)chain);
        return;
    }
    if (gTextListEnd + len + 1 <= &gTextList[0x400]) {
        *gTextListEnd = len;
        gTextListEnd++;
        strncpy((char *)gTextListEnd, (char *)gConvSjis, len);
        gTextListEnd += len;
    }
    if (*head == NULL) {
        *tail = chain;
        *head = chain;
    } else {
        (*tail)->next = chain;
    }
    *tail = Out_Last(chain);
}

/* 1 when key (len bytes) is in the list of texts at gTextList. */
s32 Conv_HasText(u8 *key, s32 len)
{
    u8 *p;
    s32 n;

    for (p = gTextList; p < gTextListEnd; p += n) {
        n = *p++;
        if (n == len && Str_CompareN((char *) p, key, n) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Cuts the list after the first chain; returns the rest. */
ConvOut *Out_Cut(ConvOut *o)
{
    ConvOut *rest;

    while (o->flags & 1) {
        o = o->next;
    }
    rest = o->next;
    o->next = NULL;
    return rest;
}

/* The chain after the first one. */
ConvOut *Out_NextChain(ConvOut *o)
{
    while (o->flags & 1) {
        o = o->next;
    }
    return o->next;
}

/* The last node of the list (NULL for none). */
ConvOut *Out_Last(ConvOut *o)
{
    ConvOut *next;

    if (o == NULL) {
        return NULL;
    }
    for (;;) {
        next = o->next;
        if (next == NULL) {
            break;
        }
        o = next;
    }
    return o;
}

/* The number of chains in the list from o. */
s32 Out_Count(ConvOut *o)
{
    s32 count = 0;

    while (o != NULL) {
        o = Out_NextChain(o);
        count++;
    }
    return count;
}

/* The width of chain o on the candidate line. */
s32 Out_ChainWidth(ConvOut *o)
{
    s32 len;

    if (o->flags == 0x80) {
        return gConvSegLen * 2;
    }
    len = (gConvSegLen - o->len) * 2;
    while (o->flags & 1) {
        len += strlen(o->text);
        o = o->next;
    }
    return len + strlen(o->text);
}

/* The chain n chains after o. */
ConvOut *Out_Nth(ConvOut *o, s32 n)
{
    while (n-- != 0) {
        o = Out_NextChain(o);
    }
    return o;
}

/* Builds the candidate line at gCandLine: the count left, then each text
 * of the current page numbered from 1 (the selected one highlighted),
 * padded with spaces to the line width. */
void CandLine_Build(void)
{
    s32 n;
    u8 *p;
    s32 i;
    ConvOut *o;
    s32 pos;

    Format3Digits(Out_Count(gCandPageFirst) - gCandPageSel - 1, D_00404DA5);
    Text_SjisToCells((u16 *) gCandLine, D_00404DA0, 0);
    pos = 10;
    o = gCandPageFirst;
    for (i = 0; i < gCandPageCount; i++) {
        D_00404D98[2] = i + '1';
        pos += Text_SjisToCells((u16 *) (&gCandLine[pos * 2]), D_00404D98, 0);
        p = &gCandLine[pos * 2];
        n = Out_ToCode2(gConvSegPos, gConvSegLen, o, p);
        if (i == gCandPageSel) {
            Cells_SetPalette((u16 *) p, n, 7);
        } else {
            Cells_SetPalette((u16 *) p, n, 0);
        }
        pos += n;
        if ((o = Out_NextChain(o)) == NULL) {
            break;
        }
    }
    for (; pos < gCandLineEnd; pos++) {
        Text_SjisToCells((u16 *) (&gCandLine[pos * 2]), (u8 *)" ", 0);
    }
}

/* Writes n (at least 0) as three digits, leading zeros as spaces. */
void Format3Digits(s32 n, u8 *buf)
{
    s32 d;

    if (n < 0) {
        n = 0;
    }
    d = n / 100;
    if (d != 0) {
        *buf++ = d + '0';
    } else {
        *buf++ = ' ';
    }
    n %= 100;
    d = n / 10;
    if (d != 0 || buf[-1] != ' ') {
        *buf++ = d + '0';
    } else {
        *buf++ = ' ';
    }
    n %= 10;
    *buf = n + '0';
}

/* Writes chain o's text (the input from from on when it is raw) to buf as
 * 2-byte codes; returns their count. */
s32 Out_ToCode2(s32 from, s32 n, ConvOut *o, u8 *buf)
{
    s32 k;

    if (o == NULL || (o->flags & 0x80)) {
        return EditLine_ToCells(from, n, (u16 *) buf, 0);
    }
    k = Out_TextToCode2(o, buf);
    buf += k * 2;
    k += EditLine_ToCells(from + o->len, n - o->len, (u16 *) buf, 0);
    return k;
}

/* Writes the text of chain o to buf as 2-byte codes; returns their count. */
s32 Out_TextToCode2(ConvOut *o, u8 *buf)
{
    s32 n = 0;

    for (;;) {
        n += Text_SjisToCells((u16 *) (buf + n * 2), o->text, 6);
        if (!(o->flags & 1)) {
            break;
        }
        o = o->next;
    }
    return n;
}

/* This file's initialised data. */
u8 gConvSuffixTails[256] = {
#include "sub_main/004045C0.inc"
};
u8 gConvSuffixes[384][4] = {
#include "sub_main/004046C0.inc"
};
s16 gConvSuffixIndex[96] = {
#include "sub_main/00404CC0.inc"
};
ConvSeg D_00404D80 = {
#include "sub_main/00404D80.inc"
};
u8 D_00404D98[8] = {
#include "sub_main/00404D98.inc"
};
u8 D_00404DA0[16] = {
#include "sub_main/00404DA0.inc"
};
