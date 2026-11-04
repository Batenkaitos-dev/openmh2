#!/usr/bin/env python3
"""VU1's microprograms as C, from the decomp's own source (a build step of the graphics front).

    python3 pc/core/gfx/vu1gen.py SRC.vsm OUT.c

Assembles SRC with tools/vu1as.py (our assembler) into the block's bytes,
finds the DMA packets of microcode in them as tools/pc/vu1rec.py finds
them in the game's files, and writes vu1rec's C for them (vu1rec's
generate(): the same compiler as the original game's). The port so builds
VU1 from source.

It fails (exit 1) if the microcode uses an EFU operation the core does not
compute (ESIN, EATAN, EATANxy, EATANxz, EEXP: the original game takes them from
the host's libm, which the core can't match bit for bit; the game's microcode
uses ERSQRT and ERLENG only; the n2 review's ruling).

The output is derived from the game's microcode: it stays under build/.
"""
from __future__ import annotations

import importlib.util
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


# The EFU operations the core counts as anomalies (gfx_vu1.c vu1_efu_calc).
UNUSED_EFU = {0x74: "EATANxy", 0x75: "EATANxz", 0x7C: "ESIN", 0x7D: "EATAN", 0x7E: "EEXP"}


def unused_efu(vu1rec, packets) -> list[str]:
    """Each use of an operation of UNUSED_EFU in the packets' code: 'NAME at WHERE'."""
    found = []
    for parts in packets:
        for _addr, code, where in parts:
            for k in range(0, len(code), 8):
                lo, up = struct.unpack_from("<II", code, k)
                d = vu1rec.decode(lo, up)
                if d["lk"] == vu1rec.LK["EFU"] and d["limm"] in UNUSED_EFU:
                    found.append(f"{UNUSED_EFU[d['limm']]} at {where}+0x{k:X}")
    return found


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__.strip().splitlines()[2].strip(), file=sys.stderr)
        return 2
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    out.parent.mkdir(parents=True, exist_ok=True)
    asm, binary = out.with_suffix(".s"), out.with_suffix(".bin")
    subprocess.run([sys.executable, str(ROOT / "tools" / "vu1as.py"), str(src), str(asm), "--bin", str(binary)],
                   check=True, stdout=subprocess.DEVNULL)
    vu1rec = load("vu1rec", ROOT / "tools" / "pc" / "vu1rec.py")
    packets = vu1rec.find_packets(binary.read_bytes(), src.name)
    if not packets:
        print(f"vu1gen: no microcode packets in {src}", file=sys.stderr)
        return 1
    bad = unused_efu(vu1rec, packets)
    if bad:
        print(f"vu1gen: {src} uses EFU operations the core does not compute: {', '.join(bad)}", file=sys.stderr)
        return 1
    text, nunits, npairs, nstates = vu1rec.generate(packets)
    if not out.exists() or out.read_text(encoding="utf-8") != text:
        out.write_text(text, encoding="utf-8", newline="\n")
    nup = sum(len(p) for p in packets)
    print(f"vu1gen: {nup} uploads in {len(packets)} packets, {nunits} distinct units, "
          f"{npairs} distinct pairs, {nstates} compiled states -> {out.name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
