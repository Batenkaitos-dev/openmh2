#ifndef MH2_CORE_IO_ADX_H
#define MH2_CORE_IO_ADX_H

/*
 * CRI ADX's common parts: the threads,
 * SVM's servers and lock, and the stream layer over the CD/DVD, under ADXF
 * (api/adxf.c) and snd's ADXT. pc/core/api/adx.c. What the original game was seen
 * doing is described below.
 *
 * The threads (func_00162220): four, made in this order, with these priorities
 * after the set-up (the main thread goes to 0x18):
 *  - the lock thread (prio 8, suspended): a busy loop that runs only while a
 *    thread holds the lock and waits (it keeps every thread below 8 out);
 *  - the VSYNC thread (0x10): woken at each VBlank by func_00161F00; wakes the
 *    main thread out of func_00162188's sleep, then runs the VSYNC servers
 *    (ADXT's decoding, snd), then sleeps;
 *  - the FS thread (0x12): woken at each VBlank; runs the FS servers (ADX's
 *    server: the stream layer's reads, then ADXF's handles), then sleeps;
 *  - the IDLE thread (0x19, suspended): resumed and woken by func_00162188,
 *    runs the IDLE servers, then sleeps.
 */
#include "common.h"

/* SVM's server types (who runs them). */
enum {
    ADX_SVR_VINT = 0,     /* func_00161F00, in the VBlank's interrupt handler */
    ADX_SVR_VSYNC,        /* the VSYNC thread */
    ADX_SVR_MAIN,         /* func_00162040, on the caller's thread */
    ADX_SVR_FS,           /* the FS thread */
    ADX_SVR_IDLE,         /* the IDLE thread */
    ADX_SVR_TYPES
};
typedef void (*AdxSvrFn)(void *arg);
/* A server of a type, run in the order added: an id (never 0), or 0 when full. */
u32 Adx_SvrAdd(int type, AdxSvrFn fn, void *arg);
void Adx_SvrRemove(u32 id);

/* SVM's lock (func_00161EC0 / func_00161ED8 for Capcom's code): the holder's
 * priority goes to 1 and the lock thread is resumed; the unlock suspends it
 * and gives the holder its priority back. It nests, as the original's: a lock
 * inside a lock, and its unlock, only count (no call). */
void Adx_Lock(void);
void Adx_Unlock(void);

/* ---- the stream layer (the CD/DVD device under it) ----
 * A stream reads sectors of one file of the disc into the caller's buffer.
 * Its reads are made by ADX's server pass (the FS thread's at each wake, and
 * the stream calls' own), in chunks of at most 512 sectors, one CD request at
 * a time; when the request is done the stream's state is ADX_STM_END. Every
 * stream that exists is visited by every pass (one lock pair each): ADXT's
 * streams count from ADXT_Create. */
enum { ADX_STM_FREE = 0, ADX_STM_STOP = 1, ADX_STM_READ = 2, ADX_STM_END = 3, ADX_STM_ERR = 4 };
typedef struct AdxStm AdxStm;
/* A stream on a file by its disc name ("\\DATA.BIN" style, as the game gives
 * it), or on the sectors [lba + ofs, lba + ofs + nsct) of an AFS file opened
 * by name (ofs and nsct in sectors; nsct < 0: the whole file). NULL when no
 * stream is free or the file is not on the disc. */
AdxStm *AdxStm_Open(const char *name, s32 ofs, s32 nsct);
/* The same on a file of a pack (Io_DiscPackFind's answer: first sector and size in bytes): the open takes the same passes,
 * and the file is not looked up. */
AdxStm *AdxStm_OpenKnown(const char *name, u32 lba, u32 size, s32 ofs, s32 nsct);
/* A stream with no file, for ADXT_Create (its stream: one lock pair, as the
 * original's create). Closed with AdxStm_Close. */
AdxStm *AdxStm_Create(void);
/* A file bound to a stream AdxStm_Create made (ADXT_StartAfs, after its stop):
 * the sectors [lba + ofs, lba + ofs + nsct) of the disc file name (an AFS
 * file's, from Adxf_AfsFile; nsct < 0: the whole file). No kernel call, as
 * the original's: the file opens at the next server pass, before a read
 * asked meanwhile (AdxStm_Read) starts; SizeSectors is nsct at once. */
void AdxStm_Bind(AdxStm *s, const char *name, s32 ofs, s32 nsct);
/* The close (the original's: 71 lock pairs and four passes with three
 * streams). */
void AdxStm_Close(AdxStm *s);
s32 AdxStm_State(const AdxStm *s);
u32 AdxStm_SizeBytes(const AdxStm *s);
u32 AdxStm_SizeSectors(const AdxStm *s);
/* nsct sectors from the stream's sector pos into buf; the state READ. */
void AdxStm_Read(AdxStm *s, u32 pos, u32 nsct, void *buf);
/* The same, with the size taken again (resize's answer, in sectors) when the
 * device takes the request, which can be passes after the ask. */
void AdxStm_ReadSized(AdxStm *s, u32 pos, u32 nsct, void *buf, u32 (*resize)(void *arg), void *arg);
/* The sectors the request has read so far. */
u32 AdxStm_Done(const AdxStm *s);
/* The read stopped (a read in flight is waited for). */
void AdxStm_Stop(AdxStm *s);
/* The stream's plain stop, with no server pass (the original's 0x164850,
 * as ADXT_Stop calls it): two lock pairs; a read in flight is waited for. */
void AdxStm_StopNow(AdxStm *s);

/* ADX's server on the FS thread (the stream layer's reads, then ADXF's). */
void Adx_FsServer(void *arg);
/* Registers Adx_FsServer with SVM (the FS thread's servers), under the
 * lock; a second call does nothing. The original's ADXT_Init registers it
 * (0x1602F8): snd's ADXT_Init calls this at that point. Before it the FS
 * thread runs no server (func_00162220 no longer registers it). */
void Adx_FsServerStart(void);
/* SVM's sleep hooks: func_00162188 (the flip wait's sleep) calls before()
 * first and after() when its SleepThread returns, as the original's
 * 0x179530 and 0x179560. NULL: none. The host's mwPlyInitSfdFx registers
 * Sofdec's last, without the lock, as the original's 0x1326F0 does. */
void Adx_SetSleepHooks(void (*before)(void), void (*after)(void));
/* The FS server's ADXF part (adxf.c), run after the stream layer's. */
void Adxf_Server(void);

/* ---- for snd's ADXT ----
 * The AFS file of a partition's file: its name (0x100 bytes), its first sector
 * in it and its sectors; 0, or -3 (the error function) for an unknown one. */
s32 Adxf_AfsFile(s32 ptid, s32 flid, char name[0x100], s32 *ofs, s32 *sct);
/* ADXERR's registration (ADXT_EntryErrFunc calls it): fn(obj, message) for
 * the libraries' errors. */
void Adx_ErrEntry(void (*fn)(void *obj, const char *msg), void *obj);

#endif
