/*
 * mh2, the 64-bit host: the window and the
 * presentation, the GS renderer (mh2gs), the sound device and the input,
 * around mh2-core, which it starts and talks to through pc/ipc/mh2ipc.h.
 * Nothing here is game-visible except the input and the readbacks, which
 * the core records.
 *
 * The modules:
 *   main.c      the options, the IPC block, the main loop (SDL's events)
 *   process.c   starting, watching and stopping mh2-core
 *   gs.c        the GS thread: the GS ring into mh2gs, readbacks, pictures
 *   present.c   the window and the presentation (SDL's renderer)
 *   input.c     the pads and the keyboard into the input mailbox
 *   audio.c     the sample ring into the sound device
 *   movie.c     the movies (mh2movie): their pictures and sound
 *   play.c      the session's files: the log and the marks
 */
#ifndef MH2HOST_H
#define MH2HOST_H

#include <stdint.h>

#include <SDL3/SDL.h>

#include "ipc/mh2ipc.h"
#include "ipc/mh2ipc_os.h"

typedef struct HostConfig {
    const char *core_path;     /* --core */
    char **core_args;          /* after --: passed to the core */
    int core_nargs;
    int no_window;             /* --no-window: no video (tests, WSL) */
    int gs_off;                /* --gs-off: no renderer */
    int mute;                  /* --mute */
    int fullscreen;            /* --fullscreen */
    int fast;                  /* --fast: the core runs as fast as it can (no real-time pacing) */
    int no_card;               /* --no-card: no default card for port 1 (tests: no user folder touched) */
    uint64_t quit_after;       /* --quit-after N: the core ends after N VSyncs (tests, Mh2IpcHeader) */
    const char *config_path;   /* --config FILE: the settings file (settings.c) */
    const char *archive;       /* --archive FILE: the disc archive (as the setting archive) */
    const char *disc_path;     /* the archive the core is given (launcher_disc): its MH2_DISC */
    const char *memcard[2];    /* --memcard1/2 FILE: the core's cards (before the settings' memcard1/2) */
    int vsync;                 /* --vsync, the setting vsync: -1 auto (on in fullscreen), 0 off, 1 on */
    const char *log_file;      /* --log-file FILE: the log (play.c; in play, by default) */
    const char *import_card;   /* --import-card FILE: copied into mh2's cards folder (cards.c), then mh2 ends */
    const char *cards_dir;     /* the setting cards: mh2's cards folder (cards.c) */
    const char *card[2];       /* the settings card1, card2: the cards' names in that folder */
    const char *old_card[2];   /* the settings memcard1, memcard2: the old version's cards (import only) */
    /* An automated run (main.c, the guard): the first option that makes it one (a test's, a pad
     * script, the core's own arguments), else NULL (the player's). It
     * never touches the user data folder: it names its cards folder and any other it needs, or stops. */
    const char *automated;
    /* The optional enhancements (off by default; they change the host's picture only, never the game's
     * state or the GS stream: PLAYING.md, "Enhancements") */
    int ssaa;                  /* --ssaa N, ssaa = N: samples per pixel, 2, 4, 8 or 16; 0 or 1: off */
    int hires;                 /* --hires, hires = on: a picture of twice the width and height (implies ssaa 4) */
    int ssaa_asked, hires_asked; /* the renderer could not make the samples (gs.c): what was asked; ssaa and hires then read off */
    /* Widescreen: unlike the two above it changes the game's state (the camera's aspect and the 2D picture's
     * shape), so it is the core's own option (--widescreen) and the host shows the 640x448 picture at 16:9, movies
     * at 4:3. Off by default. */
    int widescreen;            /* 0 off, 2 on (--widescreen, widescreen = on) */
} HostConfig;

extern HostConfig g_cfg;
extern Mh2IpcMap g_map;
extern Mh2IpcHeader *g_ipc;
extern unsigned char *g_block;

void host_log(const char *fmt, ...) SDL_PRINTF_VARARG_FUNC(1);

/* process.c */
int process_start(const char *path, char **args, int nargs);
/* 1 while it runs; when it has ended, 0 and its exit status in *code
 * (a signal: 128 + the signal). */
int process_running(int *code);
void process_kill(void);
/* The core next to this executable, or the build tree's. */
const char *process_default_core(void);

