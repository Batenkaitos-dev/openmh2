/*
 * CRI ADX's common parts as Capcom's code calls them:
 * the threads' set-up func_00162220, the VBlank hook
 * func_00161F00, the flip wait's sleep func_00162188, the MAIN servers
 * func_00162040, SVM's lock func_00161EC0 / func_00161ED8 (sub_main's
 * func_161EC0 / func_161ED8), the callback registration func_00161750,
 * func_00162878, and the file systems' set-up func_001657B8, func_00165400,
 * func_00165500; under them SVM's servers and the stream layer over libcdvd
 * (pc/core/io/adx.h).
 *
 * Written from the original's behaviour at the interfaces
 * (the kernel calls of each, on
 * the boot route). What
 * the game can see of them is the kernel's objects and order (the threads'
 * ids and priorities, the sleeps and wakes, the lock's priority changes), the
 * CD's reads, ADXF's states and when they change; the libraries' own memory is
 * the core's.
 */
#include "common.h"
#include "sif.h"
#include "libkernl.h"
#include "libcdvd.h"
#include "kernel/kern.h"
#include "io/io.h"
#include "io/io_cdvd.h"
#include "io/io_kernel.h"
#include "io/adx.h"

/* ---- the kernel calls the SDK has and the game doesn't declare (the kernel's) ---- */
s32 ReferThreadStatus(s32 tid, struct ThreadParam *st);
s32 iReferThreadStatus(s32 tid, struct ThreadParam *st);
s32 SuspendThread(s32 tid);
s32 ResumeThread(s32 tid);
s32 WakeupThread(s32 tid);
s32 iWakeupThread(s32 tid);
s32 printf(const char *fmt, ...);

#define THS_WAIT 0x04
#define THS_SUSPEND 0x08
#define TSW_SLEEP 1
#define ADX_GP 0x001D05F0u

enum { T_LOCK, T_VSYNC, T_FS, T_IDLE, T_N };

static struct {
    s32 count;                 /* set-ups made (the threads exist when > 0) */
    s32 tid[T_N];
    s32 main;                  /* the thread that made the set-up */
    s32 mainSleeping;          /* func_00162188 put it to sleep */
    s32 lockPrio;              /* the lock holder's priority before the lock */
    s32 lockDepth;             /* the lock's nesting (0: free) */
    s32 vbusy;
    u32 vcount;
    u64 vcount64;
    s32 mode;                  /* the VBlank's word 0x19EB68 (1: wake modeTid too); never set on the routes */
    s32 modeTid;
    void (*userCb)(void);      /* the VBlank's user callback (never set on the routes) */
    void (*idleCb)(void);      /* func_00161750's */
    s32 idleArg;
    void (*sleepBefore)(void); /* Adx_SetSleepHooks' (the original's 0x179530) */
    void (*sleepAfter)(void);  /* and 0x179560's */
} g_adx;

/* ------------------------------------------------------------ SVM's servers */

#define ADX_SVRS 8
static struct {
    u32 id;
    AdxSvrFn fn;
    void *arg;
} g_svr[ADX_SVR_TYPES][ADX_SVRS];
static u32 g_svrNext = 1;

u32 Adx_SvrAdd(int type, AdxSvrFn fn, void *arg)
{
    if (type < 0 || type >= ADX_SVR_TYPES)
        return 0;
    for (int i = 0; i < ADX_SVRS; i++) {
        if (!g_svr[type][i].id) {
            g_svr[type][i].id = g_svrNext++;
            g_svr[type][i].fn = fn;
            g_svr[type][i].arg = arg;
            return g_svr[type][i].id;
        }
    }
    return 0;
}

void Adx_SvrRemove(u32 id)
{
    for (int t = 0; t < ADX_SVR_TYPES; t++)
        for (int i = 0; i < ADX_SVRS; i++)
            if (g_svr[t][i].id == id)
                g_svr[t][i].id = 0;
}

static void SvrExec(int type)
{
    for (int i = 0; i < ADX_SVRS; i++)
        if (g_svr[type][i].id)
            g_svr[type][i].fn(g_svr[type][i].arg);
}

/* ------------------------------------------------------------ threads */

static int Status(s32 tid, struct ThreadParam *st)
{
    __builtin_memset(st, 0, sizeof(*st));
    return ReferThreadStatus(tid, st);
}

/* Suspended unless it is already (the original's: the status asked first). */
static void SuspendIfNot(s32 tid)
{
    struct ThreadParam st;
    Status(tid, &st);
    if (!(st.status & THS_SUSPEND))
        SuspendThread(tid);
}

