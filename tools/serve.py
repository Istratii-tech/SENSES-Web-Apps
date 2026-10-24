#!/usr/bin/env python3
"""Static file server with the cross-origin isolation headers wasm threads
need (SharedArrayBuffer is only exposed to cross-origin isolated pages).

Usage: serve.py PORT DIRECTORY [LOGFILE]

Pages may POST text to /__log; each line is printed with the client's
browser name and, if LOGFILE is given, appended to it. This lets browsers
that cannot be automated (Safari) report test results.

/__net/{tcp,udp,dns} is the WebSocket network relay of tools/net_relay.py
(the kernel's sockets and DNS go through it). TCP and UDP echo servers for
the network tests listen on 127.0.0.1:PORT+1 (ARC_NET_ECHO=0: none).

For a phone on the local network (the PWA, docs/topics/userspace.md):
ARC_BIND=0.0.0.0 listens on every interface, and ARC_TLS_CERT plus
ARC_TLS_KEY (PEM files, e.g. from tools/make_lan_cert.sh) serve HTTPS, which
service workers and wasm threads need anywhere but localhost. The relay
then also serves pages loaded from this machine's own LAN address
(https://192.168.x.y:8443/...): the phone's apps get the network.

ARC_CACHE=1: the big files of a VM (.wasm, .data, .img) may be cached and
are revalidated (Last-Modified -> 304) instead of no-store, so repeated
phone runs do not download a 65 MB image each time.

ARC_APP_PORTS=8162-8169 (or 8162,8163,...): the same directory also on these
ports, each its own origin (SENSES Web Apps puts each app on one of them:
docs/topics/per-app-origin.md), and /origins.json lists them for the pages
({"ports": [...]}) unless the directory has its own; the library page
(index.html) is then served without COOP/COEP, as its service worker serves
it. origin.html (the page an app's files move through) is served so always:
the window that opened it must keep its handle on it.
"""

import functools
import http.server
import json
import os
import ssl
import sys
import threading
import time

import net_relay

CACHE_BIG = os.environ.get('ARC_CACHE') == '1'


class IsolatedHandler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {
        **http.server.SimpleHTTPRequestHandler.extensions_map,
        '.wasm': 'application/wasm',
        '.js': 'text/javascript',
        '.webmanifest': 'application/manifest+json',
    }

    def end_headers(self):
        if os.path.basename(self.path.split('?')[0]) not in UNISOLATED:
            self.send_header('Cross-Origin-Opener-Policy', 'same-origin')
            self.send_header('Cross-Origin-Embedder-Policy', 'require-corp')
        big = os.path.splitext(self.path.split('?')[0])[1] in ('.wasm', '.data', '.img')
        self.send_header('Cache-Control', 'no-cache' if CACHE_BIG and big else 'no-store')
        super().end_headers()

    def do_GET(self):
        if net_relay.handle(self):
            return
        if APP_PORTS and self.path.split('?')[0].endswith('/origins.json') and \
                not os.path.exists(self.translate_path(self.path)):
            body = json.dumps({'ports': APP_PORTS}).encode()
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        super().do_GET()

    def do_POST(self):
        if self.path != '/__log':
            self.send_error(404)
            return
        body = self.rfile.read(int(self.headers.get('Content-Length', 0)))
        agent = browser_name(self.headers.get('User-Agent', ''))
        stamp = time.strftime('%H:%M:%S')
        lines = ['%s [%s] %s' % (stamp, agent, line)
                 for line in body.decode('utf-8', 'replace').splitlines()]
        if ECHO_LOG:
            sys.stdout.write(''.join(l + '\n' for l in lines))
            sys.stdout.flush()
        if LOGFILE:
            with open(LOGFILE, 'a') as f:
                f.writelines(l + '\n' for l in lines)
        self.send_response(204)
        self.end_headers()

    def log_message(self, fmt, *args):
        if self.command != 'POST':
            super().log_message(fmt, *args)


LOGFILE = None
ECHO_LOG = True  # the pages' /__log lines on stdout too (tools/release: only in its file)
UNISOLATED = {'origin.html'}


def parse_ports(text):
    ports = []
    for part in filter(None, (text or '').replace(' ', '').split(',')):
        a, _, b = part.partition('-')
        ports.extend(range(int(a), int(b or a) + 1))
    return ports


APP_PORTS = parse_ports(os.environ.get('ARC_APP_PORTS'))
if APP_PORTS:
    # The library page too (index.html, the directory): its service worker
    # serves it so anyway, and before that worker is there a window it opens
    # on an app's origin would be cut off from it.
    UNISOLATED |= {'index.html', ''}


def browser_name(agent):
    if 'Chrome/' in agent:
        return 'chrome'
    if 'Firefox/' in agent:
        return 'firefox'
    if 'Safari/' in agent:
        return 'safari'
    return 'other'


class Server(http.server.ThreadingHTTPServer):
    """With TLS, each connection's handshake runs on its own thread, with a
    timeout. Wrapping the listening socket did it in accept(), on the one
    thread that accepts: a phone that connected and then slept mid-handshake
    stopped the server for everyone (the PWA got no new builds)."""
    tls = None
    HANDSHAKE_TIMEOUT = 20
    # On Windows SO_REUSEADDR lets a second server bind a port in use.
    allow_reuse_address = os.name != 'nt'

    def finish_request(self, request, client_address):
        if self.tls is None:
            return super().finish_request(request, client_address)
        try:
            request.settimeout(self.HANDSHAKE_TIMEOUT)
            conn = self.tls.wrap_socket(request, server_side=True)
            conn.settimeout(None)
        except (OSError, ssl.SSLError):
            return  # gone or not TLS: the next one
        try:
            super().finish_request(conn, client_address)
        finally:
            try:
                conn.close()
            except OSError:
                pass


def main(argv):
    global LOGFILE
    port, directory = int(argv[1]), argv[2]
    LOGFILE = argv[3] if len(argv) > 3 else None
    handler = functools.partial(IsolatedHandler, directory=directory)
    bind = os.environ.get('ARC_BIND', '127.0.0.1')
    cert, key = os.environ.get('ARC_TLS_CERT'), os.environ.get('ARC_TLS_KEY')
    tls = None
    if cert:
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.load_cert_chain(cert, key)
    scheme = 'https' if tls else 'http'
    # The apps' origins: the same files on more ports, a thread each.
    for p in APP_PORTS:
        if p == port:
            continue
        extra = Server((bind, p), handler)
        extra.tls = tls
        threading.Thread(target=extra.serve_forever, daemon=True).start()
    if APP_PORTS:
        print('apps\' origins on ports %s' % ','.join(map(str, APP_PORTS)))
    with Server((bind, port), handler) as srv:
        srv.tls = tls
        print('serving %s on %s://%s:%d' % (directory, scheme, bind, port))
        if os.environ.get('ARC_NET_ECHO', '1') != '0' and \
                net_relay.start_echo_servers(port + 1):
            print('echo (tcp, udp) on 127.0.0.1:%d' % (port + 1))
        sys.stdout.flush()
        srv.serve_forever()


if __name__ == '__main__':
    main(sys.argv)
