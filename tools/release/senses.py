#!/usr/bin/env python3
"""SENSES Web Apps on your own computer: a private server for your phone.

Run it (the release's "Start SENSES" scripts do), keep its window open, and
open the address it shows on your iPhone (or any phone or browser) on the same
Wi-Fi. Python 3.8+ and its standard library only; Windows, macOS and Linux.

What it does (docs/topics/local-release.md):
- serves the release's site/ (the PWA, tools/serve.py) over HTTPS with its own
  root CA, made on the first run; the phone installs and trusts that CA once,
  from a setup page on a port of its own (PORT-1), over plain HTTP and, for
  a Safari that turns the http address into https by itself, over TLS too
  (past its certificate warning: the one before the profile); plain HTTP
  from the network to PORT is sent there too;
- serves this computer's own browser over plain http://localhost (a secure
  context already);
- gives the server a name on the home network (NAME.local, mdns.py), so a new
  address from the router does not move the phone's apps to another origin;
- the apps' own origins (ARC_APP_PORTS: PORT+1..PORT+SLOTS) and the network
  relay (/__net, tools/net_relay.py) without access to this computer itself;
- keeps its state (CA, port, name, logs) in the user's settings folder, so a
  new release unpacked anywhere finds the same CA, port and name, and the
  phone's apps and data stay where they are.

  python3 senses.py [--port N] [--site DIR] [--no-browser] [--lang ru|en]
"""

import argparse
import datetime
import ipaddress
import json
import os
import platform
import re
import socket
import ssl
import subprocess
import sys
import threading
import time
import urllib.parse
import webbrowser

HERE = os.path.dirname(os.path.abspath(__file__))
for d in (HERE, os.path.dirname(HERE)):  # the release's server/, or tools/ in the tree
    if os.path.exists(os.path.join(d, 'serve.py')) and d not in sys.path:
        sys.path.insert(0, d)

import certs  # noqa: E402
import mdns  # noqa: E402
import qr  # noqa: E402

DEFAULT_PORT = 8500
SLOTS = 16
WINDOWS = os.name == 'nt'
MAC = sys.platform == 'darwin'

# --- messages -----------------------------------------------------------------

