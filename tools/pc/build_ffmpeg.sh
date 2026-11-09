#!/usr/bin/env bash
# The project's own FFmpeg: the shared libraries the port's movie bridge (pc/movie/mh2movie.c) links.
#
#   tools/pc/build_ffmpeg.sh SYSTEM [--out DIR] [--jobs N] [--archive DIR]
#                                                              build SYSTEM's FFmpeg into DIR; with --archive, also pack
#                                                              it as DIR2/mh2-ffmpeg-VERSION-SYSTEM.zip (windows-x64) or
#                                                              .tar.xz (linux-x64), reproducibly (tools/pc/pack_ffmpeg.py)
#   tools/pc/build_ffmpeg.sh SYSTEM --stamp                    print the install stamp and stop
#   tools/pc/build_ffmpeg.sh SYSTEM --configure                print the configure line and stop
#
# SYSTEM is linux-x64 (built with the pinned clang, tools/deps/linux-x64/llvm) or windows-x64 (cross-built on Linux with
# llvm-mingw's Linux build, the one the release kit uses: KIT_MINGW_LINUX_* in tools/versions.txt). The Windows
# setup (tools/setup.ps1) installs the archive --archive makes (a release file of openmh2, its SHA-256 in tools/versions.txt)
# and runs this script, through WSL, only when asked to build or when the archive can't be had. DIR defaults to tools/deps/SYSTEM/ffmpeg and gets include/, lib/
# (Windows: bin/ for the DLLs too), LICENSE.txt, COPYING.LGPLv2.1 and the stamp (.mh2-ffmpeg-stamp).
#
# What the movies need and nothing else (checked on all 13 movies): FFmpeg's release tarball (FFMPEG_VERSION,
# FFMPEG_SHA256 and FFMPEG_URL in tools/versions.txt), the MPEG program stream demuxer, the MPEG-2 video and ADX
# decoders and the two parsers avformat_find_stream_info needs, no protocol (the bridge gives its own AVIOContext),
# no external library, and neither --enable-version3 nor --enable-gpl: the libraries are LGPL-2.1-or-later.
# --disable-x86asm and --disable-inline-asm: the decoder and the scaler are plain C on both systems, so the pictures do
# not depend on the CPU or the system.
#
# Environment: MH2_DOWNLOADS (default tools/deps/downloads) is where the tarballs are kept after their SHA-256 matched.
# MH2_FFMPEG_MIRROR_URL replaces FFMPEG_MIRROR_URL (the tarball's second source).
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
VERSIONS="$ROOT/tools/versions.txt"
DOWNLOADS=${MH2_DOWNLOADS:-$ROOT/tools/deps/downloads}

usage() { sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed -e '$d' -e 's/^# \{0,1\}//' >&2; exit 2; }

SYSTEM=${1:-}
[ -n "$SYSTEM" ] || usage
shift
OUT=""
JOBS=$(nproc 2>/dev/null || echo 4)
[ "$JOBS" -le 8 ] || JOBS=8
MODE=build
ARCHIVE_DIR=""
while [ $# -gt 0 ]; do
    case "$1" in
        --out) OUT=$2; shift 2 ;;
        --jobs) JOBS=$2; shift 2 ;;
        --archive) ARCHIVE_DIR=$2; shift 2 ;;
        --stamp) MODE=stamp; shift ;;
        --configure) MODE=configure; shift ;;
        *) usage ;;
    esac
done
case "$SYSTEM" in linux-x64 | windows-x64) ;; *) echo "unknown system $SYSTEM" >&2; usage ;; esac
[ -n "$OUT" ] || OUT="$ROOT/tools/deps/$SYSTEM/ffmpeg"

