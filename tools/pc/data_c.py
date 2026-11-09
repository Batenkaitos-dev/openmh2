#!/usr/bin/env python3
"""Definitions for the game's data objects that have no C definition.

    python3 tools/pc/data_c.py --out DIR [--platform linux|windows] [--report]
    python3 tools/pc/data_c.py --out DIR --cases BUILD --clang CC [-j N]
                                 the standing check (ctest data_cases)
    python3 tools/pc/data_c.py --lib-data-list      rewrite pc/core/lib_data.txt's table

The decomp's C declares (extern) many data objects that only the matching
build defines: from the assembly the splitter wrote for the data that is not
C yet (build/MODULE/asm/MODULE/data/bss*.o), from INCLUDE_DATA lines, and
from linker-script names (config/ld/*_aliases.ld; D_ADDR names; names a
module's symbol file gives for another module's object). mh2-core has
none of that, so this tool writes the definitions. Everything is derived from the
matching build and the user's original: it stays in build/.

What it reads: the matching C objects (what they reference and define), the
modules' ELFs (addresses, sizes), the .bss pieces, config/symbols,
config/ld/*_aliases.ld, config/pc/data_names.txt (overrides), and
pc/core/lib_data.txt (which objects are a library's: left to the platform
layer's stubs).

Where the layout comes from (layout_mode(); MH2_LAYOUT=mwcc|manifest decides):
"mwcc", the matching build's output (the objects, ELFs, maps, build/ld and the
splitter's data objects: what a Linux tree that ran the matching build has),
or "manifest", the committed layout (config/pc/layout/*.txt and
config/pc/sources.txt: names, addresses, sizes, relocation targets; no byte of
the game; tools/pc/layout_manifest.py writes and checks it) with the user's
original (build/orig) where the matching ELF's bytes were. Without MH2_LAYOUT,
a tree with build/main.elf uses "mwcc" and any other tree with the layout
directory uses "manifest" (Windows without WSL). Matching has the same
attributes either way, and the generated files are identical
(layout_manifest.py --check-model; diff -r of two --out directories).
A tree that takes "mwcc" by finding build/main.elf (no MH2_LAYOUT) stops when that layout is not the source's, i.e. the
committed manifest's (check_layout_current): the matching build is older than the source. The message names the
remedies: tools/build.sh, or MH2_LAYOUT=manifest.

What it writes into DIR:
- sizes.txt       NAME SIZE (decimal), one line per name these files size:
                  the ELF's .size of each alias and block (what COFF can't
                  say), and the size of each array defined here. Written on
                  both platforms.
- data_MODULE.c   one file per module, added to the module's objects:
                  the module's .bss block (DataC_MODULE_Bss: the span of the
                  .bss the splitter's pieces hold, 64 KiB aligned, the PS2
                  address's offset in its 64 KiB kept: pal_align.h) with every name of it an
                  alias at its PS2 offset (on ELF with .type and .size: the
                  matching build's size), so that a struct and the names of
                  its fields are one memory; INCLUDE_DATA blobs with the
                  user's bytes; and the few zero objects outside a block (the
                  linker's fill between two C objects that a name reaches).
                  The modules' .data and .rodata are C (and libraries' pieces
                  that no C names), and the matching build has no .sdata or
                  .sbss. main's libraries' .data pieces are a block too
                  (DataC_main_Data, zeros: a library's initial bytes never
                  come from the binary). Its C .data and .rodata (MOVE_C)
                  are in it at their PS2 offsets too: the matching C
                  objects' bytes, each address word (R_MIPS_32) the PC
                  address of its target, every global an alias of the
                  block, and the C's own definitions made weak (#pragma
                  weak in the file's inject header); file-static objects
                  keep the PC's copy (a hole: report.txt). And
                  data_main.c defines DataC_At (pc/core/data_c.h): the host
                  address of a resident PS2 address, Pal_Fatal for another.
- inject_*.h      aliases whose base a C file defines: the file is compiled
                  with -include of its header (a symbol that is an object
                  plus an offset can only be written where the object is).
- inject.cmake    the files and their headers, for pc/core/CMakeLists.txt.
- report.txt      what each name became, and what was left (those get the
                  generator's 4 KB stubs: gen_stubs.py).
- cases.json      the input of the standing check: each name's PC extent
                  (the bytes from it to the end of the PC object that holds
                  it, as one memory with the PS2's), the names each game TU's
                  matching object references, and the names whose matching
                  extent (their symbol's size, else up to the next name: what
                  an `extern T X[];` can't say) is larger than the PC's.
- moved.json      the C names a block lays out (MOVE_C): their bytes and
                  address words, which the standing check compares with the
                  PC's own (weak) definitions in the game objects.
- ptrs.json       the C objects of the .data blocks (moved, or file-static)
                  whose words hold an EE RAM address with no relocation in
                  the matching object, and the names each game TU declares
                  among them.

The standing check (--cases, ctest data_cases) compiles a probe of each game
TU (the TU, then `_Static_assert(sizeof(X) <= extent)` for each name it
references; clang -fsyntax-only, cached in DIR/probe by the probe's text and
its dependencies) and fails on a name the C sizes past its PC object, or a
name whose matching extent is past it (an alias past a C object's end). It
also reads the PC's game objects (ELF) and fails on a moved C name whose PC
definition isn't weak, or differs from the block in its size, a byte, or an
address word's symbol or addend. And it types each game TU's declarations of
the ptrs.json objects (clang's AST, laid out by the EE's rules as content_gen
does; cached in DIR/probe), and fails on a word any TU types as a pointer that
holds an EE RAM address with no relocation: the PC would keep the PS2's address
(gBladeDetailLabels, `char[16]` of two pointers' bytes, crashed the core).
"""
from __future__ import annotations

import argparse
import bisect
import fnmatch
import collections
import io
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(os.path.abspath(__file__)).parent.parent.parent
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tools" / "pc"))

RESIDENT = ["main", "sub_main"]
OVERLAYS = ["select", "game", "lobby", "nethttp", "gm_sub", "lbguild", "yn", "plsel"]
DATA_BLOCKS = RESIDENT + OVERLAYS       # modules whose asm .data pieces are a block too (main: the libraries'
                                        # .data, their handles; sub_main: text_end's piece, zeros; the overlays:
                                        # text_end's and rodata_end's pieces, nethttp's and yn's libraries' .data)
# Blocks whose C input sections are laid out in the block too, at their PS2 offsets :
# the bytes are the matching C objects' (each R_MIPS_32 word rewritten to its target's PC address), every global
# the sections define is an alias of the block, and the C's own definition is made weak (#pragma weak NAME in its
# file's inject header) so that the block's strong alias wins at link time
MOVE_C = {"main.data": (".data", ".rodata"),       # block key -> the sections of the C it lays out
          "sub_main.data": (".data", ".rodata"),
          "sub_main": (".bss",),       # sub_main's C .bss inside its .bss block (net_if.c's, 0x59BF80-0x59C1B0)
          # each overlay's .data and .rodata in its .data block, and its C .bss in its .bss block (session 6). An
          # overlay's blocks are RAM-backed:
          # every name is Pal_EeRam + its PS2 address, and the .data block's words that are addresses are a table
          # (DataC_OverlayWords) that the loader writes over the disc's image at each load. A module with no
          # splitter's piece there (lbguild's .data; select's, gm_sub's and plsel's .bss) gets a block from its C
          **{f"{o}.data": (".data", ".rodata") for o in OVERLAYS},
          **{o: (".bss",) for o in OVERLAYS}}
# The splitter's textbin pieces a block lays out too: their bytes are the
# matching build's (Capcom's VU1 programs: the game's own data, read by DMA), and the names the matching ELF
# gives inside them (Vu1Prog00..) are names of the block, so that the C data's words that address them
# (gVu1ProgInfoNN.upload) are their PC addresses
TEXTBIN = {"sub_main.data": ("vu1_microcode",)}
WINDOWS = 0x6AF200          # the first overlay window; below it is main, sub_main and their .bss
MANGLE = "__regcall3__"
LIB_DATA = ROOT / "pc" / "core" / "lib_data.txt"
NAMES_FILE = ROOT / "config" / "pc" / "data_names.txt"
ALIAS_LINE = re.compile(r"^\s*([A-Za-z_]\w*)\s*=\s*([A-Za-z_]\w*)\s*\+\s*(0x[0-9A-Fa-f]+|\d+)\s*;")
HEXNAME = re.compile(r"^D_([0-9A-Fa-f]{6,8})$")


HIDE = set(filter(None, os.environ.get("MH2_DATA_C_HIDE", "").split(",")))     # the inputs test (--check-inputs)


def present(p: Path) -> bool:
    """An input file is there (and not hidden by the inputs test)."""
    if not p.exists():
        return False
    try:
        return p.relative_to(ROOT).as_posix() not in HIDE
    except ValueError:          # outside the tree (a test's copy): nothing hides it
        return True


def check_inputs(out: Path) -> int:
    """ctest data_inputs: with each kind of input hidden in turn (a module's ELF, map and linker script, a data
    object of the splitter; and, for the manifest, one of its files and the original's boot executable), the
    generator must stop and name it."""
    ld = (ROOT / "build" / "ld" / "main.ld").read_text()
    obj = re.search(r"(build/main/asm/main/data/[^\s()]*\.o)\(", ld).group(1)
    bad = 0
    tests = [("mwcc", h) for h in ["build/main.elf", "build/main.map", "build/ld/main.ld", obj]]
    if not present(ROOT / "build" / "main.elf"):
        tests = []          # a tree that never ran the matching build (a fresh clone) has the manifest's inputs only
    if LAYOUT_DIR.is_dir():
        tests += [("manifest", h) for h in ["config/pc/layout/objs.txt", "config/pc/layout/blocks.txt",
                                           "build/orig/SLPM_662.80"]]
    for mode, hide in tests:
        r = subprocess.run([sys.executable, os.path.abspath(__file__), "--out", str(out / "inputs_test")],
                           env=dict(os.environ, MH2_DATA_C_HIDE=hide, MH2_LAYOUT=mode), capture_output=True, text=True)
        ok = r.returncode != 0 and hide in r.stderr + r.stdout
        bad += not ok
        print(f"data_inputs: {mode}, without {hide}: {'stops and names it' if ok else 'DID NOT STOP (' + str(r.returncode) + ')'}")
    print(f"data_inputs: {'OK' if not bad else 'FAILED'}")
    return 1 if bad else 0


def elf_symbols(path: Path):
    from elftools.elf.elffile import ELFFile
    # the file whole: pyelftools' many small reads are slow on a network share (WSL's, seen from Windows)
    e = ELFFile(io.BytesIO(Path(path).read_bytes()))
    secs = {i: s.name for i, s in enumerate(e.iter_sections())}
    out = []
    for s in e.get_section_by_name(".symtab").iter_symbols():
        if not s.name:
            continue
        sh = s["st_shndx"]
        out.append((s.name, s["st_value"], s["st_size"], s["st_info"]["type"], s["st_info"]["bind"],
                    None if sh in ("SHN_UNDEF", "SHN_ABS", "SHN_COMMON") else secs.get(sh, "?"), sh == "SHN_UNDEF"))
    return out


LAYOUT_DIR = ROOT / "config" / "pc" / "layout"


def layout_mode() -> str:
    """Where Matching takes the layout from. MH2_LAYOUT=mwcc|manifest decides; otherwise the matching build's
    output (a tree that has build/main.elf: the Linux tree that ran the matching build), else the committed
    manifest (config/pc/layout, with the user's original in build/orig: a tree that never ran MWCC)."""
    env = os.environ.get("MH2_LAYOUT", "")
    if env:
        if env not in ("mwcc", "manifest"):
            sys.exit(f"data_c.py: MH2_LAYOUT={env}: mwcc or manifest")
        return env
    if present(ROOT / "build" / "main.elf") or not LAYOUT_DIR.is_dir():
        return "mwcc"
    return "manifest"


def check_layout_current(m) -> None:
    """The guard of the build: a core made from the matching build's layout (mwcc, chosen by finding build/main.elf and
    not by MH2_LAYOUT) stops when that layout is not the source's. A matching build older than its source
    (a tree whose sources moved on after the last tools/build.sh, or ELFs copied from another) gives the core the
    addresses of an older layout, and it faults far from the cause (the 3D descriptors' upload words kept the PS2's
    addresses). The source's layout is the committed manifest (config/pc/layout, which mh2-check keeps equal to a
    fresh derivation): the matching build's, derived as layout_manifest.py --check does from the Matching in hand,
    must equal it. A third of a second a run, on top of the generator's twenty."""
    if os.environ.get("MH2_LAYOUT") or m.mode != "mwcc" or not LAYOUT_DIR.is_dir():
        return          # MH2_LAYOUT=mwcc is asked for by name (layout work); the manifest is the other mode
    import layout_manifest
    import sources
    fresh = {f"layout/{k}": v for k, v in layout_manifest.derive(m).items()}
    fresh["sources.txt"] = sources.sources_text()
    have = layout_manifest.committed()
    bad = [f for f in sorted(set(fresh) | set(have)) if fresh.get(f) != have.get(f)]
    if not bad:
        return
    more = ", ..." if len(bad) > 4 else ""
    sys.exit("\n".join([
        f"data_c.py: the layout of the matching build (build/*.elf, maps, objects) is not the source's: {len(bad)} of the "
        f"committed manifest's files differ from a fresh derivation ({', '.join(bad[:4])}{more}).",
        "The matching build is older than the source (or the manifest is). Either:",
        "  - build the matching build again (tools/build.sh), then build here again; or",
        "  - build from the committed manifest, the source's layout, which needs no matching build:",
        "    MH2_LAYOUT=manifest (and build/orig, the original's boot executable and overlays).",
        "If the matching build is the current one and its layout was meant to change:",
        "  python3 tools/pc/layout_manifest.py --write, and commit config/pc."]))


def word_fmt(w) -> str:
    """A relocated word (Matching.cinit) as a manifest line's tail."""
    if w[0] == "sym":
        return f"s {w[1]} {w[2]:X}"
    if w[0] == "ps2":
        return f"p {w[1]:X}"
    return f"n {w[1]}"


def word_parse(t: list):
    if t[0] == "s":
        return ("sym", t[1], int(t[2], 16))
    if t[0] == "p":
        return ("ps2", int(t[1], 16))
    return ("none", " ".join(t[1:]))


def spans_fmt(L) -> str:
    return " ".join(f"{a:X}-{b:X}" for a, b in L)


def spans_parse(t: list):
    return [(int(x.split("-")[0], 16), int(x.split("-")[1], 16)) for x in t]


SYM_TYPE = {"F": "STT_FUNC", "O": "STT_OBJECT", "N": "STT_NOTYPE"}


