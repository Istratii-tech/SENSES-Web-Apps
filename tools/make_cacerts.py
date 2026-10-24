#!/usr/bin/env python3
"""Builds Android's system CA store (/system/etc/security/cacerts) from a PEM
bundle, by default the build host's (/etc/ssl/cert.pem on macOS,
/etc/ssl/certs/ca-certificates.crt on Debian-like Linux).

Android 4.4's TrustedCertificateStore looks certificates up by file name:
<subject_hash_old>.<n>, the OpenSSL "old" hash (first 4 bytes of the MD5 of
the DER subject, little-endian). This computes it with a minimal DER walk
instead of calling openssl(1).

Usage: make_cacerts.py OUTDIR [BUNDLE.pem]
"""

import base64
import hashlib
import os
import re
import shutil
import sys

BUNDLES = ['/etc/ssl/cert.pem', '/etc/ssl/certs/ca-certificates.crt',
           '/etc/pki/tls/certs/ca-bundle.crt']


def der_element(data, pos):
    """Returns (start of content, end of element) of the element at pos."""
    length = data[pos + 1]
    pos += 2
    if length & 0x80:
        n = length & 0x7f
        length = int.from_bytes(data[pos:pos + n], 'big')
        pos += n
    return pos, pos + length


def subject_der(cert):
    _, _ = der_element(cert, 0)              # Certificate
    tbs, _ = der_element(cert, 0)
    pos, _ = der_element(cert, tbs)          # tbsCertificate content
    if cert[pos] == 0xa0:                    # [0] version
        pos = der_element(cert, pos)[1]
    pos = der_element(cert, pos)[1]          # serialNumber
    pos = der_element(cert, pos)[1]          # signature
    pos = der_element(cert, pos)[1]          # issuer
    pos = der_element(cert, pos)[1]          # validity
    return cert[pos:der_element(cert, pos)[1]]  # subject


def subject_hash_old(cert):
    return '%08x' % int.from_bytes(hashlib.md5(subject_der(cert)).digest()[:4], 'little')


def main(argv):
    out = argv[1]
    bundle = argv[2] if len(argv) > 2 else next((b for b in BUNDLES if os.path.exists(b)), None)
    if not bundle:
        sys.exit('make_cacerts.py: no CA bundle found; pass one')
    text = open(bundle).read()
    pems = re.findall(r'-----BEGIN CERTIFICATE-----.+?-----END CERTIFICATE-----', text, re.S)
    if os.path.isdir(out):
        shutil.rmtree(out)
    os.makedirs(out)
    seen = set()
    for pem in pems:
        der = base64.b64decode(''.join(pem.splitlines()[1:-1]))
        if der in seen:
            continue
        seen.add(der)
        h = subject_hash_old(der)
        n = 0
        while os.path.exists(os.path.join(out, '%s.%d' % (h, n))):
            n += 1
        with open(os.path.join(out, '%s.%d' % (h, n)), 'w') as f:
            f.write(pem + '\n')
    print('%s: %d certificates from %s' % (out, len(seen), bundle))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
