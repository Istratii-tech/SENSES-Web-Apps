// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <pthread.h>
#include <stdlib.h>  // posix_memalign
#include <string.h>

#include <algorithm>
#include <set>

#include "base/compiler_specific.h"
#include "base/memory/scoped_ptr.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "posix_translation/address_util.h"
#include "posix_translation/dir.h"
#include "posix_translation/test_util/file_system_background_test_common.h"
#include "posix_translation/test_util/virtual_file_system_test_common.h"
#include "posix_translation/virtual_file_system.h"
#if !defined(ARC_WASM)
#include "ppapi_mocks/ppb_tcp_socket.h"
#include "ppapi_mocks/ppb_udp_socket.h"
#endif

using ::testing::NiceMock;

namespace posix_translation {

namespace {

// Mock-ish implementation of FileStream. The behaviors of IsSelectReadReady()
// etc. can be controled via the corresponding pre-set values
// (ex. is_select_read_ready_). mmap() and munmap() record passed parameters
// for verification purpose. mmap() also retuns a pre-set value (mapped_buf_),
// and munmap() checks that too.
class TestFileStream : public FileStream {
 public:
  TestFileStream()
      : FileStream(0, ""),
        is_select_read_ready_(false),
        is_select_write_ready_(false),
        is_select_exception_ready_(false),
        flags_value_(0),
        prot_value_(0),
        offset_value_(-1),
        length_value_(0),
        mapped_buf_(NULL),
        returns_same_address_for_multiple_mmaps_(false),
        is_munmap_called_(false) {
    EnableListenerSupport();
  }

  virtual bool ReturnsSameAddressForMultipleMmaps() const OVERRIDE {
    return returns_same_address_for_multiple_mmaps_;
  }

  virtual ssize_t read(void*, size_t) OVERRIDE { return -1; }
  virtual ssize_t write(const void*, size_t) OVERRIDE { return -1; }
  virtual const char* GetStreamType() const OVERRIDE { return "test"; }

  // Returns pre-set values.
  virtual bool IsSelectReadReady() const OVERRIDE {
    return is_select_read_ready_;
  }
  virtual bool IsSelectWriteReady() const OVERRIDE {
    return is_select_write_ready_;
  }
  virtual bool IsSelectExceptionReady() const OVERRIDE {
    return is_select_exception_ready_;
  }
  virtual int16_t GetPollEvents() const OVERRIDE {
    return ((IsSelectReadReady() ? POLLIN : 0) |
            (IsSelectWriteReady() ? POLLOUT : 0) |
            (IsSelectExceptionReady() ? POLLERR : 0));
  }

  // If MAP_FIXED is specified, returns |addr|. Otherwise, returns mapped_buf_
  // or fails if |addr| is non-NULL.
  virtual void* mmap(
      void* addr, size_t length, int prot, int flags, off_t offset) OVERRIDE {
    if (!(flags & MAP_FIXED) && (addr != NULL))
      return MAP_FAILED;

    length_value_ = length;
    prot_value_ = prot;
    flags_value_ = flags;
    offset_value_ = offset;
    if (flags & MAP_FIXED)
      mapped_buf_ = addr;
    return mapped_buf_;
  }

  // Fails if |addr| does not match mapped_buf_.
  virtual int munmap(void* addr, size_t length) OVERRIDE {
    is_munmap_called_ = true;
    if (addr != mapped_buf_)
      return -1;

    length_value_ = length;
    return 0;
  }


  bool is_select_read_ready_;
  bool is_select_write_ready_;
  bool is_select_exception_ready_;
  int flags_value_;
  int prot_value_;
  off64_t offset_value_;
  size_t length_value_;
  void* mapped_buf_;
  bool returns_same_address_for_multiple_mmaps_;
  bool is_munmap_called_;
};

// Stub-ish implementation of FileSystemHandler, that simply returns the
// stream given to the constructor when open() is called.
class TestFileSystemHandler : public FileSystemHandler {
 public:
  explicit TestFileSystemHandler(scoped_refptr<FileStream> stream)
      : FileSystemHandler("TestFileSystemHandler"),
        stream_(stream) {
  }

  virtual scoped_refptr<FileStream> open(int, const std::string&, int,
                                         mode_t) OVERRIDE {
    return stream_;
  }
  virtual Dir* OnDirectoryContentsNeeded(const std::string&) OVERRIDE {
    return NULL;
  }
  virtual int stat(const std::string&, struct stat*) OVERRIDE { return -1; }
  virtual int statfs(const std::string&, struct statfs*) OVERRIDE { return -1; }

 private:
  scoped_refptr<FileStream> stream_;
};

// A dummy file path used in tests.
const char kTestPath[] = "/test.file";

}  // namespace

// This class is used to test event-related functions such as epoll_*(),
// select(), poll(), select(), as well as some other miscellaneous functions
// in VirtualFileSystem.
//
// See also other tests files for VirtualFileSystem:
// - virtual_file_system_path_test.cc (path-related functions)
// - virtual_file_system_stream_test.cc (stream-related functions)
// - virtual_file_system_host_resolver_test.cc (host resolution)
class FileSystemTest : public FileSystemBackgroundTestCommon<FileSystemTest> {
 public:
  // These tests all are fairly unique and there is no real common setup.
  DECLARE_BACKGROUND_TEST(TestDup);
  DECLARE_BACKGROUND_TEST(TestDupInvalid);
  DECLARE_BACKGROUND_TEST(TestEPollBasic);
  DECLARE_BACKGROUND_TEST(TestEPollClose);
  DECLARE_BACKGROUND_TEST(TestEPollErrorHandling);
  DECLARE_BACKGROUND_TEST(TestEPollSuccess);
  DECLARE_BACKGROUND_TEST(TestEPollUnexpectedCalls);
  DECLARE_BACKGROUND_TEST(TestGetNameInfo);
  DECLARE_BACKGROUND_TEST(TestMmap);
  DECLARE_BACKGROUND_TEST(TestInvalidMmap);
  DECLARE_BACKGROUND_TEST(TestMmapWithMemoryFile);
  DECLARE_BACKGROUND_TEST(TestAnonymousMmap);
  DECLARE_BACKGROUND_TEST(TestNoMunmap);
  DECLARE_BACKGROUND_TEST(TestPipe);
  DECLARE_BACKGROUND_TEST(TestPoll);
  DECLARE_BACKGROUND_TEST(TestSelect);
  DECLARE_BACKGROUND_TEST(TestSocket);
  DECLARE_BACKGROUND_TEST(TestSocketpair);

 protected:
  int GetOpenFD(int flags);

