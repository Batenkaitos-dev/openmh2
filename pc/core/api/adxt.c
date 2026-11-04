/* CRI ADX's players (ADXT_*): ADXT and its decoder over io's
 * common parts (pc/core/io/adx.h).
 *
 * Written from what the original game was seen doing at the interfaces: the
 * kernel calls in order under mwPlyInitSfdFx and
 * ADXT_Create at the boot, and the send data and replies of the
 * RPCs to CRI_ADX_Driver (0x90000200) and the IOP heap, logged at the original's
 * SIF. The libraries' own memory is the core's,
 * and the EE buffers the IOP writes are this file's; the players'
 * records are in the library's work area at its PS2 address (0x19DF20, through
 * the data layer's DataC_At), since
 * the game keeps their handles.
 *
 * What the game can see, and what this file reproduces:
 * - ADXT_Init (Sofdec's mwPlyInitSfdFx calls it at the boot): under one ADX
 *   lock, the bind of 0x90000200, three IOP heap blocks and CRI_ADX_Driver's
 *   set-up calls (fno 2, 0x403, 0x40C, six times 0x422 and 0x400, fno 2 again,
 *   0x40A), each with the original's arguments; then its two servers: the
 *   VSYNC server below and ADX's FS server (io's Adx_FsServerStart, the
 *   original's 0x1602F8 on the FS thread);
 * - ADXT_Create: the ADX lock taken and given back 2 * maxnch + 5 times (the
 *   input SJ, the stream, each channel's SJ, the output, each channel's output
 *   joint, the seamless object, the record), the output's RPC 0x408 inside the
 *   (maxnch + 3)th; the stream is io's (AdxStm_Create), so ADX's server pass
 *   visits it as the original's does;
 * - the record's bytes the readers return;
 * - the VSYNC server (registered by ADXT_Init, run by io's VSYNC thread each
 *   VBlank): a lock taken and given back, then under a second lock the two
 *   status blocks sent to the IOP by SifSetDma (one record each, 0x880 bytes,
 *   kind 0 first). The blocks are mailboxes: the IOP sends its copy back into
 *   the same EE buffer each VBlank; the EE adds one to the last word (0x87C)
 *   and, in block 1, writes the outputs' commands (word 0 their count, then
 *   one 16-byte entry each from 0x10: command, the IOP output, arguments);
 * - ADXT_Stop: three lock pairs; the output's command 8 (1: stopped) at the next
 *   VBlank when it differs from the last one sent. ADXT_SetOutVol: the
 *   output's command 5 (0, the level) the same way. ADXT_Pause(1) on a
 *   playing player: the output's command 2 with 0 at the next VBlank, so the
 *   IOP's answers stop, and the sends and the decode with them (area1's
 *   VBlank 18211). ADXT_Pause on stopped players: no kernel calls, no
 *   commands. ADXT_SetOutputMono: no kernel call; each player's output
 *   (playing or not) gets command 13 (-128 for mono, 0 for stereo) at the
 *   next VBlank when it changes, and a playing player's pans (command 9)
 *   change with it (the options route's sound modes, VBlanks 3562 to 4542).
 * - ADXT_StartAfs: the stop's work, the AFS file bound to the player's stream
 *   (io's AdxStm_Bind); then, in the VSYNC server, the stream's reads, the
 *   header, the decode (snd/adx_dec.c), the PCM to the IOP's rings, block 0's
 *   ring messages and the IOP's answers, commands 8, 4, 9 and 2, the states
 *   1 to 3 (the starts' state, below).
 * Seen on the routes since: two players at once and PLAYEND (combine), the
 * resume of a paused player (save-load). Not yet seen: a file's tail nearer
 * its output ring's end than the decode size. */
#include "common.h"
#include "pal.h"
#include "adx.h"
#include "sif.h"
#include "io/io.h"
#include "io/adx.h"
#include "snd/snd.h"
#include "data_c.h"

/* The SDK's calls the game doesn't declare (the kernel's, api/libkernl_sif.c). */
void *sceSifAllocIopHeap(s32 size);
u32 SifSetDma(void *transfers, s32 n);

/* ------------------------------------------------------------ errors */

/* The game's error function takes a char *; io's registration a const char *. */
static void (*sErrFn)(void *obj, char *msg);
static void *sErrObj;

static void ErrThunk(void *obj, const char *msg)
{
    sErrFn(obj, (char *)msg);
}

/* The libraries' error: the registered function called twice with its object
 * and a message (observed on the original game, session 6: ADXT_GetStat(NULL)
 * calls it twice, then returns -1). The messages are the core's own words (as
 * io's ADXF). */
static void Err(const char *msg)
{
    if (sErrFn) {
        sErrFn(sErrObj, (char *)msg);
        sErrFn(sErrObj, (char *)msg);
    }
}

/* ADXT_EntryErrFunc(func, obj): a jump into ADXERR's registration, which keeps
 * the function and its object for the libraries' errors (io's Adx_ErrEntry). */
void ADXT_EntryErrFunc(void (*func)(void *obj, char *msg), void *obj)
{
    sErrFn = func;
    sErrObj = obj;
    Adx_ErrEntry(func ? ErrThunk : 0, obj);
}

/* ------------------------------------------------------------ the lock */

/* SVM's lock is io's Adx_Lock / Adx_Unlock. It nests: only the outermost pair
 * makes kernel calls, so the inner ones of a section already locked make none
 * (the original's trace: ADXT_Init's calls run under one lock). */

/* A section of the library that only takes and gives back the lock (an SJ's
 * create, or a stream's stop: their work is the core's own memory). */
static void LockedStep(void)
{
    Adx_Lock();
    Adx_Unlock();
}

/* ------------------------------------------------------------ CRI_ADX_Driver */

#define CRI_SID 0x90000200u
#define CRI_NSJ 6               /* the IOP's ring buffers ADXT_Init makes */
#define CRI_RING 0x4100u        /* each ring's IOP bytes */
#define CRI_STAT 0x880u         /* each status block the IOP sends each VBlank */

/* The players' records: the library's own work area at its PS2 address (the
 * original's), 16 records of 0xC4 bytes at
 * 0x19DF20 in main's .data, as the original's: its ADXT_Init clears the 0xC40
 * bytes, and its ADXT_Create returns 0x19DF20, then 0x19DFE4. Their contents
 * are the core's (the library's private memory). The block's address comes
 * from the data layer's DataC_At once, at ADXT_Init (one mechanism for the
 * libraries' work areas). */
#define ADXT_MAX 16
#define ADXT_REC 0xC4u          /* each record's bytes */
#define ADXT_TABLE 0x0019DF20u

static SifClientData sCri __attribute__((aligned(64)));
static u32 sCriSend[16] __attribute__((aligned(64)));
static u32 sCriRecv[16] __attribute__((aligned(64)));
/* What the IOP writes into EE memory: its two status blocks (each VBlank, by
 * CRI_ADX_Driver's thread 4) and the rings' records named at their creates. */
/* What crosses to the IOP holds PS2 addresses, the original's: the status blocks at 0x1B9780
 * (kind 0) and 0x1B38C0 (kind 1), in main's .data, taken once through
 * DataC_At at ADXT_Init; the rings' EE records 0x1B86F0 + 0x30 i and their
 * status 0x1B94E8 + 0x14 i, which the IOP only names back (block 0's
 * answers), never writes. */
#define EE_STAT0 0x001B9780u
#define EE_STAT1 0x001B38C0u
#define EE_SJ(i) (0x001B86F0u + 0x30u * (u32)(i))
#define EE_SJSTAT(i) (0x001B94E8u + 0x14u * (u32)(i))
static u8 *sEeStat[2];
static const u32 sEeStatPs2[2] = {EE_STAT0, EE_STAT1};

