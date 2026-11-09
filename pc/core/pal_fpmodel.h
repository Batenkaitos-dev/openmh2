#ifndef PAL_FPMODEL_H
#define PAL_FPMODEL_H
/* The float model: legacy or ps2 (pal_fpmodel.c; the models are described in pc/core/crt/ps2fp.h). */
#include "pal_core.h"

/* The build's default: "ps2", the console's float results (the owner, 2026-11-06). A session loaded from a file that has no fpu_model
 * line is "legacy" whatever this says: it was made before the ps2 model. */
#define PAL_FPU_MODEL_DEFAULT "ps2"

/* main's option loop: 2 (two arguments taken) when argv[i] is --fpu-model legacy|ps2, else 0. */
s32 Pal_FpuModelOption(s32 argc, char **argv, s32 i);

/* The model in force: 1 ps2, 0 legacy (a save state is good for the model that made it: kernel/state.c). */
s32 Pal_FpuModelId(void);

/* After the options are read, before the game starts: sets the model in force (a repeated session's is its own). */
void Pal_FpuModelStart(void);

#endif
