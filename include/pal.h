#ifndef PAL_H
#define PAL_H

/*
 * The platform hooks the game's C calls on PC: the hardware the PS2's code touched directly, which on PC is
 * the platform layer's. Only PLATFORM_PC builds declare them; the matching
 * build sees nothing of this file.
 *
 * This is the one list: mh2-core's stub platform layer is generated from the
 * prototypes below (one per line, named Pal_*), so a hook is added here
 * first. The macros that use them are in hw_regs.h (the registers), dma.h
 * and ram_areas.h.
 */
#include "platform.h"
#include "common.h"

/*
 * The constructs the PS2's code spells with an instruction or with nothing,
 * which the platform layer does for the PC:
 * - PAL_POLL(): the mark of a "not ready" result or of a wait loop's
 *   iteration (Pal_Poll below). Nothing on the PS2.
 * - PAL_SYNC_EI(): `sync.l; ei`, which ends an interrupt handler. On PC,
 *   Pal_SyncEi.
 * - PAL_NOPS_8(), PAL_NOPS_7(): a run of nops, the body of a busy-wait
 *   delay. A delay takes no time on PC (section 3.6), so they are nothing.
 * - PC_ASPECT(): the camera's aspect ratio, the frame buffer's width over its height (640 / 448). The literal
 *   on the PS2. On PC it is the same unless the widescreen enhancement is on (off by default), when it is 4/3 wider (Pal_CameraAspect below): the projection (Camera_ApplyProjection), the frustum's
 *   planes (Camera_BuildFrustum) and the rail camera's yaw limit take it from here, so the picture is drawn
 *   for a 16:9 display. Other aspects (the equipment preview's own) stay literals.
 * - PC_FRUSTUM_ASPECT(): the aspect the frustum's planes (Camera_BuildFrustum) are built with: PC_ASPECT() until
 *   the widescreen enhancement's second level, then 3/4 of the 4:3 aspect, so that the planes cover the wider view.
 * - PC_VIEW_X(halfW): the half width of the player-in-view pyramid (Frustum_Build), halfW itself until the widescreen
 *   enhancement's second level, then 4/3 of it: the test follows the wider picture.
 * - PC_HUD_2D(), PC_HUD_PACKET(p, w), PC_HUD_PIVOT(x), PC_HUD_PIVOT_END(): the 2D picture's shape under the
 *   widescreen enhancement (its second level: the wider picture is shown at 16:9, so the 2D elements are drawn
 *   3/4 as wide about the picture's centre and keep their shape). A 2D emitter (src/main/prim.c) marks the packet it
 *   is about to send with PC_HUD_2D(); Prim_SendPacket's PC_HUD_PACKET(p, frame width) rewrites the marked packet's
 *   x coordinates. An element placed from a 3D point (the target marker) names that point's x as the pivot, in the
 *   frame's pixels, around its drawing: PC_HUD_PIVOT(x) ... PC_HUD_PIVOT_END(). Nothing on the PS2.
 * - PC_FREECAM_FIELD(on), PC_FREECAM_BEGIN(objs), PC_FREECAM_END(): the freecam's seam (pc/core/pal_freecam.c). The scenes
 *   that draw the world (a quest in the field, the village) bracket their Frame_Draw with PC_FREECAM_FIELD(1) and (0): the
 *   freecam is on only for their draw. Frame_Draw starts with PC_FREECAM_BEGIN(&objs), which hands over the camera's objects
 *   (PalFreecamObjs below), and ends with PC_FREECAM_END(). Nothing on the PS2.
 * - PC_LOADING_SCREEN(on): the game's loading screen opens (Loading_Open: 1) or closes (Loading_Close: 0). The host's fast
 *   loading (pc/core/ipc/host_ipc.c) does not wait for real time while it is open; nothing the game computes changes. Nothing on
 *   the PS2.
 * - PC_TEXT(s) at the top of each of the text layer's entry points (text_layer.c, font.c): s is the string the function was
 *   given, and the text pack (if one is chosen) may answer with its replacement for it (pc/core/io/host_text.c); the same
 *   string without one. Nothing on the PS2.
 * - PC_FREECAM_STAGE(stage) in front of a draw statement names the stage of Frame_Draw (PAL_STAGE_* below) that follows: the
 *   statement runs as the game has it, and while the freecam's pose hides the stage its 2D packets draw nothing (in a field
 *   scene's draw only). Nothing on the PS2.
 */
