/*
 * The widescreen enhancement's setting and the camera's aspect ratio (include/pal.h: PC_ASPECT). The game's camera
 * passes the picture's aspect, 640 / 448, to its projection and its frustum (Camera_ApplyProjection,
 * Camera_BuildFrustum) and uses it for the rail camera's yaw limit; with the widescreen setting on it is 4/3 wider
 * and the host shows the 640 x 448 picture at 16:9. An enhancement: off by default; it changes the game's state
 * (the projection matrix and the frustum planes are in RAM).
 *
 * The setting has two levels. Level 1 is the camera alone: the aspect above. Level 2 adds the 2D picture's shape
 * (pc/core/pal_hud.c) and the view tests that follow the wider picture (the frustum's planes, Frustum_Build's
 * pyramid). The core's --widescreen is level 2, the newest. The development build also takes the level from a saved session
 * ("widescreen 1" or "widescreen 2"), whatever its command line says.
 *
 * The core's --widescreen is how the host asks (mh2.ini's widescreen = on, mh2's --widescreen). Pal_WidescreenStart
 * runs once, after the options are read and before the game starts. A release build records nothing: the
 * setting is only the option.
 */
#include "pal_core.h"

#include "pal_camera.h"
#include "host.h"

static s32 sOption;   /* --widescreen */
static s32 sLevel;    /* in force: 0 off, 1 the camera, 2 the camera and the 2D picture */

s32 Pal_WidescreenOption(s32 argc, char **argv, s32 i)
{
    const char *a = argv[i], *b = "--widescreen";
    (void)argc;
    while (*a && *a == *b) {
        a++;
        b++;
    }
    if (*a || *b) {
        return 0;
    }
    sOption = 1;
    return 1;
}

void Pal_WidescreenStart(void)
{
    const char *use = sOption ? "2" : "0";
    s32 level = use[0] == '2' ? 2 : use[0] == '1' ? 1 : 0;
    sLevel = level;
    if (sLevel) {
        Host_Log(sLevel == 2 ? "widescreen: level 2, the camera and the 2D picture are 16:9 (an enhancement; it changes the game's state)"
                             : "widescreen: level 1, the camera's aspect is 16:9 (an enhancement; it changes the game's state)");
    }
}

s32 Pal_WidescreenLevel(void)
{
    return sLevel;
}

/* 640 / 448 * 4/3 (= 2560 / 1344), the float nearest to it, as a constant: no rounding mode of the machine can move
 * it. It is the product rounded toward zero (the EE's rounding), 1 ulp under the product to nearest. */
#define ASPECT_WIDE_BITS 0x3FF3CF3Du
/* 3/4 of 640 / 448 (= 15 / 14), the float nearest to it: the frustum's aspect at level 2 (below). */
#define ASPECT_FRUSTUM_BITS 0x3F892492u

f32 Pal_CameraAspect(void)
{
    if (sLevel) {
        u32 bits = ASPECT_WIDE_BITS;
        f32 aspect;
        __builtin_memcpy(&aspect, &bits, 4);
        return aspect;
    }
    return 1.4285715f;
}

/* Frustum_Build's pyramid has the half width 320 (against the half height 240); at level 2 the x planes take 4/3 of
 * it, the float nearest to 1280 / 3: 0x43D55555. */
f32 Pal_ViewHalfWidth(f32 halfW)
{
    if (sLevel >= 2 && halfW == 320.0f) {
        u32 bits = 0x43D55555u;
        f32 wide;
        __builtin_memcpy(&wide, &bits, 4);
        return wide;
    }
    return halfW;
}

/* The frustum's planes are built with tan(fov / 2) / aspect for x while the projection divides x by its aspect,
 * so the planes cover 1 / aspect^2 of the view's width. With the wide aspect a (4/3 of a4) they would cover
 * 3/4 of what the 4:3 game covers; a4 * 3/4 keeps the 4:3 ratio of planes to view (a4^2 / a). */
f32 Pal_FrustumAspect(void)
{
    if (sLevel >= 2) {
        u32 bits = ASPECT_FRUSTUM_BITS;
        f32 aspect;
        __builtin_memcpy(&aspect, &bits, 4);
        return aspect;
    }
    return Pal_CameraAspect();
}
