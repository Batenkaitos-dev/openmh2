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
#include "mh2host.h"

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

static void usage(void)
{
    fprintf(stderr, "usage: mh2 [--config FILE.ini] [--set KEY=VALUE] [--archive FILE] [--core PATH] [--no-window] [--gs-off] [--mute] [--fullscreen] [--fast] [--no-card] [--quit-after N]"
                    " [--memcard1 FILE] [--memcard2 FILE] [--import-card FILE] [--vsync auto|on|off] [--log-file FILE]"
                    " [--ssaa 1|2|4|8|16] [--hires] [--widescreen] [-- CORE ARGS...]\n");
    exit(2);
}

/* The original's play options that mh2 takes but has no use for, said in the log. */
static const char *g_ignored[16];
static int g_nignored;


/* The options that make a run automated (the guard, below): the tests' options. The player's are
 * --config, --set, --fullscreen, --mute, --vsync and --import-card (pc/host/PLAYING.md). */
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
        } else if (!strcmp(a, "--set") && i + 1 < argc) {
            settings_set(argv[++i]);
        } else if (!strcmp(a, "--config") && i + 1 < argc) {
            g_cfg.config_path = argv[++i];
        } else if (!strcmp(a, "--archive") && i + 1 < argc) {
            g_cfg.archive = argv[++i];
        } else if (!strcmp(a, "--core") && i + 1 < argc) {
            g_cfg.core_path = argv[++i];
        } else if (!strcmp(a, "--no-window")) {
            g_cfg.no_window = 1;
        } else if (!strcmp(a, "--gs-off")) {
            g_cfg.gs_off = 1;
        } else if (!strcmp(a, "--mute")) {
            g_cfg.mute = 1;
        } else if (!strcmp(a, "--no-card")) {
            g_cfg.no_card = 1;
        } else if (!strcmp(a, "--fast")) {
            g_cfg.fast = 1;
        } else if (!strcmp(a, "--fullscreen")) {
            g_cfg.fullscreen = 1;
        } else if (!strcmp(a, "--quit-after") && i + 1 < argc) {
            g_cfg.quit_after = strtoull(argv[++i], NULL, 10);
        } else if ((!strcmp(a, "--memcard1") || !strcmp(a, "--memcard2")) && i + 1 < argc) {
            g_cfg.memcard[a[9] - '1'] = argv[++i];
        } else if (!strcmp(a, "--import-card") && i + 1 < argc) {
            g_cfg.import_card = argv[++i];
        } else if (!strcmp(a, "--vsync") && i + 1 < argc) {
            const char *v = argv[++i];
            if (strcmp(v, "on") && strcmp(v, "off") && strcmp(v, "auto"))
                usage();
            g_cfg.vsync = !strcmp(v, "on") ? 1 : !strcmp(v, "off") ? 0 : -1;
        } else if (!strcmp(a, "--log-file") && i + 1 < argc) {
            g_cfg.log_file = argv[++i];
        } else if (!strcmp(a, "--hires")) {
            g_cfg.hires = 1;
        } else if (!strcmp(a, "--widescreen")) {
            g_cfg.widescreen = 2;   /* the newest level */
        } else if (!strcmp(a, "--ssaa") && i + 1 < argc) {
            g_cfg.ssaa = atoi(argv[++i]);
            if (g_cfg.ssaa != 1 && g_cfg.ssaa != 2 && g_cfg.ssaa != 4 && g_cfg.ssaa != 8 && g_cfg.ssaa != 16)
                usage();
        } else {
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
    snprintf(text, sizeof(text), "%s\n\nThe details are at the end of the log%s%s. Please send it with the session's files.",
             msg, log ? ", " : "", log ? log : "");
    present_leave_fullscreen();
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Monster Hunter 2", text, NULL);
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
        host_log("enhancements: %s%s (the picture only; the game is the same)", settings_enhancements(),
                 g_cfg.gs_off ? "; no graphics, so nothing to draw" : "");
    for (int i = 0; i < g_nignored; i++)
        host_log("%s: ignored", g_ignored[i]);
    for (int p = 1; p >= 0; p--)   /* mh2's --memcard1/2: before cards_choose, which leaves a named port alone */
        if (g_cfg.memcard[p])
            launcher_core_arg(p ? "--memcard2" : "--memcard1", g_cfg.memcard[p]);
    if (!g_cfg.core_path)
        g_cfg.core_path = process_default_core();
    if (launcher_disc() != 0) {
        fatal_box("No disc archive was found. Put mh2-disc.arc next to mh2 or in the user data folder, "
                  "or name it with --archive FILE or archive = FILE in mh2.ini.");
        return 1;
    }
    launcher_widescreen();       /* the core's --widescreen when the setting is on (settings.c) */
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
        snprintf(msg, sizeof(msg), "mh2-core could not be started (%s)", g_cfg.core_path);
        fatal_box(msg);
        return 1;
    }

    int code = 0, running = 1;
    uint64_t quit_at = 0;
    while (running) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 4)) {
            do {
                if (e.type == SDL_EVENT_QUIT)
                    request_quit();
                else if (e.type == g_present_event)
                    ;   /* drawn below */
                else if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat &&
                         (e.key.scancode == SDL_SCANCODE_F11 ||
                          (e.key.scancode == SDL_SCANCODE_RETURN && (e.key.mod & SDL_KMOD_ALT))))
                    present_toggle_fullscreen();
                input_event(&e);
            } while (SDL_PollEvent(&e));
        }
        input_update();
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
        snprintf(msg, sizeof(msg), "mh2-core stopped: %s", g_ipc->fatal_message);
        fatal_box(msg);
        if (!code)
            code = 1;   /* the core's fatal end is an error, whatever its status */
    } else if (state == MH2IPC_STATE_EXITED) {
        host_log("mh2-core ended (%d) after %llu VSyncs", code, (unsigned long long)gs_vsyncs());
    } else {
        char msg[200];
        snprintf(msg, sizeof(msg), "mh2-core stopped unexpectedly (exit status %d)", code);
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
