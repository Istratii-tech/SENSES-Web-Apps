// Page-granular mmap/munmap/mprotect/msync/madvise/mremap for wasm.
//
// Emscripten's mmap is a thin layer over malloc: it cannot unmap part of a
// mapping, honours no address (MAP_FIXED), and keeps no per-page state. ARC
// (and Android after it) relies on all three -- e.g. ashmem partially unmaps a
// shared region, and posix_translation overwrites mappings with MAP_FIXED.
//
// This implementation keeps the same "memory comes from the heap" model but
// tracks every page:
//   * mmap() allocates a page-aligned Block and records each page in it.
//   * munmap() of any page range is allowed; a Block's memory is freed once
//     its last page is unmapped. Unmapped pages inside a live Block are
//     "holes" that MAP_FIXED may reuse.
//   * MAP_FIXED works over pages this allocator owns (mapped pages or holes).
//     Arbitrary addresses cannot be honoured: wasm has a single linear memory
//     whose other pages belong to malloc, so such requests fail with ENOMEM.
//   * mprotect() validates and records protection; wasm cannot enforce it.
//   * MAP_SHARED file mappings are written back on msync() and munmap().
//   * Large anonymous mappings (heap reservations of a Java VM, hundreds of
//     MB) come from the top of the heap (sbrk) instead: that memory is
//     fresh, already zero, so it is not touched (and not committed by the
//     browser) until used. Unmapped, it is kept for later large mappings:
//     neighbours merge, a mapping takes the smallest free range it fits,
//     and a free range at the break grows instead of a new one. So do
//     mappings from 1 MB (topic userspace): a game loading its textures
//     (Angry Birds 2011) maps and unmaps hundreds of 4-15 MB buffers of
//     slowly growing sizes; through malloc each found the earlier holes a
//     little too small, the heap grew twice what was in use (600 MB), and
//     wasm memory never shrinks -- an iPhone X closed the page.
//
// These are exported as __wasm_mm_* -- the "physical memory" layer. What
// libc's mmap() means is decided by whoever binds the public names:
// libc_mm_host.cc (no kernel: straight to this allocator) or
// shim/kernel/arc_kernel.cc (app mmap goes through posix_translation's VFS,
// which uses this allocator underneath). File descriptors passed here are
// always host (Emscripten) descriptors.

#include <errno.h>
#include <limits.h>
#include <malloc.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <emscripten/console.h>
#include <emscripten/emscripten.h>

#include <atomic>
#include <map>
#include <memory>

#include "wasm_mm/wasm_mmap.h"

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

