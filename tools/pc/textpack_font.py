"""The glyph writer (the commands `glyphs` and `font` of textpack.py): the pack's letters drawn in the game's font format.

The game's font file (DATA.BIN file 0, entry 5906) holds 7,808 glyphs of 20 x 20 pixels at 2 bits each, 100 bytes a glyph, left pixel
in the high bits, level 0 (no ink) to 3 (full ink). A character map gives a pack up to 63 letters, the bytes 0xA1 to 0xDF; the game
draws those bytes with glyphs 846 to 908 (the half-width katakana's), narrow glyphs whose ink lies in the left 11 columns, the
baseline between rows 17 and 18 and capitals 15 rows high.

    textpack.py glyphs --ttf FONT.ttf --charmap charmap.txt --to PACK/glyphs.txt [--preview PICTURE.png] [--weight 1.5]
        draws each letter of the map from a TrueType font (stdlib only: a reader of `glyf` outlines and a scanline rasteriser) into a
        text file of the glyphs, one block each, that the author may edit by hand;
    textpack.py font --disc DIR --pack PACK [--glyphs FILE]
        puts those glyphs into the game's font (from the player's copy) and writes PACK/files/DATA.BIN/00000/05906.

The glyph file holds only the pack's own drawings, so it may travel with the pack; the font file `font` writes holds the game's
other glyphs and is made on the machine that has the game.

Standard library only.
"""
import os
import struct
import sys
import zlib

import textpack as tp

FONT_FILE = 5906
GLYPH_BYTES = 100
GLYPHS = 7808
FIRST_SLOT = 846          # the glyph the byte 0xA1 draws
CELL = 20                 # texels a side
COLS = 12                 # the columns a narrow glyph uses
BASELINE = 18             # the row below the last row of a capital's ink
CAP = 15                  # a capital's height in texels
ADVANCE = 10              # the columns of a cell a letter is centred in
MAXW = 10                 # the widest ink of a letter
DESC = 2                  # rows below the baseline
LEVELS = ".-+#"
WEIGHT = 1.5              # gain on the ink share: the game's strokes are two texels wide


# ---------------------------------------------------------------- TrueType outlines

