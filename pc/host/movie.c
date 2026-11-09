/*
 * Movies in the host: the core keeps a movie's timing (pc/core/ipc/ipc_movie.c)
 * and tells the host what to show and when to sound; mh2movie
 * (pc/movie, FFmpeg, loaded at run time as the original game loads it)
 * decodes the stream on its own thread, as the original's movie.c
 * does:
 *  - MOVIE_OPEN (the GS thread): the stream's place in the archive, the
 *    display mode and the game's movie volume; the decoder starts.
 *  - Each VSYNC carries the movie clock (48 kHz frames since the movie
 *    began playing) or none. At the game's flips (the VSync that follows
 *    the GS's display registers being written: the original game draws the
 *    player's current frame once a step, and shows it at the step's flip)
 *    the picture is chosen: frame 0 and every frame whose time is at most
 *    clock / 48000 s plus half a frame's, in order, on a canvas the size of
 *    the mode's screen, in place of the GS's picture. The half frame keeps
 *    the choice off the tie of a 29.97 Hz movie against two-VBlank flips
 *    (the clock, the sound's output frames counted at each VBlank, wanders by
 *    some tens of frames), so each picture holds two VSyncs; it shows a
 *    picture at most half a frame ahead of its sound. Where no flip comes in
 *    four VSyncs the choice is made anyway. The GS thread waits for the
 *    decoder's next frame, so the pictures follow the clock (--deterministic)
 *    at any decoding speed.
 *  - The sound: the samples between the header's movie_sound_begin and
 *    movie_sound_end (the sample ring's bytes) get the movie's sound mixed in
 *    at the game's volume, as the original's movie_mix does into the
 *    SPU2's output: out = clamp(out + lrintf(movie x gain)), where the
 *    original's thread runs in the round-toward-zero FPU mode: the gain's
 *    conversion to float, the float product and lrintf all round toward zero
 *    (its --wav, on Linux and on Windows alike). Here the same results in
 *    any mode: the product exactly in double, rounded toward zero to float
 *    (rz_float), then a plain conversion, which truncates. The audio
 *    thread mixes by position, so frames the device skips (catch-ups) keep
 *    it in step; where the decoder hasn't caught up (or opened the movie
 *    yet: the GS thread is behind), the movie's sound comes
 *    later (silence first), never skipped, as in the original game.
 *  - MOVIE_CLOSE: the picture stops; the decoder is freed once the sound
 *    device has played past the sound's end.
 * Without mh2movie (or FFmpeg) movies are not shown or heard; the game runs
 * the same, its timing being the core's.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "mh2host.h"
#include "movielib.h"

#define NFRAMES 6
#define RING_FRAMES (48000 * 4) /* the sound decoded ahead: 4 s */
#define MAX_IDLE 4              /* VSyncs the picture may stay without a flip */

static struct {
    int tried, ok;
    SDL_SharedObject *so;
    mh2movie_open_fn open;
    mh2movie_next_frame_fn next_frame;
    mh2movie_audio_fn audio;
    mh2movie_close_fn close;
} lib;

static struct {
    SDL_Mutex *mu;          /* the movie's state against the audio thread; with cv, the frame queue */
    SDL_Condition *cv;
    int active;             /* a decoder exists */
    int closing;            /* MOVIE_CLOSE seen: freed when the sound has played out */
    unsigned int id;
    mh2movie *m;
    mh2movie_info info;
    SDL_Thread *thread;
    SDL_AtomicInt quit, done;
    uint8_t *frame[NFRAMES];
    double pts[NFRAMES];
    int qhead, qn;
    uint8_t *canvas;        /* the screen: the last frame shown */
    int have_shown;
    unsigned int shown;     /* frames shown (the log) */
    unsigned int idle;      /* VSyncs since the picture was last chosen */
    double lead_min, lead_max;  /* the clock minus a shown frame's time, seconds (the log) */
    double half;            /* half a frame's time: the choice's margin */
    int cw, ch, px, py;
    int16_t *ring;          /* the sound: the decoder writes, the audio thread reads */
    SDL_AtomicInt ring_r, ring_w;
    unsigned int taken;     /* the movie's sound frames taken (mixed or skipped) */
    unsigned int delay;     /* frames the movie's sound runs late: the ring was dry (the original's dry ring) */
    float gain;
} mv;

/* The frames of movie `early_id`'s sound that passed before the GS thread had opened it: a delay,
 * as a dry ring's, never a skip (the original's movie plays from its first sample). The ids are the core's count of
 * movies opened, which a load of a save state sets back: a movie's id can come again, so the count is forgotten when
 * the movie closes. */
static unsigned int early_id, early_frames;

int g_movie_sound;   /* audio.c: a sound device takes the samples (else the decoder drops the sound) */

