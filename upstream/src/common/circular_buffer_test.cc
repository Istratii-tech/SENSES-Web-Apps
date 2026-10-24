// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//

#include "common/circular_buffer.h"
#include "gtest/gtest.h"

char SimpleHash(size_t index) {
  char ret = static_cast<char>(index);
  ret *= 3;
  ret ^= 0xA5;
  return ret;
}

TEST(CircularBufferTest, BasicUsage) {
  arc::CircularBuffer* buffy = new arc::CircularBuffer();

  char src[102];
  char dst[102];

  for (size_t iii = 0; iii < sizeof(src); iii++) {
    src[iii] = static_cast<char>(iii * 3);
    dst[iii] = SimpleHash(iii);
  }

  EXPECT_EQ(size_t(0), buffy->capacity());
  buffy->set_capacity(50);
  EXPECT_EQ(size_t(50), buffy->capacity());
  EXPECT_EQ(size_t(0), buffy->size());
  EXPECT_EQ(size_t(50), buffy->remaining());

  EXPECT_EQ(size_t(0), buffy->read(&dst[1], 100));
  EXPECT_EQ(size_t(50), buffy->write(&src[1], 100));
  EXPECT_EQ(size_t(50), buffy->read(&dst[1], 100));
  EXPECT_EQ(SimpleHash(0), dst[0]);
  EXPECT_EQ(SimpleHash(51), dst[51]);
  for (size_t iii = 1; iii < 51; iii++) {
    EXPECT_EQ(src[iii], dst[iii]);
  }

  EXPECT_EQ(size_t(25), buffy->write(&src[51], 25));
  EXPECT_EQ(size_t(10), buffy->write(&src[76], 10));
  EXPECT_EQ(size_t(35), buffy->read(&dst[1], 35));
  for (size_t iii = 1; iii < 36; iii++) {
    EXPECT_EQ(src[iii+50], dst[iii]);
  }

  // Test |clear|.
  for (size_t iii = 0; iii < sizeof(dst); iii++) {
    dst[iii] = SimpleHash(iii);
  }
  EXPECT_EQ(size_t(0), buffy->size());
  EXPECT_EQ(size_t(15), buffy->write(&src[1], 15));
  EXPECT_EQ(size_t(35), buffy->remaining());
  buffy->clear();
  EXPECT_EQ(size_t(0), buffy->size());
  EXPECT_EQ(size_t(50), buffy->remaining());
  EXPECT_EQ(size_t(20), buffy->write(&src[1], 20));
  EXPECT_EQ(size_t(20), buffy->size());
  EXPECT_EQ(size_t(30), buffy->remaining());
  EXPECT_EQ(size_t(20), buffy->read(&dst[1], 50));
  for (size_t iii = 1; iii < 21; iii++) {
    EXPECT_EQ(src[iii], dst[iii]);
  }

  delete buffy;
}

TEST(CircularBufferTest, SetCapacity) {
  arc::CircularBuffer buff;
  char src[20];
  char dst[20];
  for (size_t iii = 0; iii < sizeof(src); iii++) {
    src[iii] = static_cast<char>(iii);
    dst[iii] = ~src[iii];
  }

  EXPECT_EQ(size_t(0), buff.capacity());
  EXPECT_EQ(size_t(0), buff.remaining());
  buff.set_capacity(20);
  EXPECT_EQ(size_t(20), buff.capacity());
  EXPECT_EQ(size_t(20), buff.write(src, 20));
  EXPECT_EQ(size_t(10), buff.read(dst, 10));
  EXPECT_EQ(size_t(10), buff.write(src, 20));
  EXPECT_EQ(size_t(0), buff.remaining());
  buff.set_capacity(50);
  EXPECT_EQ(size_t(50), buff.capacity());
  EXPECT_EQ(size_t(20), buff.size());
  EXPECT_EQ(size_t(30), buff.remaining());
  EXPECT_EQ(size_t(20), buff.read(dst, 20));
  EXPECT_EQ(size_t(50), buff.remaining());

  for (size_t iii = 0; iii < 10; iii++) {
    EXPECT_EQ(src[iii], dst[iii + 10]);
    EXPECT_EQ(src[iii + 10], dst[iii]);
  }
}

TEST(CircularBufferTest, ExtendedUsage) {
  const size_t kMaxSize = 20;
  char src[kMaxSize];
  char dst[kMaxSize];
  for (size_t iii = 0; iii < kMaxSize; iii++) {
    src[iii] = static_cast<char>(iii);
  }
  for (size_t desired_end = 0; desired_end < kMaxSize; desired_end++) {
    for (size_t desired_start = 0; desired_start < kMaxSize; desired_start++) {
      size_t expected_size;
      if (desired_start <= desired_end) {
        expected_size = desired_end - desired_start;
      } else {
        expected_size = kMaxSize - desired_start + desired_end;
      }
      ASSERT_LE(expected_size, kMaxSize);
      arc::CircularBuffer* buff = new arc::CircularBuffer();
      buff->set_capacity(kMaxSize);

      if (desired_start <= desired_end) {
        EXPECT_EQ(desired_end, buff->write(src, desired_end));
        EXPECT_EQ(desired_start, buff->read(dst, desired_start));
      } else {
        EXPECT_EQ(kMaxSize, buff->write(src, kMaxSize));
        EXPECT_EQ(desired_start, buff->read(dst, desired_start));
        EXPECT_EQ(desired_end, buff->write(src, desired_end));
      }
      EXPECT_EQ(expected_size, buff->size());

      EXPECT_EQ(expected_size, buff->read(dst, expected_size));
      EXPECT_EQ(size_t(0), buff->size());
      for (size_t iii = 0; iii < expected_size; iii++) {
        EXPECT_EQ(src[(iii + desired_start) % kMaxSize], dst[iii]);
      }
      EXPECT_EQ(kMaxSize, buff->write(src, kMaxSize));
      EXPECT_EQ(kMaxSize, buff->read(dst, kMaxSize));
      for (size_t iii = 0; iii < expected_size; iii++) {
        EXPECT_EQ(src[iii], dst[iii]);
      }

      delete buff;
    }
  }
}
