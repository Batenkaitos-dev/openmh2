#!/usr/bin/env python3
"""The pack checker against the core: every broken pack the core refuses is refused by `textpack.py check` too.

    python3 tools/pc/textpack_cases.py WORKDIR [--core MH2-CORE --archive DISC.ARC]

Builds a small good pack and, from it, one pack for each fault the core's loaders know (pack.ini, the character map, text.tsv, a
file that cannot be read), then runs the checker on each. With --core and --archive it runs the core on each pack too (`--pack
DIR --frames 3`; a refusal exits with status 2) and prints both verdicts side by side; a case where the two disagree fails the
run. Nothing of the game is used or written: the packs hold placeholder text.
"""
import argparse
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import textpack_table as tt   # noqa: E402

GOOD_INI = b"name = case\nlanguage = en\nversion = 1\ncharmap = charmap.txt\n"
GOOD_MAP = "\u00e9\n\u00e0\n\u00fc\n\u00f1\n".encode("utf-8")
K = ["%016x" % (0x0123456789abcdef + i) for i in range(20)]
GOOD_TSV = (
    "# placeholder\n"
    + K[0] + "\t-\tHello\n"
    + K[1] + "\t%d%-8s\t%d pieces of %-8s\n"
    + K[2] + "\t-\tcaf\u00e9 \u00e0 la m\u00e9r\n"
    + K[3] + "\t-\t~C05 colour ~~ tilde ~% percent ~A02 ~B07\n"
    + K[4] + "\t-\t\\x82\\xa0 kept\n"
    + K[5] + "\t%5.2d\t%5.2d%% done\\n\n").encode("utf-8")


class Case:
    def __init__(self, name, build, note):
        self.name = name
        self.build = build
        self.note = note


def put(d, rel, data):
    p = os.path.join(d, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "wb") as f:
        f.write(data)


def good(d, ini=GOOD_INI, cmap=GOOD_MAP, tsv=GOOD_TSV):
    os.makedirs(d, exist_ok=True)
    if ini is not None:
        put(d, "pack.ini", ini)
    if cmap is not None:
        put(d, "charmap.txt", cmap)
    if tsv is not None:
        put(d, "text.tsv", tsv)


def with_tsv(extra):
    return lambda d: good(d, tsv=GOOD_TSV + extra)


def with_ini(ini):
    return lambda d: good(d, ini=ini)


def with_map(cmap):
    return lambda d: good(d, cmap=cmap)


