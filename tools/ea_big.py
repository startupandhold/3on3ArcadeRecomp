#!/usr/bin/env python3
"""List or extract EA BIG archives (BIGF/BIG4), including RefPack-compressed
(0x10FB) nested archives such as the front-end screens in data0.big.

Usage: ea_big.py ARCHIVE [--extract DIR] [--filter SUBSTRING]
Nested .big entries are unpacked recursively into DIR/<entry path>/.
"""
import argparse
import os
import struct


def refpack_decompress(data):
    """Decompresses EA RefPack (header 0x10FB or 0x11FB with a 3-byte size)."""
    flags = data[0]
    if data[1] != 0xFB:
        raise ValueError("not RefPack")
    p = 2
    large = flags & 0x80
    size_len = 4 if large else 3
    if flags & 0x01:
        p += size_len  # compressed size
    out_size = int.from_bytes(data[p:p + size_len], "big")
    p += size_len
    out = bytearray()
    while p < len(data):
        b0 = data[p]
        if b0 < 0x80:
            b1 = data[p + 1]
            p += 2
            lit = b0 & 3
            out += data[p:p + lit]
            p += lit
            length = ((b0 >> 2) & 7) + 3
            offset = ((b0 & 0x60) << 3) + b1 + 1
        elif b0 < 0xC0:
            b1, b2 = data[p + 1], data[p + 2]
            p += 3
            lit = b1 >> 6
            out += data[p:p + lit]
            p += lit
            length = (b0 & 0x3F) + 4
            offset = ((b1 & 0x3F) << 8) + b2 + 1
        elif b0 < 0xE0:
            b1, b2, b3 = data[p + 1], data[p + 2], data[p + 3]
            p += 4
            lit = b0 & 3
            out += data[p:p + lit]
            p += lit
            length = ((b0 & 0x0C) << 6) + b3 + 5
            offset = ((b0 & 0x10) << 12) + (b1 << 8) + b2 + 1
        elif b0 < 0xFC:
            lit = ((b0 & 0x1F) << 2) + 4
            p += 1
            out += data[p:p + lit]
            p += lit
            continue
        else:
            lit = b0 & 3
            p += 1
            out += data[p:p + lit]
            break
        start = len(out) - offset
        for i in range(length):
            out.append(out[start + i])
    if len(out) != out_size:
        raise ValueError(f"RefPack size {len(out)} != {out_size}")
    return bytes(out)


def refpack_compress(data):
    """EA RefPack (0x10FB) using literal runs only. Round-trips with
    refpack_decompress; good enough for archive entries the game loads."""
    out = bytearray(b"\x10\xFB")
    out += len(data).to_bytes(3, "big")
    i, n = 0, len(data)
    while i < n:
        left = n - i
        if left < 4:
            out.append(0xFC | left)
            out += data[i:]
            break
        # Opcode 0xE0..0xFB encodes lit = ((b0 & 0x1F) << 2) + 4 (4..112, step 4).
        lit = min(112, left & ~3)
        if left - lit in (1, 2, 3) and lit >= 8:
            lit -= 4
        out.append(0xE0 | ((lit - 4) >> 2))
        out += data[i:i + lit]
        i += lit
    return bytes(out)


def unpack(data):
    if len(data) >= 2 and data[1] == 0xFB and data[0] & 0xFE in (0x10, 0x90):
        return refpack_decompress(data)
    return data


def entries(data):
    magic = data[:4]
    if magic not in (b"BIGF", b"BIG4"):
        raise ValueError(f"not a BIG archive: {magic!r}")
    count = struct.unpack(">I", data[8:12])[0]
    p = 16
    out = []
    for _ in range(count):
        off, size = struct.unpack(">II", data[p:p + 8])
        p += 8
        end = data.index(b"\0", p)
        out.append((data[p:end].decode("latin1"), off, size))
        p = end + 1
    return out


def walk(data, prefix, args, depth=0):
    for name, off, size in entries(data):
        path = f"{prefix}{name}"
        if args.filter and args.filter not in path:
            continue
        blob = unpack(data[off:off + size])
        nested = blob[:4] in (b"BIGF", b"BIG4")
        if True:
            print(f"{'  ' * depth}{path} {size}{' -> ' + str(len(blob)) if len(blob) != size else ''}")
            if args.extract and not nested:
                dest = os.path.join(args.extract, path)
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                with open(dest, "wb") as f:
                    f.write(blob)
        if nested:
            walk(blob, path + "/", argparse.Namespace(extract=args.extract, filter=None), depth + 1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("archive")
    ap.add_argument("--extract")
    ap.add_argument("--filter")
    args = ap.parse_args()
    with open(args.archive, "rb") as f:
        walk(unpack(f.read()), "", args)


if __name__ == "__main__":
    main()
