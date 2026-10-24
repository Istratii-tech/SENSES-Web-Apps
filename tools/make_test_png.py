#!/usr/bin/env python3
"""Writes the PNG that tests/java/arcwasm/GraphicsTest.java decodes.

48x32 RGBA, four opaque quadrants (top left red, top right green, bottom
left blue, bottom right white) and a half-transparent black middle row
(y = 16, alpha 128). Encoded with zlib only (no imaging library), filter
type 0 on every row.

Usage: make_test_png.py OUTPUT_PNG
"""

import struct
import sys
import zlib

W, H = 48, 32


def pixel(x, y):
    if y == H // 2:
        return (0, 0, 0, 128)
    top, left = y < H // 2, x < W // 2
    if top:
        return (255, 0, 0, 255) if left else (0, 255, 0, 255)
    return (0, 0, 255, 255) if left else (255, 255, 255, 255)


def chunk(kind, data):
    body = kind + data
    return struct.pack('>I', len(data)) + body + struct.pack('>I', zlib.crc32(body))


def main(out):
    raw = b''.join(b'\0' + b''.join(bytes(pixel(x, y)) for x in range(W))
                   for y in range(H))
    png = (b'\x89PNG\r\n\x1a\n'
           + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 6, 0, 0, 0))
           + chunk(b'IDAT', zlib.compress(raw, 9))
           + chunk(b'IEND', b''))
    with open(out, 'wb') as f:
        f.write(png)


if __name__ == '__main__':
    main(*sys.argv[1:])
