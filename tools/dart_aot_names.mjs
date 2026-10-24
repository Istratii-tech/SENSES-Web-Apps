// Names the Dart functions of a Flutter app's AOT code (libapp.so) at guest
// addresses, from the live Dart heap of a VM page in headless Chrome (topic
// flutter, docs/topics/flutter.md). For hangs in Dart code: the guest
// thread dumps (Module._ndk_cpu_dump_threads(), tools/headless_cdp.mjs eval)
// show only libapp.so!_kDartSnapshotText+OFF, and snapshot parsers (blutter)
// lag behind Dart versions (3.13 merged the VM and isolate snapshots) and
// know only arm64.
//
//   node tools/dart_aot_names.mjs CDP_PORT ADDR...
//
// ADDR (hex) = where libapp.so was loaded ("loaded .../libapp.so at 0x...")
// + the value of _kDartSnapshotText (nm -D libapp.so) + OFF. Prints
// Class.function+0xN for each.
//
// How: a precompiled Code object (32-bit, no compressed pointers) is
// tags, entry, monomorphic entry, unchecked entry, monomorphic unchecked
// entry, object pool, instructions, owner, ...; the owner is the Function
// (tags, entry, unchecked entry, name, owner class, ...) or a Class (name
// first) for stubs; a String is tags, hash, length (Smi), bytes. Guest
// addresses are wasm addresses, so the page's memory is scanned for those
// four entries (within 256 bytes of each other) under the class id of the
// ones whose owner Function has the same entry, and an address belongs to
// the Code with the nearest entry below it. Dart 3.x, armeabi-v7a; another
// layout prints "?".

const [port, ...addrs] = process.argv.slice(2);
if (!port || !addrs.length) {
  console.error('usage: dart_aot_names.mjs CDP_PORT ADDR...');
  process.exit(2);
}

const inPage = pcs => {
  const mem = typeof wasmMemory !== 'undefined' ? wasmMemory : Module.wasmMemory;
  const H = new Uint32Array(mem.buffer), B = new Uint8Array(mem.buffer);
  const str = p => {
    if (!(p & 1) || p < 0x10000 || p + 12 >= B.length) return null;
    const a = p - 1, len = H[a / 4 + 2];
    if (len & 1 || len === 0 || len > 4000) return null;
    let s = '';
    for (let i = 0; i < len >> 1; i++) {
      const c = B[a + 12 + i];
      if (c < 0x20 || c > 0x7e) return null;
      s += String.fromCharCode(c);
    }
    return s;
  };
  const field = (p, w) => (p & 1) && p > 0x10000 && p + 4 * w < B.length ? H[(p - 1) / 4 + w] : 0;
  if (!globalThis.__dartAotCodes) {
    const cand = [], cids = new Map();
    for (let i = 1, n = H.length - 8; i < n; i++) {
      const e0 = H[i];
      if (e0 < 0x10000 || e0 >= B.length) continue;
      const e1 = H[i + 1], e2 = H[i + 2], e3 = H[i + 3];
      if (Math.abs(e1 - e0) > 0x100 || Math.abs(e2 - e0) > 0x100 || Math.abs(e3 - e0) > 0x100) continue;
      if (!(H[i + 6] & 1)) continue;
      cand.push(i);
      // (votes only from Code whose owner Function has the same entry)
      const fe = field(H[i + 6], 1);
      if (fe !== e0 && fe !== e1 && fe !== e2 && fe !== e3) continue;
      const cid = H[i - 1] >>> 12;
      cids.set(cid, (cids.get(cid) || 0) + 1);
    }
    let codeCid = -1, most = 0;
    for (const [cid, k] of cids) if (k > most) { most = k; codeCid = cid; }
    const codes = [];
    for (const i of cand) {
      if (H[i - 1] >>> 12 !== codeCid) continue;
      const owner = H[i + 6];
      let fn = str(field(owner, 3)), cls;
      if (fn !== null) {
        const o = field(owner, 4);  // a Class, or a PatchClass (the class first)
        cls = str(field(o, 1)) ?? str(field(field(o, 1), 1));
      } else {
        cls = str(field(owner, 1));
      }
      codes.push([Math.min(H[i], H[i + 1], H[i + 2], H[i + 3]), cls, fn]);
    }
    codes.sort((a, b) => a[0] - b[0]);
    globalThis.__dartAotCodes = codes;
  }
  const C = globalThis.__dartAotCodes;
  const out = [`${C.length} Code objects`];
  for (const pc of pcs) {
    let lo = 0, hi = C.length - 1, best = -1;
    while (lo <= hi) {
      const m = (lo + hi) >> 1;
      if (C[m][0] <= pc) { best = m; lo = m + 1; } else hi = m - 1;
    }
    const c = C[best];
    out.push(pc.toString(16) + '  ' + (c && pc - c[0] < 0x100000 ?
        `${c[1] ?? '?'}.${c[2] ?? '?'}+0x${(pc - c[0]).toString(16)}` : '?'));
  }
  return out.join('\n');
};

const list = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
const ws = new WebSocket(list.find(t => t.type === 'page').webSocketDebuggerUrl);
await new Promise(r => ws.onopen = r);
let id = 0;
const pending = new Map(), contexts = [];
ws.onmessage = m => {
  const d = JSON.parse(m.data);
  if (d.id && pending.has(d.id)) { pending.get(d.id)(d); pending.delete(d.id); }
  if (d.method === 'Runtime.executionContextCreated') contexts.push(d.params.context);
};
const send = (method, params = {}) => new Promise(r => {
  const i = ++id;
  pending.set(i, r);
  ws.send(JSON.stringify({id: i, method, params}));
});
await send('Runtime.enable');
await new Promise(r => setTimeout(r, 300));
const pcs = addrs.map(a => parseInt(a, 16));
const expr = `(typeof Module !== 'undefined' && Module._ndk_cpu_dump_threads) ? ` +
    `(${inPage.toString()})(${JSON.stringify(pcs)}) : null`;
for (const c of contexts) {
  const r = await send('Runtime.evaluate', {expression: expr, contextId: c.id, returnByValue: true});
  const v = r.result && r.result.result;
  if (v && v.value) console.log(v.value);
  else if (r.result && r.result.exceptionDetails) console.error(r.result.exceptionDetails.text);
}
ws.close();
