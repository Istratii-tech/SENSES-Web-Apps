#!/usr/bin/env python3
"""Checks every .so of a directory (an APK's lib/ABI/) for imports the NDK
bridge cannot resolve: runs `ndk_unit_test.js --check-imports` on each
library (with its DT_NEEDED, as the dynamic linker would), in parallel, and
sorts what is left into

  * system symbols without a thunk (tests/ndk/android-system-symbols.txt
    says what Android's system libraries export up to 6.0, and
    tests/ndk/android81-ndk-symbols.txt what 8.1's public NDK libraries
    export, with the API level shown) -- the bridge's to fix;
  * other unresolved symbols: they would come from a library of the app
    that is not in the directory (downloaded later, or packed), or are the
    app's own mistakes;
  * files that are not ELF libraries (packed libraries, zip archives).

  tools/ndk_check_all_imports.py DIR [--jobs N] [--verbose]

Exit status: the number of distinct system symbols without a thunk.
"""
import argparse
import collections
import concurrent.futures
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST = os.path.join(ROOT, 'build', 'ndk', 'ndk_unit_test.js')
SYSTEM = os.path.join(ROOT, 'tests', 'ndk', 'android-system-symbols.txt')
SYSTEM81 = os.path.join(ROOT, 'tests', 'ndk', 'android81-ndk-symbols.txt')


def system81():
    """{symbol: (library, API level on ARM)} of 8.1's public NDK libraries."""
    out = {}
    for line in open(SYSTEM81):
        f = line.split()
        if line.startswith('#') or len(f) < 3 or not f[2].isdigit():
            continue  # platform-private, VNDK-only or not on 32-bit ARM
        out[f[0]] = (f[1], int(f[2]))
    return out


def check(path):
    out = subprocess.run(['node', TEST, '--check-imports', path], capture_output=True,
                         text=True).stdout
    unresolved, needed, error = [], [], None
    for line in out.splitlines():
        if 'cannot load' in line:
            error = line.split('cannot load: ', 1)[-1]
        m = re.match(r'  \(missing DT_NEEDED (\S+)\)', line)
        if m:
            needed.append(m.group(1))
        elif line.startswith('  '):
            unresolved.append(line.strip())
    return os.path.basename(path), unresolved, needed, error


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dir')
    ap.add_argument('--jobs', type=int, default=8)
    ap.add_argument('--verbose', action='store_true')
    args = ap.parse_args()
    levels = system81()
    system = set(open(SYSTEM).read().split()) | set(levels)
    system_libs = {lib for lib, _ in levels.values()}
    libs = sorted(os.path.join(args.dir, f) for f in os.listdir(args.dir) if f.endswith('.so'))
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as ex:
        results = list(ex.map(check, libs))
    missing = collections.defaultdict(list)  # system symbol -> libraries
    other = collections.defaultdict(list)    # library -> unresolved non-system symbols
    absent = collections.defaultdict(list)   # absent DT_NEEDED -> libraries
    not_elf = []
    for name, unresolved, needed, error in results:
        if error:
            not_elf.append((name, error))
            continue
        for s in unresolved:
            (missing[s].append(name) if s in system else other[name].append(s))
        for n in needed:
            absent[n].append(name)
    loaded = len(results) - len(not_elf)
    print('%d files, %d ELF libraries loaded' % (len(results), loaded))
    print('\n== system symbols without a thunk: %d' % len(missing))
    for s in sorted(missing):
        level = ' API %d %s,' % (levels[s][1], levels[s][0]) if s in levels else ''
        print('  %s (%s %s)' % (s, level, ' '.join(missing[s])))
    print('\n== libraries the directory lacks (DT_NEEDED): %d' % len(absent))
    for n in sorted(absent):
        print('  %s%s, needed by %s' % (n, ' (an 8.1 system library)' if n in system_libs
                                         else '', ' '.join(sorted(absent[n]))))
    print('\n== other unresolved symbols (from those libraries, or the app\'s own): %d in %d '
          'libraries' % (sum(len(v) for v in other.values()), len(other)))
    for lib in sorted(other):
        syms = other[lib]
        shown = syms if args.verbose else syms[:6]
        print('  %s: %d%s %s' % (lib, len(syms), '' if args.verbose else ',', ' '.join(shown)))
    print('\n== not ELF libraries: %d' % len(not_elf))
    for name, error in not_elf:
        with open(os.path.join(args.dir, name), 'rb') as f:
            magic = f.read(4)
        if magic == b'PK\x03\x04':
            kind = 'a zip archive'
        elif magic != b'\x7fELF':
            kind = 'packed (%r), unpacked by the app at run time' % magic
        else:
            kind = error
        print('  %s: %s' % (name, kind))
    return min(len(missing), 255)


if __name__ == '__main__':
    sys.exit(main())