#if PLATFORM_PS2
#define PAL_POLL() ((void)0)
#define PC_ASPECT() 1.4285715f
#define PC_FRUSTUM_ASPECT() 1.4285715f
#define PC_VIEW_X(halfW) (halfW)
#define PC_HUD_2D() ((void)0)
#define PC_HUD_PACKET(p, frameWidth) ((void)0)
#define PC_HUD_QUEUED(q) ((void)0)
#define PC_HUD_PIVOT(x) ((void)0)
#define PC_HUD_PIVOT_END() ((void)0)
#define PC_FREECAM_FIELD(on) ((void)0)
#define PC_FREECAM_BEGIN(objs) ((void)0)
#define PC_FREECAM_END() ((void)0)
#define PC_FREECAM_STAGE(stage)
#define PC_TEXT(s) ((void)0)
#define PC_LOADING_SCREEN(on) ((void)0)
#define PC_GS_CAMERA(view, proj) ((void)0)
#define PAL_SYNC_EI() __asm__ volatile ("sync.l\n\tei")
#define PAL_NOPS_8() __asm__ volatile ("nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop")
#define PAL_NOPS_7() __asm__ volatile ("nop\n nop\n nop\n nop\n nop\n nop\n nop")
#else
#define PAL_POLL() Pal_Poll()
#define PAL_SYNC_EI() Pal_SyncEi()
#define PAL_NOPS_8() ((void)0)
#define PAL_NOPS_7() ((void)0)
#define PC_ASPECT() Pal_CameraAspect()
#define PC_FRUSTUM_ASPECT() Pal_FrustumAspect()
#define PC_VIEW_X(halfW) Pal_ViewHalfWidth(halfW)
#define PC_HUD_2D() Pal_HudMark()
#define PC_HUD_PACKET(p, frameWidth) Pal_HudPacket((p), (frameWidth))
#define PC_HUD_QUEUED(q) Pal_HudQueued(q)
#define PC_HUD_PIVOT(x) Pal_HudPivot(x)
#define PC_HUD_PIVOT_END() Pal_HudPivotEnd()
#define PC_FREECAM_FIELD(on) Pal_FreecamField(on)
#define PC_FREECAM_BEGIN(objs) Pal_FreecamBegin(objs)
#define PC_FREECAM_END() Pal_FreecamEnd()
#define PC_FREECAM_STAGE(stage) Pal_FreecamStage(stage);
#define PC_TEXT(s) ((s) = (void *)Pal_Text((const char *)(s)))
#define PC_LOADING_SCREEN(on) Pal_LoadingScreen(on)
#define PC_GS_CAMERA(view, proj) Pal_GsCamera((view), (proj))
#endif

#if PLATFORM_PC

/*
 * The EE's RAM and the scratchpad (defined in mh2-core). Pal_EeRam holds the
 * RAM the game addresses as fixed areas, each at its PS2 offset: the address
 * of the PS2's 0x00AC8000 is Pal_EeRam + 0xAC8000. Pal_Spr is the
 * scratchpad's 16 KB (the PS2's 0x70000000 + offset is Pal_Spr + offset).
 * Both lie below 0x10000000, and on i686
 * their addresses are link-time constants, which a static initializer may
 * use. Both are page-aligned: the game moves quadwords through them (the
 * packet templates copied into the scratchpad, the DMA tags).
 */
extern u8 Pal_EeRam[0x2000000] __attribute__((aligned(4096)));
extern u8 Pal_Spr[0x4000] __attribute__((aligned(4096)));

/*
 * The block's address of a PS2 address, and the scratchpad's of an offset, as
 * integers (u32 is a pointer's size on i686). Both are address constants, so
 * a static initializer may hold one.
 */
#define PAL_RAM(addr) ((u32)(Pal_EeRam + (addr)))
#define PAL_SPR(offset) ((u32)(Pal_Spr + (offset)))

/*
 * The PS2 address of a host pointer into the game's data or the RAM block
 * (pc/core/data_c.h): what the game hands the IOP holds
 * PS2 addresses, so a builder that stores a pointer for the IOP stores this.
 * A pointer in neither stops the core.
 */
unsigned int DataC_Ps2Of(const void *p);

/*
 * The EE's hardware registers (the timers, VIF1 and the DMA channels) and
 * the GS's privileged registers: a read returns the register's value, a
 * write takes effect in the platform. addr is the register's address on the
 * PS2 (0x1000xxxx, 0x1200xxxx; include/hw_regs.h names them). The game
 * reaches them through HW_RD32, HW_WR32, HW_RD64, HW_WR64 and HW_WR128,
 * which expand to these on PC.
 */
u32 Pal_EeRegRead32(u32 addr);
void Pal_EeRegWrite32(u32 addr, u32 value);
void Pal_EeRegWrite128(u32 addr, const void *qword);
u64 Pal_GsRegRead64(u32 addr);
void Pal_GsRegWrite64(u32 addr, u64 value);

