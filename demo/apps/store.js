// SENSES Web Apps: the page's side of the library's storage (store-worker.js
// does the work; topic senses-shell, docs/topics/senses-shell.md).
//   SensesStore.init() -> 'opfs' | 'idb'
//   SensesStore.list() -> {apps: [{meta, icon: Uint8Array | null}], skipped: [text]}
//   SensesStore.install({meta, apk: Blob, icon: Uint8Array}, onProgress(done, total))
//   An app of split APKs (topic apks-split): SensesStore.stage({file, parts:
//   [{entry, as}]}, onProgress) unpacks the chosen parts into
//   senses/apps/.staging/, .inspect({base, splits, res, locales}) reads them,
//   .commitBundle({meta, icon, parts: [{from, file}], obb}) puts them in place,
//   .clearStaging() drops them (STAGING: that directory's path)
//   SensesStore.setMeta(meta), .clearData(pkg), .uninstall(pkg), .get(pkg), .readIcon(pkg)
//   Apps on their own origins (topic per-app-origin): .remotePut({meta, icon}),
//   .remoteRemove(pkg) (the library's record of one), .removeApp(pkg) (its APKs only), .tree({prefix, skip}) ->
//   [{path, size}], .readAt({path, at, len}) -> ArrayBuffer (the library
//   sends an app's files), .receive({path, at, bytes, last}), .wipe() (the
//   app's origin takes them; a whole origin cleared)
//   Files of apps (topic app-files, files.js): .listDir(path) -> [{name, kind,
//   size, mtime}], .fileAt(path, name, type) -> File, .putFile(dir, File,
//   onProgress) -> the name it got in DIR, .removePath(path) (under
//   senses/data/STACK/media/ only)
//   SensesStore.usage() -> {usage, quota} | null, .persist() -> bool
//   Space (topic opfs-after-kill): SensesStore.storage() -> {usage, quota,
//   free, guessed, apps, data: {stack: bytes}, regen, caches: {name: bytes},
//   ...}; .freeSpace({need, keep: stack, force, stalled}) -> {before, after, removed}
//   Google's services the user installs (topic gms): install/commitBundle with
//   root: SensesStore.googleRoot(stack) put a package in that stack's /data;
//   .googleList() -> [{stack, meta}], .googleRemove(stack, pkg)
(function (root) {
  'use strict';
  var worker = null, nextId = 1, pending = {}, backend = '';
  function call(op, args, onProgress, transfer) {
    if (!worker) {
      // ?memdata=1 (the library in IndexedDB, as on iOS 16.1) reaches the
      // worker through its URL.
      var memdata = /[?&]memdata=1\b/.test(location.search) ? '?memdata=1' : '';
      worker = new Worker('store-worker.js' + memdata);
      worker.onmessage = function (e) {
        var m = e.data, p = pending[m.id];
        if (!p) return;
        if (m.progress) { if (p.progress) p.progress(m.progress[0], m.progress[1]); return; }
        delete pending[m.id];
        if (m.backend) backend = m.backend;
        if (m.ok) p.resolve(m.result);
        else {
          var err = new Error(m.error.message);
          err.name = m.error.name || 'Error';
          p.reject(err);
        }
      };
      worker.onerror = function (e) {
        Object.keys(pending).forEach(function (id) { pending[id].reject(new Error('storage worker: ' + (e.message || e))); });
        pending = {};
      };
    }
    return new Promise(function (resolve, reject) {
      var id = nextId++;
      pending[id] = {resolve: resolve, reject: reject, progress: onProgress};
      worker.postMessage({id: id, op: op, args: args}, transfer || []);
    });
  }
  var api = {
    init: function () { return call('init').then(function (b) { backend = b; return b; }); },
    get backend() { return backend; },
    list: function () { return call('list'); },
    install: function (args, onProgress) { return call('install', args, onProgress); },
    STAGING: 'senses/apps/.staging/',
    stage: function (args, onProgress) { return call('stage', args, onProgress); },
    inspect: function (args) { return call('inspect', args); },
    commitBundle: function (args) { return call('commitBundle', args); },
    clearStaging: function () { return call('clearStaging'); },
    storage: function () { return call('storage'); },
    // The worker goes after it: the caches it opened to measure them are let
    // go, which is when Safari frees the deleted ones (store-worker.js dropCache).
    freeSpace: function (args) {
      return call('freeSpace', args).then(function (r) {
        if (worker) { worker.terminate(); worker = null; }
        return r;
      });
    },
    googleRoot: function (stack) { return 'senses/data/' + stack + '/google/'; },
    googleList: function () { return call('googleList'); },
    googleRemove: function (stack, pkg) { return call('googleRemove', {stack: stack, pkg: pkg}); },
    setMeta: function (meta) { return call('setMeta', meta); },
    remotePut: function (args) { return call('remotePut', args); },
    remoteRemove: function (pkg) { return call('remoteRemove', pkg); },
    removeApp: function (pkg) { return call('removeApp', pkg); },
    tree: function (args) { return call('tree', args); },
    readAt: function (args) { return call('readAt', args); },
    // Files of apps (topic app-files, files.js)
    listDir: function (path) { return call('listDir', {path: path}); },
    fileAt: function (path, name, type) { return call('fileAt', {path: path, name: name, type: type}); },
    putFile: function (dir, file, onProgress) { return call('putFile', {dir: dir, file: file}, onProgress); },
    removePath: function (path) { return call('removePath', {path: path}); },
    receive: function (args, transfer) { return call('receive', args, null, transfer); },
    wipe: function () { return call('wipe'); },
    clearData: function (pkg) { return call('clearData', pkg); },
    uninstall: function (pkg) { return call('uninstall', pkg); },
    get: function (pkg) {
      return call('readBytes', 'senses/apps/' + pkg + '/meta.json').then(function (b) {
        return JSON.parse(new TextDecoder().decode(b));
      }, function () { return null; });
    },
    readIcon: function (pkg) {
      return call('readBytes', 'senses/apps/' + pkg + '/icon.png').then(function (b) {
        return URL.createObjectURL(new Blob([b], {type: 'image/png'}));
      }, function () { return null; });
    },
    usage: function () {
      if (!navigator.storage || !navigator.storage.estimate) return Promise.resolve(null);
      return navigator.storage.estimate().catch(function () { return null; });
    },
    persist: function () {
      if (!navigator.storage || !navigator.storage.persist) return Promise.resolve(false);
      return navigator.storage.persisted().then(function (p) {
        return p || navigator.storage.persist();
      }).catch(function () { return false; });
    },
  };
  root.SensesStore = api;
})(self);
