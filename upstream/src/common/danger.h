// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Macros that generate grep-bait to show where partially implemented code
// and completely unimplemented code is being executed.
//

#ifndef COMMON_DANGER_H_
#define COMMON_DANGER_H_

#include <stdio.h>
#include <stdlib.h>

#include "common/alog.h"

// DANGER and DANGERF should be used in partially-implemented code, where
// we have made some assumptions about how the code is used and believe
// that the implementation as exists should be adequate.
#define DANGER() do {                                                \
  ALOGW(__FILE__":%d, DANGER %s", __LINE__, __FUNCTION__);           \
} while (0)

#define DANGERF(_format, _arguments...) do {                         \
  ALOGW(__FILE__":%d, DANGER %s (" _format ")",                      \
        __LINE__, __FUNCTION__, ## _arguments);                      \
} while (0)

// NOT_IMPLEMENTED should be used where we know we have not implemented
// something that needs to be implemented.  Finished code should not
// have NOT_IMPLEMENTED in it.
#define NOT_IMPLEMENTED() do {                                       \
  ALOGE(__FILE__ ":%d, %s NOT_IMPLEMENTED", __LINE__, __FUNCTION__); \
} while (0)

#endif  // COMMON_DANGER_H_
