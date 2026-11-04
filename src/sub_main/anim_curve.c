/*
 * The animation curves: a curve's value at a time (Anim_EvalCurve), by
 * its key kind (linear, cubic or mixed keys, as floats or shorts), the
 * search for the two keys around the time (Anim_FindKeys*), and the
 * linear and cubic steps between two keys (Anim_LerpKeys, and
 * Anim_CubicKeys on VU0). A curve's key offsets count from the current
 * animation's data (Anim_SetCurveKeys).
 */

#include "common.h"
#include "objhead.h"
#include "anim_curve.h"

#if PLATFORM_PC
#include "vu0ops.h"
#endif

#pragma optimization_level 4

extern u8 *gAnimCurveKeys;

/* Sets the base the curves' key offsets count from (gAnimCurveKeys): the
 * current animation's data. */
void Anim_SetCurveKeys(u8 *keys)
{
    gAnimCurveKeys = keys;
}

/* The value of curve at time (*hint: the key found last time, kept by the caller). */
f32 Anim_EvalCurve(f32 time, AnimCurve *curve, s16 *hint)
{
    f32 value;

    switch (curve->type) {
    case ANIM_KEY_F32_LINEAR:
        value = Anim_EvalLinearF(curve, hint, time);
        break;
    case ANIM_KEY_F32_CUBIC:
        value = Anim_EvalCubicF(curve, hint, time);
        break;
    case ANIM_KEY_F32_MIXED:
        value = Anim_EvalMixedF(curve, hint, time);
        break;
    case ANIM_KEY_S16_LINEAR:
        value = Anim_EvalLinearS(curve, hint, time);
        break;
    case ANIM_KEY_S16_CUBIC:
        value = Anim_EvalCubicS(curve, hint, time);
        break;
    case ANIM_KEY_S16_MIXED:
        value = Anim_EvalMixedS(curve, hint, time);
        break;
    }
    return value;
}

/* Format 0x21: linear between f32 keys. */
f32 Anim_EvalLinearF(AnimCurve *curve, s16 *hint, f32 time)
{
    KeyF2 *prevKey = NULL;
    KeyF2 *nextKey = NULL;

    *hint = Anim_FindKeysF2(curve, &prevKey, &nextKey, *hint, time);
    if (nextKey == NULL) {
        return prevKey->v;
    }
    return Anim_LerpKeys(time, prevKey->v, prevKey->t, nextKey->v, nextKey->t);
}

/* Format 0x22: cubic between f32 keys. */
f32 Anim_EvalCubicF(AnimCurve *curve, s16 *hint, f32 time)
{
    KeyF4 *prevKey = NULL;
    KeyF4 *nextKey = NULL;

    *hint = Anim_FindKeysF4(curve, &prevKey, &nextKey, *hint, time);
    if (nextKey == NULL) {
        return prevKey->v;
    }
    return Anim_CubicKeys(time, prevKey->v, prevKey->t, prevKey->out, nextKey->v, nextKey->t, nextKey->in);
}

/* Format ANIM_KEY_F32_MIXED: f32 keys, linear (ANIM_KEY_MODE_LINEAR) or cubic (ANIM_KEY_MODE_CUBIC) by the first key of the pair. */
f32 Anim_EvalMixedF(AnimCurve *curve, s16 *hint, f32 time)
{
    KeyFM *prevKey = NULL;
    KeyFM *nextKey = NULL;

    *hint = Anim_FindKeysFM(curve, &prevKey, &nextKey, *hint, time);
    if (nextKey == NULL) {
        return prevKey->v;
    }
    switch (prevKey->mode) {
    case ANIM_KEY_MODE_LINEAR:
        return Anim_LerpKeys(time, prevKey->v, prevKey->t, nextKey->v, nextKey->t);
    case ANIM_KEY_MODE_CUBIC:
        return Anim_CubicKeys(time, prevKey->v, prevKey->t, prevKey->out, nextKey->v, nextKey->t, nextKey->in);
    }
    return 0.0f;
}

