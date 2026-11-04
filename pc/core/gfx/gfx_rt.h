#ifndef MH2_GFX_RT_H
#define MH2_GFX_RT_H

/*
 * The graphics front's own header:
 * the names its files share, and the original game runtime's names
 * that the files adapted from the original game's code (vif.c, gif.c, vu1.c, vu1_core.h) still
 * use. Platform-layer code: the game's calling convention, no system headers
 * (pc/README.md, "Calling convention"); the SSE intrinsics are the compiler's
 * own headers.
 */

/* clang's xmmintrin.h includes mm_malloc.h, which includes <stdlib.h>: this
 * guard keeps the system header out. Nothing here uses _mm_malloc. */
#ifndef __MM_MALLOC_H
#define __MM_MALLOC_H
#endif
#include <emmintrin.h>

#include "common.h"
#include "gfx.h"
#include "kernel/kern.h"

typedef __SIZE_TYPE__ size_t;

typedef union GPR {
    u64 d[2];
    s64 sd[2];
    u32 w[4];
    s32 sw[4];
    u16 h[8];
    s16 sh[8];
    u8 b[16];
    s8 sb[16];
} GPR;

typedef union VF {
    float f[4];
    u32 u[4];
    s32 s[4];
} VF;

/* The EE's float operands and results: exponent 255 is
 * an ordinary (huge) number, denormals are 0; overflow saturates to +/-Fmax,
 * underflow flushes to +/-0. */
#define FMAX_BITS 0x7F7FFFFFu
static inline u32 fbits(float f) { u32 u; __builtin_memcpy(&u, &f, 4); return u; }
static inline float fval(u32 u) { float f; __builtin_memcpy(&f, &u, 4); return f; }
static inline float fin(u32 u)
{
    u32 e = u & 0x7F800000u;
    if (e == 0x7F800000u)
        u = (u & 0x80000000u) | FMAX_BITS;
    else if (e == 0)
        u &= 0x80000000u;
    return fval(u);
}
static inline u32 fout(float f)
{
    u32 u = fbits(f);
    u32 e = u & 0x7F800000u;
    if (e == 0x7F800000u)
        return (u & 0x80000000u) | FMAX_BITS;
    if (e == 0)
        return u & 0x80000000u;
    return u;
}

#define memcpy __builtin_memcpy
#define memmove __builtin_memmove
#define memset __builtin_memset
#define memcmp __builtin_memcmp
#define offsetof __builtin_offsetof
#define UINT64_MAX 0xFFFFFFFFFFFFFFFFull
#ifndef NULL
#define NULL ((void *)0)
#endif

/* The original's logs and profiler: nothing here. Its "logged once" anomalies
 * (a VIFcode or DMA mode the game never uses) are counted (Gfx_Oddities). */
#define LOGI(...) ((void)0)
#define LOGD(...) ((void)0)
#define LOGT(...) ((void)0)
#define LOGW(...) ((void)0)
#define LOGE(...) ((void)0)
#define prof_enter(x) ((void)0)
#define prof_leave() ((void)0)
#define rt_log_once(tag, key, ...) Gfx_Oddity(tag, (u32)(key))
#define rt_fatal(msg) Pal_Fatal(msg)
void Gfx_Oddity(const char *tag, u32 key);
void Pal_Fatal(const char *what) __attribute__((noreturn));

/* The memory the GS-side units keep: VU0's micro and data memory at 0x0000
 * and 0x4000, VU1's at 0x8000 and 0xC000 (as the original's g_vu_mem). */
extern u8 g_vu_mem[0x10000];

/* ---- the units, as the original game names them ---- */
/* gfx_gif.c: the GIF's three paths, PATH3's masks, its registers */
void gif_path_write(int path, const void *data, u32 qwords);
void gif_set_path3_mask(int on);
void gif_flush_path3(void);
void gif_note_deferred(void);
int gif_path3_masked(void);
u32 gif_read_reg(u32 addr);
void gif_write_reg(u32 addr, u32 v);
void gif_reset_all(void);
/* gfx_vif.c: VIF0 and VIF1 */
void vif_init(void);
void vif_write(int idx, const void *data, u32 qwords);
void vif_write_tag(int idx, u64 hi);
u32 vif_read_reg(int idx, u32 off);
void vif_write_reg(int idx, u32 off, u32 val);
u32 vif1_top(void);
u32 vif1_itop(void);
/* gfx_vu1.c: VU1 in micro mode */
void vu1_reset(void);
void vu1_start(u32 addr);
void vu1_continue(void);
void vu1_stats(u64 *runs, u64 *pairs);
void Gfx_Vu1UseCompiled(int on);
u64 vu1_hash_bytes(u64 h, const void *p, u32 n);
/* gfx_gs.c: what the GS receives (the stream, hashed and handed on) */
void gs_gif(int path, const void *data, u32 qwords);
void gs_priv_write(u32 addr, u64 value);
/* 1: the host's pixels (the game sees them; masked from the RAM images); 0: zeros. */
int gs_read_fifo(void *dst, u32 qwords);
void gs_init(void);
void gs_write(u32 addr, u64 v, int width64);
u64 gs_read(u32 addr);
/* gfx_dma.c: the DMAC */
void hw_gif_dma_resume(void);
void dma_init(void);

/* The original's configuration switches these files read: all off in the core. */
static const struct {
    int vu1_check, vu1_interp;
    char dump_mpg[1];
} g_config = {0, 0, {0}};

#endif
