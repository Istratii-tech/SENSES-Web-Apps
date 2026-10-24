// SENSES Web Apps' app library in the browser's storage, as the VMs read it
// (topic install8, docs/topics/install8.md): senses/apps/PACKAGE/{base.apk,
// meta.json, icon.png}, and each stack's /data in senses/data/{a44,a60,a81,a10}.
// A reference for the shell (demo/apps), used by this directory's test page.
//
// The storage is the one the VM picks (shim/opfs/library_arc_opfs.js): the
// origin's OPFS where a sync access handle can write, else IndexedDB
// (idb_opfs.js: iOS 16.1, whose OPFS cannot open a file for writing) -- the
// same API either way, so the code below serves both. A worker, as sync
// access handles exist only there.
//
// Messages: {id, op, ...} -> {id, result} or {id, error}
//   probe                        -> 'opfs' | 'idb'
//   put {path, bytes}            writes a file (directories made)
//   get {path}                   -> Uint8Array | null
//   list {path}                  -> [{name, kind}]
//   remove {path}                removes a file or a directory tree
importScripts('idb_opfs.js');

let rootPromise = null;
let kind = null;

async function root() {
  if (!rootPromise) {
    rootPromise = (async () => {
      try {
        if (/[?&]memdata=1\b/.test(self.location.search)) throw new Error('memdata=1');
        const dir = await navigator.storage.getDirectory();
        const probe = await dir.getFileHandle('.senses-probe', {create: true});
        const access = await probe.createSyncAccessHandle();
        try {
          access.write(new Uint8Array([1]), {at: 0});
          await access.getSize();
        } finally {
          await access.close();
        }
        await dir.removeEntry('.senses-probe');
        kind = 'opfs';
        return dir;
      } catch (e) {
        kind = 'idb';
        return self.arcIdbOpfs.getDirectory();
      }
    })();
  }
  return rootPromise;
}

function parts(path) {
  return path.split('/').filter((c) => c);
}

async function dirAt(components, create) {
  let dir = await root();
  for (const c of components) dir = await dir.getDirectoryHandle(c, {create});
  return dir;
}

async function put(path, bytes) {
  const p = parts(path);
  const name = p.pop();
  const dir = await dirAt(p, true);
  // Write a temporary file, then move it in place: a reader (a VM) never sees
  // half an APK.
  const tmp = await dir.getFileHandle(name + '.part', {create: true});
  const access = await tmp.createSyncAccessHandle();
  try {
    await access.truncate(0);
    const CHUNK = 1 << 20;
    for (let at = 0; at < bytes.length; at += CHUNK) {
      await access.write(bytes.subarray(at, Math.min(bytes.length, at + CHUNK)), {at});
    }
    await access.flush();
  } finally {
    await access.close();
  }
  try { await dir.removeEntry(name); } catch (e) {}
  await tmp.move(dir, name);
}

async function get(path) {
  const p = parts(path);
  const name = p.pop();
  try {
    const dir = await dirAt(p, false);
    const fh = await dir.getFileHandle(name);
    const access = await fh.createSyncAccessHandle();
    try {
      const out = new Uint8Array(await access.getSize());
      await access.read(out, {at: 0});
      return out;
    } finally {
      await access.close();
    }
  } catch (e) {
    return null;
  }
}

async function list(path) {
  try {
    const dir = await dirAt(parts(path), false);
    const out = [];
    for await (const [name, h] of dir.entries()) out.push({name, kind: h.kind});
    return out;
  } catch (e) {
    return [];
  }
}

// Children first: IndexedDB's directories (idb_opfs.js) have no recursive removal.
async function removeIn(dir, name) {
  let sub = null;
  try { sub = await dir.getDirectoryHandle(name); } catch (e) {}
  if (sub) {
    const names = [];
    for await (const n of sub.keys()) names.push(n);
    for (const n of names) await removeIn(sub, n);
  }
  await dir.removeEntry(name);
}

async function remove(path) {
  const p = parts(path);
  const name = p.pop();
  try {
    await removeIn(await dirAt(p, false), name);
  } catch (e) {
    if (!e || e.name !== 'NotFoundError') throw e;
  }
}

self.onmessage = async (ev) => {
  const m = ev.data;
  try {
    let result;
    switch (m.op) {
      case 'probe': await root(); result = kind; break;
      case 'put': await put(m.path, m.bytes); break;
      case 'get': result = await get(m.path); break;
      case 'list': result = await list(m.path); break;
      case 'remove': await remove(m.path); break;
      default: throw new Error('unknown op ' + m.op);
    }
    self.postMessage({id: m.id, result});
  } catch (e) {
    self.postMessage({id: m.id, error: (e && e.name ? e.name + ': ' : '') + e});
  }
};
