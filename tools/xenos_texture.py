#!/usr/bin/env python3
"""Xbox 360 (Xenos) DXT5 textures and the PNG/RGBA helpers around them, in
pure Python.

Usage: xenos_texture.py decode TEXTURE WIDTH HEIGHT OUT.png [--skip BYTES]

Xenos textures are tiled (32x32-block macro tiles) and stored with every
16-bit word byte-swapped. Images are RGBA bytearrays, row-major.
"""
import struct
import sys
import zlib


def tiled_block(x, y, width_blocks):
    """Index of block (x, y) in a tiled texture of 16-byte blocks."""
    log_bpp = 4
    aligned = (width_blocks + 31) & ~31
    macro = ((x >> 5) + (y >> 5) * (aligned >> 5)) << (log_bpp + 7)
    micro = ((x & 7) + ((y & 0xE) << 2)) << log_bpp
    off = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4)
    return (((off & ~0x1FF) << 3) + ((y & 16) << 7) + ((off & 0x1C0) << 2)
            + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (off & 0x3F)) >> log_bpp


def _swap16(b):
    b = bytearray(b)
    b[0::2], b[1::2] = b[1::2], b[0::2]
    return b


def _rgb565(c):
    return ((c >> 11) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31)


def _to565(r, g, b):
    return ((r * 31 + 127) // 255) << 11 | ((g * 63 + 127) // 255) << 5 | ((b * 31 + 127) // 255)


def _alpha_palette(a0, a1):
    if a0 > a1:
        return [a0, a1] + [((6 - k) * a0 + (k + 1) * a1) // 7 for k in range(6)]
    return [a0, a1] + [((4 - k) * a0 + (k + 1) * a1) // 5 for k in range(4)] + [0, 255]


def _color_palette(c0, c1):
    p0, p1 = _rgb565(c0), _rgb565(c1)
    return [p0, p1, tuple((2 * a + b) // 3 for a, b in zip(p0, p1)),
            tuple((a + 2 * b) // 3 for a, b in zip(p0, p1))]


def decode_dxt5(data, width, height):
    bw, bh = width // 4, height // 4
    out = bytearray(width * height * 4)
    for by in range(bh):
        for bx in range(bw):
            i = tiled_block(bx, by, bw) * 16
            b = _swap16(data[i:i + 16])
            alphas = _alpha_palette(b[0], b[1])
            abits = int.from_bytes(b[2:8], "little")
            c0, c1 = struct.unpack_from("<HH", b, 8)
            colors = _color_palette(c0, c1)
            cbits = int.from_bytes(b[12:16], "little")
            for k in range(16):
                r, g, bl = colors[(cbits >> (2 * k)) & 3]
                o = ((by * 4 + k // 4) * width + bx * 4 + k % 4) * 4
                out[o:o + 4] = bytes((r, g, bl, alphas[(abits >> (3 * k)) & 7]))
    return out


def _encode_block(px):
    """px: 16 (r, g, b, a) tuples -> 16-byte DXT5 block (little-endian)."""
    alphas = [p[3] for p in px]
    a0, a1 = max(alphas), min(alphas)
    if a0 == a1:
        abits = 0
    else:
        pal = _alpha_palette(a0, a1)
        abits = 0
        for k, a in enumerate(alphas):
            abits |= min(range(8), key=lambda j: abs(pal[j] - a)) << (3 * k)
    visible = [p for p in px if p[3] > 0] or px
    lo = [min(p[c] for p in visible) for c in range(3)]
    hi = [max(p[c] for p in visible) for c in range(3)]
    mean = [sum(p[c] for p in visible) / len(visible) for c in range(3)]
    # Flip the bounding-box diagonal along channels that run against green.
    for c in (0, 2):
        cov = sum((p[c] - mean[c]) * (p[1] - mean[1]) for p in visible)
        if cov < 0:
            lo[c], hi[c] = hi[c], lo[c]
    c0, c1 = _to565(*hi), _to565(*lo)
    if c0 < c1:
        c0, c1 = c1, c0
    cbits = 0
    if c0 != c1:
        pal = _color_palette(c0, c1)
        for k, p in enumerate(px):
            best = min(range(4), key=lambda j: sum((pal[j][c] - p[c]) ** 2 for c in range(3)))
            cbits |= best << (2 * k)
    return (bytes((a0, a1)) + abits.to_bytes(6, "little") + struct.pack("<HH", c0, c1)
            + cbits.to_bytes(4, "little"))


def encode_dxt5(rgba, width, height):
    bw, bh = width // 4, height // 4
    out = bytearray(bw * bh * 16)
    for by in range(bh):
        for bx in range(bw):
            px = []
            for k in range(16):
                o = ((by * 4 + k // 4) * width + bx * 4 + k % 4) * 4
                px.append(tuple(rgba[o:o + 4]))
            i = tiled_block(bx, by, bw) * 16
            out[i:i + 16] = _swap16(_encode_block(px))
    return out


def read_png(path):
    """(width, height, RGBA bytearray) for 8-bit RGB/RGBA, non-interlaced PNGs."""
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG")
    p, idat = 8, bytearray()
    while p < len(data):
        n, kind = struct.unpack_from(">I4s", data, p)
        body = data[p + 8:p + 8 + n]
        if kind == b"IHDR":
            width, height, depth, color, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat += body
        p += 12 + n
    if depth != 8 or color not in (2, 6) or interlace:
        raise ValueError(f"{path}: needs an 8-bit RGB or RGBA, non-interlaced PNG")
    channels = 4 if color == 6 else 3
    raw = zlib.decompress(idat)
    stride = width * channels
    prev = bytearray(stride)
    pixels = bytearray()
    i = 0
    for _ in range(height):
        kind, line = raw[i], bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        for x in range(stride):
            a = line[x - channels] if x >= channels else 0
            b = prev[x]
            c = prev[x - channels] if x >= channels else 0
            if kind == 1:
                line[x] = (line[x] + a) & 255
            elif kind == 2:
                line[x] = (line[x] + b) & 255
            elif kind == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif kind == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        pixels += line
        prev = line
    if channels == 3:
        rgba = bytearray(width * height * 4)
        rgba[0::4], rgba[1::4], rgba[2::4] = pixels[0::3], pixels[1::3], pixels[2::3]
        rgba[3::4] = b"\xff" * (width * height)
        pixels = rgba
    return width, height, pixels


def write_png(path, width, height, rgba):
    raw = b"".join(b"\0" + bytes(rgba[y * width * 4:(y + 1) * width * 4]) for y in range(height))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def resize(rgba, width, height, new_width, new_height):
    """Area-average resample, weighting colour by alpha."""
    sx, sy = width / new_width, height / new_height
    out = bytearray(new_width * new_height * 4)
    spans_x = []
    for x in range(new_width):
        x0, x1 = x * sx, (x + 1) * sx
        spans_x.append([(i, min(x1, i + 1) - max(x0, i)) for i in range(int(x0), min(width, int(x1 + 0.999999)))])
    for y in range(new_height):
        y0, y1 = y * sy, (y + 1) * sy
        span_y = [(j, min(y1, j + 1) - max(y0, j)) for j in range(int(y0), min(height, int(y1 + 0.999999)))]
        for x in range(new_width):
            r = g = b = a = area = 0.0
            for j, wy in span_y:
                row = j * width
                for i, wx in spans_x[x]:
                    w = wx * wy
                    o = (row + i) * 4
                    pa = rgba[o + 3] * w
                    r += rgba[o] * pa
                    g += rgba[o + 1] * pa
                    b += rgba[o + 2] * pa
                    a += pa
                    area += w
            o = (y * new_width + x) * 4
            if a > 0:
                out[o:o + 4] = bytes((round(r / a), round(g / a), round(b / a), round(a / area)))
    return out


def place(rgba, width, height, canvas_width, canvas_height, x=0, y=0):
    """Copies an image into a transparent canvas at (x, y), clipping to it."""
    out = bytearray(canvas_width * canvas_height * 4)
    for row in range(height):
        cy = y + row
        if not 0 <= cy < canvas_height:
            continue
        left, right = max(0, -x), min(width, canvas_width - x)
        if left >= right:
            continue
        src = (row * width + left) * 4
        dst = (cy * canvas_width + x + left) * 4
        out[dst:dst + (right - left) * 4] = rgba[src:src + (right - left) * 4]
    return out


def main():
    if len(sys.argv) < 6 or sys.argv[1] != "decode":
        sys.exit(__doc__)
    path, width, height, out = sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), sys.argv[5]
    skip = int(sys.argv[7]) if len(sys.argv) > 7 and sys.argv[6] == "--skip" else 0
    data = open(path, "rb").read()[skip:]
    write_png(out, width, height, decode_dxt5(data, width, height))


if __name__ == "__main__":
    main()
