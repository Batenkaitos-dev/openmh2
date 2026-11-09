#!/usr/bin/env python3
"""Writes the movies' timing table beside the disc archive (setup time).

    python3 tools/pc/movie_table.py [--disc build/port/mh2-disc.arc] [--lib DIR] [--force] [--if-able]

The core's Linux build runs it with --if-able (pc/core/ipc/sources.cmake): it writes the table
once the archive (python tools/port/make_archive.py) and the movie library are made, and
says so and does nothing until then. Up to date, it returns at once.

The native port's core keeps each movie's timing without a decoder:
the original game reports a movie FINISHED at the first
VBlank end where its clock, the SPU2's output frames mixed since it began
playing, has passed both the last frame's time and the sound's length.
Both depend only on the stream. This tool measures them with the port's
own movie library (mh2movie, pc/movie: the pinned FFmpeg), driven
as the original's decoder thread drives it, and writes them to
mh2-movies.txt next to the archive. The core reads it through io.

The table is derived from the game's files: it stays in build/ (R8).

Per movie (an entry of AFS02.AFS) the line is
    I OFFSET SIZE FRAMES SHOWN SOUND PTS
- OFFSET, SIZE: the entry in AFS02.AFS (the core checks them against the
  header it reads);
- FRAMES: the frames the library decodes (-1: it can't open the movie, which
  the original game reports as finished at once);
- SHOWN: the smallest clock C (48 kHz frames) at which every frame is shown,
  that is the original's (double)C / 48000.0 >= the largest presentation
  time of frames 1 to FRAMES-1 (frame 0 shows at once);
- SOUND: the sound's length in 48 kHz frames (-1: the movie has no sound);
- PTS: that largest presentation time, for the record.
"""

from __future__ import annotations

import argparse
import ctypes
import os
import struct
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "port"))
sys.path.insert(0, str(ROOT / "tools"))
import make_archive  # noqa: E402

TABLE = "mh2-movies.txt"
VERSION = 1
RATE = 48000


class Info(ctypes.Structure):
    _fields_ = [("width", ctypes.c_int), ("height", ctypes.c_int), ("fps", ctypes.c_double),
                ("duration", ctypes.c_double), ("has_audio", ctypes.c_int)]


def load_lib(folder: Path):
    name = "mh2movie.dll" if os.name == "nt" else "mh2movie.so"
    if os.name == "nt":
        os.add_dll_directory(str(folder.resolve()))
    lib = ctypes.CDLL(str((folder / name).resolve()))
    if lib.mh2movie_abi() != 1:
        sys.exit(f"{folder / name}: another version of mh2movie")
    lib.mh2movie_open.restype = ctypes.c_void_p
    lib.mh2movie_open.argtypes = [ctypes.c_char_p, ctypes.c_uint64, ctypes.c_uint64, ctypes.POINTER(Info),
                                  ctypes.c_char_p, ctypes.c_int]
    lib.mh2movie_next_frame.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_double)]
    lib.mh2movie_audio.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int]
    lib.mh2movie_close.argtypes = [ctypes.c_void_p]
    return lib


def find_file(disc: Path, name: str) -> tuple[int, int, int]:
    """(LBA, size, byte offset in the archive) of a file, through the archive's own reader."""
    head = make_archive.read_table(disc)
    if head is None:
        sys.exit(f"{disc}: not a disc archive of this version (python tools/port/make_archive.py writes it)")
    _magic, _ver, n, esize, _start, _size = make_archive.HEADER.unpack_from(head, 0)
    for i in range(n):
        path, lba, size, off, _sha = make_archive.ENTRY.unpack_from(head, make_archive.HEADER.size + i * esize)
        if path.split(b"\0")[0].decode("ascii") == name:
            return lba, size, off
    sys.exit(f"{disc}: no {name}")


def shown_clock(pts: float) -> int:
    """The smallest C >= 0 with (double)C / 48000.0 >= pts, as the original game compares."""
    if pts <= 0.0:
        return 0
    c = int(pts * RATE)
    while c > 0 and (c - 1) / float(RATE) >= pts:
        c -= 1
    while c / float(RATE) < pts:
        c += 1
    return c


