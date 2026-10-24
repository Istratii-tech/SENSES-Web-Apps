"""WebSocket relay that gives arc-wasm's kernel TCP, UDP and DNS.

Browsers cannot open raw sockets, so posix_translation's TCPSocket/UDPSocket
(shim/posix_translation/net_*.cc) talk to this relay over WebSockets and the
relay does the real networking. Imported by tools/serve.py; standard library
only. The protocol (docs/topics/net.md has the full description):

  GET /__net/tcp?host=H&port=P   one WebSocket per TCP connection. The relay
      connects, then sends a text frame {"ok":true,"local":IP,"lport":N} or
      {"ok":false,"errno":"ECONNREFUSED"} (and closes). After that binary
      frames carry the byte stream both ways. Text frames from the relay:
      {"eof":true} (peer sent FIN), {"err":"ECONNRESET"} (then it closes).
      Text frames to the relay: {"shut":"wr"} = shutdown(SHUT_WR). Closing
      the WebSocket closes the TCP connection after the data sent before.
  GET /__net/udp?family=4|6      one WebSocket per UDP socket (bound to an
      ephemeral port). {"ok":true,"lport":N} first; then every binary frame
      is one datagram: [4|6][port, 2 bytes BE][address, 4|16 bytes][payload].
  GET /__net/dns                 text frames {"id":N,"name":S,"family":0|4|6}
      -> {"id":N,"addrs":[IP...]} or {"id":N,"error":"ENOTFOUND"|"EAI_AGAIN"}.

Security: serve.py listens on 127.0.0.1 only. A WebSocket handshake is not
subject to CORS, so any web page the user visits could try ws://localhost;
the relay therefore accepts only requests whose Host is this server
(localhost/127.0.0.1/[::1] with its port: stops DNS rebinding) and whose
Origin, if present, is this server too (stops other sites). Clients without
an Origin are local programs (Node), which can reach the network anyway.
With ARC_BIND set to another address (a phone on the LAN loading the pages
from https://192.168.x.y:8443), the Host may also be one of this machine's
own IP addresses with the server's port -- an address literal, never a name
(names could be rebound) -- and the Origin then the same, with the server's
scheme.
ARC_NET=0 disables the relay; ARC_NET_LOCAL=0 refuses loopback, private and
link-local destinations; ARC_NET_DENY=NAME,... answers lookups of those host
names (and their subdomains) "not found" (an app's backend off, the rest on).
ARC_NET_SELF=0 refuses this computer itself (loopback and its own addresses,
also when a name resolves to them): the local release (tools/release), where
anyone on the home network can reach the relay, keeps the computer's own
services out of its reach. ARC_NET_HOSTS=NAME,... are further names the
server is reached by (the release's NAME.local), accepted as Host like its
own addresses. This is a development relay: a deployment needs its
own, with authentication, quotas and a destination policy.
"""

import base64
import hashlib
import ipaddress
import json
import os
import socket
import struct
import sys
import threading
import time
import urllib.parse

GUID = b'258EAFA5-E914-47DA-95CA-C5AB0DC85B11'
CONNECT_TIMEOUT = float(os.environ.get('ARC_NET_CONNECT_TIMEOUT', '20'))
ENABLED = os.environ.get('ARC_NET', '1') != '0'
ALLOW_LOCAL = os.environ.get('ARC_NET_LOCAL', '1') != '0'
VERBOSE = os.environ.get('ARC_NET_VERBOSE', '0') != '0'
DENY = [n.strip().lower().rstrip('.') for n in os.environ.get('ARC_NET_DENY', '').split(',') if n.strip()]
ALLOW_SELF = os.environ.get('ARC_NET_SELF', '1') != '0'
HOSTS = {n.strip().lower().rstrip('.') for n in os.environ.get('ARC_NET_HOSTS', '').split(',') if n.strip()}


def denied(name):
    name = name.lower().rstrip('.')
    return any(name == d or name.endswith('.' + d) for d in DENY)


def log(msg):
    if VERBOSE:
        sys.stderr.write('[net] %s\n' % msg)
        sys.stderr.flush()


class Closed(Exception):
    pass


