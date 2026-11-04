/*
 * The movies' core side (ipc_movie.c) against stand-ins for io, the kernel's
 * events and the IPC: the status the game sees (the original's on the frame
 * clock), the clock from the SPU2's blocks, the AFS header's read, and what
 * the host is told.
 */
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "host.h"
#include "io/io.h"
#include "ipc/core_ipc.h"
#include "kernel/kern.h"
#include "snd/snd_kernel.h"
#include "pal_movie.h"

void Movie_VBlankEnd(u64 vblank);

static int sFails;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("FAIL line %d: ", __LINE__);                                                                        \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
            sFails++;                                                                                                  \
        }                                                                                                              \
    } while (0)

/* ---- stand-ins ---- */

static const char kTable[] = "# a test table\n"
                             "mh2-movies 1 afs02 100 50000 3\n"
                             "0 4096 1000 10 2000 1500 0.04\n"
                             "1 8192 1000 -1 0 -1 0.0\n"
                             "2 12288 1000 4 100 3000 0.002\n";

static int sReads, sOpens, sPlays, sCloses, sLastClock = -2;
static unsigned int sOpenId, sOpenSize, sOpenMode, sPlayId, sCloseId, sAudio;
static unsigned long long sOpenOffset;
static int sOpenVolume;

HOST void Host_Log(const char *msg) { printf("  log: %s\n", msg); }
HOST void *Host_Alloc(unsigned int size) { return malloc(size); }
HOST void Host_Free(void *p) { free(p); }

/* The fatal error: back to the test (the core would end). */
static jmp_buf sFatalJmp;
static const char *sFatalMsg;
HOST void Host_Fatal(const char *msg)
{
    printf("  fatal: %s\n", msg);
    sFatalMsg = msg;
    longjmp(sFatalJmp, 1);
}

static int sTableMode;   /* 0 the table, 1 none, 2 another version's */

int Io_DiscSideFile(const char *name, void **data, u32 *size)
{
    static const char kOld[] = "mh2-movies 0\n";
    const char *t = sTableMode == 2 ? kOld : kTable;
    size_t n = sTableMode == 2 ? sizeof(kOld) : sizeof(kTable);
    char *p;
    CHECK(strcmp(name, "mh2-movies.txt") == 0, "the table's name %s", name);
    if (sTableMode == 1)
        return -1;
    p = malloc(n);
    memcpy(p, t, n);
    *data = p;
    *size = (u32)n - 1;
    return 0;
}

/* A play that must end in the fatal error whose message holds WHAT. */
static void PlayFatal(int entry, const char *what)
{
    sFatalMsg = NULL;
    if (!setjmp(sFatalJmp))
        Pal_MoviePlay(entry, 0, 0);
    CHECK(sFatalMsg && strstr(sFatalMsg, what), "fatal (%s): %s", what, sFatalMsg ? sFatalMsg : "none");
    Pal_MovieStop();
}
int Io_DiscLocation(u32 lba, const char **path, u64 *offset)
{
    CHECK(lba == 100, "the location of sector %u", lba);
    *path = "/x/mh2-disc.arc";
    *offset = 1000000;
    return 0;
}
int Io_DiscFind(const char *path, u32 *lba, u32 *size)
{
    CHECK(strcmp(path, "/AFS02.AFS") == 0, "found %s", path);
    *lba = 100;
    *size = 50000;
    return 0;
}
int Io_DiscReadSectors(u32 lba, u32 count, void *dst)
{
    static const u32 head[] = {0x00534641, 3, 4096, 1000, 8192, 1000, 12288, 1000};
    CHECK(lba == 100 && count == 2, "read %u x %u", lba, count);
    memset(dst, 0, count * IO_SECTOR);
    memcpy(dst, head, sizeof(head));
    sReads++;
    return 0;
}
void Ipc_MovieOpen(unsigned int id, const char *path, unsigned long long offset, unsigned int size, unsigned int mode,
                   int volume)
{
    CHECK(strcmp(path, "/x/mh2-disc.arc") == 0, "open %s", path);
    sOpens++;
    sOpenId = id;
    sOpenOffset = offset;
    sOpenSize = size;
    sOpenMode = mode;
    sOpenVolume = volume;
}
void Ipc_MoviePlay(unsigned int id)
{
    sPlays++;
    sPlayId = id;
}
void Ipc_MovieClock(int clock) { sLastClock = clock; }
void Ipc_MovieClose(unsigned int id)
{
    sCloses++;
    sCloseId = id;
}
void Ipc_AudioWrite(const short *samples, unsigned int frames)
{
    (void)samples;
    sAudio += frames;
}

