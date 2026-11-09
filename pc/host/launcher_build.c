/*
 * Building the game's program from the launcher (launcher_build.h). The kit's script prints one line per event
 * ("MH2BUILD STEP n N words", "MH2BUILD PROGRESS d t", "MH2BUILD DONE path", "MH2BUILD FAIL code words"); a thread reads them
 * from the child's standard output and the window's thread takes them in with build_poll.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arcmake.h"
#include "hostfs.h"
#include "launcher_build.h"
#include "mh2host.h"

#ifdef _WIN32
#define SEP "\\"
#define EXE ".exe"
#define PYTHON "python" SEP "python.exe"
#else
#define SEP "/"
#define EXE ""
#define PYTHON "python" SEP "bin" SEP "python3"
#endif

#define NEED_FREE (2500ull * 1000 * 1000)   /* the build's files while it runs */

static struct {
    int state;
    SDL_Process *proc;
    SDL_Thread *thread;
    SDL_Mutex *mu;
    BuildStatus st;            /* what the thread has found (under mu) */
    SDL_AtomicInt finished;
    int cancelled;
    Uint64 start_ms, cancel_ms;
    int end_state;
    char core[1300];
    char log[1300];
} bd;

static const char *kit_dir(void)
{
    static char path[1300];
    const char *base = SDL_GetBasePath();
    snprintf(path, sizeof(path), "%skit" SEP, base ? base : "");
    return path;
}

