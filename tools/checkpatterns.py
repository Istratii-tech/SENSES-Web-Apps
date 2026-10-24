#!/usr/bin/env python3
"""The personal-data gate, second half: data by its FORM (the first half,
tools/checknames.sh, looks for words known in advance). Ported from SENSES.

  tools/checkpatterns.py --list FILE      paths, one per line (checknames.sh)
  tools/checkpatterns.py PATH...

Exit 0 clean, 1 found, 2 a file could not be read. Prints how many files it
checked (stderr, "шаблоны: проверено файлов N").

Looks for: a MAC address, 15 digits passing the Luhn check (IMEI), an ICCID,
private addresses (10/8, 172.16/12, 192.168/16; for the last two three parts
are enough: a subnet gives a home away as well as an address), mail addresses,
home folders with a name (the macOS and Linux homes followed by a name). Values are printed masked
(two characters at each end): the gate's output goes into reports.

Exceptions are narrow and each has its reason: exact values, and where they
are allowed. This file passes the gate itself: the patterns are expressions,
the exact values are assembled from pieces.
"""

import re
import sys

MAC = re.compile(
    rb'(?<![0-9A-Fa-f:-])(?:[0-9A-Fa-f]{2}([:-]))(?:[0-9A-Fa-f]{2}\1){4}[0-9A-Fa-f]{2}(?![0-9A-Fa-f:-])')
# Placeholders: none, broadcast, and Android's 02:00:00:00:00:00 (what apps
# get instead of the real address since Android 6).
MAC_OK = {b'00:00:00:00:00:00', b'00-00-00-00-00-00', b'ff:ff:ff:ff:ff:ff', b'FF:FF:FF:FF:FF:FF',
          b'02:00:00:00:00:00'}

DIG15 = re.compile(rb'(?<![0-9A-Za-z_.-])[0-9]{15}(?![0-9A-Za-z_]|\.[0-9])')
ICCID = re.compile(rb'(?<![0-9A-Za-z_.-])89[0-9]{17,19}(?![0-9A-Za-z_]|\.[0-9])')

IP = re.compile(
    rb'(?<![0-9A-Za-z_.])(?:'
    rb'192\.168\.[0-9]{1,3}'
    rb'|172\.(?:1[6-9]|2[0-9]|3[01])\.[0-9]{1,3}'
    rb'|10\.[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}'
    rb')(?![0-9])')
# The ranges themselves, written as networks (the release's CA is limited to
# them, tools/release/certs.py): they name no one's network.
IP_RANGES = [b'10.0.0.0/8', b'172.16.0.0/12', b'192.168.0.0/16']
# QEMU's user network (guest 10.0.2.15, host .2, DNS .3): the same for everyone.
IP_OK = re.compile(rb'^10\.0\.2\.')

# Not between '@'s: dalvik-cache names join path segments with '@'
# (data@app@com.example@base.apk), no address does.
MAIL = re.compile(
    rb'(?<![A-Za-z0-9._%+@-])[A-Za-z0-9._%+-]+@[a-z0-9-]+(?:\.[a-z0-9-]+)*\.([a-z]{2,})(?![A-Za-z0-9@-])')
# The example domains of RFC 2606 (example.com/.net/.org and below): samples.
MAIL_EXAMPLE = re.compile(rb'@(?:[a-z0-9-]+\.)*example\.(?:com|net|org)$')
MAIL_NOT_TLD = {b'jar', b'apk', b'so', b'dex', b'odex', b'oat', b'art', b'vdex', b'kt', b'py', b'sh', b'txt',
                b'xml', b'png', b'jpg', b'img', b'zip', b'gz', b'json', b'md', b'js', b'html', b'java', b'c',
                b'h', b'cc', b'cpp', b'class', b'prof', b'data', b'wasm', b'stamp'}
# The project's public signature and the assistant co-author's.
MAIL_OK = {b'istratiitech' + b'@' + b'users.noreply.github.com', b'noreply' + b'@' + b'anthropic.com'}

HOME = re.compile(rb'(?<![A-Za-z0-9_.-])/(?:Users|home)/([A-Za-z0-9][A-Za-z0-9._-]*)')
# Placeholders, and Emscripten's made-up user (HOME of its runtime).
HOME_OK = {b'Shared', b'Guest', b'user', b'username', b'USER', b'name', b'you', b'yourname', b'someone',
           b'example', b'web_user'}

