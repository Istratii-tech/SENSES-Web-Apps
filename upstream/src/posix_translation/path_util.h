// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POSIX_TRANSLATION_PATH_UTIL_H_
#define POSIX_TRANSLATION_PATH_UTIL_H_

#include <string>

#include "base/strings/string_piece.h"

namespace posix_translation {
namespace util {

// A special path component meaning "this directory."
const char kCurrentDirectory[] = ".";

// Returns a string corresponding to the directory containing the given
// path. If the string only contains one component, returns a string
// identifying kCurrentDirectory. If the string already refers to the root
// directory, returns a string identifying the root directory.  If the path
// ends with a slash, the slash is handled as if it does not exist
// (i.e. GetDirName("/foo/bar") == GetDirName("/foo/bar/") == "/foo").
std::string GetDirName(const base::StringPiece& path);

// Similar to GetDirName() but this function modifies the input parameter
// in-place.
void GetDirNameInPlace(std::string* in_out_path);

// Joins |dirname| and |basename|. Note that this function takes std::string
// rather than StringPiece as std::strings are needed internally (otherwise,
// need to copy strings).
std::string JoinPath(const std::string& dirname,
                     const std::string& basename);

// Appends a trailing separator to the string if it does not exist. If the
// input string is empty, "/" will be returned.
void EnsurePathEndsWithSlash(std::string* in_out_path);

// Returns true if |path| starts with '/'.
inline bool IsAbsolutePath(const base::StringPiece& path) {
  return path.starts_with("/");
}

// Returns true if |path| ends with '/'.
inline bool EndsWithSlash(const base::StringPiece& path) {
  return path.ends_with("/");
}

// Removes all single '.'s and replaces '//+' with '/' in |in_out_path|. The
// resulting string does not end with a slash unless it is "/", the root
// directory. The resulting string is "." if the path is equivalent of "."
// (ex. "./" and "./././").
void RemoveSingleDotsAndRedundantSlashes(std::string* in_out_path);

// Removes trailing slashes in the given path, but "/" will remain as "/".
void RemoveTrailingSlashes(std::string* in_out_path);

}  // namespace util
}  // namespace posix_translation

#endif  // POSIX_TRANSLATION_PATH_UTIL_H_