class TrueType:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.d = f.read()
        d = self.d
        tag = d[:4]
        if tag == b"OTTO":
            raise SystemExit("%s: an OpenType font with PostScript outlines; give a TrueType (glyf) font" % path)
        if tag == b"ttcf":
            raise SystemExit("%s: a font collection; give a single font file" % path)
        if tag not in (b"\x00\x01\x00\x00", b"true"):
            raise SystemExit("%s: not a TrueType font" % path)
        n = struct.unpack_from(">H", d, 4)[0]
        self.t = {}
        for i in range(n):
            tg, cs, off, ln = struct.unpack_from(">4sIII", d, 12 + 16 * i)
            self.t[tg.decode("latin-1")] = (off, ln)
        for need in ("head", "maxp", "cmap", "loca", "glyf"):
            if need not in self.t:
                raise SystemExit("%s: no '%s' table (a TrueType font with outlines is needed)" % (path, need))
        h = self.t["head"][0]
        self.upm = struct.unpack_from(">H", d, h + 18)[0]
        self.loc_fmt = struct.unpack_from(">h", d, h + 50)[0]
        self.nglyphs = struct.unpack_from(">H", d, self.t["maxp"][0] + 4)[0]
        self.cmap = self._cmap()
        self._cache = {}

    def _cmap(self):
        d = self.d
        off = self.t["cmap"][0]
        n = struct.unpack_from(">H", d, off + 2)[0]
        best = None
        for i in range(n):
            pid, eid, so = struct.unpack_from(">HHI", d, off + 4 + 8 * i)
            fmt = struct.unpack_from(">H", d, off + so)[0]
            rank = {(3, 10, 12): 4, (0, 4, 12): 4, (3, 1, 4): 3, (0, 3, 4): 3, (0, 4, 4): 2, (0, 6, 4): 2}.get((pid, eid, fmt), 0)
            if rank and (best is None or rank > best[0]):
                best = (rank, off + so, fmt)
        if not best:
            raise SystemExit("the font has no Unicode character table (cmap format 4 or 12)")
        m = {}
        o, fmt = best[1], best[2]
        if fmt == 12:
            ng = struct.unpack_from(">I", d, o + 12)[0]
            for g in range(ng):
                s, e, gid = struct.unpack_from(">III", d, o + 16 + 12 * g)
                for c in range(s, min(e, 0x2FFFF) + 1):
                    m[c] = gid + c - s
        else:
            segx2 = struct.unpack_from(">H", d, o + 6)[0]
            seg = segx2 // 2
            ends = struct.unpack_from(">%dH" % seg, d, o + 14)
            starts = struct.unpack_from(">%dH" % seg, d, o + 16 + segx2)
            deltas = struct.unpack_from(">%dh" % seg, d, o + 16 + 2 * segx2)
            ro = o + 16 + 3 * segx2
            ranges = struct.unpack_from(">%dH" % seg, d, ro)
            for k in range(seg):
                for c in range(starts[k], ends[k] + 1):
                    if c == 0xFFFF:
                        continue
                    if ranges[k] == 0:
                        g = (c + deltas[k]) & 0xFFFF
                    else:
                        p = ro + 2 * k + ranges[k] + 2 * (c - starts[k])
                        g = struct.unpack_from(">H", d, p)[0]
                        if g:
                            g = (g + deltas[k]) & 0xFFFF
                    if g:
                        m[c] = g
        return m

    def _glyph_range(self, gid):
        d = self.d
        lo = self.t["loca"][0]
        if self.loc_fmt == 0:
            a, b = struct.unpack_from(">HH", d, lo + 2 * gid)
            a, b = a * 2, b * 2
        else:
            a, b = struct.unpack_from(">II", d, lo + 4 * gid)
        return self.t["glyf"][0] + a, b - a

    def contours(self, gid, depth=0):
        """The outline of a glyph as closed polygons of (x, y) in font units (curves flattened)."""
        if gid in self._cache:
            return self._cache[gid]
        if depth > 6 or gid >= self.nglyphs:
            return []
        d = self.d
        off, ln = self._glyph_range(gid)
        if ln <= 0:
            self._cache[gid] = []
            return []
        nc = struct.unpack_from(">h", d, off)[0]
        p = off + 10
        polys = []
        if nc >= 0:
            ends = struct.unpack_from(">%dH" % nc, d, p) if nc else ()
            p += 2 * nc
            il = struct.unpack_from(">H", d, p)[0]
            p += 2 + il
            npts = ends[-1] + 1 if nc else 0
            flags = []
            while len(flags) < npts:
                f = d[p]
                p += 1
                flags.append(f)
                if f & 8:
                    r = d[p]
                    p += 1
                    flags.extend([f] * r)
            xs, ys = [], []
            v = 0
            for f in flags[:npts]:
                if f & 2:
                    dx = d[p]
                    p += 1
                    v += dx if f & 16 else -dx
                elif not f & 16:
                    v += struct.unpack_from(">h", d, p)[0]
                    p += 2
                xs.append(v)
            v = 0
            for f in flags[:npts]:
                if f & 4:
                    dy = d[p]
                    p += 1
                    v += dy if f & 32 else -dy
                elif not f & 32:
                    v += struct.unpack_from(">h", d, p)[0]
                    p += 2
                ys.append(v)
            s = 0
            for e in ends:
                pts = [(xs[i], ys[i], bool(flags[i] & 1)) for i in range(s, e + 1)]
                polys.append(_flatten(pts))
                s = e + 1
        else:
            while True:
                fl, gi = struct.unpack_from(">HH", d, p)
                p += 4
                if fl & 1:
                    a1, a2 = struct.unpack_from(">hh", d, p)
                    p += 4
                else:
                    a1, a2 = struct.unpack_from(">bb", d, p)
                    p += 2
                if not fl & 2:
                    raise SystemExit("a composite glyph placed by matching points is not supported")
                m = [1.0, 0.0, 0.0, 1.0]
                if fl & 8:
                    m[0] = m[3] = struct.unpack_from(">h", d, p)[0] / 16384.0
                    p += 2
                elif fl & 0x40:
                    m[0], m[3] = (v / 16384.0 for v in struct.unpack_from(">hh", d, p))
                    p += 4
                elif fl & 0x80:
                    m = [v / 16384.0 for v in struct.unpack_from(">hhhh", d, p)]
                    p += 8
                for poly in self.contours(gi, depth + 1):
                    polys.append([(x * m[0] + y * m[2] + a1, x * m[1] + y * m[3] + a2) for x, y in poly])
                if not fl & 0x20:
                    break
        self._cache[gid] = polys
        return polys

    def outline(self, ch):
        gid = self.cmap.get(ord(ch))
        if not gid:
            raise SystemExit("the font has no glyph for %r (U+%04X)" % (ch, ord(ch)))
        return self.contours(gid)


