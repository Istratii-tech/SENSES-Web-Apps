// SENSES Web Apps' apps on origins of their own (topic per-app-origin,
// docs/topics/per-app-origin.md), under Node: the library's real store worker
// and an app origin's, each in a context of its own with its own in-memory
// OPFS (shim/opfs/memory_opfs.js), and origins.js's send() between them
// through a stand-in for origin.html's window (its ops, as serve() runs them).
//
//   node tests/apps/origins_test.js
//
// Checked: a move (the origin cleared first, the APK, the app's data on its
// system without caches, the Google services of that system, a file of
// several pieces, meta.json with the way home), the library's record and its
// copy gone, a new version moved into the same origin with the data kept,
// the record removed.
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const ROOT = path.resolve(__dirname, '../..');
const APPS = path.join(ROOT, 'demo/apps');
const APK = path.join(ROOT, 'build/apks-split/v1/base.apk');  // make apks-split-apks

let passed = 0, failed = 0;
async function check(name, fn) {
  try {
    await fn();
    passed++;
    console.log('[ OK ] ' + name);
  } catch (e) {
    failed++;
    console.log('[FAIL] ' + name + ': ' + (e && e.stack || e));
  }
}
function expect(ok, what) { if (!ok) throw new Error(what); }
function eq(want, got, what) {
  const a = JSON.stringify(want), b = JSON.stringify(got);
  if (a !== b) throw new Error((what ? what + ': ' : '') + 'expected ' + a + ', got ' + b);
}

// A store worker in a context of its own (its own OPFS): the page's
// SensesStore over it, as store.js's call() (structured clones both ways).
function storeOf(name) {
  const ctx = vm.createContext({
    console, TextEncoder, TextDecoder, DOMException, structuredClone, setTimeout, clearTimeout, Blob, File, URL,
    process: {env: {}, versions: process.versions},
  });
  ctx.self = ctx;
  ctx.location = {search: ''};
  const pending = new Map();
  let nextId = 1;
  ctx.postMessage = (m) => {
    const p = pending.get(m.id);
    if (!p || m.progress) return;
    pending.delete(m.id);
    m = structuredClone(m);
    if (m.ok) p.resolve(m.result);
    else { const e = new Error(m.error.message); e.name = m.error.name; p.reject(e); }
  };
  ctx.importScripts = (f) => vm.runInContext(fs.readFileSync(path.join(APPS, f), 'utf8'), ctx, {filename: f});
  vm.runInContext(fs.readFileSync(path.join(ROOT, 'shim/opfs/memory_opfs.js'), 'utf8'), ctx, {filename: 'memory_opfs.js'});
  vm.runInContext(fs.readFileSync(path.join(APPS, 'store-worker.js'), 'utf8'), ctx, {filename: 'store-worker.js'});
  const call = (op, args) => new Promise((resolve, reject) => {
    const id = nextId++;
    pending.set(id, {resolve, reject});
    ctx.onmessage({data: structuredClone({id, op, args})});
  });
  const s = {name, ctx, backend: 'opfs'};
  for (const op of ['init', 'list', 'install', 'setMeta', 'clearData', 'uninstall', 'storage', 'remotePut',
                    'remoteRemove', 'removeApp', 'tree', 'readAt', 'receive', 'wipe']) s[op] = (a) => call(op, a);
  s.readIcon = () => Promise.resolve(null);
  s.write = (p, text) => {
    const b = typeof text === 'string' ? new TextEncoder().encode(text) : text;
    return s.receive({path: p, at: 0, bytes: b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength), last: true});
  };
  s.read = (p) => s.readAt({path: p, at: 0, len: 1 << 30}).then((b) => new Uint8Array(b));
  s.text = (p) => s.read(p).then((b) => new TextDecoder().decode(b));
  s.paths = (prefix) => s.tree({prefix: prefix || 'senses/'}).then((t) => t.map((f) => f.path).sort());
  return s;
}

// origins.js, as the library's page has it (localhost:8160).
const page = vm.createContext({
  console, TextEncoder, TextDecoder, URL, URLSearchParams, setTimeout, clearTimeout, Promise,
  location: {pathname: '/', origin: 'http://localhost:8160', protocol: 'http:', hostname: 'localhost', port: '8160',
             host: 'localhost:8160', href: 'http://localhost:8160/'},
  navigator: {}, matchMedia: () => ({matches: false}),
});
page.self = page;
vm.runInContext(fs.readFileSync(path.join(APPS, 'origins.js'), 'utf8'), page, {filename: 'origins.js'});
const O = page.SensesOrigins;

