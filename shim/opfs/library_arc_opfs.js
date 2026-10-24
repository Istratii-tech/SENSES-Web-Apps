// OPFS (Origin Private File System) bridge for posix_translation's
// OpfsHandler (shim/opfs/opfs_file.cc). Linked with --js-library.
//
// Everything here runs on one dedicated "OPFS thread" (a pthread idling in
// its event loop). C code on other threads proxies a request to it with
// emscripten_proxy_sync_with_ctx() and blocks; arc_opfs_start() kicks off the
// asynchronous OPFS work and calls emscripten_proxy_finish() when done. All
// FileSystemSyncAccessHandles live on this thread, as OPFS requires.
//
// Request layout: struct ArcOpfsRequest in shim/opfs/opfs_bridge.h.
//
// Storage space (topic opfs-after-kill, docs/topics/opfs-after-kill.md): a
// sync access handle's write or truncate that makes its file larger may ask
// the browser for space, synchronously. On Safari 16 (WebKit 7615, iOS
// 16.7; later ones not checked), once the origin's usage reaches its quota
// (1000 MB; OPFS, Cache Storage and IndexedDB together), that request waits
// for an answer from Safari that does not come while the page lives, and
// this thread stops inside the call -- with it every file operation of the
// VM. Each such call is marked in arc_opfs_stall (opfs_bridge.cc) for the
// page to see and say so. Where the page says how much space was free
// (?opfsfree=MB), this thread keeps the count Safari keeps and fails a write
// that would ask for more with ENOSPC instead (ArcOpfs.budgetOf, topic
// opfs-budget).
//
// Safari before 16.4 (iOS 15.2-16.3) has the first version of the API, where
// getSize, truncate, flush and close of a sync access handle return
// promises: every call to them is awaited (a no-op where they are
// synchronous), and a file is not opened again before its previous handle
// has closed. Where a handle cannot be opened at all (iOS 16.1), /data is
// kept in IndexedDB instead (shim/opfs/idb_opfs.js), or in memory where that
// is missing too (openRoot).
//
// Paths are from the origin's OPFS root (the C side adds its mount's base
// directory: "/arc-wasm/...", or "/senses/data/a81/..." and the read-only
// "/senses/apps/..." of SENSES Web Apps; topic install8). Read-only opens
// (flag 8) read through a File (getFile().slice()), not a sync access
// handle: no lock on the file (the page may read it too), none of Safari's
// ~250 handles used.

