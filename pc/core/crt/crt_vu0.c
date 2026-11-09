/* VU0's macro operations on PC (include/vu0ops.h): the global state, and
 * vu0_exec_word, the decoder of a raw COP2 word over the operations of vu0ops.h
 * (the original's special1 and special2 tables; the VU data memory forms
 * are left out). The game's C forms call the operations directly; vu0_exec_word
 * is what the VU0 oracle test compares with the original's VU0 code on random
 * words and random registers. */
#include "vu0ops.h"

Vu0State g_vu0 = {.vf = {[0] = {.u = {0, 0, 0, 0x3F800000u}}}};

#define FT(raw) (int)(((raw) >> 16) & 31)
#define FS(raw) (int)(((raw) >> 11) & 31)
#define FD(raw) (int)(((raw) >> 6) & 31)
#define DEST(raw) (((raw) >> 21) & 15)

/* Unsupported words (the VU data memory forms, unknown ones): the state is left as it is. */
static void unhandled(unsigned int raw) { (void)raw; }

static void special2(unsigned int raw)
{
    unsigned int idx = ((raw >> 4) & 0x7C) | (raw & 3), dest = DEST(raw);
    int fs = FS(raw), ft = FT(raw);
    if (idx < 0x10) { /* VADDAbc, VSUBAbc, VMADDAbc, VMSUBAbc */
        static const int ops[4] = {VU0_ADD, VU0_SUB, VU0_MADD, VU0_MSUB};
        vu0_fmac(ops[idx >> 2], (int)(idx & 3), 1, dest, 0, fs, ft);
        return;
    }
    if (idx >= 0x18 && idx < 0x1C) {
        vu0_fmac(VU0_MUL, (int)(idx & 3), 1, dest, 0, fs, ft); /* VMULAbc */
        return;
    }
    switch (idx) {
    case 0x10: case 0x11: case 0x12: case 0x13: { /* VITOF0/4/12/15 */
        static const int n[4] = {0, 4, 12, 15};
        vu0_itof(n[idx & 3], dest, ft, fs);
        return;
    }
    case 0x14: case 0x15: case 0x16: case 0x17: { /* VFTOI0/4/12/15 */
        static const int n[4] = {0, 4, 12, 15};
        vu0_ftoi(n[idx & 3], dest, ft, fs);
        return;
    }
    case 0x1C: vu0_fmac(VU0_MUL, VU0_BQ, 1, dest, 0, fs, ft); return;
    case 0x1D: vu0_abs(dest, ft, fs); return;
    case 0x1E: vu0_fmac(VU0_MUL, VU0_BI, 1, dest, 0, fs, ft); return;
    case 0x1F: vu0_clip(fs, ft); return;
    case 0x20: vu0_fmac(VU0_ADD, VU0_BQ, 1, dest, 0, fs, ft); return;
    case 0x21: vu0_fmac(VU0_MADD, VU0_BQ, 1, dest, 0, fs, ft); return;
    case 0x22: vu0_fmac(VU0_ADD, VU0_BI, 1, dest, 0, fs, ft); return;
    case 0x23: vu0_fmac(VU0_MADD, VU0_BI, 1, dest, 0, fs, ft); return;
    case 0x24: vu0_fmac(VU0_SUB, VU0_BQ, 1, dest, 0, fs, ft); return;
    case 0x25: vu0_fmac(VU0_MSUB, VU0_BQ, 1, dest, 0, fs, ft); return;
    case 0x26: vu0_fmac(VU0_SUB, VU0_BI, 1, dest, 0, fs, ft); return;
    case 0x27: vu0_fmac(VU0_MSUB, VU0_BI, 1, dest, 0, fs, ft); return;
    case 0x28: vu0_fmac(VU0_ADD, VU0_BVEC, 1, dest, 0, fs, ft); return;
    case 0x29: vu0_fmac(VU0_MADD, VU0_BVEC, 1, dest, 0, fs, ft); return;
    case 0x2A: vu0_fmac(VU0_MUL, VU0_BVEC, 1, dest, 0, fs, ft); return;
    case 0x2C: vu0_fmac(VU0_SUB, VU0_BVEC, 1, dest, 0, fs, ft); return;
    case 0x2D: vu0_fmac(VU0_MSUB, VU0_BVEC, 1, dest, 0, fs, ft); return;
    case 0x2E: vu0_opmula(0, 0, fs, ft); return;
    case 0x2F: return; /* VNOP */
    case 0x30: vu0_move(dest, ft, fs); return;
    case 0x31: vu0_mr32(dest, ft, fs); return;
    case 0x38: vu0_vdiv(fs, (int)((raw >> 21) & 3), ft, (int)((raw >> 23) & 3)); return;
    case 0x39: vu0_vsqrt(ft, (int)((raw >> 23) & 3)); return;
    case 0x3A: vu0_vrsqrt(fs, (int)((raw >> 21) & 3), ft, (int)((raw >> 23) & 3)); return;
    case 0x3B: return; /* VWAITQ */
    case 0x3C: vu0_mtir(ft, fs, (int)((raw >> 21) & 3)); return;
    case 0x3D: vu0_mfir(dest, ft, fs); return;
    case 0x40: vu0_rnext(dest, ft); return;
    case 0x41: vu0_rget(dest, ft); return;
    case 0x42: vu0_rinit(fs, (int)((raw >> 21) & 3)); return;
    case 0x43: vu0_rxor(fs, (int)((raw >> 21) & 3)); return;
    default: unhandled(raw);
    }
}

