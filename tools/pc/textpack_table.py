"""The program's strings, the text table, and the pack checker (the commands `strings`, `table` and `check` of textpack.py).

The core loads a pack's `text.tsv` and `pack.ini` with the rules of pc/core/io/host_text.c and pc/ipc/packini.h; this file
applies the same rules in Python, so a pack can be checked without the game, and says every fault it finds where the core stops
at the first. The core's own words are used wherever the core has the same rule. The format is in docs/translation-packs.md
(sections 4 and 5).

Standard library only.
"""
import hashlib
import os
import re
import struct

import textpack as tp

TEXT_LIMIT = 1024       # the game's text buffer, with its NUL (TEXT_BUF_SIZE)
INI_LIMIT = 16384       # pack.ini
MAP_LIMIT = 8192        # the character map
FONT_FILE = 5906        # DATA.BIN file 0: the glyph bits of the game's font
FONT_BYTES = 7808 * 100


# ---------------------------------------------------------------- keys, conversions, the core's text rules

def key_of(b):
    """The text table's key of a string (its bytes without the NUL): the first 8 bytes of the SHA-256, 16 hex digits."""
    return hashlib.sha256(b).hexdigest()[:16]


def utf8_one(b, i):
    """The code point and length of the UTF-8 character at b[i], or None (the core's rules: no overlong form, no surrogate)."""
    c = b[i]
    if c < 0x80:
        return c, 1
    n = 2 if 0xC2 <= c <= 0xDF else 3 if 0xE0 <= c <= 0xEF else 4 if 0xF0 <= c <= 0xF4 else 0
    if not n or i + n > len(b):
        return None
    try:
        ch = bytes(b[i:i + n]).decode("utf-8")
    except UnicodeDecodeError:
        return None
    return ord(ch), n


def scan_codes(s, strict_tilde=True):
    """The % conversions of the bytes s written one after the other, and a reason (None when the ~ codes and the conversions are
    well formed). The core's CheckCodes: a two-byte character is skipped as a unit. With strict_tilde false a ~ that is not a code
    is passed over (the way to read an original, which the core does not check)."""
    conv = b""
    n = len(s)
    i = 0

    def at(k):
        return s[k] if k < n else 0

    while i < n:
        c = s[i]
        if 0x81 <= c <= 0x9F and at(i + 1):
            i += 2
            continue
        if c >= 0xE0 and at(i + 1):
            i += 2
            continue
        if c == 0x7E:   # ~
            p1 = at(i + 1)
            if p1 == 0x7E or p1 == 0x25:
                i += 2
            elif p1 in b"ABC" and p1 and 0x30 <= at(i + 2) <= 0x39 and 0x30 <= at(i + 3) <= 0x39:
                i += 4
            elif strict_tilde:
                return conv.decode("ascii"), "a ~ that is not ~~, ~%, ~Cnn, ~Ann or ~Bnn (nn two digits)"
            else:
                i += 1
            continue
        if c == 0x25:   # %
            j = i + 1
            if at(j) == 0x25:
                i = j + 1
                continue
            while j < n and s[j] in b"-+ #0":
                j += 1
            if at(j) == 0x2A:
                j += 1
            else:
                while 0x30 <= at(j) <= 0x39:
                    j += 1
            if at(j) == 0x2E:
                j += 1
                if at(j) == 0x2A:
                    j += 1
                else:
                    while 0x30 <= at(j) <= 0x39:
                        j += 1
            while j < n and s[j] in b"hlLzjt":
                j += 1
            if j >= n or s[j] not in b"diouxXcsp":
                return conv.decode("ascii"), "a % that is not a conversion (diouxXcsp, or %%)"
            if len(conv) + (j - i) + 2 >= 200:
                return conv.decode("ascii"), "too many conversions"
            conv += s[i:j + 1]
            i = j + 1
            continue
        i += 1
    return conv.decode("ascii"), None


