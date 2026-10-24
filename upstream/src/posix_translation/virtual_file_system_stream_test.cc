// Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string.h>

#include "base/compiler_specific.h"
#include "base/memory/scoped_ptr.h"
#include "gtest/gtest.h"
#include "posix_translation/dir.h"
#include "posix_translation/test_util/file_system_background_test_common.h"
#include "posix_translation/test_util/virtual_file_system_test_common.h"
#include "posix_translation/virtual_file_system.h"

namespace posix_translation {

namespace {

// A stub/mock/fake-ish implementation of FileStream. Most functions simply
// record input parameters for verification purpose, and return
// constants. Some functions such as read() and write() have simple logic to
// provide fake read-write behaviors using the internal buffer (content_).
class TestFileStream : public FileStream {
 public:
  TestFileStream()
      : FileStream(0, ""),
        dirent_ptr_(NULL),
        sockaddr_ptr_(NULL),
        socklen_ptr_(NULL),
        optval_ptr_(NULL),
        optlen_ptr_(NULL),
        backlog_value_(0),
        flags_value_(0),
        level_value_(0),
        optname_value_(0),
        request_value_(0),
        whence_value_(0),
        offset_value_(0),
        dirent_count_value_(0),
        socklen_value_(0) {
  }

  virtual const char* GetStreamType() const OVERRIDE { return "test"; }

  virtual int accept(sockaddr* addr, socklen_t* addrlen) OVERRIDE {
    sockaddr_ptr_ = addr;
    socklen_ptr_ = addrlen;
    return kFd;
  }

  virtual int bind(const sockaddr* addr, socklen_t addrlen) OVERRIDE {
    sockaddr_ptr_ = addr;
    socklen_value_ = addrlen;
    return kFd;
  }

  virtual int connect(const sockaddr* addr, socklen_t addrlen) OVERRIDE {
    sockaddr_ptr_ = addr;
    socklen_value_ = addrlen;
    return kFd;
  }

  virtual int getdents(dirent* buf, size_t count) OVERRIDE {
    dirent_ptr_ = buf;
    dirent_count_value_ = count;
    return kFd;
  }

  virtual int getsockname(sockaddr* name, socklen_t* namelen) OVERRIDE {
    sockaddr_ptr_ = name;
    socklen_ptr_ = namelen;
    return kFd;
  }

  virtual int getsockopt(int level, int optname, void* optval,
                         socklen_t* optlen) OVERRIDE {
    level_value_ = level;
    optname_value_ = optname;
    optval_ptr_ = optval;
    optlen_ptr_ = optlen;
    return kFd;
  }

  virtual int ioctl(int request, va_list ap) OVERRIDE {
    request_value_ = request;
    return kFd;
  }

  virtual int listen(int backlog) OVERRIDE {
    backlog_value_ = backlog;
    return kFd;
  }

  virtual off64_t lseek(off64_t offset, int whence) OVERRIDE {
    offset_value_ = offset;
    whence_value_ = whence;
    return kFd;
  }

  virtual ssize_t pread(void* buf, size_t count, off64_t offset) OVERRIDE {
    ALOG_ASSERT(offset < content_.size());
    size_t length = std::min(count,
                             static_cast<size_t>(content_.size() - offset));
    memcpy(buf, content_.data() + offset, length);
    return length;
  }

  virtual ssize_t PwriteImpl(
      const void* buf, size_t count, off64_t offset) OVERRIDE {
    ALOG_ASSERT(offset < content_.size());
    content_.replace(offset, count, static_cast<const char*>(buf));
    return count;
  }

  virtual ssize_t read(void* buf, size_t count) OVERRIDE {
    size_t length = std::min(count, static_cast<size_t>(content_.size()));
    memcpy(buf, content_.data(), length);
    return length;
  }

  virtual ssize_t recv(void* buf, size_t count, int flags) OVERRIDE {
    flags_value_ = flags;
    return read(buf, count);
  }