MESSAGES = {
    'starting': ('Starting SENSES Web Apps {version}…', 'Запуск SENSES Web Apps {version}…'),
    'keys': ('Making this computer\'s certificate (once)…', 'Создаю сертификат этого компьютера (один раз)…'),
    'server_cert': ('New server certificate for {names}', 'Новый сертификат сервера для {names}'),
    'ca_ending': ('This computer\'s certificate ends on {date}. Before then run with --reset-cert and install '
                  'the new profile on the phones (the steps page shows how).',
                  'Сертификат этого компьютера кончается {date}. До этого запустите с --reset-cert и поставьте '
                  'новый профиль на телефоны (страница установки покажет шаги).'),
    'ca_ended': ('This computer\'s certificate has ended: making a new one.', 'Сертификат этого компьютера закончился: создаю новый.'),
    'reset': ('Old certificate removed. On each phone remove the old profile (iPhone: Settings > General > '
              'VPN & Device Management > SENSES Web Apps > Remove Profile), then open the address again and '
              'install the new one. The apps and their data stay.',
              'Старый сертификат удалён. На каждом телефоне удалите старый профиль (iPhone: Настройки → Основные → '
              'VPN и управление устройством → SENSES Web Apps → Удалить профиль), затем снова откройте адрес и '
              'поставьте новый. Приложения и их данные останутся.'),
    'running': ('SENSES Web Apps is running.', 'SENSES Web Apps работает.'),
    'scan': ('On your iPhone, on the same Wi-Fi, open the Camera and point it at this code:',
             'На iPhone в той же сети Wi-Fi откройте «Камеру» и наведите на этот код:'),
    'or_type': ('or type in Safari:', 'или наберите в Safari:'),
    'library': ('SENSES itself (once the phone trusts this computer):', 'Сам SENSES (когда телефон доверяет компьютеру):'),
    'conn': ('{ip} → port {port}: {kind}', '{ip} → порт {port}: {kind}'),
    'here': ('On this computer:', 'На этом компьютере:'),
    'name': ('Its name on the home network:', 'Имя в домашней сети:'),
    'no_name': ('No .local name ({err}): phones use the address.',
                'Имени .local нет ({err}): телефоны откроют по адресу.'),
    'keep_open': ('Keep this window open while you use the apps. To stop: Ctrl+C or close the window.',
                  'Не закрывайте это окно, пока пользуетесь приложениями. Остановить: Ctrl+C или закрыть окно.'),
    'connected': ('Connected: {ip} ({agent})', 'Подключилось устройство: {ip} ({agent})'),
    'phone_steps': ('{ip}: shows the steps to trust this computer (once per phone); then SENSES opens at {url}',
                    '{ip}: показывает шаги доверия этому компьютеру (один раз для телефона); потом SENSES откроется по адресу {url}'),
    'phone_ready': ('{ip}: trusts this computer; SENSES opens at {url}',
                    '{ip}: доверяет этому компьютеру; SENSES открывается по адресу {url}'),
    'no_phone': ('No phone has connected yet. Check: the phone is on the same Wi-Fi (not a guest network), '
                 'the address is typed exactly, the firewall lets Python in (see above).',
                 'Телефон пока не подключился. Проверьте: та же сеть Wi-Fi (не гостевая), адрес набран точно, '
                 'брандмауэр пропускает Python (см. выше).'),
    'stopped': ('Stopped. The apps keep their data on the phone; run SENSES again to use them.',
                'Остановлено. Данные приложений остаются на телефоне; запустите SENSES снова, чтобы ими пользоваться.'),
    'running_already': ('SENSES is already running on port {port}: opening it.',
                        'SENSES уже запущен на порту {port}: открываю его.'),
    'port_busy': ('Port {port} is used by another program. Close it, or run with --port N '
                  '(the phone will then see a new address with no apps: they stay on the old one).',
                  'Порт {port} занят другой программой. Закройте её или запустите с --port N '
                  '(тогда на телефоне будет новый адрес без приложений: они останутся на старом).'),
    'no_ports': ('No free ports found between 8500 and 9999.', 'Не нашлось свободных портов от 8500 до 9999.'),
    'no_lan': ('No home network address found: is this computer on Wi-Fi or Ethernet? Serving this computer only.',
               'Не найден адрес в домашней сети: компьютер подключён к Wi-Fi или кабелю? Работаю только для этого компьютера.'),
    'not_private': ('Address {ip} is not a home network address: phones may not reach it.',
                    'Адрес {ip} — не адрес домашней сети: телефоны могут его не увидеть.'),
    'no_site': ('No site at {site}: is this the unpacked release folder?',
                'Нет сайта в {site}: это распакованная папка релиза?'),
    'updated': ('New version {old} → {new}: on the phone, open SENSES; it updates itself (reload once if asked).',
                'Новая версия {old} → {new}: откройте SENSES на телефоне, она обновится сама (если попросит — обновите страницу).'),
    'fw_mac': ('macOS firewall is on: if asked «Accept incoming network connections?» for python3, click Allow.',
               'Брандмауэр macOS включён: на вопрос «Разрешить входящие подключения?» для python3 нажмите «Разрешить».'),
    'fw_mac_block': ('macOS firewall blocks all incoming connections: phones cannot connect. Turn off '
                     '"Block all incoming connections" in System Settings > Network > Firewall > Options.',
                     'Брандмауэр macOS блокирует все входящие подключения: телефон не подключится. Выключите '
                     '«Блокировать все входящие подключения»: Системные настройки → Сеть → Брандмауэр → Параметры.'),
    'fw_win': ('Windows may ask whether Python can use networks: tick "Private networks" and click Allow '
               '(needs an administrator once).',
               'Windows может спросить, можно ли Python работать в сетях: отметьте «Частные сети» и нажмите '
               '«Разрешить» (один раз, нужны права администратора).'),
    'fw_win_public': ('This Wi-Fi is set to Public in Windows: phones cannot connect. Make it Private: Settings > '
                      'Network & internet > Wi-Fi > (your network) > Private network.',
                      'Эта сеть Wi-Fi в Windows — «Общедоступная»: телефон не подключится. Сделайте её «Частной»: '
                      'Параметры → Сеть и Интернет → Wi-Fi → (ваша сеть) → Частная сеть.'),
    'fw_linux': ('If a firewall is on (ufw, firewalld), let in TCP {ports} and UDP 5353, e.g. '
                 'sudo ufw allow {ports}/tcp && sudo ufw allow 5353/udp',
                 'Если включён брандмауэр (ufw, firewalld), откройте TCP {ports} и UDP 5353, например '
                 'sudo ufw allow {ports}/tcp && sudo ufw allow 5353/udp'),
}
LANG = 'en'