class WebSocket:
    """Server side of RFC 6455 over a connected socket (no extensions)."""

    def __init__(self, sock, rfile, wfile):
        self.sock = sock
        self.rfile = rfile
        self.wfile = wfile
        self.lock = threading.Lock()
        self.closed = False

    def _read(self, n):
        data = self.rfile.read(n)
        if data is None or len(data) < n:
            raise Closed()
        return data

    def recv(self):
        """Returns (is_text, payload) of the next message; raises Closed."""
        message, text = b'', None
        while True:
            b0, b1 = self._read(2)
            fin, opcode = b0 & 0x80, b0 & 0x0f
            n = b1 & 0x7f
            if n == 126:
                n = struct.unpack('>H', self._read(2))[0]
            elif n == 127:
                n = struct.unpack('>Q', self._read(8))[0]
            mask = self._read(4) if b1 & 0x80 else None
            payload = self._read(n)
            if mask:
                payload = bytes(b ^ mask[i & 3] for i, b in enumerate(payload)) \
                    if n < 64 else _unmask(payload, mask)
            if opcode == 8:  # close
                self.close()
                raise Closed()
            if opcode == 9:  # ping
                self._send(10, payload)
                continue
            if opcode == 10:  # pong
                continue
            if opcode in (1, 2):
                text = opcode == 1
                message = payload
            elif opcode == 0:
                message += payload
            if fin:
                return text, message

    def _send(self, opcode, payload):
        n = len(payload)
        if n < 126:
            header = struct.pack('>BB', 0x80 | opcode, n)
        elif n < 65536:
            header = struct.pack('>BBH', 0x80 | opcode, 126, n)
        else:
            header = struct.pack('>BBQ', 0x80 | opcode, 127, n)
        with self.lock:
            if self.closed and opcode != 8:
                raise Closed()
            try:
                self.wfile.write(header + payload)
                self.wfile.flush()
            except OSError:
                self.closed = True
                raise Closed()

    def send_binary(self, data):
        self._send(2, data)

    def send_json(self, obj):
        self._send(1, json.dumps(obj).encode())

    def close(self):
        if self.closed:
            return
        try:
            self._send(8, struct.pack('>H', 1000))
        except Closed:
            pass
        self.closed = True