static void ResumeIfSuspended(s32 tid)
{
    struct ThreadParam st;
    Status(tid, &st);
    if (st.status & THS_SUSPEND)
        ResumeThread(tid);
}

static void WakeIfSleeping(s32 tid)
{
    struct ThreadParam st;
    Status(tid, &st);
    if ((st.status & THS_WAIT) && st.waitType == TSW_SLEEP)
        WakeupThread(tid);
}

static void IWakeIfSleeping(s32 tid)
{
    struct ThreadParam st;
    __builtin_memset(&st, 0, sizeof(st));
    iReferThreadStatus(tid, &st);
    if ((st.status & THS_WAIT) && st.waitType == TSW_SLEEP)
        iWakeupThread(tid);
}

/* SVM's lock nests (the original's: an inner lock and its unlock make no
 * call); only the outermost changes the holder's priority and the lock
 * thread's state. */
void Adx_Lock(void)
{
    if (g_adx.count <= 0)
        return;
    if (g_adx.lockDepth++ > 0)
        return;
    s32 tid = GetThreadId();
    g_adx.lockPrio = ChangeThreadPriority(tid, 1);
    ResumeIfSuspended(g_adx.tid[T_LOCK]);
}

void Adx_Unlock(void)
{
    if (g_adx.count <= 0 || g_adx.lockDepth <= 0)
        return;
    if (--g_adx.lockDepth > 0)
        return;
    SuspendIfNot(g_adx.tid[T_LOCK]);
    ChangeThreadPriority(GetThreadId(), g_adx.lockPrio);
}

/* The lock thread: it only keeps the processor while a lock holder waits (a
 * library wait: the polled-wait table's func_00160C20). */
static void LockThread(void *arg)
{
    (void)arg;
    Kern_LibWaitArrive();
    for (;;)
        Kern_LibWaitStep();
}

static void VsyncThread(void *arg)
{
    (void)arg;
    for (;;) {
        SuspendIfNot(g_adx.tid[T_IDLE]);
        if (g_adx.mainSleeping) {
            g_adx.mainSleeping = 0;
            WakeIfSleeping(g_adx.main);
        }
        SvrExec(ADX_SVR_VSYNC);
        SleepThread();
    }
}

static void FsThread(void *arg)
{
    (void)arg;
    for (;;) {
        SvrExec(ADX_SVR_FS);
        SleepThread();
    }
}

static void IdleThread(void *arg)
{
    (void)arg;
    for (;;) {
        SvrExec(ADX_SVR_IDLE);
        SleepThread();
    }
}

static u8 g_stackLock[0x800] __attribute__((aligned(16)));
static u8 g_stackVsync[0x1000] __attribute__((aligned(16)));
static u8 g_stackFs[0x1000] __attribute__((aligned(16)));
static u8 g_stackIdle[0x2000] __attribute__((aligned(16)));

static s32 MakeThread(void (*entry)(void *), void *stack, s32 size)
{
    struct ThreadParam p;
    __builtin_memset(&p, 0, sizeof(p));
    p.entry = (void *)entry;
    p.stack = stack;
    p.stackSize = size;
    p.gpReg = (void *)ADX_GP;
    p.initPriority = 2;
    s32 tid = CreateThread(&p);
    StartThread(tid, 0);
    return tid;
}


/* The threads' set-up (ADXPS2's): the original's order of kernel calls. A
 * param (four words; the game passes NULL) is not looked at here: its
 * priorities were never seen. */
void func_00162220(s32 param);
static void func_00162220Work(s32 param)
{
    struct ThreadParam st;
    (void)param;
    if (g_adx.count++ > 0)
        return;
    g_adx.main = GetThreadId();
    Status(g_adx.main, &st);
    g_adx.tid[T_LOCK] = MakeThread(LockThread, g_stackLock, sizeof(g_stackLock));
    SuspendIfNot(g_adx.tid[T_LOCK]);
    ChangeThreadPriority(g_adx.tid[T_LOCK], 8);
    g_adx.tid[T_VSYNC] = MakeThread(VsyncThread, g_stackVsync, sizeof(g_stackVsync));
    ChangeThreadPriority(g_adx.tid[T_VSYNC], 0x10);
    g_adx.tid[T_FS] = MakeThread(FsThread, g_stackFs, sizeof(g_stackFs));
    ChangeThreadPriority(g_adx.tid[T_FS], 0x12);
    g_adx.tid[T_IDLE] = MakeThread(IdleThread, g_stackIdle, sizeof(g_stackIdle));
    ChangeThreadPriority(g_adx.tid[T_IDLE], 0x19);
    SuspendIfNot(g_adx.tid[T_IDLE]);
    ChangeThreadPriority(g_adx.main, 0x18);
    /* No server yet: until ADXT_Init (Adx_FsServerStart) the FS thread's
     * wakes run none, as the original's. */
}

