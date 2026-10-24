#!/usr/bin/env python3
"""Checks of the release launcher (tools/release/launcher.py, topic
release-launcher) against a fake GitHub Releases on a free local port:
the latest release, SHA-256 checks (a wrong sum is refused), updating from an
old version to a new one with the same CA, port and name, keeping two
versions, rollback, no network -> the version installed before, curl
instead of Python, an untrusted HTTPS server refused, and the launcher
archive (tools/make_launcher.py).

  python3 tests/release/launcher_test.py      (make release-test)
"""

import hashlib
import json
import os
import shutil
import socket
import ssl
import subprocess
import sys
import tempfile
import time
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
RELEASE = os.path.join(ROOT, 'tools', 'release')
sys.path.insert(0, RELEASE)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import certs  # noqa: E402
import launcher  # noqa: E402
from fake_github import LATEST, FakeGitHub  # noqa: E402

TMP = tempfile.mkdtemp(prefix='senses-launcher-test-')
results = []


def check(name, ok, detail=''):
    results.append(ok)
    print('%s %s%s' % ('PASS' if ok else 'FAIL', name, ('  ' + detail) if detail and not ok else ''))


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb' if isinstance(data, bytes) else 'w') as f:
        f.write(data)
    return path


def free_port(start, need=1):
    for base in range(start, start + 400, 10):
        try:
            for p in range(base - 1, base + need):
                with socket.socket() as s:
                    s.bind(('0.0.0.0', p))
            return base
        except OSError:
            continue
    raise SystemExit('no free ports')


# --- release archives -----------------------------------------------------------

# A server that only says what it was started as (most checks), or the real one.
STUB = '''import json, os, sys
here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
with open(os.path.join(here, 'site', 'VERSION')) as f:
    version = f.read().strip()
with open(os.path.join(os.environ['SENSES_STATE'], 'launched.json'), 'w') as f:
    json.dump({'version': version, 'argv': sys.argv[1:], 'here': here}, f)
'''
REAL = [('tools/release/senses.py', 'senses.py'), ('tools/release/certs.py', 'certs.py'),
        ('tools/release/qr.py', 'qr.py'), ('tools/release/mdns.py', 'mdns.py'),
        ('tools/release/setup/index.html', 'setup/index.html'),
        ('tools/serve.py', 'serve.py'), ('tools/net_relay.py', 'net_relay.py')]


def make_zip(folder, version, variant, real=False, evil=False):
    name = 'SENSES-Web-Apps-%s-%s' % (version, variant)
    path = os.path.join(folder, name + '.zip')
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr(name + '/site/index.html', '<!doctype html><title>SENSES</title>')
        z.writestr(name + '/site/VERSION', version + '\n')
        z.writestr(name + '/site/stacks.json', '{"stacks": ["a10"]}')
        z.writestr(name + '/VERSION', version + '\n')
        if real:
            for src, dst in REAL:
                z.write(os.path.join(ROOT, src), name + '/server/' + dst)
        else:
            z.writestr(name + '/server/senses.py', STUB)
        if evil:
            z.writestr(name + '/../../escaped.txt', 'out of the folder')
    with open(path, 'rb') as f:
        digest = hashlib.sha256(f.read()).hexdigest()
    write(path + '.sha256', '%s  %s\n' % (digest, os.path.basename(path)))
    return path


def fake_github(tls=None):
    return FakeGitHub(tempfile.mkdtemp(dir=TMP, prefix='gh-'), free_port(8560), tls=tls)


def run(state, url, *args, env=None, script=None, timeout=60):
    cmd = [sys.executable, script or os.path.join(RELEASE, 'launcher.py'), '--state', state,
           '--update-url', url, '--lang', 'en'] + list(args)
    e = dict(os.environ)
    e.pop('SENSES_FETCH', None)
    e.update(env or {})
    r = subprocess.run(cmd, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=timeout, env=e)
    return r.returncode, r.stdout + r.stderr


def launched(state):
    path = os.path.join(state, 'launched.json')
    try:
        with open(path) as f:
            return json.load(f).get('version')
    except OSError:
        return None
    finally:
        if os.path.exists(path):
            os.unlink(path)


def versions(state):
    return sorted(d for d in os.listdir(os.path.join(state, 'versions')) if not d.startswith('.'))


def config(state):
    with open(os.path.join(state, 'config.json')) as f:
        return json.load(f)


# --- checks ---------------------------------------------------------------------

