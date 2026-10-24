// JavaScriptCore's own count of what remote_webgl.js allocates per frame
// (topic gl-heap): its page and thread halves in two realms of one VM (a
// message runs the page's handler at once, as in remote_webgl_test.js; the
// message is copied the way a structured clone would make it on the page),
// a stand-in WebGL2 on the page, and per frame the calls Subway Surfers
// (Unity, through the NDK bridge) makes in a run on Android 10. Run with the
// shell of the system's JavaScriptCore, the engine of Safari:
//
//   J=/System/Library/Frameworks/JavaScriptCore.framework/Versions/Current/Helpers/jsc
//   $J --logGC=1 shim/gles/remote_webgl_bench.js -- shim/gles/remote_webgl.js [FRAMES] 2>&1 |
//       python3 tools/jsc_gc_sum.py
//
// The script prints frames and messages; jsc_gc_sum.py adds up what the
// collections found allocated.

var path = arguments[0], FRAMES = +(arguments[1] || 300);
var src = read(path);

var G = { ARRAY_BUFFER: 0x8892, ELEMENT_ARRAY_BUFFER: 0x8893, COPY_WRITE_BUFFER: 0x8F37,
          PIXEL_PACK_BUFFER: 0x88EB, FRAMEBUFFER: 0x8D40, READ_FRAMEBUFFER: 0x8CA8,
          TEXTURE_2D: 0x0DE1, RGBA: 0x1908, UNSIGNED_BYTE: 0x1401, TRIANGLES: 4,
          UNSIGNED_SHORT: 0x1403, STATIC_DRAW: 0x88E4, DYNAMIC_DRAW: 0x88E8, BUFFER_SIZE: 0x8764,
          PACK_ALIGNMENT: 0x0D05, UNPACK_ALIGNMENT: 0x0CF5, COLOR_ATTACHMENT0: 0x8CE0, NO_ERROR: 0 };
var now = function () { return preciseTime() * 1000; };

// --- the page: a stand-in WebGL2 that keeps the bindings it is asked for.
var page = $262.createRealm();
var pg = page.global;
pg.print = print;
pg.performance = { now: now };
pg.location = { search: '' };
pg.wasmMemory = { buffer: new SharedArrayBuffer(64 << 20) };
pg.Module = { printErr: function (t) { if (!/frames read/.test(t)) print('page: ' + t); } };
pg.window = pg;
page.evalScript('function WebGL2RenderingContext() {}');
var proto = pg.WebGL2RenderingContext.prototype;
for (var k in G) { proto[k] = G[k]; pg.WebGL2RenderingContext[k] = G[k]; }
var bound = {};
var BINDING_OF = { 0x8892: 0x8894, 0x8893: 0x8895, 0x8F37: 0x8F37, 0x8F36: 0x8F36, 0x88EB: 0x88ED };
var methods = {
  bindBuffer: function (t, b) { bound[BINDING_OF[t] || t] = b; },
  bindFramebuffer: function (t, f) { if (t == 0x8D40 || t == 0x8CA8) bound[0x8CAA] = f; },
  getParameter: function (p) {
    if (p == 0x0D05 || p == 0x0CF5) return 4;
    return bound[p] === undefined ? (p >= 0x0D02 && p <= 0x0D04 ? 0 : null) : bound[p];
  },
  getBufferParameter: function () { return 1 << 20; },
  clientWaitSync: function () { return 0x911A; },
  getError: function () { return 0; },
  isContextLost: function () { return false; },
  getExtension: function () { return null; },
};
['createBuffer', 'createFramebuffer', 'createTexture', 'createProgram', 'fenceSync'].forEach(function (n) {
  methods[n] = function () { return new pg.Object(); };
});
['texImage2D', 'framebufferTexture2D', 'bufferData', 'bufferSubData', 'uniform4fv', 'uniformMatrix4fv', 'drawElements', 'bindTexture',
 'useProgram', 'vertexAttribPointer', 'enableVertexAttribArray', 'enable', 'disable', 'viewport',
 'clear', 'readPixels', 'pixelStorei', 'readBuffer', 'deleteSync', 'activeTexture', 'uniform1i',
 'blendFunc', 'depthMask', 'colorMask', 'getBufferSubData', 'invalidateFramebuffer',
 'deleteBuffer', 'deleteFramebuffer', 'deleteTexture'].forEach(function (n) {
  if (!methods[n]) methods[n] = function () {};
});
for (var m in methods) proto[m] = methods[m];
var gl = new pg.WebGL2RenderingContext();
pg.document = { createElement: function () { return { width: 1, height: 1, getContext: function () { return gl; } }; } };
// (Objects of the other realm are the page's own in a browser.)
var sabCheck = { value: function (v) { return Object.prototype.toString.call(v) == '[object SharedArrayBuffer]'; } };
Object.defineProperty(pg.SharedArrayBuffer, Symbol.hasInstance, sabCheck);
page.evalScript(src);

// What a structured clone makes on the page of a message.
var messages = 0;
function isSab(v) { return Object.prototype.toString.call(v) == '[object SharedArrayBuffer]'; }
function clone(v) {
  if (v === null || typeof v != 'object' || isSab(v) || ArrayBuffer.isView(v)) return v;
  if (Array.isArray(v)) return v.map(clone);
  var o = {};
  for (var k in v) o[k] = clone(v[k]);
  return o;
}

