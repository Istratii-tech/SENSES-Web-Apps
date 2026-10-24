// SENSES Web Apps' shell installing apps of split APKs (topic apks-split,
// docs/topics/apks-split.md), under Node: the page's own code
// (demo/apps/apk.js, install.js) and the real store worker
// (demo/apps/store-worker.js) on an in-memory OPFS (shim/opfs/memory_opfs.js).
//
//   node tests/apps/apks_split_test.js [--e2e] [--apks FILE.apks ...]
//
// The archives are made here from SplitInstallTest's app
// (build/apks-split/v1, v2: make apks-split-apks): an .apks as SAI makes it,
// an .xapk with an OBB file, an .apkm, a bundletool .apks (splits/,
// standalones/, toc.pb). Checked: the parts chosen for the VM (base,
// armeabi-v7a, mdpi, ru, the feature), their files in the library, meta.json,
// the label and icon from the parts, an install again (nothing rewritten), an
// update, a plain APK over an app of parts, broken archives, both inflaters.
// Opera GX (arm64 only, minSdk 29; not in git) if it is there, and every
// --apks FILE given. --e2e: the library is kept in a directory
// (ARC_NODE_OPFS_DIR) and ART 8.1 then runs the installed app from it
// (build/art8, SystemServer --senses): its report shows the parts in use.
'use strict';
const fs = require('fs');
const path = require('path');
const os = require('os');
const vm = require('vm');
const {execFileSync, spawnSync} = require('child_process');

const ROOT = path.resolve(__dirname, '../..');
const APPS = path.join(ROOT, 'demo/apps');
const PKG = 'org.arcwasm.splittest';
// A big real .apks (not in git): OPERA_APKS, else in ARC_WORKSPACE's apps8-apks; skipped when absent.
const OPERA = process.env.OPERA_APKS ||
  path.join(process.env.ARC_WORKSPACE || '/nonexistent', 'apps8-apks/Opera_GX_v3.0.4(162)(arm64).apks');
const args = process.argv.slice(2);
const e2e = args.includes('--e2e');
const extra = args.filter((a, i) => args[i - 1] === '--apks');

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

// --- the archives ----------------------------------------------------------------------
const work = fs.mkdtempSync(path.join(os.tmpdir(), 'apks-split-'));
const opfsDir = e2e ? path.join(work, 'opfs') : '';
function zip(out, cwd, files, stored) {
  execFileSync('zip', ['-q', stored ? '-0' : '-6', out].concat(files), {cwd});
}
function v(n) { return path.join(ROOT, 'build/apks-split/v' + n); }
function partsDir(n, layout) {
  // Copies of version n's parts under the names an archive uses.
  const d = path.join(work, 'src-' + n + '-' + Object.keys(layout).length + '-' + Math.random().toString(36).slice(2));
  for (const [from, to] of Object.entries(layout)) {
    fs.mkdirSync(path.dirname(path.join(d, to)), {recursive: true});
    fs.copyFileSync(path.join(v(n), from), path.join(d, to));
  }
  return d;
}
const SAI = {'base.apk': 'base.apk', 'split_config.armeabi_v7a.apk': 'split_config.armeabi_v7a.apk',
             'split_config.mdpi.apk': 'split_config.mdpi.apk', 'split_config.xhdpi.apk': 'split_config.xhdpi.apk',
             'split_config.ru.apk': 'split_config.ru.apk', 'split_feature.apk': 'split_feature.apk'};
