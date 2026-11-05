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
    int pace_tied;               /* the pacer's clock is tied to a VBlank (a load unties it) */
    int sched_have;              /* the pad schedule has held a state (a load forgets it) */
    int keys_reset;              /* a load emptied the key ring: the next event is "every key up" */
    unsigned int st_seen, st_inflight;   /* the host's last request seen, and the one being handled */
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

/* ---- the freecam's mailbox ---- */

static float bits_f(mh2ipc_u32 v)
{
    float f;
    memcpy(&f, &v, sizeof f);
    return f;
}

static mh2ipc_u32 f_bits(float f)
{
    mh2ipc_u32 v;
    memcpy(&v, &f, sizeof v);
    return v;
}

int Ipc_FreecamRead(IpcFreecam *out)
{
    memset(out, 0, sizeof(*out));
    if (!g.on)
        return 0;
    const Mh2IpcFreecam *f = &g.h->freecam;
    for (int tries = 0; tries < 8; tries++) {   /* the host's sequence lock: odd while it writes */
        mh2ipc_u32 a = mh2ipc_load(&f->seq);
        if (a == 0)
            return 0;
        if (a & 1)
            continue;
        mh2ipc_u32 flags = f->flags;
        mh2ipc_u32 eye[3] = {f->eye[0], f->eye[1], f->eye[2]};
        mh2ipc_u32 yaw = f->yaw, pitch = f->pitch, roll = f->roll, fov = f->fov;
        mh2ipc_u32 grant = f->step_grant;
        mh2ipc_fence();
        if (mh2ipc_load(&f->seq) != a)
            continue;
        out->seq = a;
        out->grant = grant;
        out->on = (int)(flags & MH2IPC_FC_ON);
        out->hold = (flags & MH2IPC_FC_HOLD) != 0;
        out->frozen = (flags & MH2IPC_FC_FROZEN) != 0;
        out->hide = (flags >> MH2IPC_FC_HIDE_SHIFT) & ((1u << MH2IPC_FC_HIDE_BITS) - 1u);
        for (int i = 0; i < 3; i++)
            out->eye[i] = bits_f(eye[i]);
        out->yaw = bits_f(yaw);
        out->pitch = bits_f(pitch);
        out->roll = bits_f(roll);
        out->fov = bits_f(fov);
        return 1;
    }
    return 0;   /* the host was writing: the pose in force stays for this frame */
}

void Ipc_FreecamCamera(const float eye[3], const float at[3], float fov)
{
    if (!g.on)
        return;
    Mh2IpcFreecam *f = &g.h->freecam;
    mh2ipc_u32 s = f->cam_seq;
    mh2ipc_store(&f->cam_seq, s + 1);   /* odd: writing */
    mh2ipc_fence();
    for (int i = 0; i < 3; i++) {
        f->cam_eye[i] = f_bits(eye[i]);
        f->cam_at[i] = f_bits(at[i]);
    }
    f->cam_fov = f_bits(fov);
    mh2ipc_store(&f->cam_seq, s + 2);
}

void Ipc_FreecamStatus(int active, unsigned int echo, unsigned long long vblank, int state, int why, unsigned int taken)
{
    if (!g.on)
        return;
    Mh2IpcFreecam *f = &g.h->freecam;
    mh2ipc_u32 s = f->st_seq;
    mh2ipc_store(&f->st_seq, s + 1);   /* odd: writing */
    mh2ipc_fence();
    f->st_active = active != 0;
    f->st_echo = echo;
    f->st_vblank = (mh2ipc_u32)vblank;
    f->st_state = (mh2ipc_u32)state;
    f->st_why = (mh2ipc_u32)why;
    f->st_taken = taken;
    mh2ipc_fence();
    mh2ipc_store(&f->st_seq, s + 2);
}

/* ---- real-time pacing ---- */

#define VBLANK_NS 16683350ull   /* the kernel's VBlank period (kern.h) */


