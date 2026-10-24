#!/usr/bin/env node
// Prints the JS/wasm stack of every thread of a Node program started with
// --inspect=127.0.0.1:PORT (artvm.js, dalvikvm.js: each pthread is a worker):
// for hangs, where no thread can print anything itself. Each worker is
// paused through the inspector (V8 takes the pause even inside Atomics.wait,
// i.e. a blocked pthread_mutex_lock/futex) and resumed after its stack is read.
//
//   node --inspect=127.0.0.1:9230 artvm.js ...   &
//   node tools/node_thread_stacks.mjs 9230 [FRAMES [PAUSE_MS]]
//
// Lines: "worker N (title):" then its frames, innermost first (wasm function
// names come from --profiling-funcs builds).
const port = process.argv[2] || '9229';
const maxFrames = parseInt(process.argv[3] || '40', 10);
const pauseMs = parseInt(process.argv[4] || '3000', 10);

const targets = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
const ws = new WebSocket(targets[0].webSocketDebuggerUrl);
await new Promise((ok, fail) => { ws.onopen = ok; ws.onerror = fail; });

let nextId = 1;
const pending = new Map();
const workers = [];
const workerWaiters = new Map();  // sessionId -> {id -> resolve}, paused callbacks
function send(method, params = {}) {
  const id = nextId++;
  ws.send(JSON.stringify({ id, method, params }));
  return new Promise((ok) => pending.set(id, ok));
}
function sendToWorker(sessionId, method, params = {}) {
  const id = nextId++;
  const w = workerWaiters.get(sessionId);
  const p = new Promise((ok) => w.replies.set(id, ok));
  send('NodeWorker.sendMessageToWorker',
       { sessionId, message: JSON.stringify({ id, method, params }) });
  return p;
}
ws.onmessage = (e) => {
  const m = JSON.parse(e.data);
  if (m.id && pending.has(m.id)) {
    pending.get(m.id)(m.result);
    pending.delete(m.id);
  } else if (m.method === 'NodeWorker.attachedToWorker') {
    workers.push(m.params);
    workerWaiters.set(m.params.sessionId, { replies: new Map(), paused: null });
  } else if (m.method === 'NodeWorker.receivedMessageFromWorker') {
    const w = workerWaiters.get(m.params.sessionId);
    const inner = JSON.parse(m.params.message);
    if (inner.id && w.replies.has(inner.id)) {
      w.replies.get(inner.id)(inner.result);
      w.replies.delete(inner.id);
    } else if (inner.method === 'Debugger.paused' && w.paused) {
      w.paused(inner.params);
    }
  }
};

await send('NodeWorker.enable', { waitForDebuggerOnStart: false });
await new Promise((ok) => setTimeout(ok, 4000));  // attachedToWorker events
console.log(`${workers.length} workers`);
for (const [i, info] of workers.entries()) {
  const w = workerWaiters.get(info.sessionId);
  await sendToWorker(info.sessionId, 'Debugger.enable');
  const paused = new Promise((ok) => { w.paused = ok; });
  await sendToWorker(info.sessionId, 'Debugger.pause');
  const r = await Promise.race([paused, new Promise((ok) => setTimeout(() => ok(null), pauseMs))]);
  console.log(`worker ${i} (${info.workerInfo.title}):`);
  if (!r) {
    console.log('  (did not pause in time: busy in wasm without interrupt checks?)');
  } else {
    for (const f of r.callFrames.slice(0, maxFrames)) {
      console.log(`  ${f.functionName || '(anonymous)'}`);
    }
  }
  await sendToWorker(info.sessionId, 'Debugger.resume');
  await sendToWorker(info.sessionId, 'Debugger.disable');
}
ws.close();
process.exit(0);
