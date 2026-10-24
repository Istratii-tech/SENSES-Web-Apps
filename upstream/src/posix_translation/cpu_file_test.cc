// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/basictypes.h"
#include "base/compiler_specific.h"
#include "base/memory/scoped_ptr.h"
#include "base/strings/stringprintf.h"
#include "gtest/gtest.h"
#include "posix_translation/cpu_file.h"
#include "posix_translation/dir.h"
#include "posix_translation/test_util/file_system_test_common.h"
#include "posix_translation/test_util/sysconf_util.h"

namespace posix_translation {

namespace {

// The number of physical/online CPUs in this test. Note that each test can
// change the number of online CPUs by creating another instance of
// ScopedNumProcessorsConfiguredSetting in the test.
static int kNumConfigured = 4;
static int kNumOnline = 2;

// Must be the same as the one in CpuFileHandler::Initialize.
static const char* kFiles[] =
  { "kernel_max", "offline", "online", "possible", "present" };

class CpuFileHandlerTest : public FileSystemTestCommon {
 protected:
  CpuFileHandlerTest()
      : num_configured_(kNumConfigured), num_online_(kNumOnline),
        handler_(new CpuFileHandler) {
  }

  ScopedNumProcessorsConfiguredSetting num_configured_;
  ScopedNumProcessorsOnlineSetting num_online_;
  scoped_ptr<FileSystemHandler> handler_;

 private:
  virtual void SetUp() OVERRIDE {
    FileSystemTestCommon::SetUp();
    handler_->OnMounted("/foo/");
    handler_->Initialize();
  }

