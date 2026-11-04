#ifndef MH2_SND_IOP_H
#define MH2_SND_IOP_H

/*
 * The sound module's internal header: the IOP's memory map and hardware (iop_hw.c), its kernel services (iop_kern.c), its
 * modules (iop_mod.c) and the SPU2 (spu2.c). The original's iop.h, adapted: the names carry the module's
 * prefixes, the original's runtime (rt.h) is replaced by snd.h, snd_kernel.h and host_snd.h. The original's R3000A
 * interpreter is not in the core: the sound modules are native.
 */

#include "snd/snd.h"
#include "snd/snd_kernel.h"
#include "snd/host_snd.h"

#ifndef NULL
#define NULL ((void *)0)
#endif
#define NS_PER_SEC 1000000000ull

/* ---- logs (the original's LOGE ... LOGT) ---- */
#define SND_LOGE(sys, ...) SndHost_Log(SND_LOG_ERR, sys, __VA_ARGS__)
#define SND_LOGW(sys, ...) SndHost_Log(SND_LOG_WARN, sys, __VA_ARGS__)
#define SND_LOGI(sys, ...) do { if (SndHost_LogLevel() >= SND_LOG_INFO) SndHost_Log(SND_LOG_INFO, sys, __VA_ARGS__); } while (0)
#define SND_LOGD(sys, ...) do { if (SndHost_LogLevel() >= SND_LOG_DEBUG) SndHost_Log(SND_LOG_DEBUG, sys, __VA_ARGS__); } while (0)
#define SND_LOGT(sys, ...) do { if (SndHost_LogLevel() >= SND_LOG_TRACE) SndHost_Log(SND_LOG_TRACE, sys, __VA_ARGS__); } while (0)
/* A warning once per key (the original's rt_log_once). */
int SndLog_Once(u32 key);
#define SND_LOG_ONCE(sys, key, ...) do { if (SndLog_Once(key)) SndHost_Log(SND_LOG_WARN, sys, __VA_ARGS__); } while (0)
void SndLog_Fatal(const char *sys, const char *msg) __attribute__((noreturn));

/* ---- small library: the platform layer has no libc (pc/README.md) ---- */
#define snd_memcpy __builtin_memcpy
#define snd_memset __builtin_memset
#define snd_memmove __builtin_memmove
static inline int snd_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
static inline int snd_strncmp(const char *a, const char *b, u32 n)
{
    for (u32 i = 0; i < n; i++) {
        if (a[i] != b[i])
            return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
        if (!a[i])
            break;
    }
    return 0;
}
static inline u32 snd_strlen(const char *s)
{
    u32 n = 0;
    while (s[n])
        n++;
    return n;
}
/* Copies at most cap - 1 characters and terminates. */
static inline void snd_strcpy_n(char *d, const char *s, u32 cap)
{
    u32 i = 0;
    for (; cap && i + 1 < cap && s[i]; i++)
        d[i] = s[i];
    if (cap)
        d[i] = 0;
}

/* ---- the IOP's registers, as the kernel's handlers and the native libsd take their arguments ---- */

/* The registers of a call: the arguments in r[4] to r[7] (the rest on the stack at r[29] + 16), the result in r[2], gp in
 * r[28] and the stack pointer in r[29] (the ABI of the IOP's MIPS). A thread's block holds its own; the kernel's handlers
 * read their arguments through it, and a native call builds one (IopNative_CallN). */
typedef struct IopCpu {
    u32 r[32];
} IopCpu;

/* The native libsd (pc/core/api/libsd.c), set by Libsd_Setting (from Kern_InitSnd): a native
 * module's call of libsd's export number `index` goes to `call` (IopNative_Sd; one call point at its return, as a
 * kernel call), and call_handler sends a handler address IOP_NATIVE_INTR + n to `intr`. */
typedef struct IopNativeLib {
    void (*call)(IopCpu *cpu, u32 index);
    u32 (*intr)(u32 func, u32 arg);
} IopNativeLib;
extern const IopNativeLib *g_iop_native_libsd;
#define IOP_NATIVE_INTR 0x1FC0E000u
/* A kernel call from native code: the function g_kern_entries names NAME, with the caller's gp and sp (NULL: an
 * interrupt context); its v0. It blocks the calling thread. */
u32 IopKern_Call(const IopCpu *caller, const char *name, u32 a0, u32 a1, u32 a2, u32 a3);
/* A callback called from a native interrupt handler: the native function func(a0, a1), in the interrupt
 * context; its v0. */
