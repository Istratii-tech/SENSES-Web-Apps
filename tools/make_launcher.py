#!/usr/bin/env python3
"""Packs the SENSES Web Apps launcher (topic release-launcher,
docs/topics/local-release.md, "Launcher"): the small download that fetches
the full release from GitHub Releases and starts it. One zip:

  SENSES-Web-Apps-launcher/
    Start SENSES.command, Start SENSES.bat, start-senses.sh
    launcher/launcher.py
    README.txt, README-ru.txt, LICENSE.txt, VERSION

  make_launcher.py --version V --out DIR

The start scripts are the release's own (tools/release/start), run
launcher/launcher.py instead of server/senses.py. The READMEs are
README-launcher*.txt with the release README's sections (the phone's steps,
the certificate, troubleshooting) put in at their @@include lines, so the
two never drift apart.
"""

import argparse
import hashlib
import os
import re
import shutil
import sys
import time
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RELEASE = os.path.join(ROOT, 'tools', 'release')
TOP = 'SENSES-Web-Apps-launcher'
STARTERS = [('start-macos.command', 'Start SENSES.command'), ('start-windows.bat', 'Start SENSES.bat'),
            ('start-linux.sh', 'start-senses.sh')]
READMES = [('README-launcher.txt', 'README.txt', 'README.txt'),
           ('README-launcher-ru.txt', 'README-ru.txt', 'README-ru.txt')]
INCLUDE = re.compile(r'^@@include (.+?) \| (.+?)@@$', re.M)


def read(path):
    with open(path, encoding='utf-8') as f:
        return f.read()


def headings(text):
    """{heading: offset} of the README's headings (a line underlined with
    --- or ===)."""
    out = {}
    for m in re.finditer(r'^(.+)\n(-{3,}|={3,})$', text, re.M):
        out[m.group(1).strip()] = m.start()
    return out


def section(text, start, end):
    h = headings(text)
    if start not in h or end not in h or h[end] <= h[start]:
        sys.exit('README: no section "%s" .. "%s"' % (start, end))
    return text[h[start]:h[end]]


def readme(template, full):
    return INCLUDE.sub(lambda m: section(full, m.group(1), m.group(2)).rstrip('\n') + '\n', template)


def starter(src):
    text = read(src)
    out = text.replace('server/senses.py', 'launcher/launcher.py').replace('server\\senses.py', 'launcher\\launcher.py')
    if out == text:
        sys.exit('%s: no server/senses.py to replace' % src)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--version', required=True)
    ap.add_argument('--out', required=True)
    a = ap.parse_args()

    files = {'launcher/launcher.py': read(os.path.join(RELEASE, 'launcher.py')),
             'LICENSE.txt': read(os.path.join(RELEASE, 'publication', 'LICENSE')),
             'VERSION': a.version + '\n'}
    for src, dst in STARTERS:
        files[dst] = starter(os.path.join(RELEASE, 'start', src))
    for template, full, dst in READMES:
        files[dst] = readme(read(os.path.join(RELEASE, template)), read(os.path.join(RELEASE, full)))

    os.makedirs(a.out, exist_ok=True)
    archive = os.path.join(a.out, 'SENSES-Web-Apps-launcher-%s.zip' % a.version)
    stage = os.path.join(a.out, TOP)
    shutil.rmtree(stage, ignore_errors=True)
    stamp = time.localtime()[:6]
    with zipfile.ZipFile(archive + '.tmp', 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name in sorted(files):
            crlf = name.endswith(('.txt', '.bat'))
            data = files[name].replace('\r\n', '\n')
            data = (data.replace('\n', '\r\n') if crlf else data).encode('utf-8')
            # The unpacked copy, for the personal-data gate (tools/checknames.sh).
            path = os.path.join(stage, name)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, 'wb') as f:
                f.write(data)
            info = zipfile.ZipInfo(TOP + '/' + name, stamp)
            info.compress_type = zipfile.ZIP_DEFLATED
            executable = name.endswith(('.command', '.sh', '.py'))
            info.external_attr = (0o100755 if executable else 0o100644) << 16
            z.writestr(info, data, compresslevel=9)
    os.replace(archive + '.tmp', archive)
    with open(archive, 'rb') as f:
        digest = hashlib.sha256(f.read()).hexdigest()
    with open(archive + '.sha256', 'w') as f:
        f.write('%s  %s\n' % (digest, os.path.basename(archive)))
    print('%s: %d files, %.0f KB' % (archive, len(files), os.path.getsize(archive) / 1024))


if __name__ == '__main__':
    main()
