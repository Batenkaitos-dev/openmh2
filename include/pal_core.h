#ifndef PAL_CORE_H
#define PAL_CORE_H

#include "platform.h"
#include "pal.h"      /* Pal_EeRam and Pal_Spr, the RAM block and the scratchpad (defined in pc/core/pal_mem.c) */

/*
 * The core's part of the platform interface (PLATFORM_PC only): the
 * memory blocks, the low-256 MB rule and overlay activation. The hardware
 * hooks are in pal.h.
 */
#if PLATFORM_PC

/*
 * Everything the game can see lies below 0x10000000 (the image, the two
 * blocks above and the game threads' stacks; the DMA code masks addresses
 * with 0x0FFFFFFF). Pal_CheckLowMemory aborts with a message naming the
 * first range that doesn't. Pal_LowAlloc hands out page-aligned memory from
 * a pool inside the image's .bss (the kernel's stacks come from it);
 * Pal_RegisterLowRange adds a range that was obtained another way, so that
 * the check covers it.
 */
void Pal_CheckLowMemory(void);
void *Pal_LowAlloc(unsigned int size, const char *what);
void Pal_RegisterLowRange(const char *what, const void *lo, unsigned int size);

/*
 * An overlay was loaded (Overlay_Relocate, src/main/main.c, after its copy of
 * the image to PAL_RAM(load)): its address words get the PC's values, its .bss
 * [load + imageSize, + bssSize) is cleared as the PS2's start-up does, and it
 * becomes its window's active overlay (pal_overlay.c). id is the OverlayId in
 * the file's header.
 */
void Pal_OverlayActivate(int id, unsigned int load, int imageSize, int bssSize);
/* Pal_OverlayInit reads MH2_OVERLAY_BSS: main calls it before any game code runs
 * (Pal_OverlayActivate calls it too if nobody has). */
void Pal_OverlayInit(void);

/* Called by the other platform files that find the game broke a rule: prints and aborts. */
void Pal_Fatal(const char *what);
/* An API function nobody has written yet was called (the generated stubs). */
void Pal_StubAbort(const char *name);

#endif /* PLATFORM_PC */
#endif /* PAL_CORE_H */