// origin.html's side (origins.js serve()'s ops) on an app origin's store.
function agentFor(store) {
  let home = null;
  const OPS = {
    hello: () => Promise.all([store.list(), store.storage()]).then(([l, st]) =>
        ({origin: 'http://localhost:8162', home, apps: l.apps.map((a) => a.meta), storage: st})),
    wipe: () => store.wipe(),
    setHome: (a) => { home = a.home; return true; },
    receive: (a) => store.receive(a),
    removeApp: (a) => store.removeApp(a.pkg),
  };
  const agent = {
    calls: [],
    call: (op, args) => {
      agent.calls.push(op);
      // (postMessage between windows: a structured clone)
      return Promise.resolve().then(() => OPS[op](structuredClone(args || {}))).then((r) => structuredClone(r));
    },
    home: () => home,
  };
  return agent;
}

(async function () {
  if (!fs.existsSync(APK)) {
    console.log('origins_test: ' + APK + ' missing: make apks-split-apks');
    process.exit(2);
  }
  const lib = storeOf('library'), app = storeOf('app');
  await lib.init();
  await app.init();
  const PKG = 'org.arcwasm.splittest', D = 'senses/data/a10/';
  let meta;

  await check('the library installs an app and it runs there', async () => {
    meta = {package: PKG, label: 'Split test', versionCode: 1, versionName: '1.0', minSdk: 21, stack: 'a10',
            installedAt: Date.now(), size: fs.statSync(APK).size};
    await lib.install({meta, apk: new File([fs.readFileSync(APK)], 'base.apk'), icon: new Uint8Array([1, 2, 3])});
    // what the VM wrote while it ran: its data (with caches), the stack's
    // Google services, another app's data, a file of several pieces
    await lib.write(D + 'data/' + PKG + '/shared_prefs/prefs.xml', '<map>1</map>');
    await lib.write(D + 'data/' + PKG + '/cache/junk', 'cache');
    await lib.write(D + 'data/' + PKG + '/code_cache/junk', 'code cache');
    await lib.write(D + 'user_de/0/' + PKG + '/files/de', 'device protected');
    const big = new Uint8Array(9 * 1048576 + 123);
    for (let i = 0; i < big.length; i += 4096) big[i] = (i / 4096) & 255;
    await lib.write(D + 'data/' + PKG + '/files/big.bin', big);
    await lib.write(D + 'google/com.google.android.gms/base.apk', 'microG');
    await lib.write(D + 'data/other.app/shared_prefs/x.xml', 'other');
    await lib.write('senses/data/a81/data/' + PKG + '/shared_prefs/old.xml', 'on 8.1 before');
    eq(1, (await lib.list()).apps.length, 'apps');
  });

  await check('the app origin had something else (a slot used before)', async () => {
    await app.write('senses/apps/old.app/base.apk', 'old');
    await app.write('senses/data/a81/data/old.app/x', 'old data');
  });

  let agent;
  await check('move: send()', async () => {
    agent = agentFor(app);
    const steps = [];
    const r = await O.send(agent, lib, meta, (done, total) => steps.push([done, total]));
    eq('wipe', agent.calls[0], 'first call');
    eq('http://localhost:8160/', agent.home(), 'home');
    eq(6, r.files, 'files sent (APK, icon, prefs, big file, user_de file, microG; meta.json after)');
    expect(steps.length > 3 && steps[steps.length - 1][0] === steps[steps.length - 1][1], 'progress');
  });

  await check('the app origin has the app, its data without caches, Google services; nothing old', async () => {
    eq(['senses/apps/' + PKG + '/base.apk', 'senses/apps/' + PKG + '/icon.png', 'senses/apps/' + PKG + '/meta.json',
        D + 'data/' + PKG + '/files/big.bin', D + 'data/' + PKG + '/shared_prefs/prefs.xml',
        D + 'google/com.google.android.gms/base.apk', D + 'user_de/0/' + PKG + '/files/de'].sort(),
       (await app.paths()).filter((p) => !/\.probe$/.test(p)), 'files');
    eq(fs.readFileSync(APK).length, (await app.read('senses/apps/' + PKG + '/base.apk')).length, 'APK size');
    expect(Buffer.from(await app.read('senses/apps/' + PKG + '/base.apk')).equals(fs.readFileSync(APK)), 'APK bytes');
    const big = await app.read(D + 'data/' + PKG + '/files/big.bin');
    eq(9 * 1048576 + 123, big.length, 'big file');
    expect(big[4096 * 2000] === (2000 & 255) && big[4096 * 2304] === (2304 & 255), 'big file bytes');
    const m = JSON.parse(await app.text('senses/apps/' + PKG + '/meta.json'));
    eq('http://localhost:8160/', m.home, 'meta.home');
    expect(!('origin' in m), 'meta.origin on the app origin');
    eq([PKG], (await app.list()).apps.map((a) => a.meta.package), 'its library');
  });

  await check('the library keeps a record, its copy and the app\'s data go', async () => {
    await lib.remotePut({meta: Object.assign({}, meta, {origin: 'http://localhost:8162/'}), icon: new Uint8Array([1, 2, 3])});
    await lib.uninstall(PKG);
    const l = await lib.list();
    eq([[PKG, 'http://localhost:8162/']], l.apps.map((a) => [a.meta.package, a.meta.origin]), 'list');
    expect(!l.apps[0].remote, 'not an update');
    eq([D + 'data/other.app/shared_prefs/x.xml', D + 'google/com.google.android.gms/base.apk',
        'senses/remote/' + PKG + '/icon.png', 'senses/remote/' + PKG + '/meta.json'],
       (await lib.paths()).filter((p) => !/\.probe$/.test(p)), 'library files');
    // the card's system switch: the record
    await lib.setMeta(Object.assign({}, l.apps[0].meta, {stack: 'a81'}));
    eq('a81', (await lib.list()).apps[0].meta.stack, 'record changed');
    await lib.setMeta(Object.assign({}, l.apps[0].meta, {stack: 'a10'}));
  });

  await check('a new version: installed here, listed with where it goes, moved into the same origin', async () => {
    const meta2 = Object.assign({}, meta, {versionCode: 2, versionName: '2.0', installedAt: Date.now()});
    await lib.install({meta: meta2, apk: new File([fs.readFileSync(APK)], 'base.apk'), icon: new Uint8Array([4, 5])});
    const l = await lib.list();
    eq(1, l.apps.length, 'one tile');
    eq(2, l.apps[0].meta.versionCode, 'the new one');
    eq('http://localhost:8162/', l.apps[0].remote && l.apps[0].remote.origin, 'remote');
    const a2 = agentFor(app);
    await O.send(a2, lib, l.apps[0].meta, null, {update: true});
    eq('removeApp', a2.calls[0], 'no wipe');
    expect(!a2.calls.includes('wipe'), 'wiped');
    eq('<map>1</map>', await app.text(D + 'data/' + PKG + '/shared_prefs/prefs.xml'), 'data kept');
    eq(2, JSON.parse(await app.text('senses/apps/' + PKG + '/meta.json')).versionCode, 'new meta');
    eq([4, 5], Array.from(await app.read('senses/apps/' + PKG + '/icon.png')), 'new icon');
    await lib.remotePut({meta: Object.assign({}, l.apps[0].meta, {origin: 'http://localhost:8162/'})});
    await lib.uninstall(PKG);
    const after = await lib.list();
    eq([[2, 'http://localhost:8162/', null]], after.apps.map((a) => [a.meta.versionCode, a.meta.origin, a.remote || null]), 'after');
  });

  await check('removed: the origin wiped, the record gone', async () => {
    await app.wipe();
    eq([], await app.paths(), 'app origin');
    await lib.remoteRemove(PKG);
    eq([], (await lib.list()).apps, 'library');
  });

  await check('origins.json: ports and hosts', async () => {
    const ctx = page;
    ctx.fetch = (u) => Promise.resolve({ok: true, json: () => Promise.resolve({ports: [8160, 8162, 8163]})});
    const cfg = await vm.runInContext('SensesOrigins.config()', ctx);
    eq(['http://localhost:8162/', 'http://localhost:8163/'], cfg.slots, 'ports (not its own)');
    eq('http://localhost:8163/', O.freeSlot(cfg, [{origin: 'http://localhost:8162/'}, {}]), 'free slot');
    expect(O.trusted('http://localhost:8169', cfg) && !O.trusted('http://evil.example', cfg) &&
           !O.trusted('https://localhost:8162', cfg), 'trusted');
    eq('on', O.mode(new URLSearchParams('origins=1')), 'mode');
  });

  console.log('origins_test: ' + passed + ' passed, ' + failed + ' failed');
  process.exit(failed ? 1 : 0);
})();
