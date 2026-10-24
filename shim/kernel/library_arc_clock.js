// The VM's monotonic clock, one for all threads. Linked with --js-library
// after Emscripten's own, so it replaces emscripten_get_now: what
// clock_gettime(CLOCK_MONOTONIC) and the other WASI clocks but REALTIME read
// (System.nanoTime, SystemClock.uptimeMillis, the guests' clocks...).
//
// Emscripten's is performance.timeOrigin + performance.now() of the worker
// that asks. The workers' time origins disagree by up to about a millisecond
// (each is the wall clock when the worker started), so a time read on one
// thread can still be "in the future" on another a moment later. Android
// compares such times across threads: a frame callback posted to a
// Choreographer from another thread is due at the poster's uptimeMillis(),
// and doFrame on the Choreographer's thread runs only the callbacks due at
// its own clock; one that is not yet due there stays queued with no vsync
// asked for, and the frames stop for good (Unity 2022 waiting for its frame
// on the way into the menu; topic unity2022-menu-hang). The vsyncs
// SurfaceFlinger stamps with its thread's clock were "in the future" for the
// apps' threads for the same reason.
//
// So no reading is earlier than the latest one any thread made: the latest is
// kept in wasm memory (arc_clock_now of shim/kernel/arc_kernel.cc), in
// microseconds modulo 2^32 (Int32 atomics: Safari 16 has no 64-bit ones;
// readings an hour apart are never compared). And a worker whose clock is
// behind another's catches up, so that it does not keep repeating the same
// time: by what it lags behind the latest reading it loaded before reading its
// own clock (that one was made earlier: a lag measured so is the clocks', never
// a pause of this thread), less 50 us (performance.now() is coarsened with
// jitter: catching up all of it would ratchet the clocks forward).
// arc_clock_now[1] counts those catch-ups, [2] has the largest lag in us;
// [3] is set by ARC_CLOCK=0: each worker's own clock, as before (the lags are
// still counted).

addToLibrary({
  $ArcClock: {
    off: 0,     // this worker's correction, ms
    cell: -1,   // index of arc_clock_now in HEAP32; 0: not linked
  },
  emscripten_get_now__deps: ['$ArcClock'],
  emscripten_get_now: () => {
    var c = ArcClock.cell;
    if (c <= 0) {
      // (Before the module is instantiated, or a program without the kernel.)
      var g = c < 0 && typeof wasmExports != 'undefined' && wasmExports && wasmExports['arc_clock_now'];
      if (!g) return performance.timeOrigin + performance.now() + ArcClock.off;
      c = ArcClock.cell = g.value >>> 2;
    }
    var latest = Atomics.load(HEAP32, c);  // (before this thread's clock: above)
    var us = Math.round((performance.timeOrigin + performance.now() + ArcClock.off) * 1000);
    var low = us | 0;  // modulo 2^32
    var own = Atomics.load(HEAP32, c + 3);  // ARC_CLOCK=0: only measured
    var lag = (latest - low) | 0;
    if (lag > 50 && latest !== 0) {
      if (!own) ArcClock.off += (lag - 50) / 1000;
      Atomics.add(HEAP32, c + 1, 1);
      if (lag > Atomics.load(HEAP32, c + 2)) Atomics.store(HEAP32, c + 2, lag);
    }
    for (;;) {
      var ahead = (low - latest) | 0;
      if (ahead <= 0 && latest !== 0) return (own ? us : us - ahead) / 1000;
      var seen = Atomics.compareExchange(HEAP32, c, latest, low);
      if (seen === latest) return us / 1000;
      latest = seen;
    }
  },
});
