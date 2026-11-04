#!/usr/bin/env python3
"""Converts a UTF-8 C source's non-ASCII text to the game's Shift-JIS bytes, as octal escapes.

    python3 tools/pc/sjis_c.py IN.c OUT.c

The matching build converts a source to Shift-JIS before MWCC reads it
(tools/mwcc_cc.py), so a string literal's bytes are the game's. clang 18
reads UTF-8 and has no execution-charset switch for Shift-JIS, and a raw
Shift-JIS byte pair whose second byte is 0x5C would read as an escape. So
every non-ASCII character becomes its Shift-JIS bytes as octal escapes (three
digits, so a digit that follows cannot extend one): the same bytes in the
literal, whatever follows. Only one generated file holds such text today
(build/assets/lbguild/lbguild.c). A file with none is copied unchanged.
"""
import sys
from pathlib import Path


def convert(text: str) -> str:
    out = []
    for ch in text:
        if ord(ch) < 0x80:
            out.append(ch)
        else:
            out.append("".join(chr(92) + ("%03o" % b) for b in ch.encode("shift_jis")))
    return "".join(out)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src = Path(sys.argv[1]).read_bytes()
    try:
        text = src.decode("utf-8")
    except UnicodeDecodeError as e:
        sys.exit(f"sjis_c.py: {sys.argv[1]}: not UTF-8: {e}")
    Path(sys.argv[2]).write_bytes(convert(text).encode("ascii"))


if __name__ == "__main__":
    main()