def convert(t, cmap):
    """The replacement (a bytes of UTF-8 with escapes) as the game's bytes: (bytes, None), or (None, reason). cmap: byte -> code
    point. The core's Convert."""
    out = bytearray()
    n = len(t)
    i = 0
    rev = {cp: b for b, cp in cmap.items()}
    while i < n:
        c = t[i]
        if c == 0x5C:
            if i + 1 >= n:
                return None, "a backslash ends the text"
            e = t[i + 1]
            if e == 0x5C:
                out.append(0x5C); i += 2
            elif e == ord("n"):
                out.append(0x0A); i += 2
            elif e == ord("t"):
                out.append(0x09); i += 2
            elif e == ord("r"):
                out.append(0x0D); i += 2
            elif e == ord("x") and i + 3 < n and _hex2(t[i + 2:i + 4]) is not None:
                v = _hex2(t[i + 2:i + 4])
                if v == 0:
                    return None, "\\x00 would end the string"
                out.append(v)
                i += 4
            else:
                return None, "an escape that is not \\\\, \\n, \\t, \\r or \\xNN (two lower-case hex digits)"
            continue
        if c < 0x80:
            if c < 0x20 and c != 0x09:
                return None, "a control character (write \\n or \\xNN)"
            out.append(c)
            i += 1
            continue
        u = utf8_one(t, i)
        if not u:
            return None, "bytes that are not UTF-8"
        cp, ln = u
        if cp not in rev:
            return None, ("the character U+%04X is not in the character map (a Japanese character is written as its Shift-JIS bytes, \\xNN)" % cp)
        out.append(rev[cp])
        i += ln
    return bytes(out), None


def _hex2(h):
    """Two lower-case hex digits as a number, or None (the core accepts 0-9 and a-f)."""
    if len(h) != 2 or any(ch not in b"0123456789abcdef" for ch in h):
        return None
    return int(h, 16)


def check_entry(l, cmap, seen):
    """One line of text.tsv (bytes, without the line end) as the core reads it: (key, converted bytes, None) or (None, None,
    reason). seen: the keys accepted so far (a set)."""
    n = len(l)
    tab2 = l.find(b"\t", 17) if n > 17 and l[16:17] == b"\t" else -1
    if tab2 < 0:
        return None, None, "the line is not KEY (16 hex digits), a tab, SIG, a tab, and the text"
    k = l[:16]
    if any(ch not in b"0123456789abcdef" for ch in k):
        return None, None, "the key is not 16 lower-case hex digits"
    key = k.decode("ascii")
    if key in seen:
        return None, None, "the key is given twice"
    txt, why = convert(l[tab2 + 1:], cmap)
    if txt is None:
        return None, None, why
    conv, bad = scan_codes(txt)
    sig = l[17:tab2]
    if bad:
        return None, None, bad
    sig_ok = (conv == "") if sig == b"-" else (conv.encode("ascii") == sig)
    if not sig_ok:
        return None, None, "the replacement's conversions are \"%s\", and the original's (SIG) are not the same" % conv
    if len(txt) >= TEXT_LIMIT:
        return None, None, "the text is %d bytes; the game's text buffer holds %d" % (len(txt), TEXT_LIMIT - 1)
    return key, txt, None


def split_lines(data):
    """The lines of a file as the core splits them: LF ends a line, one CR before it is dropped; a byte order mark at the start
    is skipped. [(number, bytes)]"""
    p = 3 if data[:3] == b"\xef\xbb\xbf" else 0
    out = []
    num = 0
    n = len(data)
    while p < n:
        e = data.find(b"\n", p)
        if e < 0:
            e = n
        t = e
        if t > p and data[t - 1] == 0x0D:
            t -= 1
        num += 1
        out.append((num, data[p:t]))
        p = e + 1
    return out


# ---------------------------------------------------------------- pack.ini and the character map (packini.h, host_text.c)

def _ws(c):
    return c in b" \t"


