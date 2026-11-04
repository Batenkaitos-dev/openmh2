#!/usr/bin/env python3
"""The layout manifest: what the native build takes from MWCC's output,
as text files, so that a tree that never ran the matching build (Windows without WSL) can still make the data
objects of mh2-core.

    python3 tools/pc/layout_manifest.py --write [DIR]   write DIR/layout/*.txt and DIR/sources.txt (default config/pc)
                                                       from the matching build (Linux); prints the sizes
    python3 tools/pc/layout_manifest.py --check         the committed files equal a fresh derivation (mh2-check)
    python3 tools/pc/layout_manifest.py --check-model   Matching built from the matching build and from the
                                                       manifest has the same attributes (every one, in order)
    python3 tools/pc/layout_manifest.py --check-bytes   the bytes data_c.py moves are the user's original's
    python3 tools/pc/layout_manifest.py --stats         lines, bytes and xz size of the committed files

The manifest holds layout and no byte of the game: symbol names, addresses, sizes and kinds of the 13 modules' ELFs;
the C input sections' places; the places of the words that are addresses and their targets (a symbol, an addend);
the names each C object defines and leaves undefined; the splitter's pieces; the blocks' extents. The bytes of the
C data are the user's: data_c.py's Matching reads them from build/orig (the boot executable for main, the
overlay's .bin at its load address for the others), where it read the matching ELF's. --check-bytes proves the two
are equal at every span data_c.py moves.

Files, in config/pc/layout (each under 1 MB; a long kind is in numbered parts, one list in order). The first line
of a file is `@ KIND NAME`; the lines (hex numbers; names have no space):
  objs.txt             O MOD SRC OBJ                        the C objects of the matching build, in ninja's order
  undef.NN.txt         U SRC NAME...                        the names the object leaves undefined (sorted)
  def.NN.txt           D SRC NAME[()]...                    the global names it defines, in symbol order; () a function
                       F NAME...                            functions defined only where another object defines the name
  sym.MOD.NN.txt       Y NAME ADDR SIZE KIND                the module ELF's symbols (F, O, N or STT_*), sorted
                       M NAME SIZE                          a .NON_MATCHING symbol's size; A NAME ADDR: a name twice
                       E LO HI                              the module's .bss section
  pieces.MOD.NN.txt    K NAME [SIZE]                        the splitter's .bss and .data pieces' names; SIZE: .bss
  blocks.txt           B KEY LO HI; P KEY LO-HI...          the blocks' extents and the spans the pieces hold
  holes.txt            H KEY LO-HI...                       the C input sections of the map inside a block
  align.MOD.NN.txt     G ADDR NAME                          the map's names at a 16-aligned address
  cdata.KEY.NN.txt     C                                    the block was scanned (an empty one has only this)
                       S ADDR SIZE SRC INDEX                the C sections moved (MOVE_C)
                       N NAME ADDR SIZE SRC                 the globals they define
                       R ADDR s NAME ADDEND | p ADDR        a word that is an address: a symbol + addend, or a block's
                       W ADDR ...                           the same, inside a file-static object
                       J ADDR FUNC ADDEND SRC HOLDER|-      a word that addresses a code label (a jump table's)
                       T LO HI NAME SRC                     the file-static objects among them
                       L ADDR SIZE NAME                     the textbin pieces laid out (TEXTBIN)
"""
from __future__ import annotations

import argparse
import io
import lzma
import os
import sys
from pathlib import Path

ROOT = Path(os.path.abspath(__file__)).parent.parent.parent
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tools" / "pc"))

import data_c  # noqa: E402
from data_c import MOVE_C, block_kind, block_mod, word_fmt, spans_fmt  # noqa: E402

PART = 700_000          # bytes: a part of a long kind stays well under the export's 1 MB