static s32 sInit;
static u8 *sTable;              /* DataC_At(ADXT_TABLE): the records */
static u32 sIopStat[2];         /* the IOP heap blocks of the status blocks */
static u32 sIopRing;            /* the rings' block */
static u32 sIopWork;            /* fno 0x40A's block */
static u32 sCriStat[2];         /* fno 2's replies */
static u32 sIopSj[CRI_NSJ];     /* fno 0x422's replies: the IOP's rings */
static u32 sIopSjObj[CRI_NSJ];  /* fno 0x400's replies */
static u8 sSjUsed[CRI_NSJ];

/* One call to CRI_ADX_Driver with the words already in sCriSend, under the lock
 * (the library's call wrapper takes it); the reply's first word. */
static u32 CriCall(s32 fno, s32 ssize, s32 rsize)
{
    Adx_Lock();
    sceSifCallRpc(&sCri, fno, 0, sCriSend, ssize, sCriRecv, rsize, 0, 0);
    Adx_Unlock();
    return rsize ? sCriRecv[0] : 0;
}

static void Send4(u32 a, u32 b, u32 c, u32 d)
{
    sCriSend[0] = a;
    sCriSend[1] = b;
    sCriSend[2] = c;
    sCriSend[3] = d;
}

/* A status block: an IOP heap block of 0x8D0 bytes, its buffer 0x40 bytes in;
 * fno 2 (kind, the EE buffer, the IOP buffer, 0x880). */
static void MakeStat(s32 kind)
{
    u32 iop = (u32)sceSifAllocIopHeap(CRI_STAT + 0x50);
    sIopStat[kind] = iop;
    sEeStat[kind] = (u8 *)DataC_At(sEeStatPs2[kind]);
    __builtin_memset(sEeStat[kind], 0, CRI_STAT);
    Send4((u32)kind, sEeStatPs2[kind], iop + 0x40, CRI_STAT);
    sCriStat[kind] = CriCall(2, 0x10, 4);
}

static void VsyncServer(void *arg);
static void PutServer(void *arg);

/* ADXT_Init: CRI ADX's start-up for the PS2's output (Sofdec's mwPlyInitSfdFx
 * calls it at the boot). Under one lock: the bind of CRI_ADX_Driver's server,
 * the status block of kind 0, fno 0x403, fno 0x40C (the output rate, 48000),
 * the rings' block (0x18640 bytes) and each ring's fno 0x422 (1, its buffer
 * 0x40 + 0x4100 * i into the block, 0x100) and fno 0x400 (its EE record, the
 * ring, 1, its EE status), the status block of kind 1, and fno 0x40A (6, a
 * block of 0x1840 bytes, its size); then the VSYNC server's registration and
 * ADX's FS server's (the original's order: 0x1602D8, then 0x1602F8). A second
 * call does nothing (not observed). */
void ADXT_Init(void)
{
    if (sInit++ > 0) {
        return;
    }
    Adx_Lock();
    sTable = (u8 *)DataC_At(ADXT_TABLE);
    __builtin_memset(sTable, 0, ADXT_MAX * ADXT_REC);
    sceSifBindRpc(&sCri, CRI_SID, 0);
    MakeStat(0);
    CriCall(0x403, 0, 0);
    sCriSend[0] = 48000;
    CriCall(0x40C, 4, 4);
    sIopRing = (u32)sceSifAllocIopHeap(CRI_NSJ * CRI_RING + 0x40);
    for (s32 i = 0; i < CRI_NSJ; i++) {
        Send4(1, sIopRing + 0x40 + CRI_RING * (u32)i, 0x100, 0);
        sIopSj[i] = CriCall(0x422, 0xC, 4);
        Send4(EE_SJ(i), sIopSj[i], 1, EE_SJSTAT(i));
        sIopSjObj[i] = CriCall(0x400, 0x10, 4);
    }
    MakeStat(1);
    sIopWork = (u32)sceSifAllocIopHeap(0x1840);
    Send4(6, sIopWork, 0x1840, 0);
    CriCall(0x40A, 0xC, 0);
    Adx_SvrAdd(ADX_SVR_VSYNC, VsyncServer, 0);
    Adx_FsServerStart();                /* ADX's FS server (the original's 0x1602F8) */
    Adx_SvrAdd(ADX_SVR_FS, PutServer, 0);
    Adx_Unlock();
}

/* ------------------------------------------------------------ the players */


typedef struct AdxtRec {
    u8 used;                    /* 0: free; 1 once made */
    s8 stat;                    /* ADXT_GetStat */
    u8 maxnch;
    u8 waitPlayStart;           /* ADXT_SetWaitPlayStart */
    u8 readyPlayStart;          /* ADXT_IsReadyPlayStart */
    u8 pad[3];
    void *work;
    s32 worksize;
    u32 iopOut;                 /* fno 0x408's reply: the IOP's output */
    AdxStm *stm;                /* the stream (io's: ADX's server pass visits it) */
    s32 sj[CRI_NSJ];            /* the IOP rings it took */
    s32 pause;                  /* ADXT_Pause's switch */
    s32 vol;                    /* ADXT_SetOutVol (0.1 dB) */
    u32 stopReq, stopSent;      /* the output's command 8: asked, last sent */
    u32 levelSent;              /* the output's command 5: last level sent */
    u32 monoSent;               /* the output's command 13: last mono word sent */
} AdxtRec;

_Static_assert(sizeof(AdxtRec) <= ADXT_REC, "an ADXT record outgrows its slot");
#define REC(i) ((AdxtRec *)(sTable + ADXT_REC * (u32)(i)))

/* ------------------------------------------------------------ the starts' state
 *
 * What a start does, as the original game was seen doing it
 * (the village route's start of AFS00's file 0x2F, the camp
 * route's of 0xA5 and 0x98):
 * - the work area: from its first 64-byte boundary, each channel's output
 *   ring (0x4000 bytes of 16-bit PCM, then a copy of its first 0xC0 bytes),
 *   then the input ring to the area's end, of which the stream fills whole
 *   sectors, all but the last sector's worth (79 of the game's 0x28054
 *   bytes), then a copy of its first 0x24 bytes;
 * - the stream: the first read 25 sectors (all five starts seen); then, at
 *   each VBlank after the decode and the send, a read whenever more than 12
 *   sectors of the ring are free (the bytes asked and not yet decoded held;
 *   exactly 12 make none: late-bulldrome's stream reads at 7952, 8034, 8116
 *   and 8627): all the whole free sectors, up to the ring's end and the
 *   file's (54 after the first read, then 12 at a time, and what is left to
 *   the ring's end);
 * - the VBlank after the first read has ended, the header: the state 2
 *   (PREP), and the output's rate and pans when they change (commands 4 and
 *   9); its stop flag is cleared (command 8 with 0) once the IOP's ring is
 *   all free again by its answers (at the header, or the first VBlank after
 *   it that finds it so: 0x98's start on the ended player of 0xA5 waited a
 *   VBlank for the IOP's flush of the old file); no decode then;
 * - each VBlank after the header's, the decode of each channel into its
 *   output ring, at most 0x1300 bytes (2,432 samples) a VBlank (0x980 for a
 *   file without loop info), whole frames, up to the ring's end and as far as
 *   the ring holds what was not sent before the last VBlank (the original's
 *   output ring frees a VBlank's send at the next VBlank's end); the input as
 *   Decode says (the ring's end, the loop's start, the end mark);
 * - then what is decoded and not sent goes to the IOP's ring (one SifSetDma
 *   per channel), as far as the IOP's answers have freed it, up to the ring's
 *   end; block 0 announces it the next VBlank (late-bulldrome: the input
 *   position at every VBlank start equal to the original's, its word
 *   0x1B46D0, to the crash at 8651);
 * - at the third VBlank after the header's, play (command 2 with 1) and the
 *   state 3 (PLAYING);
 * - the file's end: its last frame, when it holds fewer than 32 of the
 *   file's samples, is taken from the input with the frames before it, but
 *   decoded after the send once the output ring is empty (all sent before
 *   this VBlank); then its samples, and zeros to the VBlank's decode size
 *   (0x980 bytes for 0xA5: the original's transfer at 6065), and the state 4
 *   (DECEND). A file whose samples end with a whole frame (0xA6: 532,480
 *   samples) has no such frame: DECEND comes when the ring is empty after
 *   its last frame (the l2 route on the late2 card, VBlank 5747: the last
 *   0x400 bytes sent at 5746, the state 4 at 5747, nothing sent then); the
 *   zeros to the decode size are the partial frame's (not seen: the game
 *   starts the next file at once);
 * - after it, silence decoded as far as the IOP's answers free the ring
 *   (End), and PLAYEND (5) once the answers show the file's
 *   last sample played: the output stopped then (PlayStep).
 */