def cases():
    L = []

    def add(name, build, note):
        L.append(Case(name, build, note))

    # accepted by both
    add("ok", lambda d: good(d), "a good pack")
    add("ok_bom_crlf", lambda d: good(d, ini=b"\xef\xbb\xbf" + GOOD_INI.replace(b"\n", b"\r\n"),
                                        tsv=b"\xef\xbb\xbf" + GOOD_TSV.replace(b"\n", b"\r\n")), "a byte order mark and CRLF lines")
    add("ok_no_text_table", lambda d: good(d, tsv=None, cmap=None, ini=b"name = files only\n"), "no text.tsv: the pack's files only")
    add("ok_empty_file", lambda d: (good(d), put(d, "files/DATA.BIN/00000/00099", b"")), "an empty file in files/ is ignored")
    # the folder and pack.ini
    add("missing_folder", None, "no such folder")
    add("not_a_folder", lambda d: None, "a file where the folder should be")
    add("no_pack_ini", lambda d: good(d, ini=None), "no pack.ini")
    add("ini_over_16k", lambda d: good(d, ini=GOOD_INI + b"# " + b"x" * 17000 + b"\n"), "pack.ini over 16 KB")
    add("ini_no_name", with_ini(b"language = en\n"), "no name")
    add("ini_key_twice", with_ini(b"name = a\nname = b\n"), "a key twice")
    add("ini_unknown_key", with_ini(b"name = a\ncolour = red\n"), "a key that is not listed")
    add("ini_no_equals", with_ini(b"name = a\nlanguage en\n"), "a line with no =")
    add("ini_empty_value", with_ini(b"name = a\nlanguage =\n"), "an empty value")
    add("ini_control_char", with_ini(b"name = a\x01b\n"), "a control character")
    add("ini_bad_utf8", with_ini(b"name = a\xff\n"), "bytes that are not UTF-8")
    add("ini_nul", with_ini(b"name = a\x00b\n"), "a NUL byte")
    add("ini_name_long", with_ini(b"name = " + b"n" * 65 + b"\n"), "a name of 65 bytes")
    add("ini_language_long", with_ini(b"name = a\nlanguage = " + b"l" * 17 + b"\n"), "a language of 17 bytes")
    add("ini_version_long", with_ini(b"name = a\nversion = " + b"v" * 33 + b"\n"), "a version of 33 bytes")
    add("ini_key_long", with_ini(b"name = a\n" + b"k" * 16 + b" = 1\n"), "a key of 16 bytes")
    add("ini_charmap_path", with_ini(b"name = a\ncharmap = sub/map.txt\n"), "a character map name with a separator")
    add("ini_charmap_dot", with_ini(b"name = a\ncharmap = .map\n"), "a character map name with a leading dot")
    add("ini_charmap_missing", with_ini(b"name = a\ncharmap = nothere.txt\n"), "the named character map is not there")
    # the character map
    add("map_64_lines", with_map("".join(chr(0x100 + i) + "\n" for i in range(64)).encode("utf-8")), "64 lines")
    add("map_two_chars", with_map("ab\n".encode()), "two characters on a line")
    add("map_repeat", with_map("\u00e9\n\u00e9\n".encode("utf-8")), "a character twice")
    add("map_bad_utf8", with_map(b"\xff\n"), "bytes that are not UTF-8")
    add("map_8k", with_map(b"\n" * 8192), "a character map of 8 KB")
    # text.tsv
    add("tsv_short_line", with_tsv(b"0123456789abcdef\n"), "a key and no tabs")
    add("tsv_one_tab", with_tsv((K[9] + "\tonly\n").encode()), "one tab only")
    add("tsv_key_not_hex", with_tsv(b"0123456789abcdeg\t-\tx\n"), "a key with a g")
    add("tsv_key_upper", with_tsv(b"0123456789ABCDEF\t-\tx\n"), "a key in upper case")
    add("tsv_key_short", with_tsv(b"0123456789abcde\t-\tx\n"), "a key of 15 digits")
    add("tsv_key_twice", with_tsv((K[0] + "\t-\tagain\n").encode()), "a key twice")
    add("tsv_bad_escape", with_tsv((K[9] + "\t-\ta\\qb\n").encode()), "an unknown escape")
    add("tsv_ends_backslash", with_tsv((K[9] + "\t-\tab\\\n").encode()), "a backslash at the end")
    add("tsv_hex_upper", with_tsv((K[9] + "\t-\t\\xA0\n").encode()), "\\x with upper-case digits")
    add("tsv_hex_short", with_tsv((K[9] + "\t-\t\\x4\n").encode()), "\\x with one digit")
    add("tsv_x00", with_tsv((K[9] + "\t-\ta\\x00b\n").encode()), "\\x00")
    add("tsv_control", with_tsv((K[9] + "\t-\ta\x01b\n").encode()), "a control character")
    add("tsv_cr_inside", with_tsv((K[9] + "\t-\ta\rb\n").encode()), "a carriage return inside the text")
    add("tsv_bad_utf8", with_tsv((K[9] + "\t-\ta\xffb\n").encode("latin-1")), "bytes that are not UTF-8")
    add("tsv_not_in_map", with_tsv((K[9] + "\t-\t\u00e8\n").encode("utf-8")), "a letter that is not in the character map")
    add("tsv_japanese_utf8", with_tsv((K[9] + "\t-\t\u3042\n").encode("utf-8")), "a Japanese character in UTF-8")
    add("tsv_bad_tilde", with_tsv((K[9] + "\t-\ta ~X\n").encode()), "a ~ that is no code")
    add("tsv_tilde_short", with_tsv((K[9] + "\t-\t~C5\n").encode()), "~C with one digit")
    add("tsv_bad_percent", with_tsv((K[9] + "\t-\t100%!\n").encode()), "a % that is no conversion")
    add("tsv_sig_extra", with_tsv((K[9] + "\t-\t%d items\n").encode()), "a conversion the original lacks")
    add("tsv_sig_missing", with_tsv((K[9] + "\t%d\titems\n").encode()), "a conversion the original has, missing")
    add("tsv_sig_order", with_tsv((K[9] + "\t%d%s\t%s %d\n").encode()), "the conversions in another order")
    add("tsv_sig_width", with_tsv((K[9] + "\t%d\t%3d\n").encode()), "another width than SIG's")
    add("tsv_too_long", with_tsv((K[9] + "\t-\t" + "x" * 1024 + "\n").encode()), "1024 bytes of text")
    add("tsv_too_long_escapes", with_tsv((K[9] + "\t-\t" + "\\x41" * 1024 + "\n").encode()), "1024 bytes written as escapes")
    add("tsv_many_conversions", with_tsv((K[9] + "\t" + "%d" * 100 + "\t" + "%d" * 100 + "\n").encode()), "100 conversions")
    add("tsv_sig_empty_ok", with_tsv((K[9] + "\t\tno SIG at all\n").encode()), "an empty SIG is none (accepted)")
    add("tsv_1023_ok", with_tsv((K[9] + "\t-\t" + "x" * 1023 + "\n").encode()), "1023 bytes (accepted)")

    def dangling(d):
        good(d)
        os.makedirs(os.path.join(d, "files"), exist_ok=True)
        try:
            os.symlink(os.path.join(d, "nothing-there"), os.path.join(d, "files", "UNREADABLE"))
        except OSError:
            put(d, "files/UNREADABLE", b"x")
    add("unreadable_file", dangling, "a file of the pack that cannot be read (a link to nothing)")
    return L