def test_basics():
    import senses
    saved = os.environ.pop('SENSES_STATE', None)
    try:
        check('settings folder = senses.py\'s', launcher.state_dir() == senses.state_dir())
        os.environ['SENSES_STATE'] = os.path.join(TMP, 'x')
        check('SENSES_STATE honoured as senses.py does', launcher.state_dir() == senses.state_dir())
    finally:
        os.environ.pop('SENSES_STATE', None)
        if saved:
            os.environ['SENSES_STATE'] = saved
    k = launcher.version_key
    check('version order', k('2026.10.24') < k('2026.10.25') < k('2026.11.1') and
          k('2026.10.24-1a2b3c4') == k('2026.10.24') < k('2026.10.24-2') and k('v2026.10.24') == k('2026.10.24'))
    folder = tempfile.mkdtemp(dir=TMP, prefix='scan-')
    for v in ('2026.10.9', '2026.10.20'):
        make_zip(folder, v, 'android10')
    gh = FakeGitHub(folder, free_port(8560), scan=True)
    with urllib.request.urlopen(gh.url) as r:
        latest = json.loads(r.read().decode())
    gh.stop()
    check('fake_github.py: the highest version in the folder is the latest', latest['tag_name'] == 'v2026.10.20' and
          sorted(a['name'] for a in latest['assets']) == ['SENSES-Web-Apps-2026.10.20-android10.zip',
                                                          'SENSES-Web-Apps-2026.10.20-android10.zip.sha256'])
    check('sha256 file parsed', launcher.expected_sum('%s  a.zip\n' % ('ab' * 32), 'a.zip') == 'ab' * 32 and
          launcher.expected_sum('%s  b.zip\n' % ('ab' * 32), 'a.zip') is None)


