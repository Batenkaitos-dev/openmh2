/*
 * Fibers: one host stack per game thread, all on one host thread, switched
 * by hand. The stacks come
 * from the low pool (Pal_LowAlloc), so that everything the game can see lies
 * below 0x10000000 (3.3): a thread passes the addresses of its locals on, and
 * the DMA code masks addresses. Windows' own fibers can't be given a stack at
 * an address of ours, so both systems use the switch below.
 *
 * The switch saves what the i386 calling conventions (cdecl, regcall) keep
 * across a call: ebx, esi, edi, ebp, the x87 control word and MXCSR; on
 * Windows also the thread information block's exception list and stack
 * bounds, as Windows' fibers do, so that a fault on a fiber's stack is
 * reported as one.
 */
#include "kernel/kern_int.h"
#include "pal_core.h"

#define FIBER_STACK_SIZE (1024u * 1024u)
#define MAX_FIBERS 64

struct KFiber {
    void *sp;            /* the saved stack pointer while it is not running */
    u8 *lo;              /* its stack: [lo, lo + size); 0 for the main fiber */
    u32 size;
    KFiberFn fn;
    void *arg;
    int used;
};

static KFiber sFibers[MAX_FIBERS];
static KFiber sMainFiber;
static KFiber *sCurrent = &sMainFiber;

/*
 * The asan variant (MH2_VARIANT=asan): AddressSanitizer is told of every
 * switch (its fiber interface), else it takes the other stacks' frames for
 * overflows. Its runtime gives the main stack's bounds at the first switch
 * away from it; the fiber that switched is in sSwitchFrom when the switch
 * returns. Its functions are cdecl (the game's flags make regcall the
 * default).
 */
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define KFIBER_ASAN 1
#endif
#endif
#ifndef KFIBER_ASAN
#define KFIBER_ASAN 0
#endif
#if KFIBER_ASAN
__attribute__((cdecl)) void __sanitizer_start_switch_fiber(void **fakeStackSave, const void *bottom, __SIZE_TYPE__ size);
__attribute__((cdecl)) void __sanitizer_finish_switch_fiber(void *fakeStackSave, const void **bottomOld, __SIZE_TYPE__ *sizeOld);
static KFiber *sSwitchFrom;
static const void *sMainBottom;
static __SIZE_TYPE__ sMainSize;

static void AsanStart(void **fake, const KFiber *to)
{
    if (to == &sMainFiber) {
        __sanitizer_start_switch_fiber(fake, sMainBottom, sMainSize);
    } else {
        __sanitizer_start_switch_fiber(fake, to->lo, to->size);
    }
}

static void AsanFinish(void *fake)
{
    const void *bottom;
    __SIZE_TYPE__ size;
    __sanitizer_finish_switch_fiber(fake, &bottom, &size);
    if (sSwitchFrom == &sMainFiber && !sMainBottom) {
        sMainBottom = bottom;
        sMainSize = size;
    }
}
#endif

/* The switch: cdecl, (void **saveSp, void *newSp). */
__attribute__((naked, cdecl, used)) static void FiberSwap(void **saveSp __attribute__((unused)),
                                                          void *newSp __attribute__((unused)))
{
    __asm__ volatile(
        "movl 4(%esp), %eax\n\t"
        "movl 8(%esp), %edx\n\t"
        "pushl %ebp\n\t"
        "pushl %ebx\n\t"
        "pushl %esi\n\t"
        "pushl %edi\n\t"
        "subl $8, %esp\n\t"
        "fnstcw (%esp)\n\t"
        "stmxcsr 4(%esp)\n\t"
#ifdef _WIN32
        "pushl %fs:0\n\t"
        "pushl %fs:4\n\t"
        "pushl %fs:8\n\t"
#endif
        "movl %esp, (%eax)\n\t"
        "movl %edx, %esp\n\t"
#ifdef _WIN32
        "popl %fs:8\n\t"
        "popl %fs:4\n\t"
        "popl %fs:0\n\t"
#endif
        "fldcw (%esp)\n\t"
        "ldmxcsr 4(%esp)\n\t"
        "addl $8, %esp\n\t"
        "popl %edi\n\t"
        "popl %esi\n\t"
        "popl %ebx\n\t"
        "popl %ebp\n\t"
        "ret\n\t");
}

