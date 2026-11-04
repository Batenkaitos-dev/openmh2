#!/usr/bin/env python3
"""The census: a whole-file host compile of every C file of the matching build.

    python3 tools/pc/census.py [--arch i686|x86_64|both] [-j N] [--update]
                               [--files REGEX] [--kinds] [--by-file] [--show KIND]

Every C file build.ninja compiles (the decomp's src/ and build/assets/lbguild)
is compiled whole by clang with the port's flags (pc/game_flags.txt, the
flags mh2-core is built with) for i686 and for x86_64 (plain 64-bit pointers,
the plan's section 11 measurement). It reports

- the errors, by kind (the message with its names and numbers removed), and
- the warnings that matter (WARNINGS below: pointer/integer casts, implicit
  declarations, uninitialised uses, float conversions, ...), by flag,

per architecture and per file, and compares the counts with
config/pc/census_baseline.txt.

  census.py             compare: exit 1 if any count (a file's count of a kind)
                        is higher than the baseline, or a new one appears
  census.py --update    rewrite the baseline; refused if any i686 count is higher
                        (the baseline only goes down); creates it when absent.
                        The x86_64 counts are recorded but never gate: the PC
                        forms are 32-bit by design.

The baseline lines are "ARCH<TAB>FILE<TAB>KIND<TAB>COUNT" and nothing else.
"""
from __future__ import annotations

import argparse
import os
import re
import shlex
import subprocess
import sys
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(os.path.abspath(__file__)).parent.parent.parent
BASELINE = ROOT / "config" / "pc" / "census_baseline.txt"
FLAGS_FILE = ROOT / "pc" / "game_flags.txt"

# Warnings on top of clang's defaults (which stay on: they include
# implicit-function-declaration, int-conversion, array-bounds, return-type,
# parentheses, constant-conversion, ...). Each is a place where the code means
# something the PS2 and the host may not agree on.
WARNINGS = [
    "pointer-to-int-cast", "int-to-pointer-cast", "void-pointer-to-int-cast",
    "pointer-to-enum-cast", "void-pointer-to-enum-cast", "int-to-void-pointer-cast",
    "uninitialized", "sometimes-uninitialized", "conditional-uninitialized",
    "float-conversion", "implicit-float-conversion", "implicit-int-float-conversion",
    "shorten-64-to-32", "int-conversion", "incompatible-pointer-types",
    "incompatible-function-pointer-types", "cast-function-type-strict",
    "strict-prototypes", "unsequenced", "shift-count-overflow",
]
ARCH_FLAGS = {
    "i686": ["-m32", "-malign-double"],
    "x86_64": ["-m64"],
    "mingw": ["--target=i686-w64-mingw32"],       # the Windows core: llvm-mingw's clang
}
# the x86-64 run is the plan's section 11 measurement: plain compile, no
# i686-only flags, and the default calling convention
ARCH_SECTIONS = {"i686": ("common", "i686", "game"), "x86_64": ("common", "game"),
                 "mingw": ("common", "i686", "mingw", "game")}
# The rows that gate: a run on Linux gates on i686, a run on Windows on mingw. Both systems have
# the same clang version (LLVM_VERSION, tools/versions.txt), but the targets differ, so each platform
# has its own rows; the Windows run also prints its difference from the Linux rows.
GATING = ("i686", "mingw")


def game_flags(arch: str) -> list[str]:
    return section_flags(ARCH_SECTIONS[arch])


def section_flags(sections) -> list[str]:
    out, sect = [], None
    for line in FLAGS_FILE.read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        m = re.fullmatch(r"\[(\w+)\]", line)
        if m:
            sect = m.group(1)
        elif sect in sections:
            out += shlex.split(line)
    return out


def source_files() -> list[str]:
    """The C files of the matching build: build.ninja's cc inputs."""
    ninja = ROOT / "build.ninja"
    if not ninja.exists():
        sys.exit("census: build.ninja is missing (run tools/configure.py)")
    files = []
    for line in ninja.read_text().splitlines():
        m = re.match(r"build \S+: cc (\S+\.c)\b", line)
        if m:
            files.append(m.group(1))
    files.sort()
    return files


DIAG = re.compile(r"^(?P<file>[^\s:][^:]*):(?P<line>\d+):(?P<col>\d+): (?P<sev>error|warning|fatal error): (?P<msg>.*)$")
FLAGTAG = re.compile(r" \[-W([\w-]+)\]$")


def normalise(msg: str) -> str:
    m = re.search(r"'(\w+)_size_check' declared as an array with a negative size", msg)
    if m:
        return "struct size check"
    msg = re.sub(r"'[^']*'", "'X'", msg)
    msg = re.sub(r"\b\d+\b", "N", msg)
    return msg.strip()


