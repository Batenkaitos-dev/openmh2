/* GIF: the three paths into the GS. PATH1 (VU1 XGKICK) and PATH2 (VIF1
 * DIRECT) arrive in program order from the VIF1 stream; PATH3 comes from
 * GIF DMA (channel 2) or the GIF FIFO.
 *
 * PATH3 can be masked, by VIF1 MSKPATH3 or GIF_MODE.M3R, which games use to
 * order texture uploads against VU1 drawing. A GIF DMA that starts while
 * PATH3 is masked waits (its channel stays busy) until the mask is lifted,
 * and then runs at that point of the VIF1 stream.
 *
 * The backend gets whole GIFtags only (the tag and all its data in one
 * call): DMA and DIRECT deliver data in arbitrary pieces, and paraLLEl-GS's
 * fast path for PACKED data assumes a tag's loops are not split across
 * gif_transfer calls. A partial tag waits here for the rest of its data. */
#include "gfx_rt.h"
#include "host.h"
#include "pal_arena.h"

static u32 g_gif_ctrl, g_gif_mode;
static int g_path3_vif_mask;

typedef struct PathAsm {
    u8 *buf;      /* incomplete GIFtag waiting for data; starts at the tag */
    u32 cap, n;   /* capacity and fill, in qwords */
    u32 pos;      /* qwords of buf already scanned */
    u32 need;     /* qwords left in the tag being scanned (0 = next qword is a tag) */
} PathAsm;
static PathAsm g_asm_real[4];
static PathAsm *g_asm = g_asm_real;   /* the second pass's own set while it runs (gif_sandbox) */

int gif_path3_masked(void)
{
    return g_path3_vif_mask || (g_gif_mode & 1);
}

static u32 tag_qwords(const u8 *tag)
{
    u64 t;
    memcpy(&t, tag, 8);
    u32 nloop = (u32)(t & 0x7FFF), flg = (u32)(t >> 58) & 3, nreg = (u32)(t >> 60) & 15;
    if (!nreg)
        nreg = 16;
    return flg == 0 ? nloop * nreg : flg == 1 ? (nloop * nreg + 1) / 2 : nloop;
}

/* Continue scanning buf[a->pos .. n); returns the number of leading qwords
 * that form complete tags. */
static u32 scan(PathAsm *a, const u8 *buf, u32 n)
{
    u32 last = 0;
    while (a->pos < n) {
        if (a->need == 0) {
            last = a->pos;
            a->need = tag_qwords(buf + (size_t)a->pos * 16) + 1;
        }
        u32 k = n - a->pos < a->need ? n - a->pos : a->need;
        a->pos += k;
        a->need -= k;
    }
    if (a->need == 0)
        last = a->pos;
    return last;
}

static int sSandbox, sSandboxOverflow;

static void keep(PathAsm *a, const u8 *src, u32 qwords)
{
    if (sSandbox && a->n + qwords > a->cap) {   /* the pass's buffers are fixed: a tag this long ends the pass (it is not equal) */
        sSandboxOverflow = 1;
        return;
    }
    if (a->n + qwords > a->cap) {
        u32 cap = a->cap ? a->cap : 1024;
        while (cap < a->n + qwords)
            cap *= 2;
        u8 *nb = Pal_ArenaAlloc(cap * 16); /* the arena: a state holds it */
        if (!nb)
            rt_fatal("out of memory (GIF path buffer)");
        if (a->n)
            memcpy(nb, a->buf, (size_t)a->n * 16);
        Pal_ArenaFree(a->buf);
        a->buf = nb;
        a->cap = cap;
    }
    memcpy(a->buf + (size_t)a->n * 16, src, (size_t)qwords * 16);
    a->n += qwords;
}

void gif_path_write(int path, const void *data, u32 qwords)
{
    if (!qwords)
        return;
    PathAsm *a = &g_asm[path];
    if (!a->n) {   /* nothing waiting: pass complete tags straight through */
        u32 done = scan(a, data, qwords);
        if (done)
            gs_gif(path, data, done);
        a->pos -= done;
        if (done < qwords)
            keep(a, (const u8 *)data + (size_t)done * 16, qwords - done);
        return;
    }
    keep(a, data, qwords);
    u32 done = scan(a, a->buf, a->n);
    if (done) {
        gs_gif(path, a->buf, done);
        memmove(a->buf, a->buf + (size_t)done * 16, (size_t)(a->n - done) * 16);
        a->n -= done;
        a->pos -= done;
    }
}

void gif_set_path3_mask(int on)
{
    g_path3_vif_mask = on;
    if (!gif_path3_masked())
        hw_gif_dma_resume();
}

/* FLUSHA: VIF1 waits for PATH3 to finish. If a GIF DMA is waiting for the
 * mask to be lifted, it cannot finish before that, so nothing to do. */
void gif_flush_path3(void)
{
    if (!gif_path3_masked())
        hw_gif_dma_resume();
}

