// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "common/file_util.h"

#include <string.h>

#include <vector>

#include "base/strings/string_util.h"
#include "common/alog.h"

namespace arc {

const char* GetBaseName(const char* pathname) {
  ALOG_ASSERT(pathname);
  const char* pos = strrchr(pathname, '/');
  if (!pos)
    return pathname;
  return pos + 1;
}

bool IsInDirectory(const std::string& pathname, const std::string& dirname) {
  if (StartsWithASCII(pathname, dirname, true)) {
    if (dirname[dirname.size() - 1] == '/')
      return true;
    const char last = pathname[dirname.size()];
    if (last == '\0' || last == '/')
      return true;
  }
  return false;
}

}  // namespace arc
