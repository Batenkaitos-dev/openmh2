/*
 * The engine's maths: sine and cosine on VU0 (Math_SinVu0, Math_CosVu0,
 * Math_SinCos), the camera's projection and viewport matrices
 * (Camera_SetPerspective, Camera_SetOrtho, Camera_SetViewport), the 4x4
 * matrix operations (Mtx_*), the vectors (Vec_*, Vec4_*), the angle
 * conversions, the matrix slots (MtxSlot_Store, MtxSlot_Load), a point's
 * projection to the screen, the C library's maths for floats (Math_*),
 * the quaternions (Quat_*) and the view volume's sphere test
 * (Frustum_TestSphere).
 */

#include "common.h"
#include "syswork.h"
#include "model.h"
#include "engine_math.h"
#include "gs_regs.h"
#include "libm.h"

#if PLATFORM_PC
#include "vu0ops.h"
#endif

#pragma optimization_level 4

extern s32 gDispWidth; /* main: display width (divided by 640 for the font scale) */
extern s32 gSysFrameMode; /* main */

extern f32 gSinVu0Coefs[];
extern f32 gSinVu0Poly[];
/* The sqrt(1 - s * s) half of Math_SinCos (Math_Sin calls it). */
#if PLATFORM_PC
f32 Math_SinVu0(f32 x)
{
    unsigned int at __attribute__((unused)) = 0;
    unsigned int v0 __attribute__((unused)) = 0;
    unsigned int v1 __attribute__((unused)) = 0;
    unsigned int a3 __attribute__((unused)) = 0;
    unsigned int t0 __attribute__((unused)) = 0;
    f32 f0 __attribute__((unused)) = 0;
    int fcc __attribute__((unused)) = 0;
    int c0_ __attribute__((unused));
    int c1_ __attribute__((unused));
    int c2_ __attribute__((unused));
    int c3_ __attribute__((unused));
    f32 f12 __attribute__((unused)) = x;
    v0 = (unsigned int)((0xC0490FDB >> 16)) << 16; /* lui v0, (0xC0490FDB >> 16) */
    v0 = v0 | ((0xC0490FDB & 0xFFFF)); /* ori v0, v0, (0xC0490FDB & 0xFFFF) */
    f0 = PC_U2F(v0); /* mtc1 v0, f0 */
    /* nop */
    fcc = f12 <= f0; /* c.le.s f12, f0 */
    c0_ = fcc; /* bc1t L002EC308 */
    v0 = (unsigned int)((0x40490FDB >> 16)) << 16; /* lui v0, (0x40490FDB >> 16) */
    if (c0_)
        goto L002EC308;
    v0 = v0 | ((0x40490FDB & 0xFFFF)); /* ori v0, v0, (0x40490FDB & 0xFFFF) */
    f0 = PC_U2F(v0); /* mtc1 v0, f0 */
    /* nop */
    fcc = f12 < f0; /* c.lt.s f12, f0 */
    c1_ = fcc; /* bc1t L002EC358 */
    /* nop */
    if (c1_)
        goto L002EC358;
    /* nop */
L002EC308:;
    v0 = (unsigned int)(__SIZE_TYPE__)gSinVu0Coefs; /* la v0, gSinVu0Coefs */
    t0 = PC_F2U(f12); /* mfc1 t0, f12 */
    vu0_lq(6, v0); /* lqc2 vf6, 0x0(v0) */
    vu0_qmtc2(4, t0); /* qmtc2.ni t0, vf4 */
    t0 = (int)0 < (int)t0; /* slt t0, zero, t0 */
    vu0_ctc2(2, t0); /* ctc2.ni t0, vi2 */
    vu0_fmac(VU0_MUL, VU0_BX, 0, VU0_X, 7, 4, 6); /* vmulx.x vf7, vf4, vf6x */
    vu0_ftoi(0, VU0_X, 7, 7); /* vftoi0.x vf7, vf7 */
    vu0_mtir(1, 7, 0); /* vmtir vi1, vf7x */
    vu0_iaddi(3, 0, -2); /* viaddi vi3, vi0, -0x2 */
    vu0_iadd(1, 1, 2); /* viadd vi1, vi1, vi2 */
    vu0_iand(1, 1, 3); /* viand vi1, vi1, vi3 */
    vu0_mfir(VU0_X, 7, 1); /* vmfir.x vf7, vi1 */
    vu0_itof(0, VU0_X, 7, 7); /* vitof0.x vf7, vf7 */
    vu0_fmac(VU0_MUL, VU0_BY, 0, VU0_X, 7, 7, 6); /* vmuly.x vf7, vf7, vf6y */
    vu0_fmac(VU0_SUB, VU0_BVEC, 0, VU0_X, 7, 4, 7); /* vsub.x vf7, vf4, vf7 */
    t0 = vu0_qmfc2(7); /* qmfc2.ni t0, vf7 */
    f12 = PC_U2F(t0); /* mtc1 t0, f12 */
    /* nop */
L002EC358:;
    v1 = (unsigned int)(__SIZE_TYPE__)gSinVu0Poly; /* la v1, gSinVu0Poly */
    f0 = PC_U2F(0); /* mtc1 zero, f0 */
    /* nop */
    fcc = f12 < f0; /* c.lt.s f12, f0 */
    at = (unsigned int)((0x3FC90FDB >> 16)) << 16; /* lui at, (0x3FC90FDB >> 16) */
    v0 = at | ((0x3FC90FDB & 0xFFFF)); /* ori v0, at, (0x3FC90FDB & 0xFFFF) */
    c2_ = !fcc; /* bc1f L002EC390 */
    f0 = PC_U2F(v0); /* mtc1 v0, f0 */
    if (c2_)
        goto L002EC390;
    /* vnop */
    f12 = f0 + f12; /* add.s f12, f0, f12 */
    a3 = 0 + (0x1); /* addi a3, zero, 0x1 */
    goto L002EC398; /* j L002EC398 */
    /* vnop */
L002EC390:;
    f12 = f0 - f12; /* sub.s f12, f0, f12 */
    a3 = 0 + 0; /* addu a3, zero, zero */
L002EC398:;
    t0 = PC_F2U(f12); /* mfc1 t0, f12 */
    /* nop */
    vu0_qmtc2(6, t0); /* qmtc2.ni t0, vf6 */
    vu0_fmac(VU0_MUL, VU0_BX, 0, VU0_X, 7, 6, 6); /* vmulx.x vf7, vf6, vf6x */
    vu0_lq(5, v1); /* lqc2 vf5, 0x0(v1) */
    vu0_fmac(VU0_MUL, VU0_BX, 0, VU0_XYZW, 5, 5, 6); /* vmulx.xyzw vf5, vf5, vf6x */
    vu0_fmac(VU0_MUL, VU0_BW, 1, VU0_X, 0, 6, 0); /* vmulaw.x ACC, vf6, vf0w */
    vu0_fmac(VU0_MUL, VU0_BX, 0, VU0_X, 8, 7, 7); /* vmulx.x vf8, vf7, vf7x */
    vu0_fmac(VU0_SUB, VU0_BVEC, 0, 0x3, 6, 0, 0); /* vsub.zw vf6, vf0, vf0 */
    vu0_fmac(VU0_MUL, VU0_BX, 0, VU0_X, 9, 7, 8); /* vmulx.x vf9, vf7, vf8x */
    vu0_fmac(VU0_MUL, VU0_BX, 0, VU0_X, 10, 8, 8); /* vmulx.x vf10, vf8, vf8x */
    vu0_fmac(VU0_MADD, VU0_BW, 1, VU0_X, 0, 7, 5); /* vmaddaw.x ACC, vf7, vf5w */
    vu0_fmac(VU0_MADD, VU0_BZ, 1, VU0_X, 0, 8, 5); /* vmaddaz.x ACC, vf8, vf5z */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_X, 0, 9, 5); /* vmadday.x ACC, vf9, vf5y */
    vu0_fmac(VU0_MADD, VU0_BX, 0, VU0_X, 6, 10, 5); /* vmaddx.x vf6, vf10, vf5x */
    vu0_fmac(VU0_MUL, VU0_BVEC, 0, VU0_X, 7, 6, 6); /* vmul.x vf7, vf6, vf6 */
    vu0_fmac(VU0_SUB, VU0_BX, 0, VU0_W, 7, 0, 7); /* vsubx.w vf7, vf0, vf7x */
    vu0_vsqrt(7, 3); /* vsqrt Q, vf7w */
    c3_ = a3 != 0; /* bnez a3, L002EC3F8 */
    /* vwaitq */
    if (c3_)
        goto L002EC3F8;
    /* vnop */
    vu0_fmac(VU0_ADD, VU0_BQ, 0, VU0_X, 6, 0, 0); /* vaddq.x vf6, vf0, Q */
    goto L002EC400; /* b L002EC400 */
    /* vnop */
L002EC3F8:;
    vu0_fmac(VU0_SUB, VU0_BQ, 0, VU0_X, 6, 0, 0); /* vsubq.x vf6, vf0, Q */
    /* nop */
L002EC400:;
    t0 = vu0_qmfc2(6); /* qmfc2.ni t0, vf6 */
    f0 = PC_U2F(t0); /* mtc1 t0, f0 */
    /* nop */
    return f0; /* jr ra */
}
#else
asm f32 Math_SinVu0(f32 x)
{
    .set noreorder
    /* Handwritten function */
    lui v0, (0xC0490FDB >> 16)
    ori v0, v0, (0xC0490FDB & 0xFFFF)
    mtc1 v0, f0
    nop
    c.le.s f12, f0
    bc1t L002EC308
    lui v0, (0x40490FDB >> 16)
    ori v0, v0, (0x40490FDB & 0xFFFF)
    mtc1 v0, f0
    nop
    c.lt.s f12, f0
    bc1t L002EC358
    nop
    nop
L002EC308:
    la v0, gSinVu0Coefs
    mfc1 t0, f12
    lqc2 vf6, 0x0(v0)
    qmtc2.ni t0, vf4
    slt t0, zero, t0
    ctc2.ni t0, vi2
    vmulx.x vf7, vf4, vf6x
    vftoi0.x vf7, vf7
    vmtir vi1, vf7x
    viaddi vi3, vi0, -0x2
    viadd vi1, vi1, vi2
    viand vi1, vi1, vi3
    vmfir.x vf7, vi1
    vitof0.x vf7, vf7
    vmuly.x vf7, vf7, vf6y
    vsub.x vf7, vf4, vf7
    qmfc2.ni t0, vf7
    mtc1 t0, f12
    nop
L002EC358:
    la v1, gSinVu0Poly
    mtc1 zero, f0
    nop
    c.lt.s f12, f0
    lui at, (0x3FC90FDB >> 16)
    ori v0, at, (0x3FC90FDB & 0xFFFF)
    bc1f L002EC390
    mtc1 v0, f0
    vnop
    add.s f12, f0, f12
    j L002EC398
    addi a3, zero, 0x1 /* handwritten instruction */
    vnop
L002EC390:
    sub.s f12, f0, f12
    addu a3, zero, zero
L002EC398:
    mfc1 t0, f12
    nop
    qmtc2.ni t0, vf6
    vmulx.x vf7, vf6, vf6x
    lqc2 vf5, 0x0(v1)
    vmulx.xyzw vf5, vf5, vf6x
    vmulaw.x ACC, vf6, vf0w
    vmulx.x vf8, vf7, vf7x
    vsub.zw vf6, vf0, vf0
    vmulx.x vf9, vf7, vf8x
    vmulx.x vf10, vf8, vf8x
    vmaddaw.x ACC, vf7, vf5w
    vmaddaz.x ACC, vf8, vf5z
    vmadday.x ACC, vf9, vf5y
    vmaddx.x vf6, vf10, vf5x
    vmul.x vf7, vf6, vf6
    vsubx.w vf7, vf0, vf7x
    vsqrt Q, vf7w
    bnez a3, L002EC3F8
    vwaitq
    vnop
    b L002EC400
    vaddq.x vf6, vf0, Q
    vnop
L002EC3F8:
    vsubq.x vf6, vf0, Q
    nop
L002EC400:
    qmfc2.ni t0, vf6
    mtc1 t0, f0
    jr ra
    nop
}
#endif
/* The polynomial half of Math_SinCos. */
#if PLATFORM_PC
f32 Math_CosVu0(f32 x)
{
    unsigned int at __attribute__((unused)) = 0;
    unsigned int v0 __attribute__((unused)) = 0;
    unsigned int v1 __attribute__((unused)) = 0;
    unsigned int t0 __attribute__((unused)) = 0;
    unsigned int t1 __attribute__((unused)) = 0;
    f32 f0 __attribute__((unused)) = 0;
    int fcc __attribute__((unused)) = 0;
    int c0_ __attribute__((unused));
    int c1_ __attribute__((unused));
    int c2_ __attribute__((unused));
    f32 f12 __attribute__((unused)) = x;
    v0 = (unsigned int)((0xC0490FDB >> 16)) << 16; /* lui v0, (0xC0490FDB >> 16) */
    v0 = v0 | ((0xC0490FDB & 0xFFFF)); /* ori v0, v0, (0xC0490FDB & 0xFFFF) */
    f0 = PC_U2F(v0); /* mtc1 v0, f0 */
    /* nop */
    fcc = f12 <= f0; /* c.le.s f12, f0 */
    c0_ = fcc; /* bc1t L002EC448 */
    v0 = (unsigned int)((0x40490FDB >> 16)) << 16; /* lui v0, (0x40490FDB >> 16) */
    if (c0_)
        goto L002EC448;
    v0 = v0 | ((0x40490FDB & 0xFFFF)); /* ori v0, v0, (0x40490FDB & 0xFFFF) */
    f0 = PC_U2F(v0); /* mtc1 v0, f0 */
    /* nop */
    fcc = f12 < f0; /* c.lt.s f12, f0 */
    c1_ = fcc; /* bc1t L002EC498 */
    /* nop */
    if (c1_)
        goto L002EC498;
    /* nop */
L002EC448:;
    v0 = (unsigned int)(__SIZE_TYPE__)gSinVu0Coefs; /* la v0, gSinVu0Coefs */
    t0 = PC_F2U(f12); /* mfc1 t0, f12 */
    vu0_lq(6, v0); /* lqc2 vf6, 0x0(v0) */
    vu0_qmtc2(4, t0); /* qmtc2.ni t0, vf4 */
    t0 = (int)0 < (int)t0; /* slt t0, zero, t0 */
    vu0_ctc2(2, t0); /* ctc2.ni t0, vi2 */
    vu0_fmac(VU0_MUL, VU0_BX, 0, VU0_X, 7, 4, 6); /* vmulx.x vf7, vf4, vf6x */
    vu0_ftoi(0, VU0_X, 7, 7); /* vftoi0.x vf7, vf7 */
    vu0_mtir(1, 7, 0); /* vmtir vi1, vf7x */
    vu0_iaddi(3, 0, -2); /* viaddi vi3, vi0, -0x2 */
    vu0_iadd(1, 1, 2); /* viadd vi1, vi1, vi2 */
    vu0_iand(1, 1, 3); /* viand vi1, vi1, vi3 */
    vu0_mfir(VU0_X, 7, 1); /* vmfir.x vf7, vi1 */
    vu0_itof(0, VU0_X, 7, 7); /* vitof0.x vf7, vf7 */
    vu0_fmac(VU0_MUL, VU0_BY, 0, VU0_X, 7, 7, 6); /* vmuly.x vf7, vf7, vf6y */
    vu0_fmac(VU0_SUB, VU0_BVEC, 0, VU0_X, 7, 4, 7); /* vsub.x vf7, vf4, vf7 */
    t0 = vu0_qmfc2(7); /* qmfc2.ni t0, vf7 */
    f12 = PC_U2F(t0); /* mtc1 t0, f12 */
    /* nop */
L002EC498:;
    v1 = (unsigned int)(__SIZE_TYPE__)gSinVu0Poly; /* la v1, gSinVu0Poly */
    f0 = PC_U2F(0); /* mtc1 zero, f0 */
    /* nop */
    fcc = f12 < f0; /* c.lt.s f12, f0 */
    at = (unsigned int)((0x3FC90FDB >> 16)) << 16; /* lui at, (0x3FC90FDB >> 16) */
    v0 = at | ((0x3FC90FDB & 0xFFFF)); /* ori v0, at, (0x3FC90FDB & 0xFFFF) */
    c2_ = !fcc; /* bc1f L002EC4D0 */
    f0 = PC_U2F(v0); /* mtc1 v0, f0 */
    if (c2_)
        goto L002EC4D0;
    /* vnop */
    f12 = f0 + f12; /* add.s f12, f0, f12 */
    goto L002EC4D8; /* j L002EC4D8 */
    /* vnop */
    /* nop */
L002EC4D0:;
    f12 = f0 - f12; /* sub.s f12, f0, f12 */
    /* nop */
L002EC4D8:;
    t0 = PC_F2U(f12); /* mfc1 t0, f12 */
    /* nop */
    vu0_qmtc2(6, t0); /* qmtc2.ni t0, vf6 */
    vu0_fmac(VU0_MUL, VU0_BX, 0, VU0_X, 7, 6, 6); /* vmulx.x vf7, vf6, vf6x */
    vu0_lq(5, v1); /* lqc2 vf5, 0x0(v1) */
    vu0_fmac(VU0_MUL, VU0_BX, 0, VU0_XYZW, 5, 5, 6); /* vmulx.xyzw vf5, vf5, vf6x */
    vu0_fmac(VU0_MUL, VU0_BW, 1, VU0_X, 0, 6, 0); /* vmulaw.x ACC, vf6, vf0w */
    vu0_fmac(VU0_MUL, VU0_BX, 0, VU0_X, 8, 7, 7); /* vmulx.x vf8, vf7, vf7x */
    vu0_fmac(VU0_SUB, VU0_BVEC, 0, 0x7, 6, 0, 0); /* vsub.yzw vf6, vf0, vf0 */
    vu0_fmac(VU0_MUL, VU0_BX, 0, VU0_X, 9, 7, 8); /* vmulx.x vf9, vf7, vf8x */
    vu0_fmac(VU0_MUL, VU0_BX, 0, VU0_X, 10, 8, 8); /* vmulx.x vf10, vf8, vf8x */
    vu0_fmac(VU0_MADD, VU0_BW, 1, VU0_X, 0, 7, 5); /* vmaddaw.x ACC, vf7, vf5w */
    vu0_fmac(VU0_MADD, VU0_BZ, 1, VU0_X, 0, 8, 5); /* vmaddaz.x ACC, vf8, vf5z */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_X, 0, 9, 5); /* vmadday.x ACC, vf9, vf5y */
    vu0_fmac(VU0_MADD, VU0_BX, 0, VU0_X, 6, 10, 5); /* vmaddx.x vf6, vf10, vf5x */
    /* vnop */
    t1 = vu0_qmfc2(6); /* qmfc2.ni t1, vf6 */
    f0 = PC_U2F(t1); /* mtc1 t1, f0 */
    /* nop */
    return f0; /* jr ra */
}
#else
asm f32 Math_CosVu0(f32 x)
{
    .set noreorder
    lui v0, (0xC0490FDB >> 16)
    ori v0, v0, (0xC0490FDB & 0xFFFF)
    mtc1 v0, f0
    nop
    c.le.s f12, f0
    bc1t L002EC448
    lui v0, (0x40490FDB >> 16)
    ori v0, v0, (0x40490FDB & 0xFFFF)
    mtc1 v0, f0
    nop
    c.lt.s f12, f0
    bc1t L002EC498
    nop
    nop
L002EC448:
    la v0, gSinVu0Coefs
    mfc1 t0, f12
    lqc2 vf6, 0x0(v0)
    qmtc2.ni t0, vf4
    slt t0, zero, t0
    ctc2.ni t0, vi2
    vmulx.x vf7, vf4, vf6x
    vftoi0.x vf7, vf7
    vmtir vi1, vf7x
    viaddi vi3, vi0, -0x2
    viadd vi1, vi1, vi2
    viand vi1, vi1, vi3
    vmfir.x vf7, vi1
    vitof0.x vf7, vf7
    vmuly.x vf7, vf7, vf6y
    vsub.x vf7, vf4, vf7
    qmfc2.ni t0, vf7
    mtc1 t0, f12
    nop
L002EC498:
    la v1, gSinVu0Poly
    mtc1 zero, f0
    nop
    c.lt.s f12, f0
    lui at, (0x3FC90FDB >> 16)
    ori v0, at, (0x3FC90FDB & 0xFFFF)
    bc1f L002EC4D0
    mtc1 v0, f0
    vnop
    j L002EC4D8
    add.s f12, f0, f12
    vnop
    nop
L002EC4D0:
    sub.s f12, f0, f12
    nop
L002EC4D8:
    mfc1 t0, f12
    nop
    qmtc2.ni t0, vf6
    vmulx.x vf7, vf6, vf6x
    lqc2 vf5, 0x0(v1)
    vmulx.xyzw vf5, vf5, vf6x
    vmulaw.x ACC, vf6, vf0w
    vmulx.x vf8, vf7, vf7x
    vsub.yzw vf6, vf0, vf0
    vmulx.x vf9, vf7, vf8x
    vmulx.x vf10, vf8, vf8x
    vmaddaw.x ACC, vf7, vf5w
    vmaddaz.x ACC, vf8, vf5z
    vmadday.x ACC, vf9, vf5y
    vmaddx.x vf6, vf10, vf5x
    vnop
    qmfc2.ni t1, vf6
    mtc1 t1, f0
    jr ra
    nop
}
#endif
/* out[0] = sin a, out[1] = cos a (VU0 polynomial). Static: Math_GetSinCos
 * keeps values in t1 and a2 across the call, which MWCC only does for a
 * static callee it has compiled. */
#if PLATFORM_PC
static void Math_SinCos(f32 *out, f32 angle)
{
    unsigned int at __attribute__((unused)) = 0;
    unsigned int v1 __attribute__((unused)) = 0;
    unsigned int a1 __attribute__((unused)) = 0;
    unsigned int a3 __attribute__((unused)) = 0;
    unsigned int t0 __attribute__((unused)) = 0;
    f32 f0 __attribute__((unused)) = 0;
    int fcc __attribute__((unused)) = 0;
    int c0_ __attribute__((unused));
    int c1_ __attribute__((unused));
    int c2_ __attribute__((unused));
    int c3_ __attribute__((unused));
    unsigned int a0 __attribute__((unused)) = (unsigned int)(__SIZE_TYPE__)out;
    f32 f12 __attribute__((unused)) = angle;
    v1 = (unsigned int)((0xC0490FDB >> 16)) << 16; /* lui v1, (0xC0490FDB >> 16) */
    v1 = v1 | ((0xC0490FDB & 0xFFFF)); /* ori v1, v1, (0xC0490FDB & 0xFFFF) */
    f0 = PC_U2F(v1); /* mtc1 v1, f0 */
    /* nop */
    fcc = f12 <= f0; /* c.le.s f12, f0 */
    c0_ = fcc; /* bc1t L002EC568 */
    v1 = (unsigned int)((0x40490FDB >> 16)) << 16; /* lui v1, (0x40490FDB >> 16) */
    if (c0_)
        goto L002EC568;
    v1 = v1 | ((0x40490FDB & 0xFFFF)); /* ori v1, v1, (0x40490FDB & 0xFFFF) */
    f0 = PC_U2F(v1); /* mtc1 v1, f0 */
    /* nop */
    fcc = f12 < f0; /* c.lt.s f12, f0 */
    c1_ = fcc; /* bc1t L002EC5B8 */
    /* nop */
    if (c1_)
        goto L002EC5B8;
    /* nop */
L002EC568:;
    v1 = (unsigned int)(__SIZE_TYPE__)gSinVu0Coefs; /* la v1, gSinVu0Coefs */
    t0 = PC_F2U(f12); /* mfc1 t0, f12 */
    vu0_lq(6, v1); /* lqc2 vf6, 0x0(v1) */
    vu0_qmtc2(4, t0); /* qmtc2.ni t0, vf4 */
    t0 = (int)0 < (int)t0; /* slt t0, zero, t0 */
    vu0_ctc2(2, t0); /* ctc2.ni t0, vi2 */
    vu0_fmac(VU0_MUL, VU0_BX, 0, VU0_X, 7, 4, 6); /* vmulx.x vf7, vf4, vf6x */
    vu0_ftoi(0, VU0_X, 7, 7); /* vftoi0.x vf7, vf7 */
    vu0_mtir(1, 7, 0); /* vmtir vi1, vf7x */
    vu0_iaddi(3, 0, -2); /* viaddi vi3, vi0, -0x2 */
    vu0_iadd(1, 1, 2); /* viadd vi1, vi1, vi2 */
    vu0_iand(1, 1, 3); /* viand vi1, vi1, vi3 */
    vu0_mfir(VU0_X, 7, 1); /* vmfir.x vf7, vi1 */
    vu0_itof(0, VU0_X, 7, 7); /* vitof0.x vf7, vf7 */
    vu0_fmac(VU0_MUL, VU0_BY, 0, VU0_X, 7, 7, 6); /* vmuly.x vf7, vf7, vf6y */
    vu0_fmac(VU0_SUB, VU0_BVEC, 0, VU0_X, 7, 4, 7); /* vsub.x vf7, vf4, vf7 */
    t0 = vu0_qmfc2(7); /* qmfc2.ni t0, vf7 */
    f12 = PC_U2F(t0); /* mtc1 t0, f12 */
    /* nop */
L002EC5B8:;
    a1 = (unsigned int)(__SIZE_TYPE__)gSinVu0Poly; /* la a1, gSinVu0Poly */
    f0 = PC_U2F(0); /* mtc1 zero, f0 */
    /* nop */
    fcc = f12 < f0; /* c.lt.s f12, f0 */
    at = (unsigned int)((0x3FC90FDB >> 16)) << 16; /* lui at, (0x3FC90FDB >> 16) */
    v1 = at | ((0x3FC90FDB & 0xFFFF)); /* ori v1, at, (0x3FC90FDB & 0xFFFF) */
    c2_ = !fcc; /* bc1f L002EC5F0 */
    f0 = PC_U2F(v1); /* mtc1 v1, f0 */
    if (c2_)
        goto L002EC5F0;
    /* vnop */
    f12 = f0 + f12; /* add.s f12, f0, f12 */
    a3 = 0 + (0x1); /* addi a3, zero, 0x1 */
    goto L002EC5F8; /* j L002EC5F8 */
    /* vnop */
L002EC5F0:;
    f12 = f0 - f12; /* sub.s f12, f0, f12 */
    a3 = 0 + 0; /* addu a3, zero, zero */
L002EC5F8:;
    t0 = PC_F2U(f12); /* mfc1 t0, f12 */
    /* nop */
    vu0_qmtc2(6, t0); /* qmtc2.ni t0, vf6 */
    vu0_fmac(VU0_MUL, VU0_BX, 0, VU0_X, 7, 6, 6); /* vmulx.x vf7, vf6, vf6x */
    vu0_lq(5, a1); /* lqc2 vf5, 0x0(a1) */
    vu0_fmac(VU0_MUL, VU0_BX, 0, VU0_XYZW, 5, 5, 6); /* vmulx.xyzw vf5, vf5, vf6x */
    vu0_fmac(VU0_MUL, VU0_BW, 1, VU0_X, 0, 6, 0); /* vmulaw.x ACC, vf6, vf0w */
    vu0_fmac(VU0_MUL, VU0_BX, 0, VU0_X, 8, 7, 7); /* vmulx.x vf8, vf7, vf7x */
    vu0_fmac(VU0_SUB, VU0_BVEC, 0, 0x3, 6, 0, 0); /* vsub.zw vf6, vf0, vf0 */
    vu0_fmac(VU0_MUL, VU0_BX, 0, VU0_X, 9, 7, 8); /* vmulx.x vf9, vf7, vf8x */
    vu0_fmac(VU0_MUL, VU0_BX, 0, VU0_X, 10, 8, 8); /* vmulx.x vf10, vf8, vf8x */
    vu0_fmac(VU0_MADD, VU0_BW, 1, VU0_X, 0, 7, 5); /* vmaddaw.x ACC, vf7, vf5w */
    vu0_fmac(VU0_MADD, VU0_BZ, 1, VU0_X, 0, 8, 5); /* vmaddaz.x ACC, vf8, vf5z */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_X, 0, 9, 5); /* vmadday.x ACC, vf9, vf5y */
    vu0_fmac(VU0_MADD, VU0_BX, 0, VU0_X, 6, 10, 5); /* vmaddx.x vf6, vf10, vf5x */
    vu0_fmac(VU0_MUL, VU0_BVEC, 0, VU0_X, 7, 6, 6); /* vmul.x vf7, vf6, vf6 */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_Y, 6, 0, 6); /* vaddx.y vf6, vf0, vf6x */
    vu0_fmac(VU0_SUB, VU0_BX, 0, VU0_W, 7, 0, 7); /* vsubx.w vf7, vf0, vf7x */
    vu0_vsqrt(7, 3); /* vsqrt Q, vf7w */
    c3_ = a3 != 0; /* bnez a3, L002EC660 */
    /* vwaitq */
    if (c3_)
        goto L002EC660;
    /* vnop */
    vu0_fmac(VU0_ADD, VU0_BQ, 0, VU0_X, 6, 0, 0); /* vaddq.x vf6, vf0, Q */
    goto L002EC668; /* b L002EC668 */
    /* vnop */
    /* nop */
L002EC660:;
    vu0_fmac(VU0_SUB, VU0_BQ, 0, VU0_X, 6, 0, 0); /* vsubq.x vf6, vf0, Q */
    /* nop */
L002EC668:;
    vu0_sq(6, a0); /* sqc2 vf6, 0x0(a0) */
    return; /* jr ra */
}
#else
static asm void Math_SinCos(f32 *out, f32 angle)
{
    .set noreorder
    /* Handwritten function */
    lui v1, (0xC0490FDB >> 16)
    ori v1, v1, (0xC0490FDB & 0xFFFF)
    mtc1 v1, f0
    nop
    c.le.s f12, f0
    bc1t L002EC568
    lui v1, (0x40490FDB >> 16)
    ori v1, v1, (0x40490FDB & 0xFFFF)
    mtc1 v1, f0
    nop
    c.lt.s f12, f0
    bc1t L002EC5B8
    nop
    nop
L002EC568:
    la v1, gSinVu0Coefs
    mfc1 t0, f12
    lqc2 vf6, 0x0(v1)
    qmtc2.ni t0, vf4
    slt t0, zero, t0
    ctc2.ni t0, vi2
    vmulx.x vf7, vf4, vf6x
    vftoi0.x vf7, vf7
    vmtir vi1, vf7x
    viaddi vi3, vi0, -0x2
    viadd vi1, vi1, vi2
    viand vi1, vi1, vi3
    vmfir.x vf7, vi1
    vitof0.x vf7, vf7
    vmuly.x vf7, vf7, vf6y
    vsub.x vf7, vf4, vf7
    qmfc2.ni t0, vf7
    mtc1 t0, f12
    nop
L002EC5B8:
    la a1, gSinVu0Poly
    mtc1 zero, f0
    nop
    c.lt.s f12, f0
    lui at, (0x3FC90FDB >> 16)
    ori v1, at, (0x3FC90FDB & 0xFFFF)
    bc1f L002EC5F0
    mtc1 v1, f0
    vnop
    add.s f12, f0, f12
    j L002EC5F8
    addi a3, zero, 0x1 /* handwritten instruction */
    vnop
L002EC5F0:
    sub.s f12, f0, f12
    addu a3, zero, zero
L002EC5F8:
    mfc1 t0, f12
    nop
    qmtc2.ni t0, vf6
    vmulx.x vf7, vf6, vf6x
    lqc2 vf5, 0x0(a1)
    vmulx.xyzw vf5, vf5, vf6x
    vmulaw.x ACC, vf6, vf0w
    vmulx.x vf8, vf7, vf7x
    vsub.zw vf6, vf0, vf0
    vmulx.x vf9, vf7, vf8x
    vmulx.x vf10, vf8, vf8x
    vmaddaw.x ACC, vf7, vf5w
    vmaddaz.x ACC, vf8, vf5z
    vmadday.x ACC, vf9, vf5y
    vmaddx.x vf6, vf10, vf5x
    vmul.x vf7, vf6, vf6
    vaddx.y vf6, vf0, vf6x
    vsubx.w vf7, vf0, vf7x
    vsqrt Q, vf7w
    bnez a3, L002EC660
    vwaitq
    vnop
    b L002EC668
    vaddq.x vf6, vf0, Q
    vnop
    nop
L002EC660:
    vsubq.x vf6, vf0, Q
    nop
L002EC668:
    jr ra
    sqc2 vf6, 0x0(a0)
}
#endif

