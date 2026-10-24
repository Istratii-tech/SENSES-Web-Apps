// --pre-js of artvm (topic tiktok): when a pthread dies of a wasm trap
// (RuntimeError: memory access out of bounds, ...), ask the ARM CPU
// (shim/ndk/cpu, arm64: shim/ndk/cpu64) which guest code that thread was
// running before the error goes on as before. Under Node a pthread is a
// worker_threads worker; in a browser a Worker.
(function() {
  function hex(v) { return ('0000000' + (v >>> 0).toString(16)).slice(-8); }
  // The registers, read and printed here: the trapped thread's own stdio
  // may never get out (topic compat-wave3: Go's libgojni in Node).
  function raw() {
    try {
      if (typeof _ndk_cpu_trap_regs === 'function') {
        var p = _ndk_cpu_trap_regs() >>> 2;
        if (p) {
          var w = HEAPU32, lines = ['ndk_cpu: wasm trap in guest code, block ' + hex(w[p]) +
                                    (w[p + 2] ? ' (Thumb)' : ' (ARM)') + ', insn ' + hex(w[p + 1])];
          for (var i = 0; i < 16; i += 4) {
            var l = ' ';
            for (var k = i; k < i + 4; k++) l += ' r' + (k < 10 ? k + ' ' : k) + ' ' + hex(w[p + 3 + k]);
            lines.push(l);
          }
          // The top of its stack (return addresses for a backtrace by hand).
          var sp = w[p + 3 + 13] >>> 2, words = [];
          for (var s = 0; s < 64 && sp + s < w.length; s++) words.push(hex(w[sp + s]));
          for (var t = 0; t < words.length; t += 8) lines.push('  sp+' + (t * 4).toString(16) + ': ' + words.slice(t, t + 8).join(' '));
          console.error(lines.join('\n'));
        }
      }
      if (typeof _ndk64_cpu_trap_regs === 'function') {
        var q = _ndk64_cpu_trap_regs() >>> 2;
        if (q) {
          var u = HEAPU32, out = ['ndk_cpu64: wasm trap in guest code, block ' + hex(u[q]) + ', pc ' + hex(u[q + 1])];
          for (var j = 0; j < 32; j += 4) {
            var m = ' ';
            for (var n = j; n < j + 4 && n < 32; n++)
              m += (n == 31 ? '  sp ' : ' x' + (n < 10 ? n + ' ' : n) + ' ') + hex(u[q + 3 + 2 * n]) + hex(u[q + 2 + 2 * n]);
            out.push(m);
          }
          console.error(out.join('\n'));
        }
      }
    } catch (e) { console.error('guest trap report: ' + e); }
  }
  // Then symbols and the last host calls (stdio: may not appear).
  function report() {
    raw();
    try {
      if (typeof _ndk_cpu_report_trap === 'function') _ndk_cpu_report_trap();
    } catch (e) {}
    try {
      if (typeof _ndk64_cpu_report_trap === 'function') _ndk64_cpu_report_trap();
    } catch (e) {}
  }
  if (typeof process === 'object' && typeof require === 'function') {
    try {
      if (require('worker_threads').isMainThread) return;
    } catch (e) { return; }
    process.on('uncaughtException', function(e) {
      process.removeAllListeners('uncaughtException');
      if (e instanceof WebAssembly.RuntimeError) raw();
      setTimeout(function() {   // (the rest may block: after the rethrow's turn)
        if (e instanceof WebAssembly.RuntimeError) report();
      }, 0);
      throw e;
    });
  } else if (typeof WorkerGlobalScope !== 'undefined') {
    self.addEventListener('error', function(ev) {
      if (ev.error instanceof WebAssembly.RuntimeError) report();
    });
  }
})();
