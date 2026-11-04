#include "common.h"
#include "em.h"
#include "conv.h"

/* prototypes */
s32 Out_ToCode2(s32 from, s32 n, ConvOut *o, u8 *buf);
s32 Text_CellsToSjis(u16 *src, u8 *dst, s32 n);
s32 Input_BuildLine(void);
void strncpy(char *dst, const char *src, u32 n);
extern u8 gKbdCharClass[];
extern s8 gNewWordText;
extern s8 gNewWordReading;
extern u8 gConvSjis[];
extern u16 gCandLine;
extern u8 gConvInput[];
extern u8 gLearnReading;
extern u8 gLearnText;
u32 strlen(const void *s);
extern Word gNewWord;
s32 Conv_LinkCost(ConvCand *n, ConvCand *prev);
/* end of prototypes */

extern ConvRec gConvRecs[];
s32 Cand_BestCost(ConvCand *r, s32 base, s32 end);
void Cand_SetNextCosts(ConvCand *r, s32 base, s32 end);

/* Scores the candidates at pos (only those len long when len >= 0) by Cand_BestCost up to end, then takes
 * the one with the highest best cost, sets its neighbours' extra costs (Cand_SetNextCosts) and returns its
 * length; -1 on failure or when there is none. */
s32 Conv_PickBest(s32 pos, s32 end, s32 len)
{
    ConvCand *n;
    ConvCand *best;
    ConvCand *m;
    ConvRec *rec = &gConvRecs[pos];

    for (n = rec->cands; n != NULL; n = n->next) {
        if (len < 0 || n->len == len) {
            if (Cand_BestCost(n, pos, end) == -1) {
                return -1;
            }
        }
    }
    if (rec == NULL || (best = rec->cands) == NULL) {
        return -1;
    }
    for (m = best->next; m != NULL; m = m->next) {
        if (m->prio > best->prio) {
            best = m;
        }
    }
    Cand_SetNextCosts(best, pos, end);
    return best->len;
}

/* The cost of conversion node n after prev (NULL at the start, which adds
 * 50): its +0xA, 16 times its +0 and 17 times prev's +0 (n's when first),
 * and Conv_LinkCost's cost. */
s32 Conv_NodeCost(s32 pos, ConvCand *n, ConvCand *prev)
{
    s32 x;
    s32 y;
    s32 first;
    s32 cost;

    if (prev == NULL) {
        x = 0;
        y = n->len;
        first = 1;
    } else {
        x = n->len;
        y = prev->len;
        first = 0;
    }
    cost = n->extra + (x * 16 + y * 17);
    return cost + Conv_LinkCost(n, prev) + first * 50;
}

extern s32 gConvMode;
s32 Cand_IsTextSeg(ConvCand *n);

/* Sets r's best cost (+8): the highest Conv_NodeCost of r after each candidate at position base + r's
 * length (after nothing at the end or for kind 0xFF), 0 when r is plain text (Cand_IsTextSeg). -1 when there is no
 * candidate there and gConvMode >= 3, else 0. */
s32 Cand_BestCost(ConvCand *r, s32 base, s32 end)
{
    ConvCand *m;
    s32 k;
    u16 best;
    u16 c;
    s32 len;

    if (Cand_IsTextSeg(r) != 0) {
        r->prio = 0;
        return 0;
    }
    len = r->len;
    if (r->kind == 0xFF || (k = base + len) >= end) {
        best = Conv_NodeCost(base, r, NULL);
    } else {
        m = gConvRecs[k].cands;
        if (m == NULL && gConvMode >= 3) {
            return -1;
        }
        best = 0;
        for (; m != NULL; m = m->next) {
            c = Conv_NodeCost(base, r, m);
            if (best < c) {
                best = c;
            }
        }
    }
    r->prio = best;
    return 0;
}

void ConvRec_InitExtraCosts(s32 i)
{
    ConvRec *rec = &gConvRecs[i];
    ConvCand *n;

    for (n = rec->cands; n != NULL; n = n->next) {
        ConvSeg *m;

        n->extra = 0;
        m = n->seg;
        if (m != NULL && m->kind == 0x19 && m->len == 0) {
            n->extra = 10;
        }
    }
}

