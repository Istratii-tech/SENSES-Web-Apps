"""The release's own certificates, in pure Python (no openssl: Windows has
none, and the release needs only Python's standard library).

A private root CA, made once per computer and kept in the state folder (a
phone that trusts it keeps trusting it across releases and new server
certificates), and a server certificate it signs for this computer's names
and LAN addresses, made again whenever they change or it nears its end.

The CA is name-constrained (a critical nameConstraints) to what a server on a
home network can be: IPv4 addresses of the private ranges and .local names.
If its key leaked from this computer, it still could not vouch for any site
on the internet. The DNS subtree must stay: without a dNSName subtree, DNS
names would not be constrained at all (RFC 5280 4.2.1.10); an IPv6 address
matches none of the IPv4 subtrees and is refused. The CA lasts 3 years, then
phones install a new one (senses.py warns two months ahead, --reset-cert).

iOS's rules for a server certificate: RSA 2048+, SHA-256, the names in
subjectAltName, serverAuth in extendedKeyUsage, at most 825 days (397 here).
"""

import base64
import datetime
import hashlib
import ipaddress
import os
import plistlib
import secrets
import uuid

# --- DER ----------------------------------------------------------------------


def _len(n):
    if n < 0x80:
        return bytes([n])
    b = n.to_bytes((n.bit_length() + 7) // 8, 'big')
    return bytes([0x80 | len(b)]) + b


def tlv(tag, body):
    return bytes([tag]) + _len(len(body)) + body


def seq(*items):
    return tlv(0x30, b''.join(items))


def integer(n):
    return tlv(0x02, n.to_bytes(n.bit_length() // 8 + 1, 'big'))


def oid(dotted):
    parts = [int(p) for p in dotted.split('.')]
    out = bytearray([40 * parts[0] + parts[1]])
    for p in parts[2:]:
        chunk = [p & 0x7f]
        p >>= 7
        while p:
            chunk.append(0x80 | (p & 0x7f))
            p >>= 7
        out += bytes(reversed(chunk))
    return tlv(0x06, bytes(out))


NULL = b'\x05\x00'
TRUE = b'\x01\x01\xff'


def bits(data, unused=0):
    return tlv(0x03, bytes([unused]) + data)


def octets(data):
    return tlv(0x04, data)


def utf8(s):
    return tlv(0x0c, s.encode('utf-8'))


def when(t):
    if t.year < 2050:
        return tlv(0x17, t.strftime('%y%m%d%H%M%SZ').encode())
    return tlv(0x18, t.strftime('%Y%m%d%H%M%SZ').encode())


def name(cn, org='SENSES Web Apps'):
    return seq(tlv(0x31, seq(oid('2.5.4.10'), utf8(org))),
               tlv(0x31, seq(oid('2.5.4.3'), utf8(cn))))


def extension(ext_oid, value, critical=False):
    return seq(oid(ext_oid), TRUE if critical else b'', octets(value))


def _parse(data, pos=0):
    """(tag, body, next position) of the DER element at |pos|."""
    tag, n = data[pos], data[pos + 1]
    pos += 2
    if n & 0x80:
        k = n & 0x7f
        n = int.from_bytes(data[pos:pos + k], 'big')
        pos += k
    return tag, data[pos:pos + n], pos + n


def _integers(body):
    out, pos = [], 0
    while pos < len(body):
        tag, value, pos = _parse(body, pos)
        out.append(int.from_bytes(value, 'big'))
    return out


def pem(kind, der):
    b64 = base64.encodebytes(der).decode().replace('\n', '')
    lines = [b64[i:i + 64] for i in range(0, len(b64), 64)]
    return '-----BEGIN %s-----\n%s\n-----END %s-----\n' % (kind, '\n'.join(lines), kind)


def unpem(text):
    body = ''.join(l for l in text.strip().splitlines() if not l.startswith('-----'))
    return base64.b64decode(body)


# --- RSA ----------------------------------------------------------------------

_SMALL_PRIMES = [p for p in range(3, 2000) if all(p % d for d in range(2, int(p ** 0.5) + 1))]


def _probable_prime(n, rounds=12):
    for p in _SMALL_PRIMES:
        if n % p == 0:
            return n == p
    d, r = n - 1, 0
    while d % 2 == 0:
        d //= 2
        r += 1
    for _ in range(rounds):
        a = secrets.randbelow(n - 3) + 2
        x = pow(a, d, n)
        if x in (1, n - 1):
            continue
        for _ in range(r - 1):
            x = pow(x, 2, n)
            if x == n - 1:
                break
        else:
            return False
    return True


def _prime(bits, e):
    while True:
        # The top two bits set: the product of two has all 2*bits bits.
        n = secrets.randbits(bits) | (3 << (bits - 2)) | 1
        if (n - 1) % e and _probable_prime(n):
            return n


class RSAKey:
    E = 65537

    def __init__(self, n, e, d, p, q):
        self.n, self.e, self.d, self.p, self.q = n, e, d, p, q

    @classmethod
    def generate(cls, bits=2048):
        while True:
            p, q = _prime(bits // 2, cls.E), _prime(bits // 2, cls.E)
            n = p * q
            if p != q and n.bit_length() == bits:
                break
        d = _inverse(cls.E, (p - 1) * (q - 1))
        return cls(n, cls.E, d, p, q)

    def to_pem(self):
        n, e, d, p, q = self.n, self.e, self.d, self.p, self.q
        der = seq(integer(0), integer(n), integer(e), integer(d), integer(p), integer(q),
                  integer(d % (p - 1)), integer(d % (q - 1)), integer(_inverse(q, p)))
        return pem('RSA PRIVATE KEY', der)

    @classmethod
    def from_pem(cls, text):
        """PKCS#1 (BEGIN RSA PRIVATE KEY) or PKCS#8 (BEGIN PRIVATE KEY)."""
        tag, body, _ = _parse(unpem(text))
        _, _, pos = _parse(body)
        tag, value, pos = _parse(body, pos)
        if tag == 0x30:  # PKCS#8: version, algorithm, then the PKCS#1 key in an OCTET STRING
            _, inner, _ = _parse(body, pos)
            _, body, _ = _parse(inner)
        _, n, e, d, p, q = _integers(body)[:6]
        return cls(n, e, d, p, q)

    def public_der(self):
        """SubjectPublicKeyInfo."""
        return seq(seq(oid('1.2.840.113549.1.1.1'), NULL),
                   bits(seq(integer(self.n), integer(self.e))))

    def key_id(self):
        return hashlib.sha1(seq(integer(self.n), integer(self.e))).digest()

    def sign(self, data):
        """PKCS#1 v1.5 with SHA-256."""
        info = seq(seq(oid('2.16.840.1.101.3.4.2.1'), NULL), octets(hashlib.sha256(data).digest()))
        k = (self.n.bit_length() + 7) // 8
        em = b'\x00\x01' + b'\xff' * (k - len(info) - 3) + b'\x00' + info
        return pow(int.from_bytes(em, 'big'), self.d, self.n).to_bytes(k, 'big')


def _inverse(a, m):
    g, x0, x1, r0, r1 = 0, 1, 0, a, m
    while r1:
        g = r0 // r1
        r0, r1 = r1, r0 - g * r1
        x0, x1 = x1, x0 - g * x1
    return x0 % m


# --- certificates ---------------------------------------------------------------

SHA256_RSA = seq(oid('1.2.840.113549.1.1.11'), NULL)

# What the CA may vouch for (nameConstraints, permitted subtrees).
PERMITTED_NETS = ['10.0.0.0/8', '172.16.0.0/12', '192.168.0.0/16']
PERMITTED_NAMES = ['local']
CA_DAYS = 3 * 365


def permitted(address):
    ip = ipaddress.ip_address(address)
    return any(ip in ipaddress.ip_network(net) for net in PERMITTED_NETS)


def _tbs_fields(cert_der):
    _, cert, _ = _parse(cert_der)
    _, tbs, _ = _parse(cert)
    fields, pos = [], 0
    while pos < len(tbs):
        start = pos
        tag, body, pos = _parse(tbs, pos)
        fields.append((tag, body, tbs[start:pos]))
    return fields[1:] if fields[0][0] == 0xa0 else fields  # without the version


def subject(cert_der):
    """A certificate's subject Name, as encoded (a certificate it issues
    must name its issuer with exactly these bytes)."""
    return _tbs_fields(cert_der)[4][2]


def common_name(cert_der):
    rdns, pos = _parse(subject(cert_der))[1], 0
    while pos < len(rdns):
        _, rdn, pos = _parse(rdns, pos)
        _, atv, _ = _parse(rdn)
        _, oid_der, p = _parse(atv)
        if oid_der == oid('2.5.4.3')[2:]:
            return _parse(atv, p)[1].decode('utf-8', 'replace')
    return ''


def _certificate(signer, issuer, subject_cn, key, days, extensions):
    now = datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0)
    serial = int.from_bytes(secrets.token_bytes(16), 'big') >> 1
    tbs = seq(tlv(0xa0, integer(2)), integer(serial), SHA256_RSA, issuer,
              seq(when(now - datetime.timedelta(days=1)), when(now + datetime.timedelta(days=days))),
              name(subject_cn), key.public_der(), tlv(0xa3, seq(*extensions)))
    return seq(tbs, SHA256_RSA, bits(signer.sign(tbs)))


def make_ca(cn):
    key = RSAKey.generate()
    subtrees = [seq(tlv(0x82, n.encode())) for n in PERMITTED_NAMES]
    for net in PERMITTED_NETS:
        net = ipaddress.ip_network(net)
        subtrees.append(seq(tlv(0x87, net.network_address.packed + net.netmask.packed)))
    exts = [
        extension('2.5.29.19', seq(TRUE, integer(0)), critical=True),   # CA, pathlen 0
        extension('2.5.29.15', bits(b'\x06', 1), critical=True),        # keyCertSign, cRLSign
        extension('2.5.29.14', octets(key.key_id())),
        extension('2.5.29.30', seq(tlv(0xa0, b''.join(subtrees))), critical=True),
    ]
    return key, _certificate(key, name(cn), cn, key, CA_DAYS, exts)


def make_server(ca_key, ca_der, names, addresses, days=397):
    key = RSAKey.generate()
    alt = [tlv(0x82, n.encode('ascii')) for n in names] + \
          [tlv(0x87, ipaddress.ip_address(a).packed) for a in addresses if permitted(a)]
    exts = [
        extension('2.5.29.19', seq(), critical=True),
        extension('2.5.29.15', bits(b'\xa0', 5), critical=True),        # digitalSignature, keyEncipherment
        extension('2.5.29.37', seq(oid('1.3.6.1.5.5.7.3.1'))),          # serverAuth
        extension('2.5.29.17', seq(*alt)),
        extension('2.5.29.14', octets(key.key_id())),
        extension('2.5.29.35', seq(tlv(0x80, ca_key.key_id()))),
    ]
    return key, _certificate(ca_key, subject(ca_der), names[0], key, days, exts)


def not_after(cert_der):
    """The end of a certificate's validity (a datetime, UTC)."""
    validity = _tbs_fields(cert_der)[3][1]
    _, _, p = _parse(validity)
    tag, end, _ = _parse(validity, p)
    fmt = '%y%m%d%H%M%SZ' if tag == 0x17 else '%Y%m%d%H%M%SZ'
    return datetime.datetime.strptime(end.decode(), fmt).replace(tzinfo=datetime.timezone.utc)


def mobileconfig(ca_der, display, identifier):
    """An iOS configuration profile that installs the CA (Settings then shows
    it under General > About > Certificate Trust Settings)."""
    digest = hashlib.sha256(ca_der).digest()
    cert = {
        'PayloadCertificateFileName': 'senses-ca.cer',
        'PayloadContent': ca_der,
        'PayloadDescription': 'Root certificate of the SENSES Web Apps server on your computer',
        'PayloadDisplayName': display,
        'PayloadIdentifier': identifier + '.ca',
        'PayloadType': 'com.apple.security.root',
        'PayloadUUID': str(uuid.UUID(bytes=digest[:16])).upper(),
        'PayloadVersion': 1,
    }
    profile = {
        'PayloadContent': [cert],
        'PayloadDescription': 'Lets this device open SENSES Web Apps from your computer over HTTPS. '
                              'It is valid only for local addresses (.local names, home network).',
        'PayloadDisplayName': display,
        'PayloadIdentifier': identifier,
        'PayloadOrganization': 'SENSES Web Apps',
        'PayloadRemovalDisallowed': False,
        'PayloadType': 'Configuration',
        'PayloadUUID': str(uuid.UUID(bytes=digest[16:])).upper(),
        'PayloadVersion': 1,
    }
    return plistlib.dumps(profile)


def write_private(path, text):
    """A key file only its owner can read."""
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, 'w') as f:
        f.write(text)
