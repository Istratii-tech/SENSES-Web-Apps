// SENSES Web Apps: what the library (index.html) and the app page
// (run.html) share (topic senses-shell): the systems, sizes, sheets, long
// press, the log (also to tools/serve.py's /__log: how iPhones are watched)
// and the service worker.
(function (root) {
  'use strict';
  var $ = function (id) { return document.getElementById(id); };

  // The systems an app can run on; the folder of each in the build
  // (mk/topic-senses-apps.mk) is its id, as senses/data/<id>/ is its /data.
  var STACKS = {
    a81: {name: 'Android 8.1', short: '8.1', api: 27, vm: 'ART 8.1', mb: 200},
    a10: {name: 'Android 10', short: '10', api: 29, vm: 'ART 10', mb: 250},
    a60: {name: 'Android 6.0', short: '6.0', api: 23, vm: 'ART 6.0', mb: 90},
    a44: {name: 'Android 4.4', short: '4.4', api: 19, vm: 'Dalvik', mb: 95},
  };

  // Binary units, as wasm memory and storage quotas are sized.
  function size(bytes) {
    var mb = bytes / 1048576;
    if (mb >= 1024) return (mb / 1024).toFixed(1).replace('.', ',') + ' ГБ';
    if (mb >= 10) return Math.round(mb) + ' МБ';
    if (mb >= 1) return mb.toFixed(1).replace('.', ',') + ' МБ';
    return Math.max(1, Math.round(bytes / 1024)) + ' КБ';
  }

  // --- the log ------------------------------------------------------------------
  var lines = root.arcLog = [];
  function log(text) {
    text = String(text);
    lines.push((performance.now() / 1000).toFixed(2) + ' ' + text);
    if (lines.length > 5000) lines.splice(0, 1000);
    var el = $('log');
    if (el) {
      var line = document.createElement('span');
      line.textContent = text + '\n';
      el.appendChild(line);
      while (el.childNodes.length > 300) el.removeChild(el.firstChild);
      if ($('logbox') && !$('logbox').hidden) el.scrollTop = el.scrollHeight;
    }
    if (root.arcReport) arcReport(text);
  }
  function showLog() {
    $('logbox').hidden = false;
    $('log').scrollTop = $('log').scrollHeight;
  }
  document.addEventListener('DOMContentLoaded', function () {
    if ($('logback')) $('logback').onclick = function () { $('logbox').hidden = true; };
  });

  // --- sheets -------------------------------------------------------------------
  // Closed by a tap outside the box or a [data-close] button, unless modal.
  function open(id, modal) {
    var s = $(id);
    s.hidden = false;
    s.dataset.modal = modal ? '1' : '';
    if (!s.dataset.wired) {
      s.dataset.wired = '1';
      s.addEventListener('click', function (e) {
        if ((e.target === s && !s.dataset.modal) || (e.target.hasAttribute && e.target.hasAttribute('data-close')))
          s.hidden = true;
      });
    }
  }
  function close(id) { $(id).hidden = true; }
  function bar(el, fraction) {
    el.hidden = false;
    el.classList.toggle('busy', fraction == null);
    el.firstChild.style.width = fraction == null ? '' : Math.round(fraction * 100) + '%';
  }
  function warnInto(box, level, text) {
    var d = document.createElement('div');
    d.className = 'warn' + (level === 'bad' ? ' bad' : '');
    d.textContent = text;
    box.appendChild(d);
  }

  // --- tap and long press -------------------------------------------------------
  // tap(e) on a short press, long() after LONG_MS held still; a move of the
  // finger (scrolling the grid) cancels both.
  var LONG_MS = 500, SLOP = 10;
  function press(el, tap, long) {
    var timer = 0, x = 0, y = 0, id = null, fired = false;
    function cancel() { clearTimeout(timer); el.classList.remove('pressed'); id = null; }
    el.addEventListener('pointerdown', function (e) {
      if (id !== null || (e.pointerType === 'mouse' && e.button !== 0)) return;
      id = e.pointerId; x = e.clientX; y = e.clientY; fired = false;
      el.classList.add('pressed');
      timer = setTimeout(function () {
        fired = true;
        el.classList.remove('pressed');
        if (navigator.vibrate) navigator.vibrate(15);
        long(e);
      }, LONG_MS);
    });
    el.addEventListener('pointermove', function (e) {
      if (e.pointerId === id && Math.abs(e.clientX - x) + Math.abs(e.clientY - y) > SLOP) cancel();
    });
    el.addEventListener('pointerup', function (e) {
      if (e.pointerId !== id) return;
      var wasFired = fired;
      cancel();
      if (!wasFired) tap(e);
    });
    el.addEventListener('pointercancel', cancel);
    el.addEventListener('contextmenu', function (e) { e.preventDefault(); });
  }

  var standalone = navigator.standalone === true || matchMedia('(display-mode: standalone)').matches;
  var mobile = /iPhone|iPad|iPod|Android/.test(navigator.userAgent) ||
               (navigator.maxTouchPoints > 1 && /Macintosh/.test(navigator.userAgent));

  // --- service worker ---------------------------------------------------------------
  // sw.js keeps the shell and the systems for offline use and adds the
  // cross-origin isolation headers (wasm threads need them). A new build's
  // worker takes over when a page asks (the library: at once; the app page:
  // before the system starts). -> Promise<controlled>
  function serviceWorker(onUpdating) {
    checkTrust();
    if (!('serviceWorker' in navigator)) { untrusted(); return Promise.resolve(false); }
    // Safari 16 past the certificate warning: register() neither resolves
    // nor fails. 10 s without a worker in charge is the same answer.
    setTimeout(function () { if (!navigator.serviceWorker.controller) untrusted(); }, 10000);
    navigator.serviceWorker.addEventListener('controllerchange', function () {
      var bar = document.getElementById('senses-untrusted');
      if (bar && bar.dataset.why === 'worker') bar.remove();
    });
    return navigator.serviceWorker.register('sw.js').then(function (reg) {
      if (!navigator.serviceWorker.controller) return navigator.serviceWorker.ready;
      var checked = reg.update().catch(function () {});
      var timeout = new Promise(function (resolve) { setTimeout(resolve, 4000); });
      return Promise.race([checked, timeout]).then(function () {
        var next = reg.installing || reg.waiting;
        if (!next) return navigator.serviceWorker.ready;
        log('Senses: updating to a new build');
        if (onUpdating) onUpdating();
        return new Promise(function (resolve) {
          navigator.serviceWorker.addEventListener('controllerchange', function () { location.reload(); });
          function step() {
            if (next.state === 'installed') next.postMessage('skipWaiting');
            else if (next.state === 'redundant') resolve(navigator.serviceWorker.ready);
          }
          next.addEventListener('statechange', step);
          step();
        });
      });
    }).then(function () {
      if (navigator.serviceWorker.controller) return true;
      return new Promise(function (resolve) {
        navigator.serviceWorker.addEventListener('controllerchange', function () { resolve(true); });
        setTimeout(function () { resolve(!!navigator.serviceWorker.controller); }, 5000);
      });
    }).then(function (controlled) {
      if (!controlled) untrusted();
      return controlled;
    }, function (e) { log('Senses: service worker: ' + e); untrusted(); return false; });
  }

  // The local release's server (tools/release/senses.py: it has
  // /__setup/info.json) over https: does the phone trust its certificate?
  // A phone that came past Safari's "not private" warning ("Visit Website")
  // has an exception for this address, and under it the page, its requests
  // and even its service worker work -- but not for another name. So the
  // answer is a request to the server's check name (info.check,
  // NAME-check.local, which no one opens): through = trusted; refused or no
  // answer = not, and a bar sends the person to the setup page (plain HTTP,
  // a port of its own) to install the profile. Without a check name (a
  // server without mDNS) the service worker tells: none, failed, or not in
  // charge after 10 s. "It is installed" hides the bar for good (a network
  // that does not carry .local names).
  var releaseInfo = null;
  function release() {
    if (!releaseInfo) {
      releaseInfo = fetch('/__setup/info.json', {cache: 'no-store'}).then(function (r) {
        return r.ok ? r.json() : null;
      }).catch(function () { return null; });
    }
    return releaseInfo;
  }
  function checkTrust() {
    if (location.protocol !== 'https:') return;
    release().then(function (info) {
      if (!info || !info.check) return;
      var ctl = window.AbortController ? new AbortController() : null;
      var timer = setTimeout(function () { if (ctl) ctl.abort(); }, 6000);
      fetch('https://' + info.check + ':' + info.port + '/__ping', {cache: 'no-store', signal: ctl && ctl.signal})
        .then(function (r) { return r.ok ? r.json() : null; })
        .then(function (j) { clearTimeout(timer); if (!(j && j.senses)) untrustedBar(info, 'check'); },
              function () { clearTimeout(timer); untrustedBar(info, 'check'); });
    });
  }
  function untrusted() {
    if (location.protocol !== 'https:') return;
    release().then(function (info) { if (info && !info.check) untrustedBar(info, 'worker'); });
  }
  function untrustedBar(info, why) {
    try { if (localStorage.getItem('senses-trust-dismissed')) return; } catch (e) {}
    if (!info.setup_port || document.getElementById('senses-untrusted')) return;
    if (why === 'worker' && navigator.serviceWorker && navigator.serviceWorker.controller) return;
    var bar = document.createElement('div');
    bar.id = 'senses-untrusted';
    bar.dataset.why = why;
    bar.style.cssText = 'position:fixed;left:0;right:0;top:0;z-index:1000;padding:calc(12px + ' +
        'env(safe-area-inset-top)) 16px 12px;background:#5c1f1b;color:#fff;font:15px/1.4 -apple-system,' +
        'system-ui,sans-serif;text-align:center';
    var button = 'display:inline-block;margin:8px 4px 0;padding:8px 16px;border-radius:18px;font-weight:600;' +
        'text-decoration:none;border:0;font-size:15px';
    var link = document.createElement('a');
    link.href = 'http://' + location.hostname + ':' + info.setup_port + '/';
    link.textContent = 'Открыть страницу установки';
    link.style.cssText = button + ';background:#fff;color:#000';
    var hide = document.createElement('button');
    hide.textContent = 'Профиль установлен — скрыть';
    hide.style.cssText = button + ';background:transparent;color:#fff;box-shadow:inset 0 0 0 1px #fff8';
    hide.onclick = function () {
      try { localStorage.setItem('senses-trust-dismissed', '1'); } catch (e) {}
      bar.remove();
    };
    bar.appendChild(document.createTextNode('Этот телефон ещё не доверяет компьютеру, поэтому ' +
        'приложения здесь не заработают. Сначала установите профиль.'));
    bar.appendChild(document.createElement('br'));
    bar.appendChild(link);
    bar.appendChild(hide);
    document.body.appendChild(bar);
    log('Senses: the phone does not trust the server (' + why + '): the setup page is ' + link.href);
  }

  // --- the app's network (topic app-network, docs/topics/app-network.md) -----------
  // meta.net: false = no network for the app (the VM's ARC_NET=0); on when
  // absent. One switch for every app: no per-app lists or presets.

  // No page zoom from stray gestures (iOS ignores user-scalable).
  document.addEventListener('gesturestart', function (e) { e.preventDefault(); });

  root.Senses = {
    $: $, STACKS: STACKS, STACK_ORDER: ['a10', 'a81', 'a60', 'a44'], DEFAULT_STACK: 'a10',
    size: size, log: log, showLog: showLog, open: open, close: close, bar: bar, warnInto: warnInto,
    press: press, standalone: standalone, mobile: mobile, serviceWorker: serviceWorker,
  };
})(self);
