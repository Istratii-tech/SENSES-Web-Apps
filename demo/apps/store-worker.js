// SENSES Web Apps: the library's storage, in a worker (topic senses-shell,
// docs/topics/senses-shell.md, "Storage").
//
// The library is shared with the VM (topic install8, which mounts it read-only
// at /mnt/apps):
//   senses/apps/<package>/base.apk, meta.json (written last), icon.png
//       an app of split APKs (topic apks-split) also split_<split>.apk for
//       each of its parts and an .xapk's OBB files in obb/
//   senses/apps/.staging/        an archive's parts while they are checked
//   senses/data/{a44,a60,a81,a10}/   each stack's /data (the VM's)
// in the origin's OPFS, written through sync access handles (workers only;
// in Safari before 16.4 their methods return promises: every call is
// awaited). Where OPFS cannot write (iOS 16.1: "Invalid platform file
// handle"), or with ?memdata=1, the same paths in IndexedDB through
// shim/opfs/idb_opfs.js, as the VM keeps /data there
// (shim/opfs/library_arc_opfs.js makes the same choice).
//
// Messages: {id, op, args} -> {id, ok, result | error}; {id, progress} while
// a file is written.
'use strict';

const forceIdb = /[?&]memdata=1\b/.test(self.location.search);
let root = null, backend = '';
const PIECE = 1 << 20;  // bytes per read/write: blobs are read in pieces, never whole

async function opfsWritable(dir) {
  const probe = await (await dir.getDirectoryHandle('senses', {create: true}))
      .getFileHandle('.probe', {create: true});
  const h = await probe.createSyncAccessHandle();
  try {
    const n = await h.write(new Uint8Array([1]), {at: 0});
    if (n !== 1) throw new Error('short write');
    await h.flush();
  } finally {
    await h.close();
  }
}

async function init() {
  if (root) return backend;
  const why = [];
  if (!forceIdb && self.navigator.storage && navigator.storage.getDirectory) {
    try {
      const dir = await navigator.storage.getDirectory();
      await opfsWritable(dir);
      root = dir;
      backend = 'opfs';
      return backend;
    } catch (e) {
      why.push('opfs: ' + e);
    }
  } else {
    why.push(forceIdb ? 'memdata=1' : 'no OPFS');
  }
  try {
    importScripts('idb_opfs.js');
    root = await self.arcIdbOpfs.getDirectory();
    backend = 'idb';
    return backend;
  } catch (e) {
    why.push('idb: ' + e);
  }
  throw new Error('no storage: ' + why.join('; '));
}

function parts(path) { return path.split('/').filter(Boolean); }

async function dirOf(comps, create) {
  let d = root;
  for (const c of comps) d = await d.getDirectoryHandle(c, {create});
  return d;
}

async function fileHandle(path, create) {
  const p = parts(path);
  const d = await dirOf(p.slice(0, -1), create);
  return d.getFileHandle(p[p.length - 1], {create});
}

async function exists(path) {
  try {
    const p = parts(path);
    const d = await dirOf(p.slice(0, -1), false);
    try { await d.getFileHandle(p[p.length - 1]); } catch (e) { await d.getDirectoryHandle(p[p.length - 1]); }
    return true;
  } catch (e) {
    return false;
  }
}

// Whole file, through an access handle (the same on both backends).
async function readBytes(path) {
  const h = await (await fileHandle(path, false)).createSyncAccessHandle();
  try {
    const size = await h.getSize();
    const buf = new Uint8Array(size);
    for (let at = 0; at < size;) {
      const n = await h.read(buf.subarray(at, Math.min(size, at + PIECE)), {at});
      if (!n) break;
      at += n;
    }
    return buf;
  } finally {
    await h.close();
  }
}

async function writeBytes(path, bytes, progress) {
  const h = await (await fileHandle(path, true)).createSyncAccessHandle();
  try {
    await h.truncate(0);
    for (let at = 0; at < bytes.length; at += PIECE) {
      const piece = bytes.subarray(at, Math.min(bytes.length, at + PIECE));
      const n = await h.write(piece, {at});
      if (n !== piece.length) throw new DOMException('short write', 'QuotaExceededError');
      if (progress) progress(at + piece.length, bytes.length);
    }
    await h.flush();
  } finally {
    await h.close();
  }
}

