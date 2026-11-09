/* The network configuration interface library (netcnfif, Sony's, in the yn
 * overlay), offline.
 *
 * Written from the library's observed behaviour only, never from its code:
 * (the calls each function makes, its syscalls and the memory it writes).
 *
 * The library's state stays where the original game has it, in the yn overlay's
 * own data (the overlay's window, DataC_TryAt): the decode flag (0xAD1190),
 * the binding's flag (0xAD1194), the pending request's semaphore (0xAD1198,
 * -1 when none; the overlay's image starts it at -1), the RPC's client data
 * (0xAD3A80) and the request's record, which is also its answer's buffer
 * (0xAD3AC0, 0x1000 bytes). Every request is an RPC to the IOP's netcnfif
 * server (0x80001101), sent without waiting; its end function signals the
 * request's semaphore from the SIF's interrupt. The IOP's server answers a
 * record of zeros (the original's IOP stub, and the
 * core's SIF for a known service with no server: kernel/sif.c), so a work
 * area's request succeeds (result 0) and nothing reaches a network.
 *
 * Seen on the yn-errors route (the yn settings list's first row): the bind
 * (func_00AC8128 and yn_entry, the bind answered at the wait's first check),
 * AllocWorkarea (fno 0x65) with Check and GetResult, SetFNoDecode, then at the
 * quit FreeWorkarea and the end (fno 0x66 twice). GetCount, GetList,
 * LoadEntry, ConvAuthname and DataInit need settings on a card (the set-up
 * guide's), which no route has. */
#include "common.h"
#include "io/io_kernel.h"
#include "data_c.h"
#include "yn.h"

#define NC_NODECODE 0x00AD1190u   /* the flag sceNetcnfifSetFNoDecode sets */
#define NC_BOUND 0x00AD1194u      /* nonzero once the RPC is bound */
#define NC_SEMA 0x00AD1198u       /* the pending request's semaphore, -1 when none */
#define NC_PREFIX 0x00AD22A0u     /* the 7 bytes ConvAuthname looks for (the library's data, read at run time) */
#define NC_CD 0x00AD3A80u         /* the RPC's client data */
#define NC_BUF 0x00AD3AC0u        /* the request and its answer */
#define NC_BUF_SIZE 0x1000
#define NC_RECORD 0x310u          /* the record's part a request clears and GetResult copies */
#define NC_SERVER 0x80001101u     /* the IOP's netcnfif server */

/* The function numbers of the requests. */
enum { NC_GET_COUNT = 0, NC_GET_LIST = 1, NC_LOAD_ENTRY = 2, NC_ALLOC_WORKAREA = 0x65, NC_FREE_WORKAREA = 0x66 };

s32 yn_entry(void);
s32 func_00AC8128(void);
void func_00AC8DC0(void);
void func_00AC94C8(void *param);
s32 sceNetcnfifLoadEntry(char *path, s32 type, char *usrName, NetcnfData *data);
void sceNetcnfifGetList(char *path, s32 type, NetcnfEntry *list, s32 count);
void sceNetcnfifDataInit(NetcnfData *data);

static s32 *Word(u32 ps2)
{
    return (s32 *)DataC_TryAt(ps2);
}

static u8 *Bytes(u32 ps2)
{
    return (u8 *)DataC_TryAt(ps2);
}

static SifClientData *Client(void)
{
    return (SifClientData *)DataC_TryAt(NC_CD);
}

/* The binding (every request makes it first): 0 once bound. Else the bind,
 * sent without waiting (-1 if that fails), then its answer waited for
 * (the polled-wait table's wait record for yn_entry, head 0x00AC80F0); while the answer
 * names no server, a busy-wait of 65,537 going-rounds (head 0x00AC80B0) and
 * the bind again (not seen: the core's SIF names the server, as the
 * original's IOP does). Then the binding is noted; 0. */
