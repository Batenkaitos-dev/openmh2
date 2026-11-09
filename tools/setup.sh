#!/usr/bin/env bash
# Installs the pinned, project-local toolchain on Linux x64.
#
# Everything is installed under tools/deps/ (gitignored), with no administrator
# rights and no system packages. Every download is verified against the SHA-256 in
# tools/versions.txt and kept in tools/deps/downloads/, so a re-run
# downloads nothing it already has. Safe to re-run.
#
# What it needs from the system: a downloader (curl or wget), tar and gzip.
# It checks for them and says what is missing; it installs nothing with apt.
# The exception is the PC port's toolchain (without --no-native): it also needs
# distribution packages (the 32-bit libc and the libraries SDL3 is built from),
# which are installed with apt on Debian/Ubuntu (skip with --no-apt). PCSX2 is
# installed with no BIOS: this script never downloads one.
#
# Usage: tools/setup.sh [--if-needed]
#   --if-needed  do nothing, and print nothing, when the last run finished
#                with this script, versions.txt and the Python lock unchanged
#   --no-native  skip the native PC-port toolchain (clang, the 32-bit libc, SDL3,
#                FFmpeg, paraLLEl-GS)
#   --no-apt     don't install distribution packages (install them yourself)
#   --build-ffmpeg  build FFmpeg from its source tarball (needs make) instead of
#                installing the prebuilt libraries, which are checked by SHA-256
set -euo pipefail

usage() { sed -n '2,/^set /p' "$0" | sed '$d' | grep -v '^# @' | sed 's/^# \{0,1\}//'; }

IF_NEEDED=0
NO_APT=0
NO_NATIVE=0
BUILD_FFMPEG=0
for arg in "$@"; do
    case "$arg" in
        --if-needed) IF_NEEDED=1 ;;
        --no-apt) NO_APT=1 ;;
        --no-native) NO_NATIVE=1 ;;
        --build-ffmpeg) BUILD_FFMPEG=1 ;;
        -h | --help) usage; exit 0 ;;
        *) echo "unknown option: $arg" >&2; usage >&2; exit 2 ;;
    esac
done

if [ "$(uname -s)" != Linux ] || [ "$(uname -m)" != x86_64 ]; then
    echo "setup.sh runs on 64-bit Linux only. On Windows, the README says how to set up." >&2
    exit 1
fi

TOOLS="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=env.sh
source "$TOOLS/env.sh"    # sets MH2_ROOT, MH2_DEPS, PATH, UV_* variables
DOWNLOADS="$TOOLS/deps/downloads"
GH=https://github.com

# The stamp of a finished run: a hash of what the installed tools depend on, and
# the scope of the run: with --if-needed, a run whose stamp is current does nothing.
STAMP="$MH2_DEPS/.setup-stamp"
SCOPE=full
if [ "$NO_NATIVE" = 1 ]; then SCOPE=basic; fi
stamp_hash() { cat "${BASH_SOURCE[0]}" "$TOOLS/versions.txt" "$MH2_ROOT/pyproject.toml" "$MH2_ROOT/uv.lock" | sha256sum | cut -d' ' -f1; }
if [ "$IF_NEEDED" = 1 ] && [ -f "$STAMP" ]; then
    read -r s_hash s_scope <"$STAMP" || true
    if [ "$s_hash" = "$(stamp_hash)" ] && { [ "$s_scope" = full ] || [ "$SCOPE" = basic ]; }; then
        exit 0
    fi
fi

# tools/versions.txt -> V_<KEY> shell variables
while IFS='=' read -r key value; do
    value="${value%$'\r'}"
    if [[ "$key" =~ ^[A-Z0-9_]+$ ]]; then printf -v "V_$key" '%s' "$value"; fi
done <"$TOOLS/versions.txt"

# --- What the system must provide ---------------------------------------------
# Nothing is installed from here: a missing program is named, with the Debian
# or Ubuntu package that has it.
missing=()          # what is missing, and the Debian or Ubuntu package that has each
packages=()
have() { command -v "$1" >/dev/null 2>&1; }
need() { missing+=("$1"); packages+=("$2"); }
if have curl; then DOWNLOADER=curl; elif have wget; then DOWNLOADER=wget; else
    DOWNLOADER=
    need "a downloader: curl or wget" curl
