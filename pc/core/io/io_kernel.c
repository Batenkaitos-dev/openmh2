/*
 * The io module on the kernel's interface (kern.h): its start-up hook, the pads'
 * VBlank step, the IOP services of io on the kernel's registry of services,
 * the IOP-to-EE commands they send, and the calendar.
 */
#include "io/io.h"
#include "io/io_kernel.h"
#include "io/io_pad.h"

/* ------------------------------------------------------------ the start */

/* The kernel calls io directly (kernel/start.c, vblank.c): Io_Init(argc, argv)
 * at its start, and Io_PadVBlank at the VBlank's step 7 (the specification's
 * 9), so io leaves kern.h's start-up hook Kern_InitIo to its weak default. */

/* ------------------------------------------------------------ the services */

#define IO_MAX_SERVICES 24
#define IO_MAX_DEFERRED 16

typedef struct IoService {
    KernSifService svc;    /* the kernel keeps a pointer to it */
    IopRpcHandler fn;
} IoService;

static IoService g_svc[IO_MAX_SERVICES];
static u32 g_nsvc;
static u8 g_reply[0x20000];      /* the original's reply buffer: longer replies are cut */
static int g_inCall;
static struct {
    u32 cid, size;
    u8 payload[IO_SIF_CMD_MAX];
} g_deferred[IO_MAX_DEFERRED];
static u32 g_ndeferred;

static IoService *ServiceById(u32 sid)
{
    for (u32 i = 0; i < g_nsvc; i++)
        if (g_svc[i].svc.id == sid)
            return &g_svc[i];
    return 0;
}

/*
 * A call to one of io's services (the original's rpc_call for an HLE
 * service): the handler runs at the request with a zero-filled reply of the
 * client's receive size, the reply goes to the client's receive buffer, then
 * the RPC's end is queued, then the commands the handler sent. The end is
 * queued here (Kern_SifAnswer) rather than by returning KERN_SIF_ANSWERED,
 * so that the handler's commands follow it.
 */
static int Call(KernSifCall *c)
{
    IoService *s = ServiceById(c->service);
    u32 recv = c->recv && c->rsize > 0 ? (u32)c->rsize : 0;
    IopRpcCall call;
    if (recv > sizeof(g_reply)) {
        Io_Log("a reply of %u bytes to service 0x%08X cut", recv, c->service);
        recv = sizeof(g_reply);
    }
    __builtin_memset(g_reply, 0, recv);
    call.sid = c->service;
    call.fno = (u32)c->fno;
    call.data = c->send;
    call.size = c->ssize > 0 ? (u32)c->ssize : 0;
    call.reply = g_reply;
    call.recv_size = recv;
    g_inCall = 1;
    if (s && s->fn)
        s->fn(&call);
    g_inCall = 0;
    if (recv)
        __builtin_memcpy(c->recv, g_reply, recv);
    Kern_SifAnswer(c->token);
    for (u32 i = 0; i < g_ndeferred; i++)
        Kern_SifSendCmd(g_deferred[i].cid, g_deferred[i].payload, g_deferred[i].size);
    g_ndeferred = 0;
    return KERN_SIF_LATER;
}

void Iop_RegisterService(u32 sid, const char *name, IopRpcHandler handler)
{
    IoService *s = ServiceById(sid);
    if (!s) {
        if (g_nsvc == IO_MAX_SERVICES) {
            Io_Log("too many services: 0x%08X (%s) not registered", sid, (u32)name);
            return;
        }
        s = &g_svc[g_nsvc++];
    }
    s->svc.id = sid;
    s->svc.name = name;
    s->svc.call = Call;
    s->svc.server = 0;
    s->svc.buff = 0;
    s->fn = handler;
    Kern_SifRegister(&s->svc);
}

void Iop_SendCmd(u32 cid, const void *payload, u32 size)
{
    if (size > IO_SIF_CMD_MAX) {
        Io_Log("a SIF command 0x%08X of %u bytes: cut", cid, size);
        size = IO_SIF_CMD_MAX;
    }
    if (!g_inCall) {
        Kern_SifSendCmd(cid, payload, size);
        return;
    }
    if (g_ndeferred == IO_MAX_DEFERRED) {
        Io_Log("too many SIF commands in one RPC: 0x%08X dropped", cid, 0);
        return;
    }
    g_deferred[g_ndeferred].cid = cid;
    g_deferred[g_ndeferred].size = size;
    __builtin_memcpy(g_deferred[g_ndeferred].payload, payload, size);
    g_ndeferred++;
}

/* ------------------------------------------------------------ the calendar */

/* The kernel's calendar in a zone (gmtime of the seconds plus the zone's
 * offset). Days to a date: the proleptic Gregorian calendar, from 1970-03-01
 * counted in 400-year eras (the seconds fit 32 bits until 2106). */
void Io_CalendarZone(s32 utcOffsetSeconds, u32 out[6])
{
    u32 t = (u32)Kern_CalendarSeconds() + (u32)utcOffsetSeconds;
    u32 days = t / 86400u, sec = t % 86400u;
    out[0] = sec % 60u;
    out[1] = sec / 60u % 60u;
    out[2] = sec / 3600u;
    u32 z = days + 719468u;              /* days since 0000-03-01 */
    u32 era = z / 146097u;
    u32 doe = z - era * 146097u;
    u32 yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    u32 doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    u32 mp = (5u * doy + 2u) / 153u;
    u32 m = mp < 10u ? mp + 3u : mp - 9u;
    out[3] = doy - (153u * mp + 2u) / 5u + 1u;
    out[4] = m;
    out[5] = yoe + era * 400u + (m <= 2u ? 1u : 0u);
}

/* In JST (+9 h): the card's times, whatever the zone, as the original's. */
void Io_CalendarJst(u32 out[6])
{
    Io_CalendarZone(9 * 3600, out);
}