/* The display modes (the descriptor's +0xE): 512x448; 256x224; a 512x432 picture on the 448-line screen. */
static const struct {
    int w, h;
} k_modes[] = {{512, 448}, {256, 224}, {512, 448}};

static int load_lib(void)
{
    if (lib.tried)
        return lib.ok;
    lib.tried = 1;
    char path[1200], why[300];
    MovieLib ml;
    int rc = movielib_load(&ml, NULL, path, sizeof(path), why, sizeof(why));
    if (rc == MOVIELIB_NOLOAD) {
        host_log("movies: cannot load %s (%s): not shown", path, why);
        return 0;
    }
    if (rc != MOVIELIB_OK) {
        host_log("movies: %s is from another version: not shown", path);
        return 0;
    }
    lib.so = ml.so;
    lib.open = ml.open;
    lib.next_frame = ml.next_frame;
    lib.audio = ml.audio;
    lib.close = ml.close;
    lib.ok = 1;
    return 1;
}

int movie_init(void)
{
    mv.mu = SDL_CreateMutex();
    mv.cv = SDL_CreateCondition();
    if (!mv.mu || !mv.cv || !load_lib())
        return -1;
    return 0;
}

/* ------------------------------------------------------------ the decoder thread */

static void ring_push(const int16_t *pcm, int n)
{
    while (n > 0 && !SDL_GetAtomicInt(&mv.quit)) {
        uint32_t w = (uint32_t)SDL_GetAtomicInt(&mv.ring_w), r = (uint32_t)SDL_GetAtomicInt(&mv.ring_r);
        uint32_t room = RING_FRAMES - (w - r);
        if (!room) {
            SDL_Delay(2);   /* the sound device drains it in real time */
            continue;
        }
        uint32_t k = (uint32_t)n < room ? (uint32_t)n : room;
        for (uint32_t i = 0; i < k; i++) {
            uint32_t at = (w + i) % RING_FRAMES;
            mv.ring[at * 2] = pcm[i * 2];
            mv.ring[at * 2 + 1] = pcm[i * 2 + 1];
        }
        SDL_SetAtomicInt(&mv.ring_w, (int)(w + k));
        pcm += k * 2;
        n -= (int)k;
    }
}

static int SDLCALL worker(void *arg)
{
    (void)arg;
    static int16_t pcm[4096 * 2];
    for (;;) {
        SDL_LockMutex(mv.mu);
        while (mv.qn == NFRAMES && !SDL_GetAtomicInt(&mv.quit))
            SDL_WaitCondition(mv.cv, mv.mu);
        int slot = (mv.qhead + mv.qn) % NFRAMES;
        SDL_UnlockMutex(mv.mu);
        if (SDL_GetAtomicInt(&mv.quit))
            break;
        double pts = 0;
        int r = lib.next_frame(mv.m, mv.frame[slot], &pts);
        for (int n; (n = lib.audio(mv.m, pcm, 4096)) > 0;)
            if (g_movie_sound)
                ring_push(pcm, n);
        if (r <= 0) {
            if (r < 0)
                host_log("movie %u: a decoding error", mv.id);
            break;
        }
        SDL_LockMutex(mv.mu);
        mv.pts[slot] = pts;
        mv.qn++;
        SDL_BroadcastCondition(mv.cv);
        SDL_UnlockMutex(mv.mu);
    }
    SDL_LockMutex(mv.mu);
    SDL_SetAtomicInt(&mv.done, 1);
    SDL_BroadcastCondition(mv.cv);
    SDL_UnlockMutex(mv.mu);
    return 0;
}

/* ------------------------------------------------------------ the GS thread */

static void movie_free(void)
{
    if (!mv.active)
        return;
    SDL_SetAtomicInt(&mv.quit, 1);
    SDL_LockMutex(mv.mu);
    SDL_BroadcastCondition(mv.cv);
    SDL_UnlockMutex(mv.mu);
    if (mv.thread)
        SDL_WaitThread(mv.thread, NULL);
    SDL_LockMutex(mv.mu);   /* the audio thread is not mixing it */
    host_log("movie %u: %u frames shown, %u sound frames mixed, %u frames late (the decoder)", mv.id, mv.shown, mv.taken,
             mv.delay);
    if (mv.shown > 1)
        host_log("movie %u: pictures shown from %.1f ms before to %.1f ms after their time on the sound's clock", mv.id,
                 -mv.lead_min * 1e3, mv.lead_max * 1e3);
    mv.thread = NULL;
    if (mv.m)
        lib.close(mv.m);
    mv.m = NULL;
    for (int i = 0; i < NFRAMES; i++) {
        SDL_free(mv.frame[i]);
        mv.frame[i] = NULL;
    }
    SDL_free(mv.canvas);
    SDL_free(mv.ring);
    mv.canvas = NULL;
    mv.ring = NULL;
    mv.active = mv.closing = 0;
    SDL_UnlockMutex(mv.mu);
}

