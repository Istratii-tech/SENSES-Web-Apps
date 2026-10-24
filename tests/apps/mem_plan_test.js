// The VM's memory plan (shim/membudget/mem_plan.js, topic device-mem-budget):
// budget and wasm memory ceiling from what is given, the device's measured
// tab limit, or the device's kind.
//
//   node tests/apps/mem_plan_test.js
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const ROOT = path.resolve(__dirname, '../..');
const ctx = {};
vm.runInNewContext(fs.readFileSync(path.join(ROOT, 'shim/membudget/mem_plan.js'), 'utf8') +
                   '\nthis.ArcMemPlan = ArcMemPlan;', ctx);
const P = ctx.ArcMemPlan;

let passed = 0, failed = 0;
function check(name, got, want) {
  const ok = Object.keys(want).every((k) => got[k] === want[k]);
  if (ok) passed++; else failed++;
  console.log((ok ? '[ OK ] ' : '[FAIL] ') + name + ': ' + JSON.stringify(got) +
              (ok ? '' : ' want ' + JSON.stringify(want)));
}
const IPHONE = 'Mozilla/5.0 (iPhone; CPU iPhone OS 16_7 like Mac OS X) AppleWebKit/605.1.15';
const IPAD = 'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15';

// As before this topic.
check('Node, nothing given', P({}), {budget: 3072, cap: 4096});
check('?mem=1024', P({mem: '1024'}), {budget: 1024, cap: 1536});
check('?mem=1024&memcap=1536', P({mem: 1024, memcap: 1536}), {budget: 1024, cap: 1536});
check('?memcap=1536', P({memcap: 1536}), {budget: 1024, cap: 1536});
check('iPhone, not measured', P({ios: true}), {budget: 1024, cap: 1536, why: 'iOS'});
check('Android, deviceMemory 4', P({deviceMemory: 4}), {budget: 2048, cap: 2816});
check('Android, deviceMemory 8', P({deviceMemory: 8}), {budget: 3072, cap: 3840});
// Measured.
check('iPhone 12 Pro (1488 MB, 288 threads)', P({ios: true, limitMb: 1488, threadMb: (1488 - 80) / 288}),
      {budget: 1024, cap: 1536});
check('iPhone X (1520 MB, 189 threads: 7.6 MB each)', P({ios: true, limitMb: 1520, threadMb: (1520 - 80) / 189}),
      {budget: 1024, cap: 1536});
check('iPhone, 2048 MB, 7.6 MB a thread', P({ios: true, limitMb: 2048, threadMb: 7.6}), {budget: 1424, cap: 1936});
check('iPhone, 1400 MB, 7.6 MB a thread: lower', P({ios: true, limitMb: 1400, threadMb: 7.6}), {budget: 784, cap: 1296});
check('not iOS, 1520 MB, 7.6 MB a thread: as measured', P({limitMb: 1520, threadMb: 7.6}), {budget: 896, cap: 1408});
check('iPhone, 1488 MB, thread cost not measured', P({ios: true, limitMb: 1488}), {budget: 1024, cap: 1536});
check('iPhone, 2048 MB, 5 MB a thread', P({ios: true, limitMb: 2048, threadMb: 5}), {budget: 1584, cap: 2352});
check('iPhone, 3 GB, 5 MB a thread', P({ios: true, limitMb: 3072, threadMb: 5}), {budget: 2608, cap: 3376});
check('3 GB phone closing at 1000 MB', P({ios: true, limitMb: 1000, threadMb: 5}), {budget: 528, cap: 912});
check('tiny limit', P({ios: true, limitMb: 400, threadMb: 5}), {budget: 384, cap: 768});
check('a desktop (never closed: 8192)', P({limitMb: 8192, threadMb: 13.5}), {budget: 3072, cap: 4096});
check('given wins over measured', P({mem: 1024, limitMb: 2048, threadMb: 5}), {budget: 1024, cap: 1536});
// The stored result.
check('limit of the stored JSON', P.limit('{"mb":1488,"threads":288,"threadMb":4.9}') || {}, {mb: 1488, threadMb: 4.9});
check('limit of a bare number', P.limit('2048') || {}, {mb: 2048, threadMb: 0});
check('no limit', {v: P.limit(null)}, {v: null});
check('iPad asks for a Mac', {v: P.isIOS(IPAD, 5)}, {v: true});
check('a Mac', {v: P.isIOS(IPAD, 0)}, {v: false});
check('iPhone', {v: P.isIOS(IPHONE, 5)}, {v: true});

console.log(passed + ' passed, ' + failed + ' failed');
process.exit(failed ? 1 : 0);