def derive(m) -> dict:
    """The manifest of Matching M (built from the matching build): {file name: text}."""
    assert m.mode == "mwcc"
    src_of = {obj: src for mod, src, obj in m.objs}
    rev = {v: k for k, v in data_c.SYM_TYPE.items()}
    lists = {}          # (kind, name) -> [line]

    def add(kind, name, line):
        lists.setdefault((kind, name), []).append(line)

    for mod, src, obj in m.objs:
        add("objs", "-", f"O {mod} {src} {obj}")
    by_src = {}
    for n, src in m.cdef.items():
        by_src.setdefault(src, []).append(n)
    for mod, src, obj in m.objs:
        if src in by_src:
            add("def", "-", f"D {src} " + " ".join(n + ("()" if n in m.cfuncs else "") for n in by_src[src]))
    extra = sorted(m.cfuncs - set(m.cdef))
    if extra:
        add("def", "-", "F " + " ".join(extra))
    for mod, src, obj in m.objs:
        if src in m.undef_by_src:
            add("undef", "-", f"U {src} " + " ".join(sorted(m.undef_by_src[src])))
    for mod in m.modules:
        last = {}
        for a, s, n, t in m.elf[mod]:
            add("sym", mod, f"Y {n} {a:X} {s:X} {rev.get(t, t)}")
            last[n] = a
        for n, a in last.items():
            if m.elf_addr[(mod, n)] != a:
                add("sym", mod, f"A {n} {m.elf_addr[(mod, n)]:X}")
        for (mm, n), s in m.nm_size.items():
            if mm == mod:
                add("sym", mod, f"M {n} {s:X}")
        if mod in m.bss_range:
            add("sym", mod, "E %X %X" % m.bss_range[mod])
        for mm, n, sz in m.piece_scan:
            if mm == mod:
                add("pieces", mod, f"K {n}" + ("" if sz is None else f" {sz:X}"))
        for A, n in m.align16[mod]:
            add("align", mod, f"G {A:X} {n}")
    for key, (lo, hi) in m.bss_block.items():
        add("blocks", "-", f"B {key} {lo:X} {hi:X}")
    for key, spans in m.pieces.items():
        add("blocks", "-", f"P {key} {spans_fmt(spans)}".rstrip())
    for key, spans in m.holes.items():
        add("holes", "-", f"H {key} {spans_fmt(spans)}".rstrip())
    for key in MOVE_C:
        if key not in m.csecs:
            continue
        mod = block_mod(key)
        add("cdata", key, "C")          # the block was scanned (its lists may be empty)
        for a, sz, o, i in m.csecs[key]:
            add("cdata", key, f"S {a:X} {sz:X} {src_of[o]} {i}")
        for n, (k, A, sz, src) in m.cnames.items():
            if k == key:
                add("cdata", key, f"N {n} {A:X} {sz:X} {src}")
        for P, w in m.cinit[key][1].items():
            add("cdata", key, f"R {P:X} " + word_fmt(w))
        for P, w in m.static_words[key].items():
            add("cdata", key, f"W {P:X} " + word_fmt(w))
        for P, fn, A, o, holder, v in m.codewords[key]:
            add("cdata", key, f"J {P:X} {fn} {A:X} {src_of[o]} {holder or '-'}")
        for a, b, n, o in m.cstatic[key]:
            add("cdata", key, f"T {a:X} {b:X} {n} {src_of[o]}")
        for a, sz, o in m.textbins[key]:
            pre, post = f"build/{mod}/asm/{mod}/data/", ".o"
            assert o.startswith(pre) and o.endswith(post), o
            add("cdata", key, f"L {a:X} {sz:X} {o[len(pre):-len(post)]}")
    files = {}
    for (kind, name), L in sorted(lists.items()):
        parts, cur, size = [], [], 0
        for l in L:
            if size + len(l) + 1 > PART and cur:
                parts.append(cur)
                cur, size = [], 0
            cur.append(l)
            size += len(l) + 1
        parts.append(cur)
        base = kind if name == "-" else f"{kind}.{name}"
        for i, part in enumerate(parts):
            fn = f"{base}.txt" if len(parts) == 1 else f"{base}.{i + 1:02d}.txt"
            files[fn] = f"@ {kind} {name}\n" + "\n".join(part) + "\n"
    return files


