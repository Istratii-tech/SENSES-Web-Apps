// --pre-js (topic arm-chains, docs/topics/arm-chains.md): the page's catalog
// of compiled packs of ARM/A64 translations (shim/ndk/jit_pack.cc), for the
// threads that start later.
//
// A thread compiles the packs of what is hot on it, and while it runs
// guest code it never returns to its event loop: nothing can hand it a
// module another thread compiled. A worker that the page gives a new thread
// (emscripten's pool: PThread.getNewWorker) does pass through its event
// loop first, though. So:
//   - a thread that compiled a pack posts it to the page:
//     {arcJitPack: number, mod: WebAssembly.Module, n: functions};
//   - the page keeps the latest (up to MAX_FNS functions) and, when it takes
//     a worker for a new thread, first posts it those it has not had yet.
//     A worker that only keeps modules pays for them too (iPhone X: 64
//     modules ~0.6 MB a worker, tools/jitmem mode=shared&inst=0), but
//     feeding only the workers whose threads made packs before cost Subway
//     more than it saved (its catalog went unused: JSC heaps +20-50 MB, JIT
//     code +10 MB on an iPhone X); every worker gets it;
//   - the worker keeps them (globalThis.ndkJitCatalog: number -> Module);
//     when blocks of a published pack become hot on its thread, jit_pack.cc
//     instantiates that module instead of compiling its own (no compile
//     time; JavaScriptCore ~7 KB + 9 bytes per byte of code a function
//     instead of ~5.5 KB + 12: tools/jitmem mode=shared).
// The messages carry no `cmd`: emscripten's handlers on both sides pass
// them by. Not in Node (its workers get messages another way).
(function () {
  if (typeof process == 'object' && process.versions && process.versions.node) return;
  if (typeof WorkerGlobalScope != 'undefined') {
    var cat = globalThis.ndkJitCatalog = new Map();
    // (A worker keeps at most this many packs, the oldest go.)
    var KEEP = 512;
    self.addEventListener('message', function (e) {
      var d = e.data;
      if (!d || d.arcJitPack === undefined || !d.mod) return;
      cat.set(d.arcJitPack, d.mod);
      if (cat.size > KEEP) cat.delete(cat.keys().next().value);
    });
    return;
  }
  if (typeof Module != 'object') return;
  var ua = (typeof navigator == 'object' && navigator.userAgent) || '';
  var phone = /iPhone|iPad|iPod|Android/.test(ua) || (/Macintosh/.test(ua) && navigator.maxTouchPoints > 1);
  // Functions the page keeps: a pack no thread uses any more holds its
  // compiled code here.
  var MAX_FNS = phone ? 2048 : 8192;
  var list = [], seq = 0, fns = 0, given = 0;
  function add(d) {
    list.push({seq: ++seq, p: d.arcJitPack, mod: d.mod, n: d.n | 0});
    fns += d.n | 0;
    while (fns > MAX_FNS && list.length > 1) fns -= list.shift().n;
  }
  function listen(w) {
    if (!w || w.arcJitListen) return;
    w.arcJitListen = true;
    w.arcJitSeq = w.arcJitSeq || 0;
    w.addEventListener('message', function (e) {
      var d = e.data;
      if (d && d.arcJitPack !== undefined && d.mod) add(d);
    });
  }
  function feed(w) {
    listen(w);
    for (var i = 0; i < list.length; i++) {
      var e = list[i];
      if (e.seq <= w.arcJitSeq) continue;
      try { w.postMessage({arcJitPack: e.p, mod: e.mod}); given++; } catch (x) { return; }
    }
    if (seq) w.arcJitSeq = seq;
  }
  function install() {
    if (typeof PThread != 'object' || !PThread.getNewWorker || PThread.arcJitCatalog) return;
    PThread.arcJitCatalog = function () { return {packs: list.length, functions: fns, given: given}; };
    PThread.unusedWorkers.forEach(listen);
    for (var id in PThread.pthreads) listen(PThread.pthreads[id]);
    var get = PThread.getNewWorker;
    PThread.getNewWorker = function () {
      var w = get.apply(PThread, arguments);
      if (w) feed(w);
      return w;
    };
  }
  var prev = Module['onRuntimeInitialized'];
  Module['onRuntimeInitialized'] = function () {
    try { install(); } catch (e) { (Module['printErr'] || console.error)('jit_catalog: ' + e); }
    if (prev) prev();
  };
})();