class Manifest:
    """The committed layout (config/pc/layout/*.txt; tools/pc/layout_manifest.py writes and checks it): what the
    matching build's objects, ELFs, maps and splitter pieces say, without a byte of the game. Each file starts
    with `@ KIND NAME`; a kind that is long is in numbered parts (KIND.NAME.NN.txt) that are one list in order.
    The bytes of the C data are the user's original's (build/orig): Matching reads them where the matching ELF
    had them (layout_manifest.py --check-bytes: the two are equal)."""

    def __init__(self):
        if not LAYOUT_DIR.is_dir():
            sys.exit(f"data_c.py: {LAYOUT_DIR.relative_to(ROOT).as_posix()} is missing (the committed layout manifest)")
        self.by = {}        # (kind, name) -> [line split on spaces]
        self.files = []
        for p in sorted(LAYOUT_DIR.glob("*.txt")):
            if not present(p):
                continue
            lines = p.read_text().split("\n")
            h = lines[0].split(" ")
            if len(h) != 3 or h[0] != "@":
                sys.exit(f"data_c.py: {p.relative_to(ROOT).as_posix()}: no `@ KIND NAME` header")
            self.files.append(p.name)
            self.by.setdefault((h[1], h[2]), []).extend(l.split(" ") for l in lines[1:] if l)
        self.orig = None

    def need(self, kind, name="-", empty_ok=False):
        if empty_ok and (kind, name) not in self.by:
            return []           # a module with no such lines has no such file
        if (kind, name) not in self.by:
            f = f"{kind}.txt" if name == "-" else f"{kind}.{name}.NN.txt"
            sys.exit(f"data_c.py: config/pc/layout/{f} is missing (the layout manifest's {kind} lines"
                     f"{'' if name == '-' else ' of ' + name})")
        return self.by[(kind, name)]

    def load_head(self, m):
        """objs, cdef, cfuncs, refs, undef_by_src."""
        for _, mod, src, obj in self.need("objs"):
            m.objs.append((mod, src, obj))
        mod_of = {src: mod for mod, src, obj in m.objs}
        for t in self.need("def"):
            if t[0] == "D":
                for n in t[2:]:
                    if n.endswith("()"):
                        n = n[:-2]
                        m.cfuncs.add(n)
                    m.cdef.setdefault(n, t[1])
            else:               # F: functions a later object defines again (a name another object defined first)
                m.cfuncs.update(t[1:])
        for t in self.need("undef"):
            m.undef_by_src[t[1]] = set(t[2:])
            for n in t[2:]:
                m.refs[n].add(mod_of[t[1]])

    def syms(self, mod):
        """(sorted [(addr, size, name, type)], nm_size {name: size}, {name: addr}, bss range or None)."""
        L, nm, addr, rng = [], {}, {}, None
        for t in self.need("sym", mod):
            if t[0] == "Y":
                v = int(t[2], 16)
                L.append((v, int(t[3], 16), t[1], SYM_TYPE.get(t[4], t[4])))
                addr[t[1]] = v
            elif t[0] == "M":
                nm[t[1]] = int(t[2], 16)
            elif t[0] == "A":
                addr[t[1]] = int(t[2], 16)
            elif t[0] == "E":
                rng = (int(t[1], 16), int(t[2], 16))
        L.sort()
        return L, nm, addr, rng

    def load_pieces(self, m):
        for mod in m.modules:
            for t in self.need("pieces", mod, True):
                m.piece_scan.append((mod, t[1], int(t[2], 16) if len(t) > 2 else None))

    # ---- the user's original: the bytes of the C data ----
    def orig_bytes(self, load, mod, a, n):
        """N bytes at PS2 address A of module MOD in build/orig: the boot executable's image for main, the
        overlay's .bin at its load address for the others."""
        if self.orig is None:
            self.orig = {}
        if mod == "main":
            if "main" not in self.orig:
                from elftools.elf.elffile import ELFFile
                p = ROOT / "build" / "orig" / "SLPM_662.80"
                if not present(p):
                    sys.exit("data_c.py: build/orig/SLPM_662.80 is missing (the user's boot executable: tools/extract.py)")
                e = ELFFile(io.BytesIO(p.read_bytes()))
                self.orig["main"] = [(s["sh_addr"], s["sh_size"], s.data()) for s in e.iter_sections()
                                     if s["sh_type"] == "SHT_PROGBITS" and s["sh_addr"] and s["sh_size"]]
            for s0, sz, d in self.orig["main"]:
                if s0 <= a and a + n <= s0 + sz:
                    return d[a - s0:a - s0 + n]
        else:
            if mod not in self.orig:
                p = ROOT / "build" / "orig" / f"{mod}.bin"
                if not present(p):
                    sys.exit(f"data_c.py: build/orig/{mod}.bin is missing (the user's overlay: tools/extract.py)")
                self.orig[mod] = p.read_bytes()
            off = a - load[mod]
            if 0 <= off and off + n <= len(self.orig[mod]):
                return self.orig[mod][off:off + n]
        sys.exit(f"data_c.py: 0x{a:X}-0x{a + n:X} is not in the original's image of {mod}")

    def load_cdata(self, m):
        """bss_block, pieces, holes, align16 and, per block of MOVE_C, the C data's csecs, cnames, cinit, codewords,
        cstatic, static_words, textbins and textnames."""
        obj_of = {src: obj for mod, src, obj in m.objs}
        for t in self.need("blocks"):
            if t[0] == "B":
                m.bss_block[t[1]] = (int(t[2], 16), int(t[3], 16))
            elif t[0] == "P":
                m.pieces[t[1]] = spans_parse(t[2:])
        for t in self.need("holes"):
            m.holes[t[1]] = spans_parse(t[2:])
        for mod in m.modules:
            m.align16[mod] = [(int(t[1], 16), t[2]) for t in self.need("align", mod, True)]
        for key in MOVE_C:
            if (("cdata", key)) not in self.by:
                continue
            mod = block_mod(key)
            secs, statics, words, sw, cw, tb = [], [], {}, {}, [], []
            for t in self.by[("cdata", key)]:
                k = t[0]
                if k == "S":
                    secs.append((int(t[1], 16), int(t[2], 16), obj_of[t[3]], int(t[4])))
                elif k == "N":
                    if t[1] in m.cnames and m.cnames[t[1]][0] != key:
                        sys.exit(f"data_c.py: {t[1]} is a C name of {m.cnames[t[1]][0]}'s block and {key}'s")
                    m.cnames[t[1]] = (key, int(t[2], 16), int(t[3], 16), t[4])
                elif k == "R":
                    words[int(t[1], 16)] = word_parse(t[2:])
                elif k == "W":
                    sw[int(t[1], 16)] = word_parse(t[2:])
                elif k == "J":
                    cw.append((int(t[1], 16), t[2], int(t[3], 16), obj_of[t[4]], None if t[5] == "-" else t[5]))
                elif k == "T":
                    statics.append((int(t[1], 16), int(t[2], 16), t[3], obj_of[t[4]]))
                elif k == "L":
                    tb.append((int(t[1], 16), int(t[2], 16), f"build/{mod}/asm/{mod}/data/{t[3]}.o"))
            lo, hi = m.bss_block[key]
            data = bytearray(hi - lo)
            for a, sz, o, i in secs:
                if block_kind(key) == "Data":       # a .bss block's C sections are zeros
                    data[a - lo:a - lo + sz] = self.orig_bytes(m.load, mod, a, sz)
            for a, sz, o in tb:
                data[a - lo:a - lo + sz] = self.orig_bytes(m.load, mod, a, sz)
                for v, s_, n, t_ in m.elf[mod]:
                    if a <= v < a + sz and t_ != "STT_FUNC":
                        m.textnames[n] = (key, v)
            for P in sw:
                data[P - lo:P - lo + 4] = bytes(4)
            m.csecs[key] = secs
            m.textbins[key] = tb
            m.cstatic[key] = statics
            m.static_words[key] = sw
            m.codewords[key] = [(P, fn, A, o, h, int.from_bytes(self.orig_bytes(m.load, mod, P, 4), "little"))
                                for P, fn, A, o, h in cw]
            m.cinit[key] = (bytes(data), words)


