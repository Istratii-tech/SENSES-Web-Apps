// Force-included into the sources of ART 10 (after art10_wasm_compat.h); as shim/art/art_runtime_wasm.h for ART 6.0.1.
//
// ART is written for 4 KB pages (kPageSize); WebAssembly pages, and the
// kernel layer's mmap (shim/wasm_mm), are 64 KB. Mappings work as they are
// (lengths are rounded up and mappings start 64 KB-aligned), but ART also
// madvises and mprotects 4 KB-aligned ranges inside its mappings, which a
// 64 KB-page mmap refuses. Protection cannot be enforced in wasm anyway, and
// MADV_DONTNEED on ART's private anonymous memory means "reads back as
// zeros": both are done here instead. Part of the arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_ART10_ART_RUNTIME_WASM10_H_
#define ARC_WASM_SHIM_ART10_ART_RUNTIME_WASM10_H_

#include <string.h>
#include <sys/mman.h>

static inline int art_wasm_madvise(void* addr, size_t length, int advice) {
  if (advice == MADV_DONTNEED)
    memset(addr, 0, length);
  return 0;
}

static inline int art_wasm_mprotect(void* addr, size_t length, int prot) {
  (void)addr;
  (void)length;
  (void)prot;
  return 0;
}

#define madvise art_wasm_madvise
#define mprotect art_wasm_mprotect

#endif  // ARC_WASM_SHIM_ART10_ART_RUNTIME_WASM10_H_
