/*
 * The IPC's protocol test (ctest ipc_protocol): this process is both
 * sides. A thread plays the host (pc/ipc/mh2ipc.h's consumer, as pc/host
 * does), the main thread is the core (core_ipc.h). It sends more GIF data
 * than the ring holds, in pieces of every size up to past a record's
 * largest (so the ring wraps, pads and makes the core wait), with
 * privileged writes, VSyncs, readbacks and movie commands between them,
 * and checks that the host saw everything in order. Then the samples, the
 * input mailbox, the motors, the quit request and the exit; and first, the
 * headless answers.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

#include "ipc/core_ipc.h"
#include "ipc/mh2ipc.h"
#include "ipc/mh2ipc_os.h"

static Mh2IpcMap hm;
static Mh2IpcHeader *H;
static unsigned char *B;

#define FAIL(...)                                  \
    do {                                           \
        fprintf(stderr, "ipc_protocol: FAIL: ");   \
        fprintf(stderr, __VA_ARGS__);              \
        fprintf(stderr, "\n");                     \
        exit(1);                                   \
    } while (0)

/* What the host saw. */
static struct {
    unsigned int gif_words, gif_recs, priv, vsync, readbacks, movie_open, movie_close, pads;
    unsigned long long last_vblank, priv_sum;
    int last_movie;
    char error[200];
} seen;

static void host_error(const char *what)
{
    if (!seen.error[0])
        snprintf(seen.error, sizeof(seen.error), "%s", what);
}

static void host_record(const Mh2IpcRec *rec)
{
    const unsigned char *p = (const unsigned char *)(rec + 1);
    switch (rec->type) {
    case MH2IPC_REC_GIF: {
        if (rec->arg0 != 1 + seen.gif_recs % 3 && rec->arg0 != 2)
            host_error("GIF path");
        if (rec->bytes % 16 || rec->bytes > MH2IPC_REC_MAX)
            host_error("GIF size");
        for (unsigned int i = 0; i < rec->bytes / 4; i++) {
            unsigned int w;
            memcpy(&w, p + i * 4, 4);
            if (w != seen.gif_words * 2654435761u) {
                host_error("GIF data out of order");
                break;
            }
            seen.gif_words++;
        }
        seen.gif_recs++;
        break;
    }
    case MH2IPC_REC_PRIV: {
        unsigned long long v;
        memcpy(&v, p, 8);
        if (rec->bytes != 8 || rec->arg0 != 0x12000000u + (unsigned int)(seen.priv % 15) * 0x10)
            host_error("PRIV");
        seen.priv_sum += v;
        seen.priv++;
        break;
    }
    case MH2IPC_REC_VSYNC:
        memcpy(&seen.last_vblank, p, 8);
        if (rec->arg0 != (unsigned int)(seen.last_vblank & 1) || seen.last_vblank != seen.vsync)
            host_error("VSYNC");
        seen.last_movie = (int)rec->arg1 - 1;
        seen.vsync++;
        break;
    case MH2IPC_REC_READBACK: {
        unsigned int *dst = (unsigned int *)(B + H->rb_offset);
        if (rec->arg0 * 16 > H->rb_size)
            host_error("READBACK size");
        for (unsigned int i = 0; i < rec->arg0 * 4; i++)
            dst[i] = rec->arg1 * 1000003u + i;
        seen.readbacks++;
        mh2ipc_store(&H->rb_done_seq, rec->arg1);
        mh2ipc_fence();
        if (__atomic_load_n(&H->rb_core_waiting, __ATOMIC_RELAXED))
            mh2ipc_wake(&hm, MH2IPC_EV_RB_DONE, &H->rb_done_seq);
        break;
    }
    case MH2IPC_REC_MOVIE_OPEN: {
        Mh2IpcMovieOpen m;
        memcpy(&m, p, sizeof(m));
        if (m.offset_hi != 1 || m.offset_lo != 0x2345u || m.size != 777 || m.mode != 2 || m.volume != -30 || m.id != 5 ||
            strcmp((const char *)p + sizeof(m), "/disc/mh2-disc.arc") != 0)
            host_error("MOVIE_OPEN");
        seen.movie_open++;
        break;
    }
    case MH2IPC_REC_MOVIE_CLOSE:
        if (rec->arg0 != 5)
            host_error("MOVIE_CLOSE");
        seen.movie_close++;
        break;
    default:
        host_error("a record of an unknown type");
    }
}

