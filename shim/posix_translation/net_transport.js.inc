// The net thread's JavaScript (see net_bridge.h). net_bridge.cc embeds this
// file as a string and runs it on the net thread as
//   new Function('ctx', source)(ctx)
// where ctx = {errno: ERRNO_CODES, event(id, type, a, b, bytes, str),
// require, log}. It returns {command(op, id, host, port, family, bytes),
// resolve(name, family, done(code, addrs))} or null without a transport.
// Wire protocol of the relay: tools/net_relay.py.
R"JS(
'use strict';
// Events (net_bridge.cc: Event).
const OPEN = 1, OPEN_FAILED = 2, DATA = 3, EOF = 4, ERROR = 5, SENT = 6;
// Commands (net_bridge.cc: Op).
const TCP_OPEN = 1, UDP_OPEN = 2, SEND = 3, SEND_TO = 4, SHUT_WR = 5, CLOSE = 6;
// Resolve() results (net_bridge.cc).
const DNS_OK = 0, DNS_NOTFOUND = 1, DNS_AGAIN = 2;

const E = ctx.errno;
const errnoOf = (name) => (name && E[name]) || E.EIO;
const isNode = typeof process === 'object' && !!(process.versions && process.versions.node);
let relay = '';
if (isNode) {
  relay = process.env.ARC_NET_RELAY || '';
} else if (globalThis.location) {
  relay = new URL('/__net', globalThis.location.href).href.replace(/^http/, 'ws');
}
if (relay && typeof WebSocket !== 'function') return null;
if (!relay && !(isNode && ctx.require)) return null;

const conns = new Map();

// UDP datagram header, as on the relay's wire: [4|6][port BE][address].
function parseHeader(b) {
  const fam = b[0], port = (b[1] << 8) | b[2];
  let host;
  if (fam === 6) {
    const g = [];
    for (let i = 0; i < 8; i++) g.push(((b[3 + 2 * i] << 8) | b[4 + 2 * i]).toString(16));
    host = g.join(':');
  } else {
    host = b[3] + '.' + b[4] + '.' + b[5] + '.' + b[6];
  }
  return {fam, port, host, payload: b.subarray(fam === 6 ? 19 : 7)};
}

function v4First(addrs) {
  return addrs.filter((a) => a.indexOf(':') < 0).concat(addrs.filter((a) => a.indexOf(':') >= 0));
}

// --- relay: WebSockets to tools/net_relay.py ---------------------------------

