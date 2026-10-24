// Worker of opfsprobe.html (topic opfs-after-kill): see there.
'use strict';
let seq = 0;
const params = new URLSearchParams(self.location.search);
const TIMEOUT = +(params.get('timeout') || 8000);
async function log(line) {
  const text = 'opfsprobe[w] ' + (++seq) + ' ' + line;
  postMessage(text);
  try { await fetch('/__log', {method: 'POST', body: text}); } catch (e) {}
}
// Async |fn| with a time limit; the log line goes out before it starts.
async function step(name, fn) {
  await log('begin ' + name);
  const t0 = performance.now();
  let timer;
  try {
    const r = await Promise.race([
      Promise.resolve().then(fn),
      new Promise((_, no) => { timer = setTimeout(() => no(new Error('TIMEOUT')), TIMEOUT); })]);
    await log('end ' + name + ' ' + Math.round(performance.now() - t0) + ' ms' +
              (r === undefined ? '' : ': ' + (typeof r === 'string' ? r : JSON.stringify(r))));
    return r;
  } catch (e) {
    await log((e && e.message === 'TIMEOUT' ? 'TIMEOUT ' : 'FAIL ') + name + ' ' +
              Math.round(performance.now() - t0) + ' ms: ' + (e && e.name) + ': ' + e);
    return undefined;
  } finally {
    clearTimeout(timer);
  }
}
async function walk(dir, path, out) {
  for await (const [name, h] of dir.entries()) {
    const p = path + '/' + name;
    if (h.kind === 'directory') {
      out.dirs++;
      await walk(h, p, out);
    } else {
      out.files.push(p);
      try { out.bytes += (await h.getFile()).size; } catch (e) { out.errors++; }
    }
  }
  return out;
}
async function fileAt(root, path) {
  const parts = path.split('/').filter(Boolean);
  const name = parts.pop();
  let d = root;
  for (const c of parts) d = await d.getDirectoryHandle(c);
  return d.getFileHandle(name);
}
// The VM's probe (openRoot) on file |name| at the root, |bytes| written.
async function syncProbe(root, name, bytes) {
  const fh = await step(name + ': getFileHandle', () => root.getFileHandle(name, {create: true}));
  if (!fh) return;
  const a = await step(name + ': createSyncAccessHandle', () => fh.createSyncAccessHandle());
  if (!a) return;
  const buf = new Uint8Array(Math.min(bytes, 1 << 20)).fill(7);
  let at = 0;
  await log('begin ' + name + ': write ' + bytes + ' (sync)');
  const t0 = performance.now();
  try {
    while (at < bytes) at += await a.write(buf.subarray(0, Math.min(buf.length, bytes - at)), {at});
    await log('end ' + name + ': write ' + Math.round(performance.now() - t0) + ' ms, ' + at + ' bytes');
  } catch (e) {
    await log('FAIL ' + name + ': write at ' + at + ': ' + e.name + ': ' + e);
  }
  await step(name + ': getSize', async () => await a.getSize());
  await step(name + ': flush', async () => { await a.flush(); });
  await step(name + ': close', async () => { await a.close(); });
  await step(name + ': removeEntry', () => root.removeEntry(name));
}
// Synchronous log line: out even if the next call blocks this thread for good.
function syncLog(line) {
  const text = 'opfsprobe[w] ' + (++seq) + ' ' + line;
  try {
    const x = new XMLHttpRequest();
    x.open('POST', '/__log', false);
    x.send(text);
  } catch (e) {}
}
// ?steps=fill&fill=MB: one file grown to MB megabytes in 16 MB writes (each
// write past the handle's capacity asks the browser for space: WebKit's
// QuotaManager, docs/topics/opfs-after-kill.md), a line every 32 MB; the
// file is removed after.
async function fill(root, mb) {
  const name = params.get('fillname') || '.opfsprobe-fill';
  // &filldir=DIR[/DIR...]: the file in that directory (made if missing),
  // e.g. .arc-unlinked as a VM killed with files unlinked but open leaves it
  let dir = root;
  for (const c of (params.get('filldir') || '').split('/').filter(Boolean)) dir = await dir.getDirectoryHandle(c, {create: true});
  const fh = await dir.getFileHandle(name, {create: true});
  const a = await fh.createSyncAccessHandle();
  const piece = new Uint8Array(16 << 20).fill(9);
  let at = 0;
  const t0 = performance.now();
  try {
    while (at < mb * 1048576) {
      if (at % (32 << 20) === 0) syncLog('fill: writing at ' + (at >> 20) + ' MB, ' + Math.round(performance.now() - t0) + ' ms');
      at += a.write(piece, {at});
    }
    syncLog('fill: ' + (at >> 20) + ' MB written in ' + Math.round(performance.now() - t0) + ' ms');
  } catch (e) {
    syncLog('fill: FAIL at ' + (at >> 20) + ' MB: ' + e.name + ': ' + e);
  }
  a.close();
  if (!params.has('keep')) await step('fill: removeEntry', () => dir.removeEntry(name));
}
// ?steps=lock: this worker ("holder") keeps a sync access handle of
// .opfsprobe-lock open; a second worker ("other") then does to that file
// what a VM does to its files: createSyncAccessHandle, getFile, move,
// removeEntry -- an error, or a call that never returns.
async function lockHolder(root) {
  const fh = await step('holder: getFileHandle', () => root.getFileHandle('.opfsprobe-lock', {create: true}));
  self.held = await step('holder: createSyncAccessHandle', () => fh.createSyncAccessHandle());
  if (self.held) self.held.write(new Uint8Array([1, 2, 3]), {at: 0});
  postMessage({held: !!self.held});
}
async function lockOther(root) {
  const fh = await step('other: getFileHandle', () => root.getFileHandle('.opfsprobe-lock'));
  if (!fh) return;
  const a = await step('other: createSyncAccessHandle of a held file', () => fh.createSyncAccessHandle());
  if (a) await a.close();
  await step('other: getFile', async () => (await fh.getFile()).size);
  if (typeof fh.move === 'function') {
    await step('other: move', () => fh.move('.opfsprobe-lock2'));
    await step('other: move back', () => fh.move('.opfsprobe-lock'));
  }
  await step('other: removeEntry of a held file', () => root.removeEntry('.opfsprobe-lock'));
}
onmessage = async (ev) => {
  const steps = ev.data.steps;
  if (ev.data.role) {
    const root = await navigator.storage.getDirectory();
    if (ev.data.role === 'holder') await lockHolder(root);
    else if (ev.data.role === 'other') { await lockOther(root); await log('other: done'); }
    else if (ev.data.role === 'release') {
      await step('holder: close', async () => { if (self.held) await self.held.close(); });
      await step('holder: removeEntry', () => root.removeEntry('.opfsprobe-lock').catch(() => root.removeEntry('.opfsprobe-lock2')));
      await log('holder: done');
    }
    return;
  }
  const has = (s) => steps.includes(s);
  await log('worker up; crossOriginIsolated ' + self.crossOriginIsolated);
  const root = await step('getDirectory', () => navigator.storage.getDirectory());
  if (!root) { await log('done (no root)'); return; }
  if (has('tree')) {
    await step('root entries', async () => {
      const names = [];
      for await (const [n, h] of root.entries()) names.push(h.kind[0] + ':' + n);
      return names.join(' ');
    });
    await step('walk', async () => {
      const w = await walk(root, '', {dirs: 0, files: [], bytes: 0, errors: 0});
      self.allFiles = w.files;
      return {dirs: w.dirs, files: w.files.length, MB: Math.round(w.bytes / 1048576), errors: w.errors};
    });
    await step('.arc-unlinked', async () => {
      const d = await root.getDirectoryHandle('.arc-unlinked');
      const names = [];
      for await (const n of d.keys()) names.push(n);
      return names.length + ' ' + names.slice(0, 20).join(' ');
    });
  }
  if (has('rm')) {  // ?steps=rm&rm=NAME,NAME: files at the root removed
    for (const n of (params.get('rm') || '').split(',').filter(Boolean)) await step('rm ' + n, () => root.removeEntry(n));
  }
  if (has('fill')) await fill(root, +(params.get('fill') || 1200));
  if (has('probe')) await syncProbe(root, '.opfsprobe-' + Date.now().toString(36), 1);
  if (has('vmprobe')) await syncProbe(root, '.arc-opfs-probe', 1);
  if (has('big')) await syncProbe(root, '.opfsprobe-big', (+(params.get('big') || 64)) << 20);
  if (has('handles') && self.allFiles) {
    // Every file's sync access handle, opened and closed: one still locked
    // by a VM that is gone shows here.
    const files = self.allFiles.filter((f) => !f.startsWith('/senses/apps/'));
    let ok = 0;
    const bad = [];
    for (const f of files) {
      const t0 = performance.now();
      let timer;
      try {
        const fh = await fileAt(root, f);
        const a = await Promise.race([fh.createSyncAccessHandle(),
          new Promise((_, no) => { timer = setTimeout(() => no(new Error('TIMEOUT')), 3000); })]);
        await a.close();
        ok++;
      } catch (e) {
        bad.push(f + ' ' + (e && e.message === 'TIMEOUT' ? 'TIMEOUT' : e.name) + ' ' + Math.round(performance.now() - t0) + ' ms');
        if (bad.length <= 30) await log('handle ' + bad[bad.length - 1]);
      } finally {
        clearTimeout(timer);
      }
    }
    await log('handles: ' + ok + ' ok, ' + bad.length + ' bad of ' + files.length);
  }
  if (has('locks') && navigator.locks) {
    await step('locks.query', async () => {
      const q = await navigator.locks.query();
      return {held: q.held.map((l) => l.name + ':' + l.mode), pending: q.pending.map((l) => l.name)};
    });
  }
  if (has('idb')) {
    await step('idb put 1 MB', () => new Promise((yes, no) => {
      const r = indexedDB.open('opfsprobe', 1);
      r.onupgradeneeded = () => r.result.createObjectStore('s');
      r.onerror = () => no(r.error);
      r.onsuccess = () => {
        const tx = r.result.transaction('s', 'readwrite');
        tx.objectStore('s').put(new Uint8Array(1 << 20), 'k');
        tx.oncomplete = () => { r.result.close(); yes('ok'); };
        tx.onerror = () => no(tx.error);
      };
    }));
  }
  await log('done');
};
