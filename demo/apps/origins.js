// SENSES Web Apps: each app on its own origin (topic per-app-origin,
// docs/topics/per-app-origin.md).
//
// Safari before 17 gives an origin 1000 MB for OPFS, Cache Storage and
// IndexedDB together (docs/topics/opfs-after-kill.md): all the apps of one
// library shared it. An app moved to an origin of its own (a port or a
// subdomain of the library's host, one "slot" each) gets its own 1000 MB,
// and a WebKit process of its own when it runs. The library keeps a record of
// it (senses/remote/PACKAGE: meta.json with meta.origin, icon.png) and opens
// its run.html there; the app's page comes back to the library (?home=)
// saying how much memory and space it took.
//
// The slots: origins.json next to the library (tools/serve.py makes one for
// its ARC_APP_PORTS; on a static host it is a file of the site):
//   {"ports": [8162, 8163, ...]}          same scheme and host name, these ports
//   {"hosts": "app{n}.example.com", "count": 32, "home": ["https://example.com"]}
//                                          subdomains app1..app32; "home": the
//                                          library origins an app's origin obeys
// The same path as the library's on each. No origins.json: no slots, all apps
// stay in the library's origin, as before.
//
// When: ?origins=1|0|auto on the library (kept in its localStorage). auto, the
// default: only where the quota is Safari 16's (no navigator.storage.estimate;
// Safari 17+ and Chrome give an origin a share of the disk) and not in a
// Home Screen web app (there a link to another origin leaves the app: not
// checked yet).
//
// Storage of another origin is out of reach in Safari (an iframe of it gets a
// partition of its own, keyed by the top page, with a tenth of the quota), so
// files go from page to page: the library opens the app origin's origin.html
// in a window of its own (window.open, from a tap) and talks to it with
// postMessage (connect(): the library's side; serve(): origin.html's). The
// window shares the library's process (it has an opener), so the library
// keeps running while it is in front.
(function (root) {
  'use strict';
  var MODE_KEY = 'senses-origins', HOME_KEY = 'senses-home';
  var dirPath = location.pathname.replace(/[^/]*$/, '');

  // --- configuration ---------------------------------------------------------------
  var cfgPromise = null;
  // -> {slots: [base URL], homes: [origin]} or null.
  function config() {
    if (!cfgPromise) {
      // (sw.js answers within 3 s from its copy; without a worker in charge,
      // a host out of reach must not hold the page either: no slots then)
      var late = new Promise(function (resolve) { setTimeout(resolve, 5000, null); });
      cfgPromise = Promise.race([fetch('origins.json', {cache: 'no-store'}).then(function (r) {
        if (!r.ok) return null;
        return r.json().then(normalize);
      }).catch(function () { return null; }), late]);
    }
    return cfgPromise;
  }
  function normalize(j) {
    var slots = [];
    if (j && Array.isArray(j.ports)) {
      j.ports.forEach(function (p) {
        p = +p;
        if (p > 0 && p < 65536 && String(p) !== location.port)
          slots.push(location.protocol + '//' + location.hostname + ':' + p + dirPath);
      });
    } else if (j && typeof j.hosts === 'string' && j.count > 0) {
      for (var n = 1; n <= Math.min(+j.count, 1000); n++) {
        var host = j.hosts.replace('{n}', String(n));
        if (host !== location.host) slots.push(location.protocol + '//' + host + dirPath);
      }
    }
    if (!slots.length) return null;
    var homes = [].concat(j.home || []).map(function (h) {
      try { return new URL(h).origin; } catch (e) { return null; }
    }).filter(Boolean);
    return {slots: slots, homes: homes};
  }
  function originOf(url) {
    try { return new URL(url, location.href).origin; } catch (e) { return ''; }
  }
  // An origin of this site (the same host name: other ports), a slot's or a
  // library's named in origins.json: one an app's origin may obey, and one a
  // page may be sent back to.
  function trusted(origin, cfg) {
    var u;
    try { u = new URL(origin); } catch (e) { return false; }
    if (u.protocol !== location.protocol) return false;
    if (u.hostname === location.hostname) return true;
    if (!cfg) return false;
    return cfg.homes.indexOf(u.origin) >= 0 || cfg.slots.some(function (s) { return originOf(s) === u.origin; });
  }

  // 'on' | 'off' | 'auto', from ?origins= (kept) or what was kept.
  function mode(params) {
    var v = params && params.get('origins');
    try {
      if (v === '1' || v === 'on') localStorage.setItem(MODE_KEY, 'on');
      else if (v === '0' || v === 'off') localStorage.setItem(MODE_KEY, 'off');
      else if (v === 'auto') localStorage.removeItem(MODE_KEY);
      return localStorage.getItem(MODE_KEY) || 'auto';
    } catch (e) {
      return v === '1' || v === 'on' ? 'on' : v === '0' || v === 'off' ? 'off' : 'auto';
    }
  }
  // Safari 16: no estimate(), a fixed 1000 MB an origin.
  function quotaGuessed() { return !(navigator.storage && navigator.storage.estimate); }
  function standalone() {
    return navigator.standalone === true || matchMedia('(display-mode: standalone)').matches;
  }
  // Whether new apps go to origins of their own (and apps of the library's
  // origin move there when opened).
  function wanted(cfg, m) {
    if (!cfg || !cfg.slots.length || m === 'off') return false;
    return m === 'on' || (quotaGuessed() && !standalone());
  }
  // The first slot no app of |apps| ([meta]) has.
  function freeSlot(cfg, apps) {
    if (!cfg) return null;
    var used = {};
    apps.forEach(function (m) { if (m.origin) used[m.origin] = true; });
    for (var i = 0; i < cfg.slots.length; i++) if (!used[cfg.slots[i]]) return cfg.slots[i];
    return null;
  }
  // The library this page belongs to: here, unless an app's origin was given
  // one (?home= on run.html, origin.html's setHome).
  function home() {
    var h = null;
    try { h = localStorage.getItem(HOME_KEY); } catch (e) {}
    return h || null;
  }
  function setHome(url) {
    try { localStorage.setItem(HOME_KEY, url); } catch (e) {}
  }
  function library() { return location.origin + dirPath; }

  // --- the library's side ----------------------------------------------------------
  // connect(base) -> {ready: Promise<hello>, call(op, args, transfer) -> Promise,
  // go(base) -> Promise<hello>, close()}. Must run in a tap's handler (a window).
  var READY_MS = 30000;
  function connect(base) {
    var w = window.open(base + 'origin.html', 'senses-origin', 'popup,width=440,height=360');
    var expect = originOf(base), nextId = 1, pending = {}, onReady = null, closedTimer = 0;
    function onMessage(e) {
      var m = e.data;
      if (!w || e.source !== w || e.origin !== expect || !m || m.senses !== 'origin') return;
      if (m.ready) { if (onReady) { var r = onReady; onReady = null; r.ok(m); } return; }
      var p = pending[m.id];
      if (!p) return;
      delete pending[m.id];
      if (m.ok) p.ok(m.result);
      else {
        var err = new Error(m.error && m.error.message || 'error');
        err.name = m.error && m.error.name || 'Error';
        p.no(err);
      }
    }
    window.addEventListener('message', onMessage);
    function failAll(err) {
      Object.keys(pending).forEach(function (id) { pending[id].no(err); });
      pending = {};
      if (onReady) { var r = onReady; onReady = null; r.no(err); }
    }
    function waitReady() {
      return new Promise(function (ok, no) {
        onReady = {ok: ok, no: no};
        setTimeout(function () {
          if (onReady) { onReady = null; no(new Error('нет ответа от ' + expect)); }
        }, READY_MS);
      });
    }
    var agent = {
      base: base,
      ready: null,
      call: function (op, args, transfer) {
        if (!w || w.closed) return Promise.reject(new Error('окно переноса закрыто'));
        return new Promise(function (ok, no) {
          var id = nextId++;
          pending[id] = {ok: ok, no: no};
          w.postMessage({senses: 'origin', id: id, op: op, args: args}, expect, transfer || []);
        });
      },
      // The same window to another slot's origin.html.
      go: function (next) {
        var url = next + 'origin.html';
        agent.base = next;
        var was = expect;
        expect = originOf(next);
        var p = waitReady();
        if (w && !w.closed) w.postMessage({senses: 'origin', id: 0, op: 'go', args: {url: url}}, was);
        return p;
      },
      close: function () {
        clearInterval(closedTimer);
        window.removeEventListener('message', onMessage);
        try { if (w && !w.closed) w.postMessage({senses: 'origin', id: 0, op: 'close'}, expect); } catch (e) {}
        setTimeout(function () { try { if (w && !w.closed) w.close(); } catch (e) {} }, 300);
      },
    };
    if (!w) {
      agent.ready = Promise.reject(new Error('браузер не открыл окно (всплывающие окна запрещены?)'));
      return agent;
    }
    agent.ready = waitReady();
    closedTimer = setInterval(function () {
      if (w.closed) { clearInterval(closedTimer); failAll(new Error('окно переноса закрыли')); }
    }, 500);
    return agent;
  }

  // An app's files, as the library has them (store: SensesStore): the
  // library's copy (senses/apps/PACKAGE, its meta.json last), its data on
  // its system (without caches), and the Google services installed on that
  // system (topic gms; their data stays). -> [{path, size}]
  var CACHES = '/(cache|code_cache)/$';
  function filesOf(store, meta, update) {
    var pkg = meta.package, data = 'senses/data/' + (meta.stack || 'a81') + '/';
    var apks = store.tree({prefix: 'senses/apps/' + pkg + '/', skip: '/meta\\.json$|\\.part$'});
    if (update) return apks;
    var lists = [
      apks,
      store.tree({prefix: data + 'data/' + pkg + '/', skip: CACHES}),
      store.tree({prefix: data + 'user_de/0/' + pkg + '/', skip: CACHES}),
      store.tree({prefix: data + 'user/0/' + pkg + '/', skip: CACHES}),
      store.tree({prefix: data + 'google/'}),
    ];
    return Promise.all(lists).then(function (l) { return [].concat.apply([], l); });
  }
  var PIECE = 4 << 20;
  // Moves one app of the library's origin to the agent's (connected and
  // ready): the origin is cleared first (a slot used before), then the files,
  // meta.json last (with home: the library). The library's copy stays until
  // the caller removes it. o.update: a new version of the app already there:
  // its APKs replaced, nothing else touched.
  // -> {files, bytes, ms, storage: the origin's after}
  function send(agent, store, meta, onProgress, o) {
    var files, total = 0, done = 0, started = Date.now(), update = !!(o && o.update);
    var progress = onProgress || function () {};
    return filesOf(store, meta, update).then(function (list) {
      files = list;
      total = files.reduce(function (n, f) { return n + f.size; }, 0);
      progress(0, total, '');
      return update ? agent.call('removeApp', {pkg: meta.package}) : agent.call('wipe');
    }).then(function () {
      return agent.call('setHome', {home: library()});
    }).then(function () {
      return files.reduce(function (p, f) {
        return p.then(function () { return sendFile(f); });
      }, Promise.resolve());
    }).then(function () {
      var own = Object.assign({}, meta, {home: library()});
      delete own.origin;
      var bytes = new TextEncoder().encode(JSON.stringify(own)).buffer;
      return agent.call('receive', {path: 'senses/apps/' + meta.package + '/meta.json', at: 0, bytes: bytes, last: true}, [bytes]);
    }).then(function () {
      return agent.call('hello');
    }).then(function (hello) {
      if (!hello.apps.some(function (m) { return m.package === meta.package; }))
        throw new Error('в хранилище приложения его не видно после переноса');
      return {files: files.length, bytes: total, ms: Date.now() - started, storage: hello.storage};
    });
    function sendFile(f) {
      var at = 0;
      function next() {
        var len = Math.min(PIECE, f.size - at);
        return store.readAt({path: f.path, at: at, len: len}).then(function (buf) {
          var last = at + buf.byteLength >= f.size || buf.byteLength === 0;
          var n = buf.byteLength, from = at;
          return agent.call('receive', {path: f.path, at: from, bytes: buf, last: last}, [buf]).then(function () {
            at += n;
            done += n;
            progress(done, total, f.path);
            if (!last) return next();
          });
        });
      }
      return next();
    }
  }

  // --- origin.html: the app origin's side -----------------------------------------------
  // Obeys only the page that opened it, and only if that page is of a
  // trusted origin. ops: hello, wipe, setHome, receive, clearData, uninstall,
  // removeApp, storage, go, close. ui(text, fraction): what it does.
  function serve(store, ui, log) {
    var opener = window.opener;
    if (!opener) { ui('Эта страница открывается из библиотеки SENSES.', null, true); return; }
    var cfg = null, received = 0;
    config().then(function (c) {
      cfg = c;
      return store.init();
    }).then(function (backend) {
      window.addEventListener('message', function (e) {
        var m = e.data;
        if (e.source !== opener || !m || m.senses !== 'origin' || !trusted(e.origin, cfg)) return;
        handle(m, e.origin);
      });
      log('Origin: ready (' + backend + '), opened by the library');
      ui('Готово к переносу', 0);
      opener.postMessage({senses: 'origin', ready: true, origin: location.origin}, '*');
    }).catch(function (e) {
      log('Origin: no storage: ' + e);
      ui('Хранилище недоступно: ' + e.message, null, true);
    });
    function reply(m, from, ok, result, err) {
      if (!m.id) return;
      opener.postMessage({senses: 'origin', id: m.id, ok: ok, result: result,
                          error: err && {name: err.name, message: String(err.message || err)}}, from);
    }
    function summary(st) {
      return {usage: st.usage, quota: st.quota, free: st.free, guessed: st.guessed, apps: st.apps,
              data: st.data, cachesTotal: st.cachesTotal};
    }
    var OPS = {
      hello: function () {
        return Promise.all([store.list(), store.storage()]).then(function (r) {
          return {origin: location.origin, backend: store.backend, home: home(),
                  apps: r[0].apps.map(function (a) { return a.meta; }), storage: summary(r[1])};
        });
      },
      wipe: function () {
        ui('Очищаю хранилище…', null);
        log('Origin: wipe');
        try { localStorage.clear(); } catch (e) {}
        return store.wipe();
      },
      setHome: function (a) {
        if (!trusted(originOf(a.home), cfg)) throw new Error('чужая библиотека: ' + a.home);
        setHome(a.home);
        if (store.persist) store.persist().then(function (p) { log('Origin: persistent ' + p); });
        return true;
      },
      receive: function (a) {
        received += a.bytes.byteLength;
        ui('Принимаю ' + a.path.replace(/^senses\//, '') + ' · ' + Senses.size(received), null);
        return store.receive(a, [a.bytes]);
      },
      clearData: function (a) { log('Origin: clear data of ' + a.pkg); return store.clearData(a.pkg); },
      uninstall: function (a) { log('Origin: remove ' + a.pkg); return store.uninstall(a.pkg); },
      removeApp: function (a) { log('Origin: new version of ' + a.pkg); return store.removeApp(a.pkg); },
      storage: function () { return store.storage().then(summary); },
      go: function (a) {
        if (!trusted(originOf(a.url), cfg)) throw new Error('чужой адрес: ' + a.url);
        location.replace(a.url);
        return true;
      },
      close: function () { ui('Готово', 1); setTimeout(function () { window.close(); }, 500); return true; },  // (after the log's batch)
    };
    var queue = Promise.resolve();
    function handle(m, from) {
      queue = queue.then(function () {
        if (!OPS[m.op]) throw new Error('unknown op ' + m.op);
        return OPS[m.op](m.args || {});
      }).then(function (r) { reply(m, from, true, r); }, function (err) {
        log('Origin: ' + m.op + ' failed: ' + err);
        reply(m, from, false, null, err);
      });
    }
  }

  root.SensesOrigins = {
    config: config, trusted: trusted, mode: mode, wanted: wanted, quotaGuessed: quotaGuessed,
    freeSlot: freeSlot, home: home, setHome: setHome, library: library, originOf: originOf,
    connect: connect, send: send, serve: serve,
  };
})(self);
