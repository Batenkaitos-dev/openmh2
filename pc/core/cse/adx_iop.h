#ifndef MH2_CSE_ADX_IOP_H
#define MH2_CSE_ADX_IOP_H
/* The native CRI_ADX_Driver: the IOP side of CRI ADX's players, written from its
 * protocol and its behaviour at the interface (the RPCs of server 0x90000200, the status blocks the EE and the IOP
 * pass each VBlank, the rings, the libsd calls and the staging buffer the SPU2 plays), not from the module's code.
 * It runs inside the setting `audio cse` (api/libsd.c), the core's only one: cse_iop.h. */
#include "snd/snd_iop.h"

extern const IopNativeModule g_adx_iop;
#endif
