#!/usr/bin/env python3
"""Compile the game's VU1 microcode to C ahead of time.

    python tools/pc/vu1rec.py [--out DIR] [--list]

The game uploads its VU1 microprograms from its own files: each one is a
VIF packet (DMA tag, a VIFcode, then one or more MPG with code) in the ELF
or an overlay under build/orig/. This finds those packets, decodes every
instruction pair the same way gfx_vu1.c does, and writes
DIR/vu1_gen.c (default build/vu1): one C function per packet (a "unit"), which
vu1_gen_exec() enters when a run starts at an address it knows.

Timing is resolved here, at build time. What a pair does depends on the
pipeline state before it: the VF registers still being written (FMAC and
load results are ready 4 cycles after issue), the Q and P units, the
MAC/status/clip flags on their 4-cycle way to the flag registers, the
integer writes of the last 4 pairs (the branch VI delay), a pending branch
or E bit. vu1_step() keeps that state in g_vu1 at
run time; this generator follows it abstractly, relative to the current
cycle, and emits code for every (pair, pipeline state) that can be reached.
Stalls, the cycle count, when flags and Q/P arrive and the delay slots are
then constants, and the registers live in C locals until the run ends. A
loop whose body ends in the state it started with becomes a C loop. The
game's microcode almost never stalls, so few pairs need more than one
state.

The generated code calls the operations of vu1_core.h (vu1_fmac_calc() and
so on) that vu1_step() uses, so the arithmetic is the same by construction.
Operands are clamped as there, except the lanes a forward dataflow
(clamp_flow) proves already clamped: FMAC results feeding the next FMAC,
ACC in MADD chains. A MAC flag result is carried as a vector payload
(vu1_flag_r/u) until it reaches the flag registers, and the MAC word is
computed only where something reads it (FMAND..., the end of the run, a
bail-out). Each unit exists twice: the second version runs when the four
MAC sticky status bits are already set, which in this game they are after
the first run (it never uses FSSET), so it accumulates no sticky bits and
the compiler drops the flag work of every FMAC whose flags nothing reads.

Where the state is not known or would split too far (a JR to an address
computed at run time, a branch in a delay slot, too many states at one
pair, the run cycle limit), the generated code writes the whole pipeline
state back to g_vu1 and returns to the interpreter ("bail out"). A run
enters compiled code when the pipeline is idle (it always is when a run
starts) and the pending VF writes match a state prepared for that address:
a unit's start with the states in which any unit's runs end, and the
addresses where the unit's runs stop, with the states they stop in.

A unit's code is compared with micro memory once per run, when the run
enters it (micro memory cannot change during a run): partial uploads and
code written some other way run on the interpreter. --vu1-check compares
every run with the original game interpreter and checks
these decodes against the C decoder.

The output is derived from the game: it stays under build/ (not committed).
"""

from __future__ import annotations

import argparse
import struct
import sys
from collections import Counter, namedtuple
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent

# Enumerations, in the order of the earlier port's vu1_core.h.
UK = {n: i for i, n in enumerate("NOP FMAC OPMULA OPMSUB ITOF FTOI ABS CLIP UNDEF".split())}
OP = {n: i for i, n in enumerate("ADD SUB MUL MADD MSUB MAX MIN".split())}
LK = {n: i for i, n in enumerate("""NONE UNDEF LQ SQ ILW ISW IADDIU ISUBIU FCEQ FCSET FCAND FCOR FSEQ FSSET FSAND FSOR FMEQ
    FMAND FMOR FCGET B BAL JR JALR IBEQ IBNE IBLTZ IBGTZ IBLEZ IBGEZ IADD ISUB IADDI IAND IOR MOVE MR32 LQI LQD SQI SQD
    DIV SQRT RSQRT WAIT MTIR MFIR ILWR ISWR RNEXT RGET RINIT RXOR MFP XTOP XITOP XGKICK EFU""".split())}
REV4 = [0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15]
UKN = {v: k for k, v in UK.items()}
OPN = {v: k for k, v in OP.items()}
LKN = {v: k for k, v in LK.items()}

# Dec fields, in declaration order (the C initialisers are positional).
FIELDS = ("lo up valid ibit ebit uk uop uacc udm ufs uft ufd ubsrc ur1 ur2 "
          "lk ldm lfs lft lfd lis lit lid lfsf lftf lr1 lr2 lw lwq lwp limm").split()


def ft(x): return (x >> 16) & 31
def fs(x): return (x >> 11) & 31
def fd(x): return (x >> 6) & 31
def dest(x): return (x >> 21) & 15
def fsf(x): return (x >> 21) & 3
def ftf(x): return (x >> 23) & 3
def imm11(lo): return ((lo & 0x7FF) | 0xFFFFF800) if lo & 0x400 else lo & 0x7FF
def imm15(lo): return ((lo >> 10) & 0x7800) | (lo & 0x7FF)
def imm12(lo): return ((lo >> 10) & 0x800) | (lo & 0x7FF)


def decode_upper(d: dict, up: int) -> None:
    """Mirror of decode_upper() in gfx_vu1.c."""
    f = up & 0x3F
    d.update(udm=REV4[dest(up)], ufs=fs(up), uft=ft(up), ufd=fd(up), uacc=0, ubsrc=-1, uop=0, uk=UK["FMAC"])
    d["ur1"], d["ur2"] = d["ufs"], d["uft"]
    if f < 0x1C:
        d["uop"] = [OP["ADD"], OP["SUB"], OP["MADD"], OP["MSUB"], OP["MAX"], OP["MIN"], OP["MUL"]][f >> 2]
        d["ubsrc"] = f & 3
        return
    if f < 0x28:
        ops = "MUL MAX MUL MIN ADD MADD ADD MADD SUB MSUB SUB MSUB".split()
        src = [4, 5, 5, 5, 4, 4, 5, 5, 4, 4, 5, 5]
        d["uop"], d["ubsrc"], d["ur2"] = OP[ops[f - 0x1C]], src[f - 0x1C], 0
        return
    if f < 0x30:
        ops = ["ADD", "MADD", "MUL", "MAX", "SUB", "MSUB", None, "MIN"][f - 0x28]
        if ops is None:
            d["uk"] = UK["OPMSUB"]
            return
        d["uop"] = OP[ops]
        return
    if f < 0x3C:
        d["uk"], d["ur1"], d["ur2"] = UK["UNDEF"], 0, 0
        return
    idx = ((up >> 4) & 0x7C) | (up & 3)
    d["uacc"] = 1
    if idx < 0x10:
        d["uop"], d["ubsrc"] = OP[["ADD", "SUB", "MADD", "MSUB"][idx >> 2]], idx & 3
        return
    if 0x18 <= idx < 0x1C:
        d["uop"], d["ubsrc"] = OP["MUL"], idx & 3
        return
    if idx in (0x1C, 0x1E, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27):
        ops = ["MUL", None, "MUL", None, "ADD", "MADD", "ADD", "MADD", "SUB", "MSUB", "SUB", "MSUB"]
        src = [4, 0, 5, 0, 4, 4, 5, 5, 4, 4, 5, 5]
        d["uop"], d["ubsrc"], d["ur2"] = OP[ops[idx - 0x1C]], src[idx - 0x1C], 0
        return
    if idx in (0x28, 0x29, 0x2A, 0x2C, 0x2D):
        d["uop"] = OP[["ADD", "MADD", "MUL", None, "SUB", "MSUB"][idx - 0x28]]
        return
    if idx == 0x2E:
        d["uk"] = UK["OPMULA"]
        return
    if idx == 0x2B:
        d["uk"], d["uacc"] = UK["UNDEF"], 0
        return
    d["uacc"], d["ur2"] = 0, 0
    if 0x10 <= idx <= 0x13:
        d["uk"], d["uop"] = UK["ITOF"], idx & 3
    elif 0x14 <= idx <= 0x17:
        d["uk"], d["uop"] = UK["FTOI"], idx & 3
    elif idx == 0x1D:
        d["uk"] = UK["ABS"]
    elif idx == 0x1F:
        d["uk"], d["ur2"] = UK["CLIP"], d["uft"]
    elif idx == 0x2F:
        d["uk"], d["ur1"] = UK["NOP"], 0
    else:
        d["uk"], d["ur1"] = UK["UNDEF"], 0