extern MtxSlot gMtxSlots[];

/* The camera's matrices. */
extern f32 gMtxProj[16];
extern f32 gViewportMtx[16];
extern f32 gMtxProjClip[16];
extern f32 gMtxProjScreen[16];
extern f32 gMtxScreen[16];
extern f32 gMtxCamProj[16];
extern f32 gMtxClip[16];

/*
 * main's render state (the viewport), named in main.txt
 * (gSysFrameMode and gDispWidth are declared at the top of the file).
 */
extern f32 gViewClipH;
extern f32 gViewClipW;
extern f32 gViewCenterY;
extern f32 gViewCenterX;
extern f32 gViewH;
extern f32 gViewW;
extern f32 gViewY;
extern f32 gViewX;
extern f32 gSysZMax;
extern s32 gDispHeight;

/* Matrix slot `slot` = a perspective projection (Mtx_Perspective, Camera_StoreProjection), then
 * the camera matrices. */
void Camera_SetPerspective(s32 slot, f32 farZ, f32 nearZ, f32 fov, f32 aspect)
{
    f32 *mtx = gMtxSlots[slot].m[0];

    Mtx_Perspective(mtx, farZ, nearZ, fov, aspect);
    Camera_StoreProjection(mtx);
    Mtx_Mul(gMtxProjClip, gMtxCamProj, gMtxClip);
    Mtx_Mul(gMtxProjScreen, gMtxCamProj, gMtxScreen);
}

/* mtx = a perspective projection: depth nearZ to farZ, vertical field of view fov (radians),
 * aspect ratio aspect. The far depth comes first: the callers pass Camera.farZ there, and
 * the depth terms are only right that way round. */
void Mtx_Perspective(f32 *mtx, f32 farZ, f32 nearZ, f32 fov, f32 aspect)
{
    f32 cosHalf;
    f32 sinHalf;

    fov *= 0.5f;
    Math_GetSinCos(&sinHalf, &cosHalf, fov);
    Mtx_Identity(mtx);
    mtx[0] = cosHalf / sinHalf / aspect;
    mtx[5] = cosHalf / sinHalf;
    mtx[10] = -(farZ + nearZ) / (farZ - nearZ);
    mtx[11] = -1.0f;
    mtx[14] = -(2.0f * farZ * nearZ) / (farZ - nearZ);
    mtx[15] = 0.0f;
}

/* gViewportMtx = proj (a projection) with its x and y scale fitted to the viewport.
 * This function and the two viewport ones below read main's render-state globals by
 * name: as raw addresses, MWCC can't move their loads above the matrix stores. */
void Camera_StoreProjection(f32 *proj)
{
    Mtx_Copy(gViewportMtx, proj);
    gViewportMtx[0] = 2.0f * gViewportMtx[0] / (gViewClipW / (0.5f * gViewW));
    gViewportMtx[5] = 2.0f * gViewportMtx[5] / (gViewClipH / (0.5f * gViewH));
}

/* Matrix slot `slot` = an orthographic screen matrix for the viewport (x, y, width, height),
 * then the camera matrices and the GS scissor (y and height halved while gSysFrameMode is
 * 0: a field, half the lines). */
void Camera_SetOrtho(s32 slot, s32 x, s32 y, s32 width, s32 height, f32 nearZ, f32 farZ)
{
    Camera_SetViewport(gMtxSlots[slot].m[0], x, y, width, height, nearZ, farZ);
    Camera_StoreProjection(gMtxCamProj);
    Mtx_Mul(gMtxProjClip, gMtxCamProj, gMtxClip);
    Mtx_Mul(gMtxProjScreen, gMtxCamProj, gMtxScreen);
    if (gSysFrameMode == FRAME_MODE_FIELD) {
        y /= 2;
        height /= 2;
    }
    Gs_SetScissor(x, y, width, height, 2);
}

/*
 * The viewport (x, y, width, height; depth nearZ to farZ) into gMtxScreen and the screen
 * matrix `screen` (centred on the GS's 2048, 2048), and the viewport globals of main;
 * then Camera_SetViewportMtx.
 */
void Camera_SetViewport(f32 *screen, s32 x, s32 y, s32 width, s32 height, f32 nearZ, f32 farZ)
{
    f32 halfW;
    f32 halfH;
    f32 depth;

    halfW = width / 2;
    halfH = height / 2;
    Mtx_Identity(gMtxScreen);
    gMtxScreen[0] = halfW;
    gMtxScreen[5] = -halfH;
    depth = farZ - nearZ;
    gMtxScreen[10] = depth;
    gMtxScreen[12] = x + halfW;
    gMtxScreen[13] = y + halfH;
    gMtxScreen[14] = nearZ;
    gMtxScreen[15] = 1.0f;
    if (gSysFrameMode == FRAME_MODE_FIELD) {
        y /= 2;
        height /= 2;
    }
    halfW = width / 2;
    halfH = height / 2;
    Mtx_Identity(screen);
    screen[0] = halfW;
    screen[5] = -halfH;
    screen[10] = depth;
    screen[12] = x + halfW;
    screen[13] = y + halfH;
    screen[15] = 1.0f;
    screen[10] *= 0.5f * -gSysZMax;
    screen[12] += 2048 - (gDispWidth >> 1);
    screen[13] += 2048 - (gDispHeight >> 1);
    screen[14] = 0.5f * gSysZMax;
    gViewCenterX = screen[12];
    gViewCenterY = screen[13];
    gViewX = x;
    gViewY = y;
    gViewW = width;
    gViewH = height;
    gViewClipW = 2048.0f - gViewCenterX;
    if (gViewClipW < 0.0f) {
        gViewClipW *= -1.0f;
    }
    gViewClipW = 2048.0f - gViewClipW;
    gViewClipH = 2048.0f - gViewCenterY;
    if (gViewClipH < 0.0f) {
        gViewClipH *= -1.0f;
    }
    gViewClipH = 2048.0f - gViewClipH;
    Camera_SetViewportMtx(0, 0, gViewClipW, gViewClipH, nearZ, farZ);
}

/* gMtxProj = the viewport matrix for a width x height screen (depth nearZ to farZ). */
void Camera_SetViewportMtx(s32 x, s32 y, s32 width, s32 height, f32 nearZ, f32 farZ)
{
    f32 halfW;
    f32 halfH;

    Mtx_Identity(gMtxProj);
    halfW = width / 2;
    halfH = height / 2;
    gMtxProj[0] = halfW;
    gMtxProj[5] = -halfH;
    gMtxProj[15] = 1.0f;
    gMtxProj[10] = farZ - nearZ;
    gMtxProj[10] *= 0.5f * -gSysZMax;
    gMtxProj[14] = 0.5f * gSysZMax;
    gMtxProj[12] = gViewCenterX;
    gMtxProj[13] = gViewCenterY;
}
extern u8 gMtxIdentity[];
/* mtx = the identity matrix (copied from gMtxIdentity). */
#if PLATFORM_PC
void Mtx_Identity(f32 *mtx)
{
    unsigned int v1 __attribute__((unused)) = 0;
    unsigned int a0 __attribute__((unused)) = (unsigned int)(__SIZE_TYPE__)mtx;
    v1 = (unsigned int)(__SIZE_TYPE__)gMtxIdentity; /* la v1, gMtxIdentity */
    vu0_lq(4, v1); /* lqc2 vf4, 0x0(v1) */
    vu0_lq(5, v1 + 0x10); /* lqc2 vf5, 0x10(v1) */
    vu0_lq(6, v1 + 0x20); /* lqc2 vf6, 0x20(v1) */
    vu0_lq(7, v1 + 0x30); /* lqc2 vf7, 0x30(v1) */
    vu0_sq(4, a0); /* sqc2 vf4, 0x0(a0) */
    vu0_sq(5, a0 + 0x10); /* sqc2 vf5, 0x10(a0) */
    vu0_sq(6, a0 + 0x20); /* sqc2 vf6, 0x20(a0) */
    vu0_sq(7, a0 + 0x30); /* sqc2 vf7, 0x30(a0) */
    return; /* jr ra */
}
#else
asm void Mtx_Identity(f32 *mtx)
{
    .set noreorder
    la v1, gMtxIdentity
    lqc2 vf4, 0x0(v1)
    lqc2 vf5, 0x10(v1)
    lqc2 vf6, 0x20(v1)
    lqc2 vf7, 0x30(v1)
    sqc2 vf4, 0x0(a0)
    sqc2 vf5, 0x10(a0)
    sqc2 vf6, 0x20(a0)
    jr ra
    sqc2 vf7, 0x30(a0)
}
#endif
/* The 3x3 part of mtx becomes the identity; the translation stays. */
#if PLATFORM_PC
void Mtx_ClearRotation(f32 *mtx)
{
    unsigned int v1 __attribute__((unused)) = 0;
    unsigned int a0 __attribute__((unused)) = (unsigned int)(__SIZE_TYPE__)mtx;
    v1 = (unsigned int)(__SIZE_TYPE__)gMtxIdentity; /* la v1, gMtxIdentity */
    vu0_lq(4, v1); /* lqc2 vf4, 0x0(v1) */
    vu0_lq(5, v1 + 0x10); /* lqc2 vf5, 0x10(v1) */
    vu0_lq(6, v1 + 0x20); /* lqc2 vf6, 0x20(v1) */
    vu0_lq(8, a0); /* lqc2 vf8, 0x0(a0) */
    vu0_lq(9, a0 + 0x10); /* lqc2 vf9, 0x10(a0) */
    vu0_lq(10, a0 + 0x20); /* lqc2 vf10, 0x20(a0) */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, VU0_XYZ, 8, 0, 4); /* vadd.xyz vf8, vf0, vf4 */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, VU0_XYZ, 9, 0, 5); /* vadd.xyz vf9, vf0, vf5 */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, VU0_XYZ, 10, 0, 6); /* vadd.xyz vf10, vf0, vf6 */
    vu0_sq(8, a0); /* sqc2 vf8, 0x0(a0) */
    vu0_sq(9, a0 + 0x10); /* sqc2 vf9, 0x10(a0) */
    vu0_sq(10, a0 + 0x20); /* sqc2 vf10, 0x20(a0) */
    return; /* jr ra */
}
#else
asm void Mtx_ClearRotation(f32 *mtx)
{
    .set noreorder
    la v1, gMtxIdentity
    lqc2 vf4, 0x0(v1)
    lqc2 vf5, 0x10(v1)
    lqc2 vf6, 0x20(v1)
    lqc2 vf8, 0x0(a0)
    lqc2 vf9, 0x10(a0)
    lqc2 vf10, 0x20(a0)
    vadd.xyz vf8, vf0, vf4
    vadd.xyz vf9, vf0, vf5
    vadd.xyz vf10, vf0, vf6
    sqc2 vf8, 0x0(a0)
    sqc2 vf9, 0x10(a0)
    jr ra
    sqc2 vf10, 0x20(a0)
}
#endif
/* mtx = a scale matrix. */
#if PLATFORM_PC
void Mtx_Scale(f32 *mtx, f32 x, f32 y, f32 z)
{
    unsigned int v1 __attribute__((unused)) = 0;
    unsigned int a2 __attribute__((unused)) = 0;
    unsigned int a3 __attribute__((unused)) = 0;
    unsigned int t0 __attribute__((unused)) = 0;
    unsigned int a0 __attribute__((unused)) = (unsigned int)(__SIZE_TYPE__)mtx;
    f32 f12 __attribute__((unused)) = x;
    f32 f13 __attribute__((unused)) = y;
    f32 f14 __attribute__((unused)) = z;
    v1 = (unsigned int)(__SIZE_TYPE__)gMtxIdentity; /* la v1, gMtxIdentity */
    a2 = PC_F2U(f12); /* mfc1 a2, f12 */
    a3 = PC_F2U(f13); /* mfc1 a3, f13 */
    t0 = PC_F2U(f14); /* mfc1 t0, f14 */
    vu0_lq(4, v1); /* lqc2 vf4, 0x0(v1) */
    vu0_qmtc2(8, a2); /* qmtc2.ni a2, vf8 */
    vu0_qmtc2(9, a3); /* qmtc2.ni a3, vf9 */
    vu0_qmtc2(10, t0); /* qmtc2.ni t0, vf10 */
    vu0_lq(5, v1 + 0x10); /* lqc2 vf5, 0x10(v1) */
    vu0_lq(6, v1 + 0x20); /* lqc2 vf6, 0x20(v1) */
    vu0_lq(7, v1 + 0x30); /* lqc2 vf7, 0x30(v1) */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_X, 4, 0, 8); /* vaddx.x vf4, vf0, vf8x */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_Y, 5, 0, 9); /* vaddx.y vf5, vf0, vf9x */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_Z, 6, 0, 10); /* vaddx.z vf6, vf0, vf10x */
    vu0_sq(7, a0 + 0x30); /* sqc2 vf7, 0x30(a0) */
    vu0_sq(4, a0); /* sqc2 vf4, 0x0(a0) */
    vu0_sq(5, a0 + 0x10); /* sqc2 vf5, 0x10(a0) */
    vu0_sq(6, a0 + 0x20); /* sqc2 vf6, 0x20(a0) */
    return; /* jr ra */
}
#else
asm void Mtx_Scale(f32 *mtx, f32 x, f32 y, f32 z)
{
    .set noreorder
    la v1, gMtxIdentity
    mfc1 a2, f12
    mfc1 a3, f13
    mfc1 t0, f14
    lqc2 vf4, 0x0(v1)
    qmtc2.ni a2, vf8
    qmtc2.ni a3, vf9
    qmtc2.ni t0, vf10
    lqc2 vf5, 0x10(v1)
    lqc2 vf6, 0x20(v1)
    lqc2 vf7, 0x30(v1)
    vaddx.x vf4, vf0, vf8x
    vaddx.y vf5, vf0, vf9x
    vaddx.z vf6, vf0, vf10x
    sqc2 vf7, 0x30(a0)
    sqc2 vf4, 0x0(a0)
    sqc2 vf5, 0x10(a0)
    jr ra
    sqc2 vf6, 0x20(a0)
}
#endif
/* The 3x3 part of mtx becomes a scale; the translation stays. */
#if PLATFORM_PC
void Mtx_SetScale(f32 *mtx, f32 x, f32 y, f32 z)
{
    unsigned int v1 __attribute__((unused)) = 0;
    unsigned int a2 __attribute__((unused)) = 0;
    unsigned int a3 __attribute__((unused)) = 0;
    unsigned int t0 __attribute__((unused)) = 0;
    unsigned int a0 __attribute__((unused)) = (unsigned int)(__SIZE_TYPE__)mtx;
    f32 f12 __attribute__((unused)) = x;
    f32 f13 __attribute__((unused)) = y;
    f32 f14 __attribute__((unused)) = z;
    v1 = (unsigned int)(__SIZE_TYPE__)gMtxIdentity; /* la v1, gMtxIdentity */
    a2 = PC_F2U(f12); /* mfc1 a2, f12 */
    a3 = PC_F2U(f13); /* mfc1 a3, f13 */
    t0 = PC_F2U(f14); /* mfc1 t0, f14 */
    vu0_lq(4, v1); /* lqc2 vf4, 0x0(v1) */
    vu0_qmtc2(20, a2); /* qmtc2.ni a2, vf20 */
    vu0_qmtc2(21, a3); /* qmtc2.ni a3, vf21 */
    vu0_qmtc2(22, t0); /* qmtc2.ni t0, vf22 */
    vu0_lq(5, v1 + 0x10); /* lqc2 vf5, 0x10(v1) */
    vu0_lq(6, v1 + 0x20); /* lqc2 vf6, 0x20(v1) */
    vu0_lq(8, a0); /* lqc2 vf8, 0x0(a0) */
    vu0_lq(9, a0 + 0x10); /* lqc2 vf9, 0x10(a0) */
    vu0_lq(10, a0 + 0x20); /* lqc2 vf10, 0x20(a0) */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_X, 4, 0, 20); /* vaddx.x vf4, vf0, vf20x */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_Y, 5, 0, 21); /* vaddx.y vf5, vf0, vf21x */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_Z, 6, 0, 22); /* vaddx.z vf6, vf0, vf22x */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_W, 4, 8, 0); /* vaddx.w vf4, vf8, vf0x */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_W, 5, 9, 0); /* vaddx.w vf5, vf9, vf0x */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_W, 6, 10, 0); /* vaddx.w vf6, vf10, vf0x */
    vu0_sq(4, a0); /* sqc2 vf4, 0x0(a0) */
    vu0_sq(5, a0 + 0x10); /* sqc2 vf5, 0x10(a0) */
    vu0_sq(6, a0 + 0x20); /* sqc2 vf6, 0x20(a0) */
    return; /* jr ra */
}
#else
asm void Mtx_SetScale(f32 *mtx, f32 x, f32 y, f32 z)
{
    .set noreorder
    la v1, gMtxIdentity
    mfc1 a2, f12
    mfc1 a3, f13
    mfc1 t0, f14
    lqc2 vf4, 0x0(v1)
    qmtc2.ni a2, vf20
    qmtc2.ni a3, vf21
    qmtc2.ni t0, vf22
    lqc2 vf5, 0x10(v1)
    lqc2 vf6, 0x20(v1)
    lqc2 vf8, 0x0(a0)
    lqc2 vf9, 0x10(a0)
    lqc2 vf10, 0x20(a0)
    vaddx.x vf4, vf0, vf20x
    vaddx.y vf5, vf0, vf21x
    vaddx.z vf6, vf0, vf22x
    vaddx.w vf4, vf8, vf0x
    vaddx.w vf5, vf9, vf0x
    vaddx.w vf6, vf10, vf0x
    sqc2 vf4, 0x0(a0)
    sqc2 vf5, 0x10(a0)
    jr ra
    sqc2 vf6, 0x20(a0)
}
#endif
/* mtx = a translation matrix. */
#if PLATFORM_PC
void Mtx_Translation(f32 *mtx, f32 x, f32 y, f32 z)
{
    unsigned int v1 __attribute__((unused)) = 0;
    unsigned int a2 __attribute__((unused)) = 0;
    unsigned int a3 __attribute__((unused)) = 0;
    unsigned int t0 __attribute__((unused)) = 0;
    unsigned int a0 __attribute__((unused)) = (unsigned int)(__SIZE_TYPE__)mtx;
    f32 f12 __attribute__((unused)) = x;
    f32 f13 __attribute__((unused)) = y;
    f32 f14 __attribute__((unused)) = z;
    v1 = (unsigned int)(__SIZE_TYPE__)gMtxIdentity; /* la v1, gMtxIdentity */
    a2 = PC_F2U(f12); /* mfc1 a2, f12 */
    a3 = PC_F2U(f13); /* mfc1 a3, f13 */
    t0 = PC_F2U(f14); /* mfc1 t0, f14 */
    vu0_lq(7, v1 + 0x30); /* lqc2 vf7, 0x30(v1) */
    vu0_qmtc2(20, a2); /* qmtc2.ni a2, vf20 */
    vu0_qmtc2(21, a3); /* qmtc2.ni a3, vf21 */
    vu0_qmtc2(22, t0); /* qmtc2.ni t0, vf22 */
    vu0_lq(4, v1); /* lqc2 vf4, 0x0(v1) */
    vu0_lq(5, v1 + 0x10); /* lqc2 vf5, 0x10(v1) */
    vu0_lq(6, v1 + 0x20); /* lqc2 vf6, 0x20(v1) */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_X, 7, 0, 20); /* vaddx.x vf7, vf0, vf20x */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_Y, 7, 0, 21); /* vaddx.y vf7, vf0, vf21x */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_Z, 7, 0, 22); /* vaddx.z vf7, vf0, vf22x */
    vu0_sq(4, a0); /* sqc2 vf4, 0x0(a0) */
    vu0_sq(5, a0 + 0x10); /* sqc2 vf5, 0x10(a0) */
    vu0_sq(6, a0 + 0x20); /* sqc2 vf6, 0x20(a0) */
    vu0_sq(7, a0 + 0x30); /* sqc2 vf7, 0x30(a0) */
    return; /* jr ra */
}
#else
asm void Mtx_Translation(f32 *mtx, f32 x, f32 y, f32 z)
{
    .set noreorder
    la v1, gMtxIdentity
    mfc1 a2, f12
    mfc1 a3, f13
    mfc1 t0, f14
    lqc2 vf7, 0x30(v1)
    qmtc2.ni a2, vf20
    qmtc2.ni a3, vf21
    qmtc2.ni t0, vf22
    lqc2 vf4, 0x0(v1)
    lqc2 vf5, 0x10(v1)
    lqc2 vf6, 0x20(v1)
    vaddx.x vf7, vf0, vf20x
    vaddx.y vf7, vf0, vf21x
    vaddx.z vf7, vf0, vf22x
    sqc2 vf4, 0x0(a0)
    sqc2 vf5, 0x10(a0)
    sqc2 vf6, 0x20(a0)
    jr ra
    sqc2 vf7, 0x30(a0)
}
#endif

/* The 3x3 part of mtx = mtx * scale(x, y, z). */
void Mtx_ScaleBy(f32 *mtx, f32 x, f32 y, f32 z)
{
    f32 scale[16];

    Mtx_SetScale(scale, x, y, z);
    Mtx_Concat3x3(mtx, scale);
}

/* mtx's translation = (x, y, z). */
void Mtx_SetTrans(f32 *mtx, f32 x, f32 y, f32 z)
{
    mtx[12] = x;
    mtx[13] = y;
    mtx[14] = z;
}

/* mtx's translation += offset. */
void Mtx_AddTrans(f32 *mtx, f32 *offset)
{
    mtx[12] = (mtx[12] + offset[0]);
    mtx[13] = (mtx[13] + offset[1]);
    mtx[14] = (mtx[14] + offset[2]);
}

