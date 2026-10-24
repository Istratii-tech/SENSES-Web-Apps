#!/usr/bin/env python3
"""C# method names for the code of a Unity il2cpp game (armeabi-v7a).

il2cpp builds ship libil2cpp.so stripped; the names are in
assets/bin/Data/Managed/Metadata/global-metadata.dat, and each assembly's
Il2CppCodeGenModule in the library maps method tokens to code. This finds
those modules by their names (a pointer to "Assembly-CSharp.dll" in the
data segment, followed by the method count and the method pointer table),
so no Il2CppDumper is needed. Metadata version 27 (Unity 2020.2-2021.1)
only: the record layouts of other versions differ.

  il2cpp_syms.py APK find REGEX       addresses of methods matching REGEX
  il2cpp_syms.py APK types REGEX      a type's fields, methods and nested types
  il2cpp_syms.py APK dis REGEX        disassembly (objdump) with call targets named
  il2cpp_syms.py APK annotate < LOG   "libil2cpp.so+0xADDR" in a log -> method+offset
  il2cpp_syms.py APK strings REGEX    string literals of the C# code

APK may be the .apk or a directory holding libil2cpp.so and
global-metadata.dat. Addresses are file virtual addresses, as objdump and
the NDK bridge's reports (debug.ndk.watch_pc, guest stacks) print them.
Found with it: what Subway Surfers' bootstrap waited for
(docs/topics/install8.md).
"""

import bisect
import os
import re
import struct
import subprocess
import sys
import tempfile
import zipfile


