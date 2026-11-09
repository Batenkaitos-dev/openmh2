"""Read/patch a raw PS2 memory card image (528-byte pages) in place: list files, read a file,
overwrite a file's bytes (same length) and refresh the touched pages' ECC.
Layout as the game's memory card file system keeps it (the project's own code)."""
import struct, sys

PAGE = 512
RAW = 528
FAT_USED = 0x80000000
FAT_END = 0xFFFFFFFF


def _par(b):
    b ^= b >> 4
    b ^= b >> 2
    b ^= b >> 1
    return b & 1


_MASKS = [0x55, 0x33, 0x0F, 0x00, 0xAA, 0xCC, 0xF0]
_COLPAR = []
for _b in range(256):
    _m = 0
    for _i, _mk in enumerate(_MASKS):
        if _par(_b & _mk):
            _m |= 1 << _i
    _COLPAR.append(_m)


def ecc128(d):
    cp, lp0, lp1 = 0x77, 0x7F, 0x7F
    for i in range(128):
        cp ^= _COLPAR[d[i]]
        if _par(d[i]):
            lp0 ^= (~i) & 0xFF
            lp1 ^= i
    return bytes([cp, lp0 & 0x7F, lp1])


def page_ecc(data512):
    return b"".join(ecc128(data512[c * 128:(c + 1) * 128]) for c in range(4)) + b"\0\0\0\0"


class Card:
    def __init__(self, path):
        self.img = bytearray(open(path, "rb").read())
        assert len(self.img) % RAW == 0
        sb = self.img[:PAGE]
        assert sb[:28] == b"Sony PS2 Memory Card Format ", sb[:28]
        self.ppc = struct.unpack_from("<H", sb, 0x2A)[0]
        self.clusters = struct.unpack_from("<I", sb, 0x30)[0]
        self.alloc_offset = struct.unpack_from("<I", sb, 0x34)[0]
        self.alloc_end = struct.unpack_from("<I", sb, 0x38)[0]
        self.ifc = struct.unpack_from("<32I", sb, 0x50)
        self.csize = PAGE * self.ppc
        self.fpc = self.csize // 4

    def check_ecc(self):
        bad = 0
        for p in range(len(self.img) // RAW):
            o = p * RAW
            if page_ecc(bytes(self.img[o:o + PAGE])) != bytes(self.img[o + PAGE:o + RAW]):
                bad += 1
        return bad

    def _rd(self, abs_cl, off, n):
        out = b""
        while n:
            page = abs_cl * self.ppc + off // PAGE
            po = off % PAGE
            k = min(PAGE - po, n)
            o = page * RAW + po
            out += bytes(self.img[o:o + k])
            off += k
            n -= k
        return out

    def _wr(self, abs_cl, off, data, touched):
        n = len(data)
        i = 0
        while i < n:
            page = abs_cl * self.ppc + off // PAGE
            po = off % PAGE
            k = min(PAGE - po, n - i)
            o = page * RAW + po
            self.img[o:o + k] = data[i:i + k]
            touched.add(page)
            off += k
            i += k

    def _r32(self, abs_cl, idx):
        return struct.unpack("<I", self._rd(abs_cl, idx * 4, 4))[0]

    def fat_get(self, rel):
        fpc = self.fpc
        i, j = rel // (fpc * fpc), (rel // fpc) % fpc
        return self._r32(self._r32(self.ifc[i], j), rel % fpc)

    def chain(self, first):
        out = []
        c = first
        while True:
            out.append(c)
            v = self.fat_get(c)
            if v == FAT_END or not (v & FAT_USED):
                break
            c = v & ~FAT_USED
        return out

    def read_chain(self, first, length):
        data = b""
        for c in self.chain(first):
            data += self._rd(self.alloc_offset + c, 0, self.csize)
        return data[:length]

    def dir_entries(self, first):
        """(index, mode, length, cluster, name) of every used entry of a directory."""
        raw = b""
        for c in self.chain(first):
            raw += self._rd(self.alloc_offset + c, 0, self.csize)
        out = []
        n = len(raw) // 1024 * 2
        # one entry per page (512 bytes of data: the entry), pages in cluster order
        for k in range(len(raw) // PAGE):
            e = raw[k * PAGE:(k + 1) * PAGE]
            mode, length = struct.unpack_from("<HxxI", e, 0)[0], struct.unpack_from("<I", e, 4)[0]
            cl = struct.unpack_from("<I", e, 0x10)[0]
            name = e[0x40:0x60].split(b"\0")[0]
            if mode & 0x8000:
                out.append((k, mode, length, cl, name))
        return out

    def find(self, path):
        first = 0
        ent = None
        for comp in path.strip("/").split("/"):
            ent = None
            for (k, mode, length, cl, name) in self.dir_entries(first):
                if name == comp.encode():
                    ent = (k, mode, length, cl, name)
                    break
            if ent is None:
                raise KeyError(path)
            first = ent[3]
        return ent

    def read_file(self, path):
        k, mode, length, cl, name = self.find(path)
        return self.read_chain(cl, length)

    def write_file_same_len(self, path, data):
        k, mode, length, cl, name = self.find(path)
        assert len(data) == length, (len(data), length)
        touched = set()
        chain = self.chain(cl)
        for i, c in enumerate(chain):
            part = data[i * self.csize:(i + 1) * self.csize]
            if part:
                self._wr(self.alloc_offset + c, 0, part, touched)
        for p in touched:
            o = p * RAW
            self.img[o + PAGE:o + RAW] = page_ecc(bytes(self.img[o:o + PAGE]))
        return len(touched)

    def save(self, path):
        open(path, "wb").write(bytes(self.img))


if __name__ == "__main__":
    c = Card(sys.argv[1])
    print("pages with bad ECC:", c.check_ecc())
    for (k, mode, length, cl, name) in c.dir_entries(0):
        print("root", k, hex(mode), length, cl, name)
        if mode & 0x20 and name not in (b".", b".."):
            for (k2, m2, l2, c2, n2) in c.dir_entries(cl):
                print("   ", k2, hex(m2), l2, c2, n2)
