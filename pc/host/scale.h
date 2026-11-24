/*
 * The picture's scaling to the window (the Graphics page's "Picture scaling"): the filter of the sharp mode, and the size of
 * the whole-numbers mode. present.c draws the picture with them; scale.c needs SDL's threads only; a test compiles it alone.
 */
#ifndef MH2_SCALE_H
#define MH2_SCALE_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    SCALE_SMOOTH = 0,   /* the GPU's bilinear filter: the picture as it has always been drawn */
    SCALE_SHARP,        /* a cubic filter on the processor, then drawn as it is */
    SCALE_WHOLE         /* whole multiples of the picture's width, no filter: the picture is smaller, never blurred */
} ScaleMode;

/* The names of the settings' values. scale_mode_of: -1 for a word that is none. */
int scale_mode_of(const char *word);
int scale_strength_of(const char *word);   /* low 1, medium 2, high 3; -1 for another */

/* The sharp mode's filter: RGBA8 rows of SW x SH to DW x DH (strength 1 to 3). The rows are tight (SW * 4 bytes). WORKERS
 * more threads help (0: this one only). Returns 0, or -1 when memory fails (DST is then untouched). */
int scale_resample(const uint8_t *src, int sw, int sh, uint8_t *dst, int dw, int dh, int strength, int workers);

/* The whole-numbers mode: the rectangle of the 4:3 (AW:AH) picture SW wide in a window WW x WH: the largest whole multiple of
 * SW x (SW * AH / AW) that fits, centred. Sets *OW, *OH; returns the multiple, 0 when not even one fits (the caller then
 * fits the picture and filters it). */
int scale_whole_size(int ww, int wh, int sw, int aw, int ah, int *ow, int *oh);

/* Frees the buffers and the worker threads. */
void scale_free(void);

#endif
