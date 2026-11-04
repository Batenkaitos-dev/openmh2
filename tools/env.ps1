# Puts the project-local toolchain (installed by tools\setup.ps1) on PATH for
# the current PowerShell session. Dot-source it:
#
#     . .\tools\env.ps1
#
# Nothing here touches system or user-wide settings.

$MH2Root = Split-Path -Parent $PSScriptRoot
$MH2Deps = Join-Path $PSScriptRoot 'deps\windows-x64'

# uv: pinned interpreter, venv and cache all live under tools\deps.
$env:UV_PYTHON_INSTALL_DIR = Join-Path $MH2Deps 'python'
$env:UV_PROJECT_ENVIRONMENT = Join-Path $MH2Deps 'venv'
$env:UV_CACHE_DIR = Join-Path $MH2Deps 'uv-cache'
$env:UV_PYTHON_PREFERENCE = 'only-managed'
$env:UV_LINK_MODE = 'copy'

# Native toolchain for the PC port: llvm-mingw clang, SDL3 found via CMake.
$env:CC = 'clang'
$env:CXX = 'clang++'
$env:CMAKE_PREFIX_PATH = Join-Path $MH2Deps 'sdl3'

$mh2Paths = @(
    'venv\Scripts', 'uv', 'binutils',
    'llvm-mingw\bin', 'sdl3\bin'
) | ForEach-Object { Join-Path $MH2Deps $_ }
$env:PATH = (@($mh2Paths) + @($env:PATH -split ';' | Where-Object { $_ -and ($mh2Paths -notcontains $_) })) -join ';'
Remove-Variable mh2Paths
