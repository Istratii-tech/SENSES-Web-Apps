// The PWA's service worker (topic userspace, docs/topics/userspace.md "PWA").
//
// - Offline: the page and its small files are cached on install; a system's
//   files (a44/dalvikvm.*, a60/artvm.*) when the page first fetches them, so
//   they are downloaded once, with the page's progress bar. Everything is
//   served cache-first from one cache per build (VERSION is the hash of the
//   build's files, set by mk/topic-pwa.mk); a new build's worker takes over
//   when the page starting up asks it to (or once the old pages are
//   closed), then drops the old cache.
// - Cross-origin isolation: every response gets COOP/COEP/CORP headers (as
//   coi-serviceworker does), so wasm threads work on hosts that cannot set
//   them; the page reloads once after this worker takes it over.
'use strict';

const VERSION = '__VERSION__';
const CACHE = 'arcwasm-pwa-' + VERSION;
// The page; each system's files (a44/, a60/) are cached when the page first
// loads them, so only the system the user picked is downloaded.
const SHELL = ['./', 'index.html', 'manifest.webmanifest', 'report.js', 'senses-mask.png',
               'icon-180.png', 'icon-192.png', 'icon-512.png'];

self.addEventListener('install', (event) => {
  event.waitUntil(caches.open(CACHE).then((cache) => cache.addAll(SHELL)));
});

// The page asks a new build's worker to take over when Android has not
// started yet (index.html, serviceWorkerReady).
self.addEventListener('message', (event) => {
  if (event.data === 'skipWaiting') self.skipWaiting();
});

self.addEventListener('activate', (event) => {
  event.waitUntil((async () => {
    for (const name of await caches.keys()) {
      if (name.startsWith('arcwasm-pwa-') && name !== CACHE) await caches.delete(name);
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
  event.respondWith(respond(event, request, url));
});

async function respond(event, request, url) {
  const cache = await caches.open(CACHE);
  // The page's ?pkg= and similar parameters do not change the files.
  const hit = await cache.match(request, { ignoreSearch: true });
  if (hit) return isolated(hit);
  let response;
  try {
    response = await fetch(request);
  } catch (e) {
    return Response.error();
  }
  if (response.ok && response.type === 'basic') {
    // Kept alive until the whole body is stored (84 MB for dalvikvm.data).
    event.waitUntil(cache.put(url.origin + url.pathname, response.clone()).catch(() => {}));
  }
  return isolated(response);
}

function isolated(response) {
  if (response.status === 0) return response;  // opaque: headers are not ours to change
  const headers = new Headers(response.headers);
  headers.set('Cross-Origin-Opener-Policy', 'same-origin');
  headers.set('Cross-Origin-Embedder-Policy', 'require-corp');
  headers.set('Cross-Origin-Resource-Policy', 'same-origin');
  return new Response(response.body, {
    status: response.status,
    statusText: response.statusText,
    headers,
  });
}
