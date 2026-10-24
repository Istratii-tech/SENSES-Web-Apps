#!/usr/bin/env python3
"""What the personal-data gate (tools/checknames.sh) reads of a binary image:
an Android /system image (a VM's .data, system.img) or a wasm module.

  tools/unpackimage.py IMAGE OUTDIR

Writes into OUTDIR:
  strings.txt               every run of 4+ printable bytes of the image, one
                            per line (`strings -a -n 4`): paths, names,
                            addresses compiled into libraries and dex;
  zip.txt                   the strings of every DEFLATEd zip entry found in
                            the image (a jar's resources, an APK's
                            resources.arsc, assets), each line "ENTRY<TAB>
                            STRING": compressed bytes show nothing to a
                            search, so they are inflated first.

The entries are found by their local headers (PK\\3\\4), not through a central
directory: the image holds jars and APKs back to back, and the gate wants
every compressed byte, wherever it is. Stored entries are already plain in
strings.txt. Prints how many entries it inflated and how many it could not.
"""

import os
import re
import struct
import sys
import zlib

PRINTABLE = re.compile(rb'[\x20-\x7e\t]{4,}')
SAFE = re.compile(r'[^A-Za-z0-9._-]+')


def strings(data):
    return b'\n'.join(m.group(0) for m in PRINTABLE.finditer(data)) + b'\n'


def main(argv):
    image, out = argv[1], argv[2]
    os.makedirs(out, exist_ok=True)
    zips = open(os.path.join(out, 'zip.txt'), 'wb')
    with open(image, 'rb') as f:
        data = f.read()
    with open(os.path.join(out, 'strings.txt'), 'wb') as f:
        f.write(strings(data))
    inflated = failed = 0
    pos = data.find(b'PK\x03\x04')
    while pos >= 0:
        try:
            (_, _, flags, method, _, _, _, csize, _, nlen, elen) = struct.unpack('<IHHHHHIIIHH', data[pos:pos + 30])
        except struct.error:
            break
        name = data[pos + 30:pos + 30 + nlen].decode('utf-8', 'replace')
        start = pos + 30 + nlen + elen
        if method == 8:
            d = zlib.decompressobj(-15)
            try:
                # The size may be in a data descriptor after the entry (flag 8):
                # inflate until the stream ends, at most 64 MB.
                body = d.decompress(data[start:start + csize] if csize and not flags & 8 else
                                    data[start:start + (64 << 20)], 256 << 20)
                prefix = SAFE.sub('_', name)[-100:].encode() + b'\t'
                zips.write(b''.join(prefix + m.group(0) + b'\n' for m in PRINTABLE.finditer(body)))
                inflated += 1
            except zlib.error:
                failed += 1
        pos = data.find(b'PK\x03\x04', pos + 4)
    zips.close()
    print('%s: %d zip entries inflated, %d not (not a zip entry after all)' % (image, inflated, failed))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
