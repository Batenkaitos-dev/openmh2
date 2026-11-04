/*
 * ACCESS's kana-kanji conversion engine, its first unit:
 * Conv_Command, which runs a conversion command through the table gConvCommands,
 * the commands themselves, the user dictionary command and the segment steps.
 * The engine goes on in the units after it, to the conversion's costs
 * (Conv_NodeCost). It is ACCESS's middleware, kept as C.
 */
#include "common.h"
#include "em.h"
#include "conv.h"

/* prototypes */
void Conv_Reset(void);
void Conv_Init(s32);
s32 DictFile_Open(s8 *);
s32 DictFile_Close(void);
u8 *Input_LineToSjis(void);
u8 * Conv_SegsToSjis(s32, s32);
void Conv_CommitSegments(s32, s32);
s32 Debug_Sprintf(const char *fmt, ...);
char *strcpy(char *dst, const char *src);
extern s32 gConvCommands[];
extern s8 gDictPath[];
extern s32 gConvState;
extern s32 gConvInputLen;
extern s32 gConvSegLen;
extern s32 gConvSegPos;
/* end of prototypes */

extern s32 D_0040317C[];

s32 Conv_Command(s32 *cmd)
{
    s32 id = *cmd;

    if (id <= 0 || (u32)id > 0x3F) {
        return -1;
    }
    return ((s32 (*)(s32 *))D_0040317C[id])(cmd + 1);
}

s32 Romaji_ToKana(u16 *codes, s32 n, u16 *out, s32 *count);
u16 Jis_ToSjis(u16 c);
extern u16 gTextList[512];

s32 Romaji_ToKana(u16 *codes, s32 n, u16 *out, s32 *count);
u16 Jis_ToSjis(u16 c);
extern u16 gTextList[512];

/*
 * Converts the kana text args[0] (1-byte codes, in the 0x4000 or, for command 15, the
 * 0x4100 set) through gTextList and Romaji_ToKana into 2-byte characters at args[1]
 * (Jis_ToSjis; 0x82F2 becomes 0x8394). 0, or 1 + the position of a code it can't
 * convert. The `!!` gives the game's computed test of the character.
 */
s32 Conv_KanaToSjis(s32 *args)
{
    u16 out[2];
    s32 cnt;
    u8 *d;
    u8 *s;
    u16 base;
    u16 *o;
    s32 n;
    u16 *p;
    s32 k;
    u16 c;
    u8 ch;

    s = (u8 *)args[0];
    d = (u8 *)args[1];
    if (args[-1] == 15) {
        base = 0x4100;
    } else {
        base = 0x4000;
    }
    p = gTextList;
    n = 0;
    while (!!(ch = *s) && p < &gTextList[512]) {
        *p = ch | base;
        s++;
        n++;
        p++;
    }
    p = gTextList;
    while (n > 0) {
        k = Romaji_ToKana(p, n, out, &cnt);
        if (k <= 0) {
            if (k != -1) {
                return (p - gTextList) + 1;
            }
            k = 1;
        }
        p += k;
        n -= k;
        o = out;
        while (cnt-- != 0) {
            c = Jis_ToSjis(*o++);
            if (c == 0x82F2) {
                c = 0x8394;
            }
            d[0] = c >> 8;
            d[1] = c;
            d += 2;
        }
    }
    *d = 0;
    return 0;
}

extern ConvRec gConvRecs[80];

void Conv_GetTexts(s8 *, s8 *);
void CandLine_Select(s32, s32);
void ConvRec_NoteFixedLen(s32 i);
void ConvRecs_Clear(s32 from, s32 to, s32 keep);
void Conv_Convert(s32 from, s32 to, s32 mode, s32 len0);
s32 Out_Count(ConvOut *o);
void ConvRec_FreeUnchosen(s32 i, s32 mode);
s32 Input_SpanWidth(s32 from, s32 len);
extern s32 gCandPageSel;
extern s32 gCandPageCount;
s32 ConvRec_SegLen(s32 i);   /* defined as s8; its callers here use the result unextended */

s32 Input_AppendSjis(u8 *text);