def _flatten(pts, steps=8):
    """A contour of TrueType points (x, y, on-curve) as a polygon."""
    n = len(pts)
    if n == 0:
        return []
    # rotate so that the contour starts on a curve point
    start = next((i for i, p in enumerate(pts) if p[2]), None)
    if start is None:   # all off-curve: start at the midpoint of the first two
        a, b = pts[0], pts[1 % n]
        pts = [((a[0] + b[0]) / 2.0, (a[1] + b[1]) / 2.0, True)] + pts[1:] + pts[:1]
        start = 0
    pts = pts[start:] + pts[:start]
    out = [(float(pts[0][0]), float(pts[0][1]))]
    i = 1
    cur = pts[0]
    while i <= n:
        p = pts[i % n]
        if p[2]:
            out.append((float(p[0]), float(p[1])))
            cur = p
            i += 1
            continue
        nxt = pts[(i + 1) % n]
        if nxt[2]:
            end = nxt
            i += 2
        else:
            end = ((p[0] + nxt[0]) / 2.0, (p[1] + nxt[1]) / 2.0, True)
            i += 1
        for k in range(1, steps + 1):
            t = k / float(steps)
            u = 1 - t
            out.append((u * u * cur[0] + 2 * u * t * p[0] + t * t * end[0], u * u * cur[1] + 2 * u * t * p[1] + t * t * end[1]))
        cur = end
    return out


# ---------------------------------------------------------------- the rasteriser

def rasterise(polys, size, cols=COLS, rows=CELL, sub=8):
    """The ink share (0 to 1) of each texel of a cell of rows x cols texels by the polygons (in texel units, y down), non-zero winding."""
    ink = [[0.0] * cols for _ in range(rows)]
    edges = []
    for poly in polys:
        n = len(poly)
        for i in range(n):
            (x0, y0), (x1, y1) = poly[i], poly[(i + 1) % n]
            if y0 != y1:
                edges.append((x0, y0, x1, y1))
    for r in range(rows):
        for s in range(sub):
            y = r + (s + 0.5) / sub
            xs = []
            for x0, y0, x1, y1 in edges:
                if (y0 <= y < y1) or (y1 <= y < y0):
                    t = (y - y0) / (y1 - y0)
                    xs.append((x0 + t * (x1 - x0), 1 if y1 > y0 else -1))
            xs.sort()
            wind = 0
            for k in range(len(xs) - 1):
                wind += xs[k][1]
                if wind != 0:
                    a, b = xs[k][0], xs[k + 1][0]
                    c0 = max(0, int(a))
                    c1 = min(cols - 1, int(b))
                    for c in range(c0, c1 + 1):
                        lo, hi = max(a, c), min(b, c + 1)
                        if hi > lo:
                            ink[r][c] += (hi - lo) / sub
    return ink


