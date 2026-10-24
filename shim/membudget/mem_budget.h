// The app's memory budget (topic mem-budget, docs/topics/mem-budget.md).
//
// Everything of an app lives in one wasm32 linear memory -- Java heap,
// native heap, extracted dex, ARM libraries, codec buffers -- and that
// memory never shrinks; iOS reloads a tab whose footprint grows too large.
// So the VM is given a budget, as a phone has its RAM, and does what
// Android does to stay inside it:
//
//   budget   set at start (the page: device and ?mem=, Node: ARC_MEM_BUDGET_MB
//            or --mem=MB): MemTotal of /proc/meminfo, ActivityManager's
//            memory classes and MemoryInfo, dalvik.vm.heap* and -Xmx;
//   cap      the wasm memory's maximum (WebAssembly.Memory, mem_budget_pre.js):
//            growing past it fails inside the VM (an OOM the app can see)
//            before the browser kills the tab;
//   pressure in-use memory against the budget, sampled by the system server's
//            MemoryMonitor (framework/java-arcwasm8/arcwasm/server): trims
//            (ComponentCallbacks2.onTrimMemory, onLowMemory, and the caches of
//            this layer below: arc_mem_trim) and, last, lowmemorykiller's step:
//            state saved, the VM restarted, the task restored.
//
// The layers below report and trim through weak functions (no link
// dependency: each is there when its library is linked in):
//   arc_gralloc_mem_stats   shim/fw6-graphics/gralloc_sw.cc
//   arc_media_mem_stats/_trim  shim/media/media_codec.cc
//   ndk_cpu_mem_stats/_trim    shim/ndk/cpu/runtime.cc, jit.cc
//   ndk64_cpu_mem_stats/_trim  shim/ndk/cpu64/runtime64.cc
//   art_jit_mem_stats       shim/art8/dex_jit_art8.cc, shim/art10/dex_jit_art10.cc
//   arc_sf_mem_trim         shim/fw8-graphics/surface_flinger8.cc
//   arc_art_mem_maps        shim/membudget/art_maps8.cc (ART 8.1's MemMaps)
// Part of the arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_MEMBUDGET_MEM_BUDGET_H_
#define ARC_WASM_SHIM_MEMBUDGET_MEM_BUDGET_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// What the budget sets, MB unless named otherwise. Filled once, from the
// environment (ARC_MEM_BUDGET_MB, ARC_MEM_CAP_MB; mem_budget_pre.js sets
// them in the browser), before the VM starts.
typedef struct ArcMemConfig {
  uint32_t budget_mb;        // the "RAM": MemTotal, MemoryInfo.totalMem
  uint32_t cap_mb;           // the wasm memory's maximum (4096: none)
  uint32_t heap_start_mb;    // dalvik.vm.heapstartsize, -Xms
  uint32_t heap_growth_mb;   // dalvik.vm.heapgrowthlimit: getMemoryClass()
  uint32_t heap_size_mb;     // dalvik.vm.heapsize, -Xmx: getLargeMemoryClass()
  int low_ram;               // ro.config.low_ram (ActivityManager.isLowRamDevice)
  // Media (shim/media): decoders at once per kind before an idle one is
  // reclaimed (getMaxSupportedInstances), buffers per video decoder and
  // chunks queued in a WebCodecs decoder.
  int max_video_decoders;
  int max_audio_decoders;
  int video_inputs;
  int video_outputs;
  int decode_queue;
  // Fractions of the budget (per mille) for the trim levels.
  int moderate_permille, low_permille, critical_permille;
  // JIT code (topic iphone-check, docs/topics/iphone-check.md). Each thread
  // compiles and instantiates the wasm it runs itself, outside the wasm
  // memory: an iPhone's tab pays for every copy. 0: no limit.
  int cpu_jit_threshold;       // ARM + A64: runs of a block before it is translated
                               // (0: the translators' own, 50; -1: never)
  int cpu_jit_per_thread;      // ARM + A64 translations one thread instantiates
  int cpu_jit_thread_hits;     // runs of a translated block on a thread before
                               // that thread instantiates it too
  uint32_t cpu_jit_kb;         // translations' module bytes kept in all (wasm memory)
  uint32_t dex_jit_thread_kb;  // dex-jit: wasm one thread instantiates
  uint32_t dex_jit_kb;         // dex-jit: wasm compiled in all
  // ARM + A64 (topic shared-translations): guest instructions, in millions,
  // a thread runs before it instantiates translations at all (a pool's
  // threads that run little keep none).
  uint32_t cpu_jit_thread_minsns;
  // An arm64 guest's PROT_NONE reservation from this size up is a window of
  // addresses whose wasm memory comes as the guest commits it
  // (shim/ndk/ndk_libc.cc, GuestMmap64: Hermes reserves 3 GB per runtime).
  uint32_t guest_reserve_mb;
  // ARM + A64 (topic jit-module-heap): translations instantiated on all
  // threads together (each thread's copy counts; 0: no limit). Each costs
  // a thread's JavaScriptCore ~10 KB of heap and ~4 KB of JIT code, and the
  // carriers of green threads each have their own copies.
  uint32_t cpu_jit_total;
} ArcMemConfig;

