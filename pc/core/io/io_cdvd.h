#ifndef MH2_CORE_IO_CDVD_H
#define MH2_CORE_IO_CDVD_H

/*
 * The CD/DVD services between the EE's client (pc/core/api/libcdvd.c) and their
 * IOP side (io_cdvd.c): the original's six, answering
 * at the request (frame clock 1.1). Requests are the core's own structs.
 */
#include "common.h"

#define CD_SID_INIT 0x80000592u
#define CD_SID_SCMD 0x80000593u
#define CD_SID_NCMD 0x80000595u
#define CD_SID_SEARCH 0x80000597u
#define CD_SID_DISKREADY 0x8000059Au
#define CD_SID_59C 0x8000059Cu
#define CD_CMD_CALLBACK 0x80000012u   /* the IOP's callback command (never sent here) */

#define CD_DISK_PS2DVD 0x14
#define CD_COMPLETE 2
#define CD_STAT_PAUSE 0x0A
#define CD_ERR_READ 0x32

/* An N-command read (functions 1 and 3). */
typedef struct CdReadReq {
    u32 lbn, sectors;
    void *buf;
    u8 mode[3];        /* the read mode's first three bytes: [2] the sector size's pattern */
    u32 *endRecord;    /* the two unaligned ends' sizes (the core's: none) */
    u32 *pos;          /* the bytes read */
} CdReadReq;

/* The file search's request and the record it fills (sceCdlFILE). */
typedef struct CdFile {
    u32 lsn, size;
    char name[16];
    u8 date[8];
} CdFile;
typedef struct CdSearchReq {
    char name[256];
    CdFile *dest;
    s32 layer;
} CdSearchReq;

/* The library's own calls for CRI's file system (ADXF): the drive's state
 * (N-command 0xE), the last error (S-command 4), a read started without waiting
 * (its end through sceCdSync), and the file search. */
s32 IoCd_DiskReady(void);
s32 IoCd_GetError(void);
s32 IoCd_Busy(void);
s32 IoCd_Read(u32 lbn, u32 sectors, void *buf, const u8 *mode);
s32 IoCd_Search(CdFile *file, const char *name, s32 layer);

#endif
