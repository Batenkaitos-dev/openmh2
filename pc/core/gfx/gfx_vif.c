/* VIF0/VIF1, the VU interface units. They decode the VIFcode stream that
 * DMA channels 0/1 (or the EE writing the FIFOs) deliver: UNPACK into VU
 * data memory, MPG into micro memory, MSCAL/MSCALF/MSCNT to run VU1
 * microprograms, DIRECT/DIRECTHL to GIF PATH2, MSKPATH3, and VIF1's
 * double-buffering registers (BASE/OFFSET/TOPS/ITOP).
 *
 * Everything runs synchronously: a microprogram started by MSCAL finishes
 * before the next VIFcode is read, so the FLUSH* codes have nothing to wait
 * for. The game never uses VU0 micro mode (no VCALLMS), so MSCAL on VIF0 is
 * only reported. UNPACK follows PCSX2 (Vif_Unpack.cpp): V2 writes x,y,x,y,
 * V3's w comes from the next element, fill positions (CL < WL) take the
 * next element without consuming it. */
#include "gfx_rt.h"
#include "pal.h"
#include "data_c.h"

u8 g_vu_mem[0x10000] __attribute__((aligned(16)));

enum {
    VC_NOP = 0x00, VC_STCYCL = 0x01, VC_OFFSET = 0x02, VC_BASE = 0x03, VC_ITOP = 0x04, VC_STMOD = 0x05,
    VC_MSKPATH3 = 0x06, VC_MARK = 0x07, VC_FLUSHE = 0x10, VC_FLUSH = 0x11, VC_FLUSHA = 0x13,
    VC_MSCAL = 0x14, VC_MSCALF = 0x15, VC_MSCNT = 0x17, VC_STMASK = 0x20, VC_STROW = 0x30,
    VC_STCOL = 0x31, VC_MPG = 0x4A, VC_DIRECT = 0x50, VC_DIRECTHL = 0x51,
};

#define STAT_MRK (1u << 6)
#define STAT_DBF (1u << 7)
#define STAT_INT (1u << 11)
#define STAT_FDR (1u << 23)

typedef struct Vif {
    int idx;
    u8 *data;          /* VU data memory */
    u8 *micro;         /* VU micro memory */
    u32 qw_mask;       /* data memory size in qwords - 1 */
    u32 micro_mask;    /* micro memory size in bytes - 1 */
    u32 stat, err, mark, cycle, mode, num, mask, code, itops, base, ofst, tops, itop, top;
    u32 row[4], col[4];
    /* command in progress */
    u32 cmd;           /* VIFcode whose data is still arriving */
    u32 left;          /* words still expected */
    u32 got;           /* words received so far */
    u32 mpg_addr;      /* MPG: next micro memory byte address */
    u8 ubuf[256 * 16 + 32];   /* UNPACK data (at most 256 x 16 bytes) */
    u32 dq[4 * 256];   /* DIRECT data staged for PATH2 */
    u32 ndq;
    u64 stats_cmd[128];
} Vif;

static Vif g_vif[2];

u32 vif1_top(void) { return g_vif[1].top; }
u32 vif1_itop(void) { return g_vif[1].itop; }

/* ------------------------------------------------------------ UNPACK */

static u32 unpack_num(u32 code)
{
    u32 n = (code >> 16) & 0xFF;
    return n ? n : 256;
}

static u32 unpack_words(const Vif *v, u32 code)
{
    u32 cmd = (code >> 24) & 0x7F, vn = (cmd >> 2) & 3, vl = cmd & 3;
    u32 num = unpack_num(code), cl = v->cycle & 0xFF, wl = (v->cycle >> 8) & 0xFF;
    u32 elems = num;
    if (wl > cl)   /* filling write: only CL of every WL positions take data */
        elems = (num / wl) * cl + (num % wl < cl ? num % wl : cl);
    u32 bits = vl == 3 ? 16 : (vn + 1) * (32u >> vl);
    return (elems * bits + 31) / 32;
}

/* Component c of the element at `p` (size per component by vl). */
static u32 comp(const u8 *p, u32 vl, int usn, u32 c)
{
    switch (vl) {
    case 0: { u32 v; memcpy(&v, p + 4 * c, 4); return v; }
    case 1: { u16 v; memcpy(&v, p + 2 * c, 2); return usn ? v : (u32)(s32)(s16)v; }
    case 2: { u8 v = p[c]; return usn ? v : (u32)(s32)(s8)v; }
    }
    return 0;
}

static void unpack_exec_inner(Vif *v);

