#ifndef MH2_CORE_IO_FIO_H
#define MH2_CORE_IO_FIO_H

/*
 * The file service (fileio, SIF service 0x80000001) between the EE's client
 * (pc/core/api/fileio.c) and its IOP side (io_fio.c). Both are the core's own, so
 * the request is a C struct; what the game can observe is the client's order of
 * kernel calls and waits (the SDK's section 5, "The rest of the
 * fileio client") and the service's answers (the original's,
 * at the request: frame clock 1.1).
 *
 * Each request has the client's own semaphore and result word. The service
 * answers the RPC at once (its first reply word 1: accepted) and queues the
 * command FIO_CMD_ANSWER, whose EE handler stores the result and signals the
 * semaphore.
 */
#include "common.h"

#define FIO_SID 0x80000001u
#define FIO_CMD_ANSWER 0x80000011u
#define FIO_CMD_POWEROFF 0x80000013u

enum {
    FIO_OPEN = 0, FIO_CLOSE = 1, FIO_READ = 2, FIO_WRITE = 3, FIO_LSEEK = 4,
    FIO_DEVCTL = 0x17, FIO_SETUP = 0xFF
};

/* The IOP's answers: the original's. */
#define FIO_ENOENT (-2)
#define FIO_EIO (-5)
#define FIO_EBADF (-9)
#define FIO_EINVAL (-22)
#define FIO_EMFILE (-24)
#define FIO_FIRST_FD 3       /* the IOP's descriptors: 3 and up */
#define FIO_IOP_FILES 16     /* files open at once on the IOP */

typedef struct FioRequest {
    s32 sema;          /* the client's semaphore, signalled by the answer */
    s32 *result;       /* where the answer's handler stores the result */
    s32 fd;            /* the IOP's descriptor */
    s32 flags, mode;   /* open */
    void *buf;         /* read, write */
    s32 size;
    s32 offset, whence;     /* lseek */
    s32 cmd;                /* devctl */
    u32 arglen, buflen;
    void *devbuf;
    char name[0x400];       /* open's path, devctl's device */
} FioRequest;

/* The answer's payload (the command FIO_CMD_ANSWER). */
typedef struct FioAnswer {
    s32 sema;
    s32 *result;
    s32 value;
    u32 fno;
} FioAnswer;

/* The set-up's answer (FIO_SETUP, in the RPC's reply): the IOP's version and
 * its number of result records (the original's: "3000", 1). */
typedef struct FioSetupReply {
    char version[4];
    u32 records;
} FioSetupReply;

#endif
