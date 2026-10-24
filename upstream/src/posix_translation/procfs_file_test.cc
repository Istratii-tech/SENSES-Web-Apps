// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <time.h>
#include <unistd.h>

#include "base/basictypes.h"
#include "base/memory/scoped_ptr.h"
#include "base/strings/stringprintf.h"
#include "common/process_emulator.h"
#include "common/tests/option_test_helper.h"
#include "gtest/gtest.h"
#include "posix_translation/mount_point_manager.h"
#include "posix_translation/procfs_file.h"
#include "posix_translation/test_util/file_system_test_common.h"
#include "posix_translation/test_util/stub_file_system_handler.h"
#include "posix_translation/test_util/sysconf_util.h"

namespace posix_translation {

namespace {

const char kHeader[] = "HHH";
const char kBody[] = "!$1!";
const char kFooter[] = "FFF";

// The number of physical/online CPUs in this test. See also:
// procfs_file_test.cc.
static int kNumConfigured = 4;
static int kNumOnline = 2;

}  // namespace

class ProcfsHandlerTest : public FileSystemTestCommon {
 protected:
  ProcfsHandlerTest()
      : num_configured_(kNumConfigured), num_online_(kNumOnline),
        handler_(new ProcfsFileHandler(NULL)) {
    handler_->SetCpuInfoFileTemplate(kHeader, kBody, kFooter);
    arc::PopulateOptionsForTest();
    arc::ProcessEmulator::ResetForTest();
    arc::ProcessEmulator::AddProcessForTest(201, 1000, "proc_201_1000");
    arc::ProcessEmulator::AddProcessForTest(202, 10001, "proc_202_10001");
  }

  ScopedNumProcessorsConfiguredSetting num_configured_;
  ScopedNumProcessorsOnlineSetting num_online_;
  scoped_ptr<ProcfsFileHandler> handler_;

