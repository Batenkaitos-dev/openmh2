/* The IOP's side of libdbc and libpad2 (Dbc_Manager with the DualShock 2
 * driver), the services 0x80001300, 0x8000131C, 0x8000131E and 0x8000131F, as
 * the original's HLE answers them (the same protocol), and the pads' state at each VBlank. */
#include "io/io.h"
#include "io/io_kernel.h"
#include "io/io_pad.h"

#define DS2_DATA_LEN 18
#define DS2_PROFILE_LEN 5

void (*Io_PadRumble)(int port, u8 small, u8 large);

typedef struct DbcSocket {
    int used;
    u8 *buf[2];    /* the two halves (the game's buffer) */
    u32 type, port, slot;
    u32 counter;
} DbcSocket;

static DbcSocket g_sock[DBC_SOCKETS];
static u32 *g_status;   /* the client's connection words, one a socket */
static IoPadState g_pad[2] = {{1, 0, 0x80, 0x80, 0x80, 0x80, {0}}, {0, 0, 0x80, 0x80, 0x80, 0x80, {0}}};
static struct { u8 small, large; } g_rumble;

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

void Io_PadSetState(int port, const IoPadState *st)
{
    if ((u32)port >= 2)
        return;
    IoPadState s = *st;
    g_pad[port] = s;
}

void Io_PadGetState(int port, IoPadState *st)
{
    IoPadState none = {0, 0, 0x80, 0x80, 0x80, 0x80, {0}};
    /* the original's pads: port 0 only */
    *st = port == 0 ? g_pad[0] : none;
    if (port == 0)
        st->connected = 1;
}

void IoPad_Reset(void)
{
    __builtin_memset(g_sock, 0, sizeof(g_sock));
    g_status = 0;
    g_rumble.small = g_rumble.large = 0;
}

static int Connected(const DbcSocket *s)
{
    IoPadState st;
    Io_PadGetState((int)s->port, &st);
    return s->used && s->slot == 0 && st.connected;
}

static void WriteStatus(int id)
{
    if (g_status)
        g_status[id] = Connected(&g_sock[id]) ? 1 : 0;
}

static void Main(IopRpcCall *c)
{
    const u8 *q = c->data;
    u8 *r = c->reply;
    switch (c->fno) {
    case DBC_FN_VERSION:   /* libdbc 3.20 wants 0x32x */
        Wr32(r, 0, 0x0320);
        return;
    case DBC_FN_LINKS:     /* the client's connection words */
        g_status = (u32 *)Rd32(q, 4);
        for (int i = 0; i < DBC_SOCKETS; i++)
            WriteStatus(i);
        Wr32(r, 0, 1);
        return;
    case DBC_FN_CREATE: {
        int id = -1;
        for (int i = 0; i < DBC_SOCKETS; i++) {
            if (!g_sock[i].used) {
                id = i;
                break;
            }
        }
        if (id >= 0) {
            DbcSocket *s = &g_sock[id];
            __builtin_memset(s, 0, sizeof(*s));
            s->used = 1;
            s->type = Rd32(q, 0x00);
            s->port = Rd32(q, 0x08);
            s->slot = Rd32(q, 0x0C);
            s->buf[0] = (u8 *)Rd32(q, 0x28);
            s->buf[1] = (u8 *)Rd32(q, 0x2C);
            WriteStatus(id);
            Io_Log("pad: socket %d on port %u", (u32)id, s->port);
        }
        __builtin_memcpy(r, q, DBC_AREA < c->recv_size ? DBC_AREA : c->recv_size);
        Wr32(r, 0x24, (u32)id);
        return;
    }
    case DBC_FN_CLOSE: {
        u32 id = Rd32(q, 0);
        if (id < DBC_SOCKETS)
            g_sock[id].used = 0;
        Wr32(r, 4, 0);
        return;
    }
    case DBC_FN_QUERY: {
        u32 id = Rd32(q, 0);
        Wr32(r, 4, id < DBC_SOCKETS && Connected(&g_sock[id]) ? 0 : (u32)-1);
        return;
    }
    case DBC_FN_RECEIVE: {   /* {socket, cmd, size} */
        u32 id = Rd32(q, 0), cmd = Rd32(q, 4), cmdId = cmd & 0x3FFF;
        int ok = id < DBC_SOCKETS && Connected(&g_sock[id]);
        __builtin_memset(r, 0, DBC_AREA);
        Wr32(r, 0, id);
        Wr32(r, 4, cmd);
        if (!ok) {
            Wr32(r, 0x8C, (u32)-1);
            return;
        }
        if (cmdId == 2) {          /* the actuators' profile: two motors */
            Wr32(r, 8, 4);
            r[0xC] = 0x03;
        } else if (cmdId == 0xC) { /* the state */
            Wr32(r, 8, 1);
            r[0xC] = 1;            /* stable */
        } else {
            Io_Log("pad: receive command 0x%08X", cmd, 0);
            Wr32(r, 8, 0);
        }
        Wr32(r, 0x8C, 0);
        return;
    }
    default:
        Io_Log("pad: libdbc function 0x%X", c->fno, 0);
        if (c->recv_size >= 8)
            Wr32(r, 4, 0);
        return;
    }
}