// A Blob (the picked File), piece by piece: a 130 MB APK is never in memory.
async function writeBlob(path, blob, id) {
  const h = await (await fileHandle(path, true)).createSyncAccessHandle();
  let last = 0;
  try {
    await h.truncate(0);
    for (let at = 0; at < blob.size; at += PIECE) {
      const piece = new Uint8Array(await blob.slice(at, Math.min(blob.size, at + PIECE)).arrayBuffer());
      const n = await h.write(piece, {at});
      if (n !== piece.length) throw new DOMException('short write', 'QuotaExceededError');
      const now = Date.now();
      if (now - last > 100) {
        last = now;
        self.postMessage({id, progress: [at + piece.length, blob.size]});
      }
    }
    await h.flush();
  } finally {
    await h.close();
  }
}

async function removeTree(dir, name) {
  let child = null;
  try { child = await dir.getDirectoryHandle(name); } catch (e) {}
  if (child) {
    const names = [];
    for await (const n of child.keys()) names.push(n);
    for (const n of names) await removeTree(child, n);
  }
  try {
    await dir.removeEntry(name);
  } catch (e) {
    if (e.name !== 'NotFoundError') throw e;
  }
}

async function remove(path) {
  const p = parts(path);
  let d;
  try { d = await dirOf(p.slice(0, -1), false); } catch (e) { return; }
  await removeTree(d, p[p.length - 1]);
}

const decoder = new TextDecoder(), encoder = new TextEncoder();

// The installed apps: {apps: [{meta, icon: Uint8Array | null}], skipped:
// [text]}. A dir whose meta.json is not there (an install cut short) is
// removed; one whose meta.json cannot be read now (iOS 16.1: IndexedDB
// requests fail while the device is short of memory) is only skipped and
// told: removing it would lose an installed app.
async function list() {
  let apps;
  try { apps = await dirOf(['senses', 'apps'], false); } catch (e) { return {apps: [], skipped: []}; }
  const names = [];
  for await (const n of apps.keys()) names.push(n);
  const out = [], skipped = [];
  // An install cut short (the page closed while an archive was unpacked).
  if (!staging && names.includes('.staging')) await removeTree(apps, '.staging').catch(() => {});
  for (const name of names) {
    if (name.startsWith('.')) continue;
    let meta = null, error = null;
    try {
      meta = JSON.parse(decoder.decode(await readBytes('senses/apps/' + name + '/meta.json')));
    } catch (e) {
      error = e;
    }
    if (!meta || !meta.package) {
      if (error && error.name === 'NotFoundError') {
        await removeTree(apps, name).catch(() => {});
        skipped.push(name + ': unfinished install, removed');
      } else {
        skipped.push(name + ': meta.json unreadable (' + (error ? error.name + ': ' + error.message : 'empty') + '), kept');
      }
      continue;
    }
    let icon = null;
    try { icon = await readBytes('senses/apps/' + name + '/icon.png'); } catch (e) {}
    out.push({meta, icon});
  }
  // Apps on their own origins (topic per-app-origin): only their record here.
  const local = new Set(out.map((a) => a.meta.package));
  let remote = null;
  try { remote = await dirOf(parts(REMOTE), false); } catch (e) {}
  const rnames = [];
  if (remote) for await (const n of remote.keys()) rnames.push(n);
  for (const name of rnames) {
    let meta = null;
    try { meta = JSON.parse(decoder.decode(await readBytes(REMOTE + name + '/meta.json'))); } catch (e) {
      if (e.name === 'NotFoundError') await removeTree(remote, name).catch(() => {});
      else skipped.push(name + ': ' + REMOTE + ' meta.json unreadable (' + e.name + '), kept');
      continue;
    }
    // A new version installed here and not moved there yet: that one, saying
    // where it goes (index.html moves it when it is opened).
    if (local.has(meta.package)) {
      out.find((a) => a.meta.package === meta.package).remote = meta;
      continue;
    }
    let icon = null;
    try { icon = await readBytes(REMOTE + name + '/icon.png'); } catch (e) {}
    out.push({meta, icon});
  }
  return {apps: out, skipped};
}

