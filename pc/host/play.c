/*
 * What a play session keeps: the user data folder, the session's name and
 * its log (main.c: rotate_logs). The files are in the user data folder
 * instead of the working directory. Nothing here reaches the game: the
 * core gets the names of its own files only. In the table, DATA is the user
 * data folder and STAMP is the session's start, local time:
 * YYYYMMDD-HHMMSS.
 *
 *   DATA/mh2.ini                         the settings (settings.c), optional
 *   DATA/memcards/Mcd001.ps2             memory card 1 by default (main.c)
 *   DATA/mh2.log                         the log of the session that runs
 *   DATA/mh2.1.log ... mh2.5.log         the logs of the sessions before it
 *                                        (the last five kept): when a session
 *                                        starts, mh2.log becomes mh2.1.log,
 *                                        mh2.1.log becomes mh2.2.log, and so
 *                                        on; the oldest, mh2.5.log, is
 *                                        deleted
 *
 * The memory cards are chosen by name (cards.c has the rules); the core writes
 * CARD.journal, CARD.tmp and CARD.bak beside a card.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#endif

#include "mh2host.h"

#ifdef _WIN32
#define SEP "\\"
#else
#define SEP "/"
#endif

static struct {
    int have_data;
    char data[1100];     /* the user data folder, ending in its separator */
    char stamp[40];      /* session_YYYYMMDD-HHMMSS */
    void *log_handle;    /* Windows: the log file's handle, for the core's standard output */
    char log[1400];      /* the log file, when there is one */
} pl;

/* NULL in an automated run (the guard, main.c): SDL_GetPrefPath would make the folder. */
const char *play_data_dir(void)
{
    if (g_cfg.automated)
        return NULL;
    if (!pl.have_data) {
        pl.have_data = -1;
        char *pref = SDL_GetPrefPath("mh2-decomp", "mh2");
        if (pref) {
            snprintf(pl.data, sizeof(pl.data), "%s", pref);
            SDL_free(pref);
            pl.have_data = 1;
        }
    }
    return pl.have_data > 0 ? pl.data : NULL;
}

const char *play_stamp(void)
{
    if (!pl.stamp[0]) {
        time_t t = time(NULL);
        struct tm tm;
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        strftime(pl.stamp, sizeof(pl.stamp), "session_%Y%m%d-%H%M%S", &tm);
    }
    return pl.stamp;
}


/* Keep the logs of the last five sessions: mh2.log becomes mh2.1.log, and so on. */
static void rotate(const char *dir)
{
    char from[1200], to[1200];
    snprintf(to, sizeof(to), "%smh2.5.log", dir);
    remove(to);
    for (int i = 4; i >= 1; i--) {
        snprintf(from, sizeof(from), "%smh2.%d.log", dir, i);
        snprintf(to, sizeof(to), "%smh2.%d.log", dir, i + 1);
        rename(from, to);
    }
    snprintf(from, sizeof(from), "%smh2.log", dir);
    snprintf(to, sizeof(to), "%smh2.1.log", dir);
    rename(from, to);
}

/* The log's file: PATH if given, else DATA/mh2.log after rotating the older logs (an
 * explicit PATH is used as it is, and nothing is rotated). */
static int log_path(const char *path, char *out, size_t cap)
{
    if (path) {
        snprintf(out, cap, "%s", path);
        return 0;
    }
    const char *d = play_data_dir();
    if (!d)
        return -1;
    rotate(d);
    snprintf(out, cap, "%smh2.log", d);
    return 0;
}

/* The log has one writer: a thread of mh2's reads a pipe that is mh2's standard output and error
 * (its own lines, and the renderer's) and the core's (process.c hands the core the pipe), and writes
 * what comes into the log file and to the standard error mh2 had before. Lines from the two
 * processes never overwrite or split each other, even in a file on a network share. */
static struct {
    FILE *file;
    SDL_Thread *thread;
    int orig_out, orig_err;   /* mh2's standard output and error before (-1: none) */
#ifdef _WIN32
    HANDLE rd, wr;
#else
    int rd, wr;
#endif
} lg;   /* orig_out and orig_err set before use */

