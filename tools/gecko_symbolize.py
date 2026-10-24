#!/usr/bin/env python3
"""Names Gecko's functions in guest stacks of a log (topic multiproc).

The NDK bridge prints guest code as "lib.so!sym+0xOFF" (the nearest exported
symbol: libxul is stripped, so mostly meaningless) or "lib.so+0xOFF". This
tool rewrites libxul.so frames with Mozilla's Breakpad symbols of that exact
build: "XUL[nsAppShell::ProcessNextNativeEvent(bool)+0x27e]".

  tools/gecko_symbolize.py fetch LIBXUL.so DIR   # once: DIR/xul.funcs, DIR/*.dyn
  tools/gecko_symbolize.py LOG DIR > LOG.sym     # rewrite a log (or a dump)

fetch reads the GNU build-id of LIBXUL.so (from the APK's
lib/armeabi-v7a/), downloads https://symbols.mozilla.org/libxul.so/ID/libxul.so.sym
(gzip, ~160 MB; Firefox release builds are there) and keeps only the
function table (~50 MB), plus the dynamic symbols of libxul and libmozglue
(next to LIBXUL.so) to turn "sym+off" back into addresses. Needs llvm-nm
(emsdk's) on PATH or in ~/emsdk/upstream/bin.
"""

import bisect
import gzip
import os
import re
import shutil
import struct
import subprocess
import sys
import urllib.request


def build_id(path):
    with open(path, 'rb') as f:
        data = f.read(1 << 20)
    i = data.find(b'GNU\x00')
    while i >= 12:
        namesz, descsz, typ = struct.unpack_from('<III', data, i - 12)
        if namesz == 4 and typ == 3:
            return data[i + 4:i + 4 + descsz]
        i = data.find(b'GNU\x00', i + 1)
    sys.exit('%s: no GNU build-id' % path)


def debug_id(bid):
    """Breakpad's module id: the build-id's first 16 bytes as a GUID, then 0."""
    b = bid[:16].ljust(16, b'\0')
    guid = struct.pack('>IHH', *struct.unpack('<IHH', b[:8])) + b[8:]
    return guid.hex().upper() + '0'


def nm():
    for d in os.environ.get('PATH', '').split(os.pathsep) + [os.path.expanduser('~/emsdk/upstream/bin')]:
        p = os.path.join(d, 'llvm-nm')
        if os.access(p, os.X_OK):
            return p
    sys.exit('llvm-nm not found')


def fetch(libxul, out):
    os.makedirs(out, exist_ok=True)
    did = debug_id(build_id(libxul))
    url = 'https://symbols.mozilla.org/libxul.so/%s/libxul.so.sym' % did
    print('fetching', url, file=sys.stderr)
    raw = os.path.join(out, 'libxul.so.sym.gz')
    with urllib.request.urlopen(url, timeout=600) as r, open(raw, 'wb') as f:
        shutil.copyfileobj(r, f)
    with open(raw, 'rb') as f:
        gz = f.read(2) == b'\x1f\x8b'
    opener = gzip.open if gz else open
    with opener(raw, 'rt', errors='replace') as src, open(os.path.join(out, 'xul.funcs'), 'w') as dst:
        for line in src:
            p = line.split(' ')
            if p[0] == 'FUNC':
                if p[1] == 'm':  # "FUNC m addr size param name"
                    p = p[:1] + p[2:]
                dst.write('%s %s\n' % (p[1], ' '.join(p[4:]).rstrip('\n')))
            elif p[0] == 'PUBLIC':
                if p[1] == 'm':  # "PUBLIC m addr param name"
                    p = p[:1] + p[2:]
                dst.write('%s %s\n' % (p[1], ' '.join(p[3:]).rstrip('\n')))
    os.remove(raw)
    for lib in ('libxul', 'libmozglue'):
        so = os.path.join(os.path.dirname(libxul), lib + '.so')
        if os.path.exists(so):
            with open(os.path.join(out, lib + '.dyn'), 'w') as f:
                subprocess.run([nm(), '-D', '--defined-only', so], stdout=f, check=True)


def symbolize(log, d):
    dyn = {}
    for lib in ('libxul', 'libmozglue'):
        m = {}
        path = os.path.join(d, lib + '.dyn')
        if os.path.exists(path):
            for line in open(path):
                p = line.split()
                if len(p) >= 3:
                    m[p[2].split('@')[0]] = int(p[0], 16)
        dyn[lib + '.so'] = m
    funcs = []
    for line in open(os.path.join(d, 'xul.funcs'), errors='replace'):
        a, _, n = line.rstrip('\n').partition(' ')
        funcs.append((int(a, 16), n))
    funcs.sort()
    addrs = [a for a, _ in funcs]

    def sub(m):
        lib, sym, off = m.group(1), m.group(2), int(m.group(3), 16)
        if sym is None:
            v = off
        elif sym in dyn.get(lib, {}):
            v = dyn[lib][sym] + off
        else:
            return m.group(0)
        v &= ~1
        if lib == 'libxul.so':
            i = bisect.bisect_right(addrs, v) - 1
            if i >= 0:
                return 'XUL[%s+0x%x]' % (funcs[i][1][:120], v - funcs[i][0])
        return '%s+0x%x' % (lib, v)

    pat = re.compile(r'(lib[\w.]+\.so)(?:!([^\s+]+))?\+0x([0-9a-f]+)')
    for line in open(log, errors='replace'):
        sys.stdout.write(pat.sub(sub, line))


if __name__ == '__main__':
    if len(sys.argv) == 4 and sys.argv[1] == 'fetch':
        fetch(sys.argv[2], sys.argv[3])
    elif len(sys.argv) == 3:
        symbolize(sys.argv[1], sys.argv[2])
    else:
        sys.exit(__doc__)
