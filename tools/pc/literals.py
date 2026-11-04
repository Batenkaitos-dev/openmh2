#!/usr/bin/env python3
"""literals: the game's string literals stay apart, as MWCC and the PS2's linker keep them.

The rule: each
module keeps its own literals, so that a literal's address names one PS2
object. What the matching build does (its objects' anonymous @N strings):

- MWCC pools equal literals within one source file: one @N however often
  the text repeats them (lobby_dialog_box.c's "%s", nine times, is one).
  clang's front end does the same (one @.str per text in a unit).
- The PS2's linker merges nothing: an equal text in two files is two
  objects (main: 7 texts in more than one file, lobby 20), and so in two
  modules. clang marks its strings unnamed_addr, so LLVM puts them in
  mergeable sections (.rodata.str1.1, SHF_MERGE) and GNU ld would merge
  equal ones (and tails) across every object of the core.
- `#pragma dont_reuse_strings on` ... `reset` (MWCC; clang ignores it):
  every literal in between is its own object, not pooled with an equal one
  (src/lobby/inflate.c around inflate_fast: "invalid distance code" and
  "invalid literal/length code" are two objects each in inflate.o).

The rewrite of clang's unoptimised IR, run by ps2fp/ps2fpcc.py for the
game's files after its pass and stackalign:

- every private unnamed_addr C string (`[N x i8] c"...\\00"`, or
  zeroinitializer for "", and the initialisers of local char arrays,
  @__const.*) loses unnamed_addr: LLVM then places it in plain .rodata
  (.rdata on Windows), which no linker merges, and the optimiser's constant
  merging leaves it;
- what the optimiser makes later (a switch's lookup table that ends in a
  zero lands in a .rodata.strN section too) is no literal of the game and
  is left alone;
- in a function defined between a dont_reuse_strings pragma and its reset
  in the source file, each use of a string gets a copy of its own.

rewrite(text, source) -> (text, (strings, copies)).
"""
from __future__ import annotations

import re

STR = re.compile(r'^(@[-\w.$]+|@"[^"]*") = (private|internal) unnamed_addr constant (\[\d+ x i8\] (?:c".*\\00"|zeroinitializer))(.*)$')
DEFINE = re.compile(r'^define\b.*?@("[^"]*"|[-\w.$]+)\(')
PRAGMA = re.compile(r'^\s*#\s*pragma\s+dont_reuse_strings\s+(\w+)')
# a function definition's first line in C: a name and "(" at the start of a line, not a statement
CDEF = re.compile(r'^[A-Za-z_][\w \t\*]*?\b([A-Za-z_]\w*)\s*\(')
CKEYWORDS = {"if", "for", "while", "switch", "return", "sizeof", "do", "else"}


def no_reuse_functions(source_text: str) -> set[str]:
    """The names of the functions defined (their first line) where dont_reuse_strings is on."""
    names: set[str] = set()
    on = False
    for line in source_text.splitlines():
        m = PRAGMA.match(line)
        if m:
            on = m.group(1) == "on"
            continue
        if on:
            d = CDEF.match(line)
            if d and d.group(1) not in CKEYWORDS and not line.rstrip().endswith(";"):
                names.add(d.group(1))
    return names


def rewrite(text: str, source_text: str = "") -> tuple[str, tuple[int, int]]:
    lines = text.split("\n")
    strings: dict[str, str] = {}      # name -> its definition's rest (type and initializer)
    for i, ln in enumerate(lines):
        m = STR.match(ln)
        if m:
            name, link, init, tail = m.groups()
            lines[i] = f"{name} = {link} constant {init}{tail}"
            strings[name] = f"{link} constant {init}{tail}"
    copies: list[str] = []
    no_reuse = no_reuse_functions(source_text) if "dont_reuse_strings" in source_text else set()
    if no_reuse and strings:
        use = re.compile("(" + "|".join(re.escape(n) for n in sorted(strings, key=len, reverse=True)) + r")(?![-\w.$])")
        func = None
        for i, ln in enumerate(lines):
            if ln.startswith("define"):
                m = DEFINE.match(ln)
                func = m.group(1).strip('"') if m else None
                func = func[len("__regcall3__"):] if func and func.startswith("__regcall3__") else func
                continue
            if ln.startswith("}"):
                func = None
                continue
            if func in no_reuse and "@" in ln:
                def repl(m):
                    base = m.group(1)
                    new = f'{base[:-1]}.nr{len(copies)}"' if base.endswith('"') else f"{base}.nr{len(copies)}"
                    copies.append(f"{new} = {strings[base]}")
                    return new
                lines[i] = use.sub(repl, ln)
    if copies:
        # after the last global definition (before the first function), so they read as the others
        at = next((i for i, ln in enumerate(lines) if ln.startswith("define")), len(lines))
        lines[at:at] = copies + [""]
    return "\n".join(lines), (len(strings), len(copies))


if __name__ == "__main__":
    import sys
    if len(sys.argv) not in (3, 4):
        sys.stderr.write("usage: literals.py IN.ll OUT.ll [SOURCE.c]\n")
        sys.exit(2)
    src = open(sys.argv[3], encoding="latin-1").read() if len(sys.argv) == 4 else ""
    out, (n, c) = rewrite(open(sys.argv[1], encoding="utf-8", errors="surrogateescape").read(), src)
    open(sys.argv[2], "w", encoding="utf-8", errors="surrogateescape", newline="\n").write(out)
    print(f"literals: {n} strings, {c} copies")