/* One VBlank: its end's hook (the kernel's KERN_VB_END). */
static void VBlank(u64 n)
{
    Movie_VBlankEnd(n);
}

static void Spu2(u32 frames)
{
    static s16 buf[2 * 800];
    SndSys_OutputSamples(buf, frames);
}

int main(void)
{
    u64 n = 1;
    /* no table, or one of another version: the core's fatal error, never a silent FINISHED */
    sTableMode = 1;
    PlayFatal(0, "no mh2-movies.txt");
    sTableMode = 2;
    PlayFatal(0, "not a table of this version");
    sTableMode = 0;
    sReads = 0;   /* the two plays above took ids 1 and 2 */
    CHECK(Pal_MovieStatus() == 0, "stopped at first");
    VBlank(n++);
    CHECK(sLastClock == -1, "no movie: no clock (%d)", sLastClock);

    /* movie 0: 10 frames, every frame shown at 2000, sound 1500 */
    Pal_MoviePlay(0, 2, -100);
    CHECK(Pal_MovieStatus() == 1 && sReads == 1, "play: preparing (%d), the header read", Pal_MovieStatus());
    CHECK(sOpens == 1 && sOpenId == 3 && sOpenOffset == 1004096 && sOpenSize == 1000 && sOpenMode == 2 &&
              sOpenVolume == -100,
          "the host's open: id %u offset %llu size %u mode %u volume %d", sOpenId, sOpenOffset, sOpenSize, sOpenMode,
          sOpenVolume);
    Spu2(800);   /* before the VBlank end: not the movie's */
    VBlank(n++);
    CHECK(Pal_MovieStatus() == 2 && sPlays == 1 && sPlayId == 3 && sLastClock == 0, "playing at the VBlank end (%d %d)",
          Pal_MovieStatus(), sLastClock);
    Spu2(512);
    Spu2(288);
    VBlank(n++);
    CHECK(Pal_MovieStatus() == 2 && sLastClock == 800, "clock 800 (%d)", sLastClock);
    Spu2(800);
    VBlank(n++);
    CHECK(Pal_MovieStatus() == 2 && sLastClock == 1600, "clock 1600: still playing (%d)", sLastClock);
    Spu2(399);
    VBlank(n++);
    CHECK(Pal_MovieStatus() == 2 && sLastClock == 1999, "clock 1999: a frame left (%d)", sLastClock);
    Spu2(1);
    VBlank(n++);
    CHECK(Pal_MovieStatus() == 3 && sLastClock == -1, "clock 2000: finished (%d, %d)", Pal_MovieStatus(), sLastClock);
    Spu2(800);
    VBlank(n++);
    CHECK(Pal_MovieStatus() == 3, "finished stays");
    Pal_MovieStop();
    CHECK(Pal_MovieStatus() == 0 && sCloses == 1 && sCloseId == 3, "stopped, the host told");
    CHECK(sAudio == 3600, "every block to the host (%u)", sAudio);

    /* movie 2: the sound is the longer */
    Pal_MoviePlay(2, 0, 0);
    VBlank(n++);
    Spu2(2999);
    VBlank(n++);
    CHECK(Pal_MovieStatus() == 2, "the sound plays on");
    Spu2(1);
    Pal_MovieStop();   /* skipped before the VBlank end */
    CHECK(Pal_MovieStatus() == 0 && sCloses == 2 && sCloseId == 4, "skipped");
    VBlank(n++);
    CHECK(Pal_MovieStatus() == 0 && sLastClock == -1, "skipped stays stopped");

    /* movie 1: the library can't play it; entry 7: none. The game sees finished at once. */
    Pal_MoviePlay(1, 0, 0);
    CHECK(Pal_MovieStatus() == 3 && sOpens == 2, "unplayable: finished, not opened");
    VBlank(n++);
    CHECK(Pal_MovieStatus() == 3 && sLastClock == -1, "unplayable: no picture");
    Pal_MoviePlay(7, 0, 0);
    CHECK(Pal_MovieStatus() == 3 && sCloses == 2, "no such entry: finished (the error isn't opened, so not closed)");
    Pal_MovieStop();
    CHECK(Pal_MovieStatus() == 0, "stopped");

    printf("movie_test: %s\n", sFails ? "FAILED" : "OK");
    return sFails ? 1 : 0;
}