ver() { sed -n "s/^$1=//p" "$VERSIONS" | head -n1 | tr -d '\r'; }
FF_VERSION=$(ver FFMPEG_VERSION)
FF_SHA=$(ver FFMPEG_SHA256)
FF_URL=$(ver FFMPEG_URL)
FF_MIRROR=${MH2_FFMPEG_MIRROR_URL:-$(ver FFMPEG_MIRROR_URL)}
[ -n "$FF_VERSION" ] && [ -n "$FF_SHA" ] && [ -n "$FF_URL" ] || { echo "FFMPEG_VERSION, FFMPEG_SHA256 or FFMPEG_URL missing in $VERSIONS" >&2; exit 1; }
LLVM_VERSION=$(ver LLVM_VERSION)
MINGW_VERSION=$(ver LLVM_MINGW_VERSION)

# ---- the configuration (everything that decides the output is here) ---------------------------------------------------
CFG=(--prefix=/ffmpeg
     --disable-everything --disable-autodetect --disable-programs --disable-doc --disable-debug --disable-network
     --enable-shared --disable-static
     --disable-avdevice --disable-avfilter
     --enable-avcodec --enable-avformat --enable-avutil --enable-swresample --enable-swscale
     --enable-decoder=mpeg2video,adpcm_adx --enable-demuxer=mpegps,mpegvideo --enable-parser=mpegvideo,adx
     --disable-x86asm --disable-inline-asm)
case "$SYSTEM" in
    linux-x64)
        CFG+=(--arch=x86_64 --target-os=linux --cc=clang --ar=llvm-ar --nm=llvm-nm --ranlib=llvm-ranlib --strip=llvm-strip
              --enable-pic)
        CC_LABEL="clang-$LLVM_VERSION"
        ;;
    windows-x64)
        CFG+=(--enable-cross-compile --arch=x86_64 --target-os=mingw32 --cross-prefix=x86_64-w64-mingw32-
              --cc=x86_64-w64-mingw32-clang --extra-ldflags=-Wl,--no-insert-timestamp)
        CC_LABEL="llvm-mingw-$MINGW_VERSION"
        ;;
esac
CFG_LINE="./configure ${CFG[*]}"
CFG_HASH=$(printf '%s' "$CFG_LINE" | sha256sum | cut -c1-12)
STAMP="$FF_VERSION+src-${FF_SHA:0:12}+cfg-$CFG_HASH+$CC_LABEL"
case "$MODE" in
    stamp) echo "$STAMP"; exit 0 ;;
    configure) echo "$CFG_LINE"; exit 0 ;;
esac

# ---- the tools it needs ---------------------------------------------------------------------------------------------------
missing=""
for c in curl tar xz make sha256sum mktemp sed; do command -v "$c" >/dev/null 2>&1 || missing="$missing $c"; done
PYTHON=""
if [ -n "$ARCHIVE_DIR" ]; then
    for c in python3 python; do command -v "$c" >/dev/null 2>&1 && { PYTHON=$c; break; }; done
    [ -n "$PYTHON" ] || missing="$missing python3"
