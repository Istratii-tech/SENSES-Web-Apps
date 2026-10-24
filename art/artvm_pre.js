// --pre-js of artvm.js: under Node, the ARC_* environment variables
// (ARC_LOG, ARC_ART_TRACE_JNI ...) reach getenv() in the program; Emscripten
// otherwise gives it only a fixed default environment.
if (typeof process == 'object' && process.env) {
  Module['preRun'] = [].concat(Module['preRun'] || [], [function() {
    for (var k in process.env) {
      if (k.startsWith('ARC_')) ENV[k] = process.env[k];
    }
  }]);
}
// Whole native stacks in crash reports (wasm frames included).
if (typeof Error == 'function') Error.stackTraceLimit = 200;