/* settings.c: mh2.ini and the launcher */
void settings_load(const char *explicit_path);
void settings_set(const char *line);   /* --set KEY=VALUE: a settings line, over the file's */
void settings_apply(int cli_fullscreen, int cli_mute);
void settings_log(void);
/* The enhancements in force, as one line ("off", or "ssaa 4, hires on"): the log's and the summary's note of them. */
const char *settings_enhancements(void);
/* widescreen: "on" or "off" (the setting that is recorded) */
const char *settings_widescreen(void);
void launcher_core_arg(const char *name, const char *value);
void launcher_core_flag(const char *name);   /* NAME alone, before the core's own arguments */
void launcher_widescreen(void);   /* the core's --widescreen when it is on (settings.c) */
int launcher_disc(void);
/* cards.c: mh2's own cards (its folder, the first start's import, the locks) */
void cards_choose(int play, const char *card1, const char *card2, const char *old1, const char *old2,
                  const char *setting);
int cards_import(const char *file, const char *setting);   /* --import-card: the exit status */
int cards_lock(char *msg, size_t cap);

/* gs.c */
/* Before the core starts: loads mh2gs, starts the GS thread, which makes
 * the renderer and then reads the ring. 0 = a renderer, -1 = none (the
 * thread runs without), -2 = no thread. */
int gs_init(void);
void gs_stop(void);             /* asks it to end, waits */
uint64_t gs_vsyncs(void);

/* present.c */
int present_init(void);
/* Thread-safe: the picture to show next (RGBA8). Wakes the main loop. */
/* MOVIE: a movie picture (shown at 4:3 under widescreen); else the game's. */
void present_submit(const void *pixels, uint32_t width, uint32_t height, uint32_t stride, uint64_t vblank, int movie);
/* Main thread: shows the picture submitted last, if new. */
void present_draw(void);
void present_toggle_fullscreen(void);
void present_shutdown(void);
extern uint32_t g_present_event;   /* the SDL event type present_submit pushes */

/* input.c */
int input_bind(const char *key, const char *value);
void input_init(void);
void input_event(const SDL_Event *e);
/* Main thread: the current state into the mailbox, if it changed; the
 * motors from the core. */
void input_update(void);
void input_shutdown(void);

/* movie.c (movies: pc/core/ipc/ipc_movie.c keeps their timing) */
/* Loads mh2movie (beside mh2): 0, or -1 (movies not shown or heard). */
int movie_init(void);
/* The GS thread: MOVIE_OPEN and MOVIE_CLOSE; at each VSYNC, its arg1 (the
 * movie clock + 1, or 0): 1 with the movie's picture to show instead of
 * the GS's, or 0. */
void movie_open(const Mh2IpcMovieOpen *o, const char *path);
void movie_close(unsigned int id);
int movie_vsync(unsigned int clock_plus1, const uint8_t **pixels, uint32_t *w, uint32_t *h);
void movie_shutdown(void);
/* The audio thread: mixes the movie's sound into `frames` frames read from
 * the sample ring at byte position pos. */
void movie_mix(int16_t *out, unsigned int frames, unsigned int pos);
extern int g_movie_sound;   /* a sound device plays the samples */
/* audio.c */
int audio_init(void);
void audio_shutdown(void);
/* play.c */
const char *play_data_dir(void);   /* the user data folder, ending in a separator; NULL if none */
const char *play_stamp(void);      /* session_YYYYMMDD-HHMMSS: the session's start */
/* mh2's standard output and error, and the core's, into the log: PATH if given, else
 * DATA/mh2.log (rotated). 0, or -1 (no log file: they stay where they were). */
int play_log_open(const char *path);
void play_console_detach(void);   /* play on Windows: lets go of a console that is mh2's alone */
void play_log_close(void);         /* at the end, after the core: drains and closes the log */
void *play_log_handle(void);       /* Windows: the log pipe's handle for the core (process.c); else NULL */
const char *play_log_path(void);   /* the log file, or NULL */
void present_leave_fullscreen(void);

/* crash.c: crash dumps (Windows): DIR\\SESSION.mh2-crash.dmp for mh2, MH2_CRASH_DUMP for the core */
void crash_setup(const char *dir, const char *session);


#endif
