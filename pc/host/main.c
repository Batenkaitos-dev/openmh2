/*
 * mh2: the host's entry point and main loop (mh2host.h).
 *
 *   mh2 [--config FILE.ini] [--set KEY=VALUE] [--archive FILE] [--core PATH] [--no-window]
 *       [--gs-off] [--mute] [--fullscreen] [--quit-after N]
 *       [--memcard1 FILE] [--memcard2 FILE] [--import-card FILE] [--vsync auto|on|off]
 *       [--log-file FILE] [--ssaa N] [--hires] [--widescreen] [-- CORE ARGS...]
 *
 * In play (a window, and neither --quit-after nor --screenshot), each
 * session is logged to a file (play.c says where).
 *
 * An automated run (a test's options, the core's own arguments: g_auto_opts) never touches the user
 * data folder, which holds the player's cards and settings: it names its own cards folder, or stops
 * at once (guard).
 *
 * It reads the settings (mh2.ini, settings.c), finds the disc archive and
 * the cards for the core (the launcher), makes the IPC block, loads the
 * renderer, opens the window and the sound device, starts mh2-core with
 * MH2_IPC naming the block, and runs SDL's events until the core ends. Its
 * exit status is the core's.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core_args.h"
#include "launcher.h"
#include "launcher_make.h"
#include "launcher_text.h"
#include "mh2host.h"
#include "settings.h"
#include "textpack.h"

HostConfig g_cfg;
Mh2IpcMap g_map;
Mh2IpcHeader *g_ipc;
unsigned char *g_block;

/* One write a line, so that the core's lines in the log (play.c) never split one of mh2's. */
void host_log(const char *fmt, ...)
{
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line + 5, sizeof(line) - 6, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n > (int)sizeof(line) - 7)
        n = (int)sizeof(line) - 7;
    memcpy(line, "mh2: ", 5);
    line[5 + n] = '\n';
    fwrite(line, 1, (size_t)n + 6, stderr);
}

/* The options that are not settings (the settings' own are in settings.c's table), for the usage line and --help. */
typedef struct Opt {
    const char *opt, *arg;
    int text;
    int advanced;   /* listed under "Advanced" in --help: for tests and automation */
} Opt;

static const Opt g_other[] = {
    {"--config", "FILE.ini", T_O_CONFIG, 0},
    {"--set", "KEY=VALUE", T_O_SET, 0},
    {"--core", "PATH", T_O_CORE, 0},
    {"--no-window", NULL, T_O_NOWINDOW, 1},
    {"--gs-off", NULL, T_O_GSOFF, 1},
    {"--fast", NULL, T_O_FAST, 1},
    {"--no-card", NULL, T_O_NOCARD, 1},
    {"--quit-after", "N", T_O_QUITAFTER, 1},
    {"--import-card", "FILE", T_O_IMPORTCARD, 0},
    {"--log-file", "FILE", T_O_LOGFILE, 0},
    {"--launcher", NULL, T_O_LAUNCHER, 0},
    {"--no-launcher", NULL, T_O_NOLAUNCHER, 0},
    {"--help", NULL, T_O_HELP, 0},
};

static void usage(void)
{
    fprintf(stderr, "usage: mh2");
    for (size_t k = 0; k < sizeof(g_other) / sizeof(g_other[0]); k++)
        fprintf(stderr, " [%s%s%s]", g_other[k].opt, g_other[k].arg ? " " : "", g_other[k].arg ? g_other[k].arg : "");
    settings_usage_words(stderr);
    fprintf(stderr, " [-- CORE ARGS...]\n(mh2 --help describes them)\n");
    exit(2);
}

