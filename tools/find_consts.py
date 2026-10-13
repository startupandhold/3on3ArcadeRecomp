#!/usr/bin/env python3
"""Locate 1280x720 immediates and 16:9 float constants in the dumped guest image."""
import struct, sys
BASE = 0x82000000
img = open(sys.argv[1], "rb").read()
CODE = (0x822C0000, 0x82F832C4)

def u32(va):
    return struct.unpack_from(">I", img, va - BASE)[0]

# li rX,1280 (addi rX,r0,0x500) followed within 12 insns by li rY,720
pairs = []
for va in range(CODE[0], CODE[1] - 48, 4):
    w = u32(va)
    if (w & 0xFC1FFFFF) == (14 << 26) | 0x500:
        for k in range(1, 13):
            w2 = u32(va + 4 * k)
            if (w2 & 0xFC1FFFFF) == (14 << 26) | 0x2D0:
                pairs.append((va, va + 4 * k))
                break
print(f"li 1280 / li 720 pairs: {len(pairs)}")
for a, b in pairs[:40]:
    print(f"  {a:08X} {b:08X}")

for name, val in (("16/9", 16 / 9), ("9/16", 9 / 16), ("1280.0", 1280.0), ("720.0", 720.0), ("4/3", 4 / 3)):
    pat = struct.pack(">f", val)
    hits, i = [], img.find(pat)
    while i != -1:
        hits.append(BASE + i)
        i = img.find(pat, i + 1)
    print(f"float {name}: {len(hits)} hits {[hex(h) for h in hits[:12]]}")
