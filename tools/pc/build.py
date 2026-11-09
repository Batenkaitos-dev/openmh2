#!/usr/bin/env python3
"""Configures and builds OpenMH2 (CMake and Ninja).

    python3 tools/pc/build.py [--target core|host] [--platform linux|windows] [--check] [--clean] [-j N]
                              [--llvm DIR] [--llvm-mingw DIR]

core (the default) is mh2-core, the game and its platform layer, 32-bit x86: on Linux clang -m32
(pc/cmake/linux-i686.cmake), on Windows llvm-mingw's i686 target (pc/cmake/mingw-i686.cmake). It is built into
build/pc/<platform>-i686. host is mh2, 64-bit, with the same compilers (pc/host/cmake/*-x86_64.cmake) and SDL3 from
CMAKE_PREFIX_PATH (tools/env.sh, env.ps1), built into build/pc/<platform>-x86_64 with its renderer (mh2gs) and the
movie library (mh2movie) beside it. Both use clang LLVM_VERSION (tools/versions.txt): on Linux the LLVM release that
tools/setup.sh installs in tools/deps/linux-x64/llvm, on Windows llvm-mingw's; the build stops on another version.
The split and the generated sources must have been made first (README.md, "Building").

--check runs the unit tests (ctest) after the build and, for the core, the checks of the build itself.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(os.path.abspath(__file__)).parent.parent.parent


def run(cmd, **kw):
    print("+", " ".join(str(c) for c in cmd), flush=True)
    r = subprocess.run([str(c) for c in cmd], cwd=ROOT, **kw)
    if r.returncode:
        sys.exit(f"build.py: failed ({r.returncode}): {cmd[0]}")


def pinned_llvm() -> str:
    """LLVM_VERSION of tools/versions.txt: the clang of both systems."""
    for line in (ROOT / "tools" / "versions.txt").read_text().splitlines():
        if line.startswith("LLVM_VERSION="):
            return line.split("=", 1)[1].strip()
    sys.exit("build.py: tools/versions.txt has no LLVM_VERSION")


def check_clang(cc: Path):
    """The build's clang is the pinned version, on both systems."""
    want = pinned_llvm()
    if not cc.exists():
        sys.exit(f"build.py: no {cc}: run tools/setup.sh (Linux) or tools/setup.ps1 (Windows), or give --llvm / --llvm-mingw")
    out = subprocess.run([str(cc), "--version"], capture_output=True, text=True).stdout
    m = re.search(r"clang version (\d+\.\d+\.\d+)", out)
    if not m or m.group(1) != want:
        sys.exit(f"build.py: {cc} is clang {m.group(1) if m else '(unknown)'}; the pinned version is {want} "
                 "(tools/versions.txt, LLVM_VERSION)")
    print(f"build.py: clang {want} ({cc})", flush=True)


