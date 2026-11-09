/*
 * The timing of the frame rates above 60 (120 and 144 frames per second): the logic with no SDL in it, so that a test can run it.
 * In short: the display follows a line from the real picture n-1 to the real picture n,
 * and each refresh shows the point of that line at its own time. gs.c asks for the in-between pictures of the refreshes of the coming frame
 * (hfr_plan), present.c shows the picture whose time it is.
 */
#ifndef MH2_HFR_H
#define MH2_HFR_H

#include <stdint.h>

#define HFR_VBLANK_NS 16683350.0   /* the core's VBlank period (pc/core/ipc/host_ipc.c VBLANK_NS) */
#define HFR_MAX 6                  /* in-between pictures to a game frame (mh2gs.h MH2GS_MAX_MIDS) */

/* The time of the VSyncs. The core waits for real time at each VBlank, so the records reach the GS thread every VBLANK_NS to within
 * the thread's own delay; the estimate moves by an eighth of the error and starts again at the arrival of a record that is more than
 * 8 ms off (a pause, a fast-forward, a load, a hitch). */
typedef struct HfrClock {
    double est;        /* the estimated time of the last VSync, ns */
    uint64_t vblank;   /* its VBlank number */
    int have;
    uint32_t steady;   /* records in a row that arrived on the estimate (a pause, a fast-forward or a hitch sets it to 0) */
} HfrClock;

/* Takes the arrival of the record of VBlank `vblank`; returns the estimate for it. */
double hfr_clock(HfrClock *c, double arrival_ns, uint64_t vblank);

/* How many refreshes to a picture: round(refresh rate / chosen rate), at least 1. */
int hfr_divider(double refresh_hz, int rate);

/* The in-between pictures of the game frame whose real picture is shown at b_ns (the time of its VSync plus one VBlank): for the
 * refreshes of the grid ref_ns + k * refresh_ns * divider that fall in [b - R/2, b + 2 VBlanks - R/2) (R: the period of the grid),
 * alpha = (t - b) / (2 VBlanks) in 1/256, and tau[i] = t. A refresh whose alpha is under 1/32 (a millisecond) shows the real picture and has no
 * entry. Returns the count (at most max); alphas rise. */
int hfr_plan(double b_ns, double ref_ns, double refresh_ns, int divider, uint16_t *alpha, double *tau, int max);

/* 60 a second: the one in-between picture of the game frame, at half a step (alpha 128) and at the time b + P / 2. Returns 1. */
int hfr_plan_half(double b_ns, uint16_t *alpha, double *tau);

/* The step down when the card cannot keep up: 144 -> 120 -> 60 -> 30 (returned as 60 or 30 below). On a display where 120 would be
 * the same pictures as 144 (hfr_divider equal), 144 goes to 60. */
int hfr_step_down(int rate, double refresh_hz);

#endif
