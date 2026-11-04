/*
 * The run: the kernel's start-up, the game's main thread running ps2_main,
 * and the run loop. What the PS2's start-up does
 * before main that the game can see is done here, in its order (MainThread),
 * then main(1, {"cdrom0:\SLPM_662.80;1"}) on thread 1, its arguments in
 * crt0's argument block in the RAM block (start.c's KStart_PutArgs).
 */
#include "kernel/kern_int.h"
#include "kernel/klog.h"
#include "crt/crt_kernel.h"
#include "pal_core.h"
#include "host.h"
#include "io/io.h"

int ps2_main(int argc, char **argv);

static s32 sLockSema;

/* The SDK's thread 2 (priority 0), which the C library's start-up creates and starts: it waits on its
 * semaphore (id 3, the maximum 255). */
static s32 sTopSema;

static void TopThread(void *arg)
{
    (void)arg;
    for (;;) {
        Kern_SemaWait(sTopSema);
    }
}

/*
 * What the PS2's start-up does before main that the game can see (observed in the original game with gdb and
 * --log debug, the frame clock, 2026-10-26; the notes, session 1), in its order, on thread 1 at priority 0:
 * the C library's lock (semaphore 1) and a second semaphore (2); the alarm module's set-up (handler 1 on
 * Timer 2, alarm.c); semaphore 3 (initial 0, maximum 255) and thread 2 (priority 0) started; then the main
 * thread's priority becomes 1, so thread 2 runs at once, waits on semaphore 3 (a blocking wait: a delivery
 * point), and main(1, argv) follows.
 */
static void MainThread(void *arg)
{
    KThreadParam p;
    s32 top;
    char **argv;
    (void)arg;
    argv = KStart_PutArgs();   /* crt0's SetupThread, first: main's arguments in the RAM block */
    sLockSema = Kern_SemaCreate(1, 1, 0, 0x001BEB60u);
    Kern_SemaCreate(1, 1, 0, 0x001BEB70u);
    KAlarm_Init();
    sTopSema = Kern_SemaCreate(0, 255, 0, 0);
    __builtin_memset(&p, 0, sizeof(p));
    p.entry = TopThread;
    p.stack = PAL_RAM(0x001C9360u);     /* the stack the PS2's thread 2 has (masked: it runs on a fiber) */
    p.stackSize = 0x400;
    p.initPriority = 0;
    top = KThread_Create(&p);
    KThread_Start(top, 0);
    KThread_ChangePriority(KThread_GetId(), 1);
    ps2_main(1, argv);
    /* Capcom's main never returns (sub_main_entry loops); a return is the thread's exit */
}

/*
 * The boot executable's segment in the RAM block, as the PS2's loader leaves it before main (the original's
 * main.c load_elf): each loadable segment with file data copied to its address, the rest of its memory size
 * cleared; the overlay windows (no file data) are the game's. The executable comes from the disc (io), and
 * must be the v1.04 one the decomp matches (its size and FNV-1a 64, as the original game checks).
 */
#define ELF_SIZE 822368u
#define ELF_FNV1A 0x0d2f96ab77367ec7ull

static u32 Rd32(const u8 *p)
{
    return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static void LoadBootElf(void)
{
    u32 lba, size, phoff, phnum, i;
    u64 h = 0xcbf29ce484222325ull;
    u8 *elf;
    if (Io_DiscFind("cdrom0:\\SLPM_662.80;1", &lba, &size) != 0) {
        Pal_Fatal("kernel: SLPM_662.80 is not on the disc");
    }
    elf = (u8 *)Host_Alloc(size ? size : 1);
    if (!elf || Io_DiscReadBytes(lba, 0, size, elf) != 0) {
        Pal_Fatal("kernel: SLPM_662.80 could not be read");
    }
    for (i = 0; i < size; i++) {
        h = (h ^ elf[i]) * 0x100000001b3ull;
    }
    if (size != ELF_SIZE || h != ELF_FNV1A) {
        Pal_Fatal("kernel: SLPM_662.80 is not the v1.04 executable the decomp matches");
    }
    phoff = Rd32(elf + 0x1C);
    phnum = (u32)elf[0x2C] | (u32)elf[0x2D] << 8;
    for (i = 0; i < phnum; i++) {
        const u8 *ph = elf + phoff + i * 32;
        u32 off = Rd32(ph + 4), vaddr = Rd32(ph + 8), filesz = Rd32(ph + 16), memsz = Rd32(ph + 20);
        if (Rd32(ph) != 1 || !filesz) {
            continue;
        }
        if (vaddr >= sizeof(Pal_EeRam) || memsz > sizeof(Pal_EeRam) - vaddr || filesz > memsz || off > size ||
            filesz > size - off) {
            Pal_Fatal("kernel: SLPM_662.80 has a segment outside the RAM block");
        }
        __builtin_memcpy(Pal_EeRam + vaddr, elf + off, filesz);
        __builtin_memset(Pal_EeRam + vaddr + filesz, 0, memsz - filesz);
        if (g_kern_log) {
            KLog("kernel: ELF segment %08X: %X bytes (%X in memory)", vaddr, filesz, memsz);
        }
    }
    Host_Free(elf);
}

void Kern_Boot(u64 maxFrames, int argc, char **argv)
{
    Kern_Start(maxFrames, argc, argv);
    LoadBootElf();
    KThread_CreateMain(MainThread, 0, 0);
    KThread_RunIdle();
    for (;;) {
    }
}

/* ps2crt's kernel calls (crt_kernel.h). */
crt_u32 crt_k_thread_id(void)
{
    return (crt_u32)Kern_ThreadId();
}

crt_u32 crt_k_lock_sema(void)
{
    return (crt_u32)sLockSema;
}

void crt_k_sema_wait(crt_u32 sema)
{
    Kern_SemaWait((s32)sema);
}

void crt_k_sema_signal(crt_u32 sema)
{
    Kern_SemaSignal((s32)sema);
}