LOWER_OPS = {0x00: "LQ", 0x01: "SQ", 0x04: "ILW", 0x05: "ISW", 0x08: "IADDIU", 0x09: "ISUBIU", 0x10: "FCEQ",
             0x11: "FCSET", 0x12: "FCAND", 0x13: "FCOR", 0x14: "FSEQ", 0x15: "FSSET", 0x16: "FSAND", 0x17: "FSOR",
             0x18: "FMEQ", 0x1A: "FMAND", 0x1B: "FMOR", 0x1C: "FCGET", 0x20: "B", 0x21: "BAL", 0x24: "JR",
             0x25: "JALR", 0x28: "IBEQ", 0x29: "IBNE", 0x2C: "IBLTZ", 0x2D: "IBGTZ", 0x2E: "IBLEZ", 0x2F: "IBGEZ"}
IMM11 = {"LQ", "SQ", "ILW", "ISW", "B", "BAL", "IBEQ", "IBNE", "IBLTZ", "IBGTZ", "IBLEZ", "IBGEZ"}
IMM12 = {"FSEQ", "FSSET", "FSAND", "FSOR"}
IMM15 = {"IADDIU", "ISUBIU"}


def decode_lower(d: dict, lo: int) -> None:
    """Mirror of decode_lower() in gfx_vu1.c."""
    op = lo >> 25
    d.update(ldm=REV4[dest(lo)], lfs=fs(lo), lft=ft(lo), lfd=fd(lo), lis=fs(lo) & 15, lit=ft(lo) & 15,
             lid=fd(lo) & 15, lfsf=fsf(lo), lftf=ftf(lo), lr1=0, lr2=0, lw=0, lwq=0, lwp=0, limm=0)
    if op in LOWER_OPS:
        name = LOWER_OPS[op]
        d["lk"] = LK[name]
        if name == "LQ":
            d["lw"] = d["lft"]
        elif name == "SQ":
            d["lr1"] = d["lfs"]
        if name in IMM11:
            d["limm"] = imm11(lo)
        elif name in IMM12:
            d["limm"] = imm12(lo)
        elif name in IMM15:
            d["limm"] = imm15(lo)
        return
    if op != 0x40:
        d["lk"] = LK["UNDEF"]
        return
    f = lo & 0x3F
    if f in (0x30, 0x31, 0x34, 0x35):
        d["lk"] = LK[{0x30: "IADD", 0x31: "ISUB", 0x34: "IAND", 0x35: "IOR"}[f]]
        return
    if f == 0x32:
        imm = (lo >> 6) & 31
        if imm & 16:
            imm -= 32
        d["lk"], d["limm"] = LK["IADDI"], imm & 0xFFFFFFFF
        return
    if f < 0x3C:
        d["lk"] = LK["UNDEF"]
        return
    x = ((lo >> 4) & 0x7C) | (lo & 3)
    table = {
        0x30: ("MOVE", "r1 w"), 0x31: ("MR32", "r1 w"), 0x34: ("LQI", "w"), 0x36: ("LQD", "w"),
        0x35: ("SQI", "r1"), 0x37: ("SQD", "r1"), 0x38: ("DIV", "r1 r2 q"), 0x39: ("SQRT", "r2 q"),
        0x3A: ("RSQRT", "r1 r2 q"), 0x3B: ("WAIT", "q"), 0x3C: ("MTIR", "r1"), 0x3D: ("MFIR", "w"),
        0x3E: ("ILWR", ""), 0x3F: ("ISWR", ""), 0x40: ("RNEXT", "w"), 0x41: ("RGET", "w"), 0x42: ("RINIT", "r1"),
        0x43: ("RXOR", "r1"), 0x64: ("MFP", "w"), 0x68: ("XTOP", ""), 0x69: ("XITOP", ""), 0x6C: ("XGKICK", ""),
        0x7B: ("WAIT", "p"),
    }
    if x in table:
        name, regs = table[x]
        d["lk"] = LK[name]
        for r in regs.split():
            if r == "r1":
                d["lr1"] = d["lfs"]
            elif r == "r2":
                d["lr2"] = d["lft"]
            elif r == "w":
                d["lw"] = d["lft"]
            elif r == "q":
                d["lwq"] = 1
            elif r == "p":
                d["lwp"] = 1
        return
    if 0x70 <= x <= 0x7E:
        d["lr1"], d["lwp"] = d["lfs"], 1
        d["lk"] = LK["EFU"] if x != 0x77 else LK["UNDEF"]
        d["limm"] = x
        return
    d["lk"] = LK["UNDEF"]


def decode(lo: int, up: int) -> dict:
    d = {"lo": lo, "up": up, "valid": 1, "ibit": (up >> 31) & 1, "ebit": (up >> 30) & 1}
    decode_upper(d, up)
    decode_lower(d, lo)
    if d["ibit"]:
        d.update(lk=LK["NONE"], lr1=0, lr2=0, lw=0, lwq=0, lwp=0)
    return d


def _valid_code(code: bytes) -> bool:
    """Real microcode decodes without undefined operations (data rarely does)."""
    for k in range(0, len(code), 8):
        lo, up = struct.unpack_from("<II", code, k)
        d = decode(lo, up)
        if d["uk"] == UK["UNDEF"] or d["lk"] == LK["UNDEF"]:
            return False
    return True


def find_packets(data: bytes, name: str) -> list[list[tuple[int, bytes, str]]]:
    """VIF packets on qword boundaries: [DMA tag][VIFcode][MPG num, addr][code],
    possibly followed by more [VIFcode][MPG][code] in the same packet. Returns
    the MPG parts of each packet."""
    found = []
    for i in range(0, len(data) - 16, 16):
        tag, = struct.unpack_from("<I", data, i)
        mpg, = struct.unpack_from("<I", data, i + 12)
        if (mpg >> 24) & 0x7F != 0x4A:
            continue
        qwc = tag & 0xFFFF
        at = i + 12   # offset of the MPG VIFcode
        parts = []
        while True:
            num = ((mpg >> 16) & 0xFF) or 256
            addr = (mpg & 0xFFFF) * 8
            code = data[at + 4:at + 4 + num * 8]
            if addr >= 0x4000 or len(code) != num * 8 or (not parts and qwc * 16 < 8 + num * 8):
                break
            if not _valid_code(code):
                break
            parts.append((addr, code, f"{name}+0x{at + 4:X}"))
            nxt = at + 4 + num * 8   # a following [VIFcode][MPG] pair?
            if nxt + 8 > len(data):
                break
            mpg, = struct.unpack_from("<I", data, nxt + 4)
            if (mpg >> 24) & 0x7F != 0x4A:
                break
            at = nxt + 4
        if parts:
            found.append(parts)
    return found


def c_dec(d: dict) -> str:
    vals = []
    for f in FIELDS:
        v = d[f]
        vals.append(f"0x{v:08X}u" if f in ("lo", "up", "limm") else str(v))
    return "{" + ", ".join(vals) + "}"


# ------------------------------------------------------------ the pipeline, abstractly

# Latencies, as in vu1_core.h and gfx_vu1.c: FMAC and VF loads 4, flags
# 4, DIV/SQRT 7, RSQRT 13, EFU by operation (checked by vu1_gen_selfcheck).
EFU_LATENCY = {0x70: 11, 0x71: 18, 0x72: 18, 0x73: 24, 0x74: 54, 0x75: 54, 0x76: 12, 0x78: 12, 0x79: 18,
               0x7A: 12, 0x7C: 29, 0x7D: 54, 0x7E: 44}
BRANCHES = {"B", "BAL", "JR", "JALR", "IBEQ", "IBNE", "IBLTZ", "IBGTZ", "IBLEZ", "IBGEZ"}
MAX_STATES = 16          # states per pair address before the code bails out to the interpreter
STICKY_VARIANT = True    # also emit each unit for runs with the MAC sticky bits all set