/* out = mtx's translation. */
void Mtx_GetTrans(f32 *out, f32 *mtx)
{
    out[0] = mtx[12];
    out[1] = mtx[13];
    out[2] = mtx[14];
}
/* mtx = mtx * (rotation by angle about x). */
#if PLATFORM_PC
void Mtx_RotateX(f32 *mtx, f32 angle)
{
    unsigned int a2 __attribute__((unused)) = 0;
    unsigned int a3 __attribute__((unused)) = 0;
    unsigned int t1 __attribute__((unused)) = 0;
    unsigned int ra __attribute__((unused)) = 0;
    f32 f0 __attribute__((unused)) = 0;
    f32 f1 __attribute__((unused)) = 0;
    unsigned char stk_[0x20] __attribute__((aligned(16))) = {0};
    unsigned int sp = (unsigned int)(__SIZE_TYPE__)(stk_ + 0x20);
    unsigned int a0 __attribute__((unused)) = (unsigned int)(__SIZE_TYPE__)mtx;
    f32 f12 __attribute__((unused)) = angle;
    sp = sp + (-0x20); /* addiu sp, sp, -0x20 */
    t1 = a0 + 0; /* daddu t1, a0, zero */
    PC_SD(sp, ra); /* sd ra, 0x0(sp) */
    a0 = sp + (0x10); /* addiu a0, sp, 0x10 */
    Math_SinCos((f32 *)(__SIZE_TYPE__)a0, f12); /* jal Math_SinCos */
    f1 = PC_LWC1(sp + 0x10); /* lwc1 f1, 0x10(sp) */
    f0 = PC_LWC1(sp + 0x14); /* lwc1 f0, 0x14(sp) */
    a2 = PC_F2U(f1); /* mfc1 a2, f1 */
    a3 = PC_F2U(f0); /* mfc1 a3, f0 */
    vu0_lq(4, t1); /* lqc2 vf4, 0x0(t1) */
    vu0_lq(5, t1 + 0x10); /* lqc2 vf5, 0x10(t1) */
    vu0_qmtc2(20, a2); /* qmtc2.ni a2, vf20 */
    vu0_qmtc2(21, a3); /* qmtc2.ni a3, vf21 */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, 0x6, 10, 0, 0); /* vadd.yz vf10, vf0, vf0 */
    vu0_fmac(VU0_ADD, VU0_BW, 0, VU0_X, 10, 0, 0); /* vaddw.x vf10, vf0, vf0w */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, VU0_X, 11, 0, 0); /* vadd.x vf11, vf0, vf0 */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_Y, 11, 0, 21); /* vaddx.y vf11, vf0, vf21x */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_Z, 11, 0, 20); /* vaddx.z vf11, vf0, vf20x */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, VU0_X, 12, 0, 0); /* vadd.x vf12, vf0, vf0 */
    vu0_fmac(VU0_SUB, VU0_BX, 0, VU0_Y, 12, 0, 20); /* vsubx.y vf12, vf0, vf20x */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_Z, 12, 0, 21); /* vaddx.z vf12, vf0, vf21x */
    vu0_lq(6, t1 + 0x20); /* lqc2 vf6, 0x20(t1) */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 4); /* vmulax.xyz ACC, vf10, vf4x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 4); /* vmadday.xyz ACC, vf11, vf4y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 4, 12, 4); /* vmaddz.xyz vf4, vf12, vf4z */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 5); /* vmulax.xyz ACC, vf10, vf5x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 5); /* vmadday.xyz ACC, vf11, vf5y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 5, 12, 5); /* vmaddz.xyz vf5, vf12, vf5z */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 6); /* vmulax.xyz ACC, vf10, vf6x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 6); /* vmadday.xyz ACC, vf11, vf6y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 6, 12, 6); /* vmaddz.xyz vf6, vf12, vf6z */
    vu0_sq(4, t1); /* sqc2 vf4, 0x0(t1) */
    vu0_sq(5, t1 + 0x10); /* sqc2 vf5, 0x10(t1) */
    vu0_sq(6, t1 + 0x20); /* sqc2 vf6, 0x20(t1) */
    ra = (unsigned int)PC_LD(sp); /* ld ra, 0x0(sp) */
    sp = sp + (0x20); /* addiu sp, sp, 0x20 */
    return; /* jr ra */
}
#else
asm void Mtx_RotateX(f32 *mtx, f32 angle)
{
    .set noreorder
    addiu sp, sp, -0x20
    daddu t1, a0, zero
    sd ra, 0x0(sp)
    jal Math_SinCos
    addiu a0, sp, 0x10
    lwc1 f1, 0x10(sp)
    lwc1 f0, 0x14(sp)
    mfc1 a2, f1
    mfc1 a3, f0
    lqc2 vf4, 0x0(t1)
    lqc2 vf5, 0x10(t1)
    qmtc2.ni a2, vf20
    qmtc2.ni a3, vf21
    vadd.yz vf10, vf0, vf0
    vaddw.x vf10, vf0, vf0w
    vadd.x vf11, vf0, vf0
    vaddx.y vf11, vf0, vf21x
    vaddx.z vf11, vf0, vf20x
    vadd.x vf12, vf0, vf0
    vsubx.y vf12, vf0, vf20x
    vaddx.z vf12, vf0, vf21x
    lqc2 vf6, 0x20(t1)
    vmulax.xyz ACC, vf10, vf4x
    vmadday.xyz ACC, vf11, vf4y
    vmaddz.xyz vf4, vf12, vf4z
    vmulax.xyz ACC, vf10, vf5x
    vmadday.xyz ACC, vf11, vf5y
    vmaddz.xyz vf5, vf12, vf5z
    vmulax.xyz ACC, vf10, vf6x
    vmadday.xyz ACC, vf11, vf6y
    vmaddz.xyz vf6, vf12, vf6z
    sqc2 vf4, 0x0(t1)
    sqc2 vf5, 0x10(t1)
    sqc2 vf6, 0x20(t1)
    ld ra, 0x0(sp)
    jr ra
    addiu sp, sp, 0x20
}
#endif
/* mtx = mtx * (rotation by angle about y). */
#if PLATFORM_PC
void Mtx_RotateY(f32 *mtx, f32 angle)
{
    unsigned int a2 __attribute__((unused)) = 0;
    unsigned int a3 __attribute__((unused)) = 0;
    unsigned int t1 __attribute__((unused)) = 0;
    unsigned int ra __attribute__((unused)) = 0;
    f32 f0 __attribute__((unused)) = 0;
    f32 f1 __attribute__((unused)) = 0;
    unsigned char stk_[0x20] __attribute__((aligned(16))) = {0};
    unsigned int sp = (unsigned int)(__SIZE_TYPE__)(stk_ + 0x20);
    unsigned int a0 __attribute__((unused)) = (unsigned int)(__SIZE_TYPE__)mtx;
    f32 f12 __attribute__((unused)) = angle;
    sp = sp + (-0x20); /* addiu sp, sp, -0x20 */
    t1 = a0 + 0; /* daddu t1, a0, zero */
    PC_SD(sp, ra); /* sd ra, 0x0(sp) */
    a0 = sp + (0x10); /* addiu a0, sp, 0x10 */
    Math_SinCos((f32 *)(__SIZE_TYPE__)a0, f12); /* jal Math_SinCos */
    f1 = PC_LWC1(sp + 0x10); /* lwc1 f1, 0x10(sp) */
    f0 = PC_LWC1(sp + 0x14); /* lwc1 f0, 0x14(sp) */
    a2 = PC_F2U(f1); /* mfc1 a2, f1 */
    a3 = PC_F2U(f0); /* mfc1 a3, f0 */
    vu0_lq(4, t1); /* lqc2 vf4, 0x0(t1) */
    vu0_lq(5, t1 + 0x10); /* lqc2 vf5, 0x10(t1) */
    vu0_qmtc2(20, a2); /* qmtc2.ni a2, vf20 */
    vu0_qmtc2(21, a3); /* qmtc2.ni a3, vf21 */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, 0xA, 11, 0, 0); /* vadd.xz vf11, vf0, vf0 */
    vu0_fmac(VU0_ADD, VU0_BW, 0, VU0_Y, 11, 0, 0); /* vaddw.y vf11, vf0, vf0w */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, VU0_Y, 10, 0, 0); /* vadd.y vf10, vf0, vf0 */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_X, 10, 0, 21); /* vaddx.x vf10, vf0, vf21x */
    vu0_fmac(VU0_SUB, VU0_BX, 0, VU0_Z, 10, 0, 20); /* vsubx.z vf10, vf0, vf20x */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, VU0_Y, 12, 0, 0); /* vadd.y vf12, vf0, vf0 */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_X, 12, 0, 20); /* vaddx.x vf12, vf0, vf20x */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_Z, 12, 0, 21); /* vaddx.z vf12, vf0, vf21x */
    vu0_lq(6, t1 + 0x20); /* lqc2 vf6, 0x20(t1) */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 4); /* vmulax.xyz ACC, vf10, vf4x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 4); /* vmadday.xyz ACC, vf11, vf4y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 4, 12, 4); /* vmaddz.xyz vf4, vf12, vf4z */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 5); /* vmulax.xyz ACC, vf10, vf5x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 5); /* vmadday.xyz ACC, vf11, vf5y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 5, 12, 5); /* vmaddz.xyz vf5, vf12, vf5z */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 6); /* vmulax.xyz ACC, vf10, vf6x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 6); /* vmadday.xyz ACC, vf11, vf6y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 6, 12, 6); /* vmaddz.xyz vf6, vf12, vf6z */
    vu0_sq(4, t1); /* sqc2 vf4, 0x0(t1) */
    vu0_sq(5, t1 + 0x10); /* sqc2 vf5, 0x10(t1) */
    vu0_sq(6, t1 + 0x20); /* sqc2 vf6, 0x20(t1) */
    ra = (unsigned int)PC_LD(sp); /* ld ra, 0x0(sp) */
    sp = sp + (0x20); /* addiu sp, sp, 0x20 */
    return; /* jr ra */
}
#else
asm void Mtx_RotateY(f32 *mtx, f32 angle)
{
    .set noreorder
    addiu sp, sp, -0x20
    daddu t1, a0, zero
    sd ra, 0x0(sp)
    jal Math_SinCos
    addiu a0, sp, 0x10
    lwc1 f1, 0x10(sp)
    lwc1 f0, 0x14(sp)
    mfc1 a2, f1
    mfc1 a3, f0
    lqc2 vf4, 0x0(t1)
    lqc2 vf5, 0x10(t1)
    qmtc2.ni a2, vf20
    qmtc2.ni a3, vf21
    vadd.xz vf11, vf0, vf0
    vaddw.y vf11, vf0, vf0w
    vadd.y vf10, vf0, vf0
    vaddx.x vf10, vf0, vf21x
    vsubx.z vf10, vf0, vf20x
    vadd.y vf12, vf0, vf0
    vaddx.x vf12, vf0, vf20x
    vaddx.z vf12, vf0, vf21x
    lqc2 vf6, 0x20(t1)
    vmulax.xyz ACC, vf10, vf4x
    vmadday.xyz ACC, vf11, vf4y
    vmaddz.xyz vf4, vf12, vf4z
    vmulax.xyz ACC, vf10, vf5x
    vmadday.xyz ACC, vf11, vf5y
    vmaddz.xyz vf5, vf12, vf5z
    vmulax.xyz ACC, vf10, vf6x
    vmadday.xyz ACC, vf11, vf6y
    vmaddz.xyz vf6, vf12, vf6z
    sqc2 vf4, 0x0(t1)
    sqc2 vf5, 0x10(t1)
    sqc2 vf6, 0x20(t1)
    ld ra, 0x0(sp)
    jr ra
    addiu sp, sp, 0x20
}
#endif
/* mtx = mtx * (rotation by angle about z). */
#if PLATFORM_PC
void Mtx_RotateZ(f32 *mtx, f32 angle)
{
    unsigned int a2 __attribute__((unused)) = 0;
    unsigned int a3 __attribute__((unused)) = 0;
    unsigned int t1 __attribute__((unused)) = 0;
    unsigned int ra __attribute__((unused)) = 0;
    f32 f0 __attribute__((unused)) = 0;
    f32 f1 __attribute__((unused)) = 0;
    unsigned char stk_[0x20] __attribute__((aligned(16))) = {0};
    unsigned int sp = (unsigned int)(__SIZE_TYPE__)(stk_ + 0x20);
    unsigned int a0 __attribute__((unused)) = (unsigned int)(__SIZE_TYPE__)mtx;
    f32 f12 __attribute__((unused)) = angle;
    sp = sp + (-0x20); /* addiu sp, sp, -0x20 */
    t1 = a0 + 0; /* daddu t1, a0, zero */
    PC_SD(sp, ra); /* sd ra, 0x0(sp) */
    a0 = sp + (0x10); /* addiu a0, sp, 0x10 */
    Math_SinCos((f32 *)(__SIZE_TYPE__)a0, f12); /* jal Math_SinCos */
    f1 = PC_LWC1(sp + 0x10); /* lwc1 f1, 0x10(sp) */
    f0 = PC_LWC1(sp + 0x14); /* lwc1 f0, 0x14(sp) */
    a2 = PC_F2U(f1); /* mfc1 a2, f1 */
    a3 = PC_F2U(f0); /* mfc1 a3, f0 */
    vu0_lq(4, t1); /* lqc2 vf4, 0x0(t1) */
    vu0_lq(5, t1 + 0x10); /* lqc2 vf5, 0x10(t1) */
    vu0_qmtc2(20, a2); /* qmtc2.ni a2, vf20 */
    vu0_qmtc2(21, a3); /* qmtc2.ni a3, vf21 */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, VU0_Z, 10, 0, 0); /* vadd.z vf10, vf0, vf0 */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_Y, 10, 0, 20); /* vaddx.y vf10, vf0, vf20x */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_X, 10, 0, 21); /* vaddx.x vf10, vf0, vf21x */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, VU0_Z, 11, 0, 0); /* vadd.z vf11, vf0, vf0 */
    vu0_fmac(VU0_SUB, VU0_BX, 0, VU0_X, 11, 0, 20); /* vsubx.x vf11, vf0, vf20x */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_Y, 11, 0, 21); /* vaddx.y vf11, vf0, vf21x */
    vu0_mr32(VU0_XYZ, 12, 0); /* vmr32.xyz vf12, vf0 */
    vu0_lq(6, t1 + 0x20); /* lqc2 vf6, 0x20(t1) */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 4); /* vmulax.xyz ACC, vf10, vf4x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 4); /* vmadday.xyz ACC, vf11, vf4y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 4, 12, 4); /* vmaddz.xyz vf4, vf12, vf4z */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 5); /* vmulax.xyz ACC, vf10, vf5x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 5); /* vmadday.xyz ACC, vf11, vf5y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 5, 12, 5); /* vmaddz.xyz vf5, vf12, vf5z */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 6); /* vmulax.xyz ACC, vf10, vf6x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 6); /* vmadday.xyz ACC, vf11, vf6y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 6, 12, 6); /* vmaddz.xyz vf6, vf12, vf6z */
    vu0_sq(4, t1); /* sqc2 vf4, 0x0(t1) */
    vu0_sq(5, t1 + 0x10); /* sqc2 vf5, 0x10(t1) */
    vu0_sq(6, t1 + 0x20); /* sqc2 vf6, 0x20(t1) */
    ra = (unsigned int)PC_LD(sp); /* ld ra, 0x0(sp) */
    sp = sp + (0x20); /* addiu sp, sp, 0x20 */
    return; /* jr ra */
}
#else
asm void Mtx_RotateZ(f32 *mtx, f32 angle)
{
    .set noreorder
    addiu sp, sp, -0x20
    daddu t1, a0, zero
    sd ra, 0x0(sp)
    jal Math_SinCos
    addiu a0, sp, 0x10
    lwc1 f1, 0x10(sp)
    lwc1 f0, 0x14(sp)
    mfc1 a2, f1
    mfc1 a3, f0
    lqc2 vf4, 0x0(t1)
    lqc2 vf5, 0x10(t1)
    qmtc2.ni a2, vf20
    qmtc2.ni a3, vf21
    vadd.z vf10, vf0, vf0
    vaddx.y vf10, vf0, vf20x
    vaddx.x vf10, vf0, vf21x
    vadd.z vf11, vf0, vf0
    vsubx.x vf11, vf0, vf20x
    vaddx.y vf11, vf0, vf21x
    vmr32.xyz vf12, vf0
    lqc2 vf6, 0x20(t1)
    vmulax.xyz ACC, vf10, vf4x
    vmadday.xyz ACC, vf11, vf4y
    vmaddz.xyz vf4, vf12, vf4z
    vmulax.xyz ACC, vf10, vf5x
    vmadday.xyz ACC, vf11, vf5y
    vmaddz.xyz vf5, vf12, vf5z
    vmulax.xyz ACC, vf10, vf6x
    vmadday.xyz ACC, vf11, vf6y
    vmaddz.xyz vf6, vf12, vf6z
    sqc2 vf4, 0x0(t1)
    sqc2 vf5, 0x10(t1)
    sqc2 vf6, 0x20(t1)
    ld ra, 0x0(sp)
    jr ra
    addiu sp, sp, 0x20
}
#endif
/* The rotation part of mtx = rotations about x, then y, then z. */
#if PLATFORM_PC
void Mtx_SetRotXYZ(f32 *mtx, f32 x, f32 y, f32 z)
{
    unsigned int a2 __attribute__((unused)) = 0;
    unsigned int a3 __attribute__((unused)) = 0;
    unsigned int s0 __attribute__((unused)) = 0;
    unsigned int ra __attribute__((unused)) = 0;
    f32 f0 __attribute__((unused)) = 0;
    f32 f1 __attribute__((unused)) = 0;
    f32 f20 __attribute__((unused)) = 0;
    f32 f21 __attribute__((unused)) = 0;
    f32 f22 __attribute__((unused)) = 0;
    unsigned char stk_[0x60] __attribute__((aligned(16))) = {0};
    unsigned int sp = (unsigned int)(__SIZE_TYPE__)(stk_ + 0x60);
    unsigned int a0 __attribute__((unused)) = (unsigned int)(__SIZE_TYPE__)mtx;
    f32 f12 __attribute__((unused)) = x;
    f32 f13 __attribute__((unused)) = y;
    f32 f14 __attribute__((unused)) = z;
    sp = sp + (-0x60); /* addiu sp, sp, -0x60 */
    PC_SD(sp + 0x20, ra); /* sd ra, 0x20(sp) */
    PC_SQ32(sp + 0x10, s0); /* sq s0, 0x10(sp) */
    PC_SWC1(sp + 0x8, f22); /* swc1 f22, 0x8(sp) */
    s0 = a0 + 0; /* daddu s0, a0, zero */
    PC_SWC1(sp + 0x4, f21); /* swc1 f21, 0x4(sp) */
    PC_SWC1(sp, f20); /* swc1 f20, 0x0(sp) */
    f22 = f12; /* mov.s f22, f12 */
    f21 = f13; /* mov.s f21, f13 */
    f20 = f14; /* mov.s f20, f14 */
    Mtx_ClearRotation((f32 *)(__SIZE_TYPE__)a0); /* jal Mtx_ClearRotation */
    f12 = f22; /* mov.s f12, f22 */
    a0 = sp + (0x50); /* addiu a0, sp, 0x50 */
    Math_SinCos((f32 *)(__SIZE_TYPE__)a0, f12); /* jal Math_SinCos */
    f1 = PC_LWC1(sp + 0x50); /* lwc1 f1, 0x50(sp) */
    f0 = PC_LWC1(sp + 0x54); /* lwc1 f0, 0x54(sp) */
    a2 = PC_F2U(f1); /* mfc1 a2, f1 */
    a3 = PC_F2U(f0); /* mfc1 a3, f0 */
    vu0_lq(4, s0); /* lqc2 vf4, 0x0(s0) */
    vu0_lq(5, s0 + 0x10); /* lqc2 vf5, 0x10(s0) */
    vu0_qmtc2(20, a2); /* qmtc2.ni a2, vf20 */
    vu0_qmtc2(21, a3); /* qmtc2.ni a3, vf21 */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, 0x6, 10, 0, 0); /* vadd.yz vf10, vf0, vf0 */
    vu0_fmac(VU0_ADD, VU0_BW, 0, VU0_X, 10, 0, 0); /* vaddw.x vf10, vf0, vf0w */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, VU0_X, 11, 0, 0); /* vadd.x vf11, vf0, vf0 */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_Y, 11, 0, 21); /* vaddx.y vf11, vf0, vf21x */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_Z, 11, 0, 20); /* vaddx.z vf11, vf0, vf20x */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, VU0_X, 12, 0, 0); /* vadd.x vf12, vf0, vf0 */
    vu0_fmac(VU0_SUB, VU0_BX, 0, VU0_Y, 12, 0, 20); /* vsubx.y vf12, vf0, vf20x */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_Z, 12, 0, 21); /* vaddx.z vf12, vf0, vf21x */
    vu0_lq(6, s0 + 0x20); /* lqc2 vf6, 0x20(s0) */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 4); /* vmulax.xyz ACC, vf10, vf4x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 4); /* vmadday.xyz ACC, vf11, vf4y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 4, 12, 4); /* vmaddz.xyz vf4, vf12, vf4z */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 5); /* vmulax.xyz ACC, vf10, vf5x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 5); /* vmadday.xyz ACC, vf11, vf5y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 5, 12, 5); /* vmaddz.xyz vf5, vf12, vf5z */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 6); /* vmulax.xyz ACC, vf10, vf6x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 6); /* vmadday.xyz ACC, vf11, vf6y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 6, 12, 6); /* vmaddz.xyz vf6, vf12, vf6z */
    vu0_sq(4, s0); /* sqc2 vf4, 0x0(s0) */
    vu0_sq(5, s0 + 0x10); /* sqc2 vf5, 0x10(s0) */
    vu0_sq(6, s0 + 0x20); /* sqc2 vf6, 0x20(s0) */
    f12 = f21; /* mov.s f12, f21 */
    a0 = sp + (0x40); /* addiu a0, sp, 0x40 */
    Math_SinCos((f32 *)(__SIZE_TYPE__)a0, f12); /* jal Math_SinCos */
    f1 = PC_LWC1(sp + 0x40); /* lwc1 f1, 0x40(sp) */
    f0 = PC_LWC1(sp + 0x44); /* lwc1 f0, 0x44(sp) */
    a2 = PC_F2U(f1); /* mfc1 a2, f1 */
    a3 = PC_F2U(f0); /* mfc1 a3, f0 */
    vu0_lq(4, s0); /* lqc2 vf4, 0x0(s0) */
    vu0_lq(5, s0 + 0x10); /* lqc2 vf5, 0x10(s0) */
    vu0_qmtc2(20, a2); /* qmtc2.ni a2, vf20 */
    vu0_qmtc2(21, a3); /* qmtc2.ni a3, vf21 */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, 0xA, 11, 0, 0); /* vadd.xz vf11, vf0, vf0 */
    vu0_fmac(VU0_ADD, VU0_BW, 0, VU0_Y, 11, 0, 0); /* vaddw.y vf11, vf0, vf0w */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, VU0_Y, 10, 0, 0); /* vadd.y vf10, vf0, vf0 */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_X, 10, 0, 21); /* vaddx.x vf10, vf0, vf21x */
    vu0_fmac(VU0_SUB, VU0_BX, 0, VU0_Z, 10, 0, 20); /* vsubx.z vf10, vf0, vf20x */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, VU0_Y, 12, 0, 0); /* vadd.y vf12, vf0, vf0 */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_X, 12, 0, 20); /* vaddx.x vf12, vf0, vf20x */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_Z, 12, 0, 21); /* vaddx.z vf12, vf0, vf21x */
    vu0_lq(6, s0 + 0x20); /* lqc2 vf6, 0x20(s0) */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 4); /* vmulax.xyz ACC, vf10, vf4x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 4); /* vmadday.xyz ACC, vf11, vf4y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 4, 12, 4); /* vmaddz.xyz vf4, vf12, vf4z */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 5); /* vmulax.xyz ACC, vf10, vf5x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 5); /* vmadday.xyz ACC, vf11, vf5y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 5, 12, 5); /* vmaddz.xyz vf5, vf12, vf5z */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 6); /* vmulax.xyz ACC, vf10, vf6x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 6); /* vmadday.xyz ACC, vf11, vf6y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 6, 12, 6); /* vmaddz.xyz vf6, vf12, vf6z */
    vu0_sq(4, s0); /* sqc2 vf4, 0x0(s0) */
    vu0_sq(5, s0 + 0x10); /* sqc2 vf5, 0x10(s0) */
    vu0_sq(6, s0 + 0x20); /* sqc2 vf6, 0x20(s0) */
    f12 = f20; /* mov.s f12, f20 */
    a0 = sp + (0x30); /* addiu a0, sp, 0x30 */
    Math_SinCos((f32 *)(__SIZE_TYPE__)a0, f12); /* jal Math_SinCos */
    f1 = PC_LWC1(sp + 0x30); /* lwc1 f1, 0x30(sp) */
    f0 = PC_LWC1(sp + 0x34); /* lwc1 f0, 0x34(sp) */
    a2 = PC_F2U(f1); /* mfc1 a2, f1 */
    a3 = PC_F2U(f0); /* mfc1 a3, f0 */
    vu0_lq(4, s0); /* lqc2 vf4, 0x0(s0) */
    vu0_lq(5, s0 + 0x10); /* lqc2 vf5, 0x10(s0) */
    vu0_qmtc2(20, a2); /* qmtc2.ni a2, vf20 */
    vu0_qmtc2(21, a3); /* qmtc2.ni a3, vf21 */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, VU0_Z, 10, 0, 0); /* vadd.z vf10, vf0, vf0 */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_Y, 10, 0, 20); /* vaddx.y vf10, vf0, vf20x */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_X, 10, 0, 21); /* vaddx.x vf10, vf0, vf21x */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, VU0_Z, 11, 0, 0); /* vadd.z vf11, vf0, vf0 */
    vu0_fmac(VU0_SUB, VU0_BX, 0, VU0_X, 11, 0, 20); /* vsubx.x vf11, vf0, vf20x */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_Y, 11, 0, 21); /* vaddx.y vf11, vf0, vf21x */
    vu0_mr32(VU0_XYZ, 12, 0); /* vmr32.xyz vf12, vf0 */
    vu0_lq(6, s0 + 0x20); /* lqc2 vf6, 0x20(s0) */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 4); /* vmulax.xyz ACC, vf10, vf4x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 4); /* vmadday.xyz ACC, vf11, vf4y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 4, 12, 4); /* vmaddz.xyz vf4, vf12, vf4z */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 5); /* vmulax.xyz ACC, vf10, vf5x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 5); /* vmadday.xyz ACC, vf11, vf5y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 5, 12, 5); /* vmaddz.xyz vf5, vf12, vf5z */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 6); /* vmulax.xyz ACC, vf10, vf6x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 6); /* vmadday.xyz ACC, vf11, vf6y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 6, 12, 6); /* vmaddz.xyz vf6, vf12, vf6z */
    vu0_sq(4, s0); /* sqc2 vf4, 0x0(s0) */
    vu0_sq(5, s0 + 0x10); /* sqc2 vf5, 0x10(s0) */
    vu0_sq(6, s0 + 0x20); /* sqc2 vf6, 0x20(s0) */
    ra = (unsigned int)PC_LD(sp + 0x20); /* ld ra, 0x20(sp) */
    f22 = PC_LWC1(sp + 0x8); /* lwc1 f22, 0x8(sp) */
    s0 = PC_LW(sp + 0x10); /* lq s0, 0x10(sp) */
    f21 = PC_LWC1(sp + 0x4); /* lwc1 f21, 0x4(sp) */
    f20 = PC_LWC1(sp); /* lwc1 f20, 0x0(sp) */
    sp = sp + (0x60); /* addiu sp, sp, 0x60 */
    return; /* jr ra */
}
#else
asm void Mtx_SetRotXYZ(f32 *mtx, f32 x, f32 y, f32 z)
{
    .set noreorder
    addiu sp, sp, -0x60
    sd ra, 0x20(sp)
    sq s0, 0x10(sp)
    swc1 f22, 0x8(sp)
    daddu s0, a0, zero
    swc1 f21, 0x4(sp)
    swc1 f20, 0x0(sp)
    mov.s f22, f12
    mov.s f21, f13
    jal Mtx_ClearRotation
    mov.s f20, f14
    mov.s f12, f22
    jal Math_SinCos
    addiu a0, sp, 0x50
    lwc1 f1, 0x50(sp)
    lwc1 f0, 0x54(sp)
    mfc1 a2, f1
    mfc1 a3, f0
    lqc2 vf4, 0x0(s0)
    lqc2 vf5, 0x10(s0)
    qmtc2.ni a2, vf20
    qmtc2.ni a3, vf21
    vadd.yz vf10, vf0, vf0
    vaddw.x vf10, vf0, vf0w
    vadd.x vf11, vf0, vf0
    vaddx.y vf11, vf0, vf21x
    vaddx.z vf11, vf0, vf20x
    vadd.x vf12, vf0, vf0
    vsubx.y vf12, vf0, vf20x
    vaddx.z vf12, vf0, vf21x
    lqc2 vf6, 0x20(s0)
    vmulax.xyz ACC, vf10, vf4x
    vmadday.xyz ACC, vf11, vf4y
    vmaddz.xyz vf4, vf12, vf4z
    vmulax.xyz ACC, vf10, vf5x
    vmadday.xyz ACC, vf11, vf5y
    vmaddz.xyz vf5, vf12, vf5z
    vmulax.xyz ACC, vf10, vf6x
    vmadday.xyz ACC, vf11, vf6y
    vmaddz.xyz vf6, vf12, vf6z
    sqc2 vf4, 0x0(s0)
    sqc2 vf5, 0x10(s0)
    sqc2 vf6, 0x20(s0)
    mov.s f12, f21
    jal Math_SinCos
    addiu a0, sp, 0x40
    lwc1 f1, 0x40(sp)
    lwc1 f0, 0x44(sp)
    mfc1 a2, f1
    mfc1 a3, f0
    lqc2 vf4, 0x0(s0)
    lqc2 vf5, 0x10(s0)
    qmtc2.ni a2, vf20
    qmtc2.ni a3, vf21
    vadd.xz vf11, vf0, vf0
    vaddw.y vf11, vf0, vf0w
    vadd.y vf10, vf0, vf0
    vaddx.x vf10, vf0, vf21x
    vsubx.z vf10, vf0, vf20x
    vadd.y vf12, vf0, vf0
    vaddx.x vf12, vf0, vf20x
    vaddx.z vf12, vf0, vf21x
    lqc2 vf6, 0x20(s0)
    vmulax.xyz ACC, vf10, vf4x
    vmadday.xyz ACC, vf11, vf4y
    vmaddz.xyz vf4, vf12, vf4z
    vmulax.xyz ACC, vf10, vf5x
    vmadday.xyz ACC, vf11, vf5y
    vmaddz.xyz vf5, vf12, vf5z
    vmulax.xyz ACC, vf10, vf6x
    vmadday.xyz ACC, vf11, vf6y
    vmaddz.xyz vf6, vf12, vf6z
    sqc2 vf4, 0x0(s0)
    sqc2 vf5, 0x10(s0)
    sqc2 vf6, 0x20(s0)
    mov.s f12, f20
    jal Math_SinCos
    addiu a0, sp, 0x30
    lwc1 f1, 0x30(sp)
    lwc1 f0, 0x34(sp)
    mfc1 a2, f1
    mfc1 a3, f0
    lqc2 vf4, 0x0(s0)
    lqc2 vf5, 0x10(s0)
    qmtc2.ni a2, vf20
    qmtc2.ni a3, vf21
    vadd.z vf10, vf0, vf0
    vaddx.y vf10, vf0, vf20x
    vaddx.x vf10, vf0, vf21x
    vadd.z vf11, vf0, vf0
    vsubx.x vf11, vf0, vf20x
    vaddx.y vf11, vf0, vf21x
    vmr32.xyz vf12, vf0
    lqc2 vf6, 0x20(s0)
    vmulax.xyz ACC, vf10, vf4x
    vmadday.xyz ACC, vf11, vf4y
    vmaddz.xyz vf4, vf12, vf4z
    vmulax.xyz ACC, vf10, vf5x
    vmadday.xyz ACC, vf11, vf5y
    vmaddz.xyz vf5, vf12, vf5z
    vmulax.xyz ACC, vf10, vf6x
    vmadday.xyz ACC, vf11, vf6y
    vmaddz.xyz vf6, vf12, vf6z
    sqc2 vf4, 0x0(s0)
    sqc2 vf5, 0x10(s0)
    sqc2 vf6, 0x20(s0)
    ld ra, 0x20(sp)
    lwc1 f22, 0x8(sp)
    lq s0, 0x10(sp)
    lwc1 f21, 0x4(sp)
    lwc1 f20, 0x0(sp)
    jr ra
    addiu sp, sp, 0x60
}
#endif
/* The rotation part of mtx = rotations about z, then y, then x. */
#if PLATFORM_PC
void Mtx_SetRotZYX(f32 *mtx, f32 x, f32 y, f32 z)
{
    unsigned int a2 __attribute__((unused)) = 0;
    unsigned int a3 __attribute__((unused)) = 0;
    unsigned int s0 __attribute__((unused)) = 0;
    unsigned int ra __attribute__((unused)) = 0;
    f32 f0 __attribute__((unused)) = 0;
    f32 f1 __attribute__((unused)) = 0;
    f32 f20 __attribute__((unused)) = 0;
    f32 f21 __attribute__((unused)) = 0;
    f32 f22 __attribute__((unused)) = 0;
    unsigned char stk_[0x60] __attribute__((aligned(16))) = {0};
    unsigned int sp = (unsigned int)(__SIZE_TYPE__)(stk_ + 0x60);
    unsigned int a0 __attribute__((unused)) = (unsigned int)(__SIZE_TYPE__)mtx;
    f32 f12 __attribute__((unused)) = x;
    f32 f13 __attribute__((unused)) = y;
    f32 f14 __attribute__((unused)) = z;
    sp = sp + (-0x60); /* addiu sp, sp, -0x60 */
    PC_SD(sp + 0x20, ra); /* sd ra, 0x20(sp) */
    PC_SQ32(sp + 0x10, s0); /* sq s0, 0x10(sp) */
    PC_SWC1(sp + 0x8, f22); /* swc1 f22, 0x8(sp) */
    s0 = a0 + 0; /* daddu s0, a0, zero */
    PC_SWC1(sp + 0x4, f21); /* swc1 f21, 0x4(sp) */
    PC_SWC1(sp, f20); /* swc1 f20, 0x0(sp) */
    f22 = f12; /* mov.s f22, f12 */
    f21 = f13; /* mov.s f21, f13 */
    f20 = f14; /* mov.s f20, f14 */
    Mtx_ClearRotation((f32 *)(__SIZE_TYPE__)a0); /* jal Mtx_ClearRotation */
    f12 = f20; /* mov.s f12, f20 */
    a0 = sp + (0x50); /* addiu a0, sp, 0x50 */
    Math_SinCos((f32 *)(__SIZE_TYPE__)a0, f12); /* jal Math_SinCos */
    f1 = PC_LWC1(sp + 0x50); /* lwc1 f1, 0x50(sp) */
    f0 = PC_LWC1(sp + 0x54); /* lwc1 f0, 0x54(sp) */
    a2 = PC_F2U(f1); /* mfc1 a2, f1 */
    a3 = PC_F2U(f0); /* mfc1 a3, f0 */
    vu0_lq(4, s0); /* lqc2 vf4, 0x0(s0) */
    vu0_lq(5, s0 + 0x10); /* lqc2 vf5, 0x10(s0) */
    vu0_qmtc2(20, a2); /* qmtc2.ni a2, vf20 */
    vu0_qmtc2(21, a3); /* qmtc2.ni a3, vf21 */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, VU0_Z, 10, 0, 0); /* vadd.z vf10, vf0, vf0 */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_Y, 10, 0, 20); /* vaddx.y vf10, vf0, vf20x */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_X, 10, 0, 21); /* vaddx.x vf10, vf0, vf21x */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, VU0_Z, 11, 0, 0); /* vadd.z vf11, vf0, vf0 */
    vu0_fmac(VU0_SUB, VU0_BX, 0, VU0_X, 11, 0, 20); /* vsubx.x vf11, vf0, vf20x */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_Y, 11, 0, 21); /* vaddx.y vf11, vf0, vf21x */
    vu0_mr32(VU0_XYZ, 12, 0); /* vmr32.xyz vf12, vf0 */
    vu0_lq(6, s0 + 0x20); /* lqc2 vf6, 0x20(s0) */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 4); /* vmulax.xyz ACC, vf10, vf4x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 4); /* vmadday.xyz ACC, vf11, vf4y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 4, 12, 4); /* vmaddz.xyz vf4, vf12, vf4z */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 5); /* vmulax.xyz ACC, vf10, vf5x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 5); /* vmadday.xyz ACC, vf11, vf5y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 5, 12, 5); /* vmaddz.xyz vf5, vf12, vf5z */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 6); /* vmulax.xyz ACC, vf10, vf6x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 6); /* vmadday.xyz ACC, vf11, vf6y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 6, 12, 6); /* vmaddz.xyz vf6, vf12, vf6z */
    vu0_sq(4, s0); /* sqc2 vf4, 0x0(s0) */
    vu0_sq(5, s0 + 0x10); /* sqc2 vf5, 0x10(s0) */
    vu0_sq(6, s0 + 0x20); /* sqc2 vf6, 0x20(s0) */
    f12 = f21; /* mov.s f12, f21 */
    a0 = sp + (0x40); /* addiu a0, sp, 0x40 */
    Math_SinCos((f32 *)(__SIZE_TYPE__)a0, f12); /* jal Math_SinCos */
    f1 = PC_LWC1(sp + 0x40); /* lwc1 f1, 0x40(sp) */
    f0 = PC_LWC1(sp + 0x44); /* lwc1 f0, 0x44(sp) */
    a2 = PC_F2U(f1); /* mfc1 a2, f1 */
    a3 = PC_F2U(f0); /* mfc1 a3, f0 */
    vu0_lq(4, s0); /* lqc2 vf4, 0x0(s0) */
    vu0_lq(5, s0 + 0x10); /* lqc2 vf5, 0x10(s0) */
    vu0_qmtc2(20, a2); /* qmtc2.ni a2, vf20 */
    vu0_qmtc2(21, a3); /* qmtc2.ni a3, vf21 */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, 0xA, 11, 0, 0); /* vadd.xz vf11, vf0, vf0 */
    vu0_fmac(VU0_ADD, VU0_BW, 0, VU0_Y, 11, 0, 0); /* vaddw.y vf11, vf0, vf0w */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, VU0_Y, 10, 0, 0); /* vadd.y vf10, vf0, vf0 */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_X, 10, 0, 21); /* vaddx.x vf10, vf0, vf21x */
    vu0_fmac(VU0_SUB, VU0_BX, 0, VU0_Z, 10, 0, 20); /* vsubx.z vf10, vf0, vf20x */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, VU0_Y, 12, 0, 0); /* vadd.y vf12, vf0, vf0 */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_X, 12, 0, 20); /* vaddx.x vf12, vf0, vf20x */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_Z, 12, 0, 21); /* vaddx.z vf12, vf0, vf21x */
    vu0_lq(6, s0 + 0x20); /* lqc2 vf6, 0x20(s0) */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 4); /* vmulax.xyz ACC, vf10, vf4x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 4); /* vmadday.xyz ACC, vf11, vf4y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 4, 12, 4); /* vmaddz.xyz vf4, vf12, vf4z */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 5); /* vmulax.xyz ACC, vf10, vf5x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 5); /* vmadday.xyz ACC, vf11, vf5y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 5, 12, 5); /* vmaddz.xyz vf5, vf12, vf5z */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 6); /* vmulax.xyz ACC, vf10, vf6x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 6); /* vmadday.xyz ACC, vf11, vf6y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 6, 12, 6); /* vmaddz.xyz vf6, vf12, vf6z */
    vu0_sq(4, s0); /* sqc2 vf4, 0x0(s0) */
    vu0_sq(5, s0 + 0x10); /* sqc2 vf5, 0x10(s0) */
    vu0_sq(6, s0 + 0x20); /* sqc2 vf6, 0x20(s0) */
    f12 = f22; /* mov.s f12, f22 */
    a0 = sp + (0x30); /* addiu a0, sp, 0x30 */
    Math_SinCos((f32 *)(__SIZE_TYPE__)a0, f12); /* jal Math_SinCos */
    f1 = PC_LWC1(sp + 0x30); /* lwc1 f1, 0x30(sp) */
    f0 = PC_LWC1(sp + 0x34); /* lwc1 f0, 0x34(sp) */
    a2 = PC_F2U(f1); /* mfc1 a2, f1 */
    a3 = PC_F2U(f0); /* mfc1 a3, f0 */
    vu0_lq(4, s0); /* lqc2 vf4, 0x0(s0) */
    vu0_lq(5, s0 + 0x10); /* lqc2 vf5, 0x10(s0) */
    vu0_qmtc2(20, a2); /* qmtc2.ni a2, vf20 */
    vu0_qmtc2(21, a3); /* qmtc2.ni a3, vf21 */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, 0x6, 10, 0, 0); /* vadd.yz vf10, vf0, vf0 */
    vu0_fmac(VU0_ADD, VU0_BW, 0, VU0_X, 10, 0, 0); /* vaddw.x vf10, vf0, vf0w */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, VU0_X, 11, 0, 0); /* vadd.x vf11, vf0, vf0 */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_Y, 11, 0, 21); /* vaddx.y vf11, vf0, vf21x */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_Z, 11, 0, 20); /* vaddx.z vf11, vf0, vf20x */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, VU0_X, 12, 0, 0); /* vadd.x vf12, vf0, vf0 */
    vu0_fmac(VU0_SUB, VU0_BX, 0, VU0_Y, 12, 0, 20); /* vsubx.y vf12, vf0, vf20x */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_Z, 12, 0, 21); /* vaddx.z vf12, vf0, vf21x */
    vu0_lq(6, s0 + 0x20); /* lqc2 vf6, 0x20(s0) */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 4); /* vmulax.xyz ACC, vf10, vf4x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 4); /* vmadday.xyz ACC, vf11, vf4y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 4, 12, 4); /* vmaddz.xyz vf4, vf12, vf4z */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 5); /* vmulax.xyz ACC, vf10, vf5x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 5); /* vmadday.xyz ACC, vf11, vf5y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 5, 12, 5); /* vmaddz.xyz vf5, vf12, vf5z */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 6); /* vmulax.xyz ACC, vf10, vf6x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 6); /* vmadday.xyz ACC, vf11, vf6y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 6, 12, 6); /* vmaddz.xyz vf6, vf12, vf6z */
    vu0_sq(4, s0); /* sqc2 vf4, 0x0(s0) */
    vu0_sq(5, s0 + 0x10); /* sqc2 vf5, 0x10(s0) */
    vu0_sq(6, s0 + 0x20); /* sqc2 vf6, 0x20(s0) */
    ra = (unsigned int)PC_LD(sp + 0x20); /* ld ra, 0x20(sp) */
    f22 = PC_LWC1(sp + 0x8); /* lwc1 f22, 0x8(sp) */
    s0 = PC_LW(sp + 0x10); /* lq s0, 0x10(sp) */
    f21 = PC_LWC1(sp + 0x4); /* lwc1 f21, 0x4(sp) */
    f20 = PC_LWC1(sp); /* lwc1 f20, 0x0(sp) */
    sp = sp + (0x60); /* addiu sp, sp, 0x60 */
    return; /* jr ra */
}
#else
asm void Mtx_SetRotZYX(f32 *mtx, f32 x, f32 y, f32 z)
{
    .set noreorder
    addiu sp, sp, -0x60
    sd ra, 0x20(sp)
    sq s0, 0x10(sp)
    swc1 f22, 0x8(sp)
    daddu s0, a0, zero
    swc1 f21, 0x4(sp)
    swc1 f20, 0x0(sp)
    mov.s f22, f12
    mov.s f21, f13
    jal Mtx_ClearRotation
    mov.s f20, f14
    mov.s f12, f20
    jal Math_SinCos
    addiu a0, sp, 0x50
    lwc1 f1, 0x50(sp)
    lwc1 f0, 0x54(sp)
    mfc1 a2, f1
    mfc1 a3, f0
    lqc2 vf4, 0x0(s0)
    lqc2 vf5, 0x10(s0)
    qmtc2.ni a2, vf20
    qmtc2.ni a3, vf21
    vadd.z vf10, vf0, vf0
    vaddx.y vf10, vf0, vf20x
    vaddx.x vf10, vf0, vf21x
    vadd.z vf11, vf0, vf0
    vsubx.x vf11, vf0, vf20x
    vaddx.y vf11, vf0, vf21x
    vmr32.xyz vf12, vf0
    lqc2 vf6, 0x20(s0)
    vmulax.xyz ACC, vf10, vf4x
    vmadday.xyz ACC, vf11, vf4y
    vmaddz.xyz vf4, vf12, vf4z
    vmulax.xyz ACC, vf10, vf5x
    vmadday.xyz ACC, vf11, vf5y
    vmaddz.xyz vf5, vf12, vf5z
    vmulax.xyz ACC, vf10, vf6x
    vmadday.xyz ACC, vf11, vf6y
    vmaddz.xyz vf6, vf12, vf6z
    sqc2 vf4, 0x0(s0)
    sqc2 vf5, 0x10(s0)
    sqc2 vf6, 0x20(s0)
    mov.s f12, f21
    jal Math_SinCos
    addiu a0, sp, 0x40
    lwc1 f1, 0x40(sp)
    lwc1 f0, 0x44(sp)
    mfc1 a2, f1
    mfc1 a3, f0
    lqc2 vf4, 0x0(s0)
    lqc2 vf5, 0x10(s0)
    qmtc2.ni a2, vf20
    qmtc2.ni a3, vf21
    vadd.xz vf11, vf0, vf0
    vaddw.y vf11, vf0, vf0w
    vadd.y vf10, vf0, vf0
    vaddx.x vf10, vf0, vf21x
    vsubx.z vf10, vf0, vf20x
    vadd.y vf12, vf0, vf0
    vaddx.x vf12, vf0, vf20x
    vaddx.z vf12, vf0, vf21x
    lqc2 vf6, 0x20(s0)
    vmulax.xyz ACC, vf10, vf4x
    vmadday.xyz ACC, vf11, vf4y
    vmaddz.xyz vf4, vf12, vf4z
    vmulax.xyz ACC, vf10, vf5x
    vmadday.xyz ACC, vf11, vf5y
    vmaddz.xyz vf5, vf12, vf5z
    vmulax.xyz ACC, vf10, vf6x
    vmadday.xyz ACC, vf11, vf6y
    vmaddz.xyz vf6, vf12, vf6z
    sqc2 vf4, 0x0(s0)
    sqc2 vf5, 0x10(s0)
    sqc2 vf6, 0x20(s0)
    mov.s f12, f22
    jal Math_SinCos
    addiu a0, sp, 0x30
    lwc1 f1, 0x30(sp)
    lwc1 f0, 0x34(sp)
    mfc1 a2, f1
    mfc1 a3, f0
    lqc2 vf4, 0x0(s0)
    lqc2 vf5, 0x10(s0)
    qmtc2.ni a2, vf20
    qmtc2.ni a3, vf21
    vadd.yz vf10, vf0, vf0
    vaddw.x vf10, vf0, vf0w
    vadd.x vf11, vf0, vf0
    vaddx.y vf11, vf0, vf21x
    vaddx.z vf11, vf0, vf20x
    vadd.x vf12, vf0, vf0
    vsubx.y vf12, vf0, vf20x
    vaddx.z vf12, vf0, vf21x
    lqc2 vf6, 0x20(s0)
    vmulax.xyz ACC, vf10, vf4x
    vmadday.xyz ACC, vf11, vf4y
    vmaddz.xyz vf4, vf12, vf4z
    vmulax.xyz ACC, vf10, vf5x
    vmadday.xyz ACC, vf11, vf5y
    vmaddz.xyz vf5, vf12, vf5z
    vmulax.xyz ACC, vf10, vf6x
    vmadday.xyz ACC, vf11, vf6y
    vmaddz.xyz vf6, vf12, vf6z
    sqc2 vf4, 0x0(s0)
    sqc2 vf5, 0x10(s0)
    sqc2 vf6, 0x20(s0)
    ld ra, 0x20(sp)
    lwc1 f22, 0x8(sp)
    lq s0, 0x10(sp)
    lwc1 f21, 0x4(sp)
    lwc1 f20, 0x0(sp)
    jr ra
    addiu sp, sp, 0x60
}
#endif
/* mtx rotated about x, then y, then z. */
#if PLATFORM_PC
void Mtx_RotateXYZ(f32 *mtx, f32 x, f32 y, f32 z)
{
    unsigned int a2 __attribute__((unused)) = 0;
    unsigned int a3 __attribute__((unused)) = 0;
    unsigned int t1 __attribute__((unused)) = 0;
    unsigned int ra __attribute__((unused)) = 0;
    f32 f0 __attribute__((unused)) = 0;
    f32 f1 __attribute__((unused)) = 0;
    unsigned char stk_[0x40] __attribute__((aligned(16))) = {0};
    unsigned int sp = (unsigned int)(__SIZE_TYPE__)(stk_ + 0x40);
    unsigned int a0 __attribute__((unused)) = (unsigned int)(__SIZE_TYPE__)mtx;
    f32 f12 __attribute__((unused)) = x;
    f32 f13 __attribute__((unused)) = y;
    f32 f14 __attribute__((unused)) = z;
    sp = sp + (-0x40); /* addiu sp, sp, -0x40 */
    t1 = a0 + 0; /* daddu t1, a0, zero */
    PC_SD(sp, ra); /* sd ra, 0x0(sp) */
    a0 = sp + (0x30); /* addiu a0, sp, 0x30 */
    Math_SinCos((f32 *)(__SIZE_TYPE__)a0, f12); /* jal Math_SinCos */
    f1 = PC_LWC1(sp + 0x30); /* lwc1 f1, 0x30(sp) */
    f0 = PC_LWC1(sp + 0x34); /* lwc1 f0, 0x34(sp) */
    a2 = PC_F2U(f1); /* mfc1 a2, f1 */
    a3 = PC_F2U(f0); /* mfc1 a3, f0 */
    vu0_lq(4, t1); /* lqc2 vf4, 0x0(t1) */
    vu0_lq(5, t1 + 0x10); /* lqc2 vf5, 0x10(t1) */
    vu0_qmtc2(20, a2); /* qmtc2.ni a2, vf20 */
    vu0_qmtc2(21, a3); /* qmtc2.ni a3, vf21 */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, 0x6, 10, 0, 0); /* vadd.yz vf10, vf0, vf0 */
    vu0_fmac(VU0_ADD, VU0_BW, 0, VU0_X, 10, 0, 0); /* vaddw.x vf10, vf0, vf0w */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, VU0_X, 11, 0, 0); /* vadd.x vf11, vf0, vf0 */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_Y, 11, 0, 21); /* vaddx.y vf11, vf0, vf21x */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_Z, 11, 0, 20); /* vaddx.z vf11, vf0, vf20x */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, VU0_X, 12, 0, 0); /* vadd.x vf12, vf0, vf0 */
    vu0_fmac(VU0_SUB, VU0_BX, 0, VU0_Y, 12, 0, 20); /* vsubx.y vf12, vf0, vf20x */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_Z, 12, 0, 21); /* vaddx.z vf12, vf0, vf21x */
    vu0_lq(6, t1 + 0x20); /* lqc2 vf6, 0x20(t1) */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 4); /* vmulax.xyz ACC, vf10, vf4x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 4); /* vmadday.xyz ACC, vf11, vf4y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 4, 12, 4); /* vmaddz.xyz vf4, vf12, vf4z */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 5); /* vmulax.xyz ACC, vf10, vf5x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 5); /* vmadday.xyz ACC, vf11, vf5y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 5, 12, 5); /* vmaddz.xyz vf5, vf12, vf5z */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 6); /* vmulax.xyz ACC, vf10, vf6x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 6); /* vmadday.xyz ACC, vf11, vf6y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 6, 12, 6); /* vmaddz.xyz vf6, vf12, vf6z */
    vu0_sq(4, t1); /* sqc2 vf4, 0x0(t1) */
    vu0_sq(5, t1 + 0x10); /* sqc2 vf5, 0x10(t1) */
    vu0_sq(6, t1 + 0x20); /* sqc2 vf6, 0x20(t1) */
    f12 = f13; /* mov.s f12, f13 */
    a0 = sp + (0x20); /* addiu a0, sp, 0x20 */
    Math_SinCos((f32 *)(__SIZE_TYPE__)a0, f12); /* jal Math_SinCos */
    f1 = PC_LWC1(sp + 0x20); /* lwc1 f1, 0x20(sp) */
    f0 = PC_LWC1(sp + 0x24); /* lwc1 f0, 0x24(sp) */
    a2 = PC_F2U(f1); /* mfc1 a2, f1 */
    a3 = PC_F2U(f0); /* mfc1 a3, f0 */
    vu0_lq(4, t1); /* lqc2 vf4, 0x0(t1) */
    vu0_lq(5, t1 + 0x10); /* lqc2 vf5, 0x10(t1) */
    vu0_qmtc2(20, a2); /* qmtc2.ni a2, vf20 */
    vu0_qmtc2(21, a3); /* qmtc2.ni a3, vf21 */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, 0xA, 11, 0, 0); /* vadd.xz vf11, vf0, vf0 */
    vu0_fmac(VU0_ADD, VU0_BW, 0, VU0_Y, 11, 0, 0); /* vaddw.y vf11, vf0, vf0w */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, VU0_Y, 10, 0, 0); /* vadd.y vf10, vf0, vf0 */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_X, 10, 0, 21); /* vaddx.x vf10, vf0, vf21x */
    vu0_fmac(VU0_SUB, VU0_BX, 0, VU0_Z, 10, 0, 20); /* vsubx.z vf10, vf0, vf20x */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, VU0_Y, 12, 0, 0); /* vadd.y vf12, vf0, vf0 */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_X, 12, 0, 20); /* vaddx.x vf12, vf0, vf20x */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_Z, 12, 0, 21); /* vaddx.z vf12, vf0, vf21x */
    vu0_lq(6, t1 + 0x20); /* lqc2 vf6, 0x20(t1) */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 4); /* vmulax.xyz ACC, vf10, vf4x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 4); /* vmadday.xyz ACC, vf11, vf4y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 4, 12, 4); /* vmaddz.xyz vf4, vf12, vf4z */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 5); /* vmulax.xyz ACC, vf10, vf5x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 5); /* vmadday.xyz ACC, vf11, vf5y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 5, 12, 5); /* vmaddz.xyz vf5, vf12, vf5z */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 6); /* vmulax.xyz ACC, vf10, vf6x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 6); /* vmadday.xyz ACC, vf11, vf6y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 6, 12, 6); /* vmaddz.xyz vf6, vf12, vf6z */
    vu0_sq(4, t1); /* sqc2 vf4, 0x0(t1) */
    vu0_sq(5, t1 + 0x10); /* sqc2 vf5, 0x10(t1) */
    vu0_sq(6, t1 + 0x20); /* sqc2 vf6, 0x20(t1) */
    f12 = f14; /* mov.s f12, f14 */
    a0 = sp + (0x10); /* addiu a0, sp, 0x10 */
    Math_SinCos((f32 *)(__SIZE_TYPE__)a0, f12); /* jal Math_SinCos */
    f1 = PC_LWC1(sp + 0x10); /* lwc1 f1, 0x10(sp) */
    f0 = PC_LWC1(sp + 0x14); /* lwc1 f0, 0x14(sp) */
    a2 = PC_F2U(f1); /* mfc1 a2, f1 */
    a3 = PC_F2U(f0); /* mfc1 a3, f0 */
    vu0_lq(4, t1); /* lqc2 vf4, 0x0(t1) */
    vu0_lq(5, t1 + 0x10); /* lqc2 vf5, 0x10(t1) */
    vu0_qmtc2(20, a2); /* qmtc2.ni a2, vf20 */
    vu0_qmtc2(21, a3); /* qmtc2.ni a3, vf21 */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, VU0_Z, 10, 0, 0); /* vadd.z vf10, vf0, vf0 */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_Y, 10, 0, 20); /* vaddx.y vf10, vf0, vf20x */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_X, 10, 0, 21); /* vaddx.x vf10, vf0, vf21x */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, VU0_Z, 11, 0, 0); /* vadd.z vf11, vf0, vf0 */
    vu0_fmac(VU0_SUB, VU0_BX, 0, VU0_X, 11, 0, 20); /* vsubx.x vf11, vf0, vf20x */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_Y, 11, 0, 21); /* vaddx.y vf11, vf0, vf21x */
    vu0_mr32(VU0_XYZ, 12, 0); /* vmr32.xyz vf12, vf0 */
    vu0_lq(6, t1 + 0x20); /* lqc2 vf6, 0x20(t1) */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 4); /* vmulax.xyz ACC, vf10, vf4x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 4); /* vmadday.xyz ACC, vf11, vf4y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 4, 12, 4); /* vmaddz.xyz vf4, vf12, vf4z */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 5); /* vmulax.xyz ACC, vf10, vf5x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 5); /* vmadday.xyz ACC, vf11, vf5y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 5, 12, 5); /* vmaddz.xyz vf5, vf12, vf5z */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 6); /* vmulax.xyz ACC, vf10, vf6x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 6); /* vmadday.xyz ACC, vf11, vf6y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 6, 12, 6); /* vmaddz.xyz vf6, vf12, vf6z */
    vu0_sq(4, t1); /* sqc2 vf4, 0x0(t1) */
    vu0_sq(5, t1 + 0x10); /* sqc2 vf5, 0x10(t1) */
    vu0_sq(6, t1 + 0x20); /* sqc2 vf6, 0x20(t1) */
    ra = (unsigned int)PC_LD(sp); /* ld ra, 0x0(sp) */
    sp = sp + (0x40); /* addiu sp, sp, 0x40 */
    return; /* jr ra */
}
#else
asm void Mtx_RotateXYZ(f32 *mtx, f32 x, f32 y, f32 z)
{
    .set noreorder
    addiu sp, sp, -0x40
    daddu t1, a0, zero
    sd ra, 0x0(sp)
    jal Math_SinCos
    addiu a0, sp, 0x30
    lwc1 f1, 0x30(sp)
    lwc1 f0, 0x34(sp)
    mfc1 a2, f1
    mfc1 a3, f0
    lqc2 vf4, 0x0(t1)
    lqc2 vf5, 0x10(t1)
    qmtc2.ni a2, vf20
    qmtc2.ni a3, vf21
    vadd.yz vf10, vf0, vf0
    vaddw.x vf10, vf0, vf0w
    vadd.x vf11, vf0, vf0
    vaddx.y vf11, vf0, vf21x
    vaddx.z vf11, vf0, vf20x
    vadd.x vf12, vf0, vf0
    vsubx.y vf12, vf0, vf20x
    vaddx.z vf12, vf0, vf21x
    lqc2 vf6, 0x20(t1)
    vmulax.xyz ACC, vf10, vf4x
    vmadday.xyz ACC, vf11, vf4y
    vmaddz.xyz vf4, vf12, vf4z
    vmulax.xyz ACC, vf10, vf5x
    vmadday.xyz ACC, vf11, vf5y
    vmaddz.xyz vf5, vf12, vf5z
    vmulax.xyz ACC, vf10, vf6x
    vmadday.xyz ACC, vf11, vf6y
    vmaddz.xyz vf6, vf12, vf6z
    sqc2 vf4, 0x0(t1)
    sqc2 vf5, 0x10(t1)
    sqc2 vf6, 0x20(t1)
    mov.s f12, f13
    jal Math_SinCos
    addiu a0, sp, 0x20
    lwc1 f1, 0x20(sp)
    lwc1 f0, 0x24(sp)
    mfc1 a2, f1
    mfc1 a3, f0
    lqc2 vf4, 0x0(t1)
    lqc2 vf5, 0x10(t1)
    qmtc2.ni a2, vf20
    qmtc2.ni a3, vf21
    vadd.xz vf11, vf0, vf0
    vaddw.y vf11, vf0, vf0w
    vadd.y vf10, vf0, vf0
    vaddx.x vf10, vf0, vf21x
    vsubx.z vf10, vf0, vf20x
    vadd.y vf12, vf0, vf0
    vaddx.x vf12, vf0, vf20x
    vaddx.z vf12, vf0, vf21x
    lqc2 vf6, 0x20(t1)
    vmulax.xyz ACC, vf10, vf4x
    vmadday.xyz ACC, vf11, vf4y
    vmaddz.xyz vf4, vf12, vf4z
    vmulax.xyz ACC, vf10, vf5x
    vmadday.xyz ACC, vf11, vf5y
    vmaddz.xyz vf5, vf12, vf5z
    vmulax.xyz ACC, vf10, vf6x
    vmadday.xyz ACC, vf11, vf6y
    vmaddz.xyz vf6, vf12, vf6z
    sqc2 vf4, 0x0(t1)
    sqc2 vf5, 0x10(t1)
    sqc2 vf6, 0x20(t1)
    mov.s f12, f14
    jal Math_SinCos
    addiu a0, sp, 0x10
    lwc1 f1, 0x10(sp)
    lwc1 f0, 0x14(sp)
    mfc1 a2, f1
    mfc1 a3, f0
    lqc2 vf4, 0x0(t1)
    lqc2 vf5, 0x10(t1)
    qmtc2.ni a2, vf20
    qmtc2.ni a3, vf21
    vadd.z vf10, vf0, vf0
    vaddx.y vf10, vf0, vf20x
    vaddx.x vf10, vf0, vf21x
    vadd.z vf11, vf0, vf0
    vsubx.x vf11, vf0, vf20x
    vaddx.y vf11, vf0, vf21x
    vmr32.xyz vf12, vf0
    lqc2 vf6, 0x20(t1)
    vmulax.xyz ACC, vf10, vf4x
    vmadday.xyz ACC, vf11, vf4y
    vmaddz.xyz vf4, vf12, vf4z
    vmulax.xyz ACC, vf10, vf5x
    vmadday.xyz ACC, vf11, vf5y
    vmaddz.xyz vf5, vf12, vf5z
    vmulax.xyz ACC, vf10, vf6x
    vmadday.xyz ACC, vf11, vf6y
    vmaddz.xyz vf6, vf12, vf6z
    sqc2 vf4, 0x0(t1)
    sqc2 vf5, 0x10(t1)
    sqc2 vf6, 0x20(t1)
    ld ra, 0x0(sp)
    jr ra
    addiu sp, sp, 0x40
}
#endif
/* mtx rotated about x, then z, then y. */
#if PLATFORM_PC
void Mtx_RotateXZY(f32 *mtx, f32 x, f32 y, f32 z)
{
    unsigned int a2 __attribute__((unused)) = 0;
    unsigned int a3 __attribute__((unused)) = 0;
    unsigned int t1 __attribute__((unused)) = 0;
    unsigned int ra __attribute__((unused)) = 0;
    f32 f0 __attribute__((unused)) = 0;
    f32 f1 __attribute__((unused)) = 0;
    unsigned char stk_[0x40] __attribute__((aligned(16))) = {0};
    unsigned int sp = (unsigned int)(__SIZE_TYPE__)(stk_ + 0x40);
    unsigned int a0 __attribute__((unused)) = (unsigned int)(__SIZE_TYPE__)mtx;
    f32 f12 __attribute__((unused)) = x;
    f32 f13 __attribute__((unused)) = y;
    f32 f14 __attribute__((unused)) = z;
    sp = sp + (-0x40); /* addiu sp, sp, -0x40 */
    t1 = a0 + 0; /* daddu t1, a0, zero */
    PC_SD(sp, ra); /* sd ra, 0x0(sp) */
    a0 = sp + (0x30); /* addiu a0, sp, 0x30 */
    Math_SinCos((f32 *)(__SIZE_TYPE__)a0, f12); /* jal Math_SinCos */
    f1 = PC_LWC1(sp + 0x30); /* lwc1 f1, 0x30(sp) */
    f0 = PC_LWC1(sp + 0x34); /* lwc1 f0, 0x34(sp) */
    a2 = PC_F2U(f1); /* mfc1 a2, f1 */
    a3 = PC_F2U(f0); /* mfc1 a3, f0 */
    vu0_lq(4, t1); /* lqc2 vf4, 0x0(t1) */
    vu0_lq(5, t1 + 0x10); /* lqc2 vf5, 0x10(t1) */
    vu0_qmtc2(20, a2); /* qmtc2.ni a2, vf20 */
    vu0_qmtc2(21, a3); /* qmtc2.ni a3, vf21 */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, 0x6, 10, 0, 0); /* vadd.yz vf10, vf0, vf0 */
    vu0_fmac(VU0_ADD, VU0_BW, 0, VU0_X, 10, 0, 0); /* vaddw.x vf10, vf0, vf0w */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, VU0_X, 11, 0, 0); /* vadd.x vf11, vf0, vf0 */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_Y, 11, 0, 21); /* vaddx.y vf11, vf0, vf21x */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_Z, 11, 0, 20); /* vaddx.z vf11, vf0, vf20x */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, VU0_X, 12, 0, 0); /* vadd.x vf12, vf0, vf0 */
    vu0_fmac(VU0_SUB, VU0_BX, 0, VU0_Y, 12, 0, 20); /* vsubx.y vf12, vf0, vf20x */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_Z, 12, 0, 21); /* vaddx.z vf12, vf0, vf21x */
    vu0_lq(6, t1 + 0x20); /* lqc2 vf6, 0x20(t1) */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 4); /* vmulax.xyz ACC, vf10, vf4x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 4); /* vmadday.xyz ACC, vf11, vf4y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 4, 12, 4); /* vmaddz.xyz vf4, vf12, vf4z */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 5); /* vmulax.xyz ACC, vf10, vf5x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 5); /* vmadday.xyz ACC, vf11, vf5y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 5, 12, 5); /* vmaddz.xyz vf5, vf12, vf5z */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 6); /* vmulax.xyz ACC, vf10, vf6x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 6); /* vmadday.xyz ACC, vf11, vf6y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 6, 12, 6); /* vmaddz.xyz vf6, vf12, vf6z */
    vu0_sq(4, t1); /* sqc2 vf4, 0x0(t1) */
    vu0_sq(5, t1 + 0x10); /* sqc2 vf5, 0x10(t1) */
    vu0_sq(6, t1 + 0x20); /* sqc2 vf6, 0x20(t1) */
    f12 = f14; /* mov.s f12, f14 */
    a0 = sp + (0x20); /* addiu a0, sp, 0x20 */
    Math_SinCos((f32 *)(__SIZE_TYPE__)a0, f12); /* jal Math_SinCos */
    f1 = PC_LWC1(sp + 0x20); /* lwc1 f1, 0x20(sp) */
    f0 = PC_LWC1(sp + 0x24); /* lwc1 f0, 0x24(sp) */
    a2 = PC_F2U(f1); /* mfc1 a2, f1 */
    a3 = PC_F2U(f0); /* mfc1 a3, f0 */
    vu0_lq(4, t1); /* lqc2 vf4, 0x0(t1) */
    vu0_lq(5, t1 + 0x10); /* lqc2 vf5, 0x10(t1) */
    vu0_qmtc2(20, a2); /* qmtc2.ni a2, vf20 */
    vu0_qmtc2(21, a3); /* qmtc2.ni a3, vf21 */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, VU0_Z, 10, 0, 0); /* vadd.z vf10, vf0, vf0 */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_Y, 10, 0, 20); /* vaddx.y vf10, vf0, vf20x */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_X, 10, 0, 21); /* vaddx.x vf10, vf0, vf21x */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, VU0_Z, 11, 0, 0); /* vadd.z vf11, vf0, vf0 */
    vu0_fmac(VU0_SUB, VU0_BX, 0, VU0_X, 11, 0, 20); /* vsubx.x vf11, vf0, vf20x */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_Y, 11, 0, 21); /* vaddx.y vf11, vf0, vf21x */
    vu0_mr32(VU0_XYZ, 12, 0); /* vmr32.xyz vf12, vf0 */
    vu0_lq(6, t1 + 0x20); /* lqc2 vf6, 0x20(t1) */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 4); /* vmulax.xyz ACC, vf10, vf4x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 4); /* vmadday.xyz ACC, vf11, vf4y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 4, 12, 4); /* vmaddz.xyz vf4, vf12, vf4z */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 5); /* vmulax.xyz ACC, vf10, vf5x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 5); /* vmadday.xyz ACC, vf11, vf5y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 5, 12, 5); /* vmaddz.xyz vf5, vf12, vf5z */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 6); /* vmulax.xyz ACC, vf10, vf6x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 6); /* vmadday.xyz ACC, vf11, vf6y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 6, 12, 6); /* vmaddz.xyz vf6, vf12, vf6z */
    vu0_sq(4, t1); /* sqc2 vf4, 0x0(t1) */
    vu0_sq(5, t1 + 0x10); /* sqc2 vf5, 0x10(t1) */
    vu0_sq(6, t1 + 0x20); /* sqc2 vf6, 0x20(t1) */
    f12 = f13; /* mov.s f12, f13 */
    a0 = sp + (0x10); /* addiu a0, sp, 0x10 */
    Math_SinCos((f32 *)(__SIZE_TYPE__)a0, f12); /* jal Math_SinCos */
    f1 = PC_LWC1(sp + 0x10); /* lwc1 f1, 0x10(sp) */
    f0 = PC_LWC1(sp + 0x14); /* lwc1 f0, 0x14(sp) */
    a2 = PC_F2U(f1); /* mfc1 a2, f1 */
    a3 = PC_F2U(f0); /* mfc1 a3, f0 */
    vu0_lq(4, t1); /* lqc2 vf4, 0x0(t1) */
    vu0_lq(5, t1 + 0x10); /* lqc2 vf5, 0x10(t1) */
    vu0_qmtc2(20, a2); /* qmtc2.ni a2, vf20 */
    vu0_qmtc2(21, a3); /* qmtc2.ni a3, vf21 */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, 0xA, 11, 0, 0); /* vadd.xz vf11, vf0, vf0 */
    vu0_fmac(VU0_ADD, VU0_BW, 0, VU0_Y, 11, 0, 0); /* vaddw.y vf11, vf0, vf0w */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, VU0_Y, 10, 0, 0); /* vadd.y vf10, vf0, vf0 */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_X, 10, 0, 21); /* vaddx.x vf10, vf0, vf21x */
    vu0_fmac(VU0_SUB, VU0_BX, 0, VU0_Z, 10, 0, 20); /* vsubx.z vf10, vf0, vf20x */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, VU0_Y, 12, 0, 0); /* vadd.y vf12, vf0, vf0 */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_X, 12, 0, 20); /* vaddx.x vf12, vf0, vf20x */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_Z, 12, 0, 21); /* vaddx.z vf12, vf0, vf21x */
    vu0_lq(6, t1 + 0x20); /* lqc2 vf6, 0x20(t1) */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 4); /* vmulax.xyz ACC, vf10, vf4x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 4); /* vmadday.xyz ACC, vf11, vf4y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 4, 12, 4); /* vmaddz.xyz vf4, vf12, vf4z */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 5); /* vmulax.xyz ACC, vf10, vf5x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 5); /* vmadday.xyz ACC, vf11, vf5y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 5, 12, 5); /* vmaddz.xyz vf5, vf12, vf5z */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 6); /* vmulax.xyz ACC, vf10, vf6x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 6); /* vmadday.xyz ACC, vf11, vf6y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 6, 12, 6); /* vmaddz.xyz vf6, vf12, vf6z */
    vu0_sq(4, t1); /* sqc2 vf4, 0x0(t1) */
    vu0_sq(5, t1 + 0x10); /* sqc2 vf5, 0x10(t1) */
    vu0_sq(6, t1 + 0x20); /* sqc2 vf6, 0x20(t1) */
    ra = (unsigned int)PC_LD(sp); /* ld ra, 0x0(sp) */
    sp = sp + (0x40); /* addiu sp, sp, 0x40 */
    return; /* jr ra */
}
#else
asm void Mtx_RotateXZY(f32 *mtx, f32 x, f32 y, f32 z)
{
    .set noreorder
    addiu sp, sp, -0x40
    daddu t1, a0, zero
    sd ra, 0x0(sp)
    jal Math_SinCos
    addiu a0, sp, 0x30
    lwc1 f1, 0x30(sp)
    lwc1 f0, 0x34(sp)
    mfc1 a2, f1
    mfc1 a3, f0
    lqc2 vf4, 0x0(t1)
    lqc2 vf5, 0x10(t1)
    qmtc2.ni a2, vf20
    qmtc2.ni a3, vf21
    vadd.yz vf10, vf0, vf0
    vaddw.x vf10, vf0, vf0w
    vadd.x vf11, vf0, vf0
    vaddx.y vf11, vf0, vf21x
    vaddx.z vf11, vf0, vf20x
    vadd.x vf12, vf0, vf0
    vsubx.y vf12, vf0, vf20x
    vaddx.z vf12, vf0, vf21x
    lqc2 vf6, 0x20(t1)
    vmulax.xyz ACC, vf10, vf4x
    vmadday.xyz ACC, vf11, vf4y
    vmaddz.xyz vf4, vf12, vf4z
    vmulax.xyz ACC, vf10, vf5x
    vmadday.xyz ACC, vf11, vf5y
    vmaddz.xyz vf5, vf12, vf5z
    vmulax.xyz ACC, vf10, vf6x
    vmadday.xyz ACC, vf11, vf6y
    vmaddz.xyz vf6, vf12, vf6z
    sqc2 vf4, 0x0(t1)
    sqc2 vf5, 0x10(t1)
    sqc2 vf6, 0x20(t1)
    mov.s f12, f14
    jal Math_SinCos
    addiu a0, sp, 0x20
    lwc1 f1, 0x20(sp)
    lwc1 f0, 0x24(sp)
    mfc1 a2, f1
    mfc1 a3, f0
    lqc2 vf4, 0x0(t1)
    lqc2 vf5, 0x10(t1)
    qmtc2.ni a2, vf20
    qmtc2.ni a3, vf21
    vadd.z vf10, vf0, vf0
    vaddx.y vf10, vf0, vf20x
    vaddx.x vf10, vf0, vf21x
    vadd.z vf11, vf0, vf0
    vsubx.x vf11, vf0, vf20x
    vaddx.y vf11, vf0, vf21x
    vmr32.xyz vf12, vf0
    lqc2 vf6, 0x20(t1)
    vmulax.xyz ACC, vf10, vf4x
    vmadday.xyz ACC, vf11, vf4y
    vmaddz.xyz vf4, vf12, vf4z
    vmulax.xyz ACC, vf10, vf5x
    vmadday.xyz ACC, vf11, vf5y
    vmaddz.xyz vf5, vf12, vf5z
    vmulax.xyz ACC, vf10, vf6x
    vmadday.xyz ACC, vf11, vf6y
    vmaddz.xyz vf6, vf12, vf6z
    sqc2 vf4, 0x0(t1)
    sqc2 vf5, 0x10(t1)
    sqc2 vf6, 0x20(t1)
    mov.s f12, f13
    jal Math_SinCos
    addiu a0, sp, 0x10
    lwc1 f1, 0x10(sp)
    lwc1 f0, 0x14(sp)
    mfc1 a2, f1
    mfc1 a3, f0
    lqc2 vf4, 0x0(t1)
    lqc2 vf5, 0x10(t1)
    qmtc2.ni a2, vf20
    qmtc2.ni a3, vf21
    vadd.xz vf11, vf0, vf0
    vaddw.y vf11, vf0, vf0w
    vadd.y vf10, vf0, vf0
    vaddx.x vf10, vf0, vf21x
    vsubx.z vf10, vf0, vf20x
    vadd.y vf12, vf0, vf0
    vaddx.x vf12, vf0, vf20x
    vaddx.z vf12, vf0, vf21x
    lqc2 vf6, 0x20(t1)
    vmulax.xyz ACC, vf10, vf4x
    vmadday.xyz ACC, vf11, vf4y
    vmaddz.xyz vf4, vf12, vf4z
    vmulax.xyz ACC, vf10, vf5x
    vmadday.xyz ACC, vf11, vf5y
    vmaddz.xyz vf5, vf12, vf5z
    vmulax.xyz ACC, vf10, vf6x
    vmadday.xyz ACC, vf11, vf6y
    vmaddz.xyz vf6, vf12, vf6z
    sqc2 vf4, 0x0(t1)
    sqc2 vf5, 0x10(t1)
    sqc2 vf6, 0x20(t1)
    ld ra, 0x0(sp)
    jr ra
    addiu sp, sp, 0x40
}
#endif
/* mtx rotated about y, then x, then z. */
#if PLATFORM_PC
void Mtx_RotateYXZ(f32 *mtx, f32 x, f32 y, f32 z)
{
    unsigned int a2 __attribute__((unused)) = 0;
    unsigned int a3 __attribute__((unused)) = 0;
    unsigned int t1 __attribute__((unused)) = 0;
    unsigned int ra __attribute__((unused)) = 0;
    f32 f0 __attribute__((unused)) = 0;
    f32 f1 __attribute__((unused)) = 0;
    f32 f2 __attribute__((unused)) = 0;
    unsigned char stk_[0x40] __attribute__((aligned(16))) = {0};
    unsigned int sp = (unsigned int)(__SIZE_TYPE__)(stk_ + 0x40);
    unsigned int a0 __attribute__((unused)) = (unsigned int)(__SIZE_TYPE__)mtx;
    f32 f12 __attribute__((unused)) = x;
    f32 f13 __attribute__((unused)) = y;
    f32 f14 __attribute__((unused)) = z;
    sp = sp + (-0x40); /* addiu sp, sp, -0x40 */
    t1 = a0 + 0; /* daddu t1, a0, zero */
    f2 = f12; /* mov.s f2, f12 */
    PC_SD(sp, ra); /* sd ra, 0x0(sp) */
    a0 = sp + (0x30); /* addiu a0, sp, 0x30 */
    f12 = f13; /* mov.s f12, f13 */
    Math_SinCos((f32 *)(__SIZE_TYPE__)a0, f12); /* jal Math_SinCos */
    f1 = PC_LWC1(sp + 0x30); /* lwc1 f1, 0x30(sp) */
    f0 = PC_LWC1(sp + 0x34); /* lwc1 f0, 0x34(sp) */
    a2 = PC_F2U(f1); /* mfc1 a2, f1 */
    a3 = PC_F2U(f0); /* mfc1 a3, f0 */
    vu0_lq(4, t1); /* lqc2 vf4, 0x0(t1) */
    vu0_lq(5, t1 + 0x10); /* lqc2 vf5, 0x10(t1) */
    vu0_qmtc2(20, a2); /* qmtc2.ni a2, vf20 */
    vu0_qmtc2(21, a3); /* qmtc2.ni a3, vf21 */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, 0xA, 11, 0, 0); /* vadd.xz vf11, vf0, vf0 */
    vu0_fmac(VU0_ADD, VU0_BW, 0, VU0_Y, 11, 0, 0); /* vaddw.y vf11, vf0, vf0w */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, VU0_Y, 10, 0, 0); /* vadd.y vf10, vf0, vf0 */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_X, 10, 0, 21); /* vaddx.x vf10, vf0, vf21x */
    vu0_fmac(VU0_SUB, VU0_BX, 0, VU0_Z, 10, 0, 20); /* vsubx.z vf10, vf0, vf20x */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, VU0_Y, 12, 0, 0); /* vadd.y vf12, vf0, vf0 */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_X, 12, 0, 20); /* vaddx.x vf12, vf0, vf20x */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_Z, 12, 0, 21); /* vaddx.z vf12, vf0, vf21x */
    vu0_lq(6, t1 + 0x20); /* lqc2 vf6, 0x20(t1) */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 4); /* vmulax.xyz ACC, vf10, vf4x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 4); /* vmadday.xyz ACC, vf11, vf4y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 4, 12, 4); /* vmaddz.xyz vf4, vf12, vf4z */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 5); /* vmulax.xyz ACC, vf10, vf5x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 5); /* vmadday.xyz ACC, vf11, vf5y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 5, 12, 5); /* vmaddz.xyz vf5, vf12, vf5z */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 6); /* vmulax.xyz ACC, vf10, vf6x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 6); /* vmadday.xyz ACC, vf11, vf6y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 6, 12, 6); /* vmaddz.xyz vf6, vf12, vf6z */
    vu0_sq(4, t1); /* sqc2 vf4, 0x0(t1) */
    vu0_sq(5, t1 + 0x10); /* sqc2 vf5, 0x10(t1) */
    vu0_sq(6, t1 + 0x20); /* sqc2 vf6, 0x20(t1) */
    f12 = f2; /* mov.s f12, f2 */
    a0 = sp + (0x20); /* addiu a0, sp, 0x20 */
    Math_SinCos((f32 *)(__SIZE_TYPE__)a0, f12); /* jal Math_SinCos */
    f1 = PC_LWC1(sp + 0x20); /* lwc1 f1, 0x20(sp) */
    f0 = PC_LWC1(sp + 0x24); /* lwc1 f0, 0x24(sp) */
    a2 = PC_F2U(f1); /* mfc1 a2, f1 */
    a3 = PC_F2U(f0); /* mfc1 a3, f0 */
    vu0_lq(4, t1); /* lqc2 vf4, 0x0(t1) */
    vu0_lq(5, t1 + 0x10); /* lqc2 vf5, 0x10(t1) */
    vu0_qmtc2(20, a2); /* qmtc2.ni a2, vf20 */
    vu0_qmtc2(21, a3); /* qmtc2.ni a3, vf21 */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, 0x6, 10, 0, 0); /* vadd.yz vf10, vf0, vf0 */
    vu0_fmac(VU0_ADD, VU0_BW, 0, VU0_X, 10, 0, 0); /* vaddw.x vf10, vf0, vf0w */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, VU0_X, 11, 0, 0); /* vadd.x vf11, vf0, vf0 */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_Y, 11, 0, 21); /* vaddx.y vf11, vf0, vf21x */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_Z, 11, 0, 20); /* vaddx.z vf11, vf0, vf20x */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, VU0_X, 12, 0, 0); /* vadd.x vf12, vf0, vf0 */
    vu0_fmac(VU0_SUB, VU0_BX, 0, VU0_Y, 12, 0, 20); /* vsubx.y vf12, vf0, vf20x */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_Z, 12, 0, 21); /* vaddx.z vf12, vf0, vf21x */
    vu0_lq(6, t1 + 0x20); /* lqc2 vf6, 0x20(t1) */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 4); /* vmulax.xyz ACC, vf10, vf4x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 4); /* vmadday.xyz ACC, vf11, vf4y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 4, 12, 4); /* vmaddz.xyz vf4, vf12, vf4z */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 5); /* vmulax.xyz ACC, vf10, vf5x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 5); /* vmadday.xyz ACC, vf11, vf5y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 5, 12, 5); /* vmaddz.xyz vf5, vf12, vf5z */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 6); /* vmulax.xyz ACC, vf10, vf6x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 6); /* vmadday.xyz ACC, vf11, vf6y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 6, 12, 6); /* vmaddz.xyz vf6, vf12, vf6z */
    vu0_sq(4, t1); /* sqc2 vf4, 0x0(t1) */
    vu0_sq(5, t1 + 0x10); /* sqc2 vf5, 0x10(t1) */
    vu0_sq(6, t1 + 0x20); /* sqc2 vf6, 0x20(t1) */
    f12 = f14; /* mov.s f12, f14 */
    a0 = sp + (0x10); /* addiu a0, sp, 0x10 */
    Math_SinCos((f32 *)(__SIZE_TYPE__)a0, f12); /* jal Math_SinCos */
    f1 = PC_LWC1(sp + 0x10); /* lwc1 f1, 0x10(sp) */
    f0 = PC_LWC1(sp + 0x14); /* lwc1 f0, 0x14(sp) */
    a2 = PC_F2U(f1); /* mfc1 a2, f1 */
    a3 = PC_F2U(f0); /* mfc1 a3, f0 */
    vu0_lq(4, t1); /* lqc2 vf4, 0x0(t1) */
    vu0_lq(5, t1 + 0x10); /* lqc2 vf5, 0x10(t1) */
    vu0_qmtc2(20, a2); /* qmtc2.ni a2, vf20 */
    vu0_qmtc2(21, a3); /* qmtc2.ni a3, vf21 */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, VU0_Z, 10, 0, 0); /* vadd.z vf10, vf0, vf0 */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_Y, 10, 0, 20); /* vaddx.y vf10, vf0, vf20x */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_X, 10, 0, 21); /* vaddx.x vf10, vf0, vf21x */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, VU0_Z, 11, 0, 0); /* vadd.z vf11, vf0, vf0 */
    vu0_fmac(VU0_SUB, VU0_BX, 0, VU0_X, 11, 0, 20); /* vsubx.x vf11, vf0, vf20x */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_Y, 11, 0, 21); /* vaddx.y vf11, vf0, vf21x */
    vu0_mr32(VU0_XYZ, 12, 0); /* vmr32.xyz vf12, vf0 */
    vu0_lq(6, t1 + 0x20); /* lqc2 vf6, 0x20(t1) */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 4); /* vmulax.xyz ACC, vf10, vf4x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 4); /* vmadday.xyz ACC, vf11, vf4y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 4, 12, 4); /* vmaddz.xyz vf4, vf12, vf4z */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 5); /* vmulax.xyz ACC, vf10, vf5x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 5); /* vmadday.xyz ACC, vf11, vf5y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 5, 12, 5); /* vmaddz.xyz vf5, vf12, vf5z */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 6); /* vmulax.xyz ACC, vf10, vf6x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 6); /* vmadday.xyz ACC, vf11, vf6y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 6, 12, 6); /* vmaddz.xyz vf6, vf12, vf6z */
    vu0_sq(4, t1); /* sqc2 vf4, 0x0(t1) */
    vu0_sq(5, t1 + 0x10); /* sqc2 vf5, 0x10(t1) */
    vu0_sq(6, t1 + 0x20); /* sqc2 vf6, 0x20(t1) */
    ra = (unsigned int)PC_LD(sp); /* ld ra, 0x0(sp) */
    sp = sp + (0x40); /* addiu sp, sp, 0x40 */
    return; /* jr ra */
}
#else
asm void Mtx_RotateYXZ(f32 *mtx, f32 x, f32 y, f32 z)
{
    .set noreorder
    addiu sp, sp, -0x40
    daddu t1, a0, zero
    mov.s f2, f12
    sd ra, 0x0(sp)
    addiu a0, sp, 0x30
    jal Math_SinCos
    mov.s f12, f13
    lwc1 f1, 0x30(sp)
    lwc1 f0, 0x34(sp)
    mfc1 a2, f1
    mfc1 a3, f0
    lqc2 vf4, 0x0(t1)
    lqc2 vf5, 0x10(t1)
    qmtc2.ni a2, vf20
    qmtc2.ni a3, vf21
    vadd.xz vf11, vf0, vf0
    vaddw.y vf11, vf0, vf0w
    vadd.y vf10, vf0, vf0
    vaddx.x vf10, vf0, vf21x
    vsubx.z vf10, vf0, vf20x
    vadd.y vf12, vf0, vf0
    vaddx.x vf12, vf0, vf20x
    vaddx.z vf12, vf0, vf21x
    lqc2 vf6, 0x20(t1)
    vmulax.xyz ACC, vf10, vf4x
    vmadday.xyz ACC, vf11, vf4y
    vmaddz.xyz vf4, vf12, vf4z
    vmulax.xyz ACC, vf10, vf5x
    vmadday.xyz ACC, vf11, vf5y
    vmaddz.xyz vf5, vf12, vf5z
    vmulax.xyz ACC, vf10, vf6x
    vmadday.xyz ACC, vf11, vf6y
    vmaddz.xyz vf6, vf12, vf6z
    sqc2 vf4, 0x0(t1)
    sqc2 vf5, 0x10(t1)
    sqc2 vf6, 0x20(t1)
    mov.s f12, f2
    jal Math_SinCos
    addiu a0, sp, 0x20
    lwc1 f1, 0x20(sp)
    lwc1 f0, 0x24(sp)
    mfc1 a2, f1
    mfc1 a3, f0
    lqc2 vf4, 0x0(t1)
    lqc2 vf5, 0x10(t1)
    qmtc2.ni a2, vf20
    qmtc2.ni a3, vf21
    vadd.yz vf10, vf0, vf0
    vaddw.x vf10, vf0, vf0w
    vadd.x vf11, vf0, vf0
    vaddx.y vf11, vf0, vf21x
    vaddx.z vf11, vf0, vf20x
    vadd.x vf12, vf0, vf0
    vsubx.y vf12, vf0, vf20x
    vaddx.z vf12, vf0, vf21x
    lqc2 vf6, 0x20(t1)
    vmulax.xyz ACC, vf10, vf4x
    vmadday.xyz ACC, vf11, vf4y
    vmaddz.xyz vf4, vf12, vf4z
    vmulax.xyz ACC, vf10, vf5x
    vmadday.xyz ACC, vf11, vf5y
    vmaddz.xyz vf5, vf12, vf5z
    vmulax.xyz ACC, vf10, vf6x
    vmadday.xyz ACC, vf11, vf6y
    vmaddz.xyz vf6, vf12, vf6z
    sqc2 vf4, 0x0(t1)
    sqc2 vf5, 0x10(t1)
    sqc2 vf6, 0x20(t1)
    mov.s f12, f14
    jal Math_SinCos
    addiu a0, sp, 0x10
    lwc1 f1, 0x10(sp)
    lwc1 f0, 0x14(sp)
    mfc1 a2, f1
    mfc1 a3, f0
    lqc2 vf4, 0x0(t1)
    lqc2 vf5, 0x10(t1)
    qmtc2.ni a2, vf20
    qmtc2.ni a3, vf21
    vadd.z vf10, vf0, vf0
    vaddx.y vf10, vf0, vf20x
    vaddx.x vf10, vf0, vf21x
    vadd.z vf11, vf0, vf0
    vsubx.x vf11, vf0, vf20x
    vaddx.y vf11, vf0, vf21x
    vmr32.xyz vf12, vf0
    lqc2 vf6, 0x20(t1)
    vmulax.xyz ACC, vf10, vf4x
    vmadday.xyz ACC, vf11, vf4y
    vmaddz.xyz vf4, vf12, vf4z
    vmulax.xyz ACC, vf10, vf5x
    vmadday.xyz ACC, vf11, vf5y
    vmaddz.xyz vf5, vf12, vf5z
    vmulax.xyz ACC, vf10, vf6x
    vmadday.xyz ACC, vf11, vf6y
    vmaddz.xyz vf6, vf12, vf6z
    sqc2 vf4, 0x0(t1)
    sqc2 vf5, 0x10(t1)
    sqc2 vf6, 0x20(t1)
    ld ra, 0x0(sp)
    jr ra
    addiu sp, sp, 0x40
}
#endif
/* mtx rotated about z, then x, then y. */
#if PLATFORM_PC
void Mtx_RotateZXY(f32 *mtx, f32 x, f32 y, f32 z)
{
    unsigned int a2 __attribute__((unused)) = 0;
    unsigned int a3 __attribute__((unused)) = 0;
    unsigned int t1 __attribute__((unused)) = 0;
    unsigned int ra __attribute__((unused)) = 0;
    f32 f0 __attribute__((unused)) = 0;
    f32 f1 __attribute__((unused)) = 0;
    f32 f2 __attribute__((unused)) = 0;
    unsigned char stk_[0x40] __attribute__((aligned(16))) = {0};
    unsigned int sp = (unsigned int)(__SIZE_TYPE__)(stk_ + 0x40);
    unsigned int a0 __attribute__((unused)) = (unsigned int)(__SIZE_TYPE__)mtx;
    f32 f12 __attribute__((unused)) = x;
    f32 f13 __attribute__((unused)) = y;
    f32 f14 __attribute__((unused)) = z;
    sp = sp + (-0x40); /* addiu sp, sp, -0x40 */
    t1 = a0 + 0; /* daddu t1, a0, zero */
    f2 = f12; /* mov.s f2, f12 */
    PC_SD(sp, ra); /* sd ra, 0x0(sp) */
    a0 = sp + (0x30); /* addiu a0, sp, 0x30 */
    f12 = f14; /* mov.s f12, f14 */
    Math_SinCos((f32 *)(__SIZE_TYPE__)a0, f12); /* jal Math_SinCos */
    f1 = PC_LWC1(sp + 0x30); /* lwc1 f1, 0x30(sp) */
    f0 = PC_LWC1(sp + 0x34); /* lwc1 f0, 0x34(sp) */
    a2 = PC_F2U(f1); /* mfc1 a2, f1 */
    a3 = PC_F2U(f0); /* mfc1 a3, f0 */
    vu0_lq(4, t1); /* lqc2 vf4, 0x0(t1) */
    vu0_lq(5, t1 + 0x10); /* lqc2 vf5, 0x10(t1) */
    vu0_qmtc2(20, a2); /* qmtc2.ni a2, vf20 */
    vu0_qmtc2(21, a3); /* qmtc2.ni a3, vf21 */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, VU0_Z, 10, 0, 0); /* vadd.z vf10, vf0, vf0 */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_Y, 10, 0, 20); /* vaddx.y vf10, vf0, vf20x */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_X, 10, 0, 21); /* vaddx.x vf10, vf0, vf21x */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, VU0_Z, 11, 0, 0); /* vadd.z vf11, vf0, vf0 */
    vu0_fmac(VU0_SUB, VU0_BX, 0, VU0_X, 11, 0, 20); /* vsubx.x vf11, vf0, vf20x */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_Y, 11, 0, 21); /* vaddx.y vf11, vf0, vf21x */
    vu0_mr32(VU0_XYZ, 12, 0); /* vmr32.xyz vf12, vf0 */
    vu0_lq(6, t1 + 0x20); /* lqc2 vf6, 0x20(t1) */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 4); /* vmulax.xyz ACC, vf10, vf4x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 4); /* vmadday.xyz ACC, vf11, vf4y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 4, 12, 4); /* vmaddz.xyz vf4, vf12, vf4z */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 5); /* vmulax.xyz ACC, vf10, vf5x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 5); /* vmadday.xyz ACC, vf11, vf5y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 5, 12, 5); /* vmaddz.xyz vf5, vf12, vf5z */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 6); /* vmulax.xyz ACC, vf10, vf6x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 6); /* vmadday.xyz ACC, vf11, vf6y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 6, 12, 6); /* vmaddz.xyz vf6, vf12, vf6z */
    vu0_sq(4, t1); /* sqc2 vf4, 0x0(t1) */
    vu0_sq(5, t1 + 0x10); /* sqc2 vf5, 0x10(t1) */
    vu0_sq(6, t1 + 0x20); /* sqc2 vf6, 0x20(t1) */
    f12 = f2; /* mov.s f12, f2 */
    a0 = sp + (0x20); /* addiu a0, sp, 0x20 */
    Math_SinCos((f32 *)(__SIZE_TYPE__)a0, f12); /* jal Math_SinCos */
    f1 = PC_LWC1(sp + 0x20); /* lwc1 f1, 0x20(sp) */
    f0 = PC_LWC1(sp + 0x24); /* lwc1 f0, 0x24(sp) */
    a2 = PC_F2U(f1); /* mfc1 a2, f1 */
    a3 = PC_F2U(f0); /* mfc1 a3, f0 */
    vu0_lq(4, t1); /* lqc2 vf4, 0x0(t1) */
    vu0_lq(5, t1 + 0x10); /* lqc2 vf5, 0x10(t1) */
    vu0_qmtc2(20, a2); /* qmtc2.ni a2, vf20 */
    vu0_qmtc2(21, a3); /* qmtc2.ni a3, vf21 */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, 0x6, 10, 0, 0); /* vadd.yz vf10, vf0, vf0 */
    vu0_fmac(VU0_ADD, VU0_BW, 0, VU0_X, 10, 0, 0); /* vaddw.x vf10, vf0, vf0w */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, VU0_X, 11, 0, 0); /* vadd.x vf11, vf0, vf0 */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_Y, 11, 0, 21); /* vaddx.y vf11, vf0, vf21x */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_Z, 11, 0, 20); /* vaddx.z vf11, vf0, vf20x */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, VU0_X, 12, 0, 0); /* vadd.x vf12, vf0, vf0 */
    vu0_fmac(VU0_SUB, VU0_BX, 0, VU0_Y, 12, 0, 20); /* vsubx.y vf12, vf0, vf20x */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_Z, 12, 0, 21); /* vaddx.z vf12, vf0, vf21x */
    vu0_lq(6, t1 + 0x20); /* lqc2 vf6, 0x20(t1) */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 4); /* vmulax.xyz ACC, vf10, vf4x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 4); /* vmadday.xyz ACC, vf11, vf4y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 4, 12, 4); /* vmaddz.xyz vf4, vf12, vf4z */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 5); /* vmulax.xyz ACC, vf10, vf5x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 5); /* vmadday.xyz ACC, vf11, vf5y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 5, 12, 5); /* vmaddz.xyz vf5, vf12, vf5z */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 6); /* vmulax.xyz ACC, vf10, vf6x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 6); /* vmadday.xyz ACC, vf11, vf6y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 6, 12, 6); /* vmaddz.xyz vf6, vf12, vf6z */
    vu0_sq(4, t1); /* sqc2 vf4, 0x0(t1) */
    vu0_sq(5, t1 + 0x10); /* sqc2 vf5, 0x10(t1) */
    vu0_sq(6, t1 + 0x20); /* sqc2 vf6, 0x20(t1) */
    f12 = f13; /* mov.s f12, f13 */
    a0 = sp + (0x10); /* addiu a0, sp, 0x10 */
    Math_SinCos((f32 *)(__SIZE_TYPE__)a0, f12); /* jal Math_SinCos */
    f1 = PC_LWC1(sp + 0x10); /* lwc1 f1, 0x10(sp) */
    f0 = PC_LWC1(sp + 0x14); /* lwc1 f0, 0x14(sp) */
    a2 = PC_F2U(f1); /* mfc1 a2, f1 */
    a3 = PC_F2U(f0); /* mfc1 a3, f0 */
    vu0_lq(4, t1); /* lqc2 vf4, 0x0(t1) */
    vu0_lq(5, t1 + 0x10); /* lqc2 vf5, 0x10(t1) */
    vu0_qmtc2(20, a2); /* qmtc2.ni a2, vf20 */
    vu0_qmtc2(21, a3); /* qmtc2.ni a3, vf21 */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, 0xA, 11, 0, 0); /* vadd.xz vf11, vf0, vf0 */
    vu0_fmac(VU0_ADD, VU0_BW, 0, VU0_Y, 11, 0, 0); /* vaddw.y vf11, vf0, vf0w */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, VU0_Y, 10, 0, 0); /* vadd.y vf10, vf0, vf0 */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_X, 10, 0, 21); /* vaddx.x vf10, vf0, vf21x */
    vu0_fmac(VU0_SUB, VU0_BX, 0, VU0_Z, 10, 0, 20); /* vsubx.z vf10, vf0, vf20x */
    vu0_fmac(VU0_ADD, VU0_BVEC, 0, VU0_Y, 12, 0, 0); /* vadd.y vf12, vf0, vf0 */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_X, 12, 0, 20); /* vaddx.x vf12, vf0, vf20x */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_Z, 12, 0, 21); /* vaddx.z vf12, vf0, vf21x */
    vu0_lq(6, t1 + 0x20); /* lqc2 vf6, 0x20(t1) */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 4); /* vmulax.xyz ACC, vf10, vf4x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 4); /* vmadday.xyz ACC, vf11, vf4y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 4, 12, 4); /* vmaddz.xyz vf4, vf12, vf4z */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 5); /* vmulax.xyz ACC, vf10, vf5x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 5); /* vmadday.xyz ACC, vf11, vf5y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 5, 12, 5); /* vmaddz.xyz vf5, vf12, vf5z */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 6); /* vmulax.xyz ACC, vf10, vf6x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 11, 6); /* vmadday.xyz ACC, vf11, vf6y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 6, 12, 6); /* vmaddz.xyz vf6, vf12, vf6z */
    vu0_sq(4, t1); /* sqc2 vf4, 0x0(t1) */
    vu0_sq(5, t1 + 0x10); /* sqc2 vf5, 0x10(t1) */
    vu0_sq(6, t1 + 0x20); /* sqc2 vf6, 0x20(t1) */
    ra = (unsigned int)PC_LD(sp); /* ld ra, 0x0(sp) */
    sp = sp + (0x40); /* addiu sp, sp, 0x40 */
    return; /* jr ra */
}
#else
asm void Mtx_RotateZXY(f32 *mtx, f32 x, f32 y, f32 z)
{
    .set noreorder
    addiu sp, sp, -0x40
    daddu t1, a0, zero
    mov.s f2, f12
    sd ra, 0x0(sp)
    addiu a0, sp, 0x30
    jal Math_SinCos
    mov.s f12, f14
    lwc1 f1, 0x30(sp)
    lwc1 f0, 0x34(sp)
    mfc1 a2, f1
    mfc1 a3, f0
    lqc2 vf4, 0x0(t1)
    lqc2 vf5, 0x10(t1)
    qmtc2.ni a2, vf20
    qmtc2.ni a3, vf21
    vadd.z vf10, vf0, vf0
    vaddx.y vf10, vf0, vf20x
    vaddx.x vf10, vf0, vf21x
    vadd.z vf11, vf0, vf0
    vsubx.x vf11, vf0, vf20x
    vaddx.y vf11, vf0, vf21x
    vmr32.xyz vf12, vf0
    lqc2 vf6, 0x20(t1)
    vmulax.xyz ACC, vf10, vf4x
    vmadday.xyz ACC, vf11, vf4y
    vmaddz.xyz vf4, vf12, vf4z
    vmulax.xyz ACC, vf10, vf5x
    vmadday.xyz ACC, vf11, vf5y
    vmaddz.xyz vf5, vf12, vf5z
    vmulax.xyz ACC, vf10, vf6x
    vmadday.xyz ACC, vf11, vf6y
    vmaddz.xyz vf6, vf12, vf6z
    sqc2 vf4, 0x0(t1)
    sqc2 vf5, 0x10(t1)
    sqc2 vf6, 0x20(t1)
    mov.s f12, f2
    jal Math_SinCos
    addiu a0, sp, 0x20
    lwc1 f1, 0x20(sp)
    lwc1 f0, 0x24(sp)
    mfc1 a2, f1
    mfc1 a3, f0
    lqc2 vf4, 0x0(t1)
    lqc2 vf5, 0x10(t1)
    qmtc2.ni a2, vf20
    qmtc2.ni a3, vf21
    vadd.yz vf10, vf0, vf0
    vaddw.x vf10, vf0, vf0w
    vadd.x vf11, vf0, vf0
    vaddx.y vf11, vf0, vf21x
    vaddx.z vf11, vf0, vf20x
    vadd.x vf12, vf0, vf0
    vsubx.y vf12, vf0, vf20x
    vaddx.z vf12, vf0, vf21x
    lqc2 vf6, 0x20(t1)
    vmulax.xyz ACC, vf10, vf4x
    vmadday.xyz ACC, vf11, vf4y
    vmaddz.xyz vf4, vf12, vf4z
    vmulax.xyz ACC, vf10, vf5x
    vmadday.xyz ACC, vf11, vf5y
    vmaddz.xyz vf5, vf12, vf5z
    vmulax.xyz ACC, vf10, vf6x
    vmadday.xyz ACC, vf11, vf6y
    vmaddz.xyz vf6, vf12, vf6z
    sqc2 vf4, 0x0(t1)
    sqc2 vf5, 0x10(t1)
    sqc2 vf6, 0x20(t1)
    mov.s f12, f13
    jal Math_SinCos
    addiu a0, sp, 0x10
    lwc1 f1, 0x10(sp)
    lwc1 f0, 0x14(sp)
    mfc1 a2, f1
    mfc1 a3, f0
    lqc2 vf4, 0x0(t1)
    lqc2 vf5, 0x10(t1)
    qmtc2.ni a2, vf20
    qmtc2.ni a3, vf21
    vadd.xz vf11, vf0, vf0
    vaddw.y vf11, vf0, vf0w
    vadd.y vf10, vf0, vf0
    vaddx.x vf10, vf0, vf21x
    vsubx.z vf10, vf0, vf20x
    vadd.y vf12, vf0, vf0
    vaddx.x vf12, vf0, vf20x
    vaddx.z vf12, vf0, vf21x
    lqc2 vf6, 0x20(t1)
    vmulax.xyz ACC, vf10, vf4x
    vmadday.xyz ACC, vf11, vf4y
    vmaddz.xyz vf4, vf12, vf4z
    vmulax.xyz ACC, vf10, vf5x
    vmadday.xyz ACC, vf11, vf5y
    vmaddz.xyz vf5, vf12, vf5z
    vmulax.xyz ACC, vf10, vf6x
    vmadday.xyz ACC, vf11, vf6y
    vmaddz.xyz vf6, vf12, vf6z
    sqc2 vf4, 0x0(t1)
    sqc2 vf5, 0x10(t1)
    sqc2 vf6, 0x20(t1)
    ld ra, 0x0(sp)
    jr ra
    addiu sp, sp, 0x40
}
#endif

