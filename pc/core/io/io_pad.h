#ifndef MH2_CORE_IO_PAD_H
#define MH2_CORE_IO_PAD_H

/*
 * The pads: the IOP's side of libdbc and libpad2 (Dbc_Manager with the
 * DualShock 2 driver; io_pad.c, adapted from the original's) and the EE's clients (pc/core/api/libdbc.c,
 * libpad2.c, libvib.c). The request area is the original's 0x90 bytes, with
 * the same offsets; its addresses are the core's pointers.
 */
#include "common.h"

#define DBC_SID_MAIN 0x80001300u
#define DBC_SID_131C 0x8000131Cu
#define DBC_SID_SEND0 0x8000131Eu   /* and 0x8000131F: the sends, in turn */
#define DBC_FN_CREATE 0x80001301u
#define DBC_FN_CLOSE 0x80001302u
#define DBC_FN_QUERY 0x80001303u
#define DBC_FN_LINKS 0x80001304u
#define DBC_FN_RECEIVE 0x8000131Au
#define DBC_FN_VERSION 0x80001363u
#define DBC_SOCKETS 16
#define DBC_AREA 0x90

/* The state of a pad (as the original's PadState): buttons with 1 = pressed
 * (bit 0 SELECT ... bit 15 SQUARE, the DualShock 2's order), the sticks
 * (0x80 at rest) and the 12 pressures. */
typedef struct IoPadState {
    s32 connected;
    u16 buttons;
    u8 rx, ry, lx, ly;
    u8 pressure[12];
} IoPadState;

/* ---- what the other modules call ----
 * The state of port (0 or 1) from now on: the host's input
 * (the original game connects port 0 only: port 1 stays empty whatever is set). */
void Io_PadSetState(int port, const IoPadState *st);
void Io_PadGetState(int port, IoPadState *st);
/* At each VBlank's start, step 7: each socket's
 * newer half written with the pad's state. The kernel calls it. */
void Io_PadVBlank(void);
/* The motors as the game sets them (small: 0 or 1; large: 0-255), for the
 * host: a function pointer it may set; called when they change. */
extern void (*Io_PadRumble)(int port, u8 small, u8 large);

/* ---- libdbc's calls for libpad2 and libvib (api/libdbc.c) ---- */
s32 IoDbc_Command(s32 socket, u32 cmd, s32 *size, void *out);   /* the receive, with a wait */
s32 IoDbc_Send(s32 socket, u32 cmd, s32 *size, const void *data); /* a send, without a wait */
s32 IoDbc_Query(s32 socket);                                     /* the socket's set-up query */
s32 IoDbc_CreateSocket(const void *desc, void *buf0, void *buf1);
s32 IoDbc_CloseSocket(s32 socket);

void IoPad_Register(void);   /* the services (io_pad.c) */
void IoPad_Reset(void);      /* no sockets (tests) */

#endif
