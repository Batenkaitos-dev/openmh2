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
    int loading;                 /* the game's loading screen is open (Ipc_LoadingScreen) */
    int pace_movie;              /* a movie is open: no VBlank is unpaced */
    int unpaced;                 /* this VBlank is not waited for: its samples are dropped */
    unsigned int taken;          /* the pause's grants taken */
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

int Pal_GsLabels;   /* include/pal.h's labels in a release build: 1 only when the host made the pass's ring */

unsigned long long Ipc_TimeNs(void)
{
    return mh2ipc_time_ns();
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



    Pal_GsLabels = g.h->pass.size && g.h->pass.offset >= MH2IPC_TOTAL_BYTES && g.h->pass.offset + g.h->pass.size <= g.h->total_bytes;
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
    if (Pal_GsLabels)
        mh2ipc_wake(&g.map, MH2IPC_EV_PASS_DATA, &g.h->pass.head);
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

/* What the host asks of the pacing, and what the core tells it. Pacing is only the wait for real time: no value the game computes, no
 * VBlank's number and no line a recorded session holds depends on it. The modes:
 *  - the plain pause (HOLD): before the VBlank does anything, the core waits for the host to grant it (pace_grant) or to resume;
 *  - fast-forward (FF), and fast loading (the host's setting and the game's loading screen open, Ipc_LoadingScreen): the VBlank
 *    is not waited for (FF's cap: it is, at 100 / cap of its time), its samples are dropped (Ipc_AudioWrite), and the next paced
 *    VBlank ties the clocks again;
 *  - a movie open (Ipc_PaceMovie) is never unpaced: its picture and sound are timed by the VBlanks;
 *  - otherwise, with MH2IPC_HOST_PACE, the wait for the VBlank's time. */
void Ipc_LoadingScreen(int on)
{
    g.loading = on != 0;
}

void Ipc_PaceMovie(int open)
{
    g.pace_movie = open != 0;
}

static void pace_publish(unsigned int state)
{
    mh2ipc_store(&g.h->pace_state, state);
    mh2ipc_store(&g.h->pace_taken, g.taken);
    mh2ipc_store(&g.h->pace_vblank, (unsigned int)g.vblank);
}

/* The pause: returns when the host grants a VBlank, resumes, or asks the game to quit (or the host is gone). */
static void pace_hold(void)
{
    for (;;) {
        unsigned int grant = mh2ipc_load(&g.h->pace_grant);
        if (!(mh2ipc_load(&g.h->pace_flags) & MH2IPC_PACE_HOLD)) {
            g.taken = grant;   /* resumed: a step asked for just before it is not left over for the next pause */
            break;
        }
        if ((int)(grant - g.taken) > 0) {
            g.taken++;
            break;
        }
        if (mh2ipc_load(&g.h->quit_request))
            break;
        pace_publish(MH2IPC_PACE_HELD);
        mh2ipc_wait_until_ns(mh2ipc_time_ns() + 2000000ull);
        check_host();
    }
    g.pace_tied = 0;
}

/* The VBlanks the freecam's pause has put back (Pal_FreezeBias): the pacer counts the real ones. */
static unsigned long long g_pace_bias;
void Ipc_PaceBias(unsigned long long bias)
{
    g_pace_bias = bias;
}



static unsigned int sPaceState;   /* the pacing's timeline at the VBlank paced last: 0 none (not paced), 1 on it, 2 begun or broken here (the stamp tells the host's clock) */
static void pace_inner(unsigned long long vblank)
{
    static unsigned long long ref_host, ref_vblank, ref_period;
    g.vblank = vblank;
    vblank += g_pace_bias;   /* (g.vblank, the game's number, is what the pad schedule and --quit-after read) */
    if (!g.on)
        return;
    sPaceState = 0;   /* no timeline unless this call paces the VBlank */
    unsigned int pf = mh2ipc_load(&g.h->pace_flags);
    if (pf & MH2IPC_PACE_HOLD) {
        pace_hold();
        pf = mh2ipc_load(&g.h->pace_flags);
    } else {
        g.taken = mh2ipc_load(&g.h->pace_grant);
    }
    unsigned int hf = mh2ipc_load(&g.h->host_flags);
    unsigned long long period = VBLANK_NS;   /* the time one VBlank takes; 0: it is not waited for */
    g.unpaced = 0;
    if (!g.pace_movie) {
        if (pf & MH2IPC_PACE_FF) {
            unsigned int cap = mh2ipc_load(&g.h->pace_cap);
            period = cap ? VBLANK_NS * 100ull / cap : 0;
            g.unpaced = 1;
        } else if ((hf & MH2IPC_HOST_FASTLOAD) && g.loading) {
            period = 0;
            g.unpaced = 1;
        }
    }
    pace_publish((g.unpaced ? MH2IPC_PACE_UNPACED : 0) | (g.loading ? MH2IPC_PACE_LOADING : 0));
    if (!(hf & MH2IPC_HOST_PACE))
        return;
    if (!period) {
        g.pace_tied = 0;    /* the next paced VBlank ties the clocks again */
        return;
    }
    unsigned long long now = mh2ipc_time_ns();
    if (!g.pace_tied || vblank < ref_vblank || period != ref_period) {
        g.pace_tied = 1;
        ref_host = now;
        ref_vblank = vblank;
        ref_period = period;
        sPaceState = 2;   /* the pacing's timeline begins here: a jump for the host's clock */
        return;
    }
    unsigned long long due = ref_host + (vblank - ref_vblank) * period;
    sPaceState = 1;   /* on the timeline: the VBlank's due time is ref + n periods */
    if (now < due)
        mh2ipc_wait_until_ns(due);
    else if (now - due > 3 * period) {   /* a stall: carry on from here, without hurrying */
        ref_host = now;
        ref_vblank = vblank;
        sPaceState = 2;   /* a stall: the timeline goes on from here */
    }
}

void Ipc_Pace(unsigned long long vblank)
{
    pace_inner(vblank);
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

static unsigned int sGen;
static unsigned int sPlanUsed;   /* the live plan the step's windows were made at (Ipc_GsPlanUsed), for the stamp */
static int sKeepLabels;   /* the second pass wants the labels with no host attached too (and, in a development build, a time stamp before each VSync record) */
void Ipc_GsVSync(unsigned int field, unsigned long long vblank)
{
    unsigned long long t_in = sKeepLabels ? mh2ipc_time_ns() : 0;
    sGen++;
    if (g.on && sKeepLabels)   /* the second pass is on: the time the core reached this VSync, for the host's clock (the host's reader may be busy with the pass's wait when it reads the record) */
    {
        unsigned long long stamp[2];
        stamp[0] = t_in;
        stamp[1] = sPaceState;
        put(MH2IPC_REC_STAMP, sPlanUsed, 0, stamp, 16, NULL, 0);
    }
    sPlanUsed = 0;
    sPaceState = 0;
    if (g.on)
        put(MH2IPC_REC_VSYNC, field, (unsigned int)(g.movie_clock + 1), &vblank, 8, NULL, 0);
}

void Ipc_GsCamera(const float m[32])
{
    if (g.on)
        put(MH2IPC_REC_CAMERA, 0, 0, m, 32 * sizeof(float), NULL, 0);
}

/* The labels of the model draws (PC_GS_MODEL): a table by the guest address of each draw's first DMA tag, open addressing with a bounded probe. The game builds the draw lists of the frame to come while the chain of the frame before goes out, so a label is good for a few VBlanks
 * (sGen counts them); one that was used is not used again. */
#define MODEL_SLOTS 2048u
#define MODEL_PROBE 32u
#define MODEL_LIFE 8u
typedef struct {
    unsigned int top, last, handle, flags, prog, gen, used, site;
    float world[16];
    float cam[32];
    float vp[16];
} ModelLabel;
static ModelLabel sModels[MODEL_SLOTS];
static unsigned int sOpenLast;   /* the last tag of the chain whose record came last (0: none open) */

static ModelLabel *label_slot(unsigned int top)
{
    unsigned int h = (top >> 4) * 2654435761u >> 8;
    ModelLabel *use = NULL;
    for (unsigned int i = 0; i < MODEL_PROBE; i++) {
        ModelLabel *e = &sModels[(h + i) & (MODEL_SLOTS - 1)];
        if (e->top == top) {
            use = e;
            break;
        }
        if (!use && (e->top == 0 || e->used || sGen - e->gen > MODEL_LIFE))
            use = e;
        if (e->top == 0)
            break;
    }
    if (!use)
        use = &sModels[h & (MODEL_SLOTS - 1)];
    return use;
}

static float *sSlots;     /* ... and the matrix table of each (32 x 16 floats a label, beside sModels: the host's memory, not the image's) */
void Ipc_GsKeepLabels(int on)
{
    on = on && Pal_GsLabels;   /* a release's pass runs only with the ring */
    sKeepLabels = on;
    if (on && !sSlots) {
        sSlots = Host_Alloc(MODEL_SLOTS * 32 * 16 * sizeof(float));
        if (!sSlots)
            sKeepLabels = 0;
    }
}

/* The live plan (plan 11's step 5a): the host's plan for the step that follows its last anchor, when one is written; the VBlank the step's VSync record is expected to have, the moments (rising alphas
 * in 1/256) and the moments to make when the plan is not in time. 0 when the host wrote none, or is writing it. */
int Ipc_GsLivePlan(unsigned int *vblank, unsigned int *n, unsigned int *alpha, unsigned int *fb)
{
    Mh2IpcLivePlan lp, other;
    int have, i;
    if (!g.on)
        return 0;
    /* *vblank is the VBlank number the step's VSync record is expected to have: the plan of that number is taken when the host has written it (it writes each plan two steps ahead, in the slot its number names);
     * else the other slot's plan, which has an older or a newer number, for the counts */
    have = mh2ipc_liveplan_read(&g.h->live[(*vblank >> 1) & 1u], &lp);
    if (!have || lp.vblank != *vblank) {
        i = mh2ipc_liveplan_read(&g.h->live[(~(*vblank >> 1)) & 1u], &other);
        if (i && (!have || other.vblank == *vblank)) {
            lp = other;
            have = 1;
        }
    }
    if (!have)
        return 0;
    *vblank = lp.vblank;
    *n = lp.n;
    *fb = lp.fb;
    for (unsigned int i = 0; i < MH2IPC_LIVE_MAX; i++)
        alpha[i] = lp.alpha[i];
    return 1;
}

/* The VBlank number of the live plan the step's windows were made at (0: none); the stamp before the step's VSync record says it, once. */
void Ipc_GsPlanUsed(unsigned int vblank)
{
    sPlanUsed = vblank;
}

void Ipc_GsModelSet(unsigned int top, unsigned int last, unsigned int handle, unsigned int flags, unsigned int prog, const float *world, const float *cam, unsigned int site, const float *vp)
{
    if (!Pal_GsLabels)
        return;
    if (!g.on && !sKeepLabels)
        return;
    ModelLabel *use = label_slot(top);
    use->top = top;
    use->last = last;
    use->handle = handle;
    use->flags = flags;
    use->prog = prog;
    use->site = site;
    use->gen = sGen;
    use->used = 0;
    memcpy(use->world, world, sizeof(use->world));
    memcpy(use->cam, cam, sizeof(use->cam));
    memcpy(use->vp, vp, sizeof(use->vp));
    if (sKeepLabels)   /* the second pass: world is gMtxSlots, whose 32 slots hold the bones' matrices */
        memcpy(sSlots + (size_t)(use - sModels) * 512, world, 512 * sizeof(float));
}

/* The labels of the 2D packets that an element anchored to a 3D point queued (PC_GS_ANCHOR): the guest address and the size of each packet. A text's glyph packets go
 * out together in one chain (one transfer of many packets), so a label is found by its address inside the data a tag moves (Ipc_GsAnchorNext), and the DMA emulation
 * cuts the transfer at the packet's edges. Few of them are live at once; a label lives MODEL_LIFE VBlanks and is used once. */
#define ANCHOR_SLOTS 512u
typedef struct {
    unsigned int addr, qwc, id, gen, used;
    float point[3];
    float cam[32];
} AnchorLabel;
static AnchorLabel sAnch[ANCHOR_SLOTS];
static unsigned int sAnchHead, sAnchLive;
static AnchorLabel *sAnchNext;   /* the label Ipc_GsAnchorNext found */

void Ipc_GsAnchorSet(unsigned int addr, unsigned int qwc, unsigned int id, const float *point, const float *cam)
{
    if (!Pal_GsLabels)
        return;
    if (!g.on)
        return;
    AnchorLabel *e = NULL;
    for (unsigned int i = 0; i < ANCHOR_SLOTS; i++)
        if (sAnch[i].addr == addr && !sAnch[i].used && sAnch[i].gen) {
            e = &sAnch[i];   /* a label of the same packet that was never sent: the packet is new */
            break;
        }
    if (!e)
        e = &sAnch[sAnchHead++ & (ANCHOR_SLOTS - 1)];
    if (e->gen && !e->used)
        sAnchLive--;
    e->addr = addr;
    e->qwc = qwc;
    e->id = id;
    e->gen = sGen ? sGen : 1;
    e->used = 0;
    memcpy(e->point, point, sizeof(e->point));
    memcpy(e->cam, cam, sizeof(e->cam));
    sAnchLive++;
}

int Ipc_GsAnchorNext(unsigned int from, unsigned int end, unsigned int *at, unsigned int *qwc)
{
    if (!g.on || !sAnchLive)
        return 0;
    AnchorLabel *best = NULL;
    for (unsigned int i = 0; i < ANCHOR_SLOTS; i++) {
        AnchorLabel *e = &sAnch[i];
        if (!e->gen || e->used)
            continue;
        if (sGen - e->gen > MODEL_LIFE) {
            e->used = 1;   /* never sent */
            sAnchLive--;
            continue;
        }
        /* the packet starts in the data, or its first quadword is the DMA tag just before it (a cnt tag: the tag is not data, the rest of the packet is) */
        if ((e->addr >= from || e->addr + 16 == from) && e->addr < end && (!best || e->addr < best->addr))
            best = e;
    }
    if (!best)
        return 0;
    const unsigned int s = best->addr < from ? from : best->addr, t = best->addr + best->qwc * 16 < end ? best->addr + best->qwc * 16 : end;
    *at = s;
    *qwc = (t - s) / 16;
    sAnchNext = best;
    return 1;
}

void Ipc_GsAnchorBegin(void)
{
    AnchorLabel *e = sAnchNext;
    if (!e || e->used)
        return;
    Mh2IpcAnchor a;
    memcpy(a.point, e->point, sizeof(a.point));
    memcpy(a.cam, e->cam, sizeof(a.cam));
    e->used = 1;
    sAnchLive--;
    put(MH2IPC_REC_ANCHOR, e->id, 0, &a, sizeof(a), NULL, 0);
}

void Ipc_GsAnchorEnd(void)
{
    put(MH2IPC_REC_ANCHOR, 0, 0, NULL, 0, NULL, 0);
}

void Ipc_GsPass(unsigned int kind, unsigned int arg, const void *data, unsigned int qwords)
{
    if (!g.on)
        return;
    const unsigned char *p = (const unsigned char *)data;
    if (!qwords) {
        put(MH2IPC_REC_PASS, kind, arg, NULL, 0, NULL, 0);
        return;
    }
    while (qwords) {
        unsigned int n = qwords < MH2IPC_REC_MAX / 16 ? qwords : MH2IPC_REC_MAX / 16;
        put(MH2IPC_REC_PASS, kind, arg, p, n * 16, NULL, 0);
        p += n * 16;
        qwords -= n;
    }
}

/* The put's census (MH2_PASS_CENSUS): what a put's time is made of. The counters count only: no record, and nothing of the ring, depends on them. They are written by the
 * producer, which the caller's lock makes one at a time. what 1: starts the counting (the times are taken only then). what 0: out takes the IPC_PRC_N counters, in the order of
 * the enum. what 2: out takes the stretches in which pieces waited for room: their number, then for each the VBlank of its first wait, of its last, its sleeps and its
 * nanoseconds (a wait within IPC_PRC_NEAR VBlanks of the stretch's last belongs to it; the stretches after the IPC_PRC_EPS-th are counted in the last one). */
enum { IPC_PRC_PIECES, IPC_PRC_BYTES, IPC_PRC_COPY_NS, IPC_PRC_ROOM_WAITS, IPC_PRC_ROOM_SLEEPS, IPC_PRC_ROOM_NS, IPC_PRC_WAKES, IPC_PRC_WAKE_NS, IPC_PRC_FILL_MAX, IPC_PRC_FILL_SUM, IPC_PRC_N };
#define IPC_PRC_EPS 24u
#define IPC_PRC_NEAR 8u
static unsigned long long sPrc[IPC_PRC_N], sPrcEp[IPC_PRC_EPS][4];
static unsigned int sPrcEpN;
static int sPrcOn;
void Ipc_GsPassRingCensus(int what, unsigned long long *out)
{
    if (what == 1)
        sPrcOn = 1;
    if (what == 0 && out)
        for (unsigned int i = 0; i < IPC_PRC_N; i++)
            out[i] = sPrc[i];
    if (what == 2 && out) {
        out[0] = sPrcEpN;
        for (unsigned int i = 0; i < sPrcEpN * 4; i++)
            out[1 + i] = sPrcEp[i / 4][i % 4];
    }
}

static void PrcRoomWait(unsigned int sleeps, unsigned long long ns)
{
    unsigned long long v = g.vblank;   /* (the game's thread writes it: a census reads it as it is) */
    if (!sPrcEpN || (v - sPrcEp[sPrcEpN - 1][1] > IPC_PRC_NEAR && sPrcEpN < IPC_PRC_EPS)) {
        sPrcEp[sPrcEpN][0] = v;
        sPrcEp[sPrcEpN][2] = sPrcEp[sPrcEpN][3] = 0;
        sPrcEpN++;
    }
    sPrcEp[sPrcEpN - 1][1] = v;
    sPrcEp[sPrcEpN - 1][2] += sleeps;
    sPrcEp[sPrcEpN - 1][3] += ns;
}

/* The pass thread's records (MH2_PASS_THREAD=1): its own ring, a single producer (the pass thread), the host's reader thread the consumer. Only the header's pass ring and the
 * block are touched, and they do not change after the host made them. Returns 0 when there is no ring (the records are dropped). */
int Ipc_GsPassRing(unsigned int arg0, unsigned int arg1, const void *data, unsigned int qwords)
{
    if (!g.on || !g.h->pass.size)
        return 0;
    Mh2IpcRing *r = &g.h->pass;
    unsigned char *block = g.block;
    const unsigned char *p = (const unsigned char *)data;
    unsigned int first = 1;
    while (first || qwords) {
        unsigned int n = qwords < MH2IPC_REC_MAX / 16 ? qwords : MH2IPC_REC_MAX / 16, adv;
        Mh2IpcRec *rec;
        unsigned long long c0 = 0, c1 = 0, c2 = 0;
        unsigned int slept = 0, wake;
        first = 0;
        if (sPrcOn) {
            unsigned int fill = r->head - mh2ipc_load(&r->tail);
            sPrc[IPC_PRC_FILL_SUM] += fill;
            if (fill > sPrc[IPC_PRC_FILL_MAX])
                sPrc[IPC_PRC_FILL_MAX] = fill;
            c0 = mh2ipc_time_ns();
        }
        for (;;) {
            rec = mh2ipc_rec_reserve(r, block, n * 16, &adv);
            if (rec)
                break;
            unsigned int t = mh2ipc_load(&r->tail);
            mh2ipc_store(&r->producer_waiting, 1);
            mh2ipc_fence();
            rec = mh2ipc_rec_reserve(r, block, n * 16, &adv);
            if (rec) {
                mh2ipc_store(&r->producer_waiting, 0);
                break;
            }
            mh2ipc_wait(&g.map, MH2IPC_EV_PASS_SPACE, &r->tail, t, WAIT_MS);
            mh2ipc_store(&r->producer_waiting, 0);
            check_host();
            slept++;
        }
        if (sPrcOn)
            c1 = mh2ipc_time_ns();
        if (n)
            memcpy(rec + 1, p, n * 16);
        wake = (unsigned int)mh2ipc_rec_commit(r, rec, MH2IPC_REC_PASS, n * 16, arg0, arg1, adv);
        if (sPrcOn)
            c2 = mh2ipc_time_ns();
        if (wake)
            mh2ipc_wake(&g.map, MH2IPC_EV_PASS_DATA, &r->head);
        if (sPrcOn) {
            sPrc[IPC_PRC_PIECES]++;
            sPrc[IPC_PRC_BYTES] += n * 16;
            sPrc[IPC_PRC_COPY_NS] += c2 - c1;
            if (slept) {
                sPrc[IPC_PRC_ROOM_WAITS]++;
                sPrc[IPC_PRC_ROOM_SLEEPS] += slept;
                sPrc[IPC_PRC_ROOM_NS] += c1 - c0;
                PrcRoomWait(slept, c1 - c0);
            }
            if (wake) {
                sPrc[IPC_PRC_WAKES]++;
                sPrc[IPC_PRC_WAKE_NS] += mh2ipc_time_ns() - c2;
            }
        }
        p += n * 16;
        qwords -= n;
        if ((arg0 & 15u) == 7u)   /* a moment's record that goes in pieces: the pieces after the first are kind 8 */
            arg0 = (arg0 & ~15u) | 8u;
    }
    return 1;
}

int Ipc_GsModelTag(unsigned int taddr, IpcLabel *out)
{
    if (!Pal_GsLabels)
        return 0;
    if (!g.on && !sKeepLabels)
        return 0;
    unsigned int h = (taddr >> 4) * 2654435761u >> 8;
    for (unsigned int i = 0; i < MODEL_PROBE; i++) {
        ModelLabel *e = &sModels[(h + i) & (MODEL_SLOTS - 1)];
        if (e->top == 0)
            return 0;
        if (e->top == taddr && !e->used && sGen - e->gen <= MODEL_LIFE) {
            e->used = 1;
            if (out) {
                out->top = e->top;
                out->last = e->last;
                out->handle = e->handle;
                out->flags = e->flags;
                out->prog = e->prog;
                out->site = e->site;
                memcpy(out->world, e->world, sizeof(out->world));
                memcpy(out->cam, e->cam, sizeof(out->cam));
                memcpy(out->vp, e->vp, sizeof(out->vp));
                if (sKeepLabels)
                    memcpy(out->slots, sSlots + (size_t)(e - sModels) * 512, sizeof(out->slots));
            }
            if (g.on) {
                sOpenLast = e->last;
                Mh2IpcModel m;
                m.prog = e->prog;
                m.top = e->top;
                m.last = e->last;
                m.site = e->site;
                memcpy(m.world, e->world, sizeof(m.world));
                memcpy(m.cam, e->cam, sizeof(m.cam));
                put(MH2IPC_REC_MODEL, e->handle, e->flags, &m, sizeof(m), NULL, 0);
            }
            return 1;
        }
    }
    return 0;
}

void Ipc_GsModelTagEnd(unsigned int taddr)
{
    if (sOpenLast && taddr == sOpenLast && g.on) {
        sOpenLast = 0;
        put(MH2IPC_REC_MODEL, 0, 0, NULL, 0, NULL, 0);
    }
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
    if (g.on && frames && !g.unpaced)   /* an unpaced VBlank's samples are dropped, so that the sound after it is not late */
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
    /* The last entries taken, for a VBlank number that comes again (the freecam's pause puts the game back to an earlier VBlank). */
    static Mh2IpcPad hist_pad[16];
    static unsigned int hist_vb[16], hist_n;
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
            hist_vb[hist_n & 15u] = s->entries[tail & (MH2IPC_SCHED_RING - 1u)].vblank;
            hist_pad[hist_n & 15u] = cur;
            hist_n++;
            tail++;
        }
        mh2ipc_store(&s->tail, tail);
        if (tail != head || horizon >= v)
            break;
        mh2ipc_store(&s->waiting, 1);
        mh2ipc_fence();
        if (mh2ipc_load(&s->horizon) == horizon && mh2ipc_load(&s->head) == head) {
            mh2ipc_wait(&g.map, MH2IPC_EV_SCHED, &s->horizon, horizon, WAIT_MS);
        }
        mh2ipc_store(&s->waiting, 0);
        check_host();
    }
    if (hist_n && hist_vb[(hist_n - 1u) & 15u] > v) {   /* a VBlank that comes again: the pad of that time */
        unsigned int k;
        Mh2IpcPad first;
        memset(&first, 0, sizeof(first));
        first.connected = 1;
        first.rx = first.ry = first.lx = first.ly = 0x80;
        *out = first;
        for (k = 0; k < 16u && k < hist_n; k++) {
            unsigned int at = (hist_n - 1u - k) & 15u;
            if (hist_vb[at] <= v) {
                *out = hist_pad[at];
                break;
            }
        }
        return;
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

