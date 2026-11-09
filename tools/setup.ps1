<#
.SYNOPSIS
    Installs the pinned, project-local toolchain on Windows 11 x64.

.DESCRIPTION
    Everything is installed under tools\deps\ (gitignored). Nothing is
    installed system-wide and no user settings are changed. Every download is
    verified against the SHA-256 recorded in tools\versions.txt. Safe to
    re-run: steps that are already done are skipped.

    Prerequisites: Windows 11 x64 (it ships curl.exe and tar.exe) and git.

    Installs: uv, CPython + Python tools (splat64, spimdisasm, rabbitizer,
    pyelftools, pycdlib, ninja, cmake), PS2 MIPS binutils (R5900),
    llvm-mingw (clang), SDL3 (built from source), FFmpeg and paraLLEl-GS.


.PARAMETER NoNative
    Skip the native PC-port toolchain (SDL3, FFmpeg). llvm-mingw is always installed:
    the matching build parses the C with its clang.

.PARAMETER BuildFFmpeg
    Build FFmpeg from its source tarball (in WSL: tools\pc\build_ffmpeg.sh) instead of
    installing the prebuilt libraries, which are checked by SHA-256.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\setup.ps1
#>
[CmdletBinding()]
param(
    [switch]$NoNative,
    [switch]$BuildFFmpeg
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
Set-StrictMode -Version 3.0

if (-not [Environment]::Is64BitOperatingSystem) { throw '64-bit Windows is required.' }
if (-not (Get-Command git -ErrorAction SilentlyContinue)) { throw 'Git is required. Download it from https://git-scm.com/download/win.' }

. (Join-Path $PSScriptRoot 'env.ps1')    # sets $MH2Root, $MH2Deps, PATH, UV_* variables

$Downloads = Join-Path $PSScriptRoot 'deps\downloads'
$Tar = Join-Path $env:SystemRoot 'System32\tar.exe'    # bsdtar: handles zip and tar.gz
$GH = 'https://github.com'

$V = @{}
foreach ($line in Get-Content (Join-Path $PSScriptRoot 'versions.txt')) {
    if ($line -match '^\s*([A-Z0-9_]+)=(\S+)\s*$') { $V[$Matches[1]] = $Matches[2] }
}

function Invoke-Native([string]$Exe, [string[]]$ArgList) {
    & $Exe @ArgList
    if ($LASTEXITCODE -ne 0) { throw "$Exe $($ArgList -join ' ') exited with code $LASTEXITCODE" }
}

# Downloads $Url into tools\deps\downloads\$FileName (cached) and checks its SHA-256. If $Url fails or the
# file does not match, $MirrorUrl (when given) is tried, with the same check.
function Get-Pinned([string]$Url, [string]$FileName, [string]$Sha256, [string]$MirrorUrl = '') {
    New-Item -ItemType Directory -Force $Downloads | Out-Null
    $dest = Join-Path $Downloads $FileName
    if ((Test-Path $dest) -and ((Get-FileHash -Algorithm SHA256 $dest).Hash -eq $Sha256)) { return $dest }
    $part = "$dest.part"
    foreach ($u in @($Url, $MirrorUrl)) {
        if (-not $u) { continue }
        Write-Host "       downloading $u"
        & curl.exe -fsSL --retry 3 -o $part $u
        if (($LASTEXITCODE -eq 0) -and ((Get-FileHash -Algorithm SHA256 $part).Hash -eq $Sha256)) {
            Move-Item -Force $part $dest
            return $dest
        }
        Write-Host "       download failed, or its SHA-256 does not match (expected $Sha256): $u"
        Remove-Item -Force $part -ErrorAction SilentlyContinue
    }
    throw "could not get $FileName with the expected SHA-256"
}

# Runs $Body to install into $Dir unless $Dir already holds install id $Id (or $OtherId).
# By default $Dir is emptied first; -KeepContents extracts over the top. $Body may set
# $script:StepId to the id to record instead of $Id.
function Install-Step([string]$Name, [string]$Dir, [string]$Id, [scriptblock]$Body, [switch]$KeepContents, [string]$OtherId = '') {
    $full = [IO.Path]::GetFullPath($Dir)
    if (-not $full.StartsWith([IO.Path]::GetFullPath($MH2Deps) + '\')) { throw "refusing to install outside tools\deps: $full" }
    $stamp = Join-Path $full '.mh2-installed'
    if (Test-Path $stamp) {
        $have = (Get-Content -Raw $stamp).Trim()
        if (($have -eq $Id) -or ($OtherId -and ($have -eq $OtherId))) {
            Write-Host "[ok]   $Name $have"
            return
        }
    }
    Write-Host "[..]   $Name $Id"
    if ((Test-Path $full) -and -not $KeepContents) { Remove-Item -Recurse -Force $full }
    New-Item -ItemType Directory -Force $full | Out-Null
    $script:StepId = ''
    & $Body $full
    if ($script:StepId) { $Id = $script:StepId }
    Set-Content -Path $stamp -Value $Id -Encoding ascii
    Write-Host "[done] $Name $Id"
}

New-Item -ItemType Directory -Force $MH2Deps | Out-Null

# --- uv, CPython, Python packages -------------------------------------------
Install-Step 'uv' (Join-Path $MH2Deps 'uv') $V.UV_VERSION {
    param($dir)
    $zip = Get-Pinned "$GH/astral-sh/uv/releases/download/$($V.UV_VERSION)/uv-x86_64-pc-windows-msvc.zip" `
        "uv-$($V.UV_VERSION)-x86_64-pc-windows-msvc.zip" $V.UV_SHA256_WINDOWS
    Invoke-Native $Tar @('-xf', $zip, '-C', $dir)
}

Write-Host "[..]   CPython $($V.PYTHON_VERSION) and Python packages (uv.lock)"
Invoke-Native uv @('python', 'install', '--quiet', $V.PYTHON_VERSION)
Invoke-Native uv @('sync', '--locked', '--quiet', '--project', $MH2Root)
Write-Host "[done] CPython $($V.PYTHON_VERSION) and Python packages"

# --- PS2 binutils (R5900) ----------------------------------------------------
Install-Step 'binutils-mips-ps2-decompals' (Join-Path $MH2Deps 'binutils') $V.BINUTILS_PS2_VERSION {
    param($dir)
    $zip = Get-Pinned "$GH/decompals/binutils-mips-ps2-decompals/releases/download/$($V.BINUTILS_PS2_VERSION)/binutils-mips-ps2-decompals-windows-x86-64.zip" `
        "binutils-mips-ps2-decompals-$($V.BINUTILS_PS2_VERSION)-windows-x86-64.zip" $V.BINUTILS_PS2_SHA256_WINDOWS
    Invoke-Native $Tar @('-xf', $zip, '-C', $dir)
}


# --- llvm-mingw: clang/lld targeting x86_64-w64-mingw32 (UCRT) ---------------
# The matching build needs its clang too: tools/content_gen.py reads the C's
# types from clang's parse (CONTRIBUTING, "Content"). The port uses the same.
Install-Step 'llvm-mingw' (Join-Path $MH2Deps 'llvm-mingw') $V.LLVM_MINGW_VERSION {
    param($dir)
    $name = "llvm-mingw-$($V.LLVM_MINGW_VERSION)-ucrt-x86_64"
    $zip = Get-Pinned "$GH/mstorsjo/llvm-mingw/releases/download/$($V.LLVM_MINGW_VERSION)/$name.zip" "$name.zip" $V.LLVM_MINGW_SHA256
    Invoke-Native $Tar @('-xf', $zip, '-C', $dir, '--strip-components', '1')
}

if (-not $NoNative) {
    # --- SDL3, built from source with the pinned compiler ----------------------
    Install-Step 'SDL3' (Join-Path $MH2Deps 'sdl3') "$($V.SDL3_VERSION)+llvm-mingw-$($V.LLVM_MINGW_VERSION)" {
        param($dir)
        $tgz = Get-Pinned "$GH/libsdl-org/SDL/releases/download/release-$($V.SDL3_VERSION)/SDL3-$($V.SDL3_VERSION).tar.gz" `
            "SDL3-$($V.SDL3_VERSION).tar.gz" $V.SDL3_SHA256
        $work = Join-Path $MH2Deps 'sdl3-build'
        if (Test-Path $work) { Remove-Item -Recurse -Force $work }
        New-Item -ItemType Directory -Force $work | Out-Null
        Invoke-Native $Tar @('-xzf', $tgz, '-C', $work)
        $src = Join-Path $work "SDL3-$($V.SDL3_VERSION)"
        $bld = Join-Path $work 'build'
        Invoke-Native cmake @('-S', $src, '-B', $bld, '-G', 'Ninja', '-Wno-author',
            '-DCMAKE_BUILD_TYPE=Release', "-DCMAKE_INSTALL_PREFIX=$dir",
            '-DSDL_SHARED=ON', '-DSDL_STATIC=ON', '-DSDL_TESTS=OFF', '-DSDL_EXAMPLES=OFF',
            '-DSDL_INSTALL_DOCS=OFF', '--log-level=WARNING')
        Invoke-Native cmake @('--build', $bld)
        cmake --install $bld | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "cmake --install exited with code $LASTEXITCODE" }
        Remove-Item -Recurse -Force $work
    }

    # --- FFmpeg, the project's own build (LGPL-2.1-or-later; only what the movies use) -----
    # Installed from the prebuilt archive (a release file of openmh2: FFMPEG_MIRROR_URL and the SHA-256 in versions.txt, made by
    # tools\pc\build_ffmpeg.sh --archive), checked like every download. With -BuildFFmpeg, or when the archive can't be had
    # (a failed download, a wrong SHA-256), it is built from ffmpeg.org's release tarball by tools\pc\build_ffmpeg.sh, a POSIX
    # shell script (FFmpeg's configure and make): that runs in WSL (Windows Subsystem for Linux), which needs bash, make, curl,
    # tar, xz-utils and a C library for llvm-mingw's Linux clang (Ubuntu: sudo apt install make curl xz-utils). The libraries
    # are the same either way.
    $ffName = "mh2-ffmpeg-$($V.FFMPEG_VERSION)-windows-x64.zip"
    $ffBase = if ($env:MH2_FFMPEG_MIRROR_URL) { $env:MH2_FFMPEG_MIRROR_URL } else { $V.FFMPEG_MIRROR_URL }
    $ffUrl = "$ffBase/$ffName"
    $ffPrebuiltId = "$($V.FFMPEG_VERSION)+prebuilt-$($V.FFMPEG_PREBUILT_SHA256_WINDOWS.Substring(0, 12))"
    function Invoke-FFmpegScript([string[]]$ScriptArgs) {
        $script = (& wsl.exe -e wslpath -a (Join-Path $PSScriptRoot 'pc\build_ffmpeg.sh')) -join ''
        if ($LASTEXITCODE -ne 0 -or -not $script) { throw 'wsl.exe could not translate the path of tools\pc\build_ffmpeg.sh: is a WSL distribution installed?' }
        return @($script.Trim()) + $ScriptArgs
    }
    # WSL is only asked (for the build's stamp, and whether it is there) when the prebuilt is not already installed.
    $ffStampFile = Join-Path (Join-Path $MH2Deps 'ffmpeg') '.mh2-installed'
    $ffHave = if (Test-Path $ffStampFile) { (Get-Content -Raw $ffStampFile).Trim() } else { '' }
    $ffWsl = [bool](Get-Command wsl.exe -ErrorAction SilentlyContinue)
    $ffBuildId = $null
    if ($ffWsl -and ($BuildFFmpeg -or $ffHave -ne $ffPrebuiltId)) {
        try {
            $ffArgs = Invoke-FFmpegScript @('windows-x64', '--stamp')
            $ffStamp = (& wsl.exe -e bash $ffArgs 2>$null) -join ''
            if ($LASTEXITCODE -eq 0 -and $ffStamp) { $ffBuildId = $ffStamp.Trim() } else { $ffWsl = $false }
        } catch { $ffWsl = $false }
    } elseif ($ffWsl) {
        $ffWsl = $false   # nothing to build: the prebuilt is installed
    }
    function Install-FFmpegBuilt([string]$Dir) {
        $why = "download $ffUrl (SHA-256 $($V.FFMPEG_PREBUILT_SHA256_WINDOWS)) into tools\deps\downloads (or check the network) and run this setup again without -BuildFFmpeg"
        if (-not $ffWsl) {
            throw ("FFmpeg's libraries can be installed in two ways. (1) Use the prebuilt archive: $why. " +
                   '(2) Build them from source, which needs WSL (Windows Subsystem for Linux) with make, curl and xz-utils: ' +
                   'wsl.exe is not installed, or no distribution answers (wsl --install -d Ubuntu, then sudo apt install make curl xz-utils in it); ' +
                   'then run this setup again with -BuildFFmpeg.')
        }
        $out = (& wsl.exe -e wslpath -a $Dir) -join ''
        $dl = (& wsl.exe -e wslpath -a $Downloads) -join ''
        $ffArgs = Invoke-FFmpegScript @('windows-x64', '--out', $out.Trim())
        Invoke-Native wsl.exe (@('-e', 'env', "MH2_DOWNLOADS=$($dl.Trim())", 'bash') + $ffArgs)
        $script:StepId = $ffBuildId
    }
    $ffBody = {
        param($dir)
        if (-not $BuildFFmpeg) {
            try {
                $zip = Get-Pinned $ffUrl $ffName $V.FFMPEG_PREBUILT_SHA256_WINDOWS
                Invoke-Native $Tar @('-xmf', $zip, '-C', $dir)
                if (Test-Path (Join-Path $dir '.mh2-ffmpeg-stamp')) { return }
                Write-Host '       the prebuilt FFmpeg archive holds no stamp'
            } catch {
                Write-Host "       the prebuilt FFmpeg could not be had: $($_.Exception.Message)"
            }
            Write-Host '       building FFmpeg from its source tarball instead'
            Get-ChildItem -Force $dir | Remove-Item -Recurse -Force
        }
        Install-FFmpegBuilt $dir
    }
    if ($BuildFFmpeg) {
        if (-not $ffWsl) { Install-FFmpegBuilt (Join-Path $MH2Deps 'ffmpeg') }   # says what to do, and stops
        Install-Step 'FFmpeg' (Join-Path $MH2Deps 'ffmpeg') $ffBuildId $ffBody
    } else {
        Install-Step 'FFmpeg' (Join-Path $MH2Deps 'ffmpeg') $ffPrebuiltId $ffBody -OtherId $ffBuildId
    }

    # --- paraLLEl-GS source (GS renderer), pinned commit + needed submodules ---
    # Shared by both platforms: tools\deps\src\parallel-gs. The port's patches
    # to it (pc\gs\patches, LGPL like paraLLEl-GS) are applied on
    # top, and taken off again before a checkout of another commit.
    $pgs = Join-Path $PSScriptRoot 'deps\src\parallel-gs'
    $pgsPatches = @(Get-ChildItem (Join-Path $PSScriptRoot '..\pc\gs\patches') -Filter '*.patch' |
        Sort-Object Name | ForEach-Object { $_.FullName })
    function Test-PatchApplied([string]$Patch) {
        # git's complaint must not stop the script (Windows PowerShell 5.1)
        $ErrorActionPreference = 'Continue'
        & git -C $pgs apply --reverse --check $Patch 2>$null | Out-Null
        return $LASTEXITCODE -eq 0
    }
    $pgsPins = [ordered]@{
        ''                                         = $V.PARALLEL_GS_COMMIT
        'Granite'                                  = $V.GRANITE_COMMIT
        'Granite\third_party\volk'                 = $V.VOLK_COMMIT
        'Granite\third_party\khronos\vulkan-headers' = $V.VULKAN_HEADERS_COMMIT
    }
    function Test-ParallelGs {
        foreach ($sub in $pgsPins.Keys) {
            $d = if ($sub) { Join-Path $pgs $sub } else { $pgs }
            if (-not (Test-Path (Join-Path $d '.git'))) { return $false }
            $head = (& git -C $d rev-parse HEAD 2>$null)
            if ($LASTEXITCODE -ne 0 -or $head -ne $pgsPins[$sub]) { return $false }
        }
        foreach ($p in $pgsPatches) {
            if (-not (Test-PatchApplied $p)) { return $false }
        }
        return $true
    }
    $short = $V.PARALLEL_GS_COMMIT.Substring(0, 12)
    if (Test-ParallelGs) {
        Write-Host "[ok]   paraLLEl-GS $short"
    } else {
        Write-Host "[..]   paraLLEl-GS $short"
        if (-not (Test-Path (Join-Path $pgs '.git'))) {
            New-Item -ItemType Directory -Force (Split-Path $pgs) | Out-Null
            Invoke-Native git @('clone', '--quiet', '--filter=blob:none', '--no-checkout', $V.PARALLEL_GS_REPO, $pgs)
        }
        foreach ($p in $pgsPatches) {
            if (Test-PatchApplied $p) { Invoke-Native git @('-C', $pgs, 'apply', '--reverse', $p) }
        }
        Invoke-Native git @('-C', $pgs, 'fetch', '--quiet', 'origin')
        Invoke-Native git @('-C', $pgs, '-c', 'advice.detachedHead=false', 'checkout', '--quiet', $V.PARALLEL_GS_COMMIT)
        # Only what the standalone build needs (as the repo's checkout_submodules.sh).
        Invoke-Native git @('-C', $pgs, 'submodule', 'update', '--quiet', '--init', 'Granite')
        Invoke-Native git @('-C', (Join-Path $pgs 'Granite'), 'submodule', 'update', '--quiet', '--init',
            'third_party/volk', 'third_party/khronos/vulkan-headers')
        foreach ($p in $pgsPatches) {
            & git -C $pgs apply $p
            if ($LASTEXITCODE -ne 0) { throw "$p does not apply to ${pgs}: are there local changes?" }
        }
        if (-not (Test-ParallelGs)) { throw 'paraLLEl-GS submodules are not at the commits in tools\versions.txt' }
        Write-Host "[done] paraLLEl-GS $short"
    }

}


# --- Verify -------------------------------------------------------------------
Write-Host ''
$checkArgs = @((Join-Path $PSScriptRoot 'check_env.py'))
if ($NoNative) { $checkArgs += '--skip-native' }
& python @checkArgs
if ($LASTEXITCODE -ne 0) { throw 'tools\check_env.py reported failures' }
Write-Host ''
Write-Host 'Setup complete. In each new shell, run:  . .\tools\env.ps1'
