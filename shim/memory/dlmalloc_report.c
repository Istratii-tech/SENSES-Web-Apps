// Emscripten's dlmalloc (and its sbrk) with a report where it would abort:
// which of its checks failed and the first damaged chunk of the heap, with
// the bytes around it, so that what overwrote malloc's data shows (text,
// pixels, samples, pointers...). Linked with -sMALLOC=none in its place
// (mk/topic-pwa.mk). Topic userspace: Angry Birds 2.1.0 corrupts the heap
// on iPhones only, and all a phone gave was "abort() in malloc".
//
// dlmalloc's own checks without DEBUG: the free lists' links when a chunk
// is taken off one (CORRUPTION_ERROR_ACTION), and the chunk free() and
// realloc() are given (USAGE_ERROR_ACTION: not malloc's, or not in use: a
// double free). Each calls ABORT, here arc_dlmalloc_failed(its line).
#include <emscripten/console.h>
#include <emscripten/emscripten.h>
#include <emscripten/em_macros.h>
#include <emscripten/heap.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// dlmalloc's global lock, held around its calls to sbrk, is the break's
// lock of the mmap allocator (shim/wasm_mm/wasm_mmap.cc), which moves the
// break too: without a common lock dlmalloc could take memory a large
// mapping had (it extends what one sbrk gave with the next, assuming they
// follow each other). Programs without that allocator use a lock of
// their own.
void arc_sbrk_lock(void) __attribute__((weak));
void arc_sbrk_unlock(void) __attribute__((weak));
static pthread_mutex_t arc_own_sbrk_mutex = PTHREAD_MUTEX_INITIALIZER;
static void arc_global_lock(void) {
  if (arc_sbrk_lock) arc_sbrk_lock(); else pthread_mutex_lock(&arc_own_sbrk_mutex);
}
static void arc_global_unlock(void) {
  if (arc_sbrk_unlock) arc_sbrk_unlock(); else pthread_mutex_unlock(&arc_own_sbrk_mutex);
}
#define ACQUIRE_MALLOC_GLOBAL_LOCK() arc_global_lock();
#define RELEASE_MALLOC_GLOBAL_LOCK() arc_global_unlock();

static void arc_dlmalloc_failed(int line, const void* given) __attribute__((noreturn));
// Out of memory, malloc returns NULL (ENOMEM), and says so once. A check
// that fails after that is most likely a program that went on with the NULL
// (Subway Surfers 3.70 when its reservations took all the wasm memory, topic
// arm64-lazy-reserve: it wrote through it, then freed a pointer made from
// it): the abort says out of memory then, and the page says so too.
#include <errno.h>
static _Atomic int arc_out_of_memory;
static void arc_malloc_failure(void) {
  errno = ENOMEM;
  if (atomic_fetch_add(&arc_out_of_memory, 1) == 0) {
    char say[120];
    snprintf(say, sizeof(say), "dlmalloc: out of memory: the wasm memory cannot grow past %lu MB",
             (unsigned long)(emscripten_get_heap_size() >> 20));
    emscripten_err(say);
  }
}
#define MALLOC_FAILURE_ACTION arc_malloc_failure();
#define ABORT arc_dlmalloc_failed(__LINE__, 0)
#define USAGE_ERROR_ACTION(m, p) arc_dlmalloc_failed(__LINE__, (p))
// For the scan of the wasm memory by owner (arc_malloc_inspect below).
#define MALLOC_INSPECT_ALL 1
// --- Memory never handed out (topic wasm-heap-trim; tools/dlmalloc_arc.py) ----------
// An iPhone counts a page of the wasm memory in the tab's footprint from its
// first touch on (a read too), for good. dlmalloc's top chunk moves up only
// into fresh memory from sbrk: below arc_touched_end malloc has handed memory
// out or written its records, above it the heap is untouched zeros. calloc
// and arc_memalign_zeroed (the mmap allocator's mappings under 1 MB,
// shim/wasm_mm) write zeros only below it.
#include <string.h>
static uintptr_t arc_touched_end;  // under malloc's lock
static uintptr_t arc_top_foot;     // the last fake head init_top wrote at the top's end
// This thread's last cut off the top (of its current allocation): the chunk,
// its end, and where in it the untouched part starts.
static __thread uintptr_t arc_cut_start, arc_cut_end, arc_cut_fresh;
static _Atomic uint64_t arc_fresh_bytes;  // zeros not written (statistics)
void wasm_mm_zero(void* p, size_t n, int site) __attribute__((weak));