def parse_packini(data):
    """pack.ini as the core reads it: (settings dict, None) or (partial dict, the sentence)."""
    out = {}
    seen = set()
    keys = {"name": 64, "language": 16, "version": 32, "charmap": 64}
    for line, raw in split_lines(data):
        if b"\0" in raw:
            return out, "pack.ini, line %d: a NUL byte" % line
        b = raw
        s = 0
        e = len(b)
        while s < e and _ws(b[s]):
            s += 1
        while e > s and _ws(b[e - 1]):
            e -= 1
        if s == e or b[s] in b"#;":
            continue
        eq = b.find(b"=", s, e)
        if eq < 0:
            return out, "pack.ini, line %d: no = (a setting is key = value)" % line
        kb = b[s:eq].rstrip(b" \t")
        v = b[eq + 1:e].lstrip(b" \t")
        if not kb or len(kb) >= 16:
            return out, "pack.ini, line %d: the key is empty or too long" % line
        key = kb.decode("latin-1")
        if key not in keys:
            return out, "pack.ini, line %d: %s is not a key of a pack.ini (name, language, version, charmap)" % (line, key)
        if key in seen:
            return out, "pack.ini, line %d: %s is given twice" % (line, key)
        seen.add(key)
        if not v:
            return out, "pack.ini, line %d: %s has no value" % (line, key)
        if len(v) > keys[key]:
            return out, "pack.ini, line %d: %s is longer than %d bytes" % (line, key, keys[key])
        if any(c < 0x20 and c != 0x09 for c in v):
            return out, "pack.ini, line %d: %s holds a control character" % (line, key)
        try:
            v.decode("utf-8")
        except UnicodeDecodeError:
            return out, "pack.ini, line %d: %s is not UTF-8" % (line, key)
        if key == "charmap" and (v[:1] == b"." or any(c in v for c in (b"/", b"\\", b":"))):
            return out, "pack.ini, line %d: charmap is a file name in the pack's folder (no folder, no leading dot)" % line
        out[key] = v.decode("utf-8")
    if "name" not in seen:
        return out, "pack.ini has no name"
    return out, None


def parse_charmap(data, label):
    """The pack's character map: (byte -> code point, [errors]). The core's LoadMap, which stops at its first fault."""
    cmap = {}
    errs = []
    if len(data) >= MAP_LIMIT:
        return cmap, ["the character map %s is over 8 KB" % label]
    for line, raw in split_lines(data):
        if line > 63:
            errs.append("the character map %s has more than 63 lines (the bytes 0xA1 to 0xDF)" % label)
            break
        if not raw:
            continue
        u = utf8_one(raw, 0)
        if not u or u[1] != len(raw):
            errs.append("the character map %s, line %d: one UTF-8 character a line" % (label, line))
            continue
        cp = u[0]
        dup = [b for b, c in cmap.items() if c == cp]
        if dup:
            errs.append("the character map %s, line %d: the character is already on line %d" % (label, line, dup[0] - 0xA0))
            continue
        cmap[0xA0 + line] = cp
    return cmap, errs


# ---------------------------------------------------------------- the checker

class Report:
    def __init__(self):
        self.errors = []
        self.warnings = []
        self.entries = 0
        self.files = 0

    def err(self, msg):
        self.errors.append(msg)

    def warn(self, msg):
        self.warnings.append(msg)


def check_text_table(path, label, cmap, rep, maxerr=50):
    """text.tsv: every line the core checks, each fault reported. Returns the number of entries accepted."""
    with open(path, "rb") as f:
        data = f.read()
    seen = set()
    bad = 0
    for num, raw in split_lines(data):
        if not raw or raw[:1] == b"#":
            continue
        key, txt, why = check_entry(raw, cmap, seen)
        if why:
            bad += 1
            if bad <= maxerr:
                rep.err("%s, line %d: %s" % (label, num, why))
        else:
            seen.add(key)
    if bad > maxerr:
        rep.err("%s: and %d more lines with faults" % (label, bad - maxerr))
    return len(seen)


def _walk_files(root):
    out = []
    for dp, dn, fn in os.walk(root):
        dn.sort()
        for f in sorted(fn):
            full = os.path.join(dp, f)
            out.append((os.path.relpath(full, root).replace(os.sep, "/"), full))
    return out


TALK_RE = re.compile(r"^DATA\.BIN/00000/(\d{5})$")
QUEST_RE = re.compile(r"^DATA\.BIN/00000/%05d/(\d{5})$" % tp.QUEST_ARCHIVE)


