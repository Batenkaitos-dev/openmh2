/*
 * The movies' core side: the game's
 * four Sofdec wrappers (src/sub_main/movie.c, under PLATFORM_PC) call
 * Pal_MoviePlay, Pal_MovieStop and Pal_MovieStatus (include/pal_movie.h)
 * in place of CRI's player, as the original's HLE replaces them
 *, so the game's gMoviePlayer is never set and its
 * per-frame movie function does nothing.
 *
 * The status the game sees is the original's on its frame clock, kept
 * here from the stream's numbers alone, headless or not:
 *  - play: PREPARING; the original's ERROR (no such movie, no library),
 *    which the game sees as FINISHED, when the movie is missing. A missing
 *    or unusable timing table is a fatal error (LoadTable).
 *  - PLAYING at the first VBlank end after play (the original's decoder
 *    has its first frame by then on the frame clock).
 *  - The movie clock: the SPU2's output frames (snd's blocks, which pass
 *    through SndSys_OutputSamples here) while PLAYING. The original game counts
 *    the same frames in movie_mix, at each flush of the SPU2's output.
 *  - FINISHED at the first VBlank end where the clock has reached both
 *    SHOWN (every frame shown: (double)clock / 48000 >= the last frame's
 *    time) and SOUND (the sound's length: the ring is empty), the numbers
 *    tools/pc/movie_table.py measured with the original's own library and
 *    wrote to mh2-movies.txt beside the disc archive.
 *  - The stop request and the stop: STOPPED.
 *
 * The VBlank end: the original game updates the movie in gs_vblank_end, first
 * thing at the VBlank's end; the kernel's KERN_VB_END hook runs first in its
 * VBlank end, before gfx's Gfx_VBlankEnd (ipc_glue.c sets Movie_VBlankEnd).
 *
 * The AFS header is read as the original game reads it (Io_DiscReadSectors of two
 * sectors at AFS02.AFS's first); the host
 * gets the stream's place in the archive from io (Io_DiscLocation) and
 * decodes and shows it (pc/host/movie.c), and mixes its sound.
 *
 * Platform-layer code (the game's flags): it touches no game memory.
 */
#include "common.h"
#include "host.h"
#include "io/io.h"
#include "ipc/core_ipc.h"
#include "kernel/kern.h"
#include "kernel/state.h"
#include "snd/snd_kernel.h"
#include "pal_movie.h"

enum { ST_STOPPED = 0, ST_PREPARING = 1, ST_PLAYING = 2, ST_FINISHED = 3, ST_ERROR = 4 };

#define MOVIE_TABLE "mh2-movies.txt"
#define MAX_MOVIES 64
#define AFS02 "/AFS02.AFS"

typedef struct MovieTiming {
    u32 offset, size;    /* the entry in AFS02.AFS */
    s32 frames;          /* -1: the library can't play it */
    u32 shown;           /* the clock at which every frame is shown */
    s32 sound;           /* the sound's length in 48 kHz frames; -1: none */
} MovieTiming;

static struct {
    int loaded;          /* 1: the table read */
    u32 lba, fileSize;   /* AFS02.AFS, as the table says */
    u32 count;
    MovieTiming m[MAX_MOVIES];
} sTable;

static struct {
    int active;
    int opened;          /* the host was told (Ipc_MovieOpen) */
    int status;
    u32 id;              /* movies opened so far (the host's) */
    MovieTiming t;
    u32 clock;
} sMv;

static u8 sAfsHead[2 * IO_SECTOR];

/* ---- the table ---- */

static void Log(const char *msg)
{
    Host_Log(msg);
}

static const char *SkipSpace(const char *p)
{
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}

/* A decimal number, maybe negative: the next character, or NULL. */
static const char *ParseNum(const char *p, s64 *out)
{
    int neg = 0, any = 0;
    s64 v = 0;
    p = SkipSpace(p);
    if (*p == '-') {
        neg = 1;
        p++;
    }
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p++ - '0');
        any = 1;
    }
    if (!any)
        return NULL;
    *out = neg ? -v : v;
    return p;
}

static const char *NextLine(const char *p)
{
    while (*p && *p != '\n')
        p++;
    return *p ? p + 1 : p;
}

static int ParseTable(const char *p)
{
    static const char kHead[] = "mh2-movies 1 afs02";
    s64 v[7];
    u32 i, n = 0;
    while (*p == '#')
        p = NextLine(p);
    for (i = 0; kHead[i]; i++)
        if (p[i] != kHead[i])
            return -1;
    p += i;
    for (i = 0; i < 3; i++)
        if (!(p = ParseNum(p, &v[i])))
            return -1;
    sTable.lba = (u32)v[0];
    sTable.fileSize = (u32)v[1];
    sTable.count = (u32)v[2];
    if (sTable.count > MAX_MOVIES)
        return -1;
    p = NextLine(p);
    while (*p && n < sTable.count) {
        const char *q = p;
        for (i = 0; i < 6; i++)
            if (!(q = ParseNum(q, &v[i])))
                return -1;
        if (v[0] != (s64)n || v[1] < 0 || v[2] < 0 || v[4] < 0)
            return -1;
        sTable.m[n].offset = (u32)v[1];
        sTable.m[n].size = (u32)v[2];
        sTable.m[n].frames = (s32)v[3];
        sTable.m[n].shown = (u32)v[4];
        sTable.m[n].sound = (s32)v[5];
        n++;
        p = NextLine(p);
    }
    return n == sTable.count ? 0 : -1;
}