static int SDLCALL log_reader(void *unused)
{
    (void)unused;
    static char buf[16384];
    for (;;) {
#ifdef _WIN32
        DWORD n = 0;
        if (!ReadFile(lg.rd, buf, sizeof(buf), &n, NULL) || !n)
            break;
        if (lg.orig_err >= 0)
            _write(lg.orig_err, buf, (unsigned int)n);
#else
        ssize_t n = read(lg.rd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        if (lg.orig_err >= 0 && write(lg.orig_err, buf, (size_t)n) < 0)
            lg.orig_err = -1;
#endif
        fwrite(buf, 1, (size_t)n, lg.file);
        fflush(lg.file);
    }
    return 0;
}

int play_log_open(const char *path)
{
    char p[1400];
    if (log_path(path, p, sizeof(p)) != 0)
        return -1;
    lg.file = fopen(p, "wb");
    if (!lg.file) {
        host_log("cannot write the log %s", p);
        return -1;
    }
    fprintf(stderr, "mh2: the log is %s\n", p);
    fflush(stdout);
    fflush(stderr);
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    if (!CreatePipe(&lg.rd, &lg.wr, &sa, 1 << 16)) {
        fclose(lg.file);
        lg.file = NULL;
        return -1;
    }
    SetHandleInformation(lg.rd, HANDLE_FLAG_INHERIT, 0);
    lg.orig_out = _dup(1);
    lg.orig_err = _dup(2);
    HANDLE dup = NULL;
    DuplicateHandle(GetCurrentProcess(), lg.wr, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS);
    int fd = dup ? _open_osfhandle((intptr_t)dup, _O_BINARY) : -1;
    if (fd >= 0) {
        _dup2(fd, 1);
        _dup2(fd, 2);
        _close(fd);
    }
    SetStdHandle(STD_OUTPUT_HANDLE, lg.wr);
    SetStdHandle(STD_ERROR_HANDLE, lg.wr);
    pl.log_handle = lg.wr;
#else
    int fds[2];
    if (pipe(fds) != 0) {
        fclose(lg.file);
        lg.file = NULL;
        return -1;
    }
    lg.rd = fds[0];
    lg.wr = fds[1];
    fcntl(lg.rd, F_SETFD, FD_CLOEXEC);
    fcntl(lg.wr, F_SETFD, FD_CLOEXEC);   /* the core gets it as its fds 1 and 2 */
    lg.orig_out = dup(1);
    lg.orig_err = dup(2);
    if (lg.orig_out >= 0)
        fcntl(lg.orig_out, F_SETFD, FD_CLOEXEC);
    if (lg.orig_err >= 0)
        fcntl(lg.orig_err, F_SETFD, FD_CLOEXEC);
    dup2(lg.wr, 1);
    dup2(lg.wr, 2);
#endif
    setvbuf(stdout, NULL, _IONBF, 0);
    lg.thread = SDL_CreateThread(log_reader, "mh2-log", NULL);
    snprintf(pl.log, sizeof(pl.log), "%s", p);
    host_log("log: %s", p);
    return 0;
}

/* Play, with the log open: a console that is mh2's alone (a double-click; Windows opens one for a
 * console program) holds nothing but the log's name, and closing it would stop mh2 and the core at
 * once, in mid-session. mh2 lets it go; the core then starts without one (process.c).
 * Started from a command prompt, mh2 shares that console and keeps it. */
void play_console_detach(void)
{
#ifdef _WIN32
    DWORD ids[2];
    if (lg.file && GetConsoleProcessList(ids, 2) == 1 && FreeConsole())
        host_log("the console window is closed (the log has everything)");
#endif
}

/* At the end (after the core): mh2's standard output and error back, the pipe drained, the file
 * closed. */
void play_log_close(void)
{
    if (!lg.file)
        return;
    fflush(stdout);
    fflush(stderr);
#ifdef _WIN32
    if (lg.orig_out >= 0)
        _dup2(lg.orig_out, 1);
    if (lg.orig_err >= 0)
        _dup2(lg.orig_err, 2);
    CloseHandle(lg.wr);
#else
    if (lg.orig_out >= 0)
        dup2(lg.orig_out, 1);
    if (lg.orig_err >= 0)
        dup2(lg.orig_err, 2);
    close(lg.wr);
#endif
    if (lg.thread)
        SDL_WaitThread(lg.thread, NULL);   /* until every writer has closed the pipe */
    lg.thread = NULL;
    fclose(lg.file);
    lg.file = NULL;
}

const char *play_log_path(void)
{
    return pl.log[0] ? pl.log : NULL;
}

void *play_log_handle(void)
{
    return pl.log_handle;
}

