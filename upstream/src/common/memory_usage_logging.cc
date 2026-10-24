// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// TODO(crbug.com/434999): Remove this file.

#include <pthread.h>
#include <unistd.h>

#include "common/alog.h"
#include "common/memory_usage_logging.h"
#include "common/performance.h"
#include "common/trace_event.h"

#if defined(MEMORY_USAGE_LOGGING)

#undef LOG_TAG
#define LOG_TAG "MemoryUsage"

namespace arc {

namespace {

const char kResidentMemoryCounter[] = "ResidentB";
const char kVirtualMemoryCounter[] = "VirtualB";

const int kMicrosecondsBetweenLogging = 100000;  // 100ms

void* MemoryUsageLoop(void* unused) {
  while (true) {
    int virtual_bytes = 0;
    int resident_bytes = 0;
    Performance::GetInstance()->GetMemoryUsage(&virtual_bytes, &resident_bytes);
    TRACE_COUNTER1(ARC_TRACE_CATEGORY, kVirtualMemoryCounter, virtual_bytes);
    TRACE_COUNTER1(ARC_TRACE_CATEGORY, kResidentMemoryCounter,
                   resident_bytes);
    ALOGI("Memory usage: Res: %dB, Virt: %dB", resident_bytes, virtual_bytes);
    usleep(kMicrosecondsBetweenLogging);
  }
  return NULL;
}

}  // anonymous namespace

extern "C" int __real_pthread_create(
    pthread_t* thread_out,
    pthread_attr_t const* attr,
    void* (*start_routine)(void*),  // NOLINT(readability/casting)
    void* arg);

void StartMemoryUsageLogging() {
  pthread_t thread;
  // We create a thread which will not be managed by ProcessEmulator
  // by using __real_pthread_create directly. Otherwise, we will
  // crash in ALOG_ASSERT in plugin_handle.cc because we have multiple
  // threads. As the thread for memory usage logging will not use
  // PluginHandle, faking ProcessEmulator is safe.
  __real_pthread_create(&thread, NULL, MemoryUsageLoop, NULL);
}

}  // namespace arc

#else  // MEMORY_USAGE_LOGGING

namespace arc {

void StartMemoryUsageLogging() {
  // Disabled.
}

}  // namespace arc

#endif  // MEMORY_USAGE_LOGGING
