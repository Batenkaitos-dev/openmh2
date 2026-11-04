#!/usr/bin/env python3
"""asm2c: the C forms of the game's 42 assembly functions.

Each `asm` function of src/sub_main/engine_math.c, vu1_packets.c and anim_curve.c gets, in
the file, a C function under `#if PLATFORM_PC`, one statement per instruction (the original
instruction in a comment), over include/vu0ops.h's VU0 state; the original stays in the
`#else`, so the matching build's bytes do not change. The MIPS registers are locals
(32-bit: nothing the functions compute needs more), the FPU registers f32 locals, branch
delay slots are executed where the hardware executes them, and `jal` is a call whose
arguments are the registers the callee's signature names (integers a0.., floats f12..).
The stack is a local array.

  asm2c.py --apply        rewrite the three files in place (idempotent: already converted
                          functions are left alone)
  asm2c.py --test-out F   write the C forms alone to F (the test harness's input)
  asm2c.py --syms         the symbols the functions load addresses of

Checks it makes (and reports as WARNING): a register written by QMTC2 whose lanes y, z or w
are read before they are written (QMTC2 is a 128-bit copy on the EE; include/vu0ops.h gives
those lanes a fixed value).
"""
import os
import re
import sys

NL = chr(10)
ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
FILES = ["src/sub_main/engine_math.c", "src/sub_main/vu1_packets.c", "src/sub_main/anim_curve.c"]

GPR_NAMES = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
             "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"]
INT_ARGS = ["a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3"]
DEST_BITS = {"x": 8, "y": 4, "z": 2, "w": 1}
BC = {"": -1, "x": 0, "y": 1, "z": 2, "w": 3, "q": 4, "i": 5}
OPS = {"vadd": ("ADD", 0), "vsub": ("SUB", 0), "vmul": ("MUL", 0), "vmadd": ("MADD", 0), "vmsub": ("MSUB", 0),
       "vmax": ("MAX", 0), "vmini": ("MIN", 0),
       "vadda": ("ADD", 1), "vsuba": ("SUB", 1), "vmula": ("MUL", 1), "vmadda": ("MADD", 1), "vmsuba": ("MSUB", 1)}
BCNAME = {-1: "VU0_BVEC", 0: "VU0_BX", 1: "VU0_BY", 2: "VU0_BZ", 3: "VU0_BW", 4: "VU0_BQ", 5: "VU0_BI"}
FMAC = {}
for _b, (_op, _acc) in OPS.items():
    for _s, _v in BC.items():
        FMAC[_b + _s] = (_op, _v, _acc)
# no MAX/MIN with the accumulator, and the bc forms of those exist for vmax and vmini only
for _k in [k for k, v in FMAC.items() if v[2] and v[0] in ("MAX", "MIN")]:
    del FMAC[_k]


class Fail(Exception):
    pass


def parse_functions(path):
    """-> list of dicts: name, static, ret, params [(type, name)], start, end (line indexes of the
    `asm` line and the closing brace), body [(label|None, insn|None)]"""
    lines = open(os.path.join(ROOT, path), encoding="utf-8").read().split("\n")
    funcs = []
    i = 0
    while i < len(lines):
        m = re.match(r"^(static )?asm (\w+(?: \*)?)\s+(\w+)\((.*)\)\s*$", lines[i])
        if m and i + 1 < len(lines) and lines[i + 1] == "{":
            j = i + 2
            while lines[j] != "}":
                j += 1
            params = []
            if m.group(4).strip() != "void":
                for p in m.group(4).split(","):
                    p = p.strip()
                    mm = re.match(r"^(.*?)(\w+)$", p)
                    params.append((mm.group(1).strip(), mm.group(2)))
            body = []
            for t in lines[i + 2:j]:
                t = re.sub(r"/\*.*?\*/", "", t).strip()
                if not t or t.startswith("."):
                    continue
                lm = re.match(r"^(\w+):\s*(.*)$", t)
                if lm:
                    body.append(("label", lm.group(1)))
                    t = lm.group(2).strip()
                    if not t:
                        continue
                body.append(("insn", t))
            funcs.append(dict(name=m.group(3), static=bool(m.group(1)), ret=m.group(2), params=params,
                              start=i, end=j, body=body, file=path, text=lines[i:j + 1]))
            i = j
        i += 1
    return funcs