// One app, in the order the VM side expects (topic install8,
// docs/topics/install8.md "Contract with the shell"): meta.json away; the APK
// written under a name the VM does not read (it reads only *.apk) and moved
// into place, so it never sees half a file; the same APK (same size and
// version) is not written again, as a new file time means an update to the
// VM (its dex cache is rebuilt); the icon; meta.json last.
async function install(id, {meta, apk, icon, sameApk, root}) {
  const dir = (root || 'senses/apps/') + meta.package + '/';
  await remove(dir + 'meta.json');
  if (!(sameApk && await exists(dir + 'base.apk'))) {
    await remove(dir + 'base.apk.part');
    await writeBlob(dir + 'base.apk.part', apk, id);
    await moveInto(dir, 'base.apk.part', 'base.apk');
  }
  // One APK now where there were parts (topic apks-split): the old parts
  // and OBB files would not match it.
  await placeAll(dir, [{from: dir + 'base.apk', file: 'base.apk'}]);
  await remove(dir + 'obb');
  if (icon) await writeBytes(dir + 'icon.png', icon);
  else await remove(dir + 'icon.png');
  await writeBytes(dir + 'meta.json', encoder.encode(JSON.stringify(meta)));
  return true;
}

// FileSystemHandle.move (OPFS; idb_opfs.js has it too), replacing the
// target. Without move (old browsers): the bytes are copied.
async function moveInto(dirPath, from, to) {
  const d = await dirOf(parts(dirPath), true);
  const h = await d.getFileHandle(from);
  if (typeof h.move === 'function') {
    try {
      await d.removeEntry(to).catch(() => {});
      await h.move(d, to);
      return;
    } catch (e) {
      // Some browsers have move() only for directories or not across names.
    }
  }
  await copyFile(dirPath + from, dirPath + to);
  await d.removeEntry(from);
}

// A file's bytes to another file, piece by piece (never whole in memory).
async function copyFile(from, to) {
  const src = await (await fileHandle(from, false)).createSyncAccessHandle();
  try {
    const dst = await (await fileHandle(to, true)).createSyncAccessHandle();
    try {
      await dst.truncate(0);
      const size = await src.getSize(), buf = new Uint8Array(PIECE);
      for (let at = 0; at < size;) {
        const n = await src.read(buf.subarray(0, Math.min(PIECE, size - at)), {at});
        if (!n) break;
        if (await dst.write(buf.subarray(0, n), {at}) !== n) throw new DOMException('short write', 'QuotaExceededError');
        at += n;
      }
      await dst.flush();
    } finally {
      await dst.close();
    }
  } finally {
    await src.close();
  }
}

// --- an app of split APKs (.apks/.xapk/.apkm; topic apks-split) ----------------
// The page reads the archive's directory and chooses the parts
// (SensesApk.readBundle/choose); here they are unpacked into
// senses/apps/.staging/ (stage), read (inspect: the parts' manifests, the
// base's label and icon with the density and language parts' resources) and,
// once the page has checked them, moved into the app's directory (commitBundle).
const STAGING = 'senses/apps/.staging/';
// Unpacking, or unpacked and waiting for the page (the user may be asked
// first): list() leaves the staging directory alone then.
let staging = false;
let apkLoaded = false;
function apkJs() {
  if (!apkLoaded) { importScripts('apk.js'); apkLoaded = true; }
  return self.SensesApk;
}

// {file: the archive, parts: [{entry, as}], obb: [{entry, as}]}: each entry
// unpacked to STAGING + as, its CRC checked. Progress over all their bytes.
async function stage(id, {file, parts: list}) {
  const SensesApk = apkJs();
  staging = true;
  try {
    await remove(STAGING);
    const total = list.reduce((n, p) => n + p.entry.size, 0);
    let done = 0, last = 0;
    for (const p of list) {
      const h = await (await fileHandle(STAGING + p.as, true)).createSyncAccessHandle();
      try {
        await h.truncate(0);
        let at = 0;
        await SensesApk.streamEntry(file, p.entry, async (piece) => {
          const n = await h.write(piece, {at});
          if (n !== piece.length) throw new DOMException('short write', 'QuotaExceededError');
          at += n;
          done += n;
          const now = Date.now();
          if (now - last > 100) {
            last = now;
            self.postMessage({id, progress: [done, total]});
          }
        });
        await h.flush();
      } finally {
        await h.close();
      }
    }
    return true;
  } catch (e) {
    staging = false;
    throw e;
  }
}