function makeSai(n, name) {
  const d = partsDir(n, SAI);
  const out = path.join(work, name || 'v' + n + '.apks');
  zip(out, d, Object.values(SAI));
  return out;
}
function makeXapk(n) {
  const layout = {'base.apk': PKG + '.apk', 'split_config.armeabi_v7a.apk': 'config.armeabi_v7a.apk',
                  'split_config.mdpi.apk': 'config.mdpi.apk', 'split_config.xhdpi.apk': 'config.xhdpi.apk',
                  'split_config.ru.apk': 'config.ru.apk', 'split_feature.apk': 'feature.apk'};
  const d = partsDir(n, layout);
  const obb = 'Android/obb/' + PKG + '/main.' + n + '.' + PKG + '.obb';
  fs.mkdirSync(path.dirname(path.join(d, obb)), {recursive: true});
  fs.writeFileSync(path.join(d, obb), 'obb of version ' + n + '\n'.repeat(1000));
  fs.writeFileSync(path.join(d, 'manifest.json'), JSON.stringify({
    xapk_version: 2, package_name: PKG, name: 'Split test', version_code: String(n), version_name: n + '.0',
    min_sdk_version: '21', split_apks: [
      {file: PKG + '.apk', id: 'base'}, {file: 'config.armeabi_v7a.apk', id: 'config.armeabi_v7a'},
      {file: 'config.mdpi.apk', id: 'config.mdpi'}, {file: 'config.xhdpi.apk', id: 'config.xhdpi'},
      {file: 'config.ru.apk', id: 'config.ru'}, {file: 'feature.apk', id: 'feature'}],
    expansions: [{file: obb, install_location: 'EXTERNAL_STORAGE', install_path: obb}]}));
  const out = path.join(work, 'v' + n + '.xapk');
  zip(out, d, ['manifest.json'].concat(Object.values(layout)));
  zip(out, d, [obb], true);  // OBB files are stored (they are compressed already)
  return out;
}
function makeApkm(n) {
  const d = partsDir(n, SAI);
  fs.writeFileSync(path.join(d, 'info.json'), JSON.stringify({pname: PKG, versioncode: n, app_name: 'Split test'}));
  const out = path.join(work, 'v' + n + '.apkm');
  zip(out, d, ['info.json'].concat(Object.values(SAI)), true);  // stored parts: the other way out
  return out;
}
function makeBundletool(n) {
  const layout = {'base.apk': 'splits/base-master.apk', 'split_config.armeabi_v7a.apk': 'splits/base-armeabi_v7a.apk',
                  'split_config.mdpi.apk': 'splits/base-mdpi.apk', 'split_config.xhdpi.apk': 'splits/base-xhdpi.apk',
                  'split_config.ru.apk': 'splits/base-ru.apk', 'split_feature.apk': 'splits/feature-master.apk'};
  const d = partsDir(n, layout);
  fs.mkdirSync(path.join(d, 'standalones'), {recursive: true});
  fs.copyFileSync(path.join(v(n), 'base.apk'), path.join(d, 'standalones/standalone-armeabi_v7a_mdpi.apk'));
  fs.writeFileSync(path.join(d, 'toc.pb'), Buffer.from([0x0a, 0x00]));
  const out = path.join(work, 'v' + n + '-bundletool.apks');
  zip(out, d, ['toc.pb', 'standalones/standalone-armeabi_v7a_mdpi.apk'].concat(Object.values(layout)));
  return out;
}

// --- the store worker, in this process -------------------------------------------------
if (opfsDir) process.env.ARC_NODE_OPFS_DIR = opfsDir;
globalThis.self = globalThis;
self.location = {search: ''};
require(path.join(ROOT, 'shim/opfs/memory_opfs.js'));
self.importScripts = (f) => vm.runInThisContext(fs.readFileSync(path.join(APPS, f), 'utf8'), {filename: f});
const pending = new Map();
let nextId = 1;
self.postMessage = (m) => {
  const p = pending.get(m.id);
  if (!p) return;
  if (m.progress) { if (p.progress) p.progress(m.progress[0], m.progress[1]); return; }
  pending.delete(m.id);
  if (m.ok) p.resolve(m.result);
  else { const e = new Error(m.error.message); e.name = m.error.name; p.reject(e); }
};
vm.runInThisContext(fs.readFileSync(path.join(APPS, 'store-worker.js'), 'utf8'), {filename: 'store-worker.js'});
const workerOnMessage = self.onmessage;
self.importScripts('apk.js');
self.importScripts('install.js');
const SensesApk = self.SensesApk, SensesInstall = self.SensesInstall;
function call(op, a, progress) {
  return new Promise((resolve, reject) => {
    const id = nextId++;
    pending.set(id, {resolve, reject, progress});
    // As postMessage: a structured clone (the File stays a File).
    workerOnMessage({data: structuredClone({id, op, args: a})});
  });
}
const store = {
  STAGING: 'senses/apps/.staging/',
  init: () => call('init'), list: () => call('list'),
  install: (a, p) => call('install', a, p), stage: (a, p) => call('stage', a, p),
  inspect: (a) => call('inspect', a), commitBundle: (a) => call('commitBundle', a),
  clearStaging: () => call('clearStaging'), uninstall: (p) => call('uninstall', p),
};