/* Sets the extra cost (+0xA) of each candidate at position base + r's length (when before end) by
 * Conv_LinkCost against r, skipping the plain text ones (Cand_IsTextSeg; r's kind 0xFF: nothing). */
void Cand_SetNextCosts(ConvCand *r, s32 base, s32 end)
{
    s32 len = r->len;
    ConvCand *m;
    s32 k;
    s32 c;

    if (r->kind == 0xFF || (k = base + len) >= end) {
        return;
    }
    m = gConvRecs[k].cands;
    if (m == NULL) {
        return;
    }
    while (m != NULL) {
        m->extra = 0;
        if (Cand_IsTextSeg(m) == 0) {
            c = Conv_LinkCost(r, m);
            if (c > 0) {
                m->extra = c;
            }
        }
        m = m->next;
    }
}

/*
 * 1 when candidate n's segment is plain text (ConvSeg.kind 0x28 or 0x29), else 0.
 */
s32 Cand_IsTextSeg(ConvCand *n)
{
    u8 kind;
    ConvSeg *seg;

    seg = n->seg;
    if ((seg != NULL) && ((kind = seg->kind, (kind == 0x28)) || (kind == 0x29))) {
        return 1;
    }
    return 0;
}

s32 Conv_ClassPairCost(u8 a, u8 b, u8 c, s32 cost);
s32 Conv_PosPairBonus(u8 a, u8 b, s32 c);
s32 Conv_ClassCostLinked(ConvCand *n, u8 a, u8 b, s32 cost);
s32 Conv_KindClassCost(u8 k, u8 b, u8 c, s32 cost, s32 extra);

/* The connection cost of candidate n after prev: 0 when prev's word has class 0x28; +20 for some prev
 * classes, +1..+10 by n's own linked word, then the class-pair costs of Conv_ClassPairCost, Conv_KindClassCost,
 * Conv_PosPairBonus or Conv_ClassCostLinked by n's class. */
s32 Conv_LinkCost(ConvCand *n, ConvCand *prev)
{
    s32 cost = 0;
    u8 cls;
    u8 sub;
    u8 k;
    u8 pc;
    ConvSeg *w;

    sub = k = 0;
    if ((cls = n->kind) >= 0x2D && cls < 0x39) {
        k = n->sub;
    } else {
        sub = n->sub;
    }
    if (prev == NULL) {
        pc = 0;
    } else if ((w = prev->seg) == NULL) {
        pc = 0;
    } else {
        if ((pc = w->kind) == 0x28) {
            return 0;
        }
        if (pc == 0x29 || (pc == 0x19 && w->len > 0) || (pc == 0x1F && w->entry == -1)) {
            cost += 20;
        }
    }
    if ((w = n->seg) != NULL) {
        if (w->kind == 0x19 && w->len == 0) {
            cost += 10;
        } else if (w->kind == 0x1B || w->kind == 0x1C) {
            cost += 3;
        } else if (w->kind == 0x1A) {
            cost += 2;
        } else if (w->kind <= 0x19 || w->kind >= 0x1F) {
            cost += 1;
        }
    }
    if (sub != 0 || cls == 0xD) {
        return Conv_ClassPairCost(cls, sub, pc, cost);
    }
    if (cls == 0x2D) {
        return Conv_KindClassCost(k, pc, 0xFF, cost, 0);
    }
    if (pc >= 0x32 && pc < 0x39) {
        return Conv_KindClassCost((w = prev->seg) != NULL ? w->sub : 0, cls, sub, cost, 2);
    }
    if (cls >= 0x14 && cls < 0xC0) {
        return Conv_PosPairBonus(cls, pc, cost);
    }
    if (cls >= 0xC0) {
        return Conv_ClassCostLinked(n, cls, pc, cost);
    }
    return cost;
}

/* Adds a connection cost to cost by the word classes a (the word), b and c (its neighbour): +3, +10 or +15
 * for the listed pairs, cost itself otherwise. */
