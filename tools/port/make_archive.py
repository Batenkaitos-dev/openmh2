#!/usr/bin/env python3
"""Write the port's disc archive: the files the running port opens, read
once from the user's copy, so that the port needs nothing else afterwards.

    python tools/port/make_archive.py [--out FILE] [--source auto|iso|extracted] [--force]
    python tools/port/make_archive.py --verify FILE    # re-hash an archive's files
    python tools/port/make_archive.py --check-layout   # config/disc_layout.txt against the ISO
    python tools/port/make_archive.py --write-layout   # (re)write config/disc_layout.txt from the ISO

The build of the port runs it after building the executable, with --out
beside it (build/port/mh2-disc.arc, build/port-native/mh2-disc.arc). The
port reads only that file at run time; --orig is for
development.

The user's copy is found as the build finds it (tools/orig.py: $MH2_ORIG_DIR,
else the repository's parent folder): a disc image (*.iso), or the extracted
files. It is only read.

Contents: FILES below, the files the port opens or reads: the boot
executable (main.c load_elf), the game's data files (the CD/DVD
service; the movies), the IOP sound modules (the file service), and one file the game reads a sector of without opening it:
it reads 0FLIST.DIR (43 bytes, one sector) as two sectors, and the second
is the first of NTGUIDVD/BIN/BG.RGB, so that file is held too (whole, so
it is hash-checked like the others). Every route's log lists the files a
run uses ("first lookup of", "first read of").

Integrity: each file's size and SHA-1 must be the ones in config/disc.sha1.
A mismatch stops the tool and names the file; no archive is written.

Sector numbers: each file keeps its first sector (LBA) on the disc, so the
game's sector reads see the same numbers as from the ISO. They come from
config/disc_layout.txt (path, sector, size: no other data), written once
from an ISO with --write-layout. From an ISO, the ISO's directory is
checked against that table first; from extracted files, the table is all
there is.

Format (all integers little-endian):

    offset 0    header, 32 bytes
                  0  char[8]  magic: "MH2DARC" and a zero byte
                  8  u32      version (1)
                 12  u32      entry count N
                 16  u32      entry size (128)
                 20  u32      data start (a multiple of 2048)
                 24  u64      archive size in bytes
    offset 32   N entries of 128 bytes, sorted by sector
                  0  char[80] disc path, "/" separated, upper case, leading "/",
                              NUL-padded ("/IOP/MODULES/MH2DRIVE.DAT")
                 80  u32      first sector on the disc (LBA)
                 84  u32      size in bytes
                 88  u64      offset of the file's bytes in the archive
                 96  u8[20]   SHA-1 of the file (the one in config/disc.sha1)
                116  u8[12]   zero
    data        each file's bytes, uncompressed, at its offset (a multiple
                of 2048), then zeros to the next multiple of 2048 (the rest
                of its last sector, zero on the disc too: checked from an ISO)

So a file is a byte range of the archive (path and offset: the movies hand
that to FFmpeg), and sector S of a file at LBA L is 2048 bytes at
offset + (S - L) * 2048.

The archive is the user's game data: it stays on their machine, under
build/, and is never committed or shared.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import orig  # noqa: E402

ROOT = orig.ROOT
SECTOR = 2048
NAME = "mh2-disc.arc"
MAGIC = b"MH2DARC\0"
VERSION = 1
HEADER = struct.Struct("<8sIIIIQ")      # 32 bytes
ENTRY = struct.Struct("<80sIIQ20s12x")  # 128 bytes
LAYOUT = ROOT / "config" / "disc_layout.txt"
MANIFEST = ROOT / "config" / "disc.sha1"
CHUNK = 8 << 20

# The files the running port opens (disc paths as in config/disc.sha1).
FILES = [
    "SLPM_662.80",
    "IOP/MODULES/MH2DRIVE.DAT",
    "0FLIST.DIR",
    "DATA.BIN",
    "AFS00.AFS",
    "AFS01.AFS",
    "AFS02.AFS",
    "NTGUIDVD/BIN/BG.RGB",   # its first sector: the game reads 0FLIST.DIR as 2 sectors
]


def fail(msg: str) -> None:
    sys.exit(f"make_archive: {msg}")


def load_manifest() -> dict[str, tuple[str, int]]:
    out = {}
    for line in MANIFEST.read_text().splitlines():
        if line.strip() and not line.startswith("#"):
            sha1, size, path = line.split(maxsplit=2)
            out[path] = (sha1, int(size))
    return out


def load_layout() -> dict[str, tuple[int, int]]:
    """config/disc_layout.txt: path -> (lba, size)."""
    if not LAYOUT.exists():
        fail(f"no {LAYOUT.relative_to(ROOT)}: write it from an ISO with --write-layout")
    out = {}
    for line in LAYOUT.read_text().splitlines():
        if line.strip() and not line.startswith("#"):
            lba, size, path = line.split(maxsplit=2)
            out[path] = (int(lba), int(size))
    return out


def iso_layout(iso: Path) -> dict[str, tuple[int, int]]:
    """The ISO's directory: path -> (lba, size), for FILES."""
    with orig.Iso(iso) as disc:
        found = {f.path.upper(): (f.lba, f.size) for f in disc.files()}
    missing = [p for p in FILES if p.upper() not in found]
    if missing:
        fail(f"{iso}: not on the disc: {', '.join(missing)}")
    return {p: found[p.upper()] for p in FILES}