typedef struct Vec3Copy {
    f32 x, y, z;
} Vec3Copy;

/* mtx = the view matrix of a camera at eye looking at target, with up as the up direction. */
void Mtx_LookAt(f32 *mtx, f32 *eye, f32 *target, f32 *up)
{
    f32 side[3];
    f32 upAxis[3];
    f32 back[3];
    Vec3Copy upDir;

    back[0] = eye[0] - target[0];
    back[1] = eye[1] - target[1];
    back[2] = eye[2] - target[2];
    upDir = *(Vec3Copy *)up;
    Vec_Normalize(back);
    Vec_Normalize((f32 *)&upDir);
    Vec_Cross(side, (f32 *)&upDir, back);
    Vec_Normalize(side);
    Vec_Cross(upAxis, back, side);
    mtx[3] = 0.0f;
    mtx[7] = 0.0f;
    mtx[11] = 0.0f;
    mtx[15] = 1.0f;
    mtx[0] = side[0];
    mtx[4] = side[1];
    mtx[8] = side[2];
    mtx[1] = upAxis[0];
    mtx[5] = upAxis[1];
    mtx[9] = upAxis[2];
    mtx[2] = back[0];
    mtx[6] = back[1];
    mtx[10] = back[2];
    mtx[12] = -Vec_Dot(eye, side);
    mtx[13] = -Vec_Dot(eye, upAxis);
    mtx[14] = -Vec_Dot(eye, back);
}
/* Transposes the 3x3 part of mtx. */
#if PLATFORM_PC
void Mtx_Transpose3x3(f32 *mtx)
{
    unsigned int a0 __attribute__((unused)) = (unsigned int)(__SIZE_TYPE__)mtx;
    vu0_lq(8, a0); /* lqc2 vf8, 0x0(a0) */
    vu0_lq(9, a0 + 0x10); /* lqc2 vf9, 0x10(a0) */
    vu0_lq(10, a0 + 0x20); /* lqc2 vf10, 0x20(a0) */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_X, 4, 0, 8); /* vaddx.x vf4, vf0, vf8x */
    vu0_fmac(VU0_ADD, VU0_BY, 0, VU0_X, 5, 0, 8); /* vaddy.x vf5, vf0, vf8y */
    vu0_fmac(VU0_ADD, VU0_BZ, 0, VU0_X, 6, 0, 8); /* vaddz.x vf6, vf0, vf8z */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_Y, 4, 0, 9); /* vaddx.y vf4, vf0, vf9x */
    vu0_fmac(VU0_ADD, VU0_BY, 0, VU0_Y, 5, 0, 9); /* vaddy.y vf5, vf0, vf9y */
    vu0_fmac(VU0_ADD, VU0_BZ, 0, VU0_Y, 6, 0, 9); /* vaddz.y vf6, vf0, vf9z */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_Z, 4, 0, 10); /* vaddx.z vf4, vf0, vf10x */
    vu0_fmac(VU0_ADD, VU0_BY, 0, VU0_Z, 5, 0, 10); /* vaddy.z vf5, vf0, vf10y */
    vu0_fmac(VU0_ADD, VU0_BZ, 0, VU0_Z, 6, 0, 10); /* vaddz.z vf6, vf0, vf10z */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_W, 4, 8, 0); /* vaddx.w vf4, vf8, vf0x */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_W, 5, 9, 0); /* vaddx.w vf5, vf9, vf0x */
    vu0_fmac(VU0_ADD, VU0_BX, 0, VU0_W, 6, 10, 0); /* vaddx.w vf6, vf10, vf0x */
    vu0_sq(4, a0); /* sqc2 vf4, 0x0(a0) */
    vu0_sq(5, a0 + 0x10); /* sqc2 vf5, 0x10(a0) */
    vu0_sq(6, a0 + 0x20); /* sqc2 vf6, 0x20(a0) */
    return; /* jr ra */
}
#else
asm void Mtx_Transpose3x3(f32 *mtx)
{
    .set noreorder
    lqc2 vf8, 0x0(a0)
    lqc2 vf9, 0x10(a0)
    lqc2 vf10, 0x20(a0)
    vaddx.x vf4, vf0, vf8x
    vaddy.x vf5, vf0, vf8y
    vaddz.x vf6, vf0, vf8z
    vaddx.y vf4, vf0, vf9x
    vaddy.y vf5, vf0, vf9y
    vaddz.y vf6, vf0, vf9z
    vaddx.z vf4, vf0, vf10x
    vaddy.z vf5, vf0, vf10y
    vaddz.z vf6, vf0, vf10z
    vaddx.w vf4, vf8, vf0x
    vaddx.w vf5, vf9, vf0x
    vaddx.w vf6, vf10, vf0x
    sqc2 vf4, 0x0(a0)
    sqc2 vf5, 0x10(a0)
    jr ra
    sqc2 vf6, 0x20(a0)
}
#endif
/* dst = lhs * rhs (4x4, row vectors). */
#if PLATFORM_PC
void Mtx_Mul(f32 *dst, f32 *lhs, f32 *rhs)
{
    unsigned int a0 __attribute__((unused)) = (unsigned int)(__SIZE_TYPE__)dst;
    unsigned int a1 __attribute__((unused)) = (unsigned int)(__SIZE_TYPE__)lhs;
    unsigned int a2 __attribute__((unused)) = (unsigned int)(__SIZE_TYPE__)rhs;
    vu0_lq(4, a2); /* lqc2 vf4, 0x0(a2) */
    vu0_lq(5, a2 + 0x10); /* lqc2 vf5, 0x10(a2) */
    vu0_lq(6, a2 + 0x20); /* lqc2 vf6, 0x20(a2) */
    vu0_lq(7, a2 + 0x30); /* lqc2 vf7, 0x30(a2) */
    vu0_lq(8, a1); /* lqc2 vf8, 0x0(a1) */
    vu0_lq(9, a1 + 0x10); /* lqc2 vf9, 0x10(a1) */
    vu0_lq(10, a1 + 0x20); /* lqc2 vf10, 0x20(a1) */
    vu0_lq(11, a1 + 0x30); /* lqc2 vf11, 0x30(a1) */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZW, 0, 4, 8); /* vmulax.xyzw ACC, vf4, vf8x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZW, 0, 5, 8); /* vmadday.xyzw ACC, vf5, vf8y */
    vu0_fmac(VU0_MADD, VU0_BZ, 1, VU0_XYZW, 0, 6, 8); /* vmaddaz.xyzw ACC, vf6, vf8z */
    vu0_fmac(VU0_MADD, VU0_BW, 0, VU0_XYZW, 8, 7, 8); /* vmaddw.xyzw vf8, vf7, vf8w */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZW, 0, 4, 9); /* vmulax.xyzw ACC, vf4, vf9x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZW, 0, 5, 9); /* vmadday.xyzw ACC, vf5, vf9y */
    vu0_fmac(VU0_MADD, VU0_BZ, 1, VU0_XYZW, 0, 6, 9); /* vmaddaz.xyzw ACC, vf6, vf9z */
    vu0_fmac(VU0_MADD, VU0_BW, 0, VU0_XYZW, 9, 7, 9); /* vmaddw.xyzw vf9, vf7, vf9w */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZW, 0, 4, 10); /* vmulax.xyzw ACC, vf4, vf10x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZW, 0, 5, 10); /* vmadday.xyzw ACC, vf5, vf10y */
    vu0_fmac(VU0_MADD, VU0_BZ, 1, VU0_XYZW, 0, 6, 10); /* vmaddaz.xyzw ACC, vf6, vf10z */
    vu0_fmac(VU0_MADD, VU0_BW, 0, VU0_XYZW, 10, 7, 10); /* vmaddw.xyzw vf10, vf7, vf10w */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZW, 0, 4, 11); /* vmulax.xyzw ACC, vf4, vf11x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZW, 0, 5, 11); /* vmadday.xyzw ACC, vf5, vf11y */
    vu0_fmac(VU0_MADD, VU0_BZ, 1, VU0_XYZW, 0, 6, 11); /* vmaddaz.xyzw ACC, vf6, vf11z */
    vu0_fmac(VU0_MADD, VU0_BW, 0, VU0_XYZW, 11, 7, 11); /* vmaddw.xyzw vf11, vf7, vf11w */
    vu0_sq(8, a0); /* sqc2 vf8, 0x0(a0) */
    vu0_sq(9, a0 + 0x10); /* sqc2 vf9, 0x10(a0) */
    vu0_sq(10, a0 + 0x20); /* sqc2 vf10, 0x20(a0) */
    vu0_sq(11, a0 + 0x30); /* sqc2 vf11, 0x30(a0) */
    /* nop */
    return; /* jr ra */
}
#else
asm void Mtx_Mul(f32 *dst, f32 *lhs, f32 *rhs)
{
    .set noreorder
    lqc2 vf4, 0x0(a2)
    lqc2 vf5, 0x10(a2)
    lqc2 vf6, 0x20(a2)
    lqc2 vf7, 0x30(a2)
    lqc2 vf8, 0x0(a1)
    lqc2 vf9, 0x10(a1)
    lqc2 vf10, 0x20(a1)
    lqc2 vf11, 0x30(a1)
    vmulax.xyzw ACC, vf4, vf8x
    vmadday.xyzw ACC, vf5, vf8y
    vmaddaz.xyzw ACC, vf6, vf8z
    vmaddw.xyzw vf8, vf7, vf8w
    vmulax.xyzw ACC, vf4, vf9x
    vmadday.xyzw ACC, vf5, vf9y
    vmaddaz.xyzw ACC, vf6, vf9z
    vmaddw.xyzw vf9, vf7, vf9w
    vmulax.xyzw ACC, vf4, vf10x
    vmadday.xyzw ACC, vf5, vf10y
    vmaddaz.xyzw ACC, vf6, vf10z
    vmaddw.xyzw vf10, vf7, vf10w
    vmulax.xyzw ACC, vf4, vf11x
    vmadday.xyzw ACC, vf5, vf11y
    vmaddaz.xyzw ACC, vf6, vf11z
    vmaddw.xyzw vf11, vf7, vf11w
    sqc2 vf8, 0x0(a0)
    sqc2 vf9, 0x10(a0)
    sqc2 vf10, 0x20(a0)
    sqc2 vf11, 0x30(a0)
    jr ra
    nop
}
#endif
/* mtx = mtx * other (4x4). */
#if PLATFORM_PC
void Mtx_Concat(f32 *mtx, f32 *other)
{
    unsigned int a0 __attribute__((unused)) = (unsigned int)(__SIZE_TYPE__)mtx;
    unsigned int a1 __attribute__((unused)) = (unsigned int)(__SIZE_TYPE__)other;
    vu0_lq(4, a1); /* lqc2 vf4, 0x0(a1) */
    vu0_lq(5, a1 + 0x10); /* lqc2 vf5, 0x10(a1) */
    vu0_lq(6, a1 + 0x20); /* lqc2 vf6, 0x20(a1) */
    vu0_lq(7, a1 + 0x30); /* lqc2 vf7, 0x30(a1) */
    vu0_lq(8, a0); /* lqc2 vf8, 0x0(a0) */
    vu0_lq(9, a0 + 0x10); /* lqc2 vf9, 0x10(a0) */
    vu0_lq(10, a0 + 0x20); /* lqc2 vf10, 0x20(a0) */
    vu0_lq(11, a0 + 0x30); /* lqc2 vf11, 0x30(a0) */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZW, 0, 4, 8); /* vmulax.xyzw ACC, vf4, vf8x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZW, 0, 5, 8); /* vmadday.xyzw ACC, vf5, vf8y */
    vu0_fmac(VU0_MADD, VU0_BZ, 1, VU0_XYZW, 0, 6, 8); /* vmaddaz.xyzw ACC, vf6, vf8z */
    vu0_fmac(VU0_MADD, VU0_BW, 0, VU0_XYZW, 8, 7, 8); /* vmaddw.xyzw vf8, vf7, vf8w */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZW, 0, 4, 9); /* vmulax.xyzw ACC, vf4, vf9x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZW, 0, 5, 9); /* vmadday.xyzw ACC, vf5, vf9y */
    vu0_fmac(VU0_MADD, VU0_BZ, 1, VU0_XYZW, 0, 6, 9); /* vmaddaz.xyzw ACC, vf6, vf9z */
    vu0_fmac(VU0_MADD, VU0_BW, 0, VU0_XYZW, 9, 7, 9); /* vmaddw.xyzw vf9, vf7, vf9w */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZW, 0, 4, 10); /* vmulax.xyzw ACC, vf4, vf10x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZW, 0, 5, 10); /* vmadday.xyzw ACC, vf5, vf10y */
    vu0_fmac(VU0_MADD, VU0_BZ, 1, VU0_XYZW, 0, 6, 10); /* vmaddaz.xyzw ACC, vf6, vf10z */
    vu0_fmac(VU0_MADD, VU0_BW, 0, VU0_XYZW, 10, 7, 10); /* vmaddw.xyzw vf10, vf7, vf10w */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZW, 0, 4, 11); /* vmulax.xyzw ACC, vf4, vf11x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZW, 0, 5, 11); /* vmadday.xyzw ACC, vf5, vf11y */
    vu0_fmac(VU0_MADD, VU0_BZ, 1, VU0_XYZW, 0, 6, 11); /* vmaddaz.xyzw ACC, vf6, vf11z */
    vu0_fmac(VU0_MADD, VU0_BW, 0, VU0_XYZW, 11, 7, 11); /* vmaddw.xyzw vf11, vf7, vf11w */
    vu0_sq(8, a0); /* sqc2 vf8, 0x0(a0) */
    vu0_sq(9, a0 + 0x10); /* sqc2 vf9, 0x10(a0) */
    vu0_sq(10, a0 + 0x20); /* sqc2 vf10, 0x20(a0) */
    vu0_sq(11, a0 + 0x30); /* sqc2 vf11, 0x30(a0) */
    /* nop */
    return; /* jr ra */
}
#else
asm void Mtx_Concat(f32 *mtx, f32 *other)
{
    .set noreorder
    lqc2 vf4, 0x0(a1)
    lqc2 vf5, 0x10(a1)
    lqc2 vf6, 0x20(a1)
    lqc2 vf7, 0x30(a1)
    lqc2 vf8, 0x0(a0)
    lqc2 vf9, 0x10(a0)
    lqc2 vf10, 0x20(a0)
    lqc2 vf11, 0x30(a0)
    vmulax.xyzw ACC, vf4, vf8x
    vmadday.xyzw ACC, vf5, vf8y
    vmaddaz.xyzw ACC, vf6, vf8z
    vmaddw.xyzw vf8, vf7, vf8w
    vmulax.xyzw ACC, vf4, vf9x
    vmadday.xyzw ACC, vf5, vf9y
    vmaddaz.xyzw ACC, vf6, vf9z
    vmaddw.xyzw vf9, vf7, vf9w
    vmulax.xyzw ACC, vf4, vf10x
    vmadday.xyzw ACC, vf5, vf10y
    vmaddaz.xyzw ACC, vf6, vf10z
    vmaddw.xyzw vf10, vf7, vf10w
    vmulax.xyzw ACC, vf4, vf11x
    vmadday.xyzw ACC, vf5, vf11y
    vmaddaz.xyzw ACC, vf6, vf11z
    vmaddw.xyzw vf11, vf7, vf11w
    sqc2 vf8, 0x0(a0)
    sqc2 vf9, 0x10(a0)
    sqc2 vf10, 0x20(a0)
    sqc2 vf11, 0x30(a0)
    jr ra
    nop
}
#endif
/* The 3x3 part of dst = lhs * rhs (3x3); the rest of dst stays. */
#if PLATFORM_PC
void Mtx_Mul3x3(f32 *dst, f32 *lhs, f32 *rhs)
{
    unsigned int a0 __attribute__((unused)) = (unsigned int)(__SIZE_TYPE__)dst;
    unsigned int a1 __attribute__((unused)) = (unsigned int)(__SIZE_TYPE__)lhs;
    unsigned int a2 __attribute__((unused)) = (unsigned int)(__SIZE_TYPE__)rhs;
    vu0_lq(4, a2); /* lqc2 vf4, 0x0(a2) */
    vu0_lq(5, a2 + 0x10); /* lqc2 vf5, 0x10(a2) */
    vu0_lq(6, a2 + 0x20); /* lqc2 vf6, 0x20(a2) */
    vu0_lq(8, a1); /* lqc2 vf8, 0x0(a1) */
    vu0_lq(9, a1 + 0x10); /* lqc2 vf9, 0x10(a1) */
    vu0_lq(10, a1 + 0x20); /* lqc2 vf10, 0x20(a1) */
    vu0_lq(11, a0); /* lqc2 vf11, 0x0(a0) */
    vu0_lq(12, a0 + 0x10); /* lqc2 vf12, 0x10(a0) */
    vu0_lq(13, a0 + 0x20); /* lqc2 vf13, 0x20(a0) */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 4, 8); /* vmulax.xyz ACC, vf4, vf8x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 5, 8); /* vmadday.xyz ACC, vf5, vf8y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 11, 6, 8); /* vmaddz.xyz vf11, vf6, vf8z */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 4, 9); /* vmulax.xyz ACC, vf4, vf9x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 5, 9); /* vmadday.xyz ACC, vf5, vf9y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 12, 6, 9); /* vmaddz.xyz vf12, vf6, vf9z */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 4, 10); /* vmulax.xyz ACC, vf4, vf10x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 5, 10); /* vmadday.xyz ACC, vf5, vf10y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 13, 6, 10); /* vmaddz.xyz vf13, vf6, vf10z */
    vu0_sq(11, a0); /* sqc2 vf11, 0x0(a0) */
    vu0_sq(12, a0 + 0x10); /* sqc2 vf12, 0x10(a0) */
    vu0_sq(13, a0 + 0x20); /* sqc2 vf13, 0x20(a0) */
    /* nop */
    return; /* jr ra */
}
#else
asm void Mtx_Mul3x3(f32 *dst, f32 *lhs, f32 *rhs)
{
    .set noreorder
    lqc2 vf4, 0x0(a2)
    lqc2 vf5, 0x10(a2)
    lqc2 vf6, 0x20(a2)
    lqc2 vf8, 0x0(a1)
    lqc2 vf9, 0x10(a1)
    lqc2 vf10, 0x20(a1)
    lqc2 vf11, 0x0(a0)
    lqc2 vf12, 0x10(a0)
    lqc2 vf13, 0x20(a0)
    vmulax.xyz ACC, vf4, vf8x
    vmadday.xyz ACC, vf5, vf8y
    vmaddz.xyz vf11, vf6, vf8z
    vmulax.xyz ACC, vf4, vf9x
    vmadday.xyz ACC, vf5, vf9y
    vmaddz.xyz vf12, vf6, vf9z
    vmulax.xyz ACC, vf4, vf10x
    vmadday.xyz ACC, vf5, vf10y
    vmaddz.xyz vf13, vf6, vf10z
    sqc2 vf11, 0x0(a0)
    sqc2 vf12, 0x10(a0)
    sqc2 vf13, 0x20(a0)
    jr ra
    nop
}
#endif
/* The 3x3 part of mtx = mtx * other (3x3). */
#if PLATFORM_PC
void Mtx_Concat3x3(f32 *mtx, f32 *other)
{
    unsigned int a0 __attribute__((unused)) = (unsigned int)(__SIZE_TYPE__)mtx;
    unsigned int a1 __attribute__((unused)) = (unsigned int)(__SIZE_TYPE__)other;
    vu0_lq(4, a1); /* lqc2 vf4, 0x0(a1) */
    vu0_lq(5, a1 + 0x10); /* lqc2 vf5, 0x10(a1) */
    vu0_lq(6, a1 + 0x20); /* lqc2 vf6, 0x20(a1) */
    vu0_lq(8, a0); /* lqc2 vf8, 0x0(a0) */
    vu0_lq(9, a0 + 0x10); /* lqc2 vf9, 0x10(a0) */
    vu0_lq(10, a0 + 0x20); /* lqc2 vf10, 0x20(a0) */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 4, 8); /* vmulax.xyz ACC, vf4, vf8x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 5, 8); /* vmadday.xyz ACC, vf5, vf8y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 8, 6, 8); /* vmaddz.xyz vf8, vf6, vf8z */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 4, 9); /* vmulax.xyz ACC, vf4, vf9x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 5, 9); /* vmadday.xyz ACC, vf5, vf9y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 9, 6, 9); /* vmaddz.xyz vf9, vf6, vf9z */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 4, 10); /* vmulax.xyz ACC, vf4, vf10x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 5, 10); /* vmadday.xyz ACC, vf5, vf10y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 10, 6, 10); /* vmaddz.xyz vf10, vf6, vf10z */
    vu0_sq(8, a0); /* sqc2 vf8, 0x0(a0) */
    vu0_sq(9, a0 + 0x10); /* sqc2 vf9, 0x10(a0) */
    vu0_sq(10, a0 + 0x20); /* sqc2 vf10, 0x20(a0) */
    /* nop */
    return; /* jr ra */
}
#else
asm void Mtx_Concat3x3(f32 *mtx, f32 *other)
{
    .set noreorder
    lqc2 vf4, 0x0(a1)
    lqc2 vf5, 0x10(a1)
    lqc2 vf6, 0x20(a1)
    lqc2 vf8, 0x0(a0)
    lqc2 vf9, 0x10(a0)
    lqc2 vf10, 0x20(a0)
    vmulax.xyz ACC, vf4, vf8x
    vmadday.xyz ACC, vf5, vf8y
    vmaddz.xyz vf8, vf6, vf8z
    vmulax.xyz ACC, vf4, vf9x
    vmadday.xyz ACC, vf5, vf9y
    vmaddz.xyz vf9, vf6, vf9z
    vmulax.xyz ACC, vf4, vf10x
    vmadday.xyz ACC, vf5, vf10y
    vmaddz.xyz vf10, vf6, vf10z
    sqc2 vf8, 0x0(a0)
    sqc2 vf9, 0x10(a0)
    sqc2 vf10, 0x20(a0)
    jr ra
    nop
}
#endif
/* Inverse of a rotation, scale and translation matrix (transpose / scale^2). */
#if PLATFORM_PC
void Mtx_Inverse(f32 *dst, f32 *src)
{
    unsigned int v1 __attribute__((unused)) = 0;
    unsigned int a0 __attribute__((unused)) = (unsigned int)(__SIZE_TYPE__)dst;
    unsigned int a1 __attribute__((unused)) = (unsigned int)(__SIZE_TYPE__)src;
    v1 = (unsigned int)(__SIZE_TYPE__)gMtxIdentity; /* la v1, gMtxIdentity */
    vu0_lq(8, a1); /* lqc2 vf8, 0x0(a1) */
    vu0_lq(9, a1 + 0x10); /* lqc2 vf9, 0x10(a1) */
    vu0_lq(10, a1 + 0x20); /* lqc2 vf10, 0x20(a1) */
    vu0_lq(11, a1 + 0x30); /* lqc2 vf11, 0x30(a1) */
    vu0_fmac(VU0_MUL, VU0_BVEC, 0, VU0_XYZ, 1, 8, 8); /* vmul.xyz vf1, vf8, vf8 */
    vu0_fmac(VU0_ADD, VU0_BY, 0, VU0_X, 1, 1, 1); /* vaddy.x vf1, vf1, vf1y */
    vu0_fmac(VU0_ADD, VU0_BZ, 0, VU0_X, 1, 1, 1); /* vaddz.x vf1, vf1, vf1z */
    vu0_vrsqrt(0, 3, 1, 0); /* vrsqrt Q, vf0w, vf1x */
    vu0_lq(4, v1); /* lqc2 vf4, 0x0(v1) */
    vu0_lq(5, v1 + 0x10); /* lqc2 vf5, 0x10(v1) */
    vu0_lq(6, v1 + 0x20); /* lqc2 vf6, 0x20(v1) */
    vu0_lq(7, v1 + 0x30); /* lqc2 vf7, 0x30(v1) */
    vu0_fmac(VU0_MUL, VU0_BVEC, 0, VU0_XYZ, 2, 9, 9); /* vmul.xyz vf2, vf9, vf9 */
    vu0_fmac(VU0_ADD, VU0_BY, 0, VU0_X, 2, 2, 2); /* vaddy.x vf2, vf2, vf2y */
    vu0_fmac(VU0_ADD, VU0_BZ, 0, VU0_X, 2, 2, 2); /* vaddz.x vf2, vf2, vf2z */
    vu0_fmac(VU0_MUL, VU0_BVEC, 0, VU0_XYZ, 3, 10, 10); /* vmul.xyz vf3, vf10, vf10 */
    vu0_fmac(VU0_ADD, VU0_BY, 0, VU0_X, 3, 3, 3); /* vaddy.x vf3, vf3, vf3y */
    vu0_fmac(VU0_ADD, VU0_BZ, 0, VU0_X, 3, 3, 3); /* vaddz.x vf3, vf3, vf3z */
    vu0_fmac(VU0_SUB, VU0_BVEC, 0, VU0_XYZ, 21, 0, 11); /* vsub.xyz vf21, vf0, vf11 */
    /* vwaitq */
    vu0_fmac(VU0_ADD, VU0_BQ, 0, VU0_X, 20, 0, 0); /* vaddq.x vf20, vf0, Q */
    vu0_vrsqrt(0, 3, 2, 0); /* vrsqrt Q, vf0w, vf2x */
    vu0_fmac(VU0_MUL, VU0_BX, 0, VU0_X, 4, 20, 8); /* vmulx.x vf4, vf20, vf8x */
    vu0_fmac(VU0_MUL, VU0_BY, 0, VU0_X, 5, 20, 8); /* vmuly.x vf5, vf20, vf8y */
    vu0_fmac(VU0_MUL, VU0_BZ, 0, VU0_X, 6, 20, 8); /* vmulz.x vf6, vf20, vf8z */
    /* vwaitq */
    vu0_fmac(VU0_ADD, VU0_BQ, 0, VU0_Y, 20, 0, 0); /* vaddq.y vf20, vf0, Q */
    vu0_vrsqrt(0, 3, 3, 0); /* vrsqrt Q, vf0w, vf3x */
    vu0_fmac(VU0_MUL, VU0_BX, 0, VU0_Y, 4, 20, 9); /* vmulx.y vf4, vf20, vf9x */
    vu0_fmac(VU0_MUL, VU0_BY, 0, VU0_Y, 5, 20, 9); /* vmuly.y vf5, vf20, vf9y */
    vu0_fmac(VU0_MUL, VU0_BZ, 0, VU0_Y, 6, 20, 9); /* vmulz.y vf6, vf20, vf9z */
    /* vwaitq */
    vu0_fmac(VU0_ADD, VU0_BQ, 0, VU0_Z, 20, 0, 0); /* vaddq.z vf20, vf0, Q */
    vu0_fmac(VU0_MUL, VU0_BX, 0, VU0_Z, 4, 20, 10); /* vmulx.z vf4, vf20, vf10x */
    vu0_fmac(VU0_MUL, VU0_BY, 0, VU0_Z, 5, 20, 10); /* vmuly.z vf5, vf20, vf10y */
    vu0_fmac(VU0_MUL, VU0_BZ, 0, VU0_Z, 6, 20, 10); /* vmulz.z vf6, vf20, vf10z */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 4, 21); /* vmulax.xyz ACC, vf4, vf21x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 5, 21); /* vmadday.xyz ACC, vf5, vf21y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 7, 6, 21); /* vmaddz.xyz vf7, vf6, vf21z */
    vu0_fmac(VU0_MUL, VU0_BVEC, 0, VU0_XYZ, 4, 4, 20); /* vmul.xyz vf4, vf4, vf20 */
    vu0_fmac(VU0_MUL, VU0_BVEC, 0, VU0_XYZ, 5, 5, 20); /* vmul.xyz vf5, vf5, vf20 */
    vu0_fmac(VU0_MUL, VU0_BVEC, 0, VU0_XYZ, 6, 6, 20); /* vmul.xyz vf6, vf6, vf20 */
    vu0_fmac(VU0_MUL, VU0_BVEC, 0, VU0_XYZ, 7, 7, 20); /* vmul.xyz vf7, vf7, vf20 */
    vu0_sq(4, a0); /* sqc2 vf4, 0x0(a0) */
    vu0_sq(5, a0 + 0x10); /* sqc2 vf5, 0x10(a0) */
    vu0_sq(6, a0 + 0x20); /* sqc2 vf6, 0x20(a0) */
    vu0_sq(7, a0 + 0x30); /* sqc2 vf7, 0x30(a0) */
    return; /* jr ra */
}
#else
asm void Mtx_Inverse(f32 *dst, f32 *src)
{
    .set noreorder
    la v1, gMtxIdentity
    lqc2 vf8, 0x0(a1)
    lqc2 vf9, 0x10(a1)
    lqc2 vf10, 0x20(a1)
    lqc2 vf11, 0x30(a1)
    vmul.xyz vf1, vf8, vf8
    vaddy.x vf1, vf1, vf1y
    vaddz.x vf1, vf1, vf1z
    vrsqrt Q, vf0w, vf1x
    lqc2 vf4, 0x0(v1)
    lqc2 vf5, 0x10(v1)
    lqc2 vf6, 0x20(v1)
    lqc2 vf7, 0x30(v1)
    vmul.xyz vf2, vf9, vf9
    vaddy.x vf2, vf2, vf2y
    vaddz.x vf2, vf2, vf2z
    vmul.xyz vf3, vf10, vf10
    vaddy.x vf3, vf3, vf3y
    vaddz.x vf3, vf3, vf3z
    vsub.xyz vf21, vf0, vf11
    vwaitq
    vaddq.x vf20, vf0, Q
    vrsqrt Q, vf0w, vf2x
    vmulx.x vf4, vf20, vf8x
    vmuly.x vf5, vf20, vf8y
    vmulz.x vf6, vf20, vf8z
    vwaitq
    vaddq.y vf20, vf0, Q
    vrsqrt Q, vf0w, vf3x
    vmulx.y vf4, vf20, vf9x
    vmuly.y vf5, vf20, vf9y
    vmulz.y vf6, vf20, vf9z
    vwaitq
    vaddq.z vf20, vf0, Q
    vmulx.z vf4, vf20, vf10x
    vmuly.z vf5, vf20, vf10y
    vmulz.z vf6, vf20, vf10z
    vmulax.xyz ACC, vf4, vf21x
    vmadday.xyz ACC, vf5, vf21y
    vmaddz.xyz vf7, vf6, vf21z
    vmul.xyz vf4, vf4, vf20
    vmul.xyz vf5, vf5, vf20
    vmul.xyz vf6, vf6, vf20
    vmul.xyz vf7, vf7, vf20
    sqc2 vf4, 0x0(a0)
    sqc2 vf5, 0x10(a0)
    sqc2 vf6, 0x20(a0)
    jr ra
    sqc2 vf7, 0x30(a0)
}
#endif

