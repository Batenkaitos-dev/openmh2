#ifndef PAL_CAMERA_H
#define PAL_CAMERA_H
/* The camera's aspect ratio and the widescreen setting (pal_camera.c; include/pal.h has Pal_CameraAspect). */
#include "pal_core.h"

/* main's option loop: 1 (one argument taken) when argv[i] is --widescreen. */
s32 Pal_WidescreenOption(s32 argc, char **argv, s32 i);

/* After the options are read, before the game starts: the setting in force. */
void Pal_WidescreenStart(void);

#endif
