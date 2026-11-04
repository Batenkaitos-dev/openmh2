#ifndef MH2_CSE_CSE_IOP_H
#define MH2_CSE_CSE_IOP_H
/* The native Capcom Sound Engine: CSE.IRX's IOP side. The start-up, the threads and the servers
 * were written from the recorder's streams and the module's behaviour at its interfaces (the RPC servers COMM, STAT and THMN, the kernel
 * calls, the libsd calls and the replies' bytes); the voice engine (cse_eng.c) and most of cse_iop.c are written from the
 * module's code (CSE.IRX is Capcom's: the ruling on question 15). Run-time setting `audio cse`
 * (api/libsd.c; cse is the only mode of the core), cumulative: libsd, the ADX driver and this module. Equal to the original game
 * on the streams it records (--sd-record: the kernel and libsd calls with their arguments and results, the RPC requests and
 * replies). */
#include "snd/snd_iop.h"

extern const IopNativeModule g_cse_iop;
#endif
