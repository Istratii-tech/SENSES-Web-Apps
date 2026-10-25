const CACHE_NAME = 'senses-pages-__BUILD_VERSION__';
const DATA_FILE = 'a10/artvm10.data';
const DATA_MANIFEST = 'a10/artvm10.data.parts.json';
const SHELL = [
  'index.html',
  'run.html',
  'apk.js',
  'library-worker.js',
  'idb_opfs.js',
  'report.js',
  'bootstrap.js',
  'site.css',
  DATA_MANIFEST,
];

self.addEventListener('install', (event) => {
  event.waitUntil((async () => {
    const cache = await caches.open(CACHE_NAME);
    await cache.addAll(SHELL.map((path) => new URL(path, self.registration.scope).href));
    await self.skipWaiting();
  })());
});

async function dropCache(name) {
  try {
    const cache = await caches.open(name);
    for (const request of await cache.keys()) await cache.delete(request);
  } catch (error) {}
  await caches.delete(name);
}

self.addEventListener('activate', (event) => {
  event.waitUntil((async () => {
    for (const name of await caches.keys()) {
      if (name.startsWith('senses-pages-') && name !== CACHE_NAME) await dropCache(name);
    }
    await self.clients.claim();
  })());
});

async function cachedOrFetch(event, cache, url) {
  const cached = await cache.match(url);
  if (cached) return cached;
  const response = await fetch(url);
  if (response.ok && response.type === 'basic' && response.status === 200) {
    event.waitUntil(cache.put(url, response.clone()).catch(() => {}));
  }
  return response;
}

async function serveData(event, scope) {
  const dataURL = new URL(DATA_FILE, scope);
  const manifestURL = new URL(DATA_MANIFEST, scope);
  const cache = await caches.open(CACHE_NAME);
  const manifestResponse = await cachedOrFetch(event, cache, manifestURL.href);
  if (!manifestResponse.ok) return isolated(manifestResponse, manifestURL.pathname);

  let manifest;
  try {
    manifest = await manifestResponse.json();
  } catch (error) {
    return isolated(new Response('Invalid Android data manifest', { status: 500 }), dataURL.pathname);
  }
  if (!Array.isArray(manifest.parts) || !manifest.parts.length || !Number.isSafeInteger(manifest.size) || manifest.size < 1) {
    return isolated(new Response('Invalid Android data manifest', { status: 500 }), dataURL.pathname);
  }

  const parts = [];
  for (const name of manifest.parts) {
    if (typeof name !== 'string' || !/^artvm10\.data\.part-\d{3}$/.test(name)) {
      return isolated(new Response('Invalid Android data part name', { status: 500 }), dataURL.pathname);
    }
    const partURL = new URL(name, new URL('a10/', scope));
    const response = await cachedOrFetch(event, cache, partURL.href);
    if (!response.ok || !response.body) return isolated(response, partURL.pathname);
    parts.push(response);
  }

  let index = 0;
  let reader = null;
  let bytes = 0;
  const body = new ReadableStream({
    async pull(controller) {
      try {
        while (index < parts.length) {
          if (!reader) reader = parts[index].body.getReader();
          const result = await reader.read();
          if (result.done) {
            index += 1;
            reader = null;
            continue;
          }
          bytes += result.value.byteLength;
          if (bytes > manifest.size) throw new Error('Android data is larger than its manifest');
          controller.enqueue(result.value);
          return;
        }
        if (bytes !== manifest.size) throw new Error('Android data is smaller than its manifest');
        controller.close();
      } catch (error) {
        controller.error(error);
      }
    },
    cancel(reason) {
      if (reader) return reader.cancel(reason);
    },
  });

  return isolated(new Response(body, {
    status: 200,
    headers: {
      'Content-Type': 'application/octet-stream',
      'Content-Length': String(manifest.size),
    },
  }), dataURL.pathname);
}

self.addEventListener('fetch', (event) => {
  const request = event.request;
  if (request.method !== 'GET') return;
  const url = new URL(request.url);
  const scope = new URL(self.registration.scope);
  if (url.origin !== scope.origin || !url.pathname.startsWith(scope.pathname)) return;
  const dataURL = new URL(DATA_FILE, scope);
  if (url.pathname === dataURL.pathname) {
    event.respondWith(serveData(event, scope));
    return;
  }
  event.respondWith((async () => {
    const cache = await caches.open(CACHE_NAME);
    const cached = await cache.match(request, { ignoreSearch: true });
    if (cached) return isolated(cached, url.pathname);
    let response;
    try {
      response = await fetch(request);
    } catch (error) {
      return Response.error();
    }
    if (response.ok && response.type === 'basic' && response.status === 200) {
      event.waitUntil(cache.put(url.origin + url.pathname, response.clone()).catch(() => {}));
    }
    return isolated(response, url.pathname);
  })());
});

function isolated(response, pathname) {
  if (response.status === 0) return response;
  const headers = new Headers(response.headers);
  headers.set('Cross-Origin-Opener-Policy', 'same-origin');
  headers.set('Cross-Origin-Embedder-Policy', 'require-corp');
  headers.set('Cross-Origin-Resource-Policy', 'same-origin');
  if (pathname.endsWith('.wasm')) headers.set('Content-Type', 'application/wasm');
  return new Response(response.body, {
    status: response.status,
    statusText: response.statusText,
    headers,
  });
}