class Matching:
    """What the matching build says (build/, build.ninja, config/)."""

    def __init__(self, mode=None):
        """MODE: where the layout comes from (layout_mode()): "mwcc", the matching build's objects, ELFs, maps
        and the splitter's data objects (what the matching build of Linux made); "manifest", the committed
        layout (config/pc/layout, tools/pc/layout_manifest.py) and the user's original (build/orig). The
        attributes are the same either way (layout_manifest.py --check-model compares them all)."""
        from sources import MODULES, source_files, known_broken
        import configure
        self.mode = mode or layout_mode()
        man = Manifest() if self.mode == "manifest" else None
        self.modules = [m for m, _, _ in MODULES]
        self.lw = configure.LOADED_WITH
        self.load = {m: a for m, _, a in MODULES}          # the module's load address (an overlay's window)
        self.oid = {m: i for m, i, _ in MODULES}           # its OverlayId (DataC_OverlayWords)
        self.broken = set(known_broken())
        self.objs = []      # (module, src, obj)
        self.cdef = {}                          # name -> src (defined by a matching C object)
        self.cfuncs = set()                     # the functions the matching C objects define (global)
        self.refs = collections.defaultdict(set)    # undefined name -> modules whose C refers to it
        self.undef_by_src = collections.defaultdict(set)    # src -> the undefined names of its matching object
        self.elf = {}                           # module -> sorted [(addr, size, name, type)]
        self.elf_addr = {}                      # (module, name) -> addr
        self.nm_size = {}                       # (module, name) -> size, from the .NON_MATCHING symbols
        self._starts = {}
        self.bss_range = {}
        self.bss = {}                           # name -> (module, size, addr)
        self.piece_names = set()                # (module, name): the names of the splitter's .bss pieces
        self.piece_scan = []                    # (module, name, size or None): the splitter's pieces, in scan order
        self.bss_block = {}
        self.pieces = {}
        self.holes = {}         # block key -> [(lo, hi)]: the C input sections of the map inside the block (bss_holes)
        self.align16 = {}       # module -> [(PS2 address, name)]: the map's data names at 16 (write_align)
        if man:
            man.load_head(self)
        else:
            self._scan_objs(source_files)
        self._scan_elfs(man)
        if man:
            man.load_pieces(self)
        else:
            for mod in self.modules:
                self._scan_bss(mod)
        self._apply_pieces()
        # What a file that doesn't compile yet (pc/known_broken.txt) defines: zeros of the same size, until it does
        mod_of = {src: mod for mod, src, obj in self.objs}
        sizes = {(mod, n): (s, v) for mod in self.elf for v, s, n, t in self.elf[mod] if t != "STT_FUNC"}
        for n, src in self.cdef.items():
            mod = mod_of.get(src)
            if src in self.broken and (mod, n) in sizes and n not in self.bss:
                self.bss[n] = (mod, sizes[(mod, n)][0] or 4, sizes[(mod, n)][1])
        self.symsize = {}                       # name -> size: of the symbol files
        for f in (ROOT / "config" / "symbols").glob("*.txt"):
            for line in f.read_text(encoding="utf-8").splitlines():
                mm = re.match(r"\s*(\w+)\s*=\s*0x[0-9A-Fa-f]+\s*;.*?size:\s*(0x[0-9A-Fa-f]+|\d+)", line)
                if mm:
                    self.symsize.setdefault(mm.group(1), int(mm.group(2), 0))
        if not man:
            self._scan_maps()
        self.msym = {m: configure._module_symbols(m) for m in self.modules}
        self.alias = {}                         # name -> (base, off, file)
        for f in sorted((ROOT / "config" / "ld").glob("*_aliases.ld")):
            for line in f.read_text(encoding="utf-8").splitlines():
                m = ALIAS_LINE.match(line)
                if m and m.group(1) != m.group(2):         # "X = X + 0" is the object's own name
                    self.alias.setdefault(m.group(1), (m.group(2), int(m.group(3), 0), f.name))
        self.csecs = {}         # block key -> [(PS2 address, size, object, section index)]: the C sections moved
        self.cnames = {}        # name -> (block key, PS2 address, size, src): the globals they define
        self.cinit = {}         # block key -> (bytes from the block's lo, {PS2 address: relocated word})
        self.codewords = {}     # block key -> [(PS2 address, function, addend, object, @N holder, PS2 value)]
        self.cstatic = {}       # block key -> [(lo, hi, name, object)]: the file-static objects among them
        self.static_words = {}  # block key -> {PS2 address: word}: the file-static objects' relocated words
        self.textnames = {}     # name -> (block key, PS2 address): the names of the textbin pieces (TEXTBIN)
        self.textbins = {}      # block key -> [(PS2 address, size, object)]: the textbin pieces laid out
        if man:
            man.load_cdata(self)
        else:
            for key in MOVE_C:
                if block_mod(key) in self.elf:
                    self._scan_cdata(key)
            self.holes = self._map_holes()
            self.align16 = self._map_align16()

    def _scan_objs(self, source_files):
        """objs, cdef, cfuncs, refs and undef_by_src: the matching C objects' symbols (MWCC's)."""
        ninja = (ROOT / "build.ninja").read_text()
        srcs = dict((f, m) for m, f in source_files())
        for m in re.finditer(r"^build (\S+\.o): cc (\S+\.c)\b", ninja, re.M):
            if m.group(2) in srcs:
                self.objs.append((srcs[m.group(2)], m.group(2), m.group(1)))
        for mod, src, obj in self.objs:
            for name, val, size, typ, bind, sec, undef in elf_symbols(ROOT / obj):
                if undef:
                    self.refs[name].add(mod)
                    self.undef_by_src[src].add(name)
                else:
                    if typ == "STT_FUNC" and bind != "STB_LOCAL":
                        self.cfuncs.add(name)
                    if bind != "STB_LOCAL" and sec and not name.startswith("."):
                        self.cdef.setdefault(name, src)

    def _scan_elfs(self, man):
        """elf, elf_addr, nm_size, _starts, bss_range and func_names: the modules' ELFs (the manifest's Y, M, A and
        E lines for the same)."""
        for mod in self.modules:
            if man:
                L, nm, addr, rng = man.syms(mod)
                self.nm_size.update(((mod, n), s) for n, s in nm.items())
                self.elf_addr.update(((mod, n), v) for n, v in addr.items())
                if rng:
                    self.bss_range[mod] = rng
                self.elf[mod] = L
                self._starts[mod] = [x[0] for x in L]
                continue
            p = ROOT / "build" / f"{mod}.elf"
            if not present(p):          # every input is required: a missing one would silently drop names
                sys.exit(f"data_c.py: build/{mod}.elf is missing (the matching build's)")
            syms = elf_symbols(p)
            nm_size = {n[:-len(".NON_MATCHING")]: s for n, v, s, t, b, sec, u in syms if n.endswith(".NON_MATCHING")}
            self.nm_size.update(((mod, n), s) for n, s in nm_size.items())
            L = []
            for n, v, s, t, b, sec, u in syms:
                if u or "." in n or t in ("STT_SECTION", "STT_FILE") or sec is None or n == "_MACRO_INC_GUARD":
                    continue
                L.append((v, s or nm_size.get(n, 0), n, t))
                self.elf_addr[(mod, n)] = v
            L.sort()
            self.elf[mod] = L
            self._starts[mod] = [x[0] for x in L]
            from elftools.elf.elffile import ELFFile
            with open(p, "rb") as f:
                sec = ELFFile(f).get_section_by_name(f".{mod}_bss")
                if sec is not None:
                    self.bss_range[mod] = (sec["sh_addr"], sec["sh_addr"] + sec["sh_size"])
        self.func_names = {n for mod in self.elf for a, sz, n, t in self.elf[mod] if t == "STT_FUNC"}

    def _apply_pieces(self):
        """piece_names and bss, from piece_scan (the splitter's data objects, in scan order)."""
        for mod, n, sz in self.piece_scan:
            self.piece_names.add((mod, n))
            if sz is not None:
                addr = self.elf_addr.get((mod, n))
                if addr is not None:
                    self.bss.setdefault(n, (mod, sz, addr))

    def _scan_maps(self):
        # The span of each module's .bss that the splitter's pieces hold (the matching map's input sections):
        # one block on the PC, every name at its PS2 offset (a C .bss object between two pieces is a hole)
        # main's .data the same way (DATA_BLOCKS: the libraries' .data and .rodata, the splitter's data.data.o and
        # rodata.data.o, with the C .data between them a hole): block key "main.data" (block_mod, block_kind).
        # pieces: each block's spans that the splitter's pieces hold (DataC_At's resident ranges)
        self.bss_block = {}
        self.pieces = {}
        for mod in self.elf:
            mp = ROOT / "build" / f"{mod}.map"
            if not present(mp):         # the blocks come from the map: without it they would silently vanish
                sys.exit(f"data_c.py: build/{mod}.map is missing (the matching build's map, beside build/{mod}.elf)")
            text = mp.read_text()
            for key, sec in ([(mod, "bss")] + [(f"{mod}.data", "data")] * (mod in DATA_BLOCKS)):
                lo, hi = self.bss_range.get(mod, (0, 0)) if sec == "bss" else (0, self.bss_range.get(mod, (0, 0))[0])
                spans = [(int(a, 16), int(a, 16) + int(s, 16)) for a, s in re.findall(
                    r"^ \.%s\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+build/%s/asm/\S+$" % (sec, re.escape(mod)), text, re.M)]
                spans = sorted((a, b) for a, b in spans if b > a and lo <= a and b <= hi)
                if spans:
                    self.bss_block[key] = (spans[0][0], max(b for a, b in spans))
                    merged = []
                    for a, b in spans:
                        if merged and a <= merged[-1][1]:
                            merged[-1] = (merged[-1][0], max(merged[-1][1], b))
                        else:
                            merged.append((a, b))
                    self.pieces[key] = merged

    def _map_holes(self):
        """bss_holes: per block, the C input sections of the map inside it (empty when the block lays them out)."""
        holes = {}
        for key, (lo, hi) in self.bss_block.items():
            text = (ROOT / "build" / f"{block_mod(key)}.map").read_text()
            holes[key] = sorted((int(a, 16), int(a, 16) + int(s, 16)) for a, s, f in re.findall(
                r"^ \.%s\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+)$" % block_kind(key).lower(), text, re.M)
                if "/asm/" not in f and int(s, 16) and lo <= int(a, 16) < hi)
            if key in self.csecs:          # its C sections are laid out in it (MOVE_C): no hole
                holes[key] = []
        return holes

    def _map_align16(self):
        """write_align: per module, the map's names of its own data sections at a 16-aligned PS2 address."""
        inp = re.compile(r"^ (\.\S+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+)$")
        sym = re.compile(r"^\s+0x([0-9a-f]{8,16})\s+([A-Za-z_]\w*)\s*$")
        out = {}
        for mod in self.modules:
            sect = obj = None
            L = out[mod] = []
            for line in (ROOT / "build" / f"{mod}.map").read_text().splitlines():
                mm = inp.match(line)
                if mm:
                    sect, obj = mm.group(1), mm.group(4)
                    continue
                if line.startswith(" ."):
                    sect, obj = line.split()[0], None
                    continue
                ms = sym.match(line)
                if not ms or obj is None or not obj.startswith(f"build/{mod}/") or not sect:
                    continue
                if not (sect.startswith((".data", ".rodata", ".bss", ".sdata", ".sbss")) or
                        (sect == ".text" and "/asm/" in obj and "/data/" in obj)):
                    continue
                A = int(ms.group(1), 16)
                if A % 16 == 0:
                    L.append((A, ms.group(2)))
        return out

    def _scan_cdata(self, key):
        """The C input sections of block KEY (MOVE_C: its module's sections of those names, inside the block or
        right after it): their bytes (the matching ELF's, at their PS2 addresses) and the R_MIPS_32 relocations
        of the matching objects (the words that are addresses). The block grows to cover them."""
        from elftools.elf.elffile import ELFFile
        from elftools.elf.relocation import RelocationSection
        mod = block_mod(key)
        lo, hi = self.bss_block.get(key, (None, None))
        # a .data block stops before the module's .bss; a .bss block at the .bss's end. The C sections start at
        # the .bss's start for a .bss block, at the block's first piece for a resident .data block, and at the
        # overlay's load address for an overlay's (its C .data may come first: lbguild's has no piece)
        bss = self.bss_range.get(mod, (1 << 32, 0))
        top = bss[block_kind(key) == "Bss"]
        floor = bss[0] if block_kind(key) == "Bss" else lo if mod in RESIDENT else self.load[mod]
        if floor is None:
            return
        text = (ROOT / "build" / f"{mod}.map").read_text()
        src_of = {obj: src for m_, src, obj in self.objs}
        byobj = collections.defaultdict(list)       # (object, section name) -> [(PS2 address, size)]
        for sec in MOVE_C[key]:
            for a, s, o in re.findall(r"^ %s\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(build/\S+\.o)$" % re.escape(sec), text, re.M):
                a, s = int(a, 16), int(s, 16)
                if s and floor <= a < top and "/asm/" not in o:
                    if o not in src_of:
                        sys.exit(f"data_c.py: {o} ({sec} at 0x{a:X}) is not a C object of the matching build")
                    byobj[(o, sec)].append((a, s))
        # the textbin pieces (TEXTBIN): the splitter's .text input sections of those objects, inside the block or
        # right after it
        tb = []
        for nm in TEXTBIN.get(key, ()):
            o = f"build/{mod}/asm/{mod}/data/{nm}.o"
            got = [(int(a, 16), int(s, 16)) for a, s in re.findall(
                r"^ \.text\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+%s$" % re.escape(o), text, re.M) if int(s, 16)]
            if len(got) != 1 or not (floor <= got[0][0] < top):
                sys.exit(f"data_c.py: {o}'s .text is not one input section in {key}'s block ({got})")
            tb.append((got[0][0], got[0][1], o))
        starts = [a for v in byobj.values() for a, s in v] + [a for a, s, o in tb]
        if lo is None:          # no splitter's piece: the block is its C
            if not starts:
                return
            lo = hi = min(starts)
        lo = min([lo] + starts)
        hi = max([hi] + [a + s for v in byobj.values() for a, s in v] + [a + s for a, s, o in tb])
        self.bss_block[key] = (lo, hi)
        self.textbins[key] = tb
        for a, sz, o in tb:
            for v, s_, n, t in self.elf[mod]:
                if a <= v < a + sz and t != "STT_FUNC":
                    self.textnames[n] = (key, v)
        img = ELFFile(io.BytesIO((ROOT / "build" / f"{mod}.elf").read_bytes()))
        progbits = [(s["sh_addr"], s["sh_addr"] + s["sh_size"], s) for s in img.iter_sections()
                    if s["sh_type"] == "SHT_PROGBITS" and s["sh_addr"] and s["sh_size"]]

        def elf_bytes(a, n):
            for s0, s1, s in progbits:
                if s0 <= a and a + n <= s1:
                    return s.data()[a - s0:a - s0 + n]
            sys.exit(f"data_c.py: 0x{a:X}-0x{a + n:X} is not in one section of {mod}.elf")
        words = {}
        secs = []
        statics = []
        literals = []           # (lo, hi, name, object): MWCC's local @N objects (string literals, jump tables)
        code = {}               # PS2 address -> (function, addend, object): a word that addresses a code label
        objs = collections.defaultdict(dict)
        for (o, sec), spans in byobj.items():
            objs[o][sec] = spans
        for o, by_sec in objs.items():
            e = ELFFile(io.BytesIO((ROOT / o).read_bytes()))
            at = {}             # section index -> PS2 address
            for sec, spans in by_sec.items():
                mine = [(i, s) for i, s in enumerate(e.iter_sections()) if s.name == sec and s["sh_size"]]
                if [s["sh_size"] for i, s in mine] != [sz for a, sz in spans]:
                    sys.exit(f"data_c.py: {o}'s {sec} sections are not the map's ({len(mine)} and {len(spans)})")
                at.update((i, a) for (i, s), (a, sz) in zip(mine, spans))
                secs += [(a, sz, o, i) for (i, s), (a, sz) in zip(mine, spans)]
            symtab = e.get_section_by_name(".symtab")
            for sy in symtab.iter_symbols():
                sh = sy["st_shndx"]
                if sh in at and sy.name and sy["st_info"]["type"] in ("STT_OBJECT", "STT_NOTYPE"):
                    A = at[sh] + sy["st_value"]
                    if sy["st_info"]["bind"] == "STB_LOCAL" and sy.name.startswith("@"):
                        # MWCC's string literal or jump table (@N): no C object, the block's bytes (a word may
                        # address it)
                        literals.append((A, A + max(sy["st_size"], 1), sy.name, o))
                    elif sy["st_info"]["bind"] == "STB_LOCAL":
                        statics.append((A, A + max(sy["st_size"], 1), sy.name, o))
                    else:
                        if sy.name in self.cnames and self.cnames[sy.name][0] != key:
                            sys.exit(f"data_c.py: {sy.name} is a C name of {self.cnames[sy.name][0]}'s block and {key}'s")
                        self.cnames[sy.name] = (key, A, sy["st_size"], src_of[o])
            for rs in e.iter_sections():
                if not isinstance(rs, RelocationSection) or rs["sh_info"] not in at:
                    continue
                base = at[rs["sh_info"]]
                raw = e.get_section(rs["sh_info"]).data()
                for r in rs.iter_relocations():
                    P, off = base + r["r_offset"], r["r_offset"]
                    if r["r_info_type"] != 2 or P % 4:          # R_MIPS_32, aligned
                        sys.exit(f"data_c.py: {o}+{off:#x}: relocation type {r['r_info_type']} in a moved section")
                    A = int.from_bytes(raw[off:off + 4], "little")
                    sy = symtab.get_symbol(r["r_info_sym"])
                    if sy["st_info"]["bind"] != "STB_LOCAL" and sy.name:
                        words[P] = ("sym", sy.name, A)
                        if sy.name in self.func_names and A:
                            code[P] = (sy.name, A, o)
                    elif sy["st_shndx"] in at:          # this object's moved bytes: the block's own address
                        words[P] = ("ps2", at[sy["st_shndx"]] + (sy["st_value"] if sy["st_info"]["type"] != "STT_SECTION" else 0) + A)
                    else:       # a static function's or another section's address: the PC can't name it
                        tgt = e.get_section(sy["st_shndx"]).name if isinstance(sy["st_shndx"], int) else sy["st_shndx"]
                        words[P] = ("none", f"{o}: {sy.name or tgt}+{A:#x}")
        # only the C sections' bytes: the libraries' pieces stay zeros (a library's initial bytes never come from
        # the binary)
        data = bytearray(hi - lo)
        for a, sz, o, i in secs:
            if block_kind(key) == "Data":       # a .bss block's C sections are zeros
                data[a - lo:a - lo + sz] = elf_bytes(a, sz)
        for a, sz, o in tb:
            data[a - lo:a - lo + sz] = elf_bytes(a, sz)
        # a word inside a file-static object stays the PS2's bytes' zero: the PC's code uses its own copy
        statics.sort()
        self.cstatic[key] = statics
        # the file-static objects' relocated words (data_cases: a word their C types as a pointer has one)
        self.static_words[key] = {P: w for P, w in words.items() if any(a <= P < b for a, b, n, o in statics)}
        for P in list(words):
            if any(a <= P < b for a, b, n, o in statics):
                data[P - lo:P - lo + 4] = bytes(4)
                del words[P]
        bad = [w[1] for w in words.values() if w[0] == "none"]
        if bad:
            sys.exit(f"data_c.py: {len(bad)} words of the moved C data address what no block lays out: {bad[:3]}")
        # a word that addresses a code label inside a function (MWCC's jump tables: a function's symbol plus the
        # case's offset) keeps the PS2's value, the matching bytes: the PC's code never reads MWCC's tables (clang
        # compiles its own switches), so the word is the PS2's and no PC address.
        # The holder is the @N object that contains it; data_cases checks the rule (code_words.json)
        literals.sort()
        self.codewords[key] = []
        for P, (fn, A, o) in sorted(code.items()):
            if P not in words:        # inside a file-static object: its bytes are zeros already
                continue
            holder = next((n for a, b, n, oo in literals if oo == o and a <= P < b), None)
            self.codewords[key].append((P, fn, A, o, holder, int.from_bytes(elf_bytes(P, 4), "little")))
            del words[P]
        self.csecs[key] = sorted(secs)
        self.cinit[key] = (bytes(data), words)
        # DataC_At: the block is resident data at its PS2 offsets now, but for the file-static objects (the PC's
        # copies are elsewhere)
        pieces, cur = [], lo
        for a, b, n, o in statics:
            if a > cur:
                pieces.append((cur, a))
            cur = max(cur, b)
        if cur < hi:
            pieces.append((cur, hi))
        self.pieces[key] = pieces

    def _scan_bss(self, mod):
        ld = ROOT / "build" / "ld" / f"{mod}.ld"
        if not present(ld):
            sys.exit(f"data_c.py: build/ld/{mod}.ld is missing (the matching build's linker script)")
        for o in dict.fromkeys(re.findall(rf"(build/{mod}/asm/{mod}/data/[^\s()]*\.o)\(", ld.read_text())):
            p = ROOT / o
            if not present(p):
                sys.exit(f"data_c.py: {o} is missing (the splitter's data object, named by build/ld/{mod}.ld)")
            syms = elf_symbols(p)
            sz = {n[:-len(".NON_MATCHING")]: s for n, v, s, t, b, sec, u in syms if n.endswith(".NON_MATCHING")}
            for n, v, s, t, b, sec, u in syms:
                # (module, name, the NON_MATCHING size of a .bss name): _apply_pieces makes piece_names and bss of it
                size = sz[n] if sec == ".bss" and "." not in n and n != "_MACRO_INC_GUARD" and n in sz else None
                if size is not None or (sec in (".bss", ".data") and "." not in n and n != "_MACRO_INC_GUARD"):
                    self.piece_scan.append((mod, n, size))

    def cover(self, mod, addr):
        """The objects of module MOD's ELF that contain ADDR: (name, offset, size, type)."""
        L = self.elf.get(mod)
        if not L:
            return []
        i = bisect.bisect_right(self._starts[mod], addr)
        return [(n, addr - a, s, t) for a, s, n, t in L[max(0, i - 8):i] if a <= addr < a + max(s, 1)]

    # ---- what the PC link will define ----
    def real(self, name, lib):
        """('bss', module) or ('c', src, type): a definition the PC link will have for NAME."""
        if name in self.bss and name not in lib:
            return ("bss", self.bss[name][0])
        src = self.cdef.get(name)
        if src and src not in self.broken:
            return ("c", src)
        return None


def read_table(path: Path) -> list[list[str]]:
    if not path.exists():
        sys.exit(f"data_c.py: {path} is missing")
    rows = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            rows.append(line.split("\t") if "\t" in line else line.split())
    return rows