async function fileOf(path) {
  return (await fileHandle(path, false)).getFile();
}

// {base: path, splits: [path], res: [index into splits], locales} ->
// {info: the base's (SensesApk.read, with the res parts' resources), parts:
// [each split's manifest (SensesApk.readSplit)]}.
async function inspect({base, splits, res, locales}) {
  const SensesApk = apkJs();
  const files = [];
  for (const p of splits) files.push(await fileOf(p));
  const info = await SensesApk.read(await fileOf(base), {locales, splits: res.map((i) => files[i])});
  const out = [];
  for (const f of files) out.push(await SensesApk.readSplit(f));
  return {info, parts: out};
}

// {meta, icon, parts: [{from, file}], obb: [{from, file}]}: the app's
// directory gets exactly these parts and OBB files. As install(): meta.json
// away first and back last; a part moves in whole (the VM reads only *.apk);
// one that is already there (from == its place: same CRC and size as the
// installed one) stays untouched, as a new file time is an update to the VM.
async function commitBundle({meta, icon, parts: list, obb, root}) {
  const dir = (root || 'senses/apps/') + meta.package + '/';
  await remove(dir + 'meta.json');
  await placeAll(dir, list);
  await placeAll(dir + 'obb/', obb || []);
  if (!(obb || []).length) await remove(dir + 'obb');
  if (icon) await writeBytes(dir + 'icon.png', icon);
  else await remove(dir + 'icon.png');
  await writeBytes(dir + 'meta.json', encoder.encode(JSON.stringify(meta)));
  await remove(STAGING);
  staging = false;
  return true;
}

async function placeAll(dir, list) {
  const keep = new Set(list.map((p) => p.file));
  for (const p of list) {
    if (p.from === dir + p.file) continue;
    await remove(dir + p.file + '.part');
    if (p.from.startsWith(STAGING)) {
      // Moved where it can be (OPFS, idb_opfs.js), else copied.
      const fromDir = await dirOf(parts(STAGING), false);
      const h = await fromDir.getFileHandle(p.from.slice(STAGING.length));
      let moved = false;
      if (typeof h.move === 'function') {
        try {
          await h.move(await dirOf(parts(dir), true), p.file + '.part');
          moved = true;
        } catch (e) {
          // not across directories here
        }
      }
      if (!moved) await copyFile(p.from, dir + p.file + '.part');
    } else {
      await copyFile(p.from, dir + p.file + '.part');  // another app's identical part
    }
    await moveInto(dir, p.file + '.part', p.file);
  }
  // What the old version had and this one has not.
  let d;
  try { d = await dirOf(parts(dir), false); } catch (e) { return; }
  const names = [];
  for await (const n of d.keys()) names.push(n);
  for (const n of names) {
    if (dir.endsWith('/obb/') ? !keep.has(n) : (/\.apk$/.test(n) && !keep.has(n)) || n.endsWith('.part'))
      await removeTree(d, n);
  }
}

async function clearStaging() {
  await remove(STAGING);
  staging = false;
  return true;
}

async function setMeta(meta) {
  await writeBytes((meta.origin ? REMOTE : 'senses/apps/') + meta.package + '/meta.json',
                   encoder.encode(JSON.stringify(meta)));
  return true;
}

const STACKS = ['a44', 'a60', 'a81', 'a10'];
// The app's data in every stack's /data (the VM's layout, install8): its
// data dirs; uninstalling also takes its native libraries and the edited
// copy of the APK some apps get (Firefox: /data/app/PACKAGE). The VM of a
// stack also removes what is left at its next start.
async function clearData(pkg, all) {
  const subs = ['data/' + pkg, 'user_de/0/' + pkg, 'user/0/' + pkg];
  if (all) subs.push('app-lib/' + pkg, 'app/' + pkg);
  for (const s of STACKS) {
    for (const sub of subs) await remove('senses/data/' + s + '/' + sub);
  }
  return true;
}

async function uninstall(pkg) {
  await remove('senses/apps/' + pkg);
  await clearData(pkg, true);
  // (and its dex cache: "mnt@apps@PACKAGE@..."; moved to its own origin, the
  // library's would never be used again)
  for (const s of STACKS) await dropDexCache('senses/data/' + s + '/dalvik-cache/', 'apps@' + pkg + '@');
  return true;
}

