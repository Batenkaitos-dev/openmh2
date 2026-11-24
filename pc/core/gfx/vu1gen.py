#!/usr/bin/env python3
"""VU1's microprograms as C, from the decomp's own source (a build step of the graphics front).

    python3 pc/core/gfx/vu1gen.py SRC.vsm OUT.c [--extra EXTRA.vsm --array ARRAY.c]

Assembles SRC with tools/vu1as.py (our assembler) into the block's bytes,
finds the DMA packets of microcode in them as tools/pc/vu1rec.py finds
them in the game's files, and writes vu1rec's C for them (vu1rec's
generate(): the same compiler as the original game's). The port so builds
VU1 from source.

It fails (exit 1) if the microcode uses an EFU operation the core does not
compute (ESIN, EATAN, EATANxy, EATANxz, EEXP: the original game takes them from
the host's libm, which the core can't match bit for bit; the game's microcode
uses ERSQRT and ERLENG only; the n2 review's ruling).

With --extra the content core's own microcode (pc/core/content/vu1_content.vsm)
is assembled too and compiled with the game's, so that the content core's VU1
runs it as compiled code like the game's own; with --array its packets are also
written to ARRAY.c as arrays, one for each program, named as the program is
(Vu1Prog60 ...), 16-byte aligned, for the content core's descriptors to point
at. Without --extra the output is the game's alone.

The output is derived from the game's microcode: it stays under build/.
"""
from __future__ import annotations

import importlib.util
import re
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


def packet_arrays(src: Path, blob: bytes) -> str:
    """The DMA packets of blob (the programs of src, in order) as C arrays named by src's .program lines."""
    names = re.findall(r"^\.program\s+(\w+)", src.read_text(encoding="utf-8"), re.M)
    lines = ["/* The content core's VU1 programs: the packets of " + src.name + ", assembled by tools/vu1as.py. Generated; not edited by hand. */", ""]
    pos = 0
    for name in names:
        size = 16 * ((struct.unpack_from("<I", blob, pos)[0] & 0xFFFF) + 1)
        words = struct.unpack_from(f"<{size // 4}I", blob, pos)
        pos += size
        lines.append(f"unsigned int {name}[{size // 4}] __attribute__((aligned(16))) = {{")
        for k in range(0, len(words), 4):
            lines.append("    " + ", ".join(f"0x{w:08X}u" for w in words[k:k + 4]) + ",")
        lines.append("};")
        lines.append(f"const unsigned int {name}_bytes = {size};")
    if pos != len(blob):
        raise SystemExit(f"vu1gen: {src}: the packets end at {pos}, the code is {len(blob)} bytes")
    return "\n".join(lines) + "\n"


def main() -> int:
    args = sys.argv[1:]
    extra = array = None
    for opt in ("--extra", "--array"):
        if opt in args:
            k = args.index(opt)
            val = Path(args[k + 1])
            del args[k:k + 2]
            if opt == "--extra":
                extra = val
            else:
                array = val
    if len(args) != 2 or (array and not extra):
        print(__doc__.strip().splitlines()[2].strip(), file=sys.stderr)
        return 2
    src, out = Path(args[0]), Path(args[1])
    out.parent.mkdir(parents=True, exist_ok=True)
    asm, binary = out.with_suffix(".s"), out.with_suffix(".bin")
    subprocess.run([sys.executable, str(ROOT / "tools" / "vu1as.py"), str(src), str(asm), "--bin", str(binary)],
                   check=True, stdout=subprocess.DEVNULL)
    vu1rec = load("vu1rec", ROOT / "tools" / "pc" / "vu1rec.py")
    blob = binary.read_bytes()
    if extra:
        xbin = out.with_name(out.stem + "_extra.bin")
        subprocess.run([sys.executable, str(ROOT / "tools" / "vu1as.py"), str(extra), str(out.with_name(out.stem + "_extra.s")), "--bin", str(xbin)],
                       check=True, stdout=subprocess.DEVNULL)
        xblob = xbin.read_bytes()
        if array:
            text = packet_arrays(extra, xblob)
            array.parent.mkdir(parents=True, exist_ok=True)
            if not array.exists() or array.read_text(encoding="utf-8") != text:
                array.write_text(text, encoding="utf-8", newline="\n")
        blob += xblob
    packets = vu1rec.find_packets(blob, src.name)
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
