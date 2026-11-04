/*
 * mh2-core's side of the IPC (core_ipc.h): the GS ring's producer, the
 * sample ring's producer, the readback requests, the input mailbox's
 * reader, and the states. A host_ file: system headers, the host's calling
 * convention. Headless (no MH2_IPC), every call goes nowhere.
 */
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "ipc/core_ipc.h"
#include "ipc/mh2ipc.h"
#include "ipc/mh2ipc_os.h"

_Static_assert(sizeof(IpcPad) == sizeof(Mh2IpcPad), "IpcPad is Mh2IpcPad");
_Static_assert(sizeof(IpcKeyboard) == sizeof(Mh2IpcKeyboard), "IpcKeyboard is Mh2IpcKeyboard");
_Static_assert(IPC_PADS == MH2IPC_PADS && IPC_AUDIO_RATE == MH2IPC_AUDIO_RATE, "core_ipc.h and mh2ipc.h");

#define WAIT_MS 100u   /* a wait wakes this often to see whether the host still runs */

static struct {
    int on;
    Mh2IpcMap map;
    Mh2IpcHeader *h;
    unsigned char *block;
    unsigned int rb_seq;
    int movie_clock;
    unsigned long long vblank;   /* the VBlank starting (Ipc_Pace, step 2), for --quit-after */
} g = {.movie_clock = -1};

static void host_gone(void)
{
    g.on = 0;
    Host_Fatal("the host (mh2) has gone");
}

static void check_host(void)
{
    if (mh2ipc_load(&g.h->host_state) != MH2IPC_STATE_RUNNING || !mh2ipc_alive(&g.map, g.h->host_pid))
        host_gone();
}

int Ipc_Init(void)
{
    const char *name = getenv(MH2IPC_ENV);
    if (!name || !*name)
        return 0;
    if (mh2ipc_open(&g.map, name) != 0)
        Host_Fatal("the host's shared memory (MH2_IPC) could not be opened");
    g.h = (Mh2IpcHeader *)g.map.base;
    g.block = (unsigned char *)g.map.base;
    if (mh2ipc_load(&g.h->magic) != MH2IPC_MAGIC || g.h->version != MH2IPC_VERSION ||
        g.h->total_bytes > g.map.bytes || g.h->header_bytes < sizeof(Mh2IpcHeader))
        Host_Fatal("the host (mh2) speaks another version of the IPC: rebuild both");
    g.h->core_pid = mh2ipc_pid();
    mh2ipc_store(&g.h->core_state, MH2IPC_STATE_RUNNING);
    mh2ipc_wake(&g.map, MH2IPC_EV_CONTROL, &g.h->core_state);
    g.on = 1;
    return 1;
}

int Ipc_Attached(void)
{
    return g.on;
}

static void end(unsigned int state)
{
    if (!g.on)
        return;
    g.on = 0;
    mh2ipc_store(&g.h->core_state, state);
    mh2ipc_fence();
    mh2ipc_wake(&g.map, MH2IPC_EV_CONTROL, &g.h->core_state);
    mh2ipc_wake(&g.map, MH2IPC_EV_GS_DATA, &g.h->gs.head);
}

void Ipc_Exit(int code)
{
    if (g.on)
        g.h->core_exit_code = (unsigned int)code;
    end(MH2IPC_STATE_EXITED);
}

void Ipc_Fatal(const char *msg)
{
    if (!g.on)
        return;
    size_t n = strlen(msg);
    if (n >= sizeof(g.h->fatal_message))
        n = sizeof(g.h->fatal_message) - 1;
    memcpy(g.h->fatal_message, msg, n);
    g.h->fatal_message[n] = 0;
    end(MH2IPC_STATE_FATAL);
}

/* The player's quit (the window closed) comes at any VBlank; mh2's --quit-after N
 * at the start of VBlank N + 1 exactly (Ipc_Pace gives the VBlank). */
int Ipc_QuitRequested(void)
{
    if (!g.on)
        return 0;
    unsigned long long n = (unsigned long long)g.h->quit_vblank_hi << 32 | g.h->quit_vblank_lo;
    return mh2ipc_load(&g.h->quit_request) != 0 || (n && g.vblank > n);
}

/* ---- real-time pacing ---- */

#define VBLANK_NS 16683350ull   /* the kernel's VBlank period (kern.h) */

void Ipc_Pace(unsigned long long vblank)
{
    static unsigned long long ref_host, ref_vblank;
    static int tied;
    g.vblank = vblank;
    if (!g.on || !(mh2ipc_load(&g.h->host_flags) & MH2IPC_HOST_PACE))
        return;
    unsigned long long now = mh2ipc_time_ns();
    if (!tied || vblank < ref_vblank) {
        tied = 1;
        ref_host = now;
        ref_vblank = vblank;
        return;
    }
    unsigned long long due = ref_host + (vblank - ref_vblank) * VBLANK_NS;
    if (now < due)
        mh2ipc_wait_until_ns(due);
    else if (now - due > 3 * VBLANK_NS) {   /* a stall: carry on from here, without hurrying */
        ref_host = now;
        ref_vblank = vblank;
    }
}

