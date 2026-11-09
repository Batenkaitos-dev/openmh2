#!/usr/bin/env python3
"""Makes the player's release folder: the prebuilt host programs and the private build kit.

    python3 tools/pc/dist.py --out DIR [--platform linux|windows] [--deps DIR] [--host-dir DIR] [--no-kit] [--archive]

DIR/mh2-PLATFORM/ holds:
  mh2, mh2gs, mh2movie, SDL3 and FFmpeg's five libraries    the host: no byte of the game
  README.txt, licenses/                                       the player's first steps; the licences and source offers
  kit/                                                        what builds mh2-core from the player's disc:
    python/      a private Python with the build's packages (CMake and Ninja among them)
    toolchain/   the pinned compiler's 32-bit x86 part (Linux: with a static 32-bit C library and its linker's libraries)
    src/         the source the core needs, and the build's scripts
    kit.txt      the kit's identity (what the launcher compares with the core it finds)
Nothing derived from the game goes in: mh2-core, the archive and the movie table are made on the player's machine.
--deps is the folder with the installed tools (default tools/deps/<platform>-x64; the Windows kit can be made from Linux
with --deps naming a Windows installation). --host-dir holds the host build (default build/pc/<platform>-x86_64).
The Linux kit's C library comes from the Ubuntu packages pinned in tools/versions.txt (KIT_*), downloaded once into
tools/deps/downloads/kit and checked against their SHA-256.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import os
import re
import shutil
import struct
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

ROOT = Path(os.path.abspath(__file__)).parent.parent.parent

# what the core's build does not read (relative to the source tree)
SRC_TOP = ["config", "include", "src", "pc", "tools", "LICENSE", "THIRD_PARTY.txt", "pyproject.toml"]
SRC_DENY = ["pc/host", "pc/gs", "pc/movie", "tools/deps", "tools/setup.sh", "tools/setup.ps1",
            "tools/env.sh", "tools/env.ps1", "tools/check_env.py", "tools/verify_originals.py", "tools/pc/dist.py",
            "tools/pc/census.py", "tools/pc/vu1rec.py.orig"]
PY_DENY = {"test", "tests", "idlelib", "tkinter", "turtledemo", "ensurepip", "lib2to3", "__pycache__", "idle_test", "turtle.py",
           "pydoc_data", "unittest"}


def versions() -> dict:
    v = {}
    for line in (ROOT / "tools" / "versions.txt").read_text().splitlines():
        if line and not line.startswith("#") and "=" in line:
            k, x = line.split("=", 1)
            v[k.strip()] = x.strip()
    return v


def sha256(p: Path) -> str:
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def copy(src: Path, dst: Path, deny=lambda rel, p: False, base: Path | None = None):
    """Copies a tree, leaving out what DENY says (DENY gets the path relative to BASE)."""
    base = base or src
    if src.is_symlink():
        dst.parent.mkdir(parents=True, exist_ok=True)
        if dst.is_symlink() or dst.exists():
            dst.unlink()
        os.symlink(os.readlink(src), dst)
        return
    if src.is_dir():
        for c in sorted(src.iterdir()):
            rel = c.relative_to(base).as_posix()
            if not deny(rel, c):
                copy(c, dst / c.name, deny, base)
        dst.mkdir(parents=True, exist_ok=True)
        return
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)


def size_of(p: Path) -> int:
    return sum(f.stat().st_size for f in p.rglob("*") if f.is_file() and not f.is_symlink())


def mb(n: int) -> str:
    return f"{n / 1e6:.0f} MB"


# ---------------------------------------------------------------- the host

def patch_runpath(path: Path):
    """The host's libraries sit beside it: RUNPATH becomes $ORIGIN (the build's own is the build tree's absolute path)."""
    d = bytearray(path.read_bytes())
    if d[:4] != b"\x7fELF" or d[4] != 2:
        return
    shoff, = struct.unpack_from("<Q", d, 0x28)
    shentsize, shnum = struct.unpack_from("<HH", d, 0x3A)
    secs = [struct.unpack_from("<IIQQQQIIQQ", d, shoff + i * shentsize) for i in range(shnum)]
    for name, typ, flags, addr, off, size, link, info, align, entsize in secs:
        if typ != 6:   # SHT_DYNAMIC
            continue
        stroff = secs[link][4]
        for i in range(size // 16):
            tag, val = struct.unpack_from("<qQ", d, off + i * 16)
            if tag in (15, 29):   # DT_RPATH, DT_RUNPATH
                s = stroff + val
                e = d.index(b"\0", s)
                if e - s >= 7:
                    d[s:e] = b"$ORIGIN".ljust(e - s, b"\0")
    path.write_bytes(d)


def host_files(a, out: Path, deps: Path, v: dict):
    hd = Path(a.host_dir)
    out.mkdir(parents=True, exist_ok=True)
    if a.platform == "windows":
        for f in sorted(hd.iterdir()):
            if f.is_file() and f.suffix.lower() in (".exe", ".dll") and not f.name.startswith(("avdevice", "avfilter")):
                shutil.copy2(f, out / f.name)
        if not (out / "SDL3.dll").exists():
            shutil.copy2(deps / "sdl3" / "bin" / "SDL3.dll", out)
        for f in sorted((deps / "ffmpeg" / "bin").glob("*.dll")):
            if f.name.split("-")[0] in ("avcodec", "avformat", "avutil", "swresample", "swscale"):
                shutil.copy2(f, out / f.name)
    else:
        for n in ("mh2", "mh2gs.so", "mh2movie.so"):
            shutil.copy2(hd / n, out / n)
            patch_runpath(out / n)
        shutil.copy2((deps / "sdl3" / "lib" / "libSDL3.so.0").resolve(), out / "libSDL3.so.0")
        for n in ("avcodec", "avformat", "avutil", "swresample", "swscale"):
            for f in sorted((deps / "ffmpeg" / "lib").glob(f"lib{n}.so.*")):
                if f.is_symlink() and f.name.count(".") == 2:   # libX.so.N
                    shutil.copy2(f.resolve(), out / f.name)


# ---------------------------------------------------------------- the kit

def fetch(url_path: str, want: str, v: dict, base: str = "KIT_DEB_BASE") -> Path:
    d = ROOT / "tools" / "deps" / "downloads" / "kit"
    d.mkdir(parents=True, exist_ok=True)
    f = d / url_path.rsplit("/", 1)[1]
    if not f.exists() or sha256(f) != want:
        url = v[base].rstrip("/") + "/" + url_path
        print(f"dist: downloading {url}", flush=True)
        with urllib.request.urlopen(url, timeout=120) as r, open(f, "wb") as o:
            shutil.copyfileobj(r, o)
        if sha256(f) != want:
            f.unlink()
            sys.exit(f"dist: {url}: SHA-256 mismatch")
    return f


def deb_members(deb: Path):
    """The files of a .deb's data archive: (name, tarinfo, bytes-reader)."""
    raw = deb.read_bytes()
    assert raw[:8] == b"!<arch>\n"
    pos = 8
    while pos < len(raw):
        name = raw[pos:pos + 16].decode().strip()
        size = int(raw[pos + 48:pos + 58])
        body = raw[pos + 60:pos + 60 + size]
        pos += 60 + size + (size & 1)
        if name.startswith("data.tar"):
            mode = "r:xz" if name.endswith(".xz") else "r:zst" if name.endswith(".zst") else "r:*"
            return tarfile.open(fileobj=io.BytesIO(body), mode=mode)
    raise SystemExit(f"dist: {deb}: no data archive")


def unpack(deb: Path, dst: Path, want):
    """Extracts the members WANT (a function of the path inside the package) accepts."""
    with deb_members(deb) as t:
        for m in t:
            p = m.name.lstrip("./")
            if not want(p) or not (m.isfile() or m.issym()):
                continue
            tgt = dst / p
            tgt.parent.mkdir(parents=True, exist_ok=True)
            if m.issym():
                continue
            with t.extractfile(m) as fi, open(tgt, "wb") as fo:
                shutil.copyfileobj(fi, fo)
            os.chmod(tgt, 0o755 if p.endswith((".so", ".o")) or "/bin/" in p else 0o644)


def linux_sysroot(dst: Path, lic: Path, v: dict):
    """A 32-bit static C library and its headers: Ubuntu's glibc 2.39 (LGPL-2.1+) and the kernel headers, from the pinned packages."""
    debs = {k[len("KIT_DEB_"):-len("_PATH")].lower(): fetch(x, v[k[:-5] + "_SHA256"], v)
            for k, x in v.items() if k.startswith("KIT_DEB_") and k.endswith("_PATH")}
    unpack(debs["libc6_dev"], dst, lambda p: p.startswith("usr/include/") or p == "usr/share/doc/libc6-dev/copyright")
    unpack(debs["libc6_dev_i386"], dst, lambda p: (p.startswith("usr/lib32/") and (p.endswith((".a", ".o")))) or p.startswith("usr/include/"))
    unpack(debs["linux_libc_dev"], dst, lambda p: p.startswith(("usr/include/linux/", "usr/include/asm-generic/", "usr/include/x86_64-linux-gnu/asm/")))
    # clang looks for an i686 target's headers under i386-linux-gnu; glibc's are the same files for both word sizes
    shutil.copytree(dst / "usr/include/x86_64-linux-gnu", dst / "usr/include/i386-linux-gnu", dirs_exist_ok=True)
    lic.mkdir(parents=True, exist_ok=True)
    for deb, name, out in (("libc6_dev", "libc6-dev", "glibc-copyright.txt"), ("linux_libc_dev", "linux-libc-dev", "linux-headers-copyright.txt")):
        with deb_members(debs[deb]) as t:
            for m in t:
                if m.name.lstrip("./") == f"usr/share/doc/{name}/copyright":
                    (lic / out).write_bytes(t.extractfile(m).read())
    # the one script glibc leaves that names absolute paths
    (dst / "usr/lib32/libm.a").write_text("GROUP ( libm-2.39.a libmvec.a )\n") if (dst / "usr/lib32/libm-2.39.a").exists() else None


def run_tool(cmd: list, **kw):
    r = subprocess.run([str(c) for c in cmd], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, **kw)
    if r.returncode:
        sys.exit(f"dist: {cmd[0]} failed:\n" + "\n".join(r.stdout.splitlines()[-25:]))


# the parts of LLVM's source the runtime libraries build from
LLVM_SRC_PARTS = ("compiler-rt/", "libunwind/", "runtimes/", "cmake/", "libcxx/cmake/", "llvm/cmake/", "libc/cmake/")


def linux_runtimes(tc: Path, sysroot: Path, deps: Path, lic: Path, v: dict) -> Path:
    """Builds, with the kit's own clang, what a 32-bit static program needs from the compiler's runtime: compiler-rt's builtins and
    crtbegin/crtend, libunwind, and the 128-bit float comparisons glibc's printf calls (tools/pc/kit/f128.c). LLVM's source is
    Apache-2.0 with the LLVM exception. The result is cached, keyed by the source's hash and this script's rules."""
    src = fetch(v["KIT_LLVM_SRC_PATH"], v["KIT_LLVM_SRC_SHA256"], v, "KIT_GH_BASE")
    f128 = ROOT / "tools" / "pc" / "kit" / "f128.c"
    key = hashlib.sha256(v["KIT_LLVM_SRC_SHA256"].encode() + f128.read_bytes() + v["KIT_MINGW_LINUX_SHA256"].encode() + b"rules 2").hexdigest()[:12]
    work = src.parent / f"rt-{key}"
    out = work / "out"
    names = ("libclang_rt.builtins.a", "clang_rt.crtbegin.o", "clang_rt.crtend.o", "libunwind.a", "libmh2f128.a")
    if all((out / n).exists() for n in names) and (work / "LICENSES").exists():
        return out
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    top = "llvm-project-" + v["LLVM_VERSION"] + ".src/"
    print("dist: unpacking LLVM's runtime sources", flush=True)
    lics = work / "LICENSES"
    lics.mkdir()
    with tarfile.open(src, "r:xz") as t:
        for m in t:
            rel = m.name[len(top):] if m.name.startswith(top) else ""
            if rel in ("llvm/LICENSE.TXT", "compiler-rt/LICENSE.TXT", "libunwind/LICENSE.TXT"):
                (lics / (rel.split("/")[0] + "-LICENSE.txt")).write_bytes(t.extractfile(m).read())
            if rel.startswith(LLVM_SRC_PARTS) and (m.isfile() or m.isdir()):
                t.extract(m, work / "src", filter="data")
    s = work / "src" / top.rstrip("/")
    cm = deps / "venv" / "bin" / "cmake"
    ninja = deps / "venv" / "bin" / "ninja"
    cc = tc / "bin" / "clang"
    common = [f"-DCMAKE_MAKE_PROGRAM={ninja}", f"-DCMAKE_AR={tc}/bin/llvm-ar", f"-DCMAKE_RANLIB={tc}/bin/llvm-ranlib",
              f"-DCMAKE_NM={tc}/bin/llvm-nm", "-DCMAKE_SYSTEM_NAME=Linux", "-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY",
              "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_C_COMPILER={cc}", f"-DCMAKE_ASM_COMPILER={cc}"]
    hdr = f"-isystem {sysroot}/usr/include/i386-linux-gnu -isystem {sysroot}/usr/include"
    pm = f"-ffile-prefix-map={work}=/llvm"   # the build's folder does not go into the objects (assertion and __FILE__ strings)
    print("dist: building compiler-rt's builtins", flush=True)
    run_tool([cm, "-S", s / "compiler-rt/lib/builtins", "-B", work / "b1", "-G", "Ninja", *common,
              "-DCMAKE_C_COMPILER_TARGET=i386-unknown-linux-gnu", "-DCMAKE_ASM_COMPILER_TARGET=i386-unknown-linux-gnu",
              "-DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON", "-DCOMPILER_RT_BUILD_BUILTINS=ON", f"-DLLVM_CMAKE_DIR={s}/cmake",
              f"-DCMAKE_C_FLAGS=--sysroot={sysroot} {pm} -nostdinc -isystem {tc}/lib/clang/23/include {hdr}", f"-DCMAKE_ASM_FLAGS=--sysroot={sysroot}"])
    run_tool([ninja, "-C", work / "b1"])
    print("dist: building libunwind", flush=True)
    run_tool([cm, "-S", s / "runtimes", "-B", work / "b2", "-G", "Ninja", *common, "-DLLVM_ENABLE_RUNTIMES=libunwind",
              "-DLLVM_INCLUDE_TESTS=OFF", f"-DCMAKE_CXX_COMPILER={cc}", "-DCMAKE_C_COMPILER_TARGET=i686-linux-gnu",
              "-DCMAKE_CXX_COMPILER_TARGET=i686-linux-gnu", "-DCMAKE_ASM_COMPILER_TARGET=i686-linux-gnu",
              "-DLIBUNWIND_ENABLE_SHARED=OFF", "-DLIBUNWIND_ENABLE_STATIC=ON", "-DLIBUNWIND_USE_COMPILER_RT=ON",
              "-DLIBUNWIND_INCLUDE_TESTS=OFF", "-DLIBUNWIND_INCLUDE_DOCS=OFF", "-DLIBUNWIND_ENABLE_THREADS=ON",
              "-DLIBUNWIND_ENABLE_ASSERTIONS=OFF",
              f"-DCMAKE_C_FLAGS=-m32 --sysroot={sysroot} {pm}", f"-DCMAKE_CXX_FLAGS=-m32 --sysroot={sysroot} {pm} -nostdinc++ {hdr}",
              f"-DCMAKE_ASM_FLAGS=-m32 --sysroot={sysroot} {pm}"])
    run_tool([ninja, "-C", work / "b2"])
    out.mkdir()
    shutil.copy2(work / "b1/lib/linux/libclang_rt.builtins-i386.a", out / "libclang_rt.builtins.a")
    shutil.copy2(work / "b1/lib/linux/clang_rt.crtbegin-i386.o", out / "clang_rt.crtbegin.o")
    shutil.copy2(work / "b1/lib/linux/clang_rt.crtend-i386.o", out / "clang_rt.crtend.o")
    shutil.copy2(work / "b2/lib/libunwind.a", out / "libunwind.a")
    print("dist: building the 128-bit float comparisons", flush=True)
    run_tool([cc, "--target=i686-linux-gnu", "-m32", "-O2", "-fno-pie", f"--sysroot={sysroot}", pm, "-c", f128, "-o", work / "f128.o"])
    run_tool([tc / "bin" / "llvm-ar", "rcs", out / "libmh2f128.a", work / "f128.o"])
    return out


def linux_toolchain(dst: Path, lic: Path, deps: Path, v: dict):
    """The compiler: llvm-mingw's Linux build (the pinned LLVM commit; its lld needs only zlib and zstd), the glibc sysroot and the runtime."""
    tar = fetch(v["KIT_MINGW_LINUX_PATH"], v["KIT_MINGW_LINUX_SHA256"], v, "KIT_GH_BASE")
    keep_bin = ("clang-23", "lld", "llvm-ar", "llvm-nm")
    keep_lib = ("libLLVM.so.23.1", "libclang-cpp.so.23.1")
    with tarfile.open(tar, "r:xz") as t:
        for m in t:
            rel = m.name.split("/", 1)[1] if "/" in m.name else ""
            ok = (rel.startswith("bin/") and rel[4:] in keep_bin) or (rel.startswith("lib/") and rel[4:] in keep_lib) or \
                 rel.startswith("lib/clang/23/include/") or rel == "LICENSE.TXT"
            if not ok or not m.isfile():
                continue
            tgt = dst / rel
            tgt.parent.mkdir(parents=True, exist_ok=True)
            with t.extractfile(m) as fi, open(tgt, "wb") as fo:
                shutil.copyfileobj(fi, fo)
            os.chmod(tgt, 0o755 if rel.startswith(("bin/", "lib/lib")) else 0o644)
    lic.mkdir(parents=True, exist_ok=True)
    shutil.move(dst / "LICENSE.TXT", lic / "llvm-mingw-LICENSE.txt")
    for n, tgt in (("clang", "clang-23"), ("ld.lld", "lld"), ("llvm-ranlib", "llvm-ar"), ("nm", "llvm-nm")):
        os.symlink(tgt, dst / "bin" / n)
    linux_sysroot(dst / "sysroot", lic, v)
    rt = linux_runtimes(dst, dst / "sysroot", deps, lic, v)
    rd = dst / "lib" / "clang" / "23" / "lib" / "i686-unknown-linux-gnu"
    rd.mkdir(parents=True)
    for f in sorted(rt.iterdir()):
        shutil.copy2(f, rd / f.name)
    for f in sorted((rt.parent / "LICENSES").iterdir()):
        shutil.copy2(f, lic / f.name)


def kit_python(deps: Path, platform: str, dst: Path):
    base = deps / "python" / ("cpython-3.14.7-windows-x86_64-none" if platform == "windows" else "cpython-3.14.7-linux-x86_64-gnu")
    sp_src = deps / "venv" / ("Lib" if platform == "windows" else "lib/python3.14") / "site-packages"
    if not base.exists() or not sp_src.exists():
        sys.exit(f"dist: no {base} or {sp_src}: run tools/setup.{'ps1' if platform == 'windows' else 'sh'}")
    if platform == "windows":
        deny = lambda rel, p: (p.name in PY_DENY or p.name.endswith(".pdb") or rel in ("include", "libs", "Scripts", "Lib/site-packages")
                               or rel.startswith(("Lib/test", "tcl", "DLLs/tcl", "DLLs/tk", "DLLs/_tkinter")) or rel.startswith("Lib/ensurepip"))
        sp_dst = dst / "Lib" / "site-packages"
    else:
        deny = lambda rel, p: (p.name in PY_DENY or rel in ("include", "share", "lib/python3.14/site-packages", "lib/python3.14/config-3.14-x86_64-linux-gnu")
                               or rel.startswith("lib/tcl") or rel.startswith("lib/itcl") or rel.startswith("lib/thread"))
        sp_dst = dst / "lib" / "python3.14" / "site-packages"
    copy(base, dst, deny)
    sdeny = lambda rel, p: (p.name in ("__pycache__", "pip", "_virtualenv.py", "_virtualenv.pth", "graphviz", "docs", "Help", "doc",
                                       "m2c", "m2c_pycparser")   # m2c is GPL-3.0 and splat's split does not import it
                            or p.name.startswith(("pip-", "graphviz", "m2c-")) or "cmake/data/share/cmake-" in rel and rel.endswith(("/Help",)))
    copy(sp_src, sp_dst, sdeny)
    # Ninja's wheel puts its program in the environment's scripts folder, not in the package
    nin = deps / "venv" / ("Scripts" if platform == "windows" else "bin") / ("ninja.exe" if platform == "windows" else "ninja")
    shutil.copy2(nin, dst / ("ninja.exe" if platform == "windows" else "bin/ninja"))


def kit_toolchain(deps: Path, platform: str, dst: Path, lic: Path, v: dict):
    if platform == "windows":
        t = deps / "llvm-mingw"
        keep = {"clang.exe", "clang-23.exe", "i686-w64-mingw32-clang.exe", "i686-w64-windows-gnu.cfg", "i686-pc-windows-gnu.cfg",
                "ld.lld.exe", "lld.exe", "lld-link.exe", "llvm-ar.exe", "llvm-ranlib.exe", "llvm-nm.exe", "nm.exe", "i686-w64-mingw32-nm.exe", "llvm-objdump.exe", "llvm-rc.exe",
                "llvm-windres.exe", "i686-w64-mingw32-windres.exe", "windres.exe", "llvm-cvtres.exe", "llvm-dlltool.exe",
                "llvm-objcopy.exe", "llvm-strip.exe", "clang-target-wrapper.exe", "libLLVM-23.dll", "libclang-cpp.dll",
                "libc++.dll", "libunwind.dll", "libwinpthread-1.dll"}
        (dst / "bin").mkdir(parents=True, exist_ok=True)
        for n in sorted(keep):
            if (t / "bin" / n).exists():
                shutil.copy2(t / "bin" / n, dst / "bin" / n)
        for f in sorted((t / "bin").glob("*.cfg")):   # the wrappers read their target options from these
            shutil.copy2(f, dst / "bin" / f.name)
        copy(t / "include", dst / "include", lambda rel, p: p.name in ("c++",) or p.suffix in (".idl",))
        copy(t / "i686-w64-mingw32", dst / "i686-w64-mingw32", lambda rel, p: "c++" in p.name or p.suffix in (".dll",) and "bin" in rel)
        rd = t / "lib" / "clang" / "23"
        copy(rd / "include", dst / "lib" / "clang" / "23" / "include")
        for f in (rd / "lib" / "windows").glob("*builtins-i386.a"):
            (dst / "lib/clang/23/lib/windows").mkdir(parents=True, exist_ok=True)
            shutil.copy2(f, dst / "lib/clang/23/lib/windows" / f.name)
        (dst / "LICENSE.TXT").write_bytes((t / "LICENSE.TXT").read_bytes())
        (lic / "llvm-mingw-LICENSE.txt").write_bytes((t / "LICENSE.TXT").read_bytes())   # COMPONENTS.txt names it (the Linux kit moves its copy there)
    else:
        linux_toolchain(dst, lic, deps, v)


def kit_src(dst: Path, srcroot: Path):
    for top in SRC_TOP:
        s = srcroot / top
        if not s.exists():
            continue
        deny = lambda rel, p: (top + "/" + rel) in SRC_DENY or p.name in ("__pycache__",) or p.suffix == ".pyc" or \
            (top + "/" + rel).startswith(tuple(d + "/" for d in SRC_DENY))
        if s.is_dir():
            copy(s, dst / top, deny)
        else:
            dst.mkdir(parents=True, exist_ok=True)
            shutil.copy2(s, dst / top)


def tree_id(p: Path) -> str:
    h = hashlib.sha256()
    for f in sorted(p.rglob("*")):
        if f.is_file() and not f.is_symlink():
            h.update(f.relative_to(p).as_posix().encode() + b"\0")
            h.update(f.read_bytes().replace(b"\r\n", b"\n") + b"\0")
    return h.hexdigest()[:16]


def dist_info(sp: Path):
    """(name, version, licence text, [licence files]) of each installed package of a site-packages folder."""
    import email.parser
    for d in sorted(sp.glob("*.dist-info")):
        md = email.parser.Parser().parsestr((d / "METADATA").read_text(encoding="utf-8", errors="replace"))
        lic = md.get("License-Expression") or md.get("License") or ""
        if not lic or len(lic) > 80 or "\n" in lic:
            cl = [c.split("::")[-1].strip() for c in md.get_all("Classifier") or [] if c.startswith("License ::")]
            lic = "; ".join(cl) or (lic.split("\n")[0][:60] if lic else "see the package's files")
        files = [f for f in sorted(d.rglob("*")) if f.is_file() and f.parent.name in ("licenses", "license") or
                 f.is_file() and f.name.upper().startswith(("LICENSE", "COPYING", "NOTICE", "AUTHORS")) and f.parent == d]
        if lic.startswith("see"):   # no licence in the metadata: the package's licence file or its SBOM says
            for f in files:
                if "MIT License" in f.read_text(encoding="utf-8", errors="replace")[:200]:
                    lic = "MIT"
            sb = sorted((d / "sboms").glob("*.json")) if (d / "sboms").is_dir() else []
            if sb:
                import json
                comp = json.loads(sb[0].read_text(encoding="utf-8"))["metadata"]["component"]
                lic = " ".join(x.get("expression") or x.get("license", {}).get("id", "") for x in comp.get("licenses", [])) or lic
                files = files + sb
        tops = set()
        rec = d / "RECORD"
        if rec.exists():
            for line in rec.read_text(encoding="utf-8", errors="replace").splitlines():
                f = line.split(",")[0]
                if not f.startswith(".."):
                    tops.add(f.split("/")[0])
        yield md["Name"], md["Version"], lic, files, tops


def write_licenses(out: Path, plat: str, deps: Path, v: dict, with_kit: bool, srcroot: Path | None = None):
    """licenses/: the notices and texts of everything in the folder, the source offers, and an index (COMPONENTS.txt) that names every
    component of the host and the kit with its licence."""
    lic = out / "licenses"
    lic.mkdir(exist_ok=True)
    for n in ("LGPL-2.1", "LGPL-3.0", "GPL-3.0", "Apache-2.0"):
        shutil.copy2(ROOT / "tools" / "pc" / "kit" / "licenses" / f"{n}.txt", lic / f"{n}.txt")
    for dst, rels in (("OFL.txt", ["pc/host/fonts/OFL.txt"]),
                      ("SDL3-LICENSE.txt", ["tools/deps/sdl3/share/licenses/SDL3/LICENSE.txt"]),
                      ("mh2gs-Granite-LICENSE.txt", ["tools/deps/src/parallel-gs/Granite/LICENSE"]),
                      ("mh2gs-volk-LICENSE.txt", ["tools/deps/src/parallel-gs/Granite/third_party/volk/LICENSE.md"])):
        for base in [b for b in (srcroot, ROOT) if b]:
            f = base / rels[0]
            if rels[0].startswith("tools/deps/sdl3"):
                f = deps / "sdl3" / "share" / "licenses" / "SDL3" / "LICENSE.txt"
            if f.is_file():
                shutil.copy2(f, lic / dst)
                break
        else:
            sys.exit(f"dist: the licence text {dst} was not found ({rels[0]})")
    rows = [("mh2 and mh2movie (the launcher, the platform layer, the movie player)", "", "CC0-1.0", "LICENSE"),
            ("SDL3", v.get("SDL3_VERSION", ""), "zlib", "SDL3-LICENSE.txt, THIRD_PARTY.txt"),
            ("Noto Sans Regular (the launcher's font, embedded in mh2)", "v2.015", "SIL Open Font License 1.1", "OFL.txt, THIRD_PARTY.txt"),
            ("stb_truetype (in mh2), stb_image and stb_image_write (in mh2gs)", "", "public domain (Unlicense) or MIT, at the user's choice: the public-domain option is taken",
             "THIRD_PARTY.txt"),
            ("mh2gs (paraLLEl-GS, Granite, volk, Vulkan-Headers)", v.get("PARALLEL_GS_COMMIT", "")[:9], "LGPL-3.0-or-later; MIT; Apache-2.0 or MIT",
             "LGPL-3.0.txt, GPL-3.0.txt, mh2gs-Granite-LICENSE.txt, mh2gs-volk-LICENSE.txt, Apache-2.0.txt, SOURCE_OFFER.txt"),
            ("FFmpeg (avcodec, avformat, avutil, swresample, swscale)", v.get("FFMPEG_VERSION", ""),
             "LGPL-2.1-or-later (the project's own build, configured without --enable-version3, --enable-gpl and --enable-nonfree and with no external library; some files carry MIT or BSD-style licences)",
             "ffmpeg-COPYING.LGPLv2.1.txt, ffmpeg-LICENSE.md, SOURCE_OFFER.txt")]
    if with_kit:
        py = out / "kit" / "python"
        pyl = next(iter(sorted(py.rglob("LICENSE.txt"))), None)
        if pyl:
            shutil.copy2(pyl, lic / "python-LICENSE.txt")
        rows.append(("Python (python-build-standalone, uv's)", v.get("PYTHON_VERSION", ""), "PSF-2.0 (and the licences of its parts: python-LICENSE.txt)", "python-LICENSE.txt"))
        sp = py / ("Lib" if plat == "windows" else "lib/python3.14") / "site-packages"
        pkg_of = {}
        pk = ["The Python packages in kit/python (name, version, licence as the package declares it); their licence files are in python-packages/.", ""]
        for name, ver, ltxt, files, tops in dist_info(sp):
            pkg_of.update({t: (name, ltxt) for t in tops})
            pk.append(f"{name} {ver}: {ltxt}")
            for f in files:
                t = lic / "python-packages" / name
                t.mkdir(parents=True, exist_ok=True)
                shutil.copy2(f, t / f.name)
            if name.lower() in ("cmake", "ninja"):
                rows.append((f"{name} (PyPI wheel; kit/python)", ver, ltxt, f"python-packages/{name}/"))
        (lic / "python-packages.txt").write_text("\n".join(pk) + "\n")
        rows.append(("the Python packages (splat, spimdisasm, rabbitizer, pyelftools, pycdlib, PyYAML, crunch64 ...)", "", "MIT, BSD, public domain; pycdlib LGPL-2.1 (shipped unmodified)", "python-packages.txt"))
        rows.append(("LLVM: clang, lld, llvm-ar, llvm-nm, resource headers", v.get("LLVM_VERSION", ""), "Apache-2.0 with LLVM exceptions", "llvm-mingw-LICENSE.txt (LLVM's, then the other parts of that release)"))
        if plat == "linux":
            rows += [("libLLVM, libclang-cpp (llvm-mingw's Linux build; the toolchain's shared libraries)", v.get("LLVM_VERSION", ""), "Apache-2.0 with LLVM exceptions", "llvm-mingw-LICENSE.txt"),
                     ("compiler-rt builtins and crt files, libunwind (built by dist.py from LLVM's source)", v.get("LLVM_VERSION", ""), "Apache-2.0 with LLVM exceptions",
                      "compiler-rt-LICENSE.txt, libunwind-LICENSE.txt, llvm-LICENSE.txt"),
                     ("f128.c (kit/src/tools/pc/kit; built into libmh2f128.a)", "", "CC0-1.0", "LICENSE"),
                     ("glibc headers and static libraries (Ubuntu's libc6-dev, libc6-dev-i386)", "2.39", "LGPL-2.1-or-later (some files other, as glibc-copyright.txt lists)",
                      "glibc-copyright.txt, LGPL-2.1.txt, SOURCE_OFFER.txt"),
                     ("Linux kernel user-space API headers (Ubuntu's linux-libc-dev; kit/toolchain/sysroot/usr/include/linux, asm-generic, asm)", "6.8.0",
                      "GPL-2.0 WITH Linux-syscall-note (the note: they are the interface of system calls and do not make a program that includes them a derived work)",
                      "linux-headers-copyright.txt")]
        else:
            rows.append(("mingw-w64 headers and runtime, winpthreads, libc++, libunwind (llvm-mingw)", v.get("LLVM_MINGW_VERSION", ""),
                         "ZPL-2.1 and public domain; MIT; Apache-2.0 with LLVM exceptions", "llvm-mingw-LICENSE.txt"))
        rows.append(("kit/src (the project's source and build scripts)", "", "CC0-1.0 (the game's own code and data are Capcom's and are not in it), except newlib and fdlibm "
                     "(pc/core/crt), zlib (pc/core/third_party/zlib) and two PS2 math routines from PCSX2 (pc/core/third_party/pcsx2_ps2float, "
                     "GPL-3.0-or-later), which keep their own notices in their files. A mh2-core built from kit/src is GPL-3.0-or-later as a "
                     "whole; it stays on the player's PC",
                     "LICENSE, THIRD_PARTY.txt, kit/src/pc/core/crt/THIRD_PARTY.md, kit/src/pc/core/third_party/pcsx2_ps2float/COPYING, GPL-3.0.txt"))
    w = max(len(r[0]) for r in rows)
    idx = ["Every component of this release folder, its licence, and the file here that holds the notice or text.", ""]
    for r in rows:
        idx.append(f"{r[0]}{' ' + r[1] if r[1] else ''}\n    licence: {r[2]}\n    see: {r[3]}")
    (lic / "COMPONENTS.txt").write_text("\n\n".join(idx) + "\n")
    offer = [
        "SOURCE OFFERS", "",
        "The libraries below are licensed so that you may have their source. Each is named exactly here, "
        "with the commit or the version, and the project will send it to anyone who asks.", "",
        "mh2gs (mh2gs.dll / mh2gs.so): paraLLEl-GS, LGPL-3.0-or-later (LGPL-3.0.txt, GPL-3.0.txt).",
        f"  paraLLEl-GS {v.get('PARALLEL_GS_REPO', '')} commit {v.get('PARALLEL_GS_COMMIT', '')},",
        f"  Granite commit {v.get('GRANITE_COMMIT', '')}, volk commit {v.get('VOLK_COMMIT', '')}, Vulkan-Headers commit {v.get('VULKAN_HEADERS_COMMIT', '')},",
        "  and the project's patches to them (pc/gs/patches in the project's source). mh2 loads mh2gs at run time: you can replace it with your own build.", "",
        f"FFmpeg {v.get('FFMPEG_VERSION', '')} (avcodec, avformat, avutil, swresample, swscale), LGPL-2.1-or-later (ffmpeg-COPYING.LGPLv2.1.txt, ffmpeg-LICENSE.md): the project's own",
        "  build of ffmpeg.org's release tarball, shared libraries, only the MPEG program stream demuxer, the MPEG-2 video and ADX decoders and their parsers, with no external library,",
        "  no --enable-version3 and no GPL or nonfree part. The source is that tarball, unchanged:",
        f"    {v.get('FFMPEG_URL', '')}",
        f"    SHA-256 {v.get('FFMPEG_SHA256', '')}",
        "  The libraries are built from it by tools/pc/build_ffmpeg.sh (the project's source; a copy is in this folder as licenses/ffmpeg-build_ffmpeg.sh) with the configure line in",
        "  licenses/ffmpeg-configure.txt (also in each library: avcodec_configuration()). mh2movie loads the libraries at run time: your own build of the same API works in their place.", ""]
    if with_kit and plat == "linux":
        offer += [
            "glibc 2.39 (kit/toolchain/sysroot: headers and static libraries; the Linux game program that the kit builds on your computer is linked with libc.a),",
            "  LGPL-2.1-or-later (LGPL-2.1.txt). The source is Ubuntu's source package glibc 2.39-0ubuntu8.9:",
            "    http://archive.ubuntu.com/ubuntu/pool/main/g/glibc/glibc_2.39.orig.tar.xz",
            "    http://archive.ubuntu.com/ubuntu/pool/main/g/glibc/glibc_2.39-0ubuntu8.9.debian.tar.xz  (and glibc_2.39-0ubuntu8.9.dsc)",
            "  The kit's copy of libc.a is unmodified. The game program is built on your computer and is not distributed by the project; if you give it to someone",
            "  else, pass this offer on with it.", ""]
    (lic / "SOURCE_OFFER.txt").write_text("\n".join(offer) + "\n")
    if with_kit:
        write_file_licences(out, plat, lic, pkg_of)


def write_file_licences(out: Path, plat: str, lic: Path, pkg_of: dict):
    """licenses/FILES.txt: every file of the folder with the licence it is under (a line a file: path, tab, licence)."""
    apache = "Apache-2.0 WITH LLVM-exception"
    host = {"mh2": "CC0-1.0 (SDL3: zlib)", "mh2movie.so": "CC0-1.0", "mh2movie.dll": "CC0-1.0", "mh2.exe": "CC0-1.0 (SDL3: zlib)",
            "mh2gs.so": "LGPL-3.0-or-later (MIT, Apache-2.0 or MIT parts)", "mh2gs.dll": "LGPL-3.0-or-later (MIT, Apache-2.0 or MIT parts)"}
    sp = "kit/python/" + ("Lib" if plat == "windows" else "lib/python3.14") + "/site-packages/"

    def of(rel: str) -> str:
        top = rel.split("/")[0]
        if rel in host:
            return host[rel]
        if top.startswith(("libSDL3", "SDL3")):
            return "zlib"
        if top.startswith(("libav", "libsw", "avcodec", "avformat", "avutil", "swresample", "swscale")):
            return "LGPL-2.1-or-later"
        if top in ("README.txt", "licenses"):
            return "CC0-1.0" if top == "README.txt" else "see licenses/COMPONENTS.txt"
        if rel.startswith(sp):
            name, l = pkg_of.get(rel[len(sp):].split("/")[0], ("", ""))
            return f"{name}: {l}" if name else "see licenses/python-packages.txt"
        if rel == "kit/python/bin/ninja" or rel == "kit/python/ninja.exe":
            return "Apache-2.0 (ninja)"
        if rel.startswith("kit/python/"):
            return "PSF-2.0 (Python)"
        if rel.startswith(("kit/src/", "kit/start/")) or rel == "kit/kit.txt":
            return "CC0-1.0"
        if rel.endswith("libmh2f128.a"):
            return "CC0-1.0 (f128.c)"
        if "/sysroot/usr/include/" in rel and rel.split("/sysroot/usr/include/")[1].split("/")[0] in ("linux", "asm-generic", "asm") or \
                "/include/x86_64-linux-gnu/asm/" in rel or "/include/i386-linux-gnu/asm/" in rel:
            return "GPL-2.0 WITH Linux-syscall-note (kernel user-space API)"
        if "/sysroot/" in rel:
            return "LGPL-2.1-or-later (glibc)"
        if rel.startswith("kit/toolchain/"):
            return apache if plat == "linux" else apache + "; mingw-w64: ZPL-2.1, public domain, MIT, BSD"
        return "unknown"
    lines = []
    for f in sorted(out.rglob("*")):
        if f.is_file() or f.is_symlink():
            rel = f.relative_to(out).as_posix()
            if rel != "licenses/FILES.txt":
                lines.append(f"{rel}\t{of(rel)}")
    (lic / "FILES.txt").write_text("\n".join(lines) + "\n")


def check_named_texts(out: Path, with_kit: bool, srcroot: Path | None = None):
    """Stops when a licence text or notice file that THIRD_PARTY.txt, COMPONENTS.txt or SOURCE_OFFER.txt names is not in the folder."""
    lic = out / "licenses"
    kind = re.compile(r"licen[cs]e|copying|\bOFL\b|notice|third_party|copyright|\bL?GPL|apache", re.I)
    named = {}
    for f in ("THIRD_PARTY.txt", "COMPONENTS.txt", "SOURCE_OFFER.txt"):
        if not (lic / f).is_file():
            if f == "THIRD_PARTY.txt":
                print("dist: no THIRD_PARTY.txt in this source tree (it comes with the export): its names are not checked")
                continue
            sys.exit(f"dist: licenses/{f} is missing")
        t = (lic / f).read_text(errors="replace")
        t = re.sub(r"https?://\S+", " ", t)
        lines = t.splitlines()
        if f == "COMPONENTS.txt":
            lines = [x.split(":", 1)[1] for x in lines if x.strip().startswith("see:")]
        for line in lines:
            for m in re.finditer(r"[A-Za-z0-9_./+-]*[A-Za-z0-9_+-](?:\.(?:txt|md)\b|(?<=LICENSE)|(?<=COPYING))", line):
                tok = m.group(0)
                if kind.search(tok):
                    named.setdefault(tok, f)
    missing = []
    srcbase = out / "kit" / "src" if with_kit else (srcroot or ROOT)   # (the host-only folder, a test: the source tree stands for the kit's)
    # A text that comes with a library the source carries (THIRD_PARTY.txt says "COPYING beside them" of the PS2 math routines) is named
    # by its bare name: the library's own folder under pc/core/third_party holds it.
    thirdparty = sorted((srcbase / "pc" / "core" / "third_party").glob("*/*")) if (srcbase / "pc" / "core" / "third_party").is_dir() else []
    for tok, f in sorted(named.items()):
        cands = [out / tok, lic / tok, lic / Path(tok).name, srcbase / tok]
        if "/" not in tok:
            cands += [c for c in thirdparty if c.name == tok]
        if not any(c.is_file() for c in cands):
            missing.append(f"{tok} (named in {f})")
    # The two PS2 math routines are GPL-3.0-or-later: their licence text and their source note must be in the source the kit carries.
    ps2 = srcbase / "pc" / "core" / "third_party" / "pcsx2_ps2float"
    if ps2.is_dir():
        for n in ("COPYING", "SOURCE.txt"):
            if not (ps2 / n).is_file():
                missing.append(f"pc/core/third_party/pcsx2_ps2float/{n} (the PS2 math routines' licence and source note)")
    if missing:
        sys.exit("dist: licence files that the licence notes name are not in the folder: " + "; ".join(missing))
    print(f"dist: the licence files named in THIRD_PARTY.txt, COMPONENTS.txt and SOURCE_OFFER.txt are all in the folder ({len(named)} names)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True)
    ap.add_argument("--platform", choices=["linux", "windows"], default="windows" if os.name == "nt" else "linux")
    ap.add_argument("--deps")
    ap.add_argument("--host-dir")
    ap.add_argument("--src", help="the source tree the kit takes (default: this one)")
    ap.add_argument("--no-kit", action="store_true", help="the host only")
    ap.add_argument("--archive", action="store_true", help="also write DIR/mh2-PLATFORM.zip (Windows) or .tar.xz (Linux)")
    a = ap.parse_args()
    v = versions()
    plat = a.platform
    deps = Path(a.deps) if a.deps else ROOT / "tools" / "deps" / f"{plat}-x64"
    a.host_dir = a.host_dir or str(ROOT / "build" / "pc" / f"{plat}-x86_64")
    out = Path(a.out) / f"mh2-{plat}"
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    host_files(a, out, deps, v)
    lic = out / "licenses"
    lic.mkdir()
    for n in ("LICENSE", "THIRD_PARTY.txt"):   # (the exported tree's: THIRD_PARTY.txt is the public repository's)
        for base in (Path(a.src) if a.src else ROOT, ROOT):
            if (base / n).exists():
                shutil.copy2(base / n, lic / n)
                break
    for n in ("README.txt",):
        s = ROOT / "tools" / "pc" / "kit" / n
        if s.exists():
            shutil.copy2(s, out / n)
    ff = deps / "ffmpeg"   # the project's own build (tools/pc/build_ffmpeg.sh): the licence texts, the configure line and the script come with the source offer
    for src, dst in ((ff / "COPYING.LGPLv2.1", "ffmpeg-COPYING.LGPLv2.1.txt"), (ff / "LICENSE.md", "ffmpeg-LICENSE.md"),
                     (ff / "configure-line.txt", "ffmpeg-configure.txt"), (ROOT / "tools" / "pc" / "build_ffmpeg.sh", "ffmpeg-build_ffmpeg.sh")):
        if not src.is_file():
            sys.exit(f"dist: {src} is missing: run tools/setup (it installs the FFmpeg that tools/pc/build_ffmpeg.sh builds)")
        shutil.copy2(src, lic / dst)
    if not a.no_kit:
        kit = out / "kit"
        kit_python(deps, plat, kit / "python")
        kit_toolchain(deps, plat, kit / "toolchain", lic, v)
        kit_src(kit / "src", Path(a.src) if a.src else ROOT)
        (kit / "start").mkdir(); shutil.copy2(kit / "src" / "tools" / "pc" / "kit" / "build_game_main.py", kit / "start" / "__main__.py")   # the launcher's entry (no tool name in the host)
        sid = tree_id(kit / "src")
        (kit / "kit.txt").write_text(f"kit {sid}\nplatform {plat}\nllvm {v['LLVM_VERSION']}\npython {v['PYTHON_VERSION']}\n")
    write_licenses(out, plat, deps, v, not a.no_kit, Path(a.src) if a.src else None)
    check_named_texts(out, not a.no_kit, Path(a.src) if a.src else None)
    print(f"dist: {out}: {mb(size_of(out))}" + ("" if a.no_kit else
          f" (kit {mb(size_of(out / 'kit'))}: python {mb(size_of(out / 'kit/python'))}, toolchain {mb(size_of(out / 'kit/toolchain'))}, "
          f"src {mb(size_of(out / 'kit/src'))})"))
    if a.archive:
        fmt = "zip" if plat == "windows" else "xztar"
        print("dist:", shutil.make_archive(str(Path(a.out) / f"mh2-{plat}"), fmt, a.out, f"mh2-{plat}"))


if __name__ == "__main__":
    main()