def check_disc_file(rel, full, rep, src):
    """A file of files/: the game's formats and limits (the formats are in docs/translation-packs.md). src: the disc, or None."""
    name = rel
    try:
        with open(full, "rb") as f:
            data = f.read()
    except OSError:
        rep.err("files/%s can't be read" % name)
        return
    if not data:
        rep.warn("files/%s is empty: the core ignores an empty file in a pack" % name)
        return
    m = TALK_RE.match(name)
    q = QUEST_RE.match(name)
    if m and tp.TALK_FIRST <= int(m.group(1)) <= tp.TALK_LAST:
        idx = int(m.group(1))
        b = tp.lz_decompress(data)
        if b is None:
            rep.err("files/%s: not in the game's compressed format (the dialogue tables are compressed; textpack.py in writes them)" % name)
            return
        try:
            tp.talk_parse(b)
        except ValueError as e:
            rep.err("files/%s: not a dialogue table: %s" % (name, e))
            return
        if src:
            orig = src.raw(idx)
            ob = tp.lz_decompress(orig) or orig
            if len(b) > len(ob):
                rep.err("files/%s: the table is %d bytes, %d over the limit of %d (the original's size)" % (name, len(b), len(b) - len(ob), len(ob)))
            if len(data) > len(orig):
                rep.warn("files/%s: %d bytes on the disc's format, %d more than the original's %d" % (name, len(data), len(data) - len(orig), len(orig)))
        else:
            rep.warn("files/%s: the table is %d bytes; its limit, the original's size, is not checked without --disc or --archive" % (name, len(b)))
    elif q:
        idx = int(q.group(1))
        try:
            tp.quest_slots(data)
        except (ValueError, struct.error) as e:
            rep.err("files/%s: not a quest file: %s" % (name, e))
            return
        if len(data) > tp.QUEST_MAX:
            rep.err("files/%s: the quest file is %d bytes, %d over the limit of %d (the stage buffer)" % (name, len(data), len(data) - tp.QUEST_MAX, tp.QUEST_MAX))
        if src:
            arc, toc = src.quest_files()
            if idx >= len(toc):
                rep.err("files/%s: the quest archive has no entry %d" % (name, idx))
    elif name == "DATA.BIN/00000/%05d" % FONT_FILE:
        b = tp.lz_decompress(data)
        if b is None:
            rep.err("files/%s: not in the game's compressed format" % name)
        elif len(b) != FONT_BYTES:
            rep.err("files/%s: the font is %d bytes, and must be %d (7,808 glyphs of 100 bytes)" % (name, len(b), FONT_BYTES))
    else:
        rep.warn("files/%s: the checker has no rule for this file; the game reads it in place of the disc's file of that name" % name)
    if name != name.upper():
        rep.warn("files/%s: the game asks for upper-case names (%s); on Linux the pack's file name must be upper case" % (name, name.upper()))
    if len(name) + 1 >= 96:
        rep.warn("files/%s: a name of 95 characters or more is never found" % name)