def find_source(kind: str, given: Path | None = None) -> tuple[str, Path]:
    """The player's copy: GIVEN (a disc image, or a folder with the disc's files or an image), else the build's search."""
    if given is not None:
        if given.is_file():
            return "iso", given
        if not given.is_dir():
            fail(f"{given} is not a disc image or a folder")
        if any(p.name.upper() == "SYSTEM.CNF" for p in given.iterdir()):
            return "extracted", given
        isos = orig.find_isos(given)
        if isos:
            return "iso", isos[0]
        fail(f"{given} holds no disc image (*.iso) and no extracted disc files (SYSTEM.CNF)")
    base = orig.orig_dir()
    if kind in ("auto", "iso"):
        isos = orig.find_isos(base)
        if isos:
            return "iso", isos[0]
        if kind == "iso":
            fail(f"no disc image (*.iso) under {base}")
    ext = orig.find_extracted(base)
    if ext:
        return "extracted", ext
    fail(f"no disc image (*.iso) or extracted files (SYSTEM.CNF) under {base}: "
         "set MH2_ORIG_DIR to the folder that holds your copy")
    return "", base   # not reached


def check_layout(iso: Path, layout: dict[str, tuple[int, int]]) -> list[str]:
    """The differences between config/disc_layout.txt and the ISO's directory."""
    disc = iso_layout(iso)
    return [f"{p}: table {layout.get(p)}, ISO {disc[p]}" for p in FILES if layout.get(p) != disc[p]]


def expected_entries(manifest, layout) -> list[tuple[str, int, int, str]]:
    """(path, lba, size, sha1) per file, sorted by sector, from the committed tables."""
    out = []
    for p in FILES:
        if p not in manifest:
            fail(f"{p} is not in {MANIFEST.relative_to(ROOT)}")
        if p not in layout:
            fail(f"{p} is not in {LAYOUT.relative_to(ROOT)}")
        sha1, size = manifest[p]
        lba, lsize = layout[p]
        if lsize != size:
            fail(f"{p}: size {lsize} in {LAYOUT.name}, {size} in {MANIFEST.name}")
        out.append((p, lba, size, sha1))
    out.sort(key=lambda e: e[1])
    for a, b in zip(out, out[1:]):
        if a[1] + (a[2] + SECTOR - 1) // SECTOR > b[1]:
            fail(f"{a[0]} and {b[0]} overlap on the disc")
    return out


