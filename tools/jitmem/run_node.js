// node tools/jitmem/run_node.js MODE THREADS N K OPS CALLS [TBL [REP]]: the same as
// index.html under V8 (worker_threads); prints RSS before and after.
const path = require('path');
const { Worker } = require('worker_threads');
require(path.join(__dirname, 'gen.js'));
const [mode = 'each', T = 8, n = 2000, k0 = 64, ops = 24, calls = 200, tblArg = 0, rep = 0] = process.argv.slice(2).map((v, i) => i ? +v : v);
const k = mode === 'each' ? 1 : k0;
const tbl = !!tblArg;
const mem = new WebAssembly.Memory({ initial: 256, maximum: 65536, shared: true });
let modules = null;
const t0 = Date.now();
if (mode === 'shared') {
  modules = [];
  for (let first = 0; first < n; first += k) modules.push(new WebAssembly.Module(jitmemModule(first, Math.min(k, n - first), ops, true, tbl)));
}
const list = [];
let total = 0;
if (mode === 'each' || mode === 'batch')
  for (let f = 0; f < n; f += k) { const b = jitmemModule(f, Math.min(k, n - f), ops, true, tbl); list.push(b); total += b.length; }
const bytes = new SharedArrayBuffer(Math.max(total, 8)), offsets = [0], view = new Uint8Array(bytes);
let at = 0;
for (const b of list) { view.set(b, at); at += b.length; offsets.push(at); }
list.length = 0;
const rss0 = process.memoryUsage().rss;
let done = 0;
const res = [];
const ws = [];
for (let i = 0; i < T; i++) {
  const w = new Worker(path.join(__dirname, 'worker.js'));
  ws.push(w);
  w.on('message', (m) => {
    res.push(m);
    if (++done === T) {
      setTimeout(() => {
        const rss = process.memoryUsage().rss;
        const avg = (f) => Math.round(res.reduce((a, r) => a + r[f], 0) / T);
        console.log(`${mode} T=${T} n=${n} k=${k} ops=${ops} calls=${calls} tbl=${tbl} rep=${rep}: rss ${Math.round(rss / 1048576)} MB ` +
          `(+${Math.round((rss - rss0) / 1048576)}), setup ${avg('setupMs')} ms, calls ${avg('callMs')} ms, total ${Date.now() - t0} ms`);
        process.exit(0);
      }, 500);
    }
  });
  w.postMessage({ id: i, mode, memory: mem, n, k, ops, calls, cpu: 1024, tbl, rep, modules, bytes, offsets });
}
