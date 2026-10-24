// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Unit tests for socket base class implementation.

#include "posix_translation/socket_stream.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include "base/logging.h"
#include "base/memory/ref_counted.h"
#include "gtest/gtest.h"

namespace posix_translation {
namespace {

class TestSocketStream : public SocketStream {
 public:
  TestSocketStream() : SocketStream(AF_UNIX, O_RDONLY) {
  }
  virtual ~TestSocketStream() {}

  virtual ssize_t read(void* buf, size_t count) OVERRIDE {
    NOTIMPLEMENTED();
    errno = ENOSYS;
    return -1;
  }

  virtual ssize_t write(const void* buf, size_t count) OVERRIDE {
    NOTIMPLEMENTED();
    errno = ENOSYS;
    return -1;
  }

  virtual const char* GetStreamType() const OVERRIDE {
    return "test-stream";
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(TestSocketStream);
};

#define EXPECT_ERROR(expected_error, result) do { \
    EXPECT_EQ(-1, result); \
    EXPECT_EQ(expected_error, errno); \
    errno = 0; \
  } while (0)

// Thin wrapper to call SocketStream::fcntl.
int SocketFcntl(SocketStream* stream, int cmd, ...) {
  va_list ap;
  va_start(ap, cmd);
  int result = stream->fcntl(cmd, ap);
  va_end(ap);
  return result;
}

// Thin wrapper to call SocketStream::ioctl.
int SocketIoctl(SocketStream* stream, int request, ...) {
  va_list ap;
  va_start(ap, request);
  int result = stream->ioctl(request, ap);
  va_end(ap);
  return result;
}

}  // namespace

TEST(SocketStreamTest, ioctl_Invalid) {
  scoped_refptr<TestSocketStream> stream(new TestSocketStream);
  EXPECT_ERROR(EINVAL, SocketIoctl(stream.get(), -1));
}

TEST(SocketStreamTest, ioctl_FIONBIO) {
  scoped_refptr<TestSocketStream> stream(new TestSocketStream);
  EXPECT_EQ(0, SocketFcntl(stream.get(), F_GETFL) & O_NONBLOCK);
  int val = 1;
  ASSERT_EQ(0, SocketIoctl(stream.get(), FIONBIO, &val));
  EXPECT_EQ(O_NONBLOCK, SocketFcntl(stream.get(), F_GETFL) & O_NONBLOCK);
  val = 0;
  ASSERT_EQ(0, SocketIoctl(stream.get(), FIONBIO, &val));
  EXPECT_EQ(0, SocketFcntl(stream.get(), F_GETFL) & O_NONBLOCK);
}

}  // namespace posix_translation
