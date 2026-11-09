#!/usr/bin/env python3
"""The tools that make a text pack: text out and text in for the game's text, the checker, and the glyph writer.

    python3 tools/pc/textpack.py list    (--disc DIR | --archive FILE.arc)
    python3 tools/pc/textpack.py out     (--disc DIR | --archive FILE.arc) --to DIR [--charmap FILE]
    python3 tools/pc/textpack.py in      (--disc DIR | --archive FILE.arc) --from DIR --pack PACK [--name NAME] [--charmap FILE]
    python3 tools/pc/textpack.py changes (--disc DIR | --archive FILE.arc) --from DIR --to DIR [--charmap FILE]
    python3 tools/pc/textpack.py build   (--disc DIR | --archive FILE.arc) --from SOURCE --pack PACK
    python3 tools/pc/textpack.py strings (--gen DIR ... | --log FILE ...) --to STRINGS.TSV [--all]
    python3 tools/pc/textpack.py table   --from STRINGS.TSV --pack PACK [--name NAME] [--charmap FILE]
    python3 tools/pc/textpack.py check   PACK [--disc DIR | --archive FILE.arc]
    python3 tools/pc/textpack.py glyphs  --ttf FONT.ttf --charmap FILE --to PACK/glyphs.txt [--preview PICTURE.png] [--weight W]
    python3 tools/pc/textpack.py font    (--disc DIR | --archive FILE.arc) --pack PACK [--glyphs FILE]

Any Python 3 runs it (the standard library only). The text is read from the player's own copy (--disc: the folder that holds
DATA.BIN; --archive: the disc archive the port makes; --gen: the generated modules the build makes from the copy); nothing is
written but the files named by --to, --pack and --preview, and the text is never part of the port.

The game's text is in two places, and a pack replaces either:
  the disc's files (dialogue tables, quest texts)   list, out, in
  the strings the program draws (menus, help, ...)  strings, table
and the font that draws the pack's letters:         glyphs, font
and the checker runs every rule the game applies to a pack, without the game:  check

list     prints the files `out` reads, with their sizes and the strings in each.
out      writes TO/talk/NNNNN.txt for each dialogue table (DATA.BIN file 0, entries 44 to 56) and TO/quest/NNNNN.txt for each
         quest file (the entries of the quest archive, entry 6586). Each line is KEY<TAB>TEXT; '#' lines are comments.
in       reads the same files back and writes, for each file whose text changed, the game's file into PACK/files/..., in the
         game's own format (the dialogue tables compressed as the disc compresses them), plus PACK/pack.ini when there is none
         (--name). A file that is not changed makes no pack file. The sizes are checked: a rebuilt file may not decompress to
         more than the original (the game's buffers are sized for it).
changes  copies the lines of the files `out` wrote that differ from the game's own to another folder: the translation alone,
         which can be shared (the game's own text is not part of it).
build    makes a pack from shareable sources: SOURCE holds pack.ini, the character map, text.tsv, glyphs.txt and the folders
         talk/ and quest/ of changed lines; the game's own files are taken from the player's copy; the checker runs at the end.
strings  writes the program's strings as a work file, one line each: KEY, SIG, ORIGINAL, TEXT (tabs between). KEY is the first 8
         bytes of the string's SHA-256; SIG its % conversions (or -); ORIGINAL the Japanese; TEXT is empty, for the translator to
         fill in. --gen reads the string literals of the generated modules (the strings with Japanese in them; --all adds the
         strings with English words); --log reads a development build's record of the strings the game drew (MH2_TEXT_LOG).
table    reads the work file and writes PACK/text.tsv (KEY, SIG, TEXT for the lines whose TEXT is filled in), pack.ini when there
         is none (--name) and the character map (--charmap, copied as PACK/charmap.txt). Every entry is checked as the game checks
         it; one fault and nothing is written.
check    reads a pack as the game does and says what it would refuse, with the file and the line; with --disc or --archive it
         also holds the pack's disc files to their limits (the original's size).
glyphs   draws each letter of the character map from a TrueType font in the game's proportions and writes them as a text file of
         pictures the author may edit (PACK/glyphs.txt); --preview writes a picture of them.
font     puts the pictures of glyphs.txt into the game's font file and writes it into the pack (the file holds the game's other
         glyphs, so it is made on the machine that has the game).

Text syntax. The game's text is Shift-JIS (code page 932). A line is the string with these escapes: \\\\ a backslash, \\n a line
feed (the game's line break), \\t, \\r, and \\xNN a byte written as it is (every control byte, and any byte that is not a
character of the code page). The game's own codes (~Cnn colour, ~Ann number, ~Bnn icon, ~% and the % conversions of printf)
are ASCII and stay as they are. A pack's letters (accents) are the single bytes 0xA1 to 0xDF, which the game draws with the
narrow glyphs 0x859F to 0x85DD: the character map (--charmap, one character per line, the first for the byte 0xA1) tells the
tool which character is which byte, in both directions.
"""
import argparse
import os
import struct
import sys

