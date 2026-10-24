// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string>

#include "base/compiler_specific.h"
#include "base/memory/scoped_ptr.h"
#include "gtest/gtest.h"
#include "posix_translation/dev_null.h"
#include "posix_translation/directory_file_stream.h"
#include "posix_translation/test_util/file_system_test_common.h"
#include "posix_translation/test_util/mock_file_handler.h"

namespace posix_translation {

namespace {
static const time_t kLastModifiedTime = 12345;
}  // namespace

class DirectoryFileStreamTest : public FileSystemTestCommon {
 protected:
  DirectoryFileStreamTest() {
  }

  virtual void SetUp() OVERRIDE {
    FileSystemTestCommon::SetUp();
    handler_.reset(new MockFileHandler);
  }

  virtual void TearDown() OVERRIDE {
    handler_.reset();
    FileSystemTestCommon::TearDown();
  }

  scoped_refptr<FileStream> GetDirectoryFileStream() {
    return new DirectoryFileStream(
        "test", "/", handler_.get(), kLastModifiedTime);
  }
  scoped_ptr<FileSystemHandler> handler_;

 private:
  DISALLOW_COPY_AND_ASSIGN(DirectoryFileStreamTest);
};

TEST_F(DirectoryFileStreamTest, TestConstruct) {
  scoped_refptr<FileStream> stream = GetDirectoryFileStream();
}

TEST_F(DirectoryFileStreamTest, TestFtruncate) {
  scoped_refptr<FileStream> stream = GetDirectoryFileStream();
  EXPECT_EQ(-1, stream->ftruncate(0));
  EXPECT_EQ(EINVAL, errno);
}

TEST_F(DirectoryFileStreamTest, TestLseek) {
  scoped_refptr<FileStream> stream = GetDirectoryFileStream();
  errno = 0;
  EXPECT_EQ(0, stream->lseek(0, SEEK_SET));
  EXPECT_EQ(0, errno);
}

TEST_F(DirectoryFileStreamTest, TestRead) {
  scoped_refptr<FileStream> stream = GetDirectoryFileStream();
  char buf[128];
  EXPECT_EQ(-1, stream->read(buf, sizeof(buf)));
  EXPECT_EQ(EISDIR, errno);
}

TEST_F(DirectoryFileStreamTest, TestWrite) {
  scoped_refptr<FileStream> stream = GetDirectoryFileStream();
  char buf[128] = {};
  EXPECT_EQ(-1, stream->write(buf, sizeof(buf)));
  EXPECT_EQ(EBADF, errno);
}

TEST_F(DirectoryFileStreamTest, TestFstat) {
  scoped_refptr<FileStream> stream = GetDirectoryFileStream();
  struct stat st = {};
  EXPECT_EQ(0, stream->fstat(&st));
  EXPECT_NE(0U, st.st_ino);
  EXPECT_EQ(kLastModifiedTime, st.st_mtime);
  EXPECT_EQ(S_IFDIR | 0U, st.st_mode);
}

TEST_F(DirectoryFileStreamTest, TestGetDentsFail) {
  // Replace |handler_| with a one which does not support
  // OnDirectoryContentsNeeded().
  handler_.reset(new DevNullHandler);
  ASSERT_EQ(NULL, handler_->OnDirectoryContentsNeeded("/"));
  scoped_refptr<FileStream> stream = GetDirectoryFileStream();
  dirent ent;
  EXPECT_EQ(-1, stream->getdents(&ent, 1 * sizeof(dirent)));
  EXPECT_EQ(ENOENT, errno);
}

TEST_F(DirectoryFileStreamTest, TestGetDents) {
  // First case, Empty folder. We expect only 2 entries
  scoped_refptr<FileStream> stream = GetDirectoryFileStream();
  errno = 0;
  dirent first_ent[3] = {};
  ASSERT_EQ(static_cast<int>(2 * sizeof(dirent)),
            stream->getdents(first_ent, 3 * sizeof(dirent)));
  EXPECT_EQ(0, errno);
  EXPECT_STREQ(".", first_ent[0].d_name);
  EXPECT_STREQ("..", first_ent[1].d_name);

  scoped_refptr<FileStream> tmp_file =
      handler_->open(-1, "/foo", O_WRONLY | O_CREAT | O_TRUNC, 0700);
  ASSERT_TRUE(tmp_file);

  // MockFileHandler supports OnDirectoryContentsNeeded().
  scoped_ptr<Dir> dir(handler_->OnDirectoryContentsNeeded(""));
  ASSERT_TRUE(NULL != dir.get());

  // Now we created foo and expects number of entries is increased
  stream = GetDirectoryFileStream();
  dirent second_ent[3] = {};
  EXPECT_EQ(-1, stream->getdents(second_ent, 0));
  EXPECT_EQ(EINVAL, errno);
  errno = 0;
  ASSERT_EQ(static_cast<int>(3 * sizeof(dirent)),
            stream->getdents(second_ent, 3 * sizeof(dirent)));
  EXPECT_EQ(0, errno);
  EXPECT_STREQ(".", second_ent[0].d_name);
  EXPECT_STREQ("..", second_ent[1].d_name);
  EXPECT_STREQ("foo", second_ent[2].d_name);

  // Rewind the stream and read the stream again with a smaller count.
  EXPECT_EQ(0, stream->lseek(0, SEEK_SET));
  errno = 0;
  dirent third_ent[3] = {};
  ASSERT_EQ(static_cast<int>(2 * sizeof(dirent)),
            stream->getdents(third_ent, 2.5 * sizeof(dirent)));
  EXPECT_EQ(0, errno);
  EXPECT_STREQ(".", third_ent[0].d_name);
  EXPECT_STREQ("..", third_ent[1].d_name);
}

}  // namespace posix_translation