# The pipeline state before a pair, relative to the cycle it would issue at
# without a stall (vu1_step() keeps the same in g_vu1, in absolute cycles):
#   vf      pending VF writes: 32 cycles-until-ready (0 = ready)
#   q, p    Q/P result pending: cycles until it lands (None = unit idle)
#   fq      flag results on their way: (cycles, "M" MAC or "C" clip), in order
#   pend    a branch the previous pair took or may take: ("B", target),
#           ("C", target) taken when tk, ("J", pc of the JR) to jt
#   ending  the previous pair had the E bit: this pair is the last
# Payloads live in locals named after their position: a MAC result due in k
# cycles in frk/fuk, a clip result in fck. The integer writes of the last 4
# pairs (for the branch VI delay and a bail-out) are kept at run time: packed
# in hr (12 bits per pair: VI register + 1, flag-instruction bit, stall),
# old values in ho0-ho3. Each pair shifts in a constant, so within
# straight-line code the compiler knows hr and folds the branch VI rule.
State = namedtuple("State", "vf q p fq pend ending")


def entry_state(key: int) -> State:
    """The state at the start of a run: idle, but for VF writes still pending
    from the run before (2 bits per register in key, as gen_key() computes)."""
    return State(tuple((key >> (2 * r)) & 3 for r in range(32)), None, None, (), None, False)


def vf_key(vf) -> int:
    return sum(o << (2 * r) for r, o in enumerate(vf))


def VF(r: int) -> str:
    return "VU1_VF0" if r == 0 else f"vf{r}"


def VI(r: int) -> str:
    return "0u" if r == 0 else f"vi{r}"


class Unit:
    """The microcode of one VIF packet, by address."""

    def __init__(self, idx: int, parts: list[tuple[int, bytes, str]]):
        self.idx = idx
        self.where = parts[0][2]
        self.start = parts[0][0]
        self.words: dict[int, tuple[int, int]] = {}
        for addr, code, _ in parts:
            for k in range(0, len(code), 8):
                self.words[(addr + k) & 0x3FFF] = struct.unpack_from("<II", code, k)
        self.dec = {pc: decode(lo, up) for pc, (lo, up) in self.words.items()}
        self.jr_targets = self.returns()

    def lower(self, pc: int) -> str:
        d = self.dec[pc]
        return "NONE" if d["ibit"] else LKN[d["lk"]]

    def returns(self) -> dict[int, set[int]]:
        """For each JR/JALR, where it may return to: after the BAL/JALR calls
        that set its register and from whose target it can be reached (not
        counting paths through other jumps). Any other address bails out."""
        out: dict[int, set[int]] = {pc: set() for pc in self.dec if self.lower(pc) in ("JR", "JALR")}
        for call, d in self.dec.items():
            lk = self.lower(call)
            if lk not in ("BAL", "JALR") or not d["lit"]:
                continue
            reg, ret = d["lit"], (call + 16) & 0x3FFF
            if lk == "JALR":   # callee unknown: any jump through reg
                for j in out:
                    if self.dec[j]["lis"] == reg:
                        out[j].add(ret)
                continue
            seen, stack = set(), [(call + 8 + d["limm"] * 8) & 0x3FFF]
            while stack:
                pc = stack.pop()
                if pc in seen or pc not in self.dec:
                    continue
                seen.add(pc)
                lk2 = self.lower(pc)
                if lk2 in ("JR", "JALR"):
                    if self.dec[pc]["lis"] == reg:
                        out[pc].add(ret)
                    continue
                stack.append((pc + 8) & 0x3FFF)
                if lk2 in ("B", "IBEQ", "IBNE", "IBLTZ", "IBGTZ", "IBLEZ", "IBGEZ"):
                    stack.append((pc + 8 + self.dec[pc]["limm"] * 8) & 0x3FFF)
        return out

    def ranges(self) -> list[tuple[int, int]]:
        """(address, pairs) of the contiguous stretches of code."""
        out: list[tuple[int, int]] = []
        for pc in sorted(self.words):
            if out and out[-1][0] + out[-1][1] * 8 == pc:
                out[-1] = (out[-1][0], out[-1][1] + 1)
            else:
                out.append((pc, 1))
        return out


def mnemonic(d: dict) -> str:
    up = UKN[d["uk"]]
    if up == "FMAC":
        up = OPN[d["uop"]] + ("A" if d["uacc"] else "")
    lo = "I" if d["ibit"] else LKN[d["lk"]]
    return f"{up} {lo}" + (" E" if d["ebit"] else "")