async function dropDexCache(dir, key) {
  let d;
  try { d = await dirOf(parts(dir), false); } catch (e) { return; }
  const names = [];
  for await (const [n, h] of d.entries()) names.push([n, h.kind]);
  for (const [n, kind] of names) {
    if (kind === 'directory') await dropDexCache(dir + n + '/', key);
    else if (n.includes(key)) await d.removeEntry(n).catch(() => {});
  }
}

// --- apps on their own origins (topic per-app-origin, docs/topics/per-app-origin.md) ---
// The library keeps a record of each (senses/remote/PACKAGE/: meta.json with
// meta.origin, the app's origin, and icon.png); the app itself, its data and
// its systems live in that origin's storage. Moving an app there: the
// library reads its files (tree, readAt), the app's origin writes them
// (receive), both through origin.html's channel (origins.js).
const REMOTE = 'senses/remote/';

async function remotePut({meta, icon}) {
  const dir = REMOTE + meta.package + '/';
  if (icon) await writeBytes(dir + 'icon.png', icon);
  await writeBytes(dir + 'meta.json', encoder.encode(JSON.stringify(meta)));
  return true;
}
// A new version coming (origins.js send, update): the old APKs out, the
// app's data stays.
async function removeApp(pkg) {
  await remove('senses/apps/' + pkg);
  return true;
}
async function remoteRemove(pkg) {
  await remove(REMOTE + pkg);
  return true;
}

// The files under |prefix| (a directory path ending in '/'): [{path, size}],
// without the paths |skip| matches.
async function tree({prefix, skip}) {
  const out = [], re = skip ? new RegExp(skip) : null;
  let d;
  try { d = await dirOf(parts(prefix), false); } catch (e) { return out; }
  async function walk(dir, path) {
    for await (const [name, h] of dir.entries()) {
      const p = path + name;
      if (re && re.test(p + (h.kind === 'directory' ? '/' : ''))) continue;
      if (h.kind === 'directory') { await walk(h, p + '/'); continue; }
      let size = 0;
      try { size = (await h.getFile()).size; } catch (e) {}
      out.push({path: p, size});
    }
  }
  await walk(d, prefix);
  return out;
}

// Files between apps and the phone (topic app-files, files.js): a folder of a
// system's /data as {name, kind, size, mtime} entries, a file as a File
// (OPFS: read from the disk when used, never copied here; posted to the page),
// a picked File written in (a free name: "a (2).txt"), and removal under a
// system's shared storage (senses/data/STACK/media/) only.
async function listDir({path}) {
  const out = [];
  let d;
  try { d = await dirOf(parts(path), false); } catch (e) { return out; }
  for await (const [name, h] of d.entries()) {
    const e = {name, kind: h.kind, size: 0, mtime: 0};
    if (h.kind === 'file') {
      try { const f = await h.getFile(); e.size = f.size; e.mtime = f.lastModified || 0; } catch (err) {}
    }
    out.push(e);
  }
  return out;
}
async function fileAt({path, name, type}) {
  const f = await (await fileHandle(path, false)).getFile();
  let data = [f];
  if (!(f instanceof Blob)) {
    // (IndexedDB, iOS 16.1: a File's shape only; its bytes, piece by piece)
    data = [];
    for (let at = 0; at < f.size; at += PIECE) {
      data.push(new Blob([await f.slice(at, Math.min(f.size, at + PIECE)).arrayBuffer()]));
    }
  }
  return new File(data, name || parts(path).pop(), {type: type || f.type || '', lastModified: f.lastModified});
}
async function putFile(id, {dir, file}) {
  let name = String(file.name || 'file').replace(/[\\\/\x00-\x1f:]/g, '_').replace(/^\.+/, '') || 'file';
  const dot = name.lastIndexOf('.'), base = dot > 0 ? name.slice(0, dot) : name, ext = dot > 0 ? name.slice(dot) : '';
  for (let n = 2; await exists(dir + name); n++) name = base + ' (' + n + ')' + ext;
  await writeBlob(dir + name, file, id);
  return name;
}
const SHARED = /^senses\/data\/a\d\d\/media\/0\/./;
async function removePath({path}) {
  if (!SHARED.test(path) || parts(path).indexOf('..') >= 0) throw new Error('not in shared storage: ' + path);
  await remove(path);
  return true;
}