  DISALLOW_COPY_AND_ASSIGN(CpuFileHandlerTest);
};

}  // namespace

TEST_F(CpuFileHandlerTest, TestInit) {
  EXPECT_TRUE(handler_->IsInitialized());
}

TEST_F(CpuFileHandlerTest, TestStat) {
  struct stat st = {};
  EXPECT_EQ(0, handler_->stat("/foo", &st));
  EXPECT_TRUE(S_ISDIR(st.st_mode));
  EXPECT_EQ(0, handler_->stat("/foo/", &st));
  EXPECT_TRUE(S_ISDIR(st.st_mode));
  EXPECT_EQ(-1, handler_->stat("/foo/cpu", &st));
  EXPECT_EQ(ENOENT, errno);
  EXPECT_EQ(0, handler_->stat("/foo/cpu0", &st));
  EXPECT_TRUE(S_ISDIR(st.st_mode));
  EXPECT_EQ(0, handler_->stat("/foo/cpu0/", &st));
  EXPECT_TRUE(S_ISDIR(st.st_mode));
  EXPECT_EQ(-1, handler_->stat("/foo/cpu0_", &st));
  EXPECT_EQ(ENOENT, errno);
  EXPECT_EQ(-1, handler_->stat("/foo/cpu0/_", &st));
  EXPECT_EQ(ENOENT, errno);

  // |kNumConfigured| CPUs are available.
  EXPECT_EQ(0, handler_->stat("/foo/cpu1", &st));
  EXPECT_TRUE(S_ISDIR(st.st_mode));
  EXPECT_EQ(0, handler_->stat("/foo/cpu2", &st));
  EXPECT_TRUE(S_ISDIR(st.st_mode));
  EXPECT_EQ(0, handler_->stat("/foo/cpu3", &st));
  EXPECT_TRUE(S_ISDIR(st.st_mode));
  EXPECT_EQ(-1, handler_->stat("/foo/cpu4", &st));
  EXPECT_EQ(ENOENT, errno);
}

TEST_F(CpuFileHandlerTest, TestOpen) {
  EXPECT_TRUE(handler_->open(-1, "/foo", O_RDONLY, 0));
  EXPECT_TRUE(handler_->open(-1, "/foo/", O_RDONLY, 0));
  EXPECT_TRUE(!handler_->open(-1, "/foo/cpu", O_RDONLY, 0));
  EXPECT_EQ(ENOENT, errno);
  EXPECT_TRUE(handler_->open(-1, "/foo/cpu0", O_RDONLY, 0));
  EXPECT_TRUE(handler_->open(-1, "/foo/cpu0/", O_RDONLY, 0));
  EXPECT_TRUE(!handler_->open(-1, "/foo/cpu0_", O_RDONLY, 0));
  EXPECT_EQ(ENOENT, errno);
  EXPECT_TRUE(!handler_->open(-1, "/foo/cpu0/_", O_RDONLY, 0));
  EXPECT_EQ(ENOENT, errno);

  // |kNumConfigured| CPUs are available.
  EXPECT_TRUE(handler_->open(-1, "/foo/cpu1", O_RDONLY, 0));
  EXPECT_TRUE(handler_->open(-1, "/foo/cpu2", O_RDONLY, 0));
  EXPECT_TRUE(handler_->open(-1, "/foo/cpu3", O_RDONLY, 0));
  EXPECT_TRUE(!handler_->open(-1, "/foo/cpu4", O_RDONLY, 0));
  EXPECT_EQ(ENOENT, errno);
}

TEST_F(CpuFileHandlerTest, TestOpenWritable) {
  EXPECT_TRUE(!handler_->open(-1, "/foo", O_WRONLY, 0));
  EXPECT_EQ(EACCES, errno);
  EXPECT_TRUE(!handler_->open(-1, "/foo", O_RDWR, 0));
  EXPECT_EQ(EACCES, errno);
  EXPECT_TRUE(!handler_->open(-1, "/foo/cpu0", O_WRONLY, 0));
  EXPECT_EQ(EACCES, errno);
  EXPECT_TRUE(!handler_->open(-1, "/foo/cpu0", O_RDWR, 0));
  EXPECT_EQ(EACCES, errno);
}

TEST_F(CpuFileHandlerTest, TestStatFile) {
  struct stat st = {};
  for (size_t i = 0; i < arraysize(kFiles); ++i) {
    SCOPED_TRACE(kFiles[i]);
    EXPECT_EQ(0, handler_->stat("/foo/" + std::string(kFiles[i]), &st));
    EXPECT_TRUE(S_ISREG(st.st_mode));
    // Retry without the correct prefix, "/foo". This should fail.
    EXPECT_EQ(-1, handler_->stat("/" + std::string(kFiles[i]), &st));
    EXPECT_EQ(ENOENT, errno);
  }
}

TEST_F(CpuFileHandlerTest, TestOpenFile) {
  for (size_t i = 0; i < arraysize(kFiles); ++i) {
    SCOPED_TRACE(kFiles[i]);
    scoped_refptr<FileStream> stream = handler_->open(
        -1, "/foo/" + std::string(kFiles[i]), O_RDONLY, 0);
    ASSERT_TRUE(stream);
    // Confirm that mmap() is NOT suppored.
    EXPECT_EQ(MAP_FAILED, stream->mmap(NULL, 1, PROT_READ, MAP_PRIVATE, 0));
    EXPECT_EQ(EIO, errno);
    // Retry without the correct prefix, "/foo". This should fail.
    EXPECT_FALSE(handler_->open(-1, std::string(kFiles[i]), O_RDONLY, 0));
    EXPECT_EQ(ENOENT, errno);
  }
}

TEST_F(CpuFileHandlerTest, TestOpenFileWritable) {
  EXPECT_TRUE(!handler_->open(-1, "/foo/online", O_WRONLY, 0));
  EXPECT_EQ(EACCES, errno);
  EXPECT_TRUE(!handler_->open(-1, "/foo/online", O_RDWR, 0));
  EXPECT_EQ(EACCES, errno);
}

TEST_F(CpuFileHandlerTest, TestKernelMaxFile) {
  scoped_refptr<FileStream> stream =
      handler_->open(-1, "/foo/kernel_max", O_RDONLY, 0);
  ASSERT_TRUE(stream);
  char buf[128] = {};  // for easier \0 termination.
  ASSERT_EQ(3, stream->read(buf, sizeof(buf)));
  EXPECT_STREQ("63\n", buf);
}

TEST_F(CpuFileHandlerTest, TestOnline) {
  scoped_refptr<FileStream> stream =
      handler_->open(-1, "/foo/online", O_RDONLY, 0);
  ASSERT_TRUE(stream);
  char buf[128] = {};  // for easier \0 termination.
  EXPECT_LT(0, stream->read(buf, sizeof(buf)));
  EXPECT_EQ(base::StringPrintf("0-%d\n", kNumOnline - 1), buf);

  // Test the case where only one CPU is online.
  {
    ScopedNumProcessorsOnlineSetting num_online(1);
    memset(buf, 0, sizeof(buf));
    EXPECT_EQ(0, stream->lseek(0, SEEK_SET));
    EXPECT_LT(0, stream->read(buf, sizeof(buf)));
    EXPECT_STREQ("0\n", buf);
  }
}

TEST_F(CpuFileHandlerTest, TestOffline) {
  scoped_refptr<FileStream> stream =
      handler_->open(-1, "/foo/offline", O_RDONLY, 0);
  ASSERT_TRUE(stream);
  char buf[128] = {};  // for easier \0 termination.
  EXPECT_LT(0, stream->read(buf, sizeof(buf)));
  EXPECT_EQ(base::StringPrintf(
      "%d-%d\n", kNumOnline, kNumConfigured - 1), buf);

  // Test the case where all CPUs except one are online.
  {
    ScopedNumProcessorsOnlineSetting num_online(kNumConfigured - 1);
    memset(buf, 0, sizeof(buf));
    EXPECT_EQ(0, stream->lseek(0, SEEK_SET));
    EXPECT_LT(0, stream->read(buf, sizeof(buf)));
    EXPECT_EQ(base::StringPrintf("%d\n", kNumConfigured - 1), buf);
  }

  // Test the case where all CPUs are online.
  {
    ScopedNumProcessorsOnlineSetting num_online(kNumConfigured);
    memset(buf, 0, sizeof(buf));
    EXPECT_EQ(0, stream->lseek(0, SEEK_SET));
    EXPECT_LT(0, stream->read(buf, sizeof(buf)));
    EXPECT_STREQ("\n", buf);
  }
}

TEST_F(CpuFileHandlerTest, TestPossible) {
  scoped_refptr<FileStream> stream =
      handler_->open(-1, "/foo/possible", O_RDONLY, 0);
  ASSERT_TRUE(stream);
  char buf[128] = {};  // for easier \0 termination.
  EXPECT_LT(0, stream->read(buf, sizeof(buf)));
  EXPECT_EQ(base::StringPrintf("0-%d\n", kNumConfigured - 1), buf);
}

TEST_F(CpuFileHandlerTest, TestPresent) {
  scoped_refptr<FileStream> stream =
      handler_->open(-1, "/foo/present", O_RDONLY, 0);
  ASSERT_TRUE(stream);
  char buf[128] = {};  // for easier \0 termination.
  EXPECT_LT(0, stream->read(buf, sizeof(buf)));
  EXPECT_EQ(base::StringPrintf("0-%d\n", kNumConfigured - 1), buf);
}

TEST_F(CpuFileHandlerTest, TestDirectoryEntries) {
  scoped_ptr<Dir> dir(handler_->OnDirectoryContentsNeeded("/foo"));
  ASSERT_TRUE(dir.get());

  // Add 2 for "." and ".." entries.
  const int kNumDirectories = kNumConfigured + 2;
  const int kNumFiles = arraysize(kFiles);

  int num_directories_found = 0;
  int num_files_found = 0;
  while (true) {
    dirent ent;
    if (!dir->GetNext(&ent))
      break;
    if (ent.d_type == DT_DIR)
      ++num_directories_found;
    else if (ent.d_type == DT_REG)
      ++num_files_found;
  }

  EXPECT_EQ(kNumDirectories, num_directories_found);
  EXPECT_EQ(kNumFiles, num_files_found);
}

}  // namespace posix_translation
