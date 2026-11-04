/*
 * mh2-fake-core: a stand-in for mh2-core that drives the host through the
 * IPC (core_ipc.h), for the host's own checks while the game can't run yet.
 *
 *   mh2 --core build/pc/<platform>-i686/core/mh2-fake-core [-- VBLANKS]
 *
 * Each VBlank (paced at 60 Hz by the host's clock: a test program, not the
 * game) it draws a test picture with the GS (public GS registers: a
 * 640x448 display, a background, eight bars, a square that moves, and a
 * square that lights up while a pad button is held), plays a quiet tone,
 * and once a second reads back an 8x8 block of the picture. It ends after
 * VBLANKS VBlanks (0 or none: until the window closes).
 *
 * A movie (the host's player): with MH2_FAKE_MOVIE=FILE and
 * MH2_FAKE_MOVIE_AT="OFFSET SIZE" (the Sofdec stream's bytes in FILE), it
 * opens the movie at VBlank 10, plays it from VBlank 11 with the movie
 * clock running at 800 frames a VBlank, as the core does, and closes it at
 * VBlank 610.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

#include "ipc/core_ipc.h"

typedef unsigned long long u64;

static u64 now_ns(void)
{
#ifdef _WIN32
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (u64)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (u64)t.tv_sec * 1000000000ull + (u64)t.tv_nsec;
#endif
}

static void sleep_until(u64 t)
{
    for (;;) {
        u64 n = now_ns();
        if (n >= t)
            return;
#ifdef _WIN32
        Sleep((DWORD)((t - n) / 1000000ull));
        if (t - n < 1000000ull)
            return;
#else
        struct timespec d = {(time_t)((t - n) / 1000000000ull), (long)((t - n) % 1000000000ull)};
        nanosleep(&d, NULL);
#endif
    }
}

/* A GIF packet of A+D writes (PACKED, one register descriptor: A+D). */
static u64 pkt[2 * 256];
static unsigned int npkt;

static void ad(unsigned int reg, u64 v)
{
    pkt[2 + npkt * 2] = v;
    pkt[3 + npkt * 2] = reg;
    npkt++;
}

static void send(void)
{
    pkt[0] = (u64)npkt | 1ull << 15 | 1ull << 60;   /* NLOOP, EOP, PACKED, NREG 1 */
    pkt[1] = 0xE;                                   /* A+D */
    Ipc_GsGif(3, pkt, 1 + npkt);
    npkt = 0;
}

static void sprite(int x0, int y0, int x1, int y1, unsigned int rgb)
{
    ad(0x00, 6);                                                              /* PRIM: sprite */
    ad(0x01, (u64)(rgb & 0xFFFFFF) | 0x80ull << 24 | (u64)0x3F800000 << 32);  /* RGBAQ, Q = 1.0 */
    ad(0x05, (u64)(x0 << 4) | (u64)(y0 << 4) << 16);                          /* XYZ2 */
    ad(0x05, (u64)(x1 << 4) | (u64)(y1 << 4) << 16);
}

