#!/usr/bin/env python3
"""fptrace_cmp: compares the product traces of the other build and the core (the product trace).

    fptrace_cmp.py --ref REF.bin --core CORE.bin --sites SITES.txt --nm REF_EXE [--out ORDER.txt] [-v]

Both traces hold records of five 32-bit words, VBlank, site word, a, b, result, one for each add or multiply
whose result changes when the operands are exchanged (the site word: bit 31 the operation, multiply 1; the
core's low 31 bits are the pass's site id, listed in SITES.txt; the other build's are the call's return address relative
to f2_mul in REF_EXE, which names the guest function). A function is named (module, guest address) on both
sides (config/symbols/*.txt gives the core's C functions their addresses).

Per function and operation, the records of a VBlank are compared in order:
  same operands, same result      agree
  operands exchanged              the site's operand order is the other one (the result differs): reported
  other operands                  the states have diverged: that VBlank and function are not used, and counted
With --out the sites found in the exchanged order are merged into the order table (tools/pc/ps2fp/mulorder.txt).
"""
import argparse
import bisect
import os
import re
import struct
import subprocess
import sys
from collections import Counter, defaultdict

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))


def records(path):
    data = open(path, "rb").read()
    n = len(data) // 20
    return [struct.unpack_from("<5I", data, 20 * i) for i in range(n)]


def symbol_addrs():
    out = {}
    d = os.path.join(ROOT, "config", "symbols")
    for fn in os.listdir(d):
        for ln in open(os.path.join(d, fn), encoding="utf-8", errors="replace"):
            m = re.match(r"\s*(\w+)\s*=\s*0x([0-9A-Fa-f]+)\s*;", ln)
            if m:
                out[m.group(1)] = int(m.group(2), 16)
    return out


kinds = {}   # site id -> vv, vc, cv, cc: whether the source's first and second operand are constants (the sites file's last field)


def load_sites(path):
    out = {}
    dup = set()
    for ln in open(path):
        f = ln.split()
        if len(f) < 5:
            continue
        sid = int(f[0], 16) & 0x7FFFFFFF
        if sid in out and out[sid][:4] != (f[1], f[2], f[3], int(f[4])):
            dup.add(sid)
        out[sid] = (f[1], f[2], f[3], int(f[4]))
        kinds[sid] = f[5] if len(f) > 5 else "??"
    return out, dup


