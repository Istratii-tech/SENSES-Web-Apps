// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "posix_translation/path_util.h"

#include <vector>

#include "base/strings/string_split.h"
#include "common/alog.h"

namespace posix_translation {
namespace util {

std::string GetDirName(const base::StringPiece& path) {
  std::string dirname(path.data(), path.size());
  GetDirNameInPlace(&dirname);
  return dirname;
}

void GetDirNameInPlace(std::string* in_out_path) {
  ALOG_ASSERT(in_out_path);

  const size_t length = in_out_path->length();
  size_t search_start = std::string::npos;
  if ((length >= 2) && EndsWithSlash(*in_out_path))
    search_start = length - 2;

  const size_t pos = in_out_path->rfind('/', search_start);
  if (!pos)
    *in_out_path = "/";
  else if (pos == std::string::npos)
    *in_out_path = kCurrentDirectory;
  else
    in_out_path->erase(pos);
}

std::string JoinPath(const std::string& dirname,
                     const std::string& basename) {
  if (EndsWithSlash(dirname))
    return dirname + basename;
  else
    return dirname + "/" + basename;
}

void EnsurePathEndsWithSlash(std::string* in_out_path) {
  ALOG_ASSERT(in_out_path);
  if (!EndsWithSlash(*in_out_path))
    in_out_path->append("/");
}

void RemoveSingleDotsAndRedundantSlashes(std::string* in_out_path) {
  ALOG_ASSERT(in_out_path);

  if (in_out_path->find('.') == std::string::npos &&
      in_out_path->find("//") == std::string::npos) {
    // Fast path.
    if (util::EndsWithSlash(*in_out_path) && in_out_path->length() > 2U)
      in_out_path->erase(in_out_path->length() - 1);
    // Check the post condition of the function.
    ALOG_ASSERT(*in_out_path == "/" || !util::EndsWithSlash(*in_out_path));
    return;
  }
  ALOG_ASSERT(!in_out_path->empty());

  const bool is_absolute = ((*in_out_path)[0] == '/');
  std::vector<std::string> directories;
  base::SplitString(*in_out_path, '/', &directories);
  in_out_path->clear();
  if (is_absolute)
    in_out_path->assign("/");
  for (size_t i = 0; i < directories.size(); ++i) {
    const std::string& directory = directories[i];
    if (directory == "." || directory.empty())
      continue;  // Skip empty (i.e. //) or .
    in_out_path->append(directory + "/");
  }

  // When path consists of only './', we will end up with empty
  // string. Make it be ".".
  if (in_out_path->empty()) {
    in_out_path->assign(".");
    return;
  }

  // Remove the trailing "/".
  ALOG_ASSERT(util::EndsWithSlash(*in_out_path));
  if (in_out_path->length() > 2U)
    in_out_path->erase(in_out_path->length() - 1);

  // Check the post condition of the function.
  ALOG_ASSERT(*in_out_path == "/" || !util::EndsWithSlash(*in_out_path));
}

void RemoveTrailingSlashes(std::string* in_out_path) {
  while (in_out_path->length() > 1U && EndsWithSlash(*in_out_path))
    in_out_path->erase(in_out_path->length() - 1);
  ALOG_ASSERT(*in_out_path == "/" || !EndsWithSlash(*in_out_path));
}

}  // namespace util
}  // namespace posix_translation
