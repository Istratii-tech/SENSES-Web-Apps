// An in-memory Origin Private File System (--pre-js):
// - for the Node build of dalvikvm (mk/topic-activity.mk): Node has no
//   navigator.storage, so /data was not mounted there; with this,
//   shim/opfs/library_arc_opfs.js finds an OPFS as in a browser and the same
//   OpfsHandler serves /data;
// - for browsers whose OPFS cannot write (Safari before 16.4 fails to open
//   sync access handles): library_arc_opfs.js falls back to
//   globalThis.arcMemoryOpfs (graphics-web, mk/topic-pwa.mk).
// What is written lives as long as the process (nothing on disk).
//
// Only what library_arc_opfs.js uses: navigator.storage.getDirectory();
// directory handles (getDirectoryHandle, getFileHandle, removeEntry, keys,
// entries, move); file handles (getFile, createSyncAccessHandle, move);
// sync access handles (read, write, getSize, truncate, flush, close); errors
// as DOMExceptions with the names browsers use. Each thread gets its own
// instance, and only the OPFS thread ever calls it.
//
// ARC_NODE_OPFS_DIR=DIR keeps it in a directory of the host instead, as a
// browser keeps OPFS across reloads (topic mem-budget: a VM restarted for
// memory finds its /data again): read at start, files written through at
// flush() and close(), directories, removals and moves at once.
(function () {
  const fail = (name, msg) => { throw new DOMException(msg || name, name); };
  const inNode = typeof process === 'object' && process.versions && process.versions.node;
  const hostDir = (inNode && process.env.ARC_NODE_OPFS_DIR) || '';
  const fs = hostDir ? require('fs') : null;
  const pathMod = hostDir ? require('path') : null;

  class Node {
    constructor(kind, parent, name) {
      this.kind = kind;
      this.parent = parent || null;  // Node of the containing directory
      this.name = name || '';
      this.children = kind === 'directory' ? new Map() : null;
      this.data = kind === 'file' ? new Uint8Array(0) : null;
      this.size = 0;
      this.mtime = Date.now();
      this.locked = false;
    }

    hostPath() {
      return this.parent ? pathMod.join(this.parent.hostPath(), this.name) : hostDir;
    }
  }

  // Write-through to the host directory (ARC_NODE_OPFS_DIR).
  const host = {
    // (The file's time is the node's: a restarted VM sees the same times, as
    // a browser's OPFS keeps them; topic install8's records compare them.)
    file(node) {
      if (!fs) return;
      fs.writeFileSync(node.hostPath(), node.data.subarray(0, node.size));
      fs.utimesSync(node.hostPath(), new Date(), new Date(node.mtime));
    },
    dir(node) {
      if (fs) fs.mkdirSync(node.hostPath(), {recursive: true});
    },
    remove(node) {
      if (fs) fs.rmSync(node.hostPath(), {recursive: true, force: true});
    },
    // (Another thread or process may be renaming or removing files there
    // meanwhile, e.g. SplitInstallTest playing the shell: what is gone by
    // the time it is read is left out, not an uncaught ENOENT.)
    load(node) {
      let entries;
      try {
        entries = fs.readdirSync(node.hostPath(), {withFileTypes: true});
      } catch (e) {
        if (e.code === 'ENOENT') return;
        throw e;
      }
      for (const e of entries) {
        const child = new Node(e.isDirectory() ? 'directory' : 'file', node, e.name);
        if (e.isDirectory()) {
          host.load(child);
        } else {
          try {
            const bytes = fs.readFileSync(child.hostPath());
            child.data = new Uint8Array(bytes.buffer, bytes.byteOffset, bytes.length).slice();
            child.size = bytes.length;
            child.mtime = Math.floor(fs.statSync(child.hostPath()).mtimeMs);
          } catch (err) {
            if (err.code === 'ENOENT') continue;
            throw err;
          }
        }
        node.children.set(e.name, child);
      }
    },
  };

  class Handle {
    constructor(node, parent, name) {
      this.node = node;
      this.parent = parent;  // Node of the containing directory (null for the root).
      this.name = name;
      this.kind = node.kind;
    }

    async move(destDir, newName) {
      if (!this.parent) fail('InvalidModificationError', 'cannot move the root');
      if (this.node.locked) fail('NoModificationAllowedError');
      const dest = destDir.node;
      const old = dest.children.get(newName);
      if (old && old.kind !== this.node.kind) fail('TypeMismatchError');
      if (old && old.kind === 'directory' && old.children.size) fail('InvalidModificationError');
      const from = fs ? this.node.hostPath() : '';
      this.parent.children.delete(this.name);
      dest.children.set(newName, this.node);
      this.parent = dest;
      this.name = newName;
      this.node.parent = dest;
      this.node.name = newName;
      if (fs) fs.renameSync(from, this.node.hostPath());
    }
  }

  class FileHandle extends Handle {
    // size and lastModified, and slice(start, end).arrayBuffer() as a File
    // has (read-only opens of library_arc_opfs.js); readInto(view, at): the
    // bytes from |at| straight into |view| (no new buffer), their count.
    async getFile() {
      const node = this.node;
      const size = node.size;
      return {size, lastModified: node.mtime,
              slice: (start, end) => ({arrayBuffer: async () =>
                  node.data.slice(Math.min(start, size), Math.min(end, size)).buffer}),
              readInto: async (view, at) => {
                const n = Math.max(0, Math.min(view.length, size - at));
                view.set(node.data.subarray(at, at + n));
                return n;
              }};
    }

    async createSyncAccessHandle() {
      if (this.node.locked) fail('NoModificationAllowedError', 'file is locked');
      this.node.locked = true;
      return new SyncAccessHandle(this.node);
    }
  }

  class DirectoryHandle extends Handle {
    child(name, kind, create) {
      const dir = this.node.children;
      let n = dir.get(name);
      if (n && n.kind !== kind) fail('TypeMismatchError', name);
      if (!n) {
        if (!create) fail('NotFoundError', name);
        n = new Node(kind, this.node, name);
        dir.set(name, n);
        this.node.mtime = Date.now();
        if (kind === 'directory') host.dir(n); else host.file(n);
      }
      return n;
    }

    async getDirectoryHandle(name, options) {
      return new DirectoryHandle(this.child(name, 'directory', options && options.create),
                                 this.node, name);
    }

    async getFileHandle(name, options) {
      return new FileHandle(this.child(name, 'file', options && options.create), this.node, name);
    }

    async removeEntry(name) {
      const n = this.node.children.get(name);
      if (!n) fail('NotFoundError', name);
      if (n.kind === 'directory' && n.children.size) fail('InvalidModificationError', 'not empty');
      if (n.locked) fail('NoModificationAllowedError', 'file is locked');
      this.node.children.delete(name);
      host.remove(n);
    }

    async *keys() {
      for (const name of [...this.node.children.keys()]) yield name;
    }

    async *entries() {
      for (const [name, n] of [...this.node.children.entries()]) {
        yield [name, n.kind === 'directory' ? new DirectoryHandle(n, this.node, name)
                                            : new FileHandle(n, this.node, name)];
      }
    }
  }

  class SyncAccessHandle {
    constructor(node) {
      this.node = node;
    }

    read(buffer, options) {
      const at = (options && options.at) || 0;
      const n = Math.max(0, Math.min(buffer.length, this.node.size - at));
      buffer.set(this.node.data.subarray(at, at + n));
      return n;
    }

    write(buffer, options) {
      const at = (options && options.at) || 0;
      this.reserve(at + buffer.length);
      this.node.data.set(buffer, at);
      this.node.size = Math.max(this.node.size, at + buffer.length);
      this.node.mtime = Date.now();
      return buffer.length;
    }

    reserve(size) {
      if (size <= this.node.data.length) return;
      const grown = new Uint8Array(Math.max(size, this.node.data.length * 2, 4096));
      grown.set(this.node.data.subarray(0, this.node.size));
      this.node.data = grown;
    }

    getSize() {
      return this.node.size;
    }

    truncate(size) {
      this.reserve(size);
      if (size < this.node.size) this.node.data.fill(0, size, this.node.size);
      this.node.size = size;
      this.node.mtime = Date.now();
    }

    flush() {
      host.file(this.node);
    }

    close() {
      host.file(this.node);
      this.node.locked = false;
    }
  }

  const rootNode = new Node('directory');
  const root = new DirectoryHandle(rootNode, null, '');
  // The host directory is read when the storage is first opened: only the
  // OPFS thread does, not every pthread that loads this file.
  let loaded = !fs;
  const storage = {getDirectory: async () => {
    if (!loaded) {
      loaded = true;
      fs.mkdirSync(hostDir, {recursive: true});
      host.load(rootNode);
    }
    return root;
  }};
  globalThis.arcMemoryOpfs = storage;
  if (typeof process !== 'object' || !process.versions || !process.versions.node) return;
  if (typeof navigator === 'object' && navigator && navigator.storage &&
      typeof navigator.storage.getDirectory === 'function') return;
  if (typeof navigator === 'object' && navigator) {
    Object.defineProperty(navigator, 'storage', {value: storage, configurable: true});
  } else {
    globalThis.navigator = {storage};
  }
})();