def test_updates():
    zips = os.path.join(TMP, 'zips')
    os.makedirs(zips)
    state = os.path.join(TMP, 'state')
    gh = fake_github()
    dead = 'http://127.0.0.1:%d%s' % (free_port(8700), LATEST)

    code, out = run(state, dead)
    check('first start offline: a clear error', code == 1 and 'first start needs the internet' in out and
          '--install' in out, out)
    code, out = run(state, gh.url)
    check('first start, nothing published: said so', code == 1 and 'no release published' in out, out)

    v1 = make_zip(zips, '2026.10.20', 'android10')
    gh.publish(v1, make_zip(zips, '2026.10.20', 'all'))
    code, out = run(state, gh.url)
    check('first start: downloads, checks and starts the latest', code == 0 and launched(state) == '2026.10.20' and
          'SHA-256' in out and '100%' in out, out)
    check('the choice kept (no terminal: Android 10)', config(state)['launcher']['variant'] == 'android10' and
          versions(state) == ['2026.10.20-android10'], repr(config(state)))
    n = len(gh.downloads)
    code, out = run(state, gh.url)
    check('same version: no download', code == 0 and launched(state) == '2026.10.20' and len(gh.downloads) == n and
          'is the latest' in out, out)

    v2 = make_zip(zips, '2026.10.22', 'android10')
    gh.publish(v2, sums='%s  %s\n' % ('0' * 64, os.path.basename(v2)))
    code, out = run(state, gh.url)
    check('wrong SHA-256: refused, the old version starts', code == 0 and 'does not match' in out and
          launched(state) == '2026.10.20' and versions(state) == ['2026.10.20-android10'] and
          not [f for f in os.listdir(os.path.join(state, 'versions')) if f.endswith('.part')], out)
    gh.publish(v2, digest='sha256:' + '1' * 64)
    code, out = run(state, gh.url)
    check('GitHub\'s own digest differs: refused', 'does not match' in out and launched(state) == '2026.10.20', out)
    gh.publish(v2, digest='sha256:' + hashlib.sha256(open(v2, 'rb').read()).hexdigest())
    code, out = run(state, gh.url)
    check('new version: installed and started', code == 0 and launched(state) == '2026.10.22' and
          versions(state) == ['2026.10.20-android10', '2026.10.22-android10'], out)

    gh.publish(make_zip(zips, '2026.10.24', 'android10'))
    code, out = run(state, gh.url)
    check('third version: the oldest removed, two kept', launched(state) == '2026.10.24' and
          versions(state) == ['2026.10.22-android10', '2026.10.24-android10'] and 'Removed' in out, out)

    code, out = run(state, gh.url, '--rollback')
    check('--rollback: the previous version', launched(state) == '2026.10.22' and 'Rolled back' in out, out)
    n = len(gh.downloads)
    code, out = run(state, gh.url)
    check('after rollback: updates paused', launched(state) == '2026.10.22' and 'paused' in out and
          len(gh.downloads) == n, out)
    code, out = run(state, gh.url, '--update')
    check('--update: forward again without a download', launched(state) == '2026.10.24' and
          len(gh.downloads) == n and versions(state) == ['2026.10.22-android10', '2026.10.24-android10'], out)

    code, out = run(state, dead)
    check('no network: the installed version starts', code == 0 and launched(state) == '2026.10.24' and
          'No connection' in out, out)
    code, out = run(state, dead, '--no-update')
    check('--no-update: starts without asking', code == 0 and launched(state) == '2026.10.24' and
          'Checking' not in out, out)

    if shutil.which('curl'):
        gh.publish(make_zip(zips, '2026.10.26', 'android10'), make_zip(zips, '2026.10.26', 'all'))
        code, out = run(state, gh.url, env={'SENSES_FETCH': 'curl'})
        check('curl downloads too', code == 0 and launched(state) == '2026.10.26' and '100%' in out, out)

        # An HTTPS server nobody vouches for: Python refuses, curl refuses, nothing installed.
        ca_key, ca = certs.make_ca('SENSES untrusted test CA')
        key, cert = certs.make_server(ca_key, ca, ['senses-t.local'], [])
        crt = write(os.path.join(TMP, 'tls', 'server.crt'), certs.pem('CERTIFICATE', cert))
        kf = write(os.path.join(TMP, 'tls', 'server.key'), key.to_pem())
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(crt, kf)
        bad = fake_github(tls=ctx)
        bad.publish(make_zip(zips, '2026.10.28', 'android10'))
        code, out = run(state, bad.url)
        check('untrusted HTTPS: refused by Python and curl, the old version starts',
              code == 0 and launched(state) == '2026.10.26' and 'certificate could not be checked' in out and
              'curl' in out and not bad.downloads, out)
        bad.stop()
    else:
        print('SKIP curl checks: no curl')

    code, out = run(state, gh.url, '--variant', 'all')
    check('--variant all: the other archive of the same version', code == 0 and launched(state) == '2026.10.26' and
          config(state)['launcher']['current'] == '2026.10.26-all' and
          config(state)['launcher']['variant'] == 'all', out)

    os.makedirs(os.path.join(TMP, 'evil'))
    evil = make_zip(os.path.join(TMP, 'evil'), '2026.10.30', 'all', evil=True)
    gh.publish(evil)
    code, out = run(state, gh.url)
    check('a path out of the folder in the zip: refused', code == 0 and 'unsafe path' in out and
          launched(state) == '2026.10.26' and not os.path.exists(os.path.join(state, 'escaped.txt')) and
          not os.path.exists(os.path.join(TMP, 'escaped.txt')), out)

    v3 = make_zip(zips, '2026.11.01', 'android10')
    code, out = run(state, dead, '--install', v3)
    check('--install a zip without internet', code == 0 and launched(state) == '2026.11.01' and
          config(state)['launcher']['variant'] == 'android10', out)
    write(v3 + '.sha256', '%s  %s\n' % ('f' * 64, os.path.basename(v3)))
    code, out = run(state, dead, '--install', v3)
    check('--install checks the .sha256 beside it', code == 1 and 'does not match' in out, out)
    gh.stop()


def ping(port):
    for _ in range(150):
        try:
            with urllib.request.urlopen('http://127.0.0.1:%d/__ping' % port, timeout=1) as r:
                return json.loads(r.read().decode())
        except OSError:
            time.sleep(0.2)
    return {}


