// Replacements for the NaCl IRT entry points used by posix_translation.
//
//  * nacl_interface_query(NACL_IRT_RANDOM)  -> getentropy() (which Emscripten
//    backs with crypto.getRandomValues()).
//  * __nacl_futex                           -> Emscripten's futex (Atomics).
//  * real_{read,write,close,fstat,lseek64}  -> the host libc. ARC called the
//    un-hooked NaCl IRT here; in the wasm port libc itself is the "host".

#include <errno.h>
#include <limits.h>
#include <linux/futex.h>
#include <math.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#if defined(__EMSCRIPTEN__)
#include <emscripten/threading.h>
#endif

#include "native_client/src/untrusted/irt/irt.h"
#include "posix_translation/real_syscall.h"

namespace {

int GetRandomBytes(void* buf, size_t count, size_t* nread) {
  unsigned char* p = static_cast<unsigned char*>(buf);
  size_t done = 0;
  while (done < count) {
    // getentropy() is limited to 256 bytes per call.
    const size_t chunk = count - done > 256 ? 256 : count - done;
    if (getentropy(p + done, chunk) != 0)
      return errno;
    done += chunk;
  }
  *nread = done;
  return 0;
}

}  // namespace

extern "C" {

size_t nacl_interface_query(const char* interface_ident, void* table,
                            size_t tablesize) {
  if (strcmp(interface_ident, NACL_IRT_RANDOM_v0_1) == 0 &&
      tablesize >= sizeof(nacl_irt_random)) {
    static const nacl_irt_random kRandom = { GetRandomBytes };
    memcpy(table, &kRandom, sizeof(kRandom));
    return sizeof(kRandom);
  }
  return 0;
}

// Returns >=0 on success, -errno on error (NaCl convention).
int __nacl_futex(volatile void* addr, int op, int val,
                 const timespec* timeout) {
#if defined(__EMSCRIPTEN__)
  op &= ~FUTEX_PRIVATE_FLAG;
  if (op == FUTEX_WAIT) {
    double ms = INFINITY;
    if (timeout)
      ms = timeout->tv_sec * 1000.0 + timeout->tv_nsec / 1e6;
    return emscripten_futex_wait(addr, static_cast<uint32_t>(val), ms);
  }
  if (op == FUTEX_WAKE)
    return emscripten_futex_wake(addr, val);
  return -ENOSYS;
#else
  return -ENOSYS;
#endif
}

int real_close(int fd) { return close(fd); }
int real_fstat(int fd, struct stat* buf) { return fstat(fd, buf); }
off64_t real_lseek64(int fd, off64_t offset, int whence) {
  return lseek(fd, offset, whence);
}
ssize_t real_read(int fd, void* buf, size_t count) {
  return read(fd, buf, count);
}
ssize_t real_write(int fd, const void* buf, size_t count) {
  return write(fd, buf, count);
}

}  // extern "C"
