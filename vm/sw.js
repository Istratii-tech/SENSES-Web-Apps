// SENSES Web Apps: the service worker (topic senses-shell,
// docs/topics/senses-shell.md, "PWA"). As the Web Systems PWA's:
//
// - Offline: the shell (library, app page, their scripts and icons) is
//   cached on install; a system's files (a81/, a10/, a60/, a44/) when a page first
//   fetches them, so only the systems the user's apps run on are downloaded.
//   Everything is served cache-first from one cache per build (VERSION: the
//   hash of the build's files, set by mk/topic-senses-apps.mk); the systems
//   stay in their own caches, keyed by their files' hash, so a new shell
//   does not download them again.
// - Cross-origin isolation: every response gets COOP/COEP/CORP headers, so
//   wasm threads work on hosts that cannot set them.
'use strict';

const VERSION = '6d5248f899a3';
// "a81:hash a60:hash a44:hash": a system's cache changes only with its files.
const SYSTEMS = 'a10:0a8e1a52f5cc '.split(' ').filter(Boolean).map((s) => s.split(':'));
const SHELL_CACHE = 'senses-apps-' + VERSION;
const SHELL = ['./', 'index.html', 'run.html', 'memtest.html', 'origin.html', 'manifest.webmanifest', 'senses.css',
               'senses.js', 'apk.js', 'install.js', 'google.js', 'files.js', 'icon.js', 'store.js', 'store-worker.js', 'origins.js',
               'idb_opfs.js', 'report.js', 'mem_plan.js',
               'stacks.json', 'senses-mask.png', 'icon-180.png', 'icon-192.png', 'icon-512.png',
               'icon-maskable-512.png'];

function cacheFor(url) {
  const path = new URL(url).pathname;
  for (const [id, hash] of SYSTEMS) {
    if (path.indexOf('/' + id + '/') >= 0) return 'senses-sys-' + id + '-' + hash;
  }
  return SHELL_CACHE;
}

// No earlier build of this library here (a first visit, or another site's
// worker at this scope -- the GitHub Pages demo once had its own, whose pages
// never ask for skipWaiting): take over at once instead of waiting for every
// tab to close. An earlier build's pages ask for it themselves (senses.js).
let replacing = false;  // another site's worker was in charge: its pages reload once we are
self.addEventListener('install', (event) => {
  event.waitUntil((async () => {
    const ours = (await caches.keys()).some((name) => name.startsWith('senses-apps-'));
    replacing = !ours && !!self.registration.active;
    const cache = await caches.open(SHELL_CACHE);
    // From the server, not the browser's HTTP cache: GitHub Pages lets pages
    // be cached for 10 minutes, and a new build must not keep an old index.html.
    await cache.addAll(SHELL.map((path) => new Request(path, {cache: 'reload'})));
    if (!ours) await self.skipWaiting();
  })());
});

// A page asks a new build's worker to take over (senses.js, serviceWorker),
// or which cache a file goes to ({op: 'cacheFor', url}, answered on the
// message's port: run.html, whether this build's system is kept yet).
self.addEventListener('message', (event) => {
  if (event.data === 'skipWaiting') self.skipWaiting();
  if (event.data && event.data.op === 'cacheFor' && event.ports[0])
    event.ports[0].postMessage(cacheFor(new URL(event.data.url, self.registration.scope).href));
});

// A cache, entry by entry and then the cache: Safari 16 (WebKit 7615) frees a
// deleted cache's records only once no page has it open, and only if it was
// opened since the browser started -- until then they stay on the device and
// in the origin's quota (the old systems of a previous build: 380 MB; once the
// quota is used up the VM stops writing files: docs/topics/opfs-after-kill.md).
async function dropCache(name) {
  try {
    const cache = await caches.open(name);
    for (const request of await cache.keys()) await cache.delete(request);
  } catch (e) {}
  await caches.delete(name);
}

