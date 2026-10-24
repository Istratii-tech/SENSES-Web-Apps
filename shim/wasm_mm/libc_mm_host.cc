// Binds libc's memory-mapping functions straight to the wasm page allocator.
// For builds without the posix_translation kernel layer (e.g. its unit
// tests). Applications link shim/kernel/arc_kernel.cc instead.
// musl defines these names as weak aliases, so these definitions win.

#include <stdarg.h>
#include <sys/mman.h>

#include "wasm_mm/wasm_mmap.h"

extern "C" {

void* mmap(void* addr, size_t length, int prot, int flags, int fd,
           off_t offset) {
  return __wasm_mm_mmap(addr, length, prot, flags, fd, offset);
}
int munmap(void* addr, size_t length) {
  return __wasm_mm_munmap(addr, length);
}
int mprotect(void* addr, size_t length, int prot) {
  return __wasm_mm_mprotect(addr, length, prot);
}
int msync(void* addr, size_t length, int flags) {
  return __wasm_mm_msync(addr, length, flags);
}
int madvise(void* addr, size_t length, int advice) {
  return __wasm_mm_madvise(addr, length, advice);
}
int posix_madvise(void* addr, size_t length, int advice) {
  return __wasm_mm_posix_madvise(addr, length, advice);
}
void* mremap(void* old_address, size_t old_size, size_t new_size, int flags,
             ...) {
  return __wasm_mm_mremap(old_address, old_size, new_size, flags);
}

}  // extern "C"