def disc_cases(src):
    """The faults of the game's own files (limits the core does not look at): [(name, build, expected-substring, note)]. src is
    the player's disc."""
    import textpack as tp
    L = []
    t = tp.TALK_FIRST + 6
    orig = src.raw(t)
    odata = tp.lz_decompress(orig) or orig
    tables = tp.talk_parse(odata)

    def talk(d, strs):
        os.makedirs(d, exist_ok=True)
        put(d, "pack.ini", b"name = disc case\n")
        put(d, "files/DATA.BIN/00000/%05d" % t, strs)

    def longer(d):
        tb = [(sc, list(ss)) for sc, ss in tables]
        tb[0][1][0] = tb[0][1][0] + b"x" * 64
        talk(d, tp.lz_compress(tp.talk_build(tb)))

    def shorter(d):
        tb = [(sc, list(ss)) for sc, ss in tables]
        tb[0][1][0] = b"x"
        talk(d, tp.lz_compress(tp.talk_build(tb)))
    arc, toc = src.quest_files()
    q = next(i for i, (o, sz) in enumerate(toc) if sz > 2000)
    qdata = arc[toc[q][0]:toc[q][0] + toc[q][1]]

    def quest(d, data, idx=q):
        os.makedirs(d, exist_ok=True)
        put(d, "pack.ini", b"name = disc case\n")
        put(d, "files/DATA.BIN/00000/%05d/%05d" % (tp.QUEST_ARCHIVE, idx), data)

    def font(d, data):
        os.makedirs(d, exist_ok=True)
        put(d, "pack.ini", b"name = disc case\n")
        put(d, "files/DATA.BIN/00000/05906", data)
    L.append(("disc_talk_same", lambda d: talk(d, orig), None, "the original table, as it is (accepted)"))
    L.append(("disc_talk_shorter", shorter, None, "a table with a shorter string (accepted)"))
    L.append(("disc_talk_longer", longer, "over the limit", "a table longer than the original"))
    L.append(("disc_talk_raw", lambda d: talk(d, odata), "compressed format", "a table that is not compressed"))
    L.append(("disc_talk_junk", lambda d: talk(d, tp.lz_compress(b"\x01" * 4000)), "not a dialogue table", "compressed bytes that are no table"))
    L.append(("disc_quest_same", lambda d: quest(d, qdata), None, "the original quest file (accepted)"))
    L.append(("disc_quest_big", lambda d: quest(d, qdata + b"\0" * (tp.QUEST_MAX - len(qdata) + 1)), "over the limit", "a quest file over the stage buffer"))
    L.append(("disc_quest_limit", lambda d: quest(d, qdata + b"\0" * (tp.QUEST_MAX - len(qdata))), None, "a quest file of exactly the limit (accepted)"))
    L.append(("disc_quest_junk", lambda d: quest(d, b"\x01" * 300), "not a quest file", "bytes that are no quest file"))
    L.append(("disc_quest_entry", lambda d: quest(d, qdata, 9999), "no entry", "an entry the quest archive lacks"))
    font_ok = tp.lz_decompress(src.raw(5906))
    L.append(("disc_font_same", lambda d: font(d, src.raw(5906)), None, "the game's font as it is (accepted)"))
    L.append(("disc_font_short", lambda d: font(d, tp.lz_compress(font_ok[:-100])), "must be", "a font of 7,807 glyphs"))
    L.append(("disc_font_raw", lambda d: font(d, font_ok), "and must be", "a font that is not compressed"))
    return L


