#ifndef MH2_CORE_IO_MC_H
#define MH2_CORE_IO_MC_H

/*
 * The memory card service (mcserv, SIF service 0x80000400) between the EE's
 * client (pc/core/api/libmc.c) and its IOP side (io_mc.c), which answers as the
 * original's HLE service over PCSX2-format card images
 * (host_mcfs.c). Requests are the core's own structs.
 */
#include "common.h"

#define MC_SID 0x80000400u
#define MC_RES_NO_CARD (-10)
#define MC_FDS_PER_CARD 16
#define MC_MCSERV_VERSION 0x0210   /* the original's: mcserv 2.16 (libmc wants 0x20A or more) */
#define MC_MCMAN_VERSION 0x0230    /* mcman 2.48 (libmc wants 0x20E or more) */

enum {
    MC_FNO_GETINFO = 0x01, MC_FNO_OPEN = 0x02, MC_FNO_CLOSE = 0x03, MC_FNO_SEEK = 0x04,
    MC_FNO_READ = 0x05, MC_FNO_WRITE = 0x06, MC_FNO_FLUSH = 0x0A, MC_FNO_CHDIR = 0x0C,
    MC_FNO_GETDIR = 0x0D, MC_FNO_SETINFO = 0x0E, MC_FNO_DELETE = 0x0F, MC_FNO_FORMAT = 0x10,
    MC_FNO_UNFORMAT = 0x11, MC_FNO_GETENTSPACE = 0x12, MC_FNO_INIT = 0xFE
};

/* What GetInfo and Read report through their end functions. */
typedef struct McBlock {
    s32 size1, size2;          /* Read: the unaligned head and tail (the core's: none) */
    void *dest1, *dest2;
    s32 type, free, formatted; /* GetInfo */
} McBlock;

typedef struct McRequest {
    s32 fd, port, slot;
    s32 size;                  /* read; write: the bytes at buffer after the head */
    s32 offset, origin;        /* seek */
    void *buffer;
    McBlock *block;
    u32 headSize;              /* write: the head's bytes, in head */
    u8 head[16];
    s32 flags, maxent;         /* open's mode, getdir's flags, setinfo's flags */
    void *table;               /* getdir's entries (0x40 bytes each); setinfo's record */
    char name[0x400];
} McRequest;

/* The answer (the RPC's receive buffer): the result; init's two versions. */
typedef struct McAnswer {
    s32 result;
    s32 mcserv, mcman;
} McAnswer;

#endif
