// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Declares the table for wrapped functions.
//

#ifndef COMMON_WRAPPED_FUNCTIONS_H_
#define COMMON_WRAPPED_FUNCTIONS_H_

namespace arc {

struct WrappedFunction {
  // The name of function such as "access".
  const char* name;
  // The pointer to the function definition (e.g., __wrap_access).
  void* func;
};

// This array is terminated by an entry whose |name| and |func| are NULL.
extern WrappedFunction kWrappedFunctions[];

}  // namespace arc

#endif  // COMMON_WRAPPED_FUNCTIONS_H_
