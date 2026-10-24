// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POSIX_TRANSLATION_ADDRESS_UTIL_H_
#define POSIX_TRANSLATION_ADDRESS_UTIL_H_

#include "base/basictypes.h"

namespace posix_translation {
namespace util {

// Returns the page size of the running operating system.
size_t GetPageSize();

// Returns the page size as the number of bits (ex. this function returns 12
// if the page size is 4096 because 2^12 = 4096).
uint32_t GetPageSizeAsNumBits();

// Rounds up |length| to nearest multiple of the page size.
size_t RoundToPageSize(size_t length);

// Returns true if the given address is page-aligned.
bool IsPageAligned(const void* addr);

// Counts trailing zeros in the given 32-bit value. Returns 32 if the value
// is 0.
uint32_t CountTrailingZeros(uint32_t value);

}  // namespace util
}  // namespace posix_translation

#endif  // POSIX_TRANSLATION_ADDRESS_UTIL_H_