/* Starts a conversion of text args[0] (if any): converts the whole input and selects its
 * first segment; args[1] and args[2] get the texts (Conv_GetTexts). */
s32 Conv_Start(u8 **args)
{
    u8 *text;
    s8 *a;
    s8 *b;

    if (gConvState == 0) {
        return -1;
    }
    text = args[0];
    a = (s8 *)args[1];
    b = (s8 *)args[2];
    if (text != NULL) {
        if (gConvState >= 2) {
            Conv_Reset();
        }
        if (Input_AppendSjis(text) < 0) {
            return -1;
        }
    }
    if (gConvInputLen <= 0) {
        *a = 0;
        *b = 0;
        return 0;
    }
    Conv_Convert(0, gConvInputLen, 0, -1);
    gConvSegPos = 0;
    gConvSegLen = ConvRec_SegLen(0);
    CandLine_Select(0, 1);
    Conv_GetTexts(a, b);
    gConvState = 3;
    return Out_Count(gConvRecs[0].outs);
}

void Conv_GetTexts(s8 *head, s8 *tail)
{
    strcpy((char *) head, (char *) (Conv_SegsToSjis(gConvSegPos, gConvSegLen)));
    strcpy((char *) tail, (char *) (Conv_SegsToSjis(gConvSegPos + gConvSegLen, (gConvInputLen - gConvSegPos) - gConvSegLen)));
}

s32 CandLine_NextPage(s32, s32);
s32 CandLine_PrevPage(s32, s32);

s32 Conv_StepCand(void *args)
{
    s32 cmd;
    s32 line;
    s8 *head;
    s8 *tail;

    if (gConvState != 3) {
        return 0;
    }
    cmd = *(s32 *)((u8 *)args + -4);
    head = *(s8 **)((u8 *)args + 0);
    tail = *(s8 **)((u8 *)args + 4);
    switch (cmd) {
    case 20:
        if (gCandPageSel > 0) {
            gCandPageSel -= 1;
        } else if (CandLine_PrevPage(0, 0) == 0) {
            CandLine_Select(0, 1);
            line = gCandPageCount;
            if (CandLine_NextPage(0, 0) != 0) {
                do {
                    line += gCandPageCount;
                } while (CandLine_NextPage(0, 0) != 0);
            }
            CandLine_Select(line - 1, 1);
        } else {
            gCandPageSel = gCandPageCount - 1;
        }
        break;
    case 21:
        if (gCandPageSel < (gCandPageCount - 1)) {
            gCandPageSel += 1;
        } else if (CandLine_NextPage(0, 0) == 0) {
            CandLine_Select(0, 1);
        }
        break;
    }
    Conv_GetTexts((s8 *) head, (s8 *) tail);
    return gCandPageSel + 1;
}

s32 Conv_StepCandPage(s32 *arg)
{
    s8 *a;
    s8 *b;
    s32 found;
    s32 type;

    found = 0;
    if (gConvState != 3) {
        return 0;
    }
    type = arg[-1];
    a = (s8 *)arg[0];
    b = (s8 *)arg[1];
    switch (type) {
    case 22:
        if (CandLine_PrevPage(0, 0) == 0) {
            found = 1;
        }
        break;
    case 23:
        if (CandLine_NextPage(0, 0) == 0) {
            found = 1;
        }
        break;
    }
    Conv_GetTexts(a, b);
    if (found != 0) {
        return 0;
    }
    return gCandPageCount;
}

s32 Conv_CommitAll(s8 **args)
{
    s8 *temp_s0;

    if (gConvState != 3) {
        return -1;
    }
    temp_s0 = *args;
    Conv_CommitSegments(0, gConvInputLen);
    strcpy((char *) temp_s0, (char *) (Input_LineToSjis()));
    Conv_Reset();
    gConvState = 1;
    return 0;
}