s32 Conv_ClassPairCost(u8 a, u8 b, u8 c, s32 cost)
{
    if (a == 0xD && b == 0) {
        if (c > 0 && c < 0x18) {
            return cost + 3;
        }
    } else if (b >= 4 && b < 7 && c > 0 && c < 0x18) {
        if (a == 0x8D || (a == 0x8E && b == 6)) {
            return cost + 10;
        }
        if (a < 0x8D) {
            return cost + 3;
        }
    } else if (b == 8 && c >= 0x14 && c < 0x19) {
        if ((a >= 0x80 && a <= 0x8E) || a == 0x92 || a == 0x94 || a == 0xA0 || a == 0xA2 || (u32)(a - 0xAE) <= 1
            || a == 0xB1 || a == 1 || (a >= 0xA && a < 0xD)) {
            return cost + 15;
        }
        return cost + 3;
    }
    return cost;
}

/* Adds the cost bonus of the part-of-speech pair (a, b) to c: 3 for two of 20-24, 15 for 31-31, 20 for a
 * 0x21/0x26/0x27, 15 for a 0x20/0x22, 20 for 0x1B-0x1C and 5 for 0x1A-0x1A. */
s32 Conv_PosPairBonus(u8 a, u8 b, s32 c)
{
    if (a >= 20 && a < 25) {
        if (b >= 20 && b < 25) {
            return c + 3;
        }
        return c;
    }
    if (a == 31 && b == 31) {
        return c + 15;
    }
    if (a == 0x21 || a == 0x26 || a == 0x27) {
        return c + 20;
    }
    if (a == 0x20 || a == 0x22) {
        return c + 15;
    }
    if (a == 0x1B && b == 0x1C) {
        return c + 20;
    }
    if (a == 0x1A && b == 0x1A) {
        return c + 5;
    }
    return c;
}

/* Adds a cost for the word classes a and b (like Conv_ClassPairCost), with more when entry n's linked word (+4)
 * has a class 0x14..0x19 or 0x32 and ends right before n (+0 + 1 == n's +0). */
s32 Conv_ClassCostLinked(ConvCand *n, u8 a, u8 b, s32 cost)
{
    ConvSeg *q = n->seg;
    s32 adj = 0;
    s32 f;

    if (q != NULL && ((q->kind >= 0x14 && q->kind <= 0x19) || q->kind == 0x32)) {
        adj++;
        if (q->len + 1 == n->len) {
            adj++;
        }
    }
    if (a == 0xC7) {
        f = 0;
        if (b > 0 && b < 0xE) {
            f = 1;
        }
        return f ? cost + 30 : cost + 25;
    }
    if (a == 0xC8) {
        if (b == 0) {
            return cost;
        }
        return adj == 2 ? cost + 20 : cost + 5;
    }
    if (a == 0xC1 || a == 0xE1) {
        if (adj == 2) {
            cost += 5;
        }
        return cost;
    }
    if ((u32)(a - 0xC4) <= 1 || (a >= 0xCE && a < 0xD2)) {
        if ((b > 0 && b <= 0xD) || (b >= 0x16 && b < 0x18)) {
            return cost + 5;
        }
        return cost;
    }
    if (a == 0xC2) {
        if (b == 0) {
            return cost;
        }
        if (adj == 2) {
            cost += 3;
        }
        return cost;
    }
    if (a >= 0xFC && a < 0xFE && b > 0 && b < 0x18) {
        return cost + 18;
    }
    if ((a == 0xFA || a == 0xFE) && b >= 0x14 && b < 0x19) {
        return cost + 18;
    }
    return cost;
}

/* Adds 15 or 20 (and extra) to cost when the word class b (with c for kind 5) suits the kind k (0..8);
 * cost itself otherwise. */
s32 Conv_KindClassCost(u8 k, u8 b, u8 c, s32 cost, s32 extra)
{
    s32 add = 0;

    switch (k) {
    case 0:
        if ((b >= 0x14 && b <= 0x19) || b == 0x32) {
            add = 15;
        }
        break;
    case 1:
        if (b == 0x1A) {
            add = 15;
        }
        break;
    case 2:
        if (b == 0x1B || b == 0x1C) {
            add = 20;
        }
        break;
    case 3:
        if (b == 0x1F || b == 0x38) {
            add = 20;
        }
        break;
    case 4:
        if (b == 0x16) {
            add = 20;
        }
        break;
    case 5:
        if (c == 0xFF) {
            if (b > 0 && b < 0xE) {
                add = 15;
            }
        } else if ((b >= 0x80 && b < 0x8C && c == 4) || (b == 0xD && c == 0)) {
            add = 15;
        }
        break;
    case 6:
        if (b >= 0x14 && b < 0x1A) {
            add = 15;
        } else if (b == 0x1F || b == 0x38) {
            add = 20;
        }
        break;
    case 7:
        if (b == 0x1D) {
            add = 15;
        }
        break;
    case 8:
        if ((b >= 0x14 && b <= 0x19) || (b > 0 && b < 0xE)) {
            add = 20;
        }
        break;
    }
    if (add == 0) {
        return cost;
    }
    return extra + (cost + add);
}