u32 IopNative_CallbackIntr(u32 func, u32 a0, u32 a1, u32 gp);
/* The native CRI_ADX_Driver (pc/core/cse/adx_iop.c) and Capcom Sound Engine (cse/cse_iop.c), set by Libsd_Setting. The
 * module's image is in IOP memory (its header gives the size of the heap block the original's module takes), but not run:
 * setup gets the block's base and size, and returns the offset of the module's start function, which the module's native
 * functions answer (IopNative_Map): a thread entry, an RPC server function or a callback at that IOP address calls C. */
typedef u32 (*IopNativeFn)(u32 a0, u32 a1, u32 a2, u32 a3);
typedef struct IopNativeModule {
    const char *name;                      /* the IRX module's name */
    u32 (*setup)(u32 base, u32 size);      /* maps the native functions; the start function's offset from base */
} IopNativeModule;
extern const IopNativeModule *g_iop_native_adx;
extern const IopNativeModule *g_iop_native_cse;
u32 IopNative_Sd5(u32 index, u32 a0, u32 a1, u32 a2, u32 a3, u32 a4);
u32 IopNative_StackTop(void);   /* the running thread's stack top (its locals' addresses are the module's, as the original game has them) */
void IopNative_Map(u32 addr, IopNativeFn fn);
/* From a native function that runs as an IOP thread: a kernel function by name (the thread blocks in it as from guest
 * code, and its return is a call point), and a call of the native libsd's export n (its return a call point). */
u32 IopNative_Call(const char *name, u32 a0, u32 a1, u32 a2, u32 a3);
u32 IopNative_CallN(const char *name, const u32 *a, int n);   /* up to 7 arguments */
u32 IopNative_Sd(u32 index, u32 a0, u32 a1, u32 a2, u32 a3);
u32 IopNative_CallIntr(u32 func, u32 a0, u32 a1, u32 a2, u32 a3);   /* a native function, in interrupt context */
int IopNative_IsMapped(u32 addr);
/* The sound's setting (api/libsd.c): "cse" (then g_iop_native_libsd, g_iop_native_adx and g_iop_native_cse are set). */
const char *Libsd_Setting(void);
void Libsd_Option(int argc, char **argv, int i);   /* main.c: --audio cse (lle, libsd and adx are refused) */
extern u32 g_iop_libsd_lo, g_iop_libsd_size;

/* ---- memory map (iop_hw.c) ---- */
u8 IopMem_Read8(u32 addr);
u16 IopMem_Read16(u32 addr);
u32 IopMem_Read32(u32 addr);
void IopMem_Write8(u32 addr, u8 v);
void IopMem_Write16(u32 addr, u16 v);
void IopMem_Write32(u32 addr, u32 v);
static inline u32 iop_phys(u32 addr) { return addr & 0x1FFFFFFFu; }

/* ---- hardware (iop_hw.c): the interrupt lines and the DMA channels ---- */
void IopHw_Init(void);
void IopHw_RaiseIrq(int irq);     /* 0-31: INTC lines, 32+: DMA channel completion */
u64 IopHw_PendingIrqs(void);      /* raised, not yet acknowledged */
void IopHw_AckIrq(int irq);

/* ---- the IOP kernel (iop_kern.c) and its modules (iop_mod.c) ---- */
void IopKern_Run(void);               /* run the ready threads and the interrupts, within the budget */
void IopKern_Stats(void);             /* the threads and the SPU2, at info level */
/* A module's start function on a start thread (modload's StartModule for a module loaded from a buffer): the native
 * function at `entry`, called with (argc, argv); the IOP then runs until the threads it started have waited. 0, with the
 * function's v0 in *res and *returned saying whether it returned; -1 when no thread is free. */
int IopKern_StartModule(u32 entry, u32 gp, u32 argc, u32 argv, int *res, int *returned);

/* ---- the SPU2 (spu2.c) ---- */
void Spu2_Init(void);
u16 Spu2_Read(u32 off);           /* off: byte offset in the 0x1F900000 window */
void Spu2_Write(u32 off, u16 v);
/* The SPU2's DMA channels (IOP DMA 4 = core 0, 7 = core 1): reg 0 MADR,
 * 4 BCR, 8 CHCR; `mask` selects the bits a narrow write changes. */
u32 Spu2_DmaRead(int core, u32 reg);
void Spu2_DmaWrite(int core, u32 reg, u32 v, u32 mask);
void Spu2_Stats(void);

#endif