/* ---- the GS ring ---- */

/* A record of two pieces of payload; waits for room while the host
 * catches up. */
static void put(unsigned int type, unsigned int arg0, unsigned int arg1, const void *p1, unsigned int n1, const void *p2,
                unsigned int n2)
{
    Mh2IpcRing *r = &g.h->gs;
    unsigned int adv;
    Mh2IpcRec *rec;
    for (;;) {
        rec = mh2ipc_rec_reserve(r, g.block, n1 + n2, &adv);
        if (rec)
            break;
        unsigned int t = mh2ipc_load(&r->tail);
        mh2ipc_store(&r->producer_waiting, 1);
        mh2ipc_fence();
        rec = mh2ipc_rec_reserve(r, g.block, n1 + n2, &adv);
        if (rec) {
            mh2ipc_store(&r->producer_waiting, 0);
            break;
        }
        mh2ipc_wait(&g.map, MH2IPC_EV_GS_SPACE, &r->tail, t, WAIT_MS);
        mh2ipc_store(&r->producer_waiting, 0);
        check_host();
    }
    if (n1)
        memcpy(rec + 1, p1, n1);
    if (n2)
        memcpy((unsigned char *)(rec + 1) + n1, p2, n2);
    if (mh2ipc_rec_commit(r, rec, type, n1 + n2, arg0, arg1, adv))
        mh2ipc_wake(&g.map, MH2IPC_EV_GS_DATA, &r->head);
}

void Ipc_GsGif(unsigned int path, const void *data, unsigned int qwords)
{
    if (!g.on)
        return;
    const unsigned char *p = (const unsigned char *)data;
    while (qwords) {
        unsigned int n = qwords < MH2IPC_REC_MAX / 16 ? qwords : MH2IPC_REC_MAX / 16;
        put(MH2IPC_REC_GIF, path, 0, p, n * 16, NULL, 0);
        p += n * 16;
        qwords -= n;
    }
}

void Ipc_GsPrivWrite(unsigned int addr, unsigned long long value)
{
    if (g.on)
        put(MH2IPC_REC_PRIV, addr, 0, &value, 8, NULL, 0);
}

void Ipc_GsVSync(unsigned int field, unsigned long long vblank)
{
    if (g.on)
        put(MH2IPC_REC_VSYNC, field, (unsigned int)(g.movie_clock + 1), &vblank, 8, NULL, 0);
}

int Ipc_GsReadback(void *dst, unsigned int qwords)
{
    unsigned char *out = (unsigned char *)dst;
    if (!g.on) {
        memset(out, 0, (size_t)qwords * 16);
        return 0;
    }
    while (qwords) {
        unsigned int n = qwords < g.h->rb_size / 16 ? qwords : g.h->rb_size / 16;
        unsigned int seq = ++g.rb_seq;
        put(MH2IPC_REC_READBACK, n, seq, NULL, 0, NULL, 0);
        for (;;) {
            unsigned int d = mh2ipc_load(&g.h->rb_done_seq);
            if (d == seq)
                break;
            mh2ipc_store(&g.h->rb_core_waiting, 1);
            mh2ipc_fence();
            if (mh2ipc_load(&g.h->rb_done_seq) != seq)
                mh2ipc_wait(&g.map, MH2IPC_EV_RB_DONE, &g.h->rb_done_seq, d, WAIT_MS);
            mh2ipc_store(&g.h->rb_core_waiting, 0);
            check_host();
        }
        memcpy(out, g.block + g.h->rb_offset, (size_t)n * 16);
        out += (size_t)n * 16;
        qwords -= n;
    }
    return (mh2ipc_load(&g.h->host_flags) & MH2IPC_HOST_GS) ? 1 : 0;
}

/* ---- the samples ---- */

void Ipc_AudioWrite(const short *samples, unsigned int frames)
{
    if (g.on && frames)
        mh2ipc_stream_write(&g.h->audio, g.block, samples, frames * 4u);
}

/* ---- the input ---- */

static void neutral_pad(IpcPad *out)
{
    memset(out, 0, sizeof(*out));
    out->rx = out->ry = out->lx = out->ly = 0x80;
}

/* The pad schedule (Mh2IpcSched): port 0's state at the VBlank starting (g.vblank) is the last entry whose VBlank
 * is at most it. The host's horizon is read before the entries, so that when the ring holds no entry up to this
 * VBlank and the horizon has reached it, none is missing; when it hasn't, the core waits for the host. The
 * speeds of the two processes then change nothing the game sees. */
