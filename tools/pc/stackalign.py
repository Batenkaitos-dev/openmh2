#!/usr/bin/env python3
"""stackalign: local arrays of 16 bytes or more, and variable-length arrays, get 16-byte alignment.

The rule is the AMD64 psABI's (3.1.2, "Aggregates and Unions"): an array
variable of 16 bytes or more, and every C99 variable-length array, is at
least 16-byte aligned. The original game is an x86-64 program and has it; clang
gives it only on x86-64 (TargetInfo's large-array alignment), not on i686.
The core's code built with the game's flags gets the same rule for its
locals here, on both targets. Globals are not touched: tools/pc/data_c.py lays them out at their
PS2 offsets.

It is a rewrite of clang's unoptimised IR (clang -Xclang -disable-llvm-passes
-S -emit-llvm), at the start of the optimisation pipeline:

- a static alloca of an array type of 16 bytes or more (`alloca [N x T]`, the
  form clang gives an array variable) gets `align 16` if its alignment was
  lower;
- a dynamic alloca (a count that is not a constant: a VLA, alloca()) gets
  `align 16` too.

Array temporaries clang makes itself (an array compound literal) get it as
well: clang's discarded value names don't tell them apart, and a larger
alignment than the original's is never observable as a different value.

Two uses:
- as a module: rewrite(text) -> (text, count), called by ps2fp/ps2fpcc.py
  after its own pass, for the game's C;
- as a compiler launcher for the platform layer's C (pc/cmake: the
  toolchain files set CMAKE_C_COMPILER_LAUNCHER for the core):

      stackalign.py CLANG [clang options] -c FILE.c -o FILE.o

  A compile that carries the game's flags (-fdefault-calling-conv=regcall,
  pc/game_flags.txt) goes through IR; anything else (the host-facing
  objects, links, other languages) is passed to CLANG unchanged.
  STACKALIGN_KEEP=DIR keeps the .ll files.
"""
from __future__ import annotations

import os
import re
import subprocess
import sys
import tempfile

MIN_SIZE = 16
ALIGN = 16

ALLOCA = re.compile(r'^(\s*%[-\w.$]+ = alloca )(.*?)(, align )(\d+)(.*)$')
NAMED = re.compile(r'^(%[-\w.$]+|%"[^"]*") = type (.*)$')
DL = re.compile(r'^target datalayout = "([^"]*)"')