# ---------------------------------------------------------------- the disc

TALK_FIRST, TALK_LAST = 44, 56   # DATA.BIN file 0: the dialogue tables
QUEST_ARCHIVE = 6586             # DATA.BIN file 0: the quest archive (an AFS of the quest files)
QUEST_TEXT_KEYS = ["title", "goal", "suba", "subb", "clear", "fail", "contractor", "desc"]


class Source:
    """The disc's DATA.BIN, from a folder or from the port's disc archive."""

    def __init__(self, disc=None, archive=None):
        self.f = None
        self.base = 0
        if archive:
            self.f = open(archive, "rb")
            h = self.f.read(32)
            if h[:7] != b"MH2DARC":
                raise SystemExit("%s: not a disc archive" % archive)
            n, esz = struct.unpack_from("<II", h, 12)
            for i in range(n):
                e = self.f.read(esz)
                if e[:80].split(b"\0")[0] == b"/DATA.BIN":
                    self.base = struct.unpack_from("<Q", e, 88)[0]
                    self.size = struct.unpack_from("<I", e, 84)[0]
                    break
            else:
                raise SystemExit("%s: no DATA.BIN in the archive" % archive)
        else:
            path = os.path.join(disc, "DATA.BIN")
            if not os.path.isfile(path):
                raise SystemExit("%s: no DATA.BIN there" % disc)
            self.f = open(path, "rb")
            self.size = os.path.getsize(path)
        top = self.toc(0)
        self.o0, self.s0 = top[0]
        self.toc0 = self.toc(self.o0)

    def toc(self, base):
        self.f.seek(self.base + base)
        h = self.f.read(8)
        if h[:3] != b"AFS":
            raise SystemExit("DATA.BIN: not an AFS where one is expected")
        n = struct.unpack("<I", h[4:])[0]
        t = struct.unpack("<%dI" % (2 * n), self.f.read(8 * n))
        return [(t[2 * i], t[2 * i + 1]) for i in range(n)]

    def raw(self, i):
        o, sz = self.toc0[i]
        self.f.seek(self.base + self.o0 + o)
        return self.f.read(sz)

    def quest_files(self):
        arc = self.raw(QUEST_ARCHIVE)
        import io
        toc = []
        h = arc[:8]
        n = struct.unpack("<I", h[4:])[0]
        t = struct.unpack("<%dI" % (2 * n), arc[8:8 + 8 * n])
        return arc, [(t[2 * i], t[2 * i + 1]) for i in range(n)]


# ---------------------------------------------------------------- the game's LZ (src/sub_main/packs.c, LzDecompress)

def lz_decompress(src, limit=1 << 26):
    out = bytearray()
    i = 0
    bit = 0
    flags = 0
    n = len(src)
    while True:
        if i + 2 > n:
            return None
        if bit == 0:
            flags = src[i] | src[i + 1] << 8
            bit = 0x8000
            i += 2
        if i + 2 > n:
            return None
        if flags & bit:
            d = src[i] | src[i + 1] << 8
            c = d >> 11
            i += 2
            if c:
                d &= 0x7FF
            else:
                if i + 2 > n:
                    return None
                c = src[i] | src[i + 1] << 8
                i += 2
            if d == 0:
                if c == 0:
                    return bytes(out)
                out += b"\0\0" * c
            else:
                if d * 2 > len(out):
                    return None
                for _ in range(c):
                    p = len(out) - d * 2
                    out += out[p:p + 2]
        else:
            out += src[i:i + 2]
            i += 2
        bit >>= 1
        if len(out) > limit:
            return None


