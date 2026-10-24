// SENSES Web Apps: what installing a file takes, before the page asks the
// user (topic senses-shell; apps of split APKs: topic apks-split,
// docs/topics/apks-split.md). No DOM: the library page uses it, and
// tests/apps/apks_split_test.js under Node with the real store worker.
//
//   SensesInstall.prepare(file, {store, apps, locales, dpi, log, status})
//     -> {info, meta, notes, commit(meta, iconPng, old, onProgress), cancel()}
//   store: SensesStore (or the test's); apps: the installed apps' meta.json;
//   locales: the user's languages; dpi: the VM's screen density;
//   status(text, fraction, hint): what is being done.
//   info: SensesApk.read()'s, of the base APK; meta: what meta.json gets
//   besides the page's own fields (an app of parts: parts, obb, abis, size,
//   bundle); notes: [{level: 'info' | 'warn' | 'bad', text}] for the page.
//
// A plain APK is read in place and copied whole at commit. An
// .apks/.xapk/.apkm: the parts for this device (SensesApk.choose: the base,
// feature modules, one ABI, one density, the languages) are unpacked into
// the library's staging directory, their manifests checked (one app, one
// version, each its own split), moved in at commit as base.apk and
// split_SPLIT.apk, an .xapk's OBB files into obb/.
(function (root) {
  'use strict';

  function plural(n, one, few, many) {
    var t = n % 100, u = n % 10;
    return t > 10 && t < 20 ? many : u === 1 ? one : u >= 2 && u <= 4 ? few : many;
  }
  function size(n) {
    return n >= 1048576 ? (n / 1048576).toFixed(n >= 104857600 ? 0 : 1) + ' МБ' : Math.ceil(n / 1024) + ' КБ';
  }

  function prepare(file, o) {
    var log = o.log || function () {}, status = o.status || function () {};
    return SensesApk.readBundle(file).then(function (b) {
      return b.kind === 'apk' ? prepareApk(file, o) : prepareBundle(file, b, o, log, status);
    });
  }

  function prepareApk(file, o) {
    return SensesApk.read(file, {locales: o.locales}).then(function (info) {
      return {
        info: info, meta: {}, notes: [],
        commit: function (meta, iconPng, old, onProgress) {
          // The same APK again (same version and size) is not rewritten:
          // the VM would take a new file for an update (store-worker.js).
          var sameApk = !!old && !old.meta.parts && old.meta.versionCode === meta.versionCode &&
              old.meta.size === meta.size;
          return o.store.install({meta: meta, apk: file, icon: iconPng, sameApk: sameApk}, onProgress);
        },
        cancel: function () {},
      };
    });
  }

  function prepareBundle(file, b, o, log, status) {
    var store = o.store;
    var langs = (o.locales || []).slice();
    ['ru', 'en'].forEach(function (l) { if (langs.indexOf(l) < 0) langs.push(l); });
    var sel = SensesApk.choose(b, {abis: SensesApk.ABI_ORDER, dpi: o.dpi || 160, locales: langs});
    log('Install: ' + b.kind + ' of ' + b.parts.length + ' parts (' +
        b.parts.map(function (p) { return p.path; }).join(' ') + '), ' + b.obb.length + ' OBB; chosen: ' +
        sel.parts.map(function (p) { return p.path; }).join(' ') + ' (ABI ' + sel.abi + ', density ' +
        sel.density + ')');
    // Parts already in the library (the same CRC and size: an app installed
    // again) are not unpacked again, and stay as they are.
    var have = {};
    (o.apps || []).forEach(function (m) {
      (m.parts || []).forEach(function (p) {
        if (p.crc != null) have[p.crc + ':' + p.size] = 'senses/apps/' + m.package + '/' + p.file;
      });
    });
    var list = sel.parts.map(function (p, i) {
      return {part: p, path: have[p.entry.crc + ':' + p.entry.size] || null, as: 'p' + i + '.part'};
    });
    var obb = b.obb.map(function (x, i) { return {obb: x, as: 'o' + i + '.part'}; });
    var unpack = list.filter(function (x) { return !x.path; }).map(function (x) { return {entry: x.part.entry, as: x.as}; })
        .concat(obb.map(function (x) { return {entry: x.obb.entry, as: x.as}; }));
    var bytes = unpack.reduce(function (n, u) { return n + u.entry.size; }, 0);
    status('Распаковываю ' + list.length + ' ' + plural(list.length, 'часть', 'части', 'частей') +
           (obb.length ? ' и OBB' : '') + '…', 0, size(bytes));
    var cancel = function () { return store.clearStaging().catch(function () {}); };
    return store.stage({file: file, parts: unpack}, function (done, total) {
      status('Распаковываю части…', done / total, size(done) + ' из ' + size(total));
    }).then(function () {
      list.forEach(function (x) { if (!x.path) x.path = store.STAGING + x.as; });
      var splits = list.slice(1), res = [];
      splits.forEach(function (x, i) { if (x.part.kind === 'density' || x.part.kind === 'locale') res.push(i); });
      status('Проверяю части…', null, '');
      return store.inspect({base: list[0].path, splits: splits.map(function (x) { return x.path; }), res: res,
                            locales: o.locales});
    }).then(function (r) {
      var p = checkParts(b, sel, list, obb, r, store, log);
      p.cancel = cancel;
      return p;
    }).catch(function (e) {
      // (the staging directory gone before the page says what went wrong)
      return cancel().then(function () { throw e; });
    });
  }

  function bundleError(text) {
    var e = new Error(text);
    e.code = 'bundle';
    return e;
  }

  // The parts' manifests: one package, one versionCode, each a split of its
  // own; their files in the library: base.apk and split_<split>.apk.
  function checkParts(b, sel, list, obb, r, store, log) {
    var info = r.info, seen = {};
    r.parts.forEach(function (m, i) {
      var name = list[i + 1].part.path.split('/').pop();
      if (m.package !== info.package)
        throw bundleError('Часть ' + name + ' — от другого приложения (' + m.package + ', а основа — ' + info.package + ').');
      if (m.versionCode !== info.versionCode)
        throw bundleError('Часть ' + name + ' другой версии (' + m.versionCode + ', а основа — ' + info.versionCode + ').');
      if (!m.split) throw bundleError('В наборе две основные части (' + name + ') — это не одно приложение.');
      if (!/^[A-Za-z0-9_.-]+$/.test(m.split)) throw bundleError('Странное имя части: «' + m.split + '».');
      if (seen[m.split]) throw bundleError('Часть «' + m.split + '» в наборе дважды.');
      seen[m.split] = true;
    });
    if (b.package && b.package !== info.package)
      log('Install: the archive says ' + b.package + ', its base is ' + info.package);
    var base = list[0].part.entry;
    var parts = [{file: 'base.apk', split: null, kind: 'base', size: base.size, crc: base.crc}];
    var abis = (info.abis || []).slice();
    r.parts.forEach(function (m, i) {
      var x = list[i + 1].part;
      parts.push({file: 'split_' + m.split + '.apk', split: m.split, kind: m.isFeatureSplit ? 'feature' : x.kind,
                  value: x.value, code: m.hasCode, size: x.entry.size, crc: x.entry.crc});
      m.abis.forEach(function (a) { if (abis.indexOf(a) < 0) abis.push(a); });
    });
    // No part for an ABI this device runs (x86 only): its ABIs, for the warnings.
    if (!abis.length && sel.warnings.indexOf('noabi') >= 0) abis = sel.offered.abis.slice();
    var order = SensesApk.ABI_ORDER;
    abis.sort(function (a, c) { return (order.indexOf(a) + 99) % 99 - (order.indexOf(c) + 99) % 99; });
    var total = parts.reduce(function (n, p) { return n + p.size; }, 0) +
        obb.reduce(function (n, x) { return n + x.obb.entry.size; }, 0);
    var notes = [{level: 'info', text: 'Набор ' + b.kind.toUpperCase() + ': ' + parts.length + ' из ' + b.parts.length + ' ' +
                  plural(b.parts.length, 'части', 'частей', 'частей') + ' — ' + SensesApk.partsText({parts: parts}) +
                  (obb.length ? '; OBB: ' + obb.map(function (x) { return x.obb.file; }).join(', ') : '') + '.'}];
    obb.forEach(function (x) {
      if (x.obb.dir !== info.package)
        notes.push({level: 'warn', text: 'OBB ' + x.obb.file + ' лежит в папке ' + x.obb.dir + ', а пакет — ' + info.package + '.'});
    });
    return {
      info: info,
      meta: {abis: abis, size: total, parts: parts,
             obb: obb.map(function (x) { return {file: x.obb.file, size: x.obb.entry.size}; }),
             bundle: {kind: b.kind, parts: b.parts.length, offered: sel.offered}},
      notes: notes,
      commit: function (meta, iconPng) {
        return store.commitBundle({
          meta: meta, icon: iconPng,
          parts: parts.map(function (p, i) { return {from: list[i].path, file: p.file}; }),
          obb: obb.map(function (x) { return {from: store.STAGING + x.as, file: x.obb.file}; }),
        });
      },
    };
  }

  var api = {prepare: prepare};
  root.SensesInstall = api;
  if (typeof module === 'object' && module.exports) module.exports = api;
})(typeof self !== 'undefined' ? self : this);
