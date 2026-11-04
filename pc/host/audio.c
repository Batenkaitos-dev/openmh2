/*
 * The sound device: SDL pulls 48 kHz stereo from the sample ring as it
 * plays. The core makes the samples in step with its VBlanks, paced to real
 * time; the ring absorbs the difference. When the ring holds more than
 * MAX_MS (the device fell behind, or the host stalled), the oldest samples
 * beyond TARGET_MS are dropped; when it runs dry the device plays silence.
 * A movie's sound is mixed in as the samples are read (movie.c).
 * Nothing here reaches the game: audio_queued is for the core's pacing.
 * --audio-dump PREFIX writes the core's frames and the device's (mh2host.h).
 * The last minute of the device's sound is kept for the player's marks (play.c).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mh2host.h"

#define TARGET_MS 60
#define MAX_MS 250

static struct {
    SDL_AudioStream *stream;
    unsigned long long underruns, dropped;
    unsigned long long n_core, n_skipped, n_out, n_silent, n_mixed;
    unsigned long long n_silent_late, late_underruns;   /* after the core's first frame */
    /* steady play's underruns: those from
     * the core's first samples to STEADY_END_MS before its last, by their times (SDL_GetTicks) */
    Uint64 first_ms, last_ms;         /* the first and last pull that had the core's samples */
    Uint64 late_ms[4096];             /* the underruns after first_ms (the first 4096) */
    unsigned int n_late;
} au;

#define STEADY_END_MS 1500



static void SDLCALL pull(void *user, SDL_AudioStream *stream, int additional, int total)
{
    (void)user;
    (void)total;
    static unsigned char buf[16384] __attribute__((aligned(4)));
    Mh2IpcRing *r = &g_ipc->audio;
    unsigned int queued = mh2ipc_load(&r->head) - r->tail;
    unsigned int max = MH2IPC_AUDIO_RATE * MAX_MS / 1000 * 4, target = MH2IPC_AUDIO_RATE * TARGET_MS / 1000 * 4;
    if (queued > max) {
        unsigned int drop = queued - target;
        while (drop) {
            unsigned int n = drop < sizeof(buf) ? drop : (unsigned int)sizeof(buf);
            unsigned int got = mh2ipc_stream_read(r, g_block, buf, n);
            drop -= got;
        }
        au.dropped++;
    }
    while (additional > 0) {
        unsigned int want = (unsigned int)additional < sizeof(buf) ? (unsigned int)additional & ~3u : sizeof(buf);
        if (!want)
            want = 4;
        unsigned int pos = r->tail;
        unsigned int got = mh2ipc_stream_read(r, g_block, buf, want);
        movie_mix((int16_t *)buf, got / 4, pos);   /* the movie's sound, by position (movie.c) */
        if (got) {
            au.last_ms = SDL_GetTicks();
            if (!au.first_ms)
                au.first_ms = au.last_ms;
        }
        if (got < want) {
            memset(buf + got, 0, want - got);
            au.underruns++;
            if (au.first_ms && au.n_late < sizeof(au.late_ms) / sizeof(au.late_ms[0]))
                au.late_ms[au.n_late++] = SDL_GetTicks();
            au.n_silent += (want - got) / 4;
            if (au.n_core) {
                au.n_silent_late += (want - got) / 4;
                au.late_underruns++;
            }
        }
        SDL_PutAudioStreamData(stream, buf, (int)want);
        additional -= (int)want;
    }
    mh2ipc_store(&g_ipc->audio_queued, (mh2ipc_load(&r->head) - r->tail) / 4);
}

int audio_init(void)
{
    if (g_cfg.mute) {
        host_log("sound off (--mute)");
        return -1;
    }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        host_log("no sound: %s", SDL_GetError());
        return -1;
    }
    SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, (int)MH2IPC_AUDIO_RATE};
    au.stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, pull, NULL);
    if (!au.stream) {
        host_log("no sound device: %s", SDL_GetError());
        return -1;
    }
    g_movie_sound = 1;
    SDL_ResumeAudioStreamDevice(au.stream);
    host_log("sound: %s", SDL_GetCurrentAudioDriver());
    return 0;
}

void audio_shutdown(void)
{
    if (au.stream) {
        SDL_DestroyAudioStream(au.stream);
        unsigned int steady = 0;
        for (unsigned int i = 0; i < au.n_late; i++)
            steady += au.late_ms[i] + STEADY_END_MS < au.last_ms;
        host_log("sound: %llu underruns, %llu catch-ups; in steady play (from the core's first samples to %d ms "
                 "before its last, %.1f s) %u underruns%s",
                 au.underruns, au.dropped, STEADY_END_MS,
                 au.last_ms > au.first_ms + STEADY_END_MS ? (double)(au.last_ms - au.first_ms - STEADY_END_MS) / 1000.0 : 0.0,
                 steady, au.n_late == sizeof(au.late_ms) / sizeof(au.late_ms[0]) ? " or more" : "");
    }
    au.stream = NULL;
}

