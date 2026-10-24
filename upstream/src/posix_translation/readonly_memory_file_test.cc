// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <errno.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <string>

#include "base/compiler_specific.h"
#include "gtest/gtest.h"
#include "posix_translation/readonly_memory_file.h"
#include "posix_translation/test_util/file_system_test_common.h"

namespace posix_translation {

namespace {

const char kFileName[] = "/path/to/file.txt";

// A stream for testing that simply returns a file of |size| bytes. The content
// of the file is initialized with UpdateContent() in both constructor and
// SetSize().
class TestReadonlyMemoryFile : public ReadonlyMemoryFile {
 public:
  TestReadonlyMemoryFile(const std::string& pathname, int errno_for_mmap,
                         size_t size, time_t mtime)
      : ReadonlyMemoryFile(pathname, errno_for_mmap, mtime) {
    SetSize(size);
  }

  void SetSize(size_t size) {
    content_.resize(size);
    UpdateContent();
  }

  // To allow TEST_F tests to call the protected function.
  using ReadonlyMemoryFile::set_mtime;

 private:
  virtual ~TestReadonlyMemoryFile() {}

  virtual const Content& GetContent() OVERRIDE {
    return content_;
  }

  void UpdateContent() {
    for (size_t i = 0; i < content_.size(); ++i) {
      char c;
      if (i == 0)
        c = '\0';
      else if (i < content_.size() / 2)
        c = 'A';
      else
        c = 'B';
      content_[i] = c;
    }
    // |content_| is now like "\0AABBBB" (without a \0 termination at the end of
    // the buffer).
  }

  Content content_;

