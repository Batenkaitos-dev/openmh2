#!/usr/bin/env python3
"""bpcheck: no function of the core may address its frame through a base pointer.

    python3 tools/pc/bpcheck.py BUILD_DIR [--objdump LLVM_OBJDUMP] [-j N] [-v]

On i686, LLVM gives a function a base pointer, ESI, when its frame is
realigned (a local whose alignment is over the stack's: on Windows, whose
stack is 4-aligned at entry, any 8- or 16-aligned local) and it also has a
variable-sized part (a VLA, alloca, AddressSanitizer's fake stack) or an
opaque stack adjustment. The prologue is then

    push %ebp; mov %esp,%ebp; ...; and $-N,%esp; sub $M,%esp; mov %esp,%esi

and the frame's fixed objects are addressed through ESI. The core's code is
built with regcall as the default convention (pc/game_flags.txt), and i686
regcall passes the fifth integer argument in ESI. clang 23 doesn't protect
the base pointer from that: a call with five integer arguments loads ESI and
then reads further arguments or spills through it, and a function whose own
fifth argument arrives in ESI loses it to the prologue (the base-pointer
miscompile). The result is wrong code,
silently.

So this check disassembles every object of a core build directory (*.o,
*.obj) with llvm-objdump and fails if any function sets up a base pointer
(the realigning prologue followed by mov %esp,%esi), naming each one and the
instructions after it that write ESI. Run by tools/pc/build.py --check on both
targets. Exit 0 with one summary line when there is none.
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

FUNC = re.compile(r'^(?:[0-9a-f]+ )?<(.+)>:$')
RELOC = re.compile(r'\b(R_386_\w+|IMAGE_REL_I386_\w+)\s+(\S+)')
REALIGN = re.compile(r'^andl?\s+\$(-0x[0-9a-f]+|0xf+[0-9a-f]*0),\s*%esp$')
# the rest of the prologue after the realignment: the frame's allocation, a stack probe (__chkstk, _alloca)
# (on Windows: push %eax; mov $N,%eax; call __alloca or ___chkstk_ms; mov N(%esp),%eax, as regcall passes
# an argument in EAX)
FRAME_SETUP = re.compile(r'^(subl?\s+\$\S+,\s*%esp|movl?\s+\$\S+,\s*%eax|calll?\s.*(chkstk|alloca).*|subl?\s+%eax,\s*%esp'
                         r'|orl?\s+\$0x0,\s*\(%esp\)|pushl?\s+%eax|movl?\s+(0x[0-9a-f]+)?\(%esp\),\s*%eax)$')
SETS_BP = re.compile(r'^movl?\s+%esp,\s*%esi$')
# a variable change of ESP in the body: a dynamic alloca or an opaque adjustment (not the pushes and
# immediate adjustments around calls, nor the epilogue's lea -N(%ebp),%esp / mov %ebp,%esp)
WRITES_ESP = re.compile(r'^(\w+)\s+(.*),\s*%esp$')
# an instruction whose destination is ESI (AT&T: the last operand), or that exchanges it
WRITES_ESI = re.compile(r'^(?!(push|cmp|test|bt))\w+\s+(.*,\s*)?%esi$|^xchg\w*\s.*%esi')


def dynamic_esp(ins: str) -> bool:
    if re.match(r'^calll?\s.*(chkstk|alloca)', ins):
        return True
    m = WRITES_ESP.match(ins)
    if not m:
        return False
    op, src = m.group(1), m.group(2).strip()
    if op.startswith(('sub', 'add')) and src.startswith('$'):
        return False
    if op.startswith('lea') and src.endswith('(%ebp)'):
        return False
    if op.startswith('mov') and src == '%ebp':
        return False
    return True


def scan_text(text: str):
    """[(function, sets ESI from ESP after the frame setup, [variable ESP changes], [ESI writes])]: the
    functions of one disassembly that realign their frame and have a variable part or set up ESI as
    the base pointer."""
    found = []
    name, insns = None, []

    def flush():
        if name is None:
            return
        r = next((i for i, ins in enumerate(insns) if REALIGN.match(ins)), None)
        if r is None:
            return
        j = r + 1
        while j < len(insns) and FRAME_SETUP.match(insns[j]):
            j += 1
        bp = j < len(insns) and bool(SETS_BP.match(insns[j]))
        body = insns[j + 1 if bp else j:]
        dyn = [ins for ins in body if dynamic_esp(ins)]
        # LLVM's condition (X86RegisterInfo::hasBasePointer): a realigned frame with a variable part. A
        # mov %esp,%esi alone is not one: ESI is also an ordinary register holding a local's address.
        if dyn:
            writes = [ins for ins in body if WRITES_ESI.match(ins) and not ins.startswith('pop')]
            found.append((name, bp, dyn, writes))

    for line in text.splitlines():
        s = line.strip()
        m = FUNC.match(s)
        if m:
            flush()
            name, insns = m.group(1), []
            continue
        if not s or name is None:
            continue
        m = RELOC.search(s)
        if m:                     # a relocation (llvm-objdump -r): the call's or operand's symbol
            if insns:
                insns[-1] += ' <' + m.group(2) + '>'
            continue
        s = re.sub(r'\s+#.*$', '', s)
        insns.append(re.sub(r'\s+', ' ', s))
    flush()
    return found


def objdump_path(given: str | None) -> str:
    if given:
        return given
    for n in ('llvm-objdump', 'llvm-objdump.exe'):
        p = shutil.which(n)
        if p:
            return p
    sys.exit('bpcheck: no llvm-objdump on PATH (give --objdump)')


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('build_dir')
    ap.add_argument('--objdump')
    ap.add_argument('-j', type=int, default=os.cpu_count() or 4)
    ap.add_argument('-v', action='store_true', help='also print the number of objects and realigning functions')
    a = ap.parse_args(argv)
    od = objdump_path(a.objdump)
    objs = sorted(p for p in Path(a.build_dir).rglob('*') if p.suffix in ('.o', '.obj') and p.is_file())
    if not objs:
        sys.exit(f'bpcheck: no objects under {a.build_dir}')

    def one(p: Path):
        r = subprocess.run([od, '-d', '-r', '--no-show-raw-insn', '--no-leading-addr', str(p)],
                           capture_output=True, text=True, errors='replace')
        if r.returncode:
            return p, None, r.stderr.strip()[:300]
        nre = sum(1 for ln in r.stdout.splitlines() if REALIGN.match(re.sub(r'\s+', ' ', ln.strip())))
        return p, scan_text(r.stdout), nre

    bad = []
    nrealign = 0
    with ThreadPoolExecutor(max_workers=a.j) as ex:
        for p, found, info in ex.map(one, objs):
            if found is None:
                print(f'bpcheck: llvm-objdump failed on {p}: {info}')
                return 2
            nrealign += info
            for name, bp, dyn, writes in found:
                bad.append((p, name, bp, dyn, writes))
    for p, name, bp, dyn, writes in bad:
        w = '; '.join(writes[:4]) + (' ...' if len(writes) > 4 else '')
        d = '; '.join(dyn[:2])
        print(f'bpcheck: BASE POINTER {name} in {p}: realigned, {"mov %esp,%esi after the prologue, " if bp else ""}'
              f'{len(dyn)} variable ESP changes ({d}), {len(writes)} writes of ESI{": " + w if w else ""}')
    if bad:
        print(f'bpcheck: FAILED: {len(bad)} functions address their frame through ESI, which regcall '
              f'also passes arguments in (tools/pc/bpcheck.py)')
        return 1
    print(f'bpcheck: OK ({len(objs)} objects, {nrealign} realigned frames, no base pointer)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
