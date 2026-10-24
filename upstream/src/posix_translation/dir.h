// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POSIX_TRANSLATION_DIR_H_
#define POSIX_TRANSLATION_DIR_H_

#include <dirent.h>
#include <string>

namespace posix_translation {

// Interface to a directory's contents.
class Dir {
 public:
  virtual ~Dir() {}
  virtual bool GetNext(dirent* entry) = 0;
  virtual void rewinddir() = 0;

  enum Type {
    REGULAR = DT_REG,
    DIRECTORY = DT_DIR,
    SYMLINK = DT_LNK,
  };

  // Adds an entry. This can only be called before GetNext() is called for the
  // first time or right after rewinddir() is called. If |name| already exists,
  // Add() overwrites the existing one.
  virtual void Add(const std::string& name, Type type) = 0;
};

}  // namespace posix_translation
#endif  // POSIX_TRANSLATION_DIR_H_