def draw_letters(font, chars, weight=WEIGHT):
    """The glyphs of the characters, [[row string of COLS level digits] x 20] each, in the game's proportions."""
    upm = float(font.upm)
    cap = _ymax(font.outline("H"))
    up = CAP / cap          # texels per font unit above the baseline
    outs = [font.outline(ch) for ch in chars]
    # one scale for the whole set, so that the tallest letter's accent still fits above row 0, and a descender below row 19
    for o in outs:
        top = max([y for poly in o for _, y in poly] or [0])
        if top * up > BASELINE:
            up = BASELINE / float(top)
    low = min([y for o in outs for poly in o for _, y in poly] or [0])
    dn = up if low >= 0 else min(up, DESC / float(-low))
    res = []
    for o in outs:
        pts = [(x, y) for poly in o for x, y in poly]
        if not pts:
            res.append(["." * COLS] * CELL)
            continue
        x0 = min(p[0] for p in pts)
        x1 = max(p[0] for p in pts)
        sx = up
        if (x1 - x0) * sx > MAXW:
            sx = MAXW / float(x1 - x0)
        w = (x1 - x0) * sx
        left = max(0.0, (ADVANCE - w) / 2.0)
        polys = [[((x - x0) * sx + left, BASELINE - (y * up if y >= 0 else y * dn)) for x, y in poly] for poly in o]
        ink = rasterise(polys, upm)
        res.append(["".join(str(min(3, int(min(1.0, ink[r][c] * weight) * 3 + 0.5))) for c in range(COLS)) for r in range(CELL)])
    return res


def _ymax(polys):
    return max(y for poly in polys for _, y in poly)


# ---------------------------------------------------------------- the glyph file

def glyph_file_text(chars, glyphs, first=0xA1, byte_of=None):
    out = ["# Glyphs for the pack's character map, in the game's font: 12 columns by 20 rows a letter.",
           "# . no ink, - light, + heavy, # full ink. The first row is the top; the baseline lies between rows 17 and 18 (counting from 0).",
           "# Edit the pictures; keep the header lines. textpack.py font writes them into the game's font file.", ""]
    for k, (ch, g) in enumerate(zip(chars, glyphs)):
        b = byte_of[k] if byte_of else first + k
        out.append("glyph 0x%02X U+%04X %s" % (b, ord(ch), ch))
        for row in g:
            out.append("".join(LEVELS[int(v)] for v in row))
        out.append("")
    return "\n".join(out)


def read_glyph_file(path):
    """{byte: [20 rows of COLS levels]} from a glyph file; SystemExit names the line of a fault."""
    with open(path, "r", encoding="utf-8") as f:
        lines = f.read().split("\n")
    res = {}
    i = 0
    while i < len(lines):
        ln = lines[i].rstrip("\r")
        i += 1
        if not ln.strip() or ln.startswith("#"):
            continue
        parts = ln.split(" ")
        if parts[0] != "glyph" or len(parts) < 3 or not parts[1].lower().startswith("0x"):
            raise SystemExit("%s:%d: expected a header line `glyph 0xBB U+XXXX CHAR`" % (path, i))
        try:
            b = int(parts[1], 16)
        except ValueError:
            raise SystemExit("%s:%d: %s is not a byte" % (path, i, parts[1]))
        if not 0xA1 <= b <= 0xDF:
            raise SystemExit("%s:%d: the byte 0x%02X is not one of 0xA1 to 0xDF" % (path, i, b))
        if b in res:
            raise SystemExit("%s:%d: the byte 0x%02X is drawn twice" % (path, i, b))
        rows = []
        for r in range(CELL):
            if i >= len(lines):
                raise SystemExit("%s:%d: the glyph of 0x%02X has fewer than %d rows" % (path, i, b, CELL))
            row = lines[i].rstrip("\r")
            i += 1
            if len(row) != COLS or any(c not in LEVELS for c in row):
                raise SystemExit("%s:%d: a row is %d characters of . - + # (got %r)" % (path, i, COLS, row))
            rows.append("".join(str(LEVELS.index(c)) for c in row))
        res[b] = rows
    return res


def pack_glyph(rows):
    """The 100 bytes of a glyph: 20 rows of 5 bytes, four 2-bit pixels each, the left one in the high bits."""
    out = bytearray()
    for r in range(CELL):
        row = rows[r] + "0" * (CELL - COLS)
        for k in range(5):
            v = 0
            for j in range(4):
                v = v << 2 | int(row[4 * k + j])
            out.append(v)
    return bytes(out)


