/* VU1 in micro mode: runs the microprograms the game uploads with VIF1 MPG
 * and starts with MSCAL/MSCALF/MSCNT. The semantics of one instruction
 * pair are in vu1_core.h; this file holds the state, the decoder, the
 * out-of-line operations and the run loop.
 *
 * A run executes compiled code (vu1_gen.c, generated at build time from
 * the microcode found in the game's files) where it can: from addresses
 * the generator prepared, with the pipeline idle and that code in micro
 * memory; the compiled code hands the run over to the interpreter wherever
 * it cannot follow. The interpreter caches decoded pairs per address and
 * revalidates them against the raw words, so code written by any path is
 * picked up.
 *
 * the original's plain per-element interpreter is what all this was derived
 * from; --vu1-check runs every microprogram on both and reports any
 * difference in registers, data memory, timing state or GIF output.
 * --vu1-interp turns the compiled code off. */
#include "vu1_core.h"
#include "host.h"

Vu1State g_vu1;
__m128i g_vu1_lanes[16];
int g_vu1_kick_hashing;
u64 g_vu1_kick_hash;
u64 g_vu1_gen_bails;

#define vu g_vu1

#define BIT_I (1u << 31)
#define BIT_E (1u << 30)

#define FT(x) (((x) >> 16) & 31)
#define FS(x) (((x) >> 11) & 31)
#define FD(x) (((x) >> 6) & 31)
#define DEST(x) (((x) >> 21) & 15)   /* bit 3 = x ... bit 0 = w */
#define FSF(x) (((x) >> 21) & 3)
#define FTF(x) (((x) >> 23) & 3)

static Dec g_dec[2048];

/* ------------------------------------------------------------ decoder */

static u32 imm11(u32 lo) { return (lo & 0x400) ? (lo & 0x7FF) | 0xFFFFF800u : lo & 0x7FF; }
static u32 imm15(u32 lo) { return ((lo >> 10) & 0x7800) | (lo & 0x7FF); }
static u32 imm12(u32 lo) { return ((lo >> 10) & 0x800) | (lo & 0x7FF); }

