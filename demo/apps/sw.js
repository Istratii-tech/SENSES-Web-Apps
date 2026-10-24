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

const VERSION = '__VERSION__';
// "a81:hash a60:hash a44:hash": a system's cache changes only with its files.
const SYSTEMS = '__SYSTEMS__'.split(' ').filter(Boolean).map((s) => s.split(':'));
const SHELL_CACHE = 'senses-apps-' + VERSION;
const SHELL = ['./', 'index.html', 'run.html', 'memtest.html', 'origin.html', 'manifest.webmanifest', 'senses.css',
               'senses.js', 'apk.js', 'install.js', 'google.js', 'icon.js', 'store.js', 'store-worker.js', 'origins.js',
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

self.addEventListener('install', (event) => {
  event.waitUntil(caches.open(SHELL_CACHE).then((cache) => cache.addAll(SHELL)));
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
  // The apps' origins (origins.js): the host's, now; none offline.
  if (url.href === new URL('origins.json', self.registration.scope).href) return;
  event.respondWith(respond(event, request, url));
});

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
