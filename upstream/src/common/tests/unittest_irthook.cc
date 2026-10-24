// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <dirent.h>
#include <errno.h>
#include <irt_syscalls.h>
#include <nacl_dirent.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common/alog.h"
#include "common/irt_wrapper_util.h"

#if defined(__native_client__)
static ssize_t nacl_getdents_wrapper(int fd, char* buf, size_t buf_size);
#else
#include <sys/syscall.h>
#endif

// NaCl's dirent lacks d_type field, so our getdents implementation
// assumes __nacl_irt_getdents is hooked by posix_translation and
// returns Bionic's dirent, not NaCl's. See also
// bionic/libc/arch-nacl/syscalls/__getdents64.c.
//
// Due to this reason, our getdents implementation does not work for
// __nacl_irt_getdents provided by NaCl's supervisor (e.g., sel_ldr)
// for unittests. We convert NaCl's dirent to Bionic's by this
// IRT wrapper.
IRT_WRAPPER(getdents, int fd, struct dirent* ent, size_t count,
            size_t* nread) {
#if defined(__native_client__)
  // NaCl's dirent lacks d_type field, so our getdents implementation
  // assumes __nacl_irt_getdents is hooked by posix_translation and
  // returns Bionic's dirent, not NaCl's. See also
  // bionic/libc/arch-nacl/syscalls/__getdents64.c.
  //
  // Due to this reason, our getdents implementation does not work for
  // __nacl_irt_getdents provided by NaCl's supervisor (e.g., sel_ldr)
  // for unittests. We convert NaCl's dirent to Bionic's by this
  // IRT wrapper.
  ssize_t result = nacl_getdents_wrapper(
      fd, reinterpret_cast<char*>(ent), count);
#else
  // nonsfi_loader does not implement __nacl_irt_getdents, so we call
  // it directly.
  ssize_t result = syscall(__NR_getdents64, fd, ent, count);
#endif
  if (result < 0)
    return errno;
  *nread = result;
  return 0;
}

// NaCl IRT does not support O_DIRECTORY. We emulate it by calling
// fstat for unittests. Production ARC does not have this issue
// because posix_translation does support O_DIRECTORY.
IRT_WRAPPER(open, const char* pathname, int oflags, mode_t cmode, int* newfd) {
  // Do not pass O_DIRECTORY bit. nonsfi_loader on ARM does not
  // understand ARM's O_DIRECTORY which is different from
  // NACL_ABI_O_DIRECTORY.
  int result = __nacl_irt_open_real(pathname, oflags & ~O_DIRECTORY,
                                    cmode, newfd);
  if (!result && (oflags & O_DIRECTORY)) {
    struct stat st;
    if (fstat(*newfd, &st))
      LOG_ALWAYS_FATAL("fstat unexpectedly failed");
    if (!S_ISDIR(st.st_mode)) {
      if (close(*newfd))
        LOG_ALWAYS_FATAL("close unexpectedly failed");
      return ENOTDIR;
    }
  }
  return result;
}

#if defined(__native_client__)
// This should be defined after IRT_WRAPPER(getdents) because
// nacl_getdents_wrapper.h uses __nacl_irt_getdents_real.
#define DIRENT_TYPE struct dirent
#include "nacl_getdents_wrapper.h"  // NOLINT
#endif

void InjectIrtHooks() {
  DO_WRAP(getdents);
  DO_WRAP(open);
}
