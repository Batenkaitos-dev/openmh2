#ifndef PAL_FREECAM_H
#define PAL_FREECAM_H
/*
 * The freecam's core side (pal_freecam.c; the game's side is include/pal.h's PC_FREECAM_* macros).
 *
 * The pose is absolute, in the game's world: where the camera is, which way it looks, how it rolls, how wide it sees.
 * The yaw and the pitch give the direction as (sin(yaw) cos(pitch), sin(pitch), cos(yaw) cos(pitch)), the game's Y up; the
 * angles are radians. The host's controller keeps one and hands it to Pal_FreecamSetPose before each frame
 * (the IPC's mailbox block); with `on` 0, or never set, the freecam does nothing.
 */
#include "pal_core.h"

typedef struct PalFreecamPose {
    s32 on;         /* 1: the freecam has the view; 0: the game's camera */
    f32 eye[3];     /* the camera's position */
    f32 yaw;        /* radians */
    f32 pitch;      /* radians, positive looks up */
    f32 roll;       /* radians */
    f32 fov;        /* radians; 0 or less: the game's */
    u32 hide;       /* the stages of Frame_Draw to leave out, a bit for each (include/pal.h's PAL_STAGE_*); the fades and the loading
                       letters are the scene's and are never left out; read only while `on` */
} PalFreecamPose;

/* The pose in force from the next field frame's draw on. */
void Pal_FreecamSetPose(const PalFreecamPose *pose);

/*
 * The game's own camera as of the last field frame's draw (the camera the freecam starts from): its position, its
 * target and its fov. Returns 0 until a field frame has drawn.
 */
s32 Pal_FreecamGameCamera(f32 eye[3], f32 at[3], f32 *fov);

/* 1 when the last field frame's draw was made from the freecam's view. */
s32 Pal_FreecamActive(void);

/*
 * The frozen frame (the pause) and the step. The host asks with Pal_FreezeSet once per VBlank: HOLD 1 asks for the freeze, and
 * GRANT is a count that only grows, the real frames the host has let run since the start; the core runs one real frame for each
 * count it has not yet taken, and goes back to frozen frames after it. While the freeze is asked for and not yet in force
 * the game goes on, and the state says why (a freeze takes hold at a step after a field scene's draw: a quest in the field, the
 * village, the dojo; not in menus, the game's own pause or its results, not while a movie is open or a memory card request is
 * running). A frozen frame is a VBlank: the sound goes on (the host fades it out while the state is FROZEN), the VSync counter,
 * the timers and the IOP advance, and the game's tasks stand still. The pose is read at each frozen frame's draw.
 */
enum { PAL_FREEZE_RUNNING = 0, PAL_FREEZE_WAITING = 1, PAL_FREEZE_FROZEN = 2 };
enum { PAL_FREEZE_WHY_NONE = 0, PAL_FREEZE_WHY_SCENE = 1, PAL_FREEZE_WHY_MOVIE = 2, PAL_FREEZE_WHY_CARD = 3 };
void Pal_FreezeSet(s32 hold, u32 grant);
/* The state; WHY (when waiting) and TAKEN (the grants the core has used) may be null. */
s32 Pal_FreezeState(s32 *why, u32 *taken);


#endif
