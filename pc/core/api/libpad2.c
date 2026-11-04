/* The pad library (libpad2) and the vibration library (libvib) on libdbc's
 * sockets.
 *
 * Written from the libraries' behaviour at their interface, as the original's
 * clients show it: what the game
 * sees is the socket buffer it hands scePad2CreateSocket (two 0x80-byte halves
 * the IOP writes at each VBlank; the newer has the larger counter at +0x7C),
 * the results, and libdbc's kernel calls underneath. The sockets' records are
 * the core's own memory. (The records' table of the data's fields, which only
 * the library itself reads, is not kept.) */
#include "common.h"
#include "padio.h"
#include "io/io_pad.h"

#define P2_STATE_CMD 0x0101800Cu
#define VIB_PROFILE_CMD 0x01038002u
#define VIB_ACT_CMD 0x0103400Bu

typedef struct P2Socket {
    s32 made, setup;
    u32 lastCounter;
    s32 query;
    u8 *buf;
} P2Socket;

static P2Socket g_p2[DBC_SOCKETS];

static u32 Rd32(const u8 *p, u32 off)
{
    u32 v;
    __builtin_memcpy(&v, p + off, 4);
    return v;
}

static P2Socket *Rec(s32 socket)
{
    return &g_p2[(u32)socket % DBC_SOCKETS];
}

/* The newer half: the second when its counter is the larger (signed). */
static u8 *Newer(s32 socket)
{
    u8 *b = Rec(socket)->buf;
    return (s32)Rd32(b, 0x7C) < (s32)Rd32(b, 0xFC) ? b + 0x80 : b;
}

/* The socket's set-up through libdbc's query. */
static s32 Setup(s32 socket)
{
    s32 r = IoDbc_Query(socket);
    if (r >= 0) {
        Rec(socket)->setup = 1;
        Rec(socket)->query = r;
    }
    return r;
}

/* Both halves: state and validity 0, the data 0xFF, the counter 0. */
static void ResetBuffer(s32 socket)
{
    u8 *b = Rec(socket)->buf;
    for (u32 k = 0; k < 2; k++) {
        u8 *h = b + 0x80 * k;
        __builtin_memset(h, 0, 8);
        __builtin_memset(h + 0x1C, 0xFF, 0x20);
        __builtin_memset(h + 0x7C, 0, 4);
    }
}

/* 1 when the newer half's counter is not 0 and not the one seen last (it is
 * then noted), else 0. */
static int NewCounter(s32 socket)
{
    u32 cnt = Rd32(Newer(socket), 0x7C);
    P2Socket *r = Rec(socket);
    if (cnt == 0 || cnt == r->lastCounter)
        return 0;
    r->lastCounter = cnt;
    return 1;
}

s32 scePad2Init(s32 mode);
static s32 scePad2InitWork(s32 mode)
{
    (void)mode;
    for (u32 s = 0; s < DBC_SOCKETS; s++) {
        g_p2[s].made = g_p2[s].setup = 0;
        g_p2[s].lastCounter = 0;
        g_p2[s].query = 0;
    }
    return 1;
}

s32 scePad2CreateSocket(scePad2SocketParam *param, void *buf);
static s32 scePad2CreateSocketWork(scePad2SocketParam *param, void *buf)
{
    u32 desc[9];
    const u32 *p = (const u32 *)param;
    desc[0] = p[0] | 1;
    desc[1] = 1;
    for (u32 i = 1; i < 8; i++)
        desc[1 + i] = p[i];
    s32 s = IoDbc_CreateSocket(desc, buf, (u8 *)buf + 0x80);
    if (s < 0)
        return s;
    Rec(s)->buf = buf;
    Rec(s)->made = 1;
    ResetBuffer(s);
    return s;
}

s32 scePad2DeleteSocket(s32 socket);
static s32 scePad2DeleteSocketWork(s32 socket)
{
    s32 r = IoDbc_CloseSocket(socket);
    if (r < 0)
        return r;
    Rec(socket)->made = Rec(socket)->setup = 0;
    Rec(socket)->query = 0;
    return 1;
}

s32 scePad2End(void);
static s32 scePad2EndWork(void)
{
    for (s32 s = 0; s < DBC_SOCKETS; s++) {
        if (g_p2[s].made) {
            s32 r = scePad2DeleteSocket(s);
            if (r < 0)
                return r;
        }
    }
    return 1;
}

