# Puts the project-local toolchain (installed by tools/setup.sh) on PATH for
# the current shell. Source it:
#
#     source tools/env.sh
#
# Nothing here touches system or user-wide settings.

MH2_TOOLS="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MH2_ROOT="$(dirname "$MH2_TOOLS")"
MH2_DEPS="$MH2_TOOLS/deps/linux-x64"
export MH2_ROOT MH2_DEPS

# uv: pinned interpreter, venv and cache all live under tools/deps.
export UV_PYTHON_INSTALL_DIR="$MH2_DEPS/python"
export UV_PROJECT_ENVIRONMENT="$MH2_DEPS/venv"
export UV_CACHE_DIR="$MH2_DEPS/uv-cache"
export UV_PYTHON_PREFERENCE=only-managed
export UV_LINK_MODE=copy

# Native toolchain for the PC port: the pinned clang (LLVM_VERSION in
# tools/versions.txt, the official release in tools/deps/linux-x64/llvm; the
# same version as llvm-mingw's on Windows), first on PATH, so that every
# "clang" of the build and the tools is it; SDL3 found via CMake.
if [ -x "$MH2_DEPS/llvm/bin/clang" ]; then
    export CC="$MH2_DEPS/llvm/bin/clang"
    export CXX="$MH2_DEPS/llvm/bin/clang++"
else
    echo "tools/env.sh: the pinned clang is not installed (tools/setup.sh); clang is the system's" >&2
    export CC="${CC:-clang}"
    export CXX="${CXX:-clang++}"
fi
export CMAKE_PREFIX_PATH="$MH2_DEPS/sdl3${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"

for _mh2_p in binutils uv venv/bin llvm/bin; do
    case ":$PATH:" in
        *":$MH2_DEPS/$_mh2_p:"*) ;;
        *) PATH="$MH2_DEPS/$_mh2_p:$PATH" ;;
    esac
done
export PATH
unset _mh2_p