// Weak, for the layers that use it when the topic is linked in (and
// defaults when not): if (arc_mem_config) ...
const ArcMemConfig* arc_mem_config(void) __attribute__((weak));
// Test hooks (MemoryTest): another budget at run time (as on a smaller
// device; the heap limits stay), other decoder limits. 0 leaves a value.
void arc_mem_set_budget_for_test(uint32_t budget_mb);
void arc_mem_set_decoder_limits_for_test(int video, int audio);

// Cells the page's JavaScript writes (mem_budget_pre.js, webcodecs.js) and C
// reads: memory outside the wasm memory and what only the browser knows.
enum {
  ARC_MEM_JS_FRAMES = 0,       // VideoFrames open (decoded, not yet copied out)
  ARC_MEM_JS_FRAME_KB,         // their size (allocationSize), KB
  ARC_MEM_JS_DECODERS,         // WebCodecs decoders open
  ARC_MEM_JS_STORAGE_USAGE_KB, // navigator.storage.estimate()
  ARC_MEM_JS_STORAGE_QUOTA_KB,
  ARC_MEM_JS_STORAGE_VALID,    // 1 once estimate() answered
  ARC_MEM_JS_PAGE_HIDDEN,      // document.visibilityState == 'hidden'
  ARC_MEM_JS_CAP_MB,           // the maximum the memory was created with
  ARC_MEM_JS_RESTART,          // 1: the page reloads itself on request
  ARC_MEM_JS_THREADS,          // pthreads running (the main thread's PThread)
  ARC_MEM_JS_PRESSURE_PM,      // C writes: in use, per mille of the budget (the monitor's last sample)
  ARC_MEM_JS_USER_RESTART,     // the page writes 1: the user asked for a restart (the shell's button)
  ARC_MEM_JS_TOUCHED_MB,       // C writes: the wasm memory touched (MB, MemoryBudget.touched): what of
                               // it the tab holds, for the page's "low memory"; 0 unknown
  ARC_MEM_JS_COUNT = 16
};
int32_t* arc_mem_js_cells(void) __attribute__((weak));