namespace {

const size_t kPageSize = PAGE_SIZE;

struct Block {
  char* base;
  size_t num_pages;
  size_t live_pages;
  bool large;  // from TakeLargeLocked(): back to FreeLarge(), not free()
};

// Mappings at least this large come from sbrk (see the top of the file).
const size_t kLargeMapping = 1024 * 1024;

// memalign with zeros written only where the memory may have been touched
// (shim/memory/dlmalloc_report.c: the memory malloc never handed out is
// zeros nobody touched, and writing zeros would make an iPhone count it).
extern "C" void* arc_memalign_zeroed(size_t alignment, size_t bytes) __attribute__((weak));

// wasm_mm_zero's statistics (ARC_MEM_ZERO_STATS=1).
std::atomic<uint64_t> g_zero_bytes[WASM_MM_ZERO_SITES], g_zero_zero_bytes[WASM_MM_ZERO_SITES];
// Pages of 16 KB (4 GB of them) some site found all zero and wrote: how many
// distinct ones each site was the first to write (at most what it cost).
std::atomic<uint32_t> g_zero_seen[(1u << 18) / 32];
std::atomic<uint64_t> g_zero_unique[WASM_MM_ZERO_SITES];
// A switch from the environment, kept once the program's environment is
// there (the first mappings may come before Emscripten fills it in; until
// then |unset| is the value).
int EnvSwitch(const char* name, int unset, std::atomic<int>* cached) {
  int v = cached->load(std::memory_order_relaxed);
  if (v != INT_MIN)
    return v;
  if (!getenv("HOME"))
    return unset;
  const char* s = getenv(name);
  v = s ? atoi(s) : unset;
  cached->store(v, std::memory_order_relaxed);
  return v;
}

int ZeroStatsOn() {
  static std::atomic<int> cached{INT_MIN};
  return EnvSwitch("ARC_MEM_ZERO_STATS", 0, &cached);
}

// Shared by all pages of one MAP_SHARED file mapping.
struct FileBacking {
  explicit FileBacking(int fd) : fd(fd) {}
  ~FileBacking() {
    if (fd >= 0)
      close(fd);
  }
  int fd;
};

struct Page {
  Block* block;
  int prot;
  int flags;
  std::shared_ptr<FileBacking> backing;  // Only for MAP_SHARED file pages.
  off_t file_offset;
};

pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

// The break's lock: every move of the break here holds it, and so does
// malloc's (shim/memory/dlmalloc_report.c makes it dlmalloc's "global"
// lock, the one around its sbrk calls). dlmalloc assumes nobody else moves
// the break while it holds that lock: if the break moved between two of
// its sbrk calls, it would take the second call's memory as following the
// first and own memory a large mapping here owns too. With Emscripten's
// own dlmalloc, whose lock is its own, that race remains.
pthread_mutex_t g_sbrk_mutex = PTHREAD_MUTEX_INITIALIZER;
class SbrkLock {
 public:
  SbrkLock() { pthread_mutex_lock(&g_sbrk_mutex); }
  ~SbrkLock() { pthread_mutex_unlock(&g_sbrk_mutex); }
};

// Leaked on purpose: mappings may outlive static destructors.
std::map<uintptr_t, Page>& Pages() {
  static std::map<uintptr_t, Page>* pages = new std::map<uintptr_t, Page>;
  return *pages;
}

std::map<uintptr_t, Block*>& Blocks() {
  static std::map<uintptr_t, Block*>* blocks = new std::map<uintptr_t, Block*>;
  return *blocks;
}

// Unmapped large regions, by address (adjacent ones are merged).
std::map<uintptr_t, size_t>& FreeLarge() {
  static std::map<uintptr_t, size_t>* free_large = new std::map<uintptr_t, size_t>;
  return *free_large;
}

// --- Untouched and pending pages of large ranges (topic wasm-heap-trim) ---------------
// An iPhone counts every page of the wasm memory touched (read or written)
// in the tab's footprint, for good. Two marks per page of large ranges (they
// never go back to malloc, and malloc's memory never has a mark):
//   fresh    -- zeros nobody touched: from the break and never accessible
//               since (mapped PROT_NONE at most). Zeroing it is skipped.
//   pending  -- not accessible (PROT_NONE) and must read as zeros once it is
//               again; it may hold old data. Zeroed only when a mapping or
//               mprotect makes it accessible (an allocator that decommits by
//               mapping PROT_NONE over its pages -- il2cpp's GC unmapping
//               free blocks -- need not touch them now, perhaps never).
// Who makes guest memory accessible without this allocator (the NDK bridge's
// mprotect: wasm has no protection) says so: wasm_mm_guest_access.
// ARC_MM_LAZY=0: both off (every zero written at once, as before).
const size_t kMarkPages = (1ull << 32) / kPageSize;
uint32_t g_fresh[kMarkPages / 32], g_pending[kMarkPages / 32];

bool LazyOn() {
  static std::atomic<int> cached{INT_MIN};
  return EnvSwitch("ARC_MM_LAZY", 1, &cached) != 0;
}
size_t PageIndex(uintptr_t addr) { return addr / kPageSize; }
bool Marked(const uint32_t* bits, uintptr_t page_addr) {
  const size_t i = PageIndex(page_addr);
  return (bits[i / 32] >> (i % 32)) & 1;
}
void Mark(uint32_t* bits, uintptr_t page_addr, bool on) {
  const size_t i = PageIndex(page_addr);
  if (on)
    bits[i / 32] |= 1u << (i % 32);
  else
    bits[i / 32] &= ~(1u << (i % 32));
}
// The pages marked fresh, counted (wasm_mm_fresh_bytes: what of the large
// ranges an iPhone does not count, for the memory monitor's "touched").
std::atomic<size_t> g_fresh_pages{0};
void MarkFresh(uintptr_t page_addr, bool on) {
  if (Marked(g_fresh, page_addr) == on)
    return;
  Mark(g_fresh, page_addr, on);
  if (on)
    g_fresh_pages.fetch_add(1, std::memory_order_relaxed);
  else
    g_fresh_pages.fetch_sub(1, std::memory_order_relaxed);
}
// [start, start + bytes): whole pages fresh from the break.
void MarkFreshLocked(uintptr_t start, size_t bytes) {
  if (!LazyOn())
    return;
  for (uintptr_t a = start; a + kPageSize <= start + bytes; a += kPageSize) {
    MarkFresh(a, true);
    Mark(g_pending, a, false);
  }
}
// ARC_MEM_ZERO_STATS=5: a page skipped as fresh is checked to be zeros (a test).
void CheckFresh(uintptr_t a);

// Zeros the pages of [start, start + bytes) for a new mapping of |prot| in a
// large range: fresh ones are zeros already; with PROT_NONE the rest become
// pending instead (zeroed when made accessible). Accessible pages lose both marks.
void ZeroLargeLocked(uintptr_t start, size_t bytes, int prot, int site) {
  uintptr_t run = 0;  // start of a run of pages to zero now
  for (uintptr_t a = start; a < start + bytes; a += kPageSize) {
    const bool fresh = Marked(g_fresh, a);
    bool zero = !fresh;
    if (fresh)
      CheckFresh(a);
    if (prot == PROT_NONE && LazyOn()) {
      if (!fresh)
        Mark(g_pending, a, true);
      zero = false;
    } else {
      MarkFresh(a, false);
      Mark(g_pending, a, false);
    }
    if (zero && !run)
      run = a;
    if (!zero && run) {
      wasm_mm_zero(reinterpret_cast<void*>(run), a - run, site);
      run = 0;
    }
  }
  if (run)
    wasm_mm_zero(reinterpret_cast<void*>(run), start + bytes - run, site);
}

// Pages of [start, start + bytes) become accessible keeping their contents
// (mprotect): pending ones are zeroed now; none is fresh any more.
void AccessLocked(uintptr_t start, size_t bytes) {
  for (uintptr_t a = start & ~(kPageSize - 1); a < start + bytes; a += kPageSize) {
    if (Marked(g_pending, a)) {
      wasm_mm_zero(reinterpret_cast<void*>(a), kPageSize, WASM_MM_ZERO_PENDING);
      Mark(g_pending, a, false);
    }
    MarkFresh(a, false);
  }
}

void CheckFresh(uintptr_t a) {
  if (ZeroStatsOn() < 5)
    return;
  const uint64_t* w = reinterpret_cast<const uint64_t*>(a);
  for (size_t i = 0; i < kPageSize / 8; ++i) {
    if (w[i]) {
      char say[120];
      snprintf(say, sizeof(say), "wasm_mm: a fresh page is not zero: %p (word %u)",
               reinterpret_cast<void*>(a), static_cast<unsigned>(i));
      emscripten_err(say);
      memset(reinterpret_cast<void*>(a), 0, kPageSize);
      return;
    }
  }
}

void ReleaseLargeLocked(char* base, size_t bytes) {
  std::map<uintptr_t, size_t>& free_large = FreeLarge();
  uintptr_t start = reinterpret_cast<uintptr_t>(base);
  std::map<uintptr_t, size_t>::iterator next = free_large.lower_bound(start);
  if (next != free_large.end() && start + bytes == next->first) {
    bytes += next->second;
    next = free_large.erase(next);
  }
  if (next != free_large.begin()) {
    std::map<uintptr_t, size_t>::iterator prev = next;
    --prev;
    if (prev->first + prev->second == start) {
      prev->second += bytes;
      return;
    }
  }
  free_large[start] = bytes;
}

// A page-aligned region of |bytes| for a large mapping; |*zeroed| tells
// whether it is known to be zero (fresh from sbrk).
char* TakeLargeLocked(size_t bytes, bool* zeroed) {
  std::map<uintptr_t, size_t>& free_large = FreeLarge();
  // The smallest free range that fits (the big ones stay for big mappings).
  std::map<uintptr_t, size_t>::iterator best = free_large.end();
  for (std::map<uintptr_t, size_t>::iterator it = free_large.begin(); it != free_large.end(); ++it) {
    if (it->second >= bytes && (best == free_large.end() || it->second < best->second))
      best = it;
  }
  if (best != free_large.end()) {
    const uintptr_t start = best->first;
    const size_t size = best->second;
    free_large.erase(best);
    if (size > bytes)
      free_large[start + bytes] = size - bytes;
    *zeroed = false;
    return reinterpret_cast<char*>(start);
  }
  // The last free range ends at the break: grow it by what is missing.
  if (!free_large.empty()) {
    std::map<uintptr_t, size_t>::iterator last = --free_large.end();
    const uintptr_t start = last->first;
    const size_t size = last->second;
    void* p = reinterpret_cast<void*>(-1);
    bool at_break;
    {
      SbrkLock lock;
      at_break = start + size == reinterpret_cast<uintptr_t>(sbrk(0)) && size % kPageSize == 0;
      if (at_break)
        p = sbrk(bytes - size);
    }
    if (at_break) {
      if (p == reinterpret_cast<void*>(start + size)) {
        free_large.erase(last);
        *zeroed = false;  // the old part was used
        MarkFreshLocked(start + size, bytes - size);
        return reinterpret_cast<char*>(start);
      }
      // Another thread moved the break in between: what sbrk gave is a
      // fresh range of its own, kept for later.
      if (p != reinterpret_cast<void*>(-1)) {
        const uintptr_t from = (reinterpret_cast<uintptr_t>(p) + kPageSize - 1) & ~(kPageSize - 1);
        const uintptr_t to = (reinterpret_cast<uintptr_t>(p) + bytes - size) & ~(kPageSize - 1);
        if (to > from) {
          MarkFreshLocked(from, to - from);
          ReleaseLargeLocked(reinterpret_cast<char*>(from), to - from);
        }
      }
    }
  }
  // Other threads may move the break too (malloc): ask for a page more and
  // align within. The memory above the old break was never used.
  void* p;
  {
    SbrkLock lock;
    p = sbrk(bytes + kPageSize);
  }
  if (p == reinterpret_cast<void*>(-1))
    return NULL;
  const uintptr_t start = (reinterpret_cast<uintptr_t>(p) + kPageSize - 1) & ~(kPageSize - 1);
  *zeroed = true;
  MarkFreshLocked(start, bytes);
  // The break was aligned already: the page asked for more is free, kept at the break
  // (a mapping continuing this one in place takes it, TakeLargeAtLocked).
  if (start == reinterpret_cast<uintptr_t>(p)) {
    MarkFreshLocked(start + bytes, kPageSize);
    ReleaseLargeLocked(reinterpret_cast<char*>(start + bytes), kPageSize);
  }
  return reinterpret_cast<char*>(start);
}

// A large range at exactly [addr, addr + bytes) (whole pages) if nobody has it: inside a
// free range, or at the break (which moves up for it, a free range ending there
// included). NULL otherwise. For MAP_FIXED_NOREPLACE: a mapping that continues another
// in place (the NDK bridge's reservation windows grow a run of memory so).
char* TakeLargeAtLocked(uintptr_t start, size_t bytes, bool* zeroed) {
  std::map<uintptr_t, size_t>& free_large = FreeLarge();
  const uintptr_t end = start + bytes;
  std::map<uintptr_t, size_t>::iterator it = free_large.upper_bound(start);
  bool in_free = false;
  uintptr_t s = 0, n = 0;
  if (it != free_large.begin()) {
    --it;
    s = it->first;
    n = it->second;
    in_free = start < s + n;
  }
  SbrkLock lock;
  const uintptr_t brk = reinterpret_cast<uintptr_t>(sbrk(0));
  if (in_free && end <= s + n) {
    free_large.erase(it);
    if (s < start)
      free_large[s] = start - s;
    if (end < s + n)
      free_large[end] = s + n - end;
    *zeroed = false;  // (its fresh pages keep their marks)
    return reinterpret_cast<char*>(start);
  }
  const uintptr_t from = in_free ? s + n : start;  // where new memory from the break starts
  if (from != brk || brk % kPageSize != 0)
    return NULL;
  void* p = sbrk(end - from);
  if (p != reinterpret_cast<void*>(from)) {
    // (the break's lock is held: nobody moved it; out of memory)
    return NULL;
  }
  if (in_free) {
    free_large.erase(it);
    if (s < start)
      free_large[s] = start - s;
  }
  MarkFreshLocked(from, end - from);
  *zeroed = !in_free;
  return reinterpret_cast<char*>(start);
}

void FreeBlockLocked(Block* block) {
  Blocks().erase(reinterpret_cast<uintptr_t>(block->base));
  if (block->large)
    ReleaseLargeLocked(block->base, block->num_pages * kPageSize);
  else
    free(block->base);
  delete block;
}

class ScopedLock {
 public:
  ScopedLock() { pthread_mutex_lock(&g_mutex); }
  ~ScopedLock() { pthread_mutex_unlock(&g_mutex); }
};

size_t PagesFor(size_t length) {
  return (length + kPageSize - 1) / kPageSize;
}

bool IsAligned(const void* addr) {
  return (reinterpret_cast<uintptr_t>(addr) & (kPageSize - 1)) == 0;
}

// Returns the Block whose address range contains |page|, or NULL.
Block* FindOwningBlock(uintptr_t page) {
  std::map<uintptr_t, Block*>& blocks = Blocks();
  std::map<uintptr_t, Block*>::iterator it = blocks.upper_bound(page);
  if (it == blocks.begin())
    return NULL;
  --it;
  Block* block = it->second;
  const uintptr_t end =
      reinterpret_cast<uintptr_t>(block->base) + block->num_pages * kPageSize;
  return page < end ? block : NULL;
}

ssize_t PreadFully(int fd, char* buf, size_t count, off_t offset) {
  size_t done = 0;
  while (done < count) {
    const ssize_t n = pread(fd, buf + done, count - done, offset + done);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    if (n == 0)
      break;  // EOF: the rest of the mapping stays zero-filled.
    done += n;
  }
  return done;
}

// Writes a dirty MAP_SHARED page back to its file, never past EOF (writes
// through a mapping do not extend files on Linux either).
void WriteBack(uintptr_t page_addr, const Page& page) {
  if (!page.backing || !(page.prot & PROT_WRITE))
    return;
  struct stat st;
  if (fstat(page.backing->fd, &st) != 0 || page.file_offset >= st.st_size)
    return;
  size_t count = kPageSize;
  if (page.file_offset + static_cast<off_t>(count) > st.st_size)
    count = st.st_size - page.file_offset;
  const char* src = reinterpret_cast<const char*>(page_addr);
  size_t done = 0;
  while (done < count) {
    const ssize_t n = pwrite(page.backing->fd, src + done, count - done,
                             page.file_offset + done);
    if (n <= 0 && errno != EINTR)
      return;
    if (n > 0)
      done += n;
  }
}

void UnmapPageLocked(std::map<uintptr_t, Page>::iterator it) {
  WriteBack(it->first, it->second);
  Block* block = it->second.block;
  Pages().erase(it);
  if (--block->live_pages == 0)
    FreeBlockLocked(block);
}

void UnmapRangeLocked(uintptr_t start, size_t num_pages) {
  std::map<uintptr_t, Page>& pages = Pages();
  for (size_t i = 0; i < num_pages; ++i) {
    std::map<uintptr_t, Page>::iterator it = pages.find(start + i * kPageSize);
    if (it != pages.end())
      UnmapPageLocked(it);
  }
}

bool RangeIsMappedLocked(uintptr_t start, size_t num_pages) {
  std::map<uintptr_t, Page>& pages = Pages();
  for (size_t i = 0; i < num_pages; ++i) {
    if (pages.find(start + i * kPageSize) == pages.end())
      return false;
  }
  return true;
}

// Fills freshly (re)mapped memory: zeros (unless |zeroed|; in a |large|
// range only where its pages need them, ZeroLargeLocked), then file contents
// if any. |site|: who asks (wasm_mm_zero's statistics).
bool FillLocked(char* addr, size_t length, int fd, off_t offset, int flags, int prot,
                bool large, bool zeroed, int site) {
  const uintptr_t start = reinterpret_cast<uintptr_t>(addr);
  if (large && LazyOn())
    ZeroLargeLocked(start, PagesFor(length) * kPageSize, (flags & MAP_ANONYMOUS) ? prot : PROT_READ, site);
  else if (!zeroed)
    wasm_mm_zero(addr, PagesFor(length) * kPageSize, site);
  if (flags & MAP_ANONYMOUS)
    return true;
  return PreadFully(fd, addr, length, offset) >= 0;
}

void RecordPagesLocked(Block* block, uintptr_t start, size_t num_pages,
                       int prot, int flags, int fd, off_t offset) {
  std::shared_ptr<FileBacking> backing;
  if (!(flags & MAP_ANONYMOUS) && (flags & MAP_SHARED)) {
    const int dup_fd = dup(fd);
    if (dup_fd >= 0)
      backing = std::make_shared<FileBacking>(dup_fd);
  }
  for (size_t i = 0; i < num_pages; ++i) {
    Page page;
    page.block = block;
    page.prot = prot;
    page.flags = flags;
    page.backing = backing;
    page.file_offset = offset + static_cast<off_t>(i * kPageSize);
    Pages()[start + i * kPageSize] = page;
    ++block->live_pages;
  }
}

void* MapFixedLocked(void* addr, size_t length, int prot, int flags, int fd,
                     off_t offset) {
  if (ZeroStatsOn() >= 6 && length >= kLargeMapping) {
    char say[120];
    snprintf(say, sizeof(say), "wasm_mm: MAP_FIXED %p %u KB prot %d flags %x", addr,
             static_cast<unsigned>(length >> 10), prot, flags);
    emscripten_err(say);
  }
  const uintptr_t start = reinterpret_cast<uintptr_t>(addr);
  const size_t num_pages = PagesFor(length);
  // Every target page must belong to one of our Blocks, and all to the same
  // one (a Block is a single heap allocation).
  Block* block = FindOwningBlock(start);
  if (!block || FindOwningBlock(start + (num_pages - 1) * kPageSize) != block) {
    errno = ENOMEM;
    return MAP_FAILED;
  }
  // Keep the Block alive while its old pages are dropped.
  ++block->live_pages;
  UnmapRangeLocked(start, num_pages);
  if (!FillLocked(static_cast<char*>(addr), length, fd, offset, flags, prot, block->large, false,
                  WASM_MM_ZERO_FIXED)) {
    const int saved = errno;
    if (--block->live_pages == 0)
      FreeBlockLocked(block);
    errno = saved;
    return MAP_FAILED;
  }
  RecordPagesLocked(block, start, num_pages, prot, flags, fd, offset);
  --block->live_pages;
  return addr;
}

}  // namespace