addToLibrary({
  $ArcOpfs__deps: ['$UTF8ToString', '$stringToUTF8', 'malloc', '$ERRNO_CODES'],
  $ArcOpfs: {
    rootPromise: null,
    nextId: 1,
    // id -> {path, entry}; path -> {fh, access, refs, mtime, used}
    handles: new Map(),
    open: new Map(),
    // path -> promise of the last handle of a closed file, until it is closed
    closing: new Map(),
    // path -> promise of the entry being opened: requests run concurrently
    // (each awaits OPFS), and a second createSyncAccessHandle() of a file
    // fails while the first holds it (Safari: InvalidStateError, Chrome:
    // NoModificationAllowedError), e.g. two threads opening one file.
    opening: new Map(),
    // Open files keep their FileSystemSyncAccessHandle only while used:
    // Safari allows about 250 at a time (then "UnknownError: Invalid
    // platform file handle"), and an app keeps hundreds of files open
    // (TikTok: its settings store, databases). At most |maxLive| handles are
    // live; the least recently used is closed (flushed) and reopened when
    // its file is used again.
    maxLive: 192,
    live: 0,
    clock: 0,

    // ?opfstrace=1 (the page's query, which threads load the VM's script
    // with): each step of this thread's start straight to tools/serve.py's
    // /__log, by a synchronous request from this worker -- out before the
    // step, even if it blocks this thread for good (topic opfs-after-kill).
    traced: 0,
    tracing: null,  // decided on first use (this object is built at link time)
    t0: 0,
    isTracing() {
      if (ArcOpfs.tracing === null) {
        ArcOpfs.tracing = typeof self !== 'undefined' && !!self.location &&
                          /[?&]opfstrace=1\b/.test(self.location.search);
        ArcOpfs.t0 = Date.now();
      }
      return ArcOpfs.tracing;
    },
    trace(what) {
      if (!ArcOpfs.isTracing()) return;
      try {
        const x = new XMLHttpRequest();
        x.open('POST', '/__log', false);
        x.send('opfstrace +' + (Date.now() - ArcOpfs.t0) + ' ms: ' + what);
      } catch (e) {}
    },

    // The open entry of |p| (file |name| in |parent|), opening it if needed.
    async entryFor(parent, name, p, create) {
      for (;;) {
        const entry = ArcOpfs.open.get(p);
        if (entry) return entry;
        const pending = ArcOpfs.opening.get(p);
        if (pending) {
          try { await pending; } catch (e) {}  // then look again (maybe with |create|)
          continue;
        }
        const closing = ArcOpfs.closing.get(p);
        if (closing) {
          await closing;
          continue;
        }
        const opening = (async () => {
          let fh;
          try {
            fh = await parent.getFileHandle(name, {create});
          } catch (e) {
            if (e.name === 'TypeMismatchError') throw ERRNO_CODES.EISDIR;
            throw e;
          }
          const file = await fh.getFile();
          const e = {fh, access: null, refs: 0, used: 0, size: file.size,
                     mtime: file.lastModified / 1000};
          await ArcOpfs.accessOf(e);  // fails here, not on first use
          ArcOpfs.open.set(p, e);
          return e;
        })();
        ArcOpfs.opening.set(p, opening);
        try {
          return await opening;
        } finally {
          ArcOpfs.opening.delete(p);
        }
      }
    },

    // |entry|'s live access handle, (re)opened if the pool closed it.
    async accessOf(entry) {
      entry.used = ++ArcOpfs.clock;
      while (!entry.access) {
        if (entry.moving) {  // being unlinked or renamed: its handle reopens there
          await entry.moving;
          continue;
        }
        if (entry.opening) {
          await entry.opening;
          continue;
        }
        if (entry.closing) {  // evicted, its handle still closing
          await entry.closing;
          continue;
        }
        while (ArcOpfs.live >= ArcOpfs.maxLive && ArcOpfs.evictOne(entry)) {}
        entry.opening = entry.fh.createSyncAccessHandle();
        try {
          entry.access = await entry.opening;
          ArcOpfs.live++;
          ArcOpfs.capped(entry);
        } finally {
          entry.opening = null;
        }
        if (entry.closed) {  // its last descriptor closed meanwhile
          await ArcOpfs.closeAccess(entry);
          throw ERRNO_CODES.EBADF;
        }
      }
      return entry.access;
    },

    // Closes the least recently used live handle other than |keep|'s.
    evictOne(keep) {
      let lru = null;
      for (const e of ArcOpfs.open.values()) {
        if (e !== keep && e.access && (!lru || e.used < lru.used)) lru = e;
      }
      if (!lru) return false;
      ArcOpfs.closeAccess(lru);
      return true;
    },

    // Closes |entry|'s live handle; the promise (also entry.closing until
    // then) settles once it is closed. Never rejects.
    closeAccess(entry) {
      if (!entry.access) return entry.closing || Promise.resolve();
      const access = entry.access;
      entry.access = null;
      ArcOpfs.live--;
      ArcOpfs.uncapped(entry);
      const closing = (async () => {
        try {
          await access.flush();
        } finally {
          await access.close();
        }
      })().catch((e) => { err('arc-opfs: close: ' + (e && e.name) + ': ' + e); });
      entry.closing = closing;
      closing.then(() => { if (entry.closing === closing) entry.closing = null; });
      return closing;
    },

    // Runs |fn| on |entry|'s access handle; again if the pool closed that
    // handle meanwhile (another request ran while this one awaited).
    async withAccess(entry, fn) {
      for (;;) {
        const access = await ArcOpfs.accessOf(entry);
        try {
          return await fn(access);
        } catch (e) {
          if (entry.access === access) throw e;
        }
      }
    },

    // POSIX lets an open file be unlinked (or renamed over, or renamed): its
    // descriptors keep the data, the name goes. OPFS refuses to remove or
    // move a file with an open sync access handle, so the entry's handle is
    // closed and the file moved (the FileSystemFileHandle follows it; the
    // handle reopens there on use). Unlinked, it goes into UNLINKED at the
    // root and is removed when its last descriptor closes (or at the next
    // start). TDLib (Telegram X) unlinks its open binlog: EBUSY was fatal.
    UNLINKED: '.arc-unlinked',
    unlinkedSeq: 0,
    // Moves open |entry| of path |from| to |name| in |dest| (or into
    // UNLINKED when |dest| is null); |from| is free for others meanwhile.
    async moveOpen(entry, from, dest, name) {
      if (typeof entry.fh.move !== 'function') throw ERRNO_CODES.EBUSY;
      ArcOpfs.open.delete(from);
      const moving = (async () => {
        await ArcOpfs.closeAccess(entry);
        if (!dest) {
          const root = await ArcOpfs.root();
          dest = await root.getDirectoryHandle(ArcOpfs.UNLINKED, {create: true});
          name = Date.now().toString(36) + '-' + ++ArcOpfs.unlinkedSeq;
        }
        await entry.fh.move(dest, name);
        return {dir: dest, name};
      })();
      // Opens, unlinks and renames of |from| wait for it (A.closing).
      const settled = moving.then(() => {}, () => {});
      entry.moving = settled;
      ArcOpfs.closing.set(from, settled);
      try {
        return await moving;
      } catch (e) {
        if (!ArcOpfs.open.has(from)) ArcOpfs.open.set(from, entry);
        throw e;
      } finally {
        entry.moving = null;
        if (ArcOpfs.closing.get(from) === settled) ArcOpfs.closing.delete(from);
      }
    },

    root() {
      if (!ArcOpfs.rootPromise) {
        ArcOpfs.rootPromise = ArcOpfs.openRoot().then(async (root) => {
          // Files unlinked while open by a VM that ended without closing them.
          ArcOpfs.trace('root: ' + ArcOpfs.storage + '; ' + ArcOpfs.UNLINKED);
          try {
            const dir = await root.getDirectoryHandle(ArcOpfs.UNLINKED);
            for await (const name of dir.keys()) {
              ArcOpfs.trace('root: removing unlinked ' + name);
              try {
                const size = ArcOpfs.budgetOf() ? (await (await dir.getFileHandle(name)).getFile()).size : 0;
                await dir.removeEntry(name);
                ArcOpfs.released(size);
              } catch (e) {}  // open in another tab
            }
          } catch (e) {}
          ArcOpfs.trace('root: ready');
          await ArcOpfs.growTest(root);
          return root;
        });
      }
      return ArcOpfs.rootPromise;
    },

    // The OPFS root, once a file in it can be written through a sync access
    // handle. Where it cannot (iOS 16.1: "Invalid platform file handle" from
    // the first handle on), the same in IndexedDB (shim/opfs/idb_opfs.js: on
    // the device's storage, kept), else in memory (shim/opfs/memory_opfs.js:
    // not kept), whichever is linked; ArcOpfs.storage says which ('opfs',
    // 'idb', 'memory'). SENSES Web Apps' shell probes the same way and keeps
    // its app library in the same one (docs/topics/install8.md).
    async openRoot() {
      const T = ArcOpfs.trace;
      T('openRoot: getDirectory');
      const dir = await navigator.storage.getDirectory();
      ArcOpfs.storage = 'opfs';
      const fallbacks = [[globalThis.arcIdbOpfs, 'in IndexedDB (kept on this device)'],
                         [globalThis.arcMemoryOpfs, 'in memory and is not kept']]
          .filter((f) => f[0] && f[0] !== navigator.storage);
      // Nothing to fall back to, or already the in-memory one (Node).
      if (navigator.storage === globalThis.arcMemoryOpfs) ArcOpfs.storage = 'memory';
      if (!fallbacks.length || navigator.storage === globalThis.arcMemoryOpfs) return dir;
      try {
        // dalvikvm.js?memdata=1 (threads run that URL): as if it could not
        // (memdata=mem: and skip IndexedDB).
        const forced = /[?&]memdata=(1|mem)\b/.exec(self.location.search);
        if (forced) throw new Error('memdata=' + forced[1]);
        // The probe file stays (one byte, written over at each start): a
        // write inside a file's size asks the browser for no space, so a
        // full origin does not stop the VM here, before it can say so (it
        // did: the file left at 0 bytes; docs/topics/opfs-after-kill.md).
        T('openRoot: probe getFileHandle');
        const probe = await dir.getFileHandle('.arc-opfs-probe', {create: true});
        T('openRoot: probe createSyncAccessHandle');
        const access = await probe.createSyncAccessHandle();
        try {
          const probeEntry = {size: await access.getSize()};
          T('openRoot: probe write (file of ' + probeEntry.size + ' bytes)');
          ArcOpfs.capped(probeEntry);
          try {
            await ArcOpfs.growing(probeEntry, '/.arc-opfs-probe', 1, () => access.write(new Uint8Array([1]), {at: 0}));
            ArcOpfs.setSize(probeEntry, Math.max(probeEntry.size, 1));
          } catch (e) {
            // An empty probe (a new origin) on an origin with no space left
            // (storage budget): the handle opened, which is what fails where
            // OPFS cannot be written (iOS 16.1).
            if (e !== ERRNO_CODES.ENOSPC) throw e;
            T('openRoot: probe write skipped (no space)');
          } finally {
            ArcOpfs.uncapped(probeEntry);
          }
          T('openRoot: probe getSize');
          await access.getSize();
        } finally {
          T('openRoot: probe close');
          await access.close();
        }
        T('openRoot: done');
        return dir;
      } catch (e) {
        if (/memdata=mem/.test(String(e))) fallbacks.splice(0, fallbacks.length - 1);
        for (const [storage, where] of fallbacks) {
          try {
            const fallback = await storage.getDirectory();
            ArcOpfs.storage = storage === globalThis.arcIdbOpfs ? 'idb' : 'memory';
            err('arc-opfs: OPFS cannot write here (' + (e && e.name) + ': ' + e +
                '); /data is ' + where);
            return fallback;
          } catch (e2) {
            err('arc-opfs: ' + where + ': ' + (e2 && e2.name) + ': ' + e2);
          }
        }
        throw e;
      }
    },

    split(path) {
      return path.split('/').filter((c) => c.length > 0);
    },

    async dirAt(components) {
      let dir = await ArcOpfs.root();
      for (const c of components) dir = await dir.getDirectoryHandle(c);
      return dir;
    },

    // Resolves |path| to {kind: 'file'|'directory', handle, parent, name}.
    async lookup(path) {
      const parts = ArcOpfs.split(path);
      if (parts.length === 0) {
        return {kind: 'directory', handle: await ArcOpfs.root(), parent: null,
                name: ''};
      }
      const name = parts.pop();
      const parent = await ArcOpfs.dirAt(parts);
      // Not a file: a directory, or nothing (whatever the browser calls the
      // file lookup's failure: TypeMismatchError in Chrome, may differ in older
      // Safari).
      try {
        return {kind: 'file', handle: await parent.getFileHandle(name),
                parent, name};
      } catch (e) {}
      return {kind: 'directory', handle: await parent.getDirectoryHandle(name),
              parent, name};
    },

    errno(e) {
      switch (e && e.name) {
        case 'NotFoundError': return ERRNO_CODES.ENOENT;
        case 'TypeMismatchError': return ERRNO_CODES.ENOTDIR;
        case 'InvalidModificationError': return ERRNO_CODES.ENOTEMPTY;
        case 'NoModificationAllowedError': return ERRNO_CODES.EBUSY;
        case 'QuotaExceededError': return ERRNO_CODES.ENOSPC;
        case 'NotAllowedError':
        case 'SecurityError': return ERRNO_CODES.EACCES;
        case 'TypeError': return ERRNO_CODES.EINVAL;
      }
      if (typeof e === 'number') return e;
      err('arc-opfs: unexpected ' + (e && e.name) + ': ' + e);
      return ERRNO_CODES.EIO;
    },

    // Reads and writes go through buffers of PIECE bytes, kept for reuse: a
    // fresh one the size of the request (an mmap of a 14 MB APK) is refused
    // by an iPhone's Safari short of memory (RangeError: Out of memory), and
    // the app whose file it was could not start. Where that refuses even a
    // piece, one of PIECE_MIN does. The IndexedDB and in-memory storages
    // take views of the wasm memory as they are (direct()): no piece, and a
    // read-only file is read straight into the wasm memory (readInto).
    PIECE: 1 << 20,
    PIECE_MIN: 64 << 10,
    RO_AHEAD: 512 << 10,
    RO_MIN: 64 << 10,
    RO_RUNS: 3,
    aheadBytes: 0,
    pieces: [],
    takePiece(attempt) {
      const p = ArcOpfs.pieces.pop();
      if (p) return p;
      if (!attempt) {
        try {
          return new Uint8Array(ArcOpfs.PIECE);
        } catch (e) {
          if (!ArcOpfs.isOom(e)) throw e;
        }
      }
      return new Uint8Array(ArcOpfs.PIECE_MIN);
    },
    givePiece(piece) {
      if (piece.length === ArcOpfs.PIECE && ArcOpfs.pieces.length < 4) ArcOpfs.pieces.push(piece);
    },
    direct() {
      return ArcOpfs.storage !== 'opfs';
    },

    // Out of memory for ArrayBuffers (topic opfs-oom-ios,
    // docs/topics/opfs-oom-ios.md). In JavaScriptCore every ArrayBuffer of
    // the web process -- the page's and every worker's -- lives in one range
    // of addresses (the primitive Gigacage), where the shared wasm memory
    // reserves its whole maximum when it is made; past what is left,
    // "RangeError: Out of memory" with the tab far from its memory limit
    // (Subway Surfers on an iPhone 12 Pro: ~50 reads and writes refused at
    // 577 of 1536 MB of wasm memory, then "Failed to load libmain.so").
    // ArrayBuffers that are garbage give their room back when their thread's
    // GC runs, so a refused allocation is tried again (oomRetry): this
    // thread's buffers let go (lowMemory), a pause for the GCs, OOM_TRIES
    // times in all (~3 s); only then ENOMEM, and the log says so once
    // ("arc-opfs: out of memory for file data", which the page shows).
    OOM_TRIES: 8,
    OOM_WAIT_MS: 25,
    oom: {refused: 0, retried: 0, failed: 0, told: 0, toldFailed: false},
    isOom(e) {
      return !!e && (e instanceof RangeError || e.name === 'RangeError') && /memory/i.test(String(e.message || e));
    },
    // Lets this thread's buffers go: spare pieces, read-only runs, the
    // IndexedDB chunk cache.
    lowMemory() {
      const A = ArcOpfs;
      let bytes = 0;
      for (const p of A.pieces) bytes += p.length;
      A.pieces = [];
      for (const h of A.handles.values()) {
        const e = h.entry;
        if (!e.runs) continue;
        for (const r of e.runs) bytes += r.bytes.length;
        e.runs.length = 0;  // (a read in flight holds the same array)
      }
      if (globalThis.arcIdbOpfs && typeof globalThis.arcIdbOpfs.trim === 'function') globalThis.arcIdbOpfs.trim();
      return bytes;
    },
    // fn(attempt), again while it fails for want of memory (above); |what|
    // for the log.
    async oomRetry(what, fn) {
      const A = ArcOpfs;
      for (let attempt = 0; ; attempt++) {
        try {
          const r = await fn(attempt);
          if (attempt) A.oom.retried++;
          return r;
        } catch (e) {
          if (!A.isOom(e)) throw e;
          if (!attempt) A.oom.refused++;
          if (attempt + 1 >= A.OOM_TRIES) {
            A.oom.failed++;
            if (!A.oom.toldFailed) {
              A.oom.toldFailed = true;
              err('arc-opfs: out of memory for file data: ' + what + ' still refused after ' + A.OOM_TRIES +
                  ' tries (' + e + '): ENOMEM. The browser has no room left for ArrayBuffers next to ' +
                  'the wasm memory (docs/topics/opfs-oom-ios.md)');
            }
            throw ERRNO_CODES.ENOMEM;
          }
          const freed = A.lowMemory();
          if (!attempt && A.oom.told < 5) {
            A.oom.told++;
            err('arc-opfs: ' + what + ': ' + e + ': let ' + Math.round(freed / 1048576) +
                ' MB of buffers go, trying again (' + A.oom.refused + ' refused so far, ' +
                A.oom.retried + ' went through on a retry)');
          }
          await new Promise((r) => setTimeout(r, A.OOM_WAIT_MS << attempt));
        }
      }
    },

    // Requests in flight (req -> {op, path, since, step}) and a timer that
    // logs those older than STUCK_MS once, then again every STUCK_MS: which
    // operation on which file a stuck thread waits for, and the browser
    // call it waits in (|step|). A busy loop here would stop the timer too.
    STUCK_MS: 10000,
    pending: new Map(),
    watch: null,
    watchStuck() {
      if (ArcOpfs.watch) return;
      ArcOpfs.watch = setInterval(() => {
        const now = Date.now();
        for (const r of ArcOpfs.pending.values()) {
          if (now - r.since < (r.reported + 1) * ArcOpfs.STUCK_MS) continue;
          r.reported++;
          err('arc-opfs: op ' + r.op + ' ' + r.path + ' waits ' +
              Math.round((now - r.since) / 1000) + ' s in ' + r.step +
              ' (' + ArcOpfs.pending.size + ' in flight, ' + ArcOpfs.live + ' live handles, ' +
              ArcOpfs.storage + ')');
        }
      }, 5000);
    },

    // ?opfsgrow=MB (tests; the page's query, as opfstrace): at the start a
    // file of MB megabytes is written in 16 MB pieces and removed, as an app
    // that writes much -- on an origin short of quota the thread stops in
    // it, marked as any growth (demo/apps/run.html "storage full").
    async growTest(root) {
      const m = typeof self !== 'undefined' && self.location && /[?&]opfsgrow=(\d+)/.exec(self.location.search);
      if (!m) return;
      const fh = await root.getFileHandle('.arc-opfs-grow-test', {create: true});
      const access = await fh.createSyncAccessHandle();
      const entry = {size: await access.getSize()};
      ArcOpfs.capped(entry);
      const piece = new Uint8Array(16 << 20);
      try {
        for (let at = 0; at < +m[1] * 1048576; at += piece.length) {
          await ArcOpfs.growing(entry, '/.arc-opfs-grow-test', at + piece.length, () => access.write(piece, {at}));
          ArcOpfs.setSize(entry, Math.max(entry.size, at + piece.length));
        }
        err('arc-opfs: opfsgrow: ' + m[1] + ' MB written');
      } catch (e) {
        err('arc-opfs: opfsgrow: ' + (e === ERRNO_CODES.ENOSPC ? 'ENOSPC after ' + (entry.size >> 20) + ' MB' :
                                      (e && e.name) + ': ' + e));
      } finally {
        ArcOpfs.uncapped(entry);
        await access.close();
        if (await root.removeEntry('.arc-opfs-grow-test').then(() => true, () => false)) ArcOpfs.released(entry.size);
      }
    },

    // Storage budget (topic opfs-budget; docs/topics/opfs-after-kill.md
    // "Бюджет места"). ?opfsfree=MB (the page's query, as opfstrace): the
    // space the page found free for the origin before the start
    // (demo/apps/run.html, SensesStore.storage()). Against the quota WebKit
    // 7615 (Safari 16) counts the files' sizes and the unused part of each
    // open sync access handle's capacity. A handle's capacity is its file's
    // size when it opens, in the worker, and nothing granted in the network
    // process; a write that ends past it asks for a new capacity (1 MB at
    // least, the next power of two above the end below 256 MB, then the next
    // multiple of 128 MB) less what was granted to that handle -- the whole of
    // it the first time a handle grows its file -- and where the quota has
    // not that much left, it waits for good (above). truncate() asks for
    // nothing, the file's size counts all the same. This thread keeps that
    // count and fails a write or truncate that would go past the free space,
    // less BUDGET_MARGIN for the page's own writes, with ENOSPC instead of
    // calling the browser; the first one goes to the log ("arc-opfs: storage
    // full", run.html says so). Files closed, removed or made smaller give the
    // space back; nothing else of the origin is known here.
    BUDGET_MARGIN: 32 << 20,
    budgetParsed: undefined,  // decided on first use (this object is built at link time)
    // -> null, or {free, grown, told, refused}; only for OPFS (not the
    // IndexedDB or in-memory fallback).
    budgetOf() {
      if (ArcOpfs.budgetParsed === undefined) {
        // (below zero: the origin already over the quota by that much)
        const m = typeof self !== 'undefined' && self.location && /[?&]opfsfree=(-?\d+)/.exec(self.location.search);
        ArcOpfs.budgetParsed = m ? {free: +m[1] * 1048576, grown: 0, told: false, refused: 0} : null;
      }
      return ArcOpfs.storage === 'opfs' ? ArcOpfs.budgetParsed : null;
    },
    // Entries with a live handle (their unused capacity counts).
    cappedEntries: new Set(),
    capped(entry) {
      entry.wcap = entry.size || 0;  // the worker's capacity
      entry.bcap = 0;                // granted by the network process
      ArcOpfs.cappedEntries.add(entry);
    },
    uncapped(entry) {
      ArcOpfs.cappedEntries.delete(entry);
      entry.wcap = entry.bcap = 0;
    },
    // WebKit's FileSystemStorageHandle::requestNewCapacityForSyncAccessHandle.
    capacityFor(n) {
      const MB = 1048576;
      if (n < MB) return MB;
      if (n < 256 * MB) return Math.pow(2, Math.floor(Math.log2(n)) + 1);
      return 128 * MB * (Math.floor(n / (128 * MB)) + 1);
    },
    // Bytes the budget has left (may be below zero).
    spaceLeft(b) {
      let unused = 0;
      for (const e of ArcOpfs.cappedEntries) unused += Math.max(0, e.bcap - (e.size || 0));
      return b.free - b.grown - unused;
    },
    setSize(entry, size) {
      const b = ArcOpfs.budgetOf();
      if (b) b.grown += size - (entry.size || 0);
      entry.size = size;
    },
    released(bytes) {
      const b = ArcOpfs.budgetOf();
      if (b && bytes > 0) b.grown -= bytes;
    },

    // |fn|, a write (or a truncate: |truncate|) that makes |entry|'s file
    // reach |to| bytes: refused with ENOSPC past the storage budget (above),
    // and marked in arc_opfs_stall (above) while it runs if it may ask the
    // browser for space.
    growing(entry, path, to, fn, truncate) {
      const A = ArcOpfs;
      const from = entry.size || 0, bcap = entry.bcap || 0;
      const asks = !truncate && to > (entry.wcap || 0);  // WebKit 7615 asks for a capacity
      const want = asks ? A.capacityFor(to) : 0;
      const b = A.budgetOf();
      if (b) {
        const more = asks ? want - bcap : Math.max(to, bcap) - Math.max(from, bcap);
        const left = more > 0 ? A.spaceLeft(b) - A.BUDGET_MARGIN : 0;
        if (more > 0 && more > left) {
          b.refused++;
          if (!b.told) {
            b.told = true;
            const mb = (n) => n < 1048576 && n > -1048576 ? Math.ceil(n / 1024) + ' KB' :
                              Math.round(n / 104857.6) / 10 + ' MB';
            err('arc-opfs: storage full: ' + path + ' needs ' + mb(more) + ' more of the quota, ' +
                mb(Math.max(0, left)) + ' of the ' + mb(b.free) + ' free at the start left: ENOSPC, ' +
                'without asking the browser (it would not answer; docs/topics/opfs-after-kill.md)');
          }
          throw ERRNO_CODES.ENOSPC;
        }
      }
      const granted = (v) => {
        if (asks) entry.wcap = entry.bcap = want;
        return v;
      };
      if (!(asks || to > from)) return fn();
      const cell = typeof wasmExports == 'object' && wasmExports['arc_opfs_stall'];
      const pathCell = cell && wasmExports['arc_opfs_stall_path'];
      if (cell) {
        const at = Number(cell.value) >>> 0;
        HEAPF64[(at >>> 3) + 1] = Math.max(to - from, want - bcap);
        if (pathCell) stringToUTF8(path || '', Number(pathCell.value) >>> 0, 256);
        HEAPF64[at >>> 3] = Date.now();
      }
      const done = () => { if (cell) HEAPF64[(Number(cell.value) >>> 0) >>> 3] = 0; };
      let r;
      try {
        r = fn();
      } catch (e) {
        done();
        throw e;
      }
      if (r && typeof r.then === 'function') {  // Safari before 16.4: promises
        return r.then((v) => { done(); return granted(v); }, (e) => { done(); throw e; });
      }
      done();
      return granted(r);
    },

    // Field offsets of struct ArcOpfsRequest (wasm32).
    OP: 0, FLAGS: 4, ID: 8, PATH: 12, PATH2: 16, BUF: 20, LEN: 24, RESULT: 28,
    OFFSET: 32, SIZE: 40, MTIME: 48, TYPE: 56, LISTING: 60,

    // Addresses are unsigned (>>> 0): wasm memory grows past 2 GB (Firefox),
    // where a signed i32 is negative and HEAPU8.slice(-n) reads from the end.
    async run(req) {
      req >>>= 0;
      const i32 = (off) => HEAP32[(req + off) >>> 2];
      const ptr = (off) => i32(off) >>> 0;
      const f64 = (off) => HEAPF64[(req + off) >>> 3];
      const setF64 = (off, v) => { HEAPF64[(req + off) >>> 3] = v; };
      const setI32 = (off, v) => { HEAP32[(req + off) >>> 2] = v; };
      const A = ArcOpfs;
      // What this request waits in, for the stuck-request log (watchStuck).
      const flight = A.pending.get(req) || {};
      const step = (what) => { flight.step = what; };
      const op = i32(A.OP);
      const path = () => UTF8ToString(ptr(A.PATH));
      const handleFor = (id) => {
        const h = A.handles.get(id);
        if (!h) throw ERRNO_CODES.EBADF;
        return h.entry;
      };

      switch (op) {
        case 1: {  // OPEN: flags 1=create 2=exclusive 4=truncate
          const flags = i32(A.FLAGS);
          const p = path();
          const parts = A.split(p);
          const name = parts.pop();
          if (name === undefined) throw ERRNO_CODES.EISDIR;
          step('open: directories');
          const parent = await A.dirAt(parts);
          if (flags & 8) {  // read-only: a File, no access handle
            let fh;
            try {
              fh = await parent.getFileHandle(name);
            } catch (e) {
              if (e.name === 'TypeMismatchError') throw ERRNO_CODES.EISDIR;
              throw e;
            }
            const file = await fh.getFile();
            const id = A.nextId++;
            A.handles.set(id, {path: p, entry: {ro: true, file, mtime: file.lastModified / 1000}});
            setF64(A.SIZE, file.size);
            return id;
          }
          if (flags & 2) {
            let exists = true;
            try { await parent.getFileHandle(name); } catch (e) {
              if (e.name === 'TypeMismatchError') throw ERRNO_CODES.EISDIR;
              if (e.name !== 'NotFoundError') throw e;
              exists = false;
            }
            if (exists) throw ERRNO_CODES.EEXIST;
          }
          step('open: getFileHandle, createSyncAccessHandle');
          const entry = await A.entryFor(parent, name, p, !!(flags & 1));
          entry.refs++;
          if (flags & 4) {
            step('open: truncate');
            await A.withAccess(entry, (a) => a.truncate(0));
            A.setSize(entry, 0);
            entry.mtime = Date.now() / 1000;
          }
          const id = A.nextId++;
          A.handles.set(id, {path: p, entry});
          step('open: getSize');
          A.setSize(entry, await A.withAccess(entry, (a) => a.getSize()));
          setF64(A.SIZE, entry.size);
          return id;
        }
        case 2: {  // CLOSE
          const id = i32(A.ID);
          const h = A.handles.get(id);
          if (!h) throw ERRNO_CODES.EBADF;
          A.handles.delete(id);
          if (h.entry.ro) return 0;
          if (--h.entry.refs === 0) {
            h.entry.closed = true;
            if (h.entry.unlinked) {  // its name went while open (A.moveOpen)
              await h.entry.moving;
              await A.closeAccess(h.entry);
              const u = h.entry.unlinked;
              try {
                await u.dir.removeEntry(u.name);
                A.released(h.entry.size);
              } catch (e) {}
              return 0;
            }
            if (A.open.get(h.path) === h.entry) A.open.delete(h.path);
            step('close: flush, close');
            const closing = A.closeAccess(h.entry);
            A.closing.set(h.path, closing);
            try {
              await closing;
            } finally {
              if (A.closing.get(h.path) === closing) A.closing.delete(h.path);
            }
          }
          return 0;
        }
        case 3: {  // READ
          // Through a private buffer (OPFS need not accept views of the
          // shared wasm memory), in pieces (A.piece).
          const e = handleFor(i32(A.ID));
          const len = i32(A.LEN);
          const buf = ptr(A.BUF);
          const at = f64(A.OFFSET);
          if (e.ro && typeof e.file.readInto === 'function') {
            // IndexedDB, in memory: straight into the wasm memory.
            const n = await A.oomRetry('read ' + len + ' of ' + (A.handles.get(i32(A.ID)) || {}).path,
                () => e.file.readInto(HEAPU8.subarray(buf, buf + len), at));
            e.lastEnd = at + n;
            return n;
          }
          if (e.ro) {
            // File.slice, the last RO_RUNS runs kept: each slice is an
            // asynchronous read, and native code reads APK entries in small
            // pieces (an app's assets: thousands of them). A read that goes
            // on from where the last one ended takes RO_AHEAD bytes at a
            // time, one elsewhere what it asks for, at least RO_MIN: every
            // run is a new ArrayBuffer for this thread's GC, and 512 KB for
            // each asset read out of the APK's order was 128 MB in Angry
            // Birds' first 20 s for a 42 MB APK (the tab's peak).
            let done = 0;
            const runs = e.runs || (e.runs = []);
            while (done < len) {
              const pos = at + done;
              let c = null;
              for (let r = 0; r < runs.length; r++) {
                if (pos >= runs[r].at && pos < runs[r].at + runs[r].bytes.length) { c = runs[r]; break; }
              }
              if (!c) {
                const want = Math.max(Math.min(len - done, A.PIECE), pos === e.lastEnd ? A.RO_AHEAD : A.RO_MIN);
                // (short of memory, again without reading ahead)
                c = {at: pos, bytes: new Uint8Array(await A.oomRetry('read ' + want + ' at ' + pos,
                    (attempt) => e.file.slice(pos, pos + (attempt ? Math.min(want, len - done) : want)).arrayBuffer()))};
                if (!c.bytes.length) break;  // the end of the file
                runs.push(c);
                if (runs.length > A.RO_RUNS) runs.shift();
                // Each run is a new ArrayBuffer for this thread's GC: to the
                // log every 32 MB (a steady stream of them grows the tab).
                const before = A.aheadBytes;
                A.aheadBytes += c.bytes.length;
                if ((A.aheadBytes >> 25) != (before >> 25))
                  err('arc-opfs: ' + (A.aheadBytes >> 20) + ' MB read ahead from read-only files on this thread');
              }
              const n = Math.min(len - done, c.at + c.bytes.length - pos);
              HEAPU8.set(c.bytes.subarray(pos - c.at, pos - c.at + n), buf + done);
              done += n;
            }
            e.lastEnd = at + done;
            return done;
          }
          if (A.direct()) {
            step('read ' + len + ' at ' + at);
            return await A.oomRetry('read ' + len + ' at ' + at,
                () => A.withAccess(e, (a) => a.read(HEAPU8.subarray(buf, buf + len), {at})));
          }
          const tmp = await A.oomRetry('a buffer for reading', (attempt) => A.takePiece(attempt));
          try {
            let done = 0;
            while (done < len) {
              const want = Math.min(len - done, tmp.length);
              const part = want === tmp.length ? tmp : tmp.subarray(0, want);
              step('read ' + want + ' at ' + (at + done) + ' (' + done + ' of ' + len + ' done)');
              const n = await A.oomRetry('read ' + want + ' at ' + (at + done),
                  () => A.withAccess(e, (a) => a.read(part, {at: at + done})));
              HEAPU8.set(part.subarray(0, n), buf + done);
              done += n;
              if (n < want) break;  // the end of the file
            }
            return done;
          } finally {
            A.givePiece(tmp);
          }
        }
        case 4: {  // WRITE
          const h = A.handles.get(i32(A.ID));
          const e = handleFor(i32(A.ID));
          if (e.ro) throw ERRNO_CODES.EBADF;
          const len = i32(A.LEN);
          const buf = ptr(A.BUF);
          const at = f64(A.OFFSET);
          if (A.direct()) {
            step('write ' + len + ' at ' + at);
            const n = await A.oomRetry('write ' + len + ' at ' + at + ' of ' + h.path,
                () => A.withAccess(e, (a) => a.write(HEAPU8.subarray(buf, buf + len), {at})));
            if (at + n > (e.size || 0)) A.setSize(e, at + n);
            e.mtime = Date.now() / 1000;
            return n;
          }
          const tmp = await A.oomRetry('a buffer for writing', (attempt) => A.takePiece(attempt));
          try {
            let done = 0;
            while (done < len) {
              const want = Math.min(len - done, tmp.length);
              const part = want === tmp.length ? tmp : tmp.subarray(0, want);
              part.set(HEAPU8.subarray(buf + done, buf + done + want));
              step('write ' + want + ' at ' + (at + done) + ' (' + done + ' of ' + len + ' done)');
              const end = at + done + want;
              let n;
              try {
                n = await A.oomRetry('write ' + want + ' at ' + (at + done) + ' of ' + h.path,
                    () => A.withAccess(e, (a) => A.growing(e, h.path, end, () => a.write(part, {at: at + done}))));
              } catch (x) {
                if (x === ERRNO_CODES.ENOSPC && done > 0) break;  // what was written (storage budget)
                throw x;
              }
              if (at + done + n > (e.size || 0)) A.setSize(e, at + done + n);
              done += n;
              if (n < want) break;
            }
            e.mtime = Date.now() / 1000;
            return done;
          } finally {
            A.givePiece(tmp);
          }
        }
        case 5: {  // GETSIZE
          const e = handleFor(i32(A.ID));
          if (e.ro) {
            setF64(A.SIZE, e.file.size);
            setF64(A.MTIME, e.mtime);
            return 0;
          }
          A.setSize(e, await A.withAccess(e, (a) => a.getSize()));
          setF64(A.SIZE, e.size);
          setF64(A.MTIME, e.mtime);
          return 0;
        }
        case 6: {  // TRUNCATE (open file)
          const h = A.handles.get(i32(A.ID));
          const e = handleFor(i32(A.ID));
          if (e.ro) throw ERRNO_CODES.EBADF;
          const size = f64(A.OFFSET);
          await A.withAccess(e, (a) => A.growing(e, h.path, size, () => a.truncate(size), true));
          A.setSize(e, size);
          e.mtime = Date.now() / 1000;
          return 0;
        }
        case 7: {  // FLUSH
          const e = handleFor(i32(A.ID));
          if (e.access && !e.ro) await e.access.flush();  // a closed one was flushed
          return 0;
        }
        case 8: {  // STAT: type 1=file 2=directory
          const p = path();
          const open = A.open.get(p);
          if (open) {
            setI32(A.TYPE, 1);
            setF64(A.SIZE, await A.withAccess(open, (a) => a.getSize()));
            setF64(A.MTIME, open.mtime);
            return 0;
          }
          step('stat: lookup');
          const r = await A.lookup(p);
          if (r.kind === 'file') {
            const file = await r.handle.getFile();
            setI32(A.TYPE, 1);
            setF64(A.SIZE, file.size);
            setF64(A.MTIME, file.lastModified / 1000);
          } else {
            setI32(A.TYPE, 2);
            setF64(A.SIZE, 4096);
            setF64(A.MTIME, 0);
          }
          return 0;
        }
        case 9: {  // MKDIR
          const parts = A.split(path());
          const name = parts.pop();
          if (name === undefined) throw ERRNO_CODES.EEXIST;
          const parent = await A.dirAt(parts);
          // Exists as a directory, or as a file (the directory lookup fails
          // with something other than NotFoundError then).
          let exists = true;
          try { await parent.getDirectoryHandle(name); } catch (e) {
            if (e.name === 'NotFoundError') exists = false;
          }
          if (exists) throw ERRNO_CODES.EEXIST;
          await parent.getDirectoryHandle(name, {create: true});
          return 0;
        }
        case 10: {  // UNLINK (files only)
          const p = path();
          const r = await A.lookup(p);
          if (r.kind !== 'file') throw ERRNO_CODES.EISDIR;
          const open = A.open.get(p);
          if (open) {  // its descriptors keep it (A.moveOpen)
            step('unlink: moving the open file away');
            open.unlinked = await A.moveOpen(open, p, null, null);
            return 0;
          }
          step('unlink: removeEntry');
          await A.closing.get(p);
          const size = A.budgetOf() ? (await r.handle.getFile()).size : 0;
          await r.parent.removeEntry(r.name);
          A.released(size);
          return 0;
        }
        case 11: {  // RMDIR (empty directories only)
          const r = await A.lookup(path());
          if (r.kind !== 'directory') throw ERRNO_CODES.ENOTDIR;
          if (!r.parent) throw ERRNO_CODES.EBUSY;  // The mount root.
          // Check emptiness ourselves: browsers disagree on the exception
          // (Chrome: InvalidModificationError, Safari: something else).
          for await (const _ of r.handle.keys()) throw ERRNO_CODES.ENOTEMPTY;
          await r.parent.removeEntry(r.name);
          return 0;
        }
        case 12: {  // RENAME
          const from = path();
          const to = UTF8ToString(ptr(A.PATH2));
          // OpfsHandler::rename unlinks an existing |to| first.
          if (A.open.has(to)) throw ERRNO_CODES.EBUSY;
          const open = A.open.get(from);
          if (open) {
            const parts = A.split(to);
            const name = parts.pop();
            const dest = await A.dirAt(parts);
            step('rename: moving the open file');
            await A.moveOpen(open, from, dest, name);
            A.open.set(to, open);
            for (const h of A.handles.values()) if (h.entry === open) h.path = to;
            return 0;
          }
          step('rename: closing handles');
          await A.closing.get(from);
          await A.closing.get(to);
          const r = await A.lookup(from);
          if (!r.parent) throw ERRNO_CODES.EBUSY;
          const parts = A.split(to);
          const name = parts.pop();
          const dest = await A.dirAt(parts);
          if (typeof r.handle.move !== 'function') {
            throw ERRNO_CODES.EXDEV;  // Browser lacks FileSystemHandle.move().
          }
          step('rename: move');
          await r.handle.move(dest, name);
          return 0;
        }
        case 13: {  // LIST: "<type byte><name>\0"... "\0", malloc'ed
          const dir = await A.dirAt(A.split(path()));
          const parts = [];
          for await (const [name, h] of dir.entries()) {
            parts.push((h.kind === 'directory' ? 'd' : 'f') + name);
          }
          const encoded = new TextEncoder().encode(parts.join('\0') + '\0\0');
          const mem = _malloc(encoded.length);
          HEAPU8.set(encoded, mem >>> 0);
          setI32(A.LISTING, mem);
          return parts.length;
        }
      }
      throw ERRNO_CODES.ENOSYS;
    },
  },

  arc_opfs_start__deps: ['$ArcOpfs', 'emscripten_proxy_finish'],
  arc_opfs_start: (ctx, req) => {
    req >>>= 0;
    const path = HEAP32[(req + ArcOpfs.PATH) >>> 2] >>> 0;
    const flight = {op: HEAP32[(req + ArcOpfs.OP) >>> 2], path: path ? UTF8ToString(path) : '',
                    since: Date.now(), step: 'start', reported: 0};
    ArcOpfs.pending.set(req, flight);
    if (ArcOpfs.isTracing() && ArcOpfs.traced++ < 40) ArcOpfs.trace('request op ' + flight.op + ' ' + flight.path);
    ArcOpfs.watchStuck();
    ArcOpfs.run(req).then(
        (result) => { HEAP32[(req + ArcOpfs.RESULT) >>> 2] = result; },
        (e) => {
          // Browser exceptions other than "not found", with the request, for
          // telling browsers' OPFS dialects apart.
          if (typeof e !== 'number' && !(e && e.name === 'NotFoundError')) {
            const path = HEAP32[(req + ArcOpfs.PATH) >>> 2] >>> 0;
            err('arc-opfs: op ' + HEAP32[(req + ArcOpfs.OP) >>> 2] + ' ' +
                (path ? UTF8ToString(path) : '') + ': ' + (e && e.name) + ': ' + e);
          }
          HEAP32[(req + ArcOpfs.RESULT) >>> 2] = -ArcOpfs.errno(e);
        })
      .finally(() => {
        ArcOpfs.pending.delete(req);
        _emscripten_proxy_finish(ctx);
      });
  },

  arc_opfs_available__deps: ['$ArcOpfs'],
  arc_opfs_available: () => {
    ArcOpfs.trace('thread up');
    return (typeof navigator !== 'undefined' && navigator.storage &&
            typeof navigator.storage.getDirectory === 'function') ? 1 : 0;
  },
});