def derive_all() -> dict:
    """The manifest and config/pc/sources.txt: {path relative to config/pc: text}."""
    import sources
    m = data_c.Matching(mode="mwcc")
    out = {f"layout/{k}": v for k, v in derive(m).items()}
    out["sources.txt"] = sources.sources_text()
    return out


def committed() -> dict:
    out = {}
    d = ROOT / "config" / "pc"
    for p in sorted((d / "layout").glob("*.txt")):
        out[f"layout/{p.name}"] = p.read_text()
    if (d / "sources.txt").exists():
        out["sources.txt"] = (d / "sources.txt").read_text()
    return out


def write(dest: Path) -> None:
    files = derive_all()
    (dest / "layout").mkdir(parents=True, exist_ok=True)
    for old in (dest / "layout").glob("*.txt"):
        old.unlink()
    total = packed = lines = 0
    for fn, text in sorted(files.items()):
        (dest / fn).write_text(text)
        total += len(text.encode())
        packed += len(lzma.compress(text.encode(), preset=9))
        lines += text.count("\n")
    big = max(len(t.encode()) for t in files.values())
    print(f"layout_manifest: {len(files)} files, {lines} lines, {total} bytes, {packed} xz; largest file {big} bytes")


def check() -> int:
    if not ((ROOT / "build.ninja").exists() and (ROOT / "build" / "main.elf").exists()):
        print("layout_manifest: check skipped, no matching build here (build.ninja, build/*.elf)")
        return 0
    fresh, have = derive_all(), committed()
    bad = [f for f in sorted(set(fresh) | set(have)) if fresh.get(f) != have.get(f)]
    for f in bad[:8]:
        print(f"layout_manifest: {f}: {'missing' if f not in have else 'stale' if f in fresh else 'not in the fresh derivation'}")
    print(f"layout_manifest: {len(have)} committed files {'equal the fresh derivation' if not bad else 'DIFFER: ' + str(len(bad)) + ' files (python3 tools/pc/layout_manifest.py --write)'}")
    return 1 if bad else 0


def norm(x, ordered=True):
    """A value as plain data that compares equal when two models agree (dicts keep their order)."""
    if isinstance(x, dict):
        items = [(norm(k), norm(v, ordered)) for k, v in x.items()]
        return ("dict", items if ordered else sorted(items, key=repr))
    if isinstance(x, (set, frozenset)):
        return ("set", sorted(map(norm, x), key=repr))
    if isinstance(x, (list, tuple)):
        return (type(x).__name__, [norm(v, ordered) for v in x])
    return x


# attributes whose dict order is the order of the symbol tables (refs: of an object's, elf_addr: of an ELF's), which
# the manifest does not keep (it keeps the sorted order); every use is a lookup (layout_manifest.py --check-model)
UNORDERED = {"refs", "elf_addr"}


def check_model() -> int:
    a = data_c.Matching(mode="mwcc")
    b = data_c.Matching(mode="manifest")
    bad = 0
    names = sorted(set(vars(a)) | set(vars(b)))
    for k in names:
        if k == "mode":
            continue
        x, y = norm(getattr(a, k, None), k not in UNORDERED), norm(getattr(b, k, None), k not in UNORDERED)
        if x != y:
            bad += 1
            ex = ""
            if isinstance(x, tuple) and x[0] == "dict" and isinstance(y, tuple) and y[0] == "dict":
                for (k1, v1), (k2, v2) in zip(x[1], y[1]):
                    if (k1, v1) != (k2, v2):
                        ex = f" first: {repr((k1, v1))[:150]} vs {repr((k2, v2))[:150]}"
                        break
                else:
                    ex = f" ({len(x[1])} and {len(y[1])} entries)"
            print(f"layout_manifest: {k} DIFFERS{ex}")
    print(f"layout_manifest: model from the matching build and from the manifest: {len(names) - 1 - bad} of {len(names) - 1} "
          f"attributes equal, {bad} differ")
    return 1 if bad else 0