static void special1(unsigned int raw)
{
    unsigned int f = raw & 0x3F, dest = DEST(raw);
    int fd = FD(raw), fs = FS(raw), ft = FT(raw);
    if (f < 0x1C) { /* broadcast forms */
        static const int ops[7] = {VU0_ADD, VU0_SUB, VU0_MADD, VU0_MSUB, VU0_MAX, VU0_MIN, VU0_MUL};
        vu0_fmac(ops[f >> 2], (int)(f & 3), 0, dest, fd, fs, ft);
        return;
    }
    switch (f) {
    case 0x1C: vu0_fmac(VU0_MUL, VU0_BQ, 0, dest, fd, fs, ft); return;
    case 0x1D: vu0_fmac(VU0_MAX, VU0_BI, 0, dest, fd, fs, ft); return;
    case 0x1E: vu0_fmac(VU0_MUL, VU0_BI, 0, dest, fd, fs, ft); return;
    case 0x1F: vu0_fmac(VU0_MIN, VU0_BI, 0, dest, fd, fs, ft); return;
    case 0x20: vu0_fmac(VU0_ADD, VU0_BQ, 0, dest, fd, fs, ft); return;
    case 0x21: vu0_fmac(VU0_MADD, VU0_BQ, 0, dest, fd, fs, ft); return;
    case 0x22: vu0_fmac(VU0_ADD, VU0_BI, 0, dest, fd, fs, ft); return;
    case 0x23: vu0_fmac(VU0_MADD, VU0_BI, 0, dest, fd, fs, ft); return;
    case 0x24: vu0_fmac(VU0_SUB, VU0_BQ, 0, dest, fd, fs, ft); return;
    case 0x25: vu0_fmac(VU0_MSUB, VU0_BQ, 0, dest, fd, fs, ft); return;
    case 0x26: vu0_fmac(VU0_SUB, VU0_BI, 0, dest, fd, fs, ft); return;
    case 0x27: vu0_fmac(VU0_MSUB, VU0_BI, 0, dest, fd, fs, ft); return;
    case 0x28: vu0_fmac(VU0_ADD, VU0_BVEC, 0, dest, fd, fs, ft); return;
    case 0x29: vu0_fmac(VU0_MADD, VU0_BVEC, 0, dest, fd, fs, ft); return;
    case 0x2A: vu0_fmac(VU0_MUL, VU0_BVEC, 0, dest, fd, fs, ft); return;
    case 0x2B: vu0_fmac(VU0_MAX, VU0_BVEC, 0, dest, fd, fs, ft); return;
    case 0x2C: vu0_fmac(VU0_SUB, VU0_BVEC, 0, dest, fd, fs, ft); return;
    case 0x2D: vu0_fmac(VU0_MSUB, VU0_BVEC, 0, dest, fd, fs, ft); return;
    case 0x2E: vu0_opmula(1, fd, fs, ft); return;
    case 0x2F: vu0_fmac(VU0_MIN, VU0_BVEC, 0, dest, fd, fs, ft); return;
    case 0x30: vu0_iadd(fd, fs, ft); return;
    case 0x31: vu0_isub(fd, fs, ft); return;
    case 0x34: vu0_iand(fd, fs, ft); return;
    case 0x35: vu0_ior(fd, fs, ft); return;
    case 0x32: { /* VIADDI it, is, imm5 */
        int imm = (int)((raw >> 6) & 31);
        if (imm & 16)
            imm -= 32;
        vu0_iaddi(ft, fs, imm);
        return;
    }
    default: unhandled(raw); /* VCALLMS and the others */
    }
}

void vu0_exec_word(unsigned int raw)
{
    if (raw & (1u << 25)) {
        if ((raw & 0x3F) >= 0x3C)
            special2(raw);
        else
            special1(raw);
        return;
    }
    /* the register moves: the same operations with a GPR value in or out */
    unhandled(raw);
}
