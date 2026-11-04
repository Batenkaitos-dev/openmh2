#!/usr/bin/env python3
"""Fails if the objects given contain x87 instructions (fld, fst, fadd, ...): the game's convention keeps floats in SSE registers."""
import re
import shutil
import subprocess
import sys

tool = shutil.which("llvm-objdump") or shutil.which("objdump")
if not tool:
    sys.exit("no_x87: neither llvm-objdump nor objdump is installed")
bad = 0
for obj in [o for a in sys.argv[1:] for o in a.split(";") if o]:
    out = subprocess.run([tool, "-d", obj], capture_output=True, text=True, check=True).stdout
    hits = [l for l in out.splitlines() if re.search(r"\t(fld|fst|fild|fist|fadd|fsub|fmul|fdiv|fxch|fchs|fabs)\w*\b", l)]
    if hits:
        bad += 1
        print(f"no_x87: {obj}: {len(hits)} x87 instructions, first: {hits[0].strip()}")
print("no_x87: " + ("FAILED" if bad else "OK, no x87 instructions in the test objects"))
sys.exit(1 if bad else 0)