const relayTransport = {
  open(id, path, onText, onBinary) {
    const ws = new WebSocket(relay + path);
    ws.binaryType = 'arraybuffer';
    const c = {ws, open: false, done: false, pending: [], total: 0, acked: 0, timer: 0};
    conns.set(id, c);
    ws.onmessage = (ev) => {
      if (c.done) return;
      if (typeof ev.data === 'string') onText(c, JSON.parse(ev.data));
      else onBinary(c, new Uint8Array(ev.data));
    };
    ws.onclose = () => {
      if (c.done) return;
      c.done = true;
      if (c.open) ctx.event(id, ERROR, E.ECONNRESET, 0, null, null);
      else ctx.event(id, OPEN_FAILED, E.ENETUNREACH, 0, null, null);
    };
    return c;
  },
  opened(c) {
    c.open = true;
    for (const m of c.pending) c.ws.send(m);
    c.pending = [];
  },
  tcpOpen(id, host, port) {
    relayTransport.open(id, '/tcp?host=' + encodeURIComponent(host) + '&port=' + port,
      (c, m) => {
        if (!c.open) {
          if (m.ok) {
            relayTransport.opened(c);
            ctx.event(id, OPEN, m.lport | 0, 0, null, m.local || '');
          } else {
            c.done = true;
            ctx.event(id, OPEN_FAILED, errnoOf(m.errno), 0, null, null);
            c.ws.close();
          }
        } else if (m.eof) {
          ctx.event(id, EOF, 0, 0, null, null);
        } else if (m.err) {
          c.done = true;
          ctx.event(id, ERROR, errnoOf(m.err), 0, null, null);
        }
      },
      (c, bytes) => ctx.event(id, DATA, 0, 0, bytes, null));
  },
  udpOpen(id, family) {
    relayTransport.open(id, '/udp?family=' + family,
      (c, m) => {
        if (!c.open && m.ok) {
          relayTransport.opened(c);
          ctx.event(id, OPEN, m.lport | 0, 0, null, family === 6 ? '::' : '0.0.0.0');
        }
      },
      (c, bytes) => {
        const d = parseHeader(bytes);
        ctx.event(id, DATA, d.port, 0, d.payload, d.host);
      });
  },
  send(id, bytes, isText) {
    const c = conns.get(id);
    if (!c || c.done) return;
    if (!c.open) { c.pending.push(bytes); return; }
    c.ws.send(bytes);
    if (isText) return;
    c.total += bytes.length;
    relayTransport.ack(id, c);
  },
  // Reports bytes that left the WebSocket's buffer (bufferedAmount counts
  // application data only, so total - bufferedAmount have been sent).
  ack(id, c) {
    if (c.timer) return;
    const check = () => {
      c.timer = 0;
      if (c.done) return;
      const buffered = c.ws.bufferedAmount;
      const sent = c.total - Math.min(buffered, c.total);
      if (sent > c.acked) {
        ctx.event(id, SENT, sent - c.acked, 0, null, null);
        c.acked = sent;
      }
      if (c.acked < c.total) c.timer = setTimeout(check, buffered > 65536 ? 8 : 2);
    };
    check();
  },
  shutWr(id) { relayTransport.send(id, '{"shut":"wr"}', true); },
  close(id) {
    const c = conns.get(id);
    if (!c) return;
    conns.delete(id);
    c.done = true;
    if (c.timer) clearTimeout(c.timer);
    if (!c.open && c.pending.length) {
      // Flush what was sent before the connection opened, then close.
      c.ws.onmessage = (ev) => {
        if (typeof ev.data !== 'string') return;
        for (const m of c.pending) c.ws.send(m);
        c.ws.close();
      };
      return;
    }
    c.ws.close();
  },
};

// --- direct: Node's net and dgram ---------------------------------------------

const directTransport = isNode && ctx.require ? {
  net: ctx.require('net'),
  dgram: ctx.require('dgram'),
  tcpOpen(id, host, port) {
    const s = directTransport.net.connect({host, port, allowHalfOpen: true});
    const c = {s, open: false, done: false, tcp: true};
    conns.set(id, c);
    s.setNoDelay(true);
    s.on('connect', () => {
      c.open = true;
      if (!c.done) ctx.event(id, OPEN, s.localPort | 0, 0, null, s.localAddress || '');
    });
    s.on('data', (b) => {
      if (!c.done) ctx.event(id, DATA, 0, 0, new Uint8Array(b.buffer, b.byteOffset, b.length), null);
    });
    s.on('end', () => { if (!c.done) ctx.event(id, EOF, 0, 0, null, null); });
    s.on('error', (e) => {
      if (c.done) return;
      c.done = true;
      ctx.event(id, c.open ? ERROR : OPEN_FAILED, errnoOf(e.code), 0, null, null);
    });
    s.on('close', () => {
      if (c.done) return;
      c.done = true;
      if (!c.open) ctx.event(id, OPEN_FAILED, E.ECONNREFUSED, 0, null, null);
    });
  },
  udpOpen(id, family) {
    const u = directTransport.dgram.createSocket(family === 6 ? 'udp6' : 'udp4');
    const c = {u, open: false, done: false, family};
    conns.set(id, c);
    u.on('message', (msg, rinfo) => {
      if (!c.done) {
        ctx.event(id, DATA, rinfo.port, 0, new Uint8Array(msg.buffer, msg.byteOffset, msg.length),
                  rinfo.address);
      }
    });
    u.on('error', (e) => {
      if (c.done) return;
      c.done = true;
      ctx.event(id, c.open ? ERROR : OPEN_FAILED, errnoOf(e.code), 0, null, null);
    });
    u.bind(0, () => {
      c.open = true;
      if (!c.done) ctx.event(id, OPEN, u.address().port, 0, null, family === 6 ? '::' : '0.0.0.0');
    });
  },
  send(id, bytes) {
    const c = conns.get(id);
    if (!c || c.done) return;
    const n = bytes.length;
    c.s.write(Buffer.from(bytes.buffer, bytes.byteOffset, n), () => {
      if (!c.done) ctx.event(id, SENT, n, 0, null, null);
    });
  },
  sendTo(id, bytes) {
    const c = conns.get(id);
    if (!c || c.done) return;
    const d = parseHeader(bytes);
    const host = (c.family === 6 && d.fam === 4) ? '::ffff:' + d.host : d.host;
    c.u.send(Buffer.from(d.payload.buffer, d.payload.byteOffset, d.payload.length), d.port, host,
             (e) => { if (e) ctx.log('udp send to ' + host + ': ' + e.message); });
  },
  shutWr(id) {
    const c = conns.get(id);
    if (c && c.tcp && !c.done) c.s.end();
  },
  close(id) {
    const c = conns.get(id);
    if (!c) return;
    conns.delete(id);
    c.done = true;
    if (c.tcp) {
      c.s.end();  // after the data written so far
      setTimeout(() => c.s.destroy(), 10000).unref();
    } else {
      try { c.u.close(); } catch (e) { /* not bound yet */ }
    }
  },
} : null;

