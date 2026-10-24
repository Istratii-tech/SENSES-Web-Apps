// Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gtest/gtest.h"
#include "posix_translation/permission_info.h"
#include "posix_translation/test_util/file_system_test_common.h"

namespace posix_translation {

class PermissionInfoTest : public FileSystemTestCommon {
 protected:
  static uid_t GetInvalidUid() {
    return PermissionInfo::kInvalidUid;
  }
};

TEST_F(PermissionInfoTest, TestDefaultConstructor) {
  PermissionInfo info;
  EXPECT_EQ(GetInvalidUid(), info.file_uid());
  EXPECT_FALSE(info.IsValid());
  EXPECT_FALSE(info.is_writable());
}

TEST_F(PermissionInfoTest, TestConstructor) {
  static const uid_t kMyUid = 12345;
  PermissionInfo info(kMyUid, true /* writable */);
  EXPECT_EQ(kMyUid, info.file_uid());
  EXPECT_TRUE(info.IsValid());
  EXPECT_TRUE(info.is_writable());
  PermissionInfo info2(kMyUid, false /* not writable */);
  EXPECT_EQ(kMyUid, info2.file_uid());
  EXPECT_TRUE(info2.IsValid());
  EXPECT_FALSE(info2.is_writable());
}

}  // namespace posix_translation
