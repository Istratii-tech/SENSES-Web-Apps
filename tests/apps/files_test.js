// Files between apps and the phone, the page's side (topic app-files,
// docs/topics/app-files.md), under Node: the store worker's file operations
// (demo/apps/store-worker.js on shim/opfs/memory_opfs.js) and demo/apps/files.js
// on a small fake DOM -- the MIME filter of the picker, a pick sent into the
// VM in pieces as the VM acknowledges them ("SENSES: file got"), a cancel, a
// file the app saves offered for download, text shared, the library's
// «Файлы» (list, add, remove). The VM's side is AppFilesTest10 (tests/java10).
//
//   node tests/apps/files_test.js
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const ROOT = path.resolve(__dirname, '../..');
const APPS = path.join(ROOT, 'demo/apps');
let passed = 0, failed = 0;
async function check(name, fn) {
  try {
    await fn();
    passed++;
    console.log('[ OK ] ' + name);
  } catch (e) {
    failed++;
    console.log('[FAIL] ' + name + ': ' + (e && e.stack || e));
  }
}
function eq(want, got, what) {
  const a = JSON.stringify(want), b = JSON.stringify(got);
  if (a !== b) throw new Error((what ? what + ': ' : '') + 'expected ' + a + ' but was ' + b);
}
function ok(cond, what) { if (!cond) throw new Error(what); }
const tick = () => new Promise((r) => setTimeout(r, 5));
async function until(cond, what) {
  for (let i = 0; i < 400; i++) { if (cond()) return; await tick(); }
  throw new Error('waited for ' + what);
}

// --- the store worker, in this process (as tests/apps/apks_split_test.js) ---
globalThis.self = globalThis;
self.location = {search: '', href: 'https://senses.test/', origin: 'https://senses.test'};
require(path.join(ROOT, 'shim/opfs/memory_opfs.js'));
self.importScripts = (f) => vm.runInThisContext(fs.readFileSync(path.join(APPS, f), 'utf8'), {filename: f});
const pending = new Map();
let nextId = 1;
self.postMessage = (m) => {
  const p = pending.get(m.id);
  if (!p) return;
  if (m.progress) { if (p.progress) p.progress(m.progress[0], m.progress[1]); return; }
  pending.delete(m.id);
  if (m.ok) p.resolve(m.result);
  else { const e = new Error(m.error.message); e.name = m.error.name; p.reject(e); }
};
vm.runInThisContext(fs.readFileSync(path.join(APPS, 'store-worker.js'), 'utf8'), {filename: 'store-worker.js'});
const workerOnMessage = self.onmessage;
function call(op, a, progress) {
  return new Promise((resolve, reject) => {
    const id = nextId++;
    pending.set(id, {resolve, reject, progress});
    workerOnMessage({data: {id, op, args: a}});
  });
}
let free = 10 * 1024 * 1048576;
self.SensesStore = {
  usage: () => Promise.resolve({usage: 0, quota: free}),
  storage: () => Promise.resolve({free}),
  listDir: (p) => call('listDir', {path: p}),
  fileAt: (p, name, type) => call('fileAt', {path: p, name, type}),
  putFile: (dir, file, progress) => call('putFile', {dir, file}, progress),
  removePath: (p) => call('removePath', {path: p}),
};

