#!/usr/bin/env python3
"""Packs a SENSES Web Apps release to run on one's own computer (topic
local-release, docs/topics/local-release.md): the site the release target
built (SENSES_STACKS of its own, no __test/__sweep/vm/), the server
(tools/release + tools/serve.py + tools/net_relay.py), the start scripts,
READMEs, NOTICE.txt and the components' licence files, as one zip.

  make_release.py --site DIR --variant NAME --version V --out DIR [--emsdk DIR]

Nothing from Google and no APK goes in: the check below refuses .apk/.apks
files and the folders of tests.
"""

import argparse
import hashlib
import json
import os
import shutil
import sys
import time
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RELEASE = os.path.join(ROOT, 'tools', 'release')

SERVER = [('tools/release/senses.py', 'senses.py'), ('tools/release/certs.py', 'certs.py'),
          ('tools/release/qr.py', 'qr.py'), ('tools/release/mdns.py', 'mdns.py'),
          ('tools/release/setup/index.html', 'setup/index.html'), ('tools/release/setup/probe-sw.js', 'setup/probe-sw.js'),
          ('tools/serve.py', 'serve.py'), ('tools/net_relay.py', 'net_relay.py')]
STARTERS = [('start-macos.command', 'Start SENSES.command'), ('start-windows.bat', 'Start SENSES.bat'),
            ('start-linux.sh', 'start-senses.sh')]
DOCS = ['README.txt', 'README-ru.txt']
FORBIDDEN_EXT = ('.apk', '.apks', '.xapk', '.apkm', '.obb')
FORBIDDEN_DIRS = ('__test', '__sweep', '__probe', 'vm')

STACK_SOURCES = {
    'a10': ('aosp10', 'Android 10', 'android-10.0.0_r47'),
    'a81': ('aosp81', 'Android 8.1', 'android-8.1.0_r81'),
    'a60': ('aosp60', 'Android 6.0.1', 'android-6.0.1_r81'),
    'a44': ('aosp44', 'Android 4.4', 'android-4.4.4_r2'),
}
LICENSE_FILES = ('NOTICE', 'LICENSE', 'LICENSE.txt', 'LICENSE.md', 'LICENCE', 'COPYING', 'COPYRIGHT')


def aosp_licenses(tree):
    """(component path, licence kind, [licence files]) of an AOSP tree: every
    project folder down to three levels that has a NOTICE/LICENSE/COPYING
    or an AOSP MODULE_LICENSE_* marker."""
    out = []
    base = os.path.join(ROOT, 'third_party', tree)
    for dirpath, dirnames, filenames in os.walk(base):
        rel = os.path.relpath(dirpath, base)
        depth = 0 if rel == '.' else rel.count(os.sep) + 1
        dirnames[:] = sorted(d for d in dirnames if not d.startswith('.') and d not in ('tests', 'test'))
        if depth >= 3:
            dirnames[:] = []
        files = [f for f in sorted(filenames) if f in LICENSE_FILES]
        kinds = [f[len('MODULE_LICENSE_'):] for f in filenames if f.startswith('MODULE_LICENSE_')]
        if files or kinds:
            out.append((rel, ', '.join(sorted(kinds)) or '-', files))
    return out


