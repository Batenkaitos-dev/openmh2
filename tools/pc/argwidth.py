#!/usr/bin/env python3
"""argwidth: the argument widths' verification build.

The game's units declare some functions with other integer widths than their
definitions (a function declared in the unit itself, not in a header).
MWCC and clang each pass and receive such values by their
own rules: a caller that declares s32 passes the whole register, and a callee
defined u16 may use it as it is or narrow it. This pass, in ps2fpcc.py's pipeline
for `build.py --variant argwidth`, adds a check at every call that crosses such a
declaration, and at every return of a function some caller declares narrower:

  - an argument: the value the caller passes (its register: the caller's type,
    extended to 32 bits as the caller's declaration extends it) against the
    definition's parameter type: it fits when narrowing it to the parameter's
    width and extending it again gives the same register;
  - a result: the value the definition returns (extended as the definition
    extends it) against each narrower type a caller declares;
  - a result through a pointer: the call stores the pointer's result type
    (when narrower than 32 bits) and its target in two globals just before the
    call; every definition that returns an integer takes them at its entry
    when the target is itself (and clears the target), and checks each value
    it returns against that type;
  - a result at a call through a pointer, from a definition narrower than the pointer's result type: the
    value the caller reads (extended as the pointer's type extends it) against its low bits extended as the
    definition extends them (direct calls have the declaration forms instead);
  - a call that passes fewer arguments than the definition reads, a 64-bit
    argument where the definition takes a 32-bit one, or a 32-bit argument where
    it takes a 64-bit one (on i686 the following arguments move): every call is
    logged, whatever its values.

A value that doesn't fit is logged through Host_Log ("mh2-core: argwidth: SITE: 0xVALUE
at VBlank 0xV, time 0xN, ra 0xADDR", in hex: the IR calls no C library function by
name, since game data has names like snprintf), the first, second, fourth, eighth ...
time at each site, with the VBlank and, for a result, the caller's return address
(llvm-nm on mh2-core names it); and each site once, when a run first reaches it
("argwidth reached: SITE"). the verification build collects them.

    argwidth.py --table OUT [REPO]    writes the table: proto_check's `unit` and
                                      `xunit` declarations of the PC build
    argwidth.py --sites TABLE FILE.ll  the sites one unit's IR would get (a test)
    argwidth.py --self-test CLANG      two units that disagree, compiled through ps2fpcc.py with
                                      a table of their own, linked and run: the expected log
                                      (build.py --variant argwidth --check runs it)

ps2fpcc.py calls rewrite() when the command has -DMH2_ARGWIDTH_TABLE=TABLE
(pc/core/CMakeLists.txt sets it for the argwidth variant).
"""
import hashlib
import os
import re
import subprocess
import sys

TABLE_FLAG = "-DMH2_ARGWIDTH_TABLE="

# the C types of proto_check's lists: (bits, signed)
CTYPES = {"s8": (8, 1), "u8": (8, 0), "s16": (16, 1), "u16": (16, 0), "s32": (32, 1), "u32": (32, 0),
          "s64": (64, 1), "u64": (64, 0)}

LINE = re.compile(r"^(\S+) (\w+): (\S+):(\d+) (.*?) \((.*)\) \| def (\S+):(\d+) (.*?) \((.*)\)\s*$")

# The sites known to be harmless: the
# site as the log names it -> the values it logged when it was ruled (64-bit registers), the evidence and the
# ruling. A listed site is reported only when a run logs a value outside its list, so a report
# lists only new sites and changed values. Add an entry only for a site whose callee is shown not to use
# the value.
RULED = {
    "src/sub_main/hud.c: FieldMenu_Update calls through a pointer (call 2) -> PouchMenu_Open: "
    "fewer arguments, 01 read by the callee": {
        "values": {0x0},
        "evidence": "gFieldMenuDecide[...]() is an s32 (*)(void); PouchMenu_Open(void *menu) is defined with one "
                    "parameter, but its EE code never reads a0 (the first use is `addiu a0, zero, 1`), so the "
                    "PC can't differ. Seen on area1 from VBlank 16997.",
        "ruling": "keep a list of sites known to be harmless "
                  "(FieldMenu_Update calling PouchMenu_Open with no argument, which the EE's callee never reads)",
    },
    # The other two entries of the same table call.
    "src/sub_main/hud.c: FieldMenu_Update calls through a pointer (call 2) -> StatusMenu_Open: "
    "fewer arguments, 01 read by the callee": {
        "values": {0x0},
        "evidence": "the same table call as PouchMenu_Open's; StatusMenu_Open(void *menu) never uses a0 (asm/sub_main/"
                    "hud.s: no instruction reads $a0 between its glabel and endlabel). Logged on dojo-bowgun at "
                    "VBlank 8851.",
        "ruling": "StatusMenu_Open joins the RULED list",
    },
    "src/sub_main/hud.c: FieldMenu_Update calls through a pointer (call 2) -> QuestMenu_Open: "
    "fewer arguments, 01 read by the callee": {
        "values": {0x0},
        "evidence": "the same table call; QuestMenu_Open(void *menu) only sets $a0 (`addiu $a0, $zero, 5`) after "
                    "calling QuestMenu_CheckInfo(void), which does not read it either, so no use of the caller's "
                    "$a0. Seen on dojo-bowgun at VBlank 9601.",
        "ruling": "QuestMenu_Open joins the RULED list",
    },
    "src/sub_main/hud.c: FieldMenu_Update calls through a pointer (call 2) -> InfoMenu_Open: "
    "fewer arguments, 01 read by the callee": {
        "values": {0x0},
        "evidence": "the same table call; InfoMenu_Open(void *menu) never reads a0 (asm/sub_main/hud.s: no "
                    "instruction reads $a0 between its glabel and endlabel), and its call InfoMenu_Reset(void) sets "
                    "$a0 before each use. Seen on field-menu at VBlank 16966.",
        "ruling": "InfoMenu_Open joins the RULED list",
    },
}


def split_top(s, sep=","):
    out, depth, cur = [], 0, ""
    for c in s:
        if c in "([{<":
            depth += 1
        elif c in ")]}>":
            depth -= 1
        if c == sep and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += c
    if cur.strip():
        out.append(cur.strip())
    return out