def msg(key, **kw):
    return MESSAGES[key][1 if LANG == 'ru' else 0].format(**kw)


def say(text=''):
    try:
        print(text, flush=True)
    except UnicodeEncodeError:
        print(text.encode('ascii', 'replace').decode(), flush=True)


def system_language():
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


# --- state ------------------------------------------------------------------------

def state_dir():
    if os.environ.get('SENSES_STATE'):
        return os.environ['SENSES_STATE']
    if WINDOWS:
        return os.path.join(os.environ.get('APPDATA') or os.path.expanduser('~'), 'SENSES Web Apps')
    if MAC:
        return os.path.expanduser('~/Library/Application Support/SENSES Web Apps')
    return os.path.join(os.environ.get('XDG_CONFIG_HOME') or os.path.expanduser('~/.config'), 'senses-web-apps')


class State:
    def __init__(self, path):
        self.path = path
        os.makedirs(os.path.join(path, 'logs'), exist_ok=True)
        self.file = os.path.join(path, 'config.json')
        try:
            with open(self.file) as f:
                self.config = json.load(f)
        except (OSError, ValueError):
            self.config = {}

    def save(self):
        tmp = self.file + '.tmp'
        with open(tmp, 'w') as f:
            json.dump(self.config, f, indent=2, sort_keys=True)
        os.replace(tmp, self.file)

    def p(self, name):
        return os.path.join(self.path, name)


# --- network --------------------------------------------------------------------

def lan_addresses():
    """This computer's IPv4 addresses on its networks, the one with the
    default route first. No packets are sent (connect() on UDP only picks the
    route)."""
    found = []
    for probe in ('192.0.2.1', '8.8.8.8', '224.0.0.251'):
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
                s.connect((probe, 9))
                found.append(s.getsockname()[0])
        except OSError:
            pass
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            found.append(info[4][0])
    except OSError:
        pass
    out = []
    for a in found:
        ip = ipaddress.ip_address(a)
        if a not in out and not ip.is_loopback and not ip.is_unspecified and not ip.is_link_local:
            out.append(a)
    return out


def port_free(port):
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            if not WINDOWS:  # as the server binds: connections in TIME_WAIT do not count
                s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            s.bind(('0.0.0.0', port))
        return True
    except OSError:
        return False


def our_server(port):
    """The version of a SENSES server already answering on |port|, or None."""
    import urllib.request
    try:
        with urllib.request.urlopen('http://127.0.0.1:%d/__ping' % port, timeout=2) as r:
            data = json.loads(r.read().decode())
            return data.get('version') if data.get('senses') else None
    except Exception:
        return None


def host_name(state):
    if not state.config.get('name'):
        base = platform.node().split('.')[0].lower()
        base = re.sub(r'[^a-z0-9]+', '-', base).strip('-')[:24].strip('-') or 'pc'
        state.config['name'] = 'senses-' + base
    return state.config['name']


