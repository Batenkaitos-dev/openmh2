#!/usr/bin/env python3
"""A live process that holds a memory card's lock, for the launcher's headless test (the case's NAME.holder marks it).

    holder.py LOCKFILE DONEFILE

The parent starts a detached child and returns once the child has written its own process id to LOCKFILE, the way a running mh2
does (cards.c: CARD.lock holds the pid); the child lives until DONEFILE exists (the test makes it when the case is over) or two
minutes have passed. The cards page marks that card `in use`, since the pid is another live process's, not mh2's own.
"""
import os
import subprocess
import sys
import time

if len(sys.argv) == 4 and sys.argv[1] == "--child":
    lock, done = sys.argv[2], sys.argv[3]
    with open(lock, "w") as f:
        f.write(f"{os.getpid()}\n")
    end = time.monotonic() + 120
    while not os.path.exists(done) and time.monotonic() < end:
        time.sleep(0.2)
    os.remove(lock) if os.path.exists(lock) else None
    sys.exit(0)

lock, done = sys.argv[1], sys.argv[2]
kw = {}
if os.name == "nt":
    kw["creationflags"] = 0x00000008 | 0x00000200   # DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP
else:
    kw["start_new_session"] = True
subprocess.Popen([sys.executable, os.path.abspath(__file__), "--child", lock, done], stdin=subprocess.DEVNULL,
                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, close_fds=True, **kw)
end = time.monotonic() + 15
while not os.path.exists(lock) and time.monotonic() < end:
    time.sleep(0.05)
sys.exit(0 if os.path.exists(lock) else 1)
