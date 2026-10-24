// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <pthread.h>

#include <set>
#include <vector>

#include "common/alog.h"
#include "common/thread_local.h"
#include "gtest/gtest.h"

namespace arc {

DEFINE_THREAD_LOCAL(int, g_tls);

void* SetInt(void* data) {
  g_tls.Set(g_tls.Get() + reinterpret_cast<int>(data));
  // If |g_tls| does not have per-thread storage, |g_tls| may not be
  // equal to |data| here.
  return reinterpret_cast<void*>(g_tls.Get());
}

// TODO(crbug.com/362175): qemu-arm cannot reliably emulate threading
// functions so run them in a real ARM device.
TEST(ThreadLocalTest, QEMU_DISABLED_Basic) {
  static const int kNumThreads = 100;
  std::vector<pthread_t> threads(kNumThreads);

  for (int i = 0; i < kNumThreads; i++) {
    ASSERT_EQ(0, pthread_create(&threads[i], NULL,
                                &SetInt, reinterpret_cast<void*>(i)));
  }

  for (int i = 0; i < kNumThreads; i++) {
    void* data;
    pthread_join(threads[i], &data);
    int ret = reinterpret_cast<int>(data);
    EXPECT_EQ(i, ret);
  }
}

}  // namespace arc