/* A pair of words the game keeps as a DMA tag keeps its first two (the quadword count and kind, then a
 * RAM address, as DmaTag_Next and Call write them, include/dma.h), left in a packet's data: stale tags
 * sit in the buffers the model packets are built in, and a microprogram that reads past its matrices
 * takes them for floats. The PS2's address in the tag is the RAM block's offset, or an address in the
 * image (a static packet buffer); the core's is the block's host address, a float of another size: the
 * result of VU1's arithmetic with it differs, from the flush to zero of a denormal product (the original game)
 * to a normal one. So the VU gets the PS2's value. Only whole 32-bit words that look so: nothing else in
 * VU data holds a RAM address. */
static u16 image_page[65536];   /* host address >> 16 of a resident data block's page: the PS2's page + 1, else 0 */
static int image_pages_ready;

static void image_pages_init(void)
{
    /* DataC_TryAt over the whole PS2 space, a 4 KiB page at a time: a resident block's host address keeps the
     * PS2's low 16 bits (data_c.h), so a 64 KiB page of the host maps to one page of the PS2 */
    for (u32 ps2 = 0; ps2 < 0x2000000u; ps2 += 0x1000u) {
        u32 h = (u32)DataC_TryAt(ps2);
        if (h && h - (u32)Pal_EeRam >= sizeof(Pal_EeRam) && (h & 0xFFFFu) == (ps2 & 0xFFFFu))
            image_page[h >> 16] = (u16)((ps2 >> 16) + 1);
    }
    image_pages_ready = 1;
}

static inline void tag_pair(u32 *a, u32 *b)
{
    if ((*a & 0x0FFF0000u) != 0)
        return;
    if ((u32)(*b - (u32)Pal_EeRam) < sizeof(Pal_EeRam)) {
        *b -= (u32)Pal_EeRam;
        return;
    }
    if (*b >> 16 == 0)
        return;
    if (!image_pages_ready)
        image_pages_init();
    u32 pg = image_page[*b >> 16];
    if (pg)
        *b = ((pg - 1) << 16) | (*b & 0xFFFFu);
}

static void unpack_exec(Vif *v)
{
    prof_enter(PROF_VIF_UNPACK);
    unpack_exec_inner(v);
    prof_leave();
}

static void unpack_exec_inner(Vif *v)
{
    u32 code = v->cmd, cmd = (code >> 24) & 0x7F;
    u32 vn = (cmd >> 2) & 3, vl = cmd & 3, masked = (cmd >> 4) & 1;
    int usn = (code >> 14) & 1;
    u32 addr = code & 0x3FF;
    if (v->idx == 1 && (code & 0x8000))
        addr += v->tops;
    u32 num = unpack_num(code), cl = v->cycle & 0xFF, wl = (v->cycle >> 8) & 0xFF;
    if (wl == 0) {
        rt_log_once("vif", 0x100 + (u32)v->idx, "VIF%d UNPACK with WL = 0", v->idx);
        wl = cl ? cl : 1;
    }
    int fill = wl > cl;
    if (vl == 3 && vn != 3) {
        rt_log_once("vif", 0x110 + cmd, "VIF%d UNPACK format %02X invalid", v->idx, cmd);
        return;
    }
    u32 esize = vl == 3 ? 2 : (vn + 1) * (4u >> vl);
    const u8 *src = v->ubuf;
    u32 cyc = 0;
    for (u32 n = 0; n < num; n++) {
        int data_pos = !fill || cyc < cl;
        u32 in[4];
        if (vl == 3) {   /* V4-5: RGBA 5:5:5:1 */
            u16 c;
            memcpy(&c, src, 2);
            in[0] = (c & 0x1Fu) << 3;
            in[1] = ((c >> 5) & 0x1Fu) << 3;
            in[2] = ((c >> 10) & 0x1Fu) << 3;
            in[3] = ((c >> 15) & 1u) << 7;
        } else {
            switch (vn) {
            case 0: in[0] = in[1] = in[2] = in[3] = comp(src, vl, usn, 0); break;
            case 1: in[0] = in[2] = comp(src, vl, usn, 0); in[1] = in[3] = comp(src, vl, usn, 1); break;
            case 2:
                for (u32 c = 0; c < 4; c++)   /* w: the next element's x */
                    in[c] = comp(src, vl, usn, c);
                break;
            default:
                for (u32 c = 0; c < 4; c++)
                    in[c] = comp(src, vl, usn, c);
                break;
            }
        }
        if (data_pos)
            src += esize;
        if (vl == 0) {
            tag_pair(&in[0], &in[1]);
            tag_pair(&in[2], &in[3]);
        }
        u32 mrow = cyc < 3 ? cyc : 3;
        u8 *q = v->data + (addr & v->qw_mask) * 16;
        for (u32 c = 0; c < 4; c++) {
            u32 mb = masked ? (v->mask >> ((mrow * 4 + c) * 2)) & 3 : 0;
            u32 out;
            switch (mb) {
            case 0:
                out = in[c];
                if (v->mode == 1) {
                    out += v->row[c];
                } else if (v->mode == 2) {
                    v->row[c] += out;
                    out = v->row[c];
                } else if (v->mode == 3) {
                    rt_log_once("vif", 0x120 + (u32)v->idx, "VIF%d UNPACK with MODE 3", v->idx);
                }
                break;
            case 1: out = v->row[c]; break;
            case 2: out = v->col[mrow]; break;
            default: continue;   /* write-protected */
            }
            memcpy(q + 4 * c, &out, 4);
        }
        addr++;
        cyc++;
        if (cyc == wl) {
            cyc = 0;
            if (!fill)
                addr += cl - wl;   /* skipping write */
        }
    }
    v->num = 0;
}