static s32 Bind(void)
{
    SifClientData *cd = Client();
    if (*Word(NC_BOUND))
        return 0;
    for (;;) {
        if (sceSifBindRpc(cd, NC_SERVER, 1) < 0)
            return -1;
        Kern_LibWaitArrive();
        while (sceSifCheckStatRpc(cd))
            Kern_LibWaitStep();
        if (cd->serve)
            break;
        Kern_LibWaitArrive();
        for (s32 k = 0; k < 0x10001; k++)
            Kern_LibWaitStep();
    }
    *Word(NC_BOUND) = 1;
    return 0;
}

/* The binding, then the record's first 0x310 bytes cleared; the binding's
 * result. */
static s32 Init(void)
{
    s32 r = Bind();
    __builtin_memset(Bytes(NC_BUF), 0, NC_RECORD);
    return r;
}

/* A request, its record filled: -1 while another is pending. Else a
 * semaphore (maximum 1, initially 0) whose id is kept (-1 if it couldn't be
 * made), then the RPC without waiting, the record its own answer's buffer;
 * sceSifCallRpc's result. */
static s32 Request(s32 fno)
{
    SemaParam p;
    s32 sema;
    if (*Word(NC_SEMA) >= 0)
        return -1;
    __builtin_memset(&p, 0, sizeof(p));
    p.maxCount = 1;
    p.initCount = 0;
    sema = CreateSema(&p);
    *Word(NC_SEMA) = sema;
    if (sema < 0)
        return -1;
    return sceSifCallRpc(Client(), fno, 1, Bytes(NC_BUF), NC_BUF_SIZE, Bytes(NC_BUF), NC_BUF_SIZE, func_00AC94C8,
                         (void *)(unsigned long)(u32)sema);
}

/* The pending request forgotten (interrupts off around it, back on if they
 * were) and its semaphore deleted. */
static void Forget(s32 sema)
{
    s32 was = Kern_LibDIntr();
    *Word(NC_SEMA) = -1;
    if (was)
        EIntr();
    DeleteSema(sema);
}

static void PutPath(u32 at, const char *s)
{
    char *d = (char *)Bytes(at);
    while ((*d++ = *s++) != 0) {
    }
}

static s32 Check(void)
{
    s32 sema = *Word(NC_SEMA);
    if (sema < 0)
        return -1;
    if (PollSema(sema) == -1)   /* busy only for -1; any other answer counts as done */
        return 1;
    Forget(sema);
    return 0;
}

static s32 GetResult(s32 *result)
{
    __builtin_memcpy(result, Bytes(NC_BUF), NC_RECORD);
    return 0;
}

static s32 AllocWorkarea(s32 size)
{
    Init();
    *Word(NC_BUF + 0) = size;
    return Request(NC_ALLOC_WORKAREA);
}

static s32 FreeWorkarea(void)
{
    Init();
    return Request(NC_FREE_WORKAREA);
}

/* The record: +0 the list's count, +4 the decode flag, +8 the type, +0xC
 * the list's or the data's address (as the IOP sees it: the PS2 address),
 * +0x10 the path, +0x110 the user name. */
static void GetCount(char *path, s32 type)
{
    Init();
    PutPath(NC_BUF + 0x10, path);
    *Word(NC_BUF + 8) = type;
    Request(NC_GET_COUNT);
}

static void GetList(char *path, s32 type, NetcnfEntry *list, s32 count)
{
    Init();
    PutPath(NC_BUF + 0x10, path);
    *Word(NC_BUF + 0) = count;
    *Word(NC_BUF + 8) = type;
    *Word(NC_BUF + 0xC) = (s32)DataC_Ps2Of(list);
    Request(NC_GET_LIST);
}

static s32 LoadEntry(char *path, s32 type, char *usrName, NetcnfData *data)
{
    Init();
    PutPath(NC_BUF + 0x10, path);
    PutPath(NC_BUF + 0x110, usrName);
    *Word(NC_BUF + 4) = *Word(NC_NODECODE);
    *Word(NC_BUF + 8) = type;
    *Word(NC_BUF + 0xC) = (s32)DataC_Ps2Of(data);
    return Request(NC_LOAD_ENTRY);
}