class Metadata:
    def __init__(self, data):
        self.d = d = data
        magic, self.version = struct.unpack_from('<Ii', d, 0)
        if magic != 0xfab11baf:
            raise ValueError('not il2cpp metadata')
        if self.version != 27:
            raise ValueError('metadata version %d: only 27 is known here' % self.version)
        h = struct.unpack_from('<64i', d, 8)
        sec = lambda i: (h[2 * i], h[2 * i + 1])
        self.lit, self.litdata, self.str = sec(0), sec(1), sec(2)
        self.methods = sec(5)
        self.fields, self.nested = sec(11), sec(15)
        self.typedefs, self.images = sec(19), sec(20)
        self.tsize = 88  # Il2CppTypeDefinition (v27: no byrefTypeIndex)
        self.types = [self._type(i) for i in range(self.typedefs[1] // self.tsize)]
        self.parent = {}
        for t in self.types:
            for n in self.nested_of(t):
                self.parent[n] = t['idx']

    def s(self, i):
        o = self.str[0] + i
        return self.d[o:self.d.index(b'\0', o)].decode('utf8', 'replace')

    def _type(self, i):
        v = struct.unpack_from('<16i8H', self.d, self.typedefs[0] + self.tsize * i)
        return dict(idx=i, name=self.s(v[0]), ns=self.s(v[1]), field_start=v[8],
                    method_start=v[9], nested_start=v[12],
                    method_count=v[16], field_count=v[18], nested_count=v[20])

    def full_name(self, t):
        p = self.parent.get(t['idx'])
        if p is not None:
            return self.full_name(self.types[p]) + '/' + t['name']
        return (t['ns'] + '.' if t['ns'] else '') + t['name']

    def methods_of(self, t):
        for k in range(t['method_count']):  # Il2CppMethodDefinition: 32 bytes
            v = struct.unpack_from('<6i4H', self.d, self.methods[0] + 32 * (t['method_start'] + k))
            yield self.s(v[0]), v[5]  # name, token

    def fields_of(self, t):
        for k in range(t['field_count']):
            yield self.s(struct.unpack_from('<i', self.d, self.fields[0] + 12 * (t['field_start'] + k))[0])

    def nested_of(self, t):
        for k in range(t['nested_count']):
            yield struct.unpack_from('<i', self.d, self.nested[0] + 4 * (t['nested_start'] + k))[0]

    def image_list(self):
        for i in range(self.images[1] // 40):
            v = struct.unpack_from('<10i', self.d, self.images[0] + 40 * i)
            yield self.s(v[0]), v[2], v[3]  # name, first type, type count

    def literals(self):
        for i in range(self.lit[1] // 8):
            n, off = struct.unpack_from('<II', self.d, self.lit[0] + 8 * i)
            o = self.litdata[0] + off
            yield self.d[o:o + n].decode('utf8', 'replace')


class Elf32:
    def __init__(self, data):
        self.b = b = data
        if b[:4] != b'\x7fELF' or b[4] != 1:
            raise ValueError('not a 32-bit ELF')
        phoff, = struct.unpack_from('<I', b, 28)
        phentsize, phnum = struct.unpack_from('<HH', b, 42)
        self.loads = []
        for i in range(phnum):
            t, off, va, _, filesz = struct.unpack_from('<IIIII', b, phoff + i * phentsize)
            if t == 1:
                self.loads.append((va, off, filesz))

    def off(self, va):
        for v, o, n in self.loads:
            if v <= va < v + n:
                return o + va - v
        return None

    def va(self, off):
        for v, o, n in self.loads:
            if o <= off < o + n:
                return v + off - o
        return None

    def u32(self, va):
        o = self.off(va)
        return None if o is None or o + 4 > len(self.b) else struct.unpack_from('<I', self.b, o)[0]

    def data_ranges(self):
        return [(v, v + n) for v, o, n in self.loads if v != 0]


def code_gen_module(elf, name):
    """(method count, method pointer table) of the module named |name|."""
    want = name.encode() + b'\0'
    data = elf.data_ranges()
    o = elf.b.find(want)
    while o >= 0:
        if o == 0 or elf.b[o - 1] == 0:
            p = struct.pack('<I', elf.va(o) or 0)
            r = elf.b.find(p)
            while r >= 0:
                va = elf.va(r)
                cnt, table = (elf.u32(va + 4), elf.u32(va + 8)) if va else (None, None)
                if cnt and table and 0 < cnt < 500000 and any(
                        lo <= table and table + 4 * cnt <= hi for lo, hi in data):
                    return cnt, table
                r = elf.b.find(p, r + 1)
        o = elf.b.find(want, o + 1)
    return None


def load(path):
    if os.path.isdir(path):
        lib = open(os.path.join(path, 'libil2cpp.so'), 'rb').read()
        meta = open(os.path.join(path, 'global-metadata.dat'), 'rb').read()
    else:
        with zipfile.ZipFile(path) as z:
            lib = z.read('lib/armeabi-v7a/libil2cpp.so')
            meta = z.read('assets/bin/Data/Managed/Metadata/global-metadata.dat')
    return Elf32(lib), Metadata(meta), lib


def method_map(elf, md):
    addr = {}
    for name, first, count in md.image_list():
        mod = code_gen_module(elf, name)
        if not mod:
            continue
        cnt, table = mod
        for t in md.types[first:first + count]:
            full = md.full_name(t)
            for m, token in md.methods_of(t):
                rid = token & 0xffffff
                if 1 <= rid <= cnt:
                    a = elf.u32(table + 4 * (rid - 1))
                    if a:
                        addr.setdefault(a & ~1, full + '::' + m)
    return addr


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__)
    elf, md, lib = load(argv[1])
    cmd, arg = argv[2], argv[3] if len(argv) > 3 else ''
    if cmd == 'types':
        for t in md.types:
            if t['idx'] not in md.parent and re.search(arg, md.full_name(t)):
                show(md, t, 0)
        return
    if cmd == 'strings':
        for i, s in enumerate(md.literals()):
            if re.search(arg, s):
                print(i, repr(s[:200]))
        return
    addr = method_map(elf, md)
    keys = sorted(addr)

    def name(a):
        i = bisect.bisect_right(keys, a) - 1
        if i < 0 or a - keys[i] >= 0x4000:
            return None
        return '%s+0x%x' % (addr[keys[i]], a - keys[i])

    if cmd == 'find':
        for a in keys:
            if re.search(arg, addr[a]):
                print('0x%x %s' % (a, addr[a]))
    elif cmd == 'annotate':
        for line in sys.stdin:
            line = line.rstrip('\n')
            m = re.search(r'libil2cpp\.so\+0x([0-9a-f]+)', line)
            n = m and name(int(m.group(1), 16) & ~1)
            print(line + ('   <%s>' % n if n else ''))
    elif cmd == 'dis':
        with tempfile.NamedTemporaryFile(suffix='.so') as f:
            f.write(lib)
            f.flush()
            for a in keys:
                if not re.search(arg, addr[a]):
                    continue
                i = bisect.bisect_right(keys, a)
                end = min(keys[i] if i < len(keys) else a + 0x400, a + 0x2000)
                print('=====', addr[a], hex(a))
                out = subprocess.run(['objdump', '-d', '--triple=armv7', '--start-address=%#x' % a,
                                      '--stop-address=%#x' % end, f.name],
                                     capture_output=True, text=True).stdout
                for line in out.splitlines():
                    if not re.match(r'\s+[0-9a-f]+:', line):
                        continue
                    m = re.search(r'\bblx?\s+0x([0-9a-f]+)', line)
                    if m and int(m.group(1), 16) in addr:
                        line += '   ; ' + addr[int(m.group(1), 16)]
                    print(line)
    else:
        sys.exit(__doc__)


def show(md, t, depth):
    pad = '  ' * depth
    print(pad + 'class ' + md.full_name(t))
    print(pad + '  fields: ' + ', '.join(md.fields_of(t)))
    for m, token in md.methods_of(t):
        print(pad + '  %s  (token %08x)' % (m, token))
    if depth < 2:
        for n in md.nested_of(t):
            show(md, md.types[n], depth + 1)


if __name__ == '__main__':
    main(sys.argv)
