// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMMON_TRACE_EVENT_PPAPI_H_
#define COMMON_TRACE_EVENT_PPAPI_H_

#include "ppapi/c/dev/ppb_trace_event_dev.h"

namespace arc {
namespace trace {

void Init(const PPB_Trace_Event_Dev* iface);

}  // namespace trace
}  // namespace arc

#endif  // COMMON_TRACE_EVENT_PPAPI_H_