/*
 * `sync.l; ei` at the end of an interrupt handler: the EE's EI, the kernel's
 * interrupt enable, as the handler sets it before it returns. In a handler it
 * changes only what the handler reads back; the interrupts raised meanwhile
 * are taken at the return.
 */
void Pal_SyncEi(void);

/*
 * The camera's aspect ratio (PC_ASPECT above): 1.4285715f (640 / 448), or 4/3 wider (the float nearest to
 * 640 / 448 * 4/3, a constant) when the widescreen setting is on. The setting is the core's --widescreen
 * (pc/core/pal_camera.c).
 */
f32 Pal_CameraAspect(void);

/*
 * The widescreen enhancement's level: 0 off, 1 the camera (the projection, the frustum, the rail camera's yaw limit),
 * 2 the camera and the 2D picture's shape and the view tests (pc/core/pal_camera.c). Pal_FrustumAspect is the aspect
 * of Camera_BuildFrustum's planes: Pal_CameraAspect() below level 2, 3/4 of the 4:3 aspect from level 2 on.
 */
s32 Pal_WidescreenLevel(void);
f32 Pal_FrustumAspect(void);
f32 Pal_ViewHalfWidth(f32 halfW);   /* PC_VIEW_X: 320 becomes 4/3 of it at level 2, any other value stays */

/*
 * The offline hub enhancement's setting (pc/core/pal_hub.c): 1 when the village's gate leads to the online town played
 * offline (the core's --offline-hub), else 0. Off by default; with it off the game is Dos as it was. Game code asks it
 * only inside PLATFORM_PC blocks.
 */
s32 Pal_OfflineHub(void);

/*
 * The 2D picture's shape (pc/core/pal_hud.c; level 2): Pal_HudMark marks the packet the emitter is about to send,
 * Pal_HudPacket(packet, frameWidth) rewrites the x coordinates of a marked packet (the GS's 12.4 fixed point, in
 * integers: x' = c + (x - c) * 3/4 about the pivot c, the picture's centre unless Pal_HudPivot named a point) and
 * leaves a primitive that spans the frame's whole width alone (a fade, a background, a letterbox bar).
 * Pal_HudPivot(x): the pivot, in the 640-wide coordinates the emitters take; Pal_HudPivotEnd: the centre again.
 */
void Pal_HudMark(void);
/* PC_TEXT: the text pack's replacement for a string, or the string (pc/core/pal_text.c). */
const char *Pal_Text(const char *s);
void Pal_HudPacket(u32 *packet, s32 frameWidth);
void Pal_HudPivot(s32 x);
void Pal_HudPivotEnd(void);
/*
 * The freecam's hide (photo mode; pc/core/pal_hud.c): while Pal_HudHide(1) is set, a marked 2D packet keeps its place in the
 * buffers and the queue and draws nothing (its vertices go outside the scissor), so every stage runs as the game has it.
 * Set by Pal_FreecamStage for the stage in progress. Pal_HudHideStats: the packets hidden, and those of a shape it can't read.
 */
void Pal_HudHide(s32 on);
void Pal_HudQueued(u32 *queued);   /* PC_HUD_QUEUED: the queued packet, after PC_HUD_PACKET */
void Pal_HudHideStats(u32 *hidden, u32 *unparsed);

/*
 * The freecam's seam (pc/core/pal_freecam.c). The game hands over the objects the camera publishes and the draw reads:
 * the layer takes no address of game memory from anywhere else. Pal_FreecamBegin, at the start of Frame_Draw, puts the
 * freecam's view in them for the length of the draw when a pose is in force and Pal_FreecamField(1) says it is a field
 * scene's draw; Pal_FreecamEnd puts back each one that still holds the value Pal_FreecamBegin put there (the draw
 * writes the projection itself). With no pose, both do nothing.
 * view: the view matrix the draw's geometry takes (gMtxView, slot 33); viewCopy: gViewMatrix; camMatrix: gCamMatrix, the
 * view matrix's inverse (the billboards, the sky bands, the lights, the shadows); camYaw: gCamYawMtx. All 4 x 4 f32.
 * eye, at, up, fov, fovPrev, roll: the camera struct's. proj: what applyProjection (Camera_ApplyProjection) writes from
 * the camera's fov: the projection slot, the viewport matrix, the clip and screen projections, the frustum planes.
 */
typedef struct PalFreecamItem {
    void *p;
    u32 bytes;
} PalFreecamItem;

typedef struct PalFreecamObjs {
    f32 *view;
    f32 *viewCopy;
    f32 *camMatrix;
    f32 *camYaw;
    f32 *eye;
    f32 *at;
    f32 *up;
    f32 *fov;
    f32 *fovPrev;
    f32 *roll;
    PalFreecamItem proj[5];
    void (*applyProjection)(void);
} PalFreecamObjs;

