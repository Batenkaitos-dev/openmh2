/*
 * mh2-core's entry point. The game's own main is ps2_main (game_flags.txt
 * renames it): it runs on the game's main thread (thread 1), which the kernel
 * creates (pc/core/kernel/run.c). Options:
 *   --check       the start-up checks only (the low-256 MB rule, the overlays' snapshot)
 *   --frames N    stop at VBlank N
 *   --local-date  play's calendar: the host's date and time zone, read once
 *                 here. Without it, the fixed date in JST (kern.h)
 *                 is used, the same on every run.
 *   --widescreen  the camera's aspect 16:9 (pal_camera.c)
 *   snd's (Libsd_Option: --audio cse)
 *   io's (Io_Init: --disc, --memcard1, --memcard2)
 * Before the game's main the boot executable's segment is loaded into the RAM
 * block, as the PS2's loader does (kernel/run.c).
 */
#include "pal_core.h"

#include "pal_camera.h"
#include "host.h"
#include "kernel/kern_int.h"
#include "ipc/core_ipc.h"

static int StrEq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static u64 ParseU64(const char *s)
{
    u64 v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (u64)(*s++ - '0');
    }
    return v;
}

/*
 * Every fatal end goes through here, as through Pal_Fatal: Host_Fatal's own
 * callers too (the kernel's KFatal, the movie's table, the IPC, snd's host
 * part). The message goes on to the IPC (Ipc_Fatal).
 * Host_Fatal clears the hook before calling it,
 * so a fatal inside it ends the run as it is.
 */
static void HOST Main_Fatal(const char *msg)
{
    Ipc_Fatal(msg);
}

int main(int argc, char **argv)
{
    u64 frames = 0;
    int i;
    Host_SetEndHooks(Main_Fatal, Ipc_Exit);
    Pal_CheckLowMemory();
    Pal_OverlayInit();
    for (i = 1; i < argc; i++) {
        if (StrEq(argv[i], "--check")) {
            Host_Log("startup checks passed");
            return 0;
        }
        if (StrEq(argv[i], "--frames") && i + 1 < argc) {
            frames = ParseU64(argv[++i]);
            continue;
        }
        if (StrEq(argv[i], "--local-date")) {
            Kern_SetCalendarLocal();
            continue;
        }
        if (Pal_WidescreenOption(argc, argv, i)) {   /* the camera's aspect */
            continue;
        }
        { void Libsd_Option(int argc, char **argv, int i); Libsd_Option(argc, argv, i); }   /* --audio */
    }
    Pal_WidescreenStart();   /* the setting, once the options are read */
    Kern_Boot(frames, argc, argv);
}