/* dst = src (a 4x4 matrix, 16-byte aligned; hand-written, with lq/sq). */
#if PLATFORM_PC
void Mtx_Copy(f32 *dst, f32 *src)
{
    unsigned int a2_q[4] __attribute__((unused));
    unsigned int a3_q[4] __attribute__((unused));
    unsigned int t0_q[4] __attribute__((unused));
    unsigned int t1_q[4] __attribute__((unused));
    unsigned int a0 __attribute__((unused)) = (unsigned int)(__SIZE_TYPE__)dst;
    unsigned int a1 __attribute__((unused)) = (unsigned int)(__SIZE_TYPE__)src;
    PC_LQ(a2_q, a1); /* lq a2, 0x0(a1) */
    PC_LQ(a3_q, a1 + 0x10); /* lq a3, 0x10(a1) */
    PC_LQ(t0_q, a1 + 0x20); /* lq t0, 0x20(a1) */
    PC_LQ(t1_q, a1 + 0x30); /* lq t1, 0x30(a1) */
    PC_SQ(a0, a2_q); /* sq a2, 0x0(a0) */
    PC_SQ(a0 + 0x10, a3_q); /* sq a3, 0x10(a0) */
    PC_SQ(a0 + 0x20, t0_q); /* sq t0, 0x20(a0) */
    PC_SQ(a0 + 0x30, t1_q); /* sq t1, 0x30(a0) */
    return; /* jr ra */
}
#else
asm void Mtx_Copy(f32 *dst, f32 *src)
{
    .set noreorder
    lq a2, 0x0(a1)
    lq a3, 0x10(a1)
    lq t0, 0x20(a1)
    lq t1, 0x30(a1)
    sq a2, 0x0(a0)
    sq a3, 0x10(a0)
    sq t0, 0x20(a0)
    jr ra
    sq t1, 0x30(a0)
}
#endif