# ------------------------------------------------------------------ the translation

def reg(r):
    r = r.strip().lstrip("$")
    if r in GPR_NAMES:
        return r
    raise Fail("register " + r)


def freg(r):
    m = re.match(r"^\$?f(\d+)$", r.strip())
    if not m:
        raise Fail("fpu register " + r)
    return "f" + m.group(1)


def vfn(r):
    m = re.match(r"^\$?vf(\d+)$", r.strip())
    if not m:
        raise Fail("vf register " + r)
    return int(m.group(1))


def vin(r):
    m = re.match(r"^\$?vi(\d+)$", r.strip())
    if not m:
        raise Fail("vi register " + r)
    return int(m.group(1))


def memop(s):
    m = re.match(r"^(-?(?:0x)?[0-9A-Fa-f]+)\((\w+)\)$", s.strip())
    if not m:
        raise Fail("memory operand " + s)
    off = int(m.group(1), 0)
    return reg(m.group(2)), off


def ea(base, off):
    if off == 0:
        return base
    return "%s %s 0x%X" % (base, "+" if off > 0 else "-", abs(off))


def destmask(sfx):
    if not sfx:
        return 15
    return sum(DEST_BITS[c] for c in sfx)


def destname(mask):
    return {8: "VU0_X", 4: "VU0_Y", 2: "VU0_Z", 1: "VU0_W", 14: "VU0_XYZ", 15: "VU0_XYZW"}.get(mask, "0x%X" % mask)


def split_ops(s):
    # operands separated by commas outside parentheses
    out, depth, cur = [], 0, ""
    for c in s:
        if c == "(":
            depth += 1
        if c == ")":
            depth -= 1
        if c == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += c
    if cur.strip():
        out.append(cur.strip())
    return out