// {path, at, len} -> ArrayBuffer (posted, not copied).
async function readAt({path, at, len}) {
  const h = await (await fileHandle(path, false)).createSyncAccessHandle();
  try {
    const size = await h.getSize();
    const buf = new Uint8Array(Math.max(0, Math.min(len, size - at)));
    for (let got = 0; got < buf.length;) {
      const n = await h.read(buf.subarray(got), {at: at + got});
      if (!n) break;
      got += n;
    }
    return buf.buffer;
  } finally {
    await h.close();
  }
}

// The other side: {path, at, bytes, last} piece by piece into PATH.part
// (the VM reads only *.apk, meta.json is sent last), moved into place after
// the last piece. One file at a time, its handle kept open between pieces
// (Safari 16 asks for the file's whole capacity again after each open).
let incoming = null;  // {path, handle}
async function receive({path, at, bytes, last}) {
  if (incoming && (incoming.path !== path || at === 0)) {
    await incoming.handle.close();
    incoming = null;
  }
  if (!incoming) {
    if (at !== 0) throw new Error('receive: ' + path + ' does not start at 0');
    await remove(path + '.part');
    const h = await (await fileHandle(path + '.part', true)).createSyncAccessHandle();
    await h.truncate(0);
    incoming = {path, handle: h};
  }
  const data = new Uint8Array(bytes);
  if (data.length) {
    const n = await incoming.handle.write(data, {at});
    if (n !== data.length) throw new DOMException('short write', 'QuotaExceededError');
  }
  if (last) {
    await incoming.handle.flush();
    await incoming.handle.close();
    incoming = null;
    const p = parts(path);
    await moveInto(p.slice(0, -1).join('/') + '/', p[p.length - 1] + '.part', p[p.length - 1]);
  }
  return true;
}

// Everything of this origin's storage: its files and Cache Storage (an
// app's origin given to another app; the app removed).
async function wipe() {
  if (incoming) { try { await incoming.handle.close(); } catch (e) {} incoming = null; }
  const names = [];
  for await (const n of root.keys()) names.push(n);
  for (const n of names) await removeTree(root, n).catch(() => {});
  if (self.caches) for (const name of await caches.keys()) await dropCache(name);
  return true;
}

// --- Google's services the user installed (topic gms, docs/topics/gms.md) -----------
// Not in the library: per stack, in its /data as senses/data/STACK/google/PACKAGE/
// (the VM's /data/google: privileged system apps). install/commitBundle above
// put them there with root = googleRoot(stack).
const GOOGLE_STACKS = ['a81', 'a10'];
function googleRoot(stack) { return 'senses/data/' + stack + '/google/'; }

async function googleList() {
  const out = [];
  for (const stack of GOOGLE_STACKS) {
    let d;
    try { d = await dirOf(parts(googleRoot(stack)), false); } catch (e) { continue; }
    for await (const name of d.keys()) {
      if (name.startsWith('.')) continue;
      try {
        out.push({stack, meta: JSON.parse(decoder.decode(await readBytes(googleRoot(stack) + name + '/meta.json')))});
      } catch (e) {
        await removeTree(d, name).catch(() => {});  // an install cut short
      }
    }
  }
  return out;
}

// The package and its data in that stack (as uninstall() for an app), and
// the stack's dex cache of its APKs (ART: "data@google@PACKAGE@...").
async function googleRemove({stack, pkg}) {
  if (!GOOGLE_STACKS.includes(stack) || !/^[\w.]+$/.test(pkg)) throw new Error('bad package');
  const data = 'senses/data/' + stack + '/';
  await remove(googleRoot(stack) + pkg);
  for (const sub of ['data/' + pkg, 'user_de/0/' + pkg, 'user/0/' + pkg, 'app-lib/' + pkg]) await remove(data + sub);
  const key = 'data@google@' + pkg + '@';
  async function drop(dir) {
    let d;
    try { d = await dirOf(parts(dir), false); } catch (e) { return; }
    const names = [];
    for await (const [n, h] of d.entries()) names.push([n, h.kind]);
    for (const [n, kind] of names) {
      if (kind === 'directory') await drop(dir + n + '/');
      else if (n.includes(key)) await d.removeEntry(n).catch(() => {});
    }
  }
  await drop(data + 'dalvik-cache/');
  return true;
}