/* The table, read once. Without it, or with one this core can't use, the
 * core would end the movie at once where the original game plays it, and diverge
 * from there with no word: a fatal error. */
static void LoadTable(void)
{
    void *data;
    u32 size;
    int bad;
    if (sTable.loaded)
        return;
    if (Io_DiscSideFile(MOVIE_TABLE, &data, &size) != 0)
        Host_Fatal("movie: no " MOVIE_TABLE " beside the disc archive: make it in the launcher (Disc and archive, "
                   "Make the movie table), and keep it beside the archive");
    bad = ParseTable((const char *)data) != 0;
    Host_Free(data);
    if (bad)
        Host_Fatal("movie: " MOVIE_TABLE " is not a table of this version: make it again in the launcher (Disc and "
                   "archive, Make the movie table)");
    sTable.loaded = 1;
}

static u32 Rd32(const u8 *p)
{
    return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

/* ---- the game's calls ---- */

static void Stop(void)
{
    if (sMv.opened)
        Ipc_MovieClose(sMv.id);
    sMv.opened = 0;
    sMv.active = 0;
    sMv.status = ST_STOPPED;
}

void Pal_MoviePlay(int entry, int mode, int volume)
{
    u32 lba, fsize, count, off, len;
    const char *path;
    u64 base;
    Stop();
    sMv.active = 1;
    sMv.status = ST_ERROR;
    sMv.clock = 0;
    sMv.id++;
    if (!sMv.id)
        sMv.id = 1;
    /* AFS: "AFS\0", the count, then (offset, size) per entry; read as the original game reads it */
    if (Io_DiscFind(AFS02, &lba, &fsize) != 0 || Io_DiscReadSectors(lba, 2, sAfsHead) != 0) {
        Log("movie: AFS02.AFS not found on the disc");
        return;
    }
    count = Rd32(sAfsHead + 4);
    off = len = 0;
    if (sAfsHead[0] == 'A' && sAfsHead[1] == 'F' && sAfsHead[2] == 'S' && sAfsHead[3] == 0 && entry >= 0 &&
        (u32)entry < count && 8 + 8 * (u32)entry + 8 <= sizeof(sAfsHead)) {
        off = Rd32(sAfsHead + 8 + 8 * entry);
        len = Rd32(sAfsHead + 8 + 8 * entry + 4);
    }
    if (!len || (u64)off + len > fsize) {
        Log("movie: no such entry in AFS02.AFS");
        return;
    }
    LoadTable();
    if (sTable.lba != lba || sTable.fileSize != fsize || (u32)entry >= sTable.count ||
        sTable.m[entry].offset != off || sTable.m[entry].size != len)
        Host_Fatal("movie: " MOVIE_TABLE " is from another disc: make it again in the launcher (Disc and "
                   "archive, Make the movie table)");
    sMv.t = sTable.m[entry];
    if (sMv.t.frames < 0) {
        Log("movie: the movie library can't play this movie (" MOVIE_TABLE ")");
        return;
    }
    if (sMv.t.sound < 0) {
        /* the original's clock is the host's time then: nothing to reproduce (no movie of the disc) */
        Log("movie: a movie without sound");
        return;
    }
    if (Io_DiscLocation(lba, &path, &base) == 0) {
        Ipc_MovieOpen(sMv.id, path, base + off, len, (u32)mode, volume);
        sMv.opened = 1;
    }
    sMv.status = ST_PREPARING;
}

void Pal_MovieStop(void)
{
    Stop();
}

int Pal_MovieStatus(void)
{
    if (!sMv.active)
        return ST_STOPPED;
    return sMv.status == ST_ERROR ? ST_FINISHED : sMv.status;
}

/* kernel/state.h: a save state is not made, or loaded, while a movie is open (the host's player is not in it). */
int Kern_StateBlocked(void)
{
    return sMv.active;
}

/* ---- the clock and the VBlank end ---- */

/* snd's SPU2 output blocks (snd_kernel.h): the movie clock counts them while it plays (the
 * original's movie_mix), and they go on to the host's sound device. */
void SndSys_OutputSamples(const s16 *lr, u32 frames)
{
    if (sMv.active && sMv.status == ST_PLAYING)
        sMv.clock += frames;
    Ipc_AudioWrite(lr, frames);
}

/* The kernel's KERN_VB_END hook (ipc_glue.c), with the ending VBlank's number. */
void Movie_VBlankEnd(u64 vblank)
{
    (void)vblank;
    if (sMv.active && sMv.status == ST_PREPARING && sMv.t.frames > 0) {
        sMv.status = ST_PLAYING;
        sMv.clock = 0;
        Ipc_MoviePlay(sMv.id);
    }
    if (sMv.active && sMv.status == ST_PLAYING && sMv.clock >= sMv.t.shown && sMv.clock >= (u32)sMv.t.sound)
        sMv.status = ST_FINISHED;
    Ipc_MovieClock(sMv.active && sMv.status == ST_PLAYING ? (int)sMv.clock : -1);
}