class Plan:
    def __init__(self, m: Matching):
        self.m = m
        self.lib = {r[0] for r in read_table(LIB_DATA) if len(r) > 1 and r[1].startswith("lib")}
        self.over = {r[0]: r[1:] for r in read_table(NAMES_FILE)}
        self.blobs = {}
        for f in sorted((ROOT / "src").glob("*/*.c")):
            for mm in re.finditer(r'^INCLUDE_DATA\("build/blobs/(\w+)",\s*(\w+)\);', f.read_text(encoding="utf-8", errors="replace"), re.M):
                self.blobs[mm.group(2)] = mm.group(1)
        self.act = {}           # name -> action tuple
        self.left = {}          # name -> why not
        want = set(n for n in m.refs if n not in m.cdef) | {n for n, src in m.cdef.items() if src in m.broken} | set(self.blobs)
        self.demand = sorted(n for n in want if n not in m.func_names and not re.match(r"func_[0-9A-Fa-f]{6,8}$", n))
        self.cdef_fn = None
        for n in self.demand:
            r = self.resolve(n)
            if isinstance(r, str):
                self.left[n] = r
            else:
                self.act[n] = r
        for a in list(self.act.values()):       # the .bss object an alias points into is defined even if nothing names it
            if a[0] == "alias" and a[3][0] == "data" and a[1] not in self.act:
                mod, size, addr = m.bss[a[1]]
                self.act[a[1]] = ("bss", mod, size, addr)
        self.blockify()
        # the moved C objects whose nonzero words are RAM_AREA(addr) in the C (config/pc/data_names.txt ram-words)
        self.ps2_words = set()          # the PS2 addresses of the words of ps2-words rows (block addresses now)
        self.ps2_rows = {}              # ps2-words row -> [(name, words)]
        for pat, o in self.over.items():
            if o[0] != "ps2-words":
                continue
            hits = [n for n in sorted(m.cnames) if fnmatch.fnmatchcase(n, pat)]
            if not hits:
                sys.exit(f"data_c.py: config/pc/data_names.txt: {pat} names no C object a block lays out")
            self.ps2_rows[pat] = []
            for n in hits:
                key, A, size, src = m.cnames[n]
                img, words = m.cinit[key]
                lo = m.bss_block[key][0]
                k = 0
                for P in range(A, A + size - 3, 4):
                    V = int.from_bytes(img[P - lo:P - lo + 4], "little")
                    if V and P not in words and any(a <= V < b for kk in m.pieces if block_mod(kk) in RESIDENT
                                                     for a, b in m.pieces[kk]):
                        words[P] = ("ps2", V)
                        self.ps2_words.add(P)
                        k += 1
                self.ps2_rows[pat].append((n, k))
        for n, o in self.over.items():
            if o[0] == "ram-words":
                if n not in m.cnames:
                    sys.exit(f"data_c.py: config/pc/data_names.txt: {n} is not a C object a block lays out")
                key, A, size, src = m.cnames[n]
                img, words = m.cinit[key]
                lo = m.bss_block[key][0]
                # the row's offsets (the object's words that are RAM_AREA(addr) in the C), or every nonzero word
                offs = {int(x, 0) for x in o[1:]}
                for P in range(A, A + size - 3, 4):
                    V = int.from_bytes(img[P - lo:P - lo + 4], "little")
                    if offs and P - A not in offs:
                        continue
                    if offs and (not V or P in words):
                        sys.exit(f"data_c.py: config/pc/data_names.txt: {n}+{P - A:#x} is not a nonzero number")
                    if V and P not in words:
                        words[P] = ("ram", V)

    def blockify(self):
        """Each module's .bss block (Matching.bss_block): the .bss objects in it, and the aliases into them, become
        names at their PS2 offsets in one object ("block" actions: module, PS2 address, size or None), so that a
        struct and the names of its fields are one memory, as in the matching build. Every other name the matching
        build gives the block is defined there too ."""
        m = self.m

        def inblock(mod, addr):
            lo, hi = m.bss_block.get(mod, (0, 0))
            return lo <= addr < hi
        for n, a in list(self.act.items()):
            # a zero object of its own (a .bss piece, or the linker's fill an alias names) inside one of the
            # module's blocks, its .data block's too (game's D_008B0AAC): a name of the block
            k = next((k for k in (a[1], f"{a[1]}.data") if inblock(k, a[3])), None) if a[0] == "bss" else None
            if k:
                self.act[n] = ("block", k, a[3], a[2])
        # The C's globals of the moved sections (MOVE_C): names of the block at their PS2 offsets, the C's own
        # definitions weak (self.weak: src -> names, for its inject header)
        self.weak = collections.defaultdict(list)
        self.targets = set()        # the symbols the moved C data's words name (init_asm)
        for n, (key, A, size, src) in sorted(m.cnames.items()):
            self.act[n] = ("block", key, A, size or None)
            if src not in m.broken and n not in self.blobs:
                self.weak[src].append(n)
        # the textbin pieces' names (TEXTBIN): names of the block, whatever the C's references made of them
        for n, (key, A) in sorted(m.textnames.items()):
            self.act[n] = ("block", key, A, None)
            self.left.pop(n, None)
        for n, a in list(self.act.items()):
            if a[0] == "alias" and (a[3][0] == "data" or (a[3][0] == "c" and a[1] in m.cnames)):
                b = self.act.get(a[1])
                if b and b[0] == "block":
                    self.act[n] = ("block", b[1], b[2] + a[2], m.symsize.get(n) or m.nm_size.get((block_mod(b[1]), n)))
        taken = set(self.act) | set(m.cdef) | self.lib | set(self.left)
        self.dup_labels = []        # a name two modules' blocks give (overlays of one window): the first module's
        for key, (lo, hi) in m.bss_block.items():
            mod = block_mod(key)
            for v, s, nm, t in m.elf[mod]:
                if not (lo <= v < hi) or t == "STT_FUNC" or (mod, nm) not in m.piece_names:
                    continue
                if nm in taken:
                    a = self.act.get(nm)
                    if a and a[0] == "block" and a[1] != key:
                        self.dup_labels.append((nm, a[1], key))
                    continue
                self.act[nm] = ("block", key, v, s or None)
                taken.add(nm)

    def target(self, base, off, why, mod=None):
        """The action for an alias of BASE+OFF, if BASE is defined in the PC link."""
        m = self.m
        r = m.real(base, self.lib)
        if r is None:
            return f"{why}: base {base} is not defined in the PC link"
        if r[0] == "bss":
            return ("alias", base, off, ("data", r[1]), False)
        fn = base in m.cdef and self.is_func(base)
        if not fn and mod and off:
            # An offset past the end of a C object (config/ld/*_aliases.ld names the symbol before the address):
            # on the PC the next object is elsewhere, so the alias goes to the object that holds the address,
            # or, in the linker's fill between two objects (zeros), to a zero object of its own
            A = m.elf_addr.get((mod, base))
            size = next((s for a, s, n, t in m.elf.get(mod, []) if n == base and a == A), 0) if A is not None else 0
            if A is not None and size and off >= size:
                hold = [(d, o) for d, o, s, t in m.cover(mod, A + off) if t != "STT_FUNC" and m.real(d, self.lib)]
                if len(hold) == 1:
                    return self.target(hold[0][0], hold[0][1], why)
                if not m.cover(mod, A + off):
                    nxt = [a for a in m._starts[mod] if a > A + off]
                    gap = (min(nxt) - (A + off)) if nxt else 4
                    return ("bss", mod, gap, A + off)
                return f"{why}: {base}+{off:#x} is past its end, and the address 0x{A + off:X} is in several objects"
        return ("alias", base, off, ("c", r[1]), fn)

    def is_func(self, name):
        return name in self.funcs

    @property
    def funcs(self):
        if self.cdef_fn is None:
            self.cdef_fn = set(self.m.cfuncs)
        return self.cdef_fn

    def resolve(self, n):
        m = self.m
        o = self.over.get(n)
        if o:
            if o[0] == "skip":
                return "skipped by config/pc/data_names.txt"
            if o[0] == "ram":
                return ("alias", "Pal_EeRam", int(o[1], 0) if len(o) > 1 else int(HEXNAME.match(n).group(1), 16), ("ram",), False)
            if o[0] == "alias":
                return self.target(o[1], int(o[2], 0) if len(o) > 2 else 0, "override")
        if n in self.lib:
            return "a library's object (pc/core/lib_data.txt)"
        if n in m.bss:
            mod, size, addr = m.bss[n]
            return ("bss", mod, size, addr)
        if n in self.blobs:
            return ("blob", self.blobs[n])
        if n in m.alias:
            base, off, f = m.alias[n]
            return self.target(base, off, f, f[:-len("_aliases.ld")])
        addrs = {}
        hx = HEXNAME.match(n)
        for mod in m.refs.get(n, ()):
            for Y in dict.fromkeys([mod] + m.lw.get(mod, []) + RESIDENT):
                a = m.msym.get(Y, {}).get(n)
                if a is not None:
                    addrs[mod] = a
                    break
            else:
                if hx:
                    addrs[mod] = int(hx.group(1), 16)
        if not addrs:
            return "no address (a library's name, or not data)"
        if len(set(addrs.values())) > 1:
            return f"several addresses {sorted(hex(a) for a in set(addrs.values()))}"
        A = next(iter(addrs.values()))
        if hx and A >= WINDOWS and set(addrs) <= set(RESIDENT):
            # the resident code names an address of the overlay windows: a buffer in the RAM block
            # (an overlay's function it calls is an override in config/pc/data_names.txt)
            return ("alias", "Pal_EeRam", A, ("ram",), False)
        found = {}
        for mod in addrs:
            for Y in dict.fromkeys([mod] + m.lw.get(mod, []) + RESIDENT):
                for d, off, size, typ in m.cover(Y, A):
                    if d != n and m.real(d, self.lib):
                        found.setdefault((d, off), set()).add(Y)
        if not found:
            if hx and A < 0x2000000 and not any(m.cover(Y, A) for Y in m.modules):
                return ("alias", "Pal_EeRam", A, ("ram",), False)
            for mod in addrs:       # a gap between the .bss objects the splitter labelled: a name the symbol file sized
                lo, hi = self.m.bss_range.get(mod, (0, 0))
                if lo <= A < hi and n in self.m.symsize:
                    return ("bss", mod, self.m.symsize[n], A)
            blocks = {Y for mod in addrs for Y in [mod] + m.lw.get(mod, []) + RESIDENT
                      if m.bss_block.get(Y, (0, 0))[0] <= A < m.bss_block.get(Y, (0, 0))[1]}
            if len(blocks) == 1:    # a name in a module's .bss block that labels no object of its own
                Y = blocks.pop()
                return ("bss", Y, self.m.symsize.get(n) or self.m.nm_size.get((Y, n), 0), A)
            return f"address 0x{A:X} is in no object that the PC link defines"
        res = sorted(found, key=lambda k: (k[1], k[0]))
        resident = [k for k in res if found[k] & set(RESIDENT)]
        pick = resident or res
        if len({k for k in pick}) > 1:
            own = [k for k in pick if found[k] & set(addrs)]
            if len(own) == 1:
                pick = own
        if len(pick) > 1:
            if hx and A < 0x2000000:        # a window address that several overlays cover: the RAM block
                return ("alias", "Pal_EeRam", A, ("ram",), False)
            return f"address 0x{A:X} is covered by several objects: {[k[0] for k in pick][:4]}"
        return self.target(pick[0][0], pick[0][1], "address")


def align_of(addr: int) -> int:
    return min(16, addr & -addr) if addr else 16


def alias_asm(pre, name, base, off, func):
    b = (MANGLE + base) if func else base
    return f'__asm__(".globl {pre}{name}\\n.set {pre}{name}, {pre}{b}+{off}");'


def mangle(path: str) -> str:
    return re.sub(r"[^A-Za-z0-9]", "_", path)


def block_mod(key: str) -> str:
    """A block's module: block keys are the module (its .bss) or "MODULE.data" (DATA_BLOCKS)."""
    return key.split(".")[0]


def block_kind(key: str) -> str:
    return "Data" if key.endswith(".data") else "Bss"


def block_name(key: str) -> str:
    return f"DataC_{block_mod(key)}_{block_kind(key)}"


def block_keys(m) -> list:
    """The blocks in module order, each module's .data block before its .bss block."""
    return sorted(m.bss_block, key=lambda k: (m.modules.index(block_mod(k)), block_kind(k) != "Data"))


PAGE = 0x10000      # the blocks' alignment: the PS2 address's offset in its 64 KiB kept


def win_section(key: str) -> str:
    """The section of block KEY on Windows (COFF takes no alignment above 8192 in an object: pal_align.h). Names of at
    most 8 characters, one for each block (lld truncates longer ones in the image)."""
    return f".dc{'d' if block_kind(key) == 'Data' else 'b'}{block_keys_index[key]}"


block_keys_index: dict = {}
SPR_PS2, SPR_SIZE = 0x70000000, 0x4000     # the scratchpad: Pal_Spr (pc/core/pal_mem.c)


def ram_backed(key: str) -> bool:
    """An overlay's block: its names are Pal_EeRam + their PS2
    address (the window is the RAM block's own range, one memory for every overlay of the window, as on the PS2).
    The block's symbol is Pal_EeRam + (lo - lo % PAGE), so that BLOCK + (lo % PAGE) + (A - lo) holds as for every
    block. COFF defines a symbol plus an offset only in the symbol's own file: these aliases go into pal_mem.c's
    inject header (and layout_check.c, which defines its own Pal_EeRam)."""
    return block_mod(key) not in RESIDENT


def window_homes(m: Matching):
    """The overlays' blocks that overlap on the PS2 (overlays of one window, never loaded together: game's .bss
    0x8F5200-0x8F5D00 inside lobby's 0x849F80-0x8FAE00) are one memory, as on the PS2: the block with the lowest
    address is the home, its array spans their union, and the others are its array plus their offset.
    Since session 6 every overlay's block is RAM-backed (ram_backed): the windows are one memory by construction,
    and no block is grouped here.
    Returns ({module: home} for the blocks laid out in another's, {home: the union's end})."""
    home, span = {}, {}
    cur = None
    for mod in sorted((x for x in m.bss_block if not ram_backed(x)), key=lambda x: m.bss_block[x]):
        lo, hi = m.bss_block[mod]
        if cur is not None and lo < span[cur]:
            home[mod] = cur
            span[cur] = max(span[cur], hi)
        else:
            cur = mod
            span[mod] = hi
    for r in m.bss_block:
        if block_mod(r) in RESIDENT:
            assert all(m.bss_block[r][1] <= m.bss_block[x][0] or m.bss_block[x][1] <= m.bss_block[r][0]
                       for x in m.bss_block if x != r), f"{r}'s block overlaps another"
    return home, span


def emit(plan: Plan, out: Path, pre: str):
    m = plan.m
    elf = not pre
    out.mkdir(parents=True, exist_ok=True)
    files: dict[str, list[str]] = {mod: [] for mod in m.modules}
    alias_data: dict[str, list[str]] = {mod: [] for mod in m.modules}
    inject: dict[str, list[str]] = collections.defaultdict(list)
    # The blocks: 64 KiB aligned, and the PS2 address's offset in its 64 KiB kept, so that every name has the
    # alignment it has on the PS2 (and the same low 16 bits). The blocks of overlays that share a window are
    # one memory (window_homes): the home's array spans them, and every name of them is defined in its file
    # (COFF can only write a symbol plus an offset in the file that defines the symbol).
    home, span = window_homes(m)
    used = {a[1] for a in plan.act.values() if a[0] == "block"}
    plan.ram_lines = []         # the RAM-backed blocks' symbols (pal_mem.c's inject header, layout_check.c)
    plan.sizes = {}             # sizes.txt: what ELF's .size says of each name, on both platforms

    def ram_alias(sym, addr, size, typ="object"):
        line = f".globl {pre}{sym}\\n.set {pre}{sym}, {pre}Pal_EeRam+{addr}"
        if size:
            plan.sizes[sym] = size
        if elf:
            line += f"\\n.type {sym},@{typ}" + (f"\\n.size {sym},{size}" if size else "")
        plan.ram_lines.append(f'__asm__("{line}");')

    def pc_off(mod, addr):
        # the offset of PS2 address ADDR of MOD's block from its home's array
        h = home.get(mod, mod)
        return m.bss_block[h][0] % PAGE + addr - m.bss_block[h][0]
    for key in block_keys(m):
        if key not in used:
            continue
        mod = block_mod(key)
        lo, hi = m.bss_block[key]
        h = home.get(key, key)
        what = ".bss" if block_kind(key) == "Bss" else (
            ".data (the libraries' .data and .rodata, the splitter's pieces: zeros, as a library's initial bytes "
            "never come from the binary; " + ("the C .data between them: the matching C objects' bytes, each word "
                                              "that is an address the PC address of its target)" if key in m.cinit
                                              else "the C .data between them is a hole)"))
        if block_kind(key) == "Bss" and key in m.cinit:
            what += " (its C objects too, at their PS2 offsets)"
        if ram_backed(key):
            files[mod].append(f"/* {mod}'s {what} from 0x{lo:X} to 0x{hi:X}: RAM-backed, a name at PS2 address A is "
                              f"Pal_EeRam + A ({block_name(key)} is Pal_EeRam + 0x{lo - lo % PAGE:X}, defined in "
                              "pal_mem.c's inject header) */")
            for sym, addr, size in ((block_name(key), lo - lo % PAGE, lo % PAGE + hi - lo),
                                    (block_name(key) + "Lo", lo, 0), (block_name(key) + "Hi", hi, 0)):
                ram_alias(sym, addr, size, "object" if size else "notype")
            if key in m.cinit and block_kind(key) == "Data":
                files[mod] += words_asm(plan, key, pre)
            continue
        files[mod].append(f"/* {mod}'s {what} from 0x{lo:X} to 0x{hi:X}: a name at PS2 address A is "
                          f"{block_name(key)} + 0x{lo % PAGE:X} + (A - 0x{lo:X}) */")
        if key in m.cinit and block_kind(key) == "Data":
            files[mod] += init_asm(plan, key, pre, home)
        elif h == key:
            top = span.get(key, hi)
            block_keys_index.setdefault(key, len(block_keys_index))
            if elf:
                files[mod].append(f"unsigned char {block_name(key)}[{lo % PAGE + top - lo}] __attribute__((aligned({PAGE})));")
            else:     # COFF: a section of its own, uninitialised, 64 KiB aligned by the linker (pal_align.h)
                files[mod].append(f"extern unsigned char {block_name(key)}[{lo % PAGE + top - lo}];")
                files[mod].append(f'__asm__(".pushsection {win_section(key)},\\"bw\\"\\n.balign 4096\\n.globl {pre}{block_name(key)}'
                                  f'\\n{pre}{block_name(key)}:\\n.zero {lo % PAGE + top - lo}\\n.popsection");')
            plan.sizes[block_name(key)] = lo % PAGE + top - lo
        else:
            files[mod].append(f"/* One memory with {h}'s .bss, as on the PS2 (overlays of one window): {block_name(key)} "
                              f"is {block_name(h)} + 0x{pc_off(key, lo) - lo % PAGE:X}, defined in data_{block_mod(h)}.c */")
            line = f".globl {pre}{block_name(key)}\\n.set {pre}{block_name(key)}, {pre}{block_name(h)}+{pc_off(key, lo) - lo % PAGE}"
            plan.sizes[block_name(key)] = lo % PAGE + hi - lo
            if elf:
                line += f"\\n.type {block_name(key)},@object\\n.size {block_name(key)},{lo % PAGE + hi - lo}"
            alias_data[block_mod(h)].append(f'__asm__("{line}");')
        # the PC addresses of the block's PS2 range (pal_overlay.c's MH2_OVERLAY_BSS policy)
        for sfx, addr in (("Lo", lo), ("Hi", hi)):
            sym = f"{block_name(key)}{sfx}"
            line = f".globl {pre}{sym}\\n.set {pre}{sym}, {pre}{block_name(h)}+{pc_off(key, addr)}"
            plan.sizes[sym] = 0
            if elf:
                line += f"\\n.type {sym},@notype\\n.size {sym},0"
            alias_data[block_mod(h)].append(f'__asm__("{line}");')
    # A name with no size of its own reaches to the next name of its block (or the block's end): on ELF an alias
    # without .size would take its base's, the whole block's 
    starts = collections.defaultdict(list)
    for n, a in plan.act.items():
        if a[0] == "block":
            starts[a[1]].append(a[2])
    for mod in starts:
        starts[mod] = sorted(set(starts[mod]))
    for n, a in sorted(plan.act.items()):
        if a[0] == "block":
            _, mod, addr, size = a
            lo, hi = m.bss_block[mod]
            if not size:
                i = bisect.bisect_right(starts[mod], addr)
                size = (starts[mod][i] if i < len(starts[mod]) else hi) - addr
            if ram_backed(mod):
                ram_alias(n, addr, size)
                continue
            line = f".globl {pre}{n}\\n.set {pre}{n}, {pre}{block_name(home.get(mod, mod))}+{pc_off(mod, addr)}"
            if size:
                plan.sizes[n] = size
            if elf:
                line += f"\\n.type {n},@object" + (f"\\n.size {n},{size}" if size else "")
            alias_data[block_mod(home.get(mod, mod))].append(f'__asm__("{line}");')
        elif a[0] == "bss":
            _, mod, size, addr = a
            files[mod].append(f"unsigned char {n}[{size}] __attribute__((aligned({align_of(addr)})));")
            plan.sizes[n] = size
        elif a[0] == "blob":
            import data_blob
            mod = a[1]
            addr, size, target, start, vram = data_blob.lookup(mod, n)
            data = (ROOT / target).read_bytes()[addr - vram + start:addr - vram + start + size]
            if len(data) != size:
                sys.exit(f"data_c.py: {n}: {target} is too short")
            body = ",".join(str(b) for b in data)
            files[mod].append(f"unsigned char {n}[{size}] __attribute__((aligned(16))) = {{{body}}};")
            plan.sizes[n] = size
        elif a[0] == "alias":
            _, base, off, where, fn = a
            line = alias_asm(pre, n, base, off, fn)
            if where[0] == "data":
                alias_data[block_mod(home.get(where[1], where[1]))].append(line)
            elif where[0] == "c":
                inject[where[1]].append(line)
            else:
                inject["pc/core/pal_mem.c"].append(line)
    # The C's own definitions of the names a block now lays out (MOVE_C) are weak: the block's alias wins
    for src, names in plan.weak.items():
        inject[src] = [f"#pragma weak {n}" for n in names] + inject[src]
    files["main"] += data_at(m, home) + overlay_words(m)
    inject["pc/core/pal_mem.c"] += plan.ram_lines
    # a word of the moved C data that names a stub would hold the stub's address, not its target's (session 2:
    # Vu1Prog01.. made absolute by the splitter's undefined_syms_auto.txt were stubs)
    stubbed = sorted(plan.targets & set(plan.left))
    if stubbed:
        sys.exit("data_c.py: the moved C data's words name " + ", ".join(f"{n} ({plan.left[n]})" for n in stubbed[:8])
                 + ": names left to the stubs")
    layout_check(plan, out, pre)
    for mod in m.modules:
        text = [f"/* Generated by tools/pc/data_c.py: {mod}'s data objects that have no C definition. Do not edit; never commit. */"]
        text += files[mod] + alias_data[mod]
        put(out / f"data_{mod}.c", "\n".join(text) + "\n")
    cm = ["# Generated by tools/pc/data_c.py. Do not edit."]
    for src, lines in sorted(inject.items()):
        h = out / f"inject_{mangle(src)}.h"
        put(h, "/* Generated by tools/pc/data_c.py: aliases of objects this file defines, and its definitions that "
               "a block of data_MODULE.c lays out at their PS2 offsets (weak). */\n" + "\n".join(lines) + "\n")
        cm.append(f'set(MH2_INJECT_{mangle(src)} "{h.as_posix()}")')
    put(out / "inject.cmake", "\n".join(cm) + "\n")
    put(out / "sizes.txt", "".join(f"{n} {z}\n" for n, z in sorted(plan.sizes.items())))