/* The 3x3 part of mtx = src's. */
void Mtx_CopyRot(f32 *mtx, f32 *src)
{
    mtx[0] = src[0];
    mtx[1] = src[1];
    mtx[2] = src[2];
    mtx[4] = src[4];
    mtx[5] = src[5];
    mtx[6] = src[6];
    mtx[8] = src[8];
    mtx[9] = src[9];
    mtx[10] = src[10];
}
/* dst = first * weightFirst + second * weightSecond, the rotation made orthonormal again
 * (rows 0 and 1, then their cross products). */
#if PLATFORM_PC
void Mtx_Blend(f32 *dst, f32 *first, f32 *second, f32 weightFirst, f32 weightSecond)
{
    unsigned int t0 __attribute__((unused)) = 0;
    unsigned int a0 __attribute__((unused)) = (unsigned int)(__SIZE_TYPE__)dst;
    unsigned int a1 __attribute__((unused)) = (unsigned int)(__SIZE_TYPE__)first;
    unsigned int a2 __attribute__((unused)) = (unsigned int)(__SIZE_TYPE__)second;
    f32 f12 __attribute__((unused)) = weightFirst;
    f32 f13 __attribute__((unused)) = weightSecond;
    vu0_mr32(VU0_XYZW, 6, 0); /* vmr32.xyzw vf6, vf0 */
    vu0_move(VU0_XYZW, 7, 0); /* vmove.xyzw vf7, vf0 */
    vu0_mr32(VU0_XYZW, 5, 6); /* vmr32.xyzw vf5, vf6 */
    vu0_mr32(VU0_XYZW, 4, 5); /* vmr32.xyzw vf4, vf5 */
    t0 = PC_F2U(f12); /* mfc1 t0, f12 */
    /* nop */
    vu0_qmtc2(8, t0); /* qmtc2.ni t0, vf8 */
    t0 = PC_F2U(f13); /* mfc1 t0, f13 */
    /* nop */
    vu0_qmtc2(9, t0); /* qmtc2.ni t0, vf9 */
    vu0_lq(10, a1); /* lqc2 vf10, 0x0(a1) */
    vu0_lq(11, a2); /* lqc2 vf11, 0x0(a2) */
    vu0_lq(12, a1 + 0x10); /* lqc2 vf12, 0x10(a1) */
    vu0_lq(13, a2 + 0x10); /* lqc2 vf13, 0x10(a2) */
    vu0_lq(14, a1 + 0x30); /* lqc2 vf14, 0x30(a1) */
    vu0_lq(15, a2 + 0x30); /* lqc2 vf15, 0x30(a2) */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 10, 8); /* vmulax.xyz ACC, vf10, vf8x */
    vu0_fmac(VU0_MADD, VU0_BX, 0, VU0_XYZ, 10, 11, 9); /* vmaddx.xyz vf10, vf11, vf9x */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 12, 8); /* vmulax.xyz ACC, vf12, vf8x */
    vu0_fmac(VU0_MADD, VU0_BX, 0, VU0_XYZ, 12, 13, 9); /* vmaddx.xyz vf12, vf13, vf9x */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 14, 8); /* vmulax.xyz ACC, vf14, vf8x */
    vu0_fmac(VU0_MADD, VU0_BX, 0, VU0_XYZ, 7, 15, 9); /* vmaddx.xyz vf7, vf15, vf9x */
    vu0_fmac(VU0_MUL, VU0_BVEC, 0, VU0_XYZ, 4, 10, 10); /* vmul.xyz vf4, vf10, vf10 */
    vu0_fmac(VU0_ADD, VU0_BY, 0, VU0_X, 4, 4, 4); /* vaddy.x vf4, vf4, vf4y */
    vu0_fmac(VU0_ADD, VU0_BZ, 0, VU0_X, 4, 4, 4); /* vaddz.x vf4, vf4, vf4z */
    vu0_vrsqrt(0, 3, 4, 0); /* vrsqrt Q, vf0w, vf4x */
    /* vwaitq */
    vu0_fmac(VU0_MUL, VU0_BQ, 0, VU0_XYZ, 4, 10, 0); /* vmulq.xyz vf4, vf10, Q */
    vu0_fmac(VU0_MUL, VU0_BVEC, 0, VU0_XYZ, 5, 12, 12); /* vmul.xyz vf5, vf12, vf12 */
    vu0_fmac(VU0_ADD, VU0_BY, 0, VU0_X, 5, 5, 5); /* vaddy.x vf5, vf5, vf5y */
    vu0_fmac(VU0_ADD, VU0_BZ, 0, VU0_X, 5, 5, 5); /* vaddz.x vf5, vf5, vf5z */
    vu0_vrsqrt(0, 3, 5, 0); /* vrsqrt Q, vf0w, vf5x */
    /* vwaitq */
    vu0_fmac(VU0_MUL, VU0_BQ, 0, VU0_XYZ, 5, 12, 0); /* vmulq.xyz vf5, vf12, Q */
    vu0_opmula(0, 0, 4, 5); /* vopmula.xyz ACC, vf4, vf5 */
    vu0_opmula(1, 6, 5, 4); /* vopmsub.xyz vf6, vf5, vf4 */
    vu0_fmac(VU0_MUL, VU0_BVEC, 0, VU0_XYZ, 8, 6, 6); /* vmul.xyz vf8, vf6, vf6 */
    vu0_fmac(VU0_ADD, VU0_BY, 0, VU0_X, 8, 8, 8); /* vaddy.x vf8, vf8, vf8y */
    vu0_fmac(VU0_ADD, VU0_BZ, 0, VU0_X, 8, 8, 8); /* vaddz.x vf8, vf8, vf8z */
    vu0_vrsqrt(0, 3, 8, 0); /* vrsqrt Q, vf0w, vf8x */
    /* vwaitq */
    vu0_fmac(VU0_MUL, VU0_BQ, 0, VU0_XYZ, 6, 6, 0); /* vmulq.xyz vf6, vf6, Q */
    vu0_opmula(0, 0, 6, 4); /* vopmula.xyz ACC, vf6, vf4 */
    vu0_opmula(1, 5, 4, 6); /* vopmsub.xyz vf5, vf4, vf6 */
    vu0_sq(4, a0); /* sqc2 vf4, 0x0(a0) */
    vu0_sq(5, a0 + 0x10); /* sqc2 vf5, 0x10(a0) */
    vu0_sq(6, a0 + 0x20); /* sqc2 vf6, 0x20(a0) */
    vu0_sq(7, a0 + 0x30); /* sqc2 vf7, 0x30(a0) */
    return; /* jr ra */
}
#else
asm void Mtx_Blend(f32 *dst, f32 *first, f32 *second, f32 weightFirst, f32 weightSecond)
{
    .set noreorder
    vmr32.xyzw vf6, vf0
    vmove.xyzw vf7, vf0
    vmr32.xyzw vf5, vf6
    vmr32.xyzw vf4, vf5
    mfc1 t0, f12
    nop
    qmtc2.ni t0, vf8
    mfc1 t0, f13
    nop
    qmtc2.ni t0, vf9
    lqc2 vf10, 0x0(a1)
    lqc2 vf11, 0x0(a2)
    lqc2 vf12, 0x10(a1)
    lqc2 vf13, 0x10(a2)
    lqc2 vf14, 0x30(a1)
    lqc2 vf15, 0x30(a2)
    vmulax.xyz ACC, vf10, vf8x
    vmaddx.xyz vf10, vf11, vf9x
    vmulax.xyz ACC, vf12, vf8x
    vmaddx.xyz vf12, vf13, vf9x
    vmulax.xyz ACC, vf14, vf8x
    vmaddx.xyz vf7, vf15, vf9x
    vmul.xyz vf4, vf10, vf10
    vaddy.x vf4, vf4, vf4y
    vaddz.x vf4, vf4, vf4z
    vrsqrt Q, vf0w, vf4x
    vwaitq
    vmulq.xyz vf4, vf10, Q
    vmul.xyz vf5, vf12, vf12
    vaddy.x vf5, vf5, vf5y
    vaddz.x vf5, vf5, vf5z
    vrsqrt Q, vf0w, vf5x
    vwaitq
    vmulq.xyz vf5, vf12, Q
    vopmula.xyz ACC, vf4, vf5
    vopmsub.xyz vf6, vf5, vf4
    vmul.xyz vf8, vf6, vf6
    vaddy.x vf8, vf8, vf8y
    vaddz.x vf8, vf8, vf8z
    vrsqrt Q, vf0w, vf8x
    vwaitq
    vmulq.xyz vf6, vf6, Q
    vopmula.xyz ACC, vf6, vf4
    vopmsub.xyz vf5, vf4, vf6
    sqc2 vf4, 0x0(a0)
    sqc2 vf5, 0x10(a0)
    sqc2 vf6, 0x20(a0)
    jr ra
    sqc2 vf7, 0x30(a0)
}
#endif
/* out = vec * (the 3x3 part of mtx). */
#if PLATFORM_PC
void Vec_Rotate(f32 *out, f32 *vec, f32 *mtx)
{
    unsigned int v1 __attribute__((unused)) = 0;
    f32 f0 __attribute__((unused)) = 0;
    f32 f1 __attribute__((unused)) = 0;
    f32 f2 __attribute__((unused)) = 0;
    unsigned char stk_[0x10] __attribute__((aligned(16))) = {0};
    unsigned int sp = (unsigned int)(__SIZE_TYPE__)(stk_ + 0x10);
    unsigned int a0 __attribute__((unused)) = (unsigned int)(__SIZE_TYPE__)out;
    unsigned int a1 __attribute__((unused)) = (unsigned int)(__SIZE_TYPE__)vec;
    unsigned int a2 __attribute__((unused)) = (unsigned int)(__SIZE_TYPE__)mtx;
    f2 = PC_LWC1(a1); /* lwc1 f2, 0x0(a1) */
    sp = sp + (-0x10); /* addiu sp, sp, -0x10 */
    f1 = PC_LWC1(a1 + 0x4); /* lwc1 f1, 0x4(a1) */
    v1 = sp + (0x0); /* addiu v1, sp, 0x0 */
    f0 = PC_LWC1(a1 + 0x8); /* lwc1 f0, 0x8(a1) */
    PC_SWC1(v1, f2); /* swc1 f2, 0x0(v1) */
    PC_SWC1(v1 + 0x4, f1); /* swc1 f1, 0x4(v1) */
    PC_SWC1(v1 + 0x8, f0); /* swc1 f0, 0x8(v1) */
    vu0_lq(4, v1); /* lqc2 vf4, 0x0(v1) */
    vu0_lq(5, a2); /* lqc2 vf5, 0x0(a2) */
    vu0_lq(6, a2 + 0x10); /* lqc2 vf6, 0x10(a2) */
    vu0_lq(7, a2 + 0x20); /* lqc2 vf7, 0x20(a2) */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 5, 4); /* vmulax.xyz ACC, vf5, vf4x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 6, 4); /* vmadday.xyz ACC, vf6, vf4y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 4, 7, 4); /* vmaddz.xyz vf4, vf7, vf4z */
    vu0_sq(4, v1); /* sqc2 vf4, 0x0(v1) */
    v1 = sp + (0x0); /* addiu v1, sp, 0x0 */
    f2 = PC_LWC1(v1); /* lwc1 f2, 0x0(v1) */
    f1 = PC_LWC1(v1 + 0x4); /* lwc1 f1, 0x4(v1) */
    f0 = PC_LWC1(v1 + 0x8); /* lwc1 f0, 0x8(v1) */
    PC_SWC1(a0, f2); /* swc1 f2, 0x0(a0) */
    PC_SWC1(a0 + 0x4, f1); /* swc1 f1, 0x4(a0) */
    PC_SWC1(a0 + 0x8, f0); /* swc1 f0, 0x8(a0) */
    sp = sp + (0x10); /* addiu sp, sp, 0x10 */
    return; /* jr ra */
}
#else
asm void Vec_Rotate(f32 *out, f32 *vec, f32 *mtx)
{
    .set noreorder
    lwc1 f2, 0x0(a1)
    addiu sp, sp, -0x10
    lwc1 f1, 0x4(a1)
    addiu v1, sp, 0x0
    lwc1 f0, 0x8(a1)
    swc1 f2, 0x0(v1)
    swc1 f1, 0x4(v1)
    swc1 f0, 0x8(v1)
    lqc2 vf4, 0x0(v1)
    lqc2 vf5, 0x0(a2)
    lqc2 vf6, 0x10(a2)
    lqc2 vf7, 0x20(a2)
    vmulax.xyz ACC, vf5, vf4x
    vmadday.xyz ACC, vf6, vf4y
    vmaddz.xyz vf4, vf7, vf4z
    sqc2 vf4, 0x0(v1)
    addiu v1, sp, 0x0
    lwc1 f2, 0x0(v1)
    lwc1 f1, 0x4(v1)
    lwc1 f0, 0x8(v1)
    swc1 f2, 0x0(a0)
    swc1 f1, 0x4(a0)
    swc1 f0, 0x8(a0)
    jr ra
    addiu sp, sp, 0x10
}
#endif
/* vec = vec * (the 3x3 part of mtx). */
#if PLATFORM_PC
void Vec_RotateSelf(f32 *vec, f32 *mtx)
{
    unsigned int v1 __attribute__((unused)) = 0;
    f32 f0 __attribute__((unused)) = 0;
    f32 f1 __attribute__((unused)) = 0;
    f32 f2 __attribute__((unused)) = 0;
    unsigned char stk_[0x10] __attribute__((aligned(16))) = {0};
    unsigned int sp = (unsigned int)(__SIZE_TYPE__)(stk_ + 0x10);
    unsigned int a0 __attribute__((unused)) = (unsigned int)(__SIZE_TYPE__)vec;
    unsigned int a1 __attribute__((unused)) = (unsigned int)(__SIZE_TYPE__)mtx;
    f2 = PC_LWC1(a0); /* lwc1 f2, 0x0(a0) */
    sp = sp + (-0x10); /* addiu sp, sp, -0x10 */
    f1 = PC_LWC1(a0 + 0x4); /* lwc1 f1, 0x4(a0) */
    v1 = sp + (0x0); /* addiu v1, sp, 0x0 */
    f0 = PC_LWC1(a0 + 0x8); /* lwc1 f0, 0x8(a0) */
    PC_SWC1(v1, f2); /* swc1 f2, 0x0(v1) */
    PC_SWC1(v1 + 0x4, f1); /* swc1 f1, 0x4(v1) */
    PC_SWC1(v1 + 0x8, f0); /* swc1 f0, 0x8(v1) */
    vu0_lq(4, v1); /* lqc2 vf4, 0x0(v1) */
    vu0_lq(5, a1); /* lqc2 vf5, 0x0(a1) */
    vu0_lq(6, a1 + 0x10); /* lqc2 vf6, 0x10(a1) */
    vu0_lq(7, a1 + 0x20); /* lqc2 vf7, 0x20(a1) */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZ, 0, 5, 4); /* vmulax.xyz ACC, vf5, vf4x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZ, 0, 6, 4); /* vmadday.xyz ACC, vf6, vf4y */
    vu0_fmac(VU0_MADD, VU0_BZ, 0, VU0_XYZ, 4, 7, 4); /* vmaddz.xyz vf4, vf7, vf4z */
    vu0_sq(4, v1); /* sqc2 vf4, 0x0(v1) */
    v1 = sp + (0x0); /* addiu v1, sp, 0x0 */
    f2 = PC_LWC1(v1); /* lwc1 f2, 0x0(v1) */
    f1 = PC_LWC1(v1 + 0x4); /* lwc1 f1, 0x4(v1) */
    f0 = PC_LWC1(v1 + 0x8); /* lwc1 f0, 0x8(v1) */
    PC_SWC1(a0, f2); /* swc1 f2, 0x0(a0) */
    PC_SWC1(a0 + 0x4, f1); /* swc1 f1, 0x4(a0) */
    PC_SWC1(a0 + 0x8, f0); /* swc1 f0, 0x8(a0) */
    sp = sp + (0x10); /* addiu sp, sp, 0x10 */
    return; /* jr ra */
}
#else
asm void Vec_RotateSelf(f32 *vec, f32 *mtx)
{
    .set noreorder
    lwc1 f2, 0x0(a0)
    addiu sp, sp, -0x10
    lwc1 f1, 0x4(a0)
    addiu v1, sp, 0x0
    lwc1 f0, 0x8(a0)
    swc1 f2, 0x0(v1)
    swc1 f1, 0x4(v1)
    swc1 f0, 0x8(v1)
    lqc2 vf4, 0x0(v1)
    lqc2 vf5, 0x0(a1)
    lqc2 vf6, 0x10(a1)
    lqc2 vf7, 0x20(a1)
    vmulax.xyz ACC, vf5, vf4x
    vmadday.xyz ACC, vf6, vf4y
    vmaddz.xyz vf4, vf7, vf4z
    sqc2 vf4, 0x0(v1)
    addiu v1, sp, 0x0
    lwc1 f2, 0x0(v1)
    lwc1 f1, 0x4(v1)
    lwc1 f0, 0x8(v1)
    swc1 f2, 0x0(a0)
    swc1 f1, 0x4(a0)
    swc1 f0, 0x8(a0)
    jr ra
    addiu sp, sp, 0x10
}
#endif
/* out = vec * mtx (4 components). */
#if PLATFORM_PC
void Vec4_Transform(f32 *out, f32 *vec, f32 *mtx)
{
    unsigned int v1 __attribute__((unused)) = 0;
    f32 f0 __attribute__((unused)) = 0;
    f32 f1 __attribute__((unused)) = 0;
    f32 f2 __attribute__((unused)) = 0;
    f32 f3 __attribute__((unused)) = 0;
    unsigned char stk_[0x10] __attribute__((aligned(16))) = {0};
    unsigned int sp = (unsigned int)(__SIZE_TYPE__)(stk_ + 0x10);
    unsigned int a0 __attribute__((unused)) = (unsigned int)(__SIZE_TYPE__)out;
    unsigned int a1 __attribute__((unused)) = (unsigned int)(__SIZE_TYPE__)vec;
    unsigned int a2 __attribute__((unused)) = (unsigned int)(__SIZE_TYPE__)mtx;
    f3 = PC_LWC1(a1); /* lwc1 f3, 0x0(a1) */
    sp = sp + (-0x10); /* addiu sp, sp, -0x10 */
    f2 = PC_LWC1(a1 + 0x4); /* lwc1 f2, 0x4(a1) */
    v1 = sp + (0x0); /* addiu v1, sp, 0x0 */
    f1 = PC_LWC1(a1 + 0x8); /* lwc1 f1, 0x8(a1) */
    f0 = PC_LWC1(a1 + 0xC); /* lwc1 f0, 0xC(a1) */
    PC_SWC1(v1, f3); /* swc1 f3, 0x0(v1) */
    PC_SWC1(v1 + 0x4, f2); /* swc1 f2, 0x4(v1) */
    PC_SWC1(v1 + 0x8, f1); /* swc1 f1, 0x8(v1) */
    PC_SWC1(v1 + 0xC, f0); /* swc1 f0, 0xC(v1) */
    vu0_lq(4, v1); /* lqc2 vf4, 0x0(v1) */
    vu0_lq(5, a2); /* lqc2 vf5, 0x0(a2) */
    vu0_lq(6, a2 + 0x10); /* lqc2 vf6, 0x10(a2) */
    vu0_lq(7, a2 + 0x20); /* lqc2 vf7, 0x20(a2) */
    vu0_lq(8, a2 + 0x30); /* lqc2 vf8, 0x30(a2) */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZW, 0, 5, 4); /* vmulax.xyzw ACC, vf5, vf4x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZW, 0, 6, 4); /* vmadday.xyzw ACC, vf6, vf4y */
    vu0_fmac(VU0_MADD, VU0_BZ, 1, VU0_XYZW, 0, 7, 4); /* vmaddaz.xyzw ACC, vf7, vf4z */
    vu0_fmac(VU0_MADD, VU0_BW, 0, VU0_XYZW, 4, 8, 4); /* vmaddw.xyzw vf4, vf8, vf4w */
    vu0_sq(4, v1); /* sqc2 vf4, 0x0(v1) */
    v1 = sp + (0x0); /* addiu v1, sp, 0x0 */
    f3 = PC_LWC1(v1); /* lwc1 f3, 0x0(v1) */
    f2 = PC_LWC1(v1 + 0x4); /* lwc1 f2, 0x4(v1) */
    f1 = PC_LWC1(v1 + 0x8); /* lwc1 f1, 0x8(v1) */
    f0 = PC_LWC1(v1 + 0xC); /* lwc1 f0, 0xC(v1) */
    PC_SWC1(a0, f3); /* swc1 f3, 0x0(a0) */
    PC_SWC1(a0 + 0x4, f2); /* swc1 f2, 0x4(a0) */
    PC_SWC1(a0 + 0x8, f1); /* swc1 f1, 0x8(a0) */
    PC_SWC1(a0 + 0xC, f0); /* swc1 f0, 0xC(a0) */
    sp = sp + (0x10); /* addiu sp, sp, 0x10 */
    return; /* jr ra */
}
#else
asm void Vec4_Transform(f32 *out, f32 *vec, f32 *mtx)
{
    .set noreorder
    lwc1 f3, 0x0(a1)
    addiu sp, sp, -0x10
    lwc1 f2, 0x4(a1)
    addiu v1, sp, 0x0
    lwc1 f1, 0x8(a1)
    lwc1 f0, 0xC(a1)
    swc1 f3, 0x0(v1)
    swc1 f2, 0x4(v1)
    swc1 f1, 0x8(v1)
    swc1 f0, 0xC(v1)
    lqc2 vf4, 0x0(v1)
    lqc2 vf5, 0x0(a2)
    lqc2 vf6, 0x10(a2)
    lqc2 vf7, 0x20(a2)
    lqc2 vf8, 0x30(a2)
    vmulax.xyzw ACC, vf5, vf4x
    vmadday.xyzw ACC, vf6, vf4y
    vmaddaz.xyzw ACC, vf7, vf4z
    vmaddw.xyzw vf4, vf8, vf4w
    sqc2 vf4, 0x0(v1)
    addiu v1, sp, 0x0
    lwc1 f3, 0x0(v1)
    lwc1 f2, 0x4(v1)
    lwc1 f1, 0x8(v1)
    lwc1 f0, 0xC(v1)
    swc1 f3, 0x0(a0)
    swc1 f2, 0x4(a0)
    swc1 f1, 0x8(a0)
    swc1 f0, 0xC(a0)
    jr ra
    addiu sp, sp, 0x10
}
#endif
/* vec = vec * mtx (4 components). */
#if PLATFORM_PC
void Vec4_TransformSelf(f32 *vec, f32 *mtx)
{
    unsigned int v1 __attribute__((unused)) = 0;
    f32 f0 __attribute__((unused)) = 0;
    f32 f1 __attribute__((unused)) = 0;
    f32 f2 __attribute__((unused)) = 0;
    f32 f3 __attribute__((unused)) = 0;
    unsigned char stk_[0x10] __attribute__((aligned(16))) = {0};
    unsigned int sp = (unsigned int)(__SIZE_TYPE__)(stk_ + 0x10);
    unsigned int a0 __attribute__((unused)) = (unsigned int)(__SIZE_TYPE__)vec;
    unsigned int a1 __attribute__((unused)) = (unsigned int)(__SIZE_TYPE__)mtx;
    f3 = PC_LWC1(a0); /* lwc1 f3, 0x0(a0) */
    sp = sp + (-0x10); /* addiu sp, sp, -0x10 */
    f2 = PC_LWC1(a0 + 0x4); /* lwc1 f2, 0x4(a0) */
    v1 = sp + (0x0); /* addiu v1, sp, 0x0 */
    f1 = PC_LWC1(a0 + 0x8); /* lwc1 f1, 0x8(a0) */
    f0 = PC_LWC1(a0 + 0xC); /* lwc1 f0, 0xC(a0) */
    PC_SWC1(v1, f3); /* swc1 f3, 0x0(v1) */
    PC_SWC1(v1 + 0x4, f2); /* swc1 f2, 0x4(v1) */
    PC_SWC1(v1 + 0x8, f1); /* swc1 f1, 0x8(v1) */
    PC_SWC1(v1 + 0xC, f0); /* swc1 f0, 0xC(v1) */
    vu0_lq(4, v1); /* lqc2 vf4, 0x0(v1) */
    vu0_lq(5, a1); /* lqc2 vf5, 0x0(a1) */
    vu0_lq(6, a1 + 0x10); /* lqc2 vf6, 0x10(a1) */
    vu0_lq(7, a1 + 0x20); /* lqc2 vf7, 0x20(a1) */
    vu0_lq(8, a1 + 0x30); /* lqc2 vf8, 0x30(a1) */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZW, 0, 5, 4); /* vmulax.xyzw ACC, vf5, vf4x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZW, 0, 6, 4); /* vmadday.xyzw ACC, vf6, vf4y */
    vu0_fmac(VU0_MADD, VU0_BZ, 1, VU0_XYZW, 0, 7, 4); /* vmaddaz.xyzw ACC, vf7, vf4z */
    vu0_fmac(VU0_MADD, VU0_BW, 0, VU0_XYZW, 4, 8, 4); /* vmaddw.xyzw vf4, vf8, vf4w */
    vu0_sq(4, v1); /* sqc2 vf4, 0x0(v1) */
    v1 = sp + (0x0); /* addiu v1, sp, 0x0 */
    f3 = PC_LWC1(v1); /* lwc1 f3, 0x0(v1) */
    f2 = PC_LWC1(v1 + 0x4); /* lwc1 f2, 0x4(v1) */
    f1 = PC_LWC1(v1 + 0x8); /* lwc1 f1, 0x8(v1) */
    f0 = PC_LWC1(v1 + 0xC); /* lwc1 f0, 0xC(v1) */
    PC_SWC1(a0, f3); /* swc1 f3, 0x0(a0) */
    PC_SWC1(a0 + 0x4, f2); /* swc1 f2, 0x4(a0) */
    PC_SWC1(a0 + 0x8, f1); /* swc1 f1, 0x8(a0) */
    PC_SWC1(a0 + 0xC, f0); /* swc1 f0, 0xC(a0) */
    sp = sp + (0x10); /* addiu sp, sp, 0x10 */
    return; /* jr ra */
}
#else
asm void Vec4_TransformSelf(f32 *vec, f32 *mtx)
{
    .set noreorder
    lwc1 f3, 0x0(a0)
    addiu sp, sp, -0x10
    lwc1 f2, 0x4(a0)
    addiu v1, sp, 0x0
    lwc1 f1, 0x8(a0)
    lwc1 f0, 0xC(a0)
    swc1 f3, 0x0(v1)
    swc1 f2, 0x4(v1)
    swc1 f1, 0x8(v1)
    swc1 f0, 0xC(v1)
    lqc2 vf4, 0x0(v1)
    lqc2 vf5, 0x0(a1)
    lqc2 vf6, 0x10(a1)
    lqc2 vf7, 0x20(a1)
    lqc2 vf8, 0x30(a1)
    vmulax.xyzw ACC, vf5, vf4x
    vmadday.xyzw ACC, vf6, vf4y
    vmaddaz.xyzw ACC, vf7, vf4z
    vmaddw.xyzw vf4, vf8, vf4w
    sqc2 vf4, 0x0(v1)
    addiu v1, sp, 0x0
    lwc1 f3, 0x0(v1)
    lwc1 f2, 0x4(v1)
    lwc1 f1, 0x8(v1)
    lwc1 f0, 0xC(v1)
    swc1 f3, 0x0(a0)
    swc1 f2, 0x4(a0)
    swc1 f1, 0x8(a0)
    swc1 f0, 0xC(a0)
    jr ra
    addiu sp, sp, 0x10
}
#endif
/* out = (vec, 1) * mtx divided by its w. */
#if PLATFORM_PC
void Vec_Project(f32 *out, f32 *vec, f32 *mtx)
{
    unsigned int v1 __attribute__((unused)) = 0;
    unsigned int a3 __attribute__((unused)) = 0;
    f32 f0 __attribute__((unused)) = 0;
    f32 f1 __attribute__((unused)) = 0;
    f32 f2 __attribute__((unused)) = 0;
    f32 f3 __attribute__((unused)) = 0;
    unsigned char stk_[0x10] __attribute__((aligned(16))) = {0};
    unsigned int sp = (unsigned int)(__SIZE_TYPE__)(stk_ + 0x10);
    unsigned int a0 __attribute__((unused)) = (unsigned int)(__SIZE_TYPE__)out;
    unsigned int a1 __attribute__((unused)) = (unsigned int)(__SIZE_TYPE__)vec;
    unsigned int a2 __attribute__((unused)) = (unsigned int)(__SIZE_TYPE__)mtx;
    f0 = PC_LWC1(a1); /* lwc1 f0, 0x0(a1) */
    sp = sp + (-0x10); /* addiu sp, sp, -0x10 */
    v1 = (unsigned int)((0x3F800000 >> 16)) << 16; /* lui v1, (0x3F800000 >> 16) */
    a3 = sp + (0x0); /* addiu a3, sp, 0x0 */
    PC_SWC1(sp, f0); /* swc1 f0, 0x0(sp) */
    f0 = PC_LWC1(a1 + 0x4); /* lwc1 f0, 0x4(a1) */
    PC_SWC1(sp + 0x4, f0); /* swc1 f0, 0x4(sp) */
    f0 = PC_LWC1(a1 + 0x8); /* lwc1 f0, 0x8(a1) */
    PC_SWC1(sp + 0x8, f0); /* swc1 f0, 0x8(sp) */
    PC_SW(sp + 0xC, v1); /* sw v1, 0xC(sp) */
    vu0_lq(4, a3); /* lqc2 vf4, 0x0(a3) */
    vu0_lq(5, a2); /* lqc2 vf5, 0x0(a2) */
    vu0_lq(6, a2 + 0x10); /* lqc2 vf6, 0x10(a2) */
    vu0_lq(7, a2 + 0x20); /* lqc2 vf7, 0x20(a2) */
    vu0_lq(8, a2 + 0x30); /* lqc2 vf8, 0x30(a2) */
    vu0_fmac(VU0_MUL, VU0_BX, 1, VU0_XYZW, 0, 5, 4); /* vmulax.xyzw ACC, vf5, vf4x */
    vu0_fmac(VU0_MADD, VU0_BY, 1, VU0_XYZW, 0, 6, 4); /* vmadday.xyzw ACC, vf6, vf4y */
    vu0_fmac(VU0_MADD, VU0_BZ, 1, VU0_XYZW, 0, 7, 4); /* vmaddaz.xyzw ACC, vf7, vf4z */
    vu0_fmac(VU0_MADD, VU0_BW, 0, VU0_XYZW, 4, 8, 4); /* vmaddw.xyzw vf4, vf8, vf4w */
    vu0_vdiv(0, 3, 4, 3); /* vdiv Q, vf0w, vf4w */
    /* vwaitq */
    vu0_fmac(VU0_MUL, VU0_BQ, 0, VU0_XYZ, 4, 4, 0); /* vmulq.xyz vf4, vf4, Q */
    vu0_sq(4, a3); /* sqc2 vf4, 0x0(a3) */
    v1 = sp + (0x0); /* addiu v1, sp, 0x0 */
    f3 = PC_LWC1(v1); /* lwc1 f3, 0x0(v1) */
    f2 = PC_LWC1(v1 + 0x4); /* lwc1 f2, 0x4(v1) */
    f1 = PC_LWC1(v1 + 0x8); /* lwc1 f1, 0x8(v1) */
    f0 = PC_LWC1(v1 + 0xC); /* lwc1 f0, 0xC(v1) */
    PC_SWC1(a0, f3); /* swc1 f3, 0x0(a0) */
    PC_SWC1(a0 + 0x4, f2); /* swc1 f2, 0x4(a0) */
    PC_SWC1(a0 + 0x8, f1); /* swc1 f1, 0x8(a0) */
    PC_SWC1(a0 + 0xC, f0); /* swc1 f0, 0xC(a0) */
    sp = sp + (0x10); /* addiu sp, sp, 0x10 */
    return; /* jr ra */
}
#else
asm void Vec_Project(f32 *out, f32 *vec, f32 *mtx)
{
    .set noreorder
    lwc1 f0, 0x0(a1)
    addiu sp, sp, -0x10
    lui v1, (0x3F800000 >> 16)
    addiu a3, sp, 0x0
    swc1 f0, 0x0(sp)
    lwc1 f0, 0x4(a1)
    swc1 f0, 0x4(sp)
    lwc1 f0, 0x8(a1)
    swc1 f0, 0x8(sp)
    sw v1, 0xC(sp)
    lqc2 vf4, 0x0(a3)
    lqc2 vf5, 0x0(a2)
    lqc2 vf6, 0x10(a2)
    lqc2 vf7, 0x20(a2)
    lqc2 vf8, 0x30(a2)
    vmulax.xyzw ACC, vf5, vf4x
    vmadday.xyzw ACC, vf6, vf4y
    vmaddaz.xyzw ACC, vf7, vf4z
    vmaddw.xyzw vf4, vf8, vf4w
    vdiv Q, vf0w, vf4w
    vwaitq
    vmulq.xyz vf4, vf4, Q
    sqc2 vf4, 0x0(a3)
    addiu v1, sp, 0x0
    lwc1 f3, 0x0(v1)
    lwc1 f2, 0x4(v1)
    lwc1 f1, 0x8(v1)
    lwc1 f0, 0xC(v1)
    swc1 f3, 0x0(a0)
    swc1 f2, 0x4(a0)
    swc1 f1, 0x8(a0)
    swc1 f0, 0xC(a0)
    jr ra
    addiu sp, sp, 0x10
}
#endif