/* The host's GS thread: until the core has ended and the ring is empty. */
static void host_loop(void)
{
    Mh2IpcRing *r = &H->gs;
    for (;;) {
        const Mh2IpcRec *rec = mh2ipc_rec_peek(r, B);
        if (!rec) {
            unsigned int head = mh2ipc_load(&r->head);
            if (mh2ipc_load(&H->core_state) != MH2IPC_STATE_RUNNING && head == r->tail)
                return;
            mh2ipc_store(&r->consumer_waiting, 1);
            mh2ipc_fence();
            if (!mh2ipc_rec_peek(r, B))
                mh2ipc_wait(&hm, MH2IPC_EV_GS_DATA, &r->head, head, 50);
            mh2ipc_store(&r->consumer_waiting, 0);
            continue;
        }
        host_record(rec);
        if (mh2ipc_rec_release(r, rec))
            mh2ipc_wake(&hm, MH2IPC_EV_GS_SPACE, &r->tail);
    }
}

#ifdef _WIN32
static DWORD WINAPI host_thread(LPVOID a)
{
    (void)a;
    host_loop();
    return 0;
}
#else
static void *host_thread(void *a)
{
    (void)a;
    host_loop();
    return NULL;
}
#endif

static unsigned int rng = 12345;
static unsigned int rnd(unsigned int n)
{
    rng = rng * 1103515245u + 12345u;
    return (rng >> 8) % n;
}

static void headless(void)
{
    IpcPad pad;
    IpcKeyboard kb;
    unsigned char buf[64];
    memset(buf, 0xAA, sizeof(buf));
    if (Ipc_Init() != 0 || Ipc_Attached())
        FAIL("headless: attached without MH2_IPC");
    Ipc_GsGif(1, buf, 4);
    Ipc_GsPrivWrite(0x12000000u, 1);
    Ipc_GsVSync(0, 0);
    Ipc_AudioWrite((const short *)buf, 16);
    if (Ipc_GsReadback(buf, 4) != 0 || buf[0] || buf[63])
        FAIL("headless: a readback is not zeros");
    memset(&pad, 0xFF, sizeof(pad));
    if (Ipc_ReadPad(0, &pad) != 0 || pad.connected || pad.buttons || pad.lx != 0x80 || pad.ry != 0x80 || pad.pressure[3])
        FAIL("headless: the pad is not disconnected and neutral");
    memset(&kb, 0xFF, sizeof(kb));
    if (Ipc_ReadKeyboard(&kb) != 0 || kb.keys[7] || kb.modifiers)
        FAIL("headless: the keyboard");
    if (Ipc_QuitRequested())
        FAIL("headless: quit");
    Ipc_MovieOpen(1, "x", 0, 1, 0, 0);
    Ipc_MoviePlay(1);
    Ipc_MovieClock(3);
    Ipc_MovieClose(1);
    Ipc_Exit(0);
}