/* ------------------------------------------------------------ commands */

static void direct_flush(Vif *v)
{
    if (v->ndq >= 4) {
        u32 qw = v->ndq / 4;
        gif_path_write(2, v->dq, qw);
        memmove(v->dq, v->dq + qw * 4, (v->ndq - qw * 4) * 4);
        v->ndq -= qw * 4;
    }
}

static void vu_start(Vif *v, int cont, u32 addr)
{
    if (v->idx == 0) {
        rt_log_once("vif", 0x200, "VIF0 %s: VU0 micro mode is not implemented", cont ? "MSCNT" : "MSCAL");
        v->itop = v->itops;
        return;
    }
    v->itop = v->itops & 0x3FF;
    v->top = v->tops & 0x3FF;
    if (v->stat & STAT_DBF) {
        v->tops = v->base;
        v->stat &= ~STAT_DBF;
    } else {
        v->tops = v->base + v->ofst;
        v->stat |= STAT_DBF;
    }
    if (cont)
        vu1_continue();
    else
        vu1_start(addr);
}

static void command(Vif *v, u32 code)
{
    u32 cmd = (code >> 24) & 0x7F, imm = code & 0xFFFF;
    v->code = code;
    v->stats_cmd[cmd]++;
    if (code & 0x80000000u)
        rt_log_once("vif", 0x300 + (u32)v->idx, "VIF%d VIFcode %08X has the interrupt bit (not emulated)", v->idx, code);
    v->cmd = code;
    v->got = 0;
    v->left = 0;
    if (cmd >= 0x60) {   /* UNPACK */
        v->num = unpack_num(code);
        v->left = unpack_words(v, code);
        if (!v->left)
            unpack_exec(v);
        return;
    }
    switch (cmd) {
    case VC_NOP: return;
    case VC_STCYCL: v->cycle = imm; return;
    case VC_OFFSET:
        if (v->idx == 1) {
            v->ofst = imm & 0x3FF;
            v->stat &= ~STAT_DBF;
            v->tops = v->base;
        }
        return;
    case VC_BASE: if (v->idx == 1) v->base = imm & 0x3FF; return;
    case VC_ITOP: v->itops = imm & 0x3FF; return;
    case VC_STMOD: v->mode = imm & 3; return;
    case VC_MSKPATH3:
        if (v->idx == 1)
            gif_set_path3_mask((imm >> 15) & 1);
        return;
    case VC_MARK: v->mark = imm; v->stat |= STAT_MRK; return;
    case VC_FLUSHE: case VC_FLUSH: case VC_FLUSHA:
        if (cmd == VC_FLUSHA)
            gif_flush_path3();
        return;
    case VC_MSCAL: case VC_MSCALF: vu_start(v, 0, imm * 8); return;
    case VC_MSCNT: vu_start(v, 1, 0); return;
    case VC_STMASK: v->left = 1; return;
    case VC_STROW: case VC_STCOL: v->left = 4; return;
    case VC_MPG: {
        u32 n = (code >> 16) & 0xFF;
        v->left = (n ? n : 256) * 2;
        v->mpg_addr = imm * 8;
        return;
    }
    case VC_DIRECT: case VC_DIRECTHL:
        if (v->idx == 1) {
            v->left = (imm ? imm : 0x10000) * 4;
            v->ndq = 0;
            return;
        }
        break;
    }
    rt_log_once("vif", 0x400 + (u32)v->idx * 0x80 + cmd, "VIF%d VIFcode %08X not supported", v->idx, code);
}

static void command_done(Vif *v)
{
    u32 cmd = (v->cmd >> 24) & 0x7F;
    if (cmd >= 0x60) {
        memset(v->ubuf + v->got * 4, 0, 16);   /* V3's last w reads past the data */
        unpack_exec(v);
    } else if (cmd == VC_DIRECT || cmd == VC_DIRECTHL) {
        direct_flush(v);
    }
}

