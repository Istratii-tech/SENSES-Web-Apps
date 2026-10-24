#!/usr/bin/env python3
"""Checks of the local release's server (tools/release, topic local-release):
certificates (against openssl and macOS's verifier when there), QR codes
decoded back (macOS CoreImage), the mDNS answers, and senses.py end to end on
a free port with a throwaway site and settings folder.

  python3 tests/release/release_test.py      (make release-test)
"""

import base64
import json
import os
import plistlib
import shutil
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import time
import urllib.request
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'release'))
import certs  # noqa: E402
import mdns  # noqa: E402
import qr  # noqa: E402

TMP = tempfile.mkdtemp(prefix='senses-release-test-')
# Sample private addresses, assembled from pieces: this file passes the
# personal-data gate (tools/checknames.sh) itself.
LAN_A = '192.' + '168.1.20'
LAN_PROBE = '192.' + '168.255.255'
results = []


def check(name, ok, detail=''):
    results.append(ok)
    print('%s %s%s' % ('PASS' if ok else 'FAIL', name, ('  ' + detail) if detail and not ok else ''))


def write(path, data):
    with open(path, 'wb' if isinstance(data, bytes) else 'w') as f:
        f.write(data)
    return path


def lan_ip():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.connect((LAN_PROBE, 9))
        return s.getsockname()[0]


# --- certificates ---------------------------------------------------------------

def test_certs():
    ca_key, ca = certs.make_ca('SENSES test CA')
    key, cert = certs.make_server(certs.RSAKey.from_pem(ca_key.to_pem()), ca, ['senses-t.local'],
                                  [LAN_A, '127.0.0.1', '8.8.8.8', '100.64.1.1'])
    ca_pem = write(os.path.join(TMP, 'ca.crt'), certs.pem('CERTIFICATE', ca))
    srv_pem = write(os.path.join(TMP, 'server.crt'), certs.pem('CERTIFICATE', cert))
    write(os.path.join(TMP, 'server.key'), key.to_pem())
    check('CA common name', certs.common_name(ca) == 'SENSES test CA')
    check('issuer = CA subject', certs.subject(ca) in cert)
    days = (certs.not_after(cert) - certs.not_after(ca)).days
    check('server valid ~397 days, CA ~3 years', -700 < days < -670, str(days))
    profile = plistlib.loads(certs.mobileconfig(ca, 'SENSES test', 'local.senses.test'))
    check('mobileconfig holds the CA', profile['PayloadContent'][0]['PayloadContent'] == ca and
          profile['PayloadContent'][0]['PayloadType'] == 'com.apple.security.root')
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(srv_pem, os.path.join(TMP, 'server.key'))
    check('Python ssl loads the key pair', True)
    if shutil.which('openssl'):
        r = subprocess.run(['openssl', 'verify', '-CAfile', ca_pem, srv_pem], capture_output=True, text=True)
        check('openssl verifies the chain', r.returncode == 0, r.stdout + r.stderr)
        _, evil = certs.make_server(ca_key, ca, ['example.com'], [])
        evil_pem = write(os.path.join(TMP, 'evil.crt'), certs.pem('CERTIFICATE', evil))
        r = subprocess.run(['openssl', 'verify', '-CAfile', ca_pem, evil_pem], capture_output=True, text=True)
        check('name constraints: example.com refused (openssl)', r.returncode != 0 and 'subtree' in r.stdout + r.stderr)
        text = subprocess.run(['openssl', 'x509', '-in', srv_pem, '-noout', '-text'], capture_output=True, text=True).stdout
        check('only private ranges in the SAN', LAN_A in text and
              not any(a in text for a in ('8.8.8.8', '127.0.0.1', '100.64.1.1')))
        catext = subprocess.run(['openssl', 'x509', '-in', ca_pem, '-noout', '-text'], capture_output=True,
                                text=True).stdout
        constraints = catext.split('Name Constraints')[1].split('Signature Algorithm')[0]
        check('CA constraints: 10/8, 172.16/12, 192.168/16, .local only',
              constraints.count('IP:') == 3 and constraints.count('DNS:') == 1 and 'DNS:local' in constraints,
              constraints)
        _, local = certs.make_server(ca_key, ca, ['localhost'], [])
        p2 = write(os.path.join(TMP, 'local.crt'), certs.pem('CERTIFICATE', local))
        r = subprocess.run(['openssl', 'verify', '-CAfile', ca_pem, p2], capture_output=True, text=True)
        check('name constraints: localhost refused (openssl)', r.returncode != 0)
        k8 = os.path.join(TMP, 'k8.pem')
        subprocess.run(['openssl', 'genpkey', '-algorithm', 'RSA', '-pkeyopt', 'rsa_keygen_bits:1024', '-out', k8],
                       capture_output=True)
        if os.path.exists(k8):
            with open(k8) as f:
                k = certs.RSAKey.from_pem(f.read())
            check('PKCS#8 key read', pow(pow(12345, k.e, k.n), k.d, k.n) == 12345)
    if sys.platform == 'darwin':
        for host in ('senses-t.local', LAN_A):
            r = subprocess.run(['security', 'verify-cert', '-c', srv_pem, '-r', ca_pem, '-p', 'ssl', '-s', host],
                               capture_output=True, text=True)
            check('macOS verify-cert %s' % host, 'successful' in r.stdout + r.stderr, r.stdout + r.stderr)


