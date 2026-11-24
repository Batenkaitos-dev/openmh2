#!/usr/bin/env python3
"""hfr_even.py DUMPDIR [DUMPDIR ...]: how evenly the pictures between two steps change (development builds).

DUMPDIR holds the pictures of an MH2_HFR_DUMP run: rVBLANK.png (the game's own picture of a step) and mVBLANK_ALPHA.png (an in-between picture
made at that VBlank, alpha in 256ths). The run is made hosted and not in real time (the pictures are the same whatever the machine does).

For each step with the real picture before it (alpha 0), its in-between pictures and the real picture after it (alpha 1): d_j is the mean absolute
difference (over the red, green and blue bytes, 0 to 255) of pictures j and j+1, e_j the alpha between them, and the step's index is
max_j(d_j / e_j) / (sum d / sum e). 1.0 is change spread as time is; a picture shown twice makes it rise. Steps that are still (sum d under
--floor) or cut (the two real pictures differ by more than --cut) are left out. Printed for each folder: the steps used, the ones left out, and
the median, the 95th percentile, the worst index and how many steps are over 1.5.

    --control-repeat   one in-between picture of each step (the middle one) is replaced by the one before it: one picture shown twice, the index
                       of a step with an even change comes out about 2
    --control-hold     every in-between picture is replaced by the real picture before it: the step jumps at its end, the index comes out
                       about the number of pictures a step has
    --rows N           compare every Nth row only (default 1: every pixel)
    --floor F, --cut C the limits above (defaults 0.05 and 30)
    --steps            one line for each step
    --selftest         made-up pictures with a known answer (an even change, an eased one, and the two controls); prints `hfr_even selftest: OK`

The pictures are decoded here (plain Python, 8-bit RGB or RGBA PNG): about a second for a picture whose rows use the average or Paeth filters."""
import itertools
import os
import re
import struct
import sys
import tempfile
import zlib
from operator import sub