/* Feed `n` words of the VIFcode stream. */
static void vif_stream(Vif *v, const u32 *w, u32 n)
{
    while (n) {
        if (!v->left) {
            command(v, *w++);
            n--;
            continue;
        }
        u32 cmd = (v->cmd >> 24) & 0x7F;
        u32 k = n < v->left ? n : v->left;
        if (cmd >= 0x60) {
            memcpy(v->ubuf + v->got * 4, w, k * 4);
        } else if (cmd == VC_MPG) {
            for (u32 i = 0; i < k; i++) {
                memcpy(v->micro + (v->mpg_addr & v->micro_mask), &w[i], 4);
                v->mpg_addr += 4;
            }
        } else if (cmd == VC_DIRECT || cmd == VC_DIRECTHL) {
            for (u32 i = 0; i < k; i++) {
                v->dq[v->ndq++] = w[i];
                if (v->ndq == 4 * 256)
                    direct_flush(v);
            }
        } else if (cmd == VC_STMASK) {
            v->mask = w[0];
        } else if (cmd == VC_STROW || cmd == VC_STCOL) {
            for (u32 i = 0; i < k; i++)
                (cmd == VC_STROW ? v->row : v->col)[v->got + i] = w[i];
        }
        v->got += k;
        v->left -= k;
        w += k;
        n -= k;
        if (!v->left)
            command_done(v);
    }
}

void vif_write(int idx, const void *data, u32 qwords)
{
    u32 buf[4 * 64];
    const u8 *p = data;
    while (qwords) {
        u32 n = qwords < 64 ? qwords : 64;
        memcpy(buf, p, n * 16);
        vif_stream(&g_vif[idx], buf, n * 4);
        p += n * 16;
        qwords -= n;
    }
}

/* DMAtag with TTE set: its upper 64 bits go to the VIF as two VIFcodes. */
void vif_write_tag(int idx, u64 hi)
{
    u32 w[2] = {(u32)hi, (u32)(hi >> 32)};
    vif_stream(&g_vif[idx], w, 2);
}

/* ------------------------------------------------------------ registers */

static void vif_reset(Vif *v)
{
    int idx = v->idx;
    memset(v, 0, offsetof(Vif, ubuf));
    v->idx = idx;
    v->data = g_vu_mem + (idx ? 0xC000 : 0x4000);
    v->micro = g_vu_mem + (idx ? 0x8000 : 0x0000);
    v->qw_mask = idx ? 0x3FF : 0xFF;
    v->micro_mask = idx ? 0x3FFF : 0xFFF;
}

u32 vif_read_reg(int idx, u32 off)
{
    Vif *v = &g_vif[idx];
    switch (off) {
    case 0x000: return v->stat;   /* idle: VPS = 0, FQC = 0 */
    case 0x020: return v->err;
    case 0x030: return v->mark;
    case 0x040: return v->cycle;
    case 0x050: return v->mode;
    case 0x060: return v->num;
    case 0x070: return v->mask;
    case 0x080: return v->code;
    case 0x090: return v->itops;
    case 0x0A0: return v->base;
    case 0x0B0: return v->ofst;
    case 0x0C0: return v->tops;
    case 0x0D0: return v->itop;
    case 0x0E0: return v->top;
    case 0x100: case 0x110: case 0x120: case 0x130: return v->row[(off - 0x100) >> 4];
    case 0x140: case 0x150: case 0x160: case 0x170: return v->col[(off - 0x140) >> 4];
    }
    return 0;
}

void vif_write_reg(int idx, u32 off, u32 val)
{
    Vif *v = &g_vif[idx];
    switch (off) {
    case 0x000:   /* STAT: only FDR (VIF1 FIFO direction) is writable */
        if (idx == 1)
            v->stat = (v->stat & ~STAT_FDR) | (val & STAT_FDR);
        return;
    case 0x010:   /* FBRST */
        if (val & 1) {
            vif_reset(v);
            LOGD("vif", "VIF%d reset", idx);
        }
        if (val & 8)   /* STC: cancel stalls */
            v->stat &= ~(STAT_INT | 0x3F00u);
        return;
    case 0x020: v->err = val & 7; return;
    case 0x030: v->mark = val & 0xFFFF; v->stat &= ~STAT_MRK; return;
    }
    rt_log_once("vif", 0x500 + off, "VIF%d register +0x%03X write %08X ignored", idx, off, val);
}

void vif_init(void)
{
    for (int i = 0; i < 2; i++) {
        g_vif[i].idx = i;
        vif_reset(&g_vif[i]);
    }
    vu1_reset();
}