s32 Conv_CommitSegment(s32 *args)
{
    s32 n;
    s8 *text;
    s8 *a;
    s8 *b;
    s16 *count;
    s32 r;

    if (gConvState != 3) {
        return -1;
    }
    n = args[0];
    if (n <= 0 || n > gCandPageCount) {
        return 0;
    }
    text = (s8 *)args[1];
    a = (s8 *)args[2];
    b = (s8 *)args[3];
    count = (s16 *)args[4];
    gCandPageSel = n - 1;
    strcpy((char *)text, (char *)Conv_SegsToSjis(gConvSegPos, gConvSegLen));
    ConvRec_FreeUnchosen(gConvSegPos, 1);
    r = Input_SpanWidth(gConvSegPos, gConvSegLen);
    gConvSegPos += gConvSegLen;
    if (gConvSegPos >= gConvInputLen) {
        *a = 0;
        *b = 0;
        *count = 0;
        return r;
    }
    gConvSegLen = ConvRec_SegLen(gConvSegPos);
    CandLine_Select(0, 1);
    Conv_GetTexts(a, b);
    *count = Out_Count(gConvRecs[gConvSegPos].outs);
    return r;
}

s32 Conv_Open(void)
{
    if (gConvCommands[0] == 0) {
        return -1;
    }
    if (DictFile_Open(gDictPath) == -7) {
        return 1;
    }
    Conv_Init(0);
    gConvState = 1;
    return 0;
}

s32 Conv_Close(void)
{
    if (gConvCommands[0] == 0) {
        return -1;
    }
    Conv_Reset();
    DictFile_Close();
    gConvState = 0;
    return 0;
}

s32 Sjis_IsCode(u16 c);
u16 Sjis_ToJis(u16 c);
s32 Jis_ToHalfByte(s32 c);
struct Word;
s32 Dict_AddWord(struct Word *req);
s32 UserDict_WordClass(s32 n);
s32 Dict_DeleteWord(struct Word *req);
extern s32 D_00581B80;
extern u8 gNewWordReading[];


/*
 * Command 0x1E or 0x1F (args[-1]) on the word args[1] (2-byte characters, turned into
 * 1-byte codes in gNewWordReading) with the kind args[3] (UserDict_WordClass) and args[2]: through
 * Dict_AddWord or Dict_DeleteWord. 0 when done, 1 bad args[0], 2 bad kind, 3 bad character,
 * -1 when Dict_DeleteWord returns 0, 4 otherwise (also while busy).
 */
s32 UserDict_Command(s32 *args)
{
    Word req;
    s32 r;
    u8 c;
    u8 ch;
    s32 cmd;
    s32 kind;
    u8 *s;
    s32 x;
    u8 *d;
    s32 n;
    u16 code;

    if (D_00581B80 >= 2 || gConvState >= 2) {
        return 4;
    }
    cmd = args[-1];
    if (args[0] != 1 && args[0] != 0) {
        return 1;
    }
    s = (u8 *)args[1];
    x = args[2];
    if ((kind = UserDict_WordClass(args[3])) < 0) {
        return 2;
    }
    ch = *s;
    n = 0;
    d = gNewWordReading;
    while (ch != 0) {
        code = (u16)((ch & 0xFF) << 8 | s[1]);
        if (Sjis_IsCode(code) == 0) {
            return 3;
        }
        c = Jis_ToHalfByte(Sjis_ToJis(code));
        if (c == 0) {
            return 3;
        }
        *d = c;
        s += 2;
        d++;
        ch = *s;
        n++;
    }
    *d = 0;
    req.reading = gNewWordReading;
    req.len = n;
    req.kind = kind;
    req.text = (u8 *)x;
    req.unk6 = 0;
    req.unk8 = 0;
    if (cmd == 0x1E) {
        if (Dict_AddWord((struct Word *)&req) == 3) {
            return 0;
        }
    } else {
        if (cmd == 0x1F) {
            r = Dict_DeleteWord((struct Word *)&req);
            if (r == 3) {
                return 0;
            }
            if (r == 0) {
                return -1;
            }
        }
    }
    return 4;
}

extern u8 D_00403280[];

s32 UserDict_WordClass(s32 n)
{
    n -= 1;
    if (n < 0 || n >= 0x1E) {
        return -1;
    }
    return D_00403280[n];
}

s32 Conv_SetDictPath(s32 *args)
{
    strcpy((char *) gDictPath, (char *) ((s8 *) *args));
    return 0;
}

