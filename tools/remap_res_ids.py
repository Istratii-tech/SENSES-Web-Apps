#!/usr/bin/env python3
"""Renumbers references to framework-res resources (topic userspace, HTC).

An app built against another build's framework-res (HTC's) refers to the
framework's resources (package 0x01) by that build's numbers: in its code
(constants, R.styleable arrays), in its resources.arsc (values, style
parents, bag keys) and in its compiled XML (attribute values and the
attribute resource map). Where our framework-res numbers a resource of the
same name differently, this rewrites the number to ours.

Usage:
  remap_res_ids.py map OTHER_IDS OUR_IDS MAP     write MAP ("0xOTHER 0xOURS"
      lines) from two "0xID type/name" dumps; an attr the other build made
      public may be our private attr ("^attr-private")
  remap_res_ids.py apk MAP IN.apk OUT.apk        resources.arsc and res/*.xml
  remap_res_ids.py smali MAP DIR                 const literals, in place
"""
import os
import re
import struct
import sys
import zipfile

RES_STRING_POOL = 0x0001
RES_TABLE = 0x0002
RES_XML = 0x0003
RES_XML_START_ELEMENT = 0x0102
RES_XML_RESOURCE_MAP = 0x0180
RES_TABLE_PACKAGE = 0x0200
RES_TABLE_TYPE = 0x0201
TYPE_REFERENCE = 0x01
TYPE_ATTRIBUTE = 0x02


def read_ids(path):
    """name -> id; "type/name" or "type name" per line after the id."""
    ids = {}
    for line in open(path):
        parts = line.split()
        if len(parts) == 2:
            rid, name = parts
        elif len(parts) == 3:
            rid, name = parts[0], parts[1] + '/' + parts[2]
        else:
            continue
        ids[name] = int(rid, 16)
    return ids


def make_map(other_file, ours_file, out_file):
    other, ours = read_ids(other_file), read_ids(ours_file)
    n = 0
    with open(out_file, 'w') as f:
        for name, rid in sorted(other.items(), key=lambda kv: kv[1]):
            mine = ours.get(name)
            if mine is None and name.startswith('attr/'):
                mine = ours.get('^attr-private/' + name[5:])
            if mine is not None and mine != rid:
                f.write('0x%08x 0x%08x %s\n' % (rid, mine, name))
                n += 1
    print('%s: %d renumbered' % (out_file, n))


def load_map(path):
    m = {}
    for line in open(path):
        parts = line.split()
        if len(parts) >= 2:
            m[int(parts[0], 16)] = int(parts[1], 16)
    return m


class Rewriter:
    def __init__(self, idmap):
        self.idmap = idmap
        self.count = 0

    def u32(self, buf, off):
        """Remaps the uint32 at OFF if it is a renumbered id."""
        v = struct.unpack_from('<I', buf, off)[0]
        n = self.idmap.get(v)
        if n is not None:
            struct.pack_into('<I', buf, off, n)
            self.count += 1

    def value(self, buf, off):
        """Res_value at OFF: size u16, res0 u8, dataType u8, data u32."""
        if buf[off + 3] in (TYPE_REFERENCE, TYPE_ATTRIBUTE):
            self.u32(buf, off + 4)

    def xml(self, buf):
        typ, hsize, size = struct.unpack_from('<HHI', buf, 0)
        if typ != RES_XML:
            return
        off = hsize
        while off < size:
            ctype, chsize, csize = struct.unpack_from('<HHI', buf, off)
            if ctype == RES_XML_RESOURCE_MAP:
                for p in range(off + chsize, off + csize, 4):
                    self.u32(buf, p)
            elif ctype == RES_XML_START_ELEMENT:
                ext = off + chsize
                astart, asize, acount = struct.unpack_from('<HHH', buf, ext + 8)
                for i in range(acount):
                    a = ext + astart + i * asize
                    self.value(buf, a + 12)
            if csize == 0:
                break
            off += csize

    def table(self, buf):
        typ, hsize, size = struct.unpack_from('<HHI', buf, 0)
        assert typ == RES_TABLE
        off = hsize
        while off < size:
            ctype, chsize, csize = struct.unpack_from('<HHI', buf, off)
            if ctype == RES_TABLE_PACKAGE:
                self.package(buf, off, chsize, csize)
            off += csize

    def package(self, buf, start, hsize, size):
        off = start + hsize
        end = start + size
        while off < end:
            ctype, chsize, csize = struct.unpack_from('<HHI', buf, off)
            if ctype == RES_TABLE_TYPE:
                self.type_chunk(buf, off, chsize)
            off += csize

    def type_chunk(self, buf, start, hsize):
        entry_count, entries_start = struct.unpack_from('<II', buf, start + 12)
        for i in range(entry_count):
            eo = struct.unpack_from('<I', buf, start + hsize + 4 * i)[0]
            if eo == 0xffffffff:
                continue
            e = start + entries_start + eo
            esize, flags = struct.unpack_from('<HH', buf, e)
            if flags & 0x0001:  # FLAG_COMPLEX: ResTable_map_entry
                self.u32(buf, e + 8)  # parent
                count = struct.unpack_from('<I', buf, e + 12)[0]
                m = e + esize
                for _ in range(count):
                    self.u32(buf, m)  # name: an attribute (or a style item's key)
                    self.value(buf, m + 4)
                    m += 12
            else:
                self.value(buf, e + esize)


def remap_apk(idmap, src, dst):
    rw = Rewriter(idmap)
    zin = zipfile.ZipFile(src)
    tmp = dst + '.tmp'
    with zipfile.ZipFile(tmp, 'w') as zout:
        for info in zin.infolist():
            data = zin.read(info.filename)
            if info.filename.startswith('META-INF/'):
                continue  # the signature no longer matches (and is not checked here)
            if info.filename == 'resources.arsc':
                buf = bytearray(data)
                rw.table(buf)
                data = bytes(buf)
            elif info.filename.endswith('.xml') and data[:2] == b'\x03\x00':
                buf = bytearray(data)
                rw.xml(buf)
                data = bytes(buf)
            out = zipfile.ZipInfo(info.filename, info.date_time)
            out.compress_type = info.compress_type
            out.external_attr = info.external_attr
            zout.writestr(out, data)
    os.replace(tmp, dst)
    print('%s: %d references renumbered' % (dst, rw.count))


LITERAL = re.compile(r'\b0x([0-9a-f]{7,8})(?![0-9a-fA-F])')


def remap_smali(idmap, root):
    count = 0
    for d, _, files in os.walk(root):
        for f in files:
            if not f.endswith('.smali'):
                continue
            path = os.path.join(d, f)
            text = open(path).read()

            def sub(m):
                nonlocal count
                v = int(m.group(1), 16)
                n = idmap.get(v)
                if n is None:
                    return m.group(0)
                count += 1
                return '0x%x' % n
            new = LITERAL.sub(sub, text)
            if new != text:
                open(path, 'w').write(new)
    print('%s: %d constants renumbered' % (root, count))


def main(argv):
    if len(argv) == 5 and argv[1] == 'map':
        make_map(argv[2], argv[3], argv[4])
    elif len(argv) == 5 and argv[1] == 'apk':
        remap_apk(load_map(argv[2]), argv[3], argv[4])
    elif len(argv) == 4 and argv[1] == 'smali':
        remap_smali(load_map(argv[2]), argv[3])
    else:
        sys.stderr.write(__doc__)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
