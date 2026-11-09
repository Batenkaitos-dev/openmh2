#!/usr/bin/env python3
"""ps2fp: the arithmetic pass, as an LLVM IR rewriter.

It reads the textual IR clang emits before any optimisation (clang -O2
-Xclang -disable-llvm-passes -S -emit-llvm, with pc/core/crt/ps2fp.h force-
included) and rewrites, in every function except ps2fp's own, each operation
whose result differs between the EE and x86 into a call of the matching
ps2_* function that header defines:

  float fadd fsub fmul fdiv fneg, fcmp, sitofp, uitofp, fptosi, fptoui,
  calls of sqrtf and the sqrt and fabs intrinsics;
  double fadd fsub, sitofp, fptosi, fpext, fptrunc (MWCC's software doubles);
  sdiv srem udiv urem by anything but a constant other than 0 and -1;
  shl lshr ashr by anything but a constant.

Clang's own front-end constant folding has happened (the IR builder folds
what MWCC's front end folds); nothing else is folded, because the pass runs
before the optimiser. The helpers are always_inline: the second compile
(ps2fpcc.py) inlines them. Anything the pass does not know (a double
multiply, a vector float operation, an unknown float intrinsic, a 64-bit
variable division) is an error, named with its function, so that nothing is
silently left with x86 semantics.

Usage: ps2fp.py IN.ll OUT.ll [--stats]
Exit status: 0, or 1 with the errors on stderr.
"""
import os
import re
import sys
import zlib
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
ORDER_FILE = os.path.join(HERE, "mulorder.txt")

FLAGS = r"(?:(?:nnan|ninf|nsz|arcp|contract|afn|reassoc|fast)\s+)*"
INT_FLAGS = r"(?:(?:nuw|nsw|exact)\s+)*"
TAIL = r"((?:,\s*![\w.]+\s+!\d+)*)\s*$"          # , !dbg !12 ...
TOK = r"([^\s,]+)"                                 # an operand: %x, a constant

NAME = r'@(?:"([^"]*)"|([\w.$]+))'      # a global: @name, or @"\01name@@8" (decorated)
RE_DEFINE = re.compile(r"^define\b.*?" + NAME + r"\(")
RE_ARITH = re.compile(r"^(\s*)(%[\w.$-]+) = (fadd|fsub|fmul|fdiv|frem) " + FLAGS + r"(\S+) " + TOK + r",\s*" + TOK + TAIL)
RE_FNEG = re.compile(r"^(\s*)(%[\w.$-]+) = fneg " + FLAGS + r"(\S+) " + TOK + TAIL)
RE_FCMP = re.compile(r"^(\s*)(%[\w.$-]+) = fcmp " + FLAGS + r"(\w+) (\S+) " + TOK + r",\s*" + TOK + TAIL)
RE_CAST = re.compile(r"^(\s*)(%[\w.$-]+) = (sitofp|uitofp|fptosi|fptoui|fpext|fptrunc) (\S+) " + TOK + r" to (\S+)" + TAIL)
RE_INT2 = re.compile(r"^(\s*)(%[\w.$-]+) = (sdiv|srem|udiv|urem|shl|lshr|ashr) " + INT_FLAGS + r"(\S+) " + TOK + r",\s*" + TOK + TAIL)
RE_CALL = re.compile(r"^(\s*)(%[\w.$-]+) = (?:tail |musttail |notail )?call (?:[\w.]+ )*?(\S+) " + NAME + r"\(([^()]*)\)[^\n]*?" + TAIL)
RE_USED = re.compile(r"^@llvm\.(?:compiler\.)?used = appending global \[\d+ x ptr\] \[(.*)\], section \"llvm\.metadata\"\s*$")
RE_HELPER = re.compile(r"^define\b([^@]*?)(@(?:\"[^\"]*\"|[\w.$]+))\((.*)\)\s*(?:#\d+)?\s*(?:![\w.]+ !\d+\s*)*\{")  # metadata: !dbg, ubsan's !func_sanitize