int main(int argc, char **argv)
{
    unsigned long long frames = argc > 1 ? strtoull(argv[argc - 1], NULL, 10) : 0;
    int attached = Ipc_Init();
    printf("mh2-fake-core: %s\n", attached ? "attached to the host" : "headless");
    fflush(stdout);
    /* the display: 640x448 interlaced (NTSC), the frame buffer at 0 */
    Ipc_GsPrivWrite(0x12000000u, 0xFF25);                    /* PMODE: circuit 1, ALP 0xFF */
    Ipc_GsPrivWrite(0x12000010u, 4u | 32u << 3 | 1u << 10 | 2u << 13 | 4u << 21);   /* SMODE1: NTSC (CMOD 2, LC 32) */
    Ipc_GsPrivWrite(0x12000020u, 1);                         /* SMODE2: interlaced, field */
    Ipc_GsPrivWrite(0x12000070u, 10u << 9);                  /* DISPFB1: FBW 10, PSMCT32 */
    Ipc_GsPrivWrite(0x12000080u, 652u | 50u << 12 | 3u << 23 | (u64)2559 << 32 | (u64)447 << 44);   /* DISPLAY1 */
    Ipc_GsPrivWrite(0x120000E0u, 0);                         /* BGCOLOR */
    static const unsigned int bars[8] = {0xFFFFFF, 0x00FFFF, 0xFFFF00, 0x00FF00, 0xFF00FF, 0x0000FF, 0xFF0000, 0x000000};
    static short tone[2 * 800];
    unsigned int phase = 0, reads = 0;
    u64 start = now_ns();
    u64 vblank;
    const char *movie = getenv("MH2_FAKE_MOVIE"), *movie_at = getenv("MH2_FAKE_MOVIE_AT");
    unsigned long long movie_off = 0, movie_size = 0;
    if (movie && movie_at)
        movie_size = sscanf(movie_at, "%llu %llu", &movie_off, &movie_size) == 2 ? movie_size : 0;
    int movie_clock = -1;
    /* MH2_FAKE_FATAL=N: the core's fatal end at VBlank N (Host_Fatal's message to the host), with
     * status 0, so the host's own error code is what a test sees */
    const char *fatal_at = getenv("MH2_FAKE_FATAL");
    for (vblank = 0; !frames || vblank < frames; vblank++) {
        Ipc_Pace(vblank + 1);   /* the kernel's numbering: its first VBlank is 1 */
        if (Ipc_QuitRequested())
            break;
        if (fatal_at && vblank == strtoull(fatal_at, NULL, 10)) {
            printf("mh2-fake-core: fatal at VBlank %llu\n", vblank);
            fflush(stdout);
            Ipc_Fatal("the fake core's fatal error");
            return 0;
        }
        IpcPad pad;
        Ipc_ReadPad(0, &pad);
        ad(0x4C, 10ull << 16);                                    /* FRAME_1: FBW 10 */
        ad(0x4E, 1ull << 32 | 0x70);                              /* ZBUF_1: masked */
        ad(0x40, 639ull << 16 | 447ull << 48);                    /* SCISSOR_1 */
        ad(0x18, 0);                                              /* XYOFFSET_1 */
        ad(0x47, 0x30000);                                        /* TEST_1: ZTE, ZTST always */
        ad(0x1A, 1);                                              /* PRMODECONT */
        sprite(0, 0, 640, 448, 0x302010);
        for (int i = 0; i < 8; i++)
            sprite(40 + i * 70, 40, 100 + i * 70, 300, bars[i]);
        int x = (int)(vblank * 4 % 560);
        sprite(40 + x, 330, 80 + x, 370, 0xFFFFFF);
        sprite(560, 380, 600, 420, pad.buttons ? 0x00FF00 : 0x404040);
        send();
        if (vblank % 60 == 59) {
            unsigned int rb[8 * 8];
            ad(0x50, 10ull << 16);                                /* BITBLTBUF: SBW 10, PSMCT32 */
            ad(0x51, 50ull | 100ull << 16);                       /* TRXPOS: (50, 100), a white bar */
            ad(0x52, 8ull | 8ull << 32);                          /* TRXREG: 8x8 */
            ad(0x53, 1);                                          /* TRXDIR: local -> host */
            send();
            int from_host = Ipc_GsReadback(rb, sizeof(rb) / 16);
            if (reads++ < 3) {
                printf("mh2-fake-core: readback at VBlank %llu: %s, pixel 0 = %08X\n", vblank,
                       from_host ? "from the renderer" : "zeros", rb[0]);
                fflush(stdout);
            }
        }
        for (int i = 0; i < 800; i++) {
            short v = (short)(1500.0 * sin(phase++ * (2.0 * 3.14159265358979 * 440.0 / 48000.0)));
            tone[i * 2] = tone[i * 2 + 1] = v;
        }
        if (movie_size && vblank == 10)
            Ipc_MovieOpen(1, movie, movie_off, (unsigned int)movie_size, 0, 0);
        if (movie_size && vblank == 11) {
            Ipc_MoviePlay(1);
            movie_clock = 0;
        } else if (movie_clock >= 0 && vblank == 610) {
            Ipc_MovieClose(1);
            movie_clock = -1;
        } else if (movie_clock >= 0) {
            movie_clock += 800;
        }
        Ipc_MovieClock(movie_clock);
        Ipc_AudioWrite(tone, 800);
        Ipc_GsVSync((unsigned int)(vblank & 1), vblank);
        sleep_until(start + (vblank + 1) * 16683333ull);
    }
    printf("mh2-fake-core: %llu VBlanks\n", vblank);
    fflush(stdout);
    Ipc_Exit(0);
    return 0;
}
