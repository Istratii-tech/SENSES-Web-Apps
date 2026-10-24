// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "posix_translation/address_util.h"

#include "gtest/gtest.h"

namespace posix_translation {
namespace util {

TEST(AddressUtilTest, GetPageSize) {
  EXPECT_LT(0U, GetPageSize());
}

TEST(AddressUtilTest, GetPageSizeAsNumBits) {
  EXPECT_EQ(GetPageSize(), 1U << GetPageSizeAsNumBits());
}

TEST(AddressUtilTest, RoundToPageSize) {
  const size_t pagesize = GetPageSize();
  EXPECT_EQ(0U, RoundToPageSize(0));
  EXPECT_EQ(pagesize, RoundToPageSize(1));
  EXPECT_EQ(pagesize, RoundToPageSize(pagesize - 1));
  EXPECT_EQ(pagesize, RoundToPageSize(pagesize));
  EXPECT_EQ(pagesize * 2, RoundToPageSize(pagesize + 1));
}

TEST(AddressUtilTest, IsPageAligned) {
  const size_t pagesize = GetPageSize();
  uintptr_t ptr = 0x0;
  EXPECT_TRUE(IsPageAligned(reinterpret_cast<void*>(ptr)));
  ++ptr;
  EXPECT_FALSE(IsPageAligned(reinterpret_cast<void*>(ptr)));
  ptr = pagesize - 1;
  EXPECT_FALSE(IsPageAligned(reinterpret_cast<void*>(ptr)));
  ++ptr;
  EXPECT_TRUE(IsPageAligned(reinterpret_cast<void*>(ptr)));
  ++ptr;
  EXPECT_FALSE(IsPageAligned(reinterpret_cast<void*>(ptr)));
}

TEST(AddressUtilTest, TestCountTrailingZeros) {
  static const size_t kBits = 32;
  EXPECT_EQ(kBits, CountTrailingZeros(0));

  uint32_t x = 1;
  for (size_t i = 0; i < kBits; ++i, x <<= 1) {
    EXPECT_EQ(i, CountTrailingZeros(x));
  }

  x = 0xffffffff;
  for (size_t i = 0; i < kBits; ++i, x <<= 1) {
    EXPECT_EQ(i, CountTrailingZeros(x));
  }
}

}  // namespace util
}  // namespace posix_translation