/* Format 0x11: linear between s16 keys. */
f32 Anim_EvalLinearS(AnimCurve *curve, s16 *hint, f32 time)
{
    KeyS2 *prevKey = NULL;
    KeyS2 *nextKey = NULL;

    *hint = Anim_FindKeysS2(curve, &prevKey, &nextKey, *hint, time);
    if (nextKey == NULL) {
        return prevKey->v;
    }
    return Anim_LerpKeys(time, prevKey->v, prevKey->t, nextKey->v, nextKey->t);
}

/* Format 0x12: cubic between s16 keys. */
f32 Anim_EvalCubicS(AnimCurve *curve, s16 *hint, f32 time)
{
    KeyS4 *prevKey = NULL;
    KeyS4 *nextKey = NULL;

    *hint = Anim_FindKeysS4(curve, &prevKey, &nextKey, *hint, time);
    if (nextKey == NULL) {
        return prevKey->v;
    }
    return Anim_CubicKeys(time, prevKey->v, prevKey->t, prevKey->out, nextKey->v, nextKey->t, nextKey->in);
}

/* Format 0x13: s16 keys, linear or cubic by the first key of the pair. */
f32 Anim_EvalMixedS(AnimCurve *curve, s16 *hint, f32 time)
{
    KeySM *prevKey = NULL;
    KeySM *nextKey = NULL;

    *hint = Anim_FindKeysSM(curve, &prevKey, &nextKey, *hint, time);
    if (nextKey == NULL) {
        return prevKey->v;
    }
    switch (prevKey->mode) {
    case ANIM_KEY_MODE_LINEAR:
        return Anim_LerpKeys(time, prevKey->v, prevKey->t, nextKey->v, nextKey->t);
    case ANIM_KEY_MODE_CUBIC:
        return Anim_CubicKeys(time, prevKey->v, prevKey->t, prevKey->out, nextKey->v, nextKey->t, nextKey->in);
    }
    return 0.0f;
}

/*
 * The keys of curve around time, searched from key hint: *prevKeyOut and *nextKeyOut = the
 * keys before and after time (*nextKeyOut NULL at a key, before the first or after the
 * last); returns *prevKeyOut's index.
 * The five below do the same for the other key formats.
 */
s16 Anim_FindKeysF2(AnimCurve *curve, KeyF2 **prevKeyOut, KeyF2 **nextKeyOut, s16 hint, f32 time)
{
    KeyF2 *key = (KeyF2 *)(curve->keys + (u32)gAnimCurveKeys);

    *prevKeyOut = NULL;
    *nextKeyOut = NULL;
    if (time <= key[0].t || curve->count == 1) {
        *prevKeyOut = key;
        *nextKeyOut = NULL;
        return 0;
    }
    if (!(time < key[curve->count - 1].t)) {
        *prevKeyOut = &key[curve->count - 1];
        *nextKeyOut = NULL;
        return curve->count - 1;
    }
    if (hint < 0) {
        hint = 0;
    } else if (hint >= curve->count) {
        hint = curve->count - 1;
    }
    key += hint;
    for (;;) {
        if (key->t == time) {
            *prevKeyOut = key;
            *nextKeyOut = NULL;
            return hint;
        }
        if (key->t < time && time < key[1].t) {
            *prevKeyOut = key;
            *nextKeyOut = key + 1;
            return hint;
        }
        if (time < key->t) {
            key--;
            hint--;
        } else {
            key++;
            hint++;
        }
    }
}

