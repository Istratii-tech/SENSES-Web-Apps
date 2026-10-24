// An Origin Private File System kept in IndexedDB (--pre-js), for browsers
// whose OPFS cannot write: Safari before 16.4 fails to open sync access
// handles (iOS 16.1: "Invalid platform file handle"), and /data used to be
// kept in the tab's memory there (shim/opfs/memory_opfs.js): not kept between
// runs, and an app's APK alone could take the tab past its memory (topic
// userspace: installing Angry Birds on an iPhone 12 Pro). IndexedDB is on
// the device's storage, in every Safari: library_arc_opfs.js prefers this
// to the in-memory one (globalThis.arcIdbOpfs).
//
// The same subset of the OPFS API as memory_opfs.js: getDirectory();
// directory handles (getDirectoryHandle, getFileHandle, removeEntry, keys,
// entries, move); file handles (getFile, createSyncAccessHandle, move);
// access handles (read, write, getSize, truncate, flush, close), here all
// asynchronous, as Safari's first OPFS had them (library_arc_opfs.js awaits
// every call); errors as DOMExceptions with the browsers' names. Only the
// OPFS thread calls it.
//
// Database "arc-wasm-data":
//   nodes   path ("/", "/arc-wasm/data/...") -> {kind, parent, id, size, mtime}
//           (index "parent": a directory's entries)
//   chunks  [file id, index] -> Uint8Array of CHUNK bytes (holes read as
//           zeros); a file keeps its id when renamed, so moving it moves no data
// Writes go to the database at once (one transaction per write); a cache of
// chunks (16 MB, read ahead in 512 KB runs) spares reads. Nothing else stays
// in memory.
//
// ArrayBuffers are few (topic opfs-oom-ios, docs/topics/opfs-oom-ios.md): in
// Safari they share one range of addresses of the web process with the VM's
// wasm memory, which reserves its maximum there, and the VM's reads and
// writes failed with "RangeError: Out of memory" while the tab had memory to
// spare (Subway Surfers on an iPhone 12 Pro, iOS 16.1). A chunk read is the
// one the database gives (no copy); a write fills one scratch chunk per
// chunk and hands it to the database, which copies it at once (put clones
// its value), and cached chunks are updated in place; read-only files read
// straight into the caller's buffer (getFile().readInto). arcIdbOpfs.trim()
// lets the cache go (library_arc_opfs.js, when an allocation fails).
(function () {
  if (typeof indexedDB === 'undefined') return;
  const CHUNK = 65536;
  const CACHE_MAX = 256;  // chunks (16 MB)
  const READ_AHEAD = 8;   // chunks read together on a miss (files are mostly read in order)
  const fail = (name, msg) => { throw new DOMException(msg || name, name); };

  let dbPromise = null;
  function db() {
    if (!dbPromise) {
      dbPromise = new Promise((resolve, reject) => {
        const r = indexedDB.open('arc-wasm-data', 1);
        r.onupgradeneeded = () => {
          const d = r.result;
          d.createObjectStore('nodes').createIndex('parent', 'parent');
          d.createObjectStore('chunks');
        };
        r.onsuccess = () => resolve(r.result);
        r.onerror = () => reject(r.error);
        r.onblocked = () => reject(new DOMException('database blocked', 'InvalidStateError'));
      }).then(async (d) => {
        if (!(await get(d, 'nodes', '/'))) {
          await put(d, [['nodes', '/', {kind: 'directory', parent: '', id: '', size: 0,
                                        mtime: Date.now()}]]);
        }
        return d;
      });
    }
    return dbPromise;
  }
  function request(r) {
    return new Promise((resolve, reject) => {
      r.onsuccess = () => resolve(r.result);
      r.onerror = () => reject(r.error);
    });
  }
  function finished(tx) {
    return new Promise((resolve, reject) => {
      tx.oncomplete = () => resolve();
      tx.onerror = () => reject(tx.error);
      tx.onabort = () => reject(tx.error || new DOMException('transaction aborted', 'AbortError'));
    });
  }
  function get(d, store, key) {
    return request(d.transaction(store).objectStore(store).get(key));
  }
  // One read-write transaction: [store, key, value] puts (value undefined:
  // delete; key an IDBKeyRange: delete the range).
  function put(d, ops) {
    const stores = [...new Set(ops.map((o) => o[0]))];
    const tx = d.transaction(stores, 'readwrite');
    for (const [store, key, value] of ops) {
      const s = tx.objectStore(store);
      if (value === undefined) s.delete(key); else s.put(value, key);
    }
    return finished(tx);
  }
  const join = (dir, name) => (dir === '/' ? '/' : dir + '/') + name;
  const chunkRange = (id) => IDBKeyRange.bound([id, 0], [id, Infinity]);
  let idCounter = 0;
  const newId = () => Date.now().toString(36) + '.' + (idCounter++).toString(36) + '.' +
      Math.floor(Math.random() * 1e9).toString(36);

  // Chunks recently read or written: "id:index" -> Uint8Array(CHUNK).
  const cache = new Map();
  // The chunk a write or truncate fills before the database copies it.
  let scratchChunk = null;
  const scratch = () => scratchChunk || (scratchChunk = new Uint8Array(CHUNK));
  function cached(key, chunk) {
    cache.delete(key);
    cache.set(key, chunk);
    while (cache.size > CACHE_MAX) cache.delete(cache.keys().next().value);
  }
  function uncache(id) {
    for (const k of [...cache.keys()]) if (k.startsWith(id + ':')) cache.delete(k);
  }
  // Chunk |i| of file |id| (null: a hole); on a miss, it and the next
  // READ_AHEAD - 1 come in one transaction.
  async function chunkOf(d, id, i) {
    const key = id + ':' + i;
    let c = cache.get(key);
    if (c) {
      cached(key, c);
      return c;
    }
    const range = IDBKeyRange.bound([id, i], [id, i + READ_AHEAD - 1]);
    const store = d.transaction('chunks').objectStore('chunks');
    const [keys, values] = await Promise.all([request(store.getAllKeys(range)),
                                              request(store.getAll(range))]);
    c = null;
    keys.forEach((k, n) => {
      let v = values[n] instanceof Uint8Array ? values[n] : new Uint8Array(values[n]);
      if (v.length < CHUNK) {
        const full = new Uint8Array(CHUNK);
        full.set(v);
        v = full;
      }
      const ck = id + ':' + k[1];
      if (k[1] === i) c = v;
      else if (cache.has(ck)) return;  // the cached copy is the same (writes go through)
      cached(ck, v);
    });
    if (c) cached(key, c);
    return c;
  }

  // Open files: file id -> true while a sync access handle has it.
  const locked = new Set();

  class Handle {
    constructor(path, node) {
      this.path = path;
      this.node = node;
      this.kind = node.kind;
      this.name = path === '/' ? '' : path.slice(path.lastIndexOf('/') + 1);
    }

    async move(destDir, newName) {
      const d = await db();
      if (this.path === '/') fail('InvalidModificationError', 'cannot move the root');
      const node = await get(d, 'nodes', this.path);
      if (!node) fail('NotFoundError', this.path);
      if (node.kind === 'file' && locked.has(node.id)) fail('NoModificationAllowedError');
      const to = join(destDir.path, newName);
      if (to === this.path) return;
      const ops = [];
      const old = await get(d, 'nodes', to);
      if (old) {
        if (old.kind !== node.kind) fail('TypeMismatchError');
        if (old.kind === 'directory' && (await children(d, to)).length) {
          fail('InvalidModificationError', 'not empty');
        }
        if (old.kind === 'file') {
          if (locked.has(old.id)) fail('NoModificationAllowedError');
          ops.push(['chunks', chunkRange(old.id), undefined]);
          uncache(old.id);
        }
      }
      ops.push(['nodes', this.path, undefined]);
      ops.push(['nodes', to, Object.assign({}, node, {parent: destDir.path})]);
      if (node.kind === 'directory') {  // its whole subtree, by path
        const tx = d.transaction('nodes');
        const prefix = this.path + '/';
        const sub = await request(tx.objectStore('nodes').getAll(
            IDBKeyRange.bound(prefix, prefix + '￿')));
        const keys = await request(d.transaction('nodes').objectStore('nodes').getAllKeys(
            IDBKeyRange.bound(prefix, prefix + '￿')));
        keys.forEach((k, i) => {
          const moved = to + k.slice(this.path.length);
          ops.push(['nodes', k, undefined]);
          ops.push(['nodes', moved, Object.assign({}, sub[i], {
            parent: to + sub[i].parent.slice(this.path.length)})]);
        });
      }
      await put(d, ops);
      this.path = to;
      this.name = newName;
    }
  }

  async function children(d, path) {
    return request(d.transaction('nodes').objectStore('nodes').index('parent').getAllKeys(path));
  }

  class FileHandle extends Handle {
    // size and lastModified, and slice(start, end).arrayBuffer() as a File
    // has (read-only opens of library_arc_opfs.js): a snapshot of the node,
    // read through the chunk cache without locking the file; readInto(view,
    // at) reads from |at| straight into |view| (any typed array, a view of
    // shared wasm memory too) and gives the count, with no new buffer.
    async getFile() {
      const d = await db();
      const node = await get(d, 'nodes', this.path);
      if (!node) fail('NotFoundError', this.path);
      const path = this.path;
      const reader = new AccessHandle(d, path, node);
      return {size: node.size, lastModified: node.mtime,
              slice: (start, end) => ({arrayBuffer: async () => {
                const n = Math.max(0, Math.min(end, node.size) - start);
                const out = new Uint8Array(n);
                await reader.read(out, {at: start});
                return out.buffer;
              }}),
              readInto: (view, at) => reader.read(view, {at})};
    }

    async createSyncAccessHandle() {
      const d = await db();
      const node = await get(d, 'nodes', this.path);
      if (!node) fail('NotFoundError', this.path);
      if (locked.has(node.id)) fail('NoModificationAllowedError', 'file is locked');
      locked.add(node.id);
      return new AccessHandle(d, this.path, node);
    }
  }

  class DirectoryHandle extends Handle {
    async child(name, kind, create) {
      const d = await db();
      const p = join(this.path, name);
      let node = await get(d, 'nodes', p);
      if (node && node.kind !== kind) fail('TypeMismatchError', name);
      if (!node) {
        if (!create) fail('NotFoundError', name);
        node = {kind, parent: this.path, id: kind === 'file' ? newId() : '', size: 0,
                mtime: Date.now()};
        await put(d, [['nodes', p, node]]);
      }
      return new (kind === 'file' ? FileHandle : DirectoryHandle)(p, node);
    }

    getDirectoryHandle(name, options) {
      return this.child(name, 'directory', options && options.create);
    }

    getFileHandle(name, options) {
      return this.child(name, 'file', options && options.create);
    }

    async removeEntry(name) {
      const d = await db();
      const p = join(this.path, name);
      const node = await get(d, 'nodes', p);
      if (!node) fail('NotFoundError', name);
      if (node.kind === 'directory' && (await children(d, p)).length) {
        fail('InvalidModificationError', 'not empty');
      }
      if (node.kind === 'file' && locked.has(node.id)) {
        fail('NoModificationAllowedError', 'file is locked');
      }
      const ops = [['nodes', p, undefined]];
      if (node.kind === 'file') {
        ops.push(['chunks', chunkRange(node.id), undefined]);
        uncache(node.id);
      }
      await put(d, ops);
    }

    async *keys() {
      for (const p of await children(await db(), this.path)) {
        yield p.slice(p.lastIndexOf('/') + 1);
      }
    }

    async *entries() {
      const d = await db();
      const index = d.transaction('nodes').objectStore('nodes').index('parent');
      const paths = await request(index.getAllKeys(this.path));
      const nodes = await request(d.transaction('nodes').objectStore('nodes').index('parent')
          .getAll(this.path));
      for (let i = 0; i < paths.length; i++) {
        const p = paths[i];
        yield [p.slice(p.lastIndexOf('/') + 1),
               new (nodes[i].kind === 'directory' ? DirectoryHandle : FileHandle)(p, nodes[i])];
      }
    }
  }

  class AccessHandle {
    constructor(d, path, node) {
      this.d = d;
      this.path = path;
      this.node = Object.assign({}, node);
      this.queue = Promise.resolve();
    }

    // One operation at a time per file: a write reads, then writes whole chunks.
    serial(fn) {
      const run = this.queue.then(fn);
      this.queue = run.catch(() => {});
      return run;
    }

    read(buffer, options) {
      return this.serial(async () => {
        const at = (options && options.at) || 0;
        const n = Math.max(0, Math.min(buffer.length, this.node.size - at));
        buffer.fill(0, 0, n);
        for (let done = 0; done < n;) {
          const pos = at + done;
          const i = Math.floor(pos / CHUNK), off = pos % CHUNK;
          const len = Math.min(CHUNK - off, n - done);
          const c = await chunkOf(this.d, this.node.id, i);
          if (c) buffer.set(c.subarray(off, off + len), done);
          done += len;
        }
        return n;
      });
    }

    // |buffer| may be a view of shared wasm memory: what the database keeps
    // is the scratch chunk's copy, made by put() when it is called.
    write(buffer, options) {
      return this.serial(async () => {
        const at = (options && options.at) || 0;
        const id = this.node.id;
        // The chunks written in part (the first and the last) as they are,
        // before the transaction (which must not wait on anything else).
        const partial = new Map();
        if (buffer.length) {
          const first = Math.floor(at / CHUNK), last = Math.floor((at + buffer.length - 1) / CHUNK);
          if (at % CHUNK || (first === last && buffer.length < CHUNK)) {
            partial.set(first, await chunkOf(this.d, id, first));
          }
          if (last !== first && (at + buffer.length) % CHUNK) {
            partial.set(last, await chunkOf(this.d, id, last));
          }
        }
        const node = Object.assign({}, this.node, {
          size: Math.max(this.node.size, at + buffer.length), mtime: Date.now()});
        const tx = this.d.transaction(['chunks', 'nodes'], 'readwrite');
        const chunks = tx.objectStore('chunks');
        const s = scratch();
        try {
          for (let done = 0; done < buffer.length;) {
            const pos = at + done;
            const i = Math.floor(pos / CHUNK), off = pos % CHUNK;
            const len = Math.min(CHUNK - off, buffer.length - done);
            if (len < CHUNK) {
              const old = partial.get(i);
              if (old) s.set(old); else s.fill(0);
            }
            s.set(buffer.subarray(done, done + len), off);
            chunks.put(s, [id, i]);
            const c = cache.get(id + ':' + i);
            if (c) c.set(s);
            done += len;
          }
          tx.objectStore('nodes').put(node, this.path);
          await finished(tx);
        } catch (e) {
          uncache(id);  // cached chunks may hold what did not get written
          throw e;
        }
        this.node = node;
        return buffer.length;
      });
    }

    getSize() {
      return this.serial(async () => this.node.size);
    }

    truncate(size) {
      return this.serial(async () => {
        const ops = [];
        if (size < this.node.size) {
          const keep = Math.ceil(size / CHUNK);
          ops.push(['chunks', IDBKeyRange.bound([this.node.id, keep], [this.node.id, Infinity]),
                    undefined]);
          uncache(this.node.id);
          if (size % CHUNK) {  // the last chunk kept: zeros past the end
            const c = await chunkOf(this.d, this.node.id, keep - 1);
            if (c) {
              const z = scratch();  // (put() copies it)
              z.set(c);
              z.fill(0, size % CHUNK);
              ops.push(['chunks', [this.node.id, keep - 1], z]);
            }
          }
          uncache(this.node.id);
        }
        const node = Object.assign({}, this.node, {size, mtime: Date.now()});
        ops.push(['nodes', this.path, node]);
        await put(this.d, ops);
        this.node = node;
      });
    }

    flush() {
      return this.serial(async () => {});  // every write is in the database already
    }

    close() {
      return this.serial(async () => { locked.delete(this.node.id); });
    }
  }

  globalThis.arcIdbOpfs = {
    getDirectory: async () => new DirectoryHandle('/', await get(await db(), 'nodes', '/')),
    // Lets the cached chunks go (their ArrayBuffers, for the GC).
    trim: () => { cache.clear(); scratchChunk = null; },
  };
})();
