"""A name for the server on the home network: a minimal mDNS responder
(RFC 6762) that answers A queries for a few .local names (the server's, and
the setup page's check name) with this computer's LAN address.

Why a name: the phone keeps the apps' data per origin (scheme, host, port).
With an address in it, a router that hands the computer another address one
day would leave the phone's apps behind on the old origin. The name stays.
iPhones, Macs, Windows 10+, most Linux desktops and Android 12+ resolve .local
names; where one does not, the setup page falls back to the address.

It shares port 5353 with the system's own responder (mDNSResponder on macOS,
the DNS client on Windows, Avahi) through SO_REUSEADDR/SO_REUSEPORT; if the
port cannot be shared, start() returns False and only the address is used.
"""

import socket
import struct
import threading
import time

GROUP = '224.0.0.251'
PORT = 5353
TTL = 120


def _read_name(data, pos):
    labels, jumped, end = [], False, None
    for _ in range(64):
        n = data[pos]
        if n == 0:
            pos += 1
            break
        if n & 0xc0 == 0xc0:
            if not jumped:
                end = pos + 2
            pos = ((n & 0x3f) << 8) | data[pos + 1]
            jumped = True
            continue
        labels.append(data[pos + 1:pos + 1 + n].decode('utf-8', 'replace'))
        pos += 1 + n
    return '.'.join(labels).lower(), (end if jumped else pos)


def _encode_name(name):
    out = b''
    for label in name.rstrip('.').split('.'):
        raw = label.encode('utf-8')
        out += bytes([len(raw)]) + raw
    return out + b'\x00'


class Responder:
    def __init__(self, names, address):
        if isinstance(names, str):
            names = [names]
        self.names = [n.lower().rstrip('.') for n in names]
        self.address = address
        self.sock = None
        self.queries = 0

    def start(self):
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            if hasattr(socket, 'SO_REUSEPORT'):
                try:
                    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
                except OSError:
                    pass
            sock.bind(('', PORT))
            mreq = socket.inet_aton(GROUP) + socket.inet_aton(self.address)
            sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
            sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(self.address))
            sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 255)
            sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)
        except OSError as e:
            self.error = e
            return False
        self.sock = sock
        threading.Thread(target=self._loop, daemon=True).start()
        threading.Thread(target=self._announce, daemon=True).start()
        return True

    def _record(self, qname, qtype=1):
        name = _encode_name(qname)
        if qtype == 28:
            # No IPv6 address: an NSEC saying the name has only an A record
            # (RFC 6762 6.1), so resolvers do not wait out the AAAA query.
            rdata = name + bytes([0, 1, 0x40])
            return name + struct.pack('>HHIH', 47, 0x8001, TTL, len(rdata)) + rdata
        return name + struct.pack('>HHIH', 1, 0x8001, TTL, 4) + socket.inet_aton(self.address)

    def _announce(self):
        # Two unsolicited answers, a second apart (RFC 6762 8.3): caches that
        # held another address for the name drop it.
        for _ in range(2):
            for n in self.names:
                self._send(struct.pack('>HHHHHH', 0, 0x8400, 0, 1, 0, 0) + self._record(n), (GROUP, PORT))
            time.sleep(1)

    def _send(self, packet, dest):
        try:
            self.sock.sendto(packet, dest)
        except OSError:
            pass

    def _loop(self):
        while True:
            try:
                data, src = self.sock.recvfrom(9000)
                self._query(data, src)
            except OSError:
                time.sleep(1)
            except (IndexError, struct.error, UnicodeError):
                pass  # not a well-formed query

    def _query(self, data, src):
        qid, flags, qd = struct.unpack('>HHH', data[:6])
        if flags & 0x8000:
            return  # a response
        pos = 12
        for _ in range(qd):
            qname, pos = _read_name(data, pos)
            qtype, qclass = struct.unpack('>HH', data[pos:pos + 4])
            pos += 4
            if qname not in self.names or qtype not in (1, 28, 255) or qclass & 0x7fff not in (1, 255):
                continue
            self.queries += 1
            record = self._record(qname, 28 if qtype == 28 else 1)
            if src[1] != PORT:
                # A legacy one-shot query (RFC 6762 6.7): answered to its
                # sender, with its id and question.
                q = _encode_name(qname) + struct.pack('>HH', qtype, 1)
                self._send(struct.pack('>HHHHHH', qid, 0x8400, 1, 1, 0, 0) + q + record, src)
                continue
            packet = struct.pack('>HHHHHH', 0, 0x8400, 0, 1, 0, 0) + record
            self._send(packet, src if qclass & 0x8000 else (GROUP, PORT))
