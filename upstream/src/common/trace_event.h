// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMMON_TRACE_EVENT_H_
#define COMMON_TRACE_EVENT_H_

#include <stdint.h>

#define ARC_TRACE_CATEGORY "ARC"
#define ARC_MAIN_THREAD_NAME "ArcMain"

namespace arc {
namespace trace {

void SetThreadName(const char* name);
const unsigned char* GetCategoryEnabled(const char* category_name);
void AddTraceEvent(char phase,
                  const unsigned char* category_enabled,
                  const char* name,
                  uint64_t id,
                  int num_args,
                  const char** arg_names,
                  const unsigned char* arg_types,
                  const uint64_t* arg_values,
                  unsigned char flags);

}  // namespace trace
}  // namespace arc

// This header is modified from Chromium's base/debug/trace_event.h to provide
// tracing through the PPB_Trace_Event_Dev interface.

#define TRACE_EVENT_API_GET_CATEGORY_ENABLED \
    arc::trace::GetCategoryEnabled

// Add a trace event to the platform tracing system. Returns thresholdBeginId
// for use in a corresponding end TRACE_EVENT_API_ADD_TRACE_EVENT call.
// int TRACE_EVENT_API_ADD_TRACE_EVENT(
//                    char phase,
//                    const unsigned char* category_enabled,
//                    const char* name,
//                    unsigned long long id,
//                    int num_args,
//                    const char** arg_names,
//                    const unsigned char* arg_types,
//                    const unsigned long long* arg_values,
//                    int threshold_begin_id,
//                    long long threshold,
//                    unsigned char flags)
#define TRACE_EVENT_API_ADD_TRACE_EVENT arc::trace::AddTraceEvent

// Defines atomic operations used internally by the tracing system.
// Per comments in trace_event_internal.h these require no memory barrier,
// and the Chromium gcc versions are defined as plain int load/store.
#define TRACE_EVENT_API_ATOMIC_WORD int
#define TRACE_EVENT_API_ATOMIC_LOAD(var) (var)
#define TRACE_EVENT_API_ATOMIC_STORE(var, value) ((var) = (value))

// Defines visibility for classes in trace_event_internal.h.
#define TRACE_EVENT_API_CLASS_EXPORT

#include "common/trace_event_internal.h"

#endif  // COMMON_TRACE_EVENT_H_