/* Mirrors upper() and upper_regs() of the original's plain interpreter. */
static void decode_upper(Dec *d, u32 up)
{
    u32 f = up & 0x3F;
    d->udm = vu1_rev4[DEST(up)];
    d->ufs = (u8)FS(up);
    d->uft = (u8)FT(up);
    d->ufd = (u8)FD(up);
    d->uacc = 0;
    d->ubsrc = -1;
    d->uop = 0;
    d->ur1 = d->ufs;
    d->ur2 = d->uft;
    d->uk = UK_FMAC;
    if (f < 0x1C) {   /* ADDbc SUBbc MADDbc MSUBbc MAXbc MINIbc MULbc */
        static const u8 ops[7] = {OP_ADD, OP_SUB, OP_MADD, OP_MSUB, OP_MAX, OP_MIN, OP_MUL};
        d->uop = ops[f >> 2];
        d->ubsrc = (s8)(f & 3);
        return;
    }
    if (f < 0x28) {   /* q/i forms: ft is not read */
        static const u8 ops[12] = {OP_MUL, OP_MAX, OP_MUL, OP_MIN, OP_ADD, OP_MADD, OP_ADD, OP_MADD,
                                   OP_SUB, OP_MSUB, OP_SUB, OP_MSUB};
        static const s8 src[12] = {4, 5, 5, 5, 4, 4, 5, 5, 4, 4, 5, 5};
        d->uop = ops[f - 0x1C];
        d->ubsrc = src[f - 0x1C];
        d->ur2 = 0;
        return;
    }
    if (f < 0x30) {   /* ADD MADD MUL MAX SUB MSUB OPMSUB MINI */
        static const s8 ops[8] = {OP_ADD, OP_MADD, OP_MUL, OP_MAX, OP_SUB, OP_MSUB, -1, OP_MIN};
        if (ops[f - 0x28] < 0) {
            d->uk = UK_OPMSUB;
            return;
        }
        d->uop = (u8)ops[f - 0x28];
        return;
    }
    if (f < 0x3C) {
        d->uk = UK_UNDEF;
        d->ur1 = d->ur2 = 0;
        return;
    }
    u32 idx = ((up >> 4) & 0x7C) | (up & 3);
    d->uacc = 1;
    if (idx < 0x10) {   /* ADDAbc, SUBAbc, MADDAbc, MSUBAbc */
        static const u8 ops[4] = {OP_ADD, OP_SUB, OP_MADD, OP_MSUB};
        d->uop = ops[idx >> 2];
        d->ubsrc = (s8)(idx & 3);
        return;
    }
    if (idx >= 0x18 && idx < 0x1C) {   /* MULAbc */
        d->uop = OP_MUL;
        d->ubsrc = (s8)(idx & 3);
        return;
    }
    switch (idx) {
    case 0x1C: case 0x1E: case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: {
        /* MULAq MULAi ADDAq MADDAq ADDAi MADDAi SUBAq MSUBAq SUBAi MSUBAi */
        static const u8 ops[12] = {OP_MUL, 0, OP_MUL, 0, OP_ADD, OP_MADD, OP_ADD, OP_MADD, OP_SUB, OP_MSUB, OP_SUB, OP_MSUB};
        static const s8 src[12] = {4, 0, 5, 0, 4, 4, 5, 5, 4, 4, 5, 5};
        d->uop = ops[idx - 0x1C];
        d->ubsrc = src[idx - 0x1C];
        d->ur2 = 0;
        return;
    }
    case 0x28: case 0x29: case 0x2A: case 0x2C: case 0x2D: {   /* ADDA MADDA MULA SUBA MSUBA */
        static const u8 ops[6] = {OP_ADD, OP_MADD, OP_MUL, 0, OP_SUB, OP_MSUB};
        d->uop = ops[idx - 0x28];
        return;
    }
    case 0x2E: d->uk = UK_OPMULA; return;
    case 0x2B: d->uk = UK_UNDEF; d->uacc = 0; return;   /* stalls still count fs/ft, as the plain interpreter */
    }
    d->uacc = 0;
    d->ur2 = 0;
    switch (idx) {
    case 0x10: case 0x11: case 0x12: case 0x13: d->uk = UK_ITOF; d->uop = (u8)(idx & 3); return;
    case 0x14: case 0x15: case 0x16: case 0x17: d->uk = UK_FTOI; d->uop = (u8)(idx & 3); return;
    case 0x1D: d->uk = UK_ABS; return;
    case 0x1F: d->uk = UK_CLIP; d->ur2 = d->uft; return;
    case 0x2F: d->uk = UK_NOP; d->ur1 = 0; return;
    }
    d->uk = UK_UNDEF;
    d->ur1 = 0;
}

