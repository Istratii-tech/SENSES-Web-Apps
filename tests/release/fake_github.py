#!/usr/bin/env python3
"""A stand-in for GitHub Releases, for the launcher (tools/release/launcher.py)
without GitHub: answers /repos/test/senses/releases/latest as the GitHub API
does and serves the assets under /dl/.

  python3 tests/release/fake_github.py DIR [--port 8560] [--host 127.0.0.1]

DIR holds release archives (SENSES-Web-Apps-VERSION-VARIANT.zip with their
.sha256, as make release builds them); the latest release is the highest
VERSION there, looked up at each request, so copying a newer archive into DIR
publishes it. Then:

  SENSES_UPDATE_URL=http://127.0.0.1:8560/repos/test/senses/releases/latest "./Start SENSES.command"

tests/release/launcher_test.py uses FakeGitHub with publish() instead.
"""

import argparse
import http.server
import json
import os
import shutil
import sys
import threading

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
                                'tools', 'release'))
import launcher  # noqa: E402

LATEST = '/repos/test/senses/releases/latest'


class FakeGitHub:
    def __init__(self, folder, port, host='127.0.0.1', tls=None, scan=False):
        self.dir = folder
        self.scan = scan
        self.verbose = False
        self.release = None
        self.downloads = []
        owner = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                name = self.path[4:] if self.path.startswith('/dl/') else ''
                path = os.path.join(owner.dir, name)
                release = owner.latest() if self.path == LATEST else None
                if release:
                    body = json.dumps(release).encode()
                    self.send_response(200)
                    self.send_header('Content-Type', 'application/json')
                    self.send_header('Content-Length', str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                elif name and '/' not in name and os.path.isfile(path):
                    owner.downloads.append(name)
                    self.send_response(200)
                    self.send_header('Content-Type', 'application/octet-stream')
                    self.send_header('Content-Length', str(os.path.getsize(path)))
                    self.end_headers()
                    with open(path, 'rb') as f:
                        try:
                            shutil.copyfileobj(f, self.wfile, 1 << 20)
                        except (BrokenPipeError, ConnectionResetError):
                            pass
                else:
                    self.send_error(404)

            def log_message(self, fmt, *a):
                if owner.verbose:
                    sys.stderr.write('%s %s\n' % (self.address_string(), fmt % a))

        self.port = port
        self.srv = http.server.ThreadingHTTPServer((host, port), Handler)
        if tls:
            self.srv.socket = tls.wrap_socket(self.srv.socket, server_side=True)
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()
        self.base = '%s://%s:%d' % ('https' if tls else 'http', host, port)
        self.url = self.base + LATEST

    def asset(self, name, digest=None):
        a = {'name': name, 'size': os.path.getsize(os.path.join(self.dir, name)),
             'browser_download_url': self.base + '/dl/' + name}
        if digest:
            a['digest'] = digest
        return a

    def publish(self, *zips, sums=None, digest=None):
        """Copies |zips| (and their .sha256) in and makes them the latest
        release; |sums| replaces the first one's .sha256, |digest| is
        GitHub's own "digest" field of the zips."""
        assets = []
        for z in zips:
            for f in (z, z + '.sha256'):
                shutil.copyfile(f, os.path.join(self.dir, os.path.basename(f)))
            name = os.path.basename(z)
            if sums and z == zips[0]:
                with open(os.path.join(self.dir, name + '.sha256'), 'w') as f:
                    f.write(sums)
            assets += [self.asset(name, digest), self.asset(name + '.sha256')]
        self.release = {'tag_name': 'v' + launcher.ASSET.match(os.path.basename(zips[0])).group('version'),
                        'assets': assets}

    def latest(self):
        if not self.scan:
            return self.release
        found = {}
        for name in os.listdir(self.dir):
            m = launcher.ASSET.match(name)
            if m:
                found.setdefault(m.group('version'), []).append(name)
        if not found:
            return None
        version = max(found, key=launcher.version_key)
        assets = []
        for name in sorted(found[version]):
            assets.append(self.asset(name))
            if os.path.exists(os.path.join(self.dir, name + '.sha256')):
                assets.append(self.asset(name + '.sha256'))
        return {'tag_name': 'v' + version, 'assets': assets}

    def stop(self):
        self.srv.shutdown()
        self.srv.server_close()


def main():
    ap = argparse.ArgumentParser(description='A local stand-in for GitHub Releases (the launcher\'s tests).')
    ap.add_argument('dir')
    ap.add_argument('--port', type=int, default=8560)
    ap.add_argument('--host', default='127.0.0.1')
    a = ap.parse_args()
    gh = FakeGitHub(os.path.abspath(a.dir), a.port, a.host, scan=True)
    gh.verbose = True
    print('SENSES_UPDATE_URL=%s' % gh.url, flush=True)
    try:
        threading.Event().wait()
    except KeyboardInterrupt:
        gh.stop()


if __name__ == '__main__':
    main()