// --- a small DOM: what senses.js and files.js use ---
class El {
  constructor(tag) {
    this.tagName = tag.toUpperCase(); this.children = []; this.parentNode = null; this.style = {};
    this.dataset = {}; this.attrs = {}; this.listeners = {}; this._text = ''; this.hidden = false; this.id = '';
    const cls = new Set();
    this.classList = {add: (c) => cls.add(c), remove: (c) => cls.delete(c), contains: (c) => cls.has(c),
                      toggle: (c, on) => { if (on === undefined ? !cls.has(c) : on) cls.add(c); else cls.delete(c); }};
    Object.defineProperty(this, 'className', {get: () => [...cls].join(' '),
                                              set: (v) => { cls.clear(); String(v).split(/\s+/).filter(Boolean).forEach((c) => cls.add(c)); }});
  }
  get firstChild() { return this.children[0] || null; }
  get textContent() { return this._text + this.children.map((c) => c.textContent).join(''); }
  set textContent(v) { this.children.forEach((c) => { c.parentNode = null; }); this.children = []; this._text = String(v); }
  appendChild(c) { if (c.parentNode) c.remove(); c.parentNode = this; this.children.push(c); return c; }
  remove() { if (this.parentNode) { const p = this.parentNode; p.children = p.children.filter((c) => c !== this); this.parentNode = null; } }
  setAttribute(k, v) { this.attrs[k] = String(v); }
  hasAttribute(k) { return k in this.attrs; }
  addEventListener(t, f) { (this.listeners[t] = this.listeners[t] || []).push(f); }
  click() { if (this.onclick) this.onclick({target: this}); (this.listeners.click || []).forEach((f) => f({target: this})); }
  find(pred) {
    if (pred(this)) return this;
    for (const c of this.children) { const r = c.find(pred); if (r) return r; }
    return null;
  }
  select() {}
}
const body = new El('body');
globalThis.document = {
  body, createElement: (t) => new El(t), addEventListener() {}, execCommand: () => true,
  getElementById: (id) => body.find((e) => e.id === id),
};
globalThis.window = globalThis;
globalThis.matchMedia = () => ({matches: true});  // a desktop: a fine pointer
const downloads = [];
vm.runInThisContext(fs.readFileSync(path.join(APPS, 'senses.js'), 'utf8'), {filename: 'senses.js'});
vm.runInThisContext(fs.readFileSync(path.join(APPS, 'files.js'), 'utf8'), {filename: 'files.js'});
const F = self.SensesFiles, $ = (id) => document.getElementById(id);
const pickers = [];  // the <input type=file> each "Выбрать" made, as clicked
const origCreate = document.createElement;
document.createElement = (t) => {
  const e = origCreate(t);
  if (t === 'input') e.click = () => pickers.push(e);
  if (t === 'a') e.click = () => downloads.push({name: e.download, href: e.href});
  return e;
};