void Pal_FreecamField(s32 on);
void Pal_FreecamBegin(const PalFreecamObjs *objs);
void Pal_FreecamEnd(void);

/*
 * The stages of Frame_Draw after the world, in the order of the code (the freecam pose's hide mask has a bit for each). The
 * scene's own stages (the two fades and the loading letters) are never hidden by a pose.
 */
enum {
    PAL_STAGE_PRIM = 0,      /* the primitives (Prim_DrawAll) */
    PAL_STAGE_TEXT6,         /* text list 6 and page 6 */
    PAL_STAGE_TEXT0,         /* lists 0a and 0b and page 0 */
    PAL_STAGE_TEXT1,         /* list 1 and page 1 */
    PAL_STAGE_EQUIP,         /* the equipment preview */
    PAL_STAGE_TEXT2,         /* list 2 and page 2 */
    PAL_STAGE_TEXT4,         /* list 4 and page 4 */
    PAL_STAGE_OT3,           /* text layer 3's ordering table */
    PAL_STAGE_FADE_B,        /* FadeB_Draw (the scene's) */
    PAL_STAGE_TEXT3,         /* page 3 */
    PAL_STAGE_TEXT5,         /* list 5 and page 5 */
    PAL_STAGE_FADE_A,        /* FadeA_Draw, from Callbacks_Run (the scene's) */
    PAL_STAGE_LOADING,       /* Loading_DrawLetters, from Callbacks_Run (the scene's) */
    PAL_STAGE_COUNT
};
/* The stage that is about to draw (PC_FREECAM_STAGE): while the pose hides it, its 2D packets draw nothing (Pal_HudHide). Every
 * stage runs. A stage outside the list (PAL_STAGE_COUNT) is the end of the stages. */
void Pal_FreecamStage(s32 stage);

/* The loading screen is open (1) or closed (0): the pacer's signal for fast loading (pc/core/ipc/ipc_glue.c). */
void Pal_LoadingScreen(s32 on);

/*
 * The camera a model is drawn with (Model_Draw: gMtxView and gMtxProjClip, the two matrices the packet builders multiply a model's own
 * matrix by), handed to the renderer for the in-between pictures of a higher frame rate. Pal_GsCamera sends the 32 floats only when
 * they differ from the last it sent. It changes nothing the game sees. Nothing on the PS2.
 */
void Pal_GsCamera(const f32 *view, const f32 *proj);

/*
 * Asked at the start of each step of the game main loop (SubMain_Step): 1 when this step is a frozen frame (the last field
 * frame's draw again and the flip, nothing else), 0 for a real frame, 2 for the first real frame after frozen ones (its flip
 * timing is put back). See pc/core/pal_freecam.h.
 */
s32 Pal_FreezeStep(void);

/*
 * A poll event: the game's code is in a spin and
 * its test said "not ready". The polled-wait table lists the spins. The original's "not ready" is the taken
 * backward branch to a listed loop head (a `loop` record), or the return of a listed library function whose
 * result says so (a `ret` record). The game's C calls PAL_POLL() exactly there:
 *   - where the loop decides to go round again: after the test came out true, before the body runs again.
 *     A mark in a loop that waits for nothing else, or at the end of a loop that ends without going round,
 *     is wrong: the mark runs once per taken branch, not once per iteration of the body;
 *   - in a loop MWCC rotates (`while (test) { body }` becomes `if (test) do { body } while (test)`), the
 *     first test is not a branch back, so the PC form is `if (test) for (;;) { body; if (!test) break;
 *     PAL_POLL(); }` (FileQueue_Flush, Afs_ReadFileIdle);
 *   - in a `do { } while (test)` the mark follows the test: `if (test) PAL_POLL();` ends the body
 *     (OpenAfs's retry); an empty `while (test) { PAL_POLL(); }` is the same;
 *   - a `for (;;)` with its tests inside marks at its end;
 *   - a read the PS2 does once stays one read (Dma_StoreImage's gDmaRecvQwc).
 * A "ready" result calls nothing. The platform layer's versions of the library functions the file lists mark
 * their own results.
 *
 * It is a poll event when the previous one came with no wait in between: the instant is then settled (the
 * due events, the IOP, the SIF and the deliverable interrupts), and time moves to the next event once it is
 * settled. The first "not ready" after a wait only notes it and returns at once: a query is not a spin. In
 * an interrupt handler a poll event moves time and fires events but delivers nothing. The original game's
 * equivalent is sched_poll_event.
 */
void Pal_Poll(void);

#endif /* PLATFORM_PC */

#endif /* PAL_H */