def canon(g1, g2):
    """The source-level name of a global: without the \\01 prefix and the @@N
    of a decorated one (the IR text spells the byte 1 as backslash, 0, 1)."""
    n = g1 if g1 is not None else g2
    if n.startswith("\\01"):
        n = n[3:]
    n = re.sub(r"@@?\d+$", "", n)
    return n[len("__regcall3__"):] if n.startswith("__regcall3__") else n    # the core's default convention (pc/game_flags.txt)


def split_params(s):
    """The top-level comma-separated parameters of a define's list."""
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch in "(<[{":
            depth += 1
        elif ch in ")>]}":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


RE_ZEXT = re.compile(r"^\s*(%[\w.$-]+) = zext (?:i8|i16) \S+ to i32\b")
RE_ANYCALL =re.compile(r"\bcall\b[^@(]*" + NAME + r"\(")
RE_VECTOR_FP =re.compile(r"= (?:fadd|fsub|fmul|fdiv|frem|fneg|fcmp \w+|sitofp|uitofp|fptosi|fptoui|fpext|fptrunc) [^,]*<")
RE_CC = re.compile(r"\b(x86_\w+cc|fastcc|ccc|cc \d+)\b")

FLOAT_INTRINSIC = re.compile(r"^llvm\.\w+\.(?:v\d+)?(?:f32|f64|f16|f80|f128)\b|^llvm\.\w+\.f\d+$")

COMPARES = {
    # predicate: (helper, swap operands, negate)
    "oeq": ("ps2_feq", False, False), "ueq": ("ps2_feq", False, False),
    "one": ("ps2_feq", False, True), "une": ("ps2_feq", False, True),
    "olt": ("ps2_flt", False, False), "ult": ("ps2_flt", False, False),
    "ole": ("ps2_fle", False, False), "ule": ("ps2_fle", False, False),
    "ogt": ("ps2_flt", True, False), "ugt": ("ps2_flt", True, False),
    "oge": ("ps2_fle", True, False), "uge": ("ps2_fle", True, False),
}
FLOAT_ARITH = {"fadd": "ps2_fadd", "fsub": "ps2_fsub", "fmul": "ps2_fmul", "fdiv": "ps2_fdiv"}
DOUBLE_ARITH = {"fadd": "ps2_dpadd", "fsub": "ps2_dpsub", "fmul": "ps2_dpmul", "fdiv": "ps2_dpdiv"}
INT_WIDTH = {"i1": 1, "i8": 8, "i16": 16, "i32": 32, "i64": 64}


def is_const(tok):
    return not tok.startswith("%")


def int_const(tok):
    """The value of an integer constant operand, or None."""
    try:
        return int(tok)
    except ValueError:
        return None


def read_order(path=ORDER_FILE):
    """mulorder.txt: where MWCC puts the operands of a commutative float operation in the other order than the C
    source. One line per function and operation: `FILE FUNC OP N K K ...` (FILE relative to the repository, OP fadd or
    fmul, N the number of that operation in the function, K the ordinals, counted in the function's IR order, whose
    operands the pass exchanges). Result: {(file, func, op): (N, {K})}."""
    out = {}
    if not os.path.exists(path):
        return out
    with open(path, encoding="utf-8") as f:
        for ln in f:
            ln = ln.split("#")[0].split()
            if not ln:
                continue
            out[(ln[0], ln[1], ln[2])] = (int(ln[3]), {int(k) for k in ln[4:]})
    return out


