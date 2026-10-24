#!/usr/bin/env python3
"""Builds ARC's posix_translation test read-only FS image (Python 3).

Port of upstream/src/posix_translation/scripts/create_test_fs_image.py and
create_readonly_fs_image.py (Python 2) that produces a byte-identical layout:
a big-endian metadata table followed by 64KiB-aligned file contents. See the
upstream create_readonly_fs_image.py docstring for the format.

Usage: make_test_fs_image.py OUTPUT_DIR
"""

import os
import re
import struct
import sys
import tempfile
import time

_PAGE_SIZE = 64 * 1024  # NaCl used 64k pages; readonly_fs_reader expects it.
_REGULAR_FILE = 0
_SYMBOLIC_LINK = 1
_EMPTY_DIRECTORY = 2


def _normalize_path(name):
    name = re.sub(r'^\.+', '', name)
    name = re.sub(r'^([^/])', r'/\1', name)
    return re.sub(r'^/out/target/.*/root', '', name)


def _pad(buf, boundary):
    rem = len(buf) % boundary
    if rem:
        buf.extend(b'\0' * (boundary - rem))


def _file_size(name, symlink_map, empty_dirs, empty_files):
    if name in symlink_map or name in empty_dirs or name in empty_files:
        return 0
    return os.stat(name).st_size


def _generate_image(input_files, symlink_map, empty_dirs, empty_files,
                    output, pack_small=False):
    """pack_small: files smaller than a page are stored first, one after
    another (16-byte aligned), instead of each on its own 64 KiB page; only
    the larger files stay page-aligned. arc-wasm's ReadonlyFile maps such
    files by copying (readonly_file.cc). Without it the layout is ARC's,
    byte for byte."""
    metadata = bytearray()
    content = bytearray()
    names = list(input_files) + list(symlink_map) + list(empty_dirs) + list(
        empty_files)
    if pack_small:
        # (stable: small files first, in their order, then the large ones)
        names.sort(key=lambda n: _file_size(
            n, symlink_map, empty_dirs, empty_files) >= _PAGE_SIZE)
    metadata += struct.pack('>i', len(names))
    for i, name in enumerate(names):
        link_target = None
        if name in symlink_map:
            ftype, link_target, size, mtime = (
                _SYMBOLIC_LINK, symlink_map[name], 0, time.time())
        elif name in empty_dirs:
            ftype, size, mtime = _EMPTY_DIRECTORY, 0, time.time()
        elif name in empty_files:
            ftype, size, mtime = _REGULAR_FILE, 0, time.time()
        else:
            st = os.stat(name)
            ftype, size, mtime = _REGULAR_FILE, st.st_size, st.st_mtime
        if pack_small:
            _pad(content, _PAGE_SIZE if size >= _PAGE_SIZE else 16)
        _pad(metadata, 4)
        metadata += struct.pack('>iiii', len(content), size, int(mtime), ftype)
        metadata += _normalize_path(name).encode('utf-8') + b'\0'
        if link_target:
            metadata += link_target.encode('utf-8') + b'\0'
        if ftype == _REGULAR_FILE and size > 0:
            with open(name, 'rb') as f:
                content += f.read(size)
        if i < len(names) - 1 and not pack_small:
            _pad(content, _PAGE_SIZE)
    _pad(metadata, _PAGE_SIZE)
    with open(output, 'wb') as f:
        f.write(metadata + content)


def main(argv):
    outdir = os.path.realpath(argv[1])
    os.makedirs(outdir, exist_ok=True)
    output = os.path.join(outdir, 'test_readonly_fs_image.img')
    cwd = os.getcwd()
    with tempfile.TemporaryDirectory() as workdir:
        os.chdir(workdir)
        os.makedirs('test/dir/')
        files = ['test/a.odex', 'test/big.odex', 'test/b.odex',
                 'test/c0.odex', 'test/c.odex', 'test/dir/c.odex',
                 'test/dir/empty.odex']
        contents = [b'123\n', b'\0' * 90000 + b'X' * 10000, b'Z', b'', b'',
                    b'A', b'']
        for name, data in zip(files, contents):
            with open(name, 'wb') as f:
                f.write(data)
        # Each non-empty file takes its own pages; empty ones take none.
        pages = sum((len(d) + _PAGE_SIZE - 1) // _PAGE_SIZE for d in contents)
        expected_size = (pages + 1) * _PAGE_SIZE  # +1 for the metadata page.
        _generate_image(
            files,
            {'/test/symlink1': '/test/a.odex',
             '/test/symlink2': '/test/b.odex'},
            ['/test/emptydir'], ['/test/emptyfile'], output)
        os.chdir(cwd)
    actual = os.path.getsize(output)
    assert actual == expected_size, (actual, expected_size)
    print(output)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
