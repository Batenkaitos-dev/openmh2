#!/usr/bin/env bash
# Installs the pinned, project-local toolchain on Linux x64.
#
# Everything is installed under tools/deps/ (gitignored), except the
# distribution packages needed to build SDL3 and the port, which are
# installed with apt on Debian/Ubuntu (skip with --no-apt). Every download
# is verified against the SHA-256 in tools/versions.txt. Safe to re-run.
#
# Usage: tools/setup.sh [--no-apt] [--no-native]
#   --no-apt     don't install distribution packages (install them yourself)
#   --no-native  skip the native PC-port toolchain (the 32-bit libc, SDL3); clang
#                (LLVM, pinned) is installed always: the matching build parses
#                the C with it
set -euo pipefail

usage() { sed -n '2,/^set /p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; }

NO_APT=0
NO_NATIVE=0
for arg in "$@"; do
    case "$arg" in
        --no-apt) NO_APT=1 ;;
        --no-native) NO_NATIVE=1 ;;
        -h | --help) usage; exit 0 ;;
        *) echo "unknown option: $arg" >&2; usage >&2; exit 2 ;;
    esac
done

if [ "$(uname -s)" != Linux ] || [ "$(uname -m)" != x86_64 ]; then
    echo "setup.sh supports Linux x86_64 only (use tools/setup.ps1 on Windows)" >&2
    exit 1
fi

TOOLS="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=env.sh
source "$TOOLS/env.sh"    # sets MH2_ROOT, MH2_DEPS, PATH, UV_* variables
DOWNLOADS="$TOOLS/deps/downloads"
GH=https://github.com

# tools/versions.txt -> V_<KEY> shell variables
while IFS='=' read -r key value; do
    value="${value%$'\r'}"
    if [[ "$key" =~ ^[A-Z0-9_]+$ ]]; then printf -v "V_$key" '%s' "$value"; fi
done <"$TOOLS/versions.txt"

# --- Distribution packages ---------------------------------------------------
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

if [ "$NO_APT" = 0 ]; then
    if ! command -v apt-get >/dev/null; then
        echo "apt-get not found. Install the equivalents of these packages for your" >&2
        echo "distribution, then re-run with --no-apt:" >&2
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

# get_pinned URL FILENAME SHA256 [MIRROR_URL]: download into tools/deps/downloads (cached),
# verify, and print the local path. If URL fails or the file does not match the SHA-256,
# MIRROR_URL (when given and not the placeholder) is tried, with the same check.
get_pinned() {
    local url=$1 name=$2 sha=$3 mirror=${4:-} u
    local dest="$DOWNLOADS/$name"
    mkdir -p "$DOWNLOADS"
    if [ -f "$dest" ] && echo "$sha  $dest" | sha256sum -c --status; then
        echo "$dest"
        return
    fi
    for u in "$url" "$mirror"; do
        case "$u" in "" | *OPENMH2_MIRROR_URL_TODO*) continue ;; esac
        echo "       downloading $u" >&2
        if curl -fsSL --retry 3 -o "$dest.part" "$u" && echo "$sha  $dest.part" | sha256sum -c --status; then
            mv "$dest.part" "$dest"
            echo "$dest"
            return
        fi
        echo "       failed, or SHA-256 mismatch (expected $sha): $u" >&2
        rm -f "$dest.part"
    done
    echo "could not get $name with the expected SHA-256" >&2
    return 1
}

# install_step NAME DIR ID FUNCTION: run FUNCTION DIR unless DIR already holds
# install id ID. DIR is emptied first.
install_step() {
    local name=$1 dir=$2 id=$3 fn=$4
    case "$dir" in
        "$MH2_DEPS"/*) ;;
        *) echo "refusing to install outside tools/deps: $dir" >&2; exit 1 ;;
    esac
    if [ -f "$dir/.mh2-installed" ] && [ "$(cat "$dir/.mh2-installed")" = "$id" ]; then
        echo "[ok]   $name $id"
        return
    fi
    echo "[..]   $name $id"
    rm -rf "$dir"
    mkdir -p "$dir"
    "$fn" "$dir"
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
# tools/content_gen.py reads the C's types from clang's parse (the matching
# build; CONTRIBUTING, "Content"), and the port compiles with it (mh2-core, the
# host, the census and tools/pc). Only the programs the project uses are
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
install_step LLVM "$MH2_DEPS/llvm" "$V_LLVM_VERSION" install_llvm

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

    # --- FFmpeg (LGPL shared build: headers, libraries) ----------------------
    install_ffmpeg() {
        local f name="ffmpeg-$V_FFMPEG_VERSION-linux64-lgpl-shared-$V_FFMPEG_SERIES"
        # BtbN's build first; openmh2's mirror of the same file (FFMPEG_MIRROR_URL) second
        f=$(get_pinned "$GH/BtbN/FFmpeg-Builds/releases/download/$V_FFMPEG_BUILD_TAG/$name.tar.xz" \
            "$name.tar.xz" "$V_FFMPEG_SHA256_LINUX" "$V_FFMPEG_MIRROR_URL/$name.tar.xz")
        tar -xJf "$f" -C "$1" --strip-components 1
    }
    install_step FFmpeg "$MH2_DEPS/ffmpeg" "$V_FFMPEG_VERSION+$V_FFMPEG_BUILD_TAG" install_ffmpeg

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
echo
echo "Setup complete. In each new shell, run:  source tools/env.sh"