// The library's files, from the OPFS root.
async function dirList(p) {
  let d = await navigator.storage.getDirectory();
  for (const c of p.split('/').filter(Boolean)) {
    try { d = await d.getDirectoryHandle(c); } catch (e) { return null; }
  }
  const names = [];
  for await (const n of d.keys()) names.push(n);
  return names.sort();
}
async function fileInfo(p) {
  let d = await navigator.storage.getDirectory();
  const cs = p.split('/').filter(Boolean);
  for (const c of cs.slice(0, -1)) d = await d.getDirectoryHandle(c);
  const f = await (await d.getFileHandle(cs[cs.length - 1])).getFile();
  return {size: f.size, mtime: f.lastModified};
}

// The page's install (index.html's install()), without asking: prepare,
// meta.json's fields, commit.
async function install(file, opts) {
  opts = opts || {};
  const listed = await store.list();
  const old = listed.apps.find((a) => a.meta && opts.pkg && a.meta.package === opts.pkg);
  const prep = await SensesInstall.prepare(new File([fs.readFileSync(file)], path.basename(file)), {
    store, apps: listed.apps.map((a) => a.meta), locales: opts.locales || ['ru', 'en'], dpi: 160,
    log: opts.log || (() => {}), status: () => {}});
  const info = prep.info;
  const prev = listed.apps.find((a) => a.meta.package === info.package);
  const meta = Object.assign({
    package: info.package, label: info.label, versionName: info.versionName, versionCode: info.versionCode,
    minSdk: info.minSdk, targetSdk: info.targetSdk, abis: info.abis, launcher: info.launcher,
    orientation: info.orientation, stack: prev ? prev.meta.stack : 'a81', installedAt: Date.now(), size: fs.statSync(file).size,
  }, prep.meta);
  if (opts.cancel) {
    // While the page asks the user, the library may be listed again: the
    // unpacked parts stay.
    await store.list();
    const st = await dirList('senses/apps/.staging');
    expect(st && st.length, 'staging gone while waiting for the user');
    await prep.cancel();
    return {prep, meta};
  }
  await prep.commit(meta, new Uint8Array([1, 2, 3]), prev || old);
  return {prep, meta, info};
}

const V1_FILES = ['base.apk', 'icon.png', 'meta.json', 'split_config.armeabi_v7a.apk', 'split_config.mdpi.apk',
                  'split_config.ru.apk', 'split_feature.apk'];

