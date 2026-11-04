#!/usr/bin/env python3
"""The layout check: every struct and union of the game's C, host layout against the PS2's.

    python3 tools/pc/layout.py [--arch i686|mingw|x86_64] [-j N] [--update] [--files REGEX] [-v]

Run it in WSL or on Linux: the PS2 side is MWCC through wibo, a Linux program (natively on Windows
MWCC's licence check fails, and the check refuses to report on nothing). The architectures:

- i686    the Linux core: clang -m32 -malign-double.
- mingw   the Windows core: --target=i686-w64-mingw32 and the flags of pc/game_flags.txt's [mingw]
          section (-mno-ms-bitfields), no -malign-double (the ABI aligns 8-byte members to 8). The
          layout depends on the target, not on the clang's version, so the Linux clang answers for
          llvm-mingw's. --clang names another.
- x86_64  the plain 64-bit layout (informational).

The check fails (never "OK") when MWCC cannot run, when it compared no records, or when a unit that
compiled before (not listed in the baseline) no longer does.

For each struct or union the game's C defines (in include/*.h, one translation
unit per header, and in the src/ files that define their own), the offset of
every member (nested aggregates flattened, bit-fields by bit position) and the
size, as

- the host sees them: clang's -fdump-record-layouts, for i686 (the port's
  flags: -m32 -malign-double) or x86_64; and
- the PS2 sees them: the debug information of mwccps2 itself. The matching
  compiler is run on the same translation unit with -g, and the DWARF 1
  records it writes (.debug: the structure and union types, their members'
  locations, bit sizes and bit offsets) are read here. This is the compiler
  that built the game, so it is the layout, not a model of it.

Both compilers see the same file plus a list of probes (one variable of each
type, so that MWCC emits the type and clang lays it out). Every difference is
printed. config/pc/layout_baseline.txt lists the known differences; the check
fails on a difference that is not in it, and --update only removes lines.
"""
from __future__ import annotations

import argparse
import os
import re
import struct
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(os.path.abspath(__file__)).parent.parent.parent
BASELINE = ROOT / "config" / "pc" / "layout_baseline.txt"
TMP = ROOT / "build" / "pc" / "layout"
DEPS = ROOT / "tools" / "deps" / "linux-x64"
sys.path.insert(0, str(Path(os.path.abspath(__file__)).parent))
import census                                    # the flag file's reader: one list of flags for the build and the checks

# mingw: the target is i686-w64-mingw32 (its own ABI: 8-byte members align to 8, bit-fields are MS-style
# unless pc/game_flags.txt's [mingw] section says otherwise)
ARCH_FLAGS = {"i686": ["-m32", "-malign-double"], "x86_64": ["-m64"],
              "mingw": ["--target=i686-w64-mingw32"] + census.section_flags(("mingw",))}

# ---------------------------------------------------------------- DWARF 1

TAG_MEMBER, TAG_STRUCT, TAG_UNION, TAG_TYPEDEF = 0x0D, 0x13, 0x17, 0x16
AT_SIBLING, AT_LOCATION, AT_NAME, AT_FUND, AT_MOD_FUND, AT_UDT, AT_MOD_UDT = 1, 2, 3, 5, 6, 7, 8
AT_SUBSCR, AT_BYTE_SIZE, AT_BIT_OFFSET, AT_BIT_SIZE = 0x0A, 0x0B, 0x0C, 0x0D
MOD_POINTER, MOD_REFERENCE = 1, 2


def read_debug(obj: Path) -> bytes:
    from elftools.elf.elffile import ELFFile
    with open(obj, "rb") as f:
        elf = ELFFile(f)
        for s in elf.iter_sections():
            if s.name == ".debug":
                return s.data()          # the first one holds the file's types
    return b""


