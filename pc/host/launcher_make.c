/*
 * Making the disc archive from the launcher (launcher_make.h; the launcher design, 8.1). The maker is mh2's own (arcmake.c:
 * the archive; movietable.c: the movies' timing table beside it), run on a thread of its own so that the window never waits for
 * it; the job's stages move on in that thread and make_poll, in the window's thread, takes in what it has found.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "arcmake.h"
#include "hostfs.h"
#include "launcher_make.h"
#include "launcher_text.h"
#include "movietable.h"

#ifdef _WIN32
#define SEP "\\"
#else
#define SEP "/"
#endif


static struct {
    int state;               /* what make_poll reports */
    SDL_Thread *thread;
    SDL_Mutex *mu;
    SDL_AtomicInt stop;      /* Cancel */
    SDL_AtomicInt finished;  /* the thread has its result in end* */
    MakeStatus st;           /* what the thread has found (under mu) */
    int end_state;           /* the thread's result: MK_DONE, MK_FAILED or MK_CANCELLED */
    int end_made;
    char end_message[700];
    ArcTable table;
    char source[1200], out[1300], tmp[1400];
    int has_source, force;
    uint64_t t0;
} mk;

static int is_file(const char *path)
{
    SDL_PathInfo info;
    return SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_FILE;
}

static int is_dir(const char *path)
{
    SDL_PathInfo info;
    return SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_DIRECTORY;
}

/* ---------------------------------------------------------------- the words for a failure */

void make_explain(const ArcResult *r, char *out, size_t cap)
{
    char a[40], b[40];
    switch (r->code) {
    case AM_E_BADSUM: snprintf(out, cap, TX(T_MK_BADSUM), r->file); break;
    case AM_E_SHORT: snprintf(out, cap, TX(T_MK_SHORT), r->file); break;
    case AM_E_MISSING: snprintf(out, cap, TX(T_MK_MISSING), r->file, r->where); break;
    case AM_E_SIZE: snprintf(out, cap, TX(T_MK_SIZE), r->file); break;
    case AM_E_NODISC: snprintf(out, cap, TX(T_MK_NODISC), (int)strlen(r->where), r->where); break;
    case AM_E_NOSOURCE: snprintf(out, cap, TX(T_MK_NOTDISC), (int)strlen(r->where), r->where); break;
    case AM_E_NOTISO: snprintf(out, cap, TX(T_MK_NOTISO), r->where); break;
    case AM_E_NOTONDISC: snprintf(out, cap, TX(T_MK_NOTONDISC), r->file); break;
    case AM_E_LAYOUT: snprintf(out, cap, TX(T_MK_LAYOUT), r->file); break;
    case AM_E_TAIL: snprintf(out, cap, TX(T_MK_TAIL), r->file); break;
    case AM_E_SPACE:
        make_size_text(r->need, a, sizeof(a));
        make_size_text(r->have, b, sizeof(b));
        snprintf(out, cap, TX(T_MK_SPACE), a, r->where, b);
        break;
    case AM_E_READ: snprintf(out, cap, TX(T_MK_READ), r->file[0] ? r->file : r->where, r->detail); break;
    case AM_E_WRITE: snprintf(out, cap, TX(T_MK_WRITE), r->where, r->detail); break;
    case AM_E_OPEN: snprintf(out, cap, TX(T_MK_OPEN), r->where, r->detail); break;
    default: snprintf(out, cap, TX(T_MK_OTHER), r->detail[0] ? r->detail : "the table of the disc's files is not usable"); break;
    }
}

/* ---------------------------------------------------------------- sizes */

uint64_t make_free_space(const char *path)
{
    return arc_free_space(path);
}

void make_size_text(uint64_t bytes, char *out, size_t cap)
{
    if (bytes >= 1000000000ull)
        snprintf(out, cap, "%.2f GB", (double)bytes / 1e9);
    else
        snprintf(out, cap, "%.0f MB", (double)bytes / 1e6);
}

/* ---------------------------------------------------------------- the job */

static int arc_cb(void *ud, int file, int nfiles, uint64_t done, uint64_t total, const char *name)
{
    (void)ud;
    SDL_LockMutex(mk.mu);
    if (file > 0) {
        mk.st.stage = MKS_FILES;
        mk.st.file = file;
        mk.st.nfiles = nfiles;
        mk.st.done = done;
        mk.st.total = total;
        snprintf(mk.st.name, sizeof(mk.st.name), "%s", name);
    }
    SDL_UnlockMutex(mk.mu);
    return SDL_GetAtomicInt(&mk.stop);
}