/* Mirrors lower() and lower_regs() of the original's plain interpreter. */
static void decode_lower(Dec *d, u32 lo)
{
    u32 op = lo >> 25;
    d->ldm = vu1_rev4[DEST(lo)];
    d->lfs = (u8)FS(lo);
    d->lft = (u8)FT(lo);
    d->lfd = (u8)FD(lo);
    d->lis = (u8)(FS(lo) & 15);
    d->lit = (u8)(FT(lo) & 15);
    d->lid = (u8)(FD(lo) & 15);
    d->lfsf = (u8)FSF(lo);
    d->lftf = (u8)FTF(lo);
    d->lr1 = d->lr2 = d->lw = d->lwq = d->lwp = 0;
    d->limm = 0;
    switch (op) {
    case 0x00: d->lk = LK_LQ; d->lw = d->lft; d->limm = imm11(lo); return;
    case 0x01: d->lk = LK_SQ; d->lr1 = d->lfs; d->limm = imm11(lo); return;
    case 0x04: d->lk = LK_ILW; d->limm = imm11(lo); return;
    case 0x05: d->lk = LK_ISW; d->limm = imm11(lo); return;
    case 0x08: d->lk = LK_IADDIU; d->limm = imm15(lo); return;
    case 0x09: d->lk = LK_ISUBIU; d->limm = imm15(lo); return;
    case 0x10: d->lk = LK_FCEQ; return;
    case 0x11: d->lk = LK_FCSET; return;
    case 0x12: d->lk = LK_FCAND; return;
    case 0x13: d->lk = LK_FCOR; return;
    case 0x14: d->lk = LK_FSEQ; d->limm = imm12(lo); return;
    case 0x15: d->lk = LK_FSSET; d->limm = imm12(lo); return;
    case 0x16: d->lk = LK_FSAND; d->limm = imm12(lo); return;
    case 0x17: d->lk = LK_FSOR; d->limm = imm12(lo); return;
    case 0x18: d->lk = LK_FMEQ; return;
    case 0x1A: d->lk = LK_FMAND; return;
    case 0x1B: d->lk = LK_FMOR; return;
    case 0x1C: d->lk = LK_FCGET; return;
    case 0x20: d->lk = LK_B; d->limm = imm11(lo); return;
    case 0x21: d->lk = LK_BAL; d->limm = imm11(lo); return;
    case 0x24: d->lk = LK_JR; return;
    case 0x25: d->lk = LK_JALR; return;
    case 0x28: d->lk = LK_IBEQ; d->limm = imm11(lo); return;
    case 0x29: d->lk = LK_IBNE; d->limm = imm11(lo); return;
    case 0x2C: d->lk = LK_IBLTZ; d->limm = imm11(lo); return;
    case 0x2D: d->lk = LK_IBGTZ; d->limm = imm11(lo); return;
    case 0x2E: d->lk = LK_IBLEZ; d->limm = imm11(lo); return;
    case 0x2F: d->lk = LK_IBGEZ; d->limm = imm11(lo); return;
    case 0x40: break;
    default: d->lk = LK_UNDEF; return;
    }
    u32 f = lo & 0x3F;
    switch (f) {
    case 0x30: d->lk = LK_IADD; return;
    case 0x31: d->lk = LK_ISUB; return;
    case 0x32: {
        s32 imm = (s32)((lo >> 6) & 31);
        if (imm & 16)
            imm -= 32;
        d->lk = LK_IADDI;
        d->limm = (u32)imm;
        return;
    }
    case 0x34: d->lk = LK_IAND; return;
    case 0x35: d->lk = LK_IOR; return;
    case 0x3C: case 0x3D: case 0x3E: case 0x3F: break;
    default: d->lk = LK_UNDEF; return;
    }
    u32 x = ((lo >> 4) & 0x7C) | (lo & 3);
    switch (x) {
    case 0x30: d->lk = LK_MOVE; d->lr1 = d->lfs; d->lw = d->lft; return;
    case 0x31: d->lk = LK_MR32; d->lr1 = d->lfs; d->lw = d->lft; return;
    case 0x34: d->lk = LK_LQI; d->lw = d->lft; return;
    case 0x36: d->lk = LK_LQD; d->lw = d->lft; return;
    case 0x35: d->lk = LK_SQI; d->lr1 = d->lfs; return;
    case 0x37: d->lk = LK_SQD; d->lr1 = d->lfs; return;
    case 0x38: d->lk = LK_DIV; d->lr1 = d->lfs; d->lr2 = d->lft; d->lwq = 1; return;
    case 0x39: d->lk = LK_SQRT; d->lr2 = d->lft; d->lwq = 1; return;
    case 0x3A: d->lk = LK_RSQRT; d->lr1 = d->lfs; d->lr2 = d->lft; d->lwq = 1; return;
    case 0x3B: d->lk = LK_WAIT; d->lwq = 1; return;                            /* WAITQ */
    case 0x3C: d->lk = LK_MTIR; d->lr1 = d->lfs; return;
    case 0x3D: d->lk = LK_MFIR; d->lw = d->lft; return;
    case 0x3E: d->lk = LK_ILWR; return;
    case 0x3F: d->lk = LK_ISWR; return;
    case 0x40: d->lk = LK_RNEXT; d->lw = d->lft; return;
    case 0x41: d->lk = LK_RGET; d->lw = d->lft; return;
    case 0x42: d->lk = LK_RINIT; d->lr1 = d->lfs; return;
    case 0x43: d->lk = LK_RXOR; d->lr1 = d->lfs; return;
    case 0x64: d->lk = LK_MFP; d->lw = d->lft; return;
    case 0x68: d->lk = LK_XTOP; return;
    case 0x69: d->lk = LK_XITOP; return;
    case 0x6C: d->lk = LK_XGKICK; return;
    case 0x7B: d->lk = LK_WAIT; d->lwp = 1; return;                            /* WAITP */
    }
    if (x >= 0x70 && x <= 0x7E) {
        d->lr1 = d->lfs;
        d->lwp = 1;
        d->lk = x != 0x77 ? LK_EFU : LK_UNDEF;
        d->limm = x;
        return;
    }
    d->lk = LK_UNDEF;
}