u16 Cand_Prio(ConvCand *cand, s32 code)
{
    s32 c;

    c = code & 0xFFFF;
    if (c != 0) {
        return (c + 0x3E8) & 0xFFFF;
    }
    return cand->prio;
}

s32 KbdChar_ClassC0(s32 ch)
{
    return gKbdCharClass[ch & 0xFF] & 0xC0;
}

s32 KbdChar_Class80(s32 ch)
{
    return gKbdCharClass[ch & 0xFF] & 0x80;
}

s32 KbdChar_Class40(s32 ch)
{
    return gKbdCharClass[ch & 0xFF] & 0x40;
}

s32 KbdChar_Class20(s32 ch)
{
    return gKbdCharClass[ch & 0xFF] & 0x20;
}

u16 Kana_ByteToCode(u16 c);
u16 Kana_CodeForKey(u16 c);
u16 Kana_ToJis(u8 a, u16 b);
u8 *Kana_FindWord(u16 *s, u16 *e);
u8 *Kana_FindLetterWord(u16 *s, u16 *e);
extern u8 D_00404ED1[];
extern s32 gRomajiConf[];

/* Converts the romaji keys at codes (n of them, classed by gKbdCharClass) into kana at out (*count: 1 or 2):
 * vowels through the letter entries (gRomajiLetterEntries), consonant runs through Kana_FindWord/Kana_FindLetterWord, a doubled consonant
 * or n specially; returns the keys used, 0 when more are needed, -2 when they spell nothing. */
s32 Romaji_ToKana(u16 *codes, s32 n, u16 *out, s32 *count)
{
    u16 *p;
    u8 c;
    u8 *e;
    s32 i;
    u16 kind;
    s32 cls;

    i = 1;
    cls = gKbdCharClass[*(u8 *)codes] & 0xF;
    *count = 1;
    if (cls < 10) {
        if (cls < 6) {
            p = codes;
            e = D_00404ED1;
            goto found;
        }
        if (cls <= 8 || gRomajiConf[0] == 0) {
            *out = Kana_ByteToCode(codes[0]);
        } else {
            *out = Kana_CodeForKey(codes[0]);
        }
        return 1;
    }
    kind = 0;
    if (cls < 13) {
        if (cls == 12) {
            *out = Kana_ToJis(0x73, codes[0]);
            kind = 1;
        } else {
            *out = Kana_ByteToCode(codes[0]);
            if (cls == 10) {
                kind = 2;
            } else {
                kind = 3;
            }
        }
    }
    if (n == 1) {
        return -(kind != 0);
    }
    p = codes + 1;
    c = codes[1];
    if (*(u8 *)codes == c) {
        if (cls == 12) {
            return 2;
        }
        if (cls >= 13) {
            *out = Kana_ToJis(0x43, codes[1]);
            return 1;
        }
    }
    if (kind == 1 && c == 0x27) {
        return 2;
    }
    cls = gKbdCharClass[c] & 0xF;
    if (cls == 7 && kind == 3) {
        *out += 2;
        return 2;
    }
    if (cls == 6 && kind >= 2) {
        *out += 1;
        if (*(u8 *)codes == 0xB3) {
            *out = Kana_ToJis(0x74, *p);
        }
        return 2;
    }
    if (kind > 1 || (cls > 5 && cls <= 14 && kind == 1)) {
        return 1;
    }
    i = 2;
    while (1) {
        if (cls < 6) {
            if ((e = Kana_FindWord(codes, p + 1)) == NULL) {
                if ((e = Kana_FindLetterWord(codes, p)) == NULL) {
                    return -2;
                }
            found:
                e += (cls - 1) * 2;
            }
            *out = Kana_ToJis(*e++, *p);
            if (*e != 0) {
                out[1] = Kana_ToJis(*e, *p);
                *count = 2;
            }
            return i;
        }
        if (i++ >= n) {
            break;
        }
        p++;
        cls = gKbdCharClass[*(u8 *)p] & 0xF;
        if ((cls < 6 || cls > 12) && i <= 4) {
            continue;
        }
        return -2;
    }
    return 0;
}