/* Anim_FindKeysF2 for KeyF4 keys. */
s16 Anim_FindKeysF4(AnimCurve *curve, KeyF4 **prevKeyOut, KeyF4 **nextKeyOut, s16 hint, f32 time)
{
    KeyF4 *key = (KeyF4 *)(curve->keys + (u32)gAnimCurveKeys);

    *prevKeyOut = NULL;
    *nextKeyOut = NULL;
    if (time <= key[0].t || curve->count == 1) {
        *prevKeyOut = key;
        *nextKeyOut = NULL;
        return 0;
    }
    if (!(time < key[curve->count - 1].t)) {
        *prevKeyOut = &key[curve->count - 1];
        *nextKeyOut = NULL;
        return curve->count - 1;
    }
    if (hint < 0) {
        hint = 0;
    } else if (hint >= curve->count) {
        hint = curve->count - 1;
    }
    key += hint;
    for (;;) {
        if (key->t == time) {
            *prevKeyOut = key;
            *nextKeyOut = NULL;
            return hint;
        }
        if (key->t < time && time < key[1].t) {
            *prevKeyOut = key;
            *nextKeyOut = key + 1;
            return hint;
        }
        if (time < key->t) {
            key--;
            hint--;
        } else {
            key++;
            hint++;
        }
    }
}

/* Anim_FindKeysF2 for KeyFM keys. */
s16 Anim_FindKeysFM(AnimCurve *curve, KeyFM **prevKeyOut, KeyFM **nextKeyOut, s16 hint, f32 time)
{
    KeyFM *key = (KeyFM *)(curve->keys + (u32)gAnimCurveKeys);

    *prevKeyOut = NULL;
    *nextKeyOut = NULL;
    if (time <= key[0].t || curve->count == 1) {
        *prevKeyOut = key;
        *nextKeyOut = NULL;
        return 0;
    }
    if (!(time < key[curve->count - 1].t)) {
        *prevKeyOut = &key[curve->count - 1];
        *nextKeyOut = NULL;
        return curve->count - 1;
    }
    if (hint < 0) {
        hint = 0;
    } else if (hint >= curve->count) {
        hint = curve->count - 1;
    }
    key += hint;
    for (;;) {
        if (key->t == time) {
            *prevKeyOut = key;
            *nextKeyOut = NULL;
            return hint;
        }
        if (key->t < time && time < key[1].t) {
            *prevKeyOut = key;
            *nextKeyOut = key + 1;
            return hint;
        }
        if (time < key->t) {
            key--;
            hint--;
        } else {
            key++;
            hint++;
        }
    }
}

/* Anim_FindKeysF2 for KeyS2 keys. */
s16 Anim_FindKeysS2(AnimCurve *curve, KeyS2 **prevKeyOut, KeyS2 **nextKeyOut, s16 hint, f32 time)
{
    KeyS2 *key = (KeyS2 *)(curve->keys + (u32)gAnimCurveKeys);

    *prevKeyOut = NULL;
    *nextKeyOut = NULL;
    if (time <= key[0].t || curve->count == 1) {
        *prevKeyOut = key;
        *nextKeyOut = NULL;
        return 0;
    }
    if (!(time < key[curve->count - 1].t)) {
        *prevKeyOut = &key[curve->count - 1];
        *nextKeyOut = NULL;
        return curve->count - 1;
    }
    if (hint < 0) {
        hint = 0;
    } else if (hint >= curve->count) {
        hint = curve->count - 1;
    }
    key += hint;
    for (;;) {
        if (key->t == time) {
            *prevKeyOut = key;
            *nextKeyOut = NULL;
            return hint;
        }
        if (key->t < time && time < key[1].t) {
            *prevKeyOut = key;
            *nextKeyOut = key + 1;
            return hint;
        }
        if (time < key->t) {
            key--;
            hint--;
        } else {
            key++;
            hint++;
        }
    }
}