VARIADIC = None


def variadic(name: str) -> bool:
    """A function the game declares with `...`: cdecl and unmangled on the PC (pc/game_flags.txt)."""
    global VARIADIC
    if VARIADIC is None:
        VARIADIC = set()
        for d in ("include", "src", "pc/core"):
            for f in (ROOT / d).rglob("*.[ch]"):
                t = f.read_text(encoding="utf-8", errors="replace")
                VARIADIC.update(re.findall(r"\b([A-Za-z_]\w*)\s*\([^;{()]*\.\.\.\s*\)", t))
                # a function the game declares as data, `extern T name[];` (include/crt0.h's `__start` is the one
                # today; no C stores its address since PlayerFx_Durations holds numbers): a word that names it
                # names the plain symbol, not the stub's mangled one
                VARIADIC.update(re.findall(r"^\s*extern\s+[A-Za-z_][\w\s]*?\b([A-Za-z_]\w*)\s*\[\s*\]\s*;", t, re.M))
    return name in VARIADIC


def word_expr(plan: Plan, w, pre: str, home, key=None) -> tuple[str, str | None]:
    """A relocated word of block KEY (Matching.cinit): the assembler's expression of its PC value, and the symbol
    it names outside the blocks (None for a block address). A PS2 address is the block's own (an overlay's words
    address its own block or a resident one: overlays of one window overlap on the PS2) or a resident block's."""
    m = plan.m
    if w[0] == "sym":
        name, A = w[1], w[2]
        if A >= 1 << 31:
            A -= 1 << 32
        if name in m.func_names or name in plan.funcs:
            name = name if variadic(name) else MANGLE + name
        return f"{pre}{name}{A:+d}", name
    V = w[1]
    if w[0] == "ram":           # RAM_AREA(V): the RAM block's address
        return f"{pre}Pal_EeRam+{V}", "Pal_EeRam"
    own = [k for k in m.bss_block if key and block_mod(k) == block_mod(key)]
    for k in own + [k for k in m.bss_block if block_mod(k) in RESIDENT]:
        if any(a <= V < b for a, b in m.pieces.get(k, [])):
            if ram_backed(k):
                return f"{pre}Pal_EeRam+{V}", "Pal_EeRam"
            h = home.get(k, k)
            return f"{pre}{block_name(h)}+{m.bss_block[h][0] % PAGE + V - m.bss_block[h][0]}", None
    sys.exit(f"data_c.py: a word of {key}'s moved C data addresses 0x{V:X}, which neither its module's blocks nor a "
             "resident block lays out")


def init_asm(plan: Plan, key: str, pre: str, home) -> list[str]:
    """Block KEY (MOVE_C) as an initialised array in assembly: zeros, the C sections' bytes, and each relocated
    word the PC address of its target (`.long SYMBOL+ADDEND`). plan.targets gets the symbols it names."""
    m = plan.m
    lo, hi = m.bss_block[key]
    data, words = m.cinit[key]
    bn = block_name(key)
    size = lo % PAGE + hi - lo
    block_keys_index.setdefault(key, len(block_keys_index))
    lines = [".data", f".balign {PAGE}", f".globl {pre}{bn}"] if not pre else [
        f'.section {win_section(key)},\\"dw\\"', ".balign 4096", f".globl {pre}{bn}"]
    plan.sizes[bn] = size
    if not pre:
        lines += [f".type {bn},@object", f".size {bn},{size}"]
    lines += [f"{pre}{bn}:", f".zero {lo % PAGE}"]

    def raw(chunk: bytes):
        i = 0
        for z in re.finditer(rb"\x00{16,}", chunk):
            for k in range(i, z.start(), 32):
                lines.append(".byte " + ",".join(str(b) for b in chunk[k:min(k + 32, z.start())]))
            lines.append(f".zero {z.end() - z.start()}")
            i = z.end()
        for k in range(i, len(chunk), 32):
            lines.append(".byte " + ",".join(str(b) for b in chunk[k:k + 32]))
    i = 0
    for P in sorted(words):
        raw(data[i:P - lo])
        e, sym = word_expr(plan, words[P], pre, home, key)
        if sym:
            plan.targets.add(sym)
        lines.append(f".long {e}")
        i = P - lo + 4
    raw(data[i:])
    lines.append(".text")
    return [f'__asm__("' + "\\n".join(lines[k:k + 64]) + '");' for k in range(0, len(lines), 64)]


def words_asm(plan: Plan, key: str, pre: str) -> list[str]:
    """A RAM-backed .data block's words that are addresses (an overlay's): DataC_<m>_Words, pairs (PS2 address, the
    PC value), read-only, sorted by address; the loader writes each value over the disc's image at each load (every
    other byte of the image is the PS2's: the matching build is the disc's). DataC_<m>_WordCount: their number."""
    m = plan.m
    data, words = m.cinit[key]
    mod = block_mod(key)
    sym = f"DataC_{mod}_Words"
    lines = [".section .rdata,\\\"dr\\\"" if pre else ".section .rodata", ".balign 4", f".globl {pre}{sym}"]
    plan.sizes[sym] = 8 * len(words)
    if not pre:
        lines += [f".type {sym},@object", f".size {sym},{8 * len(words)}"]
    lines.append(f"{pre}{sym}:")
    for P in sorted(words):
        e, tgt = word_expr(plan, words[P], pre, {}, key)
        if tgt:
            plan.targets.add(tgt)
        lines.append(f".long 0x{P:X}, {e}")
    lines.append(".text")
    out = [f'__asm__("' + "\\n".join(lines[k:k + 64]) + '");' for k in range(0, len(lines), 64)]
    out.append(f"const unsigned int DataC_{mod}_WordCount = {len(words)};")
    plan.sizes[f"DataC_{mod}_WordCount"] = 4
    return out


def overlay_words(m: Matching) -> list[str]:
    """DataC_OverlayWords (pc/core/data_c.h): an overlay's words table by its OverlayId."""
    mods = [block_mod(k) for k in block_keys(m) if ram_backed(k) and block_kind(k) == "Data" and k in m.cinit]
    t = ["", "/* DataC_OverlayWords (pc/core/data_c.h): the words of an overlay's .data and .rodata that are addresses */",
         "typedef struct DataCWord { unsigned int ps2, value; } DataCWordT;"]
    t += [f"extern const DataCWordT DataC_{x}_Words[]; extern const unsigned int DataC_{x}_WordCount;" for x in mods]
    t += ["const DataCWordT *DataC_OverlayWords(int overlayId, unsigned int *count)", "{",
          "    switch (overlayId) {"]
    t += [f"    case {m.oid[x]}: *count = DataC_{x}_WordCount; return DataC_{x}_Words;   /* {x} */" for x in mods]
    t += ["    default: *count = 0; return 0;", "    }", "}"]
    return t


def resident_ranges(m: Matching, home):
    """DataC_At's table: the PS2 ranges of the resident blocks (main's .data, main's and sub_main's .bss) that the
    splitter's pieces hold, each laid out at its PS2 offset: [(lo, hi, block, the block's PS2 base)]. Holes (a C
    object's bytes, which the PC defines elsewhere) and the overlays' blocks (not resident) are not in it."""
    out = []
    for key in block_keys(m):
        if block_mod(key) not in RESIDENT:
            continue
        h = home.get(key, key)
        base = m.bss_block[h][0] - m.bss_block[h][0] % PAGE
        out += [(a, b, block_name(h), base) for a, b in m.pieces[key]]
    return sorted(out)


def data_at(m: Matching, home):
    """`void *DataC_At(unsigned int ps2)` (pc/core/data_c.h): the host address of a resident PS2 address."""
    R = resident_ranges(m, home)
    blocks = sorted({r[2] for r in R})
    t = ["", "/* DataC_At (pc/core/data_c.h): the host address of PS2 address PS2 in a resident block; Pal_Fatal for any",
         " * other (an overlay's window, a C object's hole, no module's data). */",
         "void Pal_Fatal(const char *what);"]
    t += [f"extern unsigned char {b}[];" for b in blocks]
    t += ["static const struct { unsigned int lo, hi; unsigned char *block; unsigned int base; } DataC_Resident[] = {"]
    t += [f"    {{0x{a:X}, 0x{b:X}, {blk}, 0x{base:X}}}," for a, b, blk, base in R]
    t += ["};",
          "void *DataC_At(unsigned int ps2)",
          "{",
          "    static char msg[] = \"DataC_At: PS2 address 0x00000000 is not resident data at its PS2 offset "
          "(an overlay's, a C object's or no module's)\";",
          "    unsigned int i;",
          "    for (i = 0; i < sizeof(DataC_Resident) / sizeof(DataC_Resident[0]); i++)",
          "        if (ps2 >= DataC_Resident[i].lo && ps2 < DataC_Resident[i].hi)",
          "            return DataC_Resident[i].block + (ps2 - DataC_Resident[i].base);",
          "    for (i = 0; i < 8; i++)",
          "        msg[24 + 7 - i] = \"0123456789ABCDEF\"[(ps2 >> (4 * i)) & 15];",
          "    Pal_Fatal(msg);",
          "    return 0;",
          "}"]
    # DataC_Ps2Of, DataC_At's inverse: over every block's home array (resident or an overlay's, at its PS2
    # offsets) and the RAM block (Pal_EeRam: PS2 address = the offset)
    pre = "DataC_Ps2Of: host pointer 0x"
    span = window_homes(m)[1]
    spans = [(m.bss_block[k][0], span.get(k, m.bss_block[k][1]), block_name(k)) for k in block_keys(m)
             if home.get(k, k) == k and not ram_backed(k)]
    t += ["",
          "/* DataC_Ps2Of (pc/core/data_c.h): the PS2 address of a host pointer into a block, the RAM block or the",
          " * scratchpad (Pal_Spr, pal_mem.c: PS2 0x70000000). */",
          "extern unsigned char Pal_EeRam[];",
          f"extern unsigned char Pal_Spr[0x{SPR_SIZE:X}];",
          *[f"extern unsigned char {blk}[];" for a_, b_, blk in spans if blk not in blocks],
          "static const struct { unsigned int lo, hi; unsigned char *block; } DataC_Blocks[] = {"]
    t += [f"    {{0x{a:X}, 0x{b:X}, {blk}}}," for a, b, blk in spans]
    t += ["};",
          "unsigned int DataC_Ps2Of(const void *p)",
          "{",
          f"    static char msg[] = \"{pre}00000000 is in no data block, the RAM block or the scratchpad\";",
          "    const unsigned char *q = (const unsigned char *)p;",
          "    unsigned int i;",
          "    for (i = 0; i < sizeof(DataC_Blocks) / sizeof(DataC_Blocks[0]); i++) {",
          f"        const unsigned char *h = DataC_Blocks[i].block + DataC_Blocks[i].lo % 0x{PAGE:X};",
          "        if (q >= h && q < h + (DataC_Blocks[i].hi - DataC_Blocks[i].lo))",
          "            return DataC_Blocks[i].lo + (unsigned int)(q - h);",
          "    }",
          "    if (q >= Pal_EeRam && q < Pal_EeRam + 0x2000000)",
          "        return (unsigned int)(q - Pal_EeRam);",
          "    if (q >= Pal_Spr && q < Pal_Spr + sizeof(Pal_Spr))",
          f"        return 0x{SPR_PS2:X}u + (unsigned int)(q - Pal_Spr);",
          "    for (i = 0; i < 8; i++)",
          f"        msg[{len(pre)} + 7 - i] = \"0123456789ABCDEF\"[((unsigned long)p >> (4 * i)) & 15];",
          "    Pal_Fatal(msg);",
          "    return 0;",
          "}"]
    # DataC_TryAt: DataC_At without the stop, over all the core's memory: a resident block, else the RAM block for an
    # address of no resident block (an overlay's window is the RAM block's: its blocks are RAM-backed) but a resident
    # block's hole (NULL), and the scratchpad (Pal_Spr)
    t += ["",
          "/* DataC_TryAt (pc/core/data_c.h): the host address of a PS2 address, or 0 for one that isn't resident. */",
          "void *DataC_TryAt(unsigned int ps2)",
          "{",
          "    unsigned int i;",
          "    for (i = 0; i < sizeof(DataC_Resident) / sizeof(DataC_Resident[0]); i++)",
          "        if (ps2 >= DataC_Resident[i].lo && ps2 < DataC_Resident[i].hi)",
          "            return DataC_Resident[i].block + (ps2 - DataC_Resident[i].base);",
          "    for (i = 0; i < sizeof(DataC_Blocks) / sizeof(DataC_Blocks[0]); i++)",
          "        if (ps2 >= DataC_Blocks[i].lo && ps2 < DataC_Blocks[i].hi)",
          "            return 0;",
          "    if (ps2 < 0x2000000)",
          "        return Pal_EeRam + ps2;",
          f"    if (ps2 >= 0x{SPR_PS2:X}u && ps2 - 0x{SPR_PS2:X}u < sizeof(Pal_Spr))",
          f"        return Pal_Spr + (ps2 - 0x{SPR_PS2:X}u);",
          "    return 0;",
          "}"]
    return t


# Pairs of names that were separate memories before the blocks (the test prints their distances):
# gSysWork's packet buffers (Packet_Alloc returned 0), partition 3's AFS record and the next (io's VBlank 67),
# partition 0's record and gStreamChan, where its table's overrun lands on the PS2 (io's session 4)
ADDR_CHECKS = [(0x19B410, "ADXF's first handle record (16 of 0x44 bytes)"), (0x19B410 + 15 * 0x44 + 0x43, "ADXF's last handle byte"),
               (0x19DF20, "the original's ADXT handle")]
LAYOUT_ANCHORS = [("gSysWork", "gPacketBlocks"), ("D_002DF850", "D_002E2D70"), ("D_002E3340", "gStreamChan"),
                  ("gTexRegPacket", "gTexRegTexa"), ("gTexRegTexa", "gTexRegTexaAddr")]  # gfx's G 121: a PATH2 packet