# ---------------------------------------------------------------- a picture of the glyphs (PNG)

def write_png(path, width, height, rows):
    """A greyscale 8-bit PNG; rows: bytes of width pixels each."""
    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    raw = b"".join(b"\0" + r for r in rows)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def preview(path, glyphs, scale=6, per_row=16):
    """All the glyphs on a dark sheet, one cell each (the game's cell: 12 of its 20 columns, 20 rows), enlarged."""
    cw, ch = COLS + 2, CELL + 2
    n = len(glyphs)
    nrows = (n + per_row - 1) // per_row
    W, H = per_row * cw * scale, nrows * ch * scale
    img = [bytearray([40]) * W for _ in range(H)]
    shade = [40, 110, 180, 255]
    for k, g in enumerate(glyphs):
        ox, oy = (k % per_row) * cw * scale, (k // per_row) * ch * scale
        for r in range(CELL):
            for c in range(COLS):
                v = shade[int(g[r][c])]
                for yy in range(scale):
                    line = img[oy + (r + 1) * scale + yy]
                    for xx in range(scale):
                        line[ox + (c + 1) * scale + xx] = v
        # the baseline and the cell's edge, faintly
        for xx in range(COLS * scale):
            img[oy + (BASELINE + 1) * scale][ox + scale + xx] = max(img[oy + (BASELINE + 1) * scale][ox + scale + xx], 70)
    write_png(path, W, H, [bytes(r) for r in img])


# ---------------------------------------------------------------- the commands

def cmd_glyphs(ttf, charmap, to, pic, weight=WEIGHT):
    if not ttf or not charmap or not to:
        raise SystemExit("glyphs needs --ttf FONT, --charmap FILE and --to GLYPHS.TXT")
    cm = tp.Charmap(charmap)
    if not cm.c2b:
        raise SystemExit("%s holds no letters" % charmap)
    bytes_ = sorted(cm.b2c)
    chars = [cm.b2c[b] for b in bytes_]
    font = TrueType(ttf)
    glyphs = draw_letters(font, chars, weight)
    os.makedirs(os.path.dirname(os.path.abspath(to)), exist_ok=True)
    with open(to, "w", encoding="utf-8", newline="\n") as f:
        f.write(glyph_file_text(chars, glyphs, byte_of=bytes_))
    print("glyphs: %d letters drawn from %s into %s" % (len(chars), os.path.basename(ttf), to))
    if pic:
        preview(pic, glyphs)
        print("glyphs: picture in %s" % pic)


def cmd_font(src, pack, glyph_path):
    if not pack:
        raise SystemExit("font needs --pack PACK")
    glyph_path = glyph_path or os.path.join(pack, "glyphs.txt")
    if not os.path.isfile(glyph_path):
        raise SystemExit("font: %s is not there (textpack.py glyphs makes it)" % glyph_path)
    new = read_glyph_file(glyph_path)
    raw = src.raw(FONT_FILE)
    data = tp.lz_decompress(raw)
    if data is None or len(data) != GLYPHS * GLYPH_BYTES:
        raise SystemExit("font: the game's font file is not what this tool reads")
    out = bytearray(data)
    for b, rows in new.items():
        i = FIRST_SLOT + b - 0xA1
        out[i * GLYPH_BYTES:(i + 1) * GLYPH_BYTES] = pack_glyph(rows)
    comp = tp.lz_compress(bytes(out))
    if tp.lz_decompress(comp) != bytes(out):
        raise SystemExit("internal error: the compressed font does not decompress to itself")
    tp.write_pack_file(pack, "DATA.BIN/00000/%05d" % FONT_FILE, comp)
    print("font: %d glyphs written; the file is %d bytes (the disc's: %d) in %s" % (
        len(new), len(comp), len(raw), os.path.join(pack, "files", "DATA.BIN", "00000", "%05d" % FONT_FILE)))
