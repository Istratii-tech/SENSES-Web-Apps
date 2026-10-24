// tools/jitmem worker: instantiates N translation-shaped functions the way
// the given mode says and calls each |calls| times (a browser Worker, or
// Node's worker_threads through run_node.js).
//   each    a module per function, compiled by this thread (shim/ndk/cpu/jit.cc today)
//   batch   modules of K functions, compiled by this thread
//   shared  modules compiled once on the page, sent here (WebAssembly.Module
//           by postMessage), only instantiated here; K functions each
//           (inst=0: kept, not instantiated)
(function () {
  var isNode = typeof process !== 'undefined' && process.versions && process.versions.node;
  var port, post;
  if (isNode) {
    var wt = require('worker_threads');
    port = wt.parentPort;
    post = function (m) { port.postMessage(m); };
    require(require('path').join(__dirname, 'gen.js'));
  } else {
    importScripts('gen.js');
    post = function (m) { postMessage(m); };
  }
  function onmsg(msg) {
    var t0 = Date.now();
    var mem = msg.memory, n = msg.n, k = msg.k || 1, ops = msg.ops, calls = msg.calls;
    var helper = function () {};
    var imports = {e: {m: mem, x: helper}};
    var table = new WebAssembly.Table({initial: 16, element: 'anyfunc'});
    var tbl = !!msg.tbl, start = table.length, nfns = 0;
    var fns = [];
    var compileMs = 0, all, off, scratch;
    function addInstance(mod, count) {
      if (tbl) {
        table.grow(count);
        new WebAssembly.Instance(mod, {e: {m: mem, x: helper, t: table, o: new WebAssembly.Global({value: 'i32'}, start + nfns)}});
      } else {
        var inst = new WebAssembly.Instance(mod, imports);
        for (var i = 0; i < count; i++) fns.push(inst.exports['f' + i]);
      }
      nfns += count;
    }
    if (msg.mode === 'shared' && msg.inst === 0) {
      // inst=0: the modules only received and kept, as a worker keeps the
      // page's catalog of packs (shim/ndk/jit_catalog.js, topic arm-chains).
      kept = msg.modules;
    } else if (msg.mode === 'shared') {
      for (var i = 0; i < msg.modules.length; i++) addInstance(msg.modules[i], Math.min(k, n - i * k));
    } else if (msg.mode !== 'none') {
      // As ndk_jit_instantiate: the bytes are in shared memory (msg.bytes,
      // msg.offsets), copied into one kept scratch buffer and compiled.
      all = new Uint8Array(msg.bytes); off = msg.offsets;
      scratch = new Uint8Array(1 << 20);
      for (var j = 0, first = 0; first < n; first += k, j++) {
        var cnt = Math.min(k, n - first);
        var c0 = Date.now();
        var code = scratch.subarray(0, off[j + 1] - off[j]);
        code.set(all.subarray(off[j], off[j + 1]));
        var mod = new WebAssembly.Module(code);
        compileMs += Date.now() - c0;
        addInstance(mod, cnt);
      }
    }
    if (!tbl) {
      table.grow(fns.length);
      for (i = 0; i < fns.length; i++) table.set(start + i, fns[i]);
      fns = null;
    }
    var t1 = Date.now();
    var run = new WebAssembly.Instance(new WebAssembly.Module(jitmemDriver()), {e: {t: table}}).exports.run;
    run(start, nfns, calls, msg.cpu);
    // rep=R (tbl=1, compiled here): R times a pack compiled again into an
    // existing pack's slots (shim/ndk/jit_pack.cc MakeRoom), its old
    // instance dropped, the new one called |calls| times: whether
    // JavaScriptCore gives the dropped ones back (topic jit-module-heap).
    var rep = msg.rep || 0;
    if (rep && tbl && msg.bytes) {
      var packs = Math.ceil(n / k);
      for (var r = 0; r < rep; r++) {
        var pj = r % packs, pcnt = Math.min(k, n - pj * k);
        var pcode = scratch.subarray(0, off[pj + 1] - off[pj]);
        pcode.set(all.subarray(off[pj], off[pj + 1]));
        new WebAssembly.Instance(new WebAssembly.Module(pcode), {e: {m: mem, x: helper, t: table, o: new WebAssembly.Global({value: 'i32'}, start + pj * k)}});
        run(start + pj * k, pcnt, calls, msg.cpu);
        // gc=MB: every 64 replacements that much of JS garbage that lives a
        // while (old generation), whether JavaScriptCore then collects the
        // dropped instances too.
        if (msg.gc && (r & 63) == 63) churn(msg.gc);
      }
    }
    var t2 = Date.now();
    self_keep = {table: table, run: run};
    post({id: msg.id, setupMs: t1 - t0, compileMs: compileMs, callMs: t2 - t1, fns: nfns});
  }
  var self_keep, kept, churnKeep = [];
  function churn(mb) {
    for (var i = 0; i < mb * 16; i++) {          // ~64 KB of small objects each
      var a = new Array(1024);
      for (var j = 0; j < 1024; j++) a[j] = {x: j, y: i};
      churnKeep.push(a);
      if (churnKeep.length > 64) churnKeep.shift();
    }
  }
  if (isNode) port.on('message', onmsg);
  else onmessage = function (e) { onmsg(e.data); };
})();