# --- certificates ------------------------------------------------------------------

def reset_certificates(state):
    for f in ('ca.key', 'ca.crt', 'server.key', 'server.crt'):
        try:
            os.remove(state.p(f))
        except OSError:
            pass
    state.config.pop('server', None)
    state.save()


def ensure_certificates(state, names, addresses):
    ca_key_file, ca_file = state.p('ca.key'), state.p('ca.crt')
    now = datetime.datetime.now(datetime.timezone.utc)
    try:
        with open(ca_file) as f:
            if certs.not_after(certs.unpem(f.read())) <= now + datetime.timedelta(days=1):
                say(msg('ca_ended'))
                reset_certificates(state)
    except (OSError, ValueError, IndexError):
        pass
    if not (os.path.exists(ca_key_file) and os.path.exists(ca_file)):
        say(msg('keys'))
        cn = 'SENSES Web Apps CA (%s, %s)' % (platform.node().split('.')[0] or 'computer',
                                                 datetime.date.today().isoformat())
        key, cert = certs.make_ca(cn)
        certs.write_private(ca_key_file, key.to_pem())
        with open(ca_file, 'w') as f:
            f.write(certs.pem('CERTIFICATE', cert))
        state.config['ca_cn'] = cn
        state.config.pop('server', None)
        state.save()
    with open(ca_file) as f:
        ca_der = certs.unpem(f.read())
    ca_end = certs.not_after(ca_der)
    if ca_end - now < datetime.timedelta(days=60):
        say('! ' + msg('ca_ending', date=ca_end.date().isoformat()))
    covered = state.config.get('server') or {}
    want = {'names': sorted(names), 'addresses': sorted(a for a in addresses if certs.permitted(a))}
    fresh = False
    try:
        with open(state.p('server.crt')) as f:
            end = certs.not_after(certs.unpem(f.read()))
        fresh = end - datetime.datetime.now(datetime.timezone.utc) > datetime.timedelta(days=30)
    except (OSError, ValueError, IndexError):
        pass
    if not fresh or covered.get('names') != want['names'] or \
            not set(want['addresses']) <= set(covered.get('addresses', [])):
        # Addresses seen before stay in: a computer that moves between two
        # networks keeps one certificate.
        want['addresses'] = sorted(set(want['addresses']) | set(covered.get('addresses', []) if fresh else []))
        with open(ca_key_file) as f:
            ca_key = certs.RSAKey.from_pem(f.read())
        # Not past the CA's own end (it would be refused then anyway).
        days = max(1, min(397, (ca_end - now).days - 1))
        key, cert = certs.make_server(ca_key, ca_der, want['names'], want['addresses'], days)
        certs.write_private(state.p('server.key'), key.to_pem())
        with open(state.p('server.crt'), 'w') as f:
            f.write(certs.pem('CERTIFICATE', cert))
        state.config['server'] = want
        state.save()
        say(msg('server_cert', names=', '.join(want['names'] + want['addresses'])))
    return ca_der


# --- the server ------------------------------------------------------------------