static void SchedPad(Mh2IpcPad *out)
{
    static Mh2IpcPad cur;
    static int have;
    Mh2IpcSched *s = &g.h->sched;
    unsigned int v = (unsigned int)g.vblank;
    if (!have) {
        memset(&cur, 0, sizeof(cur));
        cur.connected = 1;
        cur.rx = cur.ry = cur.lx = cur.ly = 0x80;
        have = 1;
    }
    for (;;) {
        unsigned int horizon = mh2ipc_load(&s->horizon);
        unsigned int head = mh2ipc_load(&s->head);
        unsigned int tail = s->tail;
        while (tail != head && s->entries[tail & (MH2IPC_SCHED_RING - 1u)].vblank <= v) {
            memcpy(&cur, &s->entries[tail & (MH2IPC_SCHED_RING - 1u)].pad, sizeof(cur));
            tail++;
        }
        mh2ipc_store(&s->tail, tail);
        if (tail != head || horizon >= v)
            break;
        mh2ipc_store(&s->waiting, 1);
        mh2ipc_fence();
        if (mh2ipc_load(&s->horizon) == horizon && mh2ipc_load(&s->head) == head)
            mh2ipc_wait(&g.map, MH2IPC_EV_SCHED, &s->horizon, horizon, WAIT_MS);
        mh2ipc_store(&s->waiting, 0);
        check_host();
    }
    *out = cur;
}

int Ipc_ReadPad(unsigned int port, IpcPad *out)
{
    if (!g.on || port >= IPC_PADS) {
        neutral_pad(out);
        return 0;
    }
    Mh2IpcPad pads[MH2IPC_PADS];
    Mh2IpcKeyboard kb;
    mh2ipc_input_read(&g.h->input, pads, &kb);
    if (port == 0 && mh2ipc_load(&g.h->sched.enabled))
        SchedPad(&pads[0]);
    memcpy(out, &pads[port], sizeof(*out));
    return (int)out->connected;
}

int Ipc_ReadKeyboard(IpcKeyboard *out)
{
    if (!g.on) {
        memset(out, 0, sizeof(*out));
        return 0;
    }
    Mh2IpcPad pads[MH2IPC_PADS];
    Mh2IpcKeyboard kb;
    mh2ipc_input_read(&g.h->input, pads, &kb);
    memcpy(out, &kb, sizeof(*out));
    return (int)out->connected;
}

int Ipc_KeyboardWanted(void)
{
    return g.on && (mh2ipc_load(&g.h->host_flags) & MH2IPC_HOST_USB_KEYBOARD) != 0;
}

int Ipc_KeyPeek(unsigned int *event)
{
    return g.on && mh2ipc_keys_peek(&g.h->keys, event);
}

void Ipc_KeyTake(void)
{
    if (g.on)
        mh2ipc_keys_take(&g.h->keys);
}


void Ipc_SetRumble(unsigned int port, unsigned int small, unsigned int large)
{
    if (g.on && port < IPC_PADS)
        mh2ipc_store(&g.h->rumble[port], (small ? 1u : 0u) | (large > 255 ? 255u : large) << 8);
}

/* ---- movies ---- */

void Ipc_MovieOpen(unsigned int id, const char *path, unsigned long long offset, unsigned int size, unsigned int mode,
                   int volume)
{
    g.movie_clock = -1;
    if (!g.on)
        return;
    Mh2IpcMovieOpen m;
    memset(&m, 0, sizeof(m));
    m.offset_lo = (unsigned int)offset;
    m.offset_hi = (unsigned int)(offset >> 32);
    m.size = size;
    m.mode = mode;
    m.volume = volume;
    m.id = id;
    put(MH2IPC_REC_MOVIE_OPEN, 0, 0, &m, sizeof(m), path, (unsigned int)strlen(path) + 1);
}

void Ipc_MoviePlay(unsigned int id)
{
    if (!g.on)
        return;
    mh2ipc_store(&g.h->movie_sound_end, ~0u);
    mh2ipc_store(&g.h->movie_sound_begin, mh2ipc_load(&g.h->audio.head));
    mh2ipc_store(&g.h->movie_sound_id, id);
}

void Ipc_MovieClock(int clock)
{
    g.movie_clock = clock < 0 ? -1 : clock;
}

void Ipc_MovieClose(unsigned int id)
{
    g.movie_clock = -1;
    if (!g.on)
        return;
    if (mh2ipc_load(&g.h->movie_sound_id) == id && mh2ipc_load(&g.h->movie_sound_end) == ~0u)
        mh2ipc_store(&g.h->movie_sound_end, mh2ipc_load(&g.h->audio.head));
    put(MH2IPC_REC_MOVIE_CLOSE, id, 0, NULL, 0, NULL, 0);
}