/* --help: from the tables, so that it names what the build takes (the release names no development option). */
static void help(void)
{
    printf("usage: mh2 [options] [-- CORE ARGS...]\n\nOptions that are not settings:\n");
    for (int advanced = 0; advanced < 2; advanced++) {
        if (advanced)
            printf("\nAdvanced options, for tests and automation:\n");
        for (size_t k = 0; k < sizeof(g_other) / sizeof(g_other[0]); k++) {
            if (g_other[k].advanced != advanced)
                continue;
            char left[80];
            snprintf(left, sizeof(left), "%s%s%s", g_other[k].opt, g_other[k].arg ? " " : "", g_other[k].arg ? g_other[k].arg : "");
            printf("  %-26s %s\n", left, TX(g_other[k].text));
        }
        if (!advanced)
            printf("  %-26s %s\n", "-- CORE ARGS...", TX(T_O_REST));
    }
    printf("\nSettings (an option here, or a line key = value in mh2.ini; the option wins):\n");
    settings_help(stdout);
    exit(0);
}

/* The original's play options that mh2 takes but has no use for, said in the log. */
static const char *g_ignored[16];
static int g_nignored;


/* The options that make a run automated (the guard, below): the tests' options. The player's are
 * --config, --set, --fullscreen, --mute, --vsync and --import-card. */
static const char *const g_auto_opts[] = {"--archive",  "--core",     "--no-window", "--gs-off",    "--fast",
                                          "--no-card",  "--quit-after", "--memcard1", "--memcard2", "--log-file"};

static void parse(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        for (size_t k = 0; k < sizeof(g_auto_opts) / sizeof(g_auto_opts[0]) && !g_cfg.automated; k++)
            if (!strcmp(a, g_auto_opts[k]))
                g_cfg.automated = g_auto_opts[k];
        if (!strcmp(a, "--")) {
            g_cfg.core_args = argv + i + 1;
            g_cfg.core_nargs = argc - i - 1;
            if (g_cfg.core_nargs && !g_cfg.automated)
                g_cfg.automated = "the core's own arguments, after --";
            return;
        } else if (!strcmp(a, "--help")) {
            help();
        } else if (!strcmp(a, "--launcher")) {
            g_cfg.launcher = 1;
        } else if (!strcmp(a, "--no-launcher")) {
            g_cfg.launcher = -1;
        } else if (!strcmp(a, "--set") && i + 1 < argc) {
            settings_set(argv[++i]);
        } else if (!strcmp(a, "--config") && i + 1 < argc) {
            g_cfg.config_path = argv[++i];
        } else if (!strcmp(a, "--core") && i + 1 < argc) {
            g_cfg.core_path = argv[++i];
        } else if (!strcmp(a, "--no-window")) {
            g_cfg.no_window = 1;
        } else if (!strcmp(a, "--gs-off")) {
            g_cfg.gs_off = 1;
        } else if (!strcmp(a, "--no-card")) {
            g_cfg.no_card = 1;
        } else if (!strcmp(a, "--fast")) {
            g_cfg.fast = 1;
        } else if (!strcmp(a, "--quit-after") && i + 1 < argc) {
            g_cfg.quit_after = strtoull(argv[++i], NULL, 10);
        } else if (!strcmp(a, "--import-card") && i + 1 < argc) {
            g_cfg.import_card = argv[++i];
        } else if (!strcmp(a, "--log-file") && i + 1 < argc) {
            g_cfg.log_file = argv[++i];
        } else {
            /* the settings' own options: the table's (settings.c) */
            int r = settings_cli(argc, argv, &i);
            if (r <= 0)
                usage();
        }
    }
}

/* --local-date first, in play only (core_args.c), so that the core's
 * arguments keep their order. */
static void local_date(void)
{
    if (!core_args_local_date(g_cfg.core_args, g_cfg.core_nargs, g_cfg.quit_after))
        return;
    char **args = malloc(sizeof(char *) * (size_t)(g_cfg.core_nargs + 2));
    if (!args)
        return;
    args[0] = "--local-date";
    for (int i = 0; i < g_cfg.core_nargs; i++)
        args[1 + i] = g_cfg.core_args[i];
    args[1 + g_cfg.core_nargs] = NULL;
    g_cfg.core_args = args;
    g_cfg.core_nargs += 1;
}

/* A fatal error: always logged; the modal box only for a player. An unattended run (no window,
 * or --quit-after or --screenshot: the tests) logs it and exits with an error code,
 * so nothing waits for a click. */
