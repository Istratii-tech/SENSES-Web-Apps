// The memory plan (topics mem-budget, device-mem-budget): the VM's budget and
// its wasm memory's ceiling, in MB, from what is known of the device. One
// function for both sides that need it: mem_budget_pre.js (--pre-js of the
// VMs, before the memory exists) and SENSES' run.html, which makes the VM's
// memory before it loads the VM (demo/apps/run.html, "memory"), so the two
// arrive at the same ceiling. A plain script: no Module, no ENV.
//
//   ArcMemPlan({mem, memcap,        given (?mem=, ?memcap=; Node ARC_MEM_BUDGET_MB, ARC_MEM_CAP_MB)
//               limitMb, threadMb,  measured: the browser closed a tab at limitMb, a
//                                   thread's worker costs threadMb (the library's
//                                   memory test, memtest.html: senses-mem-limit)
//               ios, deviceMemory}) -> {budget, cap, why, ios}
//
//   given      as given; the budget alone: the ceiling is the budget plus
//              the Java heap's reservation (untouched until used) plus
//              256 MB; the ceiling alone: the budget is 2/3 of it.
//   measured   the budget is what is left of the tab for the VM's memory in
//              use: the limit less the page (its script, the compiled module,
//              the JS of the system: PAGE_MB) and the workers of THREADS
//              threads (a busy app's count: run.html warns there too). The
//              ceiling follows the budget as above. On iOS never below the
//              plan without a measurement (1024/1536) when the tab holds at
//              least what that plan was found safe on (IOS_LIMIT): the test's
//              thread cost varies (12 Pro ~4.9 MB, iPhone X ~7.6 MB), and the
//              phones that close tabs at ~1.5 GB run well with it; below
//              that the plan goes down. iPhone X (1520 MB, 7.6) and 12 Pro
//              (1488, 4.9): 1024/1536, as without a measurement; a phone
//              that holds 2 GB: ~1424/1936 (7.6 MB a thread) to ~1584/2352
//              (5 MB); a desktop (the test stops at 8 GB): 3072/4096, as by
//              default.
//   iOS        (not measured) 1024 / 1536: a tab is closed at ~1.5 GB of
//              footprint on the phones of the last years (less on 3 GB ones).
//   deviceMemory (Chromium, Android) half of it, at most 3 GB.
//   otherwise  (Safari or Firefox on a Mac, Node) 3072 / 4096.
// Budgets stay within [MIN_BUDGET, 3072], ceilings within [512, 4096].
var ArcMemPlan = (function () {
  var PAGE_MB = 160, THREADS = 60, MIN_BUDGET = 384;
  // iOS without a measurement, and the tab limit it was found safe on
  // (iPhone X and 12 Pro, iOS 16; docs/topics/device-mem-budget.md).
  var IOS_BUDGET = 1024, IOS_CAP = 1536, IOS_LIMIT = 1488;
  // AOSP's heap size of a device of that much RAM (mem_budget.cc: Derive).
  function heapSize(mb) { return mb < 768 ? 128 : mb < 1536 ? 256 : 512; }
  function ceilingOf(budget) { return Math.min(4096, budget + heapSize(budget) + 256); }

  function plan(d) {
    d = d || {};
    var budget = +d.mem || 0, cap = +d.memcap || 0, why = 'given', ios = !!d.ios;
    if (!budget && !cap && +d.limitMb > 0) {
      var threadMb = +d.threadMb > 0 ? +d.threadMb : ios ? 5 : 2;
      budget = Math.floor((d.limitMb - PAGE_MB - THREADS * threadMb) / 16) * 16;
      // (room for more than the largest budget: a desktop, as by default)
      if (budget >= 3072) cap = 4096;
      budget = Math.max(MIN_BUDGET, Math.min(3072, budget));
      if (ios && d.limitMb >= IOS_LIMIT && budget < IOS_BUDGET) { budget = IOS_BUDGET; cap = IOS_CAP; }
      why = 'measured tab limit ' + Math.round(d.limitMb) + ' MB (' + threadMb.toFixed(1) + ' MB a thread)';
    } else if (!budget && !cap && ios) {
      budget = IOS_BUDGET; cap = IOS_CAP; why = 'iOS';
    } else if (!budget && !cap && +d.deviceMemory > 0) {
      budget = Math.min(3072, d.deviceMemory * 1024 / 2); why = 'deviceMemory';
    }
    if (!budget && !cap) { budget = 3072; cap = 4096; why = 'default'; }
    if (!cap) cap = ceilingOf(budget);
    if (!budget) budget = Math.floor(cap * 2 / 3);
    cap = Math.max(512, Math.min(4096, cap));
    return {budget: budget, cap: cap, why: why, ios: ios};
  }
  // The test result as stored (senses-mem-limit: {mb, threadMb, threads,
  // at, ua}; ?memlimit= may also be a bare number of MB) -> {mb, threadMb} or null.
  plan.limit = function (v) {
    if (typeof v == 'string') { try { v = JSON.parse(v); } catch (e) { v = +v; } }
    if (typeof v == 'number') v = {mb: v};
    return v && +v.mb > 0 ? {mb: +v.mb, threadMb: +v.threadMb || 0} : null;
  };
  // Whether the user agent is an iPhone's or iPad's (an iPad asks for the Mac's).
  plan.isIOS = function (ua, touchPoints) {
    return /iPhone|iPad|iPod/.test(ua || '') || (/Macintosh/.test(ua || '') && touchPoints > 1);
  };
  return plan;
})();