def parse_dwarf1(d: bytes) -> dict[int, dict]:
    dies: dict[int, dict] = {}
    pos = 0
    while pos + 4 <= len(d):
        ln, = struct.unpack_from("<I", d, pos)
        if ln < 4:
            break
        if ln == 4:
            pos += 4
            continue
        tag, = struct.unpack_from("<H", d, pos + 4)
        attrs: dict[int, object] = {}
        p, end = pos + 6, pos + ln
        while p + 2 <= end:
            a, = struct.unpack_from("<H", d, p)
            p += 2
            name, form = a >> 4, a & 0xF
            if form in (1, 2, 6):
                v, = struct.unpack_from("<I", d, p); p += 4
            elif form == 5:
                v, = struct.unpack_from("<H", d, p); p += 2
            elif form == 7:
                v, = struct.unpack_from("<Q", d, p); p += 8
            elif form == 3:
                n, = struct.unpack_from("<H", d, p); p += 2; v = d[p:p + n]; p += n
            elif form == 4:
                n, = struct.unpack_from("<I", d, p); p += 4; v = d[p:p + n]; p += n
            elif form == 8:
                q = d.index(b"\0", p); v = d[p:q].decode("ascii", "replace"); p = q + 1
            else:
                break
            attrs[name] = v
        dies[pos] = {"tag": tag, "attrs": attrs, "len": ln, "kids": []}
        pos += ln
    # members belong to the structure whose sibling lies after them
    offs = sorted(dies)
    for o in offs:
        e = dies[o]
        if e["tag"] in (TAG_STRUCT, TAG_UNION):
            sib = e["attrs"].get(AT_SIBLING, o + e["len"])
            q = o + e["len"]
            while q < sib and q in dies:
                if dies[q]["tag"] == TAG_MEMBER:
                    e["kids"].append(q)
                q += dies[q]["len"]
                if q < sib and q not in dies:        # the 4-byte terminator
                    q += 4
    return dies


def member_target(dies, m) -> tuple[int | None, bool]:
    """(the aggregate DIE the member's type is, after typedefs, or None; is_array_or_pointer)"""
    a = m["attrs"]
    if AT_SUBSCR in a:
        return None, True
    ref = a.get(AT_UDT)
    if ref is None and AT_MOD_UDT in a:
        blk = a[AT_MOD_UDT]
        mods, ref = blk[:-4], struct.unpack("<I", blk[-4:])[0]
        if MOD_POINTER in mods or MOD_REFERENCE in mods:
            return None, True
    seen = 0
    while ref in dies and seen < 20:
        t = dies[ref]
        if t["tag"] in (TAG_STRUCT, TAG_UNION):
            return ref, False
        if t["tag"] == TAG_TYPEDEF:
            ta = t["attrs"]
            if AT_UDT in ta:
                ref = ta[AT_UDT]
            elif AT_MOD_UDT in ta:
                blk = ta[AT_MOD_UDT]
                if MOD_POINTER in blk[:-4]:
                    return None, True
                ref = struct.unpack("<I", blk[-4:])[0]
            else:
                return None, False
            seen += 1
            continue
        return None, False
    return None, False


def flatten_dwarf(dies, so: int, base: int, prefix: str, out: dict, depth=0):
    s = dies[so]
    anon = 0
    for mo in s["kids"]:
        m = dies[mo]
        a = m["attrs"]
        name = a.get(AT_NAME)
        loc = a.get(AT_LOCATION, b"")
        off = struct.unpack_from("<I", loc, 1)[0] if len(loc) >= 5 and loc[0] == 0x04 else 0
        if AT_BIT_SIZE in a:
            name = name or f"<anon{anon}>"
            anon += 0 if a.get(AT_NAME) else 1
            out[prefix + name] = ((base + off) * 8 + a.get(AT_BIT_OFFSET, 0), a[AT_BIT_SIZE])
            continue
        tgt, _ = member_target(dies, m)
        if not name:
            if tgt is not None and depth < 8:        # an anonymous aggregate: its members belong to the parent
                flatten_dwarf(dies, tgt, base + off, prefix, out, depth + 1)
                continue
            name = f"<anon{anon}>"
            anon += 1
        out[prefix + name] = ((base + off) * 8, None)
        if tgt is not None and depth < 8:
            flatten_dwarf(dies, tgt, base + off, prefix + name + ".", out, depth + 1)