void vu1_decode(Dec *d, u32 lo, u32 up)
{
    memset(d, 0, sizeof(*d));
    d->lo = lo;
    d->up = up;
    d->valid = 1;
    d->ibit = (up & BIT_I) != 0;
    d->ebit = (up & BIT_E) != 0;
    decode_upper(d, up);
    decode_lower(d, lo);
    if (d->ibit) {   /* the lower word is an immediate */
        d->lk = LK_NONE;
        d->lr1 = d->lr2 = d->lw = d->lwq = d->lwp = 0;
    }
}

/* ------------------------------------------------------------ out-of-line parts */

void vu1_advance_slow(u64 to)
{
    while (vu.fq_n && vu.fq[vu.fq_head].ready <= to) {
        const Vu1Flag *e = &vu.fq[vu.fq_head];
        if (e->has_mac) {
            vu.mac = e->mac;
            vu.status = (vu.status & ~0xFu) | e->zsuo | (e->zsuo << 6);
        }
        if (e->has_clip)
            vu.clip = e->clip;
        vu.fq_head = (vu.fq_head + 1) & 15;
        vu.fq_n--;
    }
    if (vu.q_ready && vu.q_ready <= to) {
        vu.q = vu.q_next;
        vu.status = (vu.status & ~0x30u) | vu.q_flags | (vu.q_flags << 6);
        vu.q_ready = 0;
    }
    if (vu.p_ready && vu.p_ready <= to) {
        vu.p = vu.p_next;
        vu.p_ready = 0;
    }
    u64 due = vu.fq_n ? vu.fq[vu.fq_head].ready : UINT64_MAX;
    if (vu.q_ready && vu.q_ready < due)
        due = vu.q_ready;
    if (vu.p_ready && vu.p_ready < due)
        due = vu.p_ready;
    vu.due = due;
}

static u32 mac_bits(u32 v, int i, int over, int under)
{
    u32 bit = 8u >> i, m = 0;
    if ((v & 0x7FFFFFFFu) == 0)
        m |= bit;
    if (v & 0x80000000u)
        m |= bit << 4;
    if (under)
        m |= bit << 8;
    if (over)
        m |= bit << 12;
    return m;
}

static u32 s_res(float r, int *over)
{
    u32 u = fout(r);
    *over = (u & 0x7FFFFFFFu) == FMAX_BITS;
    return u;
}

/* OPMULA / OPMSUB, per element as in the plain interpreter. */
u32 vu1_opm_calc(int msub, const VF *s, const VF *t, const VF *acc, VF *out)
{
    static const int a[3] = {1, 2, 0}, b[3] = {2, 0, 1};
    u32 mac = 0;
    *out = (VF){{0}};
    for (int i = 0; i < 3; i++) {
        float fa = fin(s->u[a[i]]), fb = fin(t->u[b[i]]);
        float r = fa * fb;
        int ov, un = r == 0.0f && fa != 0.0f && fb != 0.0f;
        u32 p = s_res(r, &ov);
        if (msub) {
            float fc = fin(acc->u[i]), fp = fin(p);
            r = fc - fp;
            un = r == 0.0f && fc != fp;
            p = s_res(r, &ov);
        }
        out->u[i] = p;
        mac |= mac_bits(p, i, ov, un);
    }
    return mac;
}