  virtual ssize_t recvfrom(void* buf, size_t count, int flags,
                           sockaddr* addr, socklen_t* addrlen) OVERRIDE {
    flags_value_ = flags;
    sockaddr_ptr_ = addr;
    socklen_ptr_ = addrlen;
    return read(buf, count);
  }

  virtual ssize_t send(const void* buf, size_t count, int flags) OVERRIDE {
    flags_value_ = flags;
    return write(buf, count);
  }

  virtual ssize_t sendto(const void* buf, size_t count, int flags,
                         const sockaddr* dest_addr,
                         socklen_t addrlen) OVERRIDE {
    flags_value_ = flags;
    sockaddr_ptr_ = dest_addr;
    socklen_value_ = addrlen;
    return write(buf, count);
  }

  virtual int setsockopt(int level, int optname, const void* optval,
                         socklen_t optlen) OVERRIDE {
    level_value_ = level;
    optname_value_ = optname;
    optval_ptr_ = optval;
    socklen_value_ = optlen;
    return 0;
  }

  virtual ssize_t write(const void* buf, size_t count) OVERRIDE {
    content_.assign(static_cast<const char*>(buf), count);
    return count;
  }

  // The file descriptor number that the class returns.
  static const int kFd = 12345;

  const dirent* dirent_ptr_;
  const sockaddr* sockaddr_ptr_;
  const socklen_t* socklen_ptr_;
  const void* optval_ptr_;
  const socklen_t* optlen_ptr_;
  int backlog_value_;
  int flags_value_;
  int level_value_;
  int optname_value_;
  int request_value_;
  int whence_value_;
  off64_t offset_value_;
  size_t dirent_count_value_;
  socklen_t socklen_value_;