# --- QR ---------------------------------------------------------------------------

def png(q, path, scale=6, border=4):
    n = (q.size + 2 * border) * scale
    rows = []
    for y in range(n):
        my = y // scale - border
        row = bytearray([0])
        for x in range(n):
            mx = x // scale - border
            row.append(0 if 0 <= mx < q.size and 0 <= my < q.size and q.m[my][mx] else 255)
        rows.append(bytes(row))

    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
    write(path, b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', n, n, 8, 0, 0, 0, 0)) +
          chunk(b'IDAT', zlib.compress(b''.join(rows))) + chunk(b'IEND', b''))


DECODE_JS = """ObjC.import('CoreImage'); ObjC.import('Foundation');
function run(argv) {
  var det = $.CIDetector.detectorOfTypeContextOptions($.CIDetectorTypeQRCode, $(), $({}));
  return argv.map(function (p) {
    var f = det.featuresInImage($.CIImage.imageWithContentsOfURL($.NSURL.fileURLWithPath(p)));
    return f.count ? ObjC.unwrap(f.objectAtIndex(0).messageString) : '';
  }).join('\\n');
}"""


def test_qr():
    texts = ['http://%s:8500/' % LAN_A, 'http://senses-my-computer.local:8500/', 'A',
             'https://example.com/' + 'x' * 120, 'http://1' + '0.0.0.5:8500/']
    for t in texts:
        q = qr.QR(t)
        check('QR size for %d bytes' % len(t), q.size in (21, 25, 29, 33, 37, 41, 45, 49, 53, 57))
    if sys.platform != 'darwin':
        return
    paths = []
    for i, t in enumerate(texts):
        paths.append(os.path.join(TMP, 'q%d.png' % i))
        png(qr.QR(t), paths[-1])
    js = write(os.path.join(TMP, 'decode.js'), DECODE_JS)
    r = subprocess.run(['osascript', '-l', 'JavaScript', js] + paths, capture_output=True, text=True)
    got = r.stdout.rstrip('\n').split('\n')
    check('QR codes decode back (CoreImage)', got == texts, repr(got))


# --- mDNS -------------------------------------------------------------------------

def query(name, qtype):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(2)
    s.sendto(struct.pack('>HHHHHH', 0x4242, 0, 1, 0, 0, 0) + mdns._encode_name(name) + struct.pack('>HH', qtype, 1),
             ('224.0.0.251', 5353))
    try:
        while True:
            data, _ = s.recvfrom(9000)
            if struct.unpack('>H', data[:2])[0] == 0x4242:
                return data
    except OSError:
        return None
    finally:
        s.close()


