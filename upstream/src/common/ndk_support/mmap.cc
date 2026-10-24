// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "common/ndk_support/mmap.h"

#include <pthread.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

#include <string>

#include "base/md5.h"
#include "common/alog.h"
#include "common/mprotect_rwx.h"
#include "common/options.h"

namespace arc {

namespace {

uintptr_t g_mmap_hint_addr = 0x70000000;

bool ShouldAllowDefaultAddressHint() {
  // For now, we always give the default address hint on SFI NaCl as
  // the address hint will not be bad for security on SFI NaCl.
#if defined(__native_client__)
  return true;
#else
  // Old NDK apps require the default address hint. If an app crashes
  // inside its own copy of the Bionic's linker, saying something like
  // "no vspace available", you would likely need to flip this.
  //
  // As this feature is bad for security, this is only for
  // testing. You should manually flip this bit to test such apps.
  // When we need to launch apps which require this, you should ask
  // the app author to upgrade their runtime.
  return false;
#endif
}

#if defined(USE_NDK_DIRECT_EXECUTION)
pthread_once_t g_tls_init = PTHREAD_ONCE_INIT;
bool g_should_allow_rwx_pages;

// We allow a few whitelisted apps to use RWX pages. Although an APK
// package author can arbitrary choose the package name, this is safe
// as long as ARC is running for only whitelisted packages.
// TODO(crbug.com/462642): We need to update this comment and code
// when we remove ARC whitelist.
//
// Please make sure you get an approval from security team when you
// add an app to this list.
void InitShouldAllowRwxPages() {
  const std::string md5 = base::MD5String(
      Options::GetInstance()->GetString("package_name"));
  g_should_allow_rwx_pages = (
      md5 == "a1b1bbe5f63d5b96c1a0f87c197ebfae" ||
      md5 == "77f62c7141dd3730bf844c1c55e92b1f");
}

bool ShouldAllowRwxPages() {
  pthread_once(&g_tls_init, InitShouldAllowRwxPages);
  return g_should_allow_rwx_pages;
}
#endif

}  // namespace

void* MmapForNdk(void* addr, size_t length, int prot, int flags,
                 int fd, off_t offset) {
  if (!addr && !(flags & MAP_FIXED) && ShouldAllowDefaultAddressHint()) {
    // We use 0x70000000 as the first hint address. Then, the next
    // hint address will be increased by |length|, so this function
    // will likely keep satisfying the limitation of old Bionic's
    // loader which some NDKs have. On SFI NaCl, just keeping
    // specifying 0x70000000 works but it does not work on Bare Metal
    // mode. As SFI NaCl may change its internal implementation in
    // future, it would be better to always update the address hint
    // which is more likely used.
    //
    // Such NDK apps call mmap with NULL |addr| only twice at their
    // start-ups. On SFI NaCl these addresses are always not used.
    //
    // Essentially this way we emulate Android's mmap() behavior
    // better, by hinting where it shall allocate, if application has
    // no preferences.
    addr = reinterpret_cast<char*>(
        __sync_fetch_and_add(&g_mmap_hint_addr,
                             (length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1)));
  }

#if defined(USE_NDK_DIRECT_EXECUTION)
  bool needs_wx_prot = false;
  if ((prot & PROT_WRITE) && (prot & PROT_EXEC) && ShouldAllowRwxPages()) {
    prot &= ~PROT_EXEC;
    needs_wx_prot = true;
  }
  // Even when RWX mmap is requested and ShouldAllowRwxPages()
  // returns false, call normal libc's mmap which will not honor
  // RWX permissions. See also libc/arch-nacl/syscalls/mmap.c.
#endif

  void* result = mmap(addr, length, prot, flags, fd, offset);

#if defined(USE_NDK_DIRECT_EXECUTION)
  if (needs_wx_prot && result != MAP_FAILED) {
    int r = MprotectRWX(result, length);
    LOG_ALWAYS_FATAL_IF(r != 0, "RWX mprotect unexpectedly failed");
  }
#endif

  return result;
}

#if defined(USE_NDK_DIRECT_EXECUTION)
int MprotectForNdk(void* addr, size_t len, int prot) {
  if ((prot & PROT_WRITE) && (prot & PROT_EXEC) && ShouldAllowRwxPages())
    return MprotectRWX(addr, len);
  // Even when RWX mprotect is requested and ShouldAllowRwxPages()
  // returns false, call normal libc's mprotect which will not honor
  // RWX permissions. See also libc/arch-nacl/syscalls/mprotect.c.
  return mprotect(addr, len, prot);
}
#endif

}  // namespace arc