class Func:
    def __init__(self, f, protos):
        self.f = f
        self.protos = protos
        self.used = set()      # GPRs
        self.fused = set()     # FPU registers
        self.q128 = set()      # registers used as 128-bit holders (lq/sq with a non-sp base)
        self.tmp = 0
        self.temps = []
        self.lanes = {}        # vf -> set of lanes (0..3) holding QMTC2's fixed lanes (1..3), to warn
        self.syms = set()
        self.uses_sp = False
        self.warns = []

    # --- instructions that need no delay slot
    def tr(self, text):
        """one instruction -> (list of C statements, kind, extra). kind: 'plain', 'branch' (extra =
        (cond C expression or None, label)), 'jal' (extra = callee), 'ret'"""
        parts = text.split(None, 1)
        mn = parts[0]
        ops = split_ops(parts[1]) if len(parts) > 1 else []
        c = "/* %s */" % text

        def R(r):
            n = reg(r)
            if n != "zero":
                self.used.add(n)
            return n

        def F(r):
            n = freg(r)
            self.fused.add(n)
            return n

        if mn in ("nop", "vnop", "vwaitq"):
            return [c], "plain", None
        # --- branches and jumps
        if mn in ("bc1t", "bc1f"):
            return [], "branch", ("fcc" if mn == "bc1t" else "!fcc", ops[0])
        if mn == "bnez":
            return [], "branch", ("%s != 0" % R(ops[0]), ops[1])
        if mn == "beqz":
            return [], "branch", ("%s == 0" % R(ops[0]), ops[1])
        if mn in ("b", "j"):
            return [], "branch", (None, ops[0])
        if mn == "jal":
            return [], "jal", ops[0]
        if mn == "jr":
            if ops[0] != "ra":
                raise Fail("jr " + ops[0])
            return [], "ret", None
        # --- integer
        if mn == "lui":
            return ["%s = (unsigned int)(%s) << 16; %s" % (R(ops[0]), ops[1], c)], "plain", None
        if mn == "ori":
            return ["%s = %s | (%s); %s" % (R(ops[0]), R(ops[1]) if reg(ops[1]) != "zero" else "0", ops[2], c)], "plain", None
        if mn == "la":
            self.syms.add(ops[1])
            return ["%s = (unsigned int)(__SIZE_TYPE__)%s; %s" % (R(ops[0]), ops[1], c)], "plain", None
        if mn in ("addiu", "addi"):
            if reg(ops[0]) == "sp":
                self.uses_sp = True
            a = R(ops[1]) if reg(ops[1]) != "zero" else "0"
            return ["%s = %s + (%s); %s" % (R(ops[0]), a, ops[2], c)], "plain", None
        if mn in ("addu", "daddu"):
            a = R(ops[1]) if reg(ops[1]) != "zero" else "0"
            b = R(ops[2]) if reg(ops[2]) != "zero" else "0"
            return ["%s = %s + %s; %s" % (R(ops[0]), a, b, c)], "plain", None
        if mn == "slt":
            a = R(ops[1]) if reg(ops[1]) != "zero" else "0"
            b = R(ops[2]) if reg(ops[2]) != "zero" else "0"
            return ["%s = (int)%s < (int)%s; %s" % (R(ops[0]), a, b, c)], "plain", None
        if mn in ("lw", "sw", "ld", "sd", "lq", "sq"):
            base, off = memop(ops[1])
            if base == "sp":
                self.uses_sp = True
            rt = R(ops[0])
            a = ea(R(base), off)
            if mn == "lw":
                return ["%s = PC_LW(%s); %s" % (rt, a, c)], "plain", None
            if mn == "sw":
                return ["PC_SW(%s, %s); %s" % (a, rt, c)], "plain", None
            if mn == "ld":
                return ["%s = (unsigned int)PC_LD(%s); %s" % (rt, a, c)], "plain", None
            if mn == "sd":
                return ["PC_SD(%s, %s); %s" % (a, rt, c)], "plain", None
            # lq/sq: with sp as the base, a register spilled (its low 32 bits); else a 128-bit holder
            if base == "sp":
                if mn == "lq":
                    return ["%s = PC_LW(%s); %s" % (rt, a, c)], "plain", None
                return ["PC_SQ32(%s, %s); %s" % (a, rt, c)], "plain", None
            self.q128.add(rt)
            if mn == "lq":
                return ["PC_LQ(%s_q, %s); %s" % (rt, a, c)], "plain", None
            return ["PC_SQ(%s, %s_q); %s" % (a, rt, c)], "plain", None
        # --- FPU
        if mn == "mtc1":
            return ["%s = PC_U2F(%s); %s" % (F(ops[1]), R(ops[0]) if reg(ops[0]) != "zero" else "0", c)], "plain", None
        if mn == "mfc1":
            return ["%s = PC_F2U(%s); %s" % (R(ops[0]), F(ops[1]), c)], "plain", None
        if mn == "mov.s":
            return ["%s = %s; %s" % (F(ops[0]), F(ops[1]), c)], "plain", None
        if mn in ("add.s", "sub.s", "mul.s"):
            o = {"add.s": "+", "sub.s": "-", "mul.s": "*"}[mn]
            return ["%s = %s %s %s; %s" % (F(ops[0]), F(ops[1]), o, F(ops[2]), c)], "plain", None
        if mn in ("c.lt.s", "c.le.s", "c.eq.s"):
            o = {"c.lt.s": "<", "c.le.s": "<=", "c.eq.s": "=="}[mn]
            return ["fcc = %s %s %s; %s" % (F(ops[0]), o, F(ops[1]), c)], "plain", None
        if mn in ("lwc1", "swc1"):
            base, off = memop(ops[1])
            a = ea(R(base), off)
            if mn == "lwc1":
                return ["%s = PC_LWC1(%s); %s" % (F(ops[0]), a, c)], "plain", None
            return ["PC_SWC1(%s, %s); %s" % (a, F(ops[0]), c)], "plain", None
        # --- COP2
        base = mn
        if base.endswith(".ni"):
            base = base[:-3]
        if base == "qmtc2":
            n = vfn(ops[1])
            self.lanes[n] = {1, 2, 3}
            return ["vu0_qmtc2(%d, %s); %s" % (n, R(ops[0]) if reg(ops[0]) != "zero" else "0", c)], "plain", None
        if base == "qmfc2":
            return ["%s = vu0_qmfc2(%d); %s" % (R(ops[0]), vfn(ops[1]), c)], "plain", None
        if base == "ctc2":
            return ["vu0_ctc2(%d, %s); %s" % (vin(ops[1]), R(ops[0]) if reg(ops[0]) != "zero" else "0", c)], "plain", None
        if base == "cfc2":
            return ["%s = vu0_cfc2(%d); %s" % (R(ops[0]), vin(ops[1]), c)], "plain", None
        if base == "lqc2":
            b, off = memop(ops[1])
            return ["vu0_lq(%d, %s); %s" % (vfn(ops[0]), ea(R(b), off), c)], "plain", None
        if base == "sqc2":
            b, off = memop(ops[1])
            return ["vu0_sq(%d, %s); %s" % (vfn(ops[0]), ea(R(b), off), c)], "plain", None
        # VU upper/lower instructions: name[.dest]
        name, _, sfx = base.partition(".")
        dest = destmask(sfx)
        if name in FMAC:
            op, bc, acc = FMAC[name]
            fd = None if acc else vfn(ops[0])
            fs = vfn(ops[1])
            if bc == -1:
                ft = vfn(ops[2])
            elif bc in (4, 5):
                ft = 0
                if ops[2].upper() != ("Q" if bc == 4 else "I"):
                    raise Fail("operand " + ops[2])
            else:
                m = re.match(r"^\$?vf(\d+)([xyzw])$", ops[2].strip())
                if not m or "xyzw".index(m.group(2)) != bc:
                    raise Fail("broadcast operand " + ops[2])
                ft = int(m.group(1))
            if acc and ops[0].upper() != "ACC":
                raise Fail("ACC operand")
            return ["vu0_fmac(VU0_%s, %s, %d, %s, %d, %d, %d); %s" % (op, BCNAME[bc], acc, destname(dest), fd or 0, fs, ft, c)], "plain", ("fmac", dest, fd, fs, ft, bc)
        if name in ("vopmula", "vopmsub"):
            ms = name == "vopmsub"
            fd = vfn(ops[0]) if ms else 0
            return ["vu0_opmula(%d, %d, %d, %d); %s" % (ms, fd, vfn(ops[1]), vfn(ops[2]), c)], "plain", ("fmac", 14, fd if ms else None, vfn(ops[1]), vfn(ops[2]), -1)
        if name == "vmove":
            return ["vu0_move(%s, %d, %d); %s" % (destname(dest), vfn(ops[0]), vfn(ops[1]), c)], "plain", ("move", dest, vfn(ops[0]), vfn(ops[1]))
        if name == "vmr32":
            return ["vu0_mr32(%s, %d, %d); %s" % (destname(dest), vfn(ops[0]), vfn(ops[1]), c)], "plain", ("move", dest, vfn(ops[0]), vfn(ops[1]))
        m = re.match(r"^(vitof|vftoi)(0|4|12|15)$", name)
        if m:
            fn = "vu0_itof" if m.group(1) == "vitof" else "vu0_ftoi"
            return ["%s(%s, %s, %d, %d); %s" % (fn, m.group(2), destname(dest), vfn(ops[0]), vfn(ops[1]), c)], "plain", ("move", dest, vfn(ops[0]), vfn(ops[1]))
        if name == "vabs":
            return ["vu0_abs(%s, %d, %d); %s" % (destname(dest), vfn(ops[0]), vfn(ops[1]), c)], "plain", ("move", dest, vfn(ops[0]), vfn(ops[1]))
        if name in ("vdiv", "vrsqrt"):
            m1 = re.match(r"^\$?vf(\d+)([xyzw])$", ops[1].strip())
            m2 = re.match(r"^\$?vf(\d+)([xyzw])$", ops[2].strip())
            if ops[0].upper() != "Q" or not m1 or not m2:
                raise Fail(text)
            return ["vu0_%s(%s, %d, %s, %d); %s" % (name, m1.group(1), "xyzw".index(m1.group(2)), m2.group(1), "xyzw".index(m2.group(2)), c)], "plain", ("src", [(int(m1.group(1)), "xyzw".index(m1.group(2))), (int(m2.group(1)), "xyzw".index(m2.group(2)))])
        if name == "vsqrt":
            m2 = re.match(r"^\$?vf(\d+)([xyzw])$", ops[1].strip())
            if ops[0].upper() != "Q" or not m2:
                raise Fail(text)
            return ["vu0_vsqrt(%s, %d); %s" % (m2.group(1), "xyzw".index(m2.group(2)), c)], "plain", ("src", [(int(m2.group(1)), "xyzw".index(m2.group(2)))])
        if name == "vmtir":
            m1 = re.match(r"^\$?vf(\d+)([xyzw])$", ops[1].strip())
            return ["vu0_mtir(%d, %s, %d); %s" % (vin(ops[0]), m1.group(1), "xyzw".index(m1.group(2)), c)], "plain", ("src", [(int(m1.group(1)), "xyzw".index(m1.group(2)))])
        if name == "vmfir":
            return ["vu0_mfir(%s, %d, %d); %s" % (destname(dest), vfn(ops[0]), vin(ops[1]), c)], "plain", ("move0", dest, vfn(ops[0]))
        if name in ("viadd", "visub", "viand", "vior"):
            return ["vu0_i%s(%d, %d, %d); %s" % (name[2:], vin(ops[0]), vin(ops[1]), vin(ops[2]), c)], "plain", None
        if name == "viaddi":
            return ["vu0_iaddi(%d, %d, %d); %s" % (vin(ops[0]), vin(ops[1]), int(ops[2], 0), c)], "plain", None
        raise Fail("unknown instruction: " + text)

    def callee_args(self, callee):
        if callee not in self.protos:
            raise Fail("unknown callee " + callee)
        ret, params = self.protos[callee]
        ints, flts, args = 0, 0, []
        for t, n in params:
            if t.strip() == "f32":
                args.append("f%d" % (12 + flts))
                self.fused.add("f%d" % (12 + flts))
                flts += 1
            else:
                r = INT_ARGS[ints]
                self.used.add(r)
                tt = t.strip()
                args.append("(%s)(__SIZE_TYPE__)%s" % (tt, r) if tt.endswith("*") else r)
                ints += 1
        return ret, args

    def gen(self):
        f = self.f
        body = f["body"]
        out = []
        labels = {n for k, n in body if k == "label"}
        used_labels = set()
        i = 0
        # which instruction indexes are delay slots
        consumed = set()
        stmts = []   # (label or None, [C lines])
        n = len(body)
        while i < n:
            k, v = body[i]
            if k == "label":
                stmts.append(("label", v))
                i += 1
                continue
            if i in consumed:
                i += 1
                continue
            lines, kind, extra = self.tr(v)
            if kind == "plain":
                if extra:
                    self.taint(extra, v)
                stmts.append(("c", lines))
                i += 1
                continue
            # a branch, jump, call or return: the next instruction is the delay slot
            if i + 1 >= n or body[i + 1][0] != "insn":
                raise Fail("no delay slot after " + v)
            dl, dkind, dextra = self.tr(body[i + 1][1])
            if dkind != "plain":
                raise Fail("branch in a delay slot: " + body[i + 1][1])
            if dextra:
                self.taint(dextra, body[i + 1][1])
            consumed.add(i + 1)
            c = "/* %s */" % v
            if kind == "branch":
                cond, label = extra
                used_labels.add(label)
                if cond is None:
                    stmts.append(("c", dl + ["goto %s; %s" % (label, c)]))
                else:
                    t = "c%d_" % self.tmp
                    self.tmp += 1
                    self.temps.append(t)
                    stmts.append(("c", ["%s = %s; %s" % (t, cond, c)] + dl + ["if (%s)" % t, "    goto %s;" % label]))
            elif kind == "jal":
                ret, args = self.callee_args(extra)
                call = "%s(%s)" % (extra, ", ".join(args))
                if ret == "f32":
                    call = "f0 = " + call
                    self.fused.add("f0")
                stmts.append(("c", dl + ["%s; %s" % (call, c)]))
            else:  # ret
                stmts.append(("c", dl + ["return%s; %s" % (" f0" if f["ret"] == "f32" else "", c)]))
                if f["ret"] == "f32":
                    self.fused.add("f0")
            i += 1
        # the registers the parameters arrive in
        init = []
        ints, flts = 0, 0
        for t, nm in f["params"]:
            if t.strip() == "f32":
                r = "f%d" % (12 + flts)
                flts += 1
                self.fused.add(r)
                init.append("%s __attribute__((unused)) = %s;" % (r, nm))
            else:
                r = INT_ARGS[ints]
                ints += 1
                self.used.add(r)
                init.append("%s __attribute__((unused)) = (unsigned int)(__SIZE_TYPE__)%s;" % (r, nm))
        decl = []
        if "sp" in self.used:
            self.uses_sp = True
        order = [r for r in GPR_NAMES if r in self.used and r not in self.q128 and r != "sp"]
        for r in order:
            if not any(x.startswith(r + " ") for x in init):
                decl.append("unsigned int %s __attribute__((unused)) = 0;" % r)
        for r in sorted(self.q128):
            decl.append("unsigned int %s_q[4] __attribute__((unused));" % r)
        for r in sorted(self.fused, key=lambda s: int(s[1:])):
            if not any(x.startswith(r + " ") for x in init):
                decl.append("f32 %s __attribute__((unused)) = 0;" % r)
        if any("fcc" in x for s in stmts if s[0] == "c" for x in s[1]):
            decl.append("int fcc __attribute__((unused)) = 0;")
        for t in self.temps:
            decl.append("int %s __attribute__((unused));" % t)
        if self.uses_sp:
            # the frame: the largest sp adjustment (the game reads stack words it never wrote; zero here)
            fr = 0
            for k, v in f["body"]:
                m = re.match(r"^addiu sp, sp, -(0x[0-9A-Fa-f]+|\d+)$", v) if k == "insn" else None
                if m:
                    fr = max(fr, int(m.group(1), 0))
            fr = max(fr, 0x10)
            decl.append("unsigned char stk_[0x%X] __attribute__((aligned(16))) = {0};" % fr)
            decl.append("unsigned int sp = (unsigned int)(__SIZE_TYPE__)(stk_ + 0x%X);" % fr)
        # parameters feed their registers (declared initialised, in order)
        sig = "%s %s(%s)" % (f["ret"], f["name"],
                             ", ".join(("%s%s" % (t, nm)) if t.endswith("*") else ("%s %s" % (t, nm)) for t, nm in f["params"]) or "void")
        res = [("static " if f["static"] else "") + sig, "{"]
        for d in decl:
            res.append("    " + d)
        for x in init:
            reg0 = x.split(" ")[0]
            kindc = "f32 " if reg0.startswith("f") else "unsigned int "
            res.append("    " + kindc + x)
        for t, v in stmts:
            if t == "label":
                res.append("%s:;" % v)
            else:
                for l in v:
                    res.append("    " + l)
        res.append("}")
        for lab in used_labels:
            if lab not in labels:
                raise Fail("label " + lab)
        return res

    def taint(self, e, text):
        """QMTC2's lanes y, z, w are not the EE's: a read of them before a write is a warning."""
        kind = e[0]
        def rd(reg, lanes):
            if reg in self.lanes:
                bad = self.lanes[reg] & set(lanes)
                if bad:
                    self.warns.append("%s: %s reads lanes %s of vf%d, which QMTC2 left" % (self.f["name"], text, sorted(bad), reg))
        def wr(reg, mask):
            if reg in self.lanes:
                self.lanes[reg] -= {i for i in range(4) if mask & (8 >> i)}
        if kind == "fmac":
            _, dest, fd, fs, ft, bc = e
            lanes = [i for i in range(4) if dest & (8 >> i)]
            rd(fs, lanes)
            if bc == -1:
                rd(ft, lanes)
            elif 0 <= bc < 4:
                rd(ft, [bc])
            if fd is not None:
                wr(fd, dest)
        elif kind == "move":
            _, dest, fd, fs = e
            rd(fs, [i for i in range(4) if dest & (8 >> i)])
            wr(fd, dest)
        elif kind == "move0":
            wr(e[2], e[1])
        elif kind == "src":
            for r, l in e[1]:
                rd(r, [l])


