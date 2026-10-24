// node shim/gles/remote_webgl_test.js: remote_webgl.js's page and thread
// halves in one process (two vm contexts; a message runs the page's handler
// at once, so the thread's Atomics.wait finds its answer there), against a
// recording stand-in for WebGL2: the calls the page makes must be the ones
// the thread's code made, with the same data.
'use strict';
const fs = require('fs');
const vm = require('vm');
const path = require('path');
const src = fs.readFileSync(path.join(__dirname, 'remote_webgl.js'), 'utf8');

const G = { RGBA: 0x1908, UNSIGNED_BYTE: 0x1401, PACK_ALIGNMENT: 0x0D05, UNPACK_ALIGNMENT: 0x0CF5,
            TEXTURE_2D: 0x0DE1, FRAMEBUFFER: 0x8D40, COLOR_ATTACHMENT0: 0x8CE0, NO_ERROR: 0,
            COMPILE_STATUS: 0x8B81, LINK_STATUS: 0x8B82, ARRAY_BUFFER: 0x8892, STATIC_DRAW: 0x88E4,
            BUFFER_SIZE: 0x8764 };
function WebGL2RenderingContext() {}
Object.assign(WebGL2RenderingContext.prototype, G);
Object.assign(WebGL2RenderingContext, G);

const log = [];
let objs = 0;
const obj55 = { obj: 55 };
function copyArg(a) {
  if (ArrayBuffer.isView(a)) return { view: a.constructor.name, data: Array.from(a) };
  if (a && a.obj) return 'obj' + a.obj;
  return a;
}
const gl = new Proxy(new WebGL2RenderingContext(), {
  get(o, p) {
    if (p in o && typeof o[p] != 'function') return o[p];
    if (typeof p != 'string') return undefined;
    if (p == 'getParameter') return (k) => {
      log.push(['getParameter', k]);
      return k == G.PACK_ALIGNMENT || k == G.UNPACK_ALIGNMENT ? 4 : k == 0x8CAA ? { obj: 99 } :
             k == 0x1234 ? obj55 : k == 0x0BA2 ? new Int32Array([1, 2, 3, 4]) : 0;
    };
    if (p == 'getBufferSubData') return function (t, off, view) {
      for (let i = 0; i < view.length; i++) view[i] = (i * 3 + off) & 255;
      log.push(['getBufferSubData', t, off, view.length]);
    };
    if (p == 'getError') return () => 7;
    if (p == 'isContextLost') return () => false;
    if (p == 'getShaderParameter' || p == 'getProgramParameter') return () => true;
    if (p == 'getSupportedExtensions') return () => ['A', 'B'];
    if (p == 'getBufferParameter') return () => 1 << 20;
    if (/^create/.test(p)) return () => ({ obj: ++objs });
    if (p == 'readPixels') return function (x, y, w, h, f, t, view) {
      for (let i = 0; i < w * h * 4; i++) view[i] = (i * 7) & 255;
      log.push(['readPixels', x, y, w, h]);
    };
    return function () { log.push([p].concat(Array.from(arguments, copyArg))); };
  },
});
// The methods the page lists (methodsOf: for..in over the context).
const methodNames = ['activeTexture', 'bindBuffer', 'bufferData', 'bufferSubData', 'clearBufferfv',
  'createBuffer', 'deleteBuffer', 'drawArrays', 'drawBuffers', 'getError', 'getParameter',
  'getSupportedExtensions', 'readPixels', 'shaderSource', 'texImage2D', 'uniform4fv',
  'uniformMatrix4fv', 'vertexAttrib4fv', 'colorMask', 'uniform1f', 'compileShader', 'createShader',
  'bindAttribLocation', 'getBufferSubData', 'createFramebuffer', 'createTexture', 'bindFramebuffer',
  'pixelStorei', 'activeTexture', 'bindTexture', 'getBufferParameter', 'clientWaitSync', 'fenceSync'];
methodNames.forEach((n) => { Object.defineProperty(WebGL2RenderingContext.prototype, n, {
  value: function () {}, enumerable: true, configurable: true, writable: true }); });

const errs = [];
const heap = new SharedArrayBuffer(1 << 20);
const wasmMemory = { buffer: heap };
const page = { console, Atomics, SharedArrayBuffer, performance, JSON, Map, WeakMap, Proxy,
  WebGL2RenderingContext, location: { search: '?remotegl=noshared' }, wasmMemory,
  document: { createElement: () => ({ width: 1, height: 1, getContext: () => gl }) },
  Module: { printErr: (t) => errs.push(t) } };
page.window = page;
page.globalThis = page;
vm.createContext(page);
// The page's for..in over gl: the stand-in proxies its prototype.
vm.runInContext(src, page);