void vu1_opm(const Dec *d, int msub, UpRes *o)
{
    VF out;
    u32 mac = vu1_opm_calc(msub, &vu.vf[d->ufs], &vu.vf[d->uft], &vu.acc, &out);
    o->v = vload(&out);
    o->reg = msub ? d->ufd : 32;
    o->dm = 7;   /* xyz */
    vu1_push_flags(mac, 1, 0, 0);
}

void vu1_clip(const Dec *d)   /* CLIP fs.xyz, ft.w */
{
    u32 c = vu1_clip_calc(vclamp(vload(&vu.vf[d->ufs])), vclamp(vload(&vu.vf[d->uft])), vu.clip_last);
    vu.clip_last = c;
    vu1_push_flags(0, 0, c, 1);
}

void vu1_upper_undefined(const Dec *d)
{
    u32 up = d->up, f = up & 0x3F, idx = ((up >> 4) & 0x7C) | (up & 3);
    if (f < 0x3C)
        rt_log_once("vu1", 0x100 + f, "VU1 upper opcode 0x%02X undefined (%08X)", f, up);
    else
        rt_log_once("vu1", 0x200 + idx, "VU1 upper opcode 0x3C+%02X undefined (%08X)", idx, up);
}

int vu1_lower_undefined(const Dec *d, u32 pc)
{
    u32 lo = d->lo, op = lo >> 25, x = ((lo >> 4) & 0x7C) | (lo & 3);
    (void)pc; /* in the original's log line */
    if (op != 0x40)
        rt_log_once("vu1", 0x300 + op, "VU1 lower opcode 0x%02X undefined (%08X at %04X)", op, lo, pc);
    else if ((lo & 0x3C) != 0x3C)
        rt_log_once("vu1", 0x400 + (lo & 0x3F), "VU1 lower op 0x40/%02X undefined (%08X at %04X)", lo & 0x3F, lo, pc);
    else
        rt_log_once("vu1", 0x500 + x, "VU1 lower op 0x40/%02X undefined (%08X at %04X)", x, lo, pc);
    return 1;
}

void vu1_vdiv(const Dec *d, int kind)
{
    vu.q_next = vu1_div_calc(kind, vu.vf[d->lfs].u[d->lfsf], vu.vf[d->lft].u[d->lftf], &vu.q_flags);
    vu.q_ready = vu.cycle + (kind == 2 ? 13 : 7);
    if (vu.q_ready < vu.due)
        vu.due = vu.q_ready;
}

/* Latencies of the EFU ops (cycles until P is written), by extended opcode. */
u32 vu1_efu_latency(u32 op)
{
    switch (op) {
    case 0x70: return 11;   /* ESADD */
    case 0x71: return 18;   /* ERSADD */
    case 0x72: return 18;   /* ELENG */
    case 0x73: return 24;   /* ERLENG */
    case 0x74: case 0x75: return 54;   /* EATANxy/xz */
    case 0x76: return 12;   /* ESUM */
    case 0x78: return 12;   /* ESQRT */
    case 0x79: return 18;   /* ERSQRT */
    case 0x7A: return 12;   /* ERCPR */
    case 0x7C: return 29;   /* ESIN */
    case 0x7D: return 54;   /* EATAN */
    case 0x7E: return 44;   /* EEXP */
    }
    return 1;
}