u8 Char_ToUpper(u8 ch)
{
    s32 c;

    c = ch & 0xFF;
    if ((c >= 0x61) && (c < 0x7B)) {
        return (c - 0x20) & 0xFF;
    }
    return ch;
}

extern u8 gRomajiWords[];

/* Looks the cells s..e up in the word table gRomajiWords ({length, codes, text NUL}..., ended by a 0 length) by
 * their Char_ToUpper codes; returns the matching entry's text or NULL. */
u8 *Kana_FindWord(u16 *s, u16 *e)
{
    u8 *p;
    u16 *q;
    s32 k;
    u8 c;
    s32 n;

    n = e - s;
    p = gRomajiWords;
    if ((c = *p) != 0) {
        do {
            p++;
            if (n == c) {
                q = s;
                for (k = n; k > 0; k--) {
                    if (*p != Char_ToUpper(*q++)) {
                        break;
                    }
                    p++;
                }
                if (k == 0) {
                    return p;
                }
                p += k;
            } else {
                p += c;
            }
            while (*p++ != 0) {
            }
        } while ((c = *p) != 0);
    }
    return NULL;
}

extern u8 D_00404ED1[];
extern u8 *gRomajiLetterRows[];

/* Looks the one or two letters s..e up in the letter-word table gRomajiLetterRows (a row of 11-byte entries
 * {letter, text} per first letter, the row's first entry being the letter alone); returns the entry's
 * text, "" for nothing and NULL when not found. */
u8 *Kana_FindLetterWord(u16 *s, u16 *e)
{
    s32 n = e - s;
    s32 c;
    u8 **t;
    u8 *p;
    u8 *end;
    u8 *row;
    s32 key;

    if (n == 0) {
        return D_00404ED1;
    }
    if (n == 1 || n == 2) {
        c = (u8)s[0];
        if (c >= 0x61 && c < 0x7B) {
            c -= 0x61;
        } else if (c >= 0x41 && c < 0x5B) {
            c -= 0x41;
        } else {
            return NULL;
        }
        t = &gRomajiLetterRows[c];
        row = *t;
        if (n == 1) {
            return row + 1;
        }
        do {
            /* FAKEMATCH: p = row is loop-invariant here; it places p's copy of the row where the
             * original has it (the natural p = *t before the test, or p = row after the walk: 4 words). */
            p = row;
            t++;
        } while ((end = *t) == NULL);
        key = Char_ToUpper(s[1]);
        for (p += 11; p < end; p += 11) {
            if (*p == key) {
                return p + 1;
            }
        }
    }
    return NULL;
}

extern u8 *gConvEditCaret;
extern u8 *gConvEditEnd;
extern s8 *gConvInputCaret;
extern s8 *gConvInputEnd;
extern s32 gConvInputLen;
s32 Sjis_IsLead(s32 c);
u16 Sjis_ToJis(u16 c);
s32 Jis_ToHalfByte(s32 c);
s32 InChars_Width(u8 *p, u8 *end);
extern u8 gConvEditLine[];

/* Appends the Shift-JIS text s to the input's 8-byte characters at gConvEditCaret (double-byte ones as JIS
 * codes of width 2 with their class from Jis_ToHalfByte at gConvInputCaret, printable single bytes of width 1);
 * returns the new count, or -1 when it would pass 36 characters or a width of 79. */
