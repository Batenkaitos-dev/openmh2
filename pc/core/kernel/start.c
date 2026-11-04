/*
 * The kernel's start-up (Kern_Start): its state, VBlank 1 queued, the run's
 * stop, then the other modules' start-up: the host's connection
 * (Kern_InitIpc), io's and gfx's (called directly), then the other weak
 * hooks of kern.h, in that order.
 */
#include "kernel/kern_int.h"
#include "kernel/klog.h"
#include "kernel/khost.h"
#include "host.h"
#include "io/io.h"
#include "gfx/gfx.h"
#include "pal_core.h"
#include "crt/crt_kernel.h"

/* The modules' start-up hooks: weak defaults, which a module replaces with its own. */
__attribute__((weak)) void Kern_InitIo(void) {}
__attribute__((weak)) void Kern_InitGfx(void) {}
__attribute__((weak)) void Kern_InitSnd(void) {}
__attribute__((weak)) void Kern_InitVerify(void) {}
__attribute__((weak)) void Kern_InitIpc(void) {}

int g_kern_log;

/*
 * A crash (khost.h, HostK_CatchCrashes): best effort, since the process is
 * dying. Where it happened (the core's map, build/pc/.../mh2-core.map, or
 * addr2line names the function at pc), then the run's end as at --frames
 * (KVblank_Finish: the SPU2 to now, the run's last
 * VBlank, with the reason "crash"), then exit with
 * KERN_EXIT_CRASH.
 */
static void HOST KStart_OnCrash(int code, unsigned int faultAddr, unsigned int pc)
{
    KLog("crash: %s (code %x) at address %08x, pc %08x, thread %d, VBlank %llu",
         HostK_CrashName(code), (u32)code, faultAddr, pc, (int)Kern_ThreadId(), Kern_VblankCount());
    KVblank_Finish("crash");
    KLog("crash: verify's files finished, exiting with %d", KERN_EXIT_CRASH);
    HostK_Exit(KERN_EXIT_CRASH);
}

void Kern_Start(u64 maxFrames, int argc, char **argv)
{
    const char *e = Host_Getenv("MH2_KERNEL_LOG");
    HostK_CatchCrashes(KStart_OnCrash);
    g_kern_log = e && e[0] == '1';
    KSched_Reset();
    KThread_Reset();
    KTimer_Reset();
    KSif_Reset();
    KVblank_Reset();
    KVblank_SetStop(maxFrames);
    Kern_InitIpc();     /* the host's connection first, so that a fatal error from here on reaches it */
    Io_Init(argc, argv);
    Gfx_Init();
    Kern_InitIo();
    Kern_InitGfx();
    Kern_InitSnd();
    Kern_InitVerify();
}

/*
 * main's arguments where the PS2's have them: crt0's SetupThread argument
 * block (0x1C8640, libcrt0.c's ARGS), as the original's
 * SetupThread fills it (kernel.c, syscall 0x3C): argc 1 at +0, argv[] from
 * +4 (argv[0] = the block's +0x44, argv[1] 0), the string at +0x44. The
 * pointers are the core's (PAL_RAM). Returns argv, for main.
 */
#define ARGS_BLOCK 0x001C8640u

char **KStart_PutArgs(void)
{
    static const char argv0[] = "cdrom0:\\SLPM_662.80;1";
    u8 *a = Pal_EeRam + ARGS_BLOCK;
    u32 w;
    w = 1;
    __builtin_memcpy(a + 0x00, &w, 4);
    w = PAL_RAM(ARGS_BLOCK + 0x44);
    __builtin_memcpy(a + 0x04, &w, 4);
    w = 0;
    __builtin_memcpy(a + 0x08, &w, 4);
    __builtin_memcpy(a + 0x44, argv0, sizeof(argv0));
    return (char **)(a + 0x04);
}

/*
 * ps2crt's heap (crt_kernel.h): newlib's PS2 heap in the RAM block, from the
 * first break (0x01F48000) to EndOfHeap's answer (0x01FA8000), as host
 * pointers into it. These replace crt_k_default.c's weak PS2 addresses.
 * Here, with main's arguments, rather than with run.c's crt_k_* functions,
 * so that the kernel's tests link them against the weak defaults.
 */
crt_u32 crt_k_heap_start(void)
{
    return (crt_u32)PAL_RAM(0x01F48000u);
}

crt_u32 crt_k_heap_end(void)
{
    return (crt_u32)PAL_RAM(0x01FA8000u);
}