/* vec rotated by angle about the x axis. */
void Vec_RotateX(f32 *vec, f32 angle)
{
    f32 sinA;
    f32 cosA;
    Vec3Copy orig;

    orig = *(Vec3Copy *)vec;
    Math_GetSinCos(&sinA, &cosA, angle);
    vec[1] = cosA * orig.y - sinA * orig.z;
    vec[2] = sinA * orig.y + cosA * orig.z;
}

/* vec rotated by angle about the y axis. */
void Vec_RotateY(f32 *vec, f32 angle)
{
    f32 sinA;
    f32 cosA;
    Vec3Copy orig;

    orig = *(Vec3Copy *)vec;
    Math_GetSinCos(&sinA, &cosA, angle);
    vec[0] = cosA * orig.x + sinA * orig.z;
    vec[2] = -sinA * orig.x + cosA * orig.z;
}

/* vec rotated by angle about the z axis. */
void Vec_RotateZ(f32 *vec, f32 angle)
{
    f32 sinA;
    f32 cosA;
    Vec3Copy orig;

    orig = *(Vec3Copy *)vec;
    Math_GetSinCos(&sinA, &cosA, angle);
    vec[0] = cosA * orig.x - sinA * orig.y;
    vec[1] = sinA * orig.x + cosA * orig.y;
}
/* |vec|. */
#if PLATFORM_PC
f32 Vec_Length(f32 *vec)
{
    unsigned int v0 __attribute__((unused)) = 0;
    f32 f0 __attribute__((unused)) = 0;
    f32 f1 __attribute__((unused)) = 0;
    f32 f2 __attribute__((unused)) = 0;
    unsigned char stk_[0x10] __attribute__((aligned(16))) = {0};
    unsigned int sp = (unsigned int)(__SIZE_TYPE__)(stk_ + 0x10);
    unsigned int a0 __attribute__((unused)) = (unsigned int)(__SIZE_TYPE__)vec;
    f2 = PC_LWC1(a0); /* lwc1 f2, 0x0(a0) */
    sp = sp + (-0x10); /* addiu sp, sp, -0x10 */
    f1 = PC_LWC1(a0 + 0x4); /* lwc1 f1, 0x4(a0) */
    v0 = sp + (0x0); /* addiu v0, sp, 0x0 */
    f0 = PC_LWC1(a0 + 0x8); /* lwc1 f0, 0x8(a0) */
    PC_SWC1(v0, f2); /* swc1 f2, 0x0(v0) */
    PC_SWC1(v0 + 0x4, f1); /* swc1 f1, 0x4(v0) */
    PC_SWC1(v0 + 0x8, f0); /* swc1 f0, 0x8(v0) */
    vu0_lq(4, v0); /* lqc2 vf4, 0x0(v0) */
    vu0_fmac(VU0_MUL, VU0_BVEC, 0, VU0_XYZ, 4, 4, 4); /* vmul.xyz vf4, vf4, vf4 */
    vu0_fmac(VU0_ADD, VU0_BY, 0, VU0_X, 4, 4, 4); /* vaddy.x vf4, vf4, vf4y */
    vu0_fmac(VU0_ADD, VU0_BZ, 0, VU0_X, 4, 4, 4); /* vaddz.x vf4, vf4, vf4z */
    vu0_vsqrt(4, 0); /* vsqrt Q, vf4x */
    /* vwaitq */
    vu0_fmac(VU0_ADD, VU0_BQ, 0, VU0_X, 4, 0, 0); /* vaddq.x vf4, vf0, Q */
    v0 = vu0_qmfc2(4); /* qmfc2.ni v0, vf4 */
    f0 = PC_U2F(v0); /* mtc1 v0, f0 */
    sp = sp + (0x10); /* addiu sp, sp, 0x10 */
    return f0; /* jr ra */
}
#else
asm f32 Vec_Length(f32 *vec)
{
    .set noreorder
    lwc1 f2, 0x0(a0)
    addiu sp, sp, -0x10
    lwc1 f1, 0x4(a0)
    addiu v0, sp, 0x0
    lwc1 f0, 0x8(a0)
    swc1 f2, 0x0(v0)
    swc1 f1, 0x4(v0)
    swc1 f0, 0x8(v0)
    lqc2 vf4, 0x0(v0)
    vmul.xyz vf4, vf4, vf4
    vaddy.x vf4, vf4, vf4y
    vaddz.x vf4, vf4, vf4z
    vsqrt Q, vf4x
    vwaitq
    vaddq.x vf4, vf0, Q
    qmfc2.ni v0, vf4
    mtc1 v0, f0
    jr ra
    addiu sp, sp, 0x10
}
#endif
/* |from - to|. */
#if PLATFORM_PC
f32 Vec_Distance(f32 *from, f32 *to)
{
    unsigned int v0 __attribute__((unused)) = 0;
    unsigned int v1 __attribute__((unused)) = 0;
    f32 f0 __attribute__((unused)) = 0;
    f32 f1 __attribute__((unused)) = 0;
    f32 f2 __attribute__((unused)) = 0;
    unsigned char stk_[0x20] __attribute__((aligned(16))) = {0};
    unsigned int sp = (unsigned int)(__SIZE_TYPE__)(stk_ + 0x20);
    unsigned int a0 __attribute__((unused)) = (unsigned int)(__SIZE_TYPE__)from;
    unsigned int a1 __attribute__((unused)) = (unsigned int)(__SIZE_TYPE__)to;
    f2 = PC_LWC1(a0); /* lwc1 f2, 0x0(a0) */
    sp = sp + (-0x20); /* addiu sp, sp, -0x20 */
    f1 = PC_LWC1(a0 + 0x4); /* lwc1 f1, 0x4(a0) */
    v1 = sp + (0x10); /* addiu v1, sp, 0x10 */
    f0 = PC_LWC1(a0 + 0x8); /* lwc1 f0, 0x8(a0) */
    v0 = sp + (0x0); /* addiu v0, sp, 0x0 */
    PC_SWC1(v1, f2); /* swc1 f2, 0x0(v1) */
    PC_SWC1(v1 + 0x4, f1); /* swc1 f1, 0x4(v1) */
    PC_SWC1(v1 + 0x8, f0); /* swc1 f0, 0x8(v1) */
    f2 = PC_LWC1(a1); /* lwc1 f2, 0x0(a1) */
    f1 = PC_LWC1(a1 + 0x4); /* lwc1 f1, 0x4(a1) */
    f0 = PC_LWC1(a1 + 0x8); /* lwc1 f0, 0x8(a1) */
    PC_SWC1(v0, f2); /* swc1 f2, 0x0(v0) */
    PC_SWC1(v0 + 0x4, f1); /* swc1 f1, 0x4(v0) */
    PC_SWC1(v0 + 0x8, f0); /* swc1 f0, 0x8(v0) */
    vu0_lq(4, v1); /* lqc2 vf4, 0x0(v1) */
    vu0_lq(5, v0); /* lqc2 vf5, 0x0(v0) */
    vu0_fmac(VU0_SUB, VU0_BVEC, 0, VU0_XYZ, 4, 4, 5); /* vsub.xyz vf4, vf4, vf5 */
    vu0_fmac(VU0_MUL, VU0_BVEC, 0, VU0_XYZ, 4, 4, 4); /* vmul.xyz vf4, vf4, vf4 */
    vu0_fmac(VU0_ADD, VU0_BY, 0, VU0_X, 4, 4, 4); /* vaddy.x vf4, vf4, vf4y */
    vu0_fmac(VU0_ADD, VU0_BZ, 0, VU0_X, 4, 4, 4); /* vaddz.x vf4, vf4, vf4z */
    vu0_vsqrt(4, 0); /* vsqrt Q, vf4x */
    /* vwaitq */
    vu0_fmac(VU0_ADD, VU0_BQ, 0, VU0_X, 4, 0, 0); /* vaddq.x vf4, vf0, Q */
    v1 = vu0_qmfc2(4); /* qmfc2.ni v1, vf4 */
    f0 = PC_U2F(v1); /* mtc1 v1, f0 */
    sp = sp + (0x20); /* addiu sp, sp, 0x20 */
    return f0; /* jr ra */
}
#else
asm f32 Vec_Distance(f32 *from, f32 *to)
{
    .set noreorder
    lwc1 f2, 0x0(a0)
    addiu sp, sp, -0x20
    lwc1 f1, 0x4(a0)
    addiu v1, sp, 0x10
    lwc1 f0, 0x8(a0)
    addiu v0, sp, 0x0
    swc1 f2, 0x0(v1)
    swc1 f1, 0x4(v1)
    swc1 f0, 0x8(v1)
    lwc1 f2, 0x0(a1)
    lwc1 f1, 0x4(a1)
    lwc1 f0, 0x8(a1)
    swc1 f2, 0x0(v0)
    swc1 f1, 0x4(v0)
    swc1 f0, 0x8(v0)
    lqc2 vf4, 0x0(v1)
    lqc2 vf5, 0x0(v0)
    vsub.xyz vf4, vf4, vf5
    vmul.xyz vf4, vf4, vf4
    vaddy.x vf4, vf4, vf4y
    vaddz.x vf4, vf4, vf4z
    vsqrt Q, vf4x
    vwaitq
    vaddq.x vf4, vf0, Q
    qmfc2.ni v1, vf4
    mtc1 v1, f0
    jr ra
    addiu sp, sp, 0x20
}
#endif
/* vec = vec / |vec|. */
#if PLATFORM_PC
void Vec_Normalize(f32 *vec)
{
    unsigned int v1 __attribute__((unused)) = 0;
    f32 f0 __attribute__((unused)) = 0;
    f32 f1 __attribute__((unused)) = 0;
    f32 f2 __attribute__((unused)) = 0;
    unsigned char stk_[0x10] __attribute__((aligned(16))) = {0};
    unsigned int sp = (unsigned int)(__SIZE_TYPE__)(stk_ + 0x10);
    unsigned int a0 __attribute__((unused)) = (unsigned int)(__SIZE_TYPE__)vec;
    f2 = PC_LWC1(a0); /* lwc1 f2, 0x0(a0) */
    sp = sp + (-0x10); /* addiu sp, sp, -0x10 */
    f1 = PC_LWC1(a0 + 0x4); /* lwc1 f1, 0x4(a0) */
    v1 = sp + (0x0); /* addiu v1, sp, 0x0 */
    f0 = PC_LWC1(a0 + 0x8); /* lwc1 f0, 0x8(a0) */
    PC_SWC1(v1, f2); /* swc1 f2, 0x0(v1) */
    PC_SWC1(v1 + 0x4, f1); /* swc1 f1, 0x4(v1) */
    PC_SWC1(v1 + 0x8, f0); /* swc1 f0, 0x8(v1) */
    vu0_lq(4, v1); /* lqc2 vf4, 0x0(v1) */
    vu0_fmac(VU0_MUL, VU0_BVEC, 0, VU0_XYZ, 5, 4, 4); /* vmul.xyz vf5, vf4, vf4 */
    vu0_fmac(VU0_ADD, VU0_BY, 0, VU0_X, 5, 5, 5); /* vaddy.x vf5, vf5, vf5y */
    vu0_fmac(VU0_ADD, VU0_BZ, 0, VU0_X, 5, 5, 5); /* vaddz.x vf5, vf5, vf5z */
    vu0_vrsqrt(0, 3, 5, 0); /* vrsqrt Q, vf0w, vf5x */
    /* vwaitq */
    vu0_fmac(VU0_MUL, VU0_BQ, 0, VU0_XYZ, 4, 4, 0); /* vmulq.xyz vf4, vf4, Q */
    vu0_sq(4, v1); /* sqc2 vf4, 0x0(v1) */
    v1 = sp + (0x0); /* addiu v1, sp, 0x0 */
    f2 = PC_LWC1(v1); /* lwc1 f2, 0x0(v1) */
    f1 = PC_LWC1(v1 + 0x4); /* lwc1 f1, 0x4(v1) */
    f0 = PC_LWC1(v1 + 0x8); /* lwc1 f0, 0x8(v1) */
    PC_SWC1(a0, f2); /* swc1 f2, 0x0(a0) */
    PC_SWC1(a0 + 0x4, f1); /* swc1 f1, 0x4(a0) */
    PC_SWC1(a0 + 0x8, f0); /* swc1 f0, 0x8(a0) */
    sp = sp + (0x10); /* addiu sp, sp, 0x10 */
    return; /* jr ra */
}
#else
asm void Vec_Normalize(f32 *vec)
{
    .set noreorder
    lwc1 f2, 0x0(a0)
    addiu sp, sp, -0x10
    lwc1 f1, 0x4(a0)
    addiu v1, sp, 0x0
    lwc1 f0, 0x8(a0)
    swc1 f2, 0x0(v1)
    swc1 f1, 0x4(v1)
    swc1 f0, 0x8(v1)
    lqc2 vf4, 0x0(v1)
    vmul.xyz vf5, vf4, vf4
    vaddy.x vf5, vf5, vf5y
    vaddz.x vf5, vf5, vf5z
    vrsqrt Q, vf0w, vf5x
    vwaitq
    vmulq.xyz vf4, vf4, Q
    sqc2 vf4, 0x0(v1)
    addiu v1, sp, 0x0
    lwc1 f2, 0x0(v1)
    lwc1 f1, 0x4(v1)
    lwc1 f0, 0x8(v1)
    swc1 f2, 0x0(a0)
    swc1 f1, 0x4(a0)
    swc1 f0, 0x8(a0)
    jr ra
    addiu sp, sp, 0x10
}
#endif
/* lhs . rhs (3 components). */
#if PLATFORM_PC
f32 Vec_Dot(f32 *lhs, f32 *rhs)
{
    unsigned int v0 __attribute__((unused)) = 0;
    unsigned int v1 __attribute__((unused)) = 0;
    f32 f0 __attribute__((unused)) = 0;
    f32 f1 __attribute__((unused)) = 0;
    f32 f2 __attribute__((unused)) = 0;
    unsigned char stk_[0x20] __attribute__((aligned(16))) = {0};
    unsigned int sp = (unsigned int)(__SIZE_TYPE__)(stk_ + 0x20);
    unsigned int a0 __attribute__((unused)) = (unsigned int)(__SIZE_TYPE__)lhs;
    unsigned int a1 __attribute__((unused)) = (unsigned int)(__SIZE_TYPE__)rhs;
    f2 = PC_LWC1(a0); /* lwc1 f2, 0x0(a0) */
    sp = sp + (-0x20); /* addiu sp, sp, -0x20 */
    f1 = PC_LWC1(a0 + 0x4); /* lwc1 f1, 0x4(a0) */
    v1 = sp + (0x10); /* addiu v1, sp, 0x10 */
    f0 = PC_LWC1(a0 + 0x8); /* lwc1 f0, 0x8(a0) */
    v0 = sp + (0x0); /* addiu v0, sp, 0x0 */
    PC_SWC1(v1, f2); /* swc1 f2, 0x0(v1) */
    PC_SWC1(v1 + 0x4, f1); /* swc1 f1, 0x4(v1) */
    PC_SWC1(v1 + 0x8, f0); /* swc1 f0, 0x8(v1) */
    f2 = PC_LWC1(a1); /* lwc1 f2, 0x0(a1) */
    f1 = PC_LWC1(a1 + 0x4); /* lwc1 f1, 0x4(a1) */
    f0 = PC_LWC1(a1 + 0x8); /* lwc1 f0, 0x8(a1) */
    PC_SWC1(v0, f2); /* swc1 f2, 0x0(v0) */
    PC_SWC1(v0 + 0x4, f1); /* swc1 f1, 0x4(v0) */
    PC_SWC1(v0 + 0x8, f0); /* swc1 f0, 0x8(v0) */
    vu0_lq(4, v1); /* lqc2 vf4, 0x0(v1) */
    vu0_lq(5, v0); /* lqc2 vf5, 0x0(v0) */
    vu0_fmac(VU0_MUL, VU0_BVEC, 0, VU0_XYZ, 4, 4, 5); /* vmul.xyz vf4, vf4, vf5 */
    vu0_fmac(VU0_ADD, VU0_BY, 0, VU0_X, 4, 4, 4); /* vaddy.x vf4, vf4, vf4y */
    vu0_fmac(VU0_ADD, VU0_BZ, 0, VU0_X, 4, 4, 4); /* vaddz.x vf4, vf4, vf4z */
    v1 = vu0_qmfc2(4); /* qmfc2.ni v1, vf4 */
    f0 = PC_U2F(v1); /* mtc1 v1, f0 */
    sp = sp + (0x20); /* addiu sp, sp, 0x20 */
    return f0; /* jr ra */
}
#else
asm f32 Vec_Dot(f32 *lhs, f32 *rhs)
{
    .set noreorder
    lwc1 f2, 0x0(a0)
    addiu sp, sp, -0x20
    lwc1 f1, 0x4(a0)
    addiu v1, sp, 0x10
    lwc1 f0, 0x8(a0)
    addiu v0, sp, 0x0
    swc1 f2, 0x0(v1)
    swc1 f1, 0x4(v1)
    swc1 f0, 0x8(v1)
    lwc1 f2, 0x0(a1)
    lwc1 f1, 0x4(a1)
    lwc1 f0, 0x8(a1)
    swc1 f2, 0x0(v0)
    swc1 f1, 0x4(v0)
    swc1 f0, 0x8(v0)
    lqc2 vf4, 0x0(v1)
    lqc2 vf5, 0x0(v0)
    vmul.xyz vf4, vf4, vf5
    vaddy.x vf4, vf4, vf4y
    vaddz.x vf4, vf4, vf4z
    qmfc2.ni v1, vf4
    mtc1 v1, f0
    jr ra
    addiu sp, sp, 0x20
}
#endif
/* out = lhs x rhs. */
#if PLATFORM_PC
void Vec_Cross(f32 *out, f32 *lhs, f32 *rhs)
{
    unsigned int v1 __attribute__((unused)) = 0;
    unsigned int a3 __attribute__((unused)) = 0;
    f32 f0 __attribute__((unused)) = 0;
    f32 f1 __attribute__((unused)) = 0;
    f32 f2 __attribute__((unused)) = 0;
    unsigned char stk_[0x20] __attribute__((aligned(16))) = {0};
    unsigned int sp = (unsigned int)(__SIZE_TYPE__)(stk_ + 0x20);
    unsigned int a0 __attribute__((unused)) = (unsigned int)(__SIZE_TYPE__)out;
    unsigned int a1 __attribute__((unused)) = (unsigned int)(__SIZE_TYPE__)lhs;
    unsigned int a2 __attribute__((unused)) = (unsigned int)(__SIZE_TYPE__)rhs;
    f2 = PC_LWC1(a1); /* lwc1 f2, 0x0(a1) */
    sp = sp + (-0x20); /* addiu sp, sp, -0x20 */
    f1 = PC_LWC1(a1 + 0x4); /* lwc1 f1, 0x4(a1) */
    a3 = sp + (0x10); /* addiu a3, sp, 0x10 */
    f0 = PC_LWC1(a1 + 0x8); /* lwc1 f0, 0x8(a1) */
    v1 = sp + (0x0); /* addiu v1, sp, 0x0 */
    PC_SWC1(a3, f2); /* swc1 f2, 0x0(a3) */
    PC_SWC1(a3 + 0x4, f1); /* swc1 f1, 0x4(a3) */
    PC_SWC1(a3 + 0x8, f0); /* swc1 f0, 0x8(a3) */
    f2 = PC_LWC1(a2); /* lwc1 f2, 0x0(a2) */
    f1 = PC_LWC1(a2 + 0x4); /* lwc1 f1, 0x4(a2) */
    f0 = PC_LWC1(a2 + 0x8); /* lwc1 f0, 0x8(a2) */
    PC_SWC1(v1, f2); /* swc1 f2, 0x0(v1) */
    PC_SWC1(v1 + 0x4, f1); /* swc1 f1, 0x4(v1) */
    PC_SWC1(v1 + 0x8, f0); /* swc1 f0, 0x8(v1) */
    vu0_lq(4, a3); /* lqc2 vf4, 0x0(a3) */
    vu0_lq(5, v1); /* lqc2 vf5, 0x0(v1) */
    vu0_opmula(0, 0, 4, 5); /* vopmula.xyz ACC, vf4, vf5 */
    vu0_opmula(1, 6, 5, 4); /* vopmsub.xyz vf6, vf5, vf4 */
    vu0_sq(6, a3); /* sqc2 vf6, 0x0(a3) */
    v1 = sp + (0x10); /* addiu v1, sp, 0x10 */
    f2 = PC_LWC1(v1); /* lwc1 f2, 0x0(v1) */
    f1 = PC_LWC1(v1 + 0x4); /* lwc1 f1, 0x4(v1) */
    f0 = PC_LWC1(v1 + 0x8); /* lwc1 f0, 0x8(v1) */
    PC_SWC1(a0, f2); /* swc1 f2, 0x0(a0) */
    PC_SWC1(a0 + 0x4, f1); /* swc1 f1, 0x4(a0) */
    PC_SWC1(a0 + 0x8, f0); /* swc1 f0, 0x8(a0) */
    sp = sp + (0x20); /* addiu sp, sp, 0x20 */
    return; /* jr ra */
}
#else
asm void Vec_Cross(f32 *out, f32 *lhs, f32 *rhs)
{
    .set noreorder
    lwc1 f2, 0x0(a1)
    addiu sp, sp, -0x20
    lwc1 f1, 0x4(a1)
    addiu a3, sp, 0x10
    lwc1 f0, 0x8(a1)
    addiu v1, sp, 0x0
    swc1 f2, 0x0(a3)
    swc1 f1, 0x4(a3)
    swc1 f0, 0x8(a3)
    lwc1 f2, 0x0(a2)
    lwc1 f1, 0x4(a2)
    lwc1 f0, 0x8(a2)
    swc1 f2, 0x0(v1)
    swc1 f1, 0x4(v1)
    swc1 f0, 0x8(v1)
    lqc2 vf4, 0x0(a3)
    lqc2 vf5, 0x0(v1)
    vopmula.xyz ACC, vf4, vf5
    vopmsub.xyz vf6, vf5, vf4
    sqc2 vf6, 0x0(a3)
    addiu v1, sp, 0x10
    lwc1 f2, 0x0(v1)
    lwc1 f1, 0x4(v1)
    lwc1 f0, 0x8(v1)
    swc1 f2, 0x0(a0)
    swc1 f1, 0x4(a0)
    swc1 f0, 0x8(a0)
    jr ra
    addiu sp, sp, 0x20
}
#endif