 private:
  DISALLOW_COPY_AND_ASSIGN(ProcfsHandlerTest);
};

TEST_F(ProcfsHandlerTest, TestInit) {
}

TEST_F(ProcfsHandlerTest, TestStatCpuinfoFile) {
  struct stat st;
  EXPECT_EQ(0, handler_->stat("/proc/cpuinfo", &st));
  EXPECT_TRUE(S_ISREG(st.st_mode));

  // Confirm that stat() always fills the current time in st_mtime.
  EXPECT_NE(0, static_cast<time_t>(st.st_mtime));
  EXPECT_NEAR(time(NULL), st.st_mtime, 60.0 /* seconds */);

  EXPECT_EQ(-1, handler_->stat("/proc/cpuinf", &st));
  EXPECT_EQ(ENOENT, errno);
  EXPECT_EQ(-1, handler_->stat("/proc/cpuinf0", &st));
  EXPECT_EQ(ENOENT, errno);
}

TEST_F(ProcfsHandlerTest, TestOpenCpuinfoFileFile) {
  scoped_refptr<FileStream> stream =
      handler_->open(-1, "/proc/cpuinfo", O_RDONLY, 0);
  ASSERT_TRUE(stream);
  // Confirm that mmap() is NOT suppored.
  EXPECT_EQ(MAP_FAILED,
            stream->mmap(NULL, 1, PROT_READ, MAP_PRIVATE, 0));
  EXPECT_EQ(EIO, errno);

  EXPECT_FALSE(handler_->open(-1, "/proc/cpuinf", O_RDONLY, 0));
  EXPECT_EQ(ENOENT, errno);
  EXPECT_FALSE(handler_->open(-1, "/proc/cpuinf0", O_RDONLY, 0));
  EXPECT_EQ(ENOENT, errno);
}

TEST_F(ProcfsHandlerTest, TestReadCpuinfoFile) {
  scoped_refptr<FileStream> stream =
      handler_->open(-1, "/proc/cpuinfo", O_RDONLY, 0);
  ASSERT_TRUE(stream);
  char buf[128] = {};  // for easier \0 termination.
  EXPECT_LT(0, stream->read(buf, sizeof(buf)));
  EXPECT_STREQ("HHH!0!!1!FFF", buf);

  // Test the case where only one CPU is online.
  {
    ScopedNumProcessorsOnlineSetting num_online(1);
    memset(buf, 0, sizeof(buf));
    EXPECT_EQ(0, stream->lseek(0, SEEK_SET));
    EXPECT_LT(0, stream->read(buf, sizeof(buf)));
    EXPECT_STREQ("HHH!0!FFF", buf);
  }
}

TEST_F(ProcfsHandlerTest, TestFstatCpuinfoFile) {
  scoped_refptr<FileStream> stream =
      handler_->open(-1, "/proc/cpuinfo", O_RDONLY, 0);
  ASSERT_TRUE(stream);

  struct stat st;
  EXPECT_EQ(0, stream->fstat(&st));
  EXPECT_TRUE(S_ISREG(st.st_mode));

  // Confirm that fstat() always fills the current time in st_mtime too.
  EXPECT_NE(0, static_cast<time_t>(st.st_mtime));
  EXPECT_NEAR(time(NULL), st.st_mtime, 60.0 /* seconds */);
}

TEST_F(ProcfsHandlerTest, TestParsePidBasedPathMalformed) {
  std::string post_pid;
  pid_t pid;
  const char* bad_paths[] = {
    "",
    "/proc",
    "/proc/a/status",
    "/proc/1a/status",
    "/proc/100"
  };
  for (int i = 0; i < sizeof(bad_paths) / sizeof(bad_paths[0]); ++i)
    ASSERT_FALSE(handler_->ParsePidBasedPath(bad_paths[i], &pid, &post_pid));
}

TEST_F(ProcfsHandlerTest, TestParsePidBasedPathValid) {
  struct {
    const char* path;
    pid_t pid;
    const char* post_pid;
  } good_paths[] = {
    { "/proc/1/status", 1, "/status" },
    { "/proc/1/stat", 1, "/stat" },
    { "/proc/2/mem", 2, "/mem" },
    { "/proc/10/cmdline", 10, "/cmdline" },
    { "/proc/52339/cmdline", 52339, "/cmdline" }
  };
  for (int i = 0; i < sizeof(good_paths) / sizeof(good_paths[0]); ++i) {
    pid_t pid;
    std::string post_pid;
    ASSERT_TRUE(handler_->ParsePidBasedPath(good_paths[i].path, &pid,
                                            &post_pid));
    ASSERT_EQ(good_paths[i].pid, pid);
    ASSERT_STREQ(good_paths[i].post_pid, post_pid.c_str());
  }
}

TEST_F(ProcfsHandlerTest, TestStatFileContents) {
  scoped_refptr<FileStream> stream = handler_->open(-1, "/proc/201/stat",
                                                    O_RDONLY, 0);
  char buf[128] = {};  // for easier \0 termination.
  EXPECT_LT(0, stream->read(buf, sizeof(buf)));
  EXPECT_STREQ(
    "201 (proc_201_1000) R 201 201 201 0 201 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 "
    "0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n", buf);
}

TEST_F(ProcfsHandlerTest, TestStatusFileContents) {
  scoped_refptr<FileStream> stream = handler_->open(-1, "/proc/202/status",
                                                    O_RDONLY, 0);
  char buf[128] = {};  // for easier \0 termination.
  char* null = NULL;
  EXPECT_LT(0, stream->read(buf, sizeof(buf)));
  EXPECT_NE(null, strstr(buf, "Name:   proc_202_10001\n"));
  EXPECT_NE(null, strstr(buf, "Pid:    202\n"));
  EXPECT_NE(null, strstr(buf, "Uid:    10001 "));
}

TEST_F(ProcfsHandlerTest, TestCmdlineFileContents) {
  scoped_refptr<FileStream> stream = handler_->open(-1, "/proc/201/cmdline",
                                                    O_RDONLY, 0);
  char buf[128] = {};
  // The value read must be proc_201_1000\0.
  EXPECT_EQ(14, stream->read(buf, sizeof(buf)));
  EXPECT_STREQ("proc_201_1000", buf);
}

TEST_F(ProcfsHandlerTest, TestMountsFileContentsWhenNoMountPointManager) {
  scoped_refptr<FileStream> stream = handler_->open(-1, "/proc/201/mounts",
                                                    O_RDONLY, 0);
  char buf[128] = {};
  // Without passing a mount point manager, the mounts file is empty.
  EXPECT_EQ(0, stream->read(buf, sizeof(buf)));
}

TEST_F(ProcfsHandlerTest, TestMountsFileContentsWhenMountPointManagerIsPassed) {
  MountPointManager mount_points;
  StubFileSystemHandler stub_file_system_handler;

  mount_points.Add("/path/to/file", &stub_file_system_handler);
  mount_points.ChangeOwner("/path/to/file", 100);
  mount_points.Add("/path/to/dir/", &stub_file_system_handler);
  mount_points.ChangeOwner("/path/to/dir/", 1000);
  handler_->SetMountPointManager(&mount_points);

  scoped_refptr<FileStream> stream = handler_->open(-1, "/proc/201/mounts",
                                                    O_RDONLY, 0);
  char buf[256] = {};  // for easier \0 termination.
  EXPECT_LT(0, stream->read(buf, sizeof(buf)));
  const std::string& file_system_name = stub_file_system_handler.name();
  EXPECT_TRUE(strstr(buf, base::StringPrintf(
      "none /path/to/file %s uid=100,single_file 0 0\n",
      file_system_name.c_str()).c_str()));
  EXPECT_TRUE(strstr(buf, base::StringPrintf(
      "none /path/to/dir %s uid=1000 0 0\n",
      file_system_name.c_str()).c_str()));
  handler_->SetMountPointManager(NULL);
}

}  // namespace posix_translation