typedef struct AdxtPlay {
    s32 on;                     /* a start's machine runs */
    s32 nch;                    /* the file's channels */
    u32 rate;
    s32 c1, c2;                 /* the decoder's coefficients */
    s32 h1[2], h2[2];           /* each channel's last two samples */
    u32 dataOfs;                /* the first frame's file offset */
    u32 decMax;                 /* the bytes decoded a VBlank at most (each channel) */
    s32 stopClr;                /* the header's stop flag clear, waiting for the IOP's ring */
    u32 smpTotal, smpDone;      /* the file's samples, the decoded ones */
    s32 ended;                  /* the last frame decoded: silence follows */
    s32 tailIn;                 /* the last frame taken from the input */
    u32 tailAt;                 /* a last frame of fewer than 32 samples: its file offset */
    s32 markDue;                /* the end mark's turn: the VBlank after the last frame's */
    u32 decEnd;                 /* the output bytes of the file's samples */
    u8 *obuf[2];                /* each channel's output ring */
    u8 *ibuf;                   /* the input ring */
    u32 ringSct;                /* its sectors */
    u32 fileSct;                /* the file's sectors */
    u32 rdSct;                  /* the sectors asked so far (from the file's start) */
    u32 rdN;                    /* the read in flight's sectors, 0 for none */
    u32 rdAt;                   /* its first sector in the ring */
    s32 rdPut;                  /* its end seen by PutServer */
    u32 inTop;                  /* the bytes read (ended reads) */
    u32 inPos;                  /* the bytes decoded */
    s32 header;                 /* the header taken */
    s32 decodes;                /* the decodes in PREP */
    u32 dec, sent;              /* the output bytes decoded, and sent (each channel) */
    u32 sentOld;                /* sent before the last VBlank's send: the decode's room */
    u32 loopSmp;                /* the loop's start sample, 0 for none */
    u32 loopEndSmp;             /* the loop's end sample (header 0x30), 0 for none */
    u32 loopLen;                /* a looping file's lap in sectors (0 for none), from the loop's start byte's sector */
    u32 loopStartSct, loopEndSct;   /* the lap's first sector, and the sector after the loop's end byte */
    u32 loopStartByte;          /* the loop's start byte (header 0x2C) */
    u32 laps;                   /* the laps decoded to their ends */
    s32 lapEnd;                 /* a lap's last frame is decoded: the loop back is the next decode's */
    s32 lh1[2], lh2[2];         /* the decoder's history at the loop's start sample, the first time (every lap starts from it) */
    u32 iopFree;                /* the IOP ring's bytes free, by its answers */
    u32 annPos, annN, annNch;   /* the last VBlank's transfer, for block 0 */
    u32 nxtPos, nxtN, nxtNch;   /* this VBlank's, announced at the next */
    u32 playReq, playSent;      /* the output's command 2 */
    u32 rateSent;               /* command 4 */
    s32 panSent[2];             /* command 9 */
} AdxtPlay;

static AdxtPlay sPlay[ADXT_MAX];
static s32 sMono;               /* ADXT_SetOutputMono */

#define ADXT_OBUF 0x4000u       /* each output ring's PCM bytes */
#define ADXT_OSTRIDE 0x40C0u    /* the output rings' spacing in the work area */
#define ADXT_OXTRA 0xC0u        /* each output ring's extra bytes after its end */
#define ADXT_IXTRA 0x24u        /* the input ring's bytes read through after its end */
#define ADXT_FIRST_RD 25u       /* a start's first read (sectors) */
#define ADXT_RELOAD 12u         /* a read once more than these sectors are free */
#define ADXT_DEC_MAX 0x1300u    /* the bytes decoded a VBlank (each channel) */
#define ADXT_PREP_VBLANKS 3     /* the VBlanks after the header's before play */
#define IOP_RING_DATA 0x100u    /* the IOP ring's PCM from its buffer's start */

/* ------------------------------------------------------------ the VSYNC server */

/* The output's level for a volume in 0.1 dB: 256 * 10^(vol / 200), rounded
 * down (the port's own table, from that formula), 0 from -482 down. The
 * original's library was called at its interface (ADXT_SetOutVol on player
 * 0, one volume a VBlank) with every volume from 0 to -1000, and its command
 * 5 read at each of those VBlanks: the formula gives all 1,001 levels.
 * The game's own volumes (gVolumeDb) all
 * happen to give even levels. */
static const u16 sLevel[482] = {
    256, 253, 250, 247, 244, 241, 238, 236, 233, 230, 228, 225, 222, 220, 217, 215, 212, 210, 208, 205,
    203, 201, 198, 196, 194, 191, 189, 187, 185, 183, 181, 179, 177, 175, 173, 171, 169, 167, 165, 163,
    161, 159, 157, 156, 154, 152, 150, 149, 147, 145, 143, 142, 140, 139, 137, 135, 134, 132, 131, 129,
    128, 126, 125, 123, 122, 121, 119, 118, 117, 115, 114, 113, 111, 110, 109, 107, 106, 105, 104, 103,
    101, 100, 99, 98, 97, 96, 95, 94, 92, 91, 90, 89, 88, 87, 86, 85, 84, 83, 82, 81,
    80, 80, 79, 78, 77, 76, 75, 74, 73, 72, 72, 71, 70, 69, 68, 68, 67, 66, 65, 65,
    64, 63, 62, 62, 61, 60, 60, 59, 58, 57, 57, 56, 56, 55, 54, 54, 53, 52, 52, 51,
    51, 50, 49, 49, 48, 48, 47, 47, 46, 46, 45, 45, 44, 43, 43, 42, 42, 41, 41, 41,
    40, 40, 39, 39, 38, 38, 37, 37, 37, 36, 36, 35, 35, 34, 34, 34, 33, 33, 32, 32,
    32, 31, 31, 31, 30, 30, 30, 29, 29, 29, 28, 28, 28, 27, 27, 27, 26, 26, 26, 25,
    25, 25, 25, 24, 24, 24, 23, 23, 23, 23, 22, 22, 22, 22, 21, 21, 21, 21, 20, 20,
    20, 20, 19, 19, 19, 19, 18, 18, 18, 18, 18, 17, 17, 17, 17, 17, 16, 16, 16, 16,
    16, 15, 15, 15, 15, 15, 15, 14, 14, 14, 14, 14, 14, 13, 13, 13, 13, 13, 13, 12,
    12, 12, 12, 12, 12, 12, 11, 11, 11, 11, 11, 11, 11, 11, 10, 10, 10, 10, 10, 10,
    10, 10, 9, 9, 9, 9, 9, 9, 9, 9, 9, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    8, 8, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 6, 6, 6, 6, 6, 6, 6,
    6, 6, 6, 6, 6, 6, 6, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
    5, 5, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
    3, 3, 3, 3, 3, 3, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1,
};

