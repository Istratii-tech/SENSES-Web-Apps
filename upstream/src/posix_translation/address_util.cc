// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "posix_translation/address_util.h"

#include <unistd.h>

namespace posix_translation {
namespace util {

size_t GetPageSize() {
  // sysconf(_SC_PAGESIZE) is cheap as it does not call into IRT.
  return sysconf(_SC_PAGESIZE);
}

uint32_t GetPageSizeAsNumBits() {
  return CountTrailingZeros(GetPageSize());
}

size_t RoundToPageSize(size_t length) {
  const size_t page_size = GetPageSize();
  return (length + page_size - 1) & ~(page_size - 1);
}

bool IsPageAligned(const void* addr) {
  return !(reinterpret_cast<uintptr_t>(addr) & (GetPageSize() - 1));
}

uint32_t CountTrailingZeros(uint32_t value) {
  if (!value)
    return 32;  // __builtin_ctz() returns undefined result for 0.
  return __builtin_ctz(value);
}

}  // namespace util
}  // namespace posix_translation