static int movie_cb(void *ud, int i, int n, int in_movie)
{
    (void)ud;
    if (!in_movie) {
        SDL_LockMutex(mk.mu);
        mk.st.stage = MKS_MOVIES;
        mk.st.movie = i;
        mk.st.nmovies = n;
        SDL_UnlockMutex(mk.mu);
    }
    return SDL_GetAtomicInt(&mk.stop);
}

static void set_stage(int stage)
{
    SDL_LockMutex(mk.mu);
    mk.st.stage = stage;
    SDL_UnlockMutex(mk.mu);
}

static int SDLCALL worker(void *unused)
{
    (void)unused;
    int state = MK_DONE, made = 0;
    char message[700] = "";
    if (mk.has_source) {
        ArcResult r;
        ArcJob job = {.table = &mk.table, .source = mk.source, .out = mk.out, .force = mk.force, .progress = arc_cb};
        set_stage(MKS_SIZE);
        arc_make(&job, &r);
        if (r.code == AM_CANCELLED) {
            state = MK_CANCELLED;
        } else if (r.code != AM_OK) {
            state = MK_FAILED;
            make_explain(&r, message, sizeof(message));
        } else {
            made = 1;
        }
        if (r.code == AM_OK && r.up_to_date) {   /* it was whole already: shown as made */
            SDL_LockMutex(mk.mu);
            mk.st.nfiles = mk.table.n;
            mk.st.file = mk.table.n;
            mk.st.total = mk.st.done = r.total;
            SDL_UnlockMutex(mk.mu);
        }
    }
    if (state == MK_DONE) {
        char text[500];
        const char *libdir = NULL;
        set_stage(MKS_MOVIES);
        int rc = movietable_make(mk.out, libdir, movie_cb, NULL, NULL, text, sizeof(text));
        if (rc == MT_CANCELLED) {
            state = MK_CANCELLED;
        } else if (rc != MT_OK) {
            state = MK_FAILED;
            snprintf(message, sizeof(message), TX(T_MK_NOTABLE), text);
            made = 1;
        } else {
            snprintf(message, sizeof(message), "movie table written");
        }
    }
    SDL_LockMutex(mk.mu);
    mk.end_state = state;
    mk.end_made = made;
    snprintf(mk.end_message, sizeof(mk.end_message), "%s", message);
    SDL_UnlockMutex(mk.mu);
    SDL_SetAtomicInt(&mk.finished, 1);
    return 0;
}

static void reap(void)
{
    if (mk.thread) {
        SDL_WaitThread(mk.thread, NULL);
        mk.thread = NULL;
    }
}

int make_start(const char *source, const char *out, int force, char *err, size_t cap)
{
    make_reset();
    memset(&mk.st, 0, sizeof(mk.st));
    if (!mk.mu)
        mk.mu = SDL_CreateMutex();
    arc_builtin(&mk.table);
    snprintf(mk.source, sizeof(mk.source), "%s", source ? source : "");
    snprintf(mk.out, sizeof(mk.out), "%s", out);
    snprintf(mk.tmp, sizeof(mk.tmp), "%s.tmp", out);
    mk.has_source = source != NULL;   /* no source: only the movie table */
    mk.force = force;
    mk.t0 = SDL_GetTicks();
    mk.st.stage = mk.has_source ? MKS_SIZE : MKS_MOVIES;
    mk.state = MK_RUNNING;
    SDL_SetAtomicInt(&mk.stop, 0);
    SDL_SetAtomicInt(&mk.finished, 0);
    mk.thread = SDL_CreateThread(worker, "make-archive", NULL);
    if (!mk.thread) {
        snprintf(err, cap, "%s", SDL_GetError());
        mk.state = MK_IDLE;
        return -1;
    }
    return 0;
}

void make_poll(MakeStatus *st)
{
    if (mk.state == MK_RUNNING && SDL_GetAtomicInt(&mk.finished)) {
        reap();
        SDL_LockMutex(mk.mu);
        mk.state = mk.end_state;
        mk.st.archive_made = mk.end_made;
        snprintf(mk.st.message, sizeof(mk.st.message), "%s", mk.end_message);
        SDL_UnlockMutex(mk.mu);
    }
    if (mk.mu)
        SDL_LockMutex(mk.mu);
    *st = mk.st;
    if (mk.mu)
        SDL_UnlockMutex(mk.mu);
    st->state = mk.state;
    st->secs = mk.state == MK_IDLE ? 0 : (double)(SDL_GetTicks() - mk.t0) / 1000.0;
}