static u32 Level(s32 vol)
{
    if (vol >= 0) {
        return sLevel[0];
    }
    return -vol < (s32)(sizeof(sLevel) / sizeof(sLevel[0])) ? sLevel[-vol] : 0;
}

/* The two SifSetDma records (one transfer each: the EE block, its IOP buffer). */
static u32 sDma[2][4] __attribute__((aligned(16)));
static s32 sCmdN;

/* A command into block 1's list: the first nw words of its 16-byte entry
 * (command 8 leaves the fourth word as the IOP sent it back). */
static void Cmd(u32 cmd, u32 out, u32 a, u32 b, s32 nw)
{
    u32 *e = (u32 *)sEeStat[1] + 4 + 4 * sCmdN;
    if (0x10 + 0x10 * (sCmdN + 1) > 0x840) {
        Err("E1006: ADXT: too many output commands");
        return;
    }
    e[0] = cmd;
    e[1] = out;
    e[2] = a;
    if (nw > 3) {
        e[3] = b;
    }
    sCmdN++;
}

/* Each output's changes since the last VBlank, in the players' order: command
 * 8 (the stop flag), 2 with 0 (a stop's or a pause's), 5 (0, the level), 4
 * (the rate), 9 (each channel's pan), 13 (the mono word), then 2 with 1 (the
 * resume, a start's play). Seen: 8, 4, 9, 9 at a first start's header; 8, 2,
 * 5 at a start on a player that had ended; 5, 2 at a resume with a new volume
 * (save-load's VBlank 9945); 2, 5 at a pause with a new volume (the house
 * route's 5458, and the l2 route on the late2 card, 5958); 5, 9, 9, 13 and
 * 9, 9, 13 at the sound mode's changes (the options route's 3562, 3592). The
 * place of 4 against 5, and of 4, 9 and 13 against the last 2, is this
 * file's guess (never seen together). */
static void Commands(void)
{
    sCmdN = 0;
    for (s32 i = 0; i < ADXT_MAX; i++) {
        AdxtRec *r = REC(i);
        AdxtPlay *p = &sPlay[i];
        u32 level, play, mono;
        if (!r->used) {
            continue;
        }
        if (r->stopReq != r->stopSent) {
            Cmd(8, r->iopOut, r->stopReq, 0, 3);
            r->stopSent = r->stopReq;
        }
        play = p->playReq && !r->pause;
        if (play != p->playSent && !play) {
            /* A stop's (with its command 8) and a pause's: before the level's
             * command 5 (the l2 route on the late2 card, VBlank 5958: Pause(1)
             * and SetOutVol at 5957 give 2 with 0, then 5). */
            Cmd(2, r->iopOut, play, 0, 3);
            p->playSent = play;
        }
        level = Level(r->vol);
        if (level != r->levelSent) {
            Cmd(5, r->iopOut, 0, level, 4);
            r->levelSent = level;
        }
        if (p->on && p->header) {
            if (p->rate != p->rateSent) {
                Cmd(4, r->iopOut, p->rate, 0, 3);
                p->rateSent = p->rate;
            }
            for (s32 ch = 0; ch < p->nch && ch < 2; ch++) {
                s32 pan = (p->nch < 2 || sMono) ? 0 : (ch ? 15 : -15);
                if (pan != p->panSent[ch]) {
                    Cmd(9, r->iopOut, (u32)ch, (u32)pan, 4);
                    p->panSent[ch] = pan;
                }
            }
        }
        mono = sMono ? 0xFFFFFF80u : 0u;
        if (mono != r->monoSent) {
            Cmd(13, r->iopOut, mono, 0, 3);
            r->monoSent = mono;
        }
        if (play != p->playSent) {
            Cmd(2, r->iopOut, play, 0, 3);      /* the resume's, a start's play: after 5 */
            p->playSent = play;
        }
    }
    ((u32 *)sEeStat[1])[0] = (u32)sCmdN;
}

/* A status block to its IOP buffer: the last word one more, then one
 * SifSetDma of the whole block. */
static void SendBlock(s32 kind)
{
    u32 *b = (u32 *)sEeStat[kind];
    b[CRI_STAT / 4 - 1]++;
    sDma[kind][0] = (u32)sEeStat[kind];
    sDma[kind][1] = sIopStat[kind] + 0x40;
    sDma[kind][2] = CRI_STAT;
    sDma[kind][3] = 0;
    SifSetDma(sDma[kind], 1);
}

/* ------------------------------------------------------------ a start's VBlank */

static u32 Be32(const u8 *b)
{
    return ((u32)b[0] << 24) | ((u32)b[1] << 16) | ((u32)b[2] << 8) | b[3];
}

/* The input ring's byte at file offset ofs (already read). */
static u8 InByte(const AdxtPlay *p, u32 ofs)
{
    return p->ibuf[ofs % (p->ringSct * IO_SECTOR)];
}

static u32 InBe32(const AdxtPlay *p, u32 ofs)
{
    return ((u32)InByte(p, ofs) << 24) | ((u32)InByte(p, ofs + 1) << 16) | ((u32)InByte(p, ofs + 2) << 8) | InByte(p, ofs + 3);
}

/* A read's end: into the ring's start, its first ADXT_IXTRA bytes copied
 * after the ring's end too, where a frame that straddles the end is read on
 * (the original's input ring at every lap, by the VBlank after the read's
 * end: late-bulldrome's 2760). */
static void Put(AdxtPlay *p)
{
    if (!p->rdPut) {
        p->rdPut = 1;
        if (p->rdAt == 0) {
            __builtin_memcpy(p->ibuf + p->ringSct * IO_SECTOR, p->ibuf, ADXT_IXTRA);
        }
    }
}

static void StreamAsk(AdxtRec *r, AdxtPlay *p);

/* A read that has ended, in the FS pass in which it ended: the next read at
 * once when the ring has the room (StreamAsk's test), and a second pass of
 * ADX's FS server for it to start in this wake of the FS thread. Else the
 * read stays as it was, to be taken by PlayStep. Without it the ask waits
 * for the next VSYNC server (PlayStep), a VBlank later. The two orders
 * differ only when more than ADXT_RELOAD sectors are free at the read's end,
 * which a read cut short by a loop's end gives (watch-contest's BGM, the lap
 * of VBlank 62281: a chunk of 1 sector; the original's next read in the
 * pass of the chunk's end, the core's a VBlank later). The decode's room
 * (the usual ask) is PlayStep's. */
static void AskAtEnd(AdxtRec *r, AdxtPlay *p)
{
    u32 inTop = p->inTop, rdN = p->rdN;
    p->inTop = p->rdSct * IO_SECTOR;
    p->rdN = 0;
    StreamAsk(r, p);
    if (!p->rdN) {
        p->inTop = inTop;
        p->rdN = rdN;
        return;
    }
    Adx_FsServer(0);
}

/* In the FS thread each VBlank, after ADX's FS server: a read that has
 * ended is put (its data the decode's from the next VSYNC server on). */
static void PutServer(void *arg)
{
    (void)arg;
    for (s32 i = 0; i < ADXT_MAX; i++) {
        AdxtPlay *p = &sPlay[i];
        if (REC(i)->used && p->on && p->rdN && AdxStm_State(REC(i)->stm) == ADX_STM_END) {
            Put(p);
            if (p->header) {
                AskAtEnd(REC(i), p);
            }
        }
    }
}