def lz_compress(data):
    """The same format: 16-bit words, a flag word for each 16 tokens (a set bit is a copy), a copy of 1 to 31 words (or more
    with a second word) from 1 to 2047 words back, a run of zero words, and the end token. Greedy, longest match."""
    if len(data) % 2:
        data += b"\0"
    w = struct.unpack("<%dH" % (len(data) // 2), data)
    n = len(w)
    toks = []   # (is_copy, words)
    pos = {}
    i = 0

    def add(j):
        if j + 1 < n:
            lst = pos.setdefault((w[j], w[j + 1]), [])
            lst.append(j)
            if len(lst) > 48:
                del lst[0]

    while i < n:
        best_len, best_d = 0, 0
        if w[i] == 0:
            k = i
            while k < n and w[k] == 0 and k - i < 0xFFFF:
                k += 1
            best_len, best_d = k - i, 0
        if i + 1 < n:
            for j in reversed(pos.get((w[i], w[i + 1]), ())):
                d = i - j
                if d > 2047:
                    break
                k = 2
                while i + k < n and w[j + k] == w[i + k] and k < 0xFFFF:
                    k += 1
                if k > best_len:
                    best_len, best_d = k, d
        if best_len >= 2 or (best_d == 0 and best_len >= 1):
            if best_len < 32:
                toks.append((1, [(best_len << 11) | best_d]))
            else:
                toks.append((1, [best_d, best_len]))
            for j in range(i, i + best_len):
                add(j)
            i += best_len
        else:
            toks.append((0, [w[i]]))
            add(i)
            i += 1
    toks.append((1, [0, 0]))
    out = bytearray()
    for s in range(0, len(toks), 16):
        group = toks[s:s + 16]
        flags = 0
        for k, (c, _) in enumerate(group):
            if c:
                flags |= 0x8000 >> k
        out += struct.pack("<H", flags)
        for _, ws in group:
            for x in ws:
                out += struct.pack("<H", x)
    return bytes(out)


# ---------------------------------------------------------------- text <-> bytes

class Charmap:
    """The pack's letters for the bytes 0xA1 to 0xDF: one character a line, the first for 0xA1; an empty line is a byte left
    to the half-width katakana."""

    def __init__(self, path=None):
        self.b2c = {}
        self.c2b = {}
        if path:
            with open(path, "rb") as f:
                data = f.read()
            if data[:3] == b"\xef\xbb\xbf":
                data = data[3:]
            lines = data.decode("utf-8").split("\n")
            if lines and lines[-1] == "":
                lines.pop()
            if len(lines) > 63:
                raise SystemExit("%s: %d lines; the map has room for 63 characters (bytes 0xA1 to 0xDF)" % (path, len(lines)))
            for k, line in enumerate(lines):
                line = line.rstrip("\r")
                if not line:
                    continue
                if len(line) != 1:
                    raise SystemExit("%s: line %d is not one character: %r" % (path, k + 1, line))
                if line in self.c2b:
                    raise SystemExit("%s: line %d repeats a character: %r" % (path, k + 1, line))
                self.b2c[0xA1 + k] = line
                self.c2b[line] = 0xA1 + k


def bytes_to_text(b, cm):
    out = []
    i = 0
    n = len(b)
    while i < n:
        c = b[i]
        if c == 0x5C:
            out.append("\\\\"); i += 1
        elif c == 0x0A:
            out.append("\\n"); i += 1
        elif c == 0x09:
            out.append("\\t"); i += 1
        elif c == 0x0D:
            out.append("\\r"); i += 1
        elif c < 0x20 or c == 0x7F:
            out.append("\\x%02X" % c); i += 1
        elif c < 0x7F:
            out.append(chr(c)); i += 1
        elif 0xA1 <= c <= 0xDF:
            if c in cm.b2c:
                out.append(cm.b2c[c])
            else:
                ch = bytes([c]).decode("cp932", "replace")
                out.append(ch if bytes([c]) == ch.encode("cp932", "replace") and ch != "\ufffd" else "\\x%02X" % c)
            i += 1
        elif (0x81 <= c <= 0x9F or 0xE0 <= c <= 0xFC) and i + 1 < n and 0x40 <= b[i + 1] <= 0xFC and b[i + 1] != 0x7F:
            pair = bytes(b[i:i + 2])
            try:
                ch = pair.decode("cp932")
                if len(ch) == 1 and ch.encode("cp932") == pair and ch not in cm.c2b:
                    out.append(ch); i += 2
                    continue
            except (UnicodeDecodeError, UnicodeEncodeError):
                pass
            out.append("\\x%02X" % c); i += 1
        else:
            out.append("\\x%02X" % c); i += 1
    return "".join(out)


def text_to_bytes(t, cm, where):
    out = bytearray()
    i = 0
    while i < len(t):
        ch = t[i]
        if ch == "\\":
            if i + 1 >= len(t):
                raise SystemExit("%s: a backslash ends the line" % where)
            e = t[i + 1]
            if e == "\\": out.append(0x5C); i += 2
            elif e == "n": out.append(0x0A); i += 2
            elif e == "t": out.append(0x09); i += 2
            elif e == "r": out.append(0x0D); i += 2
            elif e == "x":
                h = t[i + 2:i + 4]
                try:
                    out.append(int(h, 16))
                except ValueError:
                    raise SystemExit("%s: \\x needs two hex digits" % where)
                if len(h) != 2:
                    raise SystemExit("%s: \\x needs two hex digits" % where)
                i += 4
            else:
                raise SystemExit("%s: unknown escape \\%s" % (where, e))
            continue
        if ch in cm.c2b:
            out.append(cm.c2b[ch])
        elif ord(ch) < 0x80:
            out.append(ord(ch))
        else:
            try:
                out += ch.encode("cp932")
            except UnicodeEncodeError:
                raise SystemExit("%s: the character %r (U+%04X) is not in the code page and not in the character map" % (where, ch, ord(ch)))
        i += 1
    return bytes(out)


# ---------------------------------------------------------------- the dialogue tables

def talk_parse(b):
    """[(script, [string bytes])] in the file's order; the layout is checked and ValueError says what is wrong."""
    if len(b) < 16:
        raise ValueError("too short")
    first = struct.unpack_from("<i", b, 4)[0]
    if first <= 0 or first % 8 or first > len(b):
        raise ValueError("the first table's offset is not where a record list ends")
    recs = []
    term = False
    for p in range(0, first, 8):
        s, o = struct.unpack_from("<ii", b, p)
        if s == -1 or s == 0xFFFF:
            term = True
            if p + 8 != first:
                raise ValueError("the end record is not the last record")
            continue
        recs.append((s, o))
    if not term:
        raise ValueError("no end record")
    out = []
    pos = first
    for s, o in recs:
        if o != pos:
            raise ValueError("script %d: its table is not where the last one ended (%d, not %d)" % (s, o, pos))
        cnt = struct.unpack_from("<i", b, o)[0]
        if cnt <= 0 or cnt % 4:
            raise ValueError("script %d: a table offset list of %d bytes" % (s, cnt))
        cnt //= 4
        offs = struct.unpack_from("<%di" % cnt, b, o)
        strs = []
        p = o + offs[0]
        for k, off in enumerate(offs):
            if o + off != p:
                raise ValueError("script %d line %d: strings are not in order" % (s, k))
            e = b.index(b"\0", p)
            strs.append(b[p:e])
            p = e + 1
        if b[p:p + 1] != b"\0":
            raise ValueError("script %d: no end byte after the strings" % s)
        pos = p + 1
        out.append((s, strs))
    if len(b) - pos > 3 or any(b[pos:]):
        raise ValueError("more than padding after the last table")
    return out


def talk_build(tables, total_len_hint=None):
    recs = len(tables) + 1
    hdr = recs * 8
    body = bytearray()
    heads = []
    for s, strs in tables:
        heads.append((s, hdr + len(body)))
        tab = 4 * len(strs)
        offs = []
        blob = bytearray()
        for t in strs:
            offs.append(tab + len(blob))
            blob += t + b"\0"
        body += struct.pack("<%di" % len(strs), *offs) + blob + b"\0"
    out = bytearray()
    for s, o in heads:
        out += struct.pack("<ii", s, o)
    out += struct.pack("<ii", -1, -1)
    out += body
    out += b"\0"   # the file ends with one more byte than the tables leave between them, then pads to an even length
    if len(out) % 2:
        out += b"\0"
    return bytes(out)


# ---------------------------------------------------------------- the quest files

def quest_slots(b):
    """The text slots of a quest file: [(key, pointer position, string offset)]. Raises ValueError for a file that is not a quest."""
    if len(b) < 0x74:
        raise ValueError("too short")
    info, script, nt = struct.unpack_from("<III", b, 0)[0], struct.unpack_from("<I", b, 0x10)[0], struct.unpack_from("<I", b, 0x30)[0]
    if info + 0x30 > len(b) or nt + 4 > len(b):
        raise ValueError("not a quest file")
    texts = struct.unpack_from("<I", b, info + 0x24)[0]
    if texts + 32 > len(b):
        raise ValueError("texts out of the file")
    slots = []
    for k in range(8):
        off = struct.unpack_from("<I", b, texts + 4 * k)[0]
        if not 0 < off < len(b) or b.find(b"\0", off) < 0:
            raise ValueError("a text offset out of the file")
        try:
            b[off:b.find(b"\0", off)].decode("cp932")
        except UnicodeDecodeError:
            raise ValueError("a text that is not Shift-JIS")
        slots.append((QUEST_TEXT_KEYS[k], texts + 4 * k, off))
    k = 0
    while nt + 4 * k + 4 <= len(b):
        v = struct.unpack_from("<I", b, nt + 4 * k)[0]
        if v <= nt + 4 * k or v >= len(b) or b.find(b"\0", v) < 0:
            break
        slots.append(("notice%d" % k, nt + 4 * k, v))
        k += 1
    return slots


def quest_strings(b):
    return [(key, b[off:b.find(b"\0", off)]) for key, pp, off in quest_slots(b)]


QUEST_MAX = 32768   # the stage buffer (the lobby clears 32768 bytes)


def quest_patch(b, new):
    """The quest file with the changed strings: in place when the new text fits where the old one was and nothing else points
    there, otherwise at the end of the file."""
    slots = quest_slots(b)
    out = bytearray(b)
    users = {}
    for key, pp, off in slots:
        users[off] = users.get(off, 0) + 1
    for key, pp, off in slots:
        t = new[key]
        old = b[off:b.find(b"\0", off)]
        if t == old:
            continue
        if len(t) <= len(old) and users[off] == 1:
            out[off:off + len(old) + 1] = t + b"\0" * (len(old) - len(t) + 1)
        else:
            struct.pack_into("<I", out, pp, len(out))
            out += t + b"\0"
    return bytes(out)


# ---------------------------------------------------------------- the files of the text tree

def write_lines(path, header, rows):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        for h in header:
            f.write("# " + h + "\n")
        for k, t in rows:
            f.write("%s\t%s\n" % (k, t))


def read_lines(path):
    rows = {}
    order = []
    with open(path, "r", encoding="utf-8", newline="") as f:
        data = f.read()
    if data.startswith("\ufeff"):
        data = data[1:]
    for n, line in enumerate(data.split("\n"), 1):
        line = line.rstrip("\r") if line.endswith("\r") else line
        if not line or line.startswith("#"):
            continue
        if "\t" not in line:
            raise SystemExit("%s:%d: no tab between the key and the text" % (path, n))
        k, t = line.split("\t", 1)
        if k in rows:
            raise SystemExit("%s:%d: the key %s is given twice" % (path, n, k))
        rows[k] = (t, n)
        order.append(k)
    return rows


def talk_keys(tables):
    return [("%d.%d" % (s, k), t) for s, strs in tables for k, t in enumerate(strs)]


def cmd_list(src):
    for i in range(TALK_FIRST, TALK_LAST + 1):
        raw = src.raw(i)
        b = lz_decompress(raw) or raw
        try:
            t = talk_parse(b)
            print("talk %05d: %d bytes (%d on the disc), %d scripts, %d strings" % (i, len(b), len(raw), len(t), sum(len(s) for _, s in t)))
        except ValueError as e:
            print("talk %05d: not a dialogue table (%s)" % (i, e))
    arc, toc = src.quest_files()
    good = 0
    strings = 0
    for i, (o, sz) in enumerate(toc):
        try:
            strings += len(quest_slots(arc[o:o + sz]))
            good += 1
        except ValueError:
            pass
    print("quest archive %d: %d entries, %d quest files, %d strings" % (QUEST_ARCHIVE, len(toc), good, strings))


def cmd_out(src, to, cm):
    nt = ns = 0
    for i in range(TALK_FIRST, TALK_LAST + 1):
        raw = src.raw(i)
        b = lz_decompress(raw) or raw
        tables = talk_parse(b)
        rows = [(k, bytes_to_text(t, cm)) for k, t in talk_keys(tables)]
        write_lines(os.path.join(to, "talk", "%05d.txt" % i),
                    ["Dialogue table %d (DATA.BIN file 0, entry %d). KEY is script.line; KEY<TAB>TEXT." % (i, i),
                     "Edit the text after the tab only. The escapes are listed in tools/pc/textpack.py."], rows)
        nt += 1
        ns += len(rows)
    arc, toc = src.quest_files()
    nq = 0
    for i, (o, sz) in enumerate(toc):
        try:
            strs = quest_strings(arc[o:o + sz])
        except ValueError:
            continue
        write_lines(os.path.join(to, "quest", "%05d.txt" % i),
                    ["Quest file %d (entry %d of the quest archive %d)." % (i, i, QUEST_ARCHIVE),
                     "Keys: title, goal, suba, subb, clear, fail, contractor, desc, then the notices."],
                    [(k, bytes_to_text(t, cm)) for k, t in strs])
        nq += 1
        ns += len(strs)
    print("out: %d dialogue tables and %d quest files, %d strings, in %s" % (nt, nq, ns, to))


def write_pack_file(pack, rel, data):
    path = os.path.join(pack, "files", *rel.split("/"))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def cmd_in(src, frm, pack, name, cm, charmap_file=None):
    ini = os.path.join(pack, "pack.ini")
    os.makedirs(pack, exist_ok=True)
    if not os.path.isfile(ini):
        if not name:
            raise SystemExit("%s has no pack.ini: give --name" % pack)
        with open(ini, "w", encoding="utf-8", newline="\n") as f:
            f.write("name = %s\n" % name)
            if cm.c2b:
                f.write("charmap = charmap.txt\n")
    if cm.c2b:
        if not charmap_file:
            raise SystemExit("in: the character map is in use; give --charmap FILE so that it is copied into the pack")
        import shutil
        import textpack_table as tt
        with open(ini, "rb") as f:
            have, why = tt.parse_packini(f.read(tt.INI_LIMIT + 1))
        dst = os.path.join(pack, have.get("charmap", "charmap.txt"))
        if not (os.path.exists(dst) and os.path.samefile(charmap_file, dst)):
            shutil.copyfile(charmap_file, dst)
    errors = []
    made = same = 0
    for i in range(TALK_FIRST, TALK_LAST + 1):
        path = os.path.join(frm, "talk", "%05d.txt" % i)
        if not os.path.isfile(path):
            continue
        raw = src.raw(i)
        b = lz_decompress(raw) or raw
        tables = talk_parse(b)
        rows = read_lines(path)
        keys = [k for k, _ in talk_keys(tables)]
        for k in rows:
            if k not in keys:
                errors.append("%s:%d: %s is not a string of this table" % (path, rows[k][1], k))
        new = []
        idx = 0
        for s, strs in tables:
            ns_ = []
            for k, t in enumerate(strs):
                key = "%d.%d" % (s, k)
                if key in rows:
                    t2 = text_to_bytes(rows[key][0], cm, "%s:%d" % (path, rows[key][1]))
                else:
                    t2 = t
                ns_.append(t2)
            new.append((s, ns_))
        nb = talk_build(new)
        if nb == b:
            same += 1
            continue
        if len(nb) > len(b):
            errors.append("%s: the table is %d bytes, %d more than the original; the game's buffer is sized for the original" % (path, len(nb), len(nb) - len(b)))
            continue
        comp = lz_compress(nb)
        if lz_decompress(comp) != nb:
            raise SystemExit("internal error: the compressed table %d does not decompress to itself" % i)
        write_pack_file(pack, "DATA.BIN/00000/%05d" % i, comp)
        made += 1
        print("talk %05d: %d bytes (original %d), compressed %d (original %d)" % (i, len(nb), len(b), len(comp), len(raw)))
    arc, toc = src.quest_files()
    for i, (o, sz) in enumerate(toc):
        path = os.path.join(frm, "quest", "%05d.txt" % i)
        if not os.path.isfile(path):
            continue
        b = arc[o:o + sz]
        strs = quest_strings(b)
        rows = read_lines(path)
        keys = [k for k, _ in strs]
        for k in rows:
            if k not in keys:
                errors.append("%s:%d: %s is not a string of this quest" % (path, rows[k][1], k))
        new = {}
        for k, t in strs:
            new[k] = text_to_bytes(rows[k][0], cm, "%s:%d" % (path, rows[k][1])) if k in rows else t
        nb = quest_patch(b, new)
        if nb == b:
            same += 1
            continue
        if len(nb) > QUEST_MAX:
            errors.append("%s: the quest file is %d bytes; the stage buffer holds %d" % (path, len(nb), QUEST_MAX))
            continue
        write_pack_file(pack, "DATA.BIN/00000/%05d/%05d" % (QUEST_ARCHIVE, i), nb)
        made += 1
    if cm.c2b:
        pass
    if errors:
        for e in errors:
            print("error: " + e)
        raise SystemExit(1)
    print("in: %d pack files written, %d files unchanged, in %s" % (made, same, pack))


def cmd_changes(src, frm, to, cm):
    """The lines of the text files in FROM that differ from the game's own, written to TO in the same layout: the translation
    alone, which may be shared (the game's own text is not part of it)."""
    nfiles = nlines = 0

    def keep(path, orig_rows, out_path, head):
        nonlocal nfiles, nlines
        rows = read_lines(path)
        mine = [(k, rows[k][0]) for k in rows if k in orig_rows and rows[k][0] != orig_rows[k]]
        for k in rows:
            if k not in orig_rows:
                raise SystemExit("%s:%d: %s is not a string of this file" % (path, rows[k][1], k))
        if mine:
            write_lines(out_path, head, mine)
            nfiles += 1
            nlines += len(mine)

    for i in range(TALK_FIRST, TALK_LAST + 1):
        path = os.path.join(frm, "talk", "%05d.txt" % i)
        if os.path.isfile(path):
            raw = src.raw(i)
            tables = talk_parse(lz_decompress(raw) or raw)
            keep(path, dict((k, bytes_to_text(t, cm)) for k, t in talk_keys(tables)),
                 os.path.join(to, "talk", "%05d.txt" % i), ["Dialogue table %d: the changed lines only (textpack.py in reads this)." % i])
    arc, toc = src.quest_files()
    for i, (o, sz) in enumerate(toc):
        path = os.path.join(frm, "quest", "%05d.txt" % i)
        if os.path.isfile(path):
            keep(path, dict((k, bytes_to_text(t, cm)) for k, t in quest_strings(arc[o:o + sz])),
                 os.path.join(to, "quest", "%05d.txt" % i), ["Quest file %d: the changed lines only (textpack.py in reads this)." % i])
    print("changes: %d changed lines in %d files, in %s" % (nlines, nfiles, to))


def cmd_build(src, source, pack):
    """The pack from its shareable sources: pack.ini, the character map, text.tsv, glyphs.txt and the changed lines of talk/ and
    quest/ (the files `changes` writes), with the game's own files taken from the player's copy."""
    import shutil
    import textpack_table as tt
    ini = os.path.join(source, "pack.ini")
    if not os.path.isfile(ini):
        raise SystemExit("build: %s has no pack.ini" % source)
    with open(ini, "rb") as f:
        have, why = tt.parse_packini(f.read(tt.INI_LIMIT + 1))
    if why:
        raise SystemExit("build: %s" % why)
    map_name = have.get("charmap", "charmap.txt")
    map_path = os.path.join(source, map_name)
    cm = Charmap(map_path if os.path.isfile(map_path) else None)
    os.makedirs(pack, exist_ok=True)
    for name in ("pack.ini", map_name, "text.tsv", "glyphs.txt"):
        if os.path.isfile(os.path.join(source, name)):
            shutil.copyfile(os.path.join(source, name), os.path.join(pack, name))
    if os.path.isdir(os.path.join(source, "talk")) or os.path.isdir(os.path.join(source, "quest")):
        cmd_in(src, source, pack, None, cm, map_path if cm.c2b else None)
    if os.path.isfile(os.path.join(source, "glyphs.txt")):
        import textpack_font as tf
        tf.cmd_font(src, pack, None)
    tt.cmd_check(pack, src)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("cmd", choices=["out", "in", "list", "changes", "build", "strings", "table", "check", "glyphs", "font"])
    ap.add_argument("--disc")
    ap.add_argument("--archive")
    ap.add_argument("--to")
    ap.add_argument("--from", dest="frm")
    ap.add_argument("--pack")
    ap.add_argument("--name")
    ap.add_argument("--charmap")
    ap.add_argument("--gen", action="append", default=[], help="strings: the generated modules (build/gen)")
    ap.add_argument("--log", action="append", default=[], help="strings: a development log of the strings the game drew")
    ap.add_argument("--all", action="store_true", help="strings: also the strings with no Japanese in them")
    ap.add_argument("--ttf", help="glyphs: the font file to draw the pack's letters from")
    ap.add_argument("--glyphs", help="font: the glyph file (default: PACK/glyphs.txt)")
    ap.add_argument("--weight", type=float, default=1.5, help="glyphs: how heavy the strokes are (the game's are about 1.5)")
    ap.add_argument("--preview", help="glyphs: also write a PGM picture of the glyphs here")
    ap.add_argument("path", nargs="?", help="check: the pack's folder")
    a = ap.parse_args()
    needs_disc = a.cmd in ("out", "in", "list", "changes", "build", "font") or (a.cmd == "check" and (a.disc or a.archive))
    if needs_disc and bool(a.disc) == bool(a.archive):
        raise SystemExit("give --disc DIR or --archive FILE")
    src = Source(a.disc, a.archive) if needs_disc else None
    cm = Charmap(a.charmap)
    if a.cmd == "list":
        cmd_list(src)
    elif a.cmd == "out":
        if not a.to:
            raise SystemExit("out needs --to DIR")
        cmd_out(src, a.to, cm)
    elif a.cmd == "changes":
        if not a.frm or not a.to:
            raise SystemExit("changes needs --from DIR and --to DIR")
        cmd_changes(src, a.frm, a.to, cm)
    elif a.cmd == "build":
        if not a.frm or not a.pack:
            raise SystemExit("build needs --from SOURCE and --pack PACK")
        cmd_build(src, a.frm, a.pack)
    elif a.cmd == "in":
        if not a.frm or not a.pack:
            raise SystemExit("in needs --from DIR and --pack PACK")
        cmd_in(src, a.frm, a.pack, a.name, cm, a.charmap)
    else:
        import textpack_table as tt
        if a.cmd == "strings":
            if not a.to or not (a.gen or a.log):
                raise SystemExit("strings needs --to FILE and --gen DIR or --log FILE")
            tt.cmd_strings(a.gen, a.log, a.to, a.all)
        elif a.cmd == "table":
            if not a.frm or not a.pack:
                raise SystemExit("table needs --from FILE and --pack PACK")
            tt.cmd_table(a.frm, a.pack, a.name, cm, a.charmap)
        elif a.cmd == "check":
            if not a.path:
                raise SystemExit("check needs the pack's folder")
            tt.cmd_check(a.path, src)
        else:
            import textpack_font as tf
            if a.cmd == "glyphs":
                tf.cmd_glyphs(a.ttf, a.charmap, a.to, a.preview, a.weight)
            else:
                tf.cmd_font(src, a.pack, a.glyphs)


if __name__ == "__main__":
    main()