class UnitGen:
    """Builds the (pair, state) graph of a unit and emits its C function."""

    def __init__(self, unit: Unit):
        self.u = unit
        self.nodes: dict[tuple[int, State], int] = {}
        self.order: list[tuple[int, State]] = []
        self.code: list[list[str] | None] = []
        self.xfer: list[tuple | None] = []
        self.bails: dict[tuple[object, State], int] = {}
        self.bail_list: list[tuple[object, State]] = []
        self.per_pc: Counter = Counter()
        self.entries: dict[tuple[int, int], int] = {}
        self.entry_index: dict[tuple[int, int], int] = {}
        self.ends: set[tuple[int, int]] = set()     # (TPC, VF key) where runs stop
        self.end_keys: set[int] = set()
        self.work: list[int] = []

    # ---------------------------------------------------------------- graph

    def supported(self, pc: int, S: State) -> bool:
        d = self.u.dec[pc]
        if d["uk"] == UK["UNDEF"] or d["lk"] == LK["UNDEF"]:
            return False
        return not (S.pend and not d["ibit"] and LKN[d["lk"]] in BRANCHES)   # a branch in a delay slot

    def ref(self, pc: int, S: State, entry: bool = False) -> tuple[str, int]:
        """The node for (pc, S), or a bail-out to the interpreter at pc."""
        if (pc, S) in self.nodes:
            return ("N", self.nodes[(pc, S)])
        if pc not in self.u.words or not self.supported(pc, S) or (self.per_pc[pc] >= MAX_STATES and not entry):
            return ("B", self.bail(pc, S))
        i = len(self.order)
        self.nodes[(pc, S)] = i
        self.order.append((pc, S))
        self.code.append(None)
        self.xfer.append(None)
        self.per_pc[pc] += 1
        self.work.append(i)
        return ("N", i)

    def bail(self, pc: object, S: State) -> int:
        if (pc, S) not in self.bails:
            self.bails[(pc, S)] = len(self.bail_list)
            self.bail_list.append((pc, S))
        return self.bails[(pc, S)]

    def add_entry(self, pc: int, key: int) -> bool:
        if (pc, key) in self.entries or pc not in self.u.words:
            return False
        r = self.ref(pc, entry_state(key), entry=True)
        if r[0] != "N":
            return False
        self.entries[(pc, key)] = r[1]
        return True

    def run(self) -> None:
        while self.work:
            i = self.work.pop()
            pc, S = self.order[i]
            self.code[i], self.xfer[i] = self.step(pc, S, 0)

    def finish(self) -> None:
        """With the graph complete: which register lanes hold clamped values
        at each node, then the code again, now without redundant clamps."""
        self.clamp_flow()
        for i, (pc, S) in enumerate(self.order):
            self.code[i], x = self.step(pc, S, self.cin[i])
            assert x == self.xfer[i]

    # ---------------------------------------------------------------- clamped values

    # A value is "clamped" when vclamp() would not change it: the FMAC
    # operations clamp their operands, and skip that where the generator
    # knows the lanes they use already are. Known lanes are bits of an int:
    # VF register r lane i at 4r + i, then ACC, then Q and I (whole).
    ACC_SH, Q_BIT, I_BIT = 128, 1 << 132, 1 << 133
    VF0_BITS = 0xF   # vf0 = (0, 0, 0, 1) is a clamped constant

    @staticmethod
    def lanes(c: int, r: int) -> int:
        return (c >> (4 * r)) & 15

    @staticmethod
    def set_lanes(c: int, r: int, dm: int, known: int) -> int:
        """Lanes dm of register r (32 = ACC) now hold values that are clamped
        where known has a bit."""
        sh = 4 * r
        return (c & ~(dm << sh)) | ((known & dm) << sh)

    def clamp_out(self, i: int, c: int) -> int:
        """Known-clamped lanes after the pair of node i, given those before
        (the register writes of step(), in the same order)."""
        pc, S = self.order[i]
        d = self.u.dec[pc]
        uk, lk = UKN[d["uk"]], LKN[d["lk"]]
        if S.q is not None and S.q <= self.stall_of(pc, S):
            c |= self.Q_BIT          # Q from DIV/SQRT/RSQRT: fout() or +/-Fmax
        ureg, udm, ucl = None, d["udm"], 0
        if uk == "FMAC":             # vclamp_res(), or MAX/MIN of clamped operands
            ureg, ucl = (32 if d["uacc"] else d["ufd"]), 15
        elif uk in ("OPMULA", "OPMSUB"):
            ureg, udm, ucl = (d["ufd"] if uk == "OPMSUB" else 32), 7, 15
        elif uk in ("ITOF", "ABS"):
            ureg, ucl = d["uft"], 15
        elif uk == "FTOI":           # integers
            ureg, ucl = d["uft"], 0
        ft_, fs_, ldm = d["lft"], d["lfs"], d["ldm"]
        if d["ibit"]:
            e = d["lo"] & 0x7F800000
            c = c | self.I_BIT if e not in (0, 0x7F800000) or not d["lo"] & 0x7FFFFFFF else c & ~self.I_BIT
        elif (ureg is not None and 0 < ureg < 32 and ureg == d["lw"]) or not ft_:
            pass
        elif lk in ("LQ", "LQI", "LQD", "MFIR", "RNEXT", "RGET", "MFP"):
            c = self.set_lanes(c, ft_, ldm, 0)
        elif lk == "MOVE":
            c = self.set_lanes(c, ft_, ldm, self.lanes(c, fs_))
        elif lk == "MR32":           # lane i of ft from lane i + 1 of fs
            s = self.lanes(c, fs_)
            c = self.set_lanes(c, ft_, ldm, (s >> 1) | (s & 1) << 3)
        if ureg == 32:
            c = self.set_lanes(c, 32, udm, ucl)
        elif ureg:
            c = self.set_lanes(c, ureg, udm, ucl)
        return c

    def clamp_flow(self) -> None:
        """Forward must-analysis: a lane is known clamped at a node when it is
        on every path from an entry (where nothing is known)."""
        n = len(self.order)
        top = (1 << 134) - 1
        self.cin = [top] * n
        succ: list[list[int]] = [[] for _ in range(n)]
        for i, x in enumerate(self.xfer):
            refs = []
            if x[0] == "goto":
                refs = [x[1]]
            elif x[0] == "cond":
                refs = [x[1], x[2]]
            elif x[0] == "switch":
                refs = [r for _, r in x[1]]
            succ[i] = [r[1] for r in refs if r[0] == "N"]
        work = sorted(set(self.entries.values()))
        for i in work:
            self.cin[i] = self.VF0_BITS
        while work:
            i = work.pop()
            out = self.clamp_out(i, self.cin[i])
            for j in succ[i]:
                v = self.cin[j] & out
                if v != self.cin[j]:
                    self.cin[j] = v
                    work.append(j)

    # ---------------------------------------------------------------- one pair

    def stall_of(self, pc: int, S: State) -> int:
        """Stall cycles until the operands and the Q/P unit are ready."""
        d = self.u.dec[pc]
        stall = 0
        for r in (d["ur1"], d["ur2"], d["lr1"], d["lr2"]):
            if r:
                stall = max(stall, S.vf[r])
        if d["lwq"] and S.q is not None:
            stall = max(stall, S.q)
        if d["lwp"] and S.p is not None:
            stall = max(stall, S.p)
        return stall

    def step(self, pc: int, S: State, c: int) -> tuple[list[str], tuple]:
        """C for the pair at pc in state S (mirroring vu1_step()), and where
        control goes next. c: the lanes known to be clamped (clamp_flow)."""
        d = self.u.dec[pc]
        lo, _ = self.u.words[pc]
        uk, lk = UKN[d["uk"]], LKN[d["lk"]]
        L = [f"/* {pc:04X} {mnemonic(d)} */"]
        stall = self.stall_of(pc, S)

        def cvf(r: int, need: int) -> str:
            """VF register r as an operand whose lanes need are used clamped."""
            return VF(r) if not r or self.lanes(c, r) & need == need else f"vclamp({VF(r)})"

        # Flag, Q and P results that are due by the issue cycle land first.
        rem = []
        for off, kind in S.fq:
            if off <= stall:
                L.append(f"VU1_APPLY_MAC(fr{off}, fu{off});" if kind == "M" else f"clip = fc{off};")
            else:
                rem.append((off, kind))
        q, p = S.q, S.p
        if q is not None and q <= stall:
            L.append("q = qn; st = (st & ~0x30u) | qf | qf << 6;")
            q = None
        if p is not None and p <= stall:
            L.append("p = pn;")
            p = None

        # Upper instruction: operands are read before either half writes.
        # Only the lanes the result keeps (udm) need clamped operands.
        ureg, udm, new = None, d["udm"], None
        if uk == "FMAC":
            op = OPN[d["uop"]]
            b = d["ubsrc"]
            s = cvf(d["ufs"], udm)
            if b == -1:
                t = cvf(d["uft"], udm)
            elif b == 4:
                t = "_mm_set1_epi32((int)q)" if c & self.Q_BIT else "vclamp(_mm_set1_epi32((int)q))"
            elif b == 5:
                t = "_mm_set1_epi32((int)ii)" if c & self.I_BIT else "vclamp(_mm_set1_epi32((int)ii))"
            else:
                t = f"VU1_BC({cvf(d['uft'], 1 << b)}, {b})"
            a = "acc"
            if op in ("MADD", "MSUB") and (c >> self.ACC_SH) & udm != udm:
                a = "vclamp(acc)"
            L.append(f"V4 uu_, ur_ = vu1_fmac_calc(OP_{op}, {s}, {t}, {a}, &uu_);")
            ureg = 32 if d["uacc"] else d["ufd"]
            if op not in ("MAX", "MIN"):
                new = ("M", f"vu1_flag_r(ur_, {udm})", f"vu1_flag_u(uu_, {udm})")
        elif uk in ("OPMULA", "OPMSUB"):
            ms = int(uk == "OPMSUB")
            L.append(f"VF os_, ot_, oa_, oo_; vstore(&os_, {VF(d['ufs'])}); vstore(&ot_, {VF(d['uft'])}); "
                     "vstore(&oa_, acc);")
            L.append(f"u32 om_ = vu1_opm_calc({ms}, &os_, &ot_, &oa_, &oo_); V4 ur_ = vload(&oo_);")
            ureg, udm = (d["ufd"] if ms else 32), 7
            new = ("P", "om_")
        elif uk == "ITOF":
            L.append(f"V4 ur_ = vu1_itof_calc({d['uop']}, {VF(d['ufs'])});")
            ureg = d["uft"]
        elif uk == "FTOI":
            L.append(f"V4 ur_ = vu1_ftoi_calc({d['uop']}, {cvf(d['ufs'], udm)});")
            ureg = d["uft"]
        elif uk == "ABS":
            L.append(f"V4 ur_ = _mm_and_si128({cvf(d['ufs'], udm)}, _mm_set1_epi32(0x7FFFFFFF));")
            ureg = d["uft"]
        elif uk == "CLIP":   # fs.xyz, ft.w
            L.append(f"u32 nc_ = vu1_clip_calc({cvf(d['ufs'], 7)}, {cvf(d['uft'], 8)}, clipl); clipl = nc_;")
            new = ("C", "nc_")

        # Lower instruction (or the I immediate).
        hreg, hflag = -1, 0
        pend = None
        qnew = pnew = None

        def set_vi(reg: int, expr: str, flag: int = 0) -> None:
            nonlocal hreg, hflag
            if not reg:
                return
            L.append(f"hn_ = vi{reg}; vi{reg} = ({expr}) & 0xFFFFu;")
            hreg, hflag = reg, flag

        def branch_vi(reg: int) -> str:
            """What a branch reads for VI[reg] (vu1_branch_vi())."""
            if not reg or stall:
                return VI(reg)
            return f"vu1_gen_branch_vi({reg}, vi{reg}, hr, ho0, ho1, ho2, ho3)"

        imm, ldm = d["limm"], d["ldm"]
        fs_, ft_, is_, it_, id_ = d["lfs"], d["lft"], d["lis"], d["lit"], d["lid"]
        if d["ibit"]:
            L.append(f"ii = 0x{lo:08X}u;")
        elif ureg is not None and 0 < ureg < 32 and ureg == d["lw"]:
            L.append("/* the lower op is discarded: the upper one writes the same register */")
        elif lk == "LQ":
            if ft_:
                L.append(f"vf{ft_} = vu1_ld(dmem, {VI(is_)} + 0x{imm:X}u, vf{ft_}, {ldm});")
        elif lk == "SQ":
            L.append(f"vu1_st(dmem, {VI(it_)} + 0x{imm:X}u, {VF(fs_)}, {ldm});")
        elif lk in ("ILW", "ILWR"):
            lanes = [i for i in range(4) if ldm & (1 << i)]
            if lanes:
                off = f" + 0x{imm:X}u" if lk == "ILW" else ""
                set_vi(it_, f"vu1_lw(dmem, {VI(is_)}{off}, {lanes[0]})")
        elif lk in ("ISW", "ISWR"):
            off = f" + 0x{imm:X}u" if lk == "ISW" else ""
            L.append(f"vu1_st(dmem, {VI(is_)}{off}, _mm_set1_epi32((int){VI(it_)}), {ldm});")
        elif lk in ("IADDIU", "ISUBIU", "IADDI"):
            set_vi(it_, f"{VI(is_)} {'-' if lk == 'ISUBIU' else '+'} 0x{imm:X}u")
        elif lk in ("IADD", "ISUB", "IAND", "IOR"):
            o = {"IADD": "+", "ISUB": "-", "IAND": "&", "IOR": "|"}[lk]
            set_vi(id_, f"{VI(is_)} {o} {VI(it_)}")
        elif lk == "FCEQ":
            set_vi(1, f"(clip & 0xFFFFFFu) == 0x{lo & 0xFFFFFF:X}u", 1)
        elif lk == "FCSET":
            L.append(f"clip = clipl = 0x{lo & 0xFFFFFF:X}u;")
        elif lk == "FCAND":
            set_vi(1, f"(clip & 0x{lo & 0xFFFFFF:X}u) != 0", 1)
        elif lk == "FCOR":
            set_vi(1, f"((clip | 0x{lo:X}u) & 0xFFFFFFu) == 0xFFFFFFu", 1)
        elif lk == "FSEQ":
            set_vi(it_, f"(VU1_STATUS & 0xFFFu) == 0x{imm:X}u", 1)
        elif lk == "FSSET":
            L.append(f"st = (VU1_STATUS & 0x3Fu) | 0x{imm & 0xFC0:X}u; VU1_STICKY_RESET();")
        elif lk == "FSAND":
            set_vi(it_, f"VU1_STATUS & 0x{imm:X}u", 1)
        elif lk == "FSOR":
            set_vi(it_, f"(VU1_STATUS | 0x{imm:X}u) & 0xFFFu", 1)
        elif lk == "FMEQ":
            set_vi(it_, f"(vu1_mac_of(mr, mu) & 0xFFFFu) == ({VI(is_)} & 0xFFFFu)", 1)
        elif lk == "FMAND":
            set_vi(it_, f"vu1_mac_of(mr, mu) & {VI(is_)}", 1)
        elif lk == "FMOR":
            set_vi(it_, f"(vu1_mac_of(mr, mu) | {VI(is_)}) & 0xFFFFu", 1)
        elif lk == "FCGET":
            set_vi(it_, "clip & 0xFFFu", 1)
        elif lk in ("B", "BAL"):
            if lk == "BAL":
                set_vi(it_, f"0x{(pc + 16) // 8:X}u")
            pend = ("B", (pc + 8 + imm * 8) & 0x3FFF)
        elif lk in ("JR", "JALR"):
            L.append(f"jt = ({branch_vi(is_)} * 8) & 0x3FFFu;")
            pend = ("J", pc)
            if lk == "JALR":
                set_vi(it_, f"0x{(pc + 16) // 8:X}u")
        elif lk in ("IBEQ", "IBNE", "IBLTZ", "IBGTZ", "IBLEZ", "IBGEZ"):
            a = f"(s16){branch_vi(is_)}"
            b = f"(s16){branch_vi(it_)}" if lk in ("IBEQ", "IBNE") else "0"
            rel = {"IBEQ": "==", "IBNE": "!=", "IBLTZ": "<", "IBGTZ": ">", "IBLEZ": "<=", "IBGEZ": ">="}[lk]
            L.append(f"tk = {a} {rel} {b};")
            pend = ("C", (pc + 8 + imm * 8) & 0x3FFF)
        elif lk == "MOVE":
            if ft_:
                L.append(f"vf{ft_} = vu1_lanes({VF(fs_)}, vf{ft_}, {ldm});")
        elif lk == "MR32":
            if ft_:
                L.append(f"vf{ft_} = vu1_lanes(_mm_shuffle_epi32({VF(fs_)}, _MM_SHUFFLE(0, 3, 2, 1)), vf{ft_}, {ldm});")
        elif lk in ("LQI", "LQD"):
            if lk == "LQD":
                set_vi(is_, f"{VI(is_)} - 1u")
            if ft_:
                L.append(f"vf{ft_} = vu1_ld(dmem, {VI(is_)}, vf{ft_}, {ldm});")
            if lk == "LQI":
                set_vi(is_, f"{VI(is_)} + 1u")
        elif lk in ("SQI", "SQD"):
            if lk == "SQD":
                set_vi(it_, f"{VI(it_)} - 1u")
            L.append(f"vu1_st(dmem, {VI(it_)}, {VF(fs_)}, {ldm});")
            if lk == "SQI":
                set_vi(it_, f"{VI(it_)} + 1u")
        elif lk in ("DIV", "SQRT", "RSQRT"):
            kind = ("DIV", "SQRT", "RSQRT").index(lk)
            assert q is None, "the unit stalls until Q is free"
            L.append(f"qn = vu1_div_calc({kind}, VU1_LANE({VF(fs_)}, {d['lfsf']}), VU1_LANE({VF(ft_)}, {d['lftf']}), &qf);")
            qnew = (13 if kind == 2 else 7) - 1
        elif lk == "MTIR":
            set_vi(it_, f"VU1_LANE({VF(fs_)}, {d['lfsf']})")
        elif lk == "MFIR":
            if ft_:
                L.append(f"vf{ft_} = vu1_lanes(_mm_set1_epi32((s32)(s16){VI(is_)}), vf{ft_}, {ldm});")
        elif lk in ("RNEXT", "RGET"):
            if lk == "RNEXT":
                L.append("rr = ((rr << 1 | (((rr >> 4) ^ (rr >> 22)) & 1)) & 0x7FFFFFu) | 0x3F800000u;")
            if ft_:
                L.append(f"vf{ft_} = vu1_lanes(_mm_set1_epi32((int)rr), vf{ft_}, {ldm});")
        elif lk == "RINIT":
            L.append(f"rr = 0x3F800000u | (VU1_LANE({VF(fs_)}, {d['lfsf']}) & 0x7FFFFFu);")
        elif lk == "RXOR":
            L.append(f"rr = 0x3F800000u | ((rr ^ VU1_LANE({VF(fs_)}, {d['lfsf']})) & 0x7FFFFFu);")
        elif lk == "MFP":
            if ft_:
                L.append(f"vf{ft_} = vu1_lanes(_mm_set1_epi32((int)p), vf{ft_}, {ldm});")
        elif lk == "XTOP":
            set_vi(it_, "vif1_top()")
        elif lk == "XITOP":
            set_vi(it_, "vif1_itop()")
        elif lk == "XGKICK":
            L.append(f"vu1_xgkick({VI(is_)});")
        elif lk == "EFU":
            assert p is None, "the unit stalls until P is free"
            L.append(f"{{ VF e_; vstore(&e_, {VF(fs_)}); pn = vu1_efu_calc(0x{imm:X}u, &e_, {d['lfsf']}); }}")
            pnew = EFU_LATENCY[imm] - 1
        else:
            assert lk in ("NONE", "WAIT"), lk

        # The upper result is written last.
        if ureg == 32:
            L.append(f"acc = vu1_lanes(ur_, acc, {udm});")
        elif ureg:
            L.append(f"vf{ureg} = vu1_lanes(ur_, vf{ureg}, {udm});")

        # The state after the pair, one cycle (plus the stall) later.
        k = stall + 1
        vf = [max(0, o - k) for o in S.vf]
        if ureg is not None and 0 < ureg < 32:
            vf[ureg] = 3
        if d["lw"] > 0:
            vf[d["lw"]] = 3
        fq = []
        for off, kind in rem:
            n = off - k
            L.append(f"fr{n} = fr{off}; fu{n} = fu{off};" if kind == "M" else f"fc{n} = fc{off};")
            fq.append((n, kind))
        if new and new[0] == "M":
            L.append(f"fr3 = {new[1]}; fu3 = {new[2]};")
            fq.append((3, "M"))
        elif new and new[0] == "P":
            L.append(f"vu1_mac_payload({new[1]}, &fr3, &fu3);")
            fq.append((3, "M"))
        elif new:
            L.append(f"fc3 = {new[1]};")
            fq.append((3, "C"))
        if q is not None:
            q -= k
        if qnew is not None:
            q = qnew
        if p is not None:
            p -= k
        if pnew is not None:
            p = pnew
        entry = ((hreg + 1) & 31) | hflag << 5 | min(stall, 63) << 6
        L.append("ho3 = ho2; ho2 = ho1; ho1 = ho0;" + (" ho0 = hn_;" if hreg >= 0 else ""))
        L.append(f"hr = (hr << 12 | 0x{entry:X}u) & 0xFFFFFFFFFFFFull; cyc += {k}; np++;")
        S2 = State(tuple(vf), q, p, tuple(fq), pend, S.ending or bool(d["ebit"]))

        # Where control goes: the previous pair's branch applies after this one.
        nxt = (pc + 8) & 0x3FFF
        tp = S.pend
        if S.ending:
            return L, self.end(S2, tp, nxt)
        if tp is None:   # falling through the end of micro memory to 0 could loop too
            return L, ("goto", self.ref(nxt, S2), self.bail(nxt, S2) if nxt <= pc else None)
        if tp[0] == "J":   # returns from the calls that can get here; others bail out
            out = self.bail("jt", S2)
            return L, ("switch", [(t, self.ref(t, S2)) for t in sorted(self.u.jr_targets[tp[1]])], out)
        guard = None
        if tp[1] <= pc:   # a loop (every one goes back somewhere): watch the run cycle limit
            guard = self.bail(tp[1], S2)
        if tp[0] == "B":
            return L, ("goto", self.ref(tp[1], S2), guard)
        return L, ("cond", self.ref(tp[1], S2), self.ref(nxt, S2), guard, self.bail(nxt, S2) if nxt <= pc else None)

    def end(self, S: State, tp, nxt: int) -> tuple:
        """The run ends after this pair: the pipeline drains (run() in gfx_vu1.c)."""
        L = []
        drain = max([0] + [o for o, _ in S.fq] + [x for x in (S.q, S.p) if x is not None])
        for off, kind in S.fq:
            L.append(f"VU1_APPLY_MAC(fr{off}, fu{off});" if kind == "M" else f"clip = fc{off};")
        if S.q is not None:
            L.append("q = qn; st = (st & ~0x30u) | qf | qf << 6;")
        if S.p is not None:
            L.append("p = pn;")
        if drain:
            L.append(f"cyc += {drain};")
        vf = [max(0, o - drain) for o in S.vf]
        for r in range(1, 32):
            if vf[r]:
                L.append(f"vu->vf_ready[{r}] = cyc + {vf[r]};")
        key = vf_key(vf)
        if tp is None:
            tpc, targets = f"0x{nxt:04X}u", [nxt]
        elif tp[0] == "B":
            tpc, targets = f"0x{tp[1]:04X}u", [tp[1]]
        elif tp[0] == "C":
            tpc, targets = f"tk ? 0x{tp[1]:04X}u : 0x{nxt:04X}u", [tp[1], nxt]
        else:
            tpc, targets = "jt", []
        for t in targets:
            self.ends.add((t, key))
        self.end_keys.add(key)
        L.append(f"vu->tpc = {tpc};")
        return ("end", L)

    # ---------------------------------------------------------------- C

    def bail_code(self, pc: object, S: State) -> list[str]:
        """Hand the run to the interpreter before the pair at pc, with the whole
        pipeline state in g_vu1."""
        L = []
        for i, (off, kind) in enumerate(S.fq):
            if kind == "M":
                L.append(f"{{ u32 m_ = vu1_mac_of(fr{off}, fu{off}); "
                         f"vu->fq[{i}] = (Vu1Flag){{cyc + {off}, m_, vu1_zsuo(m_), 0, 1, 0}}; }}")
            else:
                L.append(f"vu->fq[{i}] = (Vu1Flag){{cyc + {off}, 0, 0, fc{off}, 0, 1}};")
        L.append(f"vu->fq_head = 0; vu->fq_n = {len(S.fq)};")
        L.append(f"vu->q_ready = cyc + {S.q}; vu->q_next = qn; vu->q_flags = qf;" if S.q is not None
                 else "vu->q_ready = 0;")
        L.append(f"vu->p_ready = cyc + {S.p}; vu->p_next = pn;" if S.p is not None else "vu->p_ready = 0;")
        due = [x for x in ([S.fq[0][0]] if S.fq else []) + [S.q, S.p] if x is not None]
        L.append(f"vu->due = cyc + {min(due)};" if due else "vu->due = UINT64_MAX;")
        for r in range(1, 32):
            if S.vf[r]:
                L.append(f"vu->vf_ready[{r}] = cyc + {S.vf[r]};")
        L.append("vu1_gen_hist_put(hr, ho0, ho1, ho2, ho3);")
        if S.pend is None:
            L.append("vu->branch_pending = 0;")
        elif S.pend[0] == "B":
            L.append(f"vu->branch_pending = 1; vu->branch_target = 0x{S.pend[1]:04X}u;")
        elif S.pend[0] == "C":
            L.append(f"vu->branch_pending = tk; vu->branch_target = 0x{S.pend[1]:04X}u;")
        else:
            L.append("vu->branch_pending = 1; vu->branch_target = jt;")
        if S.ending:
            L.append("rc->ending = 1;")
        L.append(f"vu->tpc = {pc if isinstance(pc, str) else f'0x{pc:04X}u'};")
        return L

    def emit(self, out: list[str]) -> None:
        u = self.u
        vf_used, vf_set, vi_used, vi_set = set(), set(), set(), set()
        acc_set = False
        for pc, d in u.dec.items():
            uk, lk = UKN[d["uk"]], LKN[d["lk"]]
            if uk != "NOP":
                vf_used |= {d["ufs"], d["uft"]}
                w = {"FMAC": d["ufd"], "OPMSUB": d["ufd"], "ITOF": d["uft"], "FTOI": d["uft"], "ABS": d["uft"]}.get(uk)
                if uk in ("FMAC", "OPMULA") and d["uacc"] or uk == "OPMULA":
                    acc_set = True
                elif w is not None:
                    vf_set.add(w)
            if not d["ibit"]:
                vf_used |= {d["lfs"], d["lft"]}
                vi_used |= {d["lis"], d["lit"], d["lid"], 1}
                vf_set.add(d["lft"])
                vi_set |= {d["lis"], d["lit"], d["lid"], 1}
        vf_used = sorted((vf_used | vf_set) - {0})
        vf_set = sorted(vf_set - {0})
        vi_used = sorted((vi_used | vi_set) - {0})
        vi_set = sorted(vi_set - {0})

        # Emission order: depth first, falling through where possible.
        seq, seen = [], set()
        stack = sorted(set(self.entries.values()), reverse=True)
        while stack:
            i = stack.pop()
            while i is not None and i not in seen:
                seen.add(i)
                seq.append(i)
                x, i = self.xfer[i], None
                if x[0] == "goto" and x[1][0] == "N":
                    i = x[1][1]
                elif x[0] == "cond":
                    if x[1][0] == "N":
                        stack.append(x[1][1])
                    if x[2][0] == "N":
                        i = x[2][1]
                elif x[0] == "switch":
                    stack += [r[1] for _, r in reversed(x[1]) if r[0] == "N"]
        assert len(seq) == len(self.order)

        n = u.idx
        body = []
        out.append(f"/* unit {n}: {u.where}, {len(u.words)} pairs, {len(self.order)} states */")
        out, whole = body, out
        out.append("{")
        out.append("    Vu1State *const vu = &g_vu1;")
        out.append("    u8 *const dmem = vu1_data_mem();")
        for r in vf_used:
            out.append(f"    V4 vf{r} = vload(&vu->vf[{r}]);")
        out.append("    V4 acc = vload(&vu->acc);")
        for r in vi_used:
            out.append(f"    u32 vi{r} = vu->vi[{r}];")
        out.append("    u32 q = vu->q, p = vu->p, ii = vu->i, rr = vu->r;")
        out.append("    u32 clip = vu->clip, clipl = vu->clip_last, st = vu->status;")
        out.append("    u32 qn = 0, qf = 0, pn = 0, fc0 = 0, fc1 = 0, fc2 = 0, fc3 = 0, jt = 0;")
        out.append("    u32 hn_ = 0, ho0, ho1, ho2, ho3;")
        out.append("    V4 mr, mu, sk0, sk1, sk2, sk3;")
        out.append("    V4 fr0 = _mm_setzero_si128(), fu0 = fr0, fr1 = fr0, fu1 = fr0, fr2 = fr0, fu2 = fr0, "
                   "fr3 = fr0, fu3 = fr0;")
        out.append("    int tk = 0, ret;")
        out.append("    u64 cyc = vu->cycle, np = 0, hr = vu1_gen_hist_get(&ho0, &ho1, &ho2, &ho3);")
        out.append("    vu1_mac_payload(vu->mac, &mr, &mu);")
        out.append("    VU1_STICKY_RESET();")
        out.append("    switch (e) {")
        for (pc, key), i in sorted(self.entries.items()):
            out.append(f"    case {self.entry_index[(pc, key)]}: goto N{i};")
        out.append("    default: return -1;")
        out.append("    }")
        for pos, i in enumerate(seq):
            nxt_i = seq[pos + 1] if pos + 1 < len(seq) else None
            out.append(f"N{i}: {{")
            out += ["    " + s for s in self.code[i]]
            x = self.xfer[i]
            if x[0] == "end":
                out += ["    " + s for s in x[1]]
                out.append("    goto x_end;")
            elif x[0] == "bail":
                out.append(f"    goto B{x[1]};")
            elif x[0] == "goto":
                if x[2] is not None:
                    out.append(f"    if (cyc - rc->start > VU1_GEN_CYCLE_GUARD) goto B{x[2]};")
                if not (x[1][0] == "N" and x[1][1] == nxt_i):
                    out.append(f"    goto {x[1][0]}{x[1][1]};")
            elif x[0] == "switch":
                out.append(f"    if (cyc - rc->start > VU1_GEN_CYCLE_GUARD) goto B{x[2]};")
                out.append("    switch (jt) {")
                for t, r in x[1]:
                    out.append(f"    case 0x{t:04X}u: goto {r[0]}{r[1]};")
                out.append(f"    default: goto B{x[2]};")
                out.append("    }")
            else:   # cond
                t = f"goto {x[1][0]}{x[1][1]};"
                if x[3] is not None:
                    t = f"{{ if (cyc - rc->start > VU1_GEN_CYCLE_GUARD) goto B{x[3]}; {t} }}"
                out.append(f"    if (tk) {t}")
                if x[4] is not None:
                    out.append(f"    if (cyc - rc->start > VU1_GEN_CYCLE_GUARD) goto B{x[4]};")
                if not (x[2][0] == "N" and x[2][1] == nxt_i):
                    out.append(f"    goto {x[2][0]}{x[2][1]};")
            out.append("}")
        for b, (pc, S) in enumerate(self.bail_list):
            out.append(f"B{b}: {{")
            out += ["    " + s for s in self.bail_code(pc, S)]
            out.append("    goto x_bail;")
            out.append("}")
        out.append("x_bail:")
        out.append("    ret = 0;")
        out.append("    g_vu1_gen_bails++;")
        out.append("    goto x_out;")
        out.append("x_end:")
        out.append("    ret = 1;")
        out.append("x_out:")
        for r in vf_set:
            out.append(f"    vstore(&vu->vf[{r}], vf{r});")
        if acc_set:
            out.append("    vstore(&vu->acc, acc);")
        for r in vi_set:
            out.append(f"    vu->vi[{r}] = vi{r};")
        out.append("    vu->q = q; vu->p = p; vu->i = ii; vu->r = rr;")
        out.append("    vu->clip = clip; vu->clip_last = clipl;")
        out.append("    {")
        out.append("        u32 m = vu1_mac_of(mr, mu);")
        out.append("        vu->mac = m;")
        out.append("        vu->status = (st & ~0xFu) | vu1_zsuo(m) | vu1_sticky(sk0, sk1, sk2, sk3) << 6;")
        out.append("    }")
        out.append("    vu->cycle = cyc;")
        out.append("    vu->pairs += np;")
        out.append("    return ret;")
        out.append("}")
        out = whole

        # Two versions: the second for runs that start with the four MAC
        # sticky status bits set, which stay set without FSSET, so it
        # accumulates nothing and the compiler drops the flag work of every
        # FMAC whose MAC flags nothing reads.
        full = STICKY_VARIANT and not any(u.lower(pc) == "FSSET" for pc in u.dec)
        for suffix, sf in [("", 0)] + ([("_f", 1)] if full else []):
            out.append(f"#define VU1_SF {sf}")
            out.append(f"static int unit{n}{suffix}(Vu1Run *rc, int e)")
            out += body
            out.append("#undef VU1_SF")
        out.append("")

        # Entry: by address and pending VF writes, once the code checks out.
        words = []
        rng = []
        for addr, cnt in u.ranges():
            rng.append(f"{{0x{addr:04X}, {cnt}, {len(words)}}}")
            words += [(u.words[(addr + 8 * k) & 0x3FFF][1] << 32) | u.words[(addr + 8 * k) & 0x3FFF][0]
                      for k in range(cnt)]
        out.append(f"static const u64 unit{n}_words[] = {{")
        for k in range(0, len(words), 4):
            out.append("    " + " ".join(f"0x{w:016X}ull," for w in words[k:k + 4]))
        out.append("};")
        out.append(f"static const GenRange unit{n}_ranges[] = {{{', '.join(rng)}}};")
        out.append("")
        out.append(f"static int enter{n}(Vu1Run *rc, u32 pc, u64 key)")
        out.append("{")
        out.append("    int e;")
        out.append("    switch (pc) {")
        by_pc: dict[int, list[tuple[int, int]]] = {}
        for (pc, key), idx in self.entry_index.items():
            by_pc.setdefault(pc, []).append((key, idx))
        for pc in sorted(by_pc):
            out.append(f"    case 0x{pc:04X}:")
            conds = " else ".join(f"if (key == 0x{key:X}ull) e = {idx};" for key, idx in sorted(by_pc[pc]))
            out.append(f"        {conds}")
            out.append("        else return -1;")
            out.append("        break;")
        out.append("    default: return -1;")
        out.append("    }")
        out.append(f"    if (!unit_ok({n}, unit{n}_words, unit{n}_ranges, {len(rng)}, rc))")
        out.append("        return -1;")
        if full:
            out.append("    if ((g_vu1.status & 0x3C0u) == 0x3C0u)")
            out.append(f"        return unit{n}_f(rc, e);")
        out.append(f"    return unit{n}(rc, e);")
        out.append("}")
        out.append("")