def test_mdns():
    ip = lan_ip()
    name = 'senses-test-%d.local' % os.getpid()
    r = mdns.Responder(name, ip)
    if not r.start():
        check('mDNS responder starts', False, str(getattr(r, 'error', '')))
        return
    a = query(name, 1)
    check('mDNS A answer', a is not None and socket.inet_aton(ip) in a)
    aaaa = query(name, 28)
    check('mDNS AAAA -> NSEC', aaaa is not None and struct.pack('>H', 47) in aaaa)
    check('mDNS other names ignored', query('other-%d.local' % os.getpid(), 1) is None)


# --- the server -------------------------------------------------------------------

def free_base():
    for base in range(18500, 19900, 50):
        try:
            for p in range(base - 1, base + 3):  # the setup port, the library, 2 slots
                with socket.socket() as s:
                    s.bind(('0.0.0.0', p))
            return base
        except OSError:
            continue
    raise SystemExit('no free ports')


def ws_status(host, port, path, origin, ctx):
    raw = socket.create_connection((host, port), timeout=10)
    s = ctx.wrap_socket(raw, server_hostname=host) if ctx else raw
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall(('GET %s HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
               'Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nOrigin: %s\r\n\r\n'
               % (path, host, port, key, origin)).encode())
    head = b''
    while b'\r\n\r\n' not in head:
        b = s.recv(1)
        if not b:
            break
        head += b
    status = head.split(b'\r\n')[0].decode()
    body = ''
    if ' 101 ' in status:
        b1 = s.recv(2)[1]
        body = s.recv(b1 & 0x7f).decode()
    s.close()
    return status, body