static void fatal_box(const char *msg)
{
    host_log("%s", msg);
    if (g_cfg.no_window || g_cfg.quit_after)
        return;
    char text[2400];
    const char *log = play_log_path();
    snprintf(text, sizeof(text), "%s\n\nThe details are at the end of the log%s%s. If you report this problem, please include the log (see section 8 of the guide).",
             msg, log ? ", " : "", log ? log : "");
    present_leave_fullscreen();
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "OpenMH2", text, NULL);
}

/* The core refused to run with what it was given, or the check before it said so: a message for the player, not a fault.
 * Logged always; the box only for a player (as fatal_box). */
static void refusal_box(const char *msg)
{
    host_log("%s", msg);
    if (g_cfg.no_window || g_cfg.quit_after)
        return;
    present_leave_fullscreen();
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "OpenMH2", msg, NULL);
}

static void request_quit(void)
{
    if (!g_ipc->quit_request)
        host_log("quit requested");
    mh2ipc_store(&g_ipc->quit_request, 1);
}


/* The guard: an automated run never reads or writes the user data
 * folder (the player's cards and settings). It names its own cards folder (--set cards=DIR, or a
 * settings file: --config, or mh2.ini next to mh2), or it stops here, before anything is read or
 * made; it never offers to import a card (cards.c). */
static int guard(void)
{
    if (!g_cfg.automated || g_cfg.cards_dir)
        return 0;
    host_log("this run is automated (%s) and names no cards folder of its own: it stops before it reads or "
             "writes anything in the player's data folder (the user data folder, on Windows "
             "%%APPDATA%%\\mh2-decomp\\mh2). Name it: --set cards=DIR, or cards = DIR in a settings file "
             "(--config FILE)",
             g_cfg.automated);
    return 2;
}