const worker = { console, Atomics, SharedArrayBuffer, performance, JSON, Map, Proxy, ArrayBuffer, wasmMemory,
  Math, Date, String, Array, Int32Array, Float64Array, Uint8Array, Uint16Array, Float32Array,
  self: { location: { search: '' },
          postMessage: (m) => { if (process.env.DBG) console.log("msg", m.args[0].op, m.args[0].n, m.args[0].sync && m.args[0].sync[1], m.args[0].names); try { page.Module[m.handler].apply(null, structuredClone(m.args)); } catch (e) { console.log("page threw", e); } } } };
worker.globalThis = worker;
vm.createContext(worker);
vm.runInContext(src, worker);

// methodsOf on the page uses for..in over the real context: give it the names.
const rc = vm.runInContext('arcRemoteWebGL', worker).create({});
if (!rc) throw new Error('no context');

let failures = 0;
function expect(what, got, want) {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) { failures++; console.log('FAIL ' + what + '\n  got  ' + g + '\n  want ' + w); }
  else console.log('ok   ' + what);
}
const H8 = new Uint8Array(heap), HF = new Float32Array(heap);
for (let i = 0; i < 64; i++) HF[256 + i] = i + 0.5;

log.length = 0;
const buf = rc.createBuffer();
rc.bindBuffer(G.ARRAY_BUFFER, buf);
rc.uniform4fv(null, HF, 256, 4);
rc.uniformMatrix4fv(null, false, HF, 260, 16);
for (let i = 0; i < 100; i++) H8[4096 + i] = i;
rc.bufferData(G.ARRAY_BUFFER, H8, G.STATIC_DRAW, 4096, 100);
rc.bufferSubData(G.ARRAY_BUFFER, 8, H8, 4097, 3);
rc.clearBufferfv(0x1800, 0, HF, 256);
rc.vertexAttrib4fv(1, HF.subarray(258, 262));
const tmp = new Float32Array(8); tmp.set([1, 2, 3, 4, 5, 6, 7, 8]);
rc.uniform4fv(null, tmp, 4, 4);
rc.drawBuffers([0x8CE0, 0]);
rc.shaderSource(null, 'void main() { gl_FragColor = vec4(1.0); }');
rc.colorMask(true, false, true, false);
rc.uniform1f(null, 0.25);
rc.canvas.width = 640;
rc.drawArrays(4, 0, 3);
const err = rc.getError();
rc.deleteBuffer(buf);
rc.drawArrays(4, 3, 3);
expect('getError (number answer)', err, 7);
expect('queued calls in order', log, [
  ['bindBuffer', G.ARRAY_BUFFER, 'obj1'],
  ['uniform4fv', null, { view: 'Float32Array', data: [0.5, 1.5, 2.5, 3.5] }],
  ['uniformMatrix4fv', null, false, { view: 'Float32Array', data: Array.from({ length: 16 }, (_, i) => i + 4.5) }],
  ['bufferData', G.ARRAY_BUFFER, { view: 'Uint8Array', data: Array.from({ length: 100 }, (_, i) => i) }, G.STATIC_DRAW],
  ['bufferSubData', G.ARRAY_BUFFER, 8, { view: 'Uint8Array', data: [1, 2, 3] }],
  ['clearBufferfv', 0x1800, 0, { view: 'Float32Array', data: [0.5, 1.5, 2.5, 3.5] }],
  ['vertexAttrib4fv', 1, { view: 'Float32Array', data: [2.5, 3.5, 4.5, 5.5] }],
  ['uniform4fv', null, { view: 'Float32Array', data: [5, 6, 7, 8] }],
  ['drawBuffers', [0x8CE0, 0]],
  ['shaderSource', null, 'void main() { gl_FragColor = vec4(1.0); }'],
  ['colorMask', true, false, true, false],
  ['uniform1f', null, 0.25],
  ['drawArrays', 4, 0, 3],
]);
log.length = 0;
// A sync call flushes the rest (deleteBuffer, drawArrays).
expect('extensions (JSON answer)', rc.getSupportedExtensions(), ['A', 'B']);
expect('after the next sync call', log, [['deleteBuffer', 'obj1'], ['drawArrays', 4, 3, 3]]);

// readPixels into the heap (sync, through the page's kept buffer).
log.length = 0;
rc.readPixels(0, 0, 4, 2, G.RGBA, G.UNSIGNED_BYTE, H8, 8192);
expect('readPixels landed', Array.from(H8.subarray(8192, 8192 + 32)), Array.from({ length: 32 }, (_, i) => (i * 7) & 255));
rc.readPixels(0, 0, 4, 2, G.RGBA, G.UNSIGNED_BYTE, H8, 8192);