s32 Input_AppendSjis(u8 *s)
{
    u8 *e;
    u16 code;
    s8 *cls;
    s32 cnt;
    s32 w;
    s32 lead;
    s32 c;

    e = gConvEditCaret;
    lead = 0;
    cls = gConvInputCaret;
    cnt = gConvInputLen;
    w = InChars_Width(gConvEditLine, e);
    for (; (c = *s) != 0; s++) {
        if (lead != 0) {
            code = Sjis_ToJis((u16)lead << 8 | (u8)c);
            if (code != 0) {
                if (cnt >= 0x24 || w >= 0x4E) {
                    return -1;
                }
                *(u16 *)e = code;
                *(s32 *)(e + 4) = 2;
                e += 8;
                *cls = Jis_ToHalfByte(code);
                cnt++;
                cls++;
                w += 2;
            }
            lead = 0;
        } else if (Sjis_IsLead(c)) {
            lead = *s;
        } else if ((*s >= 0x20 && *s < 0x7F) || (*s >= 0xA0 && *s < 0xE0)) {
            if (cnt >= 0x24 || w >= 0x4F) {
                return -1;
            }
            *(u16 *)e = *s;
            cnt++;
            w++;
            *(s32 *)(e + 4) = 1;
            lead = 0;
            *cls = 0;
            e += 8;
            cls++;
        }
    }
    gConvEditCaret = e;
    gConvEditEnd = e;
    gConvInputLen = cnt;
    gConvInputCaret = cls;
    gConvInputEnd = cls;
    return cnt;
}

/* The width of the 8-byte characters from p to end: 2 for a double-byte code, else 1. */
s32 InChars_Width(u8 *p, u8 *end)
{
    s32 n = 0;

    for (; p < end; p += 8) {
        if (*(u16 *)p & 0xFF00) {
            n += 2;
        } else {
            n += 1;
        }
    }
    return n;
}

extern u8 gConvEditLine[];

s32 Input_SpanWidth(s32 i, s32 n)
{
    s32 sum = 0;
    u8 *p = gConvEditLine + i * 8;

    for (; n > 0; n--) {
        sum += *(s32 *)(p + 4);
        p += 8;
    }
    return sum;
}

s32 ConvRec_SegLen(s32 i);
void Cells_SetPalette(u16 *p, s32 n, u16 v);
s32 ConvSeg_Codes(s32 i, s32 b, u8 *d);
s32 ConvSeg_ChosenCodes(s32 i, s32 b, u8 *d);
extern s32 gConvSegLen;
extern s32 gConvSegPos;

/* Converts every segment of the input (ConvRec_SegLen gives each one's length) into the codes at gCandLine,
 * marking the segment being edited (gConvSegPos) with 7 and the others with 6; returns the codes' count. */
s32 Input_BuildLine(void)
{
    u16 *d = &gCandLine;
    s32 len;
    s32 total = 0;
    s32 i = 0;
    s32 n;

    while ((n = ConvRec_SegLen(i)) != 0) {
        if (i == gConvSegPos) {
            len = ConvSeg_ChosenCodes(i, n, (u8 *)d);
            gConvSegLen = n;
            Cells_SetPalette(d, len, 7);
        } else {
            len = ConvSeg_Codes(i, n, (u8 *)d);
            Cells_SetPalette(d, len, 6);
        }
        i += n;
        d += len;
        total += len;
    }
    return total;
}

extern s32 D_0059BB00;
extern s32 gConvState;
extern ConvOut *gCandPageFirst;
extern s32 gCandPageSel;
ConvOut *Out_Nth(ConvOut *o, s32 n);

/* Out_ToCode2 of segment i (b characters) into d, from the chosen candidate (gCandPageFirst's D_00589888th when
 * choosing, else the segment's first). */
s32 ConvSeg_ChosenCodes(s32 i, s32 b, u8 *d)
{
    ConvOut *p;

    if (D_0059BB00 == 1 || gConvState >= 3) {
        p = Out_Nth(gCandPageFirst, gCandPageSel);
    } else {
        p = Out_Nth(gConvRecs[i].outs, 0);
    }
    return Out_ToCode2(i, b, p, d);
}


s32 ConvSeg_Codes(s32 i, s32 b, u8 *d)
{
    return Out_ToCode2(i, b, gConvRecs[i].outs, d);
}

extern s32 D_0059BB00;
extern s32 gConvState;
extern ConvOut *gCandPageFirst;
extern s32 gCandPageSel;
ConvOut *Out_Cut(ConvOut *o);
ConvOut *Out_NextChain(ConvOut *o);
ConvOut *Out_Nth(ConvOut *o, s32 n);
struct Node18;
void InNode_Free(struct Node18 *n);
struct OutLink;
void Out_FreeChain(struct OutLink *n);

