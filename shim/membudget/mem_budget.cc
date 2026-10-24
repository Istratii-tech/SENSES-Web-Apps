// The budget, the sample of what memory holds and the trims of this layer
// (mem_budget.h, topic mem-budget). Part of the arc-wasm shim layer.

#include "membudget/mem_budget.h"

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <atomic>
#include <mutex>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/threading.h>
#endif

#include "wasm_mm/wasm_mmap.h"

// dlmalloc's (Emscripten's malloc; musl's <malloc.h> does not declare them).
extern "C" size_t malloc_footprint(void);
extern "C" int malloc_trim(size_t pad);
// The process's property store (shim/fw6/system_properties.cc).
extern "C" int __system_property_set(const char* key, const char* value) __attribute__((weak));
// /proc/self/maps of the NDK bridge (shim/ndk/ndk_linker.cc): ARM libraries.
extern "C" size_t arc_proc_maps(char* buf, size_t size) __attribute__((weak));

namespace {

const uint64_t kMB = 1024 * 1024;

uint32_t EnvMb(const char* name) {
  const char* v = getenv(name);
  return v && *v ? static_cast<uint32_t>(strtoul(v, nullptr, 10)) : 0;
}

// The heap sizes of AOSP's frameworks/native/build/phone-*-dalvik-heap.mk
// for a device of |mb| of RAM (the 1 GB phone: 96m/256m, 2 GB: 192m/512m),
// and the media profile: what a phone of that size would afford.
void Derive(ArcMemConfig* c) {
  const uint32_t b = c->budget_mb;
  if (b < 768) {
    c->heap_start_mb = 4; c->heap_growth_mb = 64; c->heap_size_mb = 128;
  } else if (b < 1536) {
    c->heap_start_mb = 8; c->heap_growth_mb = 96; c->heap_size_mb = 256;
  } else if (b < 3072) {
    c->heap_start_mb = 8; c->heap_growth_mb = 192; c->heap_size_mb = 512;
  } else {
    c->heap_start_mb = 8; c->heap_growth_mb = 256; c->heap_size_mb = 512;
  }
  c->low_ram = b < 1024;
  c->max_video_decoders = b < 1024 ? 2 : b < 2048 ? 3 : 4;
  c->max_audio_decoders = 8;
  c->video_inputs = b < 1536 ? 4 : 6;
  c->video_outputs = b < 1536 ? 5 : 8;
  c->decode_queue = b < 1536 ? 6 : 12;
  // ActivityManager's levels as fractions of the budget: in use above 70%
  // is RUNNING_MODERATE, 80% RUNNING_LOW, 90% RUNNING_CRITICAL.
  c->moderate_permille = 700;
  c->low_permille = 800;
  c->critical_permille = 900;
  // JIT code. A phone's tab (iPhone: budget 1024) is reloaded around 1.5 GB
  // of footprint, and JavaScriptCore keeps each thread's instances of the
  // modules apart (an ARM translation ~13 KB a thread alone, ~6 KB in a pack
  // of them, shim/ndk/jit_pack.h): a thread instantiates only what is hot on
  // it, only once it has run a while itself (a pool's threads that run
  // little keep none), and small caps per thread and in all; past them the
  // code is interpreted (Telegram X had 10 threads at 1024 translations
  // each: +280 MB against no JIT). Subway Surfers in Safari 26 on a Mac:
  // 139 000 translations on 46 of 49 threads with them at once, 42 000 on 7
  // with 50 runs and 100 million instructions; Angry Birds 3.2.0 there: 1972
  // MB with every thread at once, 1600 with 50 runs and 10 million, 1474
  // without translation (topic shared-translations).
  // From this size up, an arm64 guest's reservation is a window of addresses
  // whose memory comes as the guest commits it (Unity's Dynamic Heap: 511 MB
  // seven times; Hermes: 3 GB): one taking its size in wasm memory at once
  // left none for the rest (topic arm64-lazy-reserve). Smaller ones are wasm
  // memory as they are (untouched it costs nothing but wasm addresses).
  c->guest_reserve_mb = 64;
  // JIT code, and also a limit on all threads' translations together
  // (topic jit-module-heap): on an iPhone X Subway Surfers' carriers of green
  // threads held 19 000-23 000 (each carrier its own copies of the job
  // code), +300 MB of JavaScriptCore heaps and +100 MB of JIT code while it
  // loads; Angry Birds ~5 000, Telegram X ~4 000.
  if (b <= 1024) {
    c->cpu_jit_per_thread = 2048; c->cpu_jit_thread_hits = 200; c->cpu_jit_kb = 16 * 1024;
    c->cpu_jit_thread_minsns = 100;
    c->cpu_jit_total = 6144;
    c->dex_jit_thread_kb = 1024; c->dex_jit_kb = 4096;
  } else if (b < 2048) {
    c->cpu_jit_per_thread = 4096; c->cpu_jit_thread_hits = 50; c->cpu_jit_kb = 64 * 1024;
    c->cpu_jit_thread_minsns = 50;
    c->cpu_jit_total = 16384;
    c->dex_jit_thread_kb = 4096; c->dex_jit_kb = 16384;
  } else {
    c->cpu_jit_per_thread = 0; c->cpu_jit_thread_hits = 50; c->cpu_jit_kb = 0;
    c->cpu_jit_thread_minsns = 10;
    c->cpu_jit_total = 0;
    c->dex_jit_thread_kb = 8192; c->dex_jit_kb = 32768;
  }
}

ArcMemConfig* g_config;
std::once_flag g_config_once;

void InitConfig() {
  ArcMemConfig* c = new ArcMemConfig();
  memset(c, 0, sizeof(*c));
  // mem_budget_pre.js decides both from the device, ?mem= and ?memcap= (in
  // Node from the same variables) and passes them down here.
  c->budget_mb = EnvMb("ARC_MEM_BUDGET_MB");
  c->cap_mb = EnvMb("ARC_MEM_CAP_MB");
  if (!c->cap_mb && arc_mem_js_cells()[ARC_MEM_JS_CAP_MB] > 0)
    c->cap_mb = arc_mem_js_cells()[ARC_MEM_JS_CAP_MB];
  if (!c->cap_mb || c->cap_mb > 4096)
    c->cap_mb = 4096;
  if (!c->budget_mb)
    c->budget_mb = c->cap_mb >= 4096 ? 3072 : c->cap_mb * 2 / 3;
  if (c->budget_mb < 128)
    c->budget_mb = 128;
  Derive(c);
  // Tests and experiments: the translators' per-thread limit (eviction
  // with a small one) and hits before a thread instantiates.
  if (getenv("ARC_CPU_JIT_THRESHOLD")) c->cpu_jit_threshold = atoi(getenv("ARC_CPU_JIT_THRESHOLD"));
  if (getenv("ARC_CPU_JIT_PER_THREAD")) c->cpu_jit_per_thread = EnvMb("ARC_CPU_JIT_PER_THREAD");
  if (getenv("ARC_CPU_JIT_THREAD_HITS")) c->cpu_jit_thread_hits = EnvMb("ARC_CPU_JIT_THREAD_HITS");
  if (getenv("ARC_CPU_JIT_THREAD_MINSNS")) c->cpu_jit_thread_minsns = EnvMb("ARC_CPU_JIT_THREAD_MINSNS");
  if (getenv("ARC_GUEST_RESERVE_MB")) c->guest_reserve_mb = EnvMb("ARC_GUEST_RESERVE_MB");
  if (getenv("ARC_CPU_JIT_TOTAL")) c->cpu_jit_total = EnvMb("ARC_CPU_JIT_TOTAL");
  g_config = c;
}

std::atomic<uint32_t> g_test_budget_mb{0};
std::atomic<int> g_test_video{0}, g_test_audio{0};

alignas(16) int32_t g_js_cells[ARC_MEM_JS_COUNT];

// Sum of the address ranges of /proc/self/maps lines ("%08x-%08x ...").
int64_t ArmLibBytes() {
  if (!arc_proc_maps)
    return 0;
  size_t n = arc_proc_maps(nullptr, 0);
  if (!n)
    return 0;
  char* text = static_cast<char*>(malloc(n + 1));
  if (!text)
    return 0;
  n = arc_proc_maps(text, n);
  text[n] = '\0';
  int64_t total = 0;
  for (char* line = text; *line;) {
    char* end = strchr(line, '\n');
    unsigned long a, b;
    if (sscanf(line, "%lx-%lx", &a, &b) == 2 && b > a)
      total += b - a;
    if (!end)
      break;
    line = end + 1;
  }
  free(text);
  return total;
}

void SetProp(const char* name, const char* fmt, unsigned v) {
  char value[92];  // PROP_VALUE_MAX
  snprintf(value, sizeof(value), fmt, v);
  __system_property_set(name, value);
}

}  // namespace