def read_png(path):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("%s: not a PNG" % path)
    pos, idat, w, h, bpp = 8, [], 0, 0, 3
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            w, h, depth, ctype, _, _, inter = struct.unpack(">IIBBBBB", body)
            if depth != 8 or ctype not in (2, 6) or inter:
                raise ValueError("%s: depth %d type %d interlace %d is not read" % (path, depth, ctype, inter))
            bpp = 3 if ctype == 2 else 4
        elif kind == b"IDAT":
            idat.append(body)
        pos += 12 + n
    raw = zlib.decompress(b"".join(idat))
    stride = w * bpp
    rows, prev, i = [], bytes(stride), 0
    lo = int.from_bytes(b"\x7f" * stride, "big")
    hi = int.from_bytes(b"\x80" * stride, "big")
    for _ in range(h):
        f = raw[i]
        line = raw[i + 1:i + 1 + stride]
        i += 1 + stride
        if f == 0:
            cur = line
        elif f == 2:   # up: a bytewise sum of the row and the one above, no carry across bytes
            a, b = int.from_bytes(line, "big"), int.from_bytes(prev, "big")
            cur = (((a & lo) + (b & lo)) ^ ((a ^ b) & hi)).to_bytes(stride, "big")
        elif f == 1:   # sub: a running sum along each channel
            out = bytearray(stride)
            for c in range(bpp):
                out[c::bpp] = bytes(x & 255 for x in itertools.accumulate(line[c::bpp]))
            cur = bytes(out)
        else:   # average and Paeth
            out = bytearray(line)
            for x in range(stride):
                a = out[x - bpp] if x >= bpp else 0
                b = prev[x]
                if f == 3:
                    out[x] = (out[x] + ((a + b) >> 1)) & 255
                else:
                    c = prev[x - bpp] if x >= bpp else 0
                    p = a + b - c
                    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                    out[x] = (out[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
            cur = bytes(out)
        rows.append(cur)
        prev = cur
    if bpp == 4:
        rows = [bytes(r[k] for k in range(len(r)) if k % 4 != 3) for r in rows]
    return w, h, rows


def steps_of(d, bare=None):
    """The steps of a folder: (vblank, real picture before, [(alpha, picture)], real picture after), in order. bare, a list, gets the number of the steps that have a real
    picture on both ends and no in-between picture (a black or 2D stretch makes none: the stretch is then not the scene it was meant to be)."""
    reals, mids = {}, {}
    for f in os.listdir(d):
        m = re.match(r"r(\d+)\.png$", f)
        if m:
            reals[int(m.group(1))] = os.path.join(d, f)
            continue
        m = re.match(r"m(\d+)_(\d+)\.png$", f)
        if m:
            mids.setdefault(int(m.group(1)), []).append((int(m.group(2)), os.path.join(d, f)))
    vs = sorted(reals)
    out = []
    for before, v in zip(vs, vs[1:]):
        if v in mids:
            out.append((v, reals[before], sorted(mids[v]), reals[v]))
        elif bare is not None:
            bare.append(v)
    return out


class Pics:
    def __init__(self, rows_step):
        self.cache, self.step, self.dc = {}, rows_step, {}

    def get(self, path):
        if path not in self.cache:
            if len(self.cache) > 24:
                self.cache.pop(next(iter(self.cache)))
            self.cache[path] = read_png(path)[2]
        return self.cache[path]

    def diff(self, pa, pb):
        key = (pa, pb)
        if key not in self.dc:
            ra, rb = self.get(pa), self.get(pb)
            tot = n = 0
            for y in range(0, len(ra), self.step):
                tot += sum(map(abs, map(sub, ra[y], rb[y])))
                n += len(ra[y])
            self.dc[key] = tot / n if n else 0.0
        return self.dc[key]


def index_of(seq, alphas, pics):
    """seq: the pictures in order (the real one before, the in-between ones, the real one after), alphas: theirs in 0..1."""
    ds = [pics.diff(seq[j], seq[j + 1]) for j in range(len(seq) - 1)]
    es = [alphas[j + 1] - alphas[j] for j in range(len(seq) - 1)]
    sd, se = sum(ds), sum(es)
    worst = max(d / e for d, e in zip(ds, es) if e > 0)
    return sd, (worst / (sd / se)) if sd > 0 else 0.0


def run_folder(d, control, rows, floor, cut, show):
    pics = Pics(rows)
    idx, still, cutn, used = [], 0, 0, 0
    bare = []
    for v, before, mids, after in steps_of(d, bare):
        seq = [before] + [p for _, p in mids] + [after]
        al = [0.0] + [a / 256.0 for a, _ in mids] + [1.0]
        if pics.diff(before, after) > cut:
            cutn += 1
            continue
        if control == "repeat":
            j = (len(mids) + 1) // 2
            seq[j] = seq[j - 1]
        elif control == "hold":
            for j in range(1, len(mids) + 1):
                seq[j] = before
        sd, ix = index_of(seq, al, pics)
        if sd < floor:
            still += 1
            continue
        used += 1
        idx.append(ix)
        if show:
            print("  step %d: %d in-between, change %.3f, index %.2f" % (v, len(mids), sd, ix))
    idx.sort()
    if not idx:
        return dict(used=0, still=still, cut=cutn, bare=len(bare))
    n = len(idx)
    return dict(used=n, still=still, cut=cutn, bare=len(bare), median=idx[n // 2], p95=idx[min(n - 1, n * 95 // 100)], worst=idx[-1], over=sum(1 for x in idx if x > 1.5))


def line(label, r):
    if not r["used"]:
        return "%s: no step to measure (still %d, cut %d, with no in-between picture %d)" % (label, r["still"], r["cut"], r.get("bare", 0))
    return "%s: steps %d (still %d, cut %d left out; %d with no in-between picture), index median %.2f p95 %.2f worst %.2f, over 1.5: %d" % (
        label, r["used"], r["still"], r["cut"], r.get("bare", 0), r["median"], r["p95"], r["worst"], r["over"])


def png(path, w, h, px, kinds):
    """A made-up picture: h rows of w pixels, each row (r, g, b) of px[y], stored with the filter kinds[y % len(kinds)]."""
    bpp, rows, prev, raw = 3, [], bytes(3 * w), bytearray()
    for y in range(h):
        cur = bytes(c for _ in range(w) for c in px[y])
        f = kinds[y % len(kinds)]
        out = bytearray()
        for x in range(3 * w):
            a = cur[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            p = a + b - c
            pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
            pred = (0, a, b, (a + b) >> 1, a if pa <= pb and pa <= pc else b if pb <= pc else c)[f]
            out.append((cur[x] - pred) & 255)
        raw += bytes([f]) + out
        prev = cur
    ch = lambda k, b: struct.pack(">I", len(b)) + k + b + struct.pack(">I", zlib.crc32(k + b) & 0xFFFFFFFF)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + ch(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + ch(b"IDAT", zlib.compress(bytes(raw))) + ch(b"IEND", b""))


def selftest():
    w, h = 12, 10
    alphas = (51, 102, 154, 205)
    levels = {}
    with tempfile.TemporaryDirectory() as t:
        for name, ease in (("even", lambda a: a), ("eased", lambda a: a * a * (3 - 2 * a))):
            d = os.path.join(t, name)
            os.mkdir(d)
            for k, v in enumerate((2, 4, 6, 8)):
                base = 40 + 20 * k
                png(os.path.join(d, "r%06d.png" % v), w, h, [(base, base, base)] * h, (0, 1, 2, 3, 4))
                if k:
                    for a in alphas:
                        lv = round(base - 20 + 20 * ease(a / 256.0))
                        png(os.path.join(d, "m%06d_%03d.png" % (v, a)), w, h, [(lv, lv, lv)] * h, (4, 3, 2, 1, 0))
        # a decoder check: a coloured picture through every filter
        px = [tuple(((x * 17 + y * 5 + c * 40) * 7) % 256 for c in range(3)) for y in range(h) for x in [y]]
        png(os.path.join(t, "k.png"), 4, h, px, (0, 1, 2, 3, 4))
        got = read_png(os.path.join(t, "k.png"))[2]
        want = [bytes(c for _ in range(4) for c in p) for p in px]
        assert got == want, "the decoder differs from what was written"
        even = run_folder(os.path.join(t, "even"), None, 1, 0.05, 30, False)
        eased = run_folder(os.path.join(t, "eased"), None, 1, 0.05, 30, False)
        rep = run_folder(os.path.join(t, "even"), "repeat", 1, 0.05, 30, False)
        hold = run_folder(os.path.join(t, "even"), "hold", 1, 0.05, 30, False)
        print("selftest: " + line("even change", even))
        print("selftest: " + line("eased change", eased))
        print("selftest: " + line("control, one picture repeated", rep))
        print("selftest: " + line("control, held", hold))
        ok = even["used"] == 3 and even["worst"] < 1.15 and eased["median"] > 1.4 and 1.8 < rep["median"] < 2.6 and hold["median"] > 4.0
        print("hfr_even selftest: " + ("OK" if ok else "FAILED"))
        return 0 if ok else 1


def main():
    a = sys.argv[1:]
    control, rows, floor, cut, show, dirs, i = None, 1, 0.05, 30.0, False, [], 0
    while i < len(a):
        if a[i] == "--selftest":
            return selftest()
        elif a[i] == "--control-repeat":
            control = "repeat"
        elif a[i] == "--control-hold":
            control = "hold"
        elif a[i] == "--steps":
            show = True
        elif a[i] in ("--rows", "--floor", "--cut"):
            v = a[i + 1]
            i += 1
            if a[i - 1] == "--rows":
                rows = max(1, int(v))
            elif a[i - 1] == "--floor":
                floor = float(v)
            else:
                cut = float(v)
        else:
            dirs.append(a[i])
        i += 1
    if not dirs:
        print(__doc__)
        return 2
    for d in dirs:
        print(line(d + (" [control: %s]" % control if control else ""), run_folder(d, control, rows, floor, cut, show)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