def check_pack(path, src=None):
    """Every rule the core applies to a pack, without the game. Returns a Report."""
    rep = Report()
    if not os.path.exists(path):
        rep.err("the folder %s doesn't exist or can't be read" % path)
        return rep
    if not os.path.isdir(path):
        rep.err("the folder %s doesn't exist or can't be read" % path)
        return rep
    ini = os.path.join(path, "pack.ini")
    try:
        with open(ini, "rb") as f:
            idata = f.read(INI_LIMIT + 2)
    except OSError:
        rep.err("the folder %s has no pack.ini that can be read (a text pack holds one)" % path)
        return rep
    if len(idata) > INI_LIMIT:
        rep.err("the pack.ini of %s is over 16 KB" % path)
        return rep
    settings, why = parse_packini(idata)
    if why:
        rep.err("%s (in %s)" % (why, path))
        return rep
    rep.files += 1
    charmap_name = settings.get("charmap")
    cmap = {}
    map_path = os.path.join(path, charmap_name or "charmap.txt")
    have_map = os.path.isfile(map_path)
    if charmap_name and not have_map:
        rep.err("the character map %s that pack.ini names is not in %s" % (charmap_name, path))
    tsv = os.path.join(path, "text.tsv")
    have_tsv = os.path.isfile(tsv)
    map_bad = False
    if have_map:
        rep.files += 1
        with open(map_path, "rb") as f:
            cmap, errs = parse_charmap(f.read(MAP_LIMIT + 1), map_path)
        for e in errs:
            (rep.err if have_tsv else rep.warn)(e + ("" if have_tsv else " (not used: the pack has no text.tsv)"))
        if errs:
            cmap = {}
            map_bad = True
    if have_tsv:
        rep.files += 1
        if map_bad:
            rep.warn("text.tsv is not checked until the character map is right")
        else:
            rep.entries = check_text_table(tsv, "text.tsv", cmap, rep)
    for rel, full in _walk_files(path):
        try:
            with open(full, "rb") as probe:
                probe.read(1)
        except OSError:
            rep.err("the file %s can't be read" % rel)
            continue
        if rel in ("pack.ini", "text.tsv", "glyphs.txt", charmap_name or "charmap.txt"):
            continue
        if rel.startswith("files/"):
            rep.files += 1
            check_disc_file(rel[6:], full, rep, src)
        else:
            rep.warn("%s is not read by the game (a pack's files are pack.ini, text.tsv, the character map and files/...); it is part of the pack's hash" % rel)
    glyphs = os.path.join(path, "glyphs.txt")
    font = os.path.join(path, "files", "DATA.BIN", "00000", "%05d" % FONT_FILE)
    if os.path.isfile(glyphs):
        rep.files += 1
        have = set()
        with open(glyphs, "r", encoding="utf-8", errors="replace") as f:
            for num, ln in enumerate(f.read().splitlines(), 1):
                if ln.startswith("glyph 0x"):
                    try:
                        have.add(int(ln.split(" ")[1], 16))
                    except ValueError:
                        rep.err("glyphs.txt, line %d: %s is not a byte" % (num, ln.split(" ")[1]))
        for b in sorted(cmap):
            if b not in have:
                rep.warn("glyphs.txt has no glyph for the letter %s (byte 0x%02X): the game draws the katakana there" % (chr(cmap[b]), b))
        if not os.path.isfile(font):
            rep.warn("glyphs.txt is not in the game's font yet: textpack.py font makes files/DATA.BIN/00000/%05d" % FONT_FILE)
    elif cmap and not os.path.isfile(font):
        rep.warn("the character map has letters but the pack has no font file (files/DATA.BIN/00000/%05d): the game draws the katakana for them; textpack.py glyphs and font make it" % FONT_FILE)
    return rep


def cmd_check(path, src):
    rep = check_pack(path, src)
    for w in rep.warnings:
        print("warning: " + w)
    for e in rep.errors:
        print("error: " + e)
    if rep.errors:
        print("check: %s is refused: %d errors, %d warnings" % (path, len(rep.errors), len(rep.warnings)))
        raise SystemExit(1)
    print("check: %s is a pack the game accepts: %d text entries, %d files, %d warnings" % (path, rep.entries, rep.files, len(rep.warnings)))


# ---------------------------------------------------------------- the program's strings

_TOK = re.compile(r'/\*.*?\*/|//[^\n]*|\'(?:\\.|[^\'\\\n])*\'|"(?:\\.|[^"\\\n])*"', re.S)
_ESC = {"n": 10, "t": 9, "r": 13, "a": 7, "b": 8, "f": 12, "v": 11, "\\": 92, '"': 34, "'": 39, "?": 63}


def _unescape(body):
    out = bytearray()
    i = 0
    n = len(body)
    while i < n:
        c = body[i]
        if c != "\\":
            out.append(ord(c) & 0xFF)
            i += 1
            continue
        i += 1
        e = body[i]
        if e in "01234567":
            j = i
            while j < n and j < i + 3 and body[j] in "01234567":
                j += 1
            out.append(int(body[i:j], 8) & 0xFF)
            i = j
        elif e == "x":
            j = i + 1
            while j < n and body[j] in "0123456789abcdefABCDEF":
                j += 1
            out.append(int(body[i + 1:j], 16) & 0xFF)
            i = j
        else:
            out.append(_ESC.get(e, ord(e)))
            i += 1
    return bytes(out)


def c_strings(text):
    """The string literals of a C text, adjacent ones joined, as bytes up to the first NUL."""
    res = []
    pending = None
    last = 0
    for m in _TOK.finditer(text):
        tok = m.group(0)
        gap = text[last:m.start()]
        last = m.end()
        if tok[0] == "/":
            continue
        if tok[0] == "'":
            if pending is not None:
                res.append(pending)
                pending = None
            continue
        body = _unescape(tok[1:-1])
        if pending is not None and not gap.strip():
            pending += body
        else:
            if pending is not None:
                res.append(pending)
            pending = body
    if pending is not None:
        res.append(pending)
    return [r.split(b"\0", 1)[0] for r in res]