def layout_check(plan: Plan, out: Path, pre: str):
    """layout_check.c, the ctest data_layout (linked with the data_MODULE.c files alone): every name of a block
    is at its PS2 address's offset from the block's start, with the PS2 address's low 12 bits."""
    m = plan.m
    names = sorted((a[1], a[2], n) for n, a in plan.act.items() if a[0] == "block")
    mods = [k for k in block_keys(m) if k in {mod for mod, addr, n in names}]
    home, span = window_homes(m)
    R = resident_ranges(m, home)

    def resident(A):
        return any(a <= A < b for a, b, blk, base in R)
    text = ["/* Generated by tools/pc/data_c.py: the blocks' layout (ctest data_layout). Do not edit; never commit. */",
            "#include <setjmp.h>", "#include <stdint.h>", "#include <stdio.h>", "#include <stdlib.h>",
            "void *DataC_At(unsigned int ps2);",
            "unsigned int DataC_Ps2Of(const void *p);",
            "extern unsigned char Pal_EeRam[];",
            "static jmp_buf fatal_jb;",
            "static int fatal_armed;",
            "void Pal_Fatal(const char *what)",
            "{",
            "    printf(\"data_layout: Pal_Fatal: %s\\n\", what);",
            "    if (fatal_armed)",
            "        longjmp(fatal_jb, 1);",
            "    printf(\"data_layout: an unexpected Pal_Fatal\\n\");",
            "    _Exit(1);",
            "}",
            "/* DataC_At(ps2) stops (Pal_Fatal) for an address that is not resident data at its PS2 offset */",
            "static int stops(unsigned int ps2)",
            "{",
            "    volatile int r = 0;",
            "    fatal_armed = 1;",
            "    if (setjmp(fatal_jb) == 0)",
            "        DataC_At(ps2);",
            "    else",
            "        r = 1;",
            "    fatal_armed = 0;",
            "    return r;",
            "}"]
    # the symbols the moved C data's words name that the data files don't define (functions, the C's .bss, the
    # stubs): defined here, so that the data files link alone
    own = {n for n, x in plan.act.items() if x[0] in ("block", "bss", "blob") or (x[0] == "alias" and x[3][0] == "data")}
    text += [f'__asm__(".data\\n.globl {pre}{s}\\n{pre}{s}:\\n.long 0\\n.text");' for s in sorted(plan.targets - own - {"Pal_EeRam"})]
    # the RAM block (pal_mem.c's), whole: DataC_Ps2Of's RAM range must not overlap the blocks here either
    if not pre:
        text += ["unsigned char Pal_EeRam[0x2000000] __attribute__((aligned(65536)));",
                 f"unsigned char Pal_Spr[0x{SPR_SIZE:X}] __attribute__((aligned(65536)));"]
    else:       # COFF: a section of its own each (pal_align.h)
        text += ["extern unsigned char Pal_EeRam[0x2000000];",
                 f"extern unsigned char Pal_Spr[0x{SPR_SIZE:X}];",
                 '__asm__(".pushsection .ps2ram,\\"bw\\"\\n.balign 4096\\n.globl _Pal_EeRam\\n_Pal_EeRam:\\n.zero 0x2000000\\n.popsection");',
                 f'__asm__(".pushsection .ps2spr,\\"bw\\"\\n.balign 4096\\n.globl _Pal_Spr\\n_Pal_Spr:\\n.zero 0x{SPR_SIZE:X}\\n.popsection");']
    text += plan.ram_lines      # the RAM-backed blocks' names (COFF: defined in the file of Pal_EeRam)
    text += [f"extern unsigned char {block_name(mod)}[], {block_name(mod)}Lo[], {block_name(mod)}Hi[];" for mod in mods]
    text += [f"extern unsigned char {n}[];" for mod, addr, n in names]
    text.append("static const struct { const char *name; const unsigned char *p; int block; uint32_t ps2; int at; } T[] = {")
    text += [f'    {{"{n}", {n}, {mods.index(mod)}, 0x{addr:X}, {int(block_mod(mod) in RESIDENT and resident(addr))}}},'
             for mod, addr, n in names]
    text.append("};")
    text.append("static const struct { const char *name; const unsigned char *p; uint32_t lo, hi; } B[] = {")
    text += [f'    {{"{block_name(mod)}", {block_name(mod)}, 0x{m.bss_block[mod][0]:X}, 0x{m.bss_block[mod][1]:X}}},'
             for mod in mods]
    anchors = [(x, y, plan.act[y][2] - plan.act[x][2]) for x, y in LAYOUT_ANCHORS
               if plan.act.get(x, ("",))[0] == "block" and plan.act.get(y, ("",))[0] == "block"]
    text += ["};", "",
             "int main(void)",
             "{",
             "    unsigned i, bad = 0;"]
    for x, y, d in anchors:
        text += [f"    if ((uintptr_t)({y} - {x}) != 0x{d:X}u)",
                 f"        bad++, printf(\"data_layout: {y} - {x} is 0x%X, not 0x{d:X}\\n\", (unsigned)({y} - {x}));",
                 "    else",
                 f"        printf(\"data_layout: {y} - {x} = 0x{d:X}, as on the PS2\\n\");"]
    # the libraries' handles at the original's addresses (ADXT_Create's return, kept in gStreamAdxt[0]; ADXF's
    # records): main's .data block
    for A, why in ADDR_CHECKS:
        key = next((k for k in mods if m.bss_block[k][0] <= A < m.bss_block[k][1]), None)
        if key is None:
            continue
        lo = m.bss_block[key][0]
        bn = block_name(key)
        text += [f"    if ((unsigned char *)DataC_At(0x{A:X}) != {bn} + 0x{lo % PAGE:X} + 0x{A - lo:X})",
                 f"        bad++, printf(\"data_layout: DataC_At(0x{A:X}) is not {bn} + 0x{lo % PAGE + A - lo:X}\\n\");",
                 "    else",
                 f"        printf(\"data_layout: {why} 0x{A:X} is {bn} + 0x{lo % PAGE + A - lo:X}\\n\");"]
    # DataC_At stops for an address that isn't resident data at its PS2 offset: the first byte past each resident
    # range, the first byte of each hole and of each overlay block, and 0
    stop = {0}
    for a, b, blk, base in R:
        if not resident(b):
            stop.add(b)
    for k in mods:
        if block_mod(k) not in RESIDENT:
            stop.add(m.bss_block[k][0])
    stop = sorted(stop)
    text += [f"    static const uint32_t S[] = {{{', '.join(f'0x{x:X}' for x in stop)}}};",
             "    for (i = 0; i < sizeof(S) / sizeof(S[0]); i++)",
             "        if (!stops(S[i]))",
             "            bad++, printf(\"data_layout: DataC_At(0x%X) did not stop\\n\", (unsigned)S[i]);",
             f"    printf(\"data_layout: DataC_At stops for {len(stop)} addresses that are not resident data, "
             f"and maps {len(R)} resident ranges\\n\");"]
    # DataC_Ps2Of inverts DataC_At on every resident range (its first and last byte), maps the RAM block, and
    # stops for a pointer in neither (a local's address)
    # DataC_TryAt: equal to DataC_At on the resident ranges, 0 in a hole or an overlay's block, the RAM block
    # elsewhere below 32 MB, and DataC_Ps2Of its inverse there
    text += ["    {",
             "        void *DataC_TryAt(unsigned int ps2);",
             f"        static const uint32_t RT[] = {{{', '.join(f'0x{a:X}, 0x{b - 1:X}' for a, b, blk, base in R)}}};",
             f"        static const uint32_t ST[] = {{{', '.join(f'0x{x:X}' for x in stop if x)}}};",
             "        for (i = 0; i < sizeof(RT) / sizeof(RT[0]); i++)",
             "            if (DataC_TryAt(RT[i]) != DataC_At(RT[i]) || DataC_Ps2Of(DataC_TryAt(RT[i])) != RT[i])",
             "                bad++, printf(\"data_layout: DataC_TryAt(0x%X) is not DataC_At's\\n\", (unsigned)RT[i]);",
             "        for (i = 0; i < sizeof(ST) / sizeof(ST[0]); i++)",
             "            if (DataC_TryAt(ST[i]) != 0 && (ST[i] >= 0x2000000 || DataC_TryAt(ST[i]) != Pal_EeRam + ST[i]))",
             "                bad++, printf(\"data_layout: DataC_TryAt(0x%X) is neither 0 nor the RAM block\\n\", (unsigned)ST[i]);",
             "        if (DataC_TryAt(0x100000) != Pal_EeRam + 0x100000 || DataC_Ps2Of(DataC_TryAt(0x100000)) != 0x100000"
             " || DataC_TryAt(0x2000000) != 0)",
             "            bad++, printf(\"data_layout: DataC_TryAt is not the RAM block's below 32 MB\\n\");",
             f"        if (DataC_TryAt(0x{SPR_PS2:X}) != Pal_Spr || DataC_TryAt(0x{SPR_PS2 + SPR_SIZE - 1:X}) != Pal_Spr + 0x{SPR_SIZE - 1:X}"
             f" || DataC_TryAt(0x{SPR_PS2 + SPR_SIZE:X}) != 0 || DataC_TryAt(0x{SPR_PS2 - 1:X}) != 0"
             f" || DataC_Ps2Of(Pal_Spr) != 0x{SPR_PS2:X} || DataC_Ps2Of(Pal_Spr + 0x{SPR_SIZE - 1:X}) != 0x{SPR_PS2 + SPR_SIZE - 1:X})",
             "            bad++, printf(\"data_layout: DataC_TryAt and DataC_Ps2Of do not map the scratchpad\\n\");",
             "        else",
             f"            printf(\"data_layout: the scratchpad 0x{SPR_PS2:X}-0x{SPR_PS2 + SPR_SIZE:X} is Pal_Spr both ways\\n\");",
             "        printf(\"data_layout: DataC_TryAt equals DataC_At on the resident ranges, and DataC_Ps2Of inverts it\\n\");",
             "    }"]
    text += [f"    static const uint32_t RR[] = {{{', '.join(f'0x{a:X}, 0x{b - 1:X}' for a, b, blk, base in R)}}};",
             "    for (i = 0; i < sizeof(RR) / sizeof(RR[0]); i++)",
             "        if (DataC_Ps2Of(DataC_At(RR[i])) != RR[i])",
             "            bad++, printf(\"data_layout: DataC_Ps2Of(DataC_At(0x%X)) is 0x%X\\n\", (unsigned)RR[i], DataC_Ps2Of(DataC_At(RR[i])));",
             "    if (DataC_Ps2Of(Pal_EeRam + 0x100000) != 0x100000 || DataC_Ps2Of(Pal_EeRam + 0x1FFFFFF) != 0x1FFFFFF)",
             "        bad++, printf(\"data_layout: DataC_Ps2Of is not the RAM block's offset\\n\");",
             "    {",
             "        volatile int r = 0, local = 0;",
             "        fatal_armed = 1;",
             "        if (setjmp(fatal_jb) == 0)",
             "            DataC_Ps2Of((const void *)&local);",
             "        else",
             "            r = 1;",
             "        fatal_armed = 0;",
             "        if (!r)",
             "            bad++, printf(\"data_layout: DataC_Ps2Of did not stop for a local's address\\n\");",
             "    }",
             f"    printf(\"data_layout: DataC_Ps2Of inverts DataC_At on {len(R)} resident ranges, maps the RAM block, "
             "and stops for a local's address\\n\");"]
    for mod in mods:
        lo, hi = m.bss_block[mod]
        bn = block_name(mod)
        text += [f"    if ({bn}Lo != {bn} + 0x{lo % PAGE:X} || {bn}Hi != {bn}Lo + 0x{hi - lo:X})",
                 f"        bad++, printf(\"data_layout: {bn}Lo/Hi are not its PS2 range\\n\");"]
        h = home.get(mod)
        if h in mods:
            hl = m.bss_block[h][0]
            text += [f"    if ({bn} + 0x{lo % PAGE:X} != {block_name(h)} + 0x{hl % PAGE:X} + 0x{lo - hl:X})",
                     f"        bad++, printf(\"data_layout: {mod}'s .bss is not {h}'s memory\\n\");",
                     "    else",
                     f"        printf(\"data_layout: {mod}'s .bss 0x{lo:X}-0x{hi:X} is {h}'s memory, as on the PS2\\n\");"]
    # The resident blocks must exist and hold names: a build without them (the Windows copy without the maps)
    # would otherwise pass with an empty table
    for key in [*RESIDENT, *(f"{x}.data" for x in DATA_BLOCKS)]:
        k = sum(1 for mod, addr, n in names if mod == key)
        if key not in mods or k == 0:
            text += [f"    bad++, printf(\"data_layout: {block_name(key)} is missing or holds no name\\n\");"]
        else:
            text += [f"    printf(\"data_layout: {block_name(key)} holds {k} names\\n\");"]
    text += [
             "    for (i = 0; i < sizeof(T) / sizeof(T[0]); i++) {",
             "        uint32_t lo = B[T[i].block].lo;",
             f"        const unsigned char *want = B[T[i].block].p + lo % 0x{PAGE:X} + (T[i].ps2 - lo);",
             f"        if (T[i].p != want || ((uintptr_t)T[i].p & 0x{PAGE - 1:X}) != (T[i].ps2 & 0x{PAGE - 1:X})"
             " || T[i].ps2 < lo || T[i].ps2 >= B[T[i].block].hi) {",
             "            if (bad++ < 10)",
             "                printf(\"data_layout: %s (PS2 0x%X) is at block + 0x%X\\n\", T[i].name, (unsigned)T[i].ps2,",
             "                       (unsigned)(T[i].p - B[T[i].block].p));",
             "        }",
             "        if (DataC_Ps2Of(T[i].p) != T[i].ps2 && bad++ < 10)",
             "            printf(\"data_layout: DataC_Ps2Of(%s) is 0x%X\\n\", T[i].name, DataC_Ps2Of(T[i].p));",
             "        if (T[i].at && (const unsigned char *)DataC_At(T[i].ps2) != T[i].p && bad++ < 10)",
             "            printf(\"data_layout: DataC_At(0x%X) is not %s\\n\", (unsigned)T[i].ps2, T[i].name);",
             "    }",
             "    printf(\"data_layout: %u names in %u blocks, %u not at their PS2 offset\\n\", i,",
             "           (unsigned)(sizeof(B) / sizeof(B[0])), bad);",
             "    return bad != 0;",
             "}"]
    put(out / "layout_check.c", "\n".join(text) + "\n")


def put(path: Path, text: str):
    """Writes the file only if its content changes: a new timestamp rebuilds every file that includes it."""
    data = text.encode("utf-8")
    if not path.exists() or path.read_bytes() != data:
        path.write_bytes(data)


def report(plan: Plan, out: Path):
    cnt = collections.Counter()
    demand = set(plan.demand)
    for n, a in plan.act.items():
        if a[0] == "block":
            cnt["block" if n in demand else "block, unnamed by the C"] += 1
        else:
            cnt[a[0] if a[0] != "alias" else "alias " + a[3][0]] += 1
    lines = [f"{k}: {v}" for k, v in sorted(cnt.items())]
    home, span = window_homes(plan.m)
    for mod, (lo, hi) in plan.m.bss_block.items():
        lines.append(f"  block {block_name(mod)}: 0x{lo:X}-0x{hi:X} ({hi - lo} bytes)" +
                     (f", in {block_name(home[mod])}: one memory, as on the PS2" if mod in home else ""))
    for key, st in plan.m.cstatic.items():
        lines.append(f"  {block_name(key)}: {len(plan.m.cnames and [n for n in plan.m.cnames if plan.m.cnames[n][0] == key])} "
                     f"C names at their PS2 offsets; {len(st)} file-static objects keep the PC's copy (holes in it):")
        lines += [f"    0x{a:X}-0x{b:X} {n} ({o})" for a, b, n, o in st]
    for pat, hits in plan.ps2_rows.items():
        lines.append(f"  ps2-words {pat}: {len(hits)} C objects, {sum(k for n, k in hits)} words that are block addresses")
    for key, tb in plan.m.textbins.items():
        for a, sz, o in tb:
            k = sum(1 for n, (kk, A) in plan.m.textnames.items() if kk == key and a <= A < a + sz)
            lines.append(f"  {block_name(key)}: {o}'s .text 0x{a:X}-0x{a + sz:X} (the matching bytes), {k} names")
    for nm, first, other in plan.dup_labels:
        lines.append(f"  {nm}: in the blocks of {first} and {other}; defined in {first}'s")
    lines.append(f"left (stubs): {len(plan.left)}")
    why = collections.Counter(re.sub(r"0x[0-9A-F]+|\[.*\]", "N", w) for w in plan.left.values())
    lines += [f"  {v}  {k}" for k, v in why.most_common()]
    lines.append("")
    lines += [f"{n}\t{plan.left[n]}\trefs={','.join(sorted(plan.m.refs.get(n, ())))}" for n in sorted(plan.left)]
    (out / "report.txt").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print("data_c: " + ", ".join(f"{k} {v}" for k, v in sorted(cnt.items())) + f"; {len(plan.left)} left to the stubs")


def bss_holes(m: Matching) -> dict:
    """The C .bss objects inside each module's block (the matching map's .bss input sections that are not the
    splitter's pieces): the block keeps their bytes as a hole, while the PC defines them elsewhere."""
    return m.holes