void Ipc_Pace(unsigned long long vblank)
{
    static unsigned long long ref_host, ref_vblank;
    g.vblank = vblank;
    if (!g.on || !(mh2ipc_load(&g.h->host_flags) & MH2IPC_HOST_PACE))
        return;
    unsigned long long now = mh2ipc_time_ns();
    if (!g.pace_tied || vblank < ref_vblank) {
        g.pace_tied = 1;
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

/* Waits for the host to answer the record of sequence number `seq` (rb_done_seq). */
static void wait_done(unsigned int seq)
{
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
        wait_done(seq);
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
    Mh2IpcSched *s = &g.h->sched;
    unsigned int v = (unsigned int)g.vblank;
    if (!g.sched_have) {
        memset(&cur, 0, sizeof(cur));
        cur.connected = 1;
        cur.rx = cur.ry = cur.lx = cur.ly = 0x80;
        g.sched_have = 1;
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
    if (g.on && g.keys_reset) {
        *event = 0;   /* usage 0, up: every key up (a load emptied the ring; the keys held then are not held now) */
        return 1;
    }
    return g.on && mh2ipc_keys_peek(&g.h->keys, event);
}

void Ipc_KeyTake(void)
{
    if (!g.on)
        return;
    if (g.keys_reset)
        g.keys_reset = 0;
    else
        mh2ipc_keys_take(&g.h->keys);
}


void Ipc_SetRumble(unsigned int port, unsigned int small, unsigned int large)
{
    if (g.on && port < IPC_PADS)
        mh2ipc_store(&g.h->rumble[port], (small ? 1u : 0u) | (large > 255 ? 255u : large) << 8);
}

/* ---- save states ---- */

int Ipc_StateRequest(char *path, unsigned int cap)
{
    if (!g.on)
        return 0;
    Mh2IpcState *s = &g.h->state;
    unsigned int seq = mh2ipc_load(&s->req_seq);
    if (seq == g.st_seen)
        return 0;
    g.st_seen = seq;
    g.st_inflight = seq;
    unsigned int kind = s->req_kind;
    size_t n = strnlen(s->req_path, sizeof(s->req_path));
    if ((kind != MH2IPC_ST_SAVE && kind != MH2IPC_ST_LOAD) || n >= cap || n >= sizeof(s->req_path)) {
        Ipc_StateAnswer(g.vblank, "no such request");
        return 0;
    }
    memcpy(path, s->req_path, n);
    path[n] = 0;
    return (int)kind;
}

void Ipc_StateAnswer(unsigned long long vblank, const char *refused)
{
    if (!g.on || !g.st_inflight)
        return;
    Mh2IpcState *s = &g.h->state;
    s->ack_result = refused ? MH2IPC_ST_REFUSED : MH2IPC_ST_DONE;
    s->ack_vblank_lo = (unsigned int)vblank;
    s->ack_vblank_hi = (unsigned int)(vblank >> 32);
    size_t n = refused ? strlen(refused) : 0;
    if (n >= sizeof(s->ack_message))
        n = sizeof(s->ack_message) - 1;
    if (n)
        memcpy(s->ack_message, refused, n);
    s->ack_message[n] = 0;
    unsigned int seq = g.st_inflight;
    g.st_inflight = 0;
    mh2ipc_store(&s->ack_seq, seq);   /* last: the answer is complete when the ack shows */
}

int Ipc_StateHasGs(void)
{
    return g.on && (mh2ipc_load(&g.h->host_flags) & MH2IPC_HOST_GS) && g.h->state.identity[0];
}

const char *Ipc_StateIdentity(void)
{
    static char id[32];
    if (!g.on)
        return "";
    memcpy(id, g.h->state.identity, 31);
    id[31] = 0;
    return id;
}

const void *Ipc_StateGsGet(unsigned int *size, const char **why)
{
    Mh2IpcState *s = &g.h->state;
    *size = 0;
    *why = 0;
    if (!g.on)
        return NULL;
    unsigned int seq = ++g.rb_seq;
    put(MH2IPC_REC_STATE_GET, 0, seq, NULL, 0, NULL, 0);
    wait_done(seq);
    int r = (int)mh2ipc_load(&s->gs_result);
    if (r == 0 && s->gs_bytes > s->window_bytes)
        r = -1;
    if (r != 0) {
        *why = r == -2 ? "the renderer is in the middle of a transfer: try again" : "the renderer could not give its state";
        return NULL;
    }
    *size = s->gs_bytes;
    return g.block + s->window_offset;
}

int Ipc_StateGsSet(const void *data, unsigned int size)
{
    Mh2IpcState *s = &g.h->state;
    if (!g.on || size > s->window_bytes)
        return -1;
    memcpy(g.block + s->window_offset, data, size);
    unsigned int seq = ++g.rb_seq;
    put(MH2IPC_REC_STATE_SET, size, seq, NULL, 0, NULL, 0);
    wait_done(seq);
    return (int)mh2ipc_load(&s->gs_result);
}

void Ipc_StateLoaded(unsigned long long vblank)
{
    g.pace_tied = 0;
    if (!g.on)
        return;
    Mh2IpcState *s = &g.h->state;
    /* the key ring: what the host queued before is dropped, and the game is told every key is up */
    if (Ipc_KeyboardWanted()) {
        mh2ipc_store(&g.h->keys.tail, mh2ipc_load(&g.h->keys.head));
        g.keys_reset = 1;
    }
    /* the audio ring: the host's sound thread drops what it holds up to here (the old timeline's sound) */
    s->audio_flush_head = mh2ipc_load(&g.h->audio.head);
    s->loads++;
    mh2ipc_fence();
    mh2ipc_store(&s->audio_flush_seq, s->audio_flush_seq + 1);
    /* the pad schedule starts again at the loaded VBlank (Mh2IpcSched) */
    Mh2IpcSched *sc = &g.h->sched;
    if (mh2ipc_load(&sc->enabled)) {
        unsigned int e = sc->epoch + 1;
        sc->rewind_vblank = (unsigned int)vblank;
        mh2ipc_store(&sc->epoch, e);
        while (mh2ipc_load(&sc->epoch_ack) != e) {
            mh2ipc_wait_until_ns(mh2ipc_time_ns() + 1000000ull);
            check_host();
        }
        g.sched_have = 0;
    }
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

