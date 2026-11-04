#!/usr/bin/env python3
"""Checks that the port's patches to paraLLEl-GS are applied (tools/deps/src/parallel-gs).

    python3 tools/pc/pgs_patches.py [--check-only]

setup (tools/setup.sh, setup.ps1) applies pc/gs/patches/*.patch to the pinned paraLLEl-GS
checkout, but only when it runs. A patch added later is then missing from
every existing checkout, and the host fails to compile. build.py calls ensure() before it builds:

  - every patch reverse-checks (as setup's pgs_ok): nothing to do;
  - some do not: they are applied, in order, but only when that cannot touch anything of yours: the
    checkout is at the pinned commit, and its working tree is exactly that commit plus the patches
    that are applied (compared through a scratch index, so nothing in the checkout changes), and each
    missing patch checks forward. Otherwise ensure() stops with one line that names the step
    (tools/setup.sh or tools/setup.ps1, which also fixes a checkout at another commit).

A directory without .git (none at all, or winrun's copy of the sources) is not judged: it is whatever was
copied, and the checkout it was copied from was checked first.

Exit status 0 when the patches are applied (or were just applied), 1 with the line otherwise.
"""
from __future__ import annotations

import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(os.path.abspath(__file__)).parent.parent.parent
PGS = ROOT / "tools" / "deps" / "src" / "parallel-gs"
PATCHES = ROOT / "port" / "gs-parallel" / "patches"


def _git(args, env=None):
    r = subprocess.run(["git", "-C", str(PGS)] + [str(a) for a in args], capture_output=True, text=True, env=env)
    return r.returncode, r.stdout.strip()


def _pinned_commit() -> str:
    for line in (ROOT / "tools" / "versions.txt").read_text().splitlines():
        if line.startswith("PARALLEL_GS_COMMIT="):
            return line.split("=", 1)[1].strip()
    return ""


def _setup_step() -> str:
    return "tools\\setup.ps1" if os.name == "nt" else "bash tools/setup.sh --no-apt"


def state():
    """(applied patches, missing patches) in order, or (None, the reason) when the checkout can't be judged."""
    patches = sorted(PATCHES.glob("*.patch"))
    if not patches:
        return [], []
    if not (PGS / ".git").exists():
        # No checkout of git's to judge: either none at all (CMake stops with its own message, "run
        # tools/setup"), or winrun's copy on the Windows disk, which holds the sources without .git
        # (the build.py that called winrun has checked and patched the checkout it copied from)
        return [], []
    applied, missing = [], []
    for p in patches:
        (applied if _git(["apply", "--reverse", "--check", p])[0] == 0 else missing).append(p)
    return applied, missing


def ensure(check_only: bool = False) -> str | None:
    """None when every patch is applied (applying the missing ones when that is safe), else one line."""
    applied, missing = state()
    if applied is None:
        return f"{missing}: run {_setup_step()}"
    if not missing:
        return None
    names = ", ".join(p.name.split("-")[0] for p in missing)
    fix = f"run {_setup_step()}"
    if check_only:
        return f"paraLLEl-GS lacks patch {names} of pc/gs/patches: {fix}"
    head = _git(["rev-parse", "HEAD"])[1]
    if head != _pinned_commit():
        return f"paraLLEl-GS is not at the pinned commit (patch {names} missing): {fix}"
    # the working tree must be the pinned commit plus the applied patches, exactly (a scratch index
    # holds that; git diff against it lists any other change)
    with tempfile.TemporaryDirectory() as td:
        env = dict(os.environ, GIT_INDEX_FILE=str(Path(td) / "index"))
        if _git(["read-tree", "HEAD"], env)[0] != 0:
            return f"paraLLEl-GS: git read-tree failed: {fix}"
        for p in applied:
            if _git(["apply", "--cached", p], env)[0] != 0:
                return f"paraLLEl-GS: patch {p.name} does not reproduce the checkout: {fix}"
        rc, out = _git(["diff", "--ignore-submodules", "--name-only"], env)
        if rc != 0 or out:
            return (f"paraLLEl-GS has changes of its own ({out.splitlines()[0] if out else 'git diff failed'}), "
                    f"so patch {names} is not applied to it: {fix}")
    for p in missing:
        if _git(["apply", "--check", p])[0] != 0:
            return f"paraLLEl-GS: patch {p.name} does not apply: {fix}"
    for p in missing:
        rc, _ = _git(["apply", p])
        if rc != 0:
            return f"paraLLEl-GS: patch {p.name} failed to apply: {fix}"
        print(f"pgs_patches: applied {p.name} to {PGS}", flush=True)
    return None


def main():
    msg = ensure("--check-only" in sys.argv[1:])
    if msg:
        print(msg, file=sys.stderr)
        sys.exit(1)
    print("pgs_patches: OK")


if __name__ == "__main__":
    main()
