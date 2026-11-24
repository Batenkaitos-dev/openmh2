#ifndef GFX_PASS_PREFIX_H
#define GFX_PASS_PREFIX_H

/*
 * The second pass's own copy of the units.
 *
 * With MH2_PASS_THREAD=1 the second pass runs on a thread of its own, on its own VIF1, VU1 and GIF: gfx_pass_vif.c, gfx_pass_vu1.c,
 * gfx_pass_gif.c, gfx_pass_vu1gen.c and gfx_pass_run.c (which holds gfx_pass.c) include this header first and then the real file, so
 * that every name the real file defines for other files, and the data it keeps, becomes a name with the prefix pp_. The real units are
 * not touched: they are compiled once as they were, and the game's thread alone uses them. Where a unit calls something of the game
 * (the GS, the arena, the anomaly counter, the DMA's resume), the copy's call goes to a function of gfx_pass_run.c that does what the
 * pass needs there and touches nothing of the game's.
 *
 * What the copy's objects may reference outside themselves is a short list, written in the development check that holds it:
 * the C library, the pp_ functions of gfx_pass_run.c, the host's allocator and log, the IPC's clock, and the address of Pal_EeRam.
 */

#ifdef MH2_PASS_COPY
#error "gfx_pass_prefix.h included twice"
#endif
#define MH2_PASS_COPY 1

/* Which copy this is (the build gives -DGFX_INST or a generated file defines it first): 0 the pass thread's, the deciding one (pp_), 1 to 5 the helpers' (pp1_ to pp5_, gfx_pass_help.c). Every
 * copy has its own VIF1, VU1, GIF, compiled microprograms and gfx_pass.c; the names of one never meet the names of another. */
#ifndef GFX_INST
#define GFX_INST 0
#endif
#if GFX_INST == 0
#define PPN(x) pp_##x
#elif GFX_INST == 1
#define PPN(x) pp1_##x
#elif GFX_INST == 2
#define PPN(x) pp2_##x
#elif GFX_INST == 3
#define PPN(x) pp3_##x
#elif GFX_INST == 4
#define PPN(x) pp4_##x
#elif GFX_INST == 5
#define PPN(x) pp5_##x
#else
#error "gfx_pass_prefix.h: GFX_INST is 0 to 5"
#endif

/* gfx_vif.c */
#define vif1_itop PPN(vif1_itop)
#define vif1_top PPN(vif1_top)
#define vif_init PPN(vif_init)
#define vif_read_reg PPN(vif_read_reg)
#define vif_state PPN(vif_state)
#define vif_write PPN(vif_write)
#define vif_write_reg PPN(vif_write_reg)
#define vif_write_tag PPN(vif_write_tag)
#define vif_image_pages PPN(vif_image_pages)
#define g_vu_mem PPN(g_vu_mem)
/* gfx_vu1.c */
#define Gfx_Vu1Check PPN(Gfx_Vu1Check)
#define Gfx_Vu1CheckCounts PPN(Gfx_Vu1CheckCounts)
#define Gfx_Vu1UseCompiled PPN(Gfx_Vu1UseCompiled)
#define vu1_advance_slow PPN(vu1_advance_slow)
#define vu1_clip PPN(vu1_clip)
#define vu1_continue PPN(vu1_continue)
#define vu1_decode PPN(vu1_decode)
#define vu1_efu PPN(vu1_efu)
#define vu1_efu_calc PPN(vu1_efu_calc)
#define vu1_efu_latency PPN(vu1_efu_latency)
#define vu1_hash_bytes PPN(vu1_hash_bytes)
#define vu1_lower_undefined PPN(vu1_lower_undefined)
#define vu1_opm PPN(vu1_opm)
#define vu1_opm_calc PPN(vu1_opm_calc)
#define vu1_reset PPN(vu1_reset)
#define vu1_start PPN(vu1_start)
#define vu1_state PPN(vu1_state)
#define vu1_stats PPN(vu1_stats)
#define vu1_upper_undefined PPN(vu1_upper_undefined)
#define vu1_vdiv PPN(vu1_vdiv)
#define vu1_xgkick PPN(vu1_xgkick)
#define g_vu1 PPN(g_vu1)
#define g_vu1_gen_bails PPN(g_vu1_gen_bails)
#define g_vu1_kick_hash PPN(g_vu1_kick_hash)
#define g_vu1_kick_hashing PPN(g_vu1_kick_hashing)
#define g_vu1_lanes PPN(g_vu1_lanes)
/* gfx_gif.c */
#define gif_flush_path3 PPN(gif_flush_path3)
#define gif_note_deferred PPN(gif_note_deferred)
#define gif_path3_masked PPN(gif_path3_masked)
#define gif_path_write PPN(gif_path_write)
#define gif_pending PPN(gif_pending)
#define gif_read_reg PPN(gif_read_reg)
#define gif_reset_all PPN(gif_reset_all)
#define gif_sandbox PPN(gif_sandbox)
#define gif_sandbox_clear PPN(gif_sandbox_clear)
#define gif_sandbox_same PPN(gif_sandbox_same)
#define gif_set_path3_mask PPN(gif_set_path3_mask)
#define gif_state PPN(gif_state)
#define gif_write_reg PPN(gif_write_reg)
#define gif_real_export PPN(gif_real_export)
#define gif_real_import PPN(gif_real_import)
/* vu1_gen.c (the compiled microprograms) */
#define vu1_gen_exec PPN(vu1_gen_exec)
#define vu1_gen_selfcheck PPN(vu1_gen_selfcheck)
/* gfx_pass.c */
#define Gfx_PassFinish PPN(Gfx_PassFinish)
#define Pass_Data PPN(Pass_Data)
#define Pass_Model PPN(Pass_Model)
#define Pass_Out PPN(Pass_Out)
#define Pass_Real PPN(Pass_Real)
#define Pass_StepEnd PPN(Pass_StepEnd)
#define Pass_VBlankStart PPN(Pass_VBlankStart)
#define Pass_Tag PPN(Pass_Tag)
#define Pass_TagEnd PPN(Pass_TagEnd)
#define Pass_WindowBegin PPN(Pass_WindowBegin)
#define Pass_WindowEnd PPN(Pass_WindowEnd)
#define gGfxPassOpen PPN(gGfxPassOpen)
#define gGfxPassRun PPN(gGfxPassRun)
/* What the units call outside themselves: functions of gfx_pass_run.c with the game's nothing in them. */
#define gs_gif PPN(gs_gif)
#define gs_state PPN(gs_state)
#define Gfx_Oddity PPN(Gfx_Oddity)
#define hw_gif_dma_resume PPN(hw_gif_dma_resume)
#define Pal_ArenaAlloc PPN(Pal_ArenaAlloc)
#define Pal_ArenaFree PPN(Pal_ArenaFree)
#define Pal_Fatal PPN(Pal_Fatal)
#define DataC_TryAt PPN(DataC_TryAt)
/* the second pass's calls into the IPC: the copy's go to the pass thread's ring */
#define Ipc_GsPass PPN(Ipc_GsPass)
#define Ipc_StepReport PPN(Ipc_StepReport)
#define Ipc_GsKeepLabels PPN(Ipc_GsKeepLabels)

#endif
