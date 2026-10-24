#!/usr/bin/env python3
"""Splits an ICU common data file (icudtNNl.dat) into its items.

usage: icu_dat_split.py ICUDT.dat OUTDIR

Writes each item of the package's table of contents as a file of its own,
OUTDIR/<item name> (e.g. OUTDIR/icudt63l/coll/root.res): ICU's "files" data
packaging, which ICU4C finds under its data directory (udata.cpp,
doLoadFromIndividualFiles) and ICU4J under its data path (ICUBinary,
addDataFilesFromFolder). Topic image-on-demand: in the /system image, an app
then maps only the items it uses, not the whole 23 MB file
(docs/topics/image-on-demand.md).
"""
import os
import struct
import sys


def items(data):
    header_size = struct.unpack_from('<H', data, 0)[0]
    if data[2:4] != b'\xda\x27':
        sys.exit('not ICU data (magic)')
    big_endian = data[8]
    if data[12:16] != b'CmnD':
        sys.exit('not an ICU common data package (%r)' % data[12:16])
    order = '>' if big_endian else '<'
    toc = header_size
    count = struct.unpack_from(order + 'I', data, toc)[0]
    entries = []
    for i in range(count):
        name_at, data_at = struct.unpack_from(order + 'II', data, toc + 4 + 8 * i)
        end = data.index(b'\0', toc + name_at)
        entries.append((data[toc + name_at:end].decode('ascii'), toc + data_at))
    # Items lie in the order of the table (sorted names), each up to the next.
    starts = sorted(at for _, at in entries) + [len(data)]
    following = {at: starts[i + 1] for i, at in enumerate(starts[:-1])}
    return [(name, data[at:following[at]]) for name, at in entries]


def main(argv):
    src, out = argv[1], argv[2]
    data = open(src, 'rb').read()
    found = items(data)
    for name, body in found:
        if name.startswith('/') or '..' in name.split('/'):
            sys.exit('bad item name: ' + name)
        path = os.path.join(out, name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'wb') as f:
            f.write(body)
    print('%s: %d items, %d bytes -> %s' % (src, len(found), sum(len(b) for _, b in found), out))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
