#!/usr/bin/env python3
"""Generates the aborting stubs of the platform layer's library functions.

    python3 tools/pc/gen_stubs.py --objects @RSP --names api_names.txt \
            --headers include --out stubs.c [--platform linux|windows] [--report]

Every library entry point the game calls, and every hook in include/pal.h,
must exist for mh2-core to link. Those nobody has written yet get a stub that
aborts with its name (Pal_StubAbort, pc/core/pal_mem.c). The set is computed
from the compiled objects, so a name the game starts to use is picked up the
next build:

  stubs = (the symbols the objects use and none defines) + (api_names.txt)
          - (what the objects define)

- A function the game calls through a prototype is "__regcall3__NAME" in the
  objects (the game's calling convention, pc/README.md). Its stub is
  "void NAME(void)", compiled with the same flag, so it carries that name.
- A variadic function (a prototype with "..." in include/*.h) keeps its plain
  name in every convention; its stub takes the format and the rest.
- A plain undefined name is the host C library's or the compiler's runtime
  (memcpy for a structure copy, __divdi3, ...): left to the linker. A name
  that is none of those is game data nobody defined (a library's variable, a
  blob of the original): reported, and stubbed as an array of zero bytes
  (--data-size) so the image links; it is data the module that owns it must
  provide.
- api_names.txt lists the library functions
  and the hooks, so that a name no compiled file calls yet has a stub too.
  "NAME" is a function, "NAME ..." a variadic one, "NAME data SIZE" data.

Calls by PS2 address (pc/core/pal_ovcall.h): a function stub named func_ADDR (or D_ADDR) whose address a
module's symbol files name (config/<module>.yaml's symbol_addrs_path, the later file winning) is no
stub but an entry point that reaches the function there. In main or sub_main (resident): a jump. In
an overlay's window (pal_overlay.c's OV lines give each overlay's id and window): a table indexed by
the overlay active in the window, whose other entries stop with a fatal error naming the call.

The game's own functions get no stub: a name that a src/ file compiled into the core defines (a
function with a body that is not static, found by game_functions below) must come from that file. When
a stub would stand in for one, the generator stops and names it. A plain "inline" definition
(Lerp, Alpha8) emits no external definition under C99, so the calls clang did not inline linked to
aborting stubs of the game's own functions. A stub is for a library function nobody has written yet.
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

MANGLE = "__regcall3__"
# the compiler's and the host libc's names that appear undefined in game objects (_alloca: the
# i386 mingw stack probe of every frame over 4 KB, a data stub of which is executed)
RUNTIME = re.compile(
    r"^(mem(cpy|set|move|cmp)|str(len|cpy|cmp)|__(u?div|u?mod)di3|__(fix|float)\w*di\w*|__stack_chk_\w+|"
    r"_GLOBAL_OFFSET_TABLE_|__executable_start|_end|__ImageBase|__chkstk\w*|_alloca|_?_?ashldi3|__lshrdi3|__ashrdi3|__muldi3|__cmpdi2|__ucmpdi2|__udivmoddi4|abort|__(asan|ubsan|sanitizer|sancov)_\w+)$")


def read_list(arg: str) -> list[str]:
    if arg.startswith("@"):
        return [l.strip() for l in Path(arg[1:]).read_text().splitlines() if l.strip()]
    return [arg]


def nm(objs: list[str], nm_tool: str) -> tuple[set[str], set[str]]:
    defined, undefined = set(), set()
    # Chunks of at most 16,000 characters of paths: Windows' command line ends at 32,767,
    # and its object paths are long.
    chunks, cur, size = [], [], 0
    for o in objs:
        if cur and size + len(o) + 1 > 16000:
            chunks.append(cur)
            cur, size = [], 0
        cur.append(o)
        size += len(o) + 1
    if cur:
        chunks.append(cur)
    for chunk in chunks:
        out = subprocess.run([nm_tool, "-P", "-g", *chunk], capture_output=True, text=True, check=True).stdout
        for line in out.splitlines():
            f = line.split()
            if len(f) < 2 or line.endswith(":"):
                continue
            (undefined if f[1] == "U" else defined).add(f[0])
    return defined, undefined


def _strip_c(text: str) -> str:
    """The text without comments, string and character literals, and preprocessor lines."""
    text = re.sub(r"/\*.*?\*/|//[^\n]*", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    text = re.sub(r'"(?:\\.|[^"\\\n])*"|' + r"'(?:\\.|[^'\\\n])*'", '""', text)
    return re.sub(r"^[ \t]*#(?:[^\n]*\\\n)*[^\n]*", "", text, flags=re.M)


def game_functions(objs: list[str], root: Path) -> dict[str, str]:
    """{name: "file:line"} of the functions with a body, not static (or local, zlib's static), that the src/ files compiled into the
    core define (every #if branch counts): at brace depth 0, a "{" whose declaration (back to the last
    ";" or "}") ends in ")" and has no "=" or struct/union/enum/typedef. A plain "inline" counts: it
    emits no definition of its own under C99, so a call clang does not inline reaches a stub."""
    src = root / "src"
    files: set[Path] = set()
    for o in objs:
        for m in re.finditer(r"/src/", o):
            rel = re.sub(r"\.c\.(?:o|obj)$", ".c", o[m.end():])
            if rel.endswith(".c") and (src / rel).is_file():
                files.add(src / rel)
                break
    head = re.compile(r"(?:[^()]|\(\s*__attribute__\s*\(\([^()]*\)\)\s*\))*?\b(\w+)\s*\(")
    out: dict[str, str] = {}
    for f in sorted(files):
        text = _strip_c(f.read_text(encoding="utf-8", errors="replace"))
        depth, last, line, internal = 0, 0, 1, set()
        for i, ch in enumerate(text):
            if ch == "\n":
                line += 1
            elif ch == "{":
                if depth == 0:
                    decl = text[last:i].strip()
                    if decl.endswith(")") and "=" not in decl and not re.match(r"(?:typedef|struct|union|enum)\b[^(]*$", decl):
                        m = head.match(decl)
                        if m and not re.search(r"\b(?:static|local)\b", decl[:m.start(1)]):
                            out.setdefault(m.group(1), f"{f.relative_to(root)}:{line}")
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    last = i + 1
            elif ch == ";" and depth == 0:
                decl = text[last:i].strip()
                m = head.match(decl)
                if m and re.search(r"\b(?:static|local)\b", decl[:m.start(1)]):
                    internal.add(m.group(1))        # "static T f(...);": a later definition without the keyword is internal too
                last = i + 1
        for n in internal:
            out.pop(n, None)
    return out


SELF_TEST_C = """
/* a comment with f_comment(void) { */
#define MACRO(x) \\
    ((x) + 1)
inline int PlainInline(int a) { return a; }
static inline int StaticInline(int a) { return a; }
static int Forward(int a);
int Forward(int a) { return a; }
int Plain(void)
{
    if (1) { return 0; }
    return 1;
}
struct S { int a; } gS = { 1 };
int tbl[2] = { 1, 2 };
local int ZlibLocal(int a) { return a; }
extern inline int ExternInline(int a) { return a; }
void (*Fp)(void) = 0;
"""


def self_test() -> int:
    import tempfile
    with tempfile.TemporaryDirectory() as d:
        root = Path(d)
        (root / "src").mkdir()
        (root / "src" / "t.c").write_text(SELF_TEST_C)
        got = set(game_functions(["/x/build/./src/t.c.o"], root))
    want = {"PlainInline", "Plain", "ExternInline"}
    print(f"gen_stubs: self-test {'OK' if got == want else 'FAILED: ' + str(sorted(got))}")
    return 0 if got == want else 1


def variadics(roots: list[Path]) -> set[str]:
    """Names with a prototype that ends in "..." in the headers and in the sources' own declarations."""
    out = set()
    pat = re.compile(r"\b(\w+)\s*\([^;{}()]*(?:\([^()]*\)[^;{}()]*)*,\s*\.\.\.\s*\)\s*;")
    for r in roots:
        for h in list(r.glob("*.h")) + list(r.glob("*/*.c")):
            out |= {m.group(1) for m in pat.finditer(h.read_text(encoding="utf-8", errors="replace"))}
    return out


def overlays(pal_overlay: Path) -> list[tuple[int, str, int]]:
    """(id, module, window) of each overlay with C: pal_overlay.c's OV(id, module, window) lines."""
    pat = re.compile(r"\bOV\((\d+),\s*(\w+),\s*(\d+)\)")
    return [(int(i), m, int(g)) for i, m, g in pat.findall(pal_overlay.read_text())]


def symbol_map(root: Path, module: str) -> dict[int, str]:
    """{address: name} of a module's symbol files, in its YAML's order (a later name wins)."""
    yaml = root / "config" / (f"{module}.yaml" if module == "main" else f"overlays/{module}.yaml")
    files, inlist = [], False
    for line in yaml.read_text().splitlines():
        if line.strip().startswith("symbol_addrs_path:"):
            inlist = True
        elif inlist and line.strip().startswith("- "):
            files.append(line.strip()[2:].strip())
        elif inlist and line.strip() and not line.strip().startswith("#"):
            inlist = False
    # its own addresses: the code segment's vram less its file offset, up to the file's end (the
    # last "- [0xEND]"); a module's list also names other modules' addresses it calls
    text = yaml.read_text()
    start = int(re.search(r"^ {4}start:\s*0x([0-9A-Fa-f]+)", text, re.M).group(1), 16)
    vram = int(re.search(r"^ {4}vram:\s*0x([0-9A-Fa-f]+)", text, re.M).group(1), 16)
    end = int(re.findall(r"^\s*- \[0x([0-9A-Fa-f]+)\]", text, re.M)[-1], 16)
    lo, hi = vram - start, vram - start + end
    out: dict[int, str] = {}
    pat = re.compile(r"^\s*(\w+)\s*=\s*0x([0-9A-Fa-f]+)\s*;")
    for f in files:
        if not f.startswith("config/"):
            continue        # build/symbols: the other modules' names, generated
        for line in (root / f).read_text(encoding="utf-8", errors="replace").splitlines():
            m = pat.match(line)
            if m and lo <= int(m.group(2), 16) < hi:
                out[int(m.group(2), 16)] = m.group(1)
    return out


def address_calls(funcs: set[str], defined: set[str], root: Path, pal_overlay: Path, pre: str):
    """The entry points of the func_ADDR names a module names (module docstring); returns the C
    lines and the names handled."""
    ovs = overlays(pal_overlay)
    maps = {m: symbol_map(root, m) for m in ["main"] + [m for _, m, _ in ovs]}
    resident = ["main"] + [m for _, m, g in ovs if g == 0]
    lines, done, decl = [], set(), set()
    for f in sorted(funcs):
        m = re.fullmatch(r"(?:func|D)_([0-9A-Fa-f]{6,8})", f)     # D_: a function named as data
        if not m:
            continue
        addr = int(m.group(1), 16)

        def target(mod):
            n = maps[mod].get(addr)
            return n if n and n != f and MANGLE + n in defined else None
        res = [t for t in (target(r) for r in resident) if t]
        if res:
            if res[0] not in decl:
                lines.append(f"void {res[0]}(void);")
                decl.add(res[0])
            lines.append(f'PAL_OVJUMP("{MANGLE}{f}", "{MANGLE}{res[0]}");')
            done.add(f)
            continue
        named = [(i, mod, g) for i, mod, g in ovs if g != 0 and addr in maps[mod]]
        wins = {g for _, _, g in named}
        if len(wins) != 1:
            continue        # no module names it (or two windows do): an aborting stub
        win = wins.pop()
        known = "; ".join(f"{mod}'s {maps[mod][addr]}" + ("" if target(mod) else ", no PC function")
                          for _, mod, _ in named)
        miss = f"PalOvMiss_{f}"
        lines.append(f'static void {miss}(void) {{ Pal_OverlayCallMissing("{f}", 0x{addr:X}u, {win}, "{known}"); }}')
        ent = [miss] * 16
        for i, mod, g in ovs:
            if g == win and target(mod):
                ent[i] = target(mod)
                if ent[i] not in decl:
                    lines.append(f"void {ent[i]}(void);")
                    decl.add(ent[i])
        lines.append(f"void (*const PalOvTbl_{f}[PAL_OVCALL_IDS])(void) = {{ {', '.join(ent)} }};")
        lines.append(f'PAL_OVCALL("{MANGLE}{f}", "PalOvTbl_{f}", {win});')
        done.add(f)
    return lines, done


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--self-test", action="store_true", help="the source scan on a sample (build.py --check runs it)")
    ap.add_argument("--objects")
    ap.add_argument("--host-objects", help="the platform layer's objects: they define names, and use the host libc's")
    ap.add_argument("--names")
    ap.add_argument("--headers")
    ap.add_argument("--out")
    ap.add_argument("--platform", default="linux")
    ap.add_argument("--nm", default="nm")
    ap.add_argument("--data-size", type=int, default=0x1000)
    ap.add_argument("--report", action="store_true")
    ap.add_argument("--root", help="the repository (config/, pc/core/pal_overlay.c): the parent of --headers")
    a = ap.parse_args()
    if a.self_test:
        sys.exit(self_test())
    if not (a.objects and a.names and a.headers and a.out):
        ap.error("--objects, --names, --headers and --out are required")

    objs = read_list(a.objects)
    defined, undefined = nm(objs, a.nm)
    if a.host_objects:      # a plain undefined name of the platform layer is the host libc's, not game data
        hdef, hund = nm(read_list(a.host_objects), a.nm)
        defined |= hdef
        undefined |= {u for u in hund if u.startswith(("__regcall3__", "___regcall3__"))}
    pre = "_" if a.platform == "windows" else ""

    def plain(s: str) -> str:
        return s[1:] if pre and s.startswith(pre) else s

    defined = {plain(s) for s in defined}
    undefined = {plain(s) for s in undefined} - defined
    var = variadics([Path(a.headers), Path(a.headers).parent / "src"])
    RENAMED = {"printf": "ps2_printf", "sprintf": "ps2_sprintf"}   # game_flags.txt renames these two
    var |= set(RENAMED.values())

    funcs: set[str] = set()
    variad: set[str] = set()
    data: dict[str, int] = {}
    external: set[str] = set()
    for s in undefined:
        if s.startswith(MANGLE):
            funcs.add(s[len(MANGLE):])
        elif s in var:
            variad.add(s)
        elif RUNTIME.match(s):
            external.add(s)
        else:
            data[s] = a.data_size
    for line in Path(a.names).read_text().splitlines():
        line = line.split("#", 1)[0].split()
        if not line:
            continue
        name = RENAMED.get(line[0], line[0])
        if len(line) >= 3 and line[1] == "data":
            if name not in defined:
                data.setdefault(name, int(line[2], 0))
        elif len(line) >= 2 and line[1] == "...":
            if name not in defined:
                variad.add(name)
        elif MANGLE + name not in defined:
            funcs.add(name)
    # include/pal.h is the one list of the hardware hooks: a Pal_ prototype per line
    pal = Path(a.headers) / "pal.h"
    if pal.exists():
        for m in re.finditer(r"^[A-Za-z0-9_ *]+?[ *](Pal_[A-Za-z0-9_]+)[(]", pal.read_text(), re.M):
            funcs.add(m.group(1))
    funcs = {f for f in funcs if MANGLE + f not in defined}
    variad -= defined

    # The C names are free (a symbol can be wanted both as a function and as a variadic one);
    # the symbols are named with asm labels, the objects' own spelling.
    root = Path(a.root) if a.root else Path(a.headers).resolve().parent
    # The guard: no stub stands in for a function the game's C defines (see the module docstring)
    mine = game_functions(objs, root)
    clash = sorted((f, mine[f]) for f in funcs | variad | set(data) if f in mine)
    if clash:
        for f, where in clash:
            print(f"gen_stubs: error: {f} would be an aborting stub, but the game defines it ({where}); "
                  f"a plain inline emits no definition: declare it extern after its definition under PLATFORM_PC",
                  file=sys.stderr)
        sys.exit(1)
    calls, by_addr = address_calls(funcs, defined, root, root / "pc/core/pal_overlay.c", pre)
    funcs -= by_addr
    lines = ["/* Generated by tools/pc/gen_stubs.py. Do not edit. */",
             '#include "pal_ovcall.h"',
             "void Pal_StubAbort(const char *name);", ""] + calls + [""]
    for i, f in enumerate(sorted(funcs)):
        lines.append(f'void StubF{i}(void) __asm__("{pre}{MANGLE}{f}");')
        lines.append(f'void StubF{i}(void) {{ Pal_StubAbort("{f}"); }}')
    for i, v in enumerate(sorted(variad)):
        lines.append(f'int StubV{i}(const char *fmt, ...) __asm__("{pre}{v}");')
        lines.append(f'int StubV{i}(const char *fmt, ...) {{ (void)fmt; Pal_StubAbort("{v}"); return 0; }}')
    for i, (d, n) in enumerate(sorted(data.items())):
        lines.append(f'unsigned char StubD{i}[{n}] __asm__("{pre}{d}") __attribute__((aligned(16)));')
    Path(a.out).write_text("\n".join(lines) + "\n")
    msg = f"gen_stubs: {len(funcs)} function stubs, {len(by_addr)} calls by address, {len(variad)} variadic, {len(data)} data (pending), {len(external)} runtime names left to the linker"
    print(msg)
    if a.report or data:
        for d in sorted(data):
            print(f"  data: {d}")


if __name__ == "__main__":
    main()