/* The stream: an ended read taken. */
static void StreamTake(AdxtRec *r, AdxtPlay *p)
{
    if (p->rdN && AdxStm_State(r->stm) == ADX_STM_END) {
        p->inTop = p->rdSct * IO_SECTOR;
        Put(p);
        p->rdN = 0;
    }
}

/* A looping file's stream: the sectors from the first to the one holding the
 * loop's end byte, then the loop's start sector on to the same end again and
 * again; v counts them all (quest-clear's file 0x74: a read of 6 sectors to
 * lba 1689065 at 25977, the next at 1687380, its loop start's sector, at
 * 25991). The file sector of v. */
static u32 LoopFileSct(const AdxtPlay *p, u32 v)
{
    if (!p->loopLen || v < p->loopEndSct) {
        return v;
    }
    return p->loopStartSct + (v - p->loopEndSct) % p->loopLen;
}

/* The sectors of the read from the stream's sector v on: the first read's
 * 25, then all the whole free sectors, up to the ring's end and the file's (a
 * lap's) end. */
static u32 ReadSize(const AdxtPlay *p, u32 v)
{
    u32 ringBytes = p->ringSct * IO_SECTOR, held = v * IO_SECTOR - p->inPos, n = v == 0 ? ADXT_FIRST_RD : (ringBytes - held) / IO_SECTOR;
    u32 at = v % p->ringSct, fs = LoopFileSct(p, v), cap = p->loopLen ? p->loopEndSct - fs : p->fileSct - fs;
    if (n > p->ringSct - at) {
        n = p->ringSct - at;
    }
    return n > cap ? cap : n;
}

/* The read's size when the device takes it (AdxStm_ReadSized): the free
 * bytes then, which the decode of the VBlanks since the ask grew. */
static u32 ReadResize(void *arg)
{
    AdxtPlay *p = (AdxtPlay *)arg;
    u32 v = p->rdSct - p->rdN, n = ReadSize(p, v);
    if (n > p->rdN) {
        p->rdSct = v + n;
        p->rdN = n;
    }
    return p->rdN;
}

/* A read, when none is in flight and enough sectors are free. */
static void StreamAsk(AdxtRec *r, AdxtPlay *p)
{
    u32 ringBytes = p->ringSct * IO_SECTOR, held, n, at, fs;
    if (p->rdN || (!p->loopLen && p->rdSct >= p->fileSct)) {
        return;
    }
    held = p->rdSct * IO_SECTOR - p->inPos;
    if (p->rdSct != 0 && ringBytes - held <= ADXT_RELOAD * IO_SECTOR) {
        return;
    }
    n = ReadSize(p, p->rdSct);
    at = p->rdSct % p->ringSct;
    fs = LoopFileSct(p, p->rdSct);
    AdxStm_ReadSized(r->stm, fs, n, p->ibuf + at * IO_SECTOR, ReadResize, p);
    p->rdSct += n;
    p->rdN = n;
    p->rdAt = at;
    p->rdPut = 0;
}

/* The header, once the first read has ended: an ADX of 18-byte frames
 * (0x80 0x00, the data's offset - 4, type, frame size, bits, channels, rate,
 * samples, cutoff). Anything else: the error state. */
static void Header(AdxtRec *r, AdxtPlay *p)
{
    u8 h[20];
    for (u32 k = 0; k < sizeof(h); k++) {
        h[k] = InByte(p, k);
    }
    p->header = 1;
    if (h[0] != 0x80 || h[5] != 18 || h[6] != 4 || h[7] < 1 || h[7] > 2) {
        Err("E1010: ADXT: not an ADX this library plays");
        r->stat = ADXT_STAT_ERROR;
        p->on = 0;
        return;
    }
    p->dataOfs = (((u32)h[2] << 8) | h[3]) + 4;
    p->nch = h[7];
    p->rate = Be32(h + 8);
    p->smpTotal = Be32(h + 12);
    AdxDec_Coefs(((u32)h[16] << 8) | h[17], p->rate, &p->c1, &p->c2);
    /* A looping file (a version 4 header long enough to hold the loop's
     * fields, with its loop flag set) decodes at most ADXT_DEC_MAX bytes a
     * VBlank; one without decodes half (camp's file 0xA5, the original game).
     * Its loop's start sample (0x28) ends a VBlank's decode the first time
     * the decode reaches it (Decode). */
    p->decMax = ADXT_DEC_MAX / 2;
    p->loopSmp = 0;
    p->loopEndSmp = 0;
    p->loopLen = 0;
    p->laps = 0;
    p->lapEnd = 0;
    u32 lf = 0;   /* the loop fields' place (the flag; then the start sample, start byte, end sample, end byte) */
    if (p->dataOfs >= 0x38 && InBe32(p, 0x24)) {
        p->decMax = ADXT_DEC_MAX;
        p->loopSmp = InBe32(p, 0x28);
        lf = InByte(p, 0x12) == 4 ? 0x24 : 0;
    }
    /* A version 3 header holds them from 0x18 (l2rath's file 0xBD: flag 1, start
     * sample 858,112 at byte 0xEC000, end sample 2,531,321, end byte 0x2B7900). */
    if (InByte(p, 0x12) == 3 && p->dataOfs >= 0x2C && InBe32(p, 0x18) == 1) {
        lf = 0x18;
        p->loopSmp = InBe32(p, 0x1C);
    }
    if (lf) {
        p->loopEndSmp = InBe32(p, lf + 12);
        p->loopStartByte = InBe32(p, lf + 8);
        p->loopStartSct = p->loopStartByte / IO_SECTOR;
        p->loopEndSct = (InBe32(p, lf + 16) + IO_SECTOR - 1) / IO_SECTOR;
        if (p->loopEndSct > p->loopStartSct && p->loopEndSct <= p->fileSct && p->loopEndSmp > p->loopSmp) {
            p->loopLen = p->loopEndSct - p->loopStartSct;
        }
    }
    /* A version 4 header holds each channel's two starting history samples
     * (big-endian, from 0x18, four bytes a channel): the decoder starts from
     * them (combine's file 0x1C: -5 and -5 in both channels, the original's
     * first transfer at VBlank 11685). Every version 4 header of AFS00 has
     * equal pairs, so which of the two is the last sample isn't seen. */
    p->h1[0] = p->h1[1] = p->h2[0] = p->h2[1] = 0;
    if (InByte(p, 0x12) == 4 && p->dataOfs >= 0x18 + 4u * (u32)p->nch) {
        for (s32 ch = 0; ch < p->nch; ch++) {
            u32 at = 0x18 + 4u * (u32)ch;
            p->h1[ch] = (s16)(InByte(p, at) << 8 | InByte(p, at + 1));
            p->h2[ch] = (s16)(InByte(p, at + 2) << 8 | InByte(p, at + 3));
        }
    }
    p->inPos = p->dataOfs;
    p->stopClr = 1;
    r->stat = ADXT_STAT_PREP;
}

/* The output rings' first ADXT_OXTRA bytes, copied after their ends too
 * (the original's rings hold that copy at every VBlank seen): what was just
 * written at offset pos, n bytes. */
