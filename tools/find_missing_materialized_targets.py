#!/usr/bin/env python3
"""Find lis/addi- and lis/ori-materialized code addresses that are not
registered functions: runtime-computed function pointers the ReXGlue scanner
cannot see.

Adapted from nhl-legacy-recomp. Section ranges come from the dump's PE header,
targets must sit on a function boundary, and addresses that are already branch
labels inside a generated function (loc_XXXXXXXX:) are skipped.

Usage:
  find_missing_materialized_targets.py <guest_image_dump.bin> <generated/default>
"""
from pathlib import Path
import re
import struct
import sys

BASE = 0x82000000

img = open(sys.argv[1], "rb").read()
gen = Path(sys.argv[2])
register = next(gen.glob("*_register.cpp")).read_text()
known = {int(m, 16) for m in re.findall(r"SetFunction\(0x([0-9A-Fa-f]{8})", register)}
labels = set()
for cpp in gen.glob("*_recomp.*.cpp"):
    labels.update(int(m, 16) for m in re.findall(r"^loc_([0-9A-F]{8}):", cpp.read_text(), re.M))
print(f"known functions: {len(known)}, internal labels: {len(labels)}", file=sys.stderr)

pe = struct.unpack_from("<I", img, 0x3C)[0]
nsec = struct.unpack_from("<H", img, pe + 6)[0]
sec_tab = pe + 24 + struct.unpack_from("<H", img, pe + 20)[0]
code_ranges = []
for i in range(nsec):
    _, vsize, va = struct.unpack_from("<8sII", img, sec_tab + i * 40)
    if struct.unpack_from("<I", img, sec_tab + i * 40 + 36)[0] & 0x20000000:
        code_ranges.append((BASE + va, BASE + va + vsize))


def in_code(addr):
    return any(lo <= addr < hi for lo, hi in code_ranges)


def u32(va):
    return struct.unpack_from(">I", img, va - BASE)[0]


def boundary_before(va):
    prev = u32(va - 4)
    return prev in (0, 0x4E800020, 0x4E800420) or ((prev >> 26) == 18 and (prev & 1) == 0)


cands = {}
for lo_sec, hi_sec in code_ranges:
    for va in range(lo_sec, hi_sec - 40, 4):
        w = u32(va)
        if (w & 0xFC1F0000) != 0x3C000000:  # lis rD,imm
            continue
        rd = (w >> 21) & 31
        hi = w & 0xFFFF
        for k in range(1, 10):
            wk = u32(va + 4 * k)
            op = wk >> 26
            if op == 14 and ((wk >> 16) & 31) == rd:  # addi rY,rD,lo
                lo = wk & 0xFFFF
                lo -= 0x10000 if lo >= 0x8000 else 0
                tgt = ((hi << 16) + lo) & 0xFFFFFFFF
                ry = (wk >> 21) & 31
            elif op == 24 and ((wk >> 21) & 31) == rd:  # ori rY,rD,lo
                tgt = ((hi << 16) | (wk & 0xFFFF)) & 0xFFFFFFFF
                ry = (wk >> 16) & 31
            else:
                if (wk & 0xFC1F0000) == 0x3C000000 and ((wk >> 21) & 31) == rd:
                    break
                continue
            if in_code(tgt) and tgt % 4 == 0 and tgt not in known and tgt not in labels:
                # Skip switch-table bases (lis/addi feeding add/mtctr/bctr).
                is_switch = False
                for j in range(k + 1, k + 6):
                    wj = u32(va + 4 * j)
                    if wj == 0x4E800420:
                        is_switch = True
                        break
                    if wj == 0x4E800021 or (wj >> 26) in (16, 18):
                        break
                if not is_switch and boundary_before(tgt):
                    cands.setdefault(tgt, []).append(va)
            break

print(f"candidates: {len(cands)}", file=sys.stderr)
for tgt in sorted(cands):
    refs = ",".join(f"{r:08X}" for r in cands[tgt][:3])
    print(f'0x{tgt:08X} = {{ name = "sub_{tgt:08X}" }}  # materialized at: {refs}')
