// Packs of translated blocks per thread (topic shared-translations): see
// jit_pack.h. Built into both build/libndk_cpu.a and build/libndk_cpu64.a
// (each can be linked alone); with both, the linker takes one copy.
#include "ndk/jit_pack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <memory>
#include <mutex>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include "membudget/mem_budget.h"

#ifndef EMSCRIPTEN_KEEPALIVE
#define EMSCRIPTEN_KEEPALIVE
#endif
// For the page's log (demo/senses-apps-vm/run.html, every 5 s), all
// threads: packs compiled, of them in a cold pack's place, functions in
// them, KB of their modules; bodies made, bodies instantiated on some
// thread (topic jit-module-heap: functions / those = copies per body);
// functions instantiated now (live: not replaced, their thread not gone);
// their estimated cost to JavaScriptCore, KB (Cost, topic arm-chains);
// packs a thread took from the page's catalog instead of compiling them.
extern "C" EMSCRIPTEN_KEEPALIVE uint32_t arc_cpujit_counts[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};

namespace ndkjit {

struct Pack {
  int32_t base;   // first of its kPackMax table slots
  uint8_t n;
  uint32_t cost;  // of its functions (Cost)
  int32_t ids[kPackMax];
};

namespace {

// A pack waits at most this many runs of its blocks (on its thread) before
// it is compiled with the blocks it has: a lone hot loop is translated
// after ~1 ms of interpretation, a start-up's thousands of new blocks go 32
// to a module.
constexpr uint32_t kSealRuns = 8192;
// A cold pack: none of its blocks ran for this many runs of translated
// blocks on the thread (a busy thread runs ~10 million a second: 1 << 22
// made the start's packs cold within a second, and JavaScriptCore keeps a
// replaced instance until a full collection, which a thread running wasm
// seldom starts: tools/jitmem rep=, topic jit-module-heap).
constexpr uint32_t kColdRuns = 1u << 26;
// Functions live on all threads (arc_cpujit_counts[6]), and their cost
// (Cost; [7]), against the budget's cpu_jit_total (of kNominalCost each).
std::atomic<int32_t> g_live{0};
std::atomic<int64_t> g_live_cost{0};

// MB of JS garbage a thread makes after replacing packs (every 256
// functions dropped; ARC_CPU_JIT_CHURN, 0: none): JavaScriptCore frees a
// replaced instance only at a full collection of the thread's heap, which a
// thread running wasm seldom starts. Safari 26, tools/jitmem, 2048
// replacements on each of 4 threads: 730 MB after them without, 110-270 MB
// with 4-64 MB every 64 (68 MB without replacing); an iPhone X (iOS 16):
// 1437 MB and "Out of executable memory" without, 106 MB with 4.
int Churn() {
  static const int mb = [] {
    const char* v = getenv("ARC_CPU_JIT_CHURN");
    return v && *v ? atoi(v) : 4;
  }();
  return mb;
}

// The budget's limit of all threads' functions, as their cost (0: none).
int64_t TotalCap() {
  static const int64_t cap = arc_mem_config ? int64_t(arc_mem_config()->cpu_jit_total) * kNominalCost : 0;
  return cap;
}

void Live(int32_t delta, int64_t cost) {
  const int32_t now = g_live.fetch_add(delta, std::memory_order_relaxed) + delta;
  __atomic_store_n(&arc_cpujit_counts[6], static_cast<uint32_t>(now < 0 ? 0 : now), __ATOMIC_RELAXED);
  const int64_t c = g_live_cost.fetch_add(cost, std::memory_order_relaxed) + cost;
  __atomic_store_n(&arc_cpujit_counts[7], static_cast<uint32_t>(c < 0 ? 0 : c >> 10), __ATOMIC_RELAXED);
}

std::mutex g_mu;
// Index = id - 1; null once the block is retired.
// (Never destroyed: threads may still translate while the process exits.)
std::vector<std::shared_ptr<const std::vector<uint8_t>>>& g_bodies =
    *new std::vector<std::shared_ptr<const std::vector<uint8_t>>>;
std::atomic<bool> g_broken{false};
// By id - 1: whether the body was instantiated on some thread (counted in
// arc_cpujit_counts[5]).
std::vector<uint8_t>& g_used = *new std::vector<uint8_t>;

struct Buf {
  std::vector<uint8_t>* b;
  void byte(uint8_t v) { b->push_back(v); }
  void u32(uint32_t v) {
    do { uint8_t x = v & 0x7f; v >>= 7; if (v) x |= 0x80; b->push_back(x); } while (v);
  }
  void name(char c) { u32(1); byte(c); }
  // A section whose body is appended by |fill|.
  template <class F> void section(uint8_t id, F fill) {
    std::vector<uint8_t> body;
    Buf s{&body};
    fill(s);
    byte(id);
    u32(body.size());
    b->insert(b->end(), body.begin(), body.end());
  }
};

#ifdef __EMSCRIPTEN__
// Compiles the pack's module and instantiates it into this thread's table:
// returns the first slot (|reuse| > 0: those slots, a cold pack's), -2 when
// this thread is at the browser's limit (|max| < 0: 1024 functions on iOS,
// else none) or |max|, -1 when translation cannot work here.
// JavaScriptCore frees a dropped instance (a replaced pack's, a gone
// thread's) only at a full collection of the thread's heap, which a thread
// running wasm seldom starts: every 256 functions dropped, |churn| MB of JS
// garbage that lives a while (old generation) makes one (tools/jitmem gc=,
// topic jit-module-heap). globalThis.arcJitDrop(n, churn), made by the
// first who needs it on the thread (here twice and in
// shim/art*/dex_jit_art*.cc: the same code).
// (|churn|: see Churn below.)
// |pub| > 0: the pack's number in the page's catalog (shim/ndk/jit_catalog.js):
// the module compiled goes there; with |len| 0 it comes from there (-3 when
// this worker has not got it).
EM_JS(int, ndk_jit_pack_instantiate, (const uint8_t* bytes, int len, int kind, int n, int reuse, int max, int churn,
                                      int pub), {
  if (max < 0) {
    // Worked out once per thread: userAgent and a regex per call were
    // garbage for the thread's GC.
    if (globalThis.ndkJitMax === undefined) {
      var ua = (globalThis.navigator && navigator.userAgent) || "";
      globalThis.ndkJitMax = /iPhone|iPad|iPod/.test(ua) || (/Macintosh/.test(ua) && navigator.maxTouchPoints > 1) ? 1024 : 0;
    }
    max = globalThis.ndkJitMax;
  }
  if (reuse <= 0 && max > 0 && (globalThis.ndkJitInstances || 0) + n > max) {
    if (!globalThis.ndkJitFull) {
      globalThis.ndkJitFull = true;
      err('ndk_cpu: ' + max + ' translations on this thread: the rest is interpreted');
    }
    return -2;
  }
  try {
    var mod;
    if (len == 0) {
      mod = globalThis.ndkJitCatalog && globalThis.ndkJitCatalog.get(pub);
      if (!mod) return -3;
    } else {
      // Copied out of the shared buffer (WebAssembly.Module takes no views of
      // shared memory) into the thread's kept buffer; a fresh view of the
      // memory (HEAPU8 may be stale after growth on another thread); |bytes|
      // above 2 GB arrives negative.
      var scratch = globalThis.ndkJitScratch;
      if (!scratch || scratch.length < len)
        scratch = globalThis.ndkJitScratch = new Uint8Array(Math.max(len, 2 * (scratch ? scratch.length : 65536)));
      var code = scratch.subarray(0, len);
      code.set(new Uint8Array(wasmMemory.buffer, bytes >>> 0, len));
      mod = new WebAssembly.Module(code);
      if (pub > 0 && globalThis.ndkJitCatalog) postMessage({arcJitPack: pub, mod: mod, n: n});
    }
    var base = reuse;
    if (base <= 0 && globalThis.ndkJitFreeSlots && globalThis.ndkJitFreeSlots.length) {
      base = globalThis.ndkJitFreeSlots.pop();   // a gone thread's (ndk_jit_pack_release)
    } else if (base <= 0) {
      // Slots by many at a time: grown by one per translation (30000
      // entries), Safari copied the table each time and stalled the thread
      // for seconds. Slots past ndkJitNext stay null; emscripten's own
      // additions go at the table's end.
      var slots = 32;
      if (!globalThis.ndkJitNext || globalThis.ndkJitNext + slots > globalThis.ndkJitEnd ||
          globalThis.ndkJitEnd != wasmTable.length) {
        globalThis.ndkJitNext = wasmTable.length;
        wasmTable.grow(512);
        globalThis.ndkJitEnd = wasmTable.length;
      }
      base = globalThis.ndkJitNext;
      globalThis.ndkJitNext += slots;
    }
    new WebAssembly.Instance(mod, {e: {m: wasmMemory, t: wasmTable,
        x: wasmExports[kind ? 'ndk64_jit_exec_one' : 'ndk_jit_exec_one'],
        o: new WebAssembly.Global({value: 'i32'}, base)}});
    if (reuse > 0) {
      // The replaced instance goes only at a full collection of this
      // thread's heap (arcJitDrop above).
      var drop = globalThis.arcJitDrop || (globalThis.arcJitDrop = function (n, mb) {
        var dropped = globalThis.arcJitDropped = (globalThis.arcJitDropped || 0) + n;
        if (mb <= 0 || dropped < 256) return;
        globalThis.arcJitDropped = 0;
        var keep = [];
        for (var i = 0; i < mb * 16; i++) {
          var a = new Array(1024);
          for (var j = 0; j < 1024; j++) a[j] = {x: j};
          keep.push(a);
          if (keep.length > 64) keep.shift();
        }
      });
      drop(n, churn);
      globalThis.ndkJitReplaced = (globalThis.ndkJitReplaced || 0) + 1;
      if ((globalThis.ndkJitReplaced & 1023) == 0)
        err('ndk_cpu: ' + globalThis.ndkJitReplaced + ' packs replaced on this thread');
      return base;
    }
    // How many this thread made (each thread compiles what it runs), for
    // setting them against a device's limits.
    var before = globalThis.ndkJitInstances || 0;
    var count = globalThis.ndkJitInstances = before + n;
    globalThis.ndkJitPacks = (globalThis.ndkJitPacks || 0) + 1;
    globalThis.ndkJitBytes = (globalThis.ndkJitBytes || 0) + len;
    // At every power of two from 64 it passes.
    var power = 1 << (31 - Math.clz32(count));
    if (count >= 64 && power > before)
      err('ndk_cpu: ' + power + ' translations instantiated on this thread (' +
          globalThis.ndkJitPacks + ' modules, ' + Math.round(globalThis.ndkJitBytes / 1024) + ' KB of wasm), table ' +
          wasmTable.length);
    return base;
  } catch (e) {
    err('ndk_cpu: wasm translation disabled: ' + e);
    return -1;
  }
});
// A gone thread's pack: its slots emptied (the instance can go; the slots
// are this worker's, which the next thread on it reuses).
EM_JS(void, ndk_jit_pack_release, (int base, int slots, int n, int churn), {
  for (var i = 0; i < slots; i++) wasmTable.set(base + i, null);
  var drop = globalThis.arcJitDrop || (globalThis.arcJitDrop = function (n, mb) {
    var dropped = globalThis.arcJitDropped = (globalThis.arcJitDropped || 0) + n;
    if (mb <= 0 || dropped < 256) return;
    globalThis.arcJitDropped = 0;
    var keep = [];
    for (var i = 0; i < mb * 16; i++) {
      var a = new Array(1024);
      for (var j = 0; j < 1024; j++) a[j] = {x: j};
      keep.push(a);
      if (keep.length > 64) keep.shift();
    }
  });
  drop(n, churn);
  (globalThis.ndkJitFreeSlots || (globalThis.ndkJitFreeSlots = [])).push(base);
  globalThis.ndkJitInstances = Math.max(0, (globalThis.ndkJitInstances || 0) - n);
});
EM_JS(int, ndk_jit_pack_memory_shared, (), {
  return typeof SharedArrayBuffer != 'undefined' && wasmMemory.buffer instanceof SharedArrayBuffer ? 1 : 0;
});
// The page's catalog (jit_catalog.js, linked into SENSES' VMs): whether
// this worker takes part, whether it has pack |pub|.
EM_JS(int, ndk_jit_catalog_on, (), {
  return typeof globalThis.ndkJitCatalog == 'object' && globalThis.ndkJitCatalog instanceof Map ? 1 : 0;
});
EM_JS(int, ndk_jit_catalog_has, (int pub), {
  return globalThis.ndkJitCatalog && globalThis.ndkJitCatalog.has(pub) ? 1 : 0;
});
#else
static int ndk_jit_pack_instantiate(const uint8_t*, int, int, int, int, int, int, int) { return -1; }
static void ndk_jit_pack_release(int, int, int, int) {}
static int ndk_jit_pack_memory_shared() { return 0; }
static int ndk_jit_catalog_on() { return 0; }
static int ndk_jit_catalog_has(int) { return 0; }
#endif

// Packs published to the page's catalog (topic arm-chains): what each holds
// (by number - 1), and by id - 1 the last one that holds it (0: none). g_mu.
struct Published {
  uint8_t kind, n;
  int32_t ids[kPackMax];
};
std::vector<Published>& g_pubs = *new std::vector<Published>;
std::vector<uint32_t>& g_pub_of = *new std::vector<uint32_t>;

bool CatalogOn() {
  static const bool on = [] {
    const char* v = getenv("ARC_CPU_JIT_CATALOG");
    return !(v && *v == '0') && ndk_jit_catalog_on() != 0;
  }();
  return on;
}

void Grow(ThreadJit* t, int id, bool evict) {
  uint32_t cap = t->cap ? t->cap : 256;
  while (cap <= (uint32_t)id) cap *= 2;
  t->fns = static_cast<int32_t*>(realloc(t->fns, cap * sizeof(int32_t)));
  memset(t->fns + t->cap, 0, (cap - t->cap) * sizeof(int32_t));
  if (evict) {
    t->seen = static_cast<uint32_t*>(realloc(t->seen, cap * sizeof(uint32_t)));
    memset(t->seen + t->cap, 0, (cap - t->cap) * sizeof(uint32_t));
  }
  t->cap = cap;
}

// The module of |n| bodies: (i32) -> i32 functions after the import
// exec_one, put into the imported table at the imported offset.
void Build(std::vector<uint8_t>* out, const std::shared_ptr<const std::vector<uint8_t>>* bodies, int n) {
  out->clear();
  Buf m{out};
  const uint8_t header[] = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
  out->insert(out->end(), header, header + 8);
  m.section(1, [](Buf& s) {   // 0: (i32) -> i32, a block; 1: (i32, i32) -> (), exec_one
    s.u32(2);
    s.byte(0x60); s.u32(1); s.byte(0x7f); s.u32(1); s.byte(0x7f);
    s.byte(0x60); s.u32(2); s.byte(0x7f); s.byte(0x7f); s.u32(0);
  });
  m.section(2, [](Buf& s) {
    s.u32(4);
    s.name('e'); s.name('m'); s.byte(0x02);   // the memory, max 65536 pages (| shared)
    s.byte(MemoryShared() ? 0x03 : 0x01); s.u32(0); s.u32(65536);
    s.name('e'); s.name('x'); s.byte(0x00); s.u32(1);   // function 0: exec_one
    s.name('e'); s.name('t'); s.byte(0x01); s.byte(0x70); s.byte(0x00); s.u32(0);   // funcref table, min 0
    s.name('e'); s.name('o'); s.byte(0x03); s.byte(0x7f); s.byte(0x00);   // i32 const: first slot
  });
  m.section(3, [n](Buf& s) { s.u32(n); for (int i = 0; i < n; i++) s.u32(0); });
  m.section(9, [n](Buf& s) {   // active segment, table 0 at global 0: functions 1..n
    s.u32(1);
    s.byte(0x00); s.byte(0x23); s.u32(0); s.byte(0x0b);
    s.u32(n);
    for (int i = 0; i < n; i++) s.u32(i + 1);
  });
  m.section(10, [bodies, n](Buf& s) {
    s.u32(n);
    for (int i = 0; i < n; i++) {
      s.u32(bodies[i]->size());
      s.b->insert(s.b->end(), bodies[i]->begin(), bodies[i]->end());
    }
  });
}

// Slots for a new pack when |t| has as many functions as it may: a cold
// pack's (its blocks may be instantiated again once hot again), or -1.
// JavaScriptCore frees a replaced instance late, if at all: a second-chance
// hand that replaced 8192 a minute took a phone's tab to 1.5 GB; Safari 26
// held 2048 replaced packs a thread (+670 MB) for minutes, and an iPhone X
// (iOS 16) none of them (+1.3 GB, then "Out of executable memory":
// tools/jitmem rep=, topic jit-module-heap). With JS garbage after
// replacing (Churn) they go; under all threads' limit then as many as go
// cold (what replaces the start's code), without it a quarter of the limit
// in all; else a table's worth (|max|) a thread.
std::atomic<uint32_t> g_replaced{0};   // functions whose slots a new pack took

int MakeRoom(ThreadJit* t, int max) {
  if (!t->npacks || (int32_t)(t->clock - t->scan_at) < 0) return -1;
  const int64_t total = TotalCap();
  if (total ? !Churn() && int64_t(g_replaced.load(std::memory_order_relaxed) + kPackMax) * kNominalCost > total / 4
            : t->replaced >= (uint32_t)max / kPackMax)
    return -1;
  for (uint32_t k = 0; k < t->npacks; k++) {
    Pack& p = t->packs[t->hand];
    t->hand = t->hand + 1 == t->npacks ? 0 : t->hand + 1;
    bool cold = true;
    for (int i = 0; i < p.n && cold; i++)
      if (p.ids[i] > 0 && t->clock - t->seen[p.ids[i]] < kColdRuns) cold = false;
    if (!cold) continue;
    for (int i = 0; i < p.n; i++)
      if (p.ids[i] > 0 && t->fns[p.ids[i]] > 0) t->fns[p.ids[i]] = 0;
    t->functions -= p.n;
    t->cost -= p.cost;
    Live(-p.n, -int64_t(p.cost));
    g_replaced.fetch_add(p.n, std::memory_order_relaxed);
    p.n = 0;
    if (t->replaced++ == 0)
      fprintf(stderr, "ndk_cpu: %u translations on a thread: cold ones make room\n", (unsigned)max);
    return static_cast<int>(&p - t->packs);
  }
  // Nothing cold: no scan again before a while of runs.
  t->scan_at = t->clock + kColdRuns / 4;
  return -1;
}

// Where a pack of |cost| goes on |t|: -1 new slots, a cold pack's (its
// index), or -2 none: at the limit (this thread's, or all threads'
// together: then only its own cold packs make room) and nothing cold.
int Place(ThreadJit* t, uint32_t cost, int max, bool evict) {
  const int64_t total = TotalCap();
  const bool over_total = total && g_live_cost.load(std::memory_order_relaxed) + cost > total;
  if ((evict && max > 0 && t->cost + cost > uint64_t(max) * kNominalCost) || over_total) {
    const int slot = evict && max > 0 ? MakeRoom(t, max) : -1;
    return slot < 0 ? -2 : slot;
  }
  return -1;
}

// A pack instantiated at |base| in |slot| (-1: new): its functions |ids|
// (0: a slot this thread does not use), of |cost|.
void Install(ThreadJit* t, int slot, int base, const int32_t* ids, int n, uint32_t cost) {
  __atomic_fetch_add(&arc_cpujit_counts[0], 1, __ATOMIC_RELAXED);
  if (slot >= 0) __atomic_fetch_add(&arc_cpujit_counts[1], 1, __ATOMIC_RELAXED);
  __atomic_fetch_add(&arc_cpujit_counts[2], n, __ATOMIC_RELAXED);
  if (slot < 0) {
    if (t->npacks == t->packs_cap) {
      t->packs_cap = t->packs_cap ? 2 * t->packs_cap : 16;
      t->packs = static_cast<Pack*>(realloc(t->packs, t->packs_cap * sizeof(Pack)));
    }
    slot = static_cast<int>(t->npacks++);
  }
  Pack& p = t->packs[slot];
  p.base = base;
  p.n = static_cast<uint8_t>(n);
  p.cost = cost;
  for (int i = 0; i < n; i++) {
    p.ids[i] = ids[i];
    if (ids[i] <= 0) continue;
    t->fns[ids[i]] = base + i;
    if (t->seen) t->seen[ids[i]] = t->clock;
  }
  t->functions += n;
  t->cost += cost;
  Live(n, cost);
  std::lock_guard<std::mutex> lock(g_mu);
  if (g_used.size() < g_bodies.size()) g_used.resize(g_bodies.size());
  for (int i = 0; i < n; i++) {
    if (ids[i] <= 0 || g_used[ids[i] - 1]) continue;
    g_used[ids[i] - 1] = 1;
    __atomic_fetch_add(&arc_cpujit_counts[5], 1, __ATOMIC_RELAXED);
  }
}

// The thread's open pack of |kind| as packs from the page's catalog: those
// that hold at least a quarter of their functions among the blocks waiting
// here (the rest of them come along). Returns false when out of room.
bool FromCatalog(ThreadJit* t, Kind kind, const uint32_t* pubs, int npubs, int max, bool evict) {
  for (int k = 0; k < npubs; k++) {
    if (!ndk_jit_catalog_has((int)pubs[k])) continue;
    Published pub;
    int32_t ids[kPackMax];
    uint32_t cost = 0;
    int wanted = 0;
    {
      std::lock_guard<std::mutex> lock(g_mu);
      pub = g_pubs[pubs[k] - 1];
      for (int i = 0; i < pub.n; i++) {
        const int32_t id = pub.ids[i];
        ids[i] = 0;
        if (!g_bodies[id - 1]) continue;   // retired: its function stays unused
        cost += Cost(static_cast<uint32_t>(g_bodies[id - 1]->size()));
        if ((uint32_t)id >= t->cap || t->fns[id] > 0) continue;   // (it has its own)
        wanted += t->fns[id] == -2;
        ids[i] = id;
      }
    }
    if (pub.kind != kind || wanted * 4 < pub.n) continue;
    const int slot = Place(t, cost, max, evict);
    if (slot == -2) return false;
    const int reuse = slot >= 0 ? t->packs[slot].base : 0;
    const int base = ndk_jit_pack_instantiate(nullptr, 0, kind, pub.n, reuse, evict ? 0 : max, Churn(),
                                              (int)pubs[k]);
    if (base < 0) continue;   // (gone from this worker's catalog; -2, -1: the compile below sees it)
    Install(t, slot, base, ids, pub.n, cost);
    __atomic_fetch_add(&arc_cpujit_counts[8], 1, __ATOMIC_RELAXED);
  }
  return true;
}

// Compiles and instantiates |t|'s open pack of |kind| (what of it the
// page's catalog does not have).
void Seal(ThreadJit* t, Kind kind, int max, bool evict) {
  const uint32_t n0 = t->npending[kind];
  t->npending[kind] = 0;
  t->pending_runs[kind] = 0;
  if (!n0) return;
  const bool catalog = CatalogOn();
  uint32_t pubs[kPackMax];
  int npubs = 0;
  if (catalog) {
    std::lock_guard<std::mutex> lock(g_mu);
    for (uint32_t i = 0; i < n0; i++) {
      const int32_t id = t->pending[kind][i];
      const uint32_t pub = (uint32_t)id <= g_pub_of.size() ? g_pub_of[id - 1] : 0;
      bool seen = !pub;
      for (int k = 0; k < npubs && !seen; k++) seen = pubs[k] == pub;
      if (!seen) pubs[npubs++] = pub;
    }
  }
  if (npubs && !FromCatalog(t, kind, pubs, npubs, max, evict)) {
    for (uint32_t i = 0; i < n0; i++)
      if (t->fns[t->pending[kind][i]] == -2) t->fns[t->pending[kind][i]] = -3 - (int32_t)(kSealRuns * 8);
    return;
  }
  int32_t ids[kPackMax];
  std::shared_ptr<const std::vector<uint8_t>> bodies[kPackMax];
  int n = 0;
  uint32_t cost = 0;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    for (uint32_t i = 0; i < n0; i++) {
      const int32_t id = t->pending[kind][i];
      if (t->fns[id] != -2) continue;
      if (!g_bodies[id - 1]) { t->fns[id] = -1; continue; }   // retired meanwhile
      ids[n] = id;
      cost += Cost(static_cast<uint32_t>(g_bodies[id - 1]->size()));
      bodies[n++] = g_bodies[id - 1];
    }
  }
  if (!n) return;
  // A pack's place: a new one, or a cold one's when at the limit.
  const int slot = Place(t, cost, max, evict);
  if (slot == -2) {   // full and nothing cold: later
    for (int i = 0; i < n; i++) t->fns[ids[i]] = -3 - (int32_t)(kSealRuns * 8);
    return;
  }
  uint32_t pub = 0;
  if (catalog) {
    std::lock_guard<std::mutex> lock(g_mu);
    Published p;
    p.kind = static_cast<uint8_t>(kind);
    p.n = static_cast<uint8_t>(n);
    memcpy(p.ids, ids, n * sizeof(int32_t));
    g_pubs.push_back(p);
    pub = static_cast<uint32_t>(g_pubs.size());
  }
  if (!t->scratch) t->scratch = new std::vector<uint8_t>();
  Build(t->scratch, bodies, n);
  const int reuse = slot >= 0 ? t->packs[slot].base : 0;
  const int base = ndk_jit_pack_instantiate(t->scratch->data(), (int)t->scratch->size(), kind, n, reuse,
                                            evict ? 0 : max, Churn(), (int)pub);
  if (base == -2) {   // this thread's limit
    t->full = true;
    for (int i = 0; i < n; i++) t->fns[ids[i]] = -1;
    return;
  }
  if (base < 0) {
    g_broken.store(true);
    for (int i = 0; i < n; i++) t->fns[ids[i]] = -1;
    return;
  }
  static std::atomic<uint64_t> bytes{0};
  __atomic_store_n(&arc_cpujit_counts[3], (uint32_t)((bytes += t->scratch->size()) >> 10), __ATOMIC_RELAXED);
  if (pub) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_pub_of.size() < g_bodies.size()) g_pub_of.resize(g_bodies.size());
    for (int i = 0; i < n; i++) g_pub_of[ids[i] - 1] = pub;
  }
  Install(t, slot, base, ids, n, cost);
}

}  // namespace