/* A closed movie is freed once its sound has been played (or never began). */
static void movie_reap(void)
{
    if (!mv.active || !mv.closing)
        return;
    unsigned int end = mh2ipc_load(&g_ipc->movie_sound_end);
    unsigned int tail = mh2ipc_load(&g_ipc->audio.tail);
    if (!g_movie_sound || mh2ipc_load(&g_ipc->movie_sound_id) != mv.id || end == ~0u || (int)(tail - end) >= 0)
        movie_free();
}

/* x rounded to float toward zero, whatever the FPU mode (the original's thread's rounding) */
static float rz_float(double x)
{
    float f = (float)x;
    if (fabs((double)f) > fabs(x))
        f = nextafterf(f, 0.0f);
    return f;
}

static float gain_of(int v)
{
    if (v < -960)
        v = -960;
    return v <= -960 ? 0.0f : rz_float(pow(10.0, v / 200.0));
}

void movie_open(const Mh2IpcMovieOpen *o, const char *path)
{
    movie_free();
    if (!lib.ok)
        return;
    char err[256] = "";
    uint64_t offset = (uint64_t)o->offset_hi << 32 | o->offset_lo;
    mh2movie *m = lib.open(path, offset, o->size, &mv.info, err, (int)sizeof(err));
    if (!m) {
        host_log("movie %u: %s", o->id, err);
        return;
    }
    unsigned int mode = o->mode < 3 ? o->mode : 0;
    mv.m = m;
    mv.id = o->id;
    mv.cw = k_modes[mode].w > mv.info.width ? k_modes[mode].w : mv.info.width;
    mv.ch = k_modes[mode].h > mv.info.height ? k_modes[mode].h : mv.info.height;
    mv.px = (mv.cw - mv.info.width) / 2;
    mv.py = (mv.ch - mv.info.height) / 2;
    size_t fb = (size_t)mv.info.width * (size_t)mv.info.height * 4;
    int ok = 1;
    for (int i = 0; i < NFRAMES; i++)
        ok &= (mv.frame[i] = SDL_malloc(fb)) != NULL;
    mv.canvas = SDL_malloc((size_t)mv.cw * (size_t)mv.ch * 4);
    mv.ring = SDL_malloc((size_t)RING_FRAMES * 4);
    mv.active = 1;
    if (!ok || !mv.canvas || !mv.ring) {
        host_log("movie %u: out of memory", mv.id);
        movie_free();
        return;
    }
    for (size_t i = 0; i < (size_t)mv.cw * (size_t)mv.ch; i++)
        memcpy(mv.canvas + i * 4, "\0\0\0\xff", 4);   /* opaque black */
    mv.qhead = mv.qn = 0;
    mv.have_shown = 0;
    mv.shown = 0;
    mv.idle = 0;
    mv.lead_min = mv.lead_max = 0;
    mv.half = mv.info.fps > 1.0 ? 0.5 / mv.info.fps : 0.0;
    mv.taken = 0;
    mv.delay = 0;
    mv.closing = 0;
    mv.gain = gain_of(o->volume);
    SDL_SetAtomicInt(&mv.ring_r, 0);
    SDL_SetAtomicInt(&mv.ring_w, 0);
    SDL_SetAtomicInt(&mv.quit, 0);
    SDL_SetAtomicInt(&mv.done, 0);
    mv.thread = SDL_CreateThread(worker, "movie", NULL);
    if (!mv.thread) {
        host_log("movie %u: no decoder thread (%s)", mv.id, SDL_GetError());
        movie_free();
        return;
    }
    host_log("movie %u: %dx%d, %.2f fps, %.1f s, volume %d (gain %.9g)", mv.id, mv.info.width, mv.info.height,
             mv.info.fps, mv.info.duration, (int)o->volume, (double)mv.gain);
}

int movie_active(void)
{
    return mv.active && !mv.closing;
}

void movie_close(unsigned int id)
{
    if (mv.mu) {
        SDL_LockMutex(mv.mu);   /* the audio thread reads the early count */
        if (early_id == id)
            early_id = early_frames = 0;
        SDL_UnlockMutex(mv.mu);
    }
    if (mv.active && mv.id == id) {
        mv.closing = 1;
        movie_reap();
    }
}

/* Wait (mutex held) for a frame or the decoder's end; not while the sound ring is full (the decoder
 * then waits for the sound device). The original's det_wait_frame. */