(async () => {
  for (const n of [1, 2]) {
    if (!fs.existsSync(path.join(v(n), 'split_feature.apk')))
      throw new Error(v(n) + ' missing: make apks-split-apks');
  }
  await store.init();
  const lib = 'senses/apps/' + PKG;

  await check('.apks (SAI): parts for the VM chosen, unpacked, in the library', async () => {
    const r = await install(makeSai(1));
    eq(PKG, r.meta.package);
    eq(1, r.meta.versionCode);
    eq('Тест частей', r.meta.label, 'label from the ru part');
    eq('image', r.info.icon && r.info.icon.kind, 'icon from the mdpi part');
    eq('res/drawable-mdpi-v4/ic_launcher.png', r.info.icon.path);
    eq(['armeabi-v7a'], r.meta.abis);
    eq(null, r.meta.orientation, 'orientation (MainActivity has no screenOrientation)');
    eq(['base.apk', 'split_config.armeabi_v7a.apk', 'split_config.mdpi.apk', 'split_config.ru.apk',
        'split_feature.apk'], r.meta.parts.map((p) => p.file));
    eq(['base', 'abi', 'density', 'locale', 'feature'], r.meta.parts.map((p) => p.kind));
    eq(true, r.meta.parts[4].code, 'the feature has code');
    eq(V1_FILES, await dirList(lib));
    eq(null, await dirList('senses/apps/.staging'), 'staging left');
    const meta = JSON.parse(new TextDecoder().decode(await call('readBytes', lib + '/meta.json')));
    eq(r.meta.parts, meta.parts);
    eq(fs.statSync(path.join(v(1), 'split_config.mdpi.apk')).size, (await fileInfo(lib + '/split_config.mdpi.apk')).size);
    eq('основа, armeabi-v7a, mdpi, ru, модуль feature', SensesApk.partsText(meta));
    eq('Android 4.4 не умеет приложения из частей (split APK)', SensesApk.stackProblem(meta, 'a44'));
    eq(null, SensesApk.stackProblem(meta, 'a60'));
  });

  await check('the same .apks again: no part rewritten (the VM would take it for an update)', async () => {
    const before = {};
    for (const f of V1_FILES.filter((x) => x.endsWith('.apk'))) before[f] = await fileInfo(lib + '/' + f);
    await new Promise((r) => setTimeout(r, 20));
    let log = '';
    await install(makeSai(1, 'again.apks'), {log: (t) => { log += t + '\n'; }});
    for (const f of Object.keys(before)) eq(before[f], await fileInfo(lib + '/' + f), f);
    eq(V1_FILES, await dirList(lib));
  });

  await check('.xapk (manifest.json, base named after the package, OBB): OBB in obb/', async () => {
    await store.uninstall(PKG);
    const r = await install(makeXapk(1));
    eq('xapk', r.meta.bundle.kind);
    eq(['base.apk', 'split_config.armeabi_v7a.apk', 'split_config.mdpi.apk', 'split_config.ru.apk',
        'split_feature.apk'], r.meta.parts.map((p) => p.file));
    eq([{file: 'main.1.' + PKG + '.obb', size: 'obb of version 1'.length + 1000}], r.meta.obb);
    eq(V1_FILES.concat(['obb']).sort(), await dirList(lib));
    eq(['main.1.' + PKG + '.obb'], await dirList(lib + '/obb'));
  });

  await check('update to version 2 (.apks, no OBB): parts replaced, obb/ gone', async () => {
    const r = await install(makeSai(2));
    eq(2, r.meta.versionCode);
    eq(V1_FILES, await dirList(lib));
    eq(fs.statSync(path.join(v(2), 'base.apk')).size, (await fileInfo(lib + '/base.apk')).size);
  });

  await check('.apkm (info.json, stored parts) and bundletool .apks (splits/, standalones/ skipped)', async () => {
    await store.uninstall(PKG);
    let r = await install(makeApkm(1));
    eq('apkm', r.meta.bundle.kind);
    eq(V1_FILES, await dirList(lib));
    await store.uninstall(PKG);
    r = await install(makeBundletool(1));
    eq('bundletool', r.meta.bundle.kind);
    eq(6, r.meta.bundle.parts, 'standalones/ not counted');
    eq(['base', 'abi', 'density', 'locale', 'feature'], r.meta.parts.map((p) => p.kind));
    eq(V1_FILES, await dirList(lib));
  });

  await check('a plain APK over an app of parts: the parts and OBB removed', async () => {
    await install(makeXapk(1));
    await install(path.join(v(1), 'base.apk'));
    eq(['base.apk', 'icon.png', 'meta.json'], await dirList(lib));
  });

  await check('JS inflate (Safari before 16.4, iOS 16.1): the same result', async () => {
    await store.uninstall(PKG);
    SensesApk.nativeInflate = false;
    try {
      const r = await install(makeXapk(2));
      eq(2, r.meta.versionCode);
      eq(V1_FILES.concat(['obb']).sort(), await dirList(lib));
    } finally {
      SensesApk.nativeInflate = true;
    }
  });

  await check('broken archives: parts of two versions, cut short; staging cleaned', async () => {
    const d = partsDir(1, SAI);
    fs.copyFileSync(path.join(v(2), 'split_config.mdpi.apk'), path.join(d, 'split_config.mdpi.apk'));
    const mixed = path.join(work, 'mixed.apks');
    zip(mixed, d, Object.values(SAI));
    let err = null;
    try { await install(mixed); } catch (e) { err = e; }
    expect(err && /другой версии/.test(err.message), 'mixed versions: ' + err);
    eq(null, await dirList('senses/apps/.staging'), 'staging left after an error');
    const whole = fs.readFileSync(makeSai(1, 'cut.apks'));
    const cut = path.join(work, 'cut2.apks');
    fs.writeFileSync(cut, whole.subarray(0, whole.length - 100));
    err = null;
    try { await install(cut); } catch (e) { err = e; }
    expect(err && err.code, 'cut short: ' + err);
    const nobase = path.join(work, 'nobase.apks');
    zip(nobase, partsDir(1, SAI), ['split_config.mdpi.apk', 'split_config.ru.apk']);
    err = null;
    try { await install(nobase); } catch (e) { err = e; }
    expect(err && /основн|base|частей/.test(err.message), 'no base: ' + err);
  });

  await check('cancelled (the user said no): staging removed, library as it was', async () => {
    const before = await dirList(lib);
    await install(makeSai(1, 'cancel.apks'), {cancel: true});
    eq(before, await dirList(lib));
    eq(null, await dirList('senses/apps/.staging'));
  });

  const others = (fs.existsSync(OPERA) ? [OPERA] : []).concat(extra);
  for (const f of others) {
    await check('real archive ' + path.basename(f), async () => {
      const t = Date.now();
      let log = '';
      const r = await install(f, {log: (s) => { log += s + '\n'; }});
      const m = Object.assign({}, r.meta, {stack: 'a81'});
      console.log('  ' + log.trim());
      console.log('  ' + r.meta.package + ' ' + r.meta.versionName + ' (' + r.meta.versionCode + '), minSdk ' +
                  r.meta.minSdk + ', ABIs ' + r.meta.abis + ', parts: ' + SensesApk.partsText(r.meta) + ', ' +
                  Math.round(r.meta.size / 1048576) + ' MB, ' + (Date.now() - t) + ' ms');
      console.log('  warnings on 8.1: ' + SensesApk.warnings(m).map((w) => w.level + ': ' + w.text).join(' | '));
      console.log('  6.0: ' + SensesApk.stackProblem(m, 'a60') + '; 4.4: ' + SensesApk.stackProblem(m, 'a44'));
      const files = await dirList('senses/apps/' + r.meta.package);
      eq(r.meta.parts.map((p) => p.file).concat(['icon.png', 'meta.json']).concat(r.meta.obb.length ? ['obb'] : []).sort(),
         files);
      if (f === OPERA) {
        eq(['arm64-v8a'], r.meta.abis);
        eq(29, r.meta.minSdk);
        eq(['base.apk', 'split_config.arm64_v8a.apk', 'split_config.mdpi.apk'], r.meta.parts.map((p) => p.file));
        expect(SensesApk.warnings(m).some((w) => /arm64/.test(w.text)), 'no arm64 warning');
        expect(SensesApk.warnings(m).some((w) => /API 29/.test(w.text)), 'no minSdk warning');
        expect(SensesApk.stackProblem(m, 'a60'), 'arm64 code on 6.0');
      }
      if (!e2e) await store.uninstall(r.meta.package);
    });
  }

  // The launcher activity's screenOrientation (topic rotation): the shell
  // gives landscape apps a landscape display on 6.0, 8.1 and 10.
  const ORIENT = [['signedAngryBirds3.2.0.apk', 'landscape'], ['Angry_Birds_v2.1.0.apk', 'landscape'],
                  ['com.kiloo.subwaysurf.apk', 'portrait'], ['T-Mobile_RS_4.4.1_1661.apk', 'portrait'],
                  ['Telegram+X+0.29.1.1817-legacy-armeabi-v7a.apk', null]]
      .map(([f, o]) => [path.join(path.dirname(OPERA), f), o]).filter(([f]) => fs.existsSync(f));
  if (ORIENT.length) {
    await check('orientation of real APKs (' + ORIENT.length + ')', async () => {
      for (const [f, want] of ORIENT) {
        const info = await SensesApk.read(new File([fs.readFileSync(f)], path.basename(f)), {locales: ['en']});
        eq(want, info.orientation, path.basename(f));
      }
    });
  }

  if (e2e) {
    await check('end to end: ART 8.1 runs the app the shell installed (SystemServer --senses)', async () => {
      await store.uninstall(PKG);
      await install(makeXapk(1));
      const art8 = path.join(ROOT, 'build/art8');
      const r = spawnSync(path.join(ROOT, 'tools/run_node.sh'), ['60', 'artvm8.js', 'arcwasm.server.SystemServer',
                          '--senses', PKG], {cwd: art8, encoding: 'utf8', timeout: 120000,
                          env: Object.assign({}, process.env, {ARC_NODE_OPFS_DIR: opfsDir,
                            ARC_DATA_DIR: 'senses/data/a81', ARC_APPS_DIR: 'senses/apps', ARC_APP_OBB: PKG})});
      const out = (r.stdout || '') + (r.stderr || '');
      const lines = out.split('\n').filter((l) => /SENSES:|AppLibrary|splittest/.test(l));
      console.log('  ' + lines.slice(0, 12).join('\n  '));
      expect(/SENSES: app started org\.arcwasm\.splittest/.test(out), 'not started');
      const report = path.join(opfsDir, 'senses/data/a81/data', PKG, 'files/report');
      expect(fs.existsSync(report), 'no report');
      const rep = fs.readFileSync(report, 'utf8');
      console.log('  report: ' + rep.trim().replace(/\n/g, '; '));
      for (const want of ['version=1', 'density=mdpi', 'ru=Привет', 'feature=feature-', 'lib=loaded'])
        expect(rep.includes(want), 'no ' + want);
    });
  }

  console.log('apks_split_test: ' + passed + ' passed, ' + failed + ' failed');
  fs.rmSync(work, {recursive: true, force: true});
  process.exit(failed);
})().catch((e) => {
  console.log('apks_split_test: ' + (e && e.stack || e));
  process.exit(1);
});