def test_real_server():
    """The real server, updated from one version to the next: the same CA,
    port and name; not updated while it runs."""
    zips = os.path.join(TMP, 'real')
    os.makedirs(zips)
    state = os.path.join(TMP, 'state-real')
    gh = fake_github()
    port = free_port(8540, 3)
    srv_args = ['--no-browser', '--no-mdns', '--allow-sleep', '--slots', '2']
    cmd = [sys.executable, os.path.join(RELEASE, 'launcher.py'), '--state', state, '--update-url', gh.url,
           '--lang', 'en']

    gh.publish(make_zip(zips, '2026.10.20', 'android10', real=True))
    proc = subprocess.Popen(cmd + srv_args + ['--port', str(port)], stdin=subprocess.DEVNULL,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    info = ping(port)
    check('real server started by the launcher', info.get('version') == '2026.10.20', repr(info))
    with open(os.path.join(state, 'ca.crt')) as f:
        ca = f.read()
    name = config(state).get('name')

    gh.publish(make_zip(zips, '2026.10.22', 'android10', real=True))
    again = subprocess.run(cmd + srv_args, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=60)
    check('while it runs: no update', 'not updating while it runs' in again.stdout and
          'already running' in again.stdout and versions(state) == ['2026.10.20-android10'], again.stdout)
    proc.terminate()
    proc.communicate(timeout=15)

    proc = subprocess.Popen(cmd + srv_args, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    info = ping(port)
    proc.terminate()
    out = proc.communicate(timeout=15)[0]
    with open(os.path.join(state, 'ca.crt')) as f:
        same_ca = f.read() == ca
    check('updated: the new version on the same port', info.get('version') == '2026.10.22', out)
    check('updated: the same CA and name', same_ca and config(state).get('name') == name and
          config(state).get('port') == port, repr(config(state)))
    check('the server saw the update', 'New version 2026.10.20' in out, out)
    gh.stop()


def test_archive():
    out = os.path.join(TMP, 'launcher-zip')
    r = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'make_launcher.py'), '--version', '2026.10.25',
                        '--out', out], capture_output=True, text=True)
    path = os.path.join(out, 'SENSES-Web-Apps-launcher-2026.10.25.zip')
    check('make_launcher builds the zip', r.returncode == 0 and os.path.exists(path), r.stdout + r.stderr)
    with zipfile.ZipFile(path) as z:
        names = set(z.namelist())
        top = 'SENSES-Web-Apps-launcher/'
        want = {top + n for n in ('Start SENSES.command', 'Start SENSES.bat', 'start-senses.sh',
                                  'launcher/launcher.py', 'README.txt', 'README-ru.txt', 'LICENSE.txt', 'VERSION')}
        check('launcher zip: its files only', names == want, repr(sorted(names)))
        check('launcher zip: small', os.path.getsize(path) < 100 * 1024, str(os.path.getsize(path)))
        same = z.read(top + 'launcher/launcher.py') == open(os.path.join(RELEASE, 'launcher.py'), 'rb').read()
        check('launcher zip: launcher.py as in the tree', same)
        cmd, bat, sh = (z.read(top + n).decode() for n in ('Start SENSES.command', 'Start SENSES.bat',
                                                             'start-senses.sh'))
        check('start scripts run the launcher', 'launcher/launcher.py' in cmd and 'launcher\\launcher.py' in bat and
              'launcher/launcher.py' in sh and 'senses.py' not in cmd + bat + sh)
        check('start scripts executable', all(z.getinfo(top + n).external_attr >> 16 & 0o111
                                              for n in ('Start SENSES.command', 'start-senses.sh')))
        en, ru = z.read(top + 'README.txt').decode(), z.read(top + 'README-ru.txt').decode()
        check('README: how it works, updating, the phone\'s steps, troubleshooting',
              all(s in en for s in ('How it works', 'Updating', '--rollback', 'Certificate Trust Settings',
                                    'If the phone cannot connect', 'About the certificate')) and '@@' not in en)
        check('README-ru: как это работает, обновление, шаги телефона',
              all(s in ru for s in ('Как это работает', 'Обновление', '--rollback', 'Доверие сертификатам',
                                    'Если телефон не подключается', 'О сертификате')) and '@@' not in ru)
        z.extractall(os.path.join(out, 'unpacked'))
    # The packed launcher itself, against a fake GitHub.
    gh = fake_github()
    gh.publish(make_zip(os.path.join(out), '2026.10.20', 'android10'))
    state = os.path.join(TMP, 'state-zip')
    code, text = run(state, gh.url, script=os.path.join(out, 'unpacked', top, 'launcher', 'launcher.py'))
    check('the packed launcher installs and starts', code == 0 and launched(state) == '2026.10.20', text)
    gh.stop()


def main():
    test_basics()
    test_updates()
    test_real_server()
    test_archive()
    shutil.rmtree(TMP, ignore_errors=True)
    print('%d/%d passed' % (sum(results), len(results)))
    sys.exit(0 if all(results) else 1)


if __name__ == '__main__':
    main()