def run_core(core, arc, d, work):
    p = subprocess.run([core, "--disc", arc, "--pack", d, "--frames", "3"], cwd=work, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, timeout=300)
    err = p.stderr.decode("utf-8", "replace")
    msg = ""
    for line in err.splitlines():
        if "The translation pack was not loaded: " in line:
            msg = line.split("The translation pack was not loaded: ", 1)[1].split(". Choose another", 1)[0]
            break
    return p.returncode, msg


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("work")
    ap.add_argument("--core")
    ap.add_argument("--archive")
    a = ap.parse_args()
    root = os.path.join(a.work, "cases")
    shutil.rmtree(root, ignore_errors=True)
    os.makedirs(root)
    agree = same = total = 0
    bad = []
    for c in cases():
        d = os.path.join(root, c.name)
        if c.name == "missing_folder":
            pass
        elif c.name == "not_a_folder":
            put(root, c.name, b"name = x\n")
        else:
            c.build(d)
        rep = tt.check_pack(d)
        refused = bool(rep.errors)
        first = rep.errors[0] if refused else ""
        expect_ok = c.name.startswith("ok") or c.name.endswith("_ok")
        if refused == expect_ok:
            bad.append(c.name + " (the checker's verdict is not the case's)")
        line = "%-22s checker %-8s" % (c.name, "REFUSED" if refused else "accepted")
        if a.core:
            rc, msg = run_core(a.core, a.archive, d, root)
            core_refused = rc == 2
            core_ok = rc == 0
            line += " core %-8s" % ("REFUSED" if core_refused else "accepted" if core_ok else "EXIT %d" % rc)
            ok = (refused == core_refused) and (core_refused or core_ok)
            words = refused and core_refused and first.split(" (in ")[0] in msg.split(" (in ")[0]
            line += " %s%s" % ("agree" if ok else "DISAGREE", ", the core's words" if words else "")
            if ok:
                agree += 1
            else:
                bad.append(c.name)
            same += 1 if words else 0
        print(line + "   " + c.note)
        if refused:
            print("    checker: " + first[:200])
        if a.core and core_refused:
            print("    core:    " + msg[:200])
        total += 1
    if a.archive:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import textpack as tp
        src = tp.Source(archive=a.archive)
        dbad = []
        for name, build, want, note in disc_cases(src):
            d = os.path.join(root, name)
            build(d)
            rep = tt.check_pack(d, src)
            ok = (not rep.errors) if want is None else any(want in e for e in rep.errors)
            print("%-22s checker %-8s %s   %s" % (name, "REFUSED" if rep.errors else "accepted", "as expected" if ok else "WRONG", note))
            if rep.errors:
                print("    checker: " + rep.errors[0][:200])
            if not ok:
                dbad.append(name)
            total += 1
        if dbad:
            print("textpack_cases: the disc cases that are wrong: " + ", ".join(dbad))
            bad += dbad
    if a.core:
        print("textpack_cases: %d cases, %d agree, %d with the core's own words, %d disagree%s" % (
            total, agree, same, len(bad), (": " + ", ".join(bad)) if bad else ""))
        return 1 if bad else 0
    print("textpack_cases: %d cases (no core given: the checker only)%s" % (total, (", wrong: " + ", ".join(bad)) if bad else ""))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