def dwarf_records(obj: Path) -> dict[str, dict]:
    dies = parse_dwarf1(read_debug(obj))
    aliases: dict[int, list[str]] = {}
    for o, e in dies.items():
        if e["tag"] == TAG_TYPEDEF:
            a = e["attrs"]
            ref = a.get(AT_UDT)
            if ref in dies and dies[ref]["tag"] in (TAG_STRUCT, TAG_UNION):
                aliases.setdefault(ref, []).append(a.get(AT_NAME, ""))
    recs = {}
    for o, e in dies.items():
        if e["tag"] not in (TAG_STRUCT, TAG_UNION) or AT_BYTE_SIZE not in e["attrs"]:
            continue
        names = [n for n in [e["attrs"].get(AT_NAME)] + aliases.get(o, []) if n]
        if not names:
            continue
        mem: dict = {}
        flatten_dwarf(dies, o, 0, "", mem)
        r = {"size": e["attrs"][AT_BYTE_SIZE], "members": mem, "union": e["tag"] == TAG_UNION}
        for n in names:
            recs[n] = r
    return recs

# ---------------------------------------------------------------- clang's dump

ROW = re.compile(r"^\s*(\d+)(?::(\d+)-(\d+))? \| ( *)(.*?)\s*$")


def clang_records(text: str) -> dict[str, dict]:
    recs = {}
    for block in text.split("*** Dumping AST Record Layout\n")[1:]:
        lines = block.split("\n\n")[0].split("\n")
        rows = []
        size = None
        for ln in lines:
            m = re.match(r"^\s*\| \[sizeof=(\d+)", ln)
            if m:
                size = int(m.group(1))
                continue
            m = ROW.match(ln)
            if m:
                rows.append((int(m.group(1)), m.group(2), m.group(3), len(m.group(4)), m.group(5)))
        if not rows or size is None:
            continue
        head = rows[0][4]
        if "::(" in head or "(unnamed" in head or "(anonymous" in head:
            continue                      # a nested aggregate: dumped inside its parent
        name = head.split(" ", 1)[1] if head.startswith(("struct ", "union ")) else head
        mem: dict = {}
        stack: list[tuple[int, str, int]] = []      # (indent, path prefix, anon counter)
        counters: dict[str, int] = {}
        body = rows[1:]
        for i, (off, lo, hi, ind, txt) in enumerate(body):
            while stack and ind <= stack[-1][0]:
                stack.pop()
            prefix = stack[-1][1] if stack else ""
            is_agg = i + 1 < len(body) and body[i + 1][3] > ind
            mname = "" if txt.endswith(")") else txt.rsplit(" ", 1)[-1].lstrip("*")
            if not mname and is_agg:             # an anonymous aggregate: its members belong to the parent
                stack.append((ind, prefix, 0))
                continue
            if not mname:
                k = counters.get(prefix, 0)
                counters[prefix] = k + 1
                mname = f"<anon{k}>"
            path = prefix + mname
            if lo is not None:
                mem[path] = (off * 8 + int(lo), int(hi) - int(lo) + 1)
            else:
                mem[path] = (off * 8, None)
            if is_agg:
                stack.append((ind, path + ".", 0))
        recs[name] = {"size": size, "members": mem}
    return recs

# ---------------------------------------------------------------- the translation units

DEF_OPEN = re.compile(r"^(typedef\s+)?(struct|union)(?:\s+(\w+))?\s*\{")
DEF_CLOSE = re.compile(r"^\}\s*([\w\s,*]+);")


def record_names(path: Path) -> list[str]:
    """The C names of the records a file defines at its top level (probes for them)."""
    names: list[str] = []
    cur = None
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        m = DEF_OPEN.match(line)
        if m and cur is None:
            cur = (bool(m.group(1)), m.group(2), m.group(3))
            if line.rstrip().endswith("}") or ("}" in line and line.count("{") == line.count("}")):
                cur = None            # one-line definition: skip
            continue
        if cur is not None and line.startswith("}"):
            td, kw, tag = cur
            cur = None
            if td:
                m2 = DEF_CLOSE.match(line)
                al = [x.strip() for x in m2.group(1).split(",")] if m2 else []
                al = [x for x in al if re.fullmatch(r"\w+", x)]
                if tag:
                    names.append(f"{kw} {tag}")
                elif al:
                    names.append(al[0])
            elif tag:
                names.append(f"{kw} {tag}")
    return names


