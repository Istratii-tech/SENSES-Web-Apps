// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <errno.h>
#include <string.h>
#include <sys/uio.h>

#include <limits>
#include <string>

#include "base/compiler_specific.h"
#include "gtest/gtest.h"
#include "posix_translation/file_stream.h"
#include "posix_translation/test_util/file_system_test_common.h"

namespace posix_translation {

class FileStreamTest : public FileSystemTestCommon {
 protected:
  FileStreamTest() {}

 private:
  DISALLOW_COPY_AND_ASSIGN(FileStreamTest);
};

class TestFileStream : public FileStream {
 public:
  TestFileStream()
      : FileStream(O_RDONLY, "/dummy/file.name"),
        last_read_buf_(NULL), last_read_count_(0), last_write_count_(0),
        on_last_file_ref_(0), on_handle_notification_from_(0), last_file_(NULL),
        last_is_closing_(false) {
  }
  virtual ~TestFileStream() {
  }

  virtual ssize_t read(void* buf, size_t count) OVERRIDE {
    last_read_buf_ = buf;
    last_read_count_ = count;
    return count;
  }

  virtual ssize_t write(const void* buf, size_t count) OVERRIDE {
    // In this test, |buf| always points to an ASCII string.
    last_write_buf_ = std::string(static_cast<const char*>(buf), count);
    last_write_count_ = count;
    return count;
  }

  virtual const char* GetStreamType() const OVERRIDE {
    return "test";
  }

  virtual void OnLastFileRef() OVERRIDE {
    ++on_last_file_ref_;
  }

  virtual void HandleNotificationFrom(
      scoped_refptr<FileStream> file, bool is_closing) OVERRIDE {
    ++on_handle_notification_from_;
    last_file_ = file.get();
    last_is_closing_ = is_closing;
  }

  void EnableListenerSupport() {
    FileStream::EnableListenerSupport();
  }

  bool StartListeningTo(scoped_refptr<FileStream> file) {
    return FileStream::StartListeningTo(file);
  }

  void StopListeningTo(scoped_refptr<FileStream> file) {
    FileStream::StopListeningTo(file);
  }

  void* last_read_buf_;
  size_t last_read_count_;
  std::string last_write_buf_;
  size_t last_write_count_;
  int on_last_file_ref_;
  int on_handle_notification_from_;
  FileStream* last_file_;  // use a raw pointer not to change the ref count.
  bool last_is_closing_;

