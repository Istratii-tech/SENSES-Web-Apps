// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Provide an interface which allows to create writable executable pages.
//

#ifndef COMMON_MPROTECT_RWX_H_
#define COMMON_MPROTECT_RWX_H_

// This is only for Bare Metal mode.
#if !defined(__native_client__)

#include <stddef.h>

namespace arc {

// Creates RWX pages. When you call this function, you change must be
// reviewed by security team.
//
// This function follows libc's ABI. I.e., this function returns 0 on
// success, and returns -1 and sets errno on failure.
int MprotectRWX(void* addr, size_t len);

}  // namespace arc

#endif  // !defined(__native_client__)

#endif  // COMMON_MPROTECT_RWX_H_