/* ADX's FS server joins SVM, under the lock (the original's ADXT_Init:
 * 0x1602F8 at the FS thread's type; snd's ADXT_Init calls this). Once. */
void Adx_FsServerStart(void)
{
    static u32 id;
    Adx_Lock();
    if (!id)
        id = Adx_SvrAdd(ADX_SVR_FS, Adx_FsServer, 0);
    Adx_Unlock();
}

/* The VBlank hook (Capcom's Display_AdxVint, in the VBlank's handler). */
void func_00161F00(s32 arg);
static void func_00161F00Work(s32 arg)
{
    (void)arg;
    g_adx.vcount++;
    if (!g_adx.vbusy) {
        g_adx.vbusy = 1;
        if (g_adx.count > 0) {
            IWakeIfSleeping(g_adx.tid[T_VSYNC]);
            IWakeIfSleeping(g_adx.tid[T_FS]);
            if (g_adx.mode == 1)
                IWakeIfSleeping(g_adx.modeTid);
            g_adx.vcount64++;
        }
        SvrExec(ADX_SVR_VINT);
        g_adx.vbusy = 0;
    }
    if (g_adx.userCb)
        g_adx.userCb();
}

/* The flip wait's call (Sys_WaitFlip's loop): the IDLE thread resumed and
 * woken, then the caller sleeps until the VSYNC thread wakes it at the next
 * VBlank (the wait: SleepThread). The sleep hooks (Adx_SetSleepHooks) run
 * around it: before() first, after() when SleepThread returns (the
 * original's 0x179530 and 0x179560: Sofdec's scratchpad swap). */
void func_00162188(void);
static void func_00162188Work(void)
{
    if (g_adx.count <= 0)
        return;
    if (g_adx.sleepBefore)
        g_adx.sleepBefore();
    ResumeIfSuspended(g_adx.tid[T_IDLE]);
    WakeIfSleeping(g_adx.tid[T_IDLE]);
    g_adx.mainSleeping = 1;
    SleepThread();
    if (g_adx.sleepAfter)
        g_adx.sleepAfter();
}

/* The sleep hooks: plain stores, no lock (the original's mwPlyInitSfdFx
 * registers its hooks last, outside SVM's lock: 0x1326F0). */
void Adx_SetSleepHooks(void (*before)(void), void (*after)(void))
{
    g_adx.sleepBefore = before;
    g_adx.sleepAfter = after;
}

/* The MAIN servers (none are registered on the routes: no call is made). */
s32 func_00162040(void);
static s32 func_00162040Work(void)
{
    SvrExec(ADX_SVR_MAIN);
    return 0;
}

void func_00161EC0(void);
static void func_00161EC0Work(void)
{
    Adx_Lock();
}

void func_00161ED8(void);
static void func_00161ED8Work(void)
{
    Adx_Unlock();
}

void func_161EC0(void);
static void func_161EC0Work(void)
{
    Adx_Lock();
}

void func_161ED8(void);
static void func_161ED8Work(void)
{
    Adx_Unlock();
}

/* SVM's callback registration (Movie_InitSfd's empty function): kept, never
 * called on the routes. */
void func_00161750(void (*func)(void), s32 arg);
static void func_00161750Work(void (*func)(void), s32 arg)
{
    g_adx.idleCb = func;
    g_adx.idleArg = arg;
}

/* From Capcom's IOP start-up (module 10, (2, 2)): the seamless layer's set-up,
 * which keeps no state the game reads. */
void func_00162878(s32 a, s32 b);
static void func_00162878Work(s32 a, s32 b)
{
    (void)a;
    (void)b;
}

/* ------------------------------------------------------------ the file systems */

typedef struct AdxDvdFsParam {
    const char *root;
    s32 w[4];
} AdxDvdFsParam;
typedef struct AdxCacheParam {   /* the game's (src/main/main.c) */
    const char *list;
    s32 nfile;
    void *buf;
    s32 nsct;
} AdxCacheParam;


/* The host file system: nothing the game can see offline. */
void func_001657B8(s32 param);
static void func_001657B8Work(s32 param)
{
    (void)param;
}