def probe_tu(name: str, include: str, names: list[str], iquote: str | None) -> Path:
    TMP.mkdir(parents=True, exist_ok=True)
    out = TMP / f"{name}.c"
    body = ['#include "common.h"'] if not include.endswith('.c') else []
    body.append(f'#include "{include}"')
    for i, n in enumerate(names):
        body.append(f"#ifdef __MWERKS__\n{n} probe_{i};\n#else\n_Static_assert(sizeof({n}) > 0, \"\");\n#endif")
    out.write_text("\n".join(body) + "\n")
    return out


def run(cmd, env=None):
    return subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, errors="replace", env=env)


def mwcc_cmd() -> list[str]:
    return [str(DEPS / "wibo" / "wibo"), str(DEPS / "mwcc" / "mwccps2.exe")]


def check_tu(arch: str, clang: str, name: str, include: str, names: list[str], iquote: str | None):
    """-> (diffs, compared, notes)"""
    inc = ["-I", ".", "-I", "include", "-I", "build/gen", "-I", "src"] + (["-I", iquote] if iquote else [])
    obj = TMP / f"{name}.o"
    env = dict(os.environ, MWCIncludes=".")
    mw = ["-c", "-nostdinc", "-g", "-O0", "-i", ".", "-i", "include", "-i", "build/gen", "-i", "src"]
    if iquote:
        mw += ["-i", iquote]
    for attempt in range(4):
        # a type the file only declares can't be probed: drop it (MWCC aborts on the error) and retry
        tu = probe_tu(name, include, names, iquote)
        rel = str(tu.relative_to(ROOT))
        obj.unlink(missing_ok=True)
        p = run(mwcc_cmd() + mw + ["-o", str(obj), rel], env)
        if obj.exists():
            break
        bad = set(re.findall(r"incomplete struct/union/class '([^']+)'", p.stdout + p.stderr))
        if not bad or not names:
            return [], set(), [f"unit {name}: mwcc failed: " + (p.stdout + p.stderr).strip().splitlines()[-1:][0][:120]]
        names = [n for n in names if n not in bad]
    else:
        return [], set(), [f"unit {name}: mwcc failed"]
    c = run([clang] + ARCH_FLAGS[arch] + ["-fms-extensions", "-w", "-fsyntax-only", "-ferror-limit=0",
            "-Xclang", "-fdump-record-layouts"] + inc + [rel])
    host = clang_records(c.stdout)
    ps2 = dwarf_records(obj)
    obj.unlink(missing_ok=True)
    diffs, compared = [], set()
    for n in sorted(set(host) & set(ps2)):
        h, p2 = host[n], ps2[n]
        compared.add(n)
        if h["size"] != p2["size"]:
            diffs.append(f"{n}: size {h['size']} (host) vs {p2['size']} (PS2)")
        hm, pm = h["members"], p2["members"]
        for k in sorted(set(hm) | set(pm)):
            if k not in pm:
                diffs.append(f"{n}.{k}: not in the PS2 type")
            elif k not in hm:
                diffs.append(f"{n}.{k}: not in the host type")
            elif hm[k] != pm[k]:
                f = lambda t: f"{t[0] // 8}" + (f"+{t[0] % 8}b:{t[1]}" if t[1] is not None else "")
                diffs.append(f"{n}.{k}: offset {f(hm[k])} (host) vs {f(pm[k])} (PS2)")
    return diffs, compared, []


