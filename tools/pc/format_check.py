#!/usr/bin/env python3
"""The core's format strings against the core's C library.

    python3 tools/pc/format_check.py [--list] [--root TREE] [DIR ...]

mh2-core is a 32-bit program (a `long` is 4 bytes) whose C library keeps the PS2's
sizes (pc/core/crt): its scanf stores 8 bytes for `%l` and for `%ll`, its printf
reads one 4-byte argument for `%ll`, `%q` and `%llx` (the PS2's library has no long
long) and widens a `%l` argument by its sign. A format that is right for the
host's C library is wrong there, and the damage is silent (a store over the next
variable, an argument read twice).

Every string literal in the core's files (pc/core, pc/ipc, src, include; the
host is out of scope: it uses each system's own C library) is read as a format
and checked:

  l       a `%l` conversion without a second `l` (d i u x X o c s n p, and the
          spellings %D %O %U): use int with %d, or s64 with %lld in sscanf
  hh      `%hh`: the library reads it as `%h`
  jzt     `%j`, `%z`, `%t`: the library has no such length
  ll      `%ll`, `%q`, `PRIx64` and its kin in a call that reaches the library's
          printf (vsnprintf, vsprintf, sprintf, printf and the wrappers named in
          CRT_PRINTF below): it reads 4 bytes, so the next argument is misread

`%lf`, `%le`, `%lg` are fine (a double), and so are `%lld` in sscanf and in KLog.
A finding fails the check. A reviewed exception goes in EXCEPTIONS with its reason:
(path, rule, count, why); the count must match, so a stale row fails too.
"""
from __future__ import annotations

import os
import re
import sys
from pathlib import Path

ROOT = Path(os.path.abspath(__file__)).parent.parent.parent
ROOTS = ["pc/core", "pc/ipc", "src", "include"]
EXTS = {".c", ".h", ".cc", ".cpp", ".inc"}

# Functions and macros whose format reaches the library's printf (crt_printf.c), by name; the second table is by file
# (a macro of that name elsewhere goes to another function).
CRT_PRINTF = {
    "sprintf", "vsprintf", "vsnprintf", "printf", "ps2_printf", "ps2_sprintf", "scePrintf",
    "SndHost_Log", "SND_LOGE", "SND_LOGW", "SND_LOGI", "SND_LOGD", "SND_LOGT", "SND_LOG_ONCE", "FzLog", "mcfs_log",
    # the game's own wrappers around vsnprintf
    "Page_Printf", "Debug_Printf", "Debug_Sprintf", "Kbd_Printf", "Quest_Sprintf",
    "Text_PrintfColored", "Text_PrintfAt", "Text_Printf", "Text_PrintfStyled", "Text_PrintCodes",
}
CRT_PRINTF_IN = {
    "pc/core/api/libsd.c": {"TR"},
    "pc/core/io/host_mcfs.c": {"LOGI", "LOGW", "LOGE"},
}

# (path, rule, count, why)
EXCEPTIONS = [
    ("pc/core/third_party/zlib/trees.c", "l", 16,
     "zlib's Tracev/Tracecv macros: compiled out unless ZLIB_DEBUG is defined, which no build here does"),
    ("pc/core/third_party/zlib/crc32.c", "l", 2,
     "zlib's make_crc_table output, inside #ifdef MAKECRCH: a generator for upstream's header, never built here"),
]

SPEC = re.compile(r"%[-+ #0'*]*(?:\d+|\*)?(?:\.(?:\d+|\*)?)?(hh|h|ll|l|q|L|j|z|t)?([A-Za-z\[])")
INT_CONV = set("diouxXcsnpDOU")
PRI64 = re.compile(r"^(?:PRI|SCN)[diouxX](?:64|MAX|LEAST64|FAST64)$")


def tokens(text: str):
    """Yield (kind, text, line, callee): kinds 'str' (adjacent literals joined), 'id'."""
    i, n, line = 0, len(text), 1
    stack: list[str] = []
    prev_id = ""
    pending = None  # (text, line, callee) of a literal that the next literal may extend
    while i < n:
        c = text[i]
        if c == "\n":
            line += 1
            i += 1
            continue
        if c in " \t\r\\":
            i += 1
            continue
        if text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
            continue
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            line += text.count("\n", i, j)
            i = j
            continue
        if c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            body = text[i + 1:j]
            if pending is not None:
                pending = (pending[0] + body, pending[1], pending[2])
            else:
                pending = (body, line, stack[-1] if stack else "")
            line += text.count("\n", i, j)
            i = j + 1
            continue
        if pending is not None:
            yield ("str",) + pending
            pending = None
        if c == "'":
            j = i + 1
            while j < n and text[j] != "'":
                j += 2 if text[j] == "\\" else 1
            i = j + 1
            continue
        if c.isalpha() or c == "_":
            j = i
            while j < n and (text[j].isalnum() or text[j] == "_"):
                j += 1
            prev_id = text[i:j]
            yield ("id", prev_id, line, stack[-1] if stack else "")
            i = j
            continue
        if c == "(":
            stack.append(prev_id)
            prev_id = ""
        elif c == ")":
            if stack:
                stack.pop()
            prev_id = ""
        else:
            prev_id = ""
        i += 1
    if pending is not None:
        yield ("str",) + pending