  virtual void SetUp() OVERRIDE {
    FileSystemBackgroundTestCommon<FileSystemTest>::SetUp();
#if !defined(ARC_WASM)
    factory_.GetMock(&ppb_tcpsocket_);
    factory_.GetMock(&ppb_udpsocket_);
#endif
  }

#if !defined(ARC_WASM)
 private:
  // TCPSocket and UDPSocket are used in TestSocket implicitly.
  // So, declare NiceMock here to inject them.
  // (arc-wasm: the network-less TCPSocket/UDPSocket need no Pepper mocks.)
  NiceMock<PPB_TCPSocket_Mock>* ppb_tcpsocket_;
  NiceMock<PPB_UDPSocket_Mock>* ppb_udpsocket_;
#endif
};

TEST_F(FileSystemTest, ConstructPendingDestruct) {
  // Just tests that the initialization that runs in SetUp() itself
  // succeeds.
}

int FileSystemTest::GetOpenFD(int open_flags) {
  // |stream| is deleted when it is closed or when VirtualFileSystem is
  // destructed.
  TestFileSystemHandler handler(new TestFileStream);
  AddMountPoint(kTestPath, &handler);

  int fd = file_system_->open(kTestPath, open_flags, 0);
  EXPECT_GE(fd, 0) << "Open failed";
  ClearMountPoints();
  return fd;
}

TEST_F(FileSystemTest, TestGetInode) {
  base::AutoLock lock(mutex());

  ino_t inode = GetInode(kTestPath);
  EXPECT_GT(inode, 0U);
  ino_t another = GetInode("/some/other/path");
  EXPECT_GT(another, 0U);
  EXPECT_NE(inode, another);
  EXPECT_EQ(inode, GetInode(kTestPath));
  RemoveInode(kTestPath);
  // The same inode should not be reused.
  EXPECT_NE(inode, GetInode(kTestPath));
}

TEST_F(FileSystemTest, TestReassignInode) {
  base::AutoLock lock(mutex());

  ino_t inode = GetInode(kTestPath);
  EXPECT_GT(inode, 0U);
  ReassignInode(kTestPath, "/some/other/path");
  EXPECT_EQ(inode, GetInode("/some/other/path"));
  ino_t another = GetInode(kTestPath);
  EXPECT_NE(inode, another);
  EXPECT_GT(another, 0U);

  // Test the case where the inode for the old path has not been generated yet.
  inode = GetInode("/some/other/path/2");
  EXPECT_GT(inode, 0U);
  ReassignInode("/does/not/have/inode/yet", "/some/other/path/2");
  another = GetInode("/some/other/path/2");
  EXPECT_NE(inode, another);
  EXPECT_GT(another, 0U);
}

TEST_F(FileSystemTest, TestGetFirstUnusedDescriptor) {
  int fd = GetFirstUnusedDescriptor();
  EXPECT_GE(fd, 0);
  EXPECT_EQ(fd + 1, GetFirstUnusedDescriptor());
  EXPECT_EQ(fd + 2, GetFirstUnusedDescriptor());

  // Test if the smallest one available is returned.
  RemoveFileStream(fd + 1);
  EXPECT_EQ(fd + 1, GetFirstUnusedDescriptor());

  RemoveFileStream(fd + 2);
  EXPECT_EQ(fd + 2, GetFirstUnusedDescriptor());

  RemoveFileStream(fd);
  EXPECT_EQ(fd, GetFirstUnusedDescriptor());

  RemoveFileStream(fd + 1);
  RemoveFileStream(fd + 2);
  EXPECT_EQ(fd + 1, GetFirstUnusedDescriptor());
  EXPECT_EQ(fd + 2, GetFirstUnusedDescriptor());
}

TEST_F(FileSystemTest, TestNumOfDescriptorsAvailable) {
  // 1023 descriptors should be available.
  static const size_t kNum = kMaxFdForTesting - kMinFdForTesting + 1;
  for (size_t i = 0; i < kNum; ++i) {
    EXPECT_GE(GetFirstUnusedDescriptor(), 0) << i;
  }
  EXPECT_EQ(-1, GetFirstUnusedDescriptor());
}

TEST_F(FileSystemTest, TestTooManyDescriptors) {
  bool failed = false;
  for (size_t i = 0; i < FD_SETSIZE; ++i) {
    if (GetFirstUnusedDescriptor() < 0) {
      failed = true;
      break;
    }
  }
  EXPECT_TRUE(failed);
}

TEST_F(FileSystemTest, TestGetCurrentWorkingDirectory) {
  const char kRootDirPath[] = "/";

  EXPECT_EQ(0, file_system_->chdir(kRootDirPath));

  scoped_ptr<char, base::FreeDeleter> scoped_result;
  scoped_result.reset(file_system_->getcwd(NULL, 0));
  EXPECT_TRUE(scoped_result.get());
  EXPECT_STREQ(kRootDirPath, scoped_result.get());

  // Size should be 0 or correct value for NULL.
  scoped_result.reset(file_system_->getcwd(NULL, 1));
  EXPECT_FALSE(scoped_result.get());
  EXPECT_EQ(ERANGE, errno);
  scoped_result.reset(file_system_->getcwd(NULL, 100));
  EXPECT_TRUE(scoped_result.get());

  char buf[2];
  char* result = file_system_->getcwd(buf, 2);
  EXPECT_EQ(buf, result);
  EXPECT_STREQ(kRootDirPath, result);

  // Size argument can not be 0.
  result = file_system_->getcwd(buf, 0);
  EXPECT_EQ(EINVAL, errno);
  EXPECT_EQ(NULL, result);

  // Buffer size 1 is too small.
  result = file_system_->getcwd(buf, 1);
  EXPECT_EQ(ERANGE, errno);
  EXPECT_EQ(NULL, result);

  // Too large buffer size.
  result = file_system_->getcwd(NULL, static_cast<size_t>(-1));
  EXPECT_EQ(ENOMEM, errno);
  EXPECT_EQ(NULL, result);
}

TEST_F(FileSystemTest, TestGetNormalizedPath) {
  base::AutoLock lock(mutex());
  const VirtualFileSystem::NormalizeOption kOption =
      VirtualFileSystem::kDoNotResolveSymlinks;

  EXPECT_EQ("/", GetNormalizedPath("/", kOption));
  EXPECT_EQ("/", GetNormalizedPath("//", kOption));
  EXPECT_EQ("/", GetNormalizedPath("///", kOption));
  EXPECT_EQ("/path/to/foo", GetNormalizedPath("/path/to/./foo", kOption));
  EXPECT_EQ("/path/to/foo", GetNormalizedPath("/path/to/././foo", kOption));
  EXPECT_EQ("/path/to/foo", GetNormalizedPath("/path/to/./././foo", kOption));
  EXPECT_EQ("/path/to/foo", GetNormalizedPath("./path/to/./foo", kOption));
  EXPECT_EQ("/path/to/foo", GetNormalizedPath("././path/to/./foo", kOption));
  EXPECT_EQ("/path/to/foo", GetNormalizedPath("/path/to/foo/.", kOption));
  EXPECT_EQ("/path/to/foo", GetNormalizedPath("/path/to/foo/./.", kOption));
  EXPECT_EQ("/path/to/foo", GetNormalizedPath("/path/to/foo/././.", kOption));
  EXPECT_EQ("/path/to/foo",
            GetNormalizedPath("//././path/to/./foo/./.", kOption));
  EXPECT_EQ("/path/to/foo",
            GetNormalizedPath("/././path/to/./foo/./.", kOption));
  EXPECT_EQ("/.dot_file", GetNormalizedPath("/.dot_file", kOption));
  EXPECT_EQ("/path/to/.dot_file",
            GetNormalizedPath("/path/to/.dot_file", kOption));
  EXPECT_EQ("/ends_with_dot.", GetNormalizedPath("/ends_with_dot.", kOption));
  EXPECT_EQ("/ends_with_dot.", GetNormalizedPath("/ends_with_dot./", kOption));
  EXPECT_EQ("/ends_with_dot./a",
            GetNormalizedPath("/ends_with_dot./a", kOption));
  EXPECT_EQ("/", GetNormalizedPath(".", kOption));
  EXPECT_EQ("/", GetNormalizedPath("./", kOption));
  EXPECT_EQ("/", GetNormalizedPath(".//", kOption));
  EXPECT_EQ("/", GetNormalizedPath("./.", kOption));
  EXPECT_EQ("/", GetNormalizedPath("././", kOption));
  EXPECT_EQ("/", GetNormalizedPath("././/", kOption));
  EXPECT_EQ("", GetNormalizedPath("", kOption));
  EXPECT_EQ("/", GetNormalizedPath("../", kOption));
  EXPECT_EQ("/", GetNormalizedPath("foo/../", kOption));
  EXPECT_EQ("/bar", GetNormalizedPath("foo/../bar", kOption));

  EXPECT_EQ("/twodots/something",
            GetNormalizedPath("/twodots/with/../something", kOption));
  EXPECT_EQ("/twodots/something",
            GetNormalizedPath("/twodots/with/../something/", kOption));
  EXPECT_EQ("/something",
            GetNormalizedPath("/twodots/with/../../something", kOption));
  EXPECT_EQ("/something",
            GetNormalizedPath("/twodots/with/../../something/", kOption));
  EXPECT_EQ("/something",
            GetNormalizedPath("/twodots/with/../../../something", kOption));
  EXPECT_EQ("/something",
            GetNormalizedPath("/twodots/with/../../../something/", kOption));
  EXPECT_EQ("/", GetNormalizedPath("/twodots/with/../..", kOption));
  EXPECT_EQ("/", GetNormalizedPath("/twodots/with/../../", kOption));
  EXPECT_EQ("/", GetNormalizedPath("/twodots/with/../../../", kOption));
  EXPECT_EQ("/relative", GetNormalizedPath("twodots/../relative/", kOption));
  EXPECT_EQ("/", GetNormalizedPath("/..", kOption));
  EXPECT_EQ("/", GetNormalizedPath("/../", kOption));
  EXPECT_EQ("/a", GetNormalizedPath("/../a", kOption));
  EXPECT_EQ("/a", GetNormalizedPath("/../a/", kOption));
  EXPECT_EQ("/", GetNormalizedPath("/../..", kOption));
}

TEST_BACKGROUND_F(FileSystemTest, TestDup) {
  std::set<int> fds_used;
  int fd = GetFirstUnusedDescriptor();
  EXPECT_GE(fd, 0);
  fds_used.insert(fd);

  scoped_refptr<TestFileStream> stream = new TestFileStream;
  AddFileStream(fd, stream);

  // Must be able to use every file descriptor in pool.
  static const size_t kNum = kMaxFdForTesting - kMinFdForTesting;
  for (size_t i = 0; i < kNum; ++i) {
    errno = 0;
    int fd_dup = file_system_->dup(fd);
    EXPECT_EQ(errno, 0) << i;
    EXPECT_GE(fd_dup, 0) << i;
    // Validate that we generated unique id.
    EXPECT_EQ(fds_used.count(fd_dup), 0) << i;
    EXPECT_TRUE(fds_used.insert(fd_dup).second) << i;
    // Validate that duplicate points to the same stream.
    scoped_refptr<TestFileStream> test_stream =
        static_cast<TestFileStream*>(GetStream(fd_dup).get());
    EXPECT_EQ(stream, test_stream) << i;
  }
  // No more file descriptor in pool. Error is returned.
  errno = 0;
  int fd_dup = file_system_->dup(fd);
  EXPECT_EQ(errno, EMFILE);
  EXPECT_EQ(fd_dup, -1);
}

TEST_BACKGROUND_F(FileSystemTest, TestDupInvalid) {
  // Duplicating invalid file descriptor returns -1 and sets EBADF error. Must
  // be able to process more calls than file descriptor pool size.
  static const size_t kNum = kMaxFdForTesting - kMinFdForTesting + 2;
  for (size_t i = 0; i < kNum; ++i) {
    errno = 0;
    int fd_dup = file_system_->dup(-1);
    EXPECT_EQ(errno, EBADF) << i;
    EXPECT_EQ(fd_dup, -1) << i;
  }
}

TEST_BACKGROUND_F(FileSystemTest, TestEPollBasic) {
  int fd1;
  int ep_fd1;
  struct epoll_event ev1 = {};

  // Simple create/close
  ep_fd1 = file_system_->epoll_create1(0);
  EXPECT_GE(ep_fd1, 0);
  EXPECT_EQ(0, file_system_->close(ep_fd1));

  // Simple create, add file, close epoll, close file
  fd1 = GetOpenFD(O_RDWR | O_CREAT);
  EXPECT_GE(fd1, 0);
  ep_fd1 = file_system_->epoll_create1(0);
  EXPECT_GE(ep_fd1, 0);
  ev1.events = EPOLLIN;
  ev1.data.fd = fd1;
  EXPECT_EQ(0, file_system_->epoll_ctl(ep_fd1, EPOLL_CTL_ADD, fd1, &ev1));
  EXPECT_EQ(0, file_system_->close(ep_fd1));
  EXPECT_EQ(0, file_system_->close(fd1));

  // Simple create, add file, close file, close epoll
  fd1 = GetOpenFD(O_RDWR | O_CREAT);
  EXPECT_GE(fd1, 0);
  ep_fd1 = file_system_->epoll_create1(0);
  EXPECT_GE(ep_fd1, 0);
  ev1.events = EPOLLIN;
  ev1.data.fd = fd1;
  EXPECT_EQ(0, file_system_->epoll_ctl(ep_fd1, EPOLL_CTL_ADD, fd1, &ev1));
  EXPECT_EQ(0, file_system_->close(fd1));
  EXPECT_EQ(0, file_system_->close(ep_fd1));
}

TEST_BACKGROUND_F(FileSystemTest, TestEPollErrorHandling) {
  int fd1, fd2;
  int ep_fd1;
  struct epoll_event ev1 = {};
  struct epoll_event ev2 = {};

  // Verify error handling of epoll_ctl.
  fd1 = GetOpenFD(O_RDWR | O_CREAT);
  EXPECT_GE(fd1, 0);
  ep_fd1 = file_system_->epoll_create1(0);
  EXPECT_GE(ep_fd1, 0);
  ev1.events = EPOLLIN;
  ev1.data.fd = fd1;
  EXPECT_ERROR(
      file_system_->epoll_ctl(ep_fd1, EPOLL_CTL_DEL, fd1, &ev1), ENOENT);
  EXPECT_ERROR(
      file_system_->epoll_ctl(ep_fd1, EPOLL_CTL_MOD, fd1, &ev1), ENOENT);
  EXPECT_ERROR(
      file_system_->epoll_ctl(ep_fd1, EPOLL_CTL_ADD, ep_fd1, &ev1), EINVAL);
  EXPECT_EQ(0, file_system_->close(fd1));
  EXPECT_ERROR(
      file_system_->epoll_ctl(ep_fd1, EPOLL_CTL_ADD, fd1, &ev1), EBADF);
  EXPECT_ERROR(
      file_system_->epoll_ctl(fd1, EPOLL_CTL_ADD, ep_fd1, &ev1), EBADF);
  fd1 = GetOpenFD(O_RDWR | O_CREAT);
  EXPECT_GE(fd1, 0);
  ev1.events = EPOLLIN;
  ev1.data.fd = fd1;
  EXPECT_EQ(0, file_system_->epoll_ctl(ep_fd1, EPOLL_CTL_ADD, fd1, &ev1));
  EXPECT_ERROR(
      file_system_->epoll_ctl(ep_fd1, EPOLL_CTL_ADD, fd1, &ev1), EEXIST);
  EXPECT_EQ(0, file_system_->epoll_ctl(ep_fd1, EPOLL_CTL_DEL, fd1, &ev1));
  EXPECT_ERROR(
      file_system_->epoll_ctl(ep_fd1, EPOLL_CTL_DEL, fd1, &ev1), ENOENT);
  EXPECT_EQ(0, file_system_->close(fd1));
  EXPECT_EQ(0, file_system_->close(ep_fd1));

  // Verify passing in bogus fd to as epoll fd
  fd1 = GetOpenFD(O_RDWR | O_CREAT);
  EXPECT_GE(fd1, 0);
  fd2 = GetOpenFD(O_RDWR | O_CREAT);
  EXPECT_GE(fd2, 0);
  ev1.events = EPOLLIN;
  ev1.data.fd = fd2;
  EXPECT_ERROR(
      file_system_->epoll_ctl(fd1, EPOLL_CTL_ADD, fd2, &ev1), EINVAL);
  EXPECT_ERROR(file_system_->epoll_wait(fd1, &ev2, 1, 0), EINVAL);
  EXPECT_EQ(0, file_system_->close(fd1));
  EXPECT_EQ(0, file_system_->close(fd2));
}

TEST_BACKGROUND_F(FileSystemTest, TestEPollUnexpectedCalls) {
  int ep_fd = file_system_->epoll_create1(0);
  EXPECT_GE(ep_fd, 0);

  char buf[1];
  EXPECT_ERROR(file_system_->read(ep_fd, buf, 1), EINVAL);
  EXPECT_ERROR(file_system_->write(ep_fd, buf, 1), EINVAL);

  EXPECT_EQ(0, file_system_->close(ep_fd));
}

TEST_BACKGROUND_F(FileSystemTest, TestEPollClose) {
  int fd1, fd2, fd3;
  int ep_fd1, ep_fd2;
  struct epoll_event ev1 = {};
  struct epoll_event ev2 = {};

  // More complex testing of close ordering - close epolls first
  ep_fd1 = file_system_->epoll_create1(0);
  EXPECT_GE(ep_fd1, 0);
  ep_fd2 = file_system_->epoll_create1(0);
  EXPECT_GE(ep_fd2, 0);
  fd1 = GetOpenFD(O_RDWR | O_CREAT);
  EXPECT_GE(fd1, 0);
  fd2 = GetOpenFD(O_RDWR | O_CREAT);
  EXPECT_GE(fd2, 0);
  fd3 = GetOpenFD(O_RDWR | O_CREAT);
  EXPECT_GE(fd3, 0);
  ev1.events = EPOLLIN;
  ev1.data.fd = fd1;
  EXPECT_EQ(0, file_system_->epoll_ctl(ep_fd1, EPOLL_CTL_ADD, fd1, &ev1));
  EXPECT_EQ(0, file_system_->epoll_ctl(ep_fd2, EPOLL_CTL_ADD, fd1, &ev1));
  ev1.data.fd = fd2;
  EXPECT_EQ(0, file_system_->epoll_ctl(ep_fd1, EPOLL_CTL_ADD, fd2, &ev1));
  ev1.data.fd = fd3;
  EXPECT_EQ(0, file_system_->epoll_ctl(ep_fd2, EPOLL_CTL_ADD, fd3, &ev1));
  EXPECT_EQ(0, file_system_->close(ep_fd1));
  EXPECT_EQ(0, file_system_->close(ep_fd2));
  EXPECT_EQ(0, file_system_->close(fd1));
  EXPECT_EQ(0, file_system_->close(fd2));
  EXPECT_EQ(0, file_system_->close(fd3));

  // More complex testing of close ordering - close one file first
  ep_fd1 = file_system_->epoll_create1(0);
  EXPECT_GE(ep_fd1, 0);
  ep_fd2 = file_system_->epoll_create1(0);
  EXPECT_GE(ep_fd2, 0);
  fd1 = GetOpenFD(O_RDWR | O_CREAT);
  EXPECT_GE(fd1, 0);
  fd2 = GetOpenFD(O_RDWR | O_CREAT);
  EXPECT_GE(fd2, 0);
  fd3 = GetOpenFD(O_RDWR | O_CREAT);
  EXPECT_GE(fd3, 0);
  ev1.events = EPOLLIN;
  ev1.data.fd = fd1;
  EXPECT_EQ(0, file_system_->epoll_ctl(ep_fd1, EPOLL_CTL_ADD, fd1, &ev1));
  EXPECT_EQ(0, file_system_->epoll_ctl(ep_fd2, EPOLL_CTL_ADD, fd1, &ev1));
  ev1.data.fd = fd2;
  EXPECT_EQ(0, file_system_->epoll_ctl(ep_fd1, EPOLL_CTL_ADD, fd2, &ev1));
  ev1.data.fd = fd3;
  EXPECT_EQ(0, file_system_->epoll_ctl(ep_fd2, EPOLL_CTL_ADD, fd3, &ev1));
  EXPECT_EQ(0, file_system_->close(fd1));
  EXPECT_ERROR(
      file_system_->epoll_ctl(ep_fd1, EPOLL_CTL_DEL, fd1, &ev1), EBADF);
  EXPECT_ERROR(
      file_system_->epoll_ctl(ep_fd1, EPOLL_CTL_MOD, fd1, &ev1), EBADF);
  EXPECT_ERROR(
      file_system_->epoll_ctl(ep_fd2, EPOLL_CTL_DEL, fd1, &ev1), EBADF);
  EXPECT_EQ(0, file_system_->close(ep_fd1));
  EXPECT_EQ(0, file_system_->close(ep_fd2));
  EXPECT_ERROR(
      file_system_->epoll_ctl(ep_fd1, EPOLL_CTL_MOD, fd2, &ev1), EBADF);
  EXPECT_EQ(0, file_system_->close(fd2));
  EXPECT_EQ(0, file_system_->close(fd3));

  // Simple create, wait, close
  ep_fd1 = file_system_->epoll_create1(0);
  EXPECT_GE(ep_fd1, 0);
  EXPECT_EQ(0, file_system_->epoll_wait(ep_fd1, &ev2, 1, 0));
  EXPECT_EQ(0, file_system_->close(ep_fd1));

  // Simple create, wait, close
  ep_fd1 = file_system_->epoll_create1(0);
  EXPECT_GE(ep_fd1, 0);
  EXPECT_EQ(0, file_system_->epoll_wait(ep_fd1, &ev2, 1, 50));
  EXPECT_EQ(0, file_system_->close(ep_fd1));
}

TEST_BACKGROUND_F(FileSystemTest, TestEPollSuccess) {
  int fd1;
  int ep_fd1;
  struct epoll_event ev1 = {};
  struct epoll_event ev2 = {};

  // Test a successful epoll
  ep_fd1 = file_system_->epoll_create1(0);
  EXPECT_GE(ep_fd1, 0);
  fd1 = GetOpenFD(O_RDWR | O_CREAT);
  scoped_refptr<TestFileStream> stream1 =
      static_cast<TestFileStream*>(GetStream(fd1).get());
  stream1->is_select_read_ready_ = true;
  stream1->is_select_write_ready_ = true;

  EXPECT_GE(fd1, 0);
  ev1.events = EPOLLIN | EPOLLOUT;
  ev1.data.fd = fd1;
  EXPECT_EQ(0, file_system_->epoll_ctl(ep_fd1, EPOLL_CTL_ADD, fd1, &ev1));
  memset(&ev2, 0xA5, sizeof(ev2));
  EXPECT_EQ(1, file_system_->epoll_wait(ep_fd1, &ev2, 1, 50));
  EXPECT_EQ(ev2.events, (uint32_t)(EPOLLIN | EPOLLOUT));
  EXPECT_EQ(ev2.data.fd, fd1);
  EXPECT_EQ(0, file_system_->close(fd1));
  EXPECT_EQ(0, file_system_->close(ep_fd1));

  // Test successful epoll after modding event data
  ep_fd1 = file_system_->epoll_create1(0);
  EXPECT_GE(ep_fd1, 0);
  fd1 = GetOpenFD(O_RDWR | O_CREAT);
  stream1 = static_cast<TestFileStream*>(GetStream(fd1).get());
  stream1->is_select_read_ready_ = true;
  stream1->is_select_write_ready_ = true;

  EXPECT_GE(fd1, 0);
  ev1.events = EPOLLIN | EPOLLOUT;
  ev1.data.fd = fd1;
  EXPECT_EQ(0, file_system_->epoll_ctl(ep_fd1, EPOLL_CTL_ADD, fd1, &ev1));
  memset(&ev2, 0xA5, sizeof(ev2));
  EXPECT_EQ(1, file_system_->epoll_wait(ep_fd1, &ev2, 1, 50));
  EXPECT_EQ(ev2.events, (uint32_t)(EPOLLIN | EPOLLOUT));
  EXPECT_EQ(ev2.data.fd, fd1);
  ev1.events = EPOLLIN | EPOLLOUT;
  ev1.data.fd = -fd1;
  EXPECT_EQ(0, file_system_->epoll_ctl(ep_fd1, EPOLL_CTL_MOD, fd1, &ev1));
  memset(&ev2, 0x5A, sizeof(ev2));
  EXPECT_EQ(1, file_system_->epoll_wait(ep_fd1, &ev2, 1, 50));
  EXPECT_EQ(ev2.events, (uint32_t)(EPOLLIN | EPOLLOUT));
  EXPECT_EQ(ev2.data.fd, -fd1);
  EXPECT_EQ(0, file_system_->epoll_ctl(ep_fd1, EPOLL_CTL_DEL, fd1, &ev1));
  EXPECT_EQ(0, file_system_->epoll_wait(ep_fd1, &ev2, 1, 0));
  EXPECT_EQ(0, file_system_->close(fd1));
  EXPECT_EQ(0, file_system_->close(ep_fd1));

  // Test double-close
  EXPECT_ERROR(file_system_->close(ep_fd1), EBADF);
}

TEST_BACKGROUND_F(FileSystemTest, TestPipe) {
  int pipefd[2];
  int dupfd;
  char writebuffer[100];
  char readbuffer[100];
  EXPECT_EQ(0, file_system_->pipe2(pipefd, O_NONBLOCK));
  EXPECT_GE(pipefd[0], 0);
  EXPECT_GE(pipefd[1], 0);
  dupfd = file_system_->dup(pipefd[1]);
  EXPECT_GE(dupfd, 0);
  memset(writebuffer, 0x55, sizeof(writebuffer));
  memset(readbuffer, 0xAA, sizeof(readbuffer));
  EXPECT_ERROR(file_system_->write(pipefd[0], writebuffer, 100), EBADF);
  EXPECT_ERROR(file_system_->read(pipefd[1], readbuffer, 100), EBADF);
  EXPECT_ERROR(file_system_->read(pipefd[0], readbuffer, 100), EAGAIN);
  EXPECT_EQ(file_system_->write(pipefd[1], writebuffer, 100), 100);
  EXPECT_EQ(file_system_->read(pipefd[0], readbuffer, 100), 100);
  EXPECT_EQ(memcmp(writebuffer, readbuffer, sizeof(writebuffer)), 0);
  for (size_t iii = 0; iii < sizeof(writebuffer); iii++)
    writebuffer[iii] = static_cast<char>(iii);
  EXPECT_EQ(file_system_->write(pipefd[1], writebuffer, 50), 50);
  EXPECT_EQ(file_system_->read(pipefd[0], readbuffer, 100), 50);
  EXPECT_EQ(memcmp(writebuffer, readbuffer, 50), 0);

  EXPECT_EQ(file_system_->write(pipefd[1], writebuffer, 100), 100);
  EXPECT_EQ(file_system_->read(pipefd[0], readbuffer, 50), 50);
  EXPECT_EQ(file_system_->read(pipefd[0], readbuffer, 50), 50);
  EXPECT_EQ(memcmp(writebuffer+50, readbuffer, 50), 0);

  // Close the original write end of the pipe, but a duplicated end exists.
  EXPECT_EQ(0, file_system_->close(pipefd[1]));

  memset(readbuffer, 0xAA, sizeof(readbuffer));
  EXPECT_EQ(file_system_->write(dupfd, writebuffer, 50), 50);
  EXPECT_EQ(file_system_->read(pipefd[0], readbuffer, 50), 50);
  EXPECT_EQ(memcmp(writebuffer, readbuffer, 50), 0);

  EXPECT_EQ(0, file_system_->close(pipefd[0]));
  EXPECT_EQ(0, file_system_->close(dupfd));
}

TEST_BACKGROUND_F(FileSystemTest, TestSocketpair) {
  int sockets[2];
  char writebuffer[5000];
  char readbuffer[5000];
  int iii;
  EXPECT_EQ(0, file_system_->socketpair(AF_UNIX, SOCK_STREAM, 0, sockets));
  EXPECT_GE(sockets[0], 0);
  EXPECT_GE(sockets[1], 0);

  for (iii = 0; iii < 5000; iii++ )
    writebuffer[iii] = static_cast<char>(iii * 3);
  EXPECT_EQ(file_system_->write(sockets[0], writebuffer, 5000), 5000);
  EXPECT_EQ(file_system_->read(sockets[1], readbuffer, 3000), 3000);
  EXPECT_EQ(memcmp(writebuffer, readbuffer, 3000), 0);
  EXPECT_EQ(file_system_->read(sockets[1], readbuffer, 3000), 2000);
  EXPECT_EQ(memcmp(writebuffer + 3000, readbuffer, 2000), 0);
  for (iii = 0; iii < 5000; iii++)
    writebuffer[iii] = writebuffer[iii] ^ static_cast<char>(iii*5);
  EXPECT_EQ(file_system_->write(sockets[0], writebuffer, 5000), 5000);
  EXPECT_EQ(file_system_->read(sockets[1], readbuffer, 5000), 5000);
  EXPECT_EQ(memcmp(writebuffer, readbuffer, 5000), 0);

  memset(readbuffer, 0, sizeof(readbuffer));
  EXPECT_EQ(file_system_->write(sockets[0], writebuffer, 100), 100);
  EXPECT_EQ(file_system_->read(sockets[1], readbuffer, 5000), 100);
  EXPECT_EQ(memcmp(writebuffer, readbuffer, 100), 0);
  EXPECT_EQ(readbuffer[100], 0);

  EXPECT_EQ(0, file_system_->close(sockets[0]));
  EXPECT_EQ(0, file_system_->close(sockets[1]));
}

TEST_BACKGROUND_F(FileSystemTest, TestPoll) {
  struct pollfd fds[3] = {};

  fds[0].fd = GetFirstUnusedDescriptor();
  fds[0].events = POLLIN | POLLPRI | POLLOUT | POLLRDHUP;

  fds[1].fd = GetFirstUnusedDescriptor();
  fds[1].events = POLLIN | POLLPRI | POLLOUT | POLLRDHUP;

  fds[2].fd = GetFirstUnusedDescriptor();
  fds[2].events = POLLIN | POLLPRI | POLLOUT | POLLRDHUP;

  scoped_refptr<TestFileStream> stream0 = new TestFileStream;
  AddFileStream(fds[0].fd, stream0);

  scoped_refptr<TestFileStream> stream1 = new TestFileStream;
  AddFileStream(fds[1].fd, stream1);

  stream0->is_select_read_ready_ = false;
  stream0->is_select_write_ready_ = false;
  stream0->is_select_exception_ready_ = false;

  stream1->is_select_read_ready_ = true;
  stream1->is_select_write_ready_ = true;
  stream1->is_select_exception_ready_ = true;

  // Check a non-blocking call with one non-signaling fd, one completely
  // signaling fd, and one unknown fd.
  errno = 0;
  EXPECT_EQ(2, file_system_->poll(fds, sizeof(fds)/sizeof(fds[0]), 0));
  EXPECT_EQ(0, errno);
  EXPECT_EQ(0, fds[0].revents);
  EXPECT_EQ(POLLIN | POLLOUT | POLLERR, fds[1].revents);
  EXPECT_EQ(POLLNVAL, fds[2].revents);

  scoped_refptr<TestFileStream> stream2 = new TestFileStream;
  AddFileStream(fds[2].fd, stream2);

  stream0->is_select_read_ready_ = true;
  stream0->is_select_write_ready_ = false;
  stream0->is_select_exception_ready_ = false;

  stream1->is_select_read_ready_ = false;
  stream1->is_select_write_ready_ = true;
  stream1->is_select_exception_ready_ = false;

  stream2->is_select_read_ready_ = false;
  stream2->is_select_write_ready_ = false;
  stream2->is_select_exception_ready_ = true;

  // Check a very-short blocking timeout blocking call where the fds are
  // each distinctly signalling.
  errno = 0;
  EXPECT_EQ(3, file_system_->poll(fds, sizeof(fds)/sizeof(fds[0]), 1));
  EXPECT_EQ(0, errno);
  EXPECT_EQ(POLLIN, fds[0].revents);
  EXPECT_EQ(POLLOUT, fds[1].revents);
  EXPECT_EQ(POLLERR, fds[2].revents);

  stream0->is_select_read_ready_ = false;
  stream0->is_select_write_ready_ = false;
  stream0->is_select_exception_ready_ = false;

  stream1->is_select_read_ready_ = false;
  stream1->is_select_write_ready_ = false;
  stream1->is_select_exception_ready_ = false;

  stream2->is_select_read_ready_ = false;
  stream2->is_select_write_ready_ = false;
  stream2->is_select_exception_ready_ = false;

  // Check a non-blocking call where all fds are non-signaling.
  errno = 0;
  EXPECT_EQ(0, file_system_->poll(fds, sizeof(fds)/sizeof(fds[0]), 0));
  EXPECT_EQ(0, errno);
  EXPECT_EQ(0, fds[0].revents);
  EXPECT_EQ(0, fds[1].revents);
  EXPECT_EQ(0, fds[2].revents);
}

TEST_BACKGROUND_F(FileSystemTest, TestSelect) {
  fd_set readfds;
  fd_set writefds;
  fd_set exceptfds;
  struct timeval timeout = {};

  int fd0 = GetFirstUnusedDescriptor();
  scoped_refptr<TestFileStream> stream0 = new TestFileStream;
  AddFileStream(fd0, stream0);

  int fd1 = GetFirstUnusedDescriptor();
  scoped_refptr<TestFileStream> stream1 = new TestFileStream;
  AddFileStream(fd1, stream1);

  int fd2 = GetFirstUnusedDescriptor();
  scoped_refptr<TestFileStream> stream2 = new TestFileStream;
  AddFileStream(fd2, stream2);

  int fd3 = GetFirstUnusedDescriptor();
  scoped_refptr<TestFileStream> stream3 = new TestFileStream;
  AddFileStream(fd3, stream3);

  int nfds = 1 + std::max(std::max(fd0, fd1), std::max(fd2, fd3));

  FD_ZERO(&readfds);
  FD_ZERO(&writefds);
  FD_ZERO(&exceptfds);
  FD_SET(fd0, &readfds);
  FD_SET(fd1, &readfds);
  FD_SET(fd2, &writefds);
  FD_SET(fd3, &exceptfds);

  // Expect fd0 will never be ready, but fd1-fd3 to be immediately ready.
  stream0->is_select_read_ready_ = false;
  stream1->is_select_read_ready_ = true;
  stream2->is_select_write_ready_ = true;
  stream3->is_select_exception_ready_ = true;

  // Issue a non-blocking call with the four fds.
  errno = 0;
  EXPECT_EQ(3, file_system_->select(nfds, &readfds, &writefds, &exceptfds,
                                    &timeout));
  EXPECT_EQ(0, errno);
  EXPECT_FALSE(FD_ISSET(fd0, &readfds));
  EXPECT_TRUE(FD_ISSET(fd1, &readfds));
  EXPECT_TRUE(FD_ISSET(fd2, &writefds));
  EXPECT_TRUE(FD_ISSET(fd3, &exceptfds));

  // Issue a super-short blocking call with no fds.
  memset(&timeout, 0, sizeof(timeout));
  timeout.tv_usec = 1;
  EXPECT_EQ(0, file_system_->select(0, NULL, NULL, NULL, &timeout));
  // |timeout| should be updated.
  EXPECT_EQ(0L, timeout.tv_sec);
  EXPECT_EQ(0L, timeout.tv_usec);
}

TEST_BACKGROUND_F(FileSystemTest, TestMmap) {
  size_t length = util::GetPageSize();
  // Use non-default values, to verify that TestFileStream::mmap() is called
  // with these values, via VirtualFileSystem::mmap().
  const int prot = 123;
  const int flags = 456 & ~MAP_FIXED;
  const off64_t offset = 0;

  const int fd = GetFirstUnusedDescriptor();
  scoped_refptr<TestFileStream> stream = new TestFileStream;
  AddFileStream(fd, stream);

  void* mapped_buf;
  ASSERT_EQ(0, posix_memalign(&mapped_buf, util::GetPageSize(), length));
  stream->mapped_buf_ = mapped_buf;

  errno = 0;
  void* retval = file_system_->mmap(NULL, length, prot, flags, fd, offset);
  EXPECT_EQ(mapped_buf, retval);
  EXPECT_EQ(0, errno);
  EXPECT_EQ(length, stream->length_value_);
  EXPECT_EQ(prot, stream->prot_value_);
  EXPECT_EQ(flags, stream->flags_value_);
  EXPECT_EQ(offset, stream->offset_value_);

  errno = 0;
  EXPECT_EQ(0, file_system_->munmap(retval, length));
  EXPECT_EQ(0, errno);
  EXPECT_EQ(length, stream->length_value_);

  free(mapped_buf);
}

TEST_BACKGROUND_F(FileSystemTest, TestInvalidMmap) {
  const int fd = GetFirstUnusedDescriptor();
  scoped_refptr<TestFileStream> stream = new TestFileStream;
  AddFileStream(fd, stream);

  const uintptr_t aligned_addr = util::GetPageSize();
  const uintptr_t unaligned_addr = aligned_addr + 1;

  // Test mmap with unaligned address.
  errno = 0;
  EXPECT_EQ(MAP_FAILED, file_system_->mmap(
      reinterpret_cast<void*>(unaligned_addr), 1, PROT_WRITE,
      MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, -1, 0));
  EXPECT_EQ(EINVAL, errno);

  // Test mprotect with unaligned address.
  errno = 0;
  EXPECT_EQ(-1, file_system_->mprotect(
      reinterpret_cast<void*>(unaligned_addr), 1, PROT_READ));
  EXPECT_EQ(EINVAL, errno);

  // Test munmap with unaligned address.
  errno = 0;
  EXPECT_EQ(-1, file_system_->munmap(
      reinterpret_cast<void*>(unaligned_addr), 1));
  EXPECT_EQ(EINVAL, errno);

  // Test zero-length mmap.
  errno = 0;
  EXPECT_EQ(MAP_FAILED, file_system_->mmap(
      NULL, 0, PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0));
  EXPECT_EQ(EINVAL, errno);

  // Note: zero-length mprotect is legal.

  // Test zero-length munmap.
  errno = 0;
  EXPECT_EQ(-1, file_system_->munmap(
      reinterpret_cast<void*>(aligned_addr), 0));
  EXPECT_EQ(EINVAL, errno);

  // Test mmap with unaligned offset.
  errno = 0;
  EXPECT_EQ(MAP_FAILED, file_system_->mmap(
      NULL, 1, PROT_READ, MAP_PRIVATE, fd, 1));
  EXPECT_EQ(EINVAL, errno);
}

TEST_BACKGROUND_F(FileSystemTest, TestMmapWithMemoryFile) {
  const size_t length = util::GetPageSize();
  const int prot = PROT_READ | PROT_WRITE;
  const int flags = MAP_PRIVATE;
  const off64_t offset = 0;

  const int fd = GetFirstUnusedDescriptor();
  scoped_refptr<TestFileStream> stream = new TestFileStream;
  AddFileStream(fd, stream);

  // Mimic MemoryFile which returns the same address for multiple mmap() calls.
  // It should work though the behavior is not posix compliant.
  stream->returns_same_address_for_multiple_mmaps_ = true;

  void* mapped_buf;
  ASSERT_EQ(0, posix_memalign(&mapped_buf, util::GetPageSize(), length));
  stream->mapped_buf_ = mapped_buf;

  // Note that the reference count for a region bound to |fd| becomes 3.
  EXPECT_EQ(mapped_buf,
            file_system_->mmap(NULL, length, prot, flags, fd, offset));
  EXPECT_EQ(mapped_buf,
            file_system_->mmap(NULL, length, prot, flags, fd, offset));
  EXPECT_EQ(mapped_buf,
            file_system_->mmap(NULL, length, prot, flags, fd, offset));

  // It should not be replaced with another MemoryFileStream.
  // In that case, how to handle the reference count is not trivial.
  SetMemoryMapAbortEnableFlags(false);
  // Following failure decreases the reference count to 2 internally.
  errno = 0;
  EXPECT_EQ(MAP_FAILED, file_system_->mmap(
      mapped_buf, length, prot, flags | MAP_FIXED, fd, offset));
  EXPECT_EQ(ENODEV, errno);
  SetMemoryMapAbortEnableFlags(true);

  // It should not be replaced with another kind of FileStream, too.
  const int another_fd = GetFirstUnusedDescriptor();
  scoped_refptr<TestFileStream> another_stream = new TestFileStream;
  AddFileStream(another_fd, another_stream);
  SetMemoryMapAbortEnableFlags(false);
  // Following failure decreases the reference count to 1 internally.
  errno = 0;
  EXPECT_EQ(MAP_FAILED, file_system_->mmap(
      mapped_buf, length, prot, flags | MAP_FIXED, another_fd, offset));
  EXPECT_EQ(ENODEV, errno);
  SetMemoryMapAbortEnableFlags(true);
  EXPECT_EQ(0, file_system_->munmap(mapped_buf, length));

  // On the other hands, MemoryFile with single reference can be replaced with
  // another MemoryFileStream.
  EXPECT_EQ(mapped_buf, file_system_->mmap(
      NULL, length, prot, flags, fd, offset));
  EXPECT_EQ(mapped_buf, file_system_->mmap(
      mapped_buf, length, prot, flags | MAP_FIXED, fd, offset));
  EXPECT_EQ(0, file_system_->munmap(mapped_buf, length));

  free(mapped_buf);
}

TEST_BACKGROUND_F(FileSystemTest, TestAnonymousMmap) {
  const size_t length = util::GetPageSize();
  const size_t doubled_length = length * 2;
  const int prot = PROT_READ;
  const int anonymous_fd = -1;
  const off64_t offset = 0;

  // Call mmap() with MAP_ANONYMOUS. It should ignore |fd| and not call
  // underlaying mmap() implementation, but real mmap().
  errno = 0;
  char* anonymous_addr = static_cast<char*>(file_system_->mmap(
      NULL, doubled_length, prot, MAP_ANONYMOUS | MAP_PRIVATE, anonymous_fd,
      offset));
  EXPECT_NE(MAP_FAILED, anonymous_addr);
  EXPECT_NE(static_cast<char*>(NULL), anonymous_addr);
  EXPECT_EQ(0, errno);

  // Call mmap() with MAP_FIXED and |fd|. The address is the same with
  // previously allocated anonymous region.
  const int fd = GetFirstUnusedDescriptor();
  scoped_refptr<TestFileStream> stream = new TestFileStream;
  AddFileStream(fd, stream);
  errno = 0;
  void* retval = file_system_->mmap(
      anonymous_addr, doubled_length, prot, MAP_FIXED | MAP_PRIVATE, fd,
      offset);
  EXPECT_EQ(anonymous_addr, retval);
  EXPECT_EQ(0, errno);

  // Call mmap() with MAP_FIXED and MAP_ANONYMOUS. It should not call underlying
  // munmap() implementation to release previously allocated memory region.
  errno = 0;
  retval = file_system_->mmap(
      anonymous_addr, doubled_length, prot,
      MAP_FIXED | MAP_ANONYMOUS | MAP_PRIVATE, anonymous_fd, offset);
  EXPECT_EQ(anonymous_addr, retval);
  EXPECT_FALSE(stream->is_munmap_called_);
  EXPECT_EQ(0, errno);

  // Confirm that mprotect is supported. Note that zero-length mprotect should
  // return 0.
  EXPECT_EQ(0, file_system_->mprotect(retval, 0, PROT_READ));
  ASSERT_EQ(0, file_system_->mprotect(retval, 1, PROT_WRITE));
  static_cast<char*>(retval)[0] = 'X';  // confirm this does not crash.
  ASSERT_EQ(0, file_system_->mprotect(retval, doubled_length, prot));

  // munmap() can be called partially.
  EXPECT_EQ(0, file_system_->munmap(anonymous_addr, length));
  char* latter_half_addr = anonymous_addr + length;
  EXPECT_EQ(0, file_system_->munmap(latter_half_addr, length));
  EXPECT_EQ(0, errno);
}

TEST_BACKGROUND_F(FileSystemTest, TestNoMunmap) {
  size_t length = util::GetPageSize();
  // Use non-default values, to verify that TestFileStream::munmap() is
  // called with these values, via VirtualFileSystem::munmap().
  int prot = 123;
  int flags = 456;
  off64_t offset = 0;

  int fd = GetFirstUnusedDescriptor();
  scoped_refptr<TestFileStream> stream = new TestFileStream;
  AddFileStream(fd, stream);

  void* mapped_buf;
  ASSERT_EQ(0, posix_memalign(&mapped_buf, util::GetPageSize(), length));
  stream->mapped_buf_ = mapped_buf;

  errno = 0;
  void* retval = file_system_->mmap(NULL, length, prot, flags, fd, offset);
  EXPECT_EQ(mapped_buf, retval);
  EXPECT_EQ(0, errno);
  EXPECT_EQ(length, stream->length_value_);
  EXPECT_EQ(prot, stream->prot_value_);
  EXPECT_EQ(flags, stream->flags_value_);
  EXPECT_EQ(offset, stream->offset_value_);

  file_system_->close(fd);

  free(mapped_buf);
}

TEST_BACKGROUND_F(FileSystemTest, TestGetNameInfo) {
  sockaddr_in sin = {};
  char host[1024] = {};
  char serv[1024] = {};
  const int flags = 0;
  sin.sin_family = AF_INET;
  sin.sin_port = htons(80);
  sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  int retval;
  // Normal, just hostname
  retval = file_system_->getnameinfo(reinterpret_cast<sockaddr*>(&sin),
                                     sizeof(sin), host, sizeof(host),
                                     NULL, 0, 0);
  EXPECT_EQ(0, retval);
  EXPECT_STREQ("127.0.0.1", host);

  // Normal, just servname
  retval = file_system_->getnameinfo(reinterpret_cast<sockaddr*>(&sin),
                                     sizeof(sin), NULL, 0, serv,
                                     sizeof(serv), 0);
  EXPECT_EQ(0, retval);
  EXPECT_STREQ("80", serv);

  // Invalid request -- either hostname or servname must be requested.
  retval = file_system_->getnameinfo(reinterpret_cast<sockaddr*>(&sin),
                                     sizeof(sin), NULL, 0, NULL, 0, 0);
  EXPECT_EQ(EAI_NONAME, retval);

  // Unsupported
  sockaddr_in unsupported = {};
  unsupported.sin_family = AF_BRIDGE;
  retval = file_system_->getnameinfo(reinterpret_cast<sockaddr*>(&unsupported),
                                     sizeof(unsupported), host, sizeof(host),
                                     serv, sizeof(serv), flags);
  EXPECT_EQ(EAI_FAMILY, retval);
}

TEST_BACKGROUND_F(FileSystemTest, TestSocket) {
  int fd = 0;

  errno = -1;
  fd = file_system_->socket(AF_INET, SOCK_DGRAM, PF_INET);
  EXPECT_NE(-1, fd);
  EXPECT_EQ(-1, errno);
  EXPECT_STREQ("udp", GetStream(fd)->GetStreamType());
  EXPECT_EQ(0, file_system_->close(fd));

  errno = -1;
  fd = file_system_->socket(AF_INET6, SOCK_STREAM, PF_INET6);
  EXPECT_NE(-1, fd);
  EXPECT_EQ(-1, errno);
  EXPECT_STREQ("tcp", GetStream(fd)->GetStreamType());
  EXPECT_EQ(0, file_system_->close(fd));
  errno = -1;

  EXPECT_ERROR(file_system_->socket(AF_INET, SOCK_RAW, PF_INET), EAFNOSUPPORT);
  EXPECT_ERROR(file_system_->socket(AF_INET, SOCK_RDM, PF_INET), EAFNOSUPPORT);
  EXPECT_ERROR(
      file_system_->socket(AF_INET, SOCK_SEQPACKET, PF_INET), EAFNOSUPPORT);
  EXPECT_ERROR(
      file_system_->socket(AF_INET, SOCK_PACKET, PF_INET), EAFNOSUPPORT);
  EXPECT_ERROR(
      file_system_->socket(AF_BRIDGE, SOCK_DGRAM, PF_BRIDGE), EAFNOSUPPORT);
}

}  // namespace posix_translation
