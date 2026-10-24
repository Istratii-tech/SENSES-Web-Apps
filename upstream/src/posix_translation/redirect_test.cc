// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <errno.h>

#include "base/compiler_specific.h"
#include "base/memory/scoped_ptr.h"
#include "gtest/gtest.h"
#include "posix_translation/directory_manager.h"
#include "posix_translation/redirect.h"
#include "posix_translation/test_util/file_system_test_common.h"

namespace posix_translation {

namespace {

const char kPathAlreadyExists[] = "/alreadyexists";

class TestUnderlyingHandler : public FileSystemHandler {
 public:
  TestUnderlyingHandler()
      : FileSystemHandler("TestUnderlyingHandler"), is_initialized_(false) {
  }
  virtual ~TestUnderlyingHandler() {
  }

  virtual void Initialize() OVERRIDE { is_initialized_ = true; }
  virtual bool IsInitialized() const OVERRIDE { return is_initialized_; }

  virtual scoped_refptr<FileStream> open(
      int fd, const std::string& pathname, int oflag, mode_t cmode) OVERRIDE {
    return NULL;
  }
  virtual int stat(const std::string& pathname, struct stat* out) OVERRIDE {
    if (pathname == kPathAlreadyExists)
      return 0;
    return -1;
  }
  virtual int statfs(const std::string& pathname, struct statfs* out) OVERRIDE {
    return -1;
  }
  virtual Dir* OnDirectoryContentsNeeded(const std::string& name) OVERRIDE {
    DirectoryManager manager;
    manager.MakeDirectories(name);
    manager.AddFile(name + "/0");
    manager.AddFile(name + "/1");
    return manager.OpenDirectory(name);
  }

  bool is_initialized_;

 private:
  DISALLOW_COPY_AND_ASSIGN(TestUnderlyingHandler);
};

}  // namespace

class RedirectHandlerTestTest : public FileSystemTestCommon {
 protected:
  RedirectHandlerTestTest() {
  }
  virtual ~RedirectHandlerTestTest() {
  }

  virtual void SetUp() OVERRIDE {
    FileSystemTestCommon::SetUp();
    TestUnderlyingHandler* underlying = new TestUnderlyingHandler;

    std::vector<std::pair<std::string, std::string> > symlinks;
    symlinks.push_back(std::make_pair("/dest", "/src0"));
    symlinks.push_back(std::make_pair("/dest", "/src1"));

    handler_.reset(new RedirectHandler(underlying, symlinks, true));
    handler_->Initialize();
    EXPECT_TRUE(handler_->IsInitialized());
    // Confirm that RedirectHandler delegates the call to the underlying
    // handler.
    EXPECT_TRUE(underlying->IsInitialized());
  }

  scoped_ptr<FileSystemHandler> handler_;

 private:
  DISALLOW_COPY_AND_ASSIGN(RedirectHandlerTestTest);
};

TEST_F(RedirectHandlerTestTest, TestInit) {
  // Empty. Confirms EXPECT_TRUE calls in SetUp() do not fail.
}

// Tests if the symlinks passed to the constructor work.
TEST_F(RedirectHandlerTestTest, TestSymlinksPassedToConstructor) {
  std::string result;

  errno = 0;
  EXPECT_EQ(5, handler_->readlink("/src0", &result));
  EXPECT_EQ(0, errno);
  EXPECT_EQ("/dest", result);
  result.clear();
  errno = 0;
  EXPECT_EQ(5, handler_->readlink("/src1", &result));
  EXPECT_EQ(0, errno);
  EXPECT_EQ("/dest", result);

  errno = 0;
  EXPECT_EQ(-1, handler_->readlink("/src2", &result));
  EXPECT_EQ(EINVAL, errno);
  errno = 0;
  EXPECT_EQ(-1, handler_->readlink("/src", &result));
  EXPECT_EQ(EINVAL, errno);
}

TEST_F(RedirectHandlerTestTest, TestSymlink) {
  EXPECT_EQ(0, handler_->symlink("/proc/42", "/proc/self"));
  // Try to create the same symlink which should fail.
  errno = 0;
  EXPECT_EQ(-1, handler_->symlink("/proc/42", "/proc/self"));
  EXPECT_EQ(EEXIST, errno);
}

TEST_F(RedirectHandlerTestTest, TestSymlinkExist) {
  // Try to create a symlink with the same name underlying_ file system
  // already has.
  errno = 0;
  EXPECT_EQ(-1, handler_->symlink("/proc/42", kPathAlreadyExists));
  EXPECT_EQ(EEXIST, errno);
}

TEST_F(RedirectHandlerTestTest, TestReadlink) {
  EXPECT_EQ(0, handler_->symlink("/proc/42", "/proc/self"));

  std::string result;
  errno = 0;
  EXPECT_EQ(-1, handler_->readlink("/proc/sel", &result));
  EXPECT_EQ(EINVAL, errno);
  errno = 0;
  EXPECT_EQ(-1, handler_->readlink("/proc/self0", &result));
  EXPECT_EQ(EINVAL, errno);
  errno = 0;
  EXPECT_EQ(-1, handler_->readlink("/proc/self/maps", &result));
  EXPECT_EQ(EINVAL, errno);

  EXPECT_EQ(8, handler_->readlink("/proc/self", &result));
  EXPECT_EQ("/proc/42", result);
  // We do not have to test "/proc/self/" case because our VFS always normalizes
  // it to "/proc/self".
}

TEST_F(RedirectHandlerTestTest, TestOnDirectoryContentsNeeded) {
  EXPECT_EQ(0, handler_->symlink("/proc/42", "/dir/1"));
  EXPECT_EQ(0, handler_->symlink("/proc/42", "/dir/2"));
  EXPECT_EQ(0, handler_->symlink("/proc/42", "/dir/3"));
  scoped_ptr<Dir> dirp(handler_->OnDirectoryContentsNeeded("/dir"));
  ASSERT_TRUE(dirp.get());

  dirent entry;
  EXPECT_TRUE(dirp->GetNext(&entry));
  EXPECT_EQ(std::string("."), entry.d_name);

  EXPECT_TRUE(dirp->GetNext(&entry));
  EXPECT_EQ(std::string(".."), entry.d_name);

  EXPECT_TRUE(dirp->GetNext(&entry));
  EXPECT_EQ(std::string("0"), entry.d_name);
  EXPECT_EQ(DT_REG, entry.d_type);

  EXPECT_TRUE(dirp->GetNext(&entry));
  EXPECT_EQ(std::string("1"), entry.d_name);
  EXPECT_EQ(DT_LNK, entry.d_type);  // not DT_REG

  EXPECT_TRUE(dirp->GetNext(&entry));
  EXPECT_EQ(std::string("2"), entry.d_name);
  EXPECT_EQ(DT_LNK, entry.d_type);

  EXPECT_TRUE(dirp->GetNext(&entry));
  EXPECT_EQ(std::string("3"), entry.d_name);
  EXPECT_EQ(DT_LNK, entry.d_type);

  EXPECT_FALSE(dirp->GetNext(&entry));
}

}  // namespace posix_translation