fi
[ -z "$missing" ] || { echo "build_ffmpeg.sh needs$missing (Ubuntu: sudo apt install make curl xz-utils). Building needs make; setup installs the prebuilt archive instead, which needs none of it" >&2; exit 1; }
case "$OUT" in "" | / | /usr | /usr/*) echo "refusing to write $OUT" >&2; exit 1 ;; esac

# ---- the downloads ----------------------------------------------------------------------------------------------------
fetch() {   # fetch NAME SHA256 URL...: into $DOWNLOADS, checked; prints the path
    local name=$1 sha=$2 dest u
    shift 2
    dest="$DOWNLOADS/$name"
    mkdir -p "$DOWNLOADS"
    if [ -f "$dest" ] && echo "$sha  $dest" | sha256sum -c --status; then echo "$dest"; return; fi
    for u in "$@"; do
        [ -n "$u" ] || continue
        echo "downloading $u" >&2
        if curl -fL --retry 3 -sS -o "$dest.part" "$u" && echo "$sha  $dest.part" | sha256sum -c --status; then
            mv "$dest.part" "$dest"; echo "$dest"; return
        fi
        echo "failed, or SHA-256 mismatch (expected $sha): $u" >&2
        rm -f "$dest.part"
    done
    echo "could not get $name with the expected SHA-256" >&2
    return 1
}

WORK=$(mktemp -d "${TMPDIR:-/tmp}/mh2-ffmpeg.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
TARBALL=$(fetch "ffmpeg-$FF_VERSION.tar.xz" "$FF_SHA" "$FF_URL" "${FF_MIRROR:+$FF_MIRROR/ffmpeg-$FF_VERSION.tar.xz}")
mkdir -p "$WORK/src"
tar -xJf "$TARBALL" -C "$WORK/src" --strip-components 1

case "$SYSTEM" in
    linux-x64)
        LLVM_BIN="$ROOT/tools/deps/linux-x64/llvm/bin"
        [ -x "$LLVM_BIN/clang" ] || { echo "the pinned clang is missing ($LLVM_BIN/clang): run tools/setup.sh first" >&2; exit 1; }
        export PATH="$LLVM_BIN:$PATH"
        ;;
    windows-x64)
        mingw_path=$(ver KIT_MINGW_LINUX_PATH); mingw_sha=$(ver KIT_MINGW_LINUX_SHA256)
        DOWNLOADS="$DOWNLOADS/kit"   # the release kit keeps the same file there
        f=$(fetch "$(basename "$mingw_path")" "$mingw_sha" "$(ver KIT_GH_BASE)/$mingw_path")
        mkdir -p "$WORK/mingw"
        tar -xJf "$f" -C "$WORK/mingw" --strip-components 1
        export PATH="$WORK/mingw/bin:$PATH"
        ;;
esac

# ---- the build --------------------------------------------------------------------------------------------------------
mkdir -p "$WORK/build"
cd "$WORK/build"
echo "configure: $CFG_LINE"
if ! "$WORK/src/configure" "${CFG[@]}" >"$WORK/configure.log" 2>&1; then
    tail -n 30 "$WORK/configure.log" >&2; tail -n 30 "$WORK/build/ffbuild/config.log" >&2 || true
    echo "configure failed" >&2; exit 1
fi
make -j"$JOBS" >"$WORK/make.log" 2>&1 || { grep -m 20 -E 'error|Error' "$WORK/make.log" >&2; echo "make failed" >&2; exit 1; }
make install DESTDIR="$WORK/stage" >"$WORK/install.log" 2>&1 || { tail -n 20 "$WORK/install.log" >&2; exit 1; }

# ---- the result: include, lib (bin on Windows), the licence texts, the stamp ----------------------------------------------
STAGE="$WORK/stage/ffmpeg"
rm -rf "$STAGE/share" "$STAGE/lib/pkgconfig"
cp "$WORK/src/COPYING.LGPLv2.1" "$STAGE/COPYING.LGPLv2.1"
cp "$WORK/src/LICENSE.md" "$STAGE/LICENSE.md"
printf '%s\n' "$STAMP" >"$STAGE/.mh2-ffmpeg-stamp"
printf '%s\n' "$CFG_LINE" >"$STAGE/configure-line.txt"
rm -rf "$OUT"
mkdir -p "$(dirname "$OUT")"
cp -a "$STAGE" "$OUT"
echo "FFmpeg $STAMP -> $OUT"
if [ -n "$ARCHIVE_DIR" ]; then
    case "$SYSTEM" in linux-x64) EXT=tar.xz ;; windows-x64) EXT=zip ;; esac
    mkdir -p "$ARCHIVE_DIR"
    "$PYTHON" -I "$ROOT/tools/pc/pack_ffmpeg.py" "$SYSTEM" "$OUT" "$ARCHIVE_DIR/mh2-ffmpeg-$FF_VERSION-$SYSTEM.$EXT"
fi
