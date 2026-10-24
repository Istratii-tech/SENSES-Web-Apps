#!/usr/bin/env python3
"""Generates tests/ndk/android81-ndk-symbols.txt: what Android 8.1's system
libraries export to NDK code, with the API level each symbol appeared at on
32-bit ARM and on AArch64. tools/apk_survey.py uses it to tell which imports
of an app's native libraries are new in API 24-27 (what an 8.1 bridge must
add) or newer than 8.1 (the app must look them up with dlsym).

Sources: the *.map.txt version scripts of android-8.1.0_r81 (bionic, liblog,
libsync, libandroid, libjnigraphics, EGL/GLES, Vulkan, libnativewindow,
libmediandk, libcamera2ndk, libaaudio, OpenSL ES, OpenMAX AL), read straight
from the archives of tools/fetch_aosp81.sh.

  tools/gen_ndk81_symbols.py AOSP81_ARCHIVES > tests/ndk/android81-ndk-symbols.txt

Line format: SYMBOL LIBRARY ARM ARM64, where ARM/ARM64 is the API level, "-"
(not on that architecture), "P" (platform-private: LIBC_PRIVATE and the like,
reachable by legacy 32-bit apps only) or "V" (vendor/VNDK only).
"""
import os
import re
import sys
import tarfile

# archive (relative to AOSP81_ARCHIVES) -> {member: (library, the API level it appeared at)}
SOURCES = {
    'bionic.tar.gz': {
        'libc/libc.map.txt': ('libc.so', 3),
        'libm/libm.map.txt': ('libm.so', 3),
        'libdl/libdl.map.txt': ('libdl.so', 3),
        'libc/libstdc++.map.txt': ('libstdc++.so', 3),
    },
    'system/core.tar.gz': {
        'liblog/liblog.map.txt': ('liblog.so', 3),
        'libsync/libsync.map.txt': ('libsync.so', 26),
    },
    'frameworks/base.tar.gz': {
        'native/android/libandroid.map.txt': ('libandroid.so', 9),
        'native/graphics/jni/libjnigraphics.map.txt': ('libjnigraphics.so', 8),
    },
    'frameworks/native.tar.gz': {
        'opengl/libs/libEGL.map.txt': ('libEGL.so', 9),
        'opengl/libs/libGLESv1_CM.map.txt': ('libGLESv1_CM.so', 4),
        'opengl/libs/libGLESv2.map.txt': ('libGLESv2.so', 5),
        'opengl/libs/libGLESv3.map.txt': ('libGLESv3.so', 18),
        'vulkan/libvulkan/libvulkan.map.txt': ('libvulkan.so', 24),
        'libs/nativewindow/libnativewindow.map.txt': ('libnativewindow.so', 26),
    },
    'frameworks/av.tar.gz': {
        'media/ndk/libmediandk.map.txt': ('libmediandk.so', 21),
        'camera/ndk/libcamera2ndk.map.txt': ('libcamera2ndk.so', 24),
        'media/libaaudio/libaaudio.map.txt': ('libaaudio.so', 26),
    },
    'frameworks/wilhelm.tar.gz': {
        'libOpenSLES.map.txt': ('libOpenSLES.so', 9),
        'libOpenMAXAL.map.txt': ('libOpenMAXAL.so', 14),
    },
}
ARCHES = ('arm', 'arm64', 'mips', 'mips64', 'x86', 'x86_64')
PRIVATE_TAGS = ('platform-only', 'future')


def level(tags, arch):
    """The level a tag list gives for ARCH: None if it says nothing, '-' if
    the list names architectures and not this one."""
    named = [t for t in tags if t in ARCHES]
    if named and arch not in named:
        return '-'
    for t in tags:
        if t.startswith('introduced-%s=' % arch):
            return int(t.split('=')[1])
    for t in tags:
        if t.startswith('introduced='):
            return int(t.split('=')[1])
    return None


def parse(text, lib, base, out):
    block, block_tags = None, []
    for line in text.splitlines():
        code, _, comment = line.partition('#')
        tags = comment.split()
        code = code.strip()
        m = re.match(r'([A-Z0-9_]+)\s*\{', code)
        if m:
            block, block_tags = m.group(1), tags
            continue
        if code.startswith('}'):
            block = None
            continue
        m = re.match(r'([A-Za-z_][A-Za-z0-9_]*);$', code)
        if not m or block is None:
            continue
        sym = m.group(1)
        levels = []
        for arch in ('arm', 'arm64'):
            v = level(tags, arch)
            b = level(block_tags, arch)
            if v == '-' or b == '-':
                levels.append('-')
                continue
            if block.endswith(('_PRIVATE', '_PLATFORM', '_DEPRECATED')) or \
                    any(t in PRIVATE_TAGS for t in tags + block_tags):
                levels.append('P')
            elif 'vndk' in tags or 'vndk' in block_tags:
                levels.append('V')
            else:
                n = v if v is not None else b if b is not None else base
                if arch == 'arm64':
                    n = max(n, 21)
                levels.append(str(n))
        old = out.get((sym, lib))
        if old:  # listed twice (another block): keep the more available entry
            levels = [min(a, b, key=rank) for a, b in zip(old, levels)]
        out[(sym, lib)] = levels


def rank(v):
    return int(v) if v.isdigit() else {'P': 1000, 'V': 1001, '-': 1002}[v]


def main():
    root = sys.argv[1]
    out = {}
    for archive, members in sorted(SOURCES.items()):
        left = dict(members)
        with tarfile.open(os.path.join(root, archive), 'r|gz') as t:
            for m in t:
                if m.name in left:
                    lib, base = left.pop(m.name)
                    parse(t.extractfile(m).read().decode(), lib, base, out)
                    if not left:
                        break
        if left:
            sys.exit('%s: missing %s' % (archive, ' '.join(left)))
    print('# Generated by tools/gen_ndk81_symbols.py from android-8.1.0_r81 *.map.txt.')
    print('# SYMBOL LIBRARY API-ARM API-ARM64  (- = not there, P = platform-private, V = VNDK)')
    for (sym, lib), (arm, arm64) in sorted(out.items(), key=lambda kv: (kv[0][1], kv[0][0])):
        print(sym, lib, arm, arm64)


if __name__ == '__main__':
    main()
