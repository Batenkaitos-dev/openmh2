/* The warm-up exemption of the give-up of the in-between pictures (development builds, MH2_PASS_WARMUP=N): where a scene
 * starts, and which steps are not counted. Part of the port (CC0 1.0).
 *
 * The rule it serves: while the in-between pictures come from the pass, the last steps that had a late moment after the first
 * are counted 32 at a time, and 4 late steps in 32 give up (the pictures come from the second renderer for a while). A scene's
 * first steps are late for a reason that does not last (every draw is new, and the step carries several times the data of the
 * steps before it), so they can make the rule give up on a scene that would have held.
 *
 * A scene's start, as this file reads it: the step whose own chain (the bytes the step drew) is at least twice the chain of
 * the step before it and at least MIN_GROW bytes more. That step and the n - 1 after it are the stretch: they are not entered
 * in the history. A second jump inside a stretch does not restart it (a scene that builds up over two or three steps is one
 * start). A step made while the pictures come from the second renderer is only remembered as the step before: a scene that
 * starts there is not a start when the pass takes over again. */
#ifndef MH2GS_WARM_HPP
#define MH2GS_WARM_HPP

#include <cstdint>

struct Mh2PassWarm
{
    enum : uint64_t { MIN_GROW = 262144 };
    uint32_t n = 0;            /* the steps a start exempts (0: the exemption is off) */
    uint32_t left = 0;         /* the steps the stretch in progress still has */
    uint32_t mask = 0;         /* the stretch's late steps, bit 0 the newest */
    bool seen = false;         /* there is a step before */
    uint64_t db = 0, vs = 0;   /* the step before: its chain's bytes and the VSync it ended at */
    uint64_t vs0 = 0;          /* the VSync of the stretch's first step */
    uint64_t starts = 0, steps = 0, late = 0;   /* in all: starts, steps not counted, late steps among them */

    /* A step made while the pictures come from the second renderer. */
    void seen_step(uint64_t bytes, uint64_t vsync)
    {
        db = bytes;
        vs = vsync;
        seen = true;
    }

    /* A step made from the pass: bytes of its own chain, the VSync it ended at, whether it had a late moment after the first.
     * 0: the step is counted as any other; 1: it is not counted; 2: it is not counted and it is a start (the stretch's first).
     * The stretch's last step is the one after which left is 0. */
    int step(uint64_t bytes, uint64_t vsync, bool late_step)
    {
        int r = 0;
        if (!n)
            return 0;
        if (!left && seen && bytes >= 2 * db && bytes >= db + MIN_GROW)
        {
            left = n;
            mask = 0;
            vs0 = vsync;
            starts++;
            r = 2;
        }
        db = bytes;
        vs = vsync;
        seen = true;
        if (left)
        {
            if (!r)
                r = 1;
            mask = (mask << 1) | (late_step ? 1u : 0u);
            steps++;
            late += late_step ? 1u : 0u;
            left--;
        }
        return r;
    }
};

#endif
