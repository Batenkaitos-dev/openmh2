#!/usr/bin/env python3
"""Packs the project's FFmpeg (tools/pc/build_ffmpeg.sh's output directory) into the archive setup downloads.

    python3 tools/pc/pack_ffmpeg.py SYSTEM DIR OUT

SYSTEM is linux-x64 (OUT is a .tar.xz) or windows-x64 (OUT is a .zip). DIR is the output directory of
build_ffmpeg.sh: include/, lib/ (Windows: bin/ too), the licence texts, configure-line.txt and the stamp
(.mh2-ffmpeg-stamp). The archive holds those, with the paths relative to DIR (no top directory) and nothing else
(setup's own .mh2-installed is left out).

The archive is reproducible: the entries are sorted by path, the times are fixed (the tar's 0, the zip's 1980-01-01,
the earliest it can hold), the owners are root with no name, the modes are 0644 (0755 for what is executable or a
directory), and the compression levels are fixed. Two packings of the same directory give the same bytes. Unpack with
tar -xmf (-m: the files get the time of the unpacking, not the archive's, so a build tool sees them as new).

Only the standard library; run it with python -I.
"""
from __future__ import annotations

import io
import lzma
import os
import stat
import sys
import tarfile
import zipfile
from pathlib import Path

SKIP = {".mh2-installed"}
ZIP_TIME = (1980, 1, 1, 0, 0, 0)


def entries(root: Path) -> list[tuple[str, Path]]:
    out = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        for n in sorted(dirnames + filenames):
            p = Path(dirpath) / n
            rel = p.relative_to(root).as_posix()
            if rel in SKIP:
                continue
            out.append((rel, p))
    out.sort(key=lambda e: e[0])
    return out


def pack_tar_xz(root: Path, dest: Path) -> None:
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.PAX_FORMAT) as tf:
        for rel, p in entries(root):
            ti = tarfile.TarInfo(rel)
            ti.mtime = 0
            ti.uid = ti.gid = 0
            ti.uname = ti.gname = ""
            st = p.lstat()
            if stat.S_ISLNK(st.st_mode):
                ti.type = tarfile.SYMTYPE
                ti.linkname = os.readlink(p)
                ti.mode = 0o777
                tf.addfile(ti)
            elif stat.S_ISDIR(st.st_mode):
                ti.type = tarfile.DIRTYPE
                ti.mode = 0o755
                tf.addfile(ti)
            else:
                ti.size = st.st_size
                ti.mode = 0o755 if st.st_mode & 0o111 else 0o644
                with open(p, "rb") as f:
                    tf.addfile(ti, f)
    data = lzma.compress(buf.getvalue(), format=lzma.FORMAT_XZ, check=lzma.CHECK_CRC64,
                         preset=9 | lzma.PRESET_EXTREME)
    dest.write_bytes(data)


def pack_zip(root: Path, dest: Path) -> None:
    with zipfile.ZipFile(dest, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for rel, p in entries(root):
            st = p.lstat()
            if stat.S_ISLNK(st.st_mode):
                sys.exit(f"pack_ffmpeg: {rel} is a symbolic link: a zip made for Windows holds none")
            if stat.S_ISDIR(st.st_mode):
                zi = zipfile.ZipInfo(rel + "/", ZIP_TIME)
                zi.create_system = 3
                zi.external_attr = (stat.S_IFDIR | 0o755) << 16 | 0x10
                zf.writestr(zi, b"")
            else:
                zi = zipfile.ZipInfo(rel, ZIP_TIME)
                zi.compress_type = zipfile.ZIP_DEFLATED
                zi.create_system = 3
                zi.external_attr = (stat.S_IFREG | (0o755 if st.st_mode & 0o111 else 0o644)) << 16
                zf.writestr(zi, p.read_bytes(), compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__.split("\n\n")[0], file=sys.stderr)
        return 2
    system, d, out = sys.argv[1], Path(sys.argv[2]), Path(sys.argv[3])
    if not (d / ".mh2-ffmpeg-stamp").is_file():
        sys.exit(f"pack_ffmpeg: {d} holds no .mh2-ffmpeg-stamp: it is not an output directory of build_ffmpeg.sh")
    if system == "linux-x64" and out.name.endswith(".tar.xz"):
        pack_tar_xz(d, out)
    elif system == "windows-x64" and out.name.endswith(".zip"):
        pack_zip(d, out)
    else:
        sys.exit("pack_ffmpeg: linux-x64 makes a .tar.xz, windows-x64 a .zip")
    print(f"{out}: {out.stat().st_size} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