def extents(plan: Plan):
    """For every data name the PC link defines and the C reaches: (pc, ps2, how).
    pc: the bytes from the name's address to the end of the PC object that holds it, as one memory with the
    PS2's (for a block name, up to the block's end or the next C .bss object inside it); None for the RAM block.
    ps2: the bytes the matching build gives the name: its symbol's size, else up to the next name's address (or
    the end of the object that holds it)."""
    m = plan.m
    plan.size_past_holder = []
    holes = bss_holes(m)
    mod_of = {src: mod for mod, src, obj in m.objs}
    size_of = {}                # (module, name) -> the matching ELF's size
    for mod in m.elf:
        for v, s, n, t in m.elf[mod]:
            if t != "STT_FUNC" and s:
                size_of.setdefault((mod, n), s)

    def holder_end(mod, A, own):
        """The end of the nearest matching object (not OWN) that holds address A, or None."""
        L = m.elf.get(mod) or []
        i = bisect.bisect_right(m._starts[mod], A)
        ends = [a + s for a, s, n, t in L[max(0, i - 16):i] if a <= A < a + s and n != own and t != "STT_FUNC"]
        return min(ends) if ends else None

    def boundary(mod, A, own=None):
        L = m.elf.get(mod) or []
        i = bisect.bisect_right(m._starts[mod], A)
        e = holder_end(mod, A, own)
        nxt = m._starts[mod][i] if i < len(L) else None
        cands = ([e] if e is not None else []) + ([nxt] if nxt is not None else [])
        return (min(cands) - A) if cands else None

    def where(n):
        """(module, PS2 address, pc extent) of a plan name."""
        a = plan.act[n]
        k = a[0]
        if k == "block":
            mod, A = a[1], a[2]
            lo, hi = m.bss_block[mod]
            end = min([hi] + [h0 for h0, h1 in holes.get(mod, []) if h0 > A])
            return block_mod(mod), A, end - A
        if k == "bss":
            return a[1], a[3], a[2]
        if k == "blob":
            return None
        if k == "alias":
            base, off, wh, fn = a[1], a[2], a[3], a[4]
            if wh[0] == "ram" or fn:
                return None
            if wh[0] == "data":
                b = where(base)
                return (b[0], b[1] + off, b[2] - off) if b else None
            mod = mod_of.get(wh[1])
            if mod is None or (mod, base) not in m.elf_addr or (mod, base) not in size_of:
                return None
            return mod, m.elf_addr[(mod, base)] + off, size_of[(mod, base)] - off
        return None

    out = {}
    for n, a in plan.act.items():
        w = where(n)
        if w is None:
            continue
        mod, A, pc = w
        own = (a[3] if a[0] == "block" else a[2] if a[0] == "bss" else None) or m.symsize.get(n) \
            or m.nm_size.get((mod, n)) or size_of.get((mod, n))
        if own and a[0] == "alias" and a[3][0] == "c" and own > pc > 0:
            # a name inside a C object whose size (the symbol file's, up to the next label) reaches past the
            # object's end, into the alignment's fill: the object ends the name (gArenaEmPenHelp)
            plan.size_past_holder.append((n, own, pc))
            own = pc
        if own:
            out[n] = (pc, own, "size")
        else:
            b = boundary(mod, A, n)
            out[n] = (pc, b, "next name") if b else (pc, None, "")
    # names the C defines: their PC object is the C object (the same type, so the matching build's size)
    for n, src in m.cdef.items():
        mod = mod_of.get(src)
        if n not in out and src not in m.broken and (mod, n) in size_of:
            out[n] = (size_of[(mod, n)], size_of[(mod, n)], "C object")
    return out


def write_cases(plan: Plan, out: Path):
    """cases.json: what `--cases` checks (ctest data_cases). Per game TU (module/file name), the names its
    matching object references that have a PC extent; per name, that extent; and the names whose matching
    build's extent (their symbol's size or up to the next name: what an `extern T X[];` can't say) is larger."""
    import json
    ext = extents(plan)
    tus = {}
    for mod, src, obj in plan.m.objs:
        names = sorted(n for n in plan.m.undef_by_src.get(src, ()) if n in ext and ext[n][0] is not None)
        tus[f"{mod}/{Path(src).name}"] = names
    plan_cases = sorted([n, pc, ps2, how] for n, (pc, ps2, how) in ext.items()
                        if pc is not None and ps2 is not None and ps2 > pc)
    data = {"names": {n: e[0] for n, e in sorted(ext.items()) if e[0] is not None},
            "tus": tus, "plan_cases": plan_cases,
            "unsized": sorted(n for n, e in ext.items() if e[1] is None),
            "size_past_holder": sorted(list(x) for x in plan.size_past_holder)}
    put(out / "cases.json", json.dumps(data, indent=0, sort_keys=True) + "\n")
    # moved.json: each moved C name's bytes in its block and its relocated words (offset -> [symbol, addend], or
    # [null, PS2 address] for a block address), for the check against the PC's own (weak) definition
    moved = {}
    for n, (key, A, size, src) in sorted(plan.m.cnames.items()):
        if n in plan.blobs:         # INCLUDE_DATA: the PC's C doesn't define it (the block's bytes are the binary's)
            continue
        img, words = plan.m.cinit[key]
        lo = plan.m.bss_block[key][0]
        ws = {}
        for P in range(A - A % 4, A + size, 4):
            w = words.get(P)
            if w is None:
                continue
            e, sym = word_expr(plan, w, "", {}, key)
            ws[P - A] = [sym, int(re.search(r"[+-]\d+$", e).group(0))] if sym and w[0] != "ps2" else [None, w[1]]
            if P in plan.ps2_words:         # a ps2-words row's word: the PC's C has the PS2 number (check_moved)
                ws[P - A].append("ps2-word")
        moved[n] = {"src": src, "tu": f"{block_mod(key)}/{Path(src).name}", "size": size, "bytes": img[A - lo:A - lo + size].hex(), "words": ws}
    put(out / "moved.json", json.dumps(moved, indent=0, sort_keys=True) + "\n")
    # code_words.json: the words that address code labels (MWCC's jump tables), which keep the PS2's values: per
    # block, its symbol, lo, whether RAM-backed (an overlay's: then its words table must not name them) and the
    # words [PS2 address, function, addend, matching object, @N holder, PS2 value]
    cw = {}
    for key, L in sorted(plan.m.codewords.items()):
        if L:
            cw[key] = {"mod": block_mod(key), "block": block_name(key), "lo": plan.m.bss_block[key][0],
                       "ram": ram_backed(key), "words": [list(x) for x in L]}
    put(out / "code_words.json", json.dumps(cw, indent=0, sort_keys=True) + "\n")


def write_align(plan: Plan, out: Path):
    """align.json: every data name of a module's own sections (the matching map: .data, .rodata, .bss, and the
    textbin pieces) that is 16-aligned on the PS2, with its PS2 address, module and kind, for `--align` (ctest
    data_align): block (a block lays it out), resident (main's or sub_main's), lib (a library's object the platform
    layer defines: pc/core/lib_data.txt), overlay (an overlay's name no block lays out)."""
    import json
    m = plan.m
    names = {}
    for mod in m.modules:
        for A, n in m.align16[mod]:
            if n in names:
                continue
            a = plan.act.get(n)
            kind = ("block" if a and a[0] == "block" else "lib" if n in plan.lib else
                    "resident" if mod in RESIDENT else "overlay")
            names[n] = [A, mod, kind]
    put(out / "align.json", json.dumps(names, indent=0, sort_keys=True) + "\n")


def run_align(gen: Path, core: Path, nm: str, platform: str) -> int:
    """ctest data_align: a name 16-aligned on the PS2 must be 16-aligned in the core, whatever defines it: a block's name, a resident module's, a library
    object's, an overlay's (their C data in their blocks since session 6). Returns the exit status."""
    import json
    want = json.loads((gen / "align.json").read_text())
    r = subprocess.run([nm, str(core)], capture_output=True, text=True)
    if r.returncode or not r.stdout:
        print(f"data_align: {nm} {core} failed: {r.stderr.strip()[:200]}")
        return 1
    host = {}
    for line in r.stdout.splitlines():
        f = line.split()
        if len(f) == 3 and f[1] in "dDbBrRvV":
            n = f[2][1:] if platform == "windows" and f[2].startswith("_") else f[2]
            host.setdefault(n, int(f[0], 16))
    cnt = collections.Counter()
    bad = []
    for n, (A, mod, kind) in sorted(want.items()):
        if n not in host:
            cnt["absent"] += 1
            continue
        cnt[kind] += 1
        if host[n] % 16:
            line = f"{n} ({mod}, {kind}): PS2 0x{A:X}, core 0x{host[n]:X}"
            bad.append(line)
    print(f"data_align: {sum(cnt[k] for k in ('block', 'resident', 'lib', 'overlay'))} names 16-aligned on the PS2 "
          f"and in the core (block {cnt['block']}, resident {cnt['resident']}, lib {cnt['lib']}, overlay "
          f"{cnt['overlay']}; {cnt['absent']} not in the core): {len(bad)} are not")
    for e in bad[:20]:
        print(f"ALIGN {e}")
    print(f"data_align: {'OK' if not bad else 'FAILED'}")
    return 1 if bad else 0


PROBE_ERR = re.compile(r"^(.*?):(\d+):\d+: (error|note): (.*)$")


def probe_tu(job):
    """One game TU's probe: the TU, then a static assertion per name that its sizeof fits the PC object
    (clang -fsyntax-only). Cached by the probe's text and command and its dependencies' times and sizes."""
    import hashlib
    import json
    tag, src, cmd, names, ext, pdir = job
    pc = pdir / f"{tag}.c"
    dep = pdir / f"{tag}.d"
    cache = pdir / f"{tag}.json"
    text = f'#include "{src}"\n' + "".join(f'_Static_assert(sizeof({n}) <= {ext[n]}u, "{n}");\n' for n in names)
    put(pc, text)
    key = hashlib.sha256(("\0".join(cmd) + "\0" + text).encode()).hexdigest()
    if cache.exists():
        try:
            c = json.loads(cache.read_text())
            if c["key"] == key and all(os.stat(p).st_mtime_ns == t and os.stat(p).st_size == s for p, t, s in c["deps"]):
                return c, True
        except (OSError, ValueError, KeyError):
            pass
    r = subprocess.run(cmd + ["-fsyntax-only", "-ferror-limit=0", "-MD", "-MF", str(dep), str(pc)],
                       capture_output=True, text=True)
    res = {"key": key, "cases": [], "incomplete": [], "undeclared": [], "errors": [], "deps": []}
    last = None
    for line in r.stderr.splitlines():
        mm = PROBE_ERR.match(line)
        if not mm:
            continue
        f, ln, kind, msg = mm.group(1), int(mm.group(2)), mm.group(3), mm.group(4)
        if f != str(pc) or not 2 <= ln < 2 + len(names):
            if kind == "error":
                res["errors"].append(line[:300])
            continue
        n = names[ln - 2]
        if kind == "error" and "static assertion failed" in msg:
            res["cases"].append([n, None, ext[n]])
            last = res["cases"][-1]
        elif kind == "note" and last is not None and last[0] == n:
            v = re.search(r"evaluates to '(\d+) <= (\d+)'", msg)
            if v:
                last[1] = int(v.group(1))
        elif kind == "error" and "incomplete type" in msg:
            res["incomplete"].append(n)
        elif kind == "error" and "undeclared identifier" in msg:
            res["undeclared"].append(n)
        elif kind == "error":
            res["errors"].append(line[:300])
    if r.returncode != 0 and not (res["cases"] or res["incomplete"] or res["undeclared"] or res["errors"]):
        res["errors"].append(r.stderr.strip()[:300] or f"clang exited {r.returncode}")
    if dep.exists():
        files = re.sub(r"\\\n", " ", dep.read_text()).split(":", 1)[1].split()
        res["deps"] = [[p, os.stat(p).st_mtime_ns, os.stat(p).st_size] for p in dict.fromkeys(files) if os.path.exists(p)]
    if not res["errors"]:
        cache.write_text(json.dumps(res) + "\n")
    elif cache.exists():
        cache.unlink()
    return res, False


def check_moved(build: Path, gen: Path, objpath: dict) -> list[str]:
    """The C names a block lays out (MOVE_C) against the PC's own definitions (weak, in the game objects): the
    same size, the same bytes, and each relocated word the same symbol and addend. Returns the differences."""
    import json
    from elftools.elf.elffile import ELFFile
    from elftools.elf.relocation import RelocationSection
    p = gen / "moved.json"
    if not p.exists():
        return [f"{p} is missing (data_c.py writes it at configure time)"]
    moved = json.loads(p.read_text())
    by_obj = collections.defaultdict(list)
    bad = []
    for n, v in moved.items():
        if v["tu"] not in objpath:
            bad.append(f"{n}: no PC object for {v['src']}")
        else:
            by_obj[objpath[v["tu"]]].append(n)
    words_checked = 0
    ps2_numbers = 0
    for obj, names in sorted(by_obj.items()):
        e = ELFFile(io.BytesIO((build / obj).read_bytes()))
        symtab = e.get_section_by_name(".symtab")
        syms = {s.name: s for s in symtab.iter_symbols() if s.name and isinstance(s["st_shndx"], int)}
        rel = collections.defaultdict(dict)       # section index -> {offset: (symbol or None, type)}
        for rs in e.iter_sections():
            if isinstance(rs, RelocationSection):
                for r in rs.iter_relocations():
                    sy = symtab.get_symbol(r["r_info_sym"])
                    rel[rs["sh_info"]][r["r_offset"]] = (None if sy["st_info"]["type"] == "STT_SECTION" else sy.name,
                                                         r["r_info_type"])
        for n in names:
            want = moved[n]
            s = syms.get(n)
            if s is None or s["st_info"]["bind"] != "STB_WEAK":
                bad.append(f"{n}: not a weak definition in {obj}")
                continue
            if s["st_size"] != want["size"]:
                bad.append(f"{n}: the PC's size {s['st_size']}, the matching build's {want['size']}")
                continue
            sec = e.get_section(s["st_shndx"])
            v, size = s["st_value"], want["size"]
            pc = bytes(size) if sec["sh_type"] == "SHT_NOBITS" else sec.data()[v:v + size]
            ps2 = bytes.fromhex(want["bytes"])
            pw = {o - v: r for o, r in rel[s["st_shndx"]].items() if v <= o < v + size}
            ww = {int(o): w for o, w in want["words"].items()}
            for o in sorted(set(pw) | set(ww)):
                words_checked += 1
                r, w = pw.get(o), ww.get(o)
                add = int.from_bytes(pc[o:o + 4], "little", signed=True)
                if r is None and w is not None and len(w) > 2 and w[0] is None and add & 0xFFFFFFFF == w[1]:
                    # a ps2-words row's word (config/pc/data_names.txt): the PC's C has the PS2 address as a plain
                    # number (content_gen's .inc, as the matching build's); the block has its PC address. The same
                    # address
                    ps2_numbers += 1
                    continue
                if r is None or w is None or r[1] != 1 or (w[0] is not None and (r[0] != w[0] or add != w[1])) \
                        or (w[0] is None and r[0] is not None and r[0] not in moved):
                    bad.append(f"{n}+{o:#x}: the PC's word is {r and r[0]}{add:+d}, the block's {w}")
            for o in range(size):
                if not any(x <= o < x + 4 for x in set(pw) | set(ww)) and pc[o] != ps2[o]:
                    bad.append(f"{n}+{o:#x}: the PC's byte {pc[o]:#x}, the matching build's {ps2[o]:#x}")
                    break
    print(f"data_cases: {len(moved)} C names at their PS2 offsets checked against the PC's own definitions "
          f"({words_checked} address words, {ps2_numbers} of them ps2-words, a PS2 number in the PC's C): "
          f"{len(bad)} differences")
    return bad


def check_code_words(build: Path, gen: Path, dataobj: dict) -> list[str]:
    """The words of the moved C data that address a code label inside a function (MWCC's jump tables) keep the
    PS2's values: each lies in an @N object of its matching object (a jump table,
    which no PC code reads), a resident block's bytes there are the PS2 value with no relocation, and an
    overlay's words table (DataC_<m>_Words) doesn't name it. Returns the differences."""
    import json
    from elftools.elf.elffile import ELFFile
    from elftools.elf.relocation import RelocationSection
    p = gen / "code_words.json"
    if not p.exists():
        return [f"{p} is missing (data_c.py writes it at configure time)"]
    cw = json.loads(p.read_text())
    bad, counts = [], []
    elfs = {}
    for key, b in sorted(cw.items()):
        mod = b["mod"]
        counts.append(f"{key} {len(b['words'])}")
        if mod not in dataobj:
            bad.append(f"{key}: no PC object for data_{mod}.c")
            continue
        if mod not in elfs:
            e = ELFFile(io.BytesIO((build / dataobj[mod]).read_bytes()))
            st = e.get_section_by_name(".symtab")
            rel = collections.defaultdict(set)
            for rs in e.iter_sections():
                if isinstance(rs, RelocationSection):
                    rel[rs["sh_info"]].update(r["r_offset"] for r in rs.iter_relocations())
            elfs[mod] = (e, {s.name: s for s in st.iter_symbols() if s.name}, rel)
        e, syms, rel = elfs[mod]
        for P, fn, A, obj, holder, value in b["words"]:
            if holder is None:
                bad.append(f"0x{P:X} ({fn}{A:+#x}, {obj}): not in an @N object of its matching object")
        if b["ram"]:
            s = syms.get(f"DataC_{mod}_Words")
            if s is None:
                bad.append(f"{key}: DataC_{mod}_Words is not in {dataobj[mod]}")
                continue
            raw = e.get_section(s["st_shndx"]).data()[s["st_value"]:s["st_value"] + s["st_size"]]
            named = {int.from_bytes(raw[i:i + 4], "little") for i in range(0, len(raw), 8)}
            bad += [f"0x{P:X} ({fn}{A:+#x}): DataC_{mod}_Words names it" for P, fn, *_ in b["words"] if P in named]
        else:
            s = syms.get(b["block"])
            if s is None:
                bad.append(f"{key}: {b['block']} is not in {dataobj[mod]}")
                continue
            sec = e.get_section(s["st_shndx"])
            for P, fn, A, obj, holder, value in b["words"]:
                off = s["st_value"] + b["lo"] % PAGE + P - b["lo"]
                got = int.from_bytes(sec.data()[off:off + 4], "little")
                if off in rel[s["st_shndx"]] or got != value:
                    bad.append(f"0x{P:X} ({fn}{A:+#x}): the block has 0x{got:X}"
                               f"{' relocated' if off in rel[s['st_shndx']] else ''}, the PS2 0x{value:X}")
    print(f"data_cases: {sum(len(b['words']) for b in cw.values())} words address code labels (MWCC's jump "
          f"tables) and keep the PS2's values ({', '.join(counts)}): {len(bad)} differences")
    return bad