static void Mirror(AdxtPlay *p, u32 pos, u32 n)
{
    if (pos + n > ADXT_OBUF) {
        /* A frame that straddles the ring's end (after a lap's partial frame
         * the position is no longer a multiple of a frame): its part past the
         * end is the ring's start's too. */
        for (s32 ch = 0; ch < p->nch; ch++) {
            __builtin_memcpy(p->obuf[ch], p->obuf[ch] + ADXT_OBUF, pos + n - ADXT_OBUF);
        }
    }
    if (pos < ADXT_OXTRA) {
        if (n > ADXT_OXTRA - pos) {
            n = ADXT_OXTRA - pos;
        }
        for (s32 ch = 0; ch < p->nch; ch++) {
            __builtin_memcpy(p->obuf[ch] + ADXT_OBUF + pos, p->obuf[ch] + pos, n);
        }
    }
}

/* One frame of each channel from the input at file offset at into its ring
 * at offset dec, keep bytes of it, then zeros to size bytes. */
static void DecodeFrame(AdxtPlay *p, u32 at, u32 keep, u32 size)
{
    for (s32 ch = 0; ch < p->nch; ch++) {
        u8 f[18];
        s16 out[32];
        u8 *o = p->obuf[ch] + p->dec % ADXT_OBUF;
        for (u32 k = 0; k < 18; k++) {
            f[k] = InByte(p, at + 18u * (u32)ch + k);
        }
        AdxDec_Frame(f, out, p->c1, p->c2, &p->h1[ch], &p->h2[ch]);
        __builtin_memcpy(o, out, keep);
        if (size > keep) {
            __builtin_memset(o + keep, 0, size - keep);
        }
    }
    Mirror(p, p->dec % ADXT_OBUF, size);
    p->dec += size;
}

/* The decode: whole frames, each channel into its ring at the same offset,
 * as far as the ring holds what was sent before the last VBlank (roomFreed),
 * at most decMax bytes, up to the ring's end.
 * The input: what the ring holds from the position on, through its end and
 * the ADXT_IXTRA bytes after it, so a frame that straddles the ring's end
 * ends the VBlank's decode (late-bulldrome: the original's position 0x20,
 * 0x18, 0x10 ... after its laps). The decode's first arrival at the loop's
 * start sample ends it too (late-bulldrome's file 0x2F at 2872, 112,416
 * samples; 0x34 at 6159, 230,144).
 * The file's end: a last frame with fewer than 32 of the file's samples is
 * taken from the input with the frames before it (End decodes it later), and
 * the end mark after the last frame (0x80 0x01, its length) the VBlank after
 * (late-bulldrome's file 0xA7 at 5871 and 5872: 36 bytes, then 18). */
static void DecodeRun(AdxtPlay *p, u32 roomFreed)
{
    u32 room = ADXT_OBUF - (p->dec - roomFreed), end = ADXT_OBUF - p->dec % ADXT_OBUF;
    u32 frameBytes = 18u * (u32)p->nch, n = p->decMax;
    u32 ringBytes = p->ringSct * IO_SECTOR, ro = p->inPos % ringBytes, avail = p->inTop - p->inPos;
    u32 stop = p->loopLen && p->smpDone >= p->loopSmp ? p->loopEndSmp : p->smpTotal;
    u32 lim = p->smpDone < p->loopSmp ? p->loopSmp : stop;
    /* A loop that ends at the file's end (loop end sample = the total: VBlank 198548 of the timeout route's file
     * 0xD4, 2,197,038 samples, 14 in the last frame): the last frame is decoded whole as the lap's end frame
     * (18 samples past the loop's end, as any lap's), and the file's end is never reached. */
    int loopToEnd = p->loopLen && p->loopEndSmp >= p->smpTotal;
    if (p->markDue) {
        if (avail >= 4 && InByte(p, p->inPos) == 0x80 && InByte(p, p->inPos + 1) == 0x01) {
            u32 len = 4u + ((u32)InByte(p, p->inPos + 2) << 8 | InByte(p, p->inPos + 3));
            if (avail >= len) {
                p->inPos += len;
                p->markDue = 0;
            }
        } else {
            p->markDue = 0;
        }
        return;
    }
    if (n > room) {
        n = room;
    }
    if (avail > ringBytes + ADXT_IXTRA - ro) {
        avail = ringBytes + ADXT_IXTRA - ro;
    }
    while (n >= 64 && end && (p->smpTotal - p->smpDone >= 32 || loopToEnd) && p->smpDone < lim && avail >= frameBytes) {
        DecodeFrame(p, p->inPos, 64, 64);
        p->inPos += frameBytes;
        p->smpDone += 32;
        n -= 64;
        end = end > 64 ? end - 64 : 0;
        avail -= frameBytes;
        if (p->loopLen && !p->laps && p->smpDone == p->loopSmp) {
            for (s32 ch = 0; ch < p->nch; ch++) {
                p->lh1[ch] = p->h1[ch];
                p->lh2[ch] = p->h2[ch];
            }
        }
    }
    if (p->loopLen && p->smpDone >= p->loopEndSmp) {
        p->lapEnd = 1;
    }
    if (loopToEnd) {
        /* no tail: the loop goes on */
    } else if (p->smpDone == p->smpTotal && !p->tailIn) {
        p->tailIn = 1;
        p->markDue = 1;
    } else if (p->smpTotal - p->smpDone < 32 && !p->tailIn && n >= 64 && end && avail >= frameBytes) {
        p->tailAt = p->inPos;
        p->inPos += frameBytes;
        p->tailIn = 1;
        p->markDue = 1;
    }
}

/* The loop back of a lap whose last frame was decoded (the next decode's,
 * not the cut's: the original's input position is at the loop's end byte
 * at the VBlank between, l2rath's file 0xBD at 20939). */
static void LapJump(AdxtPlay *p)
{
    /* The lap's end, reached by whole frames: the last frame is decoded
     * whole, and the output position goes back by its samples past the
     * loop's end; the lap starts again at the loop's start frame from the
     * history the first pass had there (quest-clear's file 0x74 at 26143:
     * the last frame's 32 samples, written over by the loop's first
     * frame's from the 7th). */
    p->dec -= (p->smpDone - p->loopEndSmp) * 2u;
    for (s32 ch = 0; ch < p->nch; ch++) {
        p->h1[ch] = p->lh1[ch];
        p->h2[ch] = p->lh2[ch];
    }
    p->inPos = (p->loopEndSct + p->laps * p->loopLen - p->loopStartSct) * IO_SECTOR + p->loopStartByte;
    p->smpDone = p->loopSmp;
    p->laps++;
    p->lapEnd = 0;
}

static void Decode(AdxtPlay *p, u32 roomFreed)
{
    if (p->lapEnd) {
        LapJump(p);
    }
    DecodeRun(p, roomFreed);
}

/* After the VBlank's send: the file's end, once all of it was sent before
 * this VBlank (freed): the last frame's samples and zeros to the VBlank's
 * decode size, and the state 4 (DECEND); after it, silence. */
