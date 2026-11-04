/*
 * The CodeWarrior runtime's overlay start-up, func_00128550:
 * Overlay_Relocate calls it on an overlay's image after copying it to its load
 * address. On the PS2 it flushes the caches, clears the .bss that follows the image (the header's
 * .bss size) and calls the header's table of functions (none in the game's overlays).
 *
 * On the PC the overlay's code is part of the image, and the clear (of the same range of the RAM
 * block) is Pal_OverlayActivate's (pal_overlay.c), which Overlay_Relocate calls next with the
 * header's numbers: this version does nothing but its tap, so the call is compared with the
 * original's.
 */
#include "common.h"

void func_00128550(void *addr, s32 size)
{
}
