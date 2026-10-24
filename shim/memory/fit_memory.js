// As much wasm memory as this device allows, up to a ceiling (--pre-js; topic
// userspace: the PWA on phones, merged with topic mem-budget's ceiling). This
// is the one place that makes the program's WebAssembly.Memory:
//
// - Wasm threads need shared memory, which browsers reserve up to its maximum
//   when it is made, and a phone's Safari refuses a maximum it cannot reserve
//   (RangeError: Out of memory). Pages cannot tell how much RAM a device has,
//   so the memory is made with the ceiling or, when that is refused, the
//   biggest of a few smaller ones that is not; a module accepts memory with a
//   smaller maximum than it declares. Growth past it then fails like any
//   out-of-memory (sbrk and malloc fail inside the VM).
// - The ceiling: the build's maximum (MAXIMUM_MEMORY), lowered by
//   Module.arcwasmMemoryCap (pages): the PWA page sets it after a start that
//   ran out of memory, and the memory budget (shim/membudget/
//   mem_budget_pre.js) sets it to its plan's ceiling.
// - Module.arcwasmMemoryMax is the maximum got (bytes): the PWA page sizes the
//   Java heap by it (demo/pwa/index.html), the memory budget takes it as its
//   ceiling.
// - Module.arcwasmMemoryInitial (MB): the memory starts that big (at most
//   the maximum) instead of the module's initial size, so it need not grow
//   up to there (a test switch: growth under running threads was suspected
//   on an iPhone, wrongly; growing the memory at once with memory.grow
//   instead cost the whole size in RAM there).
//
// The page's (or Node's) main thread only: workers get the memory made here.
// Part of the arc-wasm shim layer.
// Every growth of the memory in the log, on whichever thread grows it, with
// the size reached (for setting growth against a crash on a device).
if (typeof WebAssembly !== 'undefined' && !WebAssembly.Memory.prototype.arcwasmLoggedGrow) (function () {
  var grow = WebAssembly.Memory.prototype.grow;
  WebAssembly.Memory.prototype.grow = function (pages) {
    var before = this.buffer.byteLength;
    var r = grow.call(this, pages);
    var say = typeof err == 'function' ? err : console.log;
    say('fit_memory: grew ' + Math.round(before / 1048576) + ' -> ' +
        Math.round(this.buffer.byteLength / 1048576) + ' MB');
    return r;
  };
  WebAssembly.Memory.prototype.arcwasmLoggedGrow = true;
})();
if (typeof WebAssembly !== 'undefined' && !WebAssembly.Memory.arcwasmFitted) (function () {
  var isNode = typeof process == 'object' && process.versions && process.versions.node;
  if (typeof WorkerGlobalScope != 'undefined' ||
      (isNode && !require('worker_threads').isMainThread)) return;
  var Memory = WebAssembly.Memory;
  var PAGE = 65536;
  function Fitted(desc) {
    if (!desc || !desc.shared || !desc.maximum) return new Memory(desc);
    var cap = Math.min(desc.maximum, Module['arcwasmMemoryCap'] || desc.maximum);
    cap = Math.max(cap, desc.initial);
    var tries = [cap];
    [32768 - 1, 24576, 16384, 12288, 8192].forEach(function (pages) {
      if (pages < cap && pages >= desc.initial) tries.push(pages);
    });
    var error;
    for (var i = 0; i < tries.length; i++) {
      try {
        var initial = Math.min(tries[i], Math.max(desc.initial, (Module['arcwasmMemoryInitial'] || 0) * 16));
        var memory = new Memory({ initial: initial, maximum: tries[i], shared: true });
        Module['arcwasmMemoryMax'] = tries[i] * PAGE;
        if (initial > desc.initial) {
          (Module['print'] || console.log)('fit_memory: starts with ' + initial * PAGE / 1048576 +
              ' MB (no growth up to there)');
        }
        if (i > 0) {
          (Module['printErr'] || console.error)('fit_memory: ' + cap * PAGE / 1048576 +
              ' MB of wasm memory refused, ' + tries[i] * PAGE / 1048576 + ' MB taken');
        }
        return memory;
      } catch (e) {
        error = e;
      }
    }
    throw error;
  }
  Fitted.prototype = Memory.prototype;
  Fitted.arcwasmFitted = true;
  WebAssembly.Memory = Fitted;
})();