const T = relay ? relayTransport : directTransport;

// --- DNS ---------------------------------------------------------------------

const cache = new Map();  // "family name" -> {addrs, time}
const CACHE_MS = 30000, DNS_TIMEOUT_MS = 15000;
let dnsSocket = null, dnsId = 0;
const dnsWaiting = new Map();  // id -> done

function relayResolve(name, family, done) {
  const id = ++dnsId;
  const timer = setTimeout(() => {
    if (dnsWaiting.delete(id)) done(DNS_AGAIN, []);
  }, DNS_TIMEOUT_MS);
  dnsWaiting.set(id, (code, addrs) => { clearTimeout(timer); done(code, addrs); });
  const msg = JSON.stringify({id, name, family});
  if (!dnsSocket) {
    const ws = new WebSocket(relay + '/dns');
    ws.pending = [];
    ws.onopen = () => { for (const m of ws.pending) ws.send(m); ws.pending = null; };
    ws.onmessage = (ev) => {
      const m = JSON.parse(ev.data);
      const cb = dnsWaiting.get(m.id);
      if (!cb) return;
      dnsWaiting.delete(m.id);
      if (m.addrs) cb(DNS_OK, m.addrs);
      else cb(m.error === 'EAI_AGAIN' ? DNS_AGAIN : DNS_NOTFOUND, []);
    };
    ws.onclose = () => {
      if (dnsSocket === ws) dnsSocket = null;
      for (const [k, cb] of dnsWaiting) { dnsWaiting.delete(k); cb(DNS_AGAIN, []); }
    };
    dnsSocket = ws;
  }
  if (dnsSocket.pending) dnsSocket.pending.push(msg);
  else dnsSocket.send(msg);
}

function directResolve(name, family, done) {
  ctx.require('dns').lookup(name, {all: true, family}, (e, list) => {
    if (e) {
      done((e.code === 'ENOTFOUND' || e.code === 'ENODATA') ? DNS_NOTFOUND : DNS_AGAIN, []);
    } else {
      done(DNS_OK, v4First(list.map((x) => x.address)));
    }
  });
}

function resolve(name, family, done) {
  const key = family + ' ' + name.toLowerCase();
  const hit = cache.get(key);
  if (hit && Date.now() - hit.time < CACHE_MS) { done(DNS_OK, hit.addrs); return; }
  (relay ? relayResolve : directResolve)(name, family, (code, addrs) => {
    if (code === DNS_OK) {
      if (!addrs.length) { done(DNS_NOTFOUND, []); return; }
      cache.set(key, {addrs, time: Date.now()});
    }
    done(code, addrs);
  });
}

function command(op, id, host, port, family, bytes) {
  switch (op) {
    case TCP_OPEN: T.tcpOpen(id, host, port); break;
    case UDP_OPEN: T.udpOpen(id, family); break;
    case SEND: T.send(id, bytes, false); break;
    case SEND_TO: (T.sendTo || T.send)(id, bytes, false); break;
    case SHUT_WR: T.shutWr(id); break;
    case CLOSE: T.close(id); break;
  }
}

return {command, resolve, kind: relay ? 'relay ' + relay : 'direct'};
)JS"
