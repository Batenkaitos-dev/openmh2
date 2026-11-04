#ifndef MH2_CSE_CSE_ENG_H
#define MH2_CSE_CSE_ENG_H
/* The native Capcom Sound Engine, the voice engine (cse_eng.c): CSE.IRX's functions 0x22E8 to 0x5300 and the SPU shadow
 * driver 0x5B30 to 0x6938, written from the module's code, with
 * the module's functions, their order and its data layout. cse_iop.c is the rest of the module (threads, servers,
 * start-up) and provides the kernel, libsd and queue calls this file makes. */
#include "snd/snd_iop.h"

/* provided by cse_iop.c */
u32 CseK(const char *name, u32 a0, u32 a1, u32 a2, u32 a3);   /* a kernel call of the module (IopNative_Call) */
u32 CseSd(u32 fn, u32 a, u32 b, u32 c, u32 d);                 /* a libsd call */
u32 CseSdW(u32 fn, u32 a, u32 b, u32 c, u32 d);                /* a libsd call between CpuSuspendIntr and CpuResumeIntr */
int CseDeq(u32 *w16);                                          /* 0x2240: the oldest queued request; -1 if none, else the rest */
void CseEffMsg(u32 core, u32 mode, s32 depth, u32 delay, u32 fb, u32 end);   /* 0x708: a message to the effect thread */
void CseSysClock(u32 stat);                                        /* the SysClock2USec call of the 250th round */
u32 CseScr(u32 n);                                             /* a scratch address on the running thread's side */

enum { SD_INIT = 4, SD_SETPARAM = 5, SD_GETPARAM = 6, SD_SETSWITCH = 7, SD_SETADDR = 9, SD_GETADDR = 10,
       SD_SETCOREATTR = 11, SD_GETCOREATTR = 12, SD_NOTE2PITCH = 13, SD_VOICETRANS = 17, SD_VTSTATUS = 19,
       SD_SETEFFECTATTR = 23, SD_GETEFFECTATTR = 24, SD_CLEAREFFECT = 25, SD_SETTRANSINTR = 26, SD_GETTRANSARG = 28 };

/* provided by cse_eng.c */
void CseEng_SpuInit(void);                     /* 0x5B30: the shadow's reset and its first synchronisation */
void CseEng_Init(u32 effEnd);                  /* 0x4FB8: the engine's reset (effEnd: the stack residue of its effect requests) */
void CseEng_Poll(void);                        /* 0x68B4: the voice envelopes and the mixer setting, read */
u32 CseEng_Callback(u32 n);                    /* 0x2D6C: one tick of the voice engine: the volumes, the voices, the queued requests */
void CseEng_Drive(void);                       /* 0x6878: the shadow written to the SPU */
u32 CseEng_Stat(u32 req, u32 reply);           /* 0x2BEC: a request of the STAT server; reply is the module's reply buffer */
#endif