static inline void arc_top_moved(void* top) {
  const uintptr_t end = (uintptr_t)top + 64;  // the top's header, and a margin
  if (end > arc_touched_end)
    arc_touched_end = end;
}
static inline void arc_top_carved(void* chunk, void* top) {
  arc_cut_start = (uintptr_t)chunk;
  arc_cut_end = (uintptr_t)top;
  arc_cut_fresh = arc_touched_end > arc_cut_start ? arc_touched_end : arc_cut_start;
  arc_top_moved(top);
}
// init_top writes a fake chunk's head at the top's end; when the segment
// grows, the last one is inside the top: zeros again, as the rest of it.
static inline void arc_top_foot_at(void* top, size_t size) {
  const uintptr_t start = (uintptr_t)top, foot = start + size;
  if (arc_top_foot >= start && arc_top_foot + 16 <= foot)
    memset((void*)arc_top_foot, 0, 16);
  arc_top_foot = foot;
}
static int arc_zero_check(void);
static int arc_fresh_on(void);
// Zeros over |mem| (|n| bytes), but not over what the allocation that just
// returned it cut off untouched memory: that is zeros already.
static void arc_clear(void* mem, size_t n) {
  const uintptr_t p = (uintptr_t)mem, end = p + n;
  uintptr_t to = end;
  if (arc_cut_end && p >= arc_cut_start && end <= arc_cut_end && arc_cut_fresh < end && arc_fresh_on()) {
    to = arc_cut_fresh > p + 64 ? arc_cut_fresh : p + 64;
    if (to > end)
      to = end;
  }
  if (wasm_mm_zero && to - p >= 16384)
    wasm_mm_zero(mem, to - p, 4 /* WASM_MM_ZERO_CALLOC: statistics */);
  else
    memset(mem, 0, to - p);
  if (to < end) {
    arc_fresh_bytes += end - to;
    // Its last bytes share a page with the next chunk's head: zeros there cost nothing.
    if (end - to > 64)
      memset((void*)(end - 64), 0, 64);
    if (arc_zero_check()) {
      for (const unsigned char* c = (const unsigned char*)to; c < (const unsigned char*)end; c++) {
        if (*c) {
          char say[160];
          snprintf(say, sizeof(say), "dlmalloc: untouched memory is not zero: %p in %p+%lu (fresh from %p)",
                   (const void*)c, mem, (unsigned long)n, (void*)to);
          emscripten_err(say);
          memset((void*)to, 0, end - to);
          break;
        }
      }
    }
  }
  arc_cut_start = arc_cut_end = 0;
}
// ARC_MALLOC_WHERE=MIN-MAX (bytes): each malloc of that size, with where it
// comes from (diagnostics: who takes memory in a burst).
static void arc_malloc_where(size_t n);
#define ARC_MALLOC_ENTER(n) arc_malloc_where(n)
#define ARC_TOP_MOVED(p) arc_top_moved(p)
#define ARC_TOP_FOOT(p, n) arc_top_foot_at(p, n)
#define ARC_TOP_CARVED(c, t) arc_top_carved(c, t)
#define ARC_FRESH_RESET() (arc_cut_start = arc_cut_end = 0)
#define ARC_CLEAR(mem, n) arc_clear(mem, n)

#include "dlmalloc_arc.c"
#include "libc/sbrk.c"

// Every chunk of the heap, in use (|used| > 0) or free (the top chunk too),
// under malloc's lock: |fn| must not allocate (shim/membudget/mem_budget.cc,
// topic wasm-heap-trim).
void arc_malloc_inspect(void (*fn)(void* start, void* end, size_t used, void* arg), void* arg) {
  dlmalloc_inspect_all(fn, arg);
}

// memalign, zeroed as calloc is: zeros only where the memory may have been
// touched (the mmap allocator's mappings under 1 MB, shim/wasm_mm).
void* arc_memalign_zeroed(size_t alignment, size_t bytes) {
  ARC_FRESH_RESET();
  void* mem = dlmemalign(alignment, bytes);
  if (mem)
    arc_clear(mem, bytes);
  return mem;
}

// Bytes calloc and arc_memalign_zeroed did not need to zero.
uint64_t arc_malloc_fresh_bytes(void) {
  return arc_fresh_bytes;
}

// The part of the top chunk malloc never handed out: untouched (an iPhone
// does not count it; the memory monitor's "touched", topic senses-shell-fixes).
uint64_t arc_malloc_untouched_bytes(void) {
  uint64_t n = 0;
  if (!PREACTION(gm)) {
    const uintptr_t top = (uintptr_t)gm->top, end = top + gm->topsize;
    const uintptr_t from = arc_touched_end > top ? arc_touched_end : top;
    if (top && end > from)
      n = end - from;
    POSTACTION(gm);
  }
  return n;
}

// A switch from the environment, read once the program's environment is
// there (the first mallocs come before Emscripten fills it in): -1 until then.
static int arc_env_int(const char* name, int unset) {
  if (!getenv("HOME"))
    return -1;
  const char* v = getenv(name);
  return v ? atoi(v) : unset;
}

