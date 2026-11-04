/*
 * File access for the dictionary: thin wrappers over the memory file
 * functions (MemFile_*), the path in gDictFilePath and the open file
 * in gDictFd (-1 when closed).
 */
#include "common.h"

extern char gDictFilePath[];
extern s32 gDictFd;
extern s32 gUserDictFlags;
extern s32 gNumConvMode;
u8 *Conv_NumberToText(u8 *digits, s32 len, u8 *out, s32 mode);

s32 MemFile_Open(char *path, s32 mode);
s32 MemFile_Close(s32 fd);
s32 MemFile_Read(s32 fd, void *buf, s32 n);
s32 MemFile_Write(s32 fd, void *buf, s32 n);
#if PLATFORM_PC
/* the definition's types: pos goes on sign-extended (the EE's register holds the s32 sign-extended) and the
 * result comes back as its low word (the definition returns an offset that fits in an s32) */
s64 MemFile_Seek(s32 fd, s64 pos, s32 whence);
#define MemFile_Seek(fd, pos, whence) ((s32)MemFile_Seek(fd, pos, whence))
#else
/* unit's own declaration: the definition is s64 MemFile_Seek(s32 fd, s64 pos, s32 whence). DictIo_Seek is a tail call (j MemFile_Seek): pos goes on unextended and the result comes back as it is; with the definition's types (and DictIo_Seek s64) the compiler converts them, which the original doesn't (DictIo_SeekTo as well). Positions fit in 32 bits (gSysSettings.memFileSize). */
s32 MemFile_Seek(s32 fd, s32 pos, s32 whence);
#endif
char *strcpy(char *dst, const char *src);
s32 DictIo_Open(char *path, s32 mode);
s32 DictIo_Close(s32 fd);
void DictPage_Init(void);
void Slot_ClearAll(void);

/* Collects the results of Conv_NumberToText for mode gNumConvMode first, then
 * the other modes 0..3, chaining out; *n counts them. 1, or -3 without a
 * dictionary. */
s32 Dict_CollectAll(u8 *a, s32 b, void *out, s32 *n)
{
    void *r;
    s32 i;

    if (gDictFd == -1) {
        return -3;
    }
    *n = 0;
    r = Conv_NumberToText(a, b, out, gNumConvMode);
    if (r != NULL) {
        out = r;
        (*n)++;
    }
    for (i = 0; i < 4; i++) {
        if (i != gNumConvMode) {
            r = Conv_NumberToText(a, b, out, i);
            if (r != NULL) {
                out = r;
                (*n)++;
            }
        }
    }
    return 1;
}

/* Compares n bytes of a and b like strncmp (stops after a NUL in a). */
s32 Str_CompareN(char *a, u8 *b, s32 n)
{
    s32 d;

    while (n-- != 0) {
        d = (u8)*a - *b;
        if (!d == 0) {
            return d;
        }
        if (*a == 0) {
            return 0;
        }
        a++;
        b++;
    }
    return 0;
}

s32 DictIo_Open(char *path, s32 mode)
{
    return MemFile_Open(path, mode);
}

s32 DictIo_Read(s32 fd, void *buf, s32 n)
{
    return MemFile_Read(fd, buf, n);
}

s32 DictIo_Write(s32 fd, void *buf, s32 n)
{
    return MemFile_Write(fd, buf, n);
}

s32 DictIo_Close(s32 fd)
{
    return MemFile_Close(fd);
}

s32 DictIo_Seek(s32 fd, s32 pos, s32 whence)
{
    return MemFile_Seek(fd, pos, whence);
}

void DictIo_SetPath(const char *path)
{
    strcpy(gDictFilePath, path);
}

s32 DictIo_OpenFile(void)
{
    s32 fd = DictIo_Open(gDictFilePath, gUserDictFlags);

    if (fd == -1) {
        gDictFd = -1;
        return -1;
    }
    gDictFd = fd;
    return 0;
}

s32 DictIo_CloseFile(void)
{
    if (DictIo_Close(gDictFd) == -1) {
        gDictFd = -1;
        return -1;
    }
    gDictFd = -1;
    return 0;
}

s32 DictIo_SeekTo(s32 pos)
{
    if (DictIo_Seek(gDictFd, pos, 0) == -1) {
        if (DictIo_OpenFile() != 0) {
            return -1;
        }
        if (DictIo_Seek(gDictFd, pos, 0) == -1) {
            return -1;
        }
    }
    return 0;
}

void Dict_InitCaches(void)
{
    DictPage_Init();
    Slot_ClearAll();
}

extern u8 gDictHeader[0x400];
extern char D_00403DC0[];
extern s32 gDictKeyLo;
extern s32 gDictKeyHi;
extern s32 gUserDictPage;
extern s32 gDictSavedUserPage;
extern s32 gDictReadingsDirty;
extern s32 gDictSavedConvMode;
u32 strlen(const char *s);
/* Reads the dictionary file's 0x400-byte header and checks its signature; sets the counts from it. */
s32 DictFile_ReadHeader(void)
{
    u8 *hdr;
    s32 n;

    if (DictIo_SeekTo(0) == -1) {
        return -1;
    }
    if (DictIo_Read(gDictFd, gDictHeader, 0x400) != 0x400) {
        return -1;
    }
    hdr = gDictHeader;
    if (Str_CompareN((char *)hdr, (u8 *)D_00403DC0, strlen(D_00403DC0)) != 0) {
        return -1;
    }
    gDictKeyLo = 0;
    gDictKeyHi = hdr[0x2E] + (hdr[0x2F] << 8);
    n = hdr[0x30] + (hdr[0x31] << 8);
    gUserDictPage = n;
    gDictSavedUserPage = n;
    n = hdr[0x34] + (hdr[0x35] << 8);
    gDictReadingsDirty = 0;
    gNumConvMode = n;
    gDictSavedConvMode = n;
    return 0;
}

extern s32 gDictLearn;

/* Writes the counts back into the header (+0x30: gUserDictPage, +0x32: gDictLearn, +0x34:
 * gNumConvMode, little-endian 16-bit) and the header to the file, when anything changed. */
s32 DictFile_WriteHeader(void)
{
    u8 *p = &gDictHeader[0x30];

    if (gUserDictPage == gDictSavedUserPage && gNumConvMode == gDictSavedConvMode && gDictReadingsDirty == 0) {
        return 0;
    }
    *p++ = gUserDictPage % 256;
    *p++ = gUserDictPage / 256;
    *p++ = gDictLearn % 256;
    *p++ = gDictLearn / 256;
    *p++ = gNumConvMode % 256;
    *p++ = gNumConvMode / 256;
    if (DictIo_SeekTo(0) == -1) {
        return -1;
    }
    if (DictIo_Write(gDictFd, gDictHeader, 0x400) != 0x400) {
        return -1;
    }
    return 0;
}

/* This file's initialised data. */
char D_00403DC0[0x30] = {
#include "sub_main/00403DC0.inc"
};
u32 gDecoTable[500] = {
#include "sub_main/00403DF0.inc"
};
