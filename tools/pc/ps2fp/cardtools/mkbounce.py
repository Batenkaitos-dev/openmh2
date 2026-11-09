"""Builds the bounce route's card (inj-bounce): the late card (SRC) with the equipped weapon replaced by one of its class whose
sharpness gauge tops out in blue (level 4: a gauge of 350 against the row's 330..390), the box entry it was equipped
from and the equip copy both rewritten, the weapon's owned flag set (the game's own encryption, the head kept, the sum
recomputed, the page ECC through mccard).
Usage: mkbounce.py SRC.ps2 DST.ps2 [WEAPON_ID]   (SRC is only read; the default weapon, 391, is a plain blade of the
late card's weapon class: a gauge of 350 against the row's 330..370; a game card, so out of git)
mccard.py (the memory card image) and savelib.py (the save's encryption) are beside this file."""
import hashlib
import shutil
import struct
import sys

import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from savelib import *   # noqa: E402,F401

DIR = "BISLPM-66280MH2/"
BLADE_FLAGS, BOXOFF, EQUIP, EQUIPIDX = 0x3318, 0xBC, 0x2090, 0x2186


def main(src, dst, wid):
    shutil.copyfile(src, dst)
    c = Card(dst)
    n = "play00.bin"
    d = c.read_file(DIR + n)
    plain, ver, key, sm, s = dec(d)
    assert sm == s, "the source's sum is bad"
    body = bytearray(plain[BODY:BODY + 0x37F8])
    assert body[0] != 0
    old = struct.unpack_from("<BBHH3H", body, EQUIP)
    idx = struct.unpack_from("<H", body, EQUIPIDX)[0]
    print("equipped weapon", old[:4], "box slot", idx)
    assert old[1] == 6 and idx != 0xFFFF
    assert tuple(body[BOXOFF + 12 * idx:BOXOFF + 12 * idx + 12]) == tuple(body[EQUIP:EQUIP + 12]), "box and equip differ"
    new = struct.pack("<BBHH3H", 1, 6, wid, 0, 0, 0, 0)
    body[BOXOFF + 12 * idx:BOXOFF + 12 * idx + 12] = new
    body[EQUIP:EQUIP + 12] = new
    body[BLADE_FLAGS + wid] |= 1
    newplain = plain[:BODY] + bytes(body) + plain[BODY + 0x37F8:]
    newfile = enc_same_head(newplain)
    p2, v2, k2, sm2, s2 = dec(newfile)
    assert (v2, k2) == (ver, key) and sm2 == s2 and p2[BODY:BODY + 0x37F8] == bytes(body)
    assert newfile[:4] == d[:4] and newfile[6:0x40] == d[6:0x40]
    print("pages touched", c.write_file_same_len(DIR + n, newfile))
    c.save(dst)
    print("ECC bad pages (the erased backup block's 16 expected):", Card(dst).check_ecc())
    print("dst sha256", hashlib.sha256(open(dst, "rb").read()).hexdigest())


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 391)
