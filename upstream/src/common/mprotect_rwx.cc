// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This is only for Bare Metal mode.
#if !defined(__native_client__)

#include "common/mprotect_rwx.h"

#include <irt_syscalls.h>

#include "native_client/src/trusted/service_runtime/include/bits/mman.h"

namespace arc {

int MprotectRWX(void* addr, size_t len) {
  const int prot =
      NACL_ABI_PROT_READ | NACL_ABI_PROT_WRITE | NACL_ABI_PROT_EXEC;
  int result = __nacl_irt_mprotect(addr, len, prot);
  if (result) {
    errno = result;
    return -1;
  }
  return 0;
}

}  // namespace arc

#endif  // !defined(__native_client__)