def build(path_funcs, protos):
    out = {}
    warns = []
    syms = set()
    for f in path_funcs:
        fn = Func(f, protos)
        try:
            lines = fn.gen()
        except Fail as e:
            raise SystemExit("%s: %s: %s" % (f["file"], f["name"], e))
        out[f["name"]] = lines
        warns += fn.warns
        syms |= fn.syms
    return out, warns, syms


HELPERS = ("#include \"vu0ops.h\"",)


def convert_file(path, funcs, gen, write):
    lines = open(os.path.join(ROOT, path), encoding="utf-8").read().split("\n")
    text = "\n".join(lines)
    out = []
    pos = 0
    n_conv = 0
    for f in funcs:
        if f["name"] not in gen:
            continue
        # already converted: the line before the asm line is the #else of our #if PLATFORM_PC
        if f["start"] > 0 and lines[f["start"] - 1] == "#else" and not False:
            continue
        out += lines[pos:f["start"]]
        out.append("#if PLATFORM_PC")
        out += gen[f["name"]]
        out.append("#else")
        out += lines[f["start"]:f["end"] + 1]
        out.append("#endif")
        pos = f["end"] + 1
        n_conv += 1
    out += lines[pos:]
    # the include, after the last #include of the file's head
    joined = "\n".join(out)
    if '#include "vu0ops.h"' not in joined:
        idx = max(i for i, l in enumerate(out) if l.startswith("#include") and i < 60)
        out.insert(idx + 1, "")
        out.insert(idx + 2, "#if PLATFORM_PC")
        out.insert(idx + 3, '#include "vu0ops.h"')
        out.insert(idx + 4, "#endif")
    if write:
        open(os.path.join(ROOT, path), "w", encoding="utf-8", newline="\n").write("\n".join(out))
    return n_conv