void make_cancel(void)
{
    if (mk.state != MK_RUNNING)
        return;
    SDL_SetAtomicInt(&mk.stop, 1);
    reap();   /* the thread notices within a chunk, removes OUT.tmp, and ends */
    mk.state = MK_CANCELLED;
}

void make_reset(void)
{
    if (mk.state == MK_RUNNING)
        make_cancel();
    reap();
    mk.state = MK_IDLE;
    memset(&mk.st, 0, sizeof(mk.st));
}

/* ---------------------------------------------------------------- where the player's copy may be */

typedef struct Scan {
    MakeSource *out;
    int n, max;
    char dir[1100];
    int has_cnf;
    char isos[8][300];
    int niso;
    char subdirs[48][300];
    int nsub;
} Scan;

static SDL_EnumerationResult SDLCALL scan_cb(void *ud, const char *dirname, const char *fname)
{
    Scan *s = ud;
    (void)dirname;
    char full[1500];
    snprintf(full, sizeof(full), "%s%s%s", s->dir, s->dir[strlen(s->dir) - 1] == '/' || s->dir[strlen(s->dir) - 1] == '\\' ? "" : SEP, fname);
    size_t len = strlen(fname);
    if (!SDL_strcasecmp(fname, "SYSTEM.CNF")) {
        s->has_cnf = 1;
    } else if (len > 4 && !SDL_strcasecmp(fname + len - 4, ".iso") && s->niso < 8 && is_file(full)) {
        snprintf(s->isos[s->niso++], sizeof(s->isos[0]), "%s", full);
    } else if (fname[0] != '.' && s->nsub < 48 && is_dir(full)) {
        snprintf(s->subdirs[s->nsub++], sizeof(s->subdirs[0]), "%s", full);
    }
    return SDL_ENUM_CONTINUE;
}

static void add_source(MakeSource *out, int *n, int max, const char *path, int iso)
{
    if (*n >= max)
        return;
    for (int i = 0; i < *n; i++)
        if (!strcmp(out[i].path, path))
            return;
    snprintf(out[*n].path, sizeof(out[*n].path), "%s", path);
    out[*n].iso = iso;
    (*n)++;
}

/* DIR itself (the disc's files, or images in it), then one level of folders below it for images. */
static void look_in(const char *dir, MakeSource *out, int *n, int max)
{
    Scan s;
    memset(&s, 0, sizeof(s));
    snprintf(s.dir, sizeof(s.dir), "%s", dir);
    SDL_EnumerateDirectory(dir, scan_cb, &s);
    if (s.has_cnf)
        add_source(out, n, max, dir, 0);
    for (int i = 0; i < s.niso; i++)
        add_source(out, n, max, s.isos[i], 1);
    for (int i = 0; i < s.nsub; i++) {
        Scan t;
        memset(&t, 0, sizeof(t));
        snprintf(t.dir, sizeof(t.dir), "%s", s.subdirs[i]);
        SDL_EnumerateDirectory(s.subdirs[i], scan_cb, &t);
        if (t.has_cnf)
            add_source(out, n, max, s.subdirs[i], 0);
        for (int k = 0; k < t.niso; k++)
            add_source(out, n, max, t.isos[k], 1);
    }
}

int make_sources(MakeSource *out, int max, int env_only)
{
    int n = 0;
    char dir[1300];
    const char *base = env_only ? NULL : SDL_GetBasePath();
    if (base) {   /* beside mh2, then the folder above it */
        snprintf(dir, sizeof(dir), "%s", base);
        size_t ln = strlen(dir);
        while (ln > 1 && (dir[ln - 1] == '/' || dir[ln - 1] == '\\'))
            dir[--ln] = 0;
        look_in(dir, out, &n, max);
        char *cut = NULL;
        for (char *p = dir; *p; p++)
            if (*p == '/' || *p == '\\')
                cut = p;
        if (cut && cut != dir) {
            *cut = 0;
            look_in(dir, out, &n, max);
        }
    }
    const char *env = hfs_getenv("MH2_ORIG_DIR");
    if (env && *env && is_dir(env))
        look_in(env, out, &n, max);
    return n;
}