def run_server(args, state, site, version, port, setup_port, slots, name, addresses, ca_der):
    os.environ['ARC_APP_PORTS'] = '%d-%d' % (port + 1, port + slots) if slots else ''
    os.environ['ARC_CACHE'] = '1'
    os.environ['ARC_NET_ECHO'] = '0'
    os.environ['ARC_NET_SELF'] = '0'
    os.environ['ARC_NET_HOSTS'] = name + '.local'
    import serve
    import net_relay

    serve.ECHO_LOG = args.verbose
    log_file = state.p(os.path.join('logs', 'phone-log.txt'))
    try:
        if os.path.getsize(log_file) > 20 << 20:
            os.replace(log_file, log_file + '.old')
    except OSError:
        pass
    serve.LOGFILE = log_file

    tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    tls.load_cert_chain(state.p('server.crt'), state.p('server.key'))
    ip = addresses[0] if addresses else None
    setup_dir = os.path.join(HERE, 'setup')
    seen = {}
    reports = {}  # client address -> the setup page's state there
    info = {
        'senses': True, 'version': version, 'port': port, 'setup_port': setup_port, 'slots': slots,
        'name': name + '.local' if args.mdns_ok else None, 'ip': ip, 'addresses': addresses,
        'ca': certs.common_name(ca_der), 'os': 'mac' if MAC else 'windows' if WINDOWS else 'linux',
        'computer': platform.node().split('.')[0], 'stacks': stacks_of(site),
    }
    mobileconfig = certs.mobileconfig(ca_der, 'SENSES Web Apps (%s)' % info['computer'],
                                      'local.senses.' + re.sub(r'[^a-z0-9-]', '', name))
    phone_url = 'http://%s:%d/' % (ip, setup_port) if ip else 'http://localhost:%d/' % setup_port

    class Handler(serve.IsolatedHandler):
        plain = False
        setup_only = False

        def local_client(self):
            host = (self.headers.get('Host') or '').rpartition(':')[0] or self.headers.get('Host', '')
            return ipaddress.ip_address(self.client_address[0].split('%')[0]).is_loopback and \
                host.strip('[]') in ('localhost', '127.0.0.1', '::1')

        def note_client(self):
            addr = self.client_address[0]
            if ipaddress.ip_address(addr.split('%')[0]).is_loopback or addr in seen or addr in addresses:
                return
            seen[addr] = time.time()
            say(msg('connected', ip=addr, agent=device_name(self.headers.get('User-Agent', ''))))

        def send_body(self, body, ctype, status=200, extra=()):
            self.send_response(status)
            self.send_header('Content-Type', ctype)
            self.send_header('Content-Length', str(len(body)))
            for k, v in extra:
                self.send_header(k, v)
            self.end_headers()
            if self.command != 'HEAD':
                self.wfile.write(body)

        def do_GET(self):
            path = urllib.parse.urlsplit(self.path).path
            self.note_client()
            if path == '/__ping':
                body = json.dumps({'senses': True, 'version': version, 'secure': not self.plain}).encode()
                return self.send_body(body, 'application/json', extra=[('Access-Control-Allow-Origin', '*')])
            if path == '/__setup':
                return self.send_body(b'', 'text/plain', 302, [('Location', '/__setup/')])
            if path.startswith('/__setup/'):
                return self.setup_page(path[len('/__setup/'):])
            if self.setup_only:
                return self.send_body(b'', 'text/plain', 302, [('Location', '/__setup/')])
            if self.plain and not self.local_client():
                # A phone that does not trust the CA yet: to the setup page,
                # on its own port.
                host = (self.headers.get('Host') or ip or 'localhost').rsplit(':', 1)[0]
                return self.send_body(b'', 'text/plain', 302,
                                      [('Location', 'http://%s:%d/__setup/' % (host, setup_port))])
            return super().do_GET()

        def do_HEAD(self):
            self.do_GET()

        def do_POST(self):
            if urllib.parse.urlsplit(self.path).path == '/__setup/report':
                return self.report()
            if self.setup_only or (self.plain and not self.local_client()):
                return self.send_error(404)
            return super().do_POST()

        def report(self):
            """Where the setup page on a phone is: the steps, or ready."""
            try:
                data = json.loads(self.rfile.read(min(int(self.headers.get('Content-Length', 0)), 4096)))
                step, host = str(data.get('state')), str(data.get('host'))[:80]
            except (ValueError, AttributeError):
                return self.send_error(400)
            addr = self.client_address[0]
            if step in ('steps', 'ready') and reports.get(addr) != step:
                reports[addr] = step
                url = 'https://%s:%d/' % (host, port)
                say(msg('phone_' + step, ip=addr, url=url))
            self.send_response(204)
            self.end_headers()

        def setup_page(self, rest):
            if rest == 'info.json':
                data = dict(info, clients=sorted(seen), phone_url=phone_url, reports=reports)
                return self.send_body(json.dumps(data).encode(), 'application/json')
            if rest == 'senses-ca.mobileconfig':
                return self.send_body(mobileconfig, 'application/x-apple-aspen-config')
            if rest == 'senses-ca.crt':
                return self.send_body(ca_der, 'application/x-x509-ca-cert',
                                      extra=[('Content-Disposition', 'attachment; filename="senses-ca.crt"')])
            if rest == 'qr.svg':
                return self.send_body(qr.QR(phone_url).svg().encode(), 'image/svg+xml')
            files = {'': ('index.html', 'text/html; charset=utf-8'), 'probe-sw.js': ('probe-sw.js', 'text/javascript')}
            if rest not in files:
                return self.send_error(404)
            with open(os.path.join(setup_dir, files[rest][0]), 'rb') as f:
                return self.send_body(f.read(), files[rest][1])

        def log_message(self, fmt, *a):
            if args.verbose:
                super().log_message(fmt, *a)

        def log_error(self, fmt, *a):
            if args.verbose:
                super().log_error(fmt, *a)

    class PlainHandler(Handler):
        plain = True

    class SetupHandler(PlainHandler):
        setup_only = True

    class SetupTLSHandler(Handler):
        # Safari on some iPhones turns http://ADDR:SETUP_PORT into https and
        # does not fall back when the connection closes: the setup page then
        # comes over TLS too, past "This Connection Is Not Private" -- the one
        # warning before the profile is installed.
        setup_only = True

    class Server(serve.Server):
        """HTTPS and plain HTTP on the same port: a TLS connection starts with
        a handshake record (0x16), plain HTTP with a method name."""
        daemon_threads = True
        setup_only = False

        def handle_error(self, request, client_address):
            if args.verbose:
                super().handle_error(request, client_address)

        def finish_request(self, request, client_address):
            try:
                request.settimeout(self.HANDSHAKE_TIMEOUT)
                first = request.recv(1, socket.MSG_PEEK)
            except OSError:
                return
            if not first:
                return
            tls_hello = first[0] == 0x16
            if args.verbose and not ipaddress.ip_address(client_address[0].split('%')[0]).is_loopback:
                say(msg('conn', ip=client_address[0], port=self.server_address[1],
                        kind='TLS' if tls_hello else 'HTTP'))
            if not tls_hello:
                request.settimeout(None)
                (SetupHandler if self.setup_only else PlainHandler)(request, client_address, self, directory=site)
                return
            try:
                conn = tls.wrap_socket(request, server_side=True)
                conn.settimeout(None)
            except (OSError, ssl.SSLError):
                return
            try:
                (SetupTLSHandler if self.setup_only else Handler)(conn, client_address, self, directory=site)
            finally:
                try:
                    conn.close()
                except OSError:
                    pass

    servers = []
    for p in [port] + [port + i for i in range(1, slots + 1)]:
        srv = Server(('0.0.0.0', p), Handler)
        srv.tls = tls  # what the relay checks pages' Origin against (https)
        servers.append(srv)
    setup = Server(('0.0.0.0', setup_port), SetupHandler)
    setup.tls = tls
    setup.setup_only = True
    servers.append(setup)
    for srv in servers[1:]:
        threading.Thread(target=srv.serve_forever, daemon=True).start()
    return servers[0], phone_url, info, seen


