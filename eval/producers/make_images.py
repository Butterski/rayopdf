#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Write the synthetic-corpus fixture images as deterministic 8-bit RGB PNGs.

Usage: make_images.py OUTDIR

Writes OUTDIR/chart.png (640x320, white with 5 coloured vertical bars),
OUTDIR/icon.png (64x64, a filled circle) and OUTDIR/photo.png (1024x683, an RGB
gradient plus noise from the LCG x = (1103515245*x + 12345) mod 2^31, seed 1).
Only zlib and struct are used, so the bytes are identical on every run.
Importable: write_images(outdir).
"""

import argparse
import os
import struct
import zlib

WHITE = (255, 255, 255)
BAR_COLOURS = (
    (31, 119, 180),
    (255, 127, 14),
    (44, 160, 44),
    (214, 39, 40),
    (148, 103, 189),
)
BAR_HEIGHTS = (120, 260, 180, 220, 90)


def _chunk(kind, data):
    body = kind + data
    return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)


def encode_png(width, height, rows):
    """rows: iterable of `height` bytes objects, each width*3 RGB bytes."""
    raw = b"".join(b"\x00" + bytes(row) for row in rows)
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + _chunk(b"IHDR", ihdr)
            + _chunk(b"IDAT", zlib.compress(raw, 9)) + _chunk(b"IEND", b""))


def chart_png():
    w, h = 640, 320
    base = 300  # bars stand on y = base
    bar_w, gap = 80, 40
    left = (w - (5 * bar_w + 4 * gap)) // 2
    rows = []
    for y in range(h):
        row = bytearray(bytes(WHITE) * w)
        for i, (colour, bh) in enumerate(zip(BAR_COLOURS, BAR_HEIGHTS)):
            if base - bh <= y < base:
                x0 = left + i * (bar_w + gap)
                row[x0 * 3:(x0 + bar_w) * 3] = bytes(colour) * bar_w
        if y == base:  # axis line
            row[(left - 10) * 3:(w - left + 10) * 3] = b"\x00\x00\x00" * (w - 2 * left + 20)
        rows.append(row)
    return encode_png(w, h, rows)


def icon_png():
    n = 64
    c, r2 = 31.5, 28.0 * 28.0
    fill = (0, 102, 204)
    rows = []
    for y in range(n):
        row = bytearray()
        for x in range(n):
            inside = (x - c) ** 2 + (y - c) ** 2 <= r2
            row += bytes(fill if inside else WHITE)
        rows.append(row)
    return encode_png(n, n, rows)


def photo_png():
    w, h = 1024, 683
    x_state = 1
    rows = []
    for y in range(h):
        row = bytearray(w * 3)
        g = y * 255 // (h - 1)
        for x in range(w):
            r = x * 255 // (w - 1)
            b = (x + y) * 255 // (w + h - 2)
            i = x * 3
            for k, v in enumerate((r, g, b)):
                x_state = (1103515245 * x_state + 12345) & 0x7FFFFFFF
                v += ((x_state >> 16) & 31) - 16
                row[i + k] = 0 if v < 0 else 255 if v > 255 else v
        rows.append(row)
    return encode_png(w, h, rows)


def write_images(outdir):
    os.makedirs(outdir, exist_ok=True)
    for name, make in (("chart.png", chart_png), ("icon.png", icon_png), ("photo.png", photo_png)):
        with open(os.path.join(outdir, name), "wb") as f:
            f.write(make())


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("outdir")
    args = ap.parse_args()
    write_images(args.outdir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
