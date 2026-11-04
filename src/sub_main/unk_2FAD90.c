/*
 * Command builders for the interpreter at Conv_Command: each packs a
 * command number and its arguments into a local record and runs it (the
 * handler is gConvCommands[number + 3], called with the arguments). Also the
 * initialisation of the edit line (EditLine_Clear, Conv_Reset). See ConvCmd_*.
 */
#include "common.h"

typedef struct Cmd0 {
    s32 id;
} Cmd0;

typedef struct Cmd1 {
    s32 id;
    s32 a;
} Cmd1;

typedef struct Cmd2 {
    s32 id;
    s32 a;
    s32 b;
} Cmd2;

typedef struct Cmd3 {
    s32 id;
    s32 a;
    s32 b;
    s32 c;
} Cmd3;

typedef struct Cmd5 {
    s32 id;
    s32 a;
    s32 b;
    s32 c;
    s32 d;
    s32 e;
} Cmd5;

extern s32 gConvInputEnd;
extern s32 gConvInputCaret;
extern u8 gConvInput[];
extern s32 gConvInputLen;
extern s32 gConvEditEnd;
extern s32 gConvEditCaret;
extern u8 gConvEditLine[];
extern u8 *D_0059B958;
extern s32 gEditLineCursor;
extern s32 gEditLineConvStart;
extern s32 gEditLineConvEnd;
extern u8 gEditLineText[];

s32 Conv_Command(s32 *cmd);
void ConvRecs_Clear(s32 a, s32 b, s32 c);
void Conv_ClearReading(void);

s32 Conv_CmdNone(void)
{
    return -1;
}

s32 ConvCmd_Start(char *a, char *b, char *c)
{
    Cmd3 cmd;

    cmd.id = 0x13;
    cmd.a = (s32) a;
    cmd.b = (s32) b;
    cmd.c = (s32) c;
    return Conv_Command((s32 *) &cmd);
}

s32 ConvCmd_PrevCand(char *a, char *b)
{
    Cmd2 cmd;

    cmd.id = 0x14;
    cmd.a = (s32) a;
    cmd.b = (s32) b;
    return Conv_Command((s32 *) &cmd);
}

s32 ConvCmd_NextCand(char *a, char *b)
{
    Cmd2 cmd;

    cmd.id = 0x15;
    cmd.a = (s32) a;
    cmd.b = (s32) b;
    return Conv_Command((s32 *) &cmd);
}

s32 ConvCmd_CommitAll(u8 *a)
{
    Cmd1 cmd;

    cmd.id = 0x18;
    cmd.a = (s32) a;
    return Conv_Command((s32 *) &cmd);
}

s32 ConvCmd_CommitSegment(s32 a, u8 *b, char *c, char *d, s16 *e)
{
    Cmd5 cmd;

    cmd.id = 0x19;
    cmd.a = a;
    cmd.b = (s32) b;
    cmd.c = (s32) c;
    cmd.d = (s32) d;
    cmd.e = (s32) e;
    return Conv_Command((s32 *) &cmd);
}

s32 ConvCmd_Open(void)
{
    Cmd0 cmd;

    cmd.id = 0x1C;
    return Conv_Command((s32 *) &cmd);
}

s32 ConvCmd_GrowSegment(char *a, char *b)
{
    Cmd2 cmd;

    cmd.id = 0x21;
    cmd.a = (s32) a;
    cmd.b = (s32) b;
    return Conv_Command((s32 *) &cmd);
}

s32 ConvCmd_ShrinkSegment(char *a, char *b)
{
    Cmd2 cmd;

    cmd.id = 0x22;
    cmd.a = (s32) a;
    cmd.b = (s32) b;
    return Conv_Command((s32 *) &cmd);
}

s32 ConvCmd_PrevSegment(char *a, char *b)
{
    Cmd2 cmd;

    cmd.id = 0x23;
    cmd.a = (s32) a;
    cmd.b = (s32) b;
    return Conv_Command((s32 *) &cmd);
}

s32 ConvCmd_NextSegment(char *a, char *b)
{
    Cmd2 cmd;

    cmd.id = 0x24;
    cmd.a = (s32) a;
    cmd.b = (s32) b;
    return Conv_Command((s32 *) &cmd);
}

s32 ConvCmd_SegmentForm2(char *a, char *b)
{
    Cmd2 cmd;

    cmd.id = 0x25;
    cmd.a = (s32) a;
    cmd.b = (s32) b;
    return Conv_Command((s32 *) &cmd);
}

s32 ConvCmd_SegmentForm1(char *a, char *b)
{
    Cmd2 cmd;

    cmd.id = 0x26;
    cmd.a = (s32) a;
    cmd.b = (s32) b;
    return Conv_Command((s32 *) &cmd);
}

s32 func_002FAFF0(void)
{
    return 0x48;
}

s32 func_002FB000(void)
{
    return 0x48;
}

void EditLine_Clear(void)
{
    gConvInputLen = 0;
    D_0059B958 = (u8 *)(s32)gEditLineText;   /* through (s32): one register with the words below, as MWCC matches */
    gEditLineCursor = (s32)gEditLineText;
    gEditLineConvStart = (s32)gEditLineText;
    gEditLineConvEnd = (s32)gEditLineText;
    gConvEditEnd = (s32)gConvEditLine;
    gConvEditCaret = (s32)gConvEditLine;
    gConvInputEnd = (s32)gConvInput;
    gConvInputCaret = (s32)gConvInput;
}

void Conv_Reset(void)
{
    EditLine_Clear();
    ConvRecs_Clear(0, 0x50, 0);
    Conv_ClearReading();
}