def stacks_of(site):
    try:
        with open(os.path.join(site, 'stacks.json')) as f:
            return json.load(f).get('stacks', [])
    except (OSError, ValueError):
        return []


def device_name(agent):
    for key, label in (('iPhone', 'iPhone'), ('iPad', 'iPad'), ('Android', 'Android'),
                       ('Macintosh', 'Mac'), ('Windows', 'Windows'), ('Linux', 'Linux')):
        if key in agent:
            browser = 'Chrome' if 'Chrome/' in agent or 'CriOS/' in agent else \
                'Firefox' if 'Firefox/' in agent or 'FxiOS/' in agent else 'Safari' if 'Safari/' in agent else ''
            return (label + ', ' + browser).strip(', ')
    return agent[:40] or '?'


# --- the computer -------------------------------------------------------------------

def firewall_hints(port, setup_port, slots):
    hints = []
    try:
        if MAC:
            tool = '/usr/libexec/ApplicationFirewall/socketfilterfw'
            state = subprocess.run([tool, '--getglobalstate'], capture_output=True, text=True, timeout=5).stdout
            if 'enabled' in state.lower():  # "Firewall is enabled. (State = 1)"
                block = subprocess.run([tool, '--getblockall'], capture_output=True, text=True, timeout=5).stdout
                hints.append(msg('fw_mac_block' if 'set to enabled' in block.lower() else 'fw_mac'))
        elif WINDOWS:
            hints.append(msg('fw_win'))
            out = subprocess.run(['powershell', '-NoProfile', '-Command',
                                  '(Get-NetConnectionProfile | Select-Object -ExpandProperty NetworkCategory) -join ","'],
                                 capture_output=True, text=True, timeout=15).stdout
            if 'Public' in out and 'Private' not in out and 'Domain' not in out:
                hints.append(msg('fw_win_public'))
        else:
            hints.append(msg('fw_linux', ports='%d:%d' % (min(port, setup_port), max(port + slots, setup_port))))
    except Exception:
        pass
    return hints