def check_file(rel: str, text: str, crt_wrappers: set[str]):
    """Findings of one file: (line, rule, text)."""
    out = []
    # the unit tests (a tests directory) are linked with the host's C library, whose printf reads `ll` as 8 bytes
    sinks = set() if "/tests/" in rel else CRT_PRINTF | CRT_PRINTF_IN.get(rel, set())
    for kind, s, line, callee in tokens(text):
        if kind == "id":
            if PRI64.match(s) and callee in sinks:
                out.append((line, "ll", f"{s} in {callee}()"))
            continue
        for m in SPEC.finditer(s):
            if m.start() > 0 and s[m.start() - 1] == "%":
                continue
            ln, conv = m.group(1), m.group(2)
            if ln == "l" and conv in INT_CONV:
                out.append((line, "l", f"%l{conv} in {callee}()"))
            elif ln == "hh" and conv in INT_CONV:
                out.append((line, "hh", f"%hh{conv} in {callee}()"))
            elif ln in ("j", "z", "t") and conv in INT_CONV:
                out.append((line, "jzt", f"%{ln}{conv} in {callee}()"))
            elif ln in ("ll", "q") and conv in INT_CONV and callee in sinks:
                out.append((line, "ll", f"%{ln}{conv} in {callee}()"))
    # a function that formats its own `...` with vsnprintf or vsprintf must be in CRT_PRINTF
    for m in re.finditer(r"^[A-Za-z_][\w \t\*]*?\b(\w+)\s*\([^;{)]*\.\.\.\s*\)\s*\{", text, re.M):
        body = text[m.end():m.end() + 600]
        if re.search(r"\bv(?:s|sn)printf\s*\(", body.split("\n}\n")[0]) and m.group(1) not in sinks:
            out.append((text.count("\n", 0, m.start()) + 1, "wrapper",
                        f"{m.group(1)}() formats with the library's vsnprintf: name it in CRT_PRINTF"))
    return out


def files(roots):
    for r in roots:
        base = ROOT / r
        for d, dn, fn in os.walk(base):
            dn[:] = sorted(x for x in dn if x not in (".git", "build", "__pycache__"))
            for f in sorted(fn):
                if os.path.splitext(f)[1] in EXTS:
                    yield os.path.join(d, f)


def main(argv):
    global ROOT
    if "--root" in argv:   # another tree of the same layout (an older commit's worktree)
        k = argv.index("--root")
        ROOT = Path(argv[k + 1])
        del argv[k:k + 2]
    roots = [a for a in argv if not a.startswith("--")] or ROOTS
    show = "--list" in argv
    found: list[tuple[str, int, str, str]] = []
    for p in files(roots):
        rel = os.path.relpath(p, ROOT).replace(os.sep, "/")
        try:
            text = open(p, encoding="latin-1").read()
        except OSError:
            continue
        for line, rule, what in check_file(rel, text, set()):
            found.append((rel, line, rule, what))
    # the reviewed exceptions: exact counts, with a reason
    bad = []
    counts: dict[tuple[str, str], int] = {}
    for rel, line, rule, what in found:
        counts[(rel, rule)] = counts.get((rel, rule), 0) + 1
    excused = set()
    for path, rule, n, why in EXCEPTIONS:
        if not why.strip():
            bad.append(f"exception {path} {rule}: no reason")
        got = counts.get((path, rule), 0)
        if got != n and roots == ROOTS:
            bad.append(f"exception {path} {rule}: {got} found, the table says {n}")
        excused.add((path, rule))
    nexc = 0
    for rel, line, rule, what in found:
        if (rel, rule) in excused:
            nexc += 1
            if show:
                print(f"excepted {rel}:{line}: {what}")
            continue
        bad.append(f"{rel}:{line}: [{rule}] {what}")
    for b in bad:
        print(b)
    nfiles = sum(1 for _ in files(roots))
    print(f"format strings: {len(bad)} findings, {nexc} excepted, {nfiles} files read")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