// --- storage space (topic opfs-after-kill, docs/topics/opfs-after-kill.md) -------
// The origin's storage is OPFS (or IndexedDB), Cache Storage (sw.js keeps each
// system it ran: 100-250 MB a system) and IndexedDB, under one quota. Safari
// before 17 gives an origin 1000 MB (estimate() is missing there) and, once
// that is used up, a file that grows through a sync access handle waits for
// an answer that never comes while the page lives: the VM's file thread stops
// for good. So the shell keeps an eye on the space: storage() says what is
// used by what, freeSpace() drops what comes back by itself.
const SAFARI16_QUOTA = 1000 * 1048576;
// What comes back by itself (a path's top): files of a VM's unlinked-but-open
// files it did not live to remove, an archive unpacked halfway, and the
// stacks' caches (ART's dex cache, apps' cache directories).
function regenerable(path) {
  return /^\.arc-unlinked\//.test(path) || /^senses\/apps\/\.staging\//.test(path) ||
         path === '.arc-opfs-grow-test' ||  // run.html ?opfsgrow's file, if its VM stopped
         /^senses\/data\/\w+\/dalvik-cache\//.test(path) ||
         /^senses\/data\/\w+\/(data|user\/0|user_de\/0)\/[^/]+\/(cache|code_cache)\//.test(path);
}
async function walkSizes(dir, path, out) {
  for await (const [name, h] of dir.entries()) {
    const p = path + name;
    if (h.kind === 'directory') {
      await walkSizes(h, p + '/', out);
      continue;
    }
    let size = 0;
    try { size = (await h.getFile()).size; } catch (e) {}
    const m = /^senses\/(apps|data\/(\w+))\//.exec(p);
    if (regenerable(p)) out.regen += size;
    else if (m && m[1] === 'apps') out.apps += size;
    else if (m) out.data[m[2]] = (out.data[m[2]] || 0) + size;
    else out.other += size;
    out.files += size;
  }
  return out;
}
// As sw.js's: entry by entry, then the cache (Safari 16 keeps a deleted
// cache's records in the quota otherwise).
async function dropCache(name) {
  try {
    const cache = await caches.open(name);
    for (const request of await cache.keys()) await cache.delete(request);
  } catch (e) {}
  await caches.delete(name);
}
// Safari 16 counts Cache Storage in the quota by a size it keeps in a file
// and works out again only at the first write of a browser session: caches
// deleted unopened stay in it until then (8463: 398 MB of caches gone from
// sight). One small write here is that first write, so the quota sees what
// is really there.
async function touchCaches() {
  if (!self.caches) return;
  try {
    const cache = await caches.open('senses-space');
    await cache.put(new Request('/__space'), new Response('1'));
    await cache.delete(new Request('/__space'));
  } catch (e) {}
}
// Cache Storage, by cache: {name: bytes}.
async function cacheSizes() {
  const out = {};
  if (!self.caches) return out;
  for (const name of await caches.keys()) {
    const c = await caches.open(name);
    let bytes = 0;
    for (const req of await c.keys()) {
      const r = await c.match(req);
      if (!r) continue;
      const len = +r.headers.get('Content-Length');
      bytes += len > 0 ? len : (await r.blob()).size;
    }
    out[name] = bytes;
  }
  return out;
}
// {files, apps, data: {stack: bytes}, regen, other, caches: {name: bytes},
//  cachesTotal, usage, quota, guessed}: usage and quota from estimate() where
// there is one, else the files and caches counted here and Safari 16's quota.
async function storage({stalled} = {}) {
  // (not while a request for space waits: this write would wait behind it)
  if (!stalled) await touchCaches();
  const out = await walkSizes(root, '', {files: 0, apps: 0, data: {}, regen: 0, other: 0});
  out.caches = await cacheSizes();
  out.cachesTotal = Object.values(out.caches).reduce((a, b) => a + b, 0);
  out.usage = out.files + out.cachesTotal;
  out.quota = SAFARI16_QUOTA;
  out.guessed = true;
  if (self.navigator.storage && navigator.storage.estimate) {
    try {
      const e = await navigator.storage.estimate();
      if (e && e.quota) {
        out.usage = Math.max(out.usage, e.usage || 0);
        out.quota = e.quota;
        out.guessed = false;
      }
    } catch (e) {}
  }
  out.free = Math.max(0, out.quota - out.usage);
  return out;
}
async function removeWhere(dir, path, test) {
  const names = [];
  for await (const [name, h] of dir.entries()) names.push([name, h]);
  for (const [name, h] of names) {
    const p = path + name;
    if (test(p + (h.kind === 'directory' ? '/' : ''))) await removeTree(dir, name).catch(() => {});
    else if (h.kind === 'directory') await removeWhere(h, p + '/', test);
  }
}
// Frees space until |need| bytes are free, in this order, as far as needed:
// files of a VM gone without removing them and a half-unpacked archive; apps'
// cache directories; the systems' caches in Cache Storage other than |keep|'s
// (downloaded again when an app needs them); the dex caches of the stacks
// other than |keep|, then |keep|'s (rebuilt at the next start: slower once).
// {need, keep: stack, force, stalled}: force frees all of it whatever the
// numbers say; stalled: a request for space waits (run.html "storage full"),
// so nothing here may ask for any. -> {before, after: storage(), removed: [what]}
async function freeSpace({need, keep, force, stalled}) {
  const before = await storage({stalled});
  let now = before;
  const removed = [];
  const enough = () => !force && now.free >= need;
  const steps = [
    ['unlinked and staging', () => removeWhere(root, '', (p) => p === '.arc-unlinked/' ? false :
        /^\.arc-unlinked\/./.test(p) || p === '.arc-opfs-grow-test' || (!staging && p === 'senses/apps/.staging/'))],
    ['app caches', () => removeWhere(root, '', (p) => /^senses\/data\/\w+\/(data|user\/0|user_de\/0)\/[^/]+\/(cache|code_cache)\/$/.test(p))],
    ['other systems\' caches', async () => {
      if (!self.caches) return;
      for (const name of await caches.keys()) {
        const m = /^senses-sys-(\w+)-/.exec(name);
        if (m && m[1] !== keep) await dropCache(name);
      }
    }],
    ['other stacks\' dex caches', () => removeWhere(root, '', (p) => {
      const m = /^senses\/data\/(\w+)\/dalvik-cache\/$/.exec(p);
      return !!m && m[1] !== keep;
    })],
    ['dex cache of ' + keep, () => removeWhere(root, '', (p) => p === 'senses/data/' + keep + '/dalvik-cache/')],
  ];
  for (const [what, run] of steps) {
    if (enough()) break;
    await run();
    const after = await storage({stalled});
    if (after.usage < now.usage) removed.push(what + ': ' + Math.round((now.usage - after.usage) / 1048576) + ' MB');
    now = after;
  }
  return {before, after: now, removed};
}