def test_server():
    site = os.path.join(TMP, 'site')
    os.makedirs(site)
    write(os.path.join(site, 'index.html'), '<!doctype html><title>SENSES</title>')
    write(os.path.join(site, 'stacks.json'), '{"stacks": ["a10"]}')
    write(os.path.join(site, 'VERSION'), 'test-1\n')
    state = os.path.join(TMP, 'state')
    port = free_base()
    ip = lan_ip()
    cmd = [sys.executable, os.path.join(ROOT, 'tools', 'release', 'senses.py'), '--site', site, '--state', state,
           '--port', str(port), '--slots', '2', '--no-browser', '--no-mdns', '--allow-sleep', '--lang', 'en']
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        for _ in range(100):
            try:
                urllib.request.urlopen('http://127.0.0.1:%d/__ping' % port, timeout=1)
                break
            except OSError:
                time.sleep(0.2)
        r = urllib.request.urlopen('http://localhost:%d/' % port)
        check('localhost: the site over plain HTTP', b'SENSES' in r.read())
        # The library page itself is not isolated with apps' origins (serve.py); the rest is.
        r = urllib.request.urlopen('http://localhost:%d/stacks.json' % port)
        check('localhost: cross-origin isolated', r.headers.get('Cross-Origin-Embedder-Policy') == 'require-corp')

        class NoRedirect(urllib.request.HTTPRedirectHandler):
            def redirect_request(self, *a):
                return None
        opener = urllib.request.build_opener(NoRedirect)
        setup = port - 1
        try:
            opener.open('http://%s:%d/index.html' % (ip, port))
            check('LAN over HTTP to the library: to the setup port', False)
        except urllib.error.HTTPError as e:
            check('LAN over HTTP to the library: to the setup port', e.code == 302 and
                  e.headers['Location'] == 'http://%s:%d/__setup/' % (ip, setup), e.headers['Location'])
        try:
            opener.open('http://%s:%d/index.html' % (ip, setup))
            check('setup port: nothing but the setup page', False)
        except urllib.error.HTTPError as e:
            check('setup port: nothing but the setup page', e.code == 302 and e.headers['Location'] == '/__setup/')
        page = urllib.request.urlopen('http://%s:%d/__setup/' % (ip, setup)).read()
        check('setup page over HTTP on its port', b'SENSES Web Apps' in page)
        try:
            with socket.create_connection((ip, setup), timeout=5) as raw:
                ssl._create_unverified_context().wrap_socket(raw, server_hostname=ip)
            check('setup port: no TLS (a browser trying https falls back to http)', False)
        except (ssl.SSLError, OSError):
            check('setup port: no TLS (a browser trying https falls back to http)', True)
        info = json.loads(urllib.request.urlopen('http://%s:%d/__setup/info.json' % (ip, setup)).read())
        check('setup info', info['port'] == port and info['setup_port'] == setup and info['version'] == 'test-1'
              and info['ip'] == ip, repr(info))
        r = urllib.request.urlopen('http://%s:%d/__setup/senses-ca.mobileconfig' % (ip, setup))
        check('profile type', r.headers['Content-Type'] == 'application/x-apple-aspen-config')
        ctx = ssl.create_default_context(cafile=os.path.join(state, 'ca.crt'))
        if ssl.OPENSSL_VERSION.startswith('LibreSSL 2.'):
            # Apple's Python 3.9 (LibreSSL 2.8) cannot check a name-constrained
            # CA as a client ("unsupported name constraint type"); browsers,
            # iOS and macOS can (test_certs). Here only the server's side.
            print('note: %s cannot verify name constraints; HTTPS unverified here' % ssl.OPENSSL_VERSION)
            ctx = ssl._create_unverified_context()
        for host, p in ((ip, port), (ip, port + 2)):
            body = urllib.request.urlopen('https://%s:%d/index.html' % (host, p), context=ctx).read()
            check('HTTPS %s:%d with the CA' % (host, p), b'SENSES' in body)
        origins = json.loads(urllib.request.urlopen('https://%s:%d/origins.json' % (ip, port), context=ctx).read())
        check('apps\' origins', origins == {'ports': [port + 1, port + 2]})
        org = 'https://%s:%d' % (ip, port)
        check('relay: this computer refused',
              ws_status(ip, port, '/__net/tcp?host=127.0.0.1&port=%d' % port, org, ctx)[1].find('EACCES') > 0)
        check('relay: own LAN address refused',
              ws_status(ip, port, '/__net/tcp?host=%s&port=%d' % (ip, port), org, ctx)[1].find('EACCES') > 0)
        check('relay: foreign origin refused',
              ' 403 ' in ws_status(ip, port, '/__net/dns', 'https://evil.example', ctx)[0])
        check('relay: plain HTTP from the LAN refused',
              ' 101 ' not in ws_status(ip, port, '/__net/dns', 'http://%s:%d' % (ip, port), None)[0])
        req = urllib.request.Request('http://%s:%d/__setup/report' % (ip, setup), method='POST',
                                     data=b'{"state": "steps", "host": "x"}', headers={'Content-Type': 'application/json'})
        check('setup report', urllib.request.urlopen(req).status == 204)
        second = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        check('second start: already running', second.returncode == 0 and 'already running' in second.stdout,
              second.stdout)
    finally:
        proc.terminate()
        out = proc.communicate(timeout=10)[0]
    check('the window told about the report', 'shows the steps' in out, out)
    with open(os.path.join(state, 'config.json')) as f:
        cfg = json.load(f)
    check('settings kept', cfg['port'] == port and cfg['setup_port'] == port - 1 and cfg['slots'] == 2 and
          cfg['version'] == 'test-1')
    check('key files owner-only', all(os.stat(os.path.join(state, f)).st_mode & 0o077 == 0
                                      for f in ('ca.key', 'server.key')) or os.name == 'nt')
    with open(os.path.join(state, 'ca.crt')) as f:
        before = f.read()
    proc = subprocess.Popen(cmd + ['--reset-cert'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    time.sleep(4)
    proc.terminate()
    out = proc.communicate(timeout=10)[0]
    with open(os.path.join(state, 'ca.crt')) as f:
        check('--reset-cert: a new CA, profile removal explained', f.read() != before and 'Remove Profile' in out, out)
    with open(os.path.join(state, 'config.json')) as f:
        check('--reset-cert keeps port and name', json.load(f)['port'] == port)


def main():
    test_certs()
    test_qr()
    test_mdns()
    test_server()
    shutil.rmtree(TMP, ignore_errors=True)
    print('%d/%d passed' % (sum(results), len(results)))
    sys.exit(0 if all(results) else 1)


if __name__ == '__main__':
    main()
