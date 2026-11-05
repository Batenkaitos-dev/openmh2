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
#include "hostfs.h"   /* the host's way to open a file: UTF-8 paths, and the C library's ANSI names an error */

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
    int launcher;              /* --launcher 1: show the launcher; --no-launcher -1: not (launcher.c) */
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
    /* Textures (off by default; the renderer's picture only): the textures the game draws are written to
     * <textures>/dump (--texture-dump, texture_dump = on) and replaced by the files in <textures>/packs
     * (--texture-packs, texture_packs = on). textures: the folder (--textures FOLDER, textures = FOLDER; default:
     * textures in the user data folder). */
    int texture_dump, texture_packs;
    /* The PC keyboard as the game's USB keyboard (keyboard.c; --usb-keyboard, usb_keyboard = on; off by default): the
     * game is told a keyboard is connected from its first request, and the window's keys reach it while typing. */
    int usb_keyboard;
    const char *textures;
    const char *states_dir;    /* the setting states: the folder of the save states (states.c) */
    const char *freecam_input; /* the setting freecam_game_input: neutral (default) or pad (freecam.c) */
    const char *freecam_speed; /* the setting freecam_speed: game units a second (default 400) */
    const char *freecam_pad;   /* the setting freecam_pad_toggle: on (default) or off (freecam.c) */
    const char *pictures_dir;  /* the setting pictures: the folder of photo mode's pictures (freecam.c) */
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
/* The windows' title: the project's own name, with the game named as what it runs (Capcom's name is its trademark). */
#define MH2_WINDOW_TITLE "OpenMH2 for Monster Hunter 2 (Dos)"
int launcher_disc(void);

/* cards.c: the launcher's memory cards page */
typedef struct CardInfo {
    char name[120];
    unsigned long long size;
    long long mtime;   /* seconds since 1970, 0: unknown */
    int in_use;        /* another mh2 has it open */
} CardInfo;
int cards_folder(const char *setting, char *out, size_t cap);   /* the cards folder, not made; -1 if none is known */
int cards_name_ok(const char *name);
int cards_list(const char *dir, CardInfo *out, int max, int sort_by_name);   /* the cards in it, the newest first (or by name) */
int cards_in_use(const char *dir, const char *name);
void cards_time_text(long long t, char *out, size_t cap);       /* 2026-10-30 21:14 */
int cards_old_count(const char *old1, const char *old2);        /* how many cards of the old version there are to copy */
int cards_import_old(const char *dir, const char *old1, const char *old2, int *copied, int *left);
void cards_default_name(const char *dir, char *out, size_t cap);
int cards_exists(const char *dir, const char *name);
const char *settings_archive(void);   /* the archive the settings name ("" if none) */
int launcher_disc_path(char *path, size_t cap);   /* 0: the archive found; -1: none, PATH is where it would be made */
int launcher_disc_present(void);   /* the archive is found (no side effects): the launcher shows only then */
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
/* The VBlank of the last VSync the GS thread handled: the game's, which a load of a save state sets back (the count of
 * VSyncs does not go back). Scripts and marks are by this. */
uint64_t gs_last_vblank(void);
/* The renderer's identity for a save state (build and the settings that change its state): "" without a renderer. */
void gs_state_identity(char *out, size_t cap);
int gs_request_photo(const char *path, uint64_t after);   /* photo mode: the first picture at or after VBLANK, written to PATH (UTF-8) */
int gs_photo_result(unsigned *w, unsigned *h);              /* 1 written, -1 failed, 0 nothing to report; taken once */

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
/* The key (an SDL scancode) and the pad button (an SDL_GamepadButton, or -1) bound to the pad's button NAME (up, down,
 * left, right, cross, circle, start, ...): the launcher's. 0, or -1 (no such button). */
int input_button(const char *name, int *key, int *pad);
void input_init(void);
void input_event(const SDL_Event *e);
/* Main thread: the current state into the mailbox, if it changed; the
 * motors from the core. */
