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

/* The same with the core's timeline (the stamp of a VSync record): state 0, no timeline (the rule above); 1, the VBlank was paced on the core's own timeline: its step
 * may end late by any time without being a jump (the estimate follows by at most 1 ms a VBlank); 2, the timeline began or broke at this VBlank (a pause, a stall,
 * a change of speed): the estimate is the arrival. *reset (if not NULL) is 1 when the estimate was taken from the arrival (also the first). */
double hfr_clock_timeline(HfrClock *c, double arrival_ns, uint64_t vblank, unsigned state, int *reset);

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

/* The frame rate a setting gives on a machine of CPUS logical processors: under 60 (0 is not set) it is 30; 60 is 60; above 60 (120, 144)
 * needs HFR_MIN_CPUS logical processors (the measured floor: the pass's five helpers held the in-between pictures at 120 and 144 on eight),
 * and is 60 below it. */
#define HFR_MIN_CPUS 8
int hfr_rate_rule(int fps, int cpus);

/* The in-between pictures' ring (the core's second pass): made for a rate of 60 and above, never at 30. */
int hfr_pass_ring(int rate);

/* The live alphas (the pass on): at each anchor the host predicts the next step's plan (b' = b + P on the same grid) and publishes it with the VBlank number the
 * step's VSync is expected to have; the core makes its moments at those alphas when it reads the plan in time (hfr_predict). At the next anchor each actual refresh takes the predicted
 * moment within a quarter of a refresh of its time, a refresh with none is B's, and when the real picture's time b has moved by more than a millisecond from the prediction the whole step is B's
 * (hfr_match). */
#define HFR_LIVE_STEP_NS 1e6      /* the most b may move from the prediction for the step to keep its moments */
typedef struct HfrPlan {
    double b;                     /* the real picture's time */
    int n;                        /* pictures between */
    uint16_t alpha[HFR_MAX];      /* in 1/256, rising */
    double tau[HFR_MAX];          /* their times */
} HfrPlan;

/* The plan predicted for the step after the one whose real picture is shown at b_ns: the plan of b_ns + P (hfr_plan; at 60, hfr_plan_half). Returns the count. */
int hfr_predict(double b_ns, double ref_ns, double refresh_ns, int divider, int half, HfrPlan *out);

typedef struct HfrMatch {
    int stale;                    /* b moved more than step_tol_ns from the prediction: the whole step is B's */
    int use[HFR_MAX];             /* the predicted moment each actual refresh takes, -1: B's picture */
    int matched, bs;              /* the counts of the two */
} HfrMatch;

/* Each actual refresh against the predicted plan. period_ns is the grid's step R (the refresh's times lie that far apart: a match is within R / 4); step_tol_ns is HFR_LIVE_STEP_NS
 * (a test's control passes a huge one). A moment is taken by one refresh only. */
void hfr_match(const HfrPlan *pred, const HfrPlan *act, double period_ns, double step_tol_ns, HfrMatch *m);

#endif
