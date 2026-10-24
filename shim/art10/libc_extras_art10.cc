// libc functions the Android 10 stack expects from bionic that musl in
// Emscripten lacks, beyond shim/dalvik/libc_extras.cc, shim/art/libc_extras_art.cc
// and shim/art8/libc_extras_art8.cc (shared with ART 6.0.1 and 8.1). Part of
// the arc-wasm shim layer.

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <unistd.h>

// Moving data between descriptors through a pipe (libcore 10's Linux.splice,
// used by nothing on the boot path): the VFS has no pipe buffers to splice,
// so it is not supported (callers fall back to read/write).
extern "C" ssize_t splice(int, loff_t*, int, loff_t*, size_t, unsigned int) {
  errno = ENOSYS;
  return -1;
}

// Random bytes (expat 2.2's hash salt; HAVE_GETRANDOM in mk/topic-art10.mk):
// getentropy (crypto.getRandomValues) gives at most 256 bytes per call.
extern "C" ssize_t getrandom(void* buf, size_t len, unsigned int) {
  uint8_t* p = static_cast<uint8_t*>(buf);
  size_t done = 0;
  while (done < len) {
    const size_t n = len - done < 256 ? len - done : 256;
    if (getentropy(p + done, n) != 0) {
      return done > 0 ? static_cast<ssize_t>(done) : -1;
    }
    done += n;
  }
  return static_cast<ssize_t>(done);
}