int Add(std::vector<uint8_t>&& body) {
  std::lock_guard<std::mutex> lock(g_mu);
  g_bodies.push_back(std::make_shared<const std::vector<uint8_t>>(std::move(body)));
  __atomic_fetch_add(&arc_cpujit_counts[4], 1, __ATOMIC_RELAXED);
  return static_cast<int>(g_bodies.size());
}

void Forget(int id) {
  if (id <= 0) return;
  std::lock_guard<std::mutex> lock(g_mu);
  g_bodies[id - 1].reset();
}

int Lookup(ThreadJit* t, Kind kind, int id, int max, int hits, bool evict) {
  if ((uint32_t)id >= t->cap) Grow(t, id, evict);
  int32_t v = t->fns[id];
  // The clock (with eviction) counts runs of translated blocks, those
  // interpreted for want of room too: else a full thread's packs, the
  // start's, never went cold while the code it ran then was interpreted.
  if (t->seen) ++t->clock;
  if (v > 0) {
    if (t->seen) t->seen[id] = t->clock;
    return v;
  }
  if (v == -1) return 0;
  if (v == -2) {   // waiting in the open pack
    if (++t->pending_runs[kind] >= kSealRuns) Seal(t, kind, max, evict);
    return t->fns[id] > 0 ? t->fns[id] : 0;
  }
  if (v < -3) { t->fns[id] = v + 1; return 0; }   // runs left before it is instantiated here
  if (v == 0 && hits > 0) {
    // Under all threads' limit, hotter blocks first: past half of it a
    // block waits up to 4x the runs (the code a thread runs all the time
    // rather than a start's long tail).
    if (const int64_t total = TotalCap()) {
      const int64_t live = g_live_cost.load(std::memory_order_relaxed);
      if (live > total / 2) hits += (int)((int64_t)hits * 6 * (live - total / 2) / total);
    }
    t->fns[id] = -3 - hits;
    return 0;
  }
  // Hot on this thread too: into its open pack.
  if (t->full || g_broken.load(std::memory_order_relaxed)) { t->fns[id] = -1; return 0; }
  t->fns[id] = -2;
  t->pending[kind][t->npending[kind]++] = id;
  if (t->npending[kind] == kPackMax) Seal(t, kind, max, evict);
  return t->fns[id] > 0 ? t->fns[id] : 0;
}

void Free(ThreadJit* t) {
  // (On the thread whose table it is: a thread's Cpu and a carrier's
  // translations go at its exit.)
  for (uint32_t i = 0; i < t->npacks; i++) {
    if (!t->packs[i].n) continue;
    ndk_jit_pack_release(t->packs[i].base, kPackMax, t->packs[i].n, Churn());
    Live(-t->packs[i].n, -int64_t(t->packs[i].cost));
  }
  free(t->fns);
  free(t->seen);
  free(t->packs);
  delete t->scratch;
  memset(t, 0, sizeof(*t));
}

bool Broken() { return g_broken.load(std::memory_order_relaxed); }

bool MemoryShared() {
  static const bool shared = ndk_jit_pack_memory_shared() != 0;
  return shared;
}

void Count(const ThreadJit* t, uint32_t* functions, uint32_t* packs) {
  *functions = t->functions;
  uint32_t live = 0;
  for (uint32_t i = 0; i < t->npacks; i++) live += t->packs[i].n != 0;
  *packs = live;
}

}  // namespace ndkjit