// One sample of what memory holds, bytes (counts where named so). Indexes
// of arc_mem_sample's array (and of MemoryBudget.sample() in Java).
enum {
  ARC_MEM_WASM = 0,         // linear memory size
  ARC_MEM_CAP,              // its maximum
  ARC_MEM_MALLOC_ARENA,     // malloc: from sbrk
  ARC_MEM_MALLOC_USED,      //   in use (-1: not measured in this sample)
  ARC_MEM_MALLOC_FREE,      //   free inside the arena (-1: not measured)
  ARC_MEM_MAPPED,           // mmap pages (shim/wasm_mm), incl. Java spaces
  ARC_MEM_LARGE_FREE,       // freed large mappings kept for reuse, and holes
                            // unmapped inside live ones
  ARC_MEM_DEX,              // ART: dex files in memory
  ARC_MEM_JAVA_SPACES,      // ART: heap spaces' reservations
  ARC_MEM_JAVA_LOS,         // ART: large objects (their own maps)
  ARC_MEM_ART_OTHER,        // ART: bitmaps, card table, linear alloc...
  ARC_MEM_ARM_LIBS,         // NDK: ARM libraries' segments
  ARC_MEM_ARM_BLOCKS,       // NDK: decoded blocks of the ARM interpreter
  ARC_MEM_ARM_JIT,          // JIT code kept (wasm module bytes): ARM and A64
                            // translations, dex-jit's methods
  ARC_MEM_THREADS,          // count: threads (each with its stack)
  ARC_MEM_STACKS,           // their stacks (estimate)
  ARC_MEM_GRALLOC,          // graphic buffers
  ARC_MEM_GRALLOC_COUNT,    // count
  ARC_MEM_CODEC,            // MediaCodec buffers (inputs, outputs, staging)
  ARC_MEM_CODEC_COUNT,      // count: codecs started
  ARC_MEM_JS_FRAMES_N,      // count: VideoFrames open (outside wasm)
  ARC_MEM_JS_FRAME_BYTES,   // their bytes
  ARC_MEM_STORAGE_USAGE,    // OPFS (navigator.storage), -1 unknown
  ARC_MEM_STORAGE_QUOTA,
  ARC_MEM_BUDGET,           // the budget now (bytes)
  ARC_MEM_PAGE_HIDDEN,      // 0/1
  ARC_MEM_BRK,              // the heap's break (sbrk(0)): the memory handed out
                            // (the rest of the wasm memory is growth slack)
  ARC_MEM_IMAGE,            // the /system image kept in wasm memory (readonly_file.cc,
                            // topic unity-mmap): in malloc and the mapped pages, but not
                            // "in use" -- its JavaScript copy, which it replaces, was not.
                            // Its place, reserved; what is filled of it is below
  ARC_MEM_IMAGE_FILLED,     // the image's pages filled so far (topic image-on-demand: from
                            // its source when first used; the whole image if read whole)
  ARC_MEM_UNTOUCHED,        // below the break, known never touched: fresh pages and holes of
                            // large ranges, malloc's top never handed out (an iPhone counts
                            // only touched pages of the wasm memory: MemoryBudget.touched)
  ARC_MEM_SAMPLE_COUNT
};
// Fills |out| (ARC_MEM_SAMPLE_COUNT values). |precise|: also walk malloc's
// chunks (mallinfo: takes malloc's lock for a while; the monitor asks only
// near the budget).
void arc_mem_sample(int64_t* out, int precise);

// Drops caches of this layer for trim |level| (ComponentCallbacks2's
// TRIM_MEMORY_* values); returns bytes known to be freed.
int64_t arc_mem_trim(int level);

// Asks for the VM to be restarted (lowmemorykiller's step, after the
// state is saved): the page reloads. Returns 0 when there is no page that
// can (Node): the caller exits with ARC_MEM_RESTART_EXIT instead.
int arc_mem_request_restart(void);
#define ARC_MEM_RESTART_EXIT 75

// The VM's start (art8/artvm8.cc, art/artvm.cc): -Xms, -Xmx and the growth
// limit of the budget's device class (|out|: up to |max| options; returns
// how many), and, once build.prop is loaded, dalvik.vm.heap*,
// ro.config.low_ram.
int arc_mem_budget_vm_options(char (*out)[48], int max);
void arc_mem_budget_publish_properties(void);

// --- what the layers below provide (weak) ----------------------------------------
void arc_gralloc_mem_stats(uint64_t* bytes, uint32_t* buffers) __attribute__((weak));
void arc_media_mem_stats(uint64_t* bytes, uint32_t* codecs) __attribute__((weak));
int64_t arc_media_mem_trim(int level) __attribute__((weak));
void ndk_cpu_mem_stats(uint64_t* block_bytes, uint64_t* jit_bytes) __attribute__((weak));
int64_t ndk_cpu_mem_trim(int level) __attribute__((weak));
void ndk64_cpu_mem_stats(uint64_t* block_bytes, uint64_t* jit_bytes) __attribute__((weak));
int64_t ndk64_cpu_mem_trim(int level) __attribute__((weak));
// dex-jit (shim/art8/dex_jit_art8.cc, shim/art10/dex_jit_art10.cc): wasm compiled.
void art_jit_mem_stats(uint64_t* code_bytes) __attribute__((weak));
int64_t arc_sf_mem_trim(int level) __attribute__((weak));
// ART's MemMaps by kind (art_maps8.cc): dex, heap spaces, large objects, other.
void arc_art_mem_maps(int64_t* dex, int64_t* spaces, int64_t* los, int64_t* other)
    __attribute__((weak));

#ifdef __cplusplus
}
#endif

#endif  // ARC_WASM_SHIM_MEMBUDGET_MEM_BUDGET_H_