def stamp_of(files, extra) -> str:
    """A hash of the files' contents (an absent file counts as empty) and some strings."""
    h = hashlib.sha256()
    for f in files:
        h.update(f.name.encode() + b"\0" + (f.read_bytes().replace(b"\r\n", b"\n") if f.exists() else b"") + b"\0")
    for e in extra:
        h.update(e.encode() + b"\0")
    return h.hexdigest()[:16]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--target", choices=["core", "host"], default="core")
    ap.add_argument("--platform", choices=["linux", "windows"], default="windows" if os.name == "nt" else "linux")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--clean", action="store_true")
    ap.add_argument("-j", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--llvm-mingw", default=os.environ.get("MH2_LLVM_MINGW"),
                    help="llvm-mingw's directory (Windows; default tools/deps/windows-x64/llvm-mingw, or $MH2_LLVM_MINGW)")
    ap.add_argument("--kit", action="store_true",
                    help="the release's kit (tools/pc/dist.py): on Linux the core is linked statically against the kit's C library")
    ap.add_argument("--llvm", default=os.environ.get("MH2_LLVM"),
                    help="the LLVM release's directory (Linux; default tools/deps/linux-x64/llvm, or $MH2_LLVM)")
    a = ap.parse_args()
    if a.platform == "windows" and os.name != "nt":
        sys.exit("build.py: --platform windows builds on Windows (tools/setup.ps1, then this script from PowerShell)")
    arch = "i686" if a.target == "core" else "x86_64"
    # the core's toolchain files are pc/cmake/<os>-i686.cmake; the host's (pc/host/cmake/<os>-x86_64.cmake)
    # include them, so they take the pinned compiler, and change only the target
    tool = {"linux": "linux-", "windows": "mingw-"}[a.platform] + arch
    tooldir = ROOT / "pc" / ("cmake" if a.target == "core" else "host/cmake")
    if a.kit and a.platform == "linux" and a.target == "core":
        tooldir = ROOT / "tools" / "pc" / "kit"
    bdir = ROOT / "build" / "pc" / f"{a.platform}-{arch}"
    if a.clean and bdir.exists():
        shutil.rmtree(bdir)
    # The generators (data_c.py needs pyelftools and pyyaml) run under this interpreter, the toolchain's venv
    # once tools/env.sh or env.ps1 is sourced: CMake's own search prefers the system's Python on Windows.
    cfg = ["cmake", "-S", ROOT / "pc", "-B", bdir, "-G", "Ninja", f"-DCMAKE_TOOLCHAIN_FILE={tooldir / (tool + '.cmake')}",
           "-DCMAKE_BUILD_TYPE=Release", f"-DMH2_TARGET={a.target}", f"-DPython3_EXECUTABLE={Path(sys.executable).as_posix()}"]
    if a.platform == "windows":
        tdir = Path(a.llvm_mingw) if a.llvm_mingw else ROOT / "tools" / "deps" / "windows-x64" / "llvm-mingw"
        cc = tdir / "bin" / f"{arch}-w64-mingw32-clang.exe"
        if a.llvm_mingw:
            cfg.append(f"-DMH2_LLVM_MINGW={tdir.as_posix()}")
    else:
        tdir = Path(a.llvm) if a.llvm else ROOT / "tools" / "deps" / "linux-x64" / "llvm"
        cc = tdir / "bin" / "clang"
        if a.llvm:
            cfg.append(f"-DMH2_LLVM={tdir.as_posix()}")
    check_clang(cc)
    # CMake keeps the first configure's cache (the linker flags of the toolchain file among them), and ninja
    # doesn't know the compiler launchers' scripts. So a change of either is noticed here: the toolchain
    # (its files, the clang version and path) starts a fresh configure, the launchers a rebuild of every object.
    tool_files = [tooldir / (tool + ".cmake"), ROOT / "pc" / "cmake" / (tool.split("-")[0] + "-i686.cmake")]
    stamp_tool = stamp_of(tool_files, [pinned_llvm(), str(cc)])
    launchers = [ROOT / "tools" / "pc" / "stackalign.py", ROOT / "tools" / "pc" / "literals.py",
                 ROOT / "tools" / "pc" / "ps2fp" / "ps2fpcc.py", ROOT / "tools" / "pc" / "ps2fp" / "ps2fp.py"]
    stamp_launch = stamp_of(launchers, [])
    sfile = bdir / "mh2-build-stamp.txt"
    old = ((sfile.read_text().split() if sfile.exists() else []) + ["", ""])[:2]
    if (bdir / "CMakeCache.txt").exists() and old[0] != stamp_tool:
        print("build.py: the toolchain changed since this directory's configure: configuring afresh", flush=True)
        (bdir / "CMakeCache.txt").unlink()
        shutil.rmtree(bdir / "CMakeFiles", ignore_errors=True)
    run(cfg)
    if (bdir / ".ninja_log").exists() and old[1] != stamp_launch:
        print("build.py: the compiler launchers changed: rebuilding every object", flush=True)
        run(["cmake", "--build", bdir, "--target", "clean"])
    sfile.write_text(f"{stamp_tool} {stamp_launch}\n")
    run(["cmake", "--build", bdir, "-j", a.j])
    if a.check:
        run(["ctest", "--test-dir", bdir, "--output-on-failure", "-j", a.j])
        if a.target == "core":
            run([sys.executable, ROOT / "tools" / "pc" / "sources.py", "--verify-broken"])
            run([sys.executable, ROOT / "tools" / "pc" / "gen_stubs.py", "--self-test"])
            # no function may address its frame through ESI, which regcall passes arguments in (bpcheck.py)
            od = tdir / "bin" / ("llvm-objdump.exe" if a.platform == "windows" else "llvm-objdump")
            run([sys.executable, ROOT / "tools" / "pc" / "bpcheck.py", bdir, "-j", a.j] +
                (["--objdump", od] if od.exists() else []))
    exe = "core/mh2-core" if a.target == "core" else "mh2"
    print(f"build.py: OK ({bdir.relative_to(ROOT)}/{exe}{'.exe' if a.platform == 'windows' else ''})")


if __name__ == "__main__":
    main()