const OPS = {
  googleList: () => googleList(),
  googleRemove: (args) => googleRemove(args),
  init: () => init(),
  list: () => list(),
  install: (args, id) => install(id, args),
  setMeta: (args) => setMeta(args),
  clearData: (args) => clearData(args),
  uninstall: (args) => uninstall(args),
  exists: (args) => exists(args),
  readBytes: (args) => readBytes(args),
  stage: (args, id) => stage(id, args),
  inspect: (args) => inspect(args),
  commitBundle: (args) => commitBundle(args),
  clearStaging: () => clearStaging(),
  storage: (args) => storage(args || {}),
  freeSpace: (args) => freeSpace(args),
  remotePut: (args) => remotePut(args),
  remoteRemove: (args) => remoteRemove(args),
  removeApp: (args) => removeApp(args),
  tree: (args) => tree(args),
  readAt: (args) => readAt(args),
  listDir: (args) => listDir(args),
  fileAt: (args) => fileAt(args),
  putFile: (args, id) => putFile(id, args),
  removePath: (args) => removePath(args),
  receive: (args) => receive(args),
  wipe: () => wipe(),
};

let queue = Promise.resolve();
self.onmessage = (e) => {
  const {id, op, args} = e.data;
  // One operation at a time (access handles are exclusive).
  queue = queue.then(async () => {
    try {
      if (op !== 'init') await init();
      const result = await OPS[op](args, id);
      self.postMessage({id, ok: true, result, backend}, result instanceof ArrayBuffer ? [result] : []);
    } catch (err) {
      self.postMessage({id, ok: false, error: {name: err && err.name, message: String(err && err.message || err)}});
    }
  });
};