  DISALLOW_COPY_AND_ASSIGN(TestReadonlyMemoryFile);
};

scoped_refptr<TestReadonlyMemoryFile> GetStream(size_t size, time_t mtime) {
  return new TestReadonlyMemoryFile(kFileName, 0 /* allow mmap */, size, mtime);
}

void CallIoctl(scoped_refptr<FileStream> stream, int request, ...) {
  va_list ap;
  va_start(ap, request);
  EXPECT_EQ(0, stream->ioctl(request, ap));
  va_end(ap);
}

// Use TEST_F with a class derived from FileSystemTestCommon to initialize
// VirtualFileSystem before executing a test. VirtualFileSystem is needed
// e.g. to assign an inode number to |kFileName|.
class ReadonlyMemoryFileTest : public FileSystemTestCommon {
};

}  // namespace

TEST_F(ReadonlyMemoryFileTest, TestReadEmptyStream) {
  static const ssize_t kSize = 0;
  scoped_refptr<FileStream> stream = GetStream(kSize, 0);
  ASSERT_TRUE(stream);
  char buf[32];
  EXPECT_EQ(0, stream->read(buf, sizeof(buf)));
}

TEST_F(ReadonlyMemoryFileTest, TestReadEmptyBuf) {
  static const ssize_t kSize = 0;
  scoped_refptr<FileStream> stream = GetStream(kSize, 0);
  ASSERT_TRUE(stream);
  char buf;
  EXPECT_EQ(0, stream->read(&buf, 0));
}

TEST_F(ReadonlyMemoryFileTest, TestRead) {
  static const ssize_t kSize = 16;
  scoped_refptr<FileStream> stream = GetStream(kSize, 0);
  ASSERT_TRUE(stream);
  char buf[kSize * 2];
  EXPECT_EQ(kSize, stream->read(buf, sizeof(buf)));
  EXPECT_EQ('A', buf[kSize / 2 - 1]);
  EXPECT_EQ('B', buf[kSize / 2]);
}

TEST_F(ReadonlyMemoryFileTest, TestReadShort) {
  static const ssize_t kSize = 16;
  scoped_refptr<FileStream> stream = GetStream(kSize, 0);
  ASSERT_TRUE(stream);
  char buf[kSize / 2];
  EXPECT_EQ(kSize / 2, stream->read(buf, sizeof(buf)));
  EXPECT_EQ('A', buf[kSize / 2 - 1]);
}

TEST_F(ReadonlyMemoryFileTest, TestReadExact) {
  static const ssize_t kSize = 16;
  scoped_refptr<FileStream> stream = GetStream(kSize, 0);
  ASSERT_TRUE(stream);
  char buf[kSize];
  EXPECT_EQ(kSize, stream->read(buf, sizeof(buf)));
  EXPECT_EQ('A', buf[kSize / 2 - 1]);
  EXPECT_EQ('B', buf[kSize / 2]);
}

TEST_F(ReadonlyMemoryFileTest, TestReadRepeat) {
  static const ssize_t kSize = 16;
  scoped_refptr<FileStream> stream = GetStream(kSize, 0);
  ASSERT_TRUE(stream);
  char buf[kSize * 2];
  EXPECT_EQ(kSize, stream->read(buf, sizeof(buf)));
  EXPECT_EQ('A', buf[kSize / 2 - 1]);
  EXPECT_EQ('B', buf[kSize / 2]);

  EXPECT_EQ(0, stream->read(buf, sizeof(buf)));
  EXPECT_EQ(1, stream->lseek(1, SEEK_SET));
  EXPECT_EQ(kSize - 1, stream->read(buf, sizeof(buf)));
  EXPECT_EQ('A', buf[kSize / 2 - 2]);
  EXPECT_EQ('B', buf[kSize / 2 - 1]);
}

TEST_F(ReadonlyMemoryFileTest, TestReadTwoStreams) {
  static const ssize_t kSize = 16;
  scoped_refptr<FileStream> stream = GetStream(kSize, 0);
  ASSERT_TRUE(stream);
  scoped_refptr<FileStream> stream2 = GetStream(kSize, 0);
  ASSERT_TRUE(stream2);
  // Read from two streams to make sure streams do not share internal status
  // like the current position.
  char buf[kSize];
  EXPECT_EQ(kSize, stream->read(buf, sizeof(buf)));
  EXPECT_EQ('A', buf[kSize / 2 - 1]);
  EXPECT_EQ('B', buf[kSize / 2]);
  memset(buf, 0, sizeof(buf));
  EXPECT_EQ(kSize, stream2->read(buf, sizeof(buf)));
  EXPECT_EQ('A', buf[kSize / 2 - 1]);
  EXPECT_EQ('B', buf[kSize / 2]);
}

TEST_F(ReadonlyMemoryFileTest, TestPread) {
  static const ssize_t kSize = 16;
  scoped_refptr<FileStream> stream = GetStream(kSize, 0);
  ASSERT_TRUE(stream);
  char buf[kSize * 2];
  EXPECT_EQ(kSize / 2 + 1, stream->pread(buf, sizeof(buf), kSize / 2 - 1));
  EXPECT_EQ('A', buf[0]);
  EXPECT_EQ('B', buf[1]);
}

TEST_F(ReadonlyMemoryFileTest, TestReadAfterPread) {
  static const ssize_t kSize = 16;
  scoped_refptr<FileStream> stream = GetStream(kSize, 0);
  ASSERT_TRUE(stream);
  char buf[kSize * 2];
  EXPECT_EQ(kSize / 2 + 1, stream->pread(buf, sizeof(buf), kSize / 2 - 1));
  // Then call read() to confirm that the |pos_| has not been modified by
  // pread().
  EXPECT_EQ(kSize, stream->read(buf, sizeof(buf)));
  EXPECT_EQ('A', buf[kSize / 2 - 1]);
  EXPECT_EQ('B', buf[kSize / 2]);
}

TEST_F(ReadonlyMemoryFileTest, TestPreadOutOfBound) {
  static const ssize_t kSize = 16;
  scoped_refptr<FileStream> stream = GetStream(kSize, 0);
  ASSERT_TRUE(stream);
  char buf[kSize];
  EXPECT_EQ(0, stream->pread(buf, sizeof(buf), kSize * 100));
}

TEST_F(ReadonlyMemoryFileTest, TestLseekSet) {
  static const ssize_t kSize = 16;
  scoped_refptr<FileStream> stream = GetStream(kSize, 0);
  ASSERT_TRUE(stream);
  char buf[kSize];
  EXPECT_EQ(kSize / 2 - 1, stream->lseek(kSize / 2 - 1, SEEK_SET));
  EXPECT_EQ(kSize / 2 + 1, stream->read(buf, sizeof(buf)));
  EXPECT_EQ('A', buf[0]);
  EXPECT_EQ('B', buf[1]);
}

TEST_F(ReadonlyMemoryFileTest, TestLseekCur) {
  static const ssize_t kSize = 16;
  scoped_refptr<FileStream> stream = GetStream(kSize, 0);
  ASSERT_TRUE(stream);
  EXPECT_EQ(kSize / 2 - 1, stream->lseek(kSize / 2 - 1, SEEK_SET));
  EXPECT_EQ(kSize / 2 - 2, stream->lseek(-1, SEEK_CUR));
  EXPECT_EQ(kSize / 2, stream->lseek(2, SEEK_CUR));
}

TEST_F(ReadonlyMemoryFileTest, TestLseekEnd) {
  static const ssize_t kSize = 16;
  scoped_refptr<FileStream> stream = GetStream(kSize, 0);
  ASSERT_TRUE(stream);
  char buf[kSize];
  EXPECT_EQ(kSize, stream->lseek(0, SEEK_END));
  EXPECT_EQ(0, stream->read(buf, sizeof(buf)));
  EXPECT_EQ(kSize - 1, stream->lseek(-1, SEEK_END));
  EXPECT_EQ(1, stream->read(buf, sizeof(buf)));
  EXPECT_EQ('B', buf[0]);
}

TEST_F(ReadonlyMemoryFileTest, TestFstat) {
  static const ssize_t kSize = 16;
  const time_t now = time(NULL);

  scoped_refptr<FileStream> stream = GetStream(kSize, now);
  ASSERT_TRUE(stream);
  struct stat st;
  EXPECT_EQ(0, stream->fstat(&st));
  EXPECT_EQ(static_cast<mode_t>(S_IFREG), st.st_mode);
  EXPECT_EQ(kSize, st.st_size);
  // Bionic uses unsigned long for st_*time instead of time_t.
  EXPECT_EQ(now, static_cast<time_t>(st.st_mtime));
  EXPECT_LT(0U, st.st_ino);
}

TEST_F(ReadonlyMemoryFileTest, TestFstatMtime) {
  static const ssize_t kSize = 16;
  const time_t now = time(NULL);

  scoped_refptr<TestReadonlyMemoryFile> stream = GetStream(kSize, now);
  ASSERT_TRUE(stream);
  struct stat st;
  EXPECT_EQ(0, stream->fstat(&st));
  // Bionic uses unsigned long for st_*time instead of time_t.
  EXPECT_EQ(now, static_cast<time_t>(st.st_mtime));

  stream->set_mtime(now + 1);
  EXPECT_EQ(0, stream->fstat(&st));
  EXPECT_EQ(now + 1, static_cast<time_t>(st.st_mtime));
}

TEST_F(ReadonlyMemoryFileTest, TestWrite) {
  static const ssize_t kSize = 16;
  scoped_refptr<FileStream> stream = GetStream(kSize, 0);
  ASSERT_TRUE(stream);
  char c = 'X';
  EXPECT_EQ(-1, stream->write(&c, 1));
  EXPECT_EQ(EBADF, errno);
}

TEST_F(ReadonlyMemoryFileTest, TestPwrite) {
  static const ssize_t kSize = 16;
  scoped_refptr<FileStream> stream = GetStream(kSize, 0);
  ASSERT_TRUE(stream);
  char c = 'X';
  EXPECT_EQ(-1, stream->pwrite(&c, 1, kSize / 2));
  EXPECT_EQ(EBADF, errno);
}

TEST_F(ReadonlyMemoryFileTest, TestIoctl) {
  static const ssize_t kSize = 16;
  scoped_refptr<FileStream> stream = GetStream(kSize, 0);
  ASSERT_TRUE(stream);
  int remain;
  CallIoctl(stream, FIONREAD, &remain);
  EXPECT_EQ(kSize, remain);
  char buf[kSize];
  EXPECT_EQ(kSize - 1, stream->read(buf, kSize - 1));
  CallIoctl(stream, FIONREAD, &remain);
  EXPECT_EQ(1, remain);
  EXPECT_EQ(1, stream->read(buf, kSize));
  CallIoctl(stream, FIONREAD, &remain);
  EXPECT_EQ(0, remain);
}

TEST_F(ReadonlyMemoryFileTest, TestMmapUnsupported) {
  static const size_t kSize = 3;

  scoped_refptr<FileStream> stream = new TestReadonlyMemoryFile(
      kFileName, ENODEV /* do not support mmap */, kSize, 0);

  EXPECT_EQ(MAP_FAILED,
            stream->mmap(NULL, kSize, PROT_READ, MAP_PRIVATE, 0));
  EXPECT_EQ(ENODEV, errno);
  EXPECT_EQ(MAP_FAILED,
            stream->mmap(NULL, kSize, PROT_WRITE, MAP_PRIVATE, 0));
  EXPECT_EQ(ENODEV, errno);

  // EACCES should be preferred over ENODEV.
  EXPECT_EQ(MAP_FAILED,
            stream->mmap(NULL, kSize, PROT_WRITE, MAP_SHARED, 0));
  EXPECT_EQ(EACCES, errno);
  EXPECT_EQ(MAP_FAILED,
            stream->mmap(NULL, kSize, PROT_READ | PROT_WRITE, MAP_SHARED, 0));
  EXPECT_EQ(EACCES, errno);

  // PROT_READ + MAP_SHARED mmap is not allowed either (at least for now).
  // See the comment in ReadonlyMemoryFile::mmap.
  stream = new TestReadonlyMemoryFile(
      kFileName, 0 /* support mmap */, kSize, 0);
  EXPECT_EQ(MAP_FAILED,
            stream->mmap(NULL, kSize, PROT_READ, MAP_SHARED, 0));
  EXPECT_EQ(EINVAL, errno);
}

TEST_F(ReadonlyMemoryFileTest, TestMmap) {
  static const ssize_t kSize = 16;
  scoped_refptr<FileStream> stream = GetStream(kSize, 0);
  ASSERT_TRUE(stream);

  char buf[kSize];
  EXPECT_EQ(kSize, stream->read(buf, sizeof(buf)));

  void* addr = stream->mmap(NULL, kSize, PROT_READ, MAP_PRIVATE, 0);
  ASSERT_NE(MAP_FAILED, addr);
  EXPECT_EQ(0, memcmp(addr, buf, kSize));
  EXPECT_EQ(0, stream->munmap(addr, kSize));

  // Retry with length == 1.
  uint8_t* addr2 = static_cast<uint8_t*>(
      stream->mmap(NULL, 1, PROT_READ, MAP_PRIVATE, 0));
  ASSERT_NE(MAP_FAILED, addr2);
  EXPECT_EQ('\0', addr2[0]);
  // This should not fail/crash even though the map length is 1.
  EXPECT_EQ('A', addr2[1]);
  EXPECT_EQ(0, stream->munmap(addr2, 1));
}

TEST_F(ReadonlyMemoryFileTest, TestHugeMmap) {
  const int page_size = sysconf(_SC_PAGE_SIZE);
  ASSERT_LT(0, page_size);

  const ssize_t size = page_size * 2;
  scoped_refptr<FileStream> stream = GetStream(size, 0);
  ASSERT_TRUE(stream);

  uint8_t* addr = static_cast<uint8_t*>(
      stream->mmap(NULL, size, PROT_READ, MAP_PRIVATE, 0));
  ASSERT_NE(MAP_FAILED, addr);
  EXPECT_EQ('A', addr[size / 2 - 1]);
  EXPECT_EQ('B', addr[size / 2]);
  EXPECT_EQ(0, stream->munmap(addr, size));

  // Confirm that mmap with non-zero offset also works.
  addr = static_cast<uint8_t*>(
      stream->mmap(NULL, 1, PROT_READ, MAP_PRIVATE, page_size));
  ASSERT_NE(MAP_FAILED, addr);
  EXPECT_EQ('B', addr[0]);
  EXPECT_EQ('B', addr[1]);  // same - should not fail/crash.
  EXPECT_EQ(0, stream->munmap(addr, 1));
}

TEST_F(ReadonlyMemoryFileTest, TestMmapTwice) {
  static const ssize_t kSize = 16;
  scoped_refptr<FileStream> stream = GetStream(kSize, 0);
  ASSERT_TRUE(stream);
  void* addr1 = stream->mmap(NULL, kSize, PROT_READ, MAP_PRIVATE, 0);
  ASSERT_NE(MAP_FAILED, addr1);
  void* addr2 = stream->mmap(NULL, kSize, PROT_READ, MAP_PRIVATE, 0);
  ASSERT_NE(MAP_FAILED, addr2);
  EXPECT_NE(addr1, addr2);  // POSIX requires this.
  EXPECT_EQ(0, stream->munmap(addr1, kSize));
  EXPECT_EQ(0, stream->munmap(addr2, kSize));
}

TEST_F(ReadonlyMemoryFileTest, TestMmapWithOffset) {
  static const ssize_t kSize = (64 * 1024) + 1;
  scoped_refptr<TestReadonlyMemoryFile> stream = GetStream(kSize, 0);
  ASSERT_TRUE(stream);
  char* addr = reinterpret_cast<char*>(stream->mmap(
      NULL, 1, PROT_READ, MAP_PRIVATE, 64 * 1024));
  ASSERT_NE(MAP_FAILED, addr);
  EXPECT_EQ('B', addr[0]);
  EXPECT_EQ(0, stream->munmap(addr, 1));

  // Retry with too larget offset. Confirm it does return a valid address
  // and it does not crash.
  addr = reinterpret_cast<char*>(stream->mmap(
      NULL, 2, PROT_READ, MAP_PRIVATE, 64 * 1024 * 2));
  ASSERT_NE(MAP_FAILED, addr);
  EXPECT_EQ(0, stream->munmap(addr, 2));

  stream->SetSize(kSize - 1);
  addr = reinterpret_cast<char*>(stream->mmap(
      NULL, 2, PROT_READ, MAP_PRIVATE, 64 * 1024 * 2));
  ASSERT_NE(MAP_FAILED, addr);
  EXPECT_EQ(0, stream->munmap(addr, 2));
}

TEST_F(ReadonlyMemoryFileTest, TestMmapWritablePrivate) {
  static const ssize_t kSize = 16;
  scoped_refptr<FileStream> stream = GetStream(kSize, 0);
  ASSERT_TRUE(stream);
  // Although the stream is readonly, PROT_WRITE mmap should be allowed as long
  // as the type of the mapping is MAP_PRIVATE.
  void* addr = stream->mmap(NULL, kSize, PROT_WRITE, MAP_PRIVATE, 0);
  ASSERT_NE(MAP_FAILED, addr);
  memset(addr, 0, kSize);  // this should not crash.
  EXPECT_EQ(0, stream->munmap(addr, kSize));

  addr = stream->mmap(NULL, kSize, PROT_READ | PROT_WRITE, MAP_PRIVATE, 0);
  ASSERT_NE(MAP_FAILED, addr);
  // Confirm that the previous memset() does not affect the actual content in
  // the stream.
  EXPECT_EQ('\0', static_cast<uint8_t*>(addr)[0]);
  EXPECT_EQ('A', static_cast<uint8_t*>(addr)[1]);
  EXPECT_EQ(0, stream->munmap(addr, kSize));
}

TEST_F(ReadonlyMemoryFileTest, TestMmapWritableShared) {
  static const ssize_t kSize = 16;
  scoped_refptr<FileStream> stream = GetStream(kSize, 0);
  ASSERT_TRUE(stream);
  // MAP_SHARED mapping combined with PROT_WRITE is not allowed.
  EXPECT_EQ(MAP_FAILED,
            stream->mmap(NULL, kSize, PROT_WRITE, MAP_SHARED, 0));
  EXPECT_EQ(EACCES, errno);
}

TEST_F(ReadonlyMemoryFileTest, TestGetStreamType) {
  scoped_refptr<FileStream> stream = GetStream(0, 0);
  ASSERT_TRUE(stream);
  ASSERT_TRUE(stream->GetStreamType());
  EXPECT_NE(std::string("unknown"), stream->GetStreamType());
  EXPECT_NE(std::string(), stream->GetStreamType());
  EXPECT_GE(8U, strlen(stream->GetStreamType()));
}

TEST_F(ReadonlyMemoryFileTest, TestGetSize) {
  const size_t kSize = 123;
  scoped_refptr<FileStream> stream = GetStream(kSize, 0);
  ASSERT_TRUE(stream);
  EXPECT_EQ(kSize, stream->GetSize());
}

// Do similar tests with SetSize() calls.

TEST_F(ReadonlyMemoryFileTest, TestReadDynamicallySizedFile) {
  static const ssize_t kSize = 30;
  scoped_refptr<TestReadonlyMemoryFile> stream = GetStream(5, 0);
  ASSERT_TRUE(stream);
  char buf[kSize];

  // Increase the size after read.
  EXPECT_EQ(5, stream->read(buf, sizeof(buf)));
  EXPECT_EQ('A', buf[1]);
  EXPECT_EQ('B', buf[2]);
  stream->SetSize(6);
  EXPECT_EQ(1, stream->read(buf, sizeof(buf)));
  EXPECT_EQ('B', buf[0]);

  // Increase the size during read. The size here is 6.
  EXPECT_EQ(0, stream->lseek(0, SEEK_SET));
  EXPECT_EQ(5, stream->read(buf, 5));
  EXPECT_EQ('A', buf[2]);
  EXPECT_EQ('B', buf[3]);
  stream->SetSize(20);
  EXPECT_EQ(15, stream->read(buf, sizeof(buf)));
  EXPECT_EQ('A', buf[4]);
  EXPECT_EQ('B', buf[5]);

  // Decrease the size after read. The size here is 20.
  EXPECT_EQ(0, stream->lseek(0, SEEK_SET));
  EXPECT_EQ(20, stream->read(buf, sizeof(buf)));
  EXPECT_EQ('A', buf[9]);
  EXPECT_EQ('B', buf[10]);
  stream->SetSize(10);
  EXPECT_EQ(0, stream->read(buf, sizeof(buf)));

  // Decrease the size during read. The size here is 10.
  EXPECT_EQ(0, stream->lseek(0, SEEK_SET));
  EXPECT_EQ(5, stream->read(buf, 5));
  stream->SetSize(6);
  EXPECT_EQ(1, stream->read(buf, sizeof(buf)));
  EXPECT_EQ('B', buf[0]);
}

TEST_F(ReadonlyMemoryFileTest, TestPreadDynamicallySizedFile) {
  // Directly test pread() too, just in case.
  static const ssize_t kSize = 30;
  scoped_refptr<TestReadonlyMemoryFile> stream = GetStream(6, 0);
  ASSERT_TRUE(stream);
  char buf[kSize];

  EXPECT_EQ(4, stream->pread(buf, sizeof(buf), 2));
  EXPECT_EQ('A', buf[0]);
  stream->SetSize(3);
  EXPECT_EQ(1, stream->pread(buf, sizeof(buf), 2));
  EXPECT_EQ('B', buf[0]);
  stream->SetSize(2);
  EXPECT_EQ(0, stream->pread(buf, sizeof(buf), 2));
  stream->SetSize(1);
  EXPECT_EQ(0, stream->pread(buf, sizeof(buf), 2));
  stream->SetSize(0);
  EXPECT_EQ(0, stream->pread(buf, sizeof(buf), 2));
}

TEST_F(ReadonlyMemoryFileTest, TestMmapDynamicallySizedFile) {
  static const ssize_t kSize = 20;
  scoped_refptr<TestReadonlyMemoryFile> stream = GetStream(kSize, 0);
  ASSERT_TRUE(stream);

  // Compare two mmap results before and after SetSize().
  char* addr = static_cast<char*>(
      stream->mmap(NULL, kSize, PROT_READ, MAP_PRIVATE, 0));
  ASSERT_NE(MAP_FAILED, addr);
  EXPECT_EQ('A', addr[9]);
  EXPECT_EQ('B', addr[10]);

  stream->SetSize(10);
  char* addr2 = static_cast<char*>(
      stream->mmap(NULL, 10, PROT_READ, MAP_PRIVATE, 0));
  ASSERT_NE(MAP_FAILED, addr2);
  EXPECT_EQ('A', addr2[4]);
  EXPECT_EQ('B', addr2[5]);

  EXPECT_EQ(0, stream->munmap(addr, kSize));
  EXPECT_EQ(0, stream->munmap(addr2, 10));
}

TEST_F(ReadonlyMemoryFileTest, TestFstatDynamicallySizedFile) {
  scoped_refptr<TestReadonlyMemoryFile> stream = GetStream(6, 0);
  ASSERT_TRUE(stream);

  struct stat st;
  EXPECT_EQ(0, stream->fstat(&st));
  EXPECT_EQ(6U, st.st_size);
  stream->SetSize(3);
  EXPECT_EQ(0, stream->fstat(&st));
  EXPECT_EQ(3U, st.st_size);
}

TEST_F(ReadonlyMemoryFileTest, TestIoctlDynamicallySizedFile) {
  static const ssize_t kSize = 16;
  scoped_refptr<TestReadonlyMemoryFile> stream = GetStream(kSize, 0);
  ASSERT_TRUE(stream);
  int remain;
  CallIoctl(stream, FIONREAD, &remain);
  EXPECT_EQ(kSize, remain);
  char buf[kSize];
  EXPECT_EQ(1, stream->read(buf, 1));
  CallIoctl(stream, FIONREAD, &remain);
  EXPECT_EQ(kSize - 1, remain);
  stream->SetSize(2);
  CallIoctl(stream, FIONREAD, &remain);
  EXPECT_EQ(1, remain);
  stream->SetSize(1);
  CallIoctl(stream, FIONREAD, &remain);
  EXPECT_EQ(0, remain);
}

TEST_F(ReadonlyMemoryFileTest, TestLseekDynamicallySizedFile) {
  static const ssize_t kSize = 16;
  scoped_refptr<TestReadonlyMemoryFile> stream = GetStream(kSize, 0);
  ASSERT_TRUE(stream);
  EXPECT_EQ(kSize, stream->lseek(0, SEEK_END));
  stream->SetSize(2);
  EXPECT_EQ(2, stream->lseek(0, SEEK_END));
}

TEST_F(ReadonlyMemoryFileTest, TestGetSizeDynamicallySizedFile) {
  scoped_refptr<TestReadonlyMemoryFile> stream = GetStream(6, 0);
  ASSERT_TRUE(stream);
  EXPECT_EQ(6U, stream->GetSize());
}

}  // namespace posix_translation