extern "C" {

const ArcMemConfig* arc_mem_config(void) {
  std::call_once(g_config_once, InitConfig);
  return g_config;
}

void arc_mem_set_budget_for_test(uint32_t budget_mb) {
  g_test_budget_mb.store(budget_mb);
}

void arc_mem_set_decoder_limits_for_test(int video, int audio) {
  ArcMemConfig* c = const_cast<ArcMemConfig*>(arc_mem_config());
  if (video > 0) c->max_video_decoders = video;
  if (audio > 0) c->max_audio_decoders = audio;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int32_t* arc_mem_js_cells(void) {
  return g_js_cells;
}

// posix_translation (readonly_file.cc): the /system image's bytes in wasm memory.
extern "C" size_t arc_readonly_image_bytes(void) __attribute__((weak));
extern "C" size_t arc_readonly_image_filled_bytes(void) __attribute__((weak));
// An arm64 guest's reservations not committed (shim/ndk/ndk_libc.cc).
extern "C" uint64_t ndk_reserve_idle_bytes(void) __attribute__((weak));
// Known untouched memory below the break (topic senses-shell-fixes).
extern "C" size_t wasm_mm_untouched_bytes(void) __attribute__((weak));
extern "C" uint64_t arc_malloc_untouched_bytes(void) __attribute__((weak));

void arc_mem_scan_print(void);

void arc_mem_sample(int64_t* out, int precise) {
#ifdef __EMSCRIPTEN__
  // ARC_MEM_SCAN_AT="S1 S2 ...": a scan (arc_mem_scan_print) once S seconds
  // have passed since the first sample (the monitor samples every second).
  {
    static double first = 0;
    static const char* next = getenv("ARC_MEM_SCAN_AT");
    const double now = emscripten_get_now() / 1000;
    if (!first)
      first = now;
    char* end;
    const double at = next ? strtod(next, &end) : 0;
    if (next && end != next && now - first >= at) {
      next = end;
      arc_mem_scan_print();
    }
  }
#endif
  memset(out, 0, sizeof(int64_t) * ARC_MEM_SAMPLE_COUNT);
  const ArcMemConfig* c = arc_mem_config();
#ifdef __EMSCRIPTEN__
  out[ARC_MEM_WASM] = static_cast<int64_t>(__builtin_wasm_memory_size(0)) * 65536;
#else
  out[ARC_MEM_WASM] = reinterpret_cast<intptr_t>(sbrk(0));
#endif
  out[ARC_MEM_BRK] = static_cast<int64_t>(reinterpret_cast<uintptr_t>(sbrk(0)));
  if (arc_readonly_image_bytes)
    out[ARC_MEM_IMAGE] = static_cast<int64_t>(arc_readonly_image_bytes());
  if (arc_readonly_image_filled_bytes)
    out[ARC_MEM_IMAGE_FILLED] = static_cast<int64_t>(arc_readonly_image_filled_bytes());
  if (wasm_mm_untouched_bytes)
    out[ARC_MEM_UNTOUCHED] += static_cast<int64_t>(wasm_mm_untouched_bytes());
  if (arc_malloc_untouched_bytes)
    out[ARC_MEM_UNTOUCHED] += static_cast<int64_t>(arc_malloc_untouched_bytes());
  out[ARC_MEM_CAP] = static_cast<int64_t>(c->cap_mb) * kMB;
  out[ARC_MEM_MALLOC_USED] = -1;
  out[ARC_MEM_MALLOC_FREE] = -1;
  if (precise) {
    // dlmalloc walks every chunk under its lock.
    struct mallinfo mi = mallinfo();
    out[ARC_MEM_MALLOC_ARENA] = static_cast<uint32_t>(mi.arena);
    out[ARC_MEM_MALLOC_USED] = static_cast<uint32_t>(mi.uordblks);
    out[ARC_MEM_MALLOC_FREE] = static_cast<uint32_t>(mi.fordblks);
  } else {
    out[ARC_MEM_MALLOC_ARENA] = static_cast<int64_t>(malloc_footprint());
  }
  out[ARC_MEM_MAPPED] = wasm_mm_mapped_bytes();
  // Holes in large mappings count as free: nobody uses them; so does what an
  // arm64 guest's reservations have not committed (shim/ndk/ndk_libc.cc).
  out[ARC_MEM_LARGE_FREE] = wasm_mm_large_free_bytes() + wasm_mm_large_hole_bytes() +
                            (ndk_reserve_idle_bytes ? static_cast<int64_t>(ndk_reserve_idle_bytes()) : 0);
  if (arc_art_mem_maps) {
    arc_art_mem_maps(&out[ARC_MEM_DEX], &out[ARC_MEM_JAVA_SPACES], &out[ARC_MEM_JAVA_LOS],
                     &out[ARC_MEM_ART_OTHER]);
  }
  out[ARC_MEM_ARM_LIBS] = ArmLibBytes();
  if (ndk_cpu_mem_stats) {
    uint64_t blocks = 0, jit = 0;
    ndk_cpu_mem_stats(&blocks, &jit);
    out[ARC_MEM_ARM_BLOCKS] = blocks;
    out[ARC_MEM_ARM_JIT] = jit;
  }
  if (ndk64_cpu_mem_stats) {
    uint64_t blocks = 0, jit = 0;
    ndk64_cpu_mem_stats(&blocks, &jit);
    out[ARC_MEM_ARM_BLOCKS] += blocks;
    out[ARC_MEM_ARM_JIT] += jit;
  }
  if (art_jit_mem_stats) {
    uint64_t code = 0;
    art_jit_mem_stats(&code);
    out[ARC_MEM_ARM_JIT] += code;
  }
  // Threads are emscripten pthreads (a worker each, its stack from malloc);
  // only the page's thread knows them all (mem_budget_pre.js counts).
  const int32_t threads = __atomic_load_n(&g_js_cells[ARC_MEM_JS_THREADS], __ATOMIC_RELAXED);
  out[ARC_MEM_THREADS] = threads > 0 ? threads : 1;
  // Pthread stacks are 1 MB (-sDEFAULT_PTHREAD_STACK_SIZE; ART asks the same
  // for its threads), the main one -sSTACK_SIZE.
  out[ARC_MEM_STACKS] = out[ARC_MEM_THREADS] * static_cast<int64_t>(kMB);
  if (arc_gralloc_mem_stats) {
    uint64_t bytes = 0;
    uint32_t n = 0;
    arc_gralloc_mem_stats(&bytes, &n);
    out[ARC_MEM_GRALLOC] = bytes;
    out[ARC_MEM_GRALLOC_COUNT] = n;
  }
  if (arc_media_mem_stats) {
    uint64_t bytes = 0;
    uint32_t n = 0;
    arc_media_mem_stats(&bytes, &n);
    out[ARC_MEM_CODEC] = bytes;
    out[ARC_MEM_CODEC_COUNT] = n;
  }
  const int32_t* js = g_js_cells;
  out[ARC_MEM_JS_FRAMES_N] = __atomic_load_n(&js[ARC_MEM_JS_FRAMES], __ATOMIC_RELAXED);
  out[ARC_MEM_JS_FRAME_BYTES] =
      static_cast<int64_t>(__atomic_load_n(&js[ARC_MEM_JS_FRAME_KB], __ATOMIC_RELAXED)) * 1024;
  if (__atomic_load_n(&js[ARC_MEM_JS_STORAGE_VALID], __ATOMIC_RELAXED)) {
    out[ARC_MEM_STORAGE_USAGE] =
        static_cast<int64_t>(static_cast<uint32_t>(js[ARC_MEM_JS_STORAGE_USAGE_KB])) * 1024;
    out[ARC_MEM_STORAGE_QUOTA] =
        static_cast<int64_t>(static_cast<uint32_t>(js[ARC_MEM_JS_STORAGE_QUOTA_KB])) * 1024;
  } else {
    out[ARC_MEM_STORAGE_USAGE] = out[ARC_MEM_STORAGE_QUOTA] = -1;
  }
  const uint32_t test_budget = g_test_budget_mb.load();
  out[ARC_MEM_BUDGET] = static_cast<int64_t>(test_budget ? test_budget : c->budget_mb) * kMB;
  out[ARC_MEM_PAGE_HIDDEN] = __atomic_load_n(&js[ARC_MEM_JS_PAGE_HIDDEN], __ATOMIC_RELAXED) != 0;
}

}  // extern "C"

// --- A scan of the wasm memory by owner (topic wasm-heap-trim) ---------------------
// An iPhone counts every page of the wasm memory the VM ever touched -- read or
// written -- in the tab's footprint, for good: nothing gives a page back. The
// scan tells, by owner, how much of the memory up to the break holds anything
// but zeros: what is in use, and what is free but was touched. It reads every
// page itself, so it touches them all: for a desktop browser or Node, at the
// end of a measurement (Module._arc_mem_scan_print() on the page).

// Every chunk of malloc (shim/memory/dlmalloc_report.c), when that malloc is linked.
extern "C" void arc_malloc_inspect(void (*fn)(void* start, void* end, size_t used, void* arg),
                                   void* arg) __attribute__((weak));
// Zeros calloc and the mappings under 1 MB did not write: untouched memory (the same malloc).
extern "C" uint64_t arc_malloc_fresh_bytes(void) __attribute__((weak));
#ifdef __EMSCRIPTEN__
extern "C" unsigned char __heap_base;
#endif

#ifdef __EMSCRIPTEN__
// Node with ARC_MINCORE (tools/mincore/mincore_addon.c): out[i] = 1 for each
// page of 16 KB of the wasm memory that is resident -- touched, read or
// written. Must run before the scan reads the pages. 0 where it cannot.
EM_JS(int, arc_mem_resident_js, (uint8_t* out, size_t pages), {
  try {
    var m = globalThis.arcMincore;
    if (!m && typeof process == 'object' && process.env && process.env.ARC_MINCORE &&
        typeof require == 'function')
      m = globalThis.arcMincore = require(process.env.ARC_MINCORE);
    return m && m.residency(HEAPU8, out, pages, 16384) ? 1 : 0;
  } catch (e) {
    return 0;
  }
});
#endif

namespace {

const size_t kScanPage = 16384;  // an iPhone's page
enum {
  kScanStatic,  // below the heap: static data, the main thread's stack
  kScanMallocUsed,
  kScanMallocFree,
  kScanSmallMap,
  kScanLargeMapped,
  kScanLargeHole,
  kScanLargeFree,
  kScanOther,  // not malloc's (or malloc unknown) and not a mapping
  kScanKinds
};
const char* const kScanNames[kScanKinds] = {"static",     "malloc used", "malloc free",
                                            "small maps", "large maps",  "holes",
                                            "large free", "other"};

bool PageNonZero(const uint64_t* p) {
  uint64_t acc = 0;
  for (size_t i = 0; i < kScanPage / 8; i += 8) {
    acc |= p[i] | p[i + 1] | p[i + 2] | p[i + 3] | p[i + 4] | p[i + 5] | p[i + 6] | p[i + 7];
    if (acc)
      return true;
  }
  return false;
}

// Bytes in the pages entirely inside [start, end) that are not all zeros.
uint64_t NonZeroIn(uintptr_t start, uintptr_t end) {
  uint64_t n = 0;
  for (uintptr_t p = (start + kScanPage - 1) & ~(kScanPage - 1); p + kScanPage <= end; p += kScanPage)
    n += PageNonZero(reinterpret_cast<const uint64_t*>(p)) ? kScanPage : 0;
  return n;
}

// Bytes of the pages entirely inside [start, end) marked resident in |res| (null: 0).
uint32_t ResidentIn(const uint8_t* res, uintptr_t start, uintptr_t end) {
  if (!res)
    return 0;
  uint32_t n = 0;
  for (uintptr_t p = (start + kScanPage - 1) & ~(kScanPage - 1); p + kScanPage <= end; p += kScanPage)
    n += res[p / kScanPage] ? kScanPage : 0;
  return n;
}

// A range of the detail lines: what, its size, how much of it is not zero.
struct ScanRange {
  uintptr_t start;
  uint32_t bytes, nonzero, resident;
  int8_t kind;  // WASM_MM_* (malloc's free chunks: -1)
  int16_t prot, flags;
};

// Size classes of the detail lines: up to 64 KB, 128, 256, 512 KB, 1, 4, 16 MB, more.
const uint32_t kScanSizes[] = {64 << 10, 128 << 10, 256 << 10, 512 << 10, 1 << 20, 4 << 20, 16 << 20, ~0u};
const int kScanSizeClasses = sizeof(kScanSizes) / sizeof(kScanSizes[0]);
int SizeClass(uint64_t bytes) {
  int c = 0;
  while (bytes > kScanSizes[c])
    ++c;
  return c;
}

struct ScanMap {
  uint8_t* kind;
  size_t pages;
  ScanRange* ranges;  // the biggest kMaxRanges ranges (mappings and malloc's free chunks)
  uint8_t* resident;  // 1: the page is resident (touched), when known
  int nranges;
  static const int kMaxRanges = 48;
  // By size class: count, bytes, not zero -- small mappings, malloc's free chunks.
  uint64_t small[kScanSizeClasses][3], mfree[kScanSizeClasses][3];
  // Which 32nds of the first malloc free chunks of 1-4 MB are not zero ('#').
  char chunk_maps[12][40];
  int nchunk_maps;
  // Every page from |start| to |end| (|whole|: only pages entirely inside).
  void Mark(uintptr_t start, uintptr_t end, uint8_t k, bool whole) {
    size_t from = whole ? (start + kScanPage - 1) / kScanPage : start / kScanPage;
    size_t to = whole ? end / kScanPage : (end + kScanPage - 1) / kScanPage;
    if (to > pages)
      to = pages;
    for (size_t i = from; i < to; ++i)
      kind[i] = k;
  }
  void Keep(const ScanRange& r) {
    int at = nranges;
    if (nranges == kMaxRanges) {
      if (r.bytes <= ranges[kMaxRanges - 1].bytes)
        return;
      at = kMaxRanges - 1;
    } else {
      ++nranges;
    }
    while (at > 0 && ranges[at - 1].bytes < r.bytes) {
      ranges[at] = ranges[at - 1];
      --at;
    }
    ranges[at] = r;
  }
};

void ScanMallocChunk(void* start, void* end, size_t used, void* arg) {
  ScanMap* m = static_cast<ScanMap*>(arg);
  const uintptr_t s = reinterpret_cast<uintptr_t>(start), e = reinterpret_cast<uintptr_t>(end);
  // A page shared by a chunk in use and a free one counts as in use: free
  // chunks mark only pages entirely theirs.
  m->Mark(s, e, used ? kScanMallocUsed : kScanMallocFree, !used);
  if (!used && e - s >= kScanPage) {
    const uint64_t nz = NonZeroIn(s, e);
    uint64_t* h = m->mfree[SizeClass(e - s)];
    h[0]++, h[1] += e - s, h[2] += nz;
    if (e - s >= (1 << 20))
      m->Keep({s, static_cast<uint32_t>(e - s), static_cast<uint32_t>(nz), ResidentIn(m->resident, s, e), -1,
               -1, -1});
    if (e - s >= (1 << 20) && e - s <= (4 << 20) && m->nchunk_maps < 12) {
      char* map = m->chunk_maps[m->nchunk_maps++];
      const uintptr_t part = (e - s) / 32;
      for (int i = 0; i < 32; ++i)
        map[i] = NonZeroIn(s + i * part, s + (i + 1) * part) ? '#' : '.';
      snprintf(map + 32, 8, " %uK", static_cast<unsigned>((e - s) >> 10));
    }
  }
}

void ScanMmRange(uintptr_t start, size_t bytes, int kind, int prot, int flags, void* arg) {
  static const uint8_t kKinds[] = {kScanSmallMap, kScanLargeMapped, kScanLargeHole, kScanLargeFree};
  ScanMap* m = static_cast<ScanMap*>(arg);
  m->Mark(start, start + bytes, kKinds[kind], kind == WASM_MM_SMALL);
  const uint64_t nz = kind == WASM_MM_LARGE_HOLE ? 0 : NonZeroIn(start, start + bytes);
  if (kind == WASM_MM_SMALL) {
    uint64_t* h = m->small[SizeClass(bytes)];
    h[0]++, h[1] += bytes, h[2] += nz;
  } else {
    m->Keep({start, static_cast<uint32_t>(bytes), static_cast<uint32_t>(nz),
             ResidentIn(m->resident, start, start + bytes), static_cast<int8_t>(kind),
             static_cast<int16_t>(prot), static_cast<int16_t>(flags)});
  }
}

}  // namespace

extern "C" {

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void arc_mem_scan_print(void) {
  const uintptr_t brk = reinterpret_cast<uintptr_t>(sbrk(0));
  ScanMap m;
  memset(&m, 0, sizeof(m));
  m.pages = brk / kScanPage;
  m.kind = static_cast<uint8_t*>(malloc(m.pages));
  m.ranges = static_cast<ScanRange*>(malloc(sizeof(ScanRange) * ScanMap::kMaxRanges));
  if (!m.kind || !m.ranges) {
    free(m.kind);
    free(m.ranges);
    return;
  }
  memset(m.kind, arc_malloc_inspect ? kScanOther : kScanMallocUsed, m.pages);
  // Which pages are resident (Node with ARC_MINCORE), before the scan reads them all.
#ifdef __EMSCRIPTEN__
  m.resident = static_cast<uint8_t*>(malloc(m.pages));
  if (m.resident && !arc_mem_resident_js(m.resident, m.pages)) {
    free(m.resident);
    m.resident = nullptr;
  }
#endif
#ifdef __EMSCRIPTEN__
  m.Mark(0, reinterpret_cast<uintptr_t>(&__heap_base), kScanStatic, false);
#endif
  if (arc_malloc_inspect)
    arc_malloc_inspect(ScanMallocChunk, &m);
  wasm_mm_each_range(ScanMmRange, &m);
  uint64_t total[kScanKinds] = {}, nonzero[kScanKinds] = {}, resident[kScanKinds] = {},
           resident_zero[kScanKinds] = {};
  const bool res = m.resident != nullptr;
  for (size_t i = 1; i < m.pages; ++i) {  // page 0: NULL's, never mapped
    const uint8_t k = m.kind[i];
    total[k] += kScanPage;
    const bool nz = PageNonZero(reinterpret_cast<const uint64_t*>(i * kScanPage));
    if (nz)
      nonzero[k] += kScanPage;
    if (res && m.resident[i]) {
      resident[k] += kScanPage;
      if (!nz)
        resident_zero[k] += kScanPage;
    }
  }
  free(m.kind);
  free(m.resident);
  uint64_t all = 0;
  for (int k = 0; k < kScanKinds; ++k)
    all += nonzero[k];
  char line[1024];
  int n = snprintf(line, sizeof(line), "MemScan: break %u MB, not zero %u MB (pages of 16 KB; MB all/not zero):",
                   static_cast<unsigned>(brk / kMB), static_cast<unsigned>(all / kMB));
  for (int k = 0; k < kScanKinds && n > 0 && n < static_cast<int>(sizeof(line)); ++k) {
    if (total[k])
      n += snprintf(line + n, sizeof(line) - n, " %s %u/%u", kScanNames[k],
                    static_cast<unsigned>(total[k] / kMB), static_cast<unsigned>(nonzero[k] / kMB));
  }
  fprintf(stderr, "%s\n", line);
  if (res) {
    uint64_t all_res = 0, all_rz = 0;
    for (int k = 0; k < kScanKinds; ++k)
      all_res += resident[k], all_rz += resident_zero[k];
    n = snprintf(line, sizeof(line), "MemScan: resident (touched) %u MB, of them zeros %u MB (MB resident/zeros):",
                 static_cast<unsigned>(all_res / kMB), static_cast<unsigned>(all_rz / kMB));
    for (int k = 0; k < kScanKinds && n > 0 && n < static_cast<int>(sizeof(line)); ++k) {
      if (resident[k])
        n += snprintf(line + n, sizeof(line) - n, " %s %u/%u", kScanNames[k],
                      static_cast<unsigned>(resident[k] / kMB), static_cast<unsigned>(resident_zero[k] / kMB));
    }
    fprintf(stderr, "%s\n", line);
  }
  // By size class (up to KB): count, MB, MB not zero.
  for (int which = 0; which < 2; ++which) {
    n = snprintf(line, sizeof(line), "MemScan: %s by size (up to KB: n MB/not zero):",
                 which ? "malloc's free chunks" : "small maps");
    for (int c = 0; c < kScanSizeClasses && n > 0 && n < static_cast<int>(sizeof(line)); ++c) {
      const uint64_t* h = which ? m.mfree[c] : m.small[c];
      if (h[0])
        n += snprintf(line + n, sizeof(line) - n, " %u: %u %.1f/%.1f",
                      kScanSizes[c] == ~0u ? 0u : kScanSizes[c] >> 10, static_cast<unsigned>(h[0]),
                      h[1] / 1048576.0, h[2] / 1048576.0);
    }
    fprintf(stderr, "%s\n", line);
  }
  {
    static const char* const kSites[WASM_MM_ZERO_SITES] = {"small maps", "large reuse", "MAP_FIXED",
                                                          "madvise", "calloc", "guest", "pending"};
    uint64_t bytes[WASM_MM_ZERO_SITES], zeros[WASM_MM_ZERO_SITES], unique[WASM_MM_ZERO_SITES];
    wasm_mm_zero_stats(bytes, zeros, unique);
    n = snprintf(line, sizeof(line), "MemScan: zeros written (MB / over pages all zero before / distinct such pages; "
                 "ARC_MEM_ZERO_STATS=1):");
    for (int i = 0; i < WASM_MM_ZERO_SITES; ++i)
      n += snprintf(line + n, sizeof(line) - n, " %s %.1f/%.1f/%.1f", kSites[i], bytes[i] / 1048576.0,
                    zeros[i] / 1048576.0, unique[i] / 1048576.0);
    if (arc_malloc_fresh_bytes)
      n += snprintf(line + n, sizeof(line) - n, "; not written (untouched) %.1f",
                    arc_malloc_fresh_bytes() / 1048576.0);
    fprintf(stderr, "%s\n", line);
  }
  n = snprintf(line, sizeof(line), "MemScan: tls %u B; free chunks of 1-4 MB, 32nds not zero:",
               static_cast<unsigned>(__builtin_wasm_tls_size()));
  for (int i = 0; i < m.nchunk_maps; ++i)
    n += snprintf(line + n, sizeof(line) - n, " %s", m.chunk_maps[i]);
  fprintf(stderr, "%s\n", line);
  // The biggest ranges.
  static const char* const kRangeNames[] = {"malloc-free", "small", "map", "hole", "free"};
  n = snprintf(line, sizeof(line), "MemScan: biggest (MB/not zero[/resident] kind prot flags; at):");
  for (int i = 0; i < m.nranges; ++i) {
    const ScanRange& r = m.ranges[i];
    if (n + 72 >= static_cast<int>(sizeof(line))) {
      fprintf(stderr, "%s\n", line);
      n = snprintf(line, sizeof(line), "MemScan: biggest (cont.):");
    }
    n += snprintf(line + n, sizeof(line) - n, " %.1f/%.1f", r.bytes / 1048576.0, r.nonzero / 1048576.0);
    if (res)
      n += snprintf(line + n, sizeof(line) - n, "/r%.1f", r.resident / 1048576.0);
    n += snprintf(line + n, sizeof(line) - n, " %s@%lx", kRangeNames[r.kind + 1],
                  static_cast<unsigned long>(r.start));
    if (r.kind == WASM_MM_LARGE_MAPPED)
      n += snprintf(line + n, sizeof(line) - n, " p%d f%x", r.prot, r.flags);
  }
  fprintf(stderr, "%s\n", line);
  free(m.ranges);
}

int64_t arc_mem_trim(int level) {
  int64_t freed = 0;
  if (arc_media_mem_trim)
    freed += arc_media_mem_trim(level);
  if (arc_sf_mem_trim)
    freed += arc_sf_mem_trim(level);
  if (ndk_cpu_mem_trim)
    freed += ndk_cpu_mem_trim(level);
  if (ndk64_cpu_mem_trim)
    freed += ndk64_cpu_mem_trim(level);
  // dlmalloc gives the top of its arena back to sbrk when it can (so the
  // next large mapping or arena growth reuses it).
  malloc_trim(0);
  return freed;
}

#ifdef __EMSCRIPTEN__
EM_JS(void, arc_mem_reload_page, (), {
  // On the page's thread: the page's own handler, else a reload.
  if (typeof Module.arcMemRestart == 'function') Module.arcMemRestart();
  else if (typeof location == 'object') location.reload();
});

static void ReloadPage() {
  arc_mem_reload_page();
}
#endif

int arc_mem_request_restart(void) {
#ifdef __EMSCRIPTEN__
  if (!__atomic_load_n(&g_js_cells[ARC_MEM_JS_RESTART], __ATOMIC_RELAXED))
    return 0;
  emscripten_async_run_in_main_runtime_thread(EM_FUNC_SIG_V, reinterpret_cast<void*>(ReloadPage));
  return 1;
#else
  return 0;
#endif
}

// The heap options of the budget, before the VM's own (art8/artvm8.cc,
// art/artvm.cc):
// -Xms, -Xmx and the growth limit as dalvik.vm.heap* give them to
// AndroidRuntime on a device. Returns the number written (each up to |len|).
int arc_mem_budget_vm_options(char (*out)[48], int max) {
  const ArcMemConfig* c = arc_mem_config();
  if (max < 3)
    return 0;
  snprintf(out[0], 48, "-Xms%um", c->heap_start_mb);
  snprintf(out[1], 48, "-Xmx%um", c->heap_size_mb);
  snprintf(out[2], 48, "-XX:HeapGrowthLimit=%um", c->heap_growth_mb);
  fprintf(stderr, "membudget: budget %u MB, wasm memory up to %u MB, heap %u/%u MB%s\n",
          c->budget_mb, c->cap_mb, c->heap_growth_mb, c->heap_size_mb,
          c->low_ram ? ", low-RAM device" : "");
  return 3;
}

// The properties the framework reads the budget from (after build.prop is
// loaded): ActivityManager.getMemoryClass/getLargeMemoryClass,
// isLowRamDevice.
void arc_mem_budget_publish_properties(void) {
  const ArcMemConfig* c = arc_mem_config();
  if (!__system_property_set)
    return;
  SetProp("dalvik.vm.heapstartsize", "%um", c->heap_start_mb);
  SetProp("dalvik.vm.heapgrowthlimit", "%um", c->heap_growth_mb);
  SetProp("dalvik.vm.heapsize", "%um", c->heap_size_mb);
  __system_property_set("ro.config.low_ram", c->low_ram ? "true" : "false");
  SetProp("ro.arcwasm.mem.budget_mb", "%u", c->budget_mb);
  SetProp("ro.arcwasm.mem.cap_mb", "%u", c->cap_mb);
}

}  // extern "C"