def is_text(b, everything):
    """A string worth showing to a translator: it decodes as Shift-JIS from end to end with no control byte but the line feed,
    and has a Japanese character in it (not only one or two half-width katakana, which are most often a table's data) or, with
    everything, a run of two ASCII letters."""
    if any(c < 0x20 and c != 0x0A for c in b) or any(c in (0x7F, 0x80, 0xA0, 0xFD, 0xFE, 0xFF) for c in b):
        return False
    try:
        t = b.decode("cp932")
    except UnicodeDecodeError:
        return False
    jp = [c for c in t if ord(c) >= 0x3000 or 0xFF61 <= ord(c) <= 0xFF9F]
    if jp:
        return not (len(t) < 3 and all(0xFF61 <= ord(c) <= 0xFF9F for c in t))
    return bool(everything and re.search(rb"[A-Za-z]{2}", b))


def read_log(path):
    """The development log (MH2_TEXT_LOG): [(key, bytes)]."""
    out = []
    with open(path, "r", encoding="ascii", errors="replace", newline="") as f:
        for num, line in enumerate(f.read().split("\n"), 1):
            line = line.rstrip("\r")
            if not line:
                continue
            k, _, t = line.partition("\t")
            if len(k) != 16 or not _:
                raise SystemExit("%s:%d: not a line of the text log (KEY, a tab, the string)" % (path, num))
            out.append((k, tp.text_to_bytes(t, tp.Charmap(), "%s:%d" % (path, num))))
    return out


def gen_strings(gen):
    """The string literals of the generated modules (build/gen): [(file, bytes)] in the files' order."""
    out = []
    for dp, dn, fn in os.walk(gen):
        dn.sort()
        for f in sorted(fn):
            if not f.endswith((".inc", ".h")):
                continue
            full = os.path.join(dp, f)
            with open(full, "r", encoding="latin-1", newline="") as fh:
                txt = fh.read()
            if '"' not in txt:
                continue
            rel = os.path.relpath(full, gen).replace(os.sep, "/")
            for s in c_strings(txt):
                out.append((rel, s))
    return out


def original_sig(b):
    """The SIG column for an original: its conversions (or -), or None when its % cannot be read."""
    conv, bad = scan_codes(b, strict_tilde=False)
    if bad:
        return None
    return conv if conv else "-"


def cmd_strings(gen, logs, to, everything):
    rows = []
    seen = set()
    skipped = 0
    for lg in logs:
        for k, b in read_log(lg):
            if k != key_of(b):
                raise SystemExit("%s: the key %s is not the SHA-256 of its string" % (lg, k))
            if k not in seen and b:
                seen.add(k)
                rows.append(("log", k, b))
    for gdir in gen:
        for rel, b in gen_strings(gdir):
            if not b or not is_text(b, everything):
                continue
            k = key_of(b)
            if k not in seen:
                seen.add(k)
                rows.append((rel, k, b))
    os.makedirs(os.path.dirname(os.path.abspath(to)), exist_ok=True)
    cm = tp.Charmap()
    written = 0
    cur = None
    left = []
    with open(to, "w", encoding="utf-8", newline="\n") as f:
        f.write("# The program's strings. KEY, SIG, ORIGINAL, TEXT, separated by tabs.\n"
                "# Write the translation in TEXT (the fourth column) of the strings you translate and leave the others empty;\n"
                "# do not change the first three columns. TEXT must have the same % conversions as SIG, in the same order.\n"
                "# Escapes: \\\\ a backslash, \\n a line feed, \\xNN a byte. Then: textpack.py table.\n")
        for rel, k, b in rows:
            sig = original_sig(b)
            if sig is None:
                skipped += 1
                left.append("# left out: %s\t%s\n" % (k, tp.bytes_to_text(b, cm)))
                continue
            if rel != cur:
                f.write("# %s\n" % rel)
                cur = rel
            f.write("%s\t%s\t%s\t\n" % (k, sig, tp.bytes_to_text(b, cm)))
            written += 1
        if left:
            f.write("# These strings have a % that the game's table cannot take (a float, or a code of the game's own), so a pack cannot replace them:\n")
            f.writelines(left)
    print("strings: %d strings in %s (%d left out: a %% that is not a conversion)" % (written, to, skipped))