def plan(entries) -> tuple[bytes, list[int], int]:
    """The header and table, each file's offset, and the archive's size."""
    start = -(-(HEADER.size + ENTRY.size * len(entries)) // SECTOR) * SECTOR
    offsets, pos = [], start
    for _, _, size, _ in entries:
        offsets.append(pos)
        pos += -(-size // SECTOR) * SECTOR
    head = HEADER.pack(MAGIC, VERSION, len(entries), ENTRY.size, start, pos)
    for (p, lba, size, sha1), off in zip(entries, offsets):
        head += ENTRY.pack(("/" + p.upper()).encode("ascii"), lba, size, off, bytes.fromhex(sha1))
    return head, offsets, pos


def read_table(path: Path) -> bytes | None:
    """An archive's header and table as written, or None if it isn't one."""
    try:
        with open(path, "rb") as f:
            h = f.read(HEADER.size)
            if len(h) != HEADER.size:
                return None
            magic, ver, n, esize, _start, size = HEADER.unpack(h)
            if magic != MAGIC or ver != VERSION or esize != ENTRY.size or n > 4096:
                return None
            if os.path.getsize(path) != size:
                return None
            return h + f.read(n * ENTRY.size)
    except OSError:
        return None


def copy_file(src, out, dst_off: int, path: str, size: int, sha1: str, progress=None) -> None:
    """size bytes from src (positioned) to out at dst_off, hashed on the way; PROGRESS(bytes so far) after each chunk."""
    h = hashlib.sha1()
    out.seek(dst_off)
    left = size
    while left:
        chunk = src.read(min(CHUNK, left))
        if not chunk:
            break
        h.update(chunk)
        out.write(chunk)
        left -= len(chunk)
        if progress:
            progress(size - left)
    if left or h.hexdigest() != sha1:
        raise ValueError(f"{path} does not match {MANIFEST.relative_to(ROOT)} "
                         f"({'short by %d bytes' % left if left else 'SHA-1 ' + h.hexdigest()}): "
                         "not the v1.04 disc this project is built from")
    pad = -size % SECTOR
    if pad:
        out.write(b"\0" * pad)


def write(out: Path, kind: str = "auto", given: Path | None = None, progress: bool = False) -> None:
    manifest, layout = load_manifest(), load_layout()
    src_kind, src = find_source(kind, given)
    if src_kind == "iso":
        diffs = check_layout(src, layout)
        if diffs:
            fail(f"{LAYOUT.relative_to(ROOT)} does not agree with {src}:\n  " + "\n  ".join(diffs))
    entries = expected_entries(manifest, layout)
    head, offsets, total = plan(entries)
    print(f"make_archive: {out} from the {'disc image' if src_kind == 'iso' else 'extracted files'} {src}",
          flush=True)
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_name(out.name + ".tmp")
    grand = sum(e[2] for e in entries)
    before = 0
    try:
        with open(tmp, "wb") as f:
            f.write(head)
            for idx, ((p, lba, size, sha1), off) in enumerate(zip(entries, offsets)):
                report = None
                if progress:
                    def report(done, idx=idx, p=p, before=before):
                        print(f"progress {idx + 1} {len(entries)} {before + done} {grand} {p}", flush=True)
                    report(0)
                if src_kind == "iso":
                    with open(src, "rb") as s:
                        s.seek(lba * SECTOR)
                        copy_file(s, f, off, p, size, sha1, report)
                        tail = s.read(-size % SECTOR)
                    if tail.strip(b"\0"):
                        raise ValueError(f"{p}: the rest of its last sector on the disc is not zero")
                else:
                    host = src.joinpath(*p.split("/"))
                    if not host.is_file():
                        raise ValueError(f"{p} is missing from the extracted files in {src}")
                    if host.stat().st_size != size:
                        raise ValueError(f"{p}: {host.stat().st_size} bytes, {size} in {MANIFEST.name}")
                    with open(host, "rb") as s:
                        copy_file(s, f, off, p, size, sha1, report)
                before += size
                print(f"  {p}: sector {lba}, {size} bytes, SHA-1 checked", flush=True)
            f.truncate(total)
        os.replace(tmp, out)
    except (OSError, ValueError) as e:
        try:
            tmp.unlink()
        except OSError:
            pass
        fail(str(e))
    print(f"make_archive: wrote {out} ({len(entries)} files, {total} bytes)", flush=True)


def ensure(out: Path, others: list[Path] = (), kind: str = "auto", force: bool = False,
           given: Path | None = None, progress: bool = False) -> None:
    """out up to date: kept if its table is the expected one, else linked to
    an up-to-date archive among others (the other build's), else written."""
    want = plan(expected_entries(load_manifest(), load_layout()))[0]
    if not force:
        if read_table(out) == want:
            print(f"make_archive: {out} is up to date", flush=True)
            return
        for o in others:
            if o.resolve() != out.resolve() and read_table(o) == want:
                try:
                    if out.exists():
                        out.unlink()
                    os.link(o, out)
                    print(f"make_archive: {out}: linked to {o}", flush=True)
                    return
                except OSError:
                    pass
    write(out, kind, given, progress)


def verify(path: Path) -> int:
    """Re-hash each file in an archive against its table and config/disc.sha1."""
    table = read_table(path)
    if table is None:
        print(f"{path}: not an archive of this version")
        return 1
    manifest = load_manifest()
    bad = 0
    with open(path, "rb") as f:
        for i in range(HEADER.unpack(table[:HEADER.size])[2]):
            raw, lba, size, off, sha1 = ENTRY.unpack_from(table, HEADER.size + i * ENTRY.size)
            p = raw.rstrip(b"\0").decode("ascii")
            f.seek(off)
            h, left = hashlib.sha1(), size
            while left:
                chunk = f.read(min(CHUNK, left))
                if not chunk:
                    break
                h.update(chunk)
                left -= len(chunk)
            tail = f.read(-size % SECTOR)
            ok = (not left and h.digest() == sha1 and manifest.get(p.lstrip("/")) == (sha1.hex(), size)
                  and not tail.strip(b"\0"))
            bad += not ok
            print(f"{'OK ' if ok else 'BAD'} {p}: sector {lba}, {size} bytes")
    return 1 if bad else 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", type=Path, default=ROOT / "build" / "port" / NAME, help="the archive to write")
    ap.add_argument("--source", default="auto", choices=["auto", "iso", "extracted"],
                    help="read the ISO or the extracted files (auto: the ISO if there is one)")
    ap.add_argument("--force", action="store_true", help="write it even if it is up to date")
    ap.add_argument("--from", dest="given", type=Path, metavar="PATH",
                    help="the player's copy: a disc image, or a folder with the disc's files (the launcher's)")
    ap.add_argument("--progress", action="store_true", help="print a progress line after each chunk (the launcher's)")
    ap.add_argument("--size", action="store_true", help="print the archive's size in bytes and stop (the launcher's)")
    ap.add_argument("--verify", type=Path, metavar="FILE", help="re-hash the files of an archive")
    ap.add_argument("--check-layout", action="store_true", help=f"compare {LAYOUT.name} with the ISO")
    ap.add_argument("--write-layout", action="store_true", help=f"write {LAYOUT.name} from the ISO")
    args = ap.parse_args()

    if args.verify:
        return verify(args.verify)
    if args.check_layout or args.write_layout:
        _, iso = find_source("iso")
        if args.write_layout:
            disc = iso_layout(iso)
            lines = ["# The disc's sectors for the files of the port's archive (tools/port/make_archive.py).",
                     "# Format: lba  size  path   (written by tools/port/make_archive.py --write-layout)"]
            lines += [f"{disc[p][0]}  {disc[p][1]}  {p}" for p in sorted(FILES, key=lambda p: disc[p][0])]
            LAYOUT.write_text("\n".join(lines) + "\n")
            print(f"wrote {LAYOUT.relative_to(ROOT)} from {iso}")
            return 0
        diffs = check_layout(iso, load_layout())
        print("\n".join(diffs) if diffs else f"{LAYOUT.relative_to(ROOT)} agrees with {iso}")
        return 1 if diffs else 0
    if args.size:
        print(plan(expected_entries(load_manifest(), load_layout()))[2])
        return 0
    ensure(args.out, kind=args.source, force=args.force, given=args.given, progress=args.progress)
    return 0


if __name__ == "__main__":
    sys.exit(main())
