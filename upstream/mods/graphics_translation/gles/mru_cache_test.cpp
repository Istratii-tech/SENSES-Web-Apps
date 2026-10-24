/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "graphics_translation/gles/mru_cache.h"
#include <string>
#include "gtest/gtest.h"

static const int kCacheCapacity = 4;
typedef MruCache<int, std::string> TestCache;

TEST(MruCache, InitialEmpty) {
  TestCache c(kCacheCapacity);
  EXPECT_TRUE(c.GetMostRecentlyUsed() == NULL);
}

TEST(MruCache, Get) {
  TestCache c(kCacheCapacity);
  c.Push(1, "hello");
  c.Push(2, "world");
  EXPECT_EQ(*c.Get(1), "hello");
  EXPECT_EQ(*c.Get(2), "world");
  EXPECT_TRUE(c.Get(3) == NULL);
}

TEST(MruCache, GetMostRecentlyUsed) {
  TestCache c(kCacheCapacity);
  c.Push(1, "hello");
  c.Push(2, "world");
  EXPECT_EQ(*c.GetMostRecentlyUsed(), "world");
}

TEST(MruCache, Eviction) {
  TestCache c(kCacheCapacity);
  c.Push(1, "hello");
  c.Push(2, "world");
  c.Push(3, "how");
  c.Push(4, "are");
  c.Push(5, "you");
  EXPECT_TRUE(c.Get(1) == NULL);
  EXPECT_TRUE(c.Get(2) != NULL);
}

TEST(MruCache, Mru) {
  TestCache c(kCacheCapacity);
  c.Push(1, "hello");
  EXPECT_EQ(*c.GetMostRecentlyUsed(), "hello");

  c.Push(2, "world");
  EXPECT_EQ(*c.GetMostRecentlyUsed(), "world");

  c.Get(1);
  EXPECT_EQ(*c.GetMostRecentlyUsed(), "hello");
}
