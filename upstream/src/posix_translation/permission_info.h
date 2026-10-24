// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POSIX_TRANSLATION_PERMISSION_INFO_H_
#define POSIX_TRANSLATION_PERMISSION_INFO_H_

#include <sys/types.h>
#include <unistd.h>

#include "common/export.h"

namespace posix_translation {

// Maintains info necessary to implement permissions. As our
// permission implementation is limited because we do not run multiple
// applications at once, this class does not have much information.
class ARC_EXPORT PermissionInfo {
 public:
  PermissionInfo();
  PermissionInfo(uid_t file_uid, bool is_writable);
  ~PermissionInfo();

  bool IsValid() const;

  uid_t file_uid() const { return file_uid_; }
  bool is_writable() const { return is_writable_; }

 private:
  friend class PermissionInfoTest;

  uid_t file_uid_;
  bool is_writable_;
  static const uid_t kInvalidUid;
};

}  // namespace posix_translation

#endif  // POSIX_TRANSLATION_PERMISSION_INFO_H_
