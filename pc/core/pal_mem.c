/*
 * The RAM block, the scratchpad, the low-256 MB rule and the fatal exits
 *. The game-facing half of the memory services:
 * no system headers here (host.h is the seam).
 */
#include "pal_core.h"
#include "pal_align.h"

#include "host.h"
#include "kernel/kern.h"

/* The EE's 32 MB and the scratchpad: objects of the image's .bss, 64 KiB aligned (pal_align.h: every host address of
 * the PS2's data keeps the PS2 address's low 16 bits). */
PAL_PS2_DATA(Pal_EeRam, 0x2000000, ".ps2ram");
PAL_PS2_DATA(Pal_Spr, 0x4000, ".ps2spr");

/*
 * Memory for what the game can see but the image does not hold: the game
 * threads' stacks (the kernel's fibers). A pool mapped below 0x10000000:
 * under the image, from 0x02000000 (above the PS2's 32 MB, so that no host
 * pointer there equals a PS2 address), when the image lies high enough
 * (linked at 0x08000000); else in the first free window above the image.
 * Mapped at the start-up check, before anything asks for it.
 */
#define LOW_POOL_SIZE (48u * 1024 * 1024)
#define LOW_POOL_BASE 0x02000000u
static unsigned char *sLowPool;
static unsigned int sLowPoolUsed;

#define MAX_RANGES 64
static struct {
    const char *what;
    unsigned int lo, hi;
} sRanges[MAX_RANGES];
static int sRangeCount;

void Pal_Fatal(const char *what)
{
    Host_Fatal(what);
}

void Pal_StubAbort(const char *name)
{
    Host_StubFatal(name);
}

void Pal_RegisterLowRange(const char *what, const void *lo, unsigned int size)
{
    if (sRangeCount >= MAX_RANGES) {
        Pal_Fatal("Pal_RegisterLowRange: too many ranges");
    }
    sRanges[sRangeCount].what = what;
    sRanges[sRangeCount].lo = (unsigned int)(unsigned long)lo;
    sRanges[sRangeCount].hi = (unsigned int)(unsigned long)lo + size;
    sRangeCount++;
}

static void MapLowPool(void)
{
    unsigned int lo, hi, above;
    Host_ImageRange(&lo, &hi);
    if (LOW_POOL_BASE + LOW_POOL_SIZE <= lo) {
        sLowPool = (unsigned char *)Host_LowMap(LOW_POOL_BASE, LOW_POOL_SIZE);
    }
    /* Else the first free window above the image, in 1 MB steps (on Windows the process's heaps can lie
     * just above it). */
    for (above = (hi + 0xFFFFFu) & ~0xFFFFFu; !sLowPool && above + LOW_POOL_SIZE <= 0x10000000u;
         above += 0x100000u) {
        sLowPool = (unsigned char *)Host_LowMap(above, LOW_POOL_SIZE);
    }
    if (!sLowPool) {
        Pal_Fatal("the low pool (48 MB under 0x10000000) could not be mapped");
    }
}

void *Pal_LowAlloc(unsigned int size, const char *what)
{
    unsigned int page = Host_PageSize();
    unsigned int n = (size + page - 1) / page * page;
    void *p;
    if (!sLowPool) {
        MapLowPool();
    }

    if (n > LOW_POOL_SIZE - sLowPoolUsed) {
        Pal_Fatal("Pal_LowAlloc: the low pool is out of memory");
    }
    p = sLowPool + sLowPoolUsed;
    sLowPoolUsed += n;
    (void)what;     /* covered by the pool's range */
    return p;
}

static void CheckRange(const char *what, unsigned int lo, unsigned int hi)
{
    if (hi > 0x10000000u || lo >= hi) {
        Host_Log(what);
        Pal_Fatal("a range the game can see does not lie below 0x10000000 (the one named above)");
    }
}

void Pal_CheckLowMemory(void)
{
    unsigned int lo, hi;
    int i;

    if (!sLowPool) {
        MapLowPool();
    }
    Host_ImageRange(&lo, &hi);      /* the image, with the blocks in its .bss */
    CheckRange("the executable image", lo, hi);
    CheckRange("Pal_EeRam", (unsigned int)(unsigned long)Pal_EeRam, (unsigned int)(unsigned long)Pal_EeRam + sizeof(Pal_EeRam));
    CheckRange("Pal_Spr", (unsigned int)(unsigned long)Pal_Spr, (unsigned int)(unsigned long)Pal_Spr + sizeof(Pal_Spr));
    CheckRange("the low pool", (unsigned int)(unsigned long)sLowPool, (unsigned int)(unsigned long)sLowPool + LOW_POOL_SIZE);
    for (i = 0; i < sRangeCount; i++) {
        CheckRange(sRanges[i].what, sRanges[i].lo, sRanges[i].hi);
    }
}
