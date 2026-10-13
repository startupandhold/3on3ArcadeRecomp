#!/usr/bin/env python3
"""Cross-references for the extracted game image (reference/xex/nhlzf.exe).

Finds strings in the image and the functions whose `lis`/`addi`-style
instruction pairs build their addresses.

  xex_xrefs.py str  TEXT...   strings containing TEXT and who references them
  xex_xrefs.py refs ADDR...   functions referencing an address (hex)
  xex_xrefs.py func ADDR...   strings referenced by the function containing ADDR
"""

import bisect
import os
import re
import struct
import sys

IMAGE = os.path.join(os.path.dirname(__file__), "..", "reference", "xex", "nhlzf.exe")


class Image:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        d = self.data
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        nsec = struct.unpack_from("<H", d, pe + 6)[0]
        optsz = struct.unpack_from("<H", d, pe + 20)[0]
        self.base = struct.unpack_from("<I", d, pe + 24 + 28)[0]
        self.sections = []
        for i in range(nsec):
            s = pe + 24 + optsz + 40 * i
            name = d[s:s + 8].rstrip(b"\0").decode()
            vsize, va, rsize, raw = struct.unpack_from("<IIII", d, s + 8)
            chars = struct.unpack_from("<I", d, s + 36)[0]
            self.sections.append((name, self.base + va, vsize, raw, rsize, chars))
        self._functions()
        self._xrefs()

    def read(self, address, size):
        for _, va, vsize, raw, rsize, _ in self.sections:
            if va <= address < va + min(vsize, rsize):
                off = raw + address - va
                return self.data[off:off + size]
        return None

    def _functions(self):
        sec = next(s for s in self.sections if s[0] == ".pdata")
        _, va, vsize, raw, _, _ = sec
        starts, ends = [], []
        for off in range(raw, raw + vsize, 8):
            begin, info = struct.unpack_from(">II", self.data, off)
            if begin == 0:
                continue
            starts.append(begin)
            ends.append(begin + ((info >> 8) & 0x3FFFFF) * 4)
        order = sorted(range(len(starts)), key=starts.__getitem__)
        self.fstarts = [starts[i] for i in order]
        self.fends = [ends[i] for i in order]

    def function(self, address):
        i = bisect.bisect_right(self.fstarts, address) - 1
        if i >= 0 and address < self.fends[i]:
            return self.fstarts[i]
        return None

    def _xrefs(self):
        self.refs = {}
        for name, va, vsize, raw, rsize, chars in self.sections:
            if not chars & 0x20:
                continue
            hi = {}
            age = {}
            n = min(vsize, rsize) // 4
            words = struct.unpack_from(">%dI" % n, self.data, raw)
            for i, w in enumerate(words):
                pc = va + i * 4
                op = w >> 26
                rd = (w >> 21) & 31
                ra = (w >> 16) & 31
                imm = w & 0xFFFF
                simm = imm - 0x10000 if imm & 0x8000 else imm
                if op == 15 and ra == 0:  # lis
                    hi[rd] = (imm << 16) & 0xFFFFFFFF
                    age[rd] = i
                    continue
                # addi, ori, loads/stores with a displacement
                if op in (14, 24, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45,
                          46, 47, 48, 49, 50, 51, 52, 53, 54, 55) and ra in hi and i - age[ra] < 64:
                    target = (hi[ra] + (imm if op == 24 else simm)) & 0xFFFFFFFF
                    self.refs.setdefault(target, []).append(pc)
                    if op in (14, 24) and rd != ra:
                        hi.pop(rd, None)
                elif op in (14, 24, 31, 32, 33, 34, 35, 40, 41, 42, 43) and rd in hi:
                    hi.pop(rd, None)
                if w == 0x4E800020:  # blr
                    hi.clear()

    def strings(self, needle):
        pat = re.compile(rb"[\x20-\x7e\t\n\r]{3,}\x00")
        out = []
        for name, va, vsize, raw, rsize, chars in self.sections:
            if chars & 0x20:
                continue
            blob = self.data[raw:raw + min(vsize, rsize)]
            for m in pat.finditer(blob):
                s = m.group()[:-1]
                if needle.encode() in s:
                    out.append((va + m.start(), s.decode(errors="replace")))
        return out

    def text(self, address):
        b = self.read(address, 200) or b""
        b = b.split(b"\0")[0]
        if len(b) >= 3 and all(0x20 <= c < 0x7F or c in (9, 10, 13) for c in b):
            return b.decode()
        return None


def fmt_refs(img, address):
    out = []
    for pc in img.refs.get(address, []):
        f = img.function(pc)
        out.append("sub_%08X@%08X" % (f, pc) if f else "%08X" % pc)
    return out


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return
    img = Image(IMAGE)
    cmd, args = sys.argv[1], sys.argv[2:]
    if cmd == "str":
        for needle in args:
            for address, s in img.strings(needle):
                refs = fmt_refs(img, address)
                print("%08X %r" % (address, s[:120]))
                for r in refs:
                    print("    ", r)
    elif cmd == "refs":
        for a in args:
            print(a, " ".join(fmt_refs(img, int(a, 16))))
    elif cmd == "func":
        for a in args:
            f = img.function(int(a, 16))
            if f is None:
                print(a, "not in a function")
                continue
            end = img.fends[img.fstarts.index(f)]
            print("sub_%08X .. %08X" % (f, end))
            for target, pcs in sorted(img.refs.items()):
                for pc in pcs:
                    if f <= pc < end:
                        s = img.text(target)
                        print("  %08X -> %08X %s" % (pc, target, repr(s[:100]) if s else ""))


if __name__ == "__main__":
    main()