extern "C" {

// For malloc (shim/memory/dlmalloc_report.c): the break's lock above.
void arc_sbrk_lock(void) { pthread_mutex_lock(&g_sbrk_mutex); }
void arc_sbrk_unlock(void) { pthread_mutex_unlock(&g_sbrk_mutex); }

void* __wasm_mm_mmap(void* addr, size_t length, int prot, int flags, int fd,
           off_t offset) {
  if (length == 0 || (offset & (kPageSize - 1)) ||
      !(flags & (MAP_SHARED | MAP_PRIVATE))) {
    errno = EINVAL;
    return MAP_FAILED;
  }
  if (!(flags & MAP_ANONYMOUS) && fd < 0) {
    errno = EBADF;
    return MAP_FAILED;
  }
  ScopedLock lock;
  if (flags & MAP_FIXED) {
    if (!IsAligned(addr)) {
      errno = EINVAL;
      return MAP_FAILED;
    }
    return MapFixedLocked(addr, length, prot, flags, fd, offset);
  }
  // Address hints are ignored, as the kernel is allowed to do (not MAP_FIXED_NOREPLACE).
  const size_t num_pages = PagesFor(length);
  if (num_pages > SIZE_MAX / kPageSize) {
    errno = ENOMEM;
    return MAP_FAILED;
  }
  // MAP_FIXED_NOREPLACE (anonymous): exactly there if nobody has it (a large range), else EEXIST.
  const bool noreplace = (flags & MAP_FIXED_NOREPLACE) && addr != nullptr;
  const bool large = (flags & MAP_ANONYMOUS) && (noreplace || num_pages * kPageSize >= kLargeMapping);
  bool zeroed = false;
  char* base;
  if (noreplace) {
    if (!IsAligned(addr) || !(flags & MAP_ANONYMOUS)) {
      errno = EINVAL;
      return MAP_FAILED;
    }
    base = TakeLargeAtLocked(reinterpret_cast<uintptr_t>(addr), num_pages * kPageSize, &zeroed);
    if (!base) {
      errno = EEXIST;
      return MAP_FAILED;
    }
    flags &= ~MAP_FIXED_NOREPLACE;
  } else if (large) {
    base = TakeLargeLocked(num_pages * kPageSize, &zeroed);
  } else if (arc_memalign_zeroed) {
    // Zeros only where malloc's memory may have been touched (topic wasm-heap-trim).
    base = static_cast<char*>(arc_memalign_zeroed(kPageSize, num_pages * kPageSize));
    zeroed = true;
  } else {
    base = static_cast<char*>(memalign(kPageSize, num_pages * kPageSize));
  }
  if (!base) {
    errno = ENOMEM;
    return MAP_FAILED;
  }
  if (!FillLocked(base, length, fd, offset, flags, prot, large, zeroed,
                  large ? WASM_MM_ZERO_LARGE_REUSE : WASM_MM_ZERO_SMALL_MAP)) {
    const int saved = errno;
    if (large)
      ReleaseLargeLocked(base, num_pages * kPageSize);
    else
      free(base);
    errno = saved;
    return MAP_FAILED;
  }
  if (!large && (flags & MAP_ANONYMOUS) && ZeroStatsOn() >= 4) {
    // Each small anonymous mapping and where it comes from (diagnostics).
    char say[1400];
    const int k = snprintf(say, 96, "wasm_mm: small %u KB prot %d at ",
                           static_cast<unsigned>(num_pages * kPageSize >> 10), prot);
    emscripten_get_callstack(EM_LOG_C_STACK | EM_LOG_NO_PATHS, say + k, sizeof(say) - k);
    for (char* c = say; *c; c++)
      if (*c == '\n') *c = '|';
    emscripten_err(say);
  }
  if (large && (ZeroStatsOn() >= 2 && ZeroStatsOn() != 5)) {
    // Each large mapping (diagnostics): fresh from the break, or a range used before.
    char say[1400];
    const int k = snprintf(say, 120, "wasm_mm: large %s %u KB prot %d at %p", zeroed ? "fresh" : "reuse",
                           static_cast<unsigned>(num_pages * kPageSize >> 10), prot, base);
    if (ZeroStatsOn() >= 4)
      emscripten_get_callstack(EM_LOG_C_STACK | EM_LOG_NO_PATHS, say + k, sizeof(say) - k);
    for (char* c = say; *c; c++)
      if (*c == '\n') *c = '|';
    emscripten_err(say);
  }
  Block* block = new Block;
  block->base = base;
  block->num_pages = num_pages;
  block->live_pages = 0;
  block->large = large;
  Blocks()[reinterpret_cast<uintptr_t>(base)] = block;
  RecordPagesLocked(block, reinterpret_cast<uintptr_t>(base), num_pages, prot,
                    flags, fd, offset);
  return base;
}

int __wasm_mm_munmap(void* addr, size_t length) {
  if (length == 0 || !IsAligned(addr)) {
    errno = EINVAL;
    return -1;
  }
  if (ZeroStatsOn() >= 6 && length >= kLargeMapping) {
    char say[120];
    snprintf(say, sizeof(say), "wasm_mm: munmap %p %u KB", addr, static_cast<unsigned>(length >> 10));
    emscripten_err(say);
  }
  ScopedLock lock;
  // Unmapping pages that are not mapped is not an error on Linux.
  UnmapRangeLocked(reinterpret_cast<uintptr_t>(addr), PagesFor(length));
  return 0;
}

int __wasm_mm_mprotect(void* addr, size_t length, int prot) {
  if (!IsAligned(addr)) {
    errno = EINVAL;
    return -1;
  }
  ScopedLock lock;
  const uintptr_t start = reinterpret_cast<uintptr_t>(addr);
  const size_t num_pages = PagesFor(length);
  if (!RangeIsMappedLocked(start, num_pages)) {
    errno = ENOMEM;
    return -1;
  }
  for (size_t i = 0; i < num_pages; ++i) {
    Page& page = Pages()[start + i * kPageSize];
    page.prot = prot;
    if (prot != PROT_NONE && page.block->large)
      AccessLocked(start + i * kPageSize, kPageSize);
  }
  return 0;
}

int __wasm_mm_msync(void* addr, size_t length, int flags) {
  if (!IsAligned(addr)) {
    errno = EINVAL;
    return -1;
  }
  ScopedLock lock;
  const uintptr_t start = reinterpret_cast<uintptr_t>(addr);
  const size_t num_pages = PagesFor(length);
  if (!RangeIsMappedLocked(start, num_pages)) {
    errno = ENOMEM;
    return -1;
  }
  for (size_t i = 0; i < num_pages; ++i) {
    const uintptr_t page_addr = start + i * kPageSize;
    WriteBack(page_addr, Pages()[page_addr]);
  }
  return 0;
}

int __wasm_mm_madvise(void* addr, size_t length, int advice) {
  if (!IsAligned(addr)) {
    errno = EINVAL;
    return -1;
  }
  ScopedLock lock;
  const uintptr_t start = reinterpret_cast<uintptr_t>(addr);
  const size_t num_pages = PagesFor(length);
  if (!RangeIsMappedLocked(start, num_pages)) {
    errno = ENOMEM;
    return -1;
  }
  if (advice == MADV_DONTNEED) {
    // Private anonymous pages read back as zeros afterwards.
    for (size_t i = 0; i < num_pages; ++i) {
      const uintptr_t page_addr = start + i * kPageSize;
      const Page& page = Pages()[page_addr];
      if (!(page.flags & MAP_ANONYMOUS) || !(page.flags & MAP_PRIVATE))
        continue;
      if (page.block->large && Marked(g_fresh, page_addr))
        continue;  // zeros nobody touched
      if (page.block->large && page.prot == PROT_NONE && LazyOn())
        Mark(g_pending, page_addr, true);
      else
        wasm_mm_zero(reinterpret_cast<void*>(page_addr), kPageSize, WASM_MM_ZERO_MADVISE);
    }
  }
  return 0;
}

// Diagnostics: bytes in mapped pages, and bytes held by live Blocks (mapped
// pages plus holes that keep a Block alive).
size_t wasm_mm_mapped_bytes() {
  ScopedLock lock;
  return Pages().size() * kPageSize;
}

// Unmapped large regions kept for the next large mapping (memory the wasm
// heap has but nothing uses; topic mem-budget's accounting).
size_t wasm_mm_large_free_bytes() {
  ScopedLock lock;
  size_t total = 0;
  for (std::map<uintptr_t, size_t>::const_iterator it = FreeLarge().begin();
       it != FreeLarge().end(); ++it) {
    total += it->second;
  }
  return total;
}

// Unmapped pages inside live large mappings ("holes": e.g. a Java heap's
// reservation cut down to its growth limit). Never touched or given back
// by their owner; only MAP_FIXED of the same owner uses them again.
size_t wasm_mm_large_hole_bytes() {
  ScopedLock lock;
  size_t total = 0;
  for (std::map<uintptr_t, Block*>::const_iterator it = Blocks().begin();
       it != Blocks().end(); ++it) {
    if (it->second->large)
      total += (it->second->num_pages - it->second->live_pages) * kPageSize;
  }
  return total;
}

// What of the large ranges is known untouched (the memory monitor's
// "touched", topic senses-shell-fixes): fresh pages, and the holes of live
// large mappings (not touched by their owner, see above) not fresh already.
size_t wasm_mm_untouched_bytes() {
  ScopedLock lock;
  size_t pages = g_fresh_pages.load(std::memory_order_relaxed);
  const std::map<uintptr_t, Page>& mapped = Pages();
  for (std::map<uintptr_t, Block*>::const_iterator it = Blocks().begin();
       it != Blocks().end(); ++it) {
    const Block* b = it->second;
    if (!b->large || b->live_pages == b->num_pages)
      continue;
    const uintptr_t base = reinterpret_cast<uintptr_t>(b->base);
    for (size_t i = 0; i < b->num_pages; ++i) {
      const uintptr_t a = base + i * kPageSize;
      if (!Marked(g_fresh, a) && mapped.find(a) == mapped.end())
        ++pages;
    }
  }
  return pages * kPageSize;
}

size_t wasm_mm_block_bytes() {
  ScopedLock lock;
  size_t total = 0;
  for (std::map<uintptr_t, Block*>::const_iterator it = Blocks().begin();
       it != Blocks().end(); ++it) {
    total += it->second->num_pages * kPageSize;
  }
  return total;
}

void wasm_mm_zero(void* p, size_t n, int site) {
  if (ZeroStatsOn() > 0 && site >= 0 && site < WASM_MM_ZERO_SITES) {
    const size_t kStatPage = 16384;  // an iPhone's page
    const uintptr_t start = reinterpret_cast<uintptr_t>(p);
    uint64_t zero = 0;
    for (uintptr_t a = (start + kStatPage - 1) & ~(kStatPage - 1); a + kStatPage <= start + n;
         a += kStatPage) {
      const uint64_t* w = reinterpret_cast<const uint64_t*>(a);
      uint64_t acc = 0;
      for (size_t i = 0; i < kStatPage / 8 && !acc; i += 8)
        acc = w[i] | w[i + 1] | w[i + 2] | w[i + 3] | w[i + 4] | w[i + 5] | w[i + 6] | w[i + 7];
      if (!acc) {
        zero += kStatPage;
        const uint32_t page = static_cast<uint32_t>(a >> 14);
        const uint32_t bit = 1u << (page & 31);
        if (!(g_zero_seen[page >> 5].fetch_or(bit) & bit))
          g_zero_unique[site] += kStatPage;
      }
    }
    g_zero_bytes[site] += n;
    g_zero_zero_bytes[site] += zero;
  }
  memset(p, 0, n);
}

void wasm_mm_guest_access(void* addr, size_t len, int prot) {
  if (ZeroStatsOn() >= 6 && len >= kLargeMapping) {
    char say[120];
    snprintf(say, sizeof(say), "wasm_mm: guest mprotect %p %u KB prot %d", addr,
             static_cast<unsigned>(len >> 10), prot);
    emscripten_err(say);
  }
  if (prot == PROT_NONE || len == 0)
    return;
  ScopedLock lock;
  AccessLocked(reinterpret_cast<uintptr_t>(addr), len);
}

void wasm_mm_guest_zero(void* addr, size_t len, int prot) {
  ScopedLock lock;
  const uintptr_t start = reinterpret_cast<uintptr_t>(addr), end = start + len;
  for (uintptr_t page = start & ~(kPageSize - 1); page < end; page += kPageSize) {
    const uintptr_t from = page > start ? page : start;
    const uintptr_t to = page + kPageSize < end ? page + kPageSize : end;
    const bool fresh = Marked(g_fresh, page), pending = Marked(g_pending, page);
    if (fresh)
      CheckFresh(page);
    if (prot > 0) {
      // Accessible now: a pending page is zeroed whole, a fresh one is zeros.
      if (pending)
        wasm_mm_zero(reinterpret_cast<void*>(page), kPageSize, WASM_MM_ZERO_PENDING);
      else if (!fresh)
        wasm_mm_zero(reinterpret_cast<void*>(from), to - from, WASM_MM_ZERO_GUEST);
      MarkFresh(page, false);
      Mark(g_pending, page, false);
    } else if (!fresh && !pending) {
      wasm_mm_zero(reinterpret_cast<void*>(from), to - from, WASM_MM_ZERO_GUEST);
    }
  }
}

void wasm_mm_zero_stats(uint64_t* bytes, uint64_t* zero_bytes, uint64_t* unique_bytes) {
  for (int i = 0; i < WASM_MM_ZERO_SITES; ++i) {
    bytes[i] = g_zero_bytes[i];
    zero_bytes[i] = g_zero_zero_bytes[i];
    unique_bytes[i] = g_zero_unique[i];
  }
}

void wasm_mm_each_range(void (*fn)(uintptr_t start, size_t bytes, int kind, int prot, int flags,
                                   void* arg),
                        void* arg) {
  ScopedLock lock;
  std::map<uintptr_t, Page>& pages = Pages();
  for (std::map<uintptr_t, Block*>::const_iterator it = Blocks().begin(); it != Blocks().end();
       ++it) {
    const Block* block = it->second;
    const uintptr_t base = reinterpret_cast<uintptr_t>(block->base);
    if (!block->large) {
      std::map<uintptr_t, Page>::const_iterator p = pages.find(base);
      fn(base, block->num_pages * kPageSize, WASM_MM_SMALL, p != pages.end() ? p->second.prot : -1,
         p != pages.end() ? p->second.flags : -1, arg);
      continue;
    }
    // Runs of mapped pages of one protection and flags, and of holes.
    size_t run = 0;
    int prot = -2, flags = -2;
    for (size_t i = 0; i <= block->num_pages; ++i) {
      int p = -1, f = -1;
      if (i < block->num_pages) {
        std::map<uintptr_t, Page>::const_iterator page = pages.find(base + i * kPageSize);
        if (page != pages.end()) {
          p = page->second.prot;
          f = page->second.flags;
        }
      }
      if (i > run && (i == block->num_pages || p != prot || f != flags)) {
        fn(base + run * kPageSize, (i - run) * kPageSize,
           prot >= 0 ? WASM_MM_LARGE_MAPPED : WASM_MM_LARGE_HOLE, prot, flags, arg);
        run = i;
      }
      prot = p;
      flags = f;
    }
  }
  for (std::map<uintptr_t, size_t>::const_iterator it = FreeLarge().begin();
       it != FreeLarge().end(); ++it) {
    fn(it->first, it->second, WASM_MM_LARGE_FREE, -1, -1, arg);
  }
}

int __wasm_mm_posix_madvise(void* addr, size_t length, int advice) {
  return 0;
}

void* __wasm_mm_mremap(void* old_address, size_t old_size, size_t new_size, int flags,
             ...) {
  if (!IsAligned(old_address) || new_size == 0 ||
      (flags & ~(MREMAP_MAYMOVE | MREMAP_FIXED)) || (flags & MREMAP_FIXED)) {
    errno = EINVAL;
    return MAP_FAILED;
  }
  const uintptr_t start = reinterpret_cast<uintptr_t>(old_address);
  const size_t old_pages = PagesFor(old_size);
  const size_t new_pages = PagesFor(new_size);
  int prot = 0;
  int map_flags = 0;
  {
    ScopedLock lock;
    if (!RangeIsMappedLocked(start, old_pages)) {
      errno = EFAULT;
      return MAP_FAILED;
    }
    if (new_pages <= old_pages) {
      UnmapRangeLocked(start + new_pages * kPageSize, old_pages - new_pages);
      return old_address;
    }
    const Page& first = Pages()[start];
    prot = first.prot;
    map_flags = first.flags;
  }
  // Growing in place is not possible: the following heap pages belong to
  // malloc. Move if allowed.
  if (!(flags & MREMAP_MAYMOVE) || !(map_flags & MAP_ANONYMOUS)) {
    errno = ENOMEM;
    return MAP_FAILED;
  }
  void* moved = __wasm_mm_mmap(NULL, new_size, prot,
                     (map_flags & (MAP_SHARED | MAP_PRIVATE)) | MAP_ANONYMOUS,
                     -1, 0);
  if (moved == MAP_FAILED)
    return MAP_FAILED;
  {
    ScopedLock lock;
    AccessLocked(start, old_pages * kPageSize);  // pending pages are zeros to copy
  }
  memcpy(moved, old_address, old_pages * kPageSize);
  __wasm_mm_munmap(old_address, old_size);
  return moved;
}

}  // extern "C"