def copy_licenses(stage, stacks, emsdk):
    lic = os.path.join(stage, 'licenses')
    rows = []
    for s in stacks:
        tree = STACK_SOURCES[s][0]
        for rel, kind, files in aosp_licenses(tree):
            for f in files:
                dst = os.path.join(lic, tree, rel, f)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                shutil.copyfile(os.path.join(ROOT, 'third_party', tree, rel, f), dst)
            rows.append((tree + '/' + rel, kind))
    extra = [
        ('upstream/src/LICENSE', 'arc/LICENSE'), ('upstream/src/NOTICE', 'arc/NOTICE'),
        ('upstream/mods/graphics_translation/NOTICE', 'arc/graphics_translation/NOTICE'),
        ('third_party/vulkan-headers/LICENSE.md', 'vulkan-headers/LICENSE.md'),
        ('tools/release/licenses/dawn-tint.LICENSE', 'dawn-tint/LICENSE'),
        ('tools/release/licenses/spirv-tools.LICENSE', 'spirv-tools/LICENSE'),
        ('tools/release/licenses/spirv-headers.LICENSE', 'spirv-headers/LICENSE'),
    ]
    if emsdk:
        em = os.path.join(emsdk, 'upstream', 'emscripten')
        extra += [(os.path.join(em, 'LICENSE'), 'emscripten/LICENSE'),
                  (os.path.join(em, 'system/lib/libc/musl/COPYRIGHT'), 'emscripten/musl/COPYRIGHT'),
                  (os.path.join(em, 'system/lib/libcxx/LICENSE.TXT'), 'emscripten/libcxx/LICENSE.TXT'),
                  (os.path.join(em, 'system/lib/libcxxabi/LICENSE.TXT'), 'emscripten/libcxxabi/LICENSE.TXT'),
                  (os.path.join(em, 'system/lib/compiler-rt/LICENSE.TXT'), 'emscripten/compiler-rt/LICENSE.TXT')]
    missing = []
    for src, dst in extra:
        src = os.path.join(ROOT, src)
        if not os.path.exists(src):
            missing.append(src)
            continue
        os.makedirs(os.path.dirname(os.path.join(lic, dst)), exist_ok=True)
        shutil.copyfile(src, os.path.join(lic, dst))
    if missing:
        sys.exit('licence files missing:\n  ' + '\n  '.join(missing))
    return rows


NOTICE_HEAD = """SENSES Web Apps {version} ({variant})
====================================================

SENSES Web Apps runs Android apps in a web browser. This package holds the
web app (site/), a small server to run it on your own computer (server/), and
the systems it runs apps on: {systems}, compiled to WebAssembly.

What is NOT in this package: no Android apps (APK files) and nothing from
Google (no Google Play services, no Play Store, no Google apps). Apps are the
user's own files. microG or Google Play services can be added by the user, from
the user's own files, in the app's menu (Google services); none are included.

Components and their licences
-----------------------------
The full licence texts and notices are in licenses/, in folders named after
the components' source folders.

* The Android Open Source Project (AOSP), {tags}: the Android framework, ART/
  Dalvik, the core libraries, bionic and the system libraries. Apache License
  2.0, except as noted in licenses/aospNN/: e.g. libcore's OpenJDK-derived parts
  (GNU GPL v2 with the Classpath Exception), ICU (Unicode licence), BoringSSL
  (OpenSSL/ISC), Bouncy Castle (MIT), zlib, libpng, libjpeg-turbo, FreeType
  (FreeType licence), HarfBuzz (MIT), Skia (BSD), SQLite (public domain), Expat
  (MIT), the Noto fonts (SIL Open Font License 1.1) and the Roboto fonts (Apache
  2.0), tzdata (public domain).
* ARC, App Runtime for Chrome (posix_translation, graphics_translation):
  BSD-style licence of The Chromium Authors, and Apache 2.0 (licenses/arc/).
* Emscripten runtime, musl libc, libc++, libc++abi and compiler-rt (MIT,
  University of Illinois/NCSA and Apache 2.0 with LLVM exception:
  licenses/emscripten/).
* Tint from Dawn (BSD 3-Clause), SPIRV-Tools (Apache 2.0), SPIR-V Headers and
  the Vulkan headers (MIT/Apache 2.0).
* The SENSES Web Apps shell (site/*.html, *.js), the WebAssembly ports and
  the server (server/): SENSES Web Apps' own code, under the GNU Affero General
  Public License version 3 (LICENSE.txt). Its source: the repository below.

Source code
-----------
The AOSP parts are built from the AOSP tags above (https://android.googlesource.com)
with the patches in the SENSES Web Apps repository
(https://github.com/Istratii-tech/SENSES-Web-Apps, patches/), which also holds
the rest of the source. The parts under the GNU GPL with the Classpath
Exception (libcore's ojluni of Android 8.1 and 10), exactly as built, are the
release's file SENSES-Web-Apps-{version}-gpl-sources.tar.gz.

AOSP projects in this package (folder: licence marker)
------------------------------------------------------
"""


def write_notice(stage, version, variant, stacks, rows):
    systems = ', '.join(STACK_SOURCES[s][1] for s in stacks)
    tags = ', '.join(STACK_SOURCES[s][2] for s in stacks)
    with open(os.path.join(stage, 'NOTICE.txt'), 'w', newline='\r\n') as f:
        f.write(NOTICE_HEAD.format(version=version, variant=variant, systems=systems, tags=tags))
        for path, kind in rows:
            f.write('%-60s %s\n' % (path, kind))