/* The end: the binding and the record cleared, the work area's free asked,
 * the pending request waited for and forgotten, the binding forgotten. */
static void End(void)
{
    s32 sema;
    Init();
    Request(NC_FREE_WORKAREA);
    sema = *Word(NC_SEMA);
    if (sema >= 0) {
        WaitSema(sema);
        Forget(sema);
    }
    *Word(NC_BOUND) = 0;
}

/* dst cleared (size bytes); if src begins with the library's 7-byte prefix
 * and has exactly five dots, the part between the second and the third is
 * copied, else src; at most size - 1 bytes, then a 0 (also for a size of 0
 * or 1, as the original's). 0. */
static s32 ConvAuthname(char *src, char *dst, s32 size)
{
    const u8 *prefix = Bytes(NC_PREFIX);
    const char *from = src;
    char stop = 0;
    s32 k = 0;
    if (size > 0)
        __builtin_memset(dst, 0, (u32)size);
    for (k = 0; k < 7 && src[k] == (char)prefix[k] && src[k]; k++) {
    }
    if (k == 7 || (k < 7 && src[k] == (char)prefix[k])) {
        s32 dots = 0;
        for (const char *p = src; *p; p++)
            dots += *p == '.';
        if (dots == 5) {
            const char *p = src;
            while (*p != '.')
                p++;
            p++;
            while (*p != '.')
                p++;
            from = p + 1;
            stop = '.';
        }
    }
    k = 0;
    for (s32 n = size; n > 1; n--) {
        char c = from[k];
        if (!c || (stop && c == stop))
            break;
        dst[k] = c;
        k++;
    }
    dst[k] = 0;
    return 0;
}

/* A configuration record: cleared, its numbers (0x1300-0x1318) -1, auth 4,
 * its other flags but fAuth 0xFF. */
static void DataInit(NetcnfData *data)
{
    u8 *d = (u8 *)data;
    __builtin_memset(d, 0, 0x1340);
    for (u32 o = 0x1300; o < 0x131C; o += 4)
        *(s32 *)(d + o) = -1;
    for (u32 o = 0x1320; o < 0x1329; o++)
        if (o != 0x1323)
            d[o] = o == 0x1324 ? 4 : 0xFF;
}

/* ------------------------------------------------------------ the taps
 * (the seam table's prototypes: the game's declarations) */

s32 yn_entry(void)
{
    return Bind();
}

s32 func_00AC8128(void)
{
    return Init();
}

s32 sceNetcnfifCheck(void)
{
    return Check();
}

s32 sceNetcnfifGetResult(s32 *result)
{
    return GetResult(result);
}

void sceNetcnfifSetFNoDecode(s32 flag)
{
    *Word(NC_NODECODE) = flag;
}

void sceNetcnfifGetCount(char *path, s32 type)
{
    GetCount(path, type);
}

void sceNetcnfifGetList(char *path, s32 type, NetcnfEntry *list, s32 count)
{
    GetList(path, type, list, count);
}

s32 sceNetcnfifLoadEntry(char *path, s32 type, char *usrName, NetcnfData *data)
{
    return LoadEntry(path, type, usrName, data);
}

s32 sceNetcnfifAllocWorkarea(s32 size)
{
    return AllocWorkarea(size);
}

s32 sceNetcnfifFreeWorkarea(void)
{
    return FreeWorkarea();
}

void func_00AC8DC0(void)
{
    End();
}

s32 sceNetcnfifConvAuthname(char *src, char *dst, s32 size)
{
    return ConvAuthname(src, dst, size);
}

void sceNetcnfifDataInit(NetcnfData *data)
{
    DataInit(data);
}

/* The requests' end function (sceSifCallRpc's, from the SIF's interrupt,
 * with the request's semaphore): the semaphore signalled, then interrupts
 * enabled (the library's own enable, no delivery point). */
void func_00AC94C8(void *param)
{
    iSignalSema((s32)(unsigned long)param);
    (void)Kern_LibEIntr();
}