self.addEventListener('activate', (event) => {
  event.waitUntil((async () => {
    const keep = new Set([SHELL_CACHE].concat(SYSTEMS.map(([id, hash]) => 'senses-sys-' + id + '-' + hash)));
    for (const name of await caches.keys()) {
      if (name.startsWith('senses-') && !keep.has(name)) await dropCache(name);
    }
    await self.clients.claim();
    // The other worker's pages do not know this library: show them its pages.
    if (replacing) {
      for (const client of await self.clients.matchAll({type: 'window'})) {
        try { await client.navigate(client.url); } catch (e) {}
      }
    }
  })());
});

self.addEventListener('fetch', (event) => {
  const request = event.request;
  if (request.method !== 'GET') return;
  const url = new URL(request.url);
  // Same origin only; /__log and /__net are tools/serve.py's, never cached.
  if (url.origin !== self.location.origin || url.pathname.startsWith('/__')) return;
  // vm/: the VM side's test page (topic install8), not part of the app: as served.
  if (url.href.startsWith(new URL('vm/', self.registration.scope).href)) return;
  // The apps' origins (origins.js): the copy kept, at once; the host's when
  // there is none yet.
  if (url.href === new URL('origins.json', self.registration.scope).href) {
    event.respondWith(fresh(event, request, url));
    return;
  }
  event.respondWith(respond(event, request, url));
});

// The copy kept from the last answer, at once, while the server's answer
// replaces it for the next start (the slots change only with the server's
// ports). The library and the app's page wait for origins.json before
// anything else, and with the computer out of reach (the phone's Wi-Fi off)
// iOS holds a request to its .local name for a minute instead of failing it.
// No copy yet: the server's answer if it comes within 3 s.
async function fresh(event, request, url) {
  const cache = await caches.open(SHELL_CACHE);
  const key = url.origin + url.pathname;
  const net = fetch(request).then((response) => {
    if (response.ok) return cache.put(key, response.clone()).catch(() => {}).then(() => response);
    return response;
  }, () => null);
  const kept = await cache.match(key);
  if (kept) {
    event.waitUntil(net);
    return kept;
  }
  const response = await Promise.race([net, new Promise((resolve) => setTimeout(resolve, 3000, null))]);
  return response || new Response('', {status: 404});
}

async function respond(event, request, url) {
  const cache = await caches.open(cacheFor(request.url));
  // ?pkg=, ?canvas2d=1 and the like do not change the files.
  const hit = await cache.match(request, {ignoreSearch: true});
  if (hit) return isolated(hit, libraryPage(request));
  let response;
  try {
    response = await fetch(request);
  } catch (e) {
    return Response.error();
  }
  if (response.ok && response.type === 'basic' && response.status === 200) {
    // Kept alive until the whole body is stored (200 MB for artvm8.data).
    event.waitUntil(cache.put(url.origin + url.pathname, response.clone()).catch(() => {}));
  }
  return isolated(response, libraryPage(request));
}

// The library's own page needs no cross-origin isolation (no
// SharedArrayBuffer there), and without it going from an app's page
// (isolated) to it changes the browsing context group: WebKit then gives it
// a new process and drops the one that held the app's VM with all its
// memory at once, instead of giving that back piece by piece while the next
// app's page is refused memory.
// origin.html (topic per-app-origin) neither: the library's window on an
// app's origin, which must keep its opener.
function libraryPage(request) {
  if (request.mode !== 'navigate') return false;
  const path = new URL(request.url).pathname;
  const scope = new URL(self.registration.scope).pathname;
  return path === scope || path === scope + 'index.html' || path === scope + 'origin.html';
}

function isolated(response, library) {
  if (response.status === 0) return response;  // opaque: headers are not ours to change
  const headers = new Headers(response.headers);
  if (library) {
    headers.delete('Cross-Origin-Opener-Policy');
    headers.delete('Cross-Origin-Embedder-Policy');
    headers.set('Cross-Origin-Resource-Policy', 'same-origin');
    return new Response(response.body, {status: response.status, statusText: response.statusText, headers});
  }
  headers.set('Cross-Origin-Opener-Policy', 'same-origin');
  headers.set('Cross-Origin-Embedder-Policy', 'require-corp');
  headers.set('Cross-Origin-Resource-Policy', 'same-origin');
  return new Response(response.body, {status: response.status, statusText: response.statusText, headers});
}