void input_update(void);
void input_shutdown(void);
SDL_Gamepad *input_gamepad(void);   /* the open gamepad, or NULL */

/* keyboard.c: the PC keyboard as the game's USB keyboard (the keys, the typing toggle, the Japanese keys' bindings) */
int kbd_setting(const char *key, const char *value);   /* usb_keyboard_toggle, usb_key_NAME: 0, -1 (no such setting), -2 (no such key) */
const char *kbd_toggle_name(void);
const char *kbd_key_name(const char *key);   /* the key of usb_keyboard_toggle or usb_key_NAME, or "none" */
void kbd_start(Mh2IpcKeys *ring);
int kbd_event(const SDL_Event *e);   /* a window event: 1 when it was the toggle */
void kbd_update(void);               /* once a loop: a release a full ring dropped is made good */
int kbd_typing(void);
int kbd_toggle_scancode(void);       /* the toggle key's SDL scancode (0: none) */

/* states.c: save states, the host's side (the keys, the slots, the files, the messages) */
enum { STATE_ACT_SAVE = 1, STATE_ACT_LOAD, STATE_ACT_NEXT, STATE_ACT_PREV, STATE_ACT_SET };
int states_setting(const char *key, const char *value);   /* state_key_save/_load/_next/_prev: 0, -1 (not ours), -2 (no such key) */
const char *states_key_text(const char *key);             /* the key a setting names, as SDL names it, or "none" */
const char *states_key_name(int which);                   /* STATE_ACT_SAVE..PREV: the key's name, or "none" */
void states_action(int what, int arg);                    /* STATE_ACT_*; SET takes the slot, 1 to 10 */
int states_key(const SDL_Event *e);                       /* the main loop's key event: 1 when it was a state key */
void states_update(void);                                 /* each pass of the main loop: the core's answers */
/* freecam.c: the freecam's controller (the keys, the mouse, a gamepad, the pose in the IPC's block) */
int freecam_setting(const char *key, const char *value);   /* freecam_key, freecam_key_NAME: 0, -1 (not ours), -2 (no such key) */
const char *freecam_key_text(const char *key);             /* the key a setting names, as SDL names it, or "none" */
int freecam_key(const SDL_Event *e);                       /* the main loop's key event: 1 when it was the freecam's */
void freecam_event(const SDL_Event *e);                    /* the mouse's motion and wheel, the window's focus */
void freecam_update(void);                                 /* each pass of the main loop: the pose, into the block */
void freecam_pad_filter(uint16_t *buttons);                /* input.c: the gamepad's L3 and R3 together toggle the freecam and are kept from the game */
int freecam_active(void);                                  /* the freecam is on */
int freecam_controls(void);                                /* on, and not held, and the keyboard is not typing: the keys are its */
int freecam_game_pad(void);                                /* freecam_game_input = pad: the gamepad still plays */
#if !defined(MH2_RELEASE) || defined(MH2_FREECAM_TEST)
int freecam_script_cmd(const char *tok, const char *rest); /* input.c's --pad-script: FCTOGGLE@V ...; 0 taken, -1 bad, -2 not ours */
#endif
/* present.c: a note over the picture (and in the window title) for a few seconds */
void present_note(const char *msg);
void present_set_freecam(const char *line, const char *title);   /* the freecam's line over the picture and its words in the title (NULL: none) */
void present_relative_mouse(int on);          /* the mouse looks (relative mode) or is the pointer again */

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
int movie_active(void);   /* a movie is open (its pictures or its sound are on): no save state then */
/* The audio thread: mixes the movie's sound into `frames` frames read from
 * the sample ring at byte position pos. */
void movie_mix(int16_t *out, unsigned int frames, unsigned int pos);
extern int g_movie_sound;   /* a sound device plays the samples */
/* audio.c */
int audio_init(void);
void audio_shutdown(void);
void audio_set_muted(int on);   /* the pause: fade the game's sound out, or back in (audio.c) */
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

/* present.c: the window title and a line over the picture say the keyboard is typing (kbd_event) */
void present_set_typing(int on, const char *toggle_key);


#endif