def ref_functions(exe):
    txt = subprocess.check_output(["nm", "-n", exe], text=True)
    syms = []
    f2 = None
    for ln in txt.splitlines():
        p = ln.split()
        if len(p) == 3 and p[1] in "tT":
            a = int(p[0], 16)
            if p[2] == "f2_mul":
                f2 = a
            m = re.match(r"f_([a-z_0-9]+?)_([0-9A-F]{8})(?:_hook)?$", p[2])
            syms.append((a, p[2], (m.group(1), int(m.group(2), 16)) if m else (None, p[2])))
    return f2, [s[0] for s in syms], syms


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--ref", required=True)
    ap.add_argument("--core", required=True)
    ap.add_argument("--sites", required=True)
    ap.add_argument("--nm", required=True)
    ap.add_argument("--out")
    ap.add_argument("-v", action="store_true")
    ap.add_argument("--json", help="write each site's counts (records that agree, records exchanged, kind of operands) here (default: sites.json beside CORE)")
    ap.add_argument("--first", type=int, default=20, help="the exchanged-order sites to list")
    a = ap.parse_args()
    sym = symbol_addrs()
    sites, dup = load_sites(a.sites)
    if dup:
        print("site id collisions:", len(dup))
    f2, addrs, syms = ref_functions(a.nm)

    def ref_key(w):
        off = w & 0x7FFFFFFF
        if off >= 1 << 30:
            off -= 1 << 31
        i = bisect.bisect_right(addrs, f2 + off) - 1
        return syms[i][2]

    def core_key(sid):
        rel, fn, op, k = sites[sid]
        mod = rel.split("/")[1] if rel.startswith("src/") else rel
        ad = sym.get(fn)
        if ad is None:
            m = re.match(r"func_([0-9A-Fa-f]{8})$", fn)
            ad = int(m.group(1), 16) if m else fn
        return (mod, ad)

    R = defaultdict(lambda: defaultdict(list))   # (key, op) -> vblank -> [(a, b, r)]
    C = defaultdict(lambda: defaultdict(list))   # (key, op) -> vblank -> [(a, b, r, site)]
    nref = ncore = 0
    for v, w, x, y, r in records(a.ref):
        R[(ref_key(w), w >> 31)][v].append((x, y, r))
        nref += 1
    unk = 0
    for v, w, x, y, r in records(a.core):
        sid = w & 0x7FFFFFFF
        if sid not in sites:
            unk += 1
            continue
        C[(core_key(sid), w >> 31)][v].append((x, y, r, sid))
        ncore += 1
    print("records: reference %d, core %d (unknown sites %d)" % (nref, ncore, unk))
    # The order MWCC schedules the instructions of a function is not the C source's, so the records of a function and a
    # VBlank are compared as multisets of operand pairs. A function MWCC inlined (a static helper of the C) is pooled
    # with the other unmatched functions of its module.
    def pool(key):
        return (key[0][0], "pool", key[1])
    swapped = Counter()
    agree_site = Counter()
    same = diverged = cse_core = cse_ref = 0
    firstdiv = None
    divs = []
    nunref = Counter()
    ncuncore = Counter()
    unmatched_ref = {}
    unmatched_core = {}
    matched = set(R) & set(C)
    groups = defaultdict(lambda: [defaultdict(list), defaultdict(list)])   # (group, vblank) -> [ref by unordered pair, core]
    for key, d in R.items():
        g = key if key in matched else pool(key)
        if key not in matched:
            unmatched_ref[key] = sum(len(x) for x in d.values())
        for v, lst in d.items():
            for (x, y, r) in lst:
                groups[(g, v)][0][(min(x, y), max(x, y))].append((x, y, r))
    for key, d in C.items():
        g = key if key in matched else pool(key)
        if key not in matched:
            unmatched_core[key] = sum(len(x) for x in d.values())
        for v, lst in d.items():
            for (x, y, r, sid) in lst:
                groups[(g, v)][1][(min(x, y), max(x, y))].append((x, y, r, sid))
    for (g, v), (rd, cd) in sorted(groups.items(), key=lambda kv: (kv[0][1], str(kv[0][0]))):
        for pair in set(rd) | set(cd):
            rl, cl = rd.get(pair, []), cd.get(pair, [])
            rc = Counter((x, y, r) for (x, y, r) in rl)
            refset = set(rc)
            coreset = {ev[:3] for ev in cl}
            left = []
            for ev in cl:
                if rc[ev[:3]] > 0:
                    rc[ev[:3]] -= 1
                    same += 1
                    agree_site[ev[3]] += 1
                else:
                    left.append(ev)
            rest = [k for k, n in rc.items() for _ in range(n)]
            for ev in left:
                if ev[:3] in refset:    # the same operation more often: MWCC shared a common subexpression
                    cse_core += 1
                    continue
                alt = next((k for k in rest if k[:2] == (ev[1], ev[0])), None)
                if alt is not None:
                    rest.remove(alt)
                    swapped[ev[3]] += 1
                else:
                    diverged += 1
                    divs.append((v, "core only %08X %08X -> %08X, %s" % (ev[0], ev[1], ev[2], ":".join(map(str, sites[ev[3]])))))
            rest2 = [k for k in rest if k not in coreset]
            cse_ref += len(rest) - len(rest2)
            rest = rest2
            diverged += len(rest)
            for k in rest:
                divs.append((v, "reference only %08X %08X -> %08X, %s" % (k + (g,))))
    print("functions with records on one side only: reference %d (%d records), core %d (%d records)" % (
        len(unmatched_ref), sum(unmatched_ref.values()), len(unmatched_core), sum(unmatched_core.values())))
    print("repeated operations (same operands and result, not met as often on the other side): core %d, reference %d" % (cse_core, cse_ref))
    print("agree %d, exchanged %d (sites %d), other operands %d" % (same, sum(swapped.values()), len(swapped), diverged))
    # the sites by the kind of the source's operands (vv: two variables, vc: the second a constant, cv: the first, cc)
    for kd in ("vv", "vc", "cv", "cc"):
        ok = {x for x in agree_site if kinds.get(x) == kd}
        sw = {x for x in swapped if kinds.get(x) == kd}
        print("  kind %s: sites with agreeing records %d (%d records), exchanged %d (%d records), both %d" % (
            kd, len(ok - sw), sum(agree_site[x] for x in ok), len(sw - ok), sum(swapped[x] for x in sw), len(ok & sw)))
    if not a.json:
        a.json = os.path.join(os.path.dirname(os.path.abspath(a.core)), "sites.json")
    if a.json:    # per site: records that agree, records in the exchanged order (merged over routes by the caller)
        import json
        with open(a.json, "w") as f:
            json.dump({":".join(map(str, sites[x])): [agree_site[x], swapped[x], kinds.get(x, "??")]
                       for x in set(agree_site) | set(swapped)}, f)
    divs = [d for d in divs if "'main', 'pool'" not in d[1]]     # the C library's: not compared here
    divs.sort(key=lambda d: d[0])
    for v, t in divs[:a.first]:
        print("  VBlank %d: %s" % (v, t))
    for sid, n in swapped.most_common(a.first):
        print("  swap %-60s x%d" % (":".join(map(str, sites[sid])), n))
    if a.v:
        for nm, d in (("reference only", unmatched_ref), ("core only", unmatched_core)):
            for key, n in sorted(d.items(), key=lambda kv: -kv[1])[:a.first]:
                print("  %s %s %s: %d" % (nm, key[0], hex(key[0][1]) if isinstance(key[0][1], int) else key[0][1], n))

    if a.out:
        tab = {}
        if os.path.exists(a.out):
            for ln in open(a.out):
                f = ln.split("#")[0].split()
                if f:
                    tab[(f[0], f[1], f[2])] = (int(f[3]), {int(k) for k in f[4:]})
        count = Counter((rel, fn, op) for rel, fn, op, k in sites.values())     # N: how many of an operation a function has
        for sid in swapped:    # seen in the other order than the build made it: the site's state toggles
            rel, fn, op, k = sites[sid]
            n, ks = tab.get((rel, fn, op), (count[(rel, fn, op)], set()))
            tab[(rel, fn, op)] = (count[(rel, fn, op)], ks ^ {k})
        head = [ln for ln in open(a.out) if ln.startswith("#")] if os.path.exists(a.out) else []
        with open(a.out, "w") as f:
            f.writelines(head)
            for (rel, fn, op), (n, ks) in sorted(tab.items()):
                if ks:
                    f.write("%s %s %s %d %s\n" % (rel, fn, op, n, " ".join(map(str, sorted(ks)))))
        print("table entries:", sum(1 for v in tab.values() if v[1]), "written to", a.out)


if __name__ == "__main__":
    sys.exit(main())