/* A new fiber's first instructions: ebx holds the fiber, esi FiberMain. The stack is aligned to 16 bytes
 * at the call, as the i386 System V ABI wants (Windows needs 4). */
__attribute__((naked, used)) static void FiberStart(void)
{
    __asm__ volatile(
        "andl $-16, %esp\n\t"
        "subl $12, %esp\n\t"
        "pushl %ebx\n\t"
        "call *%esi\n\t"
        "ud2\n\t");
}

__attribute__((cdecl, used)) static void FiberMain(KFiber *f)
{
#if KFIBER_ASAN
    AsanFinish(0);
#endif
    f->fn(f->arg);
    Pal_Fatal("kernel: a fiber's function returned");
}

KFiber *KFiber_Main(void)
{
    return &sMainFiber;
}

static KFiber *CreateSized(KFiberFn fn, void *arg, u32 size)
{
    KFiber *f = 0;
    u32 *sp;
    u16 fpcw;
    u32 mxcsr;
    int i;

    size = (size + 0xFFFFu) & ~0xFFFFu;
    for (i = 0; i < MAX_FIBERS && !f; i++) {     /* a free slot whose stack is big enough */
        if (!sFibers[i].used && sFibers[i].lo && sFibers[i].size >= size) {
            f = &sFibers[i];
        }
    }
    for (i = 0; i < MAX_FIBERS && !f; i++) {     /* else a slot never used */
        if (!sFibers[i].used && !sFibers[i].lo) {
            f = &sFibers[i];
        }
    }
    if (!f) {
        Pal_Fatal("kernel: more than 64 fibers");
    }
    if (!f->lo) {
        f->lo = (u8 *)Pal_LowAlloc(size, "a fiber's stack");
        f->size = size;
    }
    f->used = 1;
    f->fn = fn;
    f->arg = arg;
    __asm__ volatile("fnstcw %0" : "=m"(fpcw));
    __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
    sp = (u32 *)(f->lo + f->size);
    *--sp = 0;                          /* above the return address: nothing */
    *--sp = (u32)FiberStart;            /* ret */
    *--sp = 0;                          /* ebp */
    *--sp = (u32)f;                     /* ebx */
    *--sp = (u32)FiberMain;             /* esi */
    *--sp = 0;                          /* edi */
    *--sp = mxcsr;
    *--sp = fpcw;
#ifdef _WIN32
    *--sp = 0xFFFFFFFFu;                /* fs:0, the exception list: empty */
    *--sp = (u32)(f->lo + f->size);   /* fs:4, the stack's base (its top) */
    *--sp = (u32)f->lo;                 /* fs:8, its limit */
#endif
    f->sp = sp;
    return f;
}

KFiber *KFiber_Create(KFiberFn fn, void *arg)
{
    return CreateSized(fn, arg, FIBER_STACK_SIZE);
}

void KFiber_Switch(KFiber *from, KFiber *to)
{
    if (from == to) {
        return;
    }
    sCurrent = to;
#if KFIBER_ASAN
    {
        void *fake = 0;
        sSwitchFrom = from;
        AsanStart(&fake, to);
        FiberSwap(&from->sp, to->sp);
        AsanFinish(fake);
        return;
    }
#endif
    FiberSwap(&from->sp, to->sp);
}

void KFiber_Free(KFiber *f)
{
    if (f && f != &sMainFiber) {
        f->used = 0;      /* the stack stays with the slot for the next fiber */
    }
}

KFiber *KFiber_Current(void)
{
    return sCurrent;
}

int KFiber_Holds(const KFiber *f, const void *p)
{
    const u8 *a = (const u8 *)p;
    return f && f->lo && a >= f->lo && a < f->lo + f->size;
}

/* The raw interface (kern.h), for snd's IOP threads. */
KernFiber *Kern_FiberCreate(void (*fn)(void *arg), void *arg, u32 stackSize)
{
    return CreateSized(fn, arg, stackSize ? stackSize : FIBER_STACK_SIZE);
}

KernFiber *Kern_FiberSelf(void)
{
    return sCurrent;
}

void Kern_FiberSwitch(KernFiber *to)
{
    KFiber_Switch(sCurrent, to);
}
