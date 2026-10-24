// Drives a headless Chrome over the DevTools protocol (Node 22+: fetch and
// WebSocket are built in), for runs of the VM test page when no browser can
// stay visible (a hidden page pauses requestAnimationFrame and sends the app
// to the back). Start Chrome with a profile of its own, not the user's, on the
// Mac's own disk (on an external disk every OPFS request took seconds):
//
//   "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" --headless=new \
//       --user-data-dir=DIR --remote-debugging-port=9333 --window-size=540,960 \
//       --no-first-run --enable-unsafe-swiftshader "http://localhost:PORT/vm/index.html?..."
//
//   node tools/headless_cdp.mjs 9333 shot OUT.png      screenshot of the page
//   node tools/headless_cdp.mjs 9333 tap X Y           a click (the VM gets a touch)
//   node tools/headless_cdp.mjs 9333 drag X1 Y1 X2 Y2  press, move in 15 steps, release
//   node tools/headless_cdp.mjs 9333 eval 'JS'         in every frame (the VM is the
//                                                      iframe: Module._ndk_cpu_dump_threads())
//
// Coordinates are CSS pixels of the page, as in the screenshot.
import fs from 'fs';

const [port, cmd, ...rest] = process.argv.slice(2);
const list = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
const page = list.find(t => t.type === 'page');
const ws = new WebSocket(page.webSocketDebuggerUrl);
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
const sleep = ms => new Promise(r => setTimeout(r, ms));
const mouse = (type, x, y) => send('Input.dispatchMouseEvent',
    {type, x, y, button: 'left', buttons: type === 'mouseReleased' ? 0 : 1, clickCount: 1});
const n = rest.map(Number);

if (cmd === 'shot') {
  const r = await send('Page.captureScreenshot', {format: 'png'});
  fs.writeFileSync(rest[0], Buffer.from(r.result.data, 'base64'));
} else if (cmd === 'tap') {
  await mouse('mouseMoved', n[0], n[1]);
  await mouse('mousePressed', n[0], n[1]);
  await sleep(120);
  await mouse('mouseReleased', n[0], n[1]);
} else if (cmd === 'drag') {
  await mouse('mousePressed', n[0], n[1]);
  for (let i = 1; i <= 15; i++) {
    await sleep(40);
    await mouse('mouseMoved', n[0] + (n[2] - n[0]) * i / 15, n[1] + (n[3] - n[1]) * i / 15);
  }
  await sleep(100);
  await mouse('mouseReleased', n[2], n[3]);
} else if (cmd === 'eval') {
  await send('Runtime.enable');
  await sleep(300);
  for (const c of contexts) {
    const r = await send('Runtime.evaluate', {expression: rest[0], contextId: c.id,
                                              returnByValue: true, awaitPromise: true});
    const v = r.result && r.result.result;
    console.log(`[${c.origin} ${c.auxData && c.auxData.isDefault ? 'main' : 'other'} world]`,
                JSON.stringify(v && (v.value !== undefined ? v.value : v.description)));
  }
} else {
  console.error('usage: headless_cdp.mjs PORT shot FILE | tap X Y | drag X1 Y1 X2 Y2 | eval JS');
  process.exitCode = 2;
}
ws.close();