int main(int argc, char **argv)
{
    hfs_args(&argc, &argv);   /* Windows: the command line as UTF-8 (the C library's argv is in the ANSI code page) */
    g_cfg.vsync = -2;   /* not given (settings.c) */
    parse(argc, argv);
    /* the settings under the command line (settings.c), then the launcher: the disc, the cards */
    settings_load(g_cfg.config_path);
    settings_apply(g_cfg.fullscreen, g_cfg.mute);
    if (guard() != 0) {
        settings_log();
        return 2;
    }
    if (g_cfg.import_card)
        return cards_import(g_cfg.import_card, g_cfg.cards_dir);
    /* the launcher (launcher.c): the player's settings, before anything else starts; never in an automated run */
    if (launcher_wanted() && launcher_run() == LAUNCH_QUIT)
        return 0;
    /* play: a window, not a scripted run (a test or a picture check) */
    int play = !g_cfg.no_window && !g_cfg.quit_after;
    if ((g_cfg.log_file || play) && play_log_open(g_cfg.log_file) == 0)
        atexit(play_log_close);   /* the early returns and exits too */
    if (play)
        play_console_detach();
    if (play && play_data_dir()) {   /* the dumps of a crash (Windows): beside the log, local only (crash.c) */
        char dir[1100];
        snprintf(dir, sizeof(dir), "%s", play_data_dir());
        dir[strlen(dir) - 1] = 0;   /* without the separator */
        crash_setup(dir, play_stamp());
    }
    settings_log();
    if (strcmp(settings_enhancements(), "off"))
        host_log("enhancements: %s%s (graphics only; the game plays the same)", settings_enhancements(),
                 g_cfg.gs_off ? "; no graphics, so nothing to draw" : "");
    if (g_cfg.texture_dump || g_cfg.texture_packs)
        host_log("textures: dump %s, packs %s%s%s%s (graphics only; the game plays the same)", g_cfg.texture_dump ? "on" : "off",
                 g_cfg.texture_packs ? "on" : "off", g_cfg.textures ? ", folder " : "", g_cfg.textures ? g_cfg.textures : "",
                 g_cfg.gs_off ? "; no graphics, so nothing to write or draw" : "");
    if (g_cfg.usb_keyboard)
        host_log("keyboard typing: connected (%s switches between typing and the controller keys; it starts on the controller)",
                 kbd_toggle_name());
    for (int i = 0; i < g_nignored; i++)
        host_log("%s: ignored", g_ignored[i]);
    for (int p = 1; p >= 0; p--)   /* mh2's --memcard1/2: before cards_choose, which leaves a named port alone */
        if (g_cfg.memcard[p])
            launcher_core_arg(p ? "--memcard2" : "--memcard1", g_cfg.memcard[p]);
    if (!g_cfg.core_path)
        g_cfg.core_path = process_default_core();
    if (launcher_disc() != 0) {
        fatal_box("The game archive was not found. Put mh2-disc.arc next to mh2 or in the user data folder, "
                  "or name it with --archive FILE or archive = FILE in mh2.ini. The launcher can make it from your copy of the game.");
        return 1;
    }
    launcher_widescreen();       /* the core's --widescreen when the setting is on (settings.c) */
    launcher_offline_hub();      /* the core's --offline-hub when the setting is on (settings.c) */
    launcher_text_pack();        /* the core's --pack when the setting names a text pack (settings.c) */
    {   /* the chosen pack is looked at before the game starts, and what is wrong with it is said */
        const char *pack = core_args_pack(g_cfg.core_args, g_cfg.core_nargs);
        char why[1600];
        if (pack && textpack_check(pack, why, sizeof(why)) != 0) {
            refusal_box(why);
            return MH2IPC_EXIT_REFUSED;
        }
    }
    if (!g_cfg.no_card)   /* mh2's own cards (cards.c) */
        cards_choose(play, g_cfg.card[0], g_cfg.card[1], g_cfg.old_card[0], g_cfg.old_card[1], g_cfg.cards_dir);
    local_date();

    if (mh2ipc_create(&g_map, MH2IPC_TOTAL_BYTES) != 0) {
        host_log("the shared memory for mh2-core could not be made");
        return 1;
    }
    g_ipc = (Mh2IpcHeader *)g_map.base;
    host_log("mh2 started (pid %u)", (unsigned int)mh2ipc_pid());
    g_block = (unsigned char *)g_map.base;
    mh2ipc_layout(g_ipc, mh2ipc_pid());

    SDL_InitFlags flags = SDL_INIT_EVENTS | SDL_INIT_GAMEPAD;
    if (!g_cfg.no_window)
        flags |= SDL_INIT_VIDEO;
    if (!SDL_Init(flags)) {
        host_log("SDL_Init: %s", SDL_GetError());
        return 1;
    }
    if (!g_cfg.no_window && present_init() != 0)
        return 1;
    host_realtime_thread();   /* the main thread shows the pictures of the timed presenter */
    unsigned int hf = 0;
    int gs = gs_init();
    if (gs == -2)
        return 1;
    if (gs == 0)
        hf |= MH2IPC_HOST_GS;
    if (audio_init() == 0)
        hf |= MH2IPC_HOST_AUDIO;
    if (movie_init() == 0)
        hf |= MH2IPC_HOST_MOVIE;
    if (!g_cfg.no_window && !g_cfg.fast)
        hf |= MH2IPC_HOST_PACE;   /* real-time play (the original game: paced unless --fast or headless) */
    if (g_cfg.fast_loading)
        hf |= MH2IPC_HOST_FASTLOAD;   /* no wait for real time while the loading screen is open (pace.c) */
    if (g_cfg.usb_keyboard) {
        hf |= MH2IPC_HOST_USB_KEYBOARD;   /* before the core starts, and unchanged for the session */
        kbd_start(&g_ipc->keys);
    }
    g_ipc->host_flags = hf;
    /* --quit-after N: the core ends after N VSyncs, at the same VBlank in every run (a request
     * from here would reach it a varying number of VBlanks later) */
    g_ipc->quit_vblank_lo = (uint32_t)g_cfg.quit_after;
    g_ipc->quit_vblank_hi = (uint32_t)(g_cfg.quit_after >> 32);
    input_init();

    char lockmsg[1600];
    if (cards_lock(lockmsg, sizeof(lockmsg)) != 0) {
        fatal_box(lockmsg);
        return 1;
    }
    if (process_start(g_cfg.core_path, g_cfg.core_args, g_cfg.core_nargs) != 0) {
        char msg[1200];
        snprintf(msg, sizeof(msg), "The game could not be started (%s). mh2-core is missing or damaged. See section 8 of the guide.", g_cfg.core_path);
        fatal_box(msg);
        return 1;
    }

    int code = 0, running = 1;
    uint64_t quit_at = 0;
    while (running) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, present_wait_ms())) {
            do {
                int taken = 1;   /* the window's own keys (below) never reach the game's keyboard */
                if (e.type == SDL_EVENT_QUIT)
                    request_quit();
                else if (e.type == SDL_EVENT_WINDOW_EXPOSED || e.type == SDL_EVENT_WINDOW_RESIZED ||
                         e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED || e.type == SDL_EVENT_WINDOW_RESTORED)
                    present_dirty();   /* a paused game sends no picture: the window is drawn again with the one it has */
                else if (e.type == g_present_event)
                    ;   /* drawn below */
                else if (states_key(&e))
                    ;   /* the save states' keys (states.c) */
                else if (freecam_key(&e))
                    ;   /* the freecam's keys (freecam.c) */
                else if (pace_key(&e))
                    ;   /* the pause and the step, with the camera off (pace.c) */
                else if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat &&
                         (e.key.scancode == SDL_SCANCODE_F11 ||
                          (e.key.scancode == SDL_SCANCODE_RETURN && (e.key.mod & SDL_KMOD_ALT))))
                    present_toggle_fullscreen();
                else
                    taken = 0;
                freecam_event(&e);   /* the mouse, for the freecam */
                if (!taken)
                    kbd_event(&e);   /* the USB keyboard (a no-op unless connected) */
                input_event(&e);
            } while (SDL_PollEvent(&e));
        }
        kbd_update();
        input_update();
        states_update();
        freecam_update();
        pace_update();
        if (!g_cfg.no_window)
            present_draw();
        /* a core that doesn't end within 5 s of the request is stopped */
        if (g_ipc->quit_request && !quit_at)
            quit_at = SDL_GetTicks() + 5000;
        if (quit_at && SDL_GetTicks() > quit_at) {
            host_log("mh2-core did not end: stopping it");
            process_kill();
            quit_at = 0;
        }
        if (!process_running(&code))
            running = 0;
    }
    /* the core has ended: the GS thread shows what is left in the ring */
    gs_stop();
    unsigned int state = mh2ipc_load(&g_ipc->core_state);
    if (state == MH2IPC_STATE_FATAL) {
        g_ipc->fatal_message[sizeof(g_ipc->fatal_message) - 1] = 0;
        char msg[600];
        if (code == MH2IPC_EXIT_REFUSED) {   /* the core would not run with what it was given: its message, as it is */
            refusal_box(g_ipc->fatal_message);
        } else {
            snprintf(msg, sizeof(msg), "mh2-core stopped: %s", g_ipc->fatal_message);
            fatal_box(msg);
            if (!code)
                code = 1;   /* the core's fatal end is an error, whatever its status */
        }
    } else if (state == MH2IPC_STATE_EXITED) {
        host_log("mh2-core ended (%d) after %llu VSyncs", code, (unsigned long long)gs_vsyncs());
    } else {
        char msg[200];
        snprintf(msg, sizeof(msg), "The game stopped unexpectedly (exit status %d).", code);
        fatal_box(msg);
        if (!code)
            code = 1;
    }
    mh2ipc_store(&g_ipc->host_state, MH2IPC_STATE_EXITED);
    input_shutdown();
    audio_shutdown();
    movie_shutdown();   /* after the GS thread and the sound device, the movie's two users */
    present_shutdown();
    SDL_Quit();
    mh2ipc_close(&g_map);
    play_log_close();
    return code;
}