fi
have tar || need tar tar
have gzip || need gzip gzip
have sha256sum || need sha256sum coreutils
have git || need "git (the Python lock holds a package from a git repository)" git
have xz || need "xz (7-Zip, LLVM and FFmpeg come as .tar.xz)" xz-utils
if [ "${#missing[@]}" -gt 0 ]; then
    echo "tools/setup.sh needs these programs, and can't find them:" >&2
    printf '  - %s
' "${missing[@]}" >&2
    if have apt-get; then
        echo "On Debian or Ubuntu:  sudo apt-get install ca-certificates ${packages[*]}" >&2
    else
        echo "Install them with your distribution's package manager, then run tools/setup.sh again." >&2
    fi
    exit 1
fi

# --- Distribution packages (the PC port) ---------------------------------------
# clang is not a package: the pinned LLVM release (below, tools/deps/linux-x64/llvm).
# It needs a GCC installation for the C library's headers and start files.
# gcc-multilib: the 32-bit libc mh2-core links.
APT_BASE=(ca-certificates curl git tar xz-utils build-essential)
APT_NATIVE=(
    gcc-multilib pkg-config
    # SDL3 build dependencies (SDL docs/README-linux.md, Ubuntu 22.04+ list)
    libasound2-dev libpulse-dev libaudio-dev libfribidi-dev libjack-dev libsndio-dev
    libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev
    libxss-dev libxtst-dev libxkbcommon-dev libdrm-dev libgbm-dev libgl1-mesa-dev
    libgles2-mesa-dev libegl1-mesa-dev libdbus-1-dev libibus-1.0-dev libudev-dev
    libthai-dev libusb-1.0-0-dev libpipewire-0.3-dev libwayland-dev libdecor-0-dev
    liburing-dev
)

pkgs=("${APT_BASE[@]}")
if [ "$NO_NATIVE" = 0 ]; then pkgs+=("${APT_NATIVE[@]}"); fi

APT_NEEDED=1
if [ "$NO_NATIVE" = 1 ] && [ "${NO_PCSX2:-1}" = 1 ]; then
    APT_NEEDED=0    # without the native toolchain and PCSX2 no package is needed
fi
if [ "$APT_NEEDED" = 0 ]; then
    :
elif [ "$NO_APT" = 0 ]; then
    if ! command -v apt-get >/dev/null; then
        echo "apt-get was not found. Install the equivalents of these packages for your" >&2
        echo "distribution, then run tools/setup.sh --no-apt:" >&2
        echo "  ${pkgs[*]}" >&2
        exit 1
    fi
    SUDO=
    if [ "$(id -u)" -ne 0 ]; then SUDO=sudo; fi
    echo "[..]   apt packages"
    $SUDO apt-get update -qq
    $SUDO env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends "${pkgs[@]}" >/dev/null
    echo "[done] apt packages"
else
    echo "[skip] apt packages (--no-apt). Required: ${pkgs[*]}"
fi

# --- Helpers ------------------------------------------------------------------

# download URL FILE: curl, or wget when there is no curl
download() {
    if [ "$DOWNLOADER" = curl ]; then
        curl -fsSL --retry 3 -o "$2" "$1"
    else
        wget -q --tries=3 -O "$2" "$1"
    fi
}

# get_pinned URL FILENAME SHA256 [MIRROR_URL]: download into tools/deps/downloads (cached),
# verify, and print the local path. If URL fails or the file does not match the SHA-256,
# MIRROR_URL (when given) is tried, with the same check.
get_pinned() {
    local url=$1 name=$2 sha=$3 mirror=${4:-} u
    local dest="$DOWNLOADS/$name"
    mkdir -p "$DOWNLOADS"
    if [ -f "$dest" ] && echo "$sha  $dest" | sha256sum -c --status; then
        echo "$dest"
        return
    fi
    for u in "$url" "$mirror"; do
        [ -n "$u" ] || continue
        echo "       downloading $u" >&2
        if download "$u" "$dest.part" && echo "$sha  $dest.part" | sha256sum -c --status; then
            mv "$dest.part" "$dest"
            echo "$dest"
            return
        fi
        echo "       download failed, or its SHA-256 does not match (expected $sha): $u" >&2
        rm -f "$dest.part"
    done
    echo "could not get $name with the expected SHA-256" >&2
    return 1
}