def _unmask(payload, mask):
    # XOR in big integers: much faster than a per-byte loop in Python.
    n = len(payload)
    key = (mask * (n // 4 + 1))[:n]
    return (int.from_bytes(payload, 'big') ^ int.from_bytes(key, 'big')) \
        .to_bytes(n, 'big')


def errno_name(e):
    if isinstance(e, socket.timeout):
        return 'ETIMEDOUT'
    if isinstance(e, socket.gaierror):
        return 'EHOSTUNREACH'
    code = getattr(e, 'errno', None)
    if code:
        import errno as errno_mod
        return errno_mod.errorcode.get(code, 'EIO')
    return 'EIO'


def destination_allowed(host):
    if ALLOW_LOCAL and ALLOW_SELF:
        return True
    try:
        ip = ipaddress.ip_address(host.split('%')[0])
    except ValueError:
        return True  # Names are resolved by the OS; checked after connect.
    if ip.version == 6 and ip.ipv4_mapped:
        ip = ip.ipv4_mapped
    if not ALLOW_SELF and (ip.is_loopback or ip.is_unspecified or own_address(str(ip))):
        return False
    return ALLOW_LOCAL or not (ip.is_loopback or ip.is_private or ip.is_link_local or
                               ip.is_unspecified or ip.is_multicast)


_own_addresses = {}


def own_address(host):
    """Whether |host| (an address literal) is one of this machine's."""
    if host not in _own_addresses:
        try:
            ip = ipaddress.ip_address(host)
            with socket.socket(socket.AF_INET6 if ip.version == 6 else socket.AF_INET,
                               socket.SOCK_DGRAM) as s:
                s.bind((host, 0))  # only works for an address of this machine
            _own_addresses[host] = True
        except (ValueError, OSError):
            _own_addresses[host] = False
    return _own_addresses[host]


def allowed(handler):
    """Host/Origin checks; see the module docstring."""
    server = handler.server
    port = server.server_address[1]
    hosts = {'localhost:%d' % port, '127.0.0.1:%d' % port, '[::1]:%d' % port}
    host = handler.headers.get('Host', '')
    schemes = {'http'}
    if host not in hosts:
        # The LAN (ARC_BIND): this machine's own address, as a literal.
        if server.server_address[0] in ('127.0.0.1', 'localhost', '::1'):
            return False
        name, sep, p = host.rpartition(':')
        if not sep or p != str(port) or not (name.lower() in HOSTS or own_address(name.strip('[]'))):
            return False
        hosts = {host}
        schemes = {'https' if getattr(server, 'tls', None) else 'http'}
    origin = handler.headers.get('Origin')
    if origin is not None and origin not in {s + '://' + h for s in schemes for h in hosts}:
        return False
    return True


def handle(handler):
    """Serves a /__net/ request on an http.server handler. Returns True if
    the path was a relay path (whether or not it was accepted)."""
    url = urllib.parse.urlsplit(handler.path)
    if not url.path.startswith('/__net/'):
        return False
    kind = url.path[len('/__net/'):]
    if not ENABLED or kind not in ('tcp', 'udp', 'dns'):
        handler.send_error(404)
        return True
    if not allowed(handler):
        log('refused %s from Host=%s Origin=%s' % (
            handler.path, handler.headers.get('Host'),
            handler.headers.get('Origin')))
        handler.send_error(403, 'arc-wasm net relay: foreign origin')
        return True
    key = handler.headers.get('Sec-WebSocket-Key')
    if handler.headers.get('Upgrade', '').lower() != 'websocket' or not key:
        handler.send_error(426, 'WebSocket required')
        return True
    accept = base64.b64encode(hashlib.sha1(key.encode() + GUID).digest())
    handler.close_connection = True
    handler.wfile.write(b'HTTP/1.1 101 Switching Protocols\r\n'
                        b'Upgrade: websocket\r\nConnection: Upgrade\r\n'
                        b'Sec-WebSocket-Accept: ' + accept + b'\r\n\r\n')
    handler.wfile.flush()
    ws = WebSocket(handler.connection, handler.rfile, handler.wfile)
    query = dict(urllib.parse.parse_qsl(url.query))
    try:
        {'tcp': serve_tcp, 'udp': serve_udp, 'dns': serve_dns}[kind](ws, query)
    except Closed:
        pass
    finally:
        ws.close()
    return True


def serve_tcp(ws, query):
    host, port = query.get('host', ''), int(query.get('port', '0'))
    if not destination_allowed(host):
        ws.send_json({'ok': False, 'errno': 'EACCES'})
        return
    try:
        conn = socket.create_connection((host, port), timeout=CONNECT_TIMEOUT)
    except OSError as e:
        log('tcp %s:%d: %s' % (host, port, e))
        ws.send_json({'ok': False, 'errno': errno_name(e)})
        return
    if not destination_allowed(conn.getpeername()[0]):  # a name for a refused address
        conn.close()
        ws.send_json({'ok': False, 'errno': 'EACCES'})
        return
    conn.settimeout(None)
    conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    local = conn.getsockname()
    log('tcp %s:%d connected from %s:%d' % (host, port, local[0], local[1]))
    ws.send_json({'ok': True, 'local': local[0], 'lport': local[1]})
    counts = {'up': 0, 'down': 0, 'end': 'client closed', 't0': time.monotonic(), 'times': []}

    def mark(what, n):  # the first few transfers, with ms since connect
        if len(counts['times']) < 8:
            counts['times'].append('%s%d@%d' % (what, n, (time.monotonic() - counts['t0']) * 1000))

    def pump():  # TCP -> WebSocket
        try:
            while True:
                data = conn.recv(65536)
                if not data:
                    counts['end'] = 'server eof'
                    ws.send_json({'eof': True})
                    return
                counts['down'] += len(data)
                mark('<', len(data))
                ws.send_binary(data)
        except Closed:
            pass
        except OSError as e:
            counts['end'] = 'server ' + errno_name(e)
            try:
                ws.send_json({'err': errno_name(e)})
            except Closed:
                pass
            ws.close()
            # Unblock recv() in serve_tcp's loop below.
            try:
                ws.sock.shutdown(socket.SHUT_RD)
            except OSError:
                pass

    reader = threading.Thread(target=pump, daemon=True)
    reader.start()
    try:
        while True:  # WebSocket -> TCP
            text, data = ws.recv()
            if text:
                if json.loads(data).get('shut') == 'wr':
                    conn.shutdown(socket.SHUT_WR)
                continue
            counts['up'] += len(data)
            mark('>', len(data))
            conn.sendall(data)
    except (Closed, OSError):
        pass
    finally:
        log('tcp %s:%d from port %d closed after %d ms: %d bytes up, %d down, %s [%s]' % (
            host, port, local[1], (time.monotonic() - counts['t0']) * 1000, counts['up'],
            counts['down'], counts['end'], ' '.join(counts['times'])))
        try:
            conn.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        conn.close()


def serve_udp(ws, query):
    family = socket.AF_INET6 if query.get('family') == '6' else socket.AF_INET
    sock = socket.socket(family, socket.SOCK_DGRAM)
    sock.bind(('::' if family == socket.AF_INET6 else '0.0.0.0', 0))
    ws.send_json({'ok': True, 'lport': sock.getsockname()[1]})

    def pump():
        try:
            while True:
                data, addr = sock.recvfrom(65536)
                ip = ipaddress.ip_address(addr[0].split('%')[0])
                fam = 6 if ip.version == 6 else 4
                ws.send_binary(struct.pack('>BH', fam, addr[1]) + ip.packed + data)
        except (Closed, OSError):
            pass

    threading.Thread(target=pump, daemon=True).start()
    try:
        while True:
            text, data = ws.recv()
            if text or len(data) < 3:
                continue
            fam, port = struct.unpack('>BH', data[:3])
            n = 16 if fam == 6 else 4
            ip = ipaddress.ip_address(data[3:3 + n])
            host = str(ip)
            if family == socket.AF_INET6 and ip.version == 4:
                host = '::ffff:' + host
            if not destination_allowed(str(ip)):
                continue
            try:
                sock.sendto(data[3 + n:], (host, port))
            except OSError as e:
                log('udp sendto %s:%d: %s' % (host, port, e))
    except Closed:
        pass
    finally:
        sock.close()


def serve_dns(ws, query):
    def lookup(req):
        name, family = req.get('name', ''), req.get('family', 0)
        fam = {4: socket.AF_INET, 6: socket.AF_INET6}.get(family, socket.AF_UNSPEC)
        try:
            if denied(name):
                raise socket.gaierror(socket.EAI_NONAME, 'denied (ARC_NET_DENY)')
            infos = socket.getaddrinfo(name, None, fam, socket.SOCK_STREAM)
            addrs = []
            for info in sorted(infos, key=lambda i: i[0] != socket.AF_INET):
                a = info[4][0].split('%')[0]
                if a not in addrs:
                    addrs.append(a)
            reply = {'id': req.get('id'), 'addrs': addrs}
        except socket.gaierror as e:
            again = e.errno in (socket.EAI_AGAIN, getattr(socket, 'EAI_SYSTEM', -11))
            reply = {'id': req.get('id'), 'error': 'EAI_AGAIN' if again else 'ENOTFOUND'}
        log('dns %s -> %s' % (name, reply))
        try:
            ws.send_json(reply)
        except Closed:
            pass

    while True:
        text, data = ws.recv()
        if text:
            threading.Thread(target=lookup, args=(json.loads(data),),
                             daemon=True).start()


def start_echo_servers(port):
    """TCP and UDP echo on 127.0.0.1:port, for the kernel and Java tests."""
    def tcp_client(conn):
        with conn:
            try:
                while True:
                    data = conn.recv(65536)
                    if not data:
                        return
                    conn.sendall(data)
            except OSError:
                pass

    def tcp_loop(srv):
        while True:
            conn, _ = srv.accept()
            threading.Thread(target=tcp_client, args=(conn,), daemon=True).start()

    def udp_loop(sock):
        while True:
            data, addr = sock.recvfrom(65536)
            sock.sendto(data, addr)

    try:
        srv = socket.create_server(('127.0.0.1', port))
        udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        udp.bind(('127.0.0.1', port))
    except OSError as e:
        sys.stderr.write('net relay: no echo servers on port %d: %s\n' % (port, e))
        return False
    threading.Thread(target=tcp_loop, args=(srv,), daemon=True).start()
    threading.Thread(target=udp_loop, args=(udp,), daemon=True).start()
    return True


def main(argv):
    """net_relay.py --echo PORT: only the echo servers (PORT 0: any free
    one); prints the port, then serves until killed. For tests without
    serve.py (tools/run_java_tests.sh node)."""
    if len(argv) != 3 or argv[1] != '--echo':
        sys.exit('usage: net_relay.py --echo PORT')
    port = int(argv[2])
    for _ in range(20):
        if port == 0:
            with socket.socket() as probe:
                probe.bind(('127.0.0.1', 0))
                candidate = probe.getsockname()[1]
        else:
            candidate = port
        if start_echo_servers(candidate):
            print(candidate)
            sys.stdout.flush()
            threading.Event().wait()
        if port:
            break
    sys.exit(1)


if __name__ == '__main__':
    main(sys.argv)