def run_cases(build: Path, gen: Path, jobs: int) -> int:
    """The standing check (ctest data_cases): every name the C sizes fits the PC object that holds it, as one
    memory with the PS2's, and every name's matching extent does too. Returns the exit status."""
    import concurrent.futures as cf
    import json
    data = json.loads((gen / "cases.json").read_text())
    ext, tus = data["names"], data["tus"]
    ninja = (build / "build.ninja").read_text()
    pdir = gen / "probe"
    pdir.mkdir(exist_ok=True)
    jobs_in = []
    tujobs = []             # (TU key, tag, source, command): the pointer words' check (check_ptrs)
    objpath = {}
    dataobj = {}
    for blk in re.finditer(r"^build (core/CMakeFiles/game_(\w+)\.dir/\S+\.c\.o): C_COMPILER__game_\w+ (\S+\.c)[^\n]*\n"
                           r"((?:  [^\n]*\n)*)", ninja, re.M):
        obj, mod, src, body = blk.group(1), blk.group(2), blk.group(3), blk.group(4)
        name = Path(src).name
        if name == f"data_{mod}.c":
            dataobj[mod] = obj
            continue
        if "/gen/sjis/" in src:
            name = name[len(mod) + 1:]
        key = f"{mod}/{name}"
        if key not in tus:
            print(f"data_cases: {src}: no matching object of that name")
            return 1
        objpath[key] = obj
        var = dict(re.findall(r"^  (\w+) = (.*)$", body, re.M))
        cmd = [CLANG_PROBE[0]] + var.get("FLAGS", "").split() + var.get("INCLUDES", "").split() + var.get("DEFINES", "").split()
        jobs_in.append((re.sub(r"\W", "_", key), src, cmd, tus[key], ext, pdir))
        tujobs.append((key, re.sub(r"\W", "_", key), src, cmd))
    with cf.ThreadPoolExecutor(jobs) as ex:
        results = list(ex.map(probe_tu, jobs_in))
    cases, inc, und, errs = [], set(), set(), []
    for (tag, src, *_), (res, cached) in zip(jobs_in, results):
        cases += [(n, cs, e, src) for n, cs, e in res["cases"]]
        inc |= set(res["incomplete"])
        und |= set(res["undeclared"])
        errs += [f"{src}: {e}" for e in res["errors"]]
    sized = {n for (_, _, _, names, _, _) in jobs_in for n in names} - inc - und
    probed = sum(1 for r, cached in results if not cached)
    plan_cases = data["plan_cases"]
    inc_checked = len([n for n in inc if n not in data["unsized"]])
    print(f"data_cases: {len(jobs_in)} TUs ({probed} probed, {len(jobs_in) - probed} cached); names the C sizes: "
          f"{len(sized)}, incomplete {len(inc)} (checked by the matching build's extent: {inc_checked}), "
          f"undeclared {len(und)}; names with a matching extent: {len(ext) - len(data['unsized'])}")
    for n, cs, e, src in sorted(cases):
        print(f"CASE {n}: the C's sizeof is {cs if cs is not None else '?'}, the PC object holds {e} bytes from it ({src})")
    for n, pc, ps2, how in plan_cases:
        print(f"CASE {n}: the matching build gives it {ps2} bytes ({how}), the PC object holds {pc} from it")
    for n, own, held in data["size_past_holder"]:
        print(f"note: {n}'s matching size {own} reaches past the object that holds it ({held} bytes): checked to its end")
    for e in errs[:10]:
        print(f"ERROR {e}")
    moved_bad = check_moved(build, gen, objpath)
    for e in moved_bad[:20]:
        print(f"MOVED {e}")
    code_bad = check_code_words(build, gen, dataobj)
    for e in code_bad[:20]:
        print(f"CODE {e}")
    ptr_bad = check_ptrs(gen, tujobs, jobs)
    for e in ptr_bad[:40]:
        print(f"PTR {e}")
    bad = len({c[0] for c in cases}) + len(plan_cases) + len(moved_bad) + len(code_bad) + len(ptr_bad)
    print(f"data_cases: {'OK' if not bad and not errs else 'FAILED'}: {bad} cases, {len(errs)} probe errors")
    return 1 if bad or errs else 0


# ---- The pointer words (ctest data_cases): a word the C types as a pointer is an address word ----
# A word that holds an EE RAM address with no R_MIPS_32 relocation in the matching object is a plain number in
# both builds: the PC keeps the PS2's address, which it never translates (gBladeDetailLabels, `char[16]` of two
# pointers' bytes, crashed the core on the status page). So no TU may type such a word as a pointer: the object's
# C must say what it is (a table of pointers to its texts, a name, or RAM_AREA(addr) with a ram-words row).
PTR_LO, PTR_HI = 0x100000, 0x2000000     # above the EE kernel, below the end of the EE's RAM


def write_ptrs(plan: Plan, out: Path):
    """ptrs.json: per C object of a .data block (a moved global, or a file-static object, which keeps the PC's
    copy), the words that hold an EE RAM address with no relocation (not a jump table's, not a ram-words or
    ps2-words row's): offset -> value; and per game TU, the names it declares among them."""
    import json
    m = plan.m
    objs, tus = {}, collections.defaultdict(dict)
    src_of = {o: s for mod, s, o in m.objs}

    def scan(oid, key, A, size, words):
        img, lo = m.cinit[key][0], m.bss_block[key][0]
        cw = {c[0] for c in m.codewords.get(key, [])}
        ws = {}
        for P in range((A + 3) & ~3, A + size - 3, 4):
            V = int.from_bytes(img[P - lo:P - lo + 4], "little")
            if P not in words and P not in cw and PTR_LO <= V < PTR_HI:
                ws[P - A] = V
        return ws
    for n, (key, A, size, src) in sorted(m.cnames.items()):
        if block_kind(key) == "Data" and n not in plan.blobs:
            ws = scan(n, key, A, size, m.cinit[key][1])
            if ws:
                objs[n] = {"size": size, "src": src, "words": ws}
    for key, L in sorted(m.cstatic.items()):
        if block_kind(key) != "Data":
            continue
        for lo, hi, n, o in L:
            ws = scan(n, key, lo, hi - lo, m.static_words[key])
            if ws:
                src = src_of[o]
                objs[f"{src}:{n}"] = {"size": hi - lo, "src": src, "words": ws}
                tus[f"{block_mod(key)}/{Path(src).name}"][n] = f"{src}:{n}"
    for mod, src, obj in m.objs:
        tu = f"{mod}/{Path(src).name}"
        for n in set(m.undef_by_src.get(src, ())) | {n for n, v in m.cnames.items() if v[3] == src}:
            if n in objs:
                tus[tu][n] = n
    put(out / "ptrs.json", json.dumps({"objs": objs, "tus": tus}, indent=0, sort_keys=True) + "\n")


class PtrLayout:
    """The pointer offsets of a C type, by the EE's layout rules (content_gen's Types, bit-fields as their storage
    units: a field that would cross its type's unit starts the next one)."""

    def __init__(self, types, alias):
        self.T, self.alias, self.memo, self.has = types, alias, {}, {}

    def rec(self, rid):
        rid = self.alias.get(rid, rid)
        if rid in self.memo:
            return self.memo[rid]
        tag, fields = self.T.records[rid]
        out, bit, al, size = [], 0, 1, 0
        for name, qt, bits in fields:
            ft = self.T.parse(qt)
            fa, fs = self.align(ft), self.size(ft)
            if tag == "union":
                al = max(al, fa)
                if bits is None:
                    out.append((ft, 0))
                size = max(size, fs)
            elif bits is not None:
                if bits == 0:
                    bit = (bit + 8 * fa - 1) // (8 * fa) * (8 * fa)
                    continue
                if bit // (8 * fs) != (bit + bits - 1) // (8 * fs):
                    bit = (bit + 8 * fs - 1) // (8 * fs) * (8 * fs)
                bit += bits
                al = max(al, fa) if name else al
                size = max(size, (bit + 7) // 8)
            else:
                off = ((bit + 7) // 8 + fa - 1) // fa * fa
                out.append((ft, off))
                bit, al, size = 8 * (off + fs), max(al, fa), off + fs
        self.memo[rid] = (out, (size + al - 1) // al * al, al)
        return self.memo[rid]

    def size(self, t):
        return self.rec(t[1])[1] if t[0] == "rec" else self.size(t[1]) * t[2] if t[0] == "array" else self.T.size(t)

    def align(self, t):
        return self.rec(t[1])[2] if t[0] == "rec" else self.align(t[1]) if t[0] == "array" else self.T.align(t)

    def ptrs(self, t, base, out):
        key = repr(t)
        if self.has.get(key) is False:
            return
        n0 = len(out)
        if t[0] == "ptr":
            out.append(base)
        elif t[0] == "array":
            es = self.size(t[1])
            self.ptrs(t[1], base, out)
            if len(out) > n0:
                for i in range(1, t[2]):
                    self.ptrs(t[1], base + i * es, out)
        elif t[0] == "rec":
            for ft, off in self.rec(t[1])[0]:
                self.ptrs(ft, base + off, out)
        self.has[key] = len(out) > n0

    def view(self, qt, size):
        """The pointer offsets of an object of type QT (an unsized array: over SIZE, the matching build's)."""
        import content_gen
        s = " ".join(content_gen.QUALS.sub("", qt).split())
        unsized = "[]" in s
        if unsized:             # T[][N]: an array of T[N]
            s = s.replace("[]", "", 1).strip()
        t = self.T.parse(s)
        if unsized:
            t = ("array", t, max(1, size // max(1, self.size(t))))
        out = []
        self.ptrs(t, 0, out)
        return sorted({o for o in out if o < size})


def ptr_tu(job):
    """One game TU's pointer offsets of the objects it declares (clang's AST, typed as content_gen does): name ->
    offsets, or "error: ..." for a type the layout can't do. Cached as the probes are."""
    import hashlib
    import json
    tag, src, cmd, names, pdir = job
    dep, cache = pdir / f"{tag}.ptr.d", pdir / f"{tag}.ptr.json"
    key = hashlib.sha256(("\0".join(cmd) + "\0" + src + "\0" + json.dumps(names, sort_keys=True)).encode()).hexdigest()
    if cache.exists():
        try:
            c = json.loads(cache.read_text())
            if c["key"] == key and all(os.stat(p).st_mtime_ns == t and os.stat(p).st_size == s for p, t, s in c["deps"]):
                return c, True
        except (OSError, ValueError, KeyError):
            pass
    sys.path.insert(0, str(ROOT / "tools"))
    import content_gen
    r = subprocess.run(cmd + ["-fsyntax-only", "-w", "-ferror-limit=0", "-MD", "-MF", str(dep), "-Xclang",
                              "-ast-dump=json", src], capture_output=True)
    res = {"key": key, "views": {}, "errors": [], "deps": []}
    if r.returncode != 0 or not r.stdout:
        res["errors"].append(f"{src}: clang exited {r.returncode}: {r.stderr.decode(errors='replace')[:300]}")
        return res, False
    ast = json.loads(r.stdout)
    alias = {n["previousDecl"]: n["id"] for n in ast.get("inner", [])      # a typedef of a record declared before
             if n.get("kind") == "RecordDecl" and n.get("completeDefinition") and n.get("previousDecl")}
    lay = PtrLayout(content_gen.Types(ast), alias)
    for n in ast.get("inner", []):
        name = n.get("name")
        if n.get("kind") != "VarDecl" or name not in names:
            continue
        try:
            v = lay.view(n["type"]["qualType"], names[name])
        except Exception as e:          # noqa: BLE001 (a type the layout can't do: reported, never skipped)
            res["views"][name] = f"error: {n['type']['qualType']}: {e}"
            continue
        old = res["views"].get(name)
        res["views"][name] = sorted(set(v) | set(old if isinstance(old, list) else []))
    if dep.exists():
        files = re.sub(r"\\\n", " ", dep.read_text()).split(":", 1)[1].split()
        res["deps"] = [[p, os.stat(p).st_mtime_ns, os.stat(p).st_size] for p in dict.fromkeys(files) if os.path.exists(p)]
    cache.write_text(json.dumps(res) + "\n")
    return res, False


def check_ptrs(gen: Path, tujobs: list, jobs: int) -> list[str]:
    """Every word a TU types as a pointer that holds an EE RAM address with no relocation (ptrs.json). TUJOBS:
    (TU key, tag, source, compiler command)."""
    import concurrent.futures as cf
    import json
    p = gen / "ptrs.json"
    if not p.exists():
        return [f"{p} is missing (data_c.py writes it at configure time)"]
    data = json.loads(p.read_text())
    objs = data["objs"]
    work = []
    for tu, tag, src, cmd in tujobs:
        decl = data["tus"].get(tu, {})
        if decl:
            work.append((tu, (tag, src, cmd, {n: objs[i]["size"] for n, i in decl.items()}, gen / "probe")))
    # clang's JSON of a TU is large (up to about 1 GB in Python): a few processes at a time
    with cf.ProcessPoolExecutor(max(1, min(jobs, 6))) as ex:
        results = list(ex.map(ptr_tu, [j for tu, j in work]))
    bad, typed, words = [], 0, 0
    for (tu, job), (res, cached) in zip(work, results):
        bad += [f"{tu}: {e}" for e in res["errors"]]
        decl = data["tus"][tu]
        for n, offs in sorted(res["views"].items()):
            o = objs[decl[n]]
            if isinstance(offs, str):
                bad.append(f"{decl[n]} ({tu}): {offs}")
                continue
            typed += 1
            for off in offs:
                V = o["words"].get(str(off))
                words += V is not None
                if V is not None:
                    bad.append(f"{decl[n]}+{off:#x} ({o['src']}): {tu} types it a pointer, but the matching word "
                               f"0x{V:08X} has no relocation (the PC would keep the PS2's address)")
    print(f"data_cases: {len(objs)} data objects hold EE RAM addresses with no relocation; {len(work)} TUs "
          f"({sum(1 for r, c in results if not c)} typed, the rest cached) declare them ({typed} declarations): "
          f"{words} such words typed as pointers")
    return bad


CLANG_PROBE = ["clang"]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", type=Path)
    ap.add_argument("--platform", default="linux")
    ap.add_argument("--report", action="store_true", help="print the names left to the stubs")
    ap.add_argument("--cases", type=Path, metavar="BUILD", help="the standing check on a configured and built "
                    "PC build directory (ctest data_cases): needs --out (its gen/data) and --clang")
    ap.add_argument("--clang", help="--cases: the compiler of the game's C")
    ap.add_argument("-j", type=int, default=10)
    ap.add_argument("--check-inputs", action="store_true", help="ctest data_inputs: the generator stops on a missing input")
    ap.add_argument("--align", type=Path, metavar="CORE", help="ctest data_align: the 16-aligned PS2 names in the "
                    "core (needs --out, its gen/data, and --nm)")
    ap.add_argument("--nm", default="nm", help="--align: the toolchain's nm")
    a = ap.parse_args()
    if not a.out:
        ap.error("--out DIR")
    if a.check_inputs:
        sys.exit(check_inputs(a.out))
    if a.align:
        sys.exit(run_align(a.out, a.align, a.nm, a.platform))
    if a.cases:
        if a.clang:
            CLANG_PROBE[0] = a.clang
        sys.exit(run_cases(a.cases, a.out, a.j))
    m = Matching()
    check_layout_current(m)
    plan = Plan(m)
    emit(plan, a.out, "_" if a.platform == "windows" else "")
    report(plan, a.out)
    write_cases(plan, a.out)
    write_ptrs(plan, a.out)
    write_align(plan, a.out)
    if a.report:
        print((a.out / "report.txt").read_text())


if __name__ == "__main__":
    main()
