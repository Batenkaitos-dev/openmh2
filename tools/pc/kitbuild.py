#!/usr/bin/env python3
"""Builds mh2-core from the player's game files with the release's kit (run by the launcher's "Build the game" step).

    kit/python/python kit/src/tools/pc/kitbuild.py --disc ARCHIVE_OR_IMAGE_OR_FOLDER --work DIR --out DIR [--log FILE] [--jobs N] [--keep]

The game files are the port's disc archive (the launcher passes the one it made, so a disc is never needed again), a disc image, or
the disc's files. The steps (the launcher shows them): the program files, the split, the data tables, the compiler, the install.
The kit's source is copied to DIR first (a release folder may be read-only), the build runs there, and the finished core is
copied to OUT atomically, with built.txt (the kit's identity) and strings.tsv (the program's strings, when the kit has the text tools). DIR is deleted at the end unless --keep. Every run starts from
an empty DIR, so an interrupted or failed build leaves nothing to clean.

Standard output, one line each, for the launcher (UTF-8):
    MH2BUILD STEP n N words       a step begins
    MH2BUILD PROGRESS d t         compiler progress inside the step
    MH2BUILD DONE path            the core is in place
    MH2BUILD FAIL code words      code: disc, space, cancel, build, tool
A line "cancel" on standard input (or its end) stops the build and everything it started.
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path

SRC = Path(os.path.abspath(__file__)).parent.parent.parent    # kit/src
KIT = SRC.parent
WIN = os.name == "nt"
EXE = ".exe" if WIN else ""
NEED_FREE = 2_500_000_000   # the build's files: about 700 MB, with room
STEPS = 5

procs: list = []
cancelled = False
log = None
work_dir = None   # the build's work folder: removed when the build ends in failure or cancel, unless --keep
keep_work = False
step_no, step_words = 0, ""   # the step under way, in the words the Building page shows


def say(*a):
    print("MH2BUILD", *a, flush=True)


def note(text: str):
    if log:
        log.write(text if text.endswith("\n") else text + "\n")
        log.flush()


def kill_tree(p: subprocess.Popen):
    if p.poll() is not None:
        return
    try:
        if WIN:
            subprocess.run(["taskkill", "/F", "/T", "/PID", str(p.pid)], capture_output=True)
        else:
            os.killpg(p.pid, signal.SIGKILL)
    except Exception:
        p.kill()


def watch_stdin():
    global cancelled
    for line in sys.stdin:
        if line.strip() == "cancel":
            break
    cancelled = True
    for p in list(procs):
        kill_tree(p)


def fail(code: str, words: str):
    say("FAIL", code, words)
    if work_dir is not None and not keep_work:   # nothing is left behind (the log is in the data folder)
        for p in list(procs):
            kill_tree(p)
        shutil.rmtree(work_dir, ignore_errors=True)
    sys.exit(2 if code != "cancel" else 3)


def run(cmd, env, cwd, progress=False) -> tuple[int, str]:
    """Runs a command, keeping its output in the log and returning the last lines."""
    note("+ " + " ".join(str(c) for c in cmd))
    kw = {"start_new_session": True} if not WIN else {"creationflags": 0x00000200}   # CREATE_NEW_PROCESS_GROUP
    p = subprocess.Popen([str(c) for c in cmd], cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, **kw)
    procs.append(p)
    tail: list[str] = []
    last = 0.0
    for raw in p.stdout:
        line = raw.decode("utf-8", "replace").rstrip()
        note(line)
        tail = (tail + [line])[-12:]
        m = re.match(r"\[(\d+)/(\d+)\]", line)
        if progress and m and time.time() - last > 0.3:
            say("PROGRESS", m.group(1), m.group(2))
            last = time.time()
    rc = p.wait()
    procs.remove(p)
    if cancelled:
        fail("cancel", "stopped")
    return rc, "\n".join(tail)


def step_start(n: int, words: str):
    global step_no, step_words
    step_no, step_words = n, words
    say("STEP", n, STEPS, words)


def classify(tail: str, what: str, rc: int):
    """A command ended with status rc. The player is told the step and where the logs are; the status goes to the log."""
    note(f"{what} ended with exit status {rc}")
    low = tail.lower()
    if "no space left" in low or "not enough space" in low or "errno 28" in low:
        fail("space", "The disk is full. Free some space (the build needs about 2.5 GB while it runs) and try again.")
    if "permission denied" in low or "access is denied" in low or "winerror 5" in low:
        fail("tool", f"{what} was refused by the system (access denied). A virus scanner may be blocking the build's tools: "
                     "allow this folder, then try again.")
    fail("build", f"Building the game stopped at step {step_no} of {STEPS} ({step_words[:1].lower()}{step_words[1:]}). The logs folder says why.")


def main():
    global log
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--disc", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--log")
    ap.add_argument("--jobs", type=int, default=min(os.cpu_count() or 2, 8))
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--detached", action="store_true", help="no controlling program: the end of standard input is not a cancel")
    a = ap.parse_args()
    work, out = Path(a.work).resolve(), Path(a.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    log = open(a.log or out / "build.log", "w", encoding="utf-8", errors="replace")
    if not a.detached:
        threading.Thread(target=watch_stdin, daemon=True).start()
    t0 = time.time()
    disc = Path(a.disc)
    if not disc.exists():
        fail("disc", "The game files can't be found. Make the game archive again from your disc.")
    # a clean start: whatever an earlier build left is removed
    global work_dir, keep_work
    work_dir, keep_work = work, a.keep
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    if shutil.disk_usage(work).free < NEED_FREE:
        fail("space", "There is not enough free disk space to build the game. It needs about 2.5 GB while it runs.")
    tc = KIT / "toolchain"
    py = Path(sys.executable)
    sp = py.parent / "Lib" / "site-packages" if WIN else py.parent.parent / "lib" / "python3.14" / "site-packages"
    path = [str(py.parent), str(sp / "cmake" / "data" / "bin"), str(sp / "ninja" / "data" / "bin"), str(tc / "bin")]
    env = dict(os.environ)
    env["PATH"] = os.pathsep.join(path + [env.get("PATH", "")])
    env["MH2_ORIG_DIR"] = str(disc)
    env["CLANG"] = str(tc / "bin" / ("clang" + EXE))
    env["PYTHONUTF8"] = "1"
    env["MH2_LAYOUT"] = "manifest"   # the committed layout and the player's own files: there is no matching build here to take it from
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    env.pop("PYTHONPATH", None)
    for k in ("MH2_LLVM", "MH2_LLVM_MINGW", "CC", "CXX", "VIRTUAL_ENV"):
        env.pop(k, None)
    tree = work / "src"
    py_ = [py, "-X", "utf8"]

    step_start(1, "Reading the game from your files")
    shutil.copytree(SRC, tree, ignore=shutil.ignore_patterns("__pycache__"))
    rc, tail = run(py_ + ["tools/extract.py"], env, tree)
    if rc:
        if re.search(r"not found|No such file|not a|mismatch|sha|SHA|expected", tail):
            fail("disc", "These are not the right game files. They must come from your own copy of Monster Hunter 2 (Dos), the Japanese "
                         "PS2 disc (SLPM-66280). Make the game archive again from your disc.")
        classify(tail, "Reading your game files", rc)
    step_start(2, "Splitting the game into parts")
    rc, tail = run(py_ + ["tools/configure.py", "--split-only"], env, tree)
    if rc:
        classify(tail, "The split", rc)
    step_start(3, "Writing the game's data tables")
    rc, tail = run(py_ + ["tools/content_gen.py", "--jobs", str(a.jobs)], env, tree)
    if rc:
        classify(tail, "The data tables", rc)
    rc, tail = run(py_ + ["tools/asset_c.py", "config/assets/lbguild/lbguild.txt", "build/assets/lbguild/lbguild.c"], env, tree)
    if rc:
        classify(tail, "The data tables", rc)
    step_start(4, "Compiling the game")
    cmd = py_ + ["tools/pc/build.py", "--target", "core", "-j", str(a.jobs)]
    cmd += ["--llvm-mingw", tc] if WIN else ["--llvm", tc, "--kit"]
    rc, tail = run(cmd, env, tree, progress=True)
    if rc:
        classify(tail, "The compiler", rc)
    step_start(5, "Putting the game in place")
    plat = "windows" if WIN else "linux"
    built = tree / "build" / "pc" / f"{plat}-i686" / "core" / ("mh2-core" + EXE)
    if not built.exists():
        fail("build", "The compiler finished, but the game is not there.")
    kit_id = ""
    for line in (KIT / "kit.txt").read_text().splitlines():
        if line.startswith("kit "):
            kit_id = line.split()[1]
    tmp = out / ("mh2-core" + EXE + ".tmp")
    shutil.copyfile(built, tmp)
    os.chmod(tmp, 0o755)
    final = out / ("mh2-core" + EXE)
    os.replace(tmp, final)
    (out / "built.txt").write_text(f"kit {kit_id}\n")
    # The program's strings, as the work file a translation starts from (text packs: tools/pc/textpack.py). They are read from the data
    # tables just made, which the work folder's removal takes, so the list is made now. A failure here costs nothing: the game is built,
    # and the author can make the list again with the tool.
    strings = out / "strings.tsv"
    strings.unlink(missing_ok=True)
    if (tree / "tools" / "pc" / "textpack.py").exists():
        tmp_s = out / "strings.tsv.tmp"
        rc, tail = run(py_ + ["tools/pc/textpack.py", "strings", "--gen", "build/gen", "--to", tmp_s], env, tree)
        if rc == 0 and tmp_s.exists():
            os.replace(tmp_s, strings)
        else:
            tmp_s.unlink(missing_ok=True)
            note(f"the list of the program's strings was not made (exit status {rc})")
    note(f"done in {time.time() - t0:.0f} s")
    if not a.keep:
        shutil.rmtree(work, ignore_errors=True)
    say("DONE", final)


if __name__ == "__main__":
    main()