void gif_note_deferred(void)
{
    /* the original game counts it for its statistics */
}

u32 gif_read_reg(u32 addr)
{
    switch (addr) {
    case 0x10003000: return g_gif_ctrl & 8;
    case 0x10003010: return g_gif_mode;
    case 0x10003020:   /* STAT: idle; M3R, M3P, IMT mirror the masks */
        return (g_gif_mode & 1) | (g_path3_vif_mask ? 2u : 0) | (g_gif_mode & 4);
    }
    return 0;
}

void gif_write_reg(u32 addr, u32 v)
{
    switch (addr) {
    case 0x10003000:
        if (v & 1) {
            LOGD("gif", "GIF reset");
            for (int p = 0; p < 4; p++)
                g_asm[p].n = g_asm[p].pos = g_asm[p].need = 0;
        }
        g_gif_ctrl = v & 8;
        return;
    case 0x10003010:
        g_gif_mode = v & 5;
        if (!gif_path3_masked())
            hw_gif_dma_resume();
        return;
    }
    rt_log_once("gif", addr, "GIF register %08X write %08X ignored", addr, v);
}

/* The power-on state: no masks, nothing waiting (the original's statics). */
void gif_reset_all(void)
{
    for (int p = 0; p < 4; p++)
        g_asm[p].n = g_asm[p].pos = g_asm[p].need = 0;
    g_gif_ctrl = g_gif_mode = 0;
    g_path3_vif_mask = 0;
}

/* The second pass (gfx_pass.c): its own path buffers (`bufs`: 4 x GFX_PASS_SB_QW qwords, the pass's own memory). */
static PathAsm sSb[4];

void gif_sandbox(int on, void *bufs)
{
    if (on) {
        for (int p = 0; p < 4; p++) {
            sSb[p].buf = (u8 *)bufs + (size_t)p * GFX_PASS_SB_QW * 16;
            sSb[p].cap = GFX_PASS_SB_QW;
            sSb[p].n = sSb[p].pos = sSb[p].need = 0;
        }
        g_asm = sSb;
        sSandbox = 1;
        sSandboxOverflow = 0;
    } else {
        g_asm = g_asm_real;
        sSandbox = 0;
    }
}

/* 1 when the pass's path buffers hold what the real ones do (the stream ended in the same place); 0 after an overflow. */
int gif_sandbox_same(void)
{
    if (sSandboxOverflow)
        return 0;
    for (int p = 0; p < 4; p++)
        if (sSb[p].n != g_asm_real[p].n || (sSb[p].n && memcmp(sSb[p].buf, g_asm_real[p].buf, (size_t)sSb[p].n * 16)))
            return 0;
    return 1;
}

void gif_sandbox_clear(void)
{
    for (int p = 0; p < 4; p++)
        sSb[p].buf = 0, sSb[p].cap = sSb[p].n = sSb[p].pos = sSb[p].need = 0;
}

int gif_pending(void)
{
    for (int p = 0; p < 4; p++)
        if (g_asm_real[p].n)
            return 1;
    return 0;
}

u32 gif_state(void *buf, int load)
{
    int *s = buf;
    if (s) {
        if (load) {
            g_path3_vif_mask = s[0];
            g_gif_mode = (u32)s[1];
        } else {
            s[0] = g_path3_vif_mask;
            s[1] = (int)g_gif_mode;
        }
    }
    return 2 * sizeof(int);
}

/* The real path buffers as a window left them, for the pass that runs on a thread of its own (gfx_pass_run.c): its sandbox is compared with them at the end.
 * gif_real_export writes, for each path, n, pos and need and then n qwords, into DST (CAP bytes) and returns the bytes, or 0 when they do not fit;
 * gif_real_import puts such a record into this copy's real buffers. */
u32 gif_real_export(u8 *dst, u32 cap)
{
    u32 off = 0;
    for (int p = 0; p < 4; p++) {
        const PathAsm *a = &g_asm_real[p];
        u32 need = 12 + a->n * 16;
        if (off + need > cap)
            return 0;
        memcpy(dst + off, &a->n, 4);
        memcpy(dst + off + 4, &a->pos, 4);
        memcpy(dst + off + 8, &a->need, 4);
        if (a->n)
            memcpy(dst + off + 12, a->buf, (size_t)a->n * 16);
        off += need;
    }
    return off;
}

void gif_real_import(const u8 *src)
{
    u32 off = 0;
    for (int p = 0; p < 4; p++) {
        PathAsm *a = &g_asm_real[p];
        u32 n, pos, need;
        memcpy(&n, src + off, 4);
        memcpy(&pos, src + off + 4, 4);
        memcpy(&need, src + off + 8, 4);
        a->n = a->pos = a->need = 0;
        if (n)
            keep(a, src + off + 12, n);
        a->pos = pos;
        a->need = need;
        off += 12 + n * 16;
    }
}