def cmd_table(frm, pack, name, cm, charmap_file):
    """strings.tsv (the translator's work file) to the pack's text.tsv, every entry checked."""
    with open(frm, "rb") as f:
        data = f.read()
    cmap = {b: ord(c) for b, c in cm.b2c.items()}
    rows = []
    errors = []
    seen = set()
    translated = same = total = 0
    for num, raw in split_lines(data):
        if not raw or raw[:1] == b"#":
            continue
        cols = raw.decode("utf-8", "replace").split("\t")
        if len(cols) != 4:
            errors.append("%s:%d: the line is not KEY, SIG, ORIGINAL and TEXT with a tab between them" % (frm, num))
            continue
        k, sig, orig, text = cols
        total += 1
        if k in seen:
            errors.append("%s:%d: the key %s is given twice" % (frm, num, k))
            continue
        seen.add(k)
        if text == "":
            continue
        try:
            ob = tp.text_to_bytes(orig, tp.Charmap(), "%s:%d" % (frm, num))
            nb = tp.text_to_bytes(text, cm, "%s:%d" % (frm, num))
        except SystemExit as e:
            errors.append(str(e))
            continue
        if key_of(ob) != k:
            errors.append("%s:%d: the key is not that of the original (the first three columns were changed)" % (frm, num))
            continue
        if nb == ob:
            same += 1
            continue
        want = original_sig(ob)
        if want is None:
            errors.append("%s:%d: the original has a %% that is not a conversion; it can't be replaced" % (frm, num))
            continue
        line = (k + "\t" + want + "\t").encode("ascii") + tsv_text(nb, cm).encode("utf-8")
        key, txt, why = check_entry(line, cmap, set())
        if why:
            errors.append("%s:%d: %s" % (frm, num, why))
            continue
        rows.append(line)
        translated += 1
    if errors:
        for e in errors[:100]:
            print("error: " + e)
        if len(errors) > 100:
            print("error: and %d more" % (len(errors) - 100))
        raise SystemExit("table: nothing written; %d faults in %s" % (len(errors), frm))
    if cm.c2b and not charmap_file:
        raise SystemExit("table: the character map is in use; give --charmap FILE so that it is copied into the pack")
    os.makedirs(pack, exist_ok=True)
    ini = os.path.join(pack, "pack.ini")
    map_name = "charmap.txt"
    if not os.path.isfile(ini):
        if not name:
            raise SystemExit("%s has no pack.ini: give --name" % pack)
        with open(ini, "w", encoding="utf-8", newline="\n") as f:
            f.write("name = %s\n" % name)
            if charmap_file:
                f.write("charmap = %s\n" % map_name)
    else:
        with open(ini, "rb") as f:
            have, why = parse_packini(f.read(INI_LIMIT + 1))
        if why:
            raise SystemExit("table: %s: %s" % (ini, why))
        map_name = have.get("charmap", map_name)
    if charmap_file:
        import shutil
        dst = os.path.join(pack, map_name)
        if not (os.path.exists(dst) and os.path.samefile(charmap_file, dst)):
            shutil.copyfile(charmap_file, dst)
    out = os.path.join(pack, "text.tsv")
    with open(out, "wb") as f:
        f.write(b"# text.tsv: KEY, SIG, TEXT (made by textpack.py table)\n")
        for r in rows:
            f.write(r + b"\n")
    print("table: %d entries in %s (%d strings left as they are, %d with a text equal to the original)" % (translated, out, total - translated - same, same))


def tsv_text(b, cm):
    """The game's bytes as the text column of text.tsv: ASCII and the pack's letters as they are, every other byte over 0x7F as
    \\xNN, the escapes."""
    out = []
    for c in b:
        if c == 0x5C:
            out.append("\\\\")
        elif c == 0x0A:
            out.append("\\n")
        elif c == 0x09:
            out.append("\\t")
        elif c == 0x0D:
            out.append("\\r")
        elif c < 0x20 or c == 0x7F:
            out.append("\\x%02x" % c)
        elif c < 0x7F:
            out.append(chr(c))
        elif c in cm.b2c:
            out.append(cm.b2c[c])
        else:
            out.append("\\x%02x" % c)
    return "".join(out)