/* Anim_FindKeysF2 for KeyS4 keys. */
s16 Anim_FindKeysS4(AnimCurve *curve, KeyS4 **prevKeyOut, KeyS4 **nextKeyOut, s16 hint, f32 time)
{
    KeyS4 *key = (KeyS4 *)(curve->keys + (u32)gAnimCurveKeys);

    *prevKeyOut = NULL;
    *nextKeyOut = NULL;
    if (time <= key[0].t || curve->count == 1) {
        *prevKeyOut = key;
        *nextKeyOut = NULL;
        return 0;
    }
    if (!(time < key[curve->count - 1].t)) {
        *prevKeyOut = &key[curve->count - 1];
        *nextKeyOut = NULL;
        return curve->count - 1;
    }
    if (hint < 0) {
        hint = 0;
    } else if (hint >= curve->count) {
        hint = curve->count - 1;
    }
    key += hint;
    for (;;) {
        if (key->t == time) {
            *prevKeyOut = key;
            *nextKeyOut = NULL;
            return hint;
        }
        if (key->t < time && time < key[1].t) {
            *prevKeyOut = key;
            *nextKeyOut = key + 1;
            return hint;
        }
        if (time < key->t) {
            key--;
            hint--;
        } else {
            key++;
            hint++;
        }
    }
}

/* Anim_FindKeysF2 for KeySM keys. */
s16 Anim_FindKeysSM(AnimCurve *curve, KeySM **prevKeyOut, KeySM **nextKeyOut, s16 hint, f32 time)
{
    KeySM *key = (KeySM *)(curve->keys + (u32)gAnimCurveKeys);

    *prevKeyOut = NULL;
    *nextKeyOut = NULL;
    if (time <= key[0].t || curve->count == 1) {
        *prevKeyOut = key;
        *nextKeyOut = NULL;
        return 0;
    }
    if (!(time < key[curve->count - 1].t)) {
        *prevKeyOut = &key[curve->count - 1];
        *nextKeyOut = NULL;
        return curve->count - 1;
    }
    if (hint < 0) {
        hint = 0;
    } else if (hint >= curve->count) {
        hint = curve->count - 1;
    }
    key += hint;
    for (;;) {
        if (key->t == time) {
            *prevKeyOut = key;
            *nextKeyOut = NULL;
            return hint;
        }
        if (key->t < time && time < key[1].t) {
            *prevKeyOut = key;
            *nextKeyOut = key + 1;
            return hint;
        }
        if (time < key->t) {
            key--;
            hint--;
        } else {
            key++;
            hint++;
        }
    }
}
/* The value at time on the line through (prevTime, prevValue) and (nextTime, nextValue). */
#if PLATFORM_PC
f32 Anim_LerpKeys(f32 time, f32 prevValue, f32 prevTime, f32 nextValue, f32 nextTime)
{
    unsigned int t0 __attribute__((unused)) = 0;
    f32 f0 __attribute__((unused)) = 0;
    f32 f12 __attribute__((unused)) = time;
    f32 f13 __attribute__((unused)) = prevValue;
    f32 f14 __attribute__((unused)) = prevTime;
    f32 f15 __attribute__((unused)) = nextValue;
    f32 f16 __attribute__((unused)) = nextTime;
    t0 = PC_F2U(f12); /* mfc1 t0, f12 */
    /* nop */
    vu0_qmtc2(3, t0); /* qmtc2.ni t0, vf3 */
    t0 = PC_F2U(f14); /* mfc1 t0, f14 */
    /* nop */
    vu0_qmtc2(4, t0); /* qmtc2.ni t0, vf4 */
    t0 = PC_F2U(f16); /* mfc1 t0, f16 */
    /* nop */
    vu0_qmtc2(5, t0); /* qmtc2.ni t0, vf5 */
    t0 = PC_F2U(f13); /* mfc1 t0, f13 */
    /* nop */
    vu0_qmtc2(6, t0); /* qmtc2.ni t0, vf6 */
    t0 = PC_F2U(f15); /* mfc1 t0, f15 */
    /* nop */
    vu0_qmtc2(7, t0); /* qmtc2.ni t0, vf7 */
    vu0_fmac(VU0_SUB, VU0_BVEC, 0, VU0_X, 8, 5, 4); /* vsub.x vf8, vf5, vf4 */
    vu0_vdiv(0, 3, 8, 0); /* vdiv Q, vf0w, vf8x */
    vu0_fmac(VU0_SUB, VU0_BVEC, 0, VU0_X, 9, 7, 6); /* vsub.x vf9, vf7, vf6 */
    vu0_fmac(VU0_MUL, VU0_BVEC, 0, VU0_X, 10, 5, 6); /* vmul.x vf10, vf5, vf6 */
    vu0_fmac(VU0_MUL, VU0_BVEC, 0, VU0_X, 11, 4, 7); /* vmul.x vf11, vf4, vf7 */
    vu0_fmac(VU0_SUB, VU0_BVEC, 0, VU0_X, 10, 10, 11); /* vsub.x vf10, vf10, vf11 */
    /* vwaitq */
    vu0_fmac(VU0_MUL, VU0_BQ, 0, VU0_X, 9, 9, 0); /* vmulq.x vf9, vf9, Q */
    vu0_fmac(VU0_MUL, VU0_BQ, 0, VU0_X, 10, 10, 0); /* vmulq.x vf10, vf10, Q */
    vu0_fmac(VU0_MUL, VU0_BVEC, 0, VU0_X, 4, 9, 3); /* vmul.x vf4, vf9, vf3 */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, VU0_X, 4, 4, 10); /* vadd.x vf4, vf4, vf10 */
    t0 = vu0_qmfc2(4); /* qmfc2.ni t0, vf4 */
    f0 = PC_U2F(t0); /* mtc1 t0, f0 */
    /* nop */
    return f0; /* jr ra */
}
#else
asm f32 Anim_LerpKeys(f32 time, f32 prevValue, f32 prevTime, f32 nextValue, f32 nextTime)
{
    .set noreorder
    mfc1 t0, f12
    nop
    qmtc2.ni t0, vf3
    mfc1 t0, f14
    nop
    qmtc2.ni t0, vf4
    mfc1 t0, f16
    nop
    qmtc2.ni t0, vf5
    mfc1 t0, f13
    nop
    qmtc2.ni t0, vf6
    mfc1 t0, f15
    nop
    qmtc2.ni t0, vf7
    vsub.x vf8, vf5, vf4
    vdiv Q, vf0w, vf8x
    vsub.x vf9, vf7, vf6
    vmul.x vf10, vf5, vf6
    vmul.x vf11, vf4, vf7
    vsub.x vf10, vf10, vf11
    vwaitq
    vmulq.x vf9, vf9, Q
    vmulq.x vf10, vf10, Q
    vmul.x vf4, vf9, vf3
    vadd.x vf4, vf4, vf10
    qmfc2.ni t0, vf4
    mtc1 t0, f0
    jr ra
    nop
}
#endif
/* The value at time on the cubic between two keys (their values and times, the first key's
 * out slope and the second key's in slope). */
