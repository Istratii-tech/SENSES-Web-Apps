// The page side of the WebView of the Android 8.1 stack (topic webview8,
// docs/topics/webview8.md), a --pre-js of the page builds. Android's
// WebView (arcwasm.webview.HostWebView, in the VM) is an ordinary View that
// draws nothing; its page is an <iframe> of this page, laid over the
// display canvas where the View is, clipped to what of it is visible, with
// holes where other windows or views are above it, dimmed under a dialog.
// The browser renders the page and gives it the touches and keys that land
// on it.
//
// VM -> page: the VM's threads run a C function on this thread
// (emscripten_async_run_in_main_runtime_thread, shim/webview/webview_jni.cc),
// which hands Module.arcWebView.command() one JSON command:
//   init     {box, cap, sync}       the mailbox below; answered by "ready"
//   create   {id} / destroy {id}
//   geom     {id, show, x, y, w, h, clip, holes, dpr, dim, z}  display pixels
//   load     {id, nav, kind, url, html, base, title, js, ifaces, reason}
//              kind "doc": html in a same-origin srcdoc frame (loadData...,
//                          assets, about:blank); "url": the frame at url
//                          (same origin as the page: scripted like a doc;
//                          other origins: as they are); "fallback": a card
//                          with "open in a new tab" (pages that refuse to be
//                          framed, docs/topics/webview8.md); "copy": html, a
//                          page the VM read with its styles and images
//                          inlined (a site that would frame but sends no
//                          COEP/CORP: Safari), in a srcdoc frame of no origin
//                          (sandbox without allow-same-origin) where only the
//                          host's link handler runs (CSP), docs/topics/senses-a10.md
//   eval     {id, req, code}        evaluateJavascript
//   ifaces   {id, ifaces, js}       addJavascriptInterface / setJavaScriptEnabled
//   history  {id, delta}            history.go(delta) for a frame's own entries
//   stop     {id}
//
// Page -> VM: events, one JSON object a line, in a mailbox in the wasm
// memory (the VM's event thread sleeps on its first word with
// memory.atomic.wait; Atomics.notify from here wakes it). A frame's
// @JavascriptInterface calls are synchronous, as on Android: this thread
// writes the call into the mailbox's sync slot and spins (no Atomics.wait on
// a page's main thread) until the VM's event thread has run the method and
// written its result, or 10 s pass.
//
// Mailbox (int32 words at box): 0 seq (bumped on every write; the futex),
// 1 lock (the event area, a spin lock shared with the VM), 2 bytes used,
// 3 capacity, 4 sync state (0 idle, 1 asked, 3 taken by the VM, 2 answered,
// 4 abandoned), 5 sync bytes, 6 sync capacity, 7 events waiting here for
// room; the event area at box + 32, the sync slot after it.
//
// Events: ready {credentialless, origin, isolated, ua, lang}; start {id, nav, url};
// progress {id, nav, p}; finish {id, nav, url, title, ch, cw};
// title {id, nav, title}; console {id, level, msg, src, line};
// eval {id, req, json}; size {id, nav, cw, ch}; nav {id, url, gesture} (a link or a form in a
// scripted frame: shouldOverrideUrlLoading); framenav {id, nav, url} (the
// frame went somewhere on its own; url null where it cannot be read);
// opened {id, url} (the fallback card's button).
if (typeof window !== 'undefined' && typeof document !== 'undefined') (function () {
  var SYNC_IDLE = 0, SYNC_ASKED = 1, SYNC_ANSWERED = 2, SYNC_TAKEN = 3, SYNC_ABANDONED = 4;
  var SYNC_TIMEOUT_MS = 10000;
  var FRAME_SANDBOX = 'allow-same-origin allow-forms allow-popups allow-popups-to-escape-sandbox ' +
      'allow-modals allow-downloads allow-pointer-lock allow-presentation';
  // A copy of a site (kind "copy"): no origin (it cannot reach this page),
  // scripts for the host's handler only (the CSP in copyDoc).
  var COPY_SANDBOX = 'allow-scripts allow-popups allow-popups-to-escape-sandbox';
  var ru = /^ru\b/i.test(navigator.language || '');

  var views = {};   // id -> view
  var box = 0, cap = 0, syncCap = 0;
  var pending = [];  // encoded events waiting for room in the mailbox
  var layer = null;  // the overlay over the display canvas
  var layerKey = '';
  var encoder = new TextEncoder(), decoder = new TextDecoder();
  // (?wvsafari=1: as a browser without credentialless frames, Safari: pages
  // of other origins come as copies -- to test that in Chromium)
  var credentialless = 'credentialless' in HTMLIFrameElement.prototype && !/[?&]wvsafari=1/.test(location.search);
  // Problems go where the VM's output goes (the page's log), not only to
  // the console; ?wvdebug=1 adds every command.
  var debug = /[?&]wvdebug=1/.test(location.search);
  function report(text) {
    if (typeof err === 'function') err('webview: ' + text); else console.error('webview: ' + text);
  }

  var api = Module['arcWebView'] = {
    command: command,
    views: views,
    // For tests and pages: the frame element of WebView |id|.
    frame: function (id) { return views[id] ? views[id].frame : null; },
  };
  // What the scripted frames reach (parent.arcWebViewHost) for their
  // console, links and @JavascriptInterface objects.
  window['arcWebViewHost'] = {
    event: function (o) { send(o); },
    call: callSync,
    leaving: function (id) {
      var v = views[id];
      if (v && v.frame && v.kind === 'url') watchDocument(v, v.frame, v.nav);
    },
  };

  // --- the mailbox ---------------------------------------------------------

  var lastBuffer = null, i32 = null, u8 = null;
  function heap() {
    if (wasmMemory.buffer !== lastBuffer) {
      lastBuffer = wasmMemory.buffer;
      i32 = new Int32Array(lastBuffer);
      u8 = new Uint8Array(lastBuffer);
    }
  }
  function word(i) { return (box >>> 2) + i; }

  function lock() {
    var n = 0;
    while (Atomics.compareExchange(i32, word(1), 0, 1) !== 0) {
      if (++n > 1e7) throw new Error('webview mailbox: lock');
    }
  }
  function unlock() { Atomics.store(i32, word(1), 0); }

  function wake() {
    Atomics.add(i32, word(0), 1);
    Atomics.notify(i32, word(0));
  }

  function send(o) {
    pending.push(encoder.encode(JSON.stringify(o) + '\n'));
    pump();
  }

  // Moves what fits of |pending| into the mailbox (the VM calls this again
  // when it has emptied it and word 7 says events are waiting).
  function pump() {
    if (!box || !pending.length) return;
    heap();
    lock();
    var used = i32[word(2)], moved = 0;
    while (pending.length) {
      var b = pending[0];
      if (b.length > cap) {  // never fits: drop it
        pending.shift();
        report('event of ' + b.length + ' bytes dropped');
        continue;
      }
      if (used + b.length > cap) break;
      u8.set(b, box + 32 + used);
      used += b.length;
      pending.shift();
      moved++;
    }
    i32[word(2)] = used;
    i32[word(7)] = pending.length ? 1 : 0;
    unlock();
    if (moved) wake();
  }

  // A synchronous @JavascriptInterface call: the result's JSON (undefined
  // when the VM did not answer in time or the call failed).
  function callSync(id, obj, method, args) {
    if (!box) return undefined;
    heap();
    var s = word(4);
    var state = Atomics.load(i32, s);
    if (state === SYNC_ANSWERED || state === SYNC_ABANDONED) {
      Atomics.store(i32, s, SYNC_IDLE);  // a late answer to an abandoned call
    } else if (state !== SYNC_IDLE) {
      console.warn('webview: nested @JavascriptInterface call ' + obj + '.' + method);
      return undefined;
    }
    var req = encoder.encode(JSON.stringify({ id: id, obj: obj, m: method, args: args }));
    if (req.length > syncCap) return undefined;
    var slot = box + 32 + cap;
    u8.set(req, slot);
    i32[word(5)] = req.length;
    Atomics.store(i32, s, SYNC_ASKED);
    wake();
    var deadline = performance.now() + SYNC_TIMEOUT_MS;
    for (;;) {
      state = Atomics.load(i32, s);
      if (state === SYNC_ANSWERED) break;
      if (performance.now() > deadline) {
        if (Atomics.compareExchange(i32, s, SYNC_ASKED, SYNC_IDLE) !== SYNC_ASKED) {
          Atomics.compareExchange(i32, s, SYNC_TAKEN, SYNC_ABANDONED);
        }
        console.warn('webview: ' + obj + '.' + method + ' did not answer in ' +
                     SYNC_TIMEOUT_MS / 1000 + ' s');
        return undefined;
      }
    }
    heap();  // (the VM may have grown the memory meanwhile)
    var n = i32[word(5)];
    var text = decoder.decode(u8.slice(slot, slot + n));
    Atomics.store(i32, s, SYNC_IDLE);
    try { return JSON.parse(text); } catch (e) { return undefined; }
  }

  // --- commands ------------------------------------------------------------

  function command(json) {
    var c;
    try { c = JSON.parse(json); } catch (e) { report('bad command ' + json); return; }
    if (debug) report(c.op + ' ' + json.slice(0, 160));
    try {
      switch (c.op) {
        case 'init':
          box = c.box >>> 0; cap = c.cap; syncCap = c.sync;
          send({ ev: 'ready', credentialless: credentialless, origin: location.origin,
                 isolated: !!window.crossOriginIsolated, ua: navigator.userAgent,
                 lang: navigator.language || '' });
          break;
        case 'pump': pump(); break;
        case 'create': create(c.id); break;
        case 'destroy': destroy(c.id); break;
        case 'geom': if (views[c.id]) geometry(views[c.id], c); break;
        case 'load': if (views[c.id]) load(views[c.id], c); break;
        case 'eval': if (views[c.id]) evaluate(views[c.id], c); break;
        case 'ifaces':
          if (views[c.id]) { views[c.id].ifaces = c.ifaces || {}; views[c.id].js = !!c.js; }
          if (views[c.id] && views[c.id].scripted) inject(views[c.id]);
          break;
        case 'history': history.go(c.delta); break;
        case 'stop': if (views[c.id] && views[c.id].frame) stopFrame(views[c.id]); break;
        default: report('unknown command ' + c.op);
      }
    } catch (e) {
      report(c.op + ': ' + (e && e.stack || e));
    }
  }

  // --- the overlay -----------------------------------------------------------

  function ensureLayer() {
    if (layer) return layer;
    layer = document.createElement('div');
    layer.className = 'arc-webviews';
    // Above the display, below the shell's own overlays (demo/apps: z 5+).
    layer.style.cssText = 'position:fixed;left:0;top:0;width:0;height:0;z-index:3;' +
        'pointer-events:none;transform-origin:50% 50%;overflow:visible;';
    document.body.appendChild(layer);
    requestAnimationFrame(follow);
    window.addEventListener('resize', place);
    return layer;
  }

  // The layer has the display's size in display pixels and is laid over the
  // canvas as browser_input.js maps touches: the canvas' box (turned by
  // Module.arcwasmInput.turn, the phone held sideways) is the display.
  function place() {
    var canvas = Module['canvas'];
    if (!layer || !canvas) return;
    var r = canvas.getBoundingClientRect();
    var turn = (Module['arcwasmInput'] && Module['arcwasmInput'].turn) || 0;
    var cw = canvas.width || 1, ch = canvas.height || 1;
    var uw = turn ? r.height : r.width, uh = turn ? r.width : r.height;
    var key = [r.left, r.top, r.width, r.height, turn, cw, ch].join();
    if (key === layerKey) return;
    layerKey = key;
    var s = layer.style;
    s.width = cw + 'px';
    s.height = ch + 'px';
    s.left = (r.left + r.width / 2 - cw / 2) + 'px';
    s.top = (r.top + r.height / 2 - ch / 2) + 'px';
    s.transform = 'rotate(' + turn + 'deg) scale(' + uw / cw + ',' + uh / ch + ')';
  }

  // The canvas moves with the page's layout (the shell's panel, a phone
  // turned): followed every frame while there are WebViews.
  function follow() {
    if (!layer) return;
    place();
    for (var id in views) { requestAnimationFrame(follow); return; }
    layer.remove();
    layer = null;
    layerKey = '';
    window.removeEventListener('resize', place);
  }

  function create(id) {
    destroy(id);
    var wrap = document.createElement('div');
    // Transparent, as the frame: under it is what the WebView drew in the
    // app's window, its background color (white unless the app set one).
    wrap.style.cssText = 'position:absolute;overflow:hidden;visibility:hidden;pointer-events:auto;' +
        'left:0;top:0;width:0;height:0;';
    ensureLayer().appendChild(wrap);
    views[id] = { id: id, wrap: wrap, frame: null, card: null, nav: 0, kind: '', url: '',
                  geom: null, ifaces: {}, js: false, scripted: false, loads: 0, doc: null };
  }

  function destroy(id) {
    var v = views[id];
    if (!v) return;
    clearInterval(v.watch);
    v.wrap.remove();
    delete views[id];
  }

  function geometry(v, g) {
    v.geom = g;
    place();  // (also where rAF does not run: a hidden tab)
    var s = v.wrap.style;
    var show = g.show && g.clip && g.clip[2] > g.clip[0] && g.clip[3] > g.clip[1];
    s.visibility = show ? 'visible' : 'hidden';
    s.zIndex = g.z | 0;
    if (!show) return;
    var l = g.clip[0], t = g.clip[1], cw = g.clip[2] - l, chh = g.clip[3] - t;
    s.left = l + 'px';
    s.top = t + 'px';
    s.width = cw + 'px';
    s.height = chh + 'px';
    // Holes: the parts of other windows and views above the WebView, in the
    // wrapper's own pixels (an even-odd path: the rectangle minus them, made
    // disjoint first: where two overlap, even-odd would show the page again).
    var holes = disjoint((g.holes || []).map(function (h) {
      return [Math.max(0, h[0] - l), Math.max(0, h[1] - t), Math.min(cw, h[2] - l), Math.min(chh, h[3] - t)];
    }));
    if (holes.length) {
      var p = 'M0 0H' + cw + 'V' + chh + 'H0Z';
      holes.forEach(function (h) {
        p += 'M' + h[0] + ' ' + h[1] + 'H' + h[2] + 'V' + h[3] + 'H' + h[0] + 'Z';
      });
      s.clipPath = 'path(evenodd, "' + p + '")';
    } else {
      s.clipPath = '';
    }
    s.filter = g.dim > 0 ? 'brightness(' + (1 - g.dim) + ')' : '';
    // The page: CSS pixels of the display's density (1 CSS px = dpr display
    // pixels, as Android's WebView lays a page out in dp).
    var inner = v.frame || v.card;
    if (inner) fit(v, inner);
  }

  // Rectangles [l, t, r, b] -> the same area as rectangles that do not
  // overlap: per horizontal band between their edges, the union of their
  // spans.
  function disjoint(rects) {
    rects = rects.filter(function (r) { return r[2] > r[0] && r[3] > r[1]; });
    if (rects.length < 2) return rects;
    var ys = [];
    rects.forEach(function (r) { ys.push(r[1], r[3]); });
    ys = ys.sort(function (a, b) { return a - b; }).filter(function (y, i, a) { return !i || y !== a[i - 1]; });
    var out = [];
    for (var i = 0; i + 1 < ys.length; i++) {
      var y0 = ys[i], y1 = ys[i + 1];
      var spans = rects.filter(function (r) { return r[1] <= y0 && r[3] >= y1; })
          .map(function (r) { return [r[0], r[2]]; }).sort(function (a, b) { return a[0] - b[0]; });
      var cur = null;
      spans.forEach(function (sp) {
        if (cur && sp[0] <= cur[1]) { cur[1] = Math.max(cur[1], sp[1]); return; }
        if (cur) out.push([cur[0], y0, cur[1], y1]);
        cur = sp.slice();
      });
      if (cur) out.push([cur[0], y0, cur[1], y1]);
    }
    return out;
  }

  function fit(v, el) {
    var g = v.geom;
    if (!g || !g.show || !g.clip) return;
    var dpr = g.dpr || 1;
    var s = el.style;
    s.position = 'absolute';
    s.left = (g.x - g.clip[0]) + 'px';
    s.top = (g.y - g.clip[1]) + 'px';
    s.width = (g.w / dpr) + 'px';
    s.height = (g.h / dpr) + 'px';
    s.transformOrigin = '0 0';
    s.transform = dpr !== 1 ? 'scale(' + dpr + ')' : '';
  }

  // --- loading ---------------------------------------------------------------

  function sameOrigin(url) {
    try { return new URL(url, location.href).origin === location.origin; } catch (e) { return false; }
  }

  function load(v, c) {
    v.nav = c.nav;
    v.kind = c.kind;
    v.url = c.url || '';
    v.ifaces = c.ifaces || {};
    v.js = !!c.js;
    v.loads = 0;
    v.doc = null;
    v.scripted = false;
    clearInterval(v.watch);
    if (v.frame) { v.frame.remove(); v.frame = null; }
    if (v.card) { v.card.remove(); v.card = null; }
    if (c.kind === 'fallback') {
      v.card = card(v, c);
      v.wrap.appendChild(v.card);
      fit(v, v.card);
      send({ ev: 'start', id: v.id, nav: v.nav, url: v.url });
      send({ ev: 'finish', id: v.id, nav: v.nav, url: v.url, title: c.title || '', ch: 0, cw: 0 });
      return;
    }
    var f = document.createElement('iframe');
    f.style.border = '0';
    f.style.display = 'block';
    f.style.background = 'transparent';
    f.setAttribute('allow', 'fullscreen; autoplay; clipboard-read; clipboard-write; ' +
                   'encrypted-media; picture-in-picture; geolocation');
    var scripted = c.kind === 'doc' || (c.kind === 'url' && sameOrigin(c.url));
    v.scripted = scripted;
    // Pages of other origins: no navigating the shell away (frame busting:
    // no allow-top-navigation). Scripts only where the app enabled them
    // (WebSettings). (A frame of this page's origin could lift its sandbox:
    // it has none unless scripts are off.)
    if (c.kind === 'copy') f.setAttribute('sandbox', COPY_SANDBOX);
    else if (!scripted || !v.js) f.setAttribute('sandbox', FRAME_SANDBOX + (v.js ? ' allow-scripts' : ''));
    if (!scripted && credentialless) f.credentialless = true;
    var nav = v.nav;
    f.addEventListener('load', function () { loaded(v, f, nav); });
    v.frame = f;
    fit(v, f);
    send({ ev: 'start', id: v.id, nav: nav, url: v.url });
    // (src before the frame is in the page: no load event of an initial
    // about:blank first)
    if (c.kind === 'doc') {
      f.srcdoc = withBridge(v, c.html || '', c.base, null);
      v.wrap.appendChild(f);
    } else if (c.kind === 'copy') {
      f.srcdoc = copyDoc(v, c.html || '', c.base);
      v.wrap.appendChild(f);
      if (debug) setTimeout(function () { report('copy ' + v.id + ' after 2 s: ' + boxes(v)); }, 2000);
    } else if (scripted) {
      writePage(v, f, nav, c.url);
    } else {
      f.src = c.url;
      v.wrap.appendChild(f);
    }
  }

  // A page of this page's origin at a URL: fetched here and written into
  // the frame's first (about:blank) document with the bridge first in it,
  // which then takes the page's URL (history.replaceState): the bridge is
  // there before the page's own scripts run, its location is its URL. A
  // page that is not HTML (or a failed fetch) is navigated to instead,
  // scripted as soon as its document is seen.
  function writePage(v, f, nav, url) {
    v.wrap.appendChild(f);
    var fallback = function () {
      if (v.frame !== f || v.nav !== nav) return;
      f.src = url;
      watchDocument(v, f, nav);
    };
    fetch(url, { credentials: 'same-origin' }).then(function (r) {
      var type = r.headers.get('content-type') || '';
      if (!/html|xml/i.test(type) && type) return fallback();
      return r.text().then(function (text) {
        if (v.frame !== f || v.nav !== nav) return;
        var doc = f.contentDocument;
        if (!doc) return fallback();
        v.loads = 0;
        v.doc = doc;
        doc.open();
        doc.write(withBridge(v, text, null, r.url || url));
        doc.close();
      });
    }).catch(fallback);
  }

  function stopFrame(v) {
    try { v.frame.contentWindow.stop(); } catch (e) {}
  }

  // A page that cannot be shown in a frame: a card, its button opens the
  // page in a tab of the browser (a click: no popup blocker).
  function card(v, c) {
    var d = document.createElement('div');
    d.style.cssText = 'box-sizing:border-box;display:flex;flex-direction:column;align-items:center;' +
        'justify-content:center;gap:10px;padding:24px;background:#fff;color:#202124;text-align:center;' +
        'font:15px/1.4 system-ui,-apple-system,sans-serif;overflow:hidden;';
    var host = '';
    try { host = new URL(c.url).host; } catch (e) {}
    var h = document.createElement('div');
    h.style.cssText = 'font-size:18px;font-weight:600;overflow-wrap:anywhere;';
    h.textContent = c.title || host || c.url;
    var p = document.createElement('div');
    p.style.cssText = 'color:#5f6368;font-size:13px;overflow-wrap:anywhere;max-width:420px;';
    p.textContent = (ru ? 'Сайт ' + host + ' нельзя показать внутри приложения' :
                          host + ' cannot be shown inside the app') +
                    (c.reason ? ' (' + c.reason + ').' : '.');
    var b = document.createElement('button');
    b.textContent = ru ? 'Открыть в новой вкладке' : 'Open in a new tab';
    b.style.cssText = 'margin-top:6px;padding:10px 18px;border:0;border-radius:20px;background:#1a73e8;' +
        'color:#fff;font:inherit;font-weight:600;cursor:pointer;';
    b.addEventListener('click', function () {
      window.open(c.url, '_blank', 'noopener');
      send({ ev: 'opened', id: v.id, url: c.url });
    });
    d.appendChild(h);
    d.appendChild(p);
    d.appendChild(b);
    return d;
  }

  // A frame's load event: its page has loaded (the first one for this
  // navigation), or the frame went somewhere by itself (a link in a page of
  // another origin, a script's location.href = ..., history.back()).
  // Where things are on the page (?wvdebug=1).
  function boxes(v) {
    var box = function (el) {
      if (!el) return '-';
      var r = el.getBoundingClientRect();
      return [r.left, r.top, r.width, r.height].map(Math.round).join(',');
    };
    return 'canvas ' + box(Module['canvas']) + ' (' + (Module['canvas'] || {}).width + 'x' +
           (Module['canvas'] || {}).height + '), layer ' + box(layer) + ' ' + (layer ? layer.style.transform : '') +
           ', wrap ' + box(v.wrap) + ', frame ' + box(v.frame) + ', card ' + box(v.card);
  }

  function loaded(v, f, nav) {
    if (v.frame !== f || v.nav !== nav) return;
    v.loads++;
    if (debug) report('loaded ' + v.id + ' ' + v.kind + ': ' + boxes(v));
    var doc = null, url = null;
    try { doc = f.contentDocument; url = doc ? String(f.contentWindow.location.href) : null; } catch (e) {}
    if (doc && url === 'about:srcdoc') url = v.url;
    if (url === 'about:blank' && v.kind !== 'doc' && v.url !== 'about:blank') {
      v.loads = 0;  // the frame's initial document, not the page
      return;
    }
    if (v.scripted && doc && doc !== v.doc) inject(v);
    if (v.loads > 1) {
      send({ ev: 'framenav', id: v.id, nav: nav, url: url });
    }
    var title = '', ch = 0, cw = 0;
    if (doc) {
      title = doc.title || '';
      var el = doc.documentElement;
      if (el) { ch = el.scrollHeight; cw = el.scrollWidth; }
    }
    send({ ev: 'progress', id: v.id, nav: nav, p: 100 });
    send({ ev: 'finish', id: v.id, nav: nav, url: url || v.url, title: title, ch: ch, cw: cw });
  }

  // Same-origin pages at a URL: the bridge goes in as soon as the frame
  // has its new document (polled: there is no event for that; usually
  // before the page's own DOMContentLoaded handlers run) until it has
  // loaded, and again when the page goes away (the bridge's pagehide).
  function watchDocument(v, f, nav) {
    clearInterval(v.watch);
    v.watch = setInterval(function () {
      if (v.frame !== f || v.nav !== nav) { clearInterval(v.watch); return; }
      var doc = null;
      try { doc = f.contentDocument; } catch (e) { clearInterval(v.watch); return; }
      if (!doc || doc.URL === 'about:blank') return;
      if (doc !== v.doc) inject(v);
      if (doc.readyState === 'complete') clearInterval(v.watch);
    }, 4);
  }

  // --- the bridge in scripted frames ----------------------------------------

  function bridgeSource(v) {
    // Runs in the frame (its own globals); talks to arcWebViewHost.
    return '(' + function (ID, NAV, IFACES) {
      var H = parent.arcWebViewHost;
      if (!H || window.__arcWebView === NAV) return;
      window.__arcWebView = NAV;
      window.addEventListener('pagehide', function () { H.leaving(ID); });
      var C = window.console;
      ['log', 'info', 'warn', 'error', 'debug'].forEach(function (level) {
        var orig = C[level];
        C[level] = function () {
          var parts = [];
          for (var i = 0; i < arguments.length; i++) {
            var a = arguments[i];
            try { parts.push(typeof a === 'string' ? a : a instanceof Error ? String(a) : JSON.stringify(a)); }
            catch (e) { parts.push(String(a)); }
          }
          H.event({ ev: 'console', id: ID, level: level, msg: parts.join(' '), src: location.href, line: 0 });
          if (orig) return orig.apply(C, arguments);
        };
      });
      window.addEventListener('error', function (e) {
        H.event({ ev: 'console', id: ID, level: 'error', msg: 'Uncaught ' + (e.error || e.message),
                  src: e.filename || location.href, line: e.lineno || 0 });
      });
      Object.keys(IFACES).forEach(function (name) {
        var o = {};
        IFACES[name].forEach(function (m) {
          o[m] = function () {
            var args = [];
            for (var i = 0; i < arguments.length; i++) args.push(arguments[i] === undefined ? null : arguments[i]);
            return H.call(ID, name, m, args);
          };
        });
        try { window[name] = o; } catch (e) {}
      });
      function go(url, gesture) {
        H.event({ ev: 'nav', id: ID, url: url, gesture: gesture });
      }
      // Links: Android asks the app (shouldOverrideUrlLoading), then loads.
      document.addEventListener('click', function (e) {
        if (e.defaultPrevented || e.button !== 0) return;
        var a = e.target && e.target.closest ? e.target.closest('a[href]') : null;
        if (!a) return;
        var href = a.getAttribute('href') || '';
        if (/^\s*javascript:/i.test(href) || /^#/.test(href)) return;
        var url = a.href;
        if (/^about:srcdoc/.test(url)) return;
        e.preventDefault();
        go(url, true);
      });
      document.addEventListener('submit', function (e) {
        var f = e.target;
        if (e.defaultPrevented || !f || !f.action) return;
        e.preventDefault();
        var url = f.action;
        if ((f.method || 'get').toLowerCase() === 'get') {
          var q = new URLSearchParams(new FormData(f)).toString();
          url = url.replace(/[?#].*$/, '') + (q ? '?' + q : '');
        }
        go(url, true);
      }, true);
      var open = window.open;
      window.open = function (url) {
        if (url) go(new URL(url, document.baseURI).href, false);
        return null;
      };
      void open;
      // The page's size (WebViews that wrap their content measure by it).
      if (window.ResizeObserver && document.documentElement) {
        var lastSize = '';
        new ResizeObserver(function () {
          var el = document.documentElement;
          var size = el.scrollWidth + 'x' + el.scrollHeight;
          if (size === lastSize) return;
          lastSize = size;
          H.event({ ev: 'size', id: ID, nav: NAV, cw: el.scrollWidth, ch: el.scrollHeight });
        }).observe(document.documentElement);
      }
      // The title, as it changes.
      var last = document.title;
      new MutationObserver(function () {
        if (document.title !== last) {
          last = document.title;
          H.event({ ev: 'title', id: ID, nav: NAV, title: last });
        }
      }).observe(document, { subtree: true, childList: true, characterData: true });
    } + ')(' + v.id + ',' + v.nav + ',' + JSON.stringify(v.js ? v.ifaces || {} : {}) + ');';
  }

  // The bridge first in the page's <head>, and its <base> (loadDataWithBaseURL).
  function withBridge(v, html, base, url) {
    var head = '';
    if (url) {
      // (first: before the page's scripts and its relative URLs)
      head += '<script>try{history.replaceState(history.state,"",' +
              JSON.stringify(url).replace(/</g, '\\u003c') + ')}catch(e){}</script>';
    }
    if (base && !/^(about|data):/i.test(base)) {
      head += '<base href="' + base.replace(/&/g, '&amp;').replace(/"/g, '&quot;') + '">';
    }
    if (v.js) head += '<script>' + bridgeSource(v).replace(/<\/script/gi, '<\\/script') + '</script>';
    if (!head) return html;
    // After <head> (or <html>) when there is one, else in front.
    var m = /<head(\s[^>]*)?>/i.exec(html) || /<html(\s[^>]*)?>/i.exec(html);
    if (m) {
      var at = m.index + m[0].length;
      return html.slice(0, at) + head + html.slice(at);
    }
    var dt = /^\s*<!doctype[^>]*>/i.exec(html);
    if (dt) return dt[0] + head + html.slice(dt[0].length);
    return head + html;
  }

  // A copy of a site: its HTML with, first in <head>, a CSP that lets only
  // the host's handler run (the site's scripts and on* attributes do not),
  // <base> (links and anything not inlined resolve against the site), and the
  // handler: links and GET forms go to the app as a scripted frame's do
  // (nav: shouldOverrideUrlLoading, then a load), fragment links scroll, the
  // page's height comes back (wrap_content). Messages come with the view's
  // id and the navigation (copyMessage); the frame has no origin to check.
  function copyDoc(v, html, base) {
    var nonce = Math.random().toString(36).slice(2) + Date.now().toString(36);
    var handler = '(' + function (ID, NAV) {
      function post(m) { m.arcWebViewCopy = ID; m.nav = NAV; parent.postMessage(m, '*'); }
      document.addEventListener('click', function (e) {
        if (e.defaultPrevented || e.button !== 0) return;
        var a = e.target && e.target.closest ? e.target.closest('a[href]') : null;
        if (!a) return;
        var href = a.getAttribute('href') || '';
        if (/^\s*javascript:/i.test(href)) { e.preventDefault(); return; }
        e.preventDefault();
        if (/^#/.test(href)) {
          var el = document.getElementById(decodeURIComponent(href.slice(1))) ||
                   document.getElementsByName(decodeURIComponent(href.slice(1)))[0];
          if (el) el.scrollIntoView();
          return;
        }
        post({ url: a.href });
      }, true);
      document.addEventListener('submit', function (e) {
        var f = e.target;
        e.preventDefault();
        if (!f || !f.action || (f.method || 'get').toLowerCase() !== 'get') return;
        var q = new URLSearchParams(new FormData(f)).toString();
        post({ url: f.action.replace(/[?#].*$/, '') + (q ? '?' + q : '') });
      }, true);
      function size() {
        var el = document.documentElement;
        if (el) post({ ch: el.scrollHeight, cw: el.scrollWidth });
      }
      window.addEventListener('load', size);
      if (window.ResizeObserver && document.documentElement) new ResizeObserver(size).observe(document.documentElement);
    } + ')(' + JSON.stringify(v.id) + ',' + JSON.stringify(v.nav) + ')';
    var head = '<meta http-equiv="Content-Security-Policy" content="script-src \'nonce-' + nonce +
               '\'; object-src \'none\'">' +
               (base ? '<base href="' + base.replace(/&/g, '&amp;').replace(/"/g, '&quot;') + '">' : '') +
               '<script nonce="' + nonce + '">' + handler.replace(/<\/script/gi, '<\\/script') + '</script>';
    var m = /<head(\s[^>]*)?>/i.exec(html) || /<html(\s[^>]*)?>/i.exec(html);
    if (m) {
      var at = m.index + m[0].length;
      return html.slice(0, at) + head + html.slice(at);
    }
    var dt = /^\s*<!doctype[^>]*>/i.exec(html);
    if (dt) return dt[0] + head + html.slice(dt[0].length);
    return head + html;
  }
  window.addEventListener('message', function (e) {
    var d = e.data;
    if (!d || typeof d !== 'object' || d.arcWebViewCopy === undefined) return;
    var v = views[d.arcWebViewCopy];
    if (!v || v.kind !== 'copy' || !v.frame || e.source !== v.frame.contentWindow || d.nav !== v.nav) return;
    if (debug) report('copy ' + v.id + ': ' + JSON.stringify(d));
    if (typeof d.url === 'string' && /^https?:/i.test(d.url)) {
      send({ ev: 'nav', id: v.id, url: d.url, gesture: true });
    } else if (typeof d.ch === 'number') {
      send({ ev: 'size', id: v.id, nav: v.nav, cw: d.cw | 0, ch: d.ch | 0 });
    }
  });

  // Into a same-origin frame's current document (a frame at a URL; srcdoc
  // frames have it in their HTML already).
  function inject(v) {
    var f = v.frame, doc;
    try { doc = f && f.contentDocument; } catch (e) { return; }
    if (!doc) return;
    v.doc = doc;
    if (!v.js) return;
    try {
      f.contentWindow.eval(bridgeSource(v));
    } catch (e) {
      // (a page whose CSP forbids eval: a script element then)
      try {
        var s = doc.createElement('script');
        s.textContent = bridgeSource(v);
        (doc.head || doc.documentElement).appendChild(s);
        s.remove();
      } catch (e2) {}
    }
  }

  // evaluateJavascript: the result as JSON, as Android gives it ("null" for
  // undefined, functions and what cannot be read).
  function evaluate(v, c) {
    var json = null, f = v.frame;
    if (f && v.js) {
      try {
        var w = f.contentWindow;
        var r = w.eval(c.code);
        if (r !== undefined && typeof r !== 'function') {
          json = r && typeof r.then === 'function' ? '{}' : JSON.stringify(r);
        }
      } catch (e) {
        if (!(e instanceof DOMException) && !/cross-origin|SecurityError|Blocked a frame/i.test(String(e))) {
          send({ ev: 'console', id: v.id, level: 'error', msg: 'Uncaught ' + e, src: v.url, line: 0 });
        }
      }
    }
    send({ ev: 'eval', id: v.id, req: c.req, json: json === undefined ? null : json });
  }
})();