u32 vu1_efu_calc(u32 op, const VF *v, u32 fsf)
{
    float x = fin(v->u[0]), y = fin(v->u[1]), z = fin(v->u[2]), w = fin(v->u[3]);
    float f = fin(v->u[fsf]), p = 0;
    switch (op) {
    case 0x70: p = x * x + y * y + z * z; break;                             /* ESADD */
    case 0x71: p = x * x + y * y + z * z; if (p != 0) p = 1.0f / p; break;   /* ERSADD */
    case 0x72: p = vu1_sqrtf(x * x + y * y + z * z); break;                      /* ELENG */
    case 0x73: p = vu1_sqrtf(x * x + y * y + z * z); if (p != 0) p = 1.0f / p; break;   /* ERLENG */
    case 0x76: p = x + y + z + w; break;                                     /* ESUM */
    case 0x78: p = f >= 0 ? vu1_sqrtf(f) : f; break;                             /* ESQRT */
    case 0x79: p = f; if (f >= 0) { p = vu1_sqrtf(f); if (p != 0) p = 1.0f / p; } break;   /* ERSQRT */
    case 0x7A: p = f != 0 ? 1.0f / f : f; break;                             /* ERCPR */
    case 0x74: case 0x75: case 0x7C: case 0x7D: case 0x7E:
        /* EATANxy, EATANxz, ESIN, EATAN, EEXP: the original game takes them from the
         * host's libm; the game's microcode has none of them (ERSQRT and ERLENG only). */
        rt_log_once("vu1", 0x700 + op, "EFU operation the game's microcode does not use");
        p = 0;
        break;
    }
    return fout(p);
}

void vu1_efu(const Dec *d)
{
    vu.p_next = vu1_efu_calc(d->limm, &vu.vf[d->lfs], d->lfsf);
    vu.p_ready = vu.cycle + vu1_efu_latency(d->limm);
    if (vu.p_ready < vu.due)
        vu.due = vu.p_ready;
}

/* XGKICK: send the GIF packet at data address `a` (qwords) to PATH1. */
/* The check's interpreted run: its GIF output is hashed only (below). */
static int sKickShadow;

void vu1_xgkick(u32 a)
{
    static u8 buf[0x4000 + 16];
    u32 q = 0, addr = a & 0x3FF;
    for (;;) {
        if (q >= 0x400) {
            rt_log_once("vu1", 1, "XGKICK packet at %03X runs past 16 KiB", a & 0x3FF);
            break;
        }
        const u8 *tag = vu1_qword(addr);
        memcpy(buf + q * 16, tag, 16);
        q++;
        addr = (addr + 1) & 0x3FF;
        u64 t;
        memcpy(&t, tag, 8);
        u32 nloop = (u32)(t & 0x7FFF), eop = (u32)(t >> 15) & 1, flg = (u32)(t >> 58) & 3, nreg = (u32)(t >> 60) & 15;
        if (!nreg)
            nreg = 16;
        u32 words = flg == 0 ? nloop * nreg : flg == 1 ? (nloop * nreg + 1) / 2 : nloop;
        for (u32 k = 0; k < words && q < 0x400; k++) {
            memcpy(buf + q * 16, vu1_qword(addr), 16);
            q++;
            addr = (addr + 1) & 0x3FF;
        }
        if (eop)
            break;
    }
    if (g_vu1_kick_hashing)
        g_vu1_kick_hash = vu1_hash_bytes(g_vu1_kick_hash, buf, q * 16);
    if (!sKickShadow)
        gif_path_write(1, buf, q);
}

/* ------------------------------------------------------------ run loop */

static int g_use_gen = 1;

static void run_body(void);

/* The original game runs VU1 inside the guest's thread, whose MXCSR is 0xFFC0:
 * every exception masked, rounding toward zero, FTZ and DAZ (the original's
 * MXCSR_GUEST). The core's own code keeps the host's default. */
#define VU1_MXCSR 0xFFC0u

static void run(void)
{
    u32 csr = _mm_getcsr();
    _mm_setcsr(VU1_MXCSR);
    run_body();
    _mm_setcsr(csr);
}

/* Tests: 0 runs every pair on the interpreter (the original's --vu1-interp). */
void Gfx_Vu1UseCompiled(int on)
{
    g_use_gen = on;
}