void ConvRec_FreeUnchosen(s32 i, s32 mode)
{
    ConvOut **head = (ConvOut **)((u8 *)&gConvRecs[0].outs + i * sizeof(ConvRec));
    ConvOut *n = *head;
    ConvOut *next;
    ConvOut *stop;

    if (n != NULL) {
        if (mode == 1 && (D_0059BB00 == 1 || gConvState >= 3)) {
            stop = Out_Nth(gCandPageFirst, gCandPageSel);
            while (n != NULL) {
                if (n == stop) {
                    break;
                }
                next = Out_NextChain(n);
                InNode_Free((struct Node18 *) n);
                n = next;
            }
            *head = n;
        }
        Out_FreeChain((struct OutLink *)Out_Cut(n));
    }
}

u8 *Input_LineToSjis(void)
{
    Text_CellsToSjis(&gCandLine, gConvSjis, Input_BuildLine());
    return gConvSjis;
}

/* Converts the segments from i for len characters into codes (as Input_BuildLine) and those into Shift-JIS at
 * gConvSjis, which it returns. */
u8 *Conv_SegsToSjis(s32 i, s32 len)
{
    u16 *d = &gCandLine;
    s32 n;
    s32 end;
    s32 k;

    for (end = i + len; i < end && (n = ConvRec_SegLen(i)) != 0;) {
        if (i == gConvSegPos) {
            k = ConvSeg_ChosenCodes(i, n, (u8 *)d);
        } else {
            k = ConvSeg_Codes(i, n, (u8 *)d);
        }
        i += n;
        d += k;
    }
    Text_CellsToSjis(&gCandLine, gConvSjis, d - &gCandLine);
    return gConvSjis;
}

s32 UserDict_AddWord(Word *w);
Word *Conv_NewWord(s32 i, s32 n, ConvOut *o);
extern u64 gTextList[];
s32 UserDict_AddWordNumbered(Word *w, u64 *refs, s32 n);
s32 Dict_MarkUsed(u64 r, u16 sel, u64 *refs, s32 n);

/* Collects the dictionary references (+8 of each entry, u64) of the candidate list without repeats at
 * gTextList, then marks them used for the chosen word c (+8, +0xC), or registers the reading at i, n
 * (Conv_NewWord) as a new word numbered by them when c has none. */
void Conv_CommitWord(s32 i, s32 n, ConvOut *c, ConvCand *list)
{
    u64 *d;
    u64 r;
    u64 prev;
    ConvSeg *w;

    if (c->unkC == -1) {
        w = NULL;
    } else {
        w = c->seg;
        if (w == NULL) {
            return;
        }
    }
    d = gTextList;
    if (list == NULL || list == (ConvCand *)-1) {
        if (w != NULL) {
            *d++ = w->entry;
        }
    } else {
        prev = -1;
        do {
            if (list->seg != NULL) {
                r = list->seg->entry;
                if (r != -1 && r != prev) {
                    *d++ = r;
                    prev = r;
                }
            }
            list = list->next;
        } while (list != NULL);
    }
    if (w == NULL) {
        UserDict_AddWordNumbered(Conv_NewWord(i, n, c), gTextList, d - gTextList);
    } else {
        Dict_MarkUsed(w->entry, c->unkC, gTextList, d - gTextList);
    }
}


/* Registers the reading gLearnReading with its text gLearnText in the user dictionary, taking the word class
 * from entry o's +8 (bytes 2 and 3; 0x28/0 without one). Nothing when the reading is empty. */
void UserDict_LearnReading(ConvOut *o)
{
    Word w;
    ConvSeg *q;

    if (gLearnReading != 0) {
        w.reading = &gLearnReading;
        w.len = strlen(&gLearnReading);
        w.unk6 = 0;
        if (o == NULL || (q = o->seg) == NULL) {
            w.kind = 0x28;
            w.unk8 = 0;
        } else {
            w.kind = q->kind;
            w.unk8 = q->sub;
        }
        w.text = &gLearnText;
        UserDict_AddWord(&w);
    }
}

extern s32 D_00581B80;
void Conv_ClearReading(void);
void Conv_CollectReading(s32 i, s32 n, ConvOut *p, s32 keep);

/* Commits the segments from i up to end: each one's chosen candidate (gCandPageFirst's for the one being
 * edited) is marked used (Conv_CommitWord), or, in the modes 3, a changed reading is collected
 * (Conv_CollectReading) and registered in the user dictionary (UserDict_LearnReading). */