def keep_awake():
    """While the server runs the computer does not go to sleep on its own
    (the phone's apps would lose their server)."""
    try:
        if MAC:
            subprocess.Popen(['caffeinate', '-i', '-w', str(os.getpid())],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        elif WINDOWS:
            import ctypes
            ctypes.windll.kernel32.SetThreadExecutionState(0x80000000 | 0x00000001)  # CONTINUOUS | SYSTEM_REQUIRED
    except Exception:
        pass


def print_qr(url):
    try:
        for line in qr.QR(url).terminal():
            say('  ' + line)
    except Exception:
        pass


def main(argv=None):
    global LANG
    ap = argparse.ArgumentParser(description='SENSES Web Apps: a server for your phone on your home network.')
    ap.add_argument('--port', type=int, help='the port (first run: 8500 or the next free one; then kept)')
    ap.add_argument('--setup-port', type=int, help='the setup page\'s plain-HTTP port (first run: PORT-1; then kept)')
    ap.add_argument('--slots', type=int, help='apps\' own origins: ports PORT+1..PORT+SLOTS (default 16)')
    ap.add_argument('--site', help='the site folder (default: site/ next to server/)')
    ap.add_argument('--state', help='the settings folder (CA, port, name, logs)')
    ap.add_argument('--no-browser', action='store_true', help='do not open the setup page on this computer')
    ap.add_argument('--no-mdns', action='store_true', help='no NAME.local on the home network')
    ap.add_argument('--allow-sleep', action='store_true', help='let the computer sleep while serving')
    ap.add_argument('--reset-cert', action='store_true',
                    help='a new certificate for this computer (phones then install the new profile)')
    ap.add_argument('--lang', choices=('en', 'ru'))
    ap.add_argument('--verbose', action='store_true', help='requests and the phone\'s log in this window')
    args = ap.parse_args(argv)
    LANG = args.lang or system_language()
    if WINDOWS:
        os.system('')  # a constant, no input: turns on ANSI colours (the QR code) in the console
    try:
        sys.stdout.reconfigure(errors='replace')
    except Exception:
        pass

    site = os.path.abspath(args.site or os.path.join(os.path.dirname(HERE), 'site'))
    if not os.path.exists(os.path.join(site, 'index.html')):
        say(msg('no_site', site=site))
        return 2
    version = 'dev'
    for f in (os.path.join(site, 'VERSION'), os.path.join(os.path.dirname(HERE), 'VERSION')):
        if os.path.exists(f):
            with open(f) as fh:
                version = fh.read().strip()
            break
    say(msg('starting', version=version))
    if args.state:
        os.environ['SENSES_STATE'] = args.state
    state = State(state_dir())

    slots = args.slots if args.slots is not None else state.config.get('slots', SLOTS)
    port = args.port or state.config.get('port')
    if port and our_server(port):
        if args.reset_cert:
            say(msg('running_already', port=port) + ' --reset-cert: stop it first / сначала остановите его.')
            return 1
        say(msg('running_already', port=port))
        if not args.no_browser:
            webbrowser.open('http://localhost:%d/__setup/' % port)
        return 0
    setup_port = args.setup_port or state.config.get('setup_port')
    if port:
        # (A settings folder from before the setup port had its own: PORT-1,
        # else the first port after the slots.)
        if not setup_port:
            setup_port = port - 1 if port_free(port - 1) else port + slots + 1
        busy = [p for p in [setup_port] + list(range(port, port + slots + 1)) if not port_free(p)]
        if busy:
            say(msg('port_busy', port=busy[0]))
            return 3
    else:
        for base in range(DEFAULT_PORT, 10000 - slots, 100):
            if all(port_free(p) for p in range(base - 1, base + slots + 1)):
                port, setup_port = base, setup_port or base - 1
                break
        else:
            say(msg('no_ports'))
            return 3
    state.config['port'], state.config['setup_port'], state.config['slots'] = port, setup_port, slots
    old = state.config.get('version')
    if old and old != version:
        say(msg('updated', old=old, new=version))
    state.config['version'] = version
    state.save()

    addresses = lan_addresses()
    home = [a for a in addresses if certs.permitted(a)]  # a VPN's tunnel (198.18.x.x and the like) is not
    if not addresses:
        say(msg('no_lan'))
    elif not home:
        say(msg('not_private', ip=addresses[0]))
    addresses = home or addresses
    name = host_name(state)
    args.mdns_ok = False
    responder = None
    if not args.no_mdns and addresses:
        responder = mdns.Responder(name + '.local', addresses[0])
        args.mdns_ok = responder.start()
    if args.reset_cert:
        reset_certificates(state)
        say(msg('reset'))
    # Only names and addresses the CA may vouch for (certs.PERMITTED_*): this
    # computer itself uses http://localhost, which needs no certificate.
    ca_der = ensure_certificates(state, [name + '.local'], addresses)
    srv, phone_url, info, seen = run_server(args, state, site, version, port, setup_port, slots, name, addresses,
                                            ca_der)

    say()
    say(msg('running'))
    say()
    if addresses:
        say(msg('scan'))
        print_qr(phone_url)
        say('  ' + msg('or_type') + ' ' + phone_url)
        say('  ' + msg('library') + ' https://%s:%d/' % (name + '.local' if args.mdns_ok else addresses[0], port))
    say('  ' + msg('here') + ' http://localhost:%d/' % port)
    if args.mdns_ok:
        say('  ' + msg('name') + ' %s.local' % name)
    elif addresses and not args.no_mdns:
        say('  ' + msg('no_name', err=getattr(responder, 'error', '?')))
    say()
    for hint in firewall_hints(port, setup_port, slots):
        say('! ' + hint)
    say(msg('keep_open'))
    say()
    if not args.allow_sleep:
        keep_awake()
    if not args.no_browser:
        threading.Timer(0.5, webbrowser.open, ['http://localhost:%d/__setup/' % port]).start()

    if addresses:
        nag = threading.Timer(180, lambda: seen or say(msg('no_phone')))
        nag.daemon = True
        nag.start()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    say()
    say(msg('stopped'))
    os._exit(0)


if __name__ == '__main__':
    sys.exit(main())
