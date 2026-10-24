#!/usr/bin/env python3
"""SENSES Web Apps launcher: the small download that fetches the rest.

The release's "Start SENSES" scripts in the launcher archive run this file.
It asks GitHub Releases for the latest SENSES Web Apps, downloads the full
release archive to this computer when it is newer than the one installed,
checks its SHA-256, unpacks it into the user's settings folder
(versions/VERSION-VARIANT/) and starts that version's server/senses.py with
the same settings folder: the same CA, port and name, so the phone keeps its
apps and data (docs/topics/local-release.md, "Launcher").

The computer downloads, not the phone: GitHub serves neither the
cross-origin isolation headers nor CORS the site needs, and the apps' network
goes through this computer anyway.

- Without internet, or when GitHub fails: starts the version last installed.
- Keeps the current version and the one before it (--rollback swaps them);
  older ones are removed.
- HTTPS is always checked. A python.org Python on macOS without "Install
  Certificates.command" cannot check certificates: then the system's curl
  downloads (it uses the system's certificates), never without the check.
- Python 3.8+ and its standard library only; Windows, macOS and Linux.

  python3 launcher.py [--variant android10|all] [--rollback] [--update | --no-update]
                      [--update-url URL] [--install ZIP] [server options...]

Options it does not know go to senses.py (--port, --reset-cert, --lang, ...).
SENSES_UPDATE_URL (or --update-url) replaces the GitHub address: a URL that
answers like https://api.github.com/repos/OWNER/REPO/releases/latest.
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import socket
import ssl
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile

REPO = 'Istratii-tech/SENSES-Web-Apps'
LATEST_URL = 'https://api.github.com/repos/%s/releases/latest' % REPO
RELEASES_PAGE = 'https://github.com/%s/releases' % REPO
VARIANTS = ('android10', 'all')
ASSET = re.compile(r'^SENSES-Web-Apps-(?P<version>.+)-(?P<variant>android10|all)\.zip$')
USER_AGENT = 'SENSES-Web-Apps-launcher'
WINDOWS = os.name == 'nt'
MAC = sys.platform == 'darwin'
HERE = os.path.dirname(os.path.abspath(__file__))

# --- messages -----------------------------------------------------------------

MESSAGES = {
    'checking': ('Checking for a new SENSES Web Apps…', 'Проверяю, нет ли новой версии SENSES Web Apps…'),
    'variant': ('Systems: {variant} (to change: --variant android10 or --variant all).',
                'Системы: {variant} (сменить: --variant android10 или --variant all).'),
    'up_to_date': ('SENSES Web Apps {version} is the latest.', 'SENSES Web Apps {version} — последняя версия.'),
    'new': ('New version {version}: downloading {size} to this computer…',
            'Новая версия {version}: скачиваю {size} на этот компьютер…'),
    'first': ('Downloading SENSES Web Apps {version} ({size}) to this computer (once; then only updates)…',
              'Скачиваю SENSES Web Apps {version} ({size}) на этот компьютер (один раз, потом только обновления)…'),
    'checksum': ('Checking the download (SHA-256)…', 'Проверяю загрузку (SHA-256)…'),
    'bad_sum': ('The download is damaged or not the published file (SHA-256 does not match): not installed.',
                'Загрузка повреждена или это не опубликованный файл (SHA-256 не совпадает): не устанавливаю.'),
    'no_sum': ('The release has no {name}: refusing to install a file that cannot be checked.',
               'В выпуске нет {name}: файл без контрольной суммы не устанавливаю.'),
    'unpacking': ('Unpacking…', 'Распаковываю…'),
    'installed': ('Installed {version}.', 'Установлена версия {version}.'),
    'removed': ('Removed the old version {version}.', 'Удалена старая версия {version}.'),
    'starting': ('Starting SENSES Web Apps {version}.', 'Запускаю SENSES Web Apps {version}.'),
    'offline': ('No connection to the update server ({err}): starting the installed version {version}.',
                'Нет связи с сервером обновлений ({err}): запускаю установленную версию {version}.'),
    'failed': ('The update failed ({err}): starting the installed version {version}.',
               'Обновление не удалось ({err}): запускаю установленную версию {version}.'),
    'first_offline': ('SENSES Web Apps is not downloaded yet, and the update server cannot be reached ({err}).\n'
                      'The first start needs the internet (about 60 MB for Android 10). Check the connection and '
                      'start again.\nWithout internet on this computer: on another one download the full archive '
                      'SENSES-Web-Apps-VERSION-android10.zip from\n  {page}\nand either unpack it and run its own '
                      '"Start SENSES", or start this one with --install PATH/TO/THE.zip',
                      'SENSES Web Apps ещё не скачан, а сервер обновлений недоступен ({err}).\n'
                      'Для первого запуска нужен интернет (около 60 МБ для Android 10). Проверьте подключение и '
                      'запустите снова.\nЕсли на этом компьютере интернета нет: на другом скачайте полный архив '
                      'SENSES-Web-Apps-ВЕРСИЯ-android10.zip со страницы\n  {page}\nи либо распакуйте его и '
                      'запустите его «Start SENSES», либо запустите этот с --install ПУТЬ/К/АРХИВУ.zip'),
    'first_failed': ('SENSES Web Apps could not be downloaded: {err}', 'Не удалось скачать SENSES Web Apps: {err}'),
    'no_release': ('no release published yet at {url}', 'на {url} ещё нет опубликованного выпуска'),
    'no_asset': ('the latest release has no {variant} archive', 'в последнем выпуске нет архива {variant}'),
    'curl': ('This Python cannot check HTTPS certificates (on macOS: the python.org Python without '
             '"Install Certificates.command"). Downloading with the system\'s curl, which checks them.',
             'Этот Python не может проверить сертификаты HTTPS (в macOS: Python с python.org без '
             '«Install Certificates.command»). Скачиваю системным curl — он их проверяет.'),
    'cert': ('the server\'s certificate could not be checked ({err}); nothing is downloaded without that check',
             'сертификат сервера не удалось проверить ({err}); без этой проверки ничего не скачиваю'),
    'disk': ('not enough free space: {need} needed in {path}', 'мало места: нужно {need} в {path}'),
    'running': ('SENSES is already running on port {port}: not updating while it runs.',
                'SENSES уже работает на порту {port}: пока он запущен, не обновляю.'),
    'held': ('Updates are paused after --rollback (to resume: --update).',
             'Обновления приостановлены после --rollback (возобновить: --update).'),
    'rolled': ('Rolled back: {old} → {new}. Updates are paused until you start with --update.',
               'Откат: {old} → {new}. Обновления приостановлены, пока не запустите с --update.'),
    'no_previous': ('There is no previous version to roll back to.', 'Нет прошлой версии для отката.'),
    'local_zip': ('Installing {path}…', 'Устанавливаю {path}…'),
    'not_release': ('{path} is not a SENSES Web Apps release archive.', '{path} — не архив выпуска SENSES Web Apps.'),
    'missing': ('The installed version {version} is incomplete; downloading again.',
                'Установленная версия {version} неполная; скачиваю заново.'),
}
LANG = 'en'


def msg(key, **kw):
    return MESSAGES[key][1 if LANG == 'ru' else 0].format(**kw)


def say(text='', end='\n'):
    try:
        print(text, end=end, flush=True)
    except UnicodeEncodeError:
        print(text.encode('ascii', 'replace').decode(), end=end, flush=True)


def system_language():
    # As senses.py's.
    for var in ('LC_ALL', 'LC_MESSAGES', 'LANG', 'LANGUAGE'):
        if os.environ.get(var):
            return 'ru' if os.environ[var].lower().startswith('ru') else 'en'
    try:
        if WINDOWS:
            import ctypes
            return 'ru' if ctypes.windll.kernel32.GetUserDefaultUILanguage() & 0x3ff == 0x19 else 'en'
        if MAC:
            out = subprocess.run(['defaults', 'read', '-g', 'AppleLanguages'], capture_output=True,
                                 text=True, timeout=5).stdout
            first = re.search(r'"?([a-zA-Z-]+)', out.split('(', 1)[-1])
            return 'ru' if first and first.group(1).lower().startswith('ru') else 'en'
    except Exception:
        pass
    return 'en'


def mb(n):
    return '%.0f MB' % (n / 1e6) if n >= 1e7 else '%.1f MB' % (n / 1e6)


# --- state --------------------------------------------------------------------

def state_dir():
    # The same folder as senses.py's state_dir(): the settings it keeps (CA,
    # port, name) and, beside them, the versions this launcher installed.
    if os.environ.get('SENSES_STATE'):
        return os.environ['SENSES_STATE']
    if WINDOWS:
        return os.path.join(os.environ.get('APPDATA') or os.path.expanduser('~'), 'SENSES Web Apps')
    if MAC:
        return os.path.expanduser('~/Library/Application Support/SENSES Web Apps')
    return os.path.join(os.environ.get('XDG_CONFIG_HOME') or os.path.expanduser('~/.config'), 'senses-web-apps')


class State:
    """config.json of the settings folder (senses.py's), under the key
    "launcher": variant, current and previous version folders, hold."""

    def __init__(self, path):
        self.path = path
        self.versions = os.path.join(path, 'versions')
        os.makedirs(self.versions, exist_ok=True)
        self.file = os.path.join(path, 'config.json')

    def load(self):
        try:
            with open(self.file) as f:
                return json.load(f)
        except (OSError, ValueError):
            return {}

    @property
    def launcher(self):
        return self.load().get('launcher', {})

    def update(self, **kw):
        config = self.load()
        config.setdefault('launcher', {}).update(kw)
        tmp = self.file + '.tmp'
        with open(tmp, 'w') as f:
            json.dump(config, f, indent=2, sort_keys=True)
        os.replace(tmp, self.file)

    def folder(self, name):
        return os.path.join(self.versions, name) if name else None

    def complete(self, name):
        d = self.folder(name)
        return bool(d) and os.path.isfile(os.path.join(d, 'server', 'senses.py')) and \
            os.path.isfile(os.path.join(d, 'site', 'index.html'))


def version_key(version):
    """The numbers of a version for ordering: 2026.10.24 -> (2026, 10, 24);
    2026.10.24-2 -> (2026, 10, 24, 2). Other parts (a commit) do not count."""
    return tuple(int(t) for t in re.split(r'[.\-_+]', version.lstrip('vV')) if t.isdigit() and len(t) <= 4)


def folder_version(name):
    return name.rsplit('-', 1)[0] if name else None


def server_running(state):
    port = state.load().get('port')
    if not port:
        return None
    try:
        with urllib.request.urlopen('http://127.0.0.1:%d/__ping' % port, timeout=2) as r:
            return port if json.loads(r.read().decode()).get('senses') else None
    except Exception:
        return None


# --- downloads ----------------------------------------------------------------

class CertificateError(Exception):
    pass


class NotFound(Exception):
    pass


def cert_failure(e):
    reason = getattr(e, 'reason', e)
    return isinstance(reason, ssl.SSLCertVerificationError) or 'CERTIFICATE_VERIFY_FAILED' in str(reason)


class NoDowngrade(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg_, headers, newurl):
        if req.full_url.startswith('https:') and not newurl.startswith('https:'):
            raise urllib.error.URLError('redirect from https to %s refused' % newurl.split(':')[0])
        return super().redirect_request(req, fp, code, msg_, headers, newurl)


class Fetcher:
    """Downloads with Python, or, when Python cannot check certificates,
    with the system's curl (SENSES_FETCH=curl forces it). Plain http only
    when the update address given is http itself (tests, a local mirror)."""

    def __init__(self, base_url, mode=None):
        self.allow_http = base_url.startswith('http:')
        self.allow_file = base_url.startswith('file:')
        self.curl = (mode or os.environ.get('SENSES_FETCH')) == 'curl'
        self.told = False
        self.opener = urllib.request.build_opener(NoDowngrade)

    def check_scheme(self, url):
        scheme = urllib.parse.urlsplit(url).scheme
        if scheme != 'https' and not (self.allow_http and scheme == 'http') and \
                not (scheme == 'file' and self.allow_file):
            raise urllib.error.URLError('refusing %s address %s' % (scheme or 'relative', url))

    def to_curl(self, e):
        if self.curl or not cert_failure(e) or not shutil.which('curl'):
            raise CertificateError(str(getattr(e, 'reason', e))) if cert_failure(e) else e
        self.curl = True
        if not self.told:
            say(msg('curl'))
            self.told = True

    def curl_cmd(self, url, out, accept):
        proto = '=http,https' if self.allow_http else '=https'
        return ['curl', '-fsSL', '--proto', proto, '--proto-redir', proto, '--connect-timeout', '15',
                '--retry', '2', '-A', USER_AGENT, '-H', 'Accept: ' + accept, '-o', out, url]

    def curl_run(self, url, out, accept, size=0, progress=None):
        proc = subprocess.Popen(self.curl_cmd(url, out, accept), stdout=subprocess.DEVNULL,
                                stderr=subprocess.PIPE, text=True)
        while proc.poll() is None:
            if progress:
                try:
                    progress(os.path.getsize(out), size)
                except OSError:
                    pass
            time.sleep(0.25)
        err = proc.stderr.read().strip()
        if proc.returncode in (22, 56) and re.search(r'error: 404\b', err):
            raise NotFound(url)
        if proc.returncode in (35, 51, 58, 60, 77, 83, 90, 91):  # TLS and certificate checks
            raise CertificateError(err or 'curl exit %d' % proc.returncode)
        if proc.returncode:
            raise urllib.error.URLError(err or 'curl exit %d' % proc.returncode)
        if progress:
            progress(os.path.getsize(out), size)

    def get(self, url, accept='application/octet-stream'):
        """A small file's bytes."""
        self.check_scheme(url)
        if not self.curl or url.startswith('file:'):
            try:
                req = urllib.request.Request(url, headers={'User-Agent': USER_AGENT, 'Accept': accept})
                with self.opener.open(req, timeout=20) as r:
                    return r.read()
            except urllib.error.HTTPError as e:
                if e.code == 404:
                    raise NotFound(url)
                raise
            except urllib.error.URLError as e:
                self.to_curl(e)
        fd, tmp = tempfile.mkstemp()
        os.close(fd)
        try:
            self.curl_run(url, tmp, accept)
            with open(tmp, 'rb') as f:
                return f.read()
        finally:
            os.unlink(tmp)

    def download(self, url, out, size, progress):
        self.check_scheme(url)
        if not self.curl:
            try:
                req = urllib.request.Request(url, headers={'User-Agent': USER_AGENT,
                                                           'Accept': 'application/octet-stream'})
                with self.opener.open(req, timeout=30) as r, open(out, 'wb') as f:
                    total = int(r.headers.get('Content-Length') or size or 0)
                    done = 0
                    while True:
                        chunk = r.read(1 << 18)
                        if not chunk:
                            break
                        f.write(chunk)
                        done += len(chunk)
                        progress(done, total)
                return
            except urllib.error.HTTPError as e:
                if e.code == 404:
                    raise NotFound(url)
                raise
            except urllib.error.URLError as e:
                self.to_curl(e)
        self.curl_run(url, out, 'application/octet-stream', size, progress)


class Progress:
    def __init__(self):
        self.start = time.time()
        self.last = 0
        self.shown = -1
        self.tty = sys.stdout.isatty()

    def __call__(self, done, total):
        now = time.time()
        pct = int(done * 100 / total) if total else 0
        if self.tty:
            if now - self.last < 0.2 and done != total:
                return
            rate = done / max(now - self.start, 1e-3)
            bar = '#' * (pct * 30 // 100)
            say('\r  [%-30s] %3d%%  %s / %s  %.1f MB/s ' % (bar, pct, mb(done), mb(total), rate / 1e6), end='')
        elif pct // 10 != self.shown // 10:
            say('  %d%%  %s / %s' % (pct, mb(done), mb(total)))
        self.last, self.shown = now, pct

    def end(self):
        if self.tty and self.shown >= 0:
            say()
        self.shown = -1


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def expected_sum(text, name):
    for line in text.splitlines():
        parts = line.split()
        if len(parts) >= 1 and re.fullmatch(r'[0-9a-fA-F]{64}', parts[0]) and \
                (len(parts) == 1 or parts[-1].lstrip('*') == name):
            return parts[0].lower()
    return None


# --- installing ---------------------------------------------------------------

def unpack(state, archive, name):
    """|archive| (one top folder holding server/ and site/) into
    versions/|name|, through a temporary folder: a broken archive leaves
    nothing behind."""
    tmp = tempfile.mkdtemp(prefix='.incoming-', dir=state.versions)
    try:
        with zipfile.ZipFile(archive) as z:
            members = z.infolist()
            tops = set()
            for m in members:
                parts = m.filename.replace('\\', '/').split('/')
                if m.filename.startswith(('/', '\\')) or '..' in parts or ':' in parts[0]:
                    raise ValueError('unsafe path in the archive: %s' % m.filename)
                tops.add(parts[0])
            if len(tops) != 1:
                raise ValueError('not one top folder in the archive')
            for m in members:
                path = z.extract(m, tmp)
                mode = (m.external_attr >> 16) & 0o777
                if mode and not m.is_dir() and not WINDOWS:
                    os.chmod(path, mode | 0o600)
        top = os.path.join(tmp, tops.pop())
        if not (os.path.isfile(os.path.join(top, 'server', 'senses.py')) and
                os.path.isfile(os.path.join(top, 'site', 'index.html'))):
            raise ValueError('no server/senses.py or site/index.html in the archive')
        dest = state.folder(name)
        if os.path.exists(dest):
            shutil.rmtree(dest)
        os.replace(top, dest)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def make_current(state, name):
    cur = state.launcher.get('current')
    prev = cur if cur and cur != name else state.launcher.get('previous')
    state.update(current=name, previous=prev if prev != name else None)
    prune(state)


def prune(state):
    keep = {state.launcher.get('current'), state.launcher.get('previous')}
    for d in sorted(os.listdir(state.versions)):
        path = os.path.join(state.versions, d)
        if d in keep or not os.path.isdir(path):
            if d.endswith('.part') and os.path.isfile(path):
                os.unlink(path)
            continue
        shutil.rmtree(path, ignore_errors=True)
        if not d.startswith('.'):
            say(msg('removed', version=d))


def fetch_release(fetcher, url):
    try:
        data = json.loads(fetcher.get(url, 'application/vnd.github+json').decode('utf-8'))
    except NotFound:
        raise RuntimeError(msg('no_release', url=url))
    if not isinstance(data, dict) or not isinstance(data.get('assets'), list):
        raise RuntimeError(msg('no_release', url=url))
    return data


def pick(release, variant):
    assets = {a.get('name'): a for a in release['assets'] if isinstance(a, dict)}
    for name, a in sorted(assets.items()):
        m = ASSET.match(name or '')
        if m and m.group('variant') == variant:
            return m.group('version'), a, assets.get(name + '.sha256')
    return None, None, None


def choose_variant(release, state, args):
    if args.variant:
        if args.variant != state.launcher.get('variant'):
            state.update(variant=args.variant)
        return args.variant
    if state.launcher.get('variant') in VARIANTS:
        return state.launcher['variant']
    # Android 10 unless asked otherwise: the system SENSES opens apps on, and
    # enough for most of them (--variant all brings 4.4, 6.0.1 and 8.1).
    state.update(variant='android10')
    return 'android10'


def install(state, fetcher, release, variant):
    """Downloads, checks and unpacks the release's |variant| archive when it
    is not the current one; returns the folder name of the version to run."""
    version, asset, sum_asset = pick(release, variant)
    if not asset:
        raise RuntimeError(msg('no_asset', variant=variant))
    name = '%s-%s' % (version, variant)
    cur = state.launcher.get('current')
    if cur and state.complete(cur) and cur.endswith('-' + variant) and \
            version_key(folder_version(cur)) >= version_key(version):
        say(msg('up_to_date', version=folder_version(cur)))
        return cur
    if state.launcher.get('previous') == name and state.complete(name):
        make_current(state, name)  # the newer one was rolled back from: no need to download it again
        say(msg('installed', version=name))
        return name
    if not sum_asset:
        raise RuntimeError(msg('no_sum', name=asset['name'] + '.sha256'))
    size = asset.get('size') or 0
    free = shutil.disk_usage(state.versions).free
    if size and free < size * 4:  # the zip and what it unpacks to, with room to spare
        raise RuntimeError(msg('disk', need=mb(size * 4), path=state.versions))
    want = expected_sum(fetcher.get(sum_asset['browser_download_url']).decode('utf-8', 'replace'), asset['name'])
    if not want:
        raise RuntimeError(msg('no_sum', name=sum_asset['name']))
    digest = str(asset.get('digest') or '')
    if digest.startswith('sha256:') and digest[7:].lower() != want:
        raise RuntimeError(msg('bad_sum'))
    say(msg('new' if cur else 'first', version=version, size=mb(size)))
    part = os.path.join(state.versions, asset['name'] + '.part')
    bar = Progress()
    try:
        try:
            fetcher.download(asset['browser_download_url'], part, size, bar)
        finally:
            bar.end()
        say(msg('checksum'))
        if sha256_of(part) != want:
            raise RuntimeError(msg('bad_sum'))
        say(msg('unpacking'))
        unpack(state, part, name)
    finally:
        if os.path.exists(part):
            os.unlink(part)
    make_current(state, name)
    say(msg('installed', version=version))
    return name


def install_zip(state, path):
    m = ASSET.match(os.path.basename(path))
    if not m or not os.path.isfile(path):
        raise RuntimeError(msg('not_release', path=path))
    say(msg('local_zip', path=path))
    sums = path + '.sha256'
    if os.path.exists(sums):
        with open(sums, encoding='utf-8', errors='replace') as f:
            want = expected_sum(f.read(), os.path.basename(path))
        if want and sha256_of(path) != want:
            raise RuntimeError(msg('bad_sum'))
    name = '%s-%s' % (m.group('version'), m.group('variant'))
    unpack(state, path, name)
    state.update(variant=m.group('variant'))
    make_current(state, name)
    say(msg('installed', version=m.group('version')))
    return name


def rollback(state):
    cur, prev = state.launcher.get('current'), state.launcher.get('previous')
    if not prev or not state.complete(prev):
        say(msg('no_previous'))
        return cur
    state.update(current=prev, previous=cur, hold=True, variant=prev.rsplit('-', 1)[1])
    say(msg('rolled', old=folder_version(cur), new=folder_version(prev)))
    return prev


# --- running ------------------------------------------------------------------

def run_server(state, name, rest):
    folder = state.folder(name)
    say(msg('starting', version=folder_version(name)))
    cmd = [sys.executable, os.path.join(folder, 'server', 'senses.py')] + rest
    env = dict(os.environ, SENSES_STATE=state.path)
    sys.stdout.flush()
    if WINDOWS:
        # (os.exec* on Windows starts a new process and returns to the
        # console at once.)
        try:
            return subprocess.call(cmd, env=env)
        except KeyboardInterrupt:
            return 0
    os.execve(sys.executable, cmd, env)


def main(argv=None):
    global LANG
    ap = argparse.ArgumentParser(
        description='SENSES Web Apps launcher: downloads the latest SENSES Web Apps and starts it. '
                    'Other options go to the server (server/senses.py --help).')
    ap.add_argument('--variant', choices=VARIANTS, help='android10: Android 10 only; all: 4.4, 6.0.1, 8.1 and 10 '
                    '(asked on the first start, then kept)')
    ap.add_argument('--rollback', action='store_true', help='start the previous version (and pause updates)')
    ap.add_argument('--update', action='store_true', help='resume updates after --rollback')
    ap.add_argument('--no-update', action='store_true', help='start the installed version without checking')
    ap.add_argument('--update-url', default=os.environ.get('SENSES_UPDATE_URL') or LATEST_URL,
                    help='where to ask for the latest release (env SENSES_UPDATE_URL)')
    ap.add_argument('--install', metavar='ZIP', help='install a downloaded full release archive (no internet)')
    ap.add_argument('--state', help='the settings folder (CA, port, name, versions)')
    ap.add_argument('--lang', choices=('en', 'ru'))
    args, rest = ap.parse_known_args(argv)
    LANG = args.lang or system_language()
    if args.lang:
        rest += ['--lang', args.lang]
    try:
        sys.stdout.reconfigure(errors='replace')
    except Exception:
        pass
    if args.state:
        os.environ['SENSES_STATE'] = os.path.abspath(args.state)
    state = State(state_dir())
    if args.update:
        state.update(hold=False)

    cur = state.launcher.get('current')
    if cur and not state.complete(cur):
        say(msg('missing', version=cur))
        state.update(current=None)
        cur = None
    if args.install:
        try:
            cur = install_zip(state, args.install)
        except (RuntimeError, ValueError, zipfile.BadZipFile, OSError) as e:
            say(msg('first_failed', err=e))
            return 1
    elif args.rollback:
        cur = rollback(state)
    elif args.no_update and cur:
        pass
    elif state.launcher.get('hold') and cur:
        say(msg('held'))
    elif cur and server_running(state):
        say(msg('running', port=state.load().get('port')))
    else:
        say(msg('checking'))
        fetcher = Fetcher(args.update_url)
        try:
            fetcher.check_scheme(args.update_url)
            release = fetch_release(fetcher, args.update_url)
            variant = choose_variant(release, state, args)
            say(msg('variant', variant=variant))
            cur = install(state, fetcher, release, variant)
        except (urllib.error.URLError, socket.timeout, ConnectionError, CertificateError, NotFound,
                RuntimeError, ValueError, zipfile.BadZipFile, OSError) as e:
            if isinstance(e, CertificateError):
                err = msg('cert', err=e)
            elif isinstance(e, urllib.error.HTTPError):
                err = 'HTTP %d' % e.code
            else:
                err = str(getattr(e, 'reason', e) or type(e).__name__)
            offline = isinstance(e, (urllib.error.URLError, socket.timeout, ConnectionError)) and \
                not isinstance(e, urllib.error.HTTPError)
            if cur and state.complete(cur):
                say(msg('offline' if offline else 'failed', err=err, version=folder_version(cur)))
            elif offline:
                say(msg('first_offline', err=err, page=RELEASES_PAGE))
                return 1
            else:
                say(msg('first_failed', err=err))
                return 1
    if not cur or not state.complete(cur):
        say(msg('first_offline', err='-', page=RELEASES_PAGE))
        return 1
    if args.state:
        rest += ['--state', os.environ['SENSES_STATE']]
    return run_server(state, cur, rest)


if __name__ == '__main__':
    sys.exit(main())
