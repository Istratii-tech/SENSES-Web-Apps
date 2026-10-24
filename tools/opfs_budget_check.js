// The OPFS thread's storage budget (shim/opfs/library_arc_opfs.js, topic
// opfs-budget) against a model of WebKit 7615's quota for sync access
// handles (QuotaManager, FileSystemStorageHandle: capacity is the file's size
// in the worker and none granted in the network process at open; a write
// past it asks for capacityFor(end) less what was granted; over the quota
// the request never returns, a "stall"). Random writes, truncates, closes and
// reopens of a few files on origins with 0-1000 MB used by others, through
// the real ArcOpfs.growing()/capped()/setSize(). Fails on any stall, or on a
// refusal where the browser had MARGIN + 8 MB to spare.
//   node tools/opfs_budget_check.js [seed]
const fs = require('fs');
let lib;
global.addToLibrary = (o) => { lib = o; };
global.ERRNO_CODES = {ENOSPC: 28, EBADF: 8, EISDIR: 21};
let logs = [];
global.err = (t) => logs.push(t);
eval(fs.readFileSync(require('path').join(__dirname, '../shim/opfs/library_arc_opfs.js'), 'utf8'));
const A = lib.$ArcOpfs;
global.ArcOpfs = A;
const MB = 1048576;
function capFor(n) {
  if (n < MB) return MB;
  if (n < 256 * MB) return Math.pow(2, Math.floor(Math.log2(n)) + 1);
  return 128 * MB * (Math.floor(n / (128 * MB)) + 1);
}
let seed = +process.argv[2] || 1;
const rnd = () => { seed = (seed * 1103515245 + 12345) % 2147483648; return seed / 2147483648; };
let stalls = 0, enospc = 0, wouldHaveFit = 0, writes = 0;
for (let round = 0; round < 300; round++) {
  const quota = 1000 * MB;
  const other = Math.floor(rnd() * 900) * MB;         // caches etc.
  const files = [];                                   // {size, open: {wcap, bcap} | null}
  let disk = other;
  for (let i = 0; i < 6; i++) { const s = Math.floor(rnd() * 80 * MB); files.push({size: s, open: null}); disk += s; }
  const usage = () => disk + files.reduce((a, f) => a + (f.open ? Math.max(0, f.open.bcap - f.size) : 0), 0);
  const free0 = quota - usage();
  A.budgetParsed = {free: Math.floor(free0 / MB) * MB, grown: 0, told: false, refused: 0};
  A.storage = 'opfs';
  A.cappedEntries.clear();
  const entries = files.map((f) => ({size: f.size, f}));
  for (let step = 0; step < 200; step++) {
    const i = Math.floor(rnd() * files.length), f = files[i], e = entries[i];
    const r = rnd();
    if (!f.open && r < 0.9) { f.open = {wcap: f.size, bcap: 0}; A.capped(e); }
    if (r < 0.1 && f.open) { f.open = null; A.uncapped(e); continue; }       // evicted / closed
    if (!f.open) continue;
    const trunc = rnd() < 0.15;
    const len = Math.floor(rnd() * (rnd() < 0.1 ? 64 : 2) * MB) + 1;
    const at = rnd() < 0.7 ? f.size : Math.floor(rnd() * (f.size + 1));
    const to = trunc ? Math.floor(rnd() * (f.size + 8 * MB)) : at + len;
    // the browser's side
    const browser = () => {
      if (trunc) { disk += to - f.size; f.size = to; return; }
      if (to > f.open.wcap) {
        const nc = capFor(to), ask = nc - f.open.bcap;
        if (ask > quota - usage()) { stalls++; throw new Error('STALL'); }
        f.open.wcap = f.open.bcap = nc;
      }
      if (to > f.size) { disk += to - f.size; f.size = to; }
    };
    writes++;
    try {
      A.growing(e, '/f' + i, to, browser, trunc);
      if (trunc) A.setSize(e, to); else if (to > e.size) A.setSize(e, to);
    } catch (x) {
      if (x === 28) {
        enospc++;
        // would the browser have granted it with room to spare?
        if (!trunc && to > f.open.wcap && capFor(to) - f.open.bcap < quota - usage() - A.BUDGET_MARGIN - 8 * MB) wouldHaveFit++;
        continue;
      }
      if (x.message === 'STALL') break;
      throw x;
    }
    if (e.size !== f.size) throw new Error('size drift ' + e.size + ' ' + f.size);
    if (e.wcap !== f.open.wcap) throw new Error('cap drift');
  }
}
console.log('opfs budget: ' + writes + ' writes and truncates, ' + enospc + ' ENOSPC, ' + stalls +
            ' stalls, ' + wouldHaveFit + ' refused with room to spare' + (logs[0] ? '; first: ' + logs[0] : ''));
process.exit(stalls || wouldHaveFit ? 1 : 0);
