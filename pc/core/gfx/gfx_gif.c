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
static PathAsm g_asm[4];

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

static void keep(PathAsm *a, const u8 *src, u32 qwords)
{
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