int main(void)
{
    static unsigned int gif[MH2IPC_REC_MAX / 2];
#ifdef _WIN32
    _putenv("MH2_IPC=");
#else
    unsetenv(MH2IPC_ENV);
#endif
    headless();

    if (mh2ipc_create(&hm, MH2IPC_TOTAL_BYTES) != 0)
        FAIL("mh2ipc_create");
    H = (Mh2IpcHeader *)hm.base;
    B = (unsigned char *)hm.base;
    mh2ipc_layout(H, mh2ipc_pid());
    H->host_flags = MH2IPC_HOST_GS;
#ifdef _WIN32
    char env[160];
    snprintf(env, sizeof(env), "%s=%s", MH2IPC_ENV, hm.name);
    _putenv(env);
#else
    setenv(MH2IPC_ENV, hm.name, 1);
#endif
    if (Ipc_Init() != 1 || !Ipc_Attached() || H->core_state != MH2IPC_STATE_RUNNING || H->core_pid != mh2ipc_pid())
        FAIL("Ipc_Init did not attach");

    /* the input mailbox, both ways round */
    Mh2IpcPad pads[MH2IPC_PADS];
    Mh2IpcKeyboard kb;
    memset(pads, 0, sizeof(pads));
    memset(&kb, 0, sizeof(kb));
    pads[1].connected = 1;
    pads[1].buttons = 0x4008;
    pads[1].lx = 3;
    pads[1].pressure[11] = 200;
    kb.connected = 1;
    kb.modifiers = 0x22;
    kb.keys[0x04 >> 5] |= 1u << (0x04 & 31);
    mh2ipc_input_write(&H->input, pads, &kb);
    IpcPad p;
    IpcKeyboard k;
    if (Ipc_ReadPad(1, &p) != 1 || p.buttons != 0x4008 || p.lx != 3 || p.pressure[11] != 200)
        FAIL("the pad");
    if (Ipc_ReadPad(0, &p) != 0 || Ipc_ReadPad(2, &p) != 0 || p.lx != 0x80)
        FAIL("the pads that aren't there");
    if (Ipc_ReadKeyboard(&k) != 1 || k.modifiers != 0x22 || k.keys[0] != 0x10)
        FAIL("the keyboard");
    /* the USB keyboard's key events (Mh2IpcKeys): the host's flag, the ring in order, a full ring */
    {
        unsigned int ev = 0, n;
        if (Ipc_KeyboardWanted() || Ipc_KeyPeek(&ev))
            FAIL("a keyboard before the host asked for one");
        H->host_flags |= MH2IPC_HOST_USB_KEYBOARD;
        if (!Ipc_KeyboardWanted())
            FAIL("the keyboard flag");
        if (!mh2ipc_keys_push(&H->keys, 0x04, 1) || !mh2ipc_keys_push(&H->keys, 0x04, 0) || !mh2ipc_keys_push(&H->keys, 0xE1, 1))
            FAIL("the key ring takes events");
        if (!Ipc_KeyPeek(&ev) || ev != (0x04u | MH2IPC_KEY_DOWN) || !Ipc_KeyPeek(&ev) || ev != (0x04u | MH2IPC_KEY_DOWN))
            FAIL("Ipc_KeyPeek does not take");
        Ipc_KeyTake();
        if (!Ipc_KeyPeek(&ev) || ev != 0x04u)
            FAIL("the second event");
        Ipc_KeyTake();
        if (!Ipc_KeyPeek(&ev) || ev != (0xE1u | MH2IPC_KEY_DOWN))
            FAIL("the third event");
        Ipc_KeyTake();
        if (Ipc_KeyPeek(&ev))
            FAIL("an empty ring");
        for (n = 0; mh2ipc_keys_push(&H->keys, 0x05, 1); n++)
            ;
        if (n != MH2IPC_KEY_RING || H->keys.dropped != 1)
            FAIL("a full ring drops the new event and counts it");
        for (n = 0; Ipc_KeyPeek(&ev); n++)
            Ipc_KeyTake();
        if (n != MH2IPC_KEY_RING)
            FAIL("the full ring's events");
        H->host_flags &= ~MH2IPC_HOST_USB_KEYBOARD;
    }
    /* the pad schedule (Mh2IpcSched): port 0 by the core's VBlank, the other ports from the mailbox */
    {
        Mh2IpcSched *S = &H->sched;
        S->entries[0].vblank = 10;
        S->entries[0].pad.connected = 1;
        S->entries[0].pad.buttons = 0x4000;
        S->entries[0].pad.lx = 7;
        S->entries[1].vblank = 12;
        S->entries[1].pad.connected = 1;
        S->entries[1].pad.buttons = 0x2000;
        S->head = 2;
        S->horizon = 20;
        S->enabled = 1;
        Ipc_Pace(5);
        if (Ipc_ReadPad(0, &p) != 1 || p.buttons != 0 || p.lx != 0x80 || p.ly != 0x80)
            FAIL("the schedule: the neutral pad before its first entry");
        Ipc_Pace(10);
        if (Ipc_ReadPad(0, &p) != 1 || p.buttons != 0x4000 || p.lx != 7)
            FAIL("the schedule: an entry takes effect at its VBlank");
        Ipc_Pace(11);
        if (Ipc_ReadPad(0, &p) != 1 || p.buttons != 0x4000)
            FAIL("the schedule: an entry holds");
        Ipc_Pace(20);   /* the entry at 12, taken at once; the horizon (20) says none is missing */
        if (Ipc_ReadPad(0, &p) != 1 || p.buttons != 0x2000 || S->tail != 2)
            FAIL("the schedule: the next entry (tail %u)", S->tail);
        if (Ipc_ReadPad(1, &p) != 1 || p.buttons != 0x4008)
            FAIL("the schedule: the other ports come from the mailbox");
        S->enabled = 0;
        if (Ipc_ReadPad(0, &p) != 0)
            FAIL("the schedule off: the mailbox's port 0 (not connected)");
        Ipc_Pace(0);
    }
    Ipc_SetRumble(1, 5, 300);
    if (H->rumble[1] != (1u | 255u << 8))
        FAIL("the motors");
    if (Ipc_QuitRequested())
        FAIL("quit before the host asked");
    /* --quit-after 300: the start of VBlank 301 */
    H->quit_vblank_lo = 300;
    Ipc_Pace(300);
    if (Ipc_QuitRequested())
        FAIL("--quit-after: quit at VBlank N");
    Ipc_Pace(301);
    if (!Ipc_QuitRequested())
        FAIL("--quit-after: no quit at VBlank N + 1");
    H->quit_vblank_lo = 0;
    Ipc_Pace(0);
    H->quit_request = 1;
    if (!Ipc_QuitRequested())
        FAIL("the quit request");

    /* the samples: written whole, read in order, dropped when full */
    short s[2000];
    for (int i = 0; i < 2000; i++)
        s[i] = (short)(i * 7 - 3000);
    Ipc_AudioWrite(s, 1000);
    short back[2000];
    if (mh2ipc_stream_read(&H->audio, B, back, sizeof(back)) != 4000 || memcmp(back, s, 4000) != 0)
        FAIL("the samples");
    for (int i = 0; i < 70; i++)
        Ipc_AudioWrite(s, 1000);
    if (H->audio.dropped != 5 * 4000 || H->audio.head - H->audio.tail != 65000 * 4)
        FAIL("a full sample ring: dropped %u, queued %u", H->audio.dropped, H->audio.head - H->audio.tail);

    /* the GS stream, with a host */
#ifdef _WIN32
    HANDLE th = CreateThread(NULL, 0, host_thread, NULL, 0, NULL);
    if (!th)
        FAIL("CreateThread");
#else
    pthread_t th;
    if (pthread_create(&th, NULL, host_thread, NULL) != 0)
        FAIL("pthread_create");
#endif
    unsigned int words = 0, recs = 0, privs = 0, rbs = 0;
    unsigned long long vblank = 0, priv_sum = 0;
    static unsigned int rb[MH2IPC_RB_BYTES / 4 + 4096];
    while (words < 3u * (MH2IPC_GS_BYTES / 4)) {
        unsigned int q = rnd(32) == 0 ? MH2IPC_REC_MAX / 16 + rnd(4096) : 1 + rnd(rnd(4) ? 64 : 20000);
        if (q * 4 > sizeof(gif) / 4)
            q = sizeof(gif) / 16;
        for (unsigned int i = 0; i < q * 4; i++)
            gif[i] = (words + i) * 2654435761u;
        Ipc_GsGif(q > MH2IPC_REC_MAX / 16 ? 2 : 1 + recs % 3, gif, q);
        recs += (q + MH2IPC_REC_MAX / 16 - 1) / (MH2IPC_REC_MAX / 16);
        words += q * 4;
        if (rnd(3) == 0) {
            unsigned long long v = (unsigned long long)rnd(1u << 30) << 20;
            Ipc_GsPrivWrite(0x12000000u + (privs % 15) * 0x10, v);
            priv_sum += v;
            privs++;
        }
        if (rnd(4) == 0) {
            Ipc_MovieClock(vblank % 5 == 0 ? -1 : (int)vblank);
            Ipc_GsVSync((unsigned int)(vblank & 1), vblank);
            vblank++;
        }
        if (rnd(40) == 0) {
            /* sometimes larger than the buffer: two requests */
            unsigned int n = rnd(5) == 0 ? MH2IPC_RB_BYTES / 16 + 100 : 1 + rnd(5000);
            unsigned int first_seq = rbs + 1;
            if (Ipc_GsReadback(rb, n) != 1)
                FAIL("a readback with a renderer gave zeros");
            for (unsigned int i = 0; i < n * 4; i++) {
                unsigned int part = i / (MH2IPC_RB_BYTES / 4);
                unsigned int want = (first_seq + part) * 1000003u + (i - part * (MH2IPC_RB_BYTES / 4));
                if (rb[i] != want)
                    FAIL("readback %u: word %u", first_seq, i);
            }
            rbs += n > MH2IPC_RB_BYTES / 16 ? 2 : 1;
            if (seen.error[0])
                FAIL("the host: %s", seen.error);
        }
    }
    Ipc_MovieOpen(5, "/disc/mh2-disc.arc", 0x100002345ull, 777, 2, -30);
    {
        static const short pcm[8];
        Ipc_AudioWrite(pcm, 2);
        unsigned int begin = H->audio.head;
        Ipc_MoviePlay(5);
        if (H->movie_sound_id != 5 || H->movie_sound_begin != begin || H->movie_sound_end != ~0u)
            FAIL("the movie's sound window at play");
        Ipc_MovieClock(7);
        Ipc_GsVSync((unsigned int)(vblank & 1), vblank);
        vblank++;
        Ipc_AudioWrite(pcm, 2);
        Ipc_MovieClose(5);
        if (H->movie_sound_end != begin + 8)
            FAIL("the movie's sound window at the close");
    }
    Ipc_Exit(3);
#ifdef _WIN32
    WaitForSingleObject(th, INFINITE);
#else
    pthread_join(th, NULL);
#endif
    if (seen.error[0])
        FAIL("the host: %s", seen.error);
    if (seen.gif_words != words || seen.gif_recs != recs || seen.priv != privs || seen.priv_sum != priv_sum ||
        seen.vsync != vblank || seen.readbacks != rbs || seen.movie_open != 1 || seen.movie_close != 1 ||
        seen.last_movie != 7)
        FAIL("the host saw %u/%u GIF words in %u/%u records, %u/%u PRIV, %u/%u VSYNC, %u/%u readbacks", seen.gif_words,
             words, seen.gif_recs, recs, seen.priv, privs, seen.vsync, (unsigned int)vblank, seen.readbacks, rbs);
    if (H->core_state != MH2IPC_STATE_EXITED || H->core_exit_code != 3 || Ipc_Attached())
        FAIL("the exit");
    printf("ipc_protocol: OK (%u MB of GIF in %u records, %u VSyncs, %u readbacks)\n", words >> 18, recs,
           (unsigned int)vblank, rbs);
    mh2ipc_close(&hm);
    return 0;
}