def ctype(t):
    t = t.strip()
    return CTYPES.get(t)


def read_table(path):
    """callee -> {"def": (file, ret, [params]), "decls": [(file:line, ret, [params])]}"""
    tab = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            m = LINE.match(line.rstrip("\n"))
            if not m:
                continue
            kinds, name, dfile, dline, dret, dpar, ffile, fline, fret, fpar = m.groups()
            e = tab.setdefault(name, {"def": (f"{ffile}:{fline}", fret.strip(), split_top(fpar)), "decls": []})
            e["decls"].append((f"{dfile}:{dline}", dret.strip(), split_top(dpar)))
    return tab


def make_table(out, repo):
    pc = os.path.join(repo, "tools", "lead", "proto_check.py")
    r = subprocess.run([sys.executable, pc, repo, "--build", "pc", "--list", "unit,xunit"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"argwidth: proto_check failed: {r.stderr[-400:]}")
    lines = [l for l in r.stdout.splitlines() if LINE.match(l)]
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    tmp = out + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    # unchanged content keeps its time stamp (build.py hashes it into the launchers' stamp)
    if os.path.exists(out) and open(out, encoding="utf-8").read() == open(tmp, encoding="utf-8").read():
        os.remove(tmp)
    else:
        os.replace(tmp, out)
    return len(lines)


# ---- the IR ----

RE_DEFINE = re.compile(r"^define [^@]*@(?:__regcall3__)?([\w.$]+)\(")
RE_CALL = re.compile(r"^(\s*)(%[\w.]+ = )?((?:tail |musttail |notail )?call )(.*?)@(?:__regcall3__)?([\w.$]+)\(")
RE_RET = re.compile(r"^(\s*)ret (i\d+) (.+?)\s*$")
INT_ARG = re.compile(r"^i(\d+)((?: (?:inreg|noundef|signext|zeroext|range\(i\d+ -?\d+, -?\d+\)))*) (.+)$")

HELPER = r"""
@mh2.argw.s0 = linkonce_odr hidden constant [11 x i8] c"argwidth: \00"
@mh2.argw.s1 = linkonce_odr hidden constant [5 x i8] c": 0x\00"
@mh2.argw.s2 = linkonce_odr hidden constant [14 x i8] c" at VBlank 0x\00"
@mh2.argw.s3 = linkonce_odr hidden constant [10 x i8] c", time 0x\00"
@mh2.argw.s4 = linkonce_odr hidden constant [8 x i8] c", ra 0x\00"
@mh2.argw.s5 = linkonce_odr hidden constant [19 x i8] c"argwidth reached: \00"

; the host's C library is not called by name: the game's data has names like snprintf
define linkonce_odr hidden ptr @mh2.argw.str(ptr %d, ptr %s) noinline nounwind {
entry:
  br label %loop
loop:
  %di = phi ptr [ %d, %entry ], [ %dn, %body ]
  %si = phi ptr [ %s, %entry ], [ %sn, %body ]
  %c = load i8, ptr %si, align 1
  %z = icmp eq i8 %c, 0
  br i1 %z, label %end, label %body
body:
  store i8 %c, ptr %di, align 1
  %dn = getelementptr i8, ptr %di, i32 1
  %sn = getelementptr i8, ptr %si, i32 1
  br label %loop
end:
  store i8 0, ptr %di, align 1
  ret ptr %di
}

define linkonce_odr hidden ptr @mh2.argw.hex(ptr %d, i64 %v, i32 %n) noinline nounwind {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %i1, %loop ]
  %k0 = sub i32 %n, 1
  %k1 = sub i32 %k0, %i
  %k4 = mul i32 %k1, 4
  %sh = zext i32 %k4 to i64
  %w = lshr i64 %v, %sh
  %w8 = trunc i64 %w to i8
  %nb = and i8 %w8, 15
  %lt = icmp ult i8 %nb, 10
  %a = add i8 %nb, 48
  %b = add i8 %nb, 87
  %ch = select i1 %lt, i8 %a, i8 %b
  %p = getelementptr i8, ptr %d, i32 %i
  store i8 %ch, ptr %p, align 1
  %i1 = add i32 %i, 1
  %fin = icmp eq i32 %i1, %n
  br i1 %fin, label %end, label %loop
end:
  %e = getelementptr i8, ptr %d, i32 %n
  store i8 0, ptr %e, align 1
  ret ptr %e
}

define linkonce_odr hidden void @mh2.argw(ptr %site, ptr %cnt, i64 %reg, i32 %code, ptr %ra) noinline nounwind {
entry:
  %buf = alloca [600 x i8], align 4
  %c0 = load i32, ptr %cnt, align 4
  %seen = and i32 %c0, -2147483648
  %new = icmp eq i32 %seen, 0
  br i1 %new, label %first, label %test
first:
  %c0s = or i32 %c0, -2147483648
  store i32 %c0s, ptr %cnt, align 4
  %q0 = call ptr @mh2.argw.str(ptr %buf, ptr @mh2.argw.s5)
  %q1 = call ptr @mh2.argw.str(ptr %q0, ptr %site)
  call void @Host_Log(ptr %buf)
  br label %test
test:
  %bits32 = and i32 %code, 255
  %sg8 = lshr i32 %code, 8
  %sg = and i32 %sg8, 1
  %pb = lshr i32 %code, 16
  %always = icmp eq i32 %bits32, 0
  br i1 %always, label %bad, label %chk
chk:
  %bits = zext i32 %bits32 to i64
  %sh = sub i64 64, %bits
  %l = shl i64 %reg, %sh
  %rs = ashr i64 %l, %sh
  %ru = lshr i64 %l, %sh
  %issg = icmp ne i32 %sg, 0
  %ext = select i1 %issg, i64 %rs, i64 %ru
  %is64 = icmp eq i32 %pb, 64
  %e32 = trunc i64 %ext to i32
  %r32 = trunc i64 %reg to i32
  %eq64 = icmp eq i64 %ext, %reg
  %eq32 = icmp eq i32 %e32, %r32
  %fits = select i1 %is64, i1 %eq64, i1 %eq32
  br i1 %fits, label %done, label %bad
bad:
  %cf = load i32, ptr %cnt, align 4
  %cs = add i32 %cf, 1
  store i32 %cs, ptr %cnt, align 4
  %c = and i32 %cf, 2147483647
  %c1 = add i32 %c, 1
  %m = and i32 %c1, %c
  %p2 = icmp eq i32 %m, 0
  br i1 %p2, label %print, label %done
print:
  %vb = call x86_regcallcc i64 @__regcall3__Kern_VblankCount()
  %c64 = zext i32 %c1 to i64
  %ra64 = ptrtoint ptr %ra to i64
  %p0 = call ptr @mh2.argw.str(ptr %buf, ptr @mh2.argw.s0)
  %p1 = call ptr @mh2.argw.str(ptr %p0, ptr %site)
  %p2b = call ptr @mh2.argw.str(ptr %p1, ptr @mh2.argw.s1)
  %p3 = call ptr @mh2.argw.hex(ptr %p2b, i64 %reg, i32 16)
  %p4 = call ptr @mh2.argw.str(ptr %p3, ptr @mh2.argw.s2)
  %p5 = call ptr @mh2.argw.hex(ptr %p4, i64 %vb, i32 8)
  %p6 = call ptr @mh2.argw.str(ptr %p5, ptr @mh2.argw.s3)
  %p7 = call ptr @mh2.argw.hex(ptr %p6, i64 %c64, i32 8)
  %p8 = call ptr @mh2.argw.str(ptr %p7, ptr @mh2.argw.s4)
  %p9 = call ptr @mh2.argw.hex(ptr %p8, i64 %ra64, i32 8)
  call void @Host_Log(ptr %buf)
  br label %done
done:
  ret void
}
"""

# weak: every unit with an integer-returning definition carries the helper (its result through a pointer), test
# programs too, which link neither; they only log when a check fails
HELPER_DECLS = [
    ("@__regcall3__Kern_VblankCount(", "declare extern_weak x86_regcallcc i64 @__regcall3__Kern_VblankCount()"),
    ("@Host_Log(", "declare extern_weak void @Host_Log(ptr)"),
    ("@llvm.returnaddress(", "declare ptr @llvm.returnaddress(i32 immarg)"),
]


# ---- results through a pointer ----
# A call through a pointer whose result is an integer narrower than 32 bits stores its code (bits | signext << 8)
# in @mh2.argw.rc and its target in @mh2.argw.rt, just before the call. Every definition returning an integer of
# at most 32 bits loads both at its entry, clears @mh2.argw.rt, and keeps the code when the target is itself (else
# 0: a direct call, or a pointer of its own width). Each `ret` then checks the value (extended as the definition
# extends it) against the code, as a direct call's result is checked: the site is the definition's, the return
# address names the call. Nothing runs between the stores and the target's entry, and a target that never
# consumes them (a library or host function) can't match another definition's address.
RP_GLOBALS = [
    "@mh2.argw.rt = linkonce_odr hidden global ptr null, align 4",
    "@mh2.argw.rc = linkonce_odr hidden global i32 0, align 4",
]
RE_LABEL = re.compile(r"^[\w.$-]+:")

# ---- calls through a pointer ----
# Every unit lists its definitions in the section mh2_argw_def, one record each (24 bytes):
#   { ptr function, ptr name, ptr codes, i32 parameters, i32 log count, i32 result code }
# with one code per parameter (param_code), and the result's (0 for void). An indirect call stores its arguments (as i64 registers) and their
# codes in the site's globals and calls @mh2.argw.ind, which finds the target's record (a hash cache in front of
# a scan of the section; a target without one, a library or host function, is skipped) and compares:
#   - fewer arguments than the definition reads: logged at every call;
#   - an argument of another class (integer or pointer, float, other) or of 64 bits against 32: logged at
#     every call (on i686 it and the arguments after it move, or the callee reads another register);
#   - an integer narrower than 32 bits in the definition, passed as another width or extension: the value must
#     fit, as for a direct call.
# The log count is the callee's: "argwidth reached: SITE -> CALLEE ..." once per callee, then its findings the
# 1st, 2nd, 4th ... time.
# @mh2.argw.ind returns the record (or null); after the call, @mh2.argw.ires checks an integer result the
# caller reads as N bits (N up to 32) when the definition's is narrower: the caller's value, extended to 32
# bits as the pointer's type extends it, against the definition's low bits extended as the definition extends
# them; "SITE -> CALLEE: result: 0xVALUE ..." (ra: the caller), on the callee's count.
IND_HELPER = r"""
@__start_mh2_argw_def = extern_weak hidden global i8
@__stop_mh2_argw_def = extern_weak hidden global i8
@mh2.argw.hc = linkonce_odr hidden global [8192 x ptr] zeroinitializer, align 4
@mh2.argw.s6 = linkonce_odr hidden constant [5 x i8] c" -> \00"
@mh2.argw.s7 = linkonce_odr hidden constant [20 x i8] c": fewer arguments, \00"
@mh2.argw.s8 = linkonce_odr hidden constant [14 x i8] c": argument 0x\00"
@mh2.argw.s9 = linkonce_odr hidden constant [20 x i8] c" read by the callee\00"
@mh2.argw.s10 = linkonce_odr hidden constant [9 x i8] c": result\00"

define linkonce_odr hidden void @mh2.argw.ires(ptr %site, ptr %rec, i64 %v, i32 %n) noinline nounwind {
entry:
  %buf = alloca [700 x i8], align 4
  %none = icmp eq ptr %rec, null
  br i1 %none, label %done, label %have
have:
  %prc = getelementptr inbounds i8, ptr %rec, i32 20
  %rc = load i32, ptr %prc, align 4
  %cls = lshr i32 %rc, 12
  %db = and i32 %rc, 255
  %isint = icmp eq i32 %cls, 0
  %nz = icmp ne i32 %db, 0
  %lt = icmp ult i32 %db, %n
  %a0 = and i1 %isint, %nz
  %go = and i1 %a0, %lt
  br i1 %go, label %chk, label %done
chk:
  %v32 = trunc i64 %v to i32
  %sh = sub i32 32, %db
  %l = shl i32 %v32, %sh
  %es = ashr i32 %l, %sh
  %eu = lshr i32 %l, %sh
  %sg0 = lshr i32 %rc, 8
  %sg = and i32 %sg0, 1
  %issg = icmp ne i32 %sg, 0
  %e = select i1 %issg, i32 %es, i32 %eu
  %fits = icmp eq i32 %e, %v32
  br i1 %fits, label %done, label %bad
bad:
  %ra = call ptr @llvm.returnaddress(i32 0)
  %pname = getelementptr inbounds i8, ptr %rec, i32 4
  %name = load ptr, ptr %pname, align 4
  %pcnt = getelementptr inbounds i8, ptr %rec, i32 16
  %q0 = call ptr @mh2.argw.str(ptr %buf, ptr %site)
  %q1 = call ptr @mh2.argw.str(ptr %q0, ptr @mh2.argw.s6)
  %q2 = call ptr @mh2.argw.str(ptr %q1, ptr %name)
  %q3 = call ptr @mh2.argw.str(ptr %q2, ptr @mh2.argw.s10)
  call void @mh2.argw(ptr %buf, ptr %pcnt, i64 %v, i32 0, ptr %ra)
  br label %done
done:
  ret void
}

define linkonce_odr hidden void @mh2.argw.ilog(ptr %site, ptr %rec, i32 %i, i64 %v, i32 %code) noinline nounwind {
entry:
  %buf = alloca [700 x i8], align 4
  %pname = getelementptr inbounds i8, ptr %rec, i32 4
  %name = load ptr, ptr %pname, align 4
  %pcnt = getelementptr inbounds i8, ptr %rec, i32 16
  %q0 = call ptr @mh2.argw.str(ptr %buf, ptr %site)
  %q1 = call ptr @mh2.argw.str(ptr %q0, ptr @mh2.argw.s6)
  %q2 = call ptr @mh2.argw.str(ptr %q1, ptr %name)
  %few = icmp slt i32 %i, 0
  br i1 %few, label %f, label %a
f:
  %pdn = getelementptr inbounds i8, ptr %rec, i32 12
  %dn = load i32, ptr %pdn, align 4
  %dn64 = zext i32 %dn to i64
  %q3 = call ptr @mh2.argw.str(ptr %q2, ptr @mh2.argw.s7)
  %q4 = call ptr @mh2.argw.hex(ptr %q3, i64 %dn64, i32 2)
  %q5 = call ptr @mh2.argw.str(ptr %q4, ptr @mh2.argw.s9)
  br label %log
a:
  %i1 = add i32 %i, 1
  %i64 = zext i32 %i1 to i64
  %q6 = call ptr @mh2.argw.str(ptr %q2, ptr @mh2.argw.s8)
  %q7 = call ptr @mh2.argw.hex(ptr %q6, i64 %i64, i32 2)
  br label %log
log:
  call void @mh2.argw(ptr %buf, ptr %pcnt, i64 %v, i32 %code, ptr null)
  ret void
}

define linkonce_odr hidden ptr @mh2.argw.ind(ptr %site, ptr %fp, i32 %n, ptr %cc, ptr %vals) noinline nounwind {
entry:
  %fpi = ptrtoint ptr %fp to i32
  %h0 = lshr i32 %fpi, 2
  %h1 = and i32 %h0, 4095
  %h2 = shl i32 %h1, 1
  %pc0 = getelementptr inbounds [8192 x ptr], ptr @mh2.argw.hc, i32 0, i32 %h2
  %pc1 = getelementptr inbounds ptr, ptr %pc0, i32 1
  %cf = load ptr, ptr %pc0, align 4
  %hit = icmp eq ptr %cf, %fp
  br i1 %hit, label %cached, label %scan
cached:
  %rc = load ptr, ptr %pc1, align 4
  br label %have
scan:
  %ei = ptrtoint ptr @__stop_mh2_argw_def to i32
  br label %sloop
sloop:
  %r = phi ptr [ @__start_mh2_argw_def, %scan ], [ %rn, %snext ]
  %ri = ptrtoint ptr %r to i32
  %end = icmp uge i32 %ri, %ei
  br i1 %end, label %nf, label %sbody
sbody:
  %rf = load ptr, ptr %r, align 4
  %eq = icmp eq ptr %rf, %fp
  br i1 %eq, label %store, label %snext
snext:
  %rn = getelementptr inbounds i8, ptr %r, i32 24
  br label %sloop
nf:
  br label %store
store:
  %rs = phi ptr [ %r, %sbody ], [ null, %nf ]
  store ptr %fp, ptr %pc0, align 4
  store ptr %rs, ptr %pc1, align 4
  br label %have
have:
  %rec = phi ptr [ %rc, %cached ], [ %rs, %store ]
  %none = icmp eq ptr %rec, null
  br i1 %none, label %done, label %sig
sig:
  %pcodes = getelementptr inbounds i8, ptr %rec, i32 8
  %dcodes = load ptr, ptr %pcodes, align 4
  %pdn = getelementptr inbounds i8, ptr %rec, i32 12
  %dn = load i32, ptr %pdn, align 4
  %few = icmp slt i32 %n, %dn
  br i1 %few, label %logfew, label %pre
logfew:
  call void @mh2.argw.ilog(ptr %site, ptr %rec, i32 -1, i64 0, i32 0)
  br label %pre
pre:
  %m = select i1 %few, i32 %n, i32 %dn
  br label %loop
loop:
  %i = phi i32 [ 0, %pre ], [ %i1, %next ]
  %more = icmp slt i32 %i, %m
  br i1 %more, label %body, label %done
body:
  %pcc = getelementptr inbounds i32, ptr %cc, i32 %i
  %c = load i32, ptr %pcc, align 4
  %pd = getelementptr inbounds i32, ptr %dcodes, i32 %i
  %d = load i32, ptr %pd, align 4
  %pv = getelementptr inbounds i64, ptr %vals, i32 %i
  %v = load i64, ptr %pv, align 4
  %same = icmp eq i32 %c, %d
  br i1 %same, label %next, label %diff
diff:
  %ccl = lshr i32 %c, 12
  %dcl = lshr i32 %d, 12
  %clsne = icmp ne i32 %ccl, %dcl
  %cb = and i32 %c, 255
  %db = and i32 %d, 255
  %c64 = icmp eq i32 %cb, 64
  %d64 = icmp eq i32 %db, 64
  %w64 = xor i1 %c64, %d64
  %notint = icmp ne i32 %dcl, 0
  %a1 = or i1 %clsne, %w64
  %always = or i1 %a1, %notint
  br i1 %always, label %logal, label %narrow
logal:
  call void @mh2.argw.ilog(ptr %site, ptr %rec, i32 %i, i64 %v, i32 0)
  br label %next
narrow:
  %dlt = icmp ult i32 %db, 32
  br i1 %dlt, label %logchk, label %next
logchk:
  %dlow = and i32 %d, 511
  %code = or i32 %dlow, 2097152
  call void @mh2.argw.ilog(ptr %site, ptr %rec, i32 %i, i64 %v, i32 %code)
  br label %next
next:
  %i1 = add i32 %i, 1
  br label %loop
done:
  ret ptr %rec
}
"""

RE_ICALL = re.compile(r"^(\s*)(%[\w.]+ = )?((?:tail |musttail |notail )?call )([^@\"]*?)(%[\w.]+)\(")


def param_code(p):
    """A parameter's or argument's code: bits | signext << 8 for an integer (a pointer is a u32), 0x1000 | bits
    for float and double, 0x2000 for anything else."""
    t = p.split(None, 1)[0] if p.strip() else ""
    if re.fullmatch(r"i\d+", t):
        bits = int(t[1:])
        return bits | ((1 << 8) if bits < 32 and " signext" in p else 0)
    if t == "ptr":
        return 32
    if t in ("float", "double"):
        return 0x1000 | (32 if t == "float" else 64)
    return 0x2000


def ir_string(s):
    out = ""
    for ch in s.encode("utf-8") + b"\0":
        out += chr(ch) if 32 <= ch < 127 and ch not in (34, 92) else f"\\{ch:02X}"
    return f"[{len(s.encode('utf-8')) + 1} x i8] c\"{out}\""


def code_of(bits, signed, pbits):
    return bits | (signed << 8) | (pbits << 16)


def arg_fields(a):
    """An integer argument's (bits, ext, value), or None."""
    m = INT_ARG.match(a)
    if not m:
        return None
    attrs = m.group(2)
    ext = "s" if "signext" in attrs else ("z" if "zeroext" in attrs else "")
    return int(m.group(1)), ext, m.group(3)


def close_paren(s, i):
    """Index of the parenthesis that closes the one at s[i]."""
    depth = 0
    for j in range(i, len(s)):
        if s[j] in "([{":
            depth += 1
        elif s[j] in ")]}":
            depth -= 1
            if depth == 0:
                return j
    return -1


def rewrite(ir, table_path, unit):
    """The IR with the checks; (text, number of sites)."""
    tab = read_table(table_path)
    lines = ir.split("\n")
    defined = {m.group(1) for l in lines for m in [RE_DEFINE.match(l)] if m}
    out, sites, globs = [], 0, []
    func = None
    fret = None
    k = 0
    uh = hashlib.sha1(unit.encode("utf-8")).hexdigest()[:12]
    defs = 0
    icalls = {}
    rp = None          # the current definition's result check: (bits, ext, site) or None
    rp_entry = None    # the entry code still to place (after the define line, or after the entry block's label)
    rp_any = False

    def def_record(line, md):
        # the definition's record in mh2_argw_def (calls through a pointer find it by the function's address)
        nonlocal defs
        sym = re.search(r"@([\w.$]+)\(", line)
        start = sym.end() - 1
        end = close_paren(line, start)
        params = [p for p in split_top(line[start + 1:end]) if p != "..."] if end > 0 else []
        codes = [param_code(p) for p in params]
        j = defs
        defs += 1
        name = md.group(1)
        arr = ", ".join(f"i32 {c}" for c in codes) if codes else "i32 0"
        globs.append(f"@mh2.argw.dn.{uh}.{j} = private constant {ir_string(name)}")
        globs.append(f"@mh2.argw.dc.{uh}.{j} = private constant [{max(len(codes), 1)} x i32] [{arr}]")
        head = line[:line.index("@" + sym.group(1) + "(")]
        mh = re.search(r"\b(signext |zeroext )?i(\d+) $", head)
        if mh:
            rcode = int(mh.group(2)) | (256 if mh.group(1) == "signext " and int(mh.group(2)) < 32 else 0)
        else:
            rcode = 0 if re.search(r"\bvoid $", head) else param_code(head.split()[-1])
        globs.append(f"@mh2.argw.def.{uh}.{j} = hidden global {{ ptr, ptr, ptr, i32, i32, i32 }} {{ ptr @{sym.group(1)}, "
                     f"ptr @mh2.argw.dn.{uh}.{j}, ptr @mh2.argw.dc.{uh}.{j}, i32 {len(codes)}, i32 0, i32 {rcode} }}, "
                     f"section \"mh2_argw_def\", align 4")

    def indirect(mi, line):
        # a call through a pointer: its arguments' registers and codes, then @mh2.argw.ind
        nonlocal sites, k, rp_any
        start = mi.end() - 1
        end = close_paren(line, start)
        args = split_top(line[start + 1:end]) if end > 0 else []
        icalls[func] = icalls.get(func, 0) + 1
        n = sites
        sites += 1
        globs.append(f"@mh2.argw.site.{n} = private constant "
                     f"{ir_string(f'{unit}: {func} calls through a pointer (call {icalls[func]})')}")
        kk = max(len(args), 1)
        codes = [param_code(a) for a in args]
        arr = ", ".join(f"i32 {c}" for c in codes) if codes else "i32 0"
        globs.append(f"@mh2.argw.icc.{n} = private constant [{kk} x i32] [{arr}]")
        globs.append(f"@mh2.argw.iv.{n} = internal global [{kk} x i64] zeroinitializer, align 8")
        ind = mi.group(1)
        for i, a in enumerate(args):
            f = arg_fields(a)
            if not f:
                continue
            bits, ext, value = f
            k += 1
            if bits == 64:
                reg = value
            else:
                reg = f"%mh2.aw.{k}"
                out.append(f"{ind}{reg} = {'sext' if ext == 's' else 'zext'} i{bits} {value} to i64")
            out.append(f"{ind}store i64 {reg}, ptr getelementptr inbounds ([{kk} x i64], ptr @mh2.argw.iv.{n}, "
                       f"i32 0, i32 {i}), align 8")
        out.append(f"{ind}%mh2.ir.{n} = call ptr @mh2.argw.ind(ptr @mh2.argw.site.{n}, ptr {mi.group(5)}, "
                   f"i32 {len(args)}, ptr @mh2.argw.icc.{n}, ptr @mh2.argw.iv.{n})")
        rt = re.search(r"(signext |zeroext )?\bi(\d+)\s*$", mi.group(4))
        post = []
        if mi.group(2) and rt and int(rt.group(2)) <= 32 and "musttail" not in mi.group(3):
            rb = int(rt.group(2))
            res = mi.group(2).split("=")[0].strip()
            if rb < 32:
                post.append(f"{ind}%mh2.ir.v.{n} = {'sext' if rt.group(1) == 'signext ' else 'zext'} i{rb} {res} to i32")
                res = f"%mh2.ir.v.{n}"
            post.append(f"{ind}%mh2.ir.w.{n} = zext i32 {res} to i64")
            post.append(f"{ind}call void @mh2.argw.ires(ptr @mh2.argw.site.{n}, ptr %mh2.ir.{n}, i64 %mh2.ir.w.{n}, "
                        f"i32 {rb})")
        if mi.group(2) and rt and int(rt.group(2)) < 32:
            rp_any = True
            out.append(f"{ind}store i32 {int(rt.group(2)) | (256 if rt.group(1) == 'signext ' else 0)}, "
                       f"ptr @mh2.argw.rc, align 4")
            out.append(f"{ind}store ptr {mi.group(5)}, ptr @mh2.argw.rt, align 4")
        return post

    def site(text):
        nonlocal sites
        n = sites
        sites += 1
        globs.append(f"@mh2.argw.site.{n} = private constant {ir_string(text)}")
        globs.append(f"@mh2.argw.cnt.{n} = internal global i32 0, align 4")
        return n

    def check(indent, n, bits, ext, value, code, ra="null"):
        nonlocal k
        k += 1
        if bits == 64:
            reg = value
        else:
            reg = f"%mh2.aw.{k}"
            out.append(f"{indent}{reg} = {'sext' if ext == 's' else 'zext'} i{bits} {value} to i64")
        out.append(f"{indent}call void @mh2.argw(ptr @mh2.argw.site.{n}, ptr @mh2.argw.cnt.{n}, i64 {reg}, "
                   f"i32 {code}, ptr {ra})")

    for line in lines:
        if rp_entry is not None:
            if RE_LABEL.match(line):   # the entry block has a label: the code goes after it
                out.append(line)
                out.extend(rp_entry)
                rp_entry = None
                continue
            out.extend(rp_entry)
            rp_entry = None
        md = RE_DEFINE.match(line)
        if md:
            func = md.group(1)
            fret = None
            rp = None
            sym = re.search(r"@([\w.$]+)\(", line)
            hd = re.search(r"\b(signext |zeroext )?i(\d+) $", line[:line.index("@")])
            if (sym and hd and int(hd.group(2)) <= 32 and not func.startswith("mh2.")
                    and not line.startswith("define available_externally") and line.rstrip().endswith("{")):
                rbits = int(hd.group(2))
                rext = "s" if hd.group(1) == "signext " else "z"
                rp_any = True
                n = sites
                sites += 1
                globs.append(f"@mh2.argw.site.{n} = private constant {ir_string(f'{unit}: {func} gives i{rbits} through a pointer')}")
                globs.append(f"@mh2.argw.rcnt.{n} = internal global i32 0, align 4")
                rp = (rbits, rext, n)
                rp_entry = ["  %mh2.rp.t = load ptr, ptr @mh2.argw.rt, align 4",
                            "  %mh2.rp.c0 = load i32, ptr @mh2.argw.rc, align 4",
                            "  store ptr null, ptr @mh2.argw.rt, align 4",
                            f"  %mh2.rp.m = icmp eq ptr %mh2.rp.t, @{sym.group(1)}",
                            "  %mh2.rp.c = select i1 %mh2.rp.m, i32 %mh2.rp.c0, i32 0"]
            if not func.startswith("mh2.") and not line.startswith("define available_externally"):
                def_record(line, md)
            if func in tab and " internal " not in line[:line.index("@")]:
                # the definition's return: its register as the definition extends it
                head = line[:line.index("@")]
                mr = re.search(r"\b(signext |zeroext )?(i\d+) $", head)
                if mr:
                    fret = (int(mr.group(2)[1:]), "s" if mr.group(1) == "signext " else
                            ("z" if mr.group(1) == "zeroext " else ""))
            out.append(line)
            continue
        if line.startswith("}"):
            func = None
            rp = None
        mrp = RE_RET.match(line) if rp else None
        if mrp and mrp.group(2) == f"i{rp[0]}" and not (out and "musttail call" in out[-1]):
            rbits, rext, n = rp
            k += 1
            ind = mrp.group(1)
            out.append(f"{ind}%mh2.rp.n.{k} = icmp ne i32 %mh2.rp.c, 0")
            out.append(f"{ind}br i1 %mh2.rp.n.{k}, label %mh2.rp.y.{k}, label %mh2.rp.z.{k}")
            out.append(f"mh2.rp.y.{k}:")
            out.append(f"{ind}%mh2.rp.ra.{k} = call ptr @llvm.returnaddress(i32 0)")
            out.append(f"{ind}%mh2.rp.cc.{k} = or i32 %mh2.rp.c, {32 << 16}")
            if rbits == 32:
                reg = mrp.group(3)
                out.append(f"{ind}%mh2.rp.v.{k} = zext i32 {reg} to i64")
            else:
                out.append(f"{ind}%mh2.rp.v.{k} = {'sext' if rext == 's' else 'zext'} i{rbits} {mrp.group(3)} to i64")
            out.append(f"{ind}call void @mh2.argw(ptr @mh2.argw.site.{n}, ptr @mh2.argw.rcnt.{n}, i64 %mh2.rp.v.{k}, "
                       f"i32 %mh2.rp.cc.{k}, ptr %mh2.rp.ra.{k})")
            out.append(f"{ind}br label %mh2.rp.z.{k}")
            out.append(f"mh2.rp.z.{k}:")
        mr = RE_RET.match(line) if func in tab and fret else None
        if mr:
            dfile, dret, _ = tab[func]["def"]
            dt = ctype(dret)
            seen = set()
            for dfl, cret, _ in tab[func]["decls"]:
                ct = ctype(cret)
                if not ct or not dt or ct == dt or ct[0] >= 32 and dt[0] <= 32 or cret in seen:
                    continue
                seen.add(cret)
                n = site(f"{unit}: {func} returns {dret} to a caller declaring {cret} ({dfl})")
                k_ra = f"%mh2.aw.ra.{len(out)}"
                out.append(f"{mr.group(1)}{k_ra} = call ptr @llvm.returnaddress(i32 0)")
                bits, ext = fret
                if not ext:
                    ext = "s" if dt[1] else "z"
                check(mr.group(1), n, bits, ext, mr.group(3), code_of(min(ct[0], 32), ct[1], max(bits, 32)), k_ra)
            out.append(line)
            continue
        mc = RE_CALL.match(line)
        mi = RE_ICALL.match(line) if func and not mc else None
        post = indirect(mi, line) if mi else []
        if mc and func and mc.group(5) in tab and mc.group(5) not in defined:
            callee = mc.group(5)
            start = mc.end() - 1
            end = close_paren(line, start)
            args = split_top(line[start + 1:end]) if end > 0 else []
            dfile, dret, dpar = tab[callee]["def"]
            dpar = [p for p in dpar if p != "void"]
            fixed = [p for p in dpar if p != "..."]
            if len(args) < len(fixed):
                n = site(f"{unit}: {func} calls {callee} with {len(args)} arguments; the definition ({dfile}) "
                         f"reads {len(fixed)}")
                check(mc.group(1), n, 32, "z", "0", 0)
            for i, a in enumerate(args[:len(fixed)]):
                f = arg_fields(a)
                pt = ctype(fixed[i])
                if not f or not pt:
                    continue
                bits, ext, value = f
                if bits == 64 and pt[0] < 64:
                    n = site(f"{unit}: {func} calls {callee}: argument {i + 1} is 64-bit, the definition's "
                             f"{fixed[i]} ({dfile}): the arguments after it move")
                    check(mc.group(1), n, 64, "", value, 0)
                    continue
                if bits < 64 and pt[0] == 64:
                    n = site(f"{unit}: {func} calls {callee}: argument {i + 1} is {bits}-bit, the definition's "
                             f"{fixed[i]} ({dfile}): it and the arguments after it move")
                    check(mc.group(1), n, 32, "z", "0", 0)
                    continue
                if pt[0] >= 32:
                    continue
                if bits == pt[0] and ext == ("s" if pt[1] else "z"):
                    continue
                if not ext:
                    ext = "z"   # an i32 (or i64) argument: the register as it is
                n = site(f"{unit}: {func} calls {callee}: argument {i + 1} as i{bits}{'/' + ext if bits < 32 else ''}, "
                         f"the definition's {fixed[i]} ({dfile})")
                check(mc.group(1), n, bits, ext, value, code_of(pt[0], pt[1], 64 if bits == 64 else 32))
        out.append(line)
        out.extend(post)
    if not sites and not defs:
        return ir, 0
    text = "\n".join(out)
    extra = ([HELPER, IND_HELPER] if sites else []) + (RP_GLOBALS if rp_any else []) + globs
    if not sites:
        return text + "\n" + "\n".join(extra) + "\n", 0
    for probe, decl in HELPER_DECLS:
        if not re.search(r"^declare [^\n]*" + re.escape(probe), text, re.M) and \
                not re.search(r"^define [^\n]*" + re.escape(probe), text, re.M):
            extra.append(decl)
    return text + "\n" + "\n".join(extra) + "\n", sites


SELF_A = """void Foo_Set(int v, int w);
unsigned short Foo_Get(void);
int g_r;
void Caller(int x) { Foo_Set(x, 0x10000 + x); g_r = Foo_Get(); }
typedef void (*FooFn)(int, int);
extern FooFn g_fn;
void CallInd(int x) { g_fn(x, x); }
typedef unsigned char (*GetFn)(void);
extern GetFn g_get;
int g_r2;
void CallGet(void) { g_r2 = g_get(); }
typedef int (*SmallFn)(void);
extern SmallFn g_small;
int g_r3;
void CallSmall(void) { g_r3 = g_small(); }
"""
SELF_B = """short g_v;
void Foo_Set(unsigned short v, short w) { g_v = v + w; }
int Foo_Get(void) { return g_v - 70000; }
void Foo_Ind(unsigned char v, long long w) { g_v = v + (short)w; }
void (*g_fn)(unsigned char, long long) = Foo_Ind;
int Foo_Big(void) { return 0x1234; }
int (*g_get)(void) = Foo_Big;
int g_w = 0x12340;
unsigned char Foo_Small(void) { return g_w >> 4; }
unsigned char (*g_small)(void) = Foo_Small;
"""
SELF_RT = """#include <stdio.h>
__attribute__((cdecl)) void Host_Log(const char *m) { printf("%s\\n", m); }
__attribute__((regcall)) unsigned long long Kern_VblankCount(void) { return 42; }
__attribute__((regcall)) void Caller(int x);
__attribute__((regcall)) void CallInd(int x);
__attribute__((regcall)) void CallGet(void);
__attribute__((regcall)) void CallSmall(void);
int main(void) { Caller(5); Caller(0x12345); Caller(0x12345); Caller(7); CallInd(5); CallInd(0x12345); CallGet(); CallGet();
                 CallSmall(); CallSmall(); return 0; }
"""
SELF_TABLE = """unit,width Foo_Set: a.c:1 void (s32, s32) | def b.c:2 void (u16, s16)
unit,retw Foo_Get: a.c:2 u16 (void) | def b.c:3 s32 (void)
"""
# (site, value, time) of every logged line: Foo_Set's v doesn't fit u16 twice, w (0x10000 + x) never fits
# s16, and Foo_Get's result never fits u16; each is logged the 1st, 2nd and 4th time
SELF_EXPECT = [("argument 2", "0000000000010005", 1), ("returns", "fffffffffffeee9a", 1),
               ("argument 1", "0000000000012345", 1), ("argument 2", "0000000000022345", 2),
               ("returns", "ffffffffffff351a", 2), ("argument 1", "0000000000012345", 2),
               ("argument 2", "0000000000010007", 4), ("returns", "fffffffffffeee9e", 4)]
# through g_fn, an (int, int) pointer to Foo_Ind (u8, s64): argument 2 (32 bits for 64) at every call, argument 1
# (0x12345 doesn't fit u8) the second time; the count is Foo_Ind's, so the third finding isn't printed
SELF_EXPECT_IND = [("0x02", "0000000000000005", 1), ("0x01", "0000000000012345", 2)]
# through g_get, an unsigned char (*)(void) to Foo_Big (int): 0x1234 doesn't fit u8, logged the 1st and 2nd time
SELF_EXPECT_RP = [("0000000000001234", 1), ("0000000000001234", 2)]
# through g_small, an int (*)(void) to Foo_Small (u8), which leaves 0x1234 in EAX: the caller reads 0x1234, not
# 0x34, logged the 1st and 2nd time (ra: CallSmall)
SELF_EXPECT_IR = [("0000000000001234", 1), ("0000000000001234", 2)]


def self_test(clang):
    import tempfile
    here = os.path.dirname(os.path.abspath(__file__))
    with tempfile.TemporaryDirectory(prefix="argwidth-") as d:
        for n, t in (("a.c", SELF_A), ("b.c", SELF_B), ("rt.c", SELF_RT), ("table.txt", SELF_TABLE)):
            with open(os.path.join(d, n), "w", newline="\n") as f:
                f.write(t)
        flags = ["-m32", "-fno-pie", "-O2", "-msse2", "-Xclang", "-fdefault-calling-conv=regcall",
                 TABLE_FLAG + os.path.join(d, "table.txt")]
        for n in ("a", "b"):
            r = subprocess.run([sys.executable, os.path.join(here, "ps2fp", "ps2fpcc.py"), clang] + flags +
                               ["-c", os.path.join(d, n + ".c"), "-o", os.path.join(d, n + ".o")])
            if r.returncode:
                return f"compiling {n}.c failed"
        exe = os.path.join(d, "t")
        if subprocess.run([clang, "-m32", "-fno-pie", "-no-pie", os.path.join(d, "rt.c"), os.path.join(d, "a.o"),
                           os.path.join(d, "b.o"), "-o", exe]).returncode:
            return "linking failed"
        out = subprocess.run([exe], capture_output=True, text=True).stdout.splitlines()
    reached = [l for l in out if l.startswith("argwidth reached: ")]
    got, ind, rp, ir = [], [], [], []
    for l in out:
        m = re.match(r"argwidth: .* calls through a pointer \(call 1\) -> Foo_Small: result: 0x([0-9a-f]{16}) at VBlank "
                     r"0x0000002a, time 0x([0-9a-f]{8}), ra 0x([0-9a-f]{8})$", l)
        if m and int(m.group(3), 16):
            ir.append((m.group(1), int(m.group(2), 16)))
            continue
        m = re.match(r"argwidth: \S*b\.c: Foo_Big gives i32 through a pointer: 0x([0-9a-f]{16}) at VBlank 0x0000002a, "
                     r"time 0x([0-9a-f]{8}), ra 0x([0-9a-f]{8})$", l)
        if m and int(m.group(3), 16):
            rp.append((m.group(1), int(m.group(2), 16)))
            continue
        m = re.match(r"argwidth: .* calls through a pointer \(call 1\) -> Foo_Ind: argument (0x\d\d): "
                     r"0x([0-9a-f]{16}) at VBlank 0x0000002a, time 0x([0-9a-f]{8}), ra 0x0{8}$", l)
        if m:
            ind.append((m.group(1), m.group(2), int(m.group(3), 16)))
            continue
        m = re.match(r"argwidth: .*?(argument \d|returns).*: 0x([0-9a-f]{16}) at VBlank 0x0000002a, time 0x([0-9a-f]{8}), "
                     r"ra 0x([0-9a-f]{8})$", l)
        if m:
            got.append((m.group(1), m.group(2), int(m.group(3), 16)))
    if len(reached) != 6:
        return f"{len(reached)} sites reached, not 6: {reached}"
    if got != SELF_EXPECT:
        return f"the log differs: {got}"
    if ind != SELF_EXPECT_IND:
        return f"the indirect calls' log differs: {ind}"
    if rp != SELF_EXPECT_RP:
        return f"the result through a pointer's log differs: {rp}"
    if ir != SELF_EXPECT_IR:
        return f"the indirect results' log differs: {ir}"
    return None


def main(argv):
    if len(argv) >= 2 and argv[0] == "--table":
        repo = argv[2] if len(argv) > 2 else os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
        n = make_table(argv[1], repo)
        print(f"argwidth: {n} declarations in {argv[1]}")
        return 0
    if len(argv) == 3 and argv[0] == "--sites":
        with open(argv[2], encoding="utf-8", errors="surrogateescape") as f:
            text, n = rewrite(f.read(), argv[1], os.path.basename(argv[2]))
        for l in text.split("\n"):
            if l.startswith("@mh2.argw.site."):
                print(re.sub(r'\\([0-9A-F]{2})', lambda m: chr(int(m.group(1), 16)), l.split(chr(99) + chr(34), 1)[1]).rstrip(chr(34)).rstrip(chr(0)))
        print(f"argwidth: {n} sites")
        return 0
    if len(argv) == 2 and argv[0] == "--self-test":
        err = self_test(argv[1])
        print(f"argwidth: self-test {'FAILED: ' + err if err else 'OK'}")
        return 1 if err else 0
    sys.stderr.write(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