// The layer's frame read (egl_layer_surface.cc): one wait, state put back.
log.length = 0;
H8.fill(0, 16384, 16384 + 64);
rc.arcReadPixels(0, 0, 4, 2, H8, 16384);
expect('arcReadPixels landed', Array.from(H8.subarray(16384, 16384 + 32)), Array.from({ length: 32 }, (_, i) => (i * 7) & 255));
expect('arcReadPixels state', log.filter((c) => c[0] != 'getParameter' || [0x8CAA, 0x88ED].includes(c[1])),
       [['getParameter', 0x8CAA], ['getParameter', 0x88ED], ['bindFramebuffer', 0x8CA8, null],
        ['readPixels', 0, 0, 4, 2], ['bindFramebuffer', 0x8CA8, 'obj99']]);

// texImage2D from the heap (sync; ?remotegl=noshared: a private copy).
log.length = 0;
for (let i = 0; i < 16; i++) H8[12288 + i] = 200 + i;
rc.texImage2D(G.TEXTURE_2D, 0, G.RGBA, 2, 2, 0, G.RGBA, G.UNSIGNED_BYTE, H8, 12288);
expect('texImage2D copy', log.filter((c) => c[0] != 'getParameter'), [['texImage2D', G.TEXTURE_2D, 0, G.RGBA, 2, 2, 0, G.RGBA, G.UNSIGNED_BYTE,
  { view: 'Uint8Array', data: Array.from({ length: 16 }, (_, i) => 200 + i) }]]);

// Data larger than the stream (it grows), and many calls (QUEUE_MAX flush).
log.length = 0;
const big = 600 << 10;
for (let i = 0; i < big; i++) H8[100000 + i] = i & 255;
rc.bufferData(G.ARRAY_BUFFER, H8, G.STATIC_DRAW, 100000, big);
for (let i = 0; i < 10000; i++) rc.drawArrays(4, i, 3);
rc.getError();
const bd = log.filter((c) => c[0] != 'getParameter')[0];
expect('big bufferData', [bd[0], bd[2].data.length, bd[2].data[12345], bd[2].data[big - 1]],
       ['bufferData', big, 12345 & 255, (big - 1) & 255]);
expect('10000 queued draws', [log.length - 1, log[log.length - 1]], [10000, ['drawArrays', 4, 9999, 3]]);
// The shadow (topic gl-heap): bindings, pack parameters, buffer sizes and
// syncs are answered on the thread, without a wait for the page.
log.length = 0;
const b2 = rc.createBuffer(), fb = rc.createFramebuffer(), tex = rc.createTexture();
rc.bindBuffer(0x8F37, b2);
rc.bufferData(0x8F37, H8, G.STATIC_DRAW, 4096, 300);
rc.bindFramebuffer(0x8CA8, fb);
rc.pixelStorei(0x0D02, 7);
rc.activeTexture(0x84C3);
rc.bindTexture(G.TEXTURE_2D, tex);
const answers = [rc.getParameter(0x8F37) === b2, rc.getBufferParameter(0x8F37, G.BUFFER_SIZE),
                 rc.getParameter(0x8CAA) === fb, rc.getParameter(0x8CA6), rc.getParameter(0x0D02),
                 rc.getParameter(0x0D05), rc.getParameter(0x84E0), rc.getParameter(0x8069) === tex,
                 rc.clientWaitSync(rc.fenceSync(0x9117, 0), 1, 0)];
rc.activeTexture(0x84C0);
answers.push(rc.getParameter(0x8069));
rc.deleteBuffer(b2);
answers.push(rc.getParameter(0x8F37));
expect('shadow answers', answers, [true, 300, true, null, 7, 4, 0x84C3, true, 0x911A, null, null]);
expect('shadow: no wait for the page', log, []);
rc.getError();
expect('the calls themselves still go', log.map((c) => c[0]), ['bindBuffer', 'bufferData', 'bindFramebuffer', 'pixelStorei', 'activeTexture',
  'bindTexture', 'fenceSync', 'activeTexture', 'deleteBuffer']);

// Answers without JSON: a WebGL object by its number (the same stand-in each
// time), a typed array.
const o1 = rc.getParameter(0x1234), o2 = rc.getParameter(0x1234), vp = rc.getParameter(0x0BA2);
expect('object answer', [typeof o1.__rid, o1 === o2], ['number', true]);
expect('typed array answer', [vp.constructor.name, Array.from(vp)], ['Int32Array', [1, 2, 3, 4]]);

// getBufferSubData into memory the page cannot see (ndk_buffer_type_check's
// buffer): through the stream and back, the rest of the view kept.
const priv = new Uint8Array(16);
priv[15] = 77;
rc.getBufferSubData(G.ARRAY_BUFFER, 5, priv, 0, 8);
expect('getBufferSubData into a private view', Array.from(priv),
       [5, 8, 11, 14, 17, 20, 23, 26, 0, 0, 0, 0, 0, 0, 0, 77]);

expect('page warnings', errs.filter((e) => !/context|copies|frames read/.test(e)), []);
console.log(failures ? failures + ' FAILED' : 'all passed');
process.exit(failures ? 1 : 0);