static void arc_malloc_where(size_t n) {
  static int state;  // 0: not read yet, 1: off, 2: on
  static size_t lo, hi;
  if (state == 1)
    return;
  if (state == 0) {
    if (!getenv("HOME"))
      return;
    const char* v = getenv("ARC_MALLOC_WHERE");
    if (v) {
      char* end;
      lo = strtoul(v, &end, 10);
      hi = *end == '-' ? strtoul(end + 1, 0, 10) : lo;
    }
    state = v ? 2 : 1;
  }
  if (n < lo || n > hi)
    return;
  char say[1400];
  int k = snprintf(say, 120, "dlmalloc: malloc %lu at ", (unsigned long)n);
  emscripten_get_callstack(EM_LOG_C_STACK | EM_LOG_NO_PATHS, say + k, sizeof(say) - k);
  for (char* c = say; *c; c++)
    if (*c == '\n') *c = '|';
  emscripten_err(say);
}

// ARC_MALLOC_FRESH=0: zeros everywhere, as Emscripten's calloc (to compare).
static int arc_fresh_on(void) {
  static int on = -1;
  if (on < 0) {
    const int v = arc_env_int("ARC_MALLOC_FRESH", 1);
    if (v < 0)
      return 1;
    on = v != 0;
  }
  return on;
}

// ARC_MEM_ZERO_STATS=5: what calloc leaves as untouched is checked to be zeros (a test).
static int arc_zero_check(void) {
  static int level = -1;
  if (level < 0) {
    const int v = arc_env_int("ARC_MEM_ZERO_STATS", 0);
    if (v < 0)
      return 0;
    level = v;
  }
  return level >= 5;
}

// Prints without allocating (malloc's lock is held, and its data is bad).
#define SAY(...)                                     \
  do {                                               \
    char say_[240];                                  \
    snprintf(say_, sizeof(say_), __VA_ARGS__);       \
    emscripten_err(say_);                            \
  } while (0)

static int arc_readable(uintptr_t a, size_t n) {
  return a >= 1024 && a + n <= emscripten_get_heap_size() && a + n > a;
}

// |from| .. |to| as words and characters, 32 bytes a line.
static void arc_dump(uintptr_t from, uintptr_t to) {
  from &= ~(uintptr_t)31;
  for (uintptr_t a = from; a < to; a += 32) {
    if (!arc_readable(a, 32))
      continue;
    const uint32_t* w = (const uint32_t*)a;
    const unsigned char* c = (const unsigned char*)a;
    char text[33];
    for (int i = 0; i < 32; i++)
      text[i] = c[i] >= 0x20 && c[i] < 0x7f ? c[i] : '.';
    text[32] = 0;
    SAY("  %08lx: %08x %08x %08x %08x %08x %08x %08x %08x |%s|", (unsigned long)a, w[0], w[1],
        w[2], w[3], w[4], w[5], w[6], w[7], text);
  }
}

static void arc_report_chunk(const char* why, mstate m, msegmentptr sp, mchunkptr q,
                             mchunkptr prev) {
  SAY("dlmalloc: %s: chunk %p (head %08lx, size %lu) in segment %p+%lu; the chunk before: "
      "%p (head %08lx)", why, (void*)q, (unsigned long)q->head, (unsigned long)chunksize(q),
      sp ? (void*)sp->base : 0, sp ? (unsigned long)sp->size : 0ul, (void*)prev,
      prev ? (unsigned long)prev->head : 0ul);
  if (!cinuse(q))
    SAY("dlmalloc: free chunk's links: fd %p bk %p", (void*)q->fd, (void*)q->bk);
  if (prev && (uintptr_t)q - (uintptr_t)prev > 256) {
    SAY("dlmalloc: start of the chunk before:");
    arc_dump((uintptr_t)prev, (uintptr_t)prev + 96);
  }
  SAY("dlmalloc: around the chunk:");
  arc_dump((uintptr_t)q - 160, (uintptr_t)q + 256);
  (void)m;
}

// A free chunk's list neighbour: a chunk of the heap, or a bin's head (in
// malloc's own state).
static int arc_link_ok(mstate m, mchunkptr p) {
  if ((char*)p >= (char*)m && (char*)p < (char*)(m + 1))
    return 1;
  return arc_readable((uintptr_t)p, sizeof(struct malloc_chunk)) && ok_address(m, p);
}

