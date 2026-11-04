#ifndef MH2_KERNEL_KSIF_H
#define MH2_KERNEL_KSIF_H

/* The SIF's internals (sif.c), for the SIF's API (api/libkernl_sif.c) and the tests. */
#include "kernel/kern_int.h"

typedef struct KSifPacket KSifPacket;
struct KSifPacket {
    void (*fn)(const KSifPacket *p);   /* what the SIF's DMAC handler does with it */
    u32 a, b, c;
    void *ptr;
    u8 data[112];                      /* an IOP-to-EE command's payload (a bytes) */
};

void KSif_Queue(const KSifPacket *p);
void KSif_DropQueue(void);
void KSif_SetBufferKnown(int known);
int KSif_BufferKnown(void);
s32 KSif_DmacHandler(s32 cause, void *arg);
int KSif_RpcUp(void);                     /* sceSifInitRpc ran since the last reboot */
void KSif_SetRpcUp(int up);

const KernSifService *KSif_FindService(u32 id);
const KernSifService *KSif_ServiceByServer(u32 server);
int KSif_ServerKnown(u32 server);          /* a bind answered with this server record */
void KSif_ServiceAddresses(u32 id, u32 *server, u32 *buff);

void KSif_RpcReset(void);
int KSif_PacketAlloc(u32 *addr, u32 *num);
void KSif_PacketFree(u32 addr);
int KSif_PacketBusy(u32 addr, u32 num);

u32 KSif_Sreg(u32 n);
void KSif_SetSreg(u32 n, u32 v);
int KSif_BootEnd(void);
void KSif_Reboot(const char *img);

s32 KSif_LoadModule(const char *path, u32 iopAddr, s32 argsize, const char *args, s32 *result);
s32 KSif_SearchModule(const char *name);
void KSif_Sif1Write(u32 dest, const void *src, u32 size);
/* The next SIF DMA transfer id: once per SifSetDma the PS2 makes, SifSetDma's own and each command the SIF
 * library sends the IOP (INIT_CMD, RESET, BIND, CALL: one SifSetDma each). */
u32 KSif_DmaId(void);
/* IOP-to-EE commands: the EE's handlers (sceSifAddCmdHandler), and a service's call, during which the commands
 * it sends wait for its RPC_END (queued at KSif_EndServiceCall). */
void KSif_SetCmdHandler(u32 cid, KernSifCmdFn fn, void *data);
void KSif_BeginServiceCall(void);
u32 KSif_RegRead(u32 addr);
void KSif_RegWrite(u32 addr, u32 value);
void KSif_EndServiceCall(void);

#endif
