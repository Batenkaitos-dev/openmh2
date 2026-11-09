/*
 * The float model (pc/core/crt/ps2fp.h): 0 "legacy", 1 "ps2", the console's results where known (exponent 255 a
 * number, 0x7FFFFFFF the largest value, the Booth multiplier, ...). A saved session names its model in its header
 * ("fpu_model ps2"; a session without the line is legacy), and a run that repeats it takes the model from there, whatever
 * its command line says. Any other run takes --fpu-model, else the build's default (ps2). Pal_FpuModelStart
 * runs once, after the options are read and before the game starts: no float operation of the game has run before
 * it. A release build saves no session: the model is the option, else the default.
 */
#include "pal_core.h"

#include "pal_fpmodel.h"
#include "host.h"

extern int ps2fp_model;   /* crt_fpmodel.c */

static const char *sOption;   /* --fpu-model's value */

static s32 SameString(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

s32 Pal_FpuModelOption(s32 argc, char **argv, s32 i)
{
    if (!SameString(argv[i], "--fpu-model")) {
        return 0;
    }
    if (i + 1 >= argc || (!SameString(argv[i + 1], "legacy") && !SameString(argv[i + 1], "ps2"))) {
        Host_Fatal("--fpu-model: legacy or ps2");
    }
    sOption = argv[i + 1];
    return 2;
}

s32 Pal_FpuModelId(void)
{
    return ps2fp_model != 0;
}

void Pal_FpuModelStart(void)
{
    const char *want = sOption ? sOption : PAL_FPU_MODEL_DEFAULT;
    const char *use = want;
    ps2fp_model = SameString(use, "ps2");
    if (ps2fp_model) {
        Host_Log("float model: ps2 (the console's results where known; exponent 255 is a number, the Booth multiplier)");
    }
}