  // The content used for read(), write(), etc.
  std::string content_;
};

const int TestFileStream::kFd;

}  // namespace

// This class is used to test stream-related functions in VirtualFileSystem,
// such as read(), write(), getdents(), etc.
//
// Most tests in the class just verify that the functions in TestFileStream
// are called with expected parameters via VirtualFileSystem, and not called
// when an invalid file descripter is passed.
//
// Tests for read(), write(), and friends verify that the buffer in
// TestFileStream (content_) are modified as expected.
class FileSystemStreamTest
    : public FileSystemBackgroundTestCommon<FileSystemStreamTest> {
 public:
  DECLARE_BACKGROUND_TEST(TestAccept);
  DECLARE_BACKGROUND_TEST(TestBind);
  DECLARE_BACKGROUND_TEST(TestConnect);
  DECLARE_BACKGROUND_TEST(TestGetDents);
  DECLARE_BACKGROUND_TEST(TestGetSockName);
  DECLARE_BACKGROUND_TEST(TestGetSockOpt);
  DECLARE_BACKGROUND_TEST(TestIOCtl);
  DECLARE_BACKGROUND_TEST(TestListen);
  DECLARE_BACKGROUND_TEST(TestLSeek);
  DECLARE_BACKGROUND_TEST(TestPRead);
  DECLARE_BACKGROUND_TEST(TestPWrite);
  DECLARE_BACKGROUND_TEST(TestRead);
  DECLARE_BACKGROUND_TEST(TestReadV);
  DECLARE_BACKGROUND_TEST(TestRecv);
  DECLARE_BACKGROUND_TEST(TestRecvFrom);
  DECLARE_BACKGROUND_TEST(TestSend);
  DECLARE_BACKGROUND_TEST(TestSendTo);
  DECLARE_BACKGROUND_TEST(TestSetSockOpt);
  DECLARE_BACKGROUND_TEST(TestShutdown);
  DECLARE_BACKGROUND_TEST(TestWrite);
  DECLARE_BACKGROUND_TEST(TestWriteV);

 protected:
  typedef FileSystemBackgroundTestCommon<FileSystemStreamTest> CommonType;

  virtual void SetUp() OVERRIDE {
    CommonType::SetUp();

    fd_ = GetFirstUnusedDescriptor();
    EXPECT_GE(fd_, 0);
    stream_ = new TestFileStream();
    AddFileStream(fd_, stream_);
  }

  int fd_;
  scoped_refptr<TestFileStream> stream_;
};

TEST_BACKGROUND_F(FileSystemStreamTest, TestAccept) {
  sockaddr addr = {};
  socklen_t addrlen = 1;

  // Normal call
  errno = 0;
  EXPECT_EQ(TestFileStream::kFd,
            file_system_->accept(fd_, &addr, &addrlen));
  EXPECT_EQ(0, errno);
  EXPECT_EQ(&addr, stream_->sockaddr_ptr_);
  EXPECT_EQ(&addrlen, stream_->socklen_ptr_);

  // Bad sockfd
  EXPECT_ERROR(file_system_->accept(0, &addr, &addrlen), EBADF);
}

TEST_BACKGROUND_F(FileSystemStreamTest, TestBind) {
  sockaddr addr = {};
  socklen_t addrlen = 1;

  // Normal call.
  errno = 0;
  EXPECT_EQ(TestFileStream::kFd,
            file_system_->bind(fd_, &addr, addrlen));
  EXPECT_EQ(0, errno);
  EXPECT_EQ(&addr, stream_->sockaddr_ptr_);
  EXPECT_EQ(addrlen, stream_->socklen_value_);

  // Bad sockfd
  EXPECT_ERROR(file_system_->bind(0, &addr, addrlen), EBADF);
}

TEST_BACKGROUND_F(FileSystemStreamTest, TestConnect) {
  sockaddr addr = {};
  socklen_t addrlen = 1;

  // Normal call
  errno = 0;
  EXPECT_EQ(TestFileStream::kFd,
            file_system_->connect(fd_, &addr, addrlen));
  EXPECT_EQ(0, errno);
  EXPECT_EQ(&addr, stream_->sockaddr_ptr_);
  EXPECT_EQ(addrlen, stream_->socklen_value_);

  // Bad sockfd
  EXPECT_ERROR(file_system_->connect(0, &addr, addrlen), EBADF);
}

TEST_BACKGROUND_F(FileSystemStreamTest, TestGetDents) {
  dirent buf;
  size_t count = 123;

  // Normal call
  errno = 0;
  EXPECT_EQ(TestFileStream::kFd,
            file_system_->getdents(fd_, &buf, count));
  EXPECT_EQ(0, errno);
  EXPECT_EQ(&buf, stream_->dirent_ptr_);
  EXPECT_EQ(count, stream_->dirent_count_value_);

  // Bad fd
  EXPECT_ERROR(file_system_->getdents(0, &buf, count), EBADF);
}

TEST_BACKGROUND_F(FileSystemStreamTest, TestGetSockName) {
  sockaddr name;
  socklen_t namelen;

  // Normal call
  errno = 0;
  EXPECT_EQ(TestFileStream::kFd,
            file_system_->getsockname(fd_, &name, &namelen));
  EXPECT_EQ(0, errno);
  EXPECT_EQ(&name, stream_->sockaddr_ptr_);
  EXPECT_EQ(&namelen, stream_->socklen_ptr_);

  // Bad sockfd
  EXPECT_ERROR(file_system_->getsockname(0, &name, &namelen), EBADF);
}

TEST_BACKGROUND_F(FileSystemStreamTest, TestGetSockOpt) {
  int level = 123;
  int optname = 456;
  char optval[1024];
  socklen_t optlen = 987;

  // Normal call
  errno = 0;
  EXPECT_EQ(TestFileStream::kFd,
            file_system_->getsockopt(fd_, level, optname, &optval, &optlen));
  EXPECT_EQ(0, errno);
  EXPECT_EQ(level, stream_->level_value_);
  EXPECT_EQ(optname, stream_->optname_value_);
  EXPECT_EQ(&optval, stream_->optval_ptr_);
  EXPECT_EQ(&optlen, stream_->optlen_ptr_);

  // Bad sockfd
  EXPECT_ERROR(
      file_system_->getsockopt(0, level, optname, &optval, &optlen), EBADF);
}

TEST_BACKGROUND_F(FileSystemStreamTest, TestIOCtl) {
  const int request = 0x5301;  // CDROMPAUSE (takes an empty va_list)
  va_list ap;
  memset(&ap, 0, sizeof(ap));

  // Normal call
  errno = 0;
  EXPECT_EQ(TestFileStream::kFd, file_system_->ioctl(fd_, request, ap));
  EXPECT_EQ(0, errno);
  EXPECT_EQ(request, stream_->request_value_);

  // Bad fd
  EXPECT_ERROR(file_system_->ioctl(0, request, ap), EBADF);
}

TEST_BACKGROUND_F(FileSystemStreamTest, TestListen) {
  const int backlog = 123;

  // Normal call.
  errno = 0;
  EXPECT_EQ(TestFileStream::kFd, file_system_->listen(fd_, backlog));
  EXPECT_EQ(0, errno);

  // Bad sockfd.
  EXPECT_ERROR(file_system_->listen(0, backlog), EBADF);
}

TEST_BACKGROUND_F(FileSystemStreamTest, TestLSeek) {
  const off64_t offset = 123;
  const int whence = 456;

  // Normal call
  errno = 0;
  EXPECT_EQ(TestFileStream::kFd,
            file_system_->lseek(fd_, offset, whence));
  EXPECT_EQ(0, errno);
  EXPECT_EQ(offset, stream_->offset_value_);
  EXPECT_EQ(whence, stream_->whence_value_);

  // Bad fd
  EXPECT_ERROR(file_system_->lseek(0, offset, whence), EBADF);
}

TEST_BACKGROUND_F(FileSystemStreamTest, TestPRead) {
  char buffer[1024];
  const size_t count = sizeof(buffer);
  const size_t offset = 3;

  // Test that a portion of this content ("3456789") is read via pread().
  stream_->content_ = "0123456789";
  // Normal call
  errno = 0;
  EXPECT_EQ(7, file_system_->pread(fd_, buffer, count, offset));
  EXPECT_EQ(0, errno);
  EXPECT_EQ("3456789", std::string(buffer, 7));

  // Bad fd
  EXPECT_ERROR(file_system_->pread(0, buffer, count, offset), EBADF);
}

TEST_BACKGROUND_F(FileSystemStreamTest, TestPWrite) {
  const char buffer[] = "abcd";
  const size_t count = sizeof(buffer) - 1;
  const size_t offset = 7;

  // Test that this content becomes "0123456789abcd" via pwrite().
  stream_->content_ = "0123456789";
  // Normal call
  errno = 0;
  EXPECT_EQ(4, file_system_->pwrite(fd_, buffer, count, offset));
  EXPECT_EQ(0, errno);
  EXPECT_EQ("0123456abcd", stream_->content_);

  // Bad fd
  EXPECT_ERROR(file_system_->pwrite(0, buffer, count, offset), EBADF);
}

TEST_BACKGROUND_F(FileSystemStreamTest, TestRead) {
  char buf[1024];

  // Test that a portion the content is read via read().
  stream_->content_ = "0123456789";
  // Normal call
  errno = 0;
  EXPECT_EQ(5, file_system_->read(fd_, buf, 5));
  EXPECT_EQ(0, errno);
  EXPECT_EQ("01234", std::string(buf, 5));

  // Bad fd
  EXPECT_ERROR(file_system_->read(0, buf, sizeof(buf)), EBADF);
}

TEST_BACKGROUND_F(FileSystemStreamTest, TestReadV) {
  char buf1[1] = {};
  const size_t count1 = 0;
  char buf2[2] = {};
  const size_t count2 = sizeof(buf2);
  char buf3[3] = {};
  const size_t count3 = sizeof(buf3);

  struct iovec iov[3] = {{buf1, count1}, {buf2, count2}, {buf3, count3}};

  // Test that a portion of this content ("01") is read via the logic in
  // file_stream.cc.
  stream_->content_ = "0123456789";
  // Normal call
  errno = 0;
  EXPECT_EQ(5, file_system_->readv(fd_, iov, sizeof(iov)/sizeof(iov[0])));
  EXPECT_EQ(0, errno);
  EXPECT_EQ(std::string("\0", 1), std::string(buf1, sizeof(buf1)));
  EXPECT_EQ(std::string("01", 2), std::string(buf2, sizeof(buf2)));
  EXPECT_EQ(std::string("234", 3), std::string(buf3, sizeof(buf3)));

  // Zero length iovec array
  errno = 0;
  EXPECT_EQ(0, file_system_->readv(fd_, iov, 0));
  EXPECT_EQ(0, errno);

  // NULL iov with 0-length.
  EXPECT_EQ(0, file_system_->readv(fd_, NULL, 0));
  EXPECT_EQ(0, errno);

  // Illegal length iovec array
  errno = 0;
  EXPECT_ERROR(file_system_->readv(fd_, iov, -1), EINVAL);

  // Illegal iov_len.
  iov[0].iov_len = -1;
  errno = 0;
  EXPECT_ERROR(
      file_system_->readv(fd_, iov, sizeof(iov)/sizeof(iov[0])), EINVAL);

  // NULL iov_base with iov_len == 0.
  iov[0].iov_len = 0;
  iov[0].iov_base = NULL;
  errno = 0;
  EXPECT_EQ(0, file_system_->readv(fd_, iov, 1));
  EXPECT_EQ(0, errno);

  // EINVAL has priority to EFAULT in iov verification.
  iov[1].iov_len = -1;
  errno = 0;
  EXPECT_ERROR(file_system_->readv(fd_, iov, 2), EINVAL);

  // Bad fd
  errno = 0;
  EXPECT_ERROR(
      file_system_->readv(0, iov, sizeof(iov)/sizeof(iov[0])), EBADF);
}

TEST_BACKGROUND_F(FileSystemStreamTest, TestRecv) {
  char buf[1024];
  int flags = 456;

  // Test that a portion of the content is read via recv().
  stream_->content_ = "0123456789";
  // Normal call
  errno = 0;
  EXPECT_EQ(5, file_system_->recv(fd_, buf, 5, flags));
  EXPECT_EQ(0, errno);
  EXPECT_EQ("01234", std::string(buf, 5));
  EXPECT_EQ(flags, stream_->flags_value_);

  // Bad sockfd
  EXPECT_ERROR(file_system_->recv(0, buf, sizeof(buf), flags), EBADF);
}

TEST_BACKGROUND_F(FileSystemStreamTest, TestRecvFrom) {
  char buf[1024];
  int flags = 456;
  sockaddr addr = {};
  socklen_t addrlen;

  // Test that a portion of the content is read via recv().
  stream_->content_ = "0123456789";
  // Normal call
  errno = 0;
  EXPECT_EQ(5, file_system_->recvfrom(fd_, buf, 5, flags, &addr, &addrlen));
  EXPECT_EQ(0, errno);
  EXPECT_EQ("01234", std::string(buf, 5));
  EXPECT_EQ(flags, stream_->flags_value_);
  EXPECT_EQ(&addr, stream_->sockaddr_ptr_);
  EXPECT_EQ(&addrlen, stream_->socklen_ptr_);

  // Bad sockfd
  EXPECT_ERROR(
      file_system_->recvfrom(0, buf, sizeof(buf), flags, &addr, &addrlen),
      EBADF);
}

TEST_BACKGROUND_F(FileSystemStreamTest, TestSend) {
  // Test that the content is written to the stream via send().
  const char buf[] = "hello";
  size_t count = sizeof(buf) - 1;
  int flags = 456;

  // Normal call
  errno = 0;
  EXPECT_EQ(static_cast<ssize_t>(count),
            file_system_->send(fd_, buf, count, flags));
  EXPECT_EQ(0, errno);
  EXPECT_EQ(buf, stream_->content_);
  EXPECT_EQ(flags, stream_->flags_value_);

  // Bad sockfd
  EXPECT_ERROR(file_system_->send(0, buf, count, flags), EBADF);
}

TEST_BACKGROUND_F(FileSystemStreamTest, TestSendTo) {
  // Test that the content is written to the stream via send().
  const char buf[] = "hello";
  size_t count = sizeof(buf) - 1;
  int flags = 456;
  sockaddr dest_addr = {};
  socklen_t addrlen = 654;

  // Normal call
  errno = 0;
  EXPECT_EQ(static_cast<ssize_t>(count),
            file_system_->sendto(fd_, buf, count, flags, &dest_addr,
                                 addrlen));
  EXPECT_EQ(0, errno);
  EXPECT_EQ(buf, stream_->content_);
  EXPECT_EQ(flags, stream_->flags_value_);
  EXPECT_EQ(&dest_addr, stream_->sockaddr_ptr_);
  EXPECT_EQ(addrlen, stream_->socklen_value_);

  // Bad sockfd
  EXPECT_ERROR(
      file_system_->sendto(0, buf, count, flags, &dest_addr, addrlen),
      EBADF);
}

TEST_BACKGROUND_F(FileSystemStreamTest, TestSetSockOpt) {
  const int level = 123;
  const int optname = 456;
  const void* optval = "abc";
  socklen_t optlen = 789;

  // Normal call
  errno = 0;
  EXPECT_EQ(0, file_system_->setsockopt(fd_, level, optname, optval, optlen));
  EXPECT_EQ(0, errno);
  EXPECT_EQ(level, stream_->level_value_);
  EXPECT_EQ(optname, stream_->optname_value_);
  EXPECT_EQ(optval, stream_->optval_ptr_);
  EXPECT_EQ(optlen, stream_->socklen_value_);

  // Bad sockfd
  EXPECT_ERROR(
      file_system_->setsockopt(0, level, optname, optval, optlen), EBADF);
}

TEST_BACKGROUND_F(FileSystemStreamTest, TestShutdown) {
  const int how = 0;

  // Normal call
  errno = 0;
  EXPECT_EQ(0, file_system_->shutdown(fd_, how));
  EXPECT_EQ(0, errno);

  // Bad fd
  EXPECT_ERROR(file_system_->shutdown(0, how), EBADF);
}

TEST_BACKGROUND_F(FileSystemStreamTest, TestWrite) {
  // Test that the content is written to the stream via write().
  const char buf[] = "hello";
  const size_t count = sizeof(buf) - 1;

  // Normal call
  errno = 0;
  EXPECT_EQ(static_cast<ssize_t>(count),
            file_system_->write(fd_, buf, count));
  EXPECT_EQ(0, errno);
  EXPECT_EQ(buf, stream_->content_);

  // Bad fd
  EXPECT_ERROR(file_system_->write(0, buf, count), EBADF);
}

TEST_BACKGROUND_F(FileSystemStreamTest, TestWriteV) {
  // Test that the content in the vector is written to the stream via the
  // logic in file_stream.cc.
  char buf1[1] = {'0'};
  const size_t count1 = 0;
  char buf2[2] = {'1', '2'};
  const size_t count2 = sizeof(buf2);
  char buf3[3] = {'3', '4', '5'};
  const size_t count3 = sizeof(buf3);
  char bufnul[1] = {};
  const size_t count4 = 1;

  struct iovec iov[4] = {
    {buf1, count1}, {buf2, count2}, {buf3, count3}, {bufnul, count4}};

  const size_t content_size = count1 + count2 + count3 + count4;

  // Normal call
  errno = 0;
  EXPECT_EQ(static_cast<ssize_t>(content_size),
            file_system_->writev(fd_, iov, sizeof(iov)/sizeof(iov[0])));
  EXPECT_EQ(0, errno);
  EXPECT_EQ(std::string("12345\0", 6), stream_->content_);

  // Zero length iovec array
  errno = 0;
  EXPECT_EQ(0, file_system_->writev(fd_, iov, 0));
  EXPECT_EQ(0, errno);
  // Bad length iovec array
  EXPECT_ERROR(file_system_->writev(fd_, iov, -1), EINVAL);
  // Bad fd
  EXPECT_ERROR(
      file_system_->writev(0, iov, sizeof(iov)/sizeof(iov[0])), EBADF);
}

}  // namespace posix_translation