class Rewriter:
    def __init__(self, text, name, order=None, trace=None):
        self.name = name
        self.rel = os.path.relpath(os.path.abspath(name), ROOT).replace(os.sep, "/")   # the source's name in mulorder.txt
        self.order = order if order is not None else {}
        self.trace = trace             # a path: trace build (MH2_FPTRACE): float add and multiply carry a site id, written there
        self.opk = Counter()           # (function, op) -> the operations met so far
        self.sites = []
        self.lines = text.split("\n")
        self.errors = []
        self.stats = Counter()
        self.cc = {}                   # helper name -> its calling convention text
        self.sym = {}                  # helper name -> its symbol in the IR (decorated on Windows)
        self.pattrs = {}               # helper name -> the attributes its parameters need at a call (inreg)
        self.defined = set()
        for ln in self.lines:
            m = RE_HELPER.match(ln)
            if m:
                sym = m.group(2)
                nm = canon(None, sym[1:].strip('"'))
                if not nm.startswith("ps2_"):
                    continue
                self.defined.add(nm)
                self.sym[nm] = sym
                c = RE_CC.search(m.group(1))
                self.cc[nm] = c.group(1) + " " if c else ""
                self.pattrs[nm] = ["inreg" if " inreg" in p else "" for p in split_params(m.group(3))]
        self.func = None
        self.n = 0
        self.fstats = {}               # function -> Counter of helper calls the pass made in it
        self.callees = {}              # function -> Counter of the functions it calls (fpcheck)
        self.internal = set()          # the functions with internal linkage (static)
        self.zexts = set()             # the i32 values of the current function made by zext of an i8/i16

    # ---------------------------------------------------------------- utils
    def err(self, ln, msg):
        self.errors.append("%s: %s: %s" % (self.name, self.func, msg + "  [" + ln.strip()[:140] + "]"))

    def tmp(self):
        self.n += 1
        return "%%ps2fp.%d" % self.n

    def call(self, helper, rtype, args, dst, ind, tail):
        """A call of a helper: dst = call rtype @helper(args)."""
        if helper not in self.defined:
            self.errors.append("%s: %s: the helper %s is not defined (ps2fp.h force-included?)" % (self.name, self.func, helper))
        self.stats[helper] += 1
        self.fstats.setdefault(self.func, Counter())[helper] += 1
        attrs = self.pattrs.get(helper, [])
        argl = []
        for i, a in enumerate(args):      # "type value": the parameter attributes go between
            ty, val = a.split(" ", 1)
            at = attrs[i] if i < len(attrs) else ""
            argl.append("%s %s%s" % (ty, at + " " if at else "", val))
        return "%s%s = call %s%s %s(%s)%s" % (ind, dst, self.cc.get(helper, ""), rtype,
                                              self.sym.get(helper, "@" + helper), ", ".join(argl), tail)

    def conv(self, op, src, ty, to, dst, ind, tail):
        return "%s%s = %s %s %s to %s%s" % (ind, dst, op, ty, src, to, tail)

    # -------------------------------------------------------------- rewrites
    def arith(self, m, ln):
        ind, dst, op, ty, a, b, tail = m.groups()
        if op == "frem":
            self.err(ln, "frem")
            return [ln]
        if ty == "float":
            helper, args = FLOAT_ARITH[op], ["float " + a, "float " + b]
            a_orig = (a, b)
            if op in ("fadd", "fmul"):
                k = self.opk[(self.func, op)]
                self.opk[(self.func, op)] += 1
                ent = self.order.get((self.rel, self.func, op))
                if ent and k in ent[1]:
                    args.reverse()     # MWCC's operand order (mulorder.txt)
                if self.trace:
                    sid = zlib.crc32(("%s:%s:%s:%d" % (self.rel, self.func, op, k)).encode()) & 0xFFFFFFFF
                    kind = ("c" if is_const(a_orig[0]) else "v") + ("c" if is_const(a_orig[1]) else "v")   # the source's operands: constant or not
                    self.sites.append("%08x %s %s %s %d %s" % (sid, self.rel, self.func, op, k, kind))
                    helper += "_t"
                    args.append("i32 %d" % (sid - (1 << 32) if sid >= 1 << 31 else sid))
            return [self.call(helper, "float", args, dst, ind, tail)]
        if ty == "double":
            h = DOUBLE_ARITH[op]
            out = []
            ia, ib, ir = self.tmp(), self.tmp(), self.tmp()
            out.append(self.conv("bitcast", a, "double", "i64", ia, ind, tail))
            out.append(self.conv("bitcast", b, "double", "i64", ib, ind, tail))
            out.append(self.call(h, "i64", ["i64 " + ia, "i64 " + ib], ir, ind, tail))
            out.append(self.conv("bitcast", ir, "i64", "double", dst, ind, tail))
            return out
        self.err(ln, "float operation on " + ty)
        return [ln]

    def fneg(self, m, ln):
        ind, dst, ty, a, tail = m.groups()
        if ty == "float":
            return [self.call("ps2_fneg", "float", ["float " + a], dst, ind, tail)]
        self.err(ln, "fneg on " + ty)
        return [ln]

    def fcmp(self, m, ln):
        ind, dst, pred, ty, a, b, tail = m.groups()
        if ty != "float":
            self.err(ln, "fcmp on " + ty)
            return [ln]
        if pred in ("true", "false"):
            return ["%s%s = icmp eq i32 0, %d%s" % (ind, dst, 0 if pred == "true" else 1, tail)]
        if pred == "ord":
            return ["%s%s = icmp eq i32 0, 0%s" % (ind, dst, tail)]
        if pred == "uno":
            return ["%s%s = icmp eq i32 0, 1%s" % (ind, dst, tail)]
        if pred not in COMPARES:
            self.err(ln, "fcmp " + pred)
            return [ln]
        helper, swap, neg = COMPARES[pred]
        if swap:
            a, b = b, a
        t = self.tmp()
        return [self.call(helper, "i32", ["float " + a, "float " + b], t, ind, tail),
                "%s%s = icmp %s i32 %s, 0%s" % (ind, dst, "eq" if neg else "ne", t, tail)]

    def cast(self, m, ln):
        ind, dst, op, ty, v, to, tail = m.groups()
        out = []
        if op in ("sitofp", "uitofp") and to == "float":
            w = INT_WIDTH.get(ty)
            if w is None or w > 32:
                self.err(ln, "%s from %s to float" % (op, ty))
                return [ln]
            src = v
            if w < 32:      # a small integer converts through a 32-bit register (cvt.s.w)
                src = self.tmp()
                out.append(self.conv("sext" if op == "sitofp" else "zext", v, ty, "i32", src, ind, tail))
            helper = "ps2_u2f" if (op == "uitofp" and w == 32) else "ps2_i2f"
            # a small unsigned (C promotes it: a zext, then sitofp i32): MWCC may use its
            # u32 sequence for it, which gives the same value (fpcheck allows either)
            if (op == "uitofp" and w < 32) or (op == "sitofp" and w == 32 and v in self.zexts):
                self.fstats.setdefault(self.func, Counter())["uflex"] += 1
            out.append(self.call(helper, "float", ["i32 " + src], dst, ind, tail))
            return out
        if op in ("fptosi", "fptoui") and ty == "float":
            w = INT_WIDTH.get(to)
            if w is None or w > 32:
                self.err(ln, "%s from float to %s" % (op, to))
                return [ln]
            helper = "ps2_f2i" if op == "fptosi" else "ps2_f2u"
            if w == 32:
                return [self.call(helper, "i32", ["float " + v], dst, ind, tail)]
            t = self.tmp()
            out.append(self.call(helper, "i32", ["float " + v], t, ind, tail))
            out.append(self.conv("trunc", t, "i32", to, dst, ind, tail))
            return out
        # MWCC's software doubles: litodp, dptoli, fptodp, dptofp
        if op in ("sitofp", "uitofp") and to == "double" and INT_WIDTH.get(ty, 64) <= 32 and not (op == "uitofp" and ty == "i32"):
            src = v
            if ty != "i32":     # a small integer converts through an int (litodp)
                src = self.tmp()
                out.append(self.conv("sext" if op == "sitofp" else "zext", v, ty, "i32", src, ind, tail))
            t = self.tmp()
            out.append(self.call("ps2_litodp", "i64", ["i32 " + src], t, ind, tail))
            out.append(self.conv("bitcast", t, "i64", "double", dst, ind, tail))
            return out
        if op == "fptosi" and ty == "double" and INT_WIDTH.get(to, 64) <= 32:
            t = self.tmp()
            out.append(self.conv("bitcast", v, "double", "i64", t, ind, tail))
            if to == "i32":
                out.append(self.call("ps2_dptoli", "i32", ["i64 " + t], dst, ind, tail))
            else:
                r = self.tmp()
                out.append(self.call("ps2_dptoli", "i32", ["i64 " + t], r, ind, tail))
                out.append(self.conv("trunc", r, "i32", to, dst, ind, tail))
            return out
        if op == "fpext" and ty == "float" and to == "double":
            t = self.tmp()
            return [self.call("ps2_fptodp", "i64", ["float " + v], t, ind, tail), self.conv("bitcast", t, "i64", "double", dst, ind, tail)]
        if op == "fptrunc" and ty == "double" and to == "float":
            t = self.tmp()
            return [self.conv("bitcast", v, "double", "i64", t, ind, tail), self.call("ps2_dptofp", "float", ["i64 " + t], dst, ind, tail)]
        self.err(ln, "%s %s to %s" % (op, ty, to))
        return [ln]

    def int2(self, m, ln):
        ind, dst, op, ty, a, b, tail = m.groups()
        w = INT_WIDTH.get(ty)
        if op in ("shl", "lshr", "ashr"):
            if is_const(b):
                return [ln]
            if w not in (32, 64):
                self.err(ln, "variable %s of %s" % (op, ty))
                return [ln]
            out, cnt = [], b
            if w == 64:    # the count is an i64 in IR; the helper takes the low bits
                cnt = self.tmp()
                out.append(self.conv("trunc", b, "i64", "i32", cnt, ind, tail))
            out.append(self.call("ps2_%s%d" % (op, w), ty, ["%s %s" % (ty, a), "i32 " + cnt], dst, ind, tail))
            return out
        v = int_const(b)
        if v is not None and not (v == 0 or (v == -1 and op in ("sdiv", "srem"))):
            return [ln]      # a constant divisor: the compiler divides by it, never traps
        if w == 32:
            return [self.call("ps2_" + op, "i32", ["i32 " + a, "i32 " + b], dst, ind, tail)]
        if w in (8, 16):
            ext = "sext" if op in ("sdiv", "srem") else "zext"
            ta, tb, tr = self.tmp(), self.tmp(), self.tmp()
            return [self.conv(ext, a, ty, "i32", ta, ind, tail), self.conv(ext, b, ty, "i32", tb, ind, tail),
                    self.call("ps2_" + op, "i32", ["i32 " + ta, "i32 " + tb], tr, ind, tail),
                    self.conv("trunc", tr, "i32", ty, dst, ind, tail)]
        self.err(ln, "%s of %s by a variable" % (op, ty))
        return [ln]

    def callrw(self, m, ln):
        ind, dst, rty, n1, n2, args, tail = m.groups()
        name = canon(n1, n2)
        if name in ("sqrtf", "llvm.sqrt.f32") and rty == "float":
            a = re.search(r"float (?:[\w ]*? )?(%[\w.$-]+|[^\s,]+)\s*$", args.strip())
            if a:
                return [self.call("ps2_fsqrt", "float", ["float " + a.group(1)], dst, ind, tail)]
        if name == "llvm.fabs.f32":
            a = re.search(r"float (?:[\w ]*? )?(%[\w.$-]+|[^\s,]+)\s*$", args.strip())
            if a:
                return [self.call("ps2_fabs", "float", ["float " + a.group(1)], dst, ind, tail)]
        if FLOAT_INTRINSIC.match(name):
            self.err(ln, "float intrinsic " + name)
        return [ln]

    # ----------------------------------------------------------------- main
    def run(self):
        out = []
        skip = False
        for ln in self.lines:
            if ln.startswith("define"):
                m = RE_DEFINE.match(ln)
                self.func = canon(m.group(1), m.group(2)) if m else "?"
                self.zexts = set()
                if ln.startswith("define internal") or ln.startswith("define private"):
                    self.internal.add(self.func)
                skip = self.func.startswith(("ps2_", "ps2fp_", "ps2dp_", "_mm_"))
                out.append(ln)
                continue
            if ln.startswith("}"):
                for (rel, fn, op), (n, ks) in self.order.items():
                    if rel == self.rel and fn == self.func and self.opk[(fn, op)] != n:
                        self.errors.append("%s: %s: mulorder.txt is stale: %d %s, it says %d" % (self.name, fn, self.opk[(fn, op)], op, n))
                self.func = None
                skip = False
                out.append(ln)
                continue
            if ln.startswith(("@llvm.compiler.used", "@llvm.used")):
                m = RE_USED.match(ln)
                if m:
                    ents = [e.strip() for e in split_params(m.group(1))]
                    keep = [e for e in ents if not re.match(r'ptr @(?:"(?:\\01)?)?ps2_', e)]
                    if keep:
                        out.append('%s = appending global [%d x ptr] [%s], section "llvm.metadata"' % (ln.split(" ")[0], len(keep), ", ".join(keep)))
                    continue
            if skip or self.func is None or not ln.startswith("  "):
                out.append(ln)
                continue
            zm = RE_ZEXT.match(ln)
            if zm:
                self.zexts.add(zm.group(1))
            if " asm " in ln:
                out.append(ln)
                continue
            if " call " in ln:
                cm = RE_ANYCALL.search(ln)
                if cm:
                    cn = canon(cm.group(1), cm.group(2))
                    if not cn.startswith(("llvm.", "ps2_")):
                        self.callees.setdefault(self.func, Counter())[cn] += 1
            if RE_VECTOR_FP.search(ln):
                self.err(ln, "vector float operation")
                out.append(ln)
                continue
            r = None
            m = RE_ARITH.match(ln)
            if m:
                r = self.arith(m, ln)
            if r is None:
                m = RE_FNEG.match(ln)
                if m:
                    r = self.fneg(m, ln)
            if r is None:
                m = RE_FCMP.match(ln)
                if m:
                    r = self.fcmp(m, ln)
            if r is None:
                m = RE_CAST.match(ln)
                if m:
                    r = self.cast(m, ln)
            if r is None:
                m = RE_INT2.match(ln)
                if m:
                    r = self.int2(m, ln)
            if r is None and " call " in ln:
                m = RE_CALL.match(ln)
                if m:
                    r = self.callrw(m, ln)
            out.extend(r if r is not None else [ln])
        return "\n".join(out)


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    if len(args) != 2:
        sys.stderr.write(__doc__)
        return 2
    with open(args[0], encoding="utf-8", errors="surrogateescape") as f:
        text = f.read()
    rw = Rewriter(text, args[0])
    res = rw.run()
    if rw.errors:
        for e in rw.errors[:50]:
            sys.stderr.write(e + "\n")
        if len(rw.errors) > 50:
            sys.stderr.write("... and %d more\n" % (len(rw.errors) - 50))
        return 1
    with open(args[1], "w", encoding="utf-8", errors="surrogateescape", newline="\n") as f:
        f.write(res)
    if "--stats" in argv:
        sys.stdout.write(" ".join("%s=%d" % kv for kv in sorted(rw.stats.items())) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