def selftest() -> bool:
    """MWCC compiles a trivial file here (a failed licence check or a missing wibo does not)."""
    TMP.mkdir(parents=True, exist_ok=True)
    src, obj = TMP / "selftest.c", TMP / "selftest.o"
    src.write_text("struct T { int a; } t;\n")
    obj.unlink(missing_ok=True)
    run(mwcc_cmd() + ["-c", "-nostdinc", "-g", "-o", str(obj), str(src.relative_to(ROOT))], dict(os.environ, MWCIncludes="."))
    ok = obj.exists() and bool(read_debug(obj))
    obj.unlink(missing_ok=True)
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--arch", choices=list(ARCH_FLAGS), default="i686")
    ap.add_argument("--clang", default="clang", help="the host compiler (default: clang on PATH)")
    ap.add_argument("-j", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--update", action="store_true")
    ap.add_argument("--files", help="regex: only the units whose file path matches")
    ap.add_argument("-v", action="store_true", help="print every difference")
    a = ap.parse_args()

    if not (DEPS / "wibo" / "wibo").exists() or not (DEPS / "mwcc" / "mwccps2.exe").exists():
        sys.exit("layout: FAILED: MWCC (tools/deps/linux-x64/mwcc, wibo) is not installed; the PS2 side cannot run")
    if not selftest():
        sys.exit("layout: FAILED: MWCC does not run here (on Windows its licence check fails; run this in WSL or on Linux)")
    units = []                                         # (name, include, names, iquote)
    probe = ROOT / "pc" / "tests" / "layout_probe.h"   # structs whose layout depends on the target's ABI rules
    units.append(("h_layout_probe", "pc/tests/layout_probe.h", record_names(probe), None))
    for h in sorted((ROOT / "include").glob("*.h")):
        names = record_names(h)
        if names:
            units.append((f"h_{h.stem}", h.name, names, None))
    srcs = []
    for line in (ROOT / "build.ninja").read_text().splitlines():
        m = re.match(r"build \S+: cc (\S+\.c)\b", line)
        if m:
            srcs.append(m.group(1))
    for s in sorted(srcs):
        if s.startswith("build/"):
            continue
        names = record_names(ROOT / s)
        if names:
            units.append(("s_" + s.replace("/", "_")[:-2], s, names, str(Path(s).parent)))
    if a.files:
        units = [u for u in units if re.search(a.files, u[1])]

    alld, compared, notes = set(), set(), []
    with ThreadPoolExecutor(a.j) as ex:
        for d, n, nt in ex.map(lambda u: check_tu(a.arch, a.clang, *u), units):
            alld.update(d)
            compared |= n
            notes += nt
    for nt in notes:
        print("  note:", nt)
    if not compared or len(notes) == len(units):
        sys.exit(f"layout: FAILED: {len(compared)} records compared, {len(notes)} of {len(units)} units not compiled by MWCC")
    # a unit MWCC cannot compile is a baseline line too: "ARCH<TAB>unit NAME: mwcc failed"
    unit_lines = {nt.split(": mwcc failed")[0] + ": mwcc failed" for nt in notes}
    base = set()
    if BASELINE.exists():
        base = {l for l in BASELINE.read_text().splitlines() if l and not l.startswith("#") and l.startswith(a.arch + "\t")}
    cur = {f"{a.arch}\t{d}" for d in alld | unit_lines}
    if a.v:
        for d in sorted(alld):
            print("  " + d)
    sizes = sum(1 for d in alld if ": size " in d)
    checked = set()
    for f in list((ROOT / "include").glob("*.h")) + [ROOT / x for x in srcs if not x.startswith("build/")]:
        checked |= set(re.findall(r"(\w+)_size_check", f.read_text(encoding="utf-8", errors="replace")))
    print(f"  size-checked types compared: {len(checked & compared)} of {len(checked)}"
          + (f"; not compared: {', '.join(sorted(checked - compared)[:12])}" if checked - compared else ""))
    print(f"layout {a.arch}: {len(units)} units, {len(compared)} distinct records compared, "
          f"{len({d.split(':')[0].split('.')[0] for d in alld})} records differ ({sizes} in size, {len(alld) - sizes} in members); "
          f"{len(notes)} units not compiled")
    new = sorted(cur - base)
    if a.update:
        other = [l for l in BASELINE.read_text().splitlines() if l and not l.startswith("#") and not l.startswith(a.arch + "\t")] if BASELINE.exists() else []
        if new and BASELINE.exists():
            for l in new[:20]:
                print("  NEW", l)
            print(f"layout: --update refused: {len(new)} differences are not in the baseline")
            sys.exit(1)
        BASELINE.parent.mkdir(parents=True, exist_ok=True)
        BASELINE.write_text("# tools/pc/layout.py: known layout differences, host vs PS2 (ARCH<TAB>difference)\n"
                            + "\n".join(sorted(set(other) | cur)) + "\n")
        print(f"layout: baseline written ({len(cur)} lines for {a.arch})")
        return
    if new:
        for l in new[:30]:
            print("  NEW", l)
        print(f"layout: WORSE: {len(new)} differences are not in the baseline")
        sys.exit(1)
    print(f"layout: OK, no differences beyond the baseline ({len(base - cur)} fewer)")


if __name__ == "__main__":
    main()
