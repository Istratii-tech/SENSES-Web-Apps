// Diagnostics exported by the arc-wasm page-granular mmap (wasm_mmap.cc).
#ifndef ARC_WASM_SHIM_WASM_MM_WASM_MMAP_H_
#define ARC_WASM_SHIM_WASM_MM_WASM_MMAP_H_

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

size_t wasm_mm_mapped_bytes();
size_t wasm_mm_block_bytes();
size_t wasm_mm_large_free_bytes();
size_t wasm_mm_large_hole_bytes();
// Pages of large ranges known untouched: fresh ones and holes (an iPhone does not count them).
size_t wasm_mm_untouched_bytes();

// The ranges this allocator holds, for a scan of the wasm memory by owner
// (topic wasm-heap-trim, shim/membudget/mem_budget.cc): |fn| gets each one,
// under the allocator's lock (it must not allocate or map). Mapped runs are
// split where protection or flags change (|prot|, |flags|: those of the run;
// -1 for the other kinds).
enum {
  WASM_MM_SMALL = 0,     // a Block from malloc (mappings under 1 MB), whole
  WASM_MM_LARGE_MAPPED,  // mapped pages of a large Block
  WASM_MM_LARGE_HOLE,    // unmapped pages inside a live large Block
  WASM_MM_LARGE_FREE,    // freed large ranges kept for the next large mapping
};
void wasm_mm_each_range(void (*fn)(uintptr_t start, size_t bytes, int kind, int prot, int flags,
                                   void* arg),
                        void* arg);

// Zeros written over memory, by who writes them (topic wasm-heap-trim). An
// iPhone counts a page of the wasm memory in the tab's footprint from its
// first touch on, so zeros written over a page nobody touched yet cost a page.
// wasm_mm_zero is memset(p, 0, n); with ARC_MEM_ZERO_STATS=1 it also counts,
// before writing, the 16 KB pages inside [p, p + n) that hold only zeros
// already (untouched, or touched zeros): an upper bound of what the site
// costs needlessly. wasm_mm_zero_stats: bytes, those bytes, and of them the
// distinct pages this site was the first to write, by site.
enum {
  WASM_MM_ZERO_SMALL_MAP,    // a mapping from malloc (under 1 MB)
  WASM_MM_ZERO_LARGE_REUSE,  // a large mapping in a range used before
  WASM_MM_ZERO_FIXED,        // MAP_FIXED over pages of a mapping
  WASM_MM_ZERO_MADVISE,      // MADV_DONTNEED
  WASM_MM_ZERO_CALLOC,       // malloc's calloc (shim/memory/dlmalloc_report.c)
  WASM_MM_ZERO_GUEST,        // the NDK bridge: a guest's mapping or madvise in 4 KB pages
  WASM_MM_ZERO_PENDING,      // pages zeroed late, when made accessible again
  WASM_MM_ZERO_SITES
};
void wasm_mm_zero(void* p, size_t n, int site);
void wasm_mm_zero_stats(uint64_t* bytes, uint64_t* zero_bytes, uint64_t* unique_bytes);

// The NDK bridge's side of the marks of large ranges (wasm_mmap.cc: fresh and
// pending pages). Guest memory has no protection in wasm, so the bridge maps
// nothing for these; it says what they mean:
//   wasm_mm_guest_access  mprotect: |prot| other than PROT_NONE makes the
//                         pages accessible (pending ones are zeroed first);
//   wasm_mm_guest_zero    zeros over [addr, addr + len) for a guest's
//                         mapping in 4 KB pages (|prot|: its protection) or
//                         madvise (|prot| -1): not where they are already.
void wasm_mm_guest_access(void* addr, size_t len, int prot);
void wasm_mm_guest_zero(void* addr, size_t len, int prot);

// Page-granular memory mapping on the wasm heap (see wasm_mmap.cc). Same
// contracts as the POSIX functions; |fd| is a host (Emscripten) descriptor.
void* __wasm_mm_mmap(void* addr, size_t length, int prot, int flags, int fd,
                     off_t offset);
int __wasm_mm_munmap(void* addr, size_t length);
int __wasm_mm_mprotect(void* addr, size_t length, int prot);
int __wasm_mm_msync(void* addr, size_t length, int flags);
int __wasm_mm_madvise(void* addr, size_t length, int advice);
int __wasm_mm_posix_madvise(void* addr, size_t length, int advice);
void* __wasm_mm_mremap(void* old_address, size_t old_size, size_t new_size,
                       int flags, ...);

#ifdef __cplusplus
}
#endif

#endif  // ARC_WASM_SHIM_WASM_MM_WASM_MMAP_H_