/* The CD/DVD file system: the root (the game's "\\"). */
void func_00165400(AdxDvdFsParam *param);
static void func_00165400Work(AdxDvdFsParam *param)
{
    (void)param;
}

/*
 * The cache of the file list (the original's func_00165500, observed with
 * --lib-tree and its RAM writes on the boot route): the game's work area
 * (param->buf, param->nsct bytes) is cleared and holds up to nsct / 0x29
 * files: first a (first sector, size) pair for each (8 bytes), then each
 * name upper case in 0x21 bytes. The list itself is read into the library's
 * own buffer of two sectors (cleared first). Before the list's search the
 * library waits until no N-command is in flight, and after its read it
 * waits for the read's end: both are library waits (the polled-wait table, the two loops
 * of func_0016F0D8), so the arrival is a delivery point and the read's end
 * (the SIF's) comes before the first check, as in the original game. Each name's
 * search asks sceCdSync(1) once first (a query, not a wait).
 */
#define ADX_CACHE_PAIR 8u
#define ADX_CACHE_NAME 0x21u
static u8 *g_cacheWork;    /* the game's work area */
static s32 g_ncache, g_cacheCap;
static u8 g_cacheList[2 * IO_SECTOR] __attribute__((aligned(64)));   /* the library's buffer */

static int IsSpace(char c)
{
    return c == '\r' || c == '\n' || c == ' ' || c == '\t' || c == 0;
}

static void CdWaitIdle(void)
{
    Kern_LibWaitArrive();
    while (sceCdSync(1, 0))
        Kern_LibWaitStep();
}

void func_00165500(AdxCacheParam *param);
static void func_00165500Work(AdxCacheParam *param)
{
    static const u8 mode[4] = {0, 0, 0, 0};
    CdFile f;
    char path[64];
    g_ncache = 0;
    g_cacheCap = 0;
    g_cacheWork = 0;
    if (!param || !param->list || !param->buf)
        return;
    __builtin_memset(g_cacheList, 0, sizeof(g_cacheList));
    __builtin_memset(param->buf, 0, param->nsct > 0 ? (u32)param->nsct : 0);
    g_cacheWork = param->buf;
    g_cacheCap = param->nsct > 0 ? (s32)((u32)param->nsct / (ADX_CACHE_PAIR + ADX_CACHE_NAME)) : 0;
    u32 n = 0;
    path[n++] = '\\';
    for (const char *s = param->list; *s && n < sizeof(path) - 3; s++)
        path[n++] = (*s >= 'a' && *s <= 'z') ? (char)(*s - 32) : *s;
    path[n++] = ';';
    path[n++] = '1';
    path[n] = 0;
    CdWaitIdle();
    if (IoCd_Search(&f, path, 0) != 1)
        return;
    u32 nsct = ((f.size + 0x7FFu) >> 11) + 1;
    if (nsct > sizeof(g_cacheList) / IO_SECTOR) {
        Io_Log("adx: the file list's %u sectors cut to the buffer's", nsct, 0);
        nsct = sizeof(g_cacheList) / IO_SECTOR;
    }
    if (IoCd_Read(f.lsn, nsct, g_cacheList, mode) != 1)
        return;
    CdWaitIdle();
    const char *t = (const char *)g_cacheList;
    u32 size = f.size;   /* the list's: f is each name's search from here on */
    if (size > sizeof(g_cacheList))
        size = sizeof(g_cacheList);
    u32 pos = 0;
    while (pos < size && g_ncache < param->nfile && g_ncache < g_cacheCap) {
        while (pos < size && IsSpace(t[pos]))
            pos++;
        if (pos >= size)
            break;
        char name[ADX_CACHE_NAME];
        u32 k = 0;
        while (pos < size && !IsSpace(t[pos]) && k < sizeof(name) - 1) {
            char c = t[pos++];
            name[k++] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
        }
        name[k] = 0;
        n = 0;
        path[n++] = '\\';
        for (u32 i = 0; i < k; i++)
            path[n++] = name[i];
        path[n++] = ';';
        path[n++] = '1';
        path[n] = 0;
        sceCdSync(1, 0);
        if (IoCd_Search(&f, path, 0) == 1) {
            u32 *pair = (u32 *)(g_cacheWork + ADX_CACHE_PAIR * (u32)g_ncache);
            pair[0] = f.lsn;
            pair[1] = f.size;
            __builtin_memcpy(g_cacheWork + ADX_CACHE_PAIR * (u32)g_cacheCap + ADX_CACHE_NAME * (u32)g_ncache,
                             name, k + 1);
            g_ncache++;
            printf("DVCI: \"%s\" found.\n", name);
        }
    }
    printf("DVCI: Total %d files.\n", g_ncache);
}