def prepared(src: str) -> tuple[str, list[str]]:
    """The file to compile: the source itself, or (UTF-8 text in it) its Shift-JIS escaped copy."""
    data = (ROOT / src).read_bytes()
    if all(b < 0x80 for b in data):
        return src, []
    sys.path.insert(0, str(ROOT / "tools" / "pc"))
    from sjis_c import convert
    tmp = ROOT / "build" / "pc" / "census-tmp" / src.replace("/", "_")
    tmp.parent.mkdir(parents=True, exist_ok=True)
    tmp.write_bytes(convert(data.decode("utf-8")).encode("ascii"))
    return str(tmp.relative_to(ROOT)), ["-iquote", str(Path(src).parent)]


def compile_one(arch: str, src: str):
    cfile, extra = prepared(src)
    cmd = [os.environ.get("MH2_CLANG", "clang")] + ARCH_FLAGS[arch] + game_flags(arch) + [
        "-ferror-limit=0", "-fno-color-diagnostics", "-fno-caret-diagnostics",
        "-fno-diagnostics-show-note-include-stack", "-fdiagnostics-format=clang",
        "-I", "include", "-I", "build/gen", "-I", "src",
    ] + extra + [f"-W{w}" for w in WARNINGS] + ["-c", "-O0", "-o", os.devnull, cfile]
    p = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, errors="replace")
    counts: Counter = Counter()
    first: dict = {}
    for line in p.stderr.splitlines():
        m = DIAG.match(line)
        if not m:
            continue
        sev, msg = m.group("sev"), m.group("msg")
        if sev == "warning":
            t = FLAGTAG.search(msg)
            kind = f"warn {t.group(1)}" if t else "warn (untagged)"
        else:
            msg = FLAGTAG.sub("", msg)
            kind = "error " + normalise(msg)
        counts[kind] += 1
        first.setdefault(kind, f"{m.group('file')}:{m.group('line')}: {msg}")
    if p.returncode != 0 and not any(k.startswith("error") for k in counts):
        counts["error (compiler failed)"] += 1
        first.setdefault("error (compiler failed)", p.stderr.strip()[:200])
    return src, counts, first


def run(arch: str, files: list[str], jobs: int):
    result: dict[str, Counter] = {}
    first: dict[str, str] = {}
    with ThreadPoolExecutor(jobs) as ex:
        for src, counts, fst in ex.map(lambda f: compile_one(arch, f), files):
            result[src] = counts
            for k, v in fst.items():
                first.setdefault(k, f"{src}: {v}" if not v.startswith(src) else v)
    return result, first


def flatten(results: dict[str, dict[str, Counter]]) -> dict[tuple, int]:
    flat = {}
    for arch, per_file in results.items():
        for src, counts in per_file.items():
            for kind, n in counts.items():
                flat[(arch, src, kind)] = n
    return flat


def read_baseline() -> dict[tuple, int]:
    base = {}
    if BASELINE.exists():
        for line in BASELINE.read_text().splitlines():
            if not line or line.startswith("#"):
                continue
            arch, src, kind, n = line.split("\t")
            base[(arch, src, kind)] = int(n)
    return base


def write_baseline(flat: dict[tuple, int]):
    BASELINE.parent.mkdir(parents=True, exist_ok=True)
    lines = ["# tools/pc/census.py baseline: ARCH, FILE, KIND, COUNT. Counts only go down",
             "# (census.py --update refuses to raise one)..",
             "# i686 and x86_64: Linux's clang. mingw: llvm-mingw's clang, from a run on Windows (census.py --platform windows)."]
    lines += [f"{a}\t{s}\t{k}\t{n}" for (a, s, k), n in sorted(flat.items())]
    BASELINE.write_bytes(("\n".join(lines) + "\n").encode())      # LF on Windows too


