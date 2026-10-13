#!/usr/bin/env python3
"""Find code addresses referenced from data sections (vtables, callback tables)
that the ReXGlue scanner did not register as functions.

Adapted from nhl-legacy-recomp. Section ranges are read from the PE header of
the dumped image instead of being hard-coded.

Usage:
  find_missing_indirect_targets.py <guest_image_dump.bin> <generated/default/*_register.cpp>

Output: TOML [functions] lines for candidates whose first instruction looks
like a plausible function start and whose preceding word looks like a
function boundary (blr/b/bctr/padding).
"""
import re
import struct
import sys

BASE = 0x82000000

img = open(sys.argv[1], "rb").read()
register = open(sys.argv[2], "rb").read()

known = {int(m.group(1), 16) for m in re.finditer(rb"SetFunction\(0x([0-9A-Fa-f]{8})", register)}
print(f"known functions: {len(known)}", file=sys.stderr)

pe = struct.unpack_from("<I", img, 0x3C)[0]
nsec = struct.unpack_from("<H", img, pe + 6)[0]
sec_tab = pe + 24 + struct.unpack_from("<H", img, pe + 20)[0]
code_ranges, data_ranges = [], []
for i in range(nsec):
    name, vsize, va = struct.unpack_from("<8sII", img, sec_tab + i * 40)
    chars = struct.unpack_from("<I", img, sec_tab + i * 40 + 36)[0]
    name = name.rstrip(b"\0").decode()
    lo = BASE + va
    if chars & 0x20000000:  # IMAGE_SCN_MEM_EXECUTE
        code_ranges.append((lo, lo + vsize))
    elif name in (".rdata", ".data"):
        data_ranges.append((lo, lo + vsize))


def in_code(addr):
    return any(lo <= addr < hi for lo, hi in code_ranges)


def word(va):
    off = va - BASE
    if off < 0 or off + 4 > len(img):
        return None
    return struct.unpack_from(">I", img, off)[0]


def plausible_start(w):
    if w is None or w == 0:
        return False
    op = w >> 26
    if w == 0x7D8802A6:  # mflr r12
        return True
    if (w & 0xFFFF0000) == 0x94210000:  # stwu r1,-X(r1)
        return True
    if op in (14, 15):  # addi/addis (li/lis)
        return True
    if op in (32, 33, 34, 35, 40, 42, 58):  # lwz/lwzu/lbz/lbzu/lhz/lha/ld
        return True
    if op in (36, 38, 44, 62):  # stw/stb/sth/std
        return True
    if w == 0x4E800020:  # blr
        return True
    if op == 18:  # b/bl
        return True
    if op in (10, 11):  # cmpli/cmpi
        return True
    if op == 31 and ((w >> 1) & 0x3FF) in (444, 266, 40, 0, 32):  # mr/add/subf/cmp/cmpl
        return True
    if op in (48, 50):  # lfs/lfd
        return True
    return False


def boundary_before(va):
    w = word(va - 4)
    if w is None:
        return False
    if w == 0:
        return True
    if w in (0x4E800020, 0x4E800420):  # blr / bctr
        return True
    op = w >> 26
    if op == 18 and (w & 1) == 0:  # unconditional b, no link
        return True
    if op == 19 and ((w >> 1) & 0x3FF) == 16 and ((w >> 21) & 0x14) == 0x14:  # bclr always
        return True
    return False


cands = {}
for lo, hi in data_ranges:
    for va in range(lo & ~3, hi & ~3, 4):
        w = word(va)
        if w is None or (w & 3) or w in known or not in_code(w):
            continue
        if plausible_start(word(w)) and boundary_before(w):
            cands.setdefault(w, []).append(va)

print(f"candidates: {len(cands)}", file=sys.stderr)
for t in sorted(cands):
    refs = ",".join(f"{r:08X}" for r in cands[t][:3])
    print(f'0x{t:08X} = {{ name = "sub_{t:08X}" }}  # data refs: {refs}')