def build(units: list[Unit]) -> list[UnitGen]:
    """Explore every unit from its entries until no new entry appears. Runs
    start at a unit's start (MSCAL) with nothing pending or with what any
    unit's runs end with (the program before may be another one), and where
    the unit's runs stop (MSCNT) as they stopped."""
    gens = [UnitGen(u) for u in units]
    for g in gens:
        g.add_entry(g.u.start, 0)
    while True:
        for g in gens:
            g.run()
        changed = False
        end_keys = sorted(set().union(*(g.end_keys for g in gens)))
        for g in gens:
            for pc, key in sorted(g.ends):
                changed |= g.add_entry(pc, key)
                changed |= g.add_entry(pc, 0)
            for key in end_keys:
                changed |= g.add_entry(g.u.start, key)
        if not changed:
            break
    for g in gens:
        g.finish()
        g.entry_index = {pk: k for k, pk in enumerate(sorted(g.entries))}
    return gens


def generate(packets: list[list[tuple[int, bytes, str]]]) -> tuple[str, int, int, int]:
    units: list[Unit] = []
    seen = set()
    for parts in packets:
        sig = tuple((a, c) for a, c, _ in parts)
        if sig not in seen:
            seen.add(sig)
            units.append(Unit(len(units), parts))
    gens = build(units)

    decs: dict[tuple[int, int], str] = {}
    for u in units:
        for lo_up in u.words.values():
            decs.setdefault(lo_up, f"D{len(decs)}")
    efu_ops = sorted({d["limm"] for u in units for d in u.dec.values() if not d["ibit"] and LKN[d["lk"]] == "EFU"})

    out = ["/* Generated by tools/pc/vu1rec.py from the game's VU1 microcode.",
           " * Derived from the game: do not edit, commit or share. */",
           '#include "vu1_core.h"', "",
           "#if defined(__GNUC__) || defined(__clang__)",
           '#pragma GCC diagnostic ignored "-Wunused-label"',
           '#pragma GCC diagnostic ignored "-Wunused-variable"',
           '#pragma GCC diagnostic ignored "-Wunused-but-set-variable"',
           "#endif", "",
           "/* Decodes of every pair, for the self-check against vu1_decode(). */"]
    for (lo, up), name in decs.items():
        out.append(f"static const Dec {name} = {c_dec(decode(lo, up))};")
    out += ["", "static const Dec *const g_all[] = {"]
    out += [f"    &{n}," for n in decs.values()]
    out += ["};", "",
            "int vu1_gen_selfcheck(void)", "{", "    int bad = 0;",
            "    for (unsigned i = 0; i < sizeof(g_all) / sizeof(g_all[0]); i++) {",
            "        Dec t;", "        vu1_decode(&t, g_all[i]->lo, g_all[i]->up);",
            "        bad += memcmp(&t, g_all[i], sizeof(t)) != 0;", "    }"]
    out += [f"    bad += vu1_efu_latency(0x{op:X}) != {EFU_LATENCY[op]};" for op in efu_ops]
    out += ["    return bad;", "}", "",
            "/* A MAC result lands in the flag registers. */",
            "#define VU1_APPLY_MAC(r, u) do { mr = (r); mu = (u); if (!VU1_SF) VU1_STICKY(r, u); } while (0)",
            "/* The status register (bits 0-3 follow the MAC flags; see vu1_sticky()). */",
            "#define VU1_STATUS ((st & ~0xFu) | vu1_zsuo(vu1_mac_of(mr, mu)) | vu1_sticky(sk0, sk1, sk2, sk3) << 6)",
            "/* Loops bail out to the interpreter this close to the run cycle limit, so",
            " * that it stops the run at the same pair (no pass through a unit is longer). */",
            "#define VU1_GEN_CYCLE_GUARD (VU1_RUN_CYCLE_LIMIT - 0x100000u)", "",
            "typedef struct GenRange {", "    u32 addr, n, at;", "} GenRange;", "",
            f"static u64 s_ok[{len(units)}], s_bad[{len(units)}];", "",
            "/* Is the unit's code in micro memory? Checked once per run. */",
            "static int unit_ok(int u, const u64 *w, const GenRange *r, int nr, const Vu1Run *rc)",
            "{",
            "    if (s_ok[u] == rc->serial)", "        return 1;",
            "    if (s_bad[u] == rc->serial)", "        return 0;",
            "    const u8 *code = vu1_code_mem();",
            "    for (int i = 0; i < nr; i++)",
            "        if (memcmp(code + r[i].addr, w + r[i].at, r[i].n * 8u)) {",
            "            s_bad[u] = rc->serial;", "            return 0;", "        }",
            "    s_ok[u] = rc->serial;", "    return 1;", "}", "",
            "/* Compiled code starts with an idle pipeline and the flags as vu1_step()",
            " * keeps them. */",
            "static int gen_idle(const Vu1Run *rc)", "{",
            "    const Vu1State *vu = &g_vu1;",
            "    if (vu->fq_n || vu->q_ready || vu->p_ready || vu->branch_pending || rc->ending)",
            "        return 0;",
            "    return (vu->status & 0xF) == vu1_zsuo(vu->mac) && !(vu->mac & (vu->mac >> 12) & 0xF);",
            "}", "",
            "/* Pending VF writes, 2 bits per register: cycles until ready. */",
            "static u64 gen_key(void)", "{",
            "    const Vu1State *vu = &g_vu1;", "    u64 k = 0;",
            "    for (int r = 1; r < 32; r++)",
            "        if (vu->vf_ready[r] > vu->cycle) {",
            "            u64 o = vu->vf_ready[r] - vu->cycle;",
            "            if (o > 3)", "                return ~0ull;",
            "            k |= o << (2 * r);", "        }",
            "    return k;", "}", ""]
    nstates = 0
    for g in gens:
        g.emit(out)
        nstates += len(g.order)

    # vu1_gen_exec: the units that start at TPC with the code found there.
    starts: dict[int, dict[int, list[int]]] = {}
    for g in gens:
        for pc in sorted({pc for pc, _ in g.entries}):
            lo, up = g.u.words[pc]
            starts.setdefault(pc, {}).setdefault((up << 32) | lo, []).append(g.u.idx)
    out += ["int vu1_gen_exec(Vu1Run *rc)", "{",
            "    u32 pc = g_vu1.tpc & VU1_CODE_MASK;", "    u64 w, key;", "    int r;",
            "    memcpy(&w, vu1_code_mem() + pc, 8);",
            "    switch (pc) {"]
    for pc in sorted(starts):
        out.append(f"    case 0x{pc:04X}:")
        out.append("        if (!gen_idle(rc) || (key = gen_key()) == ~0ull)")
        out.append("            return 0;")
        out.append("        switch (w) {")
        for w, us in sorted(starts[pc].items()):
            out.append(f"        case 0x{w:016X}ull:")
            for n in us:
                out.append(f"            if ((r = enter{n}(rc, pc, key)) >= 0)")
                out.append("                return r;")
            out.append("            return 0;")
        out.append("        }")
        out.append("        return 0;")
    out += ["    }", "    return 0;", "}", ""]
    return "\n".join(out), len(units), len(decs), nstates


