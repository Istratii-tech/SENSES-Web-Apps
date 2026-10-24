// arc-wasm: what SENSES Web Apps' shell reads from an APK or an archive of
// parts (.apks/.xapk/.apkm), for tools/app_sweep.py (topic apps-sweep): the
// same parser (demo/apps/apk.js), so the sweep picks the stack and the parts
// as the shell does.
//
//   node tools/app_sweep_meta.js FILE   -> one line of JSON on stdout:
//   {kind: "apk"|"apks"|..., package, versionCode, versionName, minSdk,
//    targetSdk, abis, launcher, label, warnings, stack, stackProblems,
//    parts: [{path, split, kind, abis, size}] (bundles: the chosen parts),
//    obb: [{path, file}]}
// or {error: CODE, message} (exit 0 either way).
'use strict';
const fs = require('fs');
const path = require('path');
const SensesApk = require(path.join(process.env.ARC_ROOT || path.join(__dirname, '..'), 'demo', 'apps', 'apk.js'));

async function blobOf(file) {
  return fs.openAsBlob ? fs.openAsBlob(file) : new Blob([fs.readFileSync(file)]);
}

async function main(file) {
  const blob = await blobOf(file);
  const bundle = await SensesApk.readBundle(blob);
  const out = {kind: bundle.kind, parts: [], obb: []};
  let meta;
  if (bundle.kind === 'apk') {
    meta = await SensesApk.read(blob);
  } else {
    const chosen = SensesApk.choose(bundle);
    const blobs = [];
    for (const p of chosen.parts) {
      const bytes = await SensesApk.readEntry(blob, p.entry);
      const b = new Blob([bytes]);
      const m = await SensesApk.readSplit(b);
      blobs.push({p, b, m});
    }
    const base = blobs.find((x) => x.p.kind === 'base');
    meta = await SensesApk.read(base.b, {splits: blobs.filter((x) => x !== base).map((x) => x.b)});
    const abis = new Set(meta.abis);
    for (const x of blobs) x.m.abis.forEach((a) => abis.add(a));
    meta.abis = [...abis];
    out.parts = blobs.map((x) => ({path: x.p.path, split: x.m.split, kind: x.p.kind,
                                   abis: x.m.abis, size: x.b.size}));
    out.obb = bundle.obb.map((o) => ({path: o.path, file: o.file}));
  }
  delete meta.icon;
  Object.assign(out, meta);
  out.stack = SensesApk.defaultStack(meta);
  out.stackProblems = {};
  for (const s of ['a10', 'a81', 'a60', 'a44']) {
    const why = SensesApk.stackProblem(Object.assign({parts: out.parts}, meta), s);
    if (why) out.stackProblems[s] = why;
  }
  return out;
}

main(process.argv[2]).then((o) => {
  process.stdout.write(JSON.stringify(o) + '\n');
}, (e) => {
  process.stdout.write(JSON.stringify({error: e.code || 'error', message: String(e.message || e)}) + '\n');
});
