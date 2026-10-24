#!/usr/bin/env python3
"""Builds a small Android /system image in ARC's read-only FS format.

Contents:
  /system/build.prop                  a few build properties (unless --add gives one)
  /system/media/arc_demo_texture.rgba 128x128 RGBA8 texture (Android head)

Extra files are copied in with --add HOST_PATH:IMAGE_PATH, where IMAGE_PATH
starts with system/ (e.g. --add build/core.jar:system/framework/core.jar);
of two --add of one IMAGE_PATH, the later is kept.
A directory HOST_PATH adds the files under it below IMAGE_PATH.

Files smaller than 64 KiB are packed together at the start of the contents
instead of each taking a 64 KiB page (pack_small in make_test_fs_image.py).

With --store-dex, the classes*.dex of every .jar added are stored (not
deflated) and their data starts on a 64 KiB boundary of the jar, as
zipalign does for a device's boot class path: ART 10 then maps them
straight from the jar (ZipEntry::MapDirectlyFromFile) instead of
extracting a copy into anonymous memory, and a large file starts on a
64 KiB page of the image (above), so the mapping is the image's own pages
(upstream/src/posix_translation/readonly_file.cc). 64 KiB, not 4 KiB: the
wasm page, the offsets mmap accepts.

Usage: make_system_image.py OUTPUT_IMG [--store-dex] [--add HOST_PATH:IMAGE_PATH ...]
"""

import math
import os
import re
import shutil
import struct
import sys
import tempfile
import zipfile

DEX_ALIGN = 64 * 1024
# zipalign's extra field for padding (ZipAlign.cpp, kExtraAlignmentHeaderId).
_ALIGN_EXTRA_ID = 0xD935
_DEX_NAME = re.compile(r'classes\d*\.dex$')

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_test_fs_image import _generate_image  # noqa: E402

SIZE = 128
GREEN = (61, 220, 132)
BACKGROUND = (236, 240, 245)
EYE = (255, 255, 255)


def android_head():
    """Android robot head: a dome with two eyes and two antennae."""
    px = bytearray()
    cx, base = SIZE / 2, SIZE * 0.72
    radius = SIZE * 0.36
    for y in range(SIZE):
        for x in range(SIZE):
            fx, fy = x + 0.5, y + 0.5
            color = BACKGROUND
            # Dome: upper half disc above |base|.
            if fy <= base and math.hypot(fx - cx, fy - base) <= radius:
                color = GREEN
            # Antennae: thick lines from the dome outward.
            for side in (-1, 1):
                ax0, ay0 = cx + side * radius * 0.45, base - radius * 0.85
                ax1, ay1 = cx + side * radius * 0.75, base - radius * 1.3
                t = max(0.0, min(1.0, ((fx - ax0) * (ax1 - ax0) +
                                       (fy - ay0) * (ay1 - ay0)) /
                                 ((ax1 - ax0) ** 2 + (ay1 - ay0) ** 2)))
                d = math.hypot(fx - (ax0 + t * (ax1 - ax0)),
                               fy - (ay0 + t * (ay1 - ay0)))
                if d <= SIZE * 0.025:
                    color = GREEN
                # Eyes.
                if math.hypot(fx - (cx + side * radius * 0.42),
                              fy - (base - radius * 0.45)) <= SIZE * 0.045:
                    color = EYE
            px += bytes(color) + b'\xff'
    return bytes(px)


def store_dex(src, dest):
    """Copies jar |src| to |dest| with its classes*.dex stored, each one's
    data DEX_ALIGN-aligned; the other entries first (they fill what would
    be padding), deflated, in their order."""
    with zipfile.ZipFile(src) as zin, open(dest, 'wb') as raw:
        entries = zin.infolist()
        dex = [e for e in entries if _DEX_NAME.match(e.filename)]
        rest = [e for e in entries if e not in dex]
        with zipfile.ZipFile(raw, 'w') as zout:
            for e in rest:
                out = zipfile.ZipInfo(e.filename, e.date_time)
                out.external_attr = e.external_attr
                out.compress_type = (zipfile.ZIP_STORED if e.is_dir()
                                     else zipfile.ZIP_DEFLATED)
                zout.writestr(out, zin.read(e))
            for e in dex:
                data = zin.read(e)
                out = zipfile.ZipInfo(e.filename, e.date_time)
                out.external_attr = e.external_attr
                out.compress_type = zipfile.ZIP_STORED
                # Local header: 30 bytes, the name, then the extra field
                # (4-byte header + padding) right before the data.
                start = raw.tell() + 30 + len(e.filename.encode()) + 4
                pad = -start % DEX_ALIGN
                out.extra = struct.pack('<HH', _ALIGN_EXTRA_ID, pad) + bytes(pad)
                zout.writestr(out, data)
                out.extra = b''  # the padding only in the local header
    with zipfile.ZipFile(dest) as z, open(dest, 'rb') as f:
        for e in z.infolist():
            if _DEX_NAME.match(e.filename):
                f.seek(e.header_offset + 26)
                name_len, extra_len = struct.unpack('<HH', f.read(4))
                at = e.header_offset + 30 + name_len + extra_len
                assert e.compress_type == zipfile.ZIP_STORED and at % DEX_ALIGN == 0, (dest, e)


def main(argv):
    output = os.path.abspath(argv[1])
    extra = []
    args = argv[2:]
    dex_stored = False
    while args:
        if args[0] == '--store-dex':
            dex_stored = True
            args = args[1:]
            continue
        if args[0] != '--add' or len(args) < 2:
            sys.exit('usage: make_system_image.py OUTPUT_IMG [--store-dex] [--add SRC:DEST ...]')
        src, dest = args[1].rsplit(':', 1)
        if not dest.startswith('system/'):
            sys.exit('--add destination must start with system/: ' + dest)
        if os.path.isdir(src):
            for root, _, files in sorted(os.walk(src)):
                for name in sorted(files):
                    path = os.path.join(root, name)
                    extra.append((os.path.abspath(path),
                                  os.path.join(dest, os.path.relpath(path, src))))
        else:
            extra.append((os.path.abspath(src), dest))
        args = args[2:]
    # A later --add of the same image path replaces an earlier one (a topic
    # included later overrides a file of an earlier one, e.g. build.prop).
    last = {dest: i for i, (_, dest) in enumerate(extra)}
    extra = [e for i, e in enumerate(extra) if last[e[1]] == i]
    os.makedirs(os.path.dirname(output), exist_ok=True)
    cwd = os.getcwd()
    with tempfile.TemporaryDirectory() as work:
        os.chdir(work)
        os.makedirs('system/media')
        with open('system/build.prop', 'w') as f:
            f.write('ro.build.version.release=5.0.2\n'
                    'ro.product.model=arc-wasm\n'
                    'ro.product.cpu.abi=wasm32\n')
        with open('system/media/arc_demo_texture.rgba', 'wb') as f:
            f.write(android_head())
        for src, dest in extra:
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            if dex_stored and dest.endswith('.jar'):
                store_dex(src, dest)
            else:
                shutil.copyfile(src, dest)
        # An --add of system/build.prop replaces the placeholder above.
        defaults = [f for f in ['system/build.prop', 'system/media/arc_demo_texture.rgba']
                    if f not in [dest for _, dest in extra]]
        # Small files (CA certificates, build.prop ...) packed: each would
        # otherwise take a 64 KiB page of the image.
        _generate_image(defaults + [dest for _, dest in extra],
                        {}, [], [], output, pack_small=True)
        os.chdir(cwd)
    print(output)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