 private:
  DISALLOW_COPY_AND_ASSIGN(TestFileStream);
};

TEST_F(FileStreamTest, TestConstruct) {
  scoped_refptr<TestFileStream> stream = new TestFileStream;
  stream->CheckNotClosed();
  EXPECT_EQ(0, stream->on_last_file_ref_);
}

TEST_F(FileStreamTest, TestFileRef) {
  scoped_refptr<TestFileStream> stream = new TestFileStream;
  stream->AddFileRef();
  stream->CheckNotClosed();
  stream->ReleaseFileRef();
  EXPECT_EQ(1, stream->on_last_file_ref_);
}

TEST_F(FileStreamTest, TestMultipleFileRef) {
  scoped_refptr<TestFileStream> stream = new TestFileStream;
  stream->AddFileRef();
  {
    scoped_refptr<FileStream> another_ref = stream;
    stream->AddFileRef();
    stream->CheckNotClosed();
    stream->ReleaseFileRef();
  }
  EXPECT_EQ(0, stream->on_last_file_ref_);
  stream->CheckNotClosed();
  stream->ReleaseFileRef();
  EXPECT_EQ(1, stream->on_last_file_ref_);
}

TEST_F(FileStreamTest, TestNotifications) {
  scoped_refptr<TestFileStream> stream = new TestFileStream;
  stream->EnableListenerSupport();
  stream->AddFileRef();
  scoped_refptr<TestFileStream> stream2 = new TestFileStream;
  scoped_refptr<TestFileStream> stream3 = new TestFileStream;
  scoped_refptr<TestFileStream> stream4 = new TestFileStream;
  EXPECT_TRUE(stream2->StartListeningTo(stream));
  EXPECT_TRUE(stream3->StartListeningTo(stream));
  EXPECT_TRUE(stream4->StartListeningTo(stream));
  stream3->StopListeningTo(stream);

  // Remove a file ref from |stream|,
  EXPECT_EQ(0, stream2->on_handle_notification_from_);
  EXPECT_EQ(0, stream3->on_handle_notification_from_);
  EXPECT_EQ(0, stream4->on_handle_notification_from_);
  stream->CheckNotClosed();
  stream->ReleaseFileRef();

  // then confirm that |stream2| and |stream4| receive notifications from
  // |stream|.
  EXPECT_EQ(1, stream2->on_handle_notification_from_);
  EXPECT_EQ(0, stream3->on_handle_notification_from_);
  EXPECT_EQ(1, stream4->on_handle_notification_from_);
  EXPECT_EQ(stream.get(), stream2->last_file_);
  EXPECT_TRUE(NULL == stream3->last_file_);  // unchanged
  EXPECT_EQ(stream.get(), stream4->last_file_);
  EXPECT_TRUE(stream2->last_is_closing_);
  EXPECT_FALSE(stream3->last_is_closing_);  // unchanged
  EXPECT_TRUE(stream4->last_is_closing_);
}

TEST_F(FileStreamTest, TestNotificationsWithTighterScope) {
  scoped_refptr<TestFileStream> stream = new TestFileStream;
  stream->EnableListenerSupport();
  stream->AddFileRef();
  {
    scoped_refptr<TestFileStream> stream2 = new TestFileStream;
    EXPECT_TRUE(stream2->StartListeningTo(stream));
  }
  // Confirm this does not crash. The second stream is still alive here since
  // stream->listeners_ holds a reference to the second stream.
  stream->ReleaseFileRef();
}

TEST_F(FileStreamTest, TestPermission) {
  scoped_refptr<FileStream> stream = new TestFileStream;
  // Test the default permission info.
  EXPECT_FALSE(stream->permission().IsValid());
  EXPECT_FALSE(stream->permission().is_writable());

  // Test setter/getter.
  static const uid_t kFileUid = 54321;
  static const bool kIsWritable = true;
  stream->set_permission(PermissionInfo(kFileUid, kIsWritable));
  EXPECT_TRUE(stream->permission().IsValid());
  EXPECT_EQ(kFileUid, stream->permission().file_uid());
  EXPECT_EQ(kIsWritable, stream->permission().is_writable());
}

TEST_F(FileStreamTest, TestReadV) {
  scoped_refptr<TestFileStream> stream = new TestFileStream;
  struct iovec iov[2] = {};

  // Test invalid args.
  errno = 0;
  EXPECT_EQ(-1, stream->readv(iov, -1));
  EXPECT_EQ(EINVAL, errno);
  errno = 0;
  EXPECT_EQ(-1, stream->readv(NULL, -1));
  EXPECT_EQ(EINVAL, errno);

  // Test 0 byte readv.
  errno = 0;
  EXPECT_EQ(0, stream->readv(iov, 0));
  EXPECT_EQ(0, errno);

  // Test a valid readv call.
  errno = 0;
  char buf0[128] = {};
  char buf1[64] = {};
  iov[0].iov_base = buf0;
  iov[0].iov_len = sizeof(buf0);
  iov[1].iov_base = buf1;
  iov[1].iov_len = sizeof(buf1);
  const ssize_t result = stream->readv(iov, 2);
  // Confirm that the result is 0 < result <= sum_of_the_buffer_lengths.
  // Note that our current implementation always performs short-read which
  // should be POSIX compliant.
  EXPECT_LT(0, result);
  EXPECT_GE(static_cast<ssize_t>(sizeof(buf0) + sizeof(buf1)), result);
  // Do the same check for last_read_count_.
  EXPECT_LT(0U, stream->last_read_count_);
  EXPECT_GE(sizeof(buf0) + sizeof(buf1), stream->last_read_count_);
  EXPECT_EQ(0, errno);
}

TEST_F(FileStreamTest, TestWriteV) {
  scoped_refptr<TestFileStream> stream = new TestFileStream;
  struct iovec iov[UIO_MAXIOV + 1] = {};  // 1023 items.

  // Test invalid args.
  errno = 0;
  EXPECT_EQ(-1, stream->writev(iov, -1));
  EXPECT_EQ(EINVAL, errno);
  errno = 0;
  EXPECT_EQ(-1, stream->writev(NULL, -1));
  EXPECT_EQ(EINVAL, errno);
  errno = 0;
  EXPECT_EQ(-1, stream->writev(iov, UIO_MAXIOV + 1));
  EXPECT_EQ(EINVAL, errno);

  // Test 0 byte writev.
  errno = 0;
  EXPECT_EQ(0, stream->writev(iov, 0));
  EXPECT_EQ(0, errno);

  // Test integer overflows.
  errno = 0;
  iov[0].iov_len = std::numeric_limits<size_t>::max() / 2;
  iov[1].iov_len = iov[0].iov_len + 1;
  EXPECT_EQ(-1, stream->writev(iov, 2));
  EXPECT_EQ(EINVAL, errno);

  // Test a valid writev call.
  errno = 0;
  char buf0[] = "buf0";
  char buf1[] = "buf1";
  iov[0].iov_base = buf0;
  iov[0].iov_len = strlen(buf0);
  iov[1].iov_base = buf1;
  iov[1].iov_len = strlen(buf1);
  const ssize_t result = stream->writev(iov, 2);
  EXPECT_EQ(static_cast<ssize_t>(strlen(buf0) + strlen(buf1)), result);
  EXPECT_EQ(strlen(buf0) + strlen(buf1), stream->last_write_count_);
  EXPECT_STREQ("buf0buf1", stream->last_write_buf_.c_str());
  EXPECT_EQ(0, errno);
}

TEST_F(FileStreamTest, TestMadvise) {
  static const size_t kSize = 8;
  scoped_refptr<TestFileStream> stream = new TestFileStream;
  EXPECT_EQ(0, stream->madvise(0, kSize, MADV_NORMAL));
  EXPECT_EQ(-1, stream->madvise(0, kSize, MADV_DONTNEED));
  EXPECT_EQ(EINVAL, errno);
  EXPECT_EQ(-1, stream->madvise(0, kSize, MADV_REMOVE));
  EXPECT_EQ(ENOSYS, errno);
  EXPECT_EQ(-1, stream->madvise(0, kSize, ~0));
  EXPECT_EQ(EINVAL, errno);
}

}  // namespace posix_translation