static u64 sGenEnds; /* runs that the compiled code finished */

static void run_body(void)
{
    static u64 serial;
    Vu1Run rc = {vu.cycle, 0, ++serial};
    for (int i = 0; i < 4; i++)
        vu.hist[i] = (Vu1Hist){-1, 0, 0, 0};
    vu.hist_pos = 0;
    vu.branch_pending = 0;
    vu.runs++;
    const u8 *code = vu1_code_mem();
    u32 start_pc = vu.tpc & VU1_CODE_MASK;
    int first = 1;
    for (;;) {
        if (g_use_gen && vu1_gen_exec(&rc)) {
            sGenEnds++;
            break;
        }
        u32 pc = vu.tpc & VU1_CODE_MASK;
        u32 lo, up;
        memcpy(&lo, code + pc, 4);
        memcpy(&up, code + pc + 4, 4);
        /* A run whose first pair has no compiled code: a microprogram the
         * generator never saw in the game's files (a new effect). Once per
         * program, so play tests show where the interpreter takes over. */
        if (first && g_use_gen && pc == start_pc)
            rt_log_once("vu1", pc ^ lo ^ (up * 0x9E3779B1u), "a microprogram at %04X (%08X %08X) is not compiled: interpreted",
                        pc, up, lo);
        first = 0;
        Dec *d = &g_dec[pc >> 3];
        if (!d->valid || d->lo != lo || d->up != up)
            vu1_decode(d, lo, up);
        if (vu1_step(d, pc, &rc))
            break;
    }
    /* The VU finishes its pipelines before it reports idle. */
    u64 end = vu.cycle;
    for (int n = 0; n < vu.fq_n; n++)
        if (vu.fq[(vu.fq_head + n) & 15].ready > end)
            end = vu.fq[(vu.fq_head + n) & 15].ready;
    if (vu.q_ready > end)
        end = vu.q_ready;
    if (vu.p_ready > end)
        end = vu.p_ready;
    vu1_advance_slow(end);
    vu.cycle = end;
}

/* ------------------------------------------------------------ checking */