# Third-party text and code that carry such values on their own:
# tools/checkpatterns-allow.b64 (base64, as tools/badwords.b64: the values
# are not in the repository's text), lines
#   value<TAB>KIND<TAB>PATH-REGEX<TAB>EXACT VALUE<TAB>WHY
#   line<TAB>KIND<TAB>PATH-REGEX<TAB>EXACT LINE<TAB>WHY
# "line" is for the strings of an image (tools/unpackimage.py, a path with
# ".unpacked/"): the whole string the value stands in must be exactly this
# one, not just the value anywhere in the image. KIND is the kind printed
# below (почта, частный адрес, ...).
def load_allow():
    import base64
    import os
    rules = []
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'checkpatterns-allow.b64')
    try:
        with open(path, 'rb') as f:
            text = base64.b64decode(f.read()).decode('utf-8')
    except OSError:
        return rules
    for line in text.splitlines():
        if line.strip() and not line.startswith('#'):
            mode, kind, rx, exact, _why = line.split('\t', 4)
            rules.append((mode, kind, re.compile(rx), exact.encode('utf-8')))
    return rules


ALLOW = load_allow()


def allowed(path, kind, value, data, pos):
    line = None
    for mode, k, rx, exact in ALLOW:
        if k != kind or not rx.search(path):
            continue
        if mode == 'value' and value.lower() == exact.lower():
            return True
        if mode == 'line':
            if line is None:
                s, e = data.rfind(b'\n', 0, pos) + 1, data.find(b'\n', pos)
                line = data[s:e if e >= 0 else len(data)]
                if path.endswith('zip.txt'):
                    line = line.partition(b'\t')[2]  # "ENTRY<TAB>STRING"
            if line == exact:
                return True
    return False


def luhn(d):
    s, alt = 0, False
    for ch in reversed(d):
        n = ch - 48
        if alt:
            n *= 2
            if n > 9:
                n -= 9
        s += n
        alt = not alt
    return s % 10 == 0


def mask(v):
    t = v.decode('utf-8', 'replace')
    return t if len(t) <= 4 else t[:2] + '…' + t[-2:]


def scan(path, data):
    out = []

    def hit(m, kind, value):
        if not allowed(path, kind, value, data, m.start()):
            out.append((m.start(), kind, mask(value)))
    for m in MAC.finditer(data):
        if m.group(0) not in MAC_OK:
            hit(m, 'MAC', m.group(0))
    for m in DIG15.finditer(data):
        v = m.group(0)
        # A real 15-digit number has some 12-14 runs of equal digits; table
        # data in images ("999222444888666", bitmaps) has a few long ones.
        if len(set(v)) >= 4 and len(re.findall(rb'([0-9])\1*', v)) >= 8 and luhn(v):
            hit(m, 'IMEI', v)
    for m in ICCID.finditer(data):
        hit(m, 'ICCID', m.group(0))
    for m in IP.finditer(data):
        v = m.group(0)
        if not (IP_OK.match(v) or any(data.startswith(r, m.start()) for r in IP_RANGES)):
            hit(m, 'частный адрес', v)
    for m in MAIL.finditer(data):
        v = m.group(0)
        if not (v.lower() in MAIL_OK or m.group(1) in MAIL_NOT_TLD or MAIL_EXAMPLE.search(v.lower())):
            hit(m, 'почта', v)
    for m in HOME.finditer(data):
        if m.group(1) not in HOME_OK:
            hit(m, 'домашний каталог', m.group(0))
    return sorted((data.count(b'\n', 0, p) + 1, k, v) for p, k, v in out)


def main(argv):
    paths, args = [], list(argv)
    while args:
        a = args.pop(0)
        if a == '--list':
            with open(args.pop(0), encoding='utf-8') as f:
                paths += [l.rstrip('\n') for l in f if l.strip()]
        else:
            paths.append(a)
    rc, n = 0, 0
    for p in paths:
        try:
            with open(p, 'rb') as f:
                data = f.read()
        except OSError:
            print('⛔ не прочитан (а значит и не проверен): %s' % p)
            rc = 2
            continue
        n += 1
        found = scan(p, data)
        if found:
            rc = rc or 1
            print('⛔ %s' % p)
            for ln, kind, val in found[:5]:
                print('     %d: %s: %s' % (ln, kind, val))
            if len(found) > 5:
                print('     … и ещё %d' % (len(found) - 5))
    print('шаблоны: проверено файлов %d' % n, file=sys.stderr)
    return rc


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
