#!/usr/bin/env python3
"""Draws the PWA's home screen icon (the Android robot on a dark square) as
a PNG, without image libraries: every shape is a signed distance field, and
the distance at a pixel gives its antialiased coverage.

Usage: make_pwa_icon.py SIZE OUT.png

The robot stays inside the middle 80% of the square, so the icon also works
as a "maskable" one (platforms crop it to a circle or a rounded square).
"""

import math
import struct
import sys
import zlib

BACKGROUND = (0x12, 0x16, 0x1f)
GREEN = (0x3d, 0xdc, 0x84)


def circle(px, py, cx, cy, r):
    return math.hypot(px - cx, py - cy) - r


def capsule(px, py, ax, ay, bx, by, r):
    dx, dy = bx - ax, by - ay
    t = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)))
    return math.hypot(px - ax - t * dx, py - ay - t * dy) - r


def round_box(px, py, x0, y0, x1, y1, r):
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    qx = abs(px - cx) - (x1 - x0) / 2 + r
    qy = abs(py - cy) - (y1 - y0) / 2 + r
    return math.hypot(max(qx, 0.0), max(qy, 0.0)) + min(max(qx, qy), 0.0) - r


def robot(x, y):
    """Distance from (x, y) in the unit square to the robot (< 0: inside)."""
    y -= 0.03  # the figure's centre of mass to the middle
    head = max(circle(x, y, 0.5, 0.40, 0.20), y - 0.385)
    eyes = min(circle(x, y, 0.43, 0.31, 0.022), circle(x, y, 0.57, 0.31, 0.022))
    head = max(head, -eyes)
    antennae = min(capsule(x, y, 0.415, 0.235, 0.375, 0.165, 0.011),
                   capsule(x, y, 0.585, 0.235, 0.625, 0.165, 0.011))
    body = round_box(x, y, 0.30, 0.405, 0.70, 0.67, 0.045)
    arms = min(capsule(x, y, 0.245, 0.45, 0.245, 0.60, 0.042),
               capsule(x, y, 0.755, 0.45, 0.755, 0.60, 0.042))
    legs = min(capsule(x, y, 0.42, 0.62, 0.42, 0.75, 0.042),
               capsule(x, y, 0.58, 0.62, 0.58, 0.75, 0.042))
    return min(head, antennae, body, arms, legs)


def render(size):
    rows = []
    for j in range(size):
        row = bytearray([0])  # PNG filter type: none
        y = (j + 0.5) / size
        for i in range(size):
            d = robot((i + 0.5) / size, y) * size
            a = max(0.0, min(1.0, 0.5 - d))
            row += bytes(round(b + (g - b) * a) for b, g in zip(BACKGROUND, GREEN))
        rows.append(bytes(row))
    return b''.join(rows)


def png(size, pixels):
    def chunk(kind, data):
        return (struct.pack('>I', len(data)) + kind + data +
                struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff))
    header = struct.pack('>IIBBBBB', size, size, 8, 2, 0, 0, 0)  # 8-bit RGB
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header) +
            chunk(b'IDAT', zlib.compress(pixels, 9)) + chunk(b'IEND', b''))


def main(argv):
    size = int(argv[1])
    with open(argv[2], 'wb') as f:
        f.write(png(size, render(size)))


if __name__ == '__main__':
    main(sys.argv)