class Layout:
    """Sizes and ABI alignments of IR types under a module's data layout."""

    def __init__(self, dl: str, named: dict[str, str]):
        self.ptr = (4, 4)
        self.ints = {1: 1, 8: 1, 16: 2, 32: 4, 64: 4, 128: 16}
        self.floats = {16: 2, 32: 4, 64: 8, 80: 4, 128: 16}
        self.vecs = {64: 8, 128: 16}
        for spec in dl.split('-'):
            m = re.match(r'^p(\d*):(\d+):(\d+)', spec)
            if m and m.group(1) in ('', '0'):
                self.ptr = (int(m.group(2)) // 8, int(m.group(3)) // 8)
                continue
            m = re.match(r'^([ifv])(\d+):(\d+)', spec)
            if m:
                tbl = {'i': self.ints, 'f': self.floats, 'v': self.vecs}[m.group(1)]
                tbl[int(m.group(2))] = int(m.group(3)) // 8
        self.named = named
        self.cache: dict[str, tuple[int, int]] = {}

    def int_align(self, bits: int) -> int:
        if bits in self.ints:
            return self.ints[bits]
        bigger = [b for b in self.ints if b >= bits]
        return self.ints[min(bigger)] if bigger else self.ints[max(self.ints)]

    def parse(self, s: str, i: int = 0):
        """(size, align, end) of the type starting at s[i]."""
        while s[i] == ' ':
            i += 1
        c = s[i]
        if c == '[' or (c == '<' and s[i + 1] != '{'):
            close = ']' if c == '[' else '>'
            m = re.compile(r'\s*(\d+) x ').match(s, i + 1)
            n = int(m.group(1))
            size, align, j = self.parse(s, m.end())
            while s[j] == ' ':
                j += 1
            assert s[j] == close, s
            if c == '[':
                return size * n, align, j + 1
            total = size * n
            va = self.vecs.get(total * 8, 1)
            while va < total and va < 16 and total * 8 not in self.vecs:
                va *= 2
            return (total + va - 1) // va * va, va, j + 1
        if c == '{' or s.startswith('<{', i):
            packed = c == '<'
            j = i + (2 if packed else 1)
            size, align = 0, 1
            while True:
                while s[j] == ' ':
                    j += 1
                if s[j] == '}':
                    j += 1
                    break
                fs, fa, j = self.parse(s, j)
                if packed:
                    fa = 1
                size = (size + fa - 1) // fa * fa + fs
                align = max(align, fa)
                while s[j] == ' ':
                    j += 1
                if s[j] == ',':
                    j += 1
            if packed:
                assert s[j] == '>', s
                j += 1
                align = 1
            return (size + align - 1) // align * align, align, j
        m = re.compile(r'%[-\w.$]+|%"[^"]*"').match(s, i)
        if m:
            name = m.group(0)
            if name not in self.cache:
                body = self.named[name]
                if body.strip() == 'opaque':
                    raise ValueError(f'opaque type {name}')
                sz, al, _ = self.parse(body, 0)
                self.cache[name] = (sz, al)
            sz, al = self.cache[name]
            return sz, al, m.end()
        m = re.compile(r'i(\d+)\b').match(s, i)
        if m:
            bits = int(m.group(1))
            al = self.int_align(bits)
            sz = (bits + 7) // 8
            return (sz + al - 1) // al * al, al, m.end()
        for name, bits in (('half', 16), ('bfloat', 16), ('float', 32), ('double', 64), ('x86_fp80', 80),
                           ('fp128', 128), ('ppc_fp128', 128)):
            if s.startswith(name, i) and not s[i + len(name):i + len(name) + 1].isalnum():
                al = self.floats.get(bits, 16)
                sz = 12 if (bits == 80 and al == 4) else 16 if bits == 80 else bits // 8
                return (sz + al - 1) // al * al, al, i + len(name)
        if s.startswith('ptr', i):
            j = i + 3
            m = re.compile(r' addrspace\(\d+\)').match(s, j)
            if m:
                j = m.end()
            return self.ptr[0], self.ptr[1], j
        raise ValueError(f'unknown IR type at {s[i:i + 40]!r}')

    def size(self, ty: str) -> int:
        return self.parse(ty.strip(), 0)[0]


def split_alloca_operands(rest: str):
    """'TYPE' or 'TYPE, iN COUNT' (inalloca/addrspace forms are left alone): (type, count or None)."""
    depth = 0
    for k, ch in enumerate(rest):
        if ch in '[{<(':
            depth += 1
        elif ch in ']}>)':
            depth -= 1
        elif ch == ',' and depth == 0:
            return rest[:k], rest[k + 1:].strip()
    return rest, None


def rewrite(text: str) -> tuple[str, int]:
    """The IR with the rule applied, and the number of allocas changed."""
    lines = text.split('\n')
    dl = ''
    named: dict[str, str] = {}
    for ln in lines:
        m = DL.match(ln)
        if m:
            dl = m.group(1)
            continue
        m = NAMED.match(ln)
        if m:
            named[m.group(1)] = m.group(2)
        elif ln.startswith('define '):
            break
    lay = Layout(dl, named)
    n = 0
    for k, ln in enumerate(lines):
        if ' = alloca ' not in ln:
            continue
        m = ALLOCA.match(ln)
        if not m or int(m.group(4)) >= ALIGN:
            continue
        operands = m.group(2)
        if operands.startswith('inalloca ') or 'addrspace' in operands:
            continue
        ty, count = split_alloca_operands(operands)
        if count is not None:
            cm = re.match(r'^i\d+ (-?\d+)$', count)
            if cm is None:          # a dynamic count: a VLA or alloca()
                bump = True
            else:
                bump = ty.strip().startswith('[') and lay.size(ty) * int(cm.group(1)) >= MIN_SIZE
        else:
            bump = ty.strip().startswith('[') and lay.size(ty) >= MIN_SIZE
        if bump:
            lines[k] = f'{m.group(1)}{m.group(2)}{m.group(3)}{ALIGN}{m.group(5)}'
            n += 1
    return '\n'.join(lines), n


# ---------------------------------------------------------------------------------------------
# the launcher
GAME_FLAG = '-fdefault-calling-conv=regcall'
SKIP_STAGE2 = {'-c', '-S', '-E', '-MD', '-MMD', '-M', '-MM', '-MP'}
TAKES_VALUE = {'-o', '-MF', '-MT', '-MQ', '-I', '-isystem', '-iquote', '-D', '-U', '-include', '-x', '-target',
               '-Xclang', '-Xlinker', '-imacros', '-idirafter', '-isysroot', '-MJ', '-working-directory'}


def stage2_args(args):
    """The options of the original command that still matter for compiling IR."""
    out = []
    i = 0
    while i < len(args):
        a = args[i]
        if a in SKIP_STAGE2:
            i += 1
        elif a in ('-o', '-MF', '-MT', '-MQ', '-I', '-isystem', '-iquote', '-D', '-U', '-include', '-x', '-imacros',
                   '-idirafter', '-isysroot', '-MJ', '-working-directory'):
            i += 2
        elif a in ('-target', '-Xclang'):
            out += [a, args[i + 1]]
            i += 2
        elif a[:2] in ('-I', '-D', '-U') or a.startswith(('-std=', '-W', '-w', '-ferror-limit', '-isystem',
                                                          '-include')):
            i += 1
        elif a.endswith(('.c', '.cpp', '.cc')):
            i += 1
        else:
            out.append(a)
            i += 1
    return out


def main(argv) -> int:
    if len(argv) < 2:
        sys.stderr.write(__doc__)
        return 2
    cc, args = argv[1], argv[2:]
    out = None
    srcs = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == '-o':
            out = args[i + 1]
            i += 2
        elif a in TAKES_VALUE:
            i += 2
        elif a.startswith('-'):
            i += 1
        else:
            srcs.append(a)
            i += 1
    if ('-c' not in args or len(srcs) != 1 or not srcs[0].endswith('.c') or out is None or '-E' in args
            or '-S' in args or GAME_FLAG not in args):
        return subprocess.call([cc] + args)
    src = srcs[0]
    keep = os.environ.get('STACKALIGN_KEEP')
    tmp = keep if keep else tempfile.mkdtemp(prefix='stackalign-')
    os.makedirs(tmp, exist_ok=True)
    base = os.path.join(tmp, os.path.basename(src))
    ll1, ll2 = base + '.ll', base + '.sa.ll'
    try:
        a1 = []
        j = 0
        while j < len(args):
            if args[j] == '-o':
                j += 2
            elif args[j] == '-c':
                j += 1
            else:
                a1.append(args[j])
                j += 1
        r = subprocess.call([cc] + a1 + ['-Xclang', '-disable-llvm-passes', '-S', '-emit-llvm', '-o', ll1])
        if r:
            return r
        with open(ll1, encoding='utf-8', errors='surrogateescape') as f:
            text = f.read()
        try:
            res, _ = rewrite(text)
        except (ValueError, KeyError, AssertionError, IndexError) as e:
            sys.stderr.write(f'stackalign: {src}: {e}\n')
            return 1
        with open(ll2, 'w', encoding='utf-8', errors='surrogateescape', newline='\n') as f:
            f.write(res)
        return subprocess.call([cc, '-Wno-override-module', '-Wno-unused-command-line-argument'] + stage2_args(args)
                               + ['-c', ll2, '-o', out])
    finally:
        if not keep:
            for p in (ll1, ll2):
                if os.path.exists(p):
                    os.remove(p)
            try:
                os.rmdir(tmp)
            except OSError:
                pass


if __name__ == '__main__':
    sys.exit(main(sys.argv))