static void wait_frame(void)
{
    while (mv.qn == 0 && !SDL_GetAtomicInt(&mv.done) &&
           (uint32_t)SDL_GetAtomicInt(&mv.ring_w) - (uint32_t)SDL_GetAtomicInt(&mv.ring_r) < RING_FRAMES - 8192)
        SDL_WaitConditionTimeout(mv.cv, mv.mu, 10);
}

int movie_vsync(unsigned int clock_plus1, int flip, const uint8_t **pixels, uint32_t *w, uint32_t *h)
{
    movie_reap();
    if (!mv.active || mv.closing || !clock_plus1)
        return 0;
    double t = (double)(clock_plus1 - 1) / 48000.0;
    int choose = flip || ++mv.idle >= MAX_IDLE;   /* the picture is chosen at the game's flip (or when none comes) */
    if (choose) {
        mv.idle = 0;
        t += mv.half;
    }
    SDL_LockMutex(mv.mu);
    while (choose) {
        wait_frame();
        if (!(mv.qn > 0 && (!mv.have_shown || mv.pts[mv.qhead] <= t)))
            break;
        if (mv.have_shown) {
            double lead = (double)(clock_plus1 - 1) / 48000.0 - mv.pts[mv.qhead];
            if (mv.shown == 1 || lead < mv.lead_min)
                mv.lead_min = lead;
            if (mv.shown == 1 || lead > mv.lead_max)
                mv.lead_max = lead;
        }
        size_t row = (size_t)mv.info.width * 4;
        for (int y = 0; y < mv.info.height; y++)
            memcpy(mv.canvas + ((size_t)(mv.py + y) * (size_t)mv.cw + (size_t)mv.px) * 4,
                   mv.frame[mv.qhead] + (size_t)y * row, row);
        mv.have_shown = 1;
        mv.shown++;
        mv.qhead = (mv.qhead + 1) % NFRAMES;
        mv.qn--;
        SDL_BroadcastCondition(mv.cv);
    }
    SDL_UnlockMutex(mv.mu);
    if (!mv.have_shown)
        return 0;
    *pixels = mv.canvas;
    *w = (uint32_t)mv.cw;
    *h = (uint32_t)mv.ch;
    return 1;
}

void movie_shutdown(void)
{
    movie_free();
    if (lib.so)
        SDL_UnloadObject(lib.so);
    lib.so = NULL;
    lib.ok = 0;
}

/* ------------------------------------------------------------ the audio thread */


void movie_mix(int16_t *out, unsigned int frames, unsigned int pos)
{
    unsigned int id = mh2ipc_load(&g_ipc->movie_sound_id);
    unsigned int begin = mh2ipc_load(&g_ipc->movie_sound_begin);
    unsigned int end = mh2ipc_load(&g_ipc->movie_sound_end);
    if (!id || !mv.mu)
        return;
    SDL_LockMutex(mv.mu);
    if (!(mv.active && mv.id == id && mv.ring)) {   /* not open yet: count its frames that pass */
        for (unsigned int i = 0; i < frames; i++, pos += 4) {
            if ((int)(pos - begin) < 0 || (end != ~0u && (int)(pos - end) >= 0))
                continue;
            if (early_id != id)
                early_id = id, early_frames = 0;
            early_frames = (pos - begin) / 4 + 1;
        }
    } else {
        uint32_t r = (uint32_t)SDL_GetAtomicInt(&mv.ring_r), w = (uint32_t)SDL_GetAtomicInt(&mv.ring_w);
        if (early_id == id && mv.taken == 0 && mv.delay < early_frames)
            mv.delay = early_frames;
        for (unsigned int i = 0; i < frames; i++, pos += 4) {
            if ((int)(pos - begin) < 0 || (end != ~0u && (int)(pos - end) >= 0))
                continue;
            unsigned int at_pos = (pos - begin) / 4;   /* this frame's place from the sound's start */
            unsigned int k = at_pos - mv.delay;          /* the movie's sound frame due here */
            while (mv.taken < k && r != w) {      /* frames the device skipped (catch-ups): skip theirs */
                r++;
                mv.taken++;
            }
            if (r == w) {   /* not decoded yet: silence here, and the movie's sound comes that much
                             * later, as the original's movie_mix (its dry ring delays, never skips) */
                mv.delay = at_pos + 1 - mv.taken;
                continue;
            }
            uint32_t at = r % RING_FRAMES;
            for (int c = 0; c < 2; c++) {
                int v = out[i * 2 + c] + (int)rz_float((double)mv.ring[at * 2 + c] * mv.gain);   /* exact, then toward zero */
                out[i * 2 + c] = (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
            }
            r++;
            mv.taken++;
        }
        SDL_SetAtomicInt(&mv.ring_r, (int)r);
    }
    SDL_UnlockMutex(mv.mu);
}
