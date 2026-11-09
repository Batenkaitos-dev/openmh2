#!/usr/bin/env python3
"""ps2fpcc: compiles a C file through the ps2fp pass.

Used as a compiler launcher (CMake: CMAKE_C_COMPILER_LAUNCHER, or the
target property C_COMPILER_LAUNCHER), so it is given the compiler first:

    ps2fpcc.py CLANG [clang options] -c FILE.c -o FILE.o

1. CLANG compiles FILE.c to LLVM IR with its optimiser off
   (-Xclang -disable-llvm-passes), with pc/core/crt/ps2fp.h force-included;
2. ps2fp.py rewrites the IR's float, shift and division operations to calls
   of that header's always_inline functions;
   stackalign.py raises local arrays' alignment, and literals.py keeps the
   string literals out of the linker's merging (the game's files only);
   argwidth.py adds its checks when the command has -DMH2_ARGWIDTH_TABLE=
   (build.py --variant argwidth);
3. CLANG compiles the IR with the options of the original command (-O2,
   -g, the target and code generation options), which inlines the helpers.

Anything that is not a single-file compile (a link, a preprocess, a compile
of something other than a .c file) is passed to CLANG unchanged.
PS2FP_KEEP=DIR keeps the intermediate .ll files in DIR.
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ps2fp  # noqa: E402
sys.path.insert(0, os.path.dirname(HERE))
import stackalign  # noqa: E402  (tools/pc/stackalign.py: local arrays of 16 bytes or more get 16-byte alignment)
import literals  # noqa: E402  (tools/pc/literals.py: the game's string literals are never merged)
import argwidth  # noqa: E402  (tools/pc/argwidth.py: the argument widths' verification build)

HEADER = os.path.normpath(os.path.join(HERE, "..", "..", "..", "pc", "core", "crt", "ps2fp.h"))

# options that need a value in the next argument
TAKES_VALUE = {"-o", "-MF", "-MT", "-MQ", "-I", "-isystem", "-iquote", "-D", "-include", "-x", "-target", "-Xclang",
               "-Xlinker", "-imacros", "-idirafter", "-isysroot", "-MJ", "-working-directory"}


def stage2_args(args):
    """The options of the original command that still matter for compiling IR."""
    out = []
    i = 0
    while i < len(args):
        a = args[i]
        if a in ("-c", "-S", "-E", "-MD", "-MMD", "-M", "-MM", "-MP"):
            i += 1
        elif a in ("-o", "-MF", "-MT", "-MQ", "-I", "-isystem", "-iquote", "-D", "-include", "-x", "-imacros",
                   "-idirafter", "-isysroot", "-MJ", "-working-directory"):
            i += 2
        elif a == "-target":
            out += [a, args[i + 1]]
            i += 2
        elif a == "-Xclang":
            out += [a, args[i + 1]]    # codegen options travel with the command
            i += 2
        elif a[:2] in ("-I", "-D") or a.startswith(("-std=", "-W", "-w", "-ferror-limit", "-isystem", "-include")):
            i += 1
        elif a.endswith((".c", ".cpp", ".cc")):
            i += 1
        else:
            out.append(a)
            i += 1
    return out


def main(argv):
    if len(argv) < 2:
        sys.stderr.write(__doc__)
        return 2
    cc, args = argv[1], argv[2:]
    src = out = None
    i = 0
    srcs = []
    while i < len(args):
        a = args[i]
        if a == "-o":
            out = args[i + 1]
            i += 2
        elif a in TAKES_VALUE:
            i += 2
        elif a.startswith("-"):
            i += 1
        else:
            srcs.append(a)
            i += 1
    if "-c" not in args or len(srcs) != 1 or not srcs[0].endswith(".c") or out is None or "-E" in args or "-S" in args:
        return subprocess.call([cc] + args)
    src = srcs[0]
    keep = os.environ.get("PS2FP_KEEP")
    tmp = keep if keep else tempfile.mkdtemp(prefix="ps2fp-")
    os.makedirs(tmp, exist_ok=True)
    base = os.path.join(tmp, os.path.basename(src).replace(os.sep, "_"))
    ll1, ll2 = base + ".ll", base + ".ps2fp.ll"
    try:
        # 1. unoptimised IR (clang emits its own dependency file here)
        a1 = [a for a in args if a != "-c"]
        j = 0
        a1b = []
        while j < len(a1):
            if a1[j] == "-o":
                j += 2
            else:
                a1b.append(a1[j])
                j += 1
        r = subprocess.call([cc] + a1b + ["-include", HEADER, "-Xclang", "-disable-llvm-passes", "-S", "-emit-llvm",
                                         "-o", ll1])
        if r:
            return r
        # 2. the pass
        with open(ll1, encoding="utf-8", errors="surrogateescape") as f:
            text = f.read()
        trace = os.environ.get("PS2FP_SITES") if "-DMH2_FPTRACE=1" in args else None   # the trace build
        rw = ps2fp.Rewriter(text, src, order=ps2fp.read_order(), trace=trace)
        res = rw.run()
        if trace and rw.sites:
            with open(trace, "a", encoding="utf-8") as f:
                f.write("".join(s + "\n" for s in rw.sites))
        if rw.errors:
            for e in rw.errors[:50]:
                sys.stderr.write("ps2fp: " + e + "\n")
            return 1
        if stackalign.GAME_FLAG in args:
            try:
                res, _ = stackalign.rewrite(res)
            except (ValueError, KeyError, AssertionError, IndexError) as e:
                sys.stderr.write(f"stackalign: {src}: {e}\n")
                return 1
        # the game's string literals stay apart, as on the PS2 (tools/pc/literals.py)
        with open(src, encoding="latin-1") as f:
            res, _ = literals.rewrite(res, f.read())
        table = next((a[len(argwidth.TABLE_FLAG):] for a in args if a.startswith(argwidth.TABLE_FLAG)), None)
        if table:
            unit = os.path.relpath(os.path.abspath(src), os.path.join(HERE, "..", "..", ".."))
            res, _ = argwidth.rewrite(res, table, unit.replace(os.sep, "/"))
        with open(ll2, "w", encoding="utf-8", errors="surrogateescape", newline="\n") as f:
            f.write(res)
        # 3. code generation
        return subprocess.call([cc, "-Wno-override-module", "-Wno-unused-command-line-argument"] + stage2_args(args) +
                               ["-c", ll2, "-o", out])
    finally:
        if not keep:
            for p in (ll1, ll2):
                if os.path.exists(p):
                    os.remove(p)
            try:
                os.rmdir(tmp)
            except OSError:
                pass


if __name__ == "__main__":
    sys.exit(main(sys.argv))
