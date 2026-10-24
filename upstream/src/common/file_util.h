// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// This file defines utility functions for working with files.

#ifndef COMMON_FILE_UTIL_H_
#define COMMON_FILE_UTIL_H_

#include <string>

namespace arc {

// Returns the base name of the given path.
const char* GetBaseName(const char* pathname);

// Returns true if pathname is inside the given directory.
bool IsInDirectory(const std::string& pathname, const std::string& dirname);

}  // namespace arc

#endif  // COMMON_FILE_UTIL_H_