static void End(AdxtRec *r, AdxtPlay *p, u32 freed)
{
    u32 room = ADXT_OBUF - (p->dec - freed), end = ADXT_OBUF - p->dec % ADXT_OBUF, n = p->decMax;
    if (p->ended) {
        /* After the file's end, silence: as far as the ring's extra bytes
         * (what passes its end is the ring's start's), so the IOP plays on
         * until PLAYEND (combine's VBlanks 11861 to 11868: 0x740 bytes sent
         * to the ring's end, then 0xA40 from its start). */
        u32 pos = p->dec % ADXT_OBUF, xend = ADXT_OBUF + ADXT_OXTRA - pos;
        if (n > room) {
            n = room;
        }
        if (n > xend) {
            n = xend;
        }
        for (s32 ch = 0; ch < p->nch; ch++) {
            __builtin_memset(p->obuf[ch] + pos, 0, n);
            if (pos + n > ADXT_OBUF) {
                __builtin_memset(p->obuf[ch], 0, pos + n - ADXT_OBUF);
            }
        }
        Mirror(p, pos, n);
        p->dec += n;
        return;
    }
    if (p->tailIn && p->dec == freed) {
        /* Up to the ring's end and the extra bytes after it, as the silence
         * (pets' file at 5882: 384 to the end, 192 more, so the next VBlank's
         * 2,432 end at 2,624). */
        u32 size = p->decMax < end + ADXT_OXTRA ? p->decMax : end + ADXT_OXTRA, keep = (p->smpTotal - p->smpDone) * 2u;
        p->decEnd = p->dec + keep;
        if (keep) {
            DecodeFrame(p, p->tailAt, keep, size);
        } else {
            for (s32 ch = 0; ch < p->nch; ch++) {
                __builtin_memset(p->obuf[ch] + p->dec % ADXT_OBUF, 0, size);
            }
            Mirror(p, p->dec % ADXT_OBUF, size);
            p->dec += size;
        }
        p->smpDone = p->smpTotal;
        p->ended = 1;
        if (r->stat == ADXT_STAT_PLAYING) {
            r->stat = ADXT_STAT_DECEND;
        }
    }
}

/* What is decoded and not sent, to the IOP's rings as far as its answers
 * have freed them (one SifSetDma a channel, each 0x10 bytes of records). */
static u32 sPcmDma[2][4] __attribute__((aligned(16)));

static void Send(AdxtRec *r, AdxtPlay *p)
{
    u32 at = p->sent % ADXT_OBUF, n = p->dec - p->sent;
    if (n > p->iopFree) {
        n = p->iopFree;
    }
    if (n > ADXT_OBUF - at) {
        n = ADXT_OBUF - at;
    }
    if (!n) {
        return;
    }
    for (s32 ch = 0; ch < p->nch; ch++) {
        sPcmDma[ch][0] = (u32)(p->obuf[ch] + at);
        sPcmDma[ch][1] = sIopRing + 0x40 + CRI_RING * (u32)r->sj[ch] + IOP_RING_DATA + at;
        sPcmDma[ch][2] = n;
        sPcmDma[ch][3] = 0;
        SifSetDma(sPcmDma[ch], 1);
    }
    p->sent += n;
    p->iopFree -= n;
    p->nxtPos = at;
    p->nxtN = n;
    p->nxtNch = (u32)p->nch;
}

/* A start's work at a VBlank: the ended read taken, the header (the VBlank
 * after the first read ends: no decode then), the stop flag's clear; from the
 * next VBlank the decode, then the send of what is decoded, then play once
 * PREP has had its VBlanks; last, a read asked (late-bulldrome: the
 * original's first decode at 2746, the VBlank after the header's, with its
 * first send in that same VBlank; a read asked after a decode that leaves
 * more than 12 sectors free, at 2758). */
static void PlayStep(AdxtRec *r, AdxtPlay *p)
{
    u32 freed = p->sent, roomFreed = p->sentOld;
    s32 first = !p->header;
    /* PLAYEND: the IOP's answers show the file's last sample played (the
     * output ring's bytes sent less those the IOP holds); the output stopped
     * as by ADXT_Stop (commands 8 with 1 and 2 with 0 at this VBlank), and
     * nothing more sent (combine's VBlank 11869). */
    if (r->stat == ADXT_STAT_DECEND && (s32)(p->sent - (ADXT_OBUF - p->iopFree) - p->decEnd) >= 0) {
        r->stat = ADXT_STAT_PLAYEND;
        r->stopReq = 1;
        p->playReq = 0;
        p->on = 0;
        return;
    }
    StreamTake(r, p);
    p->sentOld = freed;
    if (first) {
        if (p->inTop == 0) {
            StreamAsk(r, p);
            return;
        }
        Header(r, p);
        if (!p->on) {
            StreamAsk(r, p);
            return;
        }
    }
    if (p->stopClr && p->iopFree == ADXT_OBUF) {
        r->stopReq = 0;
        p->stopClr = 0;
    }
    if (!first) {
        if (!p->ended) {
            Decode(p, roomFreed);
        }
        Send(r, p);
        if (r->stat == ADXT_STAT_PREP && ++p->decodes == ADXT_PREP_VBLANKS) {
            p->playReq = 1;
            r->stat = ADXT_STAT_PLAYING;
        }
        End(r, p, freed);
    }
    StreamAsk(r, p);
}

/* Block 0: the IOP's answers taken (each frees bytes of a ring, a stopped
 * player's too: the IOP's flush after a stop), then the last VBlank's
 * transfers announced (a restarted player's old file's too); a VBlank with
 * none leaves the answer's words and sends the count 0. */
static void Block0(void)
{
    u32 *b = (u32 *)sEeStat[0];
    u32 n = b[0], k = 0;
    for (u32 e = 0; e < n && 4 + 4 * e + 3 < CRI_STAT / 4; e++) {
        const u32 *a = b + 4 + 4 * e;
        if (a[0] != 0) {
            continue;
        }
        for (s32 i = 0; i < ADXT_MAX; i++) {
            AdxtRec *r = REC(i);
            if (r->used && a[1] == EE_SJSTAT(r->sj[0])) {
                sPlay[i].iopFree += a[3];
            }
        }
    }
    for (s32 i = 0; i < ADXT_MAX; i++) {
        AdxtRec *r = REC(i);
        AdxtPlay *p = &sPlay[i];
        if (!r->used) {
            continue;
        }
        if (!p->annN) {
            p->annPos = p->nxtPos;
            p->annN = p->nxtN;
            p->annNch = p->nxtNch;
            p->nxtN = 0;
            continue;
        }
        for (s32 ch = 0; ch < (s32)p->annNch; ch++) {
            u32 *e = b + 4 + 4 * k++;
            e[0] = 0x100;
            e[1] = sIopSjObj[r->sj[ch]];
            e[2] = sIopRing + 0x40 + CRI_RING * (u32)r->sj[ch] + IOP_RING_DATA + p->annPos;
            e[3] = p->annN;
        }
        p->annPos = p->nxtPos;
        p->annN = p->nxtN;
        p->annNch = p->nxtNch;
        p->nxtN = 0;
    }
    b[0] = k;
}

/* ADXT's VSYNC server (io's VSYNC thread, each VBlank): a lock pair, then
 * under the lock each start's work, the outputs' commands and the two
 * blocks. */
static void VsyncServer(void *arg)
{
    (void)arg;
    LockedStep();
    Adx_Lock();
    for (s32 i = 0; i < ADXT_MAX; i++) {
        if (REC(i)->used && sPlay[i].on) {
            PlayStep(REC(i), &sPlay[i]);
        }
    }
    Block0();
    Commands();
    SendBlock(0);
    SendBlock(1);
    Adx_Unlock();
}