def measure(lib, disc: Path, offset: int, size: int):
    info = Info()
    err = ctypes.create_string_buffer(512)
    m = lib.mh2movie_open(str(disc).encode(), offset, size, ctypes.byref(info), err, len(err))
    if not m:
        return -1, 0, -1, 0.0, err.value.decode(errors="replace")
    rgba = ctypes.create_string_buffer(info.width * info.height * 4)
    audio = (ctypes.c_int16 * (2 * 4096))()
    pts = ctypes.c_double()
    frames = samples = 0
    top = None
    rc = 1
    while True:   # as movie.c's worker: a frame, then the sound decoded meanwhile
        rc = lib.mh2movie_next_frame(m, rgba, ctypes.byref(pts))
        while True:
            n = lib.mh2movie_audio(m, audio, 4096)
            if n <= 0:
                break
            samples += n
        if rc <= 0:
            break
        if frames > 0 and (top is None or pts.value > top):
            top = pts.value
        frames += 1
    lib.mh2movie_close(m)
    if rc < 0:
        return -1, 0, -1, 0.0, f"decoding error after {frames} frames"
    top = top if top is not None else 0.0
    return frames, shown_clock(top), samples if info.has_audio else -1, top, ""


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--disc", type=Path, default=ROOT / "build" / "port" / "mh2-disc.arc")
    ap.add_argument("--lib", type=Path, default=ROOT / "build" / "port", help="the folder with mh2movie")
    ap.add_argument("--out", type=Path, help="the table (default: mh2-movies.txt beside the archive)")
    ap.add_argument("--force", action="store_true", help="write it even if it is up to date")
    ap.add_argument("--progress", action="store_true", help="print 'progress I N' before each movie (the launcher's)")
    ap.add_argument("--if-able", action="store_true",
                    help="without the archive or the movie library, say so and succeed (the core's build)")
    a = ap.parse_args()
    disc = a.disc.resolve()
    out = a.out or disc.parent / TABLE
    if a.if_able:
        lib_name = "mh2movie.dll" if sys.platform == "win32" else "mh2movie.so"
        missing = [str(x) for x in (disc, a.lib / lib_name) if not x.is_file()]
        if missing:
            print(f"movie_table: not written: no {' or '.join(missing)} yet (python tools/port/make_archive.py makes the archive, the movie build the library)")
            return 0
    lba, fsize, foff = find_file(disc, "/AFS02.AFS")
    with open(disc, "rb") as f:
        f.seek(foff)
        hdr = f.read(8)
        magic, count = struct.unpack("<4sI", hdr)
        if magic != b"AFS\0" or count > 1000:
            sys.exit("AFS02.AFS: not an AFS file")
        toc = f.read(8 * count)
    entries = [struct.unpack_from("<II", toc, 8 * i) for i in range(count)]
    head = f"mh2-movies {VERSION} afs02 {lba} {fsize} {count}"
    if not a.force and out.is_file() and out.stat().st_mtime >= disc.stat().st_mtime:
        first = out.read_text(encoding="ascii", errors="replace").splitlines()[:2]
        if len(first) == 2 and first[1] == head:
            print(f"movie_table: {out} is up to date")
            return 0
    lib = load_lib(a.lib)
    lines = ["# The movies' timing, measured from your copy of the disc. The launcher makes this file.", head]
    t0 = time.monotonic()
    for i, (off, size) in enumerate(entries):
        if a.progress:
            print(f"progress {i + 1} {count}", flush=True)
        frames, shown, sound, top, err = measure(lib, disc, foff + off, size)
        if err:
            print(f"movie_table: movie {i}: {err}")
        lines.append(f"{i} {off} {size} {frames} {shown} {sound} {top!r}")
        print(f"movie_table: movie {i}: {frames} frames, shown at {shown}, sound {sound}", flush=True)
    tmp = out.with_suffix(".tmp")
    tmp.write_bytes(("\n".join(lines) + "\n").encode("ascii"))   # LF on every system, as the launcher's maker writes it
    os.replace(tmp, out)
    print(f"movie_table: {count} movies -> {out} ({time.monotonic() - t0:.0f} s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