u64 vu1_hash_bytes(u64 h, const void *p, u32 n)
{
    const u8 *b = p;
    for (u32 i = 0; i < n; i++) {
        h ^= b[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

/*
 * The compiled microprograms against the interpreter (the original's
 * --vu1-check, checked_run): with the check on, every run is made twice
 * from the same state, first on the interpreter (its GIF output hashed, not
 * sent), then as usual (compiled where it can be, its output sent). The two
 * must leave the same registers (VF 1-31, ACC, VI, Q, P, I, R, the flags, TPC,
 * the cycle count, the VF ready times), the same data memory and the same GIF
 * output. A difference is an anomaly ("vu1-check", the run's start address).
 * The core keeps the second run's state. In the original game the interpreter of
 * the check is the plain one this file's interpreter was derived
 * from; in the core it is this file's, which runs the same pairs.
 */
static int sCheck;
static u64 sChecks, sCheckDiffs;
static Vu1State sPre, sWant;
static u8 sPreData[0x4000], sWantData[0x4000];

void Gfx_Vu1Check(int on)
{
    sCheck = on;
}

u64 Gfx_Vu1CheckCounts(u64 *differ)
{
    if (differ)
        *differ = sCheckDiffs;
    return sChecks;
}

static int SameRun(const Vu1State *got, const Vu1State *want, const Vu1State *pre)
{
    int r;
    for (r = 1; r < 32; r++)
        if (memcmp(&got->vf[r], &want->vf[r], 16))
            return 0;
    if (memcmp(&got->acc, &want->acc, 16) || memcmp(got->vi, want->vi, sizeof(got->vi)))
        return 0;
    if (got->q != want->q || got->p != want->p || got->i != want->i || got->r != want->r)
        return 0;
    if (got->mac != want->mac || got->status != want->status || got->clip != want->clip ||
        got->clip_last != want->clip_last)
        return 0;
    if (got->tpc != want->tpc || got->cycle - pre->cycle != want->cycle - pre->cycle)
        return 0;
    for (r = 1; r < 32; r++) {
        u64 a = got->vf_ready[r] > got->cycle ? got->vf_ready[r] - got->cycle : 0;
        u64 b = want->vf_ready[r] > want->cycle ? want->vf_ready[r] - want->cycle : 0;
        if (a != b)
            return 0;
    }
    return 1;
}

static char *PutDec(char *p, u64 v)
{
    char t[24];
    int n = 0;
    do {
        t[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        *p++ = t[--n];
    return p;
}

static char *PutStr(char *p, const char *s)
{
    while (*s)
        *p++ = *s++;
    return p;
}

/* At 1, 2, 4, ... checked runs: how many, how many the compiled code
 * finished, how many differed (the check's only report: the core has no
 * end-of-run hook for gfx). */
static void CheckProgress(void)
{
    char line[160], *p = line;
    p = PutStr(p, "mh2-core: vu1-check: ");
    p = PutDec(p, sChecks);
    p = PutStr(p, " runs, ");
    p = PutDec(p, sGenEnds);
    p = PutStr(p, " finished compiled, ");
    p = PutDec(p, sCheckDiffs);
    p = PutStr(p, " differ");
    *p = 0;
    Host_Log(line);
}

static void checked_run(void)
{
    u64 wantKick, gotKick;
    int gen = g_use_gen;
    u32 start = vu.tpc;
    memcpy(&sPre, &vu, sizeof(vu));
    memcpy(sPreData, vu1_data_mem(), sizeof(sPreData));
    g_use_gen = 0;
    sKickShadow = 1;
    g_vu1_kick_hashing = 1;
    g_vu1_kick_hash = 0xcbf29ce484222325ull;
    run();
    wantKick = g_vu1_kick_hash;
    memcpy(&sWant, &vu, sizeof(vu));
    memcpy(sWantData, vu1_data_mem(), sizeof(sWantData));
    memcpy(&vu, &sPre, sizeof(vu));
    memcpy(vu1_data_mem(), sPreData, sizeof(sPreData));
    g_use_gen = gen;
    sKickShadow = 0;
    g_vu1_kick_hash = 0xcbf29ce484222325ull;
    run();
    gotKick = g_vu1_kick_hash;
    g_vu1_kick_hashing = 0;
    sChecks++;
    if (!SameRun(&vu, &sWant, &sPre) || memcmp(vu1_data_mem(), sWantData, sizeof(sWantData)) ||
        gotKick != wantKick) {
        sCheckDiffs++;
        Gfx_Oddity("vu1-check", start);
    }
    if (!(sChecks & (sChecks - 1)))
        CheckProgress();
}

/* ------------------------------------------------------------ interface */

void vu1_start(u32 addr)
{
    vu.tpc = addr & VU1_CODE_MASK;
    if (sCheck) {
        checked_run();
        return;
    }
    prof_enter(PROF_VU1);
    run();
    prof_leave();
}

void vu1_continue(void)
{
    if (sCheck) {
        checked_run();
        return;
    }
    prof_enter(PROF_VU1);
    run();
    prof_leave();
}

void vu1_reset(void)
{
    memset(&vu, 0, sizeof(vu));
    memset(g_dec, 0, sizeof(g_dec));
    for (u32 m = 0; m < 16; m++)
        g_vu1_lanes[m] = _mm_set_epi32((m & 8) ? -1 : 0, (m & 4) ? -1 : 0, (m & 2) ? -1 : 0, (m & 1) ? -1 : 0);
    vu.vf[0].u[3] = 0x3F800000u;
    vu.r = 0x3F800000u;
    vu.due = UINT64_MAX;
    g_use_gen = !g_config.vu1_interp;
}

void vu1_stats(u64 *runs, u64 *pairs)
{
    *runs = vu.runs;
    *pairs = vu.pairs;
}