# install_step NAME DIR ID FUNCTION [OTHER_ID]: run FUNCTION DIR unless DIR already holds
# install id ID (or OTHER_ID). DIR is emptied first. FUNCTION may set STEP_ID to the id
# to record instead of ID.
install_step() {
    local name=$1 dir=$2 id=$3 fn=$4 other=${5:-}
    case "$dir" in
        "$MH2_DEPS"/*) ;;
        *) echo "refusing to install outside tools/deps: $dir" >&2; exit 1 ;;
    esac
    if [ -f "$dir/.mh2-installed" ] && { [ "$(cat "$dir/.mh2-installed")" = "$id" ] || [ "$(cat "$dir/.mh2-installed")" = "${other:-$id}" ]; }; then
        echo "[ok]   $name $(cat "$dir/.mh2-installed")"
        return
    fi
    echo "[..]   $name $id"
    rm -rf "$dir"
    mkdir -p "$dir"
    STEP_ID=""
    "$fn" "$dir"
    id=${STEP_ID:-$id}
    echo "$id" >"$dir/.mh2-installed"
    echo "[done] $name $id"
}

mkdir -p "$MH2_DEPS"

# --- uv, CPython, Python packages -------------------------------------------
install_uv() {
    local f
    f=$(get_pinned "$GH/astral-sh/uv/releases/download/$V_UV_VERSION/uv-x86_64-unknown-linux-gnu.tar.gz" \
        "uv-$V_UV_VERSION-x86_64-unknown-linux-gnu.tar.gz" "$V_UV_SHA256_LINUX")
    tar -xzf "$f" -C "$1" --strip-components 1
}
install_step uv "$MH2_DEPS/uv" "$V_UV_VERSION" install_uv

echo "[..]   CPython $V_PYTHON_VERSION and Python packages (uv.lock)"
uv python install --quiet "$V_PYTHON_VERSION"
uv sync --locked --quiet --project "$MH2_ROOT"
echo "[done] CPython $V_PYTHON_VERSION and Python packages"

# --- PS2 binutils (R5900) ----------------------------------------------------
install_binutils() {
    local f
    f=$(get_pinned "$GH/decompals/binutils-mips-ps2-decompals/releases/download/$V_BINUTILS_PS2_VERSION/binutils-mips-ps2-decompals-linux-x86-64.tar.gz" \
        "binutils-mips-ps2-decompals-$V_BINUTILS_PS2_VERSION-linux-x86-64.tar.gz" "$V_BINUTILS_PS2_SHA256_LINUX")
    tar -xzf "$f" -C "$1"
}
install_step binutils-mips-ps2-decompals "$MH2_DEPS/binutils" "$V_BINUTILS_PS2_VERSION" install_binutils



# --- LLVM (clang), the official release ---------------------------------------
# The port compiles with it (mh2-core, the host, the census and tools/pc). Only the programs the project uses are
# unpacked (about 1 GB of the release's 12): clang, lld, the binary tools,
# opt, the profile tools, and clang's resource directory (lib/clang).
LLVM_PROGRAMS=(clang clang-${V_LLVM_VERSION%%.*} clang++ clang-cpp ld.lld lld llvm-ar llvm-ranlib llvm-nm
    llvm-objdump llvm-readelf llvm-readobj llvm-objcopy llvm-strip llvm-symbolizer llvm-addr2line
    llvm-dis llvm-as opt llvm-profdata llvm-cov)
install_llvm() {
    local name="LLVM-$V_LLVM_VERSION-Linux-X64" f members=() p
    f=$(get_pinned "$GH/llvm/llvm-project/releases/download/llvmorg-$V_LLVM_VERSION/$name.tar.xz" \
        "$name.tar.xz" "$V_LLVM_SHA256_LINUX")
    for p in "${LLVM_PROGRAMS[@]}"; do members+=("$name/bin/$p"); done
    members+=("$name/lib/clang")
    tar -xJf "$f" -C "$1" --strip-components 1 "${members[@]}"
}
if [ "$NO_NATIVE" = 0 ]; then
    install_step LLVM "$MH2_DEPS/llvm" "$V_LLVM_VERSION" install_llvm
fi
source "$TOOLS/env.sh"    # CC is the pinned clang from here on

if [ "$NO_NATIVE" = 0 ]; then
    # --- SDL3, built from source with the pinned clang -----------------------
    install_sdl3() {
        local f work="$MH2_DEPS/sdl3-build"
        f=$(get_pinned "$GH/libsdl-org/SDL/releases/download/release-$V_SDL3_VERSION/SDL3-$V_SDL3_VERSION.tar.gz" \
            "SDL3-$V_SDL3_VERSION.tar.gz" "$V_SDL3_SHA256")
        rm -rf "$work"
        mkdir -p "$work"
        tar -xzf "$f" -C "$work"
        cmake -S "$work/SDL3-$V_SDL3_VERSION" -B "$work/build" -G Ninja -Wno-author \
            -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$1" \
            -DSDL_SHARED=ON -DSDL_STATIC=ON -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
            -DSDL_INSTALL_DOCS=OFF --log-level=WARNING
        cmake --build "$work/build"
        cmake --install "$work/build" >/dev/null
        rm -rf "$work"
    }
    cc_id="$($CC -dumpfullversion -dumpversion 2>/dev/null | head -n1)"
    install_step SDL3 "$MH2_DEPS/sdl3" "$V_SDL3_VERSION+$CC-$cc_id" install_sdl3

    # --- FFmpeg, the project's own build (LGPL-2.1-or-later; only what the movies use) -----
    # Installed from the prebuilt archive (a release file of openmh2: FFMPEG_MIRROR_URL and the SHA-256 in versions.txt, made by
    # tools/pc/build_ffmpeg.sh --archive), checked like every download. With --build-ffmpeg, or when the archive can't be had
    # (a failed download, a wrong SHA-256), it is built from ffmpeg.org's release tarball by tools/pc/build_ffmpeg.sh with the
    # pinned clang, which needs make: the libraries are the same either way.
    ff_name="mh2-ffmpeg-$V_FFMPEG_VERSION-linux-x64.tar.xz"
    ff_url="${MH2_FFMPEG_MIRROR_URL:-$V_FFMPEG_MIRROR_URL}/$ff_name"
    ff_prebuilt_id="$V_FFMPEG_VERSION+prebuilt-${V_FFMPEG_PREBUILT_SHA256_LINUX:0:12}"
    ff_build_id="$(bash "$TOOLS/pc/build_ffmpeg.sh" linux-x64 --stamp)"
    install_ffmpeg() {
        local f
        if [ "$BUILD_FFMPEG" = 0 ]; then
            if f=$(get_pinned "$ff_url" "$ff_name" "$V_FFMPEG_PREBUILT_SHA256_LINUX") && tar -xmJf "$f" -C "$1" &&
                [ -f "$1/.mh2-ffmpeg-stamp" ]; then
                return
            fi
            echo "       the prebuilt FFmpeg could not be had: building it from its source tarball" >&2
            rm -rf "$1"
            mkdir -p "$1"
        fi
        if ! have make; then
            {
                echo "FFmpeg: building it from source needs make, and make is not installed."
                echo "  Either install make (Debian or Ubuntu: sudo apt-get install make) and run tools/setup.sh again,"
                echo "  or install the prebuilt libraries: download $ff_url"
                echo "  (SHA-256 $V_FFMPEG_PREBUILT_SHA256_LINUX) into tools/deps/downloads/ and run tools/setup.sh again without --build-ffmpeg."
            } >&2
            exit 1
        fi
        MH2_DOWNLOADS="$DOWNLOADS" bash "$TOOLS/pc/build_ffmpeg.sh" linux-x64 --out "$1"
        STEP_ID="$ff_build_id"
    }
    if [ "$BUILD_FFMPEG" = 1 ]; then
        install_step FFmpeg "$MH2_DEPS/ffmpeg" "$ff_build_id" install_ffmpeg
    else
        install_step FFmpeg "$MH2_DEPS/ffmpeg" "$ff_prebuilt_id" install_ffmpeg "$ff_build_id"
    fi

    # --- paraLLEl-GS source (GS renderer), pinned commit + needed submodules --
    # Shared by both platforms: tools/deps/src/parallel-gs. The port's patches
    # to it (pc/gs/patches, LGPL like paraLLEl-GS) are applied on
    # top, and taken off again before a checkout of another commit.
    pgs="$TOOLS/deps/src/parallel-gs"
    pgs_patches=("$TOOLS"/../pc/gs/patches/*.patch)
    pgs_patched() {
        local p
        for p in "${pgs_patches[@]}"; do
            git -C "$pgs" apply --reverse --check "$p" 2>/dev/null || return 1
        done
    }
    pgs_ok() {
        [ "$(git -C "$pgs" rev-parse HEAD 2>/dev/null)" = "$V_PARALLEL_GS_COMMIT" ] &&
            [ "$(git -C "$pgs/Granite" rev-parse HEAD 2>/dev/null)" = "$V_GRANITE_COMMIT" ] &&
            [ "$(git -C "$pgs/Granite/third_party/volk" rev-parse HEAD 2>/dev/null)" = "$V_VOLK_COMMIT" ] &&
            [ "$(git -C "$pgs/Granite/third_party/khronos/vulkan-headers" rev-parse HEAD 2>/dev/null)" = "$V_VULKAN_HEADERS_COMMIT" ] &&
            pgs_patched
    }
    if pgs_ok; then
        echo "[ok]   paraLLEl-GS ${V_PARALLEL_GS_COMMIT:0:12}"
    else
        echo "[..]   paraLLEl-GS ${V_PARALLEL_GS_COMMIT:0:12}"
        if [ ! -e "$pgs/.git" ]; then
            mkdir -p "$(dirname "$pgs")"
            git clone --quiet --filter=blob:none --no-checkout "$V_PARALLEL_GS_REPO" "$pgs"
        fi
        for p in "${pgs_patches[@]}"; do
            if git -C "$pgs" apply --reverse --check "$p" 2>/dev/null; then
                git -C "$pgs" apply --reverse "$p"
            fi
        done
        git -C "$pgs" fetch --quiet origin
        git -C "$pgs" -c advice.detachedHead=false checkout --quiet "$V_PARALLEL_GS_COMMIT"
        # Only what the standalone build needs (as the repo's checkout_submodules.sh).
        git -C "$pgs" submodule update --quiet --init Granite
        git -C "$pgs/Granite" submodule update --quiet --init third_party/volk third_party/khronos/vulkan-headers
        for p in "${pgs_patches[@]}"; do
            if ! git -C "$pgs" apply "$p"; then
                echo "$p does not apply to $pgs: are there local changes?" >&2
                exit 1
            fi
        done
        if ! pgs_ok; then
            echo "paraLLEl-GS submodules are not at the commits in tools/versions.txt" >&2
            exit 1
        fi
        echo "[done] paraLLEl-GS ${V_PARALLEL_GS_COMMIT:0:12}"
    fi

fi



# --- Verify -------------------------------------------------------------------
echo
check_args=()
if [ "$NO_NATIVE" = 1 ]; then check_args+=(--skip-native); fi
python "$TOOLS/check_env.py" "${check_args[@]}"
echo "$(stamp_hash) $SCOPE" >"$STAMP"
echo
if [ -f "$TOOLS/build.sh" ]; then
    echo "Setup complete. tools/build.sh builds with no further step; for the tools in your own shell: source tools/env.sh"
else
    echo "Setup complete. In each new shell, run:  source tools/env.sh"
fi