s32 scePad2Read(s32 socket, u8 *data);
static s32 scePad2ReadWork(s32 socket, u8 *data)
{
    P2Socket *r = Rec(socket);
    if (!r->made)
        return -1;
    if (!r->setup && Setup(socket) < 0)
        return -1;
    u8 *h = Newer(socket);
    u32 len = h[2];
    if (len)
        __builtin_memcpy(data, h + 0x1C, len);
    return Rd32(h, 4) ? (s32)len : -1;
}

s32 scePad2GetState(s32 socket);
static s32 scePad2GetStateWork(s32 socket)
{
    if (!Rec(socket)->setup && Setup(socket) < 0)
        return 0;
    s32 v;
    if (!NewCounter(socket)) {
        u8 b = 0;
        s32 size = 0;
        if (IoDbc_Command(socket, P2_STATE_CMD, &size, &b) < 0)
            goto reset;
        v = b;
    } else {
        u8 *h = Newer(socket);
        if (!Rd32(h, 4) && Setup(socket) < 0)
            goto reset;
        v = h[0];
    }
    return v;
reset:
    ResetBuffer(socket);
    Rec(socket)->setup = 0;
    return 0;
}

s32 scePad2GetButtonProfile(s32 socket, u8 *profile);
static s32 scePad2GetButtonProfileWork(s32 socket, u8 *profile)
{
    scePad2GetState(socket);
    if (!Rec(socket)->setup && Setup(socket) < 0)
        return -1;
    u8 *h = Newer(socket);
    u32 n = h[3];
    if (n)
        __builtin_memcpy(profile, h + 0x1C + h[2], n);
    return Rd32(h, 4) ? (s32)n : -1;
}

s32 sceVibGetProfile(s32 socket, u8 *profile);
static s32 sceVibGetProfileWork(s32 socket, u8 *profile)
{
    s32 size = 0;
    s32 r = IoDbc_Command(socket, VIB_PROFILE_CMD, &size, profile);
    return r < 0 ? r : size;
}

/* The two sizes, the data and the profile after it as the send's bytes, with
 * 0x28 as the send's size (in the frame after them: data longer than 0x28
 * overwrites it, as in the SDK's frame). */
s32 sceVibSetActParam(s32 socket, s32 profileSize, u8 *profile, s32 dataSize, u8 *data);
static s32 sceVibSetActParamWork(s32 socket, s32 profileSize, u8 *profile, s32 dataSize, u8 *data)
{
    u8 frame[0x200];
    s32 size;
    __builtin_memset(frame, 0, sizeof(frame));
    size = 0x28;
    __builtin_memcpy(frame + 0x30, &size, 4);
    __builtin_memcpy(frame, &profileSize, 4);
    __builtin_memcpy(frame + 4, &dataSize, 4);
    if (dataSize > 0 && 8 + dataSize <= (s32)sizeof(frame))
        __builtin_memcpy(frame + 8, data, (u32)dataSize);
    if (profileSize > 0 && 8 + dataSize + profileSize <= (s32)sizeof(frame))
        __builtin_memcpy(frame + 8 + dataSize, profile, (u32)profileSize);
    __builtin_memcpy(&size, frame + 0x30, 4);
    return IoDbc_Send(socket, VIB_ACT_CMD, &size, frame);
}

/* ------------------------------------------------------------ the taps
 * verify's trace tap for each function the game calls (pc/core/verify/verify.h). */

s32 scePad2Init(s32 mode)
{
    return scePad2InitWork(mode);
}

s32 scePad2CreateSocket(scePad2SocketParam *param, void *buf)
{
    return scePad2CreateSocketWork(param, buf);
}

s32 scePad2DeleteSocket(s32 socket)
{
    return scePad2DeleteSocketWork(socket);
}

s32 scePad2End(void)
{
    return scePad2EndWork();
}

s32 scePad2Read(s32 socket, u8 *data)
{
    return scePad2ReadWork(socket, data);
}

s32 scePad2GetState(s32 socket)
{
    return scePad2GetStateWork(socket);
}

s32 scePad2GetButtonProfile(s32 socket, u8 *profile)
{
    return scePad2GetButtonProfileWork(socket, profile);
}

s32 sceVibGetProfile(s32 socket, u8 *profile)
{
    return sceVibGetProfileWork(socket, profile);
}

s32 sceVibSetActParam(s32 socket, s32 profileSize, u8 *profile, s32 dataSize, u8 *data)
{
    return sceVibSetActParamWork(socket, profileSize, profile, dataSize, data);
}