def main() -> int:
    global MAX_STATES, STICKY_VARIANT
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", type=Path, default=ROOT / "build" / "vu1")
    ap.add_argument("--list", action="store_true", help="list the microcode found")
    ap.add_argument("--max-states", type=int, default=MAX_STATES,
                    help="states per pair address (1 makes the code hand over to the interpreter often: a test)")
    ap.add_argument("--no-sticky-variant", action="store_true",
                    help="only the general version of each unit (to test it with --vu1-check)")
    args = ap.parse_args()
    MAX_STATES = args.max_states
    STICKY_VARIANT = not args.no_sticky_variant
    orig = ROOT / "build" / "orig"
    files = [orig / "SLPM_662.80"] + sorted(orig.glob("*.bin"))
    packets = []
    for p in files:
        if p.exists():
            packets += find_packets(p.read_bytes(), p.name)
    if args.list:
        for parts in packets:
            for addr, code, where in parts:
                print(f"{where:24} -> micro 0x{addr:04X}, {len(code) // 8} pairs")
    text, nunits, npairs, nstates = generate(packets)
    args.out.mkdir(parents=True, exist_ok=True)
    path = args.out / "vu1_gen.c"
    if not path.exists() or path.read_text(encoding="utf-8") != text:
        path.write_text(text, encoding="utf-8", newline="\n")
    nup = sum(len(p) for p in packets)
    print(f"VU1 microcode: {nup} uploads in {len(packets)} packets, {nunits} distinct units, "
          f"{npairs} distinct pairs, {nstates} compiled states -> {path.name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
