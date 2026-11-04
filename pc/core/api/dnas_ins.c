/* DNAS's install library (the dnas_ins overlay, Sony's): the network's
 * offline answers.
 *
 * Capcom's only calls are NetXfer_Step (the lobby's downloaded data, online)
 * and NetXfer_Step2 (src/sub_main/dnas_check.c), which passes a patch
 * file's data (recsSize bytes, as DNAS encoded them; Patch_LoadStep copies
 * them to the buffer after the file) through the library, which gives the
 * records PatchExecCS applies: at state 0
 * func_AE2518() (a result below 0 fails), at state 10 func_AE2588(size,
 * buf, &out) (below 0 fails; out is the size of what comes out, which must
 * fit maxSize), at 11 func_AE2EC8(handle, out), then func_AE2570() at the
 * end and on a failure. NetXfer_Step2 runs for a patch on a card (Patch_LoadStep, from
 * CardBoot_LoadPatch, CardSys_LoadPatch, CardNew_LoadPatch and
 * CardLoad_LoadPatch, when the card's patch file has records and a newer
 * version than the game's) and for one from the network (LobbyPatch_Install).
 *
 * The port has no DNAS (port-plan D4: until a patch for version 1.04 is
 * known, the PC port reports a patch and doesn't apply it). So the start
 * succeeds, the size step reports the patch in the log and fails, and the
 * end does nothing. NetXfer_Step2 then fails (-1), Patch_LoadStep gives
 * PATCH_FAILED, and each of the four card callers goes on without
 * Patch_InstallFile, as it does for a patch that is older or has no records:
 * nothing reaches PATCH_RECS, so PatchExecCS applies nothing. The records'
 * buffer and out are left as they are. State 11's handle (the game's
 * undefined local, plan 14) is never reached. NetXfer_Step's other pair
 * (func_AE8638, func_AE8688) stays an aborting stub: online only. */
#include "common.h"
#include "io/io.h"

s32 func_AE2518(void);
s32 func_AE2588(s32 size, s32 buf, s32 *out);
void func_AE2570(void);

s32 func_AE2518(void)
{
    return 0;
}

s32 func_AE2588(s32 size, s32 buf, s32 *out)
{
    (void)buf;
    (void)out;
    Io_Log("patch: a patch (%d bytes of DNAS-encoded records) is not applied: the PC port has no DNAS (port-plan D4)",
           (u32)size, 0);
    return -1;
}

void func_AE2570(void)
{
}
