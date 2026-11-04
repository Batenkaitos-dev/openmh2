/* libdbc, the pads' connection library under libpad2 and libvib: sceDbcInit,
 * sceDbcEnd and the calls the other two make (io_pad.h).
 *
 * Written from the libraries' behaviour at their interface, as the original's
 * clients show it: the kernel calls the game can observe (the semaphore
 * sceDbcInit makes, the binds, the RPCs and their waits, WaitSema and
 * SignalSema around each request) are the SDK's, in its order. The library's
 * words and request areas are the core's own memory. */
#include "common.h"
#include "padio.h"
#include "io/io_kernel.h"
#include "io/io_pad.h"

static struct {
    int inited;
    s32 sema;
    u32 sendIdx;   /* the send's client used last (0 or 1) */
    SifClientData client, client1C, send[2];
    u8 area[DBC_AREA];          /* the command's request and answer */
    u8 sendArea[2][0xC0];
    u32 links[DBC_SOCKETS];     /* the IOP's connection words */
    u32 open[DBC_SOCKETS];      /* their copy at each query */
} g_dbc;

static u32 Rd32(const u8 *p, u32 off)
{
    u32 v;
    __builtin_memcpy(&v, p + off, 4);
    return v;
}

static void Wr32(u8 *p, u32 off, u32 v)
{
    __builtin_memcpy(p + off, &v, 4);
}

static s32 Rpc(u32 fn)
{
    return sceSifCallRpc(&g_dbc.client, (s32)fn, 0, g_dbc.area, DBC_AREA, g_dbc.area, DBC_AREA, 0, 0);
}

/* A bind, again while the IOP has no server; 0 when a bind fails. */
static int Bind(SifClientData *cl, u32 sid)
{
    for (;;) {
        if (sceSifBindRpc(cl, sid, 0) < 0)
            return 0;
        if (cl->serve)
            return 1;
    }
}

s32 sceDbcInit(void);
static s32 sceDbcInitWork(void)
{
    if (g_dbc.inited == 1)
        return 1;
    g_dbc.sema = 0;
    sceSifInitRpc(0);
    if (!Bind(&g_dbc.client, DBC_SID_MAIN))
        return 0;
    Rpc(DBC_FN_VERSION);
    if ((Rd32(g_dbc.area, 0) & ~0xFu) != 0x320u)
        return 0;
    if (!Bind(&g_dbc.client1C, DBC_SID_131C))
        return 0;
    for (u32 i = 0; i < 2; i++)
        if (!Bind(&g_dbc.send[i], DBC_SID_SEND0 + i))
            return 0;
    SemaParam p;
    __builtin_memset(&p, 0, sizeof(p));
    p.maxCount = 0x7F;
    p.initCount = 1;
    g_dbc.sema = CreateSema(&p);
    if (g_dbc.sema < 0)
        return 0;
    for (u32 i = 0; i < DBC_SOCKETS; i++)
        g_dbc.open[i] = 0;
    Wr32(g_dbc.area, 4, (u32)g_dbc.links);   /* the connection words' address */
    Rpc(DBC_FN_LINKS);
    g_dbc.inited = 1;
    return 1;
}

s32 sceDbcEnd(void);
static s32 sceDbcEndWork(void)
{
    if (!g_dbc.inited)
        return 0;
    g_dbc.inited = 0;
    if (g_dbc.sema >= 0)
        DeleteSema(g_dbc.sema);
    return 1;
}

s32 IoDbc_CreateSocket(const void *desc, void *buf0, void *buf1)
{
    if (!g_dbc.inited)
        return 0;
    WaitSema(g_dbc.sema);
    __builtin_memcpy(g_dbc.area, desc, 0x24);
    Wr32(g_dbc.area, 0x28, (u32)buf0);
    Wr32(g_dbc.area, 0x2C, (u32)buf1);
    if (Rpc(DBC_FN_CREATE) < 0) {
        SignalSema(g_dbc.sema);
        return 0;
    }
    s32 r = (s32)Rd32(g_dbc.area, 0x24);
    SignalSema(g_dbc.sema);
    return r;
}

