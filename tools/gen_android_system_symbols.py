#!/usr/bin/env python3
"""Generates tests/ndk/android-system-symbols.txt: the names Android's system
libraries export to NDK code (32-bit ARM), for telling a system import
apart from a symbol of an app library that is missing.

Sources: bionic 6.0.1's libc.map, libm.map, libdl.map (entries for arm),
and the NDK's stub libraries of android-18 (libEGL, libGLESv1_CM/v2/v3,
libOpenSLES, libOpenMAXAL, libandroid, libjnigraphics, liblog, libz,
libstdc++ ...), plus a few later additions apps of API 21+ use.

  tools/gen_android_system_symbols.py BIONIC60_ARCHIVE NDK_ARCHIVE LLVM_NM \\
      > tests/ndk/android-system-symbols.txt
"""
import os
import re
import subprocess
import sys
import tarfile
import tempfile

EXTRA = ['AAsset_getLength64', 'AAsset_getRemainingLength64', 'AAsset_seek64',
         'AAsset_openFileDescriptor64', '__android_log_buf_print']


def main():
    bionic, ndk, nm = sys.argv[1:4]
    names = set(EXTRA)
    with tarfile.open(bionic) as t:
        for m in ('libc/libc.map', 'libm/libm.map', 'libdl/libdl.map'):
            for line in t.extractfile(m).read().decode().splitlines():
                mm = re.match(r'\s+([A-Za-z_][A-Za-z0-9_]*);\s*(#\s*(.*))?$', line)
                if mm and (not mm.group(3) or 'arm' in mm.group(3).split()):
                    names.add(mm.group(1))
    prefix = '9/platforms/android-18/arch-arm/usr/lib/'
    with tempfile.TemporaryDirectory() as d, tarfile.open(ndk) as t:
        for m in t.getmembers():
            if m.name.startswith(prefix) and m.name.endswith('.so'):
                path = os.path.join(d, os.path.basename(m.name))
                with open(path, 'wb') as f:
                    f.write(t.extractfile(m).read())
                out = subprocess.run([nm, '-D', '--defined-only', path], capture_output=True,
                                     text=True).stdout
                names.update(l.split()[-1].split('@')[0] for l in out.splitlines() if l.strip())
    for n in sorted(names):
        print(n)


if __name__ == '__main__':
    main()