static int read_line(const char *path, const char *key, char *out, size_t cap)
{
    FILE *f = hfs_fopen(path, "rb");
    if (!f)
        return 0;
    char line[300];
    size_t kl = strlen(key);
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, key, kl) && line[kl] == ' ') {
            size_t n = strcspn(line + kl + 1, "\r\n");
            snprintf(out, cap, "%.*s", (int)n, line + kl + 1);
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

int build_kit_present(char *id, size_t cap)
{
    char p[1400], b[80] = "";
    snprintf(p, sizeof(p), "%skit.txt", kit_dir());
    if (!read_line(p, "kit", b, sizeof(b)))
        return 0;
    snprintf(p, sizeof(p), "%s%s", kit_dir(), PYTHON);
    FILE *f = hfs_fopen(p, "rb");
    if (!f)
        return 0;
    fclose(f);
    if (id)
        snprintf(id, cap, "%s", b);
    return 1;
}

static const char *data_sub(const char *sub)
{
    static char path[4][1300];
    static int n;
    const char *d = play_data_dir();
    if (!d)
        return "";
    char *p = path[n++ & 3];
    snprintf(p, 1300, "%s%s", d, sub);
    return p;
}

const char *build_core_path(void)
{
    snprintf(bd.core, sizeof(bd.core), "%s", data_sub("core" SEP "mh2-core" EXE));
    return bd.core;
}

const char *build_log_path(void)
{
    snprintf(bd.log, sizeof(bd.log), "%s", data_sub("build.log"));
    return bd.log;
}

int build_core_current(void)
{
    char id[80], have[80];
    if (!build_kit_present(id, sizeof(id)))
        return 0;
    FILE *f = hfs_fopen(build_core_path(), "rb");
    if (!f)
        return 0;
    fclose(f);
    return read_line(data_sub("core" SEP "built.txt"), "kit", have, sizeof(have)) && !strcmp(id, have);
}

static void apply_line(const char *line)
{
    if (strncmp(line, "MH2BUILD ", 9))
        return;
    line += 9;
    SDL_LockMutex(bd.mu);
    int a, b;
    char t[200];
    if (sscanf(line, "STEP %d %d %199[^\n]", &a, &b, t) == 3) {
        bd.st.step = a;
        bd.st.nsteps = b;
        snprintf(bd.st.step_text, sizeof(bd.st.step_text), "%s", t);
        bd.st.done = bd.st.total = 0;
    } else if (sscanf(line, "PROGRESS %d %d", &a, &b) == 2) {
        bd.st.done = a;
        bd.st.total = b;
    } else if (!strncmp(line, "DONE", 4)) {
        bd.end_state = BD_DONE;
    } else if (!strncmp(line, "FAIL ", 5)) {
        char code[16];
        const char *m = line + 5;
        if (sscanf(m, "%15s", code) == 1) {
            snprintf(bd.st.code, sizeof(bd.st.code), "%s", code);
            m += strlen(code);
            while (*m == ' ')
                m++;
            snprintf(bd.st.message, sizeof(bd.st.message), "%s", m);
            bd.end_state = !strcmp(code, "cancel") ? BD_CANCELLED : BD_FAILED;
        }
    }
    SDL_UnlockMutex(bd.mu);
}

static int SDLCALL reader(void *unused)
{
    (void)unused;
    SDL_IOStream *out = (SDL_IOStream *)SDL_GetPointerProperty(SDL_GetProcessProperties(bd.proc), SDL_PROP_PROCESS_STDOUT_POINTER, NULL);
    char buf[2048], line[1024];
    size_t n = 0;
    for (;;) {
        size_t got = out ? SDL_ReadIO(out, buf, sizeof(buf)) : 0;
        if (!got) {
            if (out && SDL_GetIOStatus(out) == SDL_IO_STATUS_NOT_READY) {   /* nothing yet: the pipe does not block */
                SDL_Delay(30);
                continue;
            }
            break;
        }
        for (size_t i = 0; i < got; i++) {
            if (buf[i] == '\n' || n + 1 >= sizeof(line)) {
                line[n] = 0;
                if (n && line[n - 1] == '\r')
                    line[n - 1] = 0;
                apply_line(line);
                n = 0;
            } else {
                line[n++] = buf[i];
            }
        }
    }
    int code = -1;
    SDL_WaitProcess(bd.proc, true, &code);
    SDL_LockMutex(bd.mu);
    if (!bd.end_state) {
        bd.end_state = bd.cancelled ? BD_CANCELLED : BD_FAILED;
        if (bd.end_state == BD_FAILED) {
            snprintf(bd.st.code, sizeof(bd.st.code), "build");
            snprintf(bd.st.message, sizeof(bd.st.message), "Building the game stopped unexpectedly. See the logs folder.");
            FILE *lf = hfs_fopen(build_log_path(), "ab");   /* the status is for the log, not for the dialog */
            if (lf) {
                fprintf(lf, "the build program ended with exit status %d\n", code);
                fclose(lf);
            }
        }
    }
    SDL_UnlockMutex(bd.mu);
    SDL_SetAtomicInt(&bd.finished, 1);
    return 0;
}

int build_start(const char *disc, char *err, size_t cap)
{
    if (bd.state == BD_RUNNING)
        return -1;
    char id[80];
    if (!build_kit_present(id, sizeof(id))) {
        snprintf(err, cap, "The kit folder is missing or damaged, so the game can't be built. It should be next to mh2. Unpack the release again.");
        return -1;
    }
    const char *data = play_data_dir();
    if (!data) {
        snprintf(err, cap, "The user data folder can't be created, so the game can't be built.");
        return -1;
    }
    if (arc_free_space(data) < NEED_FREE) {
        snprintf(err, cap, "There is not enough free disk space to build the game. It needs about 2.5 GB while it runs. The finished game takes 24 MB.");
        return -1;
    }
    char script[1400], py[1400], work[1400], out[1400], log[1400];
    snprintf(py, sizeof(py), "%s%s", kit_dir(), PYTHON);
    snprintf(script, sizeof(script), "%sstart", kit_dir());
    snprintf(work, sizeof(work), "%sbuild", data);
    snprintf(out, sizeof(out), "%score", data);
    snprintf(log, sizeof(log), "%sbuild.log", data);
    const char *args[] = {py, script, "--disc", disc, "--work", work, "--out", out, "--log", log, NULL};
    SDL_PropertiesID pr = SDL_CreateProperties();
    SDL_SetPointerProperty(pr, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)args);
    SDL_SetNumberProperty(pr, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetNumberProperty(pr, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetBooleanProperty(pr, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
    SDL_SetBooleanProperty(pr, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, false);
    SDL_Process *p = SDL_CreateProcessWithProperties(pr);
    SDL_DestroyProperties(pr);
    if (!p) {
        snprintf(err, cap, "The game could not be built: the build did not start (%s).", SDL_GetError());
        return -1;
    }
    if (!bd.mu)
        bd.mu = SDL_CreateMutex();
    if (bd.thread)
        SDL_WaitThread(bd.thread, NULL);
    memset(&bd.st, 0, sizeof(bd.st));
    bd.proc = p;
    bd.cancelled = 0;
    bd.end_state = 0;
    bd.start_ms = SDL_GetTicks();
    SDL_SetAtomicInt(&bd.finished, 0);
    bd.state = BD_RUNNING;
    bd.st.state = BD_RUNNING;
    bd.thread = SDL_CreateThread(reader, "build", NULL);
    host_log("the game's build started (%s)", disc);
    return 0;
}

void build_poll(BuildStatus *st)
{
    if (bd.state == BD_RUNNING) {
        if (bd.cancelled && !SDL_GetAtomicInt(&bd.finished) && SDL_GetTicks() - bd.cancel_ms > 10000)
            SDL_KillProcess(bd.proc, true);   /* the script did not end it */
        if (SDL_GetAtomicInt(&bd.finished)) {
            SDL_WaitThread(bd.thread, NULL);
            bd.thread = NULL;
            SDL_DestroyProcess(bd.proc);
            bd.proc = NULL;
            bd.state = bd.end_state;
            host_log("the game's build ended (%d)", bd.state);
        }
    }
    SDL_LockMutex(bd.mu ? bd.mu : (bd.mu = SDL_CreateMutex()));
    bd.st.state = bd.state;
    bd.st.secs = bd.state == BD_IDLE ? 0 : (double)(SDL_GetTicks() - bd.start_ms) / 1000.0;
    *st = bd.st;
    SDL_UnlockMutex(bd.mu);
}

void build_cancel(void)
{
    if (bd.state != BD_RUNNING || bd.cancelled)
        return;
    bd.cancelled = 1;
    bd.cancel_ms = SDL_GetTicks();
    SDL_IOStream *in = SDL_GetProcessInput(bd.proc);
    if (in) {
        SDL_WriteIO(in, "cancel\n", 7);
        SDL_FlushIO(in);
    }
}

void build_reset(void)
{
    if (bd.state == BD_RUNNING)
        return;
    bd.state = BD_IDLE;
    memset(&bd.st, 0, sizeof(bd.st));
}