#if PLATFORM_PC
f32 Anim_CubicKeys(f32 time, f32 prevValue, f32 prevTime, f32 prevOutSlope, f32 nextValue, f32 nextTime, f32 nextInSlope)
{
    unsigned int t0 __attribute__((unused)) = 0;
    f32 f0 __attribute__((unused)) = 0;
    f32 f12 __attribute__((unused)) = time;
    f32 f13 __attribute__((unused)) = prevValue;
    f32 f14 __attribute__((unused)) = prevTime;
    f32 f15 __attribute__((unused)) = prevOutSlope;
    f32 f16 __attribute__((unused)) = nextValue;
    f32 f17 __attribute__((unused)) = nextTime;
    f32 f18 __attribute__((unused)) = nextInSlope;
    t0 = PC_F2U(f12); /* mfc1 t0, f12 */
    /* nop */
    vu0_qmtc2(3, t0); /* qmtc2.ni t0, vf3 */
    t0 = PC_F2U(f14); /* mfc1 t0, f14 */
    /* nop */
    vu0_qmtc2(4, t0); /* qmtc2.ni t0, vf4 */
    t0 = PC_F2U(f17); /* mfc1 t0, f17 */
    /* nop */
    vu0_qmtc2(5, t0); /* qmtc2.ni t0, vf5 */
    t0 = PC_F2U(f13); /* mfc1 t0, f13 */
    /* nop */
    vu0_qmtc2(6, t0); /* qmtc2.ni t0, vf6 */
    t0 = PC_F2U(f16); /* mfc1 t0, f16 */
    /* nop */
    vu0_qmtc2(7, t0); /* qmtc2.ni t0, vf7 */
    t0 = PC_F2U(f15); /* mfc1 t0, f15 */
    /* nop */
    vu0_qmtc2(8, t0); /* qmtc2.ni t0, vf8 */
    t0 = PC_F2U(f18); /* mfc1 t0, f18 */
    /* nop */
    vu0_qmtc2(2, t0); /* qmtc2.ni t0, vf2 */
    vu0_fmac(VU0_SUB, VU0_BVEC, 0, VU0_X, 9, 3, 4); /* vsub.x vf9, vf3, vf4 */
    vu0_fmac(VU0_SUB, VU0_BVEC, 0, VU0_X, 10, 5, 4); /* vsub.x vf10, vf5, vf4 */
    vu0_vdiv(0, 3, 10, 0); /* vdiv Q, vf0w, vf10x */
    /* vwaitq */
    vu0_fmac(VU0_ADD, VU0_BQ, 0, VU0_X, 10, 0, 0); /* vaddq.x vf10, vf0, Q */
    vu0_fmac(VU0_MUL, VU0_BVEC, 0, VU0_X, 11, 10, 10); /* vmul.x vf11, vf10, vf10 */
    vu0_fmac(VU0_MUL, VU0_BVEC, 0, VU0_X, 12, 9, 9); /* vmul.x vf12, vf9, vf9 */
    vu0_fmac(VU0_MUL, VU0_BVEC, 0, VU0_X, 13, 12, 10); /* vmul.x vf13, vf12, vf10 */
    vu0_fmac(VU0_MUL, VU0_BVEC, 0, VU0_X, 14, 12, 9); /* vmul.x vf14, vf12, vf9 */
    vu0_fmac(VU0_MUL, VU0_BVEC, 0, VU0_X, 14, 14, 11); /* vmul.x vf14, vf14, vf11 */
    vu0_fmac(VU0_ADD, VU0_BW, 0, VU0_X, 1, 0, 0); /* vaddw.x vf1, vf0, vf0w */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, VU0_X, 1, 1, 1); /* vadd.x vf1, vf1, vf1 */
    vu0_fmac(VU0_MUL, VU0_BVEC, 0, VU0_X, 15, 1, 14); /* vmul.x vf15, vf1, vf14 */
    vu0_fmac(VU0_MUL, VU0_BVEC, 0, VU0_X, 15, 15, 10); /* vmul.x vf15, vf15, vf10 */
    vu0_fmac(VU0_ADD, VU0_BW, 0, VU0_X, 1, 1, 0); /* vaddw.x vf1, vf1, vf0w */
    vu0_fmac(VU0_MUL, VU0_BVEC, 0, VU0_X, 16, 1, 12); /* vmul.x vf16, vf1, vf12 */
    vu0_fmac(VU0_MUL, VU0_BVEC, 0, VU0_X, 16, 16, 11); /* vmul.x vf16, vf16, vf11 */
    vu0_fmac(VU0_SUB, VU0_BVEC, 0, VU0_X, 17, 15, 16); /* vsub.x vf17, vf15, vf16 */
    vu0_fmac(VU0_ADD, VU0_BW, 0, VU0_X, 17, 17, 0); /* vaddw.x vf17, vf17, vf0w */
    vu0_fmac(VU0_MUL, VU0_BVEC, 0, VU0_X, 17, 6, 17); /* vmul.x vf17, vf6, vf17 */
    vu0_fmac(VU0_SUB, VU0_BVEC, 0, VU0_X, 18, 16, 15); /* vsub.x vf18, vf16, vf15 */
    vu0_fmac(VU0_MUL, VU0_BVEC, 0, VU0_X, 18, 7, 18); /* vmul.x vf18, vf7, vf18 */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, VU0_X, 17, 17, 18); /* vadd.x vf17, vf17, vf18 */
    vu0_fmac(VU0_SUB, VU0_BVEC, 0, VU0_X, 18, 14, 13); /* vsub.x vf18, vf14, vf13 */
    vu0_fmac(VU0_SUB, VU0_BVEC, 0, VU0_X, 18, 18, 13); /* vsub.x vf18, vf18, vf13 */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, VU0_X, 18, 18, 9); /* vadd.x vf18, vf18, vf9 */
    vu0_fmac(VU0_MUL, VU0_BVEC, 0, VU0_X, 18, 8, 18); /* vmul.x vf18, vf8, vf18 */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, VU0_X, 17, 17, 18); /* vadd.x vf17, vf17, vf18 */
    vu0_fmac(VU0_SUB, VU0_BVEC, 0, VU0_X, 18, 14, 13); /* vsub.x vf18, vf14, vf13 */
    vu0_fmac(VU0_MUL, VU0_BVEC, 0, VU0_X, 18, 2, 18); /* vmul.x vf18, vf2, vf18 */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, VU0_X, 17, 17, 18); /* vadd.x vf17, vf17, vf18 */
    t0 = vu0_qmfc2(17); /* qmfc2.ni t0, vf17 */
    f0 = PC_U2F(t0); /* mtc1 t0, f0 */
    /* nop */
    return f0; /* jr ra */
}
#else
asm f32 Anim_CubicKeys(f32 time, f32 prevValue, f32 prevTime, f32 prevOutSlope, f32 nextValue, f32 nextTime, f32 nextInSlope)
{
    .set noreorder
    mfc1 t0, f12
    nop
    qmtc2.ni t0, vf3
    mfc1 t0, f14
    nop
    qmtc2.ni t0, vf4
    mfc1 t0, f17
    nop
    qmtc2.ni t0, vf5
    mfc1 t0, f13
    nop
    qmtc2.ni t0, vf6
    mfc1 t0, f16
    nop
    qmtc2.ni t0, vf7
    mfc1 t0, f15
    nop
    qmtc2.ni t0, vf8
    mfc1 t0, f18
    nop
    qmtc2.ni t0, vf2
    vsub.x vf9, vf3, vf4
    vsub.x vf10, vf5, vf4
    vdiv Q, vf0w, vf10x
    vwaitq
    vaddq.x vf10, vf0, Q
    vmul.x vf11, vf10, vf10
    vmul.x vf12, vf9, vf9
    vmul.x vf13, vf12, vf10
    vmul.x vf14, vf12, vf9
    vmul.x vf14, vf14, vf11
    vaddw.x vf1, vf0, vf0w
    vadd.x vf1, vf1, vf1
    vmul.x vf15, vf1, vf14
    vmul.x vf15, vf15, vf10
    vaddw.x vf1, vf1, vf0w
    vmul.x vf16, vf1, vf12
    vmul.x vf16, vf16, vf11
    vsub.x vf17, vf15, vf16
    vaddw.x vf17, vf17, vf0w
    vmul.x vf17, vf6, vf17
    vsub.x vf18, vf16, vf15
    vmul.x vf18, vf7, vf18
    vadd.x vf17, vf17, vf18
    vsub.x vf18, vf14, vf13
    vsub.x vf18, vf18, vf13
    vadd.x vf18, vf18, vf9
    vmul.x vf18, vf8, vf18
    vadd.x vf17, vf17, vf18
    vsub.x vf18, vf14, vf13
    vmul.x vf18, vf2, vf18
    vadd.x vf17, vf17, vf18
    qmfc2.ni t0, vf17
    mtc1 t0, f0
    jr ra
    nop
}
#endif