s32 Conv_GrowSegment(s8 **args)
{
    s8 *a;
    s8 *b;

    if (gConvState != 3) {
        return -1;
    }
    a = args[0];
    b = args[1];
    if (gConvSegPos + gConvSegLen >= gConvInputLen) {
        return 0;
    }
    ConvRec_NoteFixedLen(gConvSegPos);
    ConvRecs_Clear(gConvSegPos, gConvInputLen, 1);
    gConvSegLen++;
    Conv_Convert(gConvSegPos, gConvInputLen, 1, gConvSegLen);
    CandLine_Select(0, 1);
    Conv_GetTexts(a, b);
    return Out_Count(gConvRecs[gConvSegPos].outs);
}

s32 Conv_ShrinkSegment(s8 **args)
{
    s8 *a;
    s8 *b;

    if (gConvState != 3) {
        return -1;
    }
    a = args[0];
    b = args[1];
    if (gConvSegLen < 2) {
        return 0;
    }
    ConvRec_NoteFixedLen(gConvSegPos);
    ConvRecs_Clear(gConvSegPos, gConvInputLen, 1);
    gConvSegLen--;
    Conv_Convert(gConvSegPos, gConvInputLen, 1, gConvSegLen);
    CandLine_Select(0, 1);
    Conv_GetTexts(a, b);
    return Out_Count(gConvRecs[gConvSegPos].outs);
}


s32 Conv_PrevSegment(s8 **args)
{
    s8 *a;
    s8 *b;
    s32 pos;
    s32 len = 0;

    if (gConvState != 3) {
        return -1;
    }
    a = args[0];
    b = args[1];
    if (gConvSegPos == 0) {
        return 0;
    }
    ConvRec_FreeUnchosen(gConvSegPos, 0);
    for (pos = 0; pos < gConvSegPos; pos += len) {
        len = ConvRec_SegLen(pos);
        if (pos + len >= gConvSegPos) {
            break;
        }
    }
    gConvSegPos = pos;
    gConvSegLen = len;
    CandLine_Select(0, 1);
    Conv_GetTexts(a, b);
    return Out_Count(gConvRecs[gConvSegPos].outs);
}


s32 Conv_NextSegment(s8 **args)
{
    s8 *a;
    s8 *b;

    if (gConvState != 3) {
        return -1;
    }
    a = args[0];
    b = args[1];
    if (gConvSegPos + gConvSegLen >= gConvInputLen) {
        return 0;
    }
    ConvRec_FreeUnchosen(gConvSegPos, 0);
    gConvSegPos += gConvSegLen;
    gConvSegLen = ConvRec_SegLen(gConvSegPos);
    CandLine_Select(0, 1);
    Conv_GetTexts(a, b);
    return Out_Count(gConvRecs[gConvSegPos].outs);
}

void Conv_SetSegmentForm(s32);
extern s32 D_00588FCC[];

s32 Conv_SegmentForm(void *args)
{
    s32 temp_v1;
    s32 form;
    s8 *temp_s1;
    s8 *temp_s0;

    if (gConvState != 3) {
        return -1;
    }
    temp_v1 = *(s32 *)((u8 *)args + -4);
    temp_s1 = *(s8 **)((u8 *)args + 0);
    temp_s0 = *(s8 **)((u8 *)args + 4);
    switch (temp_v1) {
    case 37:
        form = 2;
        break;
    case 38:
        form = 1;
        break;
    case 39:
        form = 3;
        break;
    case 40:
        form = 4;
        break;
    default:
        form = 4;
        break;
    }
    Conv_SetSegmentForm(form);
    if (*(s32 *)((u8 *)D_00588FCC + gConvSegPos * 0x1C) == 0) {
        return -1;
    }
    CandLine_Select(0, 1);
    Conv_GetTexts((s8 *) temp_s1, (s8 *) temp_s0);
    return 0;
}

/* This file's initialised data. */
s32 Conv_CmdNone(void);
s32 gConvCommands[68] = {
#include "sub_main/00403170.inc"
};
u8 D_00403280[32] = {
#include "sub_main/00403280.inc"
};
s8 gDictPath[128] = {
#include "sub_main/004032A0.inc"
};