(async () => {
  await check('the picker\'s filter: wildcards and known types, any file for unknown ones', async () => {
    eq('image/*', F.acceptFor(['image/*']));
    eq('application/pdf,.pdf', F.acceptFor(['application/pdf']));
    eq('image/*,application/pdf,.pdf', F.acceptFor(['image/*', 'application/pdf']));
    eq('', F.acceptFor(['*/*']));
    eq('', F.acceptFor(['application/octet-stream']));
    eq('', F.acceptFor(['application/x-unknown-thing']));
    eq('изображение', F.describe(['image/*']));
    eq('PDF, TXT (текст)', F.describe(['application/pdf', 'text/plain']));
    eq('любой файл', F.describe(['application/pdf', 'application/octet-stream']));
    eq('любой файл', F.describe(['application/x-weird']));
  });

  await check('store worker: a file into a system\'s Download (free names), listed, read back as a File, '
              + 'removed; nothing removed outside /sdcard', async () => {
    const dir = 'senses/data/a10/media/0/Download/';
    eq('a.txt', await call('putFile', {dir, file: new File(['hello'], 'a.txt', {type: 'text/plain'})}));
    eq('a (2).txt', await call('putFile', {dir, file: new File(['hello!'], 'a.txt')}));
    eq('x_y.bin', await call('putFile', {dir, file: new File(['1'], 'x/y.bin')}));
    const list = (await call('listDir', {path: dir})).map((e) => e.name + ':' + e.kind + ':' + e.size).sort();
    eq(['a (2).txt:file:6', 'a.txt:file:5', 'x_y.bin:file:1'], list);
    eq(['Download:directory:0'], (await call('listDir', {path: 'senses/data/a10/media/0/'})).map((e) => e.name + ':' + e.kind + ':' + e.size));
    const f = await call('fileAt', {path: dir + 'a (2).txt', name: 'a (2).txt', type: 'text/plain'});
    ok(f instanceof File, 'not a File');
    eq(['a (2).txt', 'text/plain', 'hello!'], [f.name, f.type, await f.text()]);
    await call('removePath', {path: dir + 'x_y.bin'});
    eq(2, (await call('listDir', {path: dir})).length);
    let refused = null;
    await call('removePath', {path: 'senses/apps/x'}).catch((e) => { refused = e.message; });
    ok(/not in shared storage/.test(refused || ''), 'removed outside: ' + refused);
    eq([], await call('listDir', {path: 'senses/data/a81/media/0/'}));
  });

  const sent = [];
  let accepted = true;
  const app = F.app({
    send: (name, bytes) => { if (!accepted) return false; sent.push({name, bytes: Uint8Array.from(bytes)}); return true; },
    dataDir: () => 'senses/data/a10', name: () => 'Notes',
  });

  await check('open: a pick shows the sheet with its own button; the file goes in 1 MB pieces, at most 3 '
              + 'beyond what the VM acknowledged, then "end"; done closes the sheet', async () => {
    ok(app.watch('SENSES: file pick p1 0 application/pdf'), 'not taken');
    ok(!$('filepick').hidden, 'sheet hidden');
    eq('«Notes» просит файл: PDF.', $('fpnote').textContent);
    $('fpchoose').click();
    eq(1, pickers.length);
    eq(['application/pdf,.pdf', false], [pickers[0].accept, pickers[0].multiple]);
    const size = 5 * 1048576 + 123, data = new Uint8Array(size);
    for (let i = 0; i < size; i++) data[i] = (i * 7) & 255;
    pickers[0].files = [new File([data], 'book.pdf', {type: 'application/pdf'})];
    pickers[0].onchange();
    await until(() => sent.length >= 4, 'begin and 3 pieces');
    await tick();
    eq('arcwasm-files:begin:p1', sent[0].name);
    eq([{name: 'book.pdf', type: 'application/pdf', size}], JSON.parse(new TextDecoder().decode(sent[0].bytes)));
    eq(['arcwasm-files:part:p1:0:0', 'arcwasm-files:part:p1:0:1048576', 'arcwasm-files:part:p1:0:2097152'],
       sent.slice(1).map((s) => s.name), 'window');
    let acked = 0;
    while (true) {
      const parts = sent.filter((s) => /:part:/.test(s.name));
      const total = parts.reduce((n, s) => n + s.bytes.length, 0);
      if (acked >= size) break;
      acked = Math.min(total, acked + 1048576);
      app.watch('SENSES: file got p1 ' + acked);
      await tick(); await tick();
    }
    await until(() => sent[sent.length - 1].name === 'arcwasm-files:end:p1', 'end');
    const parts = sent.filter((s) => /:part:/.test(s.name));
    eq(6, parts.length);
    const back = new Uint8Array(size);
    parts.forEach((s) => back.set(s.bytes, +s.name.split(':').pop()));
    ok(back.every((b, i) => b === data[i]), 'bytes differ');
    app.watch('SENSES: file done p1 1');
    ok($('filepick').hidden, 'sheet still open');
  });

  await check('open: no room (Safari\'s quota) is said, nothing is sent; then the user cancels', async () => {
    sent.length = 0;
    free = 100 * 1048576;
    app.watch('SENSES: file pick p2 1 image/*');
    eq('Выбрать файлы', $('fpchoose').textContent);
    $('fpchoose').click();
    const input = pickers[pickers.length - 1];
    eq(['image/*', true], [input.accept, input.multiple]);
    input.files = [new File([new Uint8Array(50 * 1048576)], 'big.jpg', {type: 'image/jpeg'})];
    input.onchange();
    await until(() => /Не хватает места/.test($('fpstatus').textContent), 'no space');
    eq(0, sent.length);
    $('fpcancel').click();
    eq(['arcwasm-files:cancel:p2'], sent.map((s) => s.name));
    ok($('filepick').hidden, 'sheet still open');
    free = 10 * 1024 * 1048576;
  });

  await check('save: a file the app wrote is read from the VM\'s storage and downloaded (desktop, no '
              + 'share sheet, no save dialog)', async () => {
    await call('putFile', {dir: 'senses/data/a10/senses-files/out/s1/', file: new File(['saved!'], 'report.txt')});
    app.watch('SENSES: file save s1 6 text/plain /data/senses-files/out/s1/report.txt');
    ok(!$('filesave').hidden, 'sheet hidden');
    await until(() => $('fsgo').textContent === 'Скачать', 'ready');
    ok($('fsalt').hidden, 'second download button');
    eq('report.txt', $('fslist').children[0].children[0].textContent);
    $('fsgo').click();
    await until(() => downloads.length === 1, 'download');
    eq('report.txt', downloads[0].name);
  });

  await check('save on a phone: the share sheet with the file («Сохранить в Файлы»)', async () => {
    const shared = [];
    navigator.canShare = (d) => !!(d && d.files && d.files.length);
    navigator.share = (d) => { shared.push(d); return Promise.resolve(); };
    Object.defineProperty(navigator, 'maxTouchPoints', {value: 5, configurable: true});
    globalThis.matchMedia = () => ({matches: false});
    try {
      $('filesave').hidden = true;
      app.watch('SENSES: file save s1 6 text/plain /data/senses-files/out/s1/report.txt');
      await until(() => $('fsgo').textContent === 'Сохранить в Файлы', 'ready');
      ok(!$('fsalt').hidden, 'no download button besides');
      $('fsgo').click();
      await until(() => shared.length === 1, 'shared');
      eq(['report.txt'], shared[0].files.map((f) => f.name));
      await until(() => $('filesave').hidden, 'sheet closed');
    } finally {
      globalThis.matchMedia = () => ({matches: true});
      delete navigator.canShare;
      delete navigator.share;
      Object.defineProperty(navigator, 'maxTouchPoints', {value: 0, configurable: true});
    }
  });

  await check('text: shared text is shown and copied', async () => {
    let copied = null;
    Object.defineProperty(navigator, 'clipboard', {value: {writeText: (t) => { copied = t; return Promise.resolve(); }},
                                                   configurable: true});
    app.watch('SENSES: file text t1 hi%20there%0Aline%202');
    eq('hi there\nline 2', $('fstext').textContent);
    eq('Скопировать', $('fsgo').textContent);
    $('fsgo').click();
    await until(() => copied !== null, 'copied');
    eq('hi there\nline 2', copied);
  });

  await check('library «Файлы»: Android 10\'s /sdcard/Download listed, a file added from the phone, '
              + 'removed after a second tap', async () => {
    F.library({remote: 0});
    await until(() => $('fllist').children.length && !/Читаю/.test($('fllist').textContent), 'list');
    eq('Android 10 · /sdcard/Download', $('flwhere').textContent);
    eq(['a (2).txt', 'a.txt'], $('fllist').children.map((li) => li.children[0].textContent));
    const box = $('files').children[0];
    box.children.find((b) => b.textContent === 'Добавить файлы с телефона').click();
    const input = pickers[pickers.length - 1];
    eq(true, input.multiple);
    input.files = [new File(['photo'], 'photo.jpg', {type: 'image/jpeg'})];
    input.onchange();
    await until(() => /Файл добавлен/.test($('flstatus').textContent), 'added');
    await until(() => $('fllist').children.length === 3, 'listed');
    const li = $('fllist').children.find((x) => x.children[0].textContent === 'photo.jpg');
    li.onclick({target: li});
    const del = li.find((e) => e.textContent === 'Удалить');
    del.click();
    eq('Точно удалить?', del.textContent);
    del.click();
    await until(() => $('fllist').children.length === 2, 'removed');
    $('flup').click();
    await until(() => /\/sdcard$/.test($('flwhere').textContent) && !/Читаю/.test($('fllist').textContent), 'up');
    eq('Download/', $('fllist').children[0].children[0].textContent);
  });

  console.log('files_test: ' + passed + ' passed, ' + failed + ' failed');
  process.exit(failed);
})();