// --- the GL thread.
var thread = $262.createRealm();
var th = thread.global;
th.print = print;
th.performance = { now: now };
th.wasmMemory = pg.wasmMemory;
th.self = { location: { search: '' }, postMessage: function (m) {
  messages++;
  var c = clone(m);
  pg.Module[c.handler].apply(null, c.args);
} };
Object.defineProperty(th.SharedArrayBuffer, Symbol.hasInstance, sabCheck);
thread.evalScript(src);
var rc = thread.evalScript('arcRemoteWebGL').create({});
if (!rc) throw new Error('no context');

var H = new th.Uint8Array(th.wasmMemory.buffer), HF = new th.Float32Array(th.wasmMemory.buffer);
var vbo = [], tex = [], prog = [];
for (var i = 0; i < 16; i++) {
  vbo.push(rc.createBuffer());
  rc.bindBuffer(G.COPY_WRITE_BUFFER, vbo[i]);
  rc.bufferData(G.COPY_WRITE_BUFFER, 1 << 16, G.DYNAMIC_DRAW);
  tex.push(rc.createTexture());
  prog.push(rc.createProgram());
}
var fbo = rc.createFramebuffer();
var FRAME_AT = 32 << 20, HEAP_HIGH = 40 << 20;

// One frame of Subway in a run, as the NDK bridge calls the context.
function frame(f) {
  // ~12 buffer maps: BufferBinding (glMapBufferRange, glFlushMappedBufferRange,
  // glUnmapBuffer), BUFFER_SIZE, the upload.
  for (var i = 0; i < 12; i++) {
    var b = vbo[(f + i) & 15];
    rc.bindBuffer(G.COPY_WRITE_BUFFER, b);
    rc.getParameter(0x8F37);
    rc.getBufferParameter(G.COPY_WRITE_BUFFER, G.BUFFER_SIZE);
    rc.getParameter(0x8F37);
    rc.bufferSubData(G.COPY_WRITE_BUFFER, 0, H, HEAP_HIGH + i * 4096, 1536 + i * 64);
    rc.getParameter(0x8F37);
  }
  rc.bindFramebuffer(G.FRAMEBUFFER, fbo);
  rc.viewport(0, 0, 480, 360);
  rc.clear(0x4500);
  // ~80 draws with their state and uniforms (~950 calls).
  for (var d = 0; d < 80; d++) {
    rc.useProgram(prog[d & 15]);
    rc.bindBuffer(G.ARRAY_BUFFER, vbo[d & 15]);
    rc.vertexAttribPointer(0, 3, 0x1406, false, 32, 0);
    rc.vertexAttribPointer(1, 2, 0x1406, false, 32, 12);
    rc.enableVertexAttribArray(0);
    rc.activeTexture(0x84C0);
    rc.bindTexture(G.TEXTURE_2D, tex[d & 15]);
    rc.uniform4fv(null, HF, (HEAP_HIGH >> 2) + 16 * d, 4);
    rc.uniform4fv(null, HF, (HEAP_HIGH >> 2) + 16 * d + 4, 4);
    rc.uniformMatrix4fv(null, false, HF, (HEAP_HIGH >> 2) + 64 * d, 16);
    rc.uniform1i(null, 0);
    rc.bindBuffer(G.ELEMENT_ARRAY_BUFFER, vbo[(d + 1) & 15]);
    if (d & 1) rc.enable(0x0BE2); else rc.disable(0x0BE2);
    rc.drawElements(G.TRIANGLES, 300, G.UNSIGNED_SHORT, 0);
  }
  rc.invalidateFramebuffer(G.FRAMEBUFFER, th.Array.of(0x8D00));
  // Two fences (Unity's).
  for (var s = 0; s < 2; s++) {
    var sync = rc.fenceSync(0x9117, 0);
    rc.clientWaitSync(sync, 1, 0);
    rc.deleteSync(sync);
  }
  // The frame read (ndk_egl.cc ReadFrame).
  var readFb = rc.getParameter(0x8CAA), pack = rc.getParameter(0x88ED), saved = [];
  for (var p = 0; p < 4; p++) saved.push(rc.getParameter([0x0D05, 0x0D02, 0x0D03, 0x0D04][p]));
  rc.bindFramebuffer(G.READ_FRAMEBUFFER, fbo);
  rc.readBuffer(G.COLOR_ATTACHMENT0);
  rc.bindBuffer(G.PIXEL_PACK_BUFFER, null);
  rc.pixelStorei(0x0D05, 4);
  rc.pixelStorei(0x0D02, 0);
  rc.pixelStorei(0x0D03, 0);
  rc.pixelStorei(0x0D04, 0);
  rc.readPixels(0, 0, 480, 360, G.RGBA, G.UNSIGNED_BYTE, H, FRAME_AT);
  for (var q = 0; q < 4; q++) rc.pixelStorei([0x0D05, 0x0D02, 0x0D03, 0x0D04][q], saved[q]);
  rc.bindBuffer(G.PIXEL_PACK_BUFFER, pack);
  rc.bindFramebuffer(G.READ_FRAMEBUFFER, readFb);
}

for (var w = 0; w < 30; w++) frame(w);  // warm up (the JITs)
gc();
print('BENCH START');
var t0 = now();
for (var f = 0; f < FRAMES; f++) frame(f);
var ms = now() - t0;
var m0 = messages;
gc();
print('BENCH END frames ' + FRAMES + ', ' + (ms / FRAMES).toFixed(2) + ' ms a frame, messages ' + messages);