def copy_site(src, dst, version):
    for dirpath, dirnames, filenames in os.walk(src):
        rel = os.path.relpath(dirpath, src)
        dirnames[:] = sorted(d for d in dirnames if d not in FORBIDDEN_DIRS)
        for f in sorted(filenames):
            if f.lower().endswith(FORBIDDEN_EXT):
                sys.exit('refusing to pack %s: no APKs in a release' % os.path.join(dirpath, f))
            if f.startswith('.'):
                continue
            os.makedirs(os.path.join(dst, rel), exist_ok=True)
            shutil.copy2(os.path.join(dirpath, f), os.path.join(dst, rel, f))
    with open(os.path.join(dst, 'VERSION'), 'w') as f:
        f.write(version + '\n')


def text(src, dst, crlf=False):
    with open(src, encoding='utf-8') as f:
        data = f.read()
    with open(dst, 'w', encoding='utf-8', newline='\r\n' if crlf else '\n') as f:
        f.write(data)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--site', required=True)
    ap.add_argument('--variant', required=True)
    ap.add_argument('--version', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--emsdk', default=os.environ.get('EMSDK'))
    a = ap.parse_args()

    with open(os.path.join(a.site, 'stacks.json')) as f:
        stacks = json.load(f)['stacks']
    name = 'SENSES-Web-Apps-%s-%s' % (a.version, a.variant)
    stage = os.path.join(a.out, name)
    shutil.rmtree(stage, ignore_errors=True)
    os.makedirs(stage)

    copy_site(a.site, os.path.join(stage, 'site'), a.version)
    for src, dst in SERVER:
        os.makedirs(os.path.dirname(os.path.join(stage, 'server', dst)), exist_ok=True)
        shutil.copyfile(os.path.join(ROOT, src), os.path.join(stage, 'server', dst))
    for src, dst in STARTERS:
        text(os.path.join(RELEASE, 'start', src), os.path.join(stage, dst), crlf=dst.endswith('.bat'))
    for d in DOCS:
        text(os.path.join(RELEASE, d), os.path.join(stage, d), crlf=True)
    # SENSES Web Apps' own code: GNU AGPL v3 (the same file as the repository's LICENSE).
    text(os.path.join(RELEASE, 'publication', 'LICENSE'), os.path.join(stage, 'LICENSE.txt'), crlf=True)
    with open(os.path.join(stage, 'VERSION'), 'w') as f:
        f.write(a.version + '\n')
    rows = copy_licenses(stage, stacks, a.emsdk)
    write_notice(stage, a.version, a.variant, stacks, rows)

    # The zip: one top folder; the start scripts executable (macOS Archive
    # Utility and unzip keep the mode); fixed times, so the same tree gives
    # the same archive.
    archive = os.path.join(a.out, name + '.zip')
    stamp = time.localtime()[:6]
    raw = 0
    with zipfile.ZipFile(archive + '.tmp', 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for dirpath, dirnames, filenames in os.walk(stage):
            dirnames.sort()
            for f in sorted(filenames):
                path = os.path.join(dirpath, f)
                arc = os.path.join(name, os.path.relpath(path, stage)).replace(os.sep, '/')
                info = zipfile.ZipInfo(arc, stamp)
                info.compress_type = zipfile.ZIP_DEFLATED
                executable = f in ('Start SENSES.command', 'start-senses.sh') or f == 'senses.py'
                info.external_attr = ((0o100755 if executable else 0o100644) << 16)
                with open(path, 'rb') as fh:
                    data = fh.read()
                raw += len(data)
                z.writestr(info, data, compresslevel=6)
    os.replace(archive + '.tmp', archive)
    digest = hashlib.sha256()
    with open(archive, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            digest.update(chunk)
    with open(archive + '.sha256', 'w') as f:
        f.write('%s  %s\n' % (digest.hexdigest(), os.path.basename(archive)))
    print('%s: %d files, %.1f MB unpacked, %.1f MB zip (%s)' % (
        archive, sum(len(fs) for _, _, fs in os.walk(stage)), raw / 1e6,
        os.path.getsize(archive) / 1e6, ', '.join(stacks)))


if __name__ == '__main__':
    main()