s32 IoDbc_CloseSocket(s32 socket)
{
    if (!g_dbc.inited)
        return 0;
    WaitSema(g_dbc.sema);
    Wr32(g_dbc.area, 0, (u32)socket);
    if (Rpc(DBC_FN_CLOSE) < 0) {
        SignalSema(g_dbc.sema);
        return 0;
    }
    s32 r = (s32)Rd32(g_dbc.area, 4);
    SignalSema(g_dbc.sema);
    return r;
}

/* The socket's set-up query: -12 unless the IOP says it's connected. */
s32 IoDbc_Query(s32 socket)
{
    s32 on = Kern_LibDIntr();
    for (u32 i = 0; i < DBC_SOCKETS; i++)
        g_dbc.open[i] = g_dbc.links[i];
    int ok = g_dbc.open[socket & (DBC_SOCKETS - 1)] == 1 && (u32)socket < DBC_SOCKETS;
    if (on)
        (void)Kern_LibEIntr();
    if (!ok)
        return -12;
    WaitSema(g_dbc.sema);
    Wr32(g_dbc.area, 0, (u32)socket);
    if (Rpc(DBC_FN_QUERY) < 0) {
        SignalSema(g_dbc.sema);
        return 0;
    }
    s32 r = (s32)Rd32(g_dbc.area, 4);
    SignalSema(g_dbc.sema);
    return r;
}

/* A command with its answer: the result (+0x8C); when not negative, the
 * answer's size through size and its bytes to out. */
s32 IoDbc_Command(s32 socket, u32 cmd, s32 *size, void *out)
{
    WaitSema(g_dbc.sema);
    Wr32(g_dbc.area, 0, (u32)socket);
    Wr32(g_dbc.area, 4, cmd);
    Wr32(g_dbc.area, 8, (u32)*size);
    if (Rpc(DBC_FN_RECEIVE) < 0) {
        SignalSema(g_dbc.sema);
        return 0;
    }
    s32 res = (s32)Rd32(g_dbc.area, 0x8C);
    if (res >= 0) {
        s32 n = (s32)Rd32(g_dbc.area, 8);
        *size = n;
        for (s32 i = 0; i < n; i++)
            ((u8 *)out)[i] = g_dbc.area[0xC + i];
    }
    SignalSema(g_dbc.sema);
    return res;
}

/* A send on the next of the two clients, without a wait: 1 sent, else 0 (a size
 * over 0x80, or that client's last send still in flight). */
s32 IoDbc_Send(s32 socket, u32 cmd, s32 *size, const void *data)
{
    if (*size > 0x80)
        return 0;
    WaitSema(g_dbc.sema);
    u32 idx = g_dbc.sendIdx + 1;
    if ((s32)idx >= 2)
        idx = 0;
    g_dbc.sendIdx = idx;
    u8 *buf = g_dbc.sendArea[idx];
    s32 n = *size;
    Wr32(buf, 0, (u32)socket);
    Wr32(buf, 4, cmd);
    Wr32(buf, 8, (u32)n);
    for (s32 i = 0; i < n; i++)
        buf[0xC + i] = ((const u8 *)data)[i];
    if (sceSifCheckStatRpc(&g_dbc.send[idx]) == 0 &&
        sceSifCallRpc(&g_dbc.send[idx], (s32)idx, 1, buf, DBC_AREA, buf, DBC_AREA, 0, 0) >= 0) {
        SignalSema(g_dbc.sema);
        return 1;
    }
    SignalSema(g_dbc.sema);
    return 0;
}

/* ------------------------------------------------------------ the taps
 * verify's trace tap for each function the game calls (pc/core/verify/verify.h). */

s32 sceDbcInit(void)
{
    return sceDbcInitWork();
}

s32 sceDbcEnd(void)
{
    return sceDbcEndWork();
}