/* A name of the cache (the game's names, with or without a leading '\\'),
 * read from the game's work area. */
static int CacheFind(const char *name, u32 *lsn, u32 *size)
{
    if (*name == '\\' || *name == '/')
        name++;
    for (s32 i = 0; g_cacheWork && i < g_ncache; i++) {
        const char *a = (const char *)g_cacheWork + ADX_CACHE_PAIR * (u32)g_cacheCap + ADX_CACHE_NAME * (u32)i;
        const char *b = name;
        while (*a && (*a == *b || (*b >= 'a' && *b <= 'z' && *a == *b - 32))) {
            a++;
            b++;
        }
        if (!*a && (!*b || *b == ';')) {
            const u32 *pair = (const u32 *)(g_cacheWork + ADX_CACHE_PAIR * (u32)i);
            *lsn = pair[0];
            *size = pair[1];
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------ the stream layer
 *
 * As the original's was seen on the boot route:
 * every stream (ADXT's from their
 * create, ADXF's from its open) is visited by ADX's server pass, and the pass
 * and the stream calls take SVM's lock in a fixed pattern. A pass:
 *   its own lock pair; the device's exec (one pair); the device's server (two
 *   pairs); the streams (one pair, then one per stream); ADXF's server (one
 *   pair); the streams again; the device's server; the device's exec.
 * The CD device takes one request at a time: a stream's request is started in
 * the streams' part (four pairs), accepted by the device's server (one pair),
 * issued by the device's exec (sceCdRead, then the check: sceCdSync(1) and
 * the last error asked, at each exec while it reads), released by the server
 * (one pair) once a check saw it ended, and taken by its stream (two pairs,
 * or one and the next chunk's start).
 * A stop while the device reads the stream's chunk (stop-read, VBlank 2895:
 * seen in the original) neither waits for the
 * read nor drops it: the device's exec goes on checking it, and when it has
 * ended the streams' part frees the device and leaves the stream as it is
 * (no pairs, no next chunk).
 */

#define ADX_STMS 16
#define ADX_CHUNK 512u

struct AdxStm {
    s32 used;
    s32 state;
    s32 file;        /* a file is bound (opened by a pass) */
    s32 opening;     /* the file's open is the next pass's */
    char name[64];
    s32 ofs, want;   /* the open's sector offset, and its sectors (< 0: the file's) */
    s32 known;       /* the file is a pack's, whose first sector and size are known: no cache, no search */
    u32 klsn, ksize;
    u32 lsn;         /* the file's first sector (with the AFS offset) */
    u32 size;        /* bytes */
    u32 nsct;        /* sectors */
    u32 pos, req, done;
    u8 *buf;
    u32 inflight;    /* sectors of the device's request, 0 for none */
    s32 queued;      /* a request for the next streams' part (2: the next chunk of the request being read) */
    u32 (*resize)(void *arg);   /* the request's size when the device takes it, or NULL */
    void *resizeArg;
};

static AdxStm g_stm[ADX_STMS];

enum { DEV_IDLE, DEV_QUEUED, DEV_ACCEPTED, DEV_READING, DEV_DONE, DEV_ENDED };
static struct {
    AdxStm *s;       /* the request's stream */
    s32 state;
    s32 err;
    s32 stopped;     /* the stream was stopped while the device read */
} g_dev;

static void LockPair(void)
{
    Adx_Lock();
    Adx_Unlock();
}

/* A stream's slot: ADXT's from the first (the original's 0x1A6F58, 0x1A6FB8),
 * ADXF's from the last (0x1A7558, the sixteenth of 0x60 bytes). */
static AdxStm *NewStm(int fromTop)
{
    for (int k = 0; k < ADX_STMS; k++) {
        AdxStm *s = &g_stm[fromTop ? ADX_STMS - 1 - k : k];
        if (s->used)
            continue;
        __builtin_memset(s, 0, sizeof(*s));
        s->used = 1;
        s->state = ADX_STM_STOP;
        return s;
    }
    return 0;
}

AdxStm *AdxStm_Create(void)
{
    LockPair();
    return NewStm(0);
}

/* A file bound to a stream AdxStm_Create made, for ADXT_StartAfs (the
 * original's 0x163FE8 under 0x1678F0, village VBlank 2742: no kernel call,
 * no lock, no pass). The name, the sector offset and the sectors are stored;
 * the file opens at the next pass's streams' part (OpenFile: the cache, no
 * call for a cached name), before a request queued meanwhile starts. The
 * stream must be stopped (ADXT_StartAfs stops it first). */
void AdxStm_Bind(AdxStm *s, const char *name, s32 ofs, s32 nsct)
{
    if (!s || !name)
        return;
    u32 n = 0;
    while (name[n] && n < sizeof(s->name) - 1) {
        s->name[n] = name[n];
        n++;
    }
    s->name[n] = 0;
    s->file = 0;
    s->ofs = ofs;
    s->want = nsct;
    s->lsn = 0;
    s->size = nsct < 0 ? 0 : (u32)nsct << 11;
    s->nsct = nsct < 0 ? 0 : (u32)nsct;
    s->pos = s->req = s->done = 0;
    s->buf = 0;
    s->queued = 0;
    s->state = ADX_STM_STOP;
    s->opening = 1;
}

/* The open of a stream's file, in a pass (the cache first, else a search). */
static void OpenFile(AdxStm *s)
{
    u32 lsn, size;
    s->opening = 0;
    if (s->known) {   /* a pack's file: the open the cache would have answered, with the pack's place */
        lsn = s->klsn;
        size = s->ksize;
    } else if (!CacheFind(s->name, &lsn, &size)) {
        CdFile f;
        char path[64];
        u32 n = 0;
        if (s->name[0] != '\\')
            path[n++] = '\\';
        for (const char *c = s->name; *c && *c != ';' && n < sizeof(path) - 3; c++)
            path[n++] = (*c >= 'a' && *c <= 'z') ? (char)(*c - 32) : *c;
        path[n++] = ';';
        path[n++] = '1';
        path[n] = 0;
        if (IoCd_Search(&f, path, 0) != 1) {
            s->state = ADX_STM_ERR;
            return;
        }
        lsn = f.lsn;
        size = f.size;
    }
    s->file = 1;
    s->lsn = lsn + (u32)s->ofs;
    if (s->want < 0) {
        s->size = size;
        s->nsct = (size + 0x7FFu) >> 11;
    } else {
        s->nsct = (u32)s->want;
        s->size = (u32)s->want << 11;
    }
}

/* A request's (or the next chunk's) start: the device's request queued. */
static void Start(AdxStm *s)
{
    LockPair();
    LockPair();
    LockPair();
    LockPair();
    u32 left = s->req - s->done;
    u32 n = left < ADX_CHUNK ? left : ADX_CHUNK;
    if (s->pos + s->done + n > s->nsct) {   /* past the file's end */
        s->state = ADX_STM_ERR;
        return;
    }
    s->inflight = n;
    g_dev.s = s;
    g_dev.state = DEV_QUEUED;
    g_dev.err = 0;
    g_dev.stopped = 0;
}

/* The device's check of its read: sceCdSync(1)'s answer taken, then the
 * last error asked (an RPC that waits), then the read ended if it was not
 * busy before the question. The original's: at the issue (the original game with
 * --frame-clock: busy; its next pass: ended, one more question) and, with
 * --deterministic, ended at the issue (no question at the next pass). */
static void DevCheck(void)
{
    s32 busy = IoCd_Busy();
    s32 e = IoCd_GetError();
    if (e > 0)
        g_dev.err = e;
    if (!busy)
        g_dev.state = DEV_DONE;
}

static void DevExec(void)
{
    static const u8 mode[4] = {0, 0, 0, 0};
    LockPair();
    if (g_dev.state == DEV_ACCEPTED) {
        AdxStm *s = g_dev.s;
        if (IoCd_Read(s->lsn + s->pos + s->done, s->inflight, s->buf + (s->done << 11), mode) != 1) {
            g_dev.err = -1;
            g_dev.state = DEV_DONE;
            return;
        }
        g_dev.state = DEV_READING;
        DevCheck();
    } else if (g_dev.state == DEV_READING) {
        DevCheck();
    }
}

static void DevServer(void)
{
    LockPair();
    LockPair();
    if (g_dev.state == DEV_QUEUED) {
        LockPair();
        g_dev.state = DEV_ACCEPTED;
    } else if (g_dev.state == DEV_DONE) {
        LockPair();
        g_dev.state = DEV_ENDED;
    }
}

/* second: the pass's second visit. A read the stream was stopped during is freed there, not at the first visit:
 * a request queued meanwhile (a restart in the same VBlank: late2-khezu 30491, StartAfs over a read in
 * flight) starts at the next pass, the original's R a VBlank after a plain start's. */
static void Streams(int second)
{
    LockPair();
    for (int i = 0; i < ADX_STMS; i++) {
        AdxStm *s = &g_stm[i];
        if (!s->used)
            continue;
        LockPair();
        if (s->opening)
            OpenFile(s);
        if (g_dev.s == s && g_dev.state == DEV_ENDED && (second || !g_dev.stopped)) {
            s32 err = g_dev.err;
            s32 stopped = g_dev.stopped;
            g_dev.s = 0;
            g_dev.state = DEV_IDLE;
            g_dev.stopped = 0;
            if (stopped) {   /* stopped while it read (stop-read, VBlank 2896) */
                s->inflight = 0;
                continue;
            }
            if (err) {
                LockPair();
                LockPair();
                s->inflight = 0;
                s->state = ADX_STM_ERR;
                continue;
            }
            s->done += s->inflight;
            s->inflight = 0;
            if (s->done >= s->req) {
                LockPair();
                LockPair();
                s->state = ADX_STM_END;
            } else {
                LockPair();
                /* The next chunk of a file read waits its turn, as a new request does, and the streams'
                 * part takes the stream in the lowest slot first (a lone read starts at this pass's
                 * second visit, as before). Starting it here took the device before a stream already
                 * waiting for it: on late-cephadrome, late-gypceros (VBlank 7595) the original game reads the
                 * BGM stream's 12 sectors, then the last chunk (140 sectors of a 1,164-sector file) at
                 * 7596; the core read them the other way round and ran a frame ahead. queued 2: the
                 * same request, its size kept. */
                s->queued = 2;
            }
        } else if (s->queued && g_dev.state == DEV_IDLE && s->state == ADX_STM_READ) {
            s32 chunk = s->queued == 2;
            s->queued = 0;
            if (!chunk && s->resize && s->req) {
                s->req = s->resize(s->resizeArg);
            }
            if (s->req == 0)
                s->state = ADX_STM_END;
            else
                Start(s);
        }
    }
}

/* ADX's server pass (the original's 0x160140): on the FS thread at each
 * wake, and inside the stream calls on the caller's thread. */
void Adx_FsServer(void *arg)
{
    (void)arg;
    LockPair();
    DevExec();
    DevServer();
    Streams(0);
    Adxf_Server();
    Streams(1);
    DevServer();
    DevExec();
}

/* ADXF's open (the original's 0x164818): the open asked under the lock, then
 * the wait (the polled-wait table's head 0x164830) with a pass per going-round. */
static AdxStm *OpenCommon(const char *name, s32 ofs, s32 nsct, s32 known, u32 klsn, u32 ksize)
{
    if (!name)
        return 0;
    LockPair();   /* the create */
    AdxStm *s = NewStm(1);
    if (!s)
        return 0;
    Adx_Lock();
    u32 n = 0;
    while (name[n] && n < sizeof(s->name) - 1) {
        s->name[n] = name[n];
        n++;
    }
    s->name[n] = 0;
    s->ofs = ofs;
    s->want = nsct;
    s->known = known;
    s->klsn = klsn;
    s->ksize = ksize;
    s->opening = 1;
    Adx_Unlock();
    Kern_LibWaitArrive();
    Adx_FsServer(0);
    while (s->opening) {
        Kern_LibWaitStep();
        Adx_FsServer(0);
    }
    if (s->state == ADX_STM_ERR) {
        __builtin_memset(s, 0, sizeof(*s));
        return 0;
    }
    return s;
}

AdxStm *AdxStm_Open(const char *name, s32 ofs, s32 nsct)
{
    return OpenCommon(name, ofs, nsct, 0, 0, 0);
}

AdxStm *AdxStm_OpenKnown(const char *name, u32 lba, u32 size, s32 ofs, s32 nsct)
{
    return OpenCommon(name, ofs, nsct, 1, lba, size);
}

/* The device's request of a stream dropped, a read in progress waited for.
 * Not seen on the routes (a stop during a
 * read"): a request queued or accepted but not issued at the stop, and a
 * stream closed while the device still holds its read. */
static void DropDrive(AdxStm *s)
{
    if (g_dev.state == DEV_READING)
        sceCdSync(0, 0);
    g_dev.s = 0;
    g_dev.state = DEV_IDLE;
    g_dev.stopped = 0;
    s->inflight = 0;
}

/* The device at a stream's stop. A read it has issued is left to it
 * (stop-read, VBlank 2895: no sceCdSync(0); at 2896 the device's exec asks
 * sceCdSync(1) and the error once more): the stop marks it, and its end is
 * the streams' part's, with nothing for the stream. A request not yet issued
 * is dropped. */
static void StopDrive(AdxStm *s)
{
    if (g_dev.s != s)
        return;
    if (g_dev.state == DEV_READING || g_dev.state == DEV_DONE || g_dev.state == DEV_ENDED) {
        g_dev.stopped = 1;
        return;
    }
    DropDrive(s);
}

/* The stop's first step (the original's 0x164A50): a pair, then a pass. */
static void StopAsk(void)
{
    LockPair();
    Adx_FsServer(0);
}

/* The stream's plain stop (the original's 0x164850, ADXT_Stop's: no pass):
 * an ask (a pair), then the stop under a second pair. */
static void StopNow(AdxStm *s)
{
    LockPair();
    Adx_Lock();
    StopDrive(s);
    s->queued = 0;
    s->state = ADX_STM_STOP;
    Adx_Unlock();
}

/* The stop (0x164890): its ask and a pass, the plain stop, and a pass when it
 * unbinds the stream's file (the close's first stop). */
static void Stop(AdxStm *s, int unbind)
{
    StopAsk();
    StopNow(s);
    if (unbind && s->file) {
        s->file = 0;
        Adx_FsServer(0);
    }
}

/* The close: the stop with the file's unbind, then the destroy (0x164748: an
 * ask, then a stop again). 71 lock pairs with three streams. */
void AdxStm_Close(AdxStm *s)
{
    if (!s)
        return;
    Stop(s, 1);
    StopAsk();
    Stop(s, 0);
    if (g_dev.s == s)
        DropDrive(s);   /* the device still holds its read: not seen */
    __builtin_memset(s, 0, sizeof(*s));
    s->state = ADX_STM_FREE;
}

s32 AdxStm_State(const AdxStm *s)
{
    return s ? s->state : ADX_STM_FREE;
}

u32 AdxStm_SizeBytes(const AdxStm *s)
{
    return s ? s->size : 0;
}

u32 AdxStm_SizeSectors(const AdxStm *s)
{
    return s ? s->nsct : 0;
}

u32 AdxStm_Done(const AdxStm *s)
{
    return s ? s->done : 0;
}

/* A request, for the next streams' part (no kernel call of its own). */
void AdxStm_Read(AdxStm *s, u32 pos, u32 nsct, void *buf)
{
    s->pos = pos;
    s->req = nsct;
    s->done = 0;
    s->buf = buf;
    s->queued = 1;
    s->resize = 0;
    s->state = ADX_STM_READ;
}

/* AdxStm_Read, its size taken again when the device takes the request
 * (resize's answer, in sectors): the original's stream layer sizes a read from
 * the ring's free bytes at its start, a VBlank later than the ask when another
 * stream's read held the device (drink's file 0x25 at 10587: 13 sectors asked
 * with 12.46 free, 13.02 at the start). */
void AdxStm_ReadSized(AdxStm *s, u32 pos, u32 nsct, void *buf, u32 (*resize)(void *arg), void *arg)
{
    AdxStm_Read(s, pos, nsct, buf);
    s->resize = resize;
    s->resizeArg = arg;
}

void AdxStm_Stop(AdxStm *s)
{
    if (!s)
        return;
    Stop(s, 0);
}

void AdxStm_StopNow(AdxStm *s)
{
    if (!s)
        return;
    StopNow(s);
}

/* ------------------------------------------------------------ the taps
 * verify's trace tap for each function the game calls. */

void func_00162220(s32 param)
{
    func_00162220Work(param);
}

void func_00161F00(s32 arg)
{
    func_00161F00Work(arg);
}

void func_00162188(void)
{
    func_00162188Work();
}

/* Traced as void (the seam table): no caller reads its result. */
s32 func_00162040(void)
{
    s32 r = func_00162040Work();
    return r;
}

void func_00161EC0(void)
{
    func_00161EC0Work();
}

void func_00161ED8(void)
{
    func_00161ED8Work();
}

void func_161EC0(void)
{
    func_161EC0Work();
}

void func_161ED8(void)
{
    func_161ED8Work();
}

void func_00162878(s32 a, s32 b)
{
    func_00162878Work(a, b);
}

void func_001657B8(s32 param)
{
    func_001657B8Work(param);
}

void func_00165400(AdxDvdFsParam *param)
{
    func_00165400Work(param);
}

void func_00165500(AdxCacheParam *param)
{
    func_00165500Work(param);
}

void func_00161750(void (*func)(void), s32 arg)
{
    func_00161750Work(func, arg);
}
