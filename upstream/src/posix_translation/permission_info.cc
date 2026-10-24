// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "posix_translation/permission_info.h"

namespace posix_translation {

const uid_t PermissionInfo::kInvalidUid = static_cast<uid_t>(-1);

PermissionInfo::PermissionInfo()
    : file_uid_(kInvalidUid),
      is_writable_(false) {
}

PermissionInfo::PermissionInfo(uid_t file_uid, bool is_writable)
    : file_uid_(file_uid),
      is_writable_(is_writable) {
}

PermissionInfo::~PermissionInfo() {
}

bool PermissionInfo::IsValid() const {
  return file_uid_ != kInvalidUid;
}

}  // namespace posix_translation