def main():
    argv = sys.argv[1:]
    allf = {p: parse_functions(p) for p in FILES}
    flat = [f for p in FILES for f in allf[p]]
    protos = {f["name"]: (f["ret"], f["params"]) for f in flat}
    gen, warns, syms = build(flat, protos)
    for w in warns:
        print("WARNING:", w)
    if "--syms" in argv:
        print("\n".join(sorted(syms)))
        return
    if "--test-out" in argv:
        d = argv[argv.index("--test-out") + 1]
        os.makedirs(d, exist_ok=True)
        syms_addr = {}
        for line in open(os.path.join(ROOT, "config/symbols/sub_main.txt"), encoding="utf-8"):
            m = re.match(r"^(\w+) = (0x[0-9A-Fa-f]+);", line)
            if m:
                syms_addr[m.group(1)] = int(m.group(2), 16)
        o = ['/* Generated by tools/pc/ps2fp/asm2c.py --test-out: the C forms alone. */', '#include "asm_testpre.h"', ""]
        c = ['/* Generated by tools/pc/ps2fp/asm2c.py --test-out: one call per function, the table. */', ""]
        tab = []
        for f in flat:
            o.append("/* %s: %s */" % (f["file"], f["name"]))
            l = list(gen[f["name"]])
            if l[0].startswith("static "):
                l[0] = l[0][7:]
            o += l
            o.append("")
            args, ni, nf = [], 0, 0
            for t, nm in f["params"]:
                if t.strip() == "f32":
                    args.append("fp[%d]" % nf)
                    nf += 1
                else:
                    args.append("(%s)(__SIZE_TYPE__)ip[%d]" % (t.strip(), ni))
                    ni += 1
            rf = f["ret"] == "f32"
            c.insert(1, "%s %s(%s);" % (f["ret"], f["name"], ", ".join(("%s%s" % (t, nm)) if t.endswith("*") else ("%s %s" % (t, nm)) for t, nm in f["params"]) or "void"))
            c.append("static __attribute__((cdecl)) void call_%s(const unsigned *ip, const float *fp, float *ret)" % f["name"])
            c.append("{")
            c.append("    (void)ip; (void)fp; *ret = 0;")
            c.append("    %s%s(%s);" % ("*ret = " if rf else "", f["name"], ", ".join(args)))
            c.append("}")
            tab.append('    {"%s", 0x%08X, %d, %d, %d, call_%s},' % (f["name"], syms_addr[f["name"]], ni, nf, rf, f["name"]))
        c.append("")
        c.append("static const AsmFn g_fns[] = {")
        c += tab
        c.append("};")
        open(os.path.join(d, "gen_funcs.c"), "w").write(NL.join(o))
        open(os.path.join(d, "gen_calls.h"), "w").write(NL.join(c) + NL)
        print("%d functions" % len(flat))
        return
    if "--apply" in argv:
        n = 0
        for p in FILES:
            n += convert_file(p, allf[p], gen, True)
        print("%d functions converted (%d in total)" % (n, len(flat)))
        return
    print("%d functions parse and translate" % len(flat))


if __name__ == "__main__":
    main()
