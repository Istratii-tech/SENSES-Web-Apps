// --pre-js of artvm8 (topic mem-budget, shim/membudget/mem_budget.h): the
// memory budget and the wasm memory's ceiling, decided before the memory
// exists, and what only the page knows, for the VM to read.
//
//   plan    ArcMemPlan (mem_plan.js, the --pre-js before this one; SENSES'
//           run.html makes the memory with the same plan) from:
//             ?mem=MB, ?memcap=MB on the page (Node: ARC_MEM_BUDGET_MB,
//               ARC_MEM_CAP_MB): as given;
//             the device's measured tab limit (topic device-mem-budget):
//               Module.arcMemLimit (run.html: the library's memory test,
//               senses-mem-limit), else ?memlimit=MB or that test's JSON
//               (Node: ARC_MEM_LIMIT_MB, ARC_MEM_THREAD_MB): the limit less
//               the page and the threads' workers;
//             else by device: iPhone/iPad 1024 MB (ceiling 1536), a browser
//               that tells navigator.deviceMemory half of it (up to 3 GB),
//               others 3 GB.
//   cap     The shared WebAssembly.Memory is created with the plan's ceiling
//           as its maximum instead of 4 GB (shim/memory/fit_memory.js, or
//           less where the device refuses it: the ceiling is then what it
//           gave, the budget at most 2/3 of it): growing past it fails inside
//           the VM (malloc returns NULL, the system server's monitor acts
//           before), where the browser would kill the tab.
//   cells   arc_mem_js_cells(): OPFS usage and quota (navigator.storage.estimate,
//           every 10 s), whether the page is hidden, the ceiling, whether the
//           page reloads itself on request (arc_mem_request_restart), the
//           threads running (every second).
// The budget and the ceiling reach C as ARC_MEM_BUDGET_MB / ARC_MEM_CAP_MB.
var ArcMemBudget = (function () {
  var isNode = typeof process == 'object' && process.versions && process.versions.node;
  var isWorker = typeof WorkerGlobalScope != 'undefined' ||
      (isNode && !require('worker_threads').isMainThread);
  function plan() {
    var d = {};
    if (isNode) {
      d.mem = process.env.ARC_MEM_BUDGET_MB;
      d.memcap = process.env.ARC_MEM_CAP_MB;
      d.limitMb = process.env.ARC_MEM_LIMIT_MB;
      d.threadMb = process.env.ARC_MEM_THREAD_MB;
    } else {
      var q = typeof location == 'object' ? new URLSearchParams(location.search) : null;
      if (q) { d.mem = q.get('mem'); d.memcap = q.get('memcap'); }
      var lim = ArcMemPlan.limit(Module['arcMemLimit']) || (q && ArcMemPlan.limit(q.get('memlimit')));
      if (lim) { d.limitMb = lim.mb; d.threadMb = lim.threadMb; }
      if (typeof navigator == 'object') {
        d.ios = ArcMemPlan.isIOS(navigator.userAgent, navigator.maxTouchPoints);
        d.deviceMemory = navigator.deviceMemory;
      }
    }
    return ArcMemPlan(d);
  }

  var p = plan();
  if (!isWorker) {
    // The memory Emscripten creates (initMemory) is made by
    // shim/memory/fit_memory.js (linked with this): at most the plan's
    // ceiling, less where the device refuses that much.
    var capPages = p.cap * 16;
    if (!Module['arcwasmMemoryCap'] || Module['arcwasmMemoryCap'] > capPages) {
      Module['arcwasmMemoryCap'] = capPages;
    }
    Module['preRun'] = [].concat(Module['preRun'] || [], [function () {
      // What the device gave: the ceiling, and the budget within it.
      var got = Math.floor((Module['arcwasmMemoryMax'] || 0) / 1048576);
      if (got && got < p.cap) {
        p.cap = got;
        p.budget = Math.min(p.budget, Math.floor(got * 2 / 3));
        (Module['printErr'] || console.error)('membudget: wasm memory got ' + got +
            ' MB (the build\'s or the device\'s limit): budget ' + p.budget + ' MB');
      }
      ENV['ARC_MEM_BUDGET_MB'] = String(p.budget);
      ENV['ARC_MEM_CAP_MB'] = String(p.cap);
      // iOS (JavaScriptCore): translation of ARM code to wasm as elsewhere,
      // within the phone budget's limits (mem_budget.cc). It was off here while
      // every translation was a module of its own on every thread (Angry Birds
      // +140 MB, Telegram X +60 MB on an iPhone X); in packs, on the threads
      // that run, Angry Birds' menu stalls 5 s instead of 15 for ~40 MB (topic
      // shared-translations). Off: &env=ARC_CPU_JIT_THRESHOLD=-1 or debug.ndk.jit=0.
    }]);
    (Module['printErr'] || console.error)('membudget: ' + p.why + ': budget ' + p.budget +
        ' MB, wasm memory up to ' + p.cap + ' MB');
  }

  // The cells' index in HEAP32, asked once per thread: the export may not
  // be called once the program has exited (ASSERTIONS abort), and timers
  // and events of the page come later than that.
  var cellsAt = 0;
  function cells() {
    if (!cellsAt && typeof _arc_mem_js_cells == 'function' &&
        !(typeof runtimeExited != 'undefined' && runtimeExited)) {
      cellsAt = _arc_mem_js_cells() >> 2;
    }
    return cellsAt;
  }
  function set(i, v) {
    if (typeof runtimeExited != 'undefined' && runtimeExited) return;
    var c = cells();
    if (c) Atomics.store(HEAP32, c + i, v | 0);
  }
  function add(i, v) {
    var c = cells();
    if (c) Atomics.add(HEAP32, c + i, v | 0);
  }

  // The page's side: storage, visibility, restart (after the runtime is up).
  var timers = [];
  function startPage() {
    set(7, p.cap);  // ARC_MEM_JS_CAP_MB
    set(8, typeof location == 'object' && typeof document == 'object' ? 1 : 0);  // RESTART
    var threads = function () {  // THREADS: the program's thread and the workers running
      if (exited()) return;
      // (PThread.pthreads: the running ones by their pthread_t; older
      // Emscripten kept PThread.runningWorkers.)
      if (typeof PThread != 'object') return;
      var running = PThread.pthreads ? Object.keys(PThread.pthreads).length
          : PThread.runningWorkers ? PThread.runningWorkers.length : 0;
      set(9, 1 + running);
      retireIdleWorkers();
      // (and the idle ones kept for new threads: each a worker the tab holds)
      var spare = PThread.unusedWorkers ? PThread.unusedWorkers.length : 0;
      // For the page (SENSES' run.html: a "low memory, reload" offer): the
      // monitor's fill of the budget, per mille (PRESSURE_PM), the threads
      // and the workers (on an iPhone each costs the tab ~5 MB besides), the
      // wasm memory touched (TOUCHED_MB: what of it the tab holds; 0 where
      // no monitor says it) and the memory's size.
      if (typeof Module['arcMemPressure'] == 'function') {
        var c = cells();
        try {
          Module['arcMemPressure']({pm: c ? Atomics.load(HEAP32, c + 10) : 0, threads: 1 + running,
                                    budget: p.budget, ios: p.ios,
                                    touchedMb: c ? Atomics.load(HEAP32, c + 12) : 0,
                                    workers: running + spare,
                                    wasmMb: typeof wasmMemory == 'object' && wasmMemory ?
                                        Math.round(wasmMemory.buffer.byteLength / 1048576) : 0});
        } catch (e) {}
      }
    };
    // The page's "Перезагрузить": the system server saves the app's task and
    // asks for the reload (MemoryMonitor, as for its own restart for memory);
    // a VM too busy to do it in 6 s is reloaded as it is.
    Module['arcMemUserRestart'] = function () {
      set(11, 1);  // USER_RESTART
      setTimeout(function () {
        (Module['printErr'] || console.error)('membudget: no restart from the system in 6 s: reloading');
        if (typeof Module['arcMemRestart'] == 'function') Module['arcMemRestart']();
        else location.reload();
      }, 6000);
    };
    // Workers of threads that exited go back to Emscripten's pool and stay
    // there for good: an app that once had 73 threads and now runs 54 kept
    // 19 idle workers, each a JavaScriptCore heap and module instance (~4-5
    // MB on an iPhone; topic ios-threads). SPARE_WORKERS stay for the next
    // threads; those beyond, idle IDLE_MS, are terminated (oldest first).
    // (10 s, was 30: on phones idle Java pool threads end after 5 s, topic
    // heavy-apps-mem, and their workers should not outlive them by long.)
    var SPARE_WORKERS = 4, IDLE_MS = 10000, retired = 0;
    var retireIdleWorkers = function () {
      var pool = PThread.unusedWorkers;
      if (isNode || !pool) return;
      var now = Date.now();
      for (var i = 0; i < pool.length; i++) pool[i].arcIdleSince = pool[i].arcIdleSince || now;
      for (var id in PThread.pthreads) PThread.pthreads[id].arcIdleSince = 0;
      var n = 0;
      // (getNewWorker takes from the end: the oldest are at the start)
      while (pool.length > SPARE_WORKERS && now - pool[0].arcIdleSince >= IDLE_MS) {
        var w = pool.shift();
        w.onmessage = function () {};
        w.terminate();
        n++;
      }
      if (n) {
        retired += n;
        (Module['printErr'] || console.error)('membudget: ' + n + ' idle thread workers terminated (' +
            retired + ' in all, ' + pool.length + ' spare)');
      }
    };
    threads();
    var t = setInterval(threads, 1000);
    if (t && t.unref) t.unref();  // (Node: not what keeps the process alive)
    timers.push(t);
    if (typeof document == 'object') {
      var vis = function () { set(6, document.visibilityState == 'hidden' ? 1 : 0); };
      document.addEventListener('visibilitychange', vis);
      vis();
    }
    if (typeof navigator == 'object' && navigator.storage && navigator.storage.estimate && !isNode) {
      var poll = function () {
        if (exited()) return;
        navigator.storage.estimate().then(function (e) {
          set(3, Math.min(2147483647, Math.floor((e.usage || 0) / 1024)));
          set(4, Math.min(2147483647, Math.floor((e.quota || 0) / 1024)));
          set(5, 1);
        }, function () {});
      };
      poll();
      timers.push(setInterval(poll, 10000));
    }
  }
  // Nothing more once the program has exited.
  function exited() {
    if (typeof runtimeExited == 'undefined' || !runtimeExited) return false;
    timers.forEach(clearInterval);
    timers = [];
    return true;
  }
  if (!isWorker) {
    var prev = Module['onRuntimeInitialized'];
    Module['onRuntimeInitialized'] = function () {
      try { startPage(); } catch (e) { (Module['printErr'] || console.error)('membudget: ' + e); }
      if (prev) prev();
    };
  }
  // For webcodecs.js (media thread): VideoFrames open and their size.
  return {
    plan: p,
    frameOpened: function (bytes) { add(0, 1); add(1, Math.ceil(bytes / 1024)); },
    frameClosed: function (bytes) { add(0, -1); add(1, -Math.ceil(bytes / 1024)); },
    decoders: function (delta) { add(2, delta); }
  };
})();