def check_bytes() -> int:
    from elftools.elf.elffile import ELFFile
    m = data_c.Matching(mode="mwcc")
    load = {mod: a for mod, a in m.load.items()}
    bad = checked = nbytes = 0
    orig_main = ELFFile(io.BytesIO((ROOT / "build" / "orig" / "SLPM_662.80").read_bytes()))
    prog = [(s["sh_addr"], s["sh_size"], s.data()) for s in orig_main.iter_sections()
            if s["sh_type"] == "SHT_PROGBITS" and s["sh_addr"] and s["sh_size"]]

    def orig_bytes(mod, a, n):
        if mod == "main":
            for s0, sz, d in prog:
                if s0 <= a and a + n <= s0 + sz:
                    return d[a - s0:a - s0 + n]
            return None
        b = (ROOT / "build" / "orig" / f"{mod}.bin").read_bytes()
        off = a - load[mod]
        return b[off:off + n] if 0 <= off and off + n <= len(b) else None

    for mod in m.modules:
        elf = ELFFile(io.BytesIO((ROOT / "build" / f"{mod}.elf").read_bytes()))
        sec = [(s["sh_addr"], s["sh_size"], s.data()) for s in elf.iter_sections()
               if s["sh_type"] == "SHT_PROGBITS" and s["sh_addr"] and s["sh_size"]]
        spans = [(a, sz) for key in m.csecs if block_mod(key) == mod and block_kind(key) == "Data"
                 for a, sz, o, i in m.csecs[key]]
        spans += [(a, sz) for key in m.textbins if block_mod(key) == mod for a, sz, o in m.textbins[key]]
        spans += [(P, 4) for key in m.codewords if block_mod(key) == mod for P, fn, A, o, h, v in m.codewords[key]]
        for a, sz in spans:
            e = next((d[a - s0:a - s0 + sz] for s0, n0, d in sec if s0 <= a and a + sz <= s0 + n0), None)
            o = orig_bytes(mod, a, sz)
            checked += 1
            nbytes += sz
            if e is None or o is None or e != o:
                bad += 1
                if bad <= 5:
                    print(f"DIFF {mod} {a:X}+{sz:X}: elf {'none' if e is None else 'ok'}, orig {'none' if o is None else 'ok'}")
    print(f"layout_manifest: {checked} spans, {nbytes} bytes, {bad} differ between the matching ELF and build/orig")
    return 1 if bad else 0


def stats() -> int:
    d = ROOT / "config" / "pc"
    tot = lines = packed = 0
    n = 0
    for p in sorted((d / "layout").glob("*.txt")) + [d / "sources.txt"]:
        b = p.read_bytes()
        n += 1
        tot += len(b)
        lines += b.count(b"\n")
        packed += len(lzma.compress(b, preset=9))
        print(f"{p.name:32s} {b.count(chr(10).encode()):7d} lines {len(b):8d} bytes")
    print(f"{n} files, {lines} lines, {tot} bytes, {packed} xz (per file)")
    return 0


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--write", nargs="?", const=str(ROOT / "config" / "pc"), metavar="DIR")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--check-model", action="store_true")
    ap.add_argument("--check-bytes", action="store_true")
    ap.add_argument("--stats", action="store_true")
    a = ap.parse_args()
    rc = 0
    if a.write:
        write(Path(a.write))
    if a.check:
        rc |= check()
    if a.check_model:
        rc |= check_model()
    if a.check_bytes:
        rc |= check_bytes()
    if a.stats:
        rc |= stats()
    if not (a.write or a.check or a.check_model or a.check_bytes or a.stats):
        ap.print_help()
    sys.exit(rc)


if __name__ == "__main__":
    main()
