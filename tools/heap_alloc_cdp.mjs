// JS allocation per thread of a page in a headless Chrome, over the DevTools
// protocol (Node 22+), topic gl-heap: the page, its frames and every worker
// (pthreads) sampled with HeapProfiler for SECONDS, objects later collected
// included, so the sum is what each thread allocated, garbage or not. Start
// Chrome as tools/headless_cdp.mjs says, then
//
//   node tools/heap_alloc_cdp.mjs PORT SECONDS [OUT.json]
//
// prints, per thread (the GL thread is the one whose GLctx is a stand-in of
// shim/gles/remote_webgl.js), MB allocated, MB per frame shown (the VM page's
// Module.arcwasmPresenter, the 2D display) and the top allocating functions.

import fs from 'fs';

const [port, secs = '20', out] = process.argv.slice(2);
const ver = await (await fetch(`http://127.0.0.1:${port}/json/version`)).json();
const ws = new WebSocket(ver.webSocketDebuggerUrl);
await new Promise(r => ws.onopen = r);
let id = 0;
const pending = new Map();
const sessions = new Map();  // sessionId -> {type, url, contexts}
const seen = new Set();  // targetIds: one session each (a sampling profiler is per isolate)
ws.onmessage = m => {
  const d = JSON.parse(m.data);
  if (d.id && pending.has(d.id)) { pending.get(d.id)(d); pending.delete(d.id); return; }
  if (d.method === 'Target.attachedToTarget') {
    const s = d.params.sessionId, t = d.params.targetInfo;
    if (seen.has(t.targetId)) {
      send('Target.detachFromTarget', {sessionId: s});
      return;
    }
    seen.add(t.targetId);
    sessions.set(s, {type: t.type, url: t.url, contexts: []});
    attach(s);
  } else if (d.method === 'Target.detachedFromTarget') {
    const s = sessions.get(d.params.sessionId);
    if (s) s.gone = true;
  } else if (d.method === 'Runtime.executionContextCreated' && d.sessionId) {
    const s = sessions.get(d.sessionId);
    if (s) s.contexts.push(d.params.context);
  }
};
// A thread blocked in Atomics.wait answers when it wakes, if ever: |ms| at
// most, then {timeout: true}.
const send = (method, params = {}, sessionId, ms = 10000) => new Promise(r => {
  const i = ++id;
  pending.set(i, r);
  ws.send(JSON.stringify(sessionId ? {id: i, method, params, sessionId} : {id: i, method, params}));
  setTimeout(() => { if (pending.has(i)) { pending.delete(i); r({timeout: true}); } }, ms);
});
const sleep = ms => new Promise(r => setTimeout(r, ms));

async function attach(s) {
  await send('Target.setAutoAttach', {autoAttach: true, waitForDebuggerOnStart: false, flatten: true}, s);
  await send('Runtime.enable', {}, s);
}

const targets = (await send('Target.getTargets')).result.targetInfos;
for (const t of targets.filter(t => t.type === 'page' && /^http/.test(t.url))) {
  await send('Target.attachToTarget', {targetId: t.targetId, flatten: true});
}
await sleep(3000);

async function evalIn(s, expr, ctx) {
  const r = await send('Runtime.evaluate', ctx ? {expression: expr, contextId: ctx, returnByValue: true}
                                               : {expression: expr, returnByValue: true}, s, 3000);
  return r.result && r.result.result ? r.result.result.value : undefined;
}

// Frames shown so far: in any context of the page that has the presenter.
async function frames() {
  for (const [s, info] of sessions) {
    if (info.type !== 'page' && info.type !== 'iframe') continue;
    for (const c of info.contexts) {
      const v = await evalIn(s, 'typeof Module != "undefined" && Module.arcwasmPresenter ? ' +
                             'Module.arcwasmPresenter.shown : -1', c.id);
      if (typeof v == 'number' && v >= 0) return v;
    }
  }
  return 0;
}

const WHO = 'typeof GLctx != "undefined" && GLctx && GLctx.arcRemote ? "GL thread" : ' +
    'typeof window != "undefined" ? "page " + location.pathname : "worker"';
const started = [];
await Promise.all([...sessions].filter(([s, info]) => !info.gone).map(async ([s]) => {
  const r = await send('HeapProfiler.startSampling', {samplingInterval: 8192,
      includeObjectsCollectedByMajorGC: true, includeObjectsCollectedByMinorGC: true}, s, 30000);
  if (r.result) started.push(s);
}));
console.error(`sampling ${started.length} of ${sessions.size} targets`);
const f0 = await frames();
const t0 = Date.now();
await sleep(+secs * 1000);
const f1 = await frames();
const dt = (Date.now() - t0) / 1000;
const nframes = f1 - f0;

const rows = [];
for (const s of started) {
  const info = sessions.get(s);
  let who = await evalIn(s, WHO, undefined);
  if (info.type === 'page' || info.type === 'iframe') {
    // A page's own context first (its default), then its frames'.
    who = [];
    for (const c of info.contexts) {
      const w = await evalIn(s, WHO, c.id);
      if (w) who.push(w);
    }
    who = [...new Set(who)].join(' + ') || info.type;
  }
  const r = await send('HeapProfiler.stopSampling', {}, s, 30000);
  if (!r.result) { console.error('no profile from', info.type, who, r.timeout ? '(timeout)' : r.error); continue; }
  const prof = r.result.profile;
  const self = new Map();
  let total = 0;
  for (const smp of prof.samples || []) total += smp.size;
  const bySite = new Map(), byPath = new Map();
  (function walk(n, path) {
    const cf = n.callFrame;
    const key = (cf.functionName || '(anon)') + ' ' + cf.url.replace(/^.*\//, '').replace(/\?.*$/, '') +
        ':' + (cf.lineNumber + 1);
    const here = path.concat(cf.functionName || '(anon)').slice(-4);
    if (n.selfSize) {
      bySite.set(key, (bySite.get(key) || 0) + n.selfSize);
      byPath.set(here.join(' > '), (byPath.get(here.join(' > ')) || 0) + n.selfSize);
    }
    for (const c of n.children || []) walk(c, here);
  })(prof.head, []);
  const paths = [...byPath].sort((a, b) => b[1] - a[1]).slice(0, 8)
      .map(([k, v]) => [k, +(v / 1048576).toFixed(2)]);
  const top = [...bySite].sort((a, b) => b[1] - a[1]).slice(0, 12)
      .map(([k, v]) => [k, +(v / 1048576).toFixed(2)]);
  rows.push({who: who || info.type, type: info.type, mb: +(total / 1048576).toFixed(2), top, paths});
}
rows.sort((a, b) => b.mb - a.mb);
const sum = rows.reduce((a, r) => a + r.mb, 0);
console.log(`${dt.toFixed(1)} s, ${nframes} frames shown (${(nframes / dt).toFixed(1)}/s), ` +
            `${rows.length} threads, ${sum.toFixed(1)} MB allocated in all` +
            (nframes ? `, ${(sum * 1024 / nframes).toFixed(1)} KB a frame` : ''));
for (const r of rows.slice(0, 15)) {
  console.log(`${r.mb.toFixed(2).padStart(8)} MB ${nframes ? (r.mb * 1024 / nframes).toFixed(1).padStart(7) + ' KB/frame' : ''}  ${r.who}`);
  for (const [k, v] of r.top.slice(0, r.mb > 1 ? 8 : 3)) console.log(`           ${v.toFixed(2).padStart(7)}  ${k}`);
}
if (out) fs.writeFileSync(out, JSON.stringify({secs: dt, frames: nframes, rows}, null, 1));
ws.close();
