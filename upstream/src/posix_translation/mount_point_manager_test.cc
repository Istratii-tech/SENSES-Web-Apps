// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/compiler_specific.h"
#include "gtest/gtest.h"
#include "posix_translation/file_system_handler.h"
#include "posix_translation/mount_point_manager.h"
#include "posix_translation/test_util/stub_file_system_handler.h"

namespace posix_translation {

TEST(MountPointManagerTest, MountUnmountTest) {
  MountPointManager mount_points;
  StubFileSystemHandler handler;
  uid_t uid;

  mount_points.Add("/path/to/file", &handler);
  EXPECT_EQ(&handler, mount_points.GetFileSystemHandler("/path/to/file", &uid));
  mount_points.Remove("/path/to/file");
  EXPECT_EQ(NULL, mount_points.GetFileSystemHandler("/path/to/file", &uid));

  mount_points.Add("/path/to/dir/", &handler);
  EXPECT_EQ(&handler, mount_points.GetFileSystemHandler("/path/to/dir/", &uid));
  mount_points.Remove("/path/to/dir/");
  EXPECT_EQ(NULL, mount_points.GetFileSystemHandler("/path/to/dir/", &uid));
}

TEST(MountPointManagerTest, TestGetFileSystemHandler_mount_file) {
  MountPointManager mount_points;
  StubFileSystemHandler handler;
  mount_points.Add("/path/to/file", &handler);
  mount_points.ChangeOwner("/path/to/file", 1000);
  uid_t uid;
  EXPECT_EQ(&handler, mount_points.GetFileSystemHandler("/path/to/file", &uid));
  EXPECT_EQ(1000U, uid);
  EXPECT_EQ(NULL, mount_points.GetFileSystemHandler("/path/to/file_", &uid));
  EXPECT_EQ(0U, uid);
  EXPECT_EQ(NULL, mount_points.GetFileSystemHandler("/path/to/file2", &uid));
  EXPECT_EQ(0U, uid);
  EXPECT_EQ(NULL, mount_points.GetFileSystemHandler("/path/to/file/", &uid));
  EXPECT_EQ(0U, uid);
  EXPECT_EQ(NULL, mount_points.GetFileSystemHandler("/path/to/file/foo", &uid));
  EXPECT_EQ(0U, uid);
  EXPECT_EQ(NULL, mount_points.GetFileSystemHandler("/path/to/fil", &uid));
  EXPECT_EQ(0U, uid);
  EXPECT_EQ(NULL, mount_points.GetFileSystemHandler("path/to/fil", &uid));
  EXPECT_EQ(0U, uid);
  EXPECT_EQ(NULL, mount_points.GetFileSystemHandler("path/to/file", &uid));
  EXPECT_EQ(0U, uid);
  EXPECT_EQ(NULL, mount_points.GetFileSystemHandler("file", &uid));
  EXPECT_EQ(0U, uid);
  EXPECT_EQ(NULL, mount_points.GetFileSystemHandler("file1", &uid));
  EXPECT_EQ(0U, uid);
}

TEST(MountPointManagerTest, TestGetFileSystemHandler_mount_dir) {
  MountPointManager mount_points;
  StubFileSystemHandler handler;
  mount_points.Add("/path/to/dir/", &handler);
  mount_points.ChangeOwner("/path/to/dir/", 1000);
  uid_t uid;
  EXPECT_EQ(&handler, mount_points.GetFileSystemHandler("/path/to/dir", &uid));
  EXPECT_EQ(1000U, uid);
  EXPECT_EQ(&handler, mount_points.GetFileSystemHandler("/path/to/dir/", &uid));
  EXPECT_EQ(1000U, uid);
  EXPECT_EQ(&handler, mount_points.GetFileSystemHandler("/path/to/dir/1",
                                                        &uid));
  EXPECT_EQ(1000U, uid);
  EXPECT_EQ(&handler, mount_points.GetFileSystemHandler("/path/to/dir/1/2",
                                                        &uid));
  EXPECT_EQ(1000U, uid);
  EXPECT_EQ(NULL, mount_points.GetFileSystemHandler("/path/", &uid));
  EXPECT_EQ(0U, uid);
  EXPECT_EQ(NULL, mount_points.GetFileSystemHandler("/path", &uid));
  EXPECT_EQ(0U, uid);
  EXPECT_EQ(NULL, mount_points.GetFileSystemHandler("/", &uid));
  EXPECT_EQ(0U, uid);
  EXPECT_EQ(NULL, mount_points.GetFileSystemHandler(".", &uid));
  EXPECT_EQ(0U, uid);
  EXPECT_EQ(NULL, mount_points.GetFileSystemHandler("path/to/dir", &uid));
  EXPECT_EQ(0U, uid);
  EXPECT_EQ(NULL, mount_points.GetFileSystemHandler("path/to/dir1", &uid));
  EXPECT_EQ(0U, uid);
  EXPECT_EQ(NULL, mount_points.GetFileSystemHandler("dir", &uid));
  EXPECT_EQ(0U, uid);
  EXPECT_EQ(NULL, mount_points.GetFileSystemHandler("dir1", &uid));
  EXPECT_EQ(0U, uid);

  mount_points.Add("/", &handler);
  mount_points.ChangeOwner("/", 2000);
  EXPECT_EQ(&handler, mount_points.GetFileSystemHandler("/", &uid));
  EXPECT_EQ(2000U, uid);
}

TEST(MountPointManagerTest, TestGetFileSystemHandler_empty) {
  MountPointManager mount_points;
  StubFileSystemHandler handler;
  mount_points.Add("/path/to/dir/", &handler);
  mount_points.ChangeOwner("/path/to/dir/", 1000);
  uid_t uid;
  EXPECT_TRUE(NULL == mount_points.GetFileSystemHandler("", &uid));
}

}  // namespace posix_translation