void Vec_Copy(f32 *dst, f32 *src)
{
    *(Vec3Copy *)dst = *(Vec3Copy *)src;
}

/* Radians to a 16-bit angle (ANGLE_TURN to a turn, in [0, ANGLE_TURN)). */
u32 Math_RadToAngle(f32 rad)
{
    f32 deg = (180.0f * rad) / 3.1415927f;

    if (deg < 0.0f) {
        deg += 360.0f;
    }
    return (u32)(182.04445f * deg);
}

/* A 16-bit angle (ANGLE_TURN to a turn) in radians, in (-pi, pi]. */
f32 Math_Angle16ToRad(u16 angle)
{
    f32 rad = 3.1415927f * (0.0054931641f * angle) / 180.0f;

    if (rad > 3.1415927f) {
        rad += -6.2831855f;
    }
    return rad;
}

/* mtx into slot `slot` of main's matrix table gMtxSlots. */
void MtxSlot_Store(s32 slot, MtxSlot *mtx)
{
    gMtxSlots[slot] = *mtx;
}

/* Slot `slot` of main's matrix table gMtxSlots into mtx. */
void MtxSlot_Load(MtxSlot *mtx, s32 slot)
{
    *mtx = gMtxSlots[slot];
}

extern f32 gMtxView[];
extern f32 gMtxProjScreen[];

/* out = point projected by gMtxSlots[0] * gMtxView * gMtxProjScreen. */
void Camera_ProjectPoint(f32 *out, f32 *point)
{
    f32 mtx[16];

    Mtx_Mul(mtx, (f32 *)gMtxSlots, gMtxView);
    Mtx_Concat(mtx, gMtxProjScreen);
    Vec_Project(out, point, mtx);
}

/* |x|. */
f32 Math_Absf(f32 x)
{
    f32 ret;

    ret = x;
    if (ret < 0.0f) {
        ret = -ret;
    }
    return ret;
}

f32 Math_Acos(f32 x)
{
    return acosf(x);
}

/* asin x = atan(x / sqrt(1 - x * x)). */
f32 Math_Asin(f32 x)
{
    return Math_Atan(x / Math_Sqrtf(1.0f + (-x * x)));
}

f32 Math_Atan(f32 x)
{
    return atanf(x);
}

/* atan2(y, x). */
f32 Math_Atan2(f32 y, f32 x)
{
    return atan2f(y, x);
}

f32 Math_Ceil(f32 x)
{
    return ceilf(x);
}

/* e to the x. */
f32 Math_Exp(f32 x)
{
    return expf(x);
}

f32 Math_Sin(f32 x)
{
    return Math_SinVu0(x);
}

f32 Math_Cos(f32 x)
{
    return Math_CosVu0(x);
}

/* *sinOut = sin angle, *cosOut = cos angle. */
void Math_GetSinCos(f32 *sinOut, f32 *cosOut, f32 angle)
{
    f32 sinCos[4];

    Math_SinCos(sinCos, angle);
    *sinOut = sinCos[0];
    *cosOut = sinCos[1];
}

f32 Math_Floor(f32 x)
{
    return floorf(x);
}

/* x to the y. */
f32 Math_Pow(f32 x, f32 y)
{
    return powf(x, y);
}
/* sqrt x (VU0). */
#if PLATFORM_PC
f32 Math_Sqrtf(f32 x)
{
    unsigned int t0 __attribute__((unused)) = 0;
    f32 f0 __attribute__((unused)) = 0;
    f32 f12 __attribute__((unused)) = x;
    t0 = PC_F2U(f12); /* mfc1 t0, f12 */
    /* nop */
    vu0_qmtc2(4, t0); /* qmtc2.ni t0, vf4 */
    vu0_vsqrt(4, 0); /* vsqrt Q, vf4x */
    /* vwaitq */
    vu0_fmac(VU0_ADD, VU0_BQ, 0, VU0_X, 4, 0, 0); /* vaddq.x vf4, vf0, Q */
    t0 = vu0_qmfc2(4); /* qmfc2.ni t0, vf4 */
    f0 = PC_U2F(t0); /* mtc1 t0, f0 */
    /* nop */
    return f0; /* jr ra */
}
#else
asm f32 Math_Sqrtf(f32 x)
{
    .set noreorder
    mfc1 t0, f12
    nop
    qmtc2.ni t0, vf4
    vsqrt Q, vf4x
    vwaitq
    vaddq.x vf4, vf0, Q
    qmfc2.ni t0, vf4
    mtc1 t0, f0
    jr ra
    nop
}
#endif

/* The rotation matrix of quaternion quat (x, y, z, w), scaled by 2 / |quat|^2 (0 for a
 * zero quat). */
void Quat_ToMatrix(f32 *quat, f32 (*mtx)[4])
{
    f32 normSq = quat[0] * quat[0] + quat[1] * quat[1] + quat[2] * quat[2] + quat[3] * quat[3];
    f32 scale;
    f32 xx, yy, zz, xy, xz, yz, wx, wy, wz;

    scale = (normSq > 0.0f) ? 2.0f / normSq : 0.0f;
    xx = scale * (quat[0] * quat[0]);
    xy = scale * (quat[0] * quat[1]);
    xz = scale * (quat[0] * quat[2]);
    wx = scale * (quat[3] * quat[0]);
    yy = scale * (quat[1] * quat[1]);
    yz = scale * (quat[1] * quat[2]);
    wy = scale * (quat[3] * quat[1]);
    wz = scale * (quat[3] * quat[2]);
    zz = scale * (quat[2] * quat[2]);
    mtx[0][0] = 1.0f - (yy + zz);
    mtx[0][1] = xy + wz;
    mtx[0][2] = xz - wy;
    mtx[1][0] = xy - wz;
    mtx[1][1] = 1.0f - (xx + zz);
    mtx[1][2] = yz + wx;
    mtx[2][0] = xz + wy;
    mtx[2][1] = yz - wx;
    mtx[2][2] = 1.0f - (xx + yy);
    mtx[0][3] = mtx[1][3] = mtx[2][3] = 0.0f;
    mtx[3][0] = mtx[3][1] = mtx[3][2] = 0.0f;
    mtx[3][3] = 1.0f;
}

/* quat (its axis in quat[0..2]) = the rotation by angle about that axis. */
void Quat_FromAxisAngle(f32 *quat, f32 angle)
{
    f32 len;
    f32 halfSin;

    angle *= 0.5f;
    len = Math_Sqrtf(quat[0] * quat[0] + quat[1] * quat[1] + quat[2] * quat[2]);
    quat[3] = 0.0f;
    quat[0] /= len;
    quat[1] /= len;
    quat[2] /= len;
    halfSin = Math_Sin(angle);
    quat[3] = Math_Cos(angle);
    quat[0] *= halfSin;
    quat[1] *= halfSin;
    quat[2] *= halfSin;
}

/* quat = the rotation by angle about axis. */
void Quat_Set(f32 *axis, f32 *quat, f32 angle)
{
    quat[3] = 0.0f;
    quat[0] = axis[0];
    quat[1] = axis[1];
    quat[2] = axis[2];
    Quat_FromAxisAngle(quat, angle);
}

/*
 * Whether the sphere at center of the given radius is in the view volume `planes` (four
 * side planes through the eye at planes[0], [4], [8], [12]; the depth range at planes[16],
 * [17]); out = center in view space (mtx, the view matrix).
 */
s32 Frustum_TestSphere(f32 *center, f32 *out, f32 *mtx, f32 *planes, f32 radius)
{
    out[2] = mtx[2] * center[0] + mtx[6] * center[1] + mtx[10] * center[2] + mtx[14];
    if (out[2] - radius > planes[16]) {
        return 0;
    }
    if (out[2] + radius < planes[17]) {
        return 0;
    }
    out[0] = mtx[0] * center[0] + mtx[4] * center[1] + mtx[8] * center[2] + mtx[12];
    if (out[0] * planes[0] + out[2] * planes[2] > radius) {
        return 0;
    }
    if (out[0] * planes[4] + out[2] * planes[6] > radius) {
        return 0;
    }
    out[1] = mtx[1] * center[0] + mtx[5] * center[1] + mtx[9] * center[2] + mtx[13];
    if (out[1] * planes[9] + out[2] * planes[10] > radius) {
        return 0;
    }
    if (out[1] * planes[13] + out[2] * planes[14] > radius) {
        return 0;
    }
    return 1;
}

/* This file's initialised data. */
f32 gSinVu0Coefs[4] = {
#include "sub_main/00402550.inc"
};
f32 gSinVu0Poly[4] = {
#include "sub_main/00402560.inc"
};