def summary(arch: str, per_file: dict[str, Counter]) -> str:
    err = sum(n for c in per_file.values() for k, n in c.items() if k.startswith("error"))
    warn = sum(n for c in per_file.values() for k, n in c.items() if k.startswith("warn"))
    bad = sum(1 for c in per_file.values() if any(k.startswith("error") for k in c))
    return f"{arch}: {len(per_file)} files, {len(per_file) - bad} without errors, {err} errors, {warn} warnings"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--arch", choices=["i686", "x86_64", "mingw", "both"], help="default: both (i686 and x86_64) on Linux, mingw on Windows")
    ap.add_argument("--platform", choices=["linux", "windows"], default="windows" if os.name == "nt" else "linux",
                    help="windows: the census with llvm-mingw's clang against the mingw rows (from WSL: through winrun.py)")
    ap.add_argument("-j", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--update", action="store_true", help="rewrite the baseline (refused if a count is higher)")
    ap.add_argument("--files", help="regex: only the files whose path matches (no baseline write)")
    ap.add_argument("--kinds", action="store_true", help="print the totals by kind")
    ap.add_argument("--by-file", action="store_true", help="print the totals by file")
    ap.add_argument("--list-failing", action="store_true", help="print the files that don't compile on i686 (pc/known_broken.txt)")
    ap.add_argument("--show", metavar="KIND", help="print the first diagnostic of each kind containing KIND")
    a = ap.parse_args()

    if a.platform == "windows" and os.name != "nt":
        sys.path.insert(0, str(Path(os.path.abspath(__file__)).parent))
        import winrun
        sys.exit(winrun.run(["census.py"] + sys.argv[1:]))
    if a.arch is None:
        a.arch = "mingw" if a.platform == "windows" else "both"
    if a.platform == "windows" and a.arch != "mingw":
        sys.exit("census: on Windows only --arch mingw")
    files = source_files()
    if a.files:
        files = [f for f in files if re.search(a.files, f)]
        if a.update:
            sys.exit("census: --update needs the whole set of files")
    if a.list_failing:
        res, _ = run("i686", files, a.j)
        for f in sorted(res):
            if any(k.startswith("error") for k in res[f]):
                print(f)
        return
    archs = ["i686", "x86_64"] if a.arch == "both" else [a.arch]
    if "mingw" in archs and a.update and not any(k[0] == "mingw" for k in read_baseline()):
        print("census: no mingw rows yet: creating them")
    results, firsts = {}, {}
    for arch in archs:
        results[arch], firsts[arch] = run(arch, files, a.j)
        print(summary(arch, results[arch]))
        if a.kinds:
            tot = Counter()
            for c in results[arch].values():
                tot.update(c)
            for k, n in sorted(tot.items(), key=lambda kv: -kv[1]):
                print(f"  {n:6d}  {k}")
        if a.by_file:
            for src, c in sorted(results[arch].items(), key=lambda kv: -sum(kv[1].values())):
                if c:
                    e = sum(n for k, n in c.items() if k.startswith("error"))
                    w = sum(n for k, n in c.items() if k.startswith("warn"))
                    print(f"  {src}: {e} errors, {w} warnings")
        if a.show:
            for k, v in firsts[arch].items():
                if a.show in k:
                    print(f"  [{k}] {v}")

    flat = flatten(results)
    base = read_baseline()
    if a.files:
        base = {k: v for k, v in base.items() if k[0] in archs and re.search(a.files, k[1])}
    else:
        base = {k: v for k, v in base.items() if k[0] in archs}
    # The x86-64 run is informational: the PC forms of pointer-holding code
    # (PAL_RAM and the like) are 32-bit by design and cast pointers to integers there. Only i686 gates.
    have_rows = {arch for arch in archs if any(k[0] == arch for k in base)}
    worse = sorted((k, base.get(k, 0), n) for k, n in flat.items()
                   if n > base.get(k, 0) and k[0] in GATING and (k[0] in have_rows or not a.update))
    better = sum(base[k] - flat.get(k, 0) for k in base if base[k] > flat.get(k, 0))

    if a.update:
        if worse and BASELINE.exists():
            for (arch, src, kind), was, now in worse[:20]:
                print(f"  WORSE {arch} {src}: {kind}: {was} -> {now}")
            print(f"census: --update refused: {len(worse)} counts are higher than the baseline")
            sys.exit(1)
        old = {k: v for k, v in read_baseline().items() if k[0] not in archs}      # the other platform's rows stay
        flat.update(old)
        write_baseline(flat)
        print(f"census: baseline written ({len(flat)} entries; {better} fewer than before)")
        return
    if "mingw" in archs:
        # the Windows run against Linux's rows: informational (the same clang version, other targets)
        lin = {k: v for k, v in read_baseline().items() if k[0] == "i686" and (not a.files or re.search(a.files, k[1]))}
        win = {(k[1], k[2]): v for k, v in flat.items() if k[0] == "mingw"}
        diffs = sorted((s, kd, lin.get(("i686", s, kd), 0), win.get((s, kd), 0)) for s, kd in
                       {(k[1], k[2]) for k in lin} | set(win) if lin.get(("i686", s, kd), 0) != win.get((s, kd), 0))
        print(f"census: Windows (mingw) against the Linux (i686) rows: {len(diffs)} counts differ"
              + (" (informational: the same clang version, so a difference comes from the target)" if diffs else ""))
        for s, kd, l, w in diffs[:40]:
            print(f"  {s}: {kd}: Linux {l}, Windows {w}")
    if not BASELINE.exists():
        print("census: no baseline (config/pc/census_baseline.txt); run --update")
        sys.exit(1)
    if "mingw" in archs and "mingw" not in have_rows:
        print("census: the baseline has no mingw rows yet: census.py --platform windows --update records them")
        sys.exit(1)
    if worse:
        for (arch, src, kind), was, now in worse[:30]:
            print(f"  WORSE {arch} {src}: {kind}: {was} -> {now}")
        extra = f" (first 30 of {len(worse)} shown)" if len(worse) > 30 else ""
        print(f"census: WORSE than the baseline{extra}")
        sys.exit(1)
    print(f"census: OK, no worse than the baseline ({better} fewer; --update to record)")


if __name__ == "__main__":
    main()