void Conv_CommitSegments(s32 i, s32 end)
{
    s8 k;
    ConvRec *rec;
    ConvOut *c;
    s32 n;

    while (i < end && (n = ConvRec_SegLen(i)) != 0) {
        rec = &gConvRecs[i];
        if (i == gConvSegPos && (D_0059BB00 == 1 || gConvState >= 3)) {
            c = Out_Nth(gCandPageFirst, gCandPageSel);
        } else {
            c = Out_Nth(rec->outs, 0);
        }
        if (D_00581B80 != 3 && gConvState < 3) {
        commit:
            Conv_CommitWord(i, n, c, rec->cands);
            i += n;
        } else {
            if ((k = rec->fixedLen) != 0 && k != rec->len) {
                Conv_CollectReading(i, n, c, 1);
                Conv_CommitWord(i, n, c, rec->cands);
            } else if (gLearnReading != 0) {
                Conv_CollectReading(i, n, c, 0);
                UserDict_LearnReading(c);
                Conv_ClearReading();
            } else {
                goto commit;
            }
            i += n;
        }
    }
}

void Conv_ClearReading(void)
{
    gLearnReading = 0;
}

/* Copies segment i (n characters, or entry p's count when appending) of the input to the reading buffer
 * gLearnReading and its conversion by entry p to gLearnText (at their ends unless keep); clears the reading
 * when the copied text holds a NUL. */
void Conv_CollectReading(s32 i, s32 n, ConvOut *p, s32 keep)
{
    u8 *s = &gLearnReading;
    u8 *t = &gLearnText;

    if (!keep) {
        s += strlen(s);
        t += strlen(t);
        n = p->len;
    }
    strncpy((char *)s, (const char *)&gConvInput[i], n);
    s[n] = 0;
    if (strlen(s) != n) {
        Conv_ClearReading();
        return;
    }
    Text_CellsToSjis(&gCandLine, t, Out_ToCode2(i, n, p, (u8 *)&gCandLine));
}

Word *Conv_NewWord(s32 i, s32 n, ConvOut *o)
{
    strncpy((char *) &gNewWordReading, (const char *) ((s8 *) &gConvInput[i]), (u32) n);
    (&gNewWordReading)[n] = 0;
    Text_CellsToSjis(&gCandLine, (u8 *)&gNewWordText, Out_ToCode2(i, n, o, (u8 *) &gCandLine));
    gNewWord.len = (s16) n;
    gNewWord.reading = (u8 *) &gNewWordReading;
    gNewWord.kind = 0x19;
    gNewWord.text = (u8 *) &gNewWordText;
    gNewWord.unk6 = 0;
    gNewWord.unk8 = 0;
    return &gNewWord;
}

/* This file's initialised data. */
u8 gKbdCharClass[256] = {
#include "sub_main/00404DC0.inc"
};
s32 gRomajiConf[4] = {
#include "sub_main/00404EC0.inc"
};
/* The letter-word table: 11-byte entries, which gRomajiLetterRows points into. */
u8 gRomajiLetterEntries[528] = {
#include "sub_main/00404ED0.inc"
};
u8 *gRomajiLetterRows[28] = {
    NULL, &gRomajiLetterEntries[11], &gRomajiLetterEntries[33], &gRomajiLetterEntries[66],
    NULL, &gRomajiLetterEntries[99], &gRomajiLetterEntries[121], &gRomajiLetterEntries[154],
    NULL, &gRomajiLetterEntries[176], &gRomajiLetterEntries[198], &gRomajiLetterEntries[231],
    &gRomajiLetterEntries[253], &gRomajiLetterEntries[275], NULL, &gRomajiLetterEntries[297],
    &gRomajiLetterEntries[319], &gRomajiLetterEntries[341], &gRomajiLetterEntries[363], &gRomajiLetterEntries[396],
    NULL, &gRomajiLetterEntries[440], &gRomajiLetterEntries[451], &gRomajiLetterEntries[462],
    &gRomajiLetterEntries[484], &gRomajiLetterEntries[495], &gRomajiLetterEntries[517], NULL,
};
u8 gRomajiWords[40] = {
#include "sub_main/00405150.inc"
};