static ADXT Create(s32 maxnch, void *work, s32 worksize)
{
    AdxtRec *r = 0;
    s32 i, ch, n = 0;

    if (maxnch < 0 || worksize < 0 || !work) {
        Err("E1001: ADXT_Create: parameter error");
        return 0;
    }
    for (i = 0; i < ADXT_MAX; i++) {
        if (!REC(i)->used) {
            r = REC(i);
            break;
        }
    }
    if (!r) {
        Err("E1002: ADXT_Create: no player free");
        return 0;
    }
    __builtin_memset(r, 0, sizeof(*r));
    r->maxnch = (u8)maxnch;
    r->work = work;
    r->worksize = worksize;
    r->levelSent = Level(0);            /* 0 dB: no command until a change */
    r->monoSent = 0;                    /* stereo: no command until a change */
    LockedStep();                       /* the input SJ */
    r->stm = AdxStm_Create();           /* the stream: io's, one lock pair */
    for (ch = 0; ch < maxnch; ch++) {
        LockedStep();                   /* the channel's SJ */
    }
    /* The output: maxnch, 0, then each channel's IOP ring (the first free ones,
     * in ADXT_Init's order: the game's two players take rings 0-1 and 2-3). */
    __builtin_memset(sCriSend, 0, sizeof(sCriSend));
    sCriSend[0] = (u32)maxnch;
    for (i = 0; i < CRI_NSJ && n < maxnch && n < 2; i++) {
        if (!sSjUsed[i]) {
            sSjUsed[i] = 1;
            r->sj[n] = i;
            sCriSend[2 + n] = sIopSj[i];
            n++;
        }
    }
    r->iopOut = CriCall(0x408, 0x10, 4);
    for (ch = 0; ch < maxnch; ch++) {
        LockedStep();                   /* the channel's output joint */
    }
    LockedStep();                       /* the seamless object */
    Adx_Lock();                             /* the record: stopped, then in use */
    r->stat = ADXT_STAT_STOP;
    r->used = 1;
    sPlay[((u8 *)r - sTable) / ADXT_REC].iopFree = ADXT_OBUF;   /* the IOP's ring, by its answers */
    Adx_Unlock();
    return (ADXT)r;
}

/* ADXT_Create(maxnch, work, worksize): a player; 0 for a parameter refused or
 * no record free (the error function called). */
ADXT ADXT_Create(s32 maxnch, void *work, s32 worksize)
{
    return (ADXT)(Create(maxnch, work, worksize));
}

/* ADXT_GetStat(adxt): the record's state; NULL: the error function, then -1. */
s32 ADXT_GetStat(ADXT adxt)
{
    if (!adxt) {
        Err("E1003: ADXT_GetStat: NULL handle");
        return -1;
    }
    return ((AdxtRec *)adxt)->stat;
}

/* ADXT_IsReadyPlayStart(adxt): the ready flag; NULL: the error function, then -1. */
s32 ADXT_IsReadyPlayStart(ADXT adxt)
{
    if (!adxt) {
        Err("E1004: ADXT_IsReadyPlayStart: NULL handle");
        return -1;
    }
    return (s8)((AdxtRec *)adxt)->readyPlayStart;
}

/* ADXT_SetWaitPlayStart(adxt, flag): the flag's low byte; NULL: the error
 * function. */
void ADXT_SetWaitPlayStart(ADXT adxt, s32 flag)
{
    if (!adxt) {
        Err("E1005: ADXT_SetWaitPlayStart: NULL handle");
    } else {
        ((AdxtRec *)adxt)->waitPlayStart = (u8)flag;
    }
}

/* The stop's work (ADXT_Stop's, and ADXT_StartAfs's first): the stream's
 * plain stop (io's AdxStm_StopNow, the original's 0x164850: two lock pairs,
 * no server pass), then under a third lock (0x166350) the record stopped, the
 * output's stop flag set and play cleared. */
static void StopWork(AdxtRec *r)
{
    AdxtPlay *p = &sPlay[((u8 *)r - sTable) / ADXT_REC];
    AdxStm_StopNow(r->stm);
    Adx_Lock();
    r->stat = ADXT_STAT_STOP;
    r->stopReq = 1;
    p->on = 0;
    p->rdN = 0;
    p->playReq = 0;
    Adx_Unlock();
}

/* ADXT_Stop(adxt): the stream's stop (two lock pairs), then the record
 * stopped and its output's stop flag asked, under a third lock (the
 * original's three pairs: boot VBlanks 78 and 92). NULL: the error function. */
void ADXT_Stop(ADXT adxt)
{
    if (!adxt) {
        Err("E1007: ADXT_Stop: NULL handle");
    } else {
        StopWork((AdxtRec *)adxt);
    }
}

/* ADXT_Pause(adxt, sw): the switch kept (no kernel call). It holds the
 * output's play (command 2) off while set: a playing player's command 2 with
 * 0 at the next VBlank (area1's 18211); on a stopped player, no command.
 * NULL: the error function. */
void ADXT_Pause(ADXT adxt, s32 sw)
{
    if (!adxt) {
        Err("E1008: ADXT_Pause: NULL handle");
    } else {
        ((AdxtRec *)adxt)->pause = sw;
    }
}

/* ADXT_SetOutVol(adxt, vol): the volume in 0.1 dB, sent as the output's level
 * at the next VBlank when it changes (no kernel call). NULL: the error function. */
void ADXT_SetOutVol(ADXT adxt, s32 vol)
{
    if (!adxt) {
        Err("E1009: ADXT_SetOutVol: NULL handle");
    } else {
        ((AdxtRec *)adxt)->vol = vol;
    }
}

/* ADXT_SetOutputMono(flag): mono or stereo for every player (no kernel
 * call): each output's command 13 and a playing player's pans (command 9:
 * both 0 for mono) at the next VBlank (Commands). */
void ADXT_SetOutputMono(s32 flag)
{
    sMono = flag;
}

/* ADXT_StartAfs(adxt, ptid, flid): the start of a partition's file (io's
 * Adxf_AfsFile: its AFS file, first sector and sectors) on the player's
 * stream (io's AdxStm_Bind): first the stop's work (the output's stop flag
 * set, play cleared; three lock pairs, as ADXT_Stop's and the
 * original's), then the file bound with no
 * kernel call, the state 1 (DECINFO); the VSYNC server does the rest (the
 * starts' state, above). NULL, or a file the partition lacks: the error
 * function. */
void ADXT_StartAfs(ADXT adxt, s32 ptid, s32 flid)
{
    if (!adxt) {
        Err("E1011: ADXT_StartAfs: NULL handle");
    } else {
        AdxtRec *r = (AdxtRec *)adxt;
        AdxtPlay *p = &sPlay[((u8 *)r - sTable) / ADXT_REC];
        char name[0x100];
        s32 ofs, sct;
        StopWork(r);
        if (Adxf_AfsFile(ptid, flid, name, &ofs, &sct) != 0) {
            Err("E1012: ADXT_StartAfs: no such file");
        } else {
            u8 *base = (u8 *)(((u32)r->work + 63u) & ~63u);
            u32 ibytes;
            p->on = 1;
            p->nch = 0;
            p->header = 0;
            p->decodes = 0;
            p->h1[0] = p->h1[1] = p->h2[0] = p->h2[1] = 0;
            p->smpDone = p->smpTotal = 0;
            p->ended = 0;
            p->tailIn = p->markDue = 0;
            p->obuf[0] = base;
            p->obuf[1] = base + ADXT_OSTRIDE;
            p->ibuf = base + ADXT_OSTRIDE * r->maxnch;
            ibytes = (u32)((u8 *)r->work + r->worksize - p->ibuf);
            p->ringSct = ibytes / IO_SECTOR - 1;
            p->fileSct = (u32)sct;
            p->rdSct = p->rdN = p->inTop = p->inPos = 0;
            p->dec = p->sent = p->sentOld = 0;
            r->stat = ADXT_STAT_DECINFO;
            /* The AFS file's name, after the input ring's copied bytes (the
             * original's work area from the start on: "afs00.afs" at
             * 0x29EE24 on late-bulldrome, by the VBlank after the start). */
            {
                u8 *at = p->ibuf + p->ringSct * IO_SECTOR + ADXT_IXTRA, *top = (u8 *)r->work + r->worksize;
                for (u32 k = 0; k < sizeof(name) && at + k < top; k++) {
                    at[k] = (u8)name[k];
                    if (!name[k]) {
                        break;
                    }
                }
            }
            AdxStm_Bind(r->stm, name, ofs, sct);
        }
    }
}