// Walks every segment chunk by chunk; reports up to three chunks that are
// not right. Returns how many it found.
static unsigned long arc_chunks, arc_bytes;
static int arc_segments;
static int arc_walk(mstate m) {
  int segments = 0, found = 0;
  arc_chunks = arc_bytes = 0;
#define ARC_BAD(...)                         \
  do {                                       \
    arc_report_chunk(__VA_ARGS__);           \
    if (++found >= 3) return found;          \
  } while (0)
  for (msegmentptr sp = &m->seg; sp != 0 && segments < 4096; sp = sp->next, segments++) {
    if (!arc_readable((uintptr_t)sp, sizeof(*sp)) || !arc_readable((uintptr_t)sp->base, sp->size)) {
      SAY("dlmalloc: segment record %p is damaged (base %p size %lu)", (void*)sp,
          (void*)sp->base, (unsigned long)sp->size);
      return found + 1;
    }
    arc_segments = segments + 1;
    mchunkptr q = align_as_chunk(sp->base);
    mchunkptr prev = 0;
    while (segment_holds(sp, q) && q != m->top && q->head != FENCEPOST_HEAD) {
      size_t sz = chunksize(q);
      char* end = sp->base + sp->size;
      if (sz < MIN_CHUNK_SIZE || (sz & CHUNK_ALIGN_MASK) != 0 || (char*)q + sz > end) {
        ARC_BAD("bad size (the rest of this segment is not walked)", m, sp, q, prev);
        break;
      }
      if (prev && !pinuse(q) != !cinuse(prev))
        ARC_BAD("its in-use bit for the chunk before is wrong", m, sp, q, prev);
      mchunkptr next = (mchunkptr)((char*)q + sz);
      if (!cinuse(q)) {
        if (prev && !cinuse(prev))
          ARC_BAD("two free chunks side by side", m, sp, q, prev);
        if ((char*)next < end && next->prev_foot != sz)
          ARC_BAD("the next chunk's record of this free one's size is wrong", m, sp, q, prev);
        if (q != m->dv) {
          if (!arc_link_ok(m, q->fd) || !arc_link_ok(m, q->bk)) {
            ARC_BAD("free chunk's list links are damaged", m, sp, q, prev);
          } else {
            mchunkptr bad = q->fd->bk != q ? q->fd : q->bk->fd != q ? q->bk : 0;
            if (bad && (char*)bad >= (char*)m && (char*)bad < (char*)(m + 1)) {
              // Linked to a bin that does not list it: free by its header,
              // not by malloc's lists (an old header written back?).
              SAY("dlmalloc: free chunk %p links to bin head %p, whose list is %p .. %p",
                  (void*)q, (void*)bad, (void*)bad->fd, (void*)bad->bk);
              ARC_BAD("free chunk not on its bin's list", m, sp, q, prev);
            } else if (bad) {
              SAY("dlmalloc: free chunk %p's list neighbour %p does not link back", (void*)q,
                  (void*)bad);
              ARC_BAD("free chunk's list neighbour is damaged", m, segment_holding(m, (char*)bad),
                      bad, 0);
            }
          }
        }
      }
      arc_chunks++;
      arc_bytes += sz;
      prev = q;
      q = next;
    }
  }
#undef ARC_BAD
  return found;
}

// For the page after a thread's wasm trap (a damaged pointer read inside
// malloc traps before any of its checks): the same walk, from outside.
EMSCRIPTEN_KEEPALIVE void arc_dlmalloc_walk(void) {
  SAY("dlmalloc: walking the heap after a crash");
  if (!arc_walk(gm))
    SAY("dlmalloc: every chunk looks right (%lu chunks, %lu MB, %d segments)", arc_chunks,
        arc_bytes >> 20, arc_segments);
  SAY("dlmalloc: top %p size %lu, dv %p size %lu, least address %p", (void*)gm->top,
      (unsigned long)gm->topsize, (void*)gm->dv, (unsigned long)gm->dvsize,
      (void*)gm->least_addr);
}

static void arc_dlmalloc_failed(int line, const void* given) {
  static _Atomic int once;
  if (atomic_fetch_add(&once, 1) == 0) {
    SAY("dlmalloc: check at dlmalloc.c:%d failed (heap damaged, or a pointer freed twice or "
        "not from malloc); walking the heap", line);
    if (given) {
      SAY("dlmalloc: the chunk given (free/realloc): %p", given);
      arc_dump((uintptr_t)given - 96, (uintptr_t)given + 96);
    }
    if (!arc_walk(gm))
      SAY("dlmalloc: every chunk looks right (%lu chunks, %lu MB; a bin's links, or the pointer "
          "given to free)", arc_chunks, arc_bytes >> 20);
    SAY("dlmalloc: top %p size %lu, dv %p size %lu, least address %p", (void*)gm->top,
        (unsigned long)gm->topsize, (void*)gm->dv, (unsigned long)gm->dvsize,
        (void*)gm->least_addr);
  }
  if (atomic_load(&arc_out_of_memory))
    EM_ASM({ abort("out of memory: malloc ran out, then its check at dlmalloc.c:" + $0 +
                   " failed (a pointer made from a failed allocation?)"); }, line);
  abort();
}