/* The sends (0x8000131E and F in turn): the actuators, command 0x0103400B,
 * {n1, n2, n2 motor bytes, ...}; a DualShock 2 has n2 = 2: the small motor,
 * then the large one. */
static void Async(IopRpcCall *c)
{
    if (c->size >= 0xC + 8) {
        const u8 *q = c->data;
        u32 id = Rd32(q, 0), cmd = Rd32(q, 4), size = Rd32(q, 8);
        if ((cmd & 0x3FFF) == 0xB && size >= 8 && id < DBC_SOCKETS && g_sock[id].used) {
            u32 n2 = Rd32(q, 0xC + 4);
            if (n2 >= 2 && 8 + n2 <= size) {
                u8 small = q[0xC + 8], large = q[0xC + 9];
                if (small != g_rumble.small || large != g_rumble.large) {
                    g_rumble.small = small;
                    g_rumble.large = large;
                    if (Io_PadRumble)
                        Io_PadRumble((int)g_sock[id].port, small, large);
                }
            }
        }
    }
    if (c->recv_size > 0x8C)
        Wr32(c->reply, 0x8C, 0);
}

void Io_PadVBlank(void)
{
    static const u8 profile[DS2_PROFILE_LEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    for (int i = 0; i < DBC_SOCKETS; i++) {
        DbcSocket *s = &g_sock[i];
        if (!s->used)
            continue;
        IoPadState st;
        Io_PadGetState((int)s->port, &st);
        if (!(s->slot == 0 && st.connected))
            continue;
        u8 half[0x80];
        __builtin_memset(half, 0, sizeof(half));
        half[0] = 1;   /* stable */
        half[2] = DS2_DATA_LEN;
        half[3] = DS2_PROFILE_LEN;
        Wr32(half, 4, 1);   /* valid */
        u8 *d = half + 0x1C;
        d[0] = (u8)~(st.buttons & 0xFF);
        d[1] = (u8)~(st.buttons >> 8);
        d[2] = st.rx;
        d[3] = st.ry;
        d[4] = st.lx;
        d[5] = st.ly;
        __builtin_memcpy(d + 6, st.pressure, 12);
        __builtin_memcpy(d + DS2_DATA_LEN, profile, DS2_PROFILE_LEN);
        s->counter++;
        Wr32(half, 0x7C, s->counter);
        if (s->buf[s->counter & 1])
            __builtin_memcpy(s->buf[s->counter & 1], half, sizeof(half));
    }
}

void IoPad_Register(void)
{
    Iop_RegisterService(DBC_SID_MAIN, "dbcman", Main);
    Iop_RegisterService(DBC_SID_131C, "dbcman 131C", Async);
    Iop_RegisterService(DBC_SID_SEND0, "dbcman 131E", Async);
    Iop_RegisterService(DBC_SID_SEND0 + 1, "dbcman 131F", Async);
}
