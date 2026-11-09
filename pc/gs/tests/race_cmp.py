#!/usr/bin/env python3
"""race_cmp.py REF.txt RUN.txt: compares the picture hashes of two runs written with MH2GS_MIDHASH (a development build).

Each line is "CHAIN K HASH": the chain (a step the in-between pictures were made for), K -1 for the real picture and
0.. for the in-between ones, and an FNV-1a of the picture's pixels. REF is the oracle run (MH2GS_SERIAL=1: the GPU is
waited for after every step of a chain, so nothing runs ahead and no race between the queues is possible); RUN is the
run under test. Prints how many pictures are common, how many differ, how many of them are real ones, the count for
each K and the first three. Exit status 0 when none differ, 1 when some do, 2 when no picture is common.

The race test (at 144 or 60), the same session and the same alphas (MH2_HFR_ALPHAS=51,102,154,205) three times,
--fps 144 --gs-hash, MH2GS_MID_WAIT=1, inj-rath-rest to VSync 5000 (chain 549 is the one that shows the race, near VSync 4,980):
  1. MH2GS_SERIAL=1                                     (the oracle)
  2. MH2GS_TEST_STALL=400                               (the ordering of patch 0005 on, the restores stalled)
  3. MH2GS_TEST_STALL=400 MH2GS_TEST_NOSYNC=1           (the ordering off: this one must differ)
Run 2 against run 1 must give 0 differ, run 3 against run 1 some (49 of 2,782 at 60 and at 144 on 74cbb4b63's build). A stall of
120 does not show it on that build (it did on session 29's, 3,041 of 5,770 differ; why is not proved, the restore is
probably over before the next picture's upload is submitted): run 3 then gives 0 to 2 differ. Check that the control fails each
time, before trusting a run 2 that passes.
"""
import sys


def load(path):
    d = {}
    with open(path) as f:
        for ln in f:
            t = ln.split()
            if len(t) == 3:
                d[(int(t[0]), int(t[1]))] = t[2]
    return d


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    a, b = load(sys.argv[1]), load(sys.argv[2])
    keys = [k for k in a if k in b]
    bad = [k for k in keys if a[k] != b[k]]
    by_k = {}
    for _, k in bad:
        by_k[k] = by_k.get(k, 0) + 1
    real = sum(1 for _, k in bad if k == -1)
    print("%d of %d pictures common, %d differ (real %d), by K %s, first %s"
          % (len(keys), len(a), len(bad), real, sorted(by_k.items()), bad[:3]))
    if not keys:
        return 2
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
