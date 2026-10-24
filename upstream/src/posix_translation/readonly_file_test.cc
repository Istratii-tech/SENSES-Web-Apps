// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <arpa/inet.h>  // htonl
#include <time.h>
#include <unistd.h>

#include <string>

#include "base/compiler_specific.h"
#include "base/memory/scoped_ptr.h"
#include "gtest/gtest.h"
#include "posix_translation/readonly_file.h"
#include "posix_translation/readonly_fs_reader_test.h"
#include "posix_translation/readonly_memory_file.h"
#include "posix_translation/test_util/file_system_test_common.h"
#include "posix_translation/test_util/mmap_util.h"

namespace posix_translation {

namespace {

const char kBadFile[] = "does_not_exist";
const char kImageFile[] = "/tmp/test.img";
const ssize_t kReadAheadSize = 256;
const time_t kImageFileMtime = 12345;

// A stream for testing which works as the underlying stream for ReadonlyFile
// (the test target) and provides actual file content to the stream.
class TestUnderlyingStream : public ReadonlyMemoryFile {
 public:
  TestUnderlyingStream(const uint8_t* content, size_t size)
      : ReadonlyMemoryFile(kImageFile, 0, kImageFileMtime),
        content_(content, content + size) {
  }
  virtual ~TestUnderlyingStream() {}

 private:
  virtual const Content& GetContent() OVERRIDE {
    return content_;
  }

  const Content content_;

  DISALLOW_COPY_AND_ASSIGN(TestUnderlyingStream);
};

// A handler for creating a TestUnderlyingStream stream. A TestUnderlyingHandler
// instance is passed to ReadonlyFileHandler (another test target), and
// TestUnderlyingHandler::open() is called by the handler.
class TestUnderlyingHandler : public FileSystemHandler {
 public:
  TestUnderlyingHandler() : FileSystemHandler("TestUnderlyingHandler") {
    initialized_ = test_image_.Init(
        ARC_TARGET_PATH "/posix_translation_fs_images/"
        "test_readonly_fs_image.img");
  }
  virtual ~TestUnderlyingHandler() {}

  virtual bool IsInitialized() const OVERRIDE { return initialized_; }
  virtual scoped_refptr<FileStream> open(
      int fd, const std::string& pathname, int oflag, mode_t cmode) OVERRIDE {
    if (pathname == kImageFile) {
      return new TestUnderlyingStream(reinterpret_cast<const uint8_t*>(
          test_image_.data()), test_image_.size());
    }
    errno = ENOENT;
    return NULL;
  }
  virtual int stat(const std::string& pathname, struct stat* out) OVERRIDE {
    return -1;
  }
  virtual int statfs(const std::string& pathname, struct statfs* out) OVERRIDE {
    return -1;
  }
  virtual Dir* OnDirectoryContentsNeeded(const std::string& name) OVERRIDE {
    return NULL;
  }

 private:
  MmappedFile test_image_;
  bool initialized_;

  DISALLOW_COPY_AND_ASSIGN(TestUnderlyingHandler);
};

}  // namespace

class ReadonlyFileTest : public FileSystemTestCommon {
 protected:
  ReadonlyFileTest() {
  }

  virtual void SetUp() OVERRIDE {
    FileSystemTestCommon::SetUp();

    // Although we use NaClManifestHandler as an underlying handler for
    // ReadonlyFileHandler for production, it does not work inside unit
    // test. Assuming ReadonlyMemoryFileHandler works fine, we use it as a
    // replacement.
    underlying_handler_.reset(new TestUnderlyingHandler);
    handler_.reset(new ReadonlyFileHandler(kImageFile, kReadAheadSize,
                                           underlying_handler_.get()));
    handler_->Initialize();
    ASSERT_TRUE(handler_->IsInitialized());
  }

  virtual void TearDown() OVERRIDE {
    underlying_handler_.reset();
    FileSystemTestCommon::TearDown();
  }
  void CallIoctl(scoped_refptr<FileStream> stream, int request, ...);

  scoped_ptr<ReadonlyFileHandler> handler_;

 private:
  scoped_ptr<FileSystemHandler> underlying_handler_;

  DISALLOW_COPY_AND_ASSIGN(ReadonlyFileTest);
};

TEST_F(ReadonlyFileTest, TestOpen) {
  // Cannot open files in writable mode.
  scoped_refptr<FileStream> stream =
      handler_->open(-1 /* fd */, kTestFiles[0].filename, O_WRONLY, 0);
  EXPECT_FALSE(stream);
  stream = handler_->open(-1, kTestFiles[0].filename, O_RDWR, 0);
  EXPECT_FALSE(stream);

  stream = handler_->open(-1, kTestFiles[0].filename, O_RDONLY, 0);
  ASSERT_TRUE(stream);

  // Test if it is possible to open the same file again.
  scoped_refptr<FileStream> stream2 = handler_->open(
      -1, kTestFiles[0].filename, O_RDONLY, 0);
  ASSERT_TRUE(stream2);
  EXPECT_NE(stream, stream2);

  // Test O_DIRECTORY.
  errno = 0;
  scoped_refptr<FileStream> stream3 = handler_->open(
      -1, kTestFiles[0].filename, O_RDONLY | O_DIRECTORY, 0);
  EXPECT_FALSE(stream3);
  EXPECT_EQ(ENOTDIR, errno);
}

TEST_F(ReadonlyFileTest, TestMmap) {
  scoped_refptr<FileStream> stream =
      handler_->open(-1, kTestFiles[0].filename, O_RDONLY, 0);
  ASSERT_TRUE(stream);

  // Try to map the first file in the image.
  char* file0 = reinterpret_cast<char*>(stream->mmap(
      NULL,  kTestFiles[0].size, PROT_READ, MAP_PRIVATE, 0));
  ASSERT_NE(MAP_FAILED, file0);
  EXPECT_EQ(0, strncmp(file0, "123\n", 4));

  // Do the same again and compare two addresses.
  char* file0_2 = reinterpret_cast<char*>(stream->mmap(
      NULL,  kTestFiles[0].size, PROT_READ, MAP_PRIVATE, 0));
  ASSERT_NE(MAP_FAILED, file0_2);
  EXPECT_NE(file0, file0_2);
  EXPECT_EQ(0, stream->munmap(file0, kTestFiles[0].size));
  EXPECT_EQ(0, stream->munmap(file0_2, kTestFiles[0].size));

  // Try to map the second file in the image with zero and non-zero offset.
  stream = handler_->open(-1, kTestFiles[1].filename, O_RDONLY, 0);
  ASSERT_TRUE(stream);
  const size_t kPageSizeMultiple = 64 * 1024;
  char* file1 = reinterpret_cast<char*>(stream->mmap(
      NULL, kPageSizeMultiple * 2, PROT_READ, MAP_PRIVATE, 0));
  ASSERT_NE(MAP_FAILED, file1);
  EXPECT_EQ(0, file1[0]);
  EXPECT_EQ(0, file1[89999]);
  EXPECT_EQ('X', file1[90000]);
  EXPECT_EQ(0, stream->munmap(file1, kPageSizeMultiple * 2));

  file1 = reinterpret_cast<char*>(stream->mmap(
      NULL, kPageSizeMultiple, PROT_READ, MAP_PRIVATE, kPageSizeMultiple));
  ASSERT_NE(MAP_FAILED, file1);
  EXPECT_EQ(0, file1[0]);  // confirm this does not crash.
  EXPECT_EQ(0, file1[89999 - kPageSizeMultiple]);
  EXPECT_EQ('X', file1[90000 - kPageSizeMultiple]);
  EXPECT_EQ(0, stream->munmap(file1, kPageSizeMultiple));

  // Try to map the second file with too large offset. This should NOT be
  // rejected (see the comment in ReadonlyFile::mmap).
  file1 = reinterpret_cast<char*>(stream->mmap(
      NULL, 1, PROT_READ, MAP_PRIVATE, kPageSizeMultiple * 10));
  ASSERT_NE(MAP_FAILED, file1);
  EXPECT_EQ(0, stream->munmap(file1, 1));

  // Try to map the second file with too large length. This should NOT be
  // rejected either (see the comment in ReadonlyFile::mmap).
  file1 = reinterpret_cast<char*>(stream->mmap(
      NULL, kTestFiles[1].size * 10, PROT_READ, MAP_PRIVATE, 0));
  ASSERT_NE(MAP_FAILED, file1);
  EXPECT_EQ(0, stream->munmap(file1, kTestFiles[1].size * 10));

  // Try to map a file in the middle of the image file.
  stream = handler_->open(-1, kTestFiles[5].filename, O_RDONLY, 0);
  ASSERT_TRUE(stream);
  char* file5 = reinterpret_cast<char*>(stream->mmap(
      NULL,  kTestFiles[5].size, PROT_READ, MAP_PRIVATE, 0));
  ASSERT_NE(MAP_FAILED, file5);
  EXPECT_EQ(0, strncmp(file5, "A", 1));
  EXPECT_EQ(0, stream->munmap(file5, kTestFiles[5].size));

  // TODO(crbug.com/373818): Re-enable the test.
#if !(defined(__arm__) && !defined(__native_client__))
  // Zero-length mmap should always fail.
  errno = 0;
  EXPECT_EQ(MAP_FAILED, stream->mmap(NULL, 0, PROT_READ, MAP_PRIVATE, 0));
  EXPECT_EQ(EINVAL, errno);
#endif

  // Unaligned offset should always be rejected.
  errno = 0;
  EXPECT_EQ(MAP_FAILED, stream->mmap(NULL, 1, PROT_READ, MAP_PRIVATE, 1));
  EXPECT_EQ(EINVAL, errno);
}

TEST_F(ReadonlyFileTest, TestMkdir) {
  // mkdir is not supported.
  EXPECT_EQ(-1, handler_->mkdir("/tmp/directory", 0777));
  EXPECT_EQ(EACCES, errno);
  EXPECT_EQ(-1, handler_->mkdir("/test/dir", 0777));
  EXPECT_EQ(EEXIST, errno);
}

TEST_F(ReadonlyFileTest, TestTruncate) {
  // truncate is not supported.
  EXPECT_EQ(-1, handler_->truncate(kTestFiles[0].filename, 0));
  EXPECT_EQ(EACCES, errno);
  EXPECT_EQ(-1, handler_->truncate(kBadFile, 0));
  EXPECT_EQ(ENOENT, errno);
}

TEST_F(ReadonlyFileTest, TestUnlink) {
  // unlink is not supported.
  EXPECT_EQ(-1, handler_->unlink(kTestFiles[0].filename));
  EXPECT_EQ(EACCES, errno);
  EXPECT_EQ(-1, handler_->unlink(kBadFile));
  EXPECT_EQ(ENOENT, errno);
}

TEST_F(ReadonlyFileTest, TestRename) {
  // rename is not supported except renaming to the same file case.
  EXPECT_EQ(
      0, handler_->rename(kTestFiles[0].filename, kTestFiles[0].filename));
  EXPECT_EQ(-1, handler_->rename(kBadFile, kBadFile));
  EXPECT_EQ(ENOENT, errno);
  EXPECT_EQ(-1, handler_->rename(kTestFiles[0].filename, ""));
  EXPECT_EQ(ENOENT, errno);
  EXPECT_EQ(-1, handler_->rename(kTestFiles[0].filename, kBadFile));
  EXPECT_EQ(EACCES, errno);
}

TEST_F(ReadonlyFileTest, TestStat) {
  const struct stat kZeroBuf = {};
  struct stat statbuf = {};
  EXPECT_EQ(-1, handler_->stat(kBadFile, &statbuf));
  EXPECT_EQ(ENOENT, errno);
  for (size_t i = 0; i < kNumTestFiles; ++i) {
    EXPECT_EQ(0, handler_->stat(kTestFiles[i].filename, &statbuf)) << i;
    EXPECT_EQ(kTestFiles[i].size, statbuf.st_size) << i;
    // ReadonlyFile does not set permission bits, relying VirtualFileSystem.
    EXPECT_EQ(static_cast<mode_t>(S_IFREG), statbuf.st_mode) << i;
    EXPECT_NE(kZeroBuf.st_ino, statbuf.st_ino);
    EXPECT_LT(kZeroBuf.st_mtime, statbuf.st_mtime);
    EXPECT_EQ(kZeroBuf.st_atime, statbuf.st_atime);  // we do not support this.
    EXPECT_EQ(kZeroBuf.st_ctime, statbuf.st_ctime);  // we do not support this.
  }
  struct stat statbuf2 = {};
  EXPECT_EQ(0, handler_->stat(kTestFiles[0].filename, &statbuf2));
  // Check i-node uniqueness.
  EXPECT_NE(statbuf.st_ino, statbuf2.st_ino);

  // Try to stat directories.
  EXPECT_EQ(0, handler_->stat("/", &statbuf));
  // ReadonlyFile does not set permission bits, relying VirtualFileSystem.
  EXPECT_EQ(static_cast<mode_t>(S_IFDIR), statbuf.st_mode);
  EXPECT_EQ(kImageFileMtime, statbuf.st_mtime);
  EXPECT_EQ(0, handler_->stat("/test/", &statbuf));
  EXPECT_EQ(static_cast<mode_t>(S_IFDIR), statbuf.st_mode);
  EXPECT_EQ(0, handler_->stat("/test", &statbuf));
  EXPECT_EQ(static_cast<mode_t>(S_IFDIR), statbuf.st_mode);
  EXPECT_EQ(0, handler_->stat("/test/dir/", &statbuf));
  EXPECT_EQ(static_cast<mode_t>(S_IFDIR), statbuf.st_mode);
  EXPECT_EQ(0, handler_->stat("/test/dir", &statbuf));
  EXPECT_EQ(static_cast<mode_t>(S_IFDIR), statbuf.st_mode);
  EXPECT_EQ(-1, handler_->stat("/test/dir2", &statbuf));
  EXPECT_EQ(ENOENT, errno);
}

TEST_F(ReadonlyFileTest, TestRead) {
  scoped_refptr<FileStream> stream =
      handler_->open(-1 /* fd */, kTestFiles[0].filename, O_RDONLY, 0);
  ASSERT_TRUE(stream);

  char c = 0;
  EXPECT_EQ(1, stream->read(&c, 1));
  EXPECT_EQ('1', c);
  EXPECT_EQ(1, stream->read(&c, 1));
  EXPECT_EQ('2', c);
  EXPECT_EQ(1, stream->read(&c, 1));
  EXPECT_EQ('3', c);
  EXPECT_EQ(1, stream->read(&c, 1));
  EXPECT_EQ('\n', c);
  EXPECT_EQ(0 /* EOF */, stream->read(&c, 1));
  EXPECT_EQ(0 /* EOF */, stream->read(&c, 1));

  // Seek then read again.
  EXPECT_EQ(1, stream->lseek(1, SEEK_SET));
  EXPECT_EQ(1, stream->read(&c, 1));
  EXPECT_EQ('2', c);
  EXPECT_EQ(4, stream->lseek(0, SEEK_END));
  EXPECT_EQ(3, stream->lseek(-1, SEEK_CUR));
  EXPECT_EQ(1, stream->read(&c, 1));
  EXPECT_EQ('\n', c);
  EXPECT_EQ(0, stream->read(&c, 1));

  // Try pread(). Confirm the syscall does not update |offset_|.
  EXPECT_EQ(1, stream->pread(&c, 1, 2));
  EXPECT_EQ('3', c);
  EXPECT_EQ(0, stream->read(&c, 1));  // still return zero
  EXPECT_EQ(0, stream->pread(&c, 1, 12345));
}

TEST_F(ReadonlyFileTest, TestReadAhead) {
  // Use the large (100k) file for this test.
  scoped_refptr<FileStream> stream =
      handler_->open(-1 /* fd */, kTestFiles[1].filename, O_RDONLY, 0);
  ASSERT_TRUE(stream);

  // Confirm that we can read bytes larger than |kReadAheadSize| at once.
  char buf[kReadAheadSize * 10];
  memset(buf, 'A', sizeof(buf));
  EXPECT_EQ(89999, stream->lseek(89999, SEEK_SET));
  EXPECT_EQ(kReadAheadSize + 1, stream->read(buf, kReadAheadSize + 1));
  EXPECT_EQ('\0', buf[0]);
  EXPECT_EQ('X', buf[1]);
  EXPECT_EQ('X', buf[kReadAheadSize]);
  EXPECT_EQ('A', buf[kReadAheadSize + 1]);

  memset(buf, 'A', sizeof(buf));
  EXPECT_EQ(89999, stream->lseek(89999, SEEK_SET));
  EXPECT_EQ(kReadAheadSize, stream->read(buf, kReadAheadSize));
  EXPECT_EQ('\0', buf[0]);
  EXPECT_EQ('X', buf[1]);
  EXPECT_EQ('X', buf[kReadAheadSize - 1]);
  EXPECT_EQ('A', buf[kReadAheadSize]);

  // Try to fill the cache. Confirm that read() returns 1, not |kReadAheadSize|.
  memset(buf, 'A', sizeof(buf));
  EXPECT_EQ(89999, stream->lseek(89999, SEEK_SET));
  EXPECT_EQ(1, stream->read(buf, 1));
  EXPECT_EQ('\0', buf[0]);
  EXPECT_EQ('A', buf[1]);

  // Test the cache-hit case.
  memset(buf, 'A', sizeof(buf));
  EXPECT_EQ(89999, stream->lseek(89999, SEEK_SET));
  EXPECT_EQ(2, stream->read(buf, 2));
  EXPECT_EQ('\0', buf[0]);
  EXPECT_EQ('X', buf[1]);
  EXPECT_EQ('A', buf[2]);

  // The same. Cache-hit case.
  memset(buf, 'A', sizeof(buf));
  EXPECT_EQ(89999, stream->lseek(89999, SEEK_SET));
  EXPECT_EQ(kReadAheadSize - 1, stream->read(buf, kReadAheadSize - 1));
  EXPECT_EQ('\0', buf[0]);
  EXPECT_EQ('X', buf[1]);
  EXPECT_EQ('X', buf[kReadAheadSize - 2]);
  EXPECT_EQ('A', buf[kReadAheadSize - 1]);

  // Cache-miss.
  memset(buf, 'A', sizeof(buf));
  EXPECT_EQ(89999, stream->lseek(89999, SEEK_SET));
  EXPECT_EQ(kReadAheadSize, stream->read(buf, kReadAheadSize));
  EXPECT_EQ('\0', buf[0]);
  EXPECT_EQ('X', buf[1]);
  EXPECT_EQ('X', buf[kReadAheadSize - 1]);
  EXPECT_EQ('A', buf[kReadAheadSize]);

  // Cache-miss again.
  memset(buf, 'A', sizeof(buf));
  EXPECT_EQ(89998, stream->lseek(89998, SEEK_SET));
  EXPECT_EQ(3, stream->read(buf, 3));
  EXPECT_EQ('\0', buf[0]);
  EXPECT_EQ('\0', buf[1]);
  EXPECT_EQ('X', buf[2]);
  EXPECT_EQ('A', buf[3]);

  // Clear the cache.
  stream = handler_->open(-1, kTestFiles[1].filename, O_RDONLY, 0);
  ASSERT_TRUE(stream);

  // Seek near the end of the file. Confirm that read-ahead works fine in that
  // case too.
  memset(buf, 'A', sizeof(buf));
  EXPECT_EQ(99990, stream->lseek(-10, SEEK_END));
  EXPECT_EQ(1, stream->read(buf, 1));
  EXPECT_EQ('X', buf[0]);
  EXPECT_EQ('A', buf[1]);

  memset(buf, 'A', sizeof(buf));
  EXPECT_EQ(9, stream->read(buf, kReadAheadSize - 1));
  EXPECT_EQ('X', buf[0]);
  EXPECT_EQ('X', buf[8]);
  EXPECT_EQ('A', buf[9]);

  memset(buf, 'A', sizeof(buf));
  EXPECT_EQ(99980, stream->lseek(-20, SEEK_END));
  EXPECT_EQ(20, stream->read(buf, kReadAheadSize - 1));
  EXPECT_EQ('X', buf[0]);
  EXPECT_EQ('X', buf[19]);
  EXPECT_EQ('A', buf[20]);

  memset(buf, 'A', sizeof(buf));
  EXPECT_EQ(99970, stream->lseek(-30, SEEK_END));
  EXPECT_EQ(30, stream->read(buf, kReadAheadSize));
  EXPECT_EQ('X', buf[0]);
  EXPECT_EQ('X', buf[29]);
  EXPECT_EQ('A', buf[30]);
}

TEST_F(ReadonlyFileTest, TestReadAheadOneByte) {
  // Test the sequential 1-byte read case (crbug.com/288552).
  scoped_refptr<FileStream> stream =
      handler_->open(-1 /* fd */, kTestFiles[1].filename, O_RDONLY, 0);
  ASSERT_TRUE(stream);

  // Use assert not to output 90k failures.
  char c;
  for (size_t i = 0; i < 90000; ++i) {
    c = 0xff;
    // Because we are in a tight loop, try to avoid using SCOPED_TRACE
    // and only construct strings lazily when there is actually a
    // failure.
    ASSERT_EQ(1, stream->read(&c, 1)) << "at " << i;
    ASSERT_EQ('\0', c) << "at " << i;
  }
  // The same. Use assert.
  for (size_t i = 0; i < 10000; ++i) {
    c = 0xff;
    // The same. Only lazily construct error messages.
    ASSERT_EQ(1, stream->read(&c, 1)) << "at " << i;
    ASSERT_EQ('X', c)<< "at " << i;
  }

  // Just in case, confirm that read() recognizes EOF properly.
  EXPECT_EQ(0, stream->read(&c, 1));
  EXPECT_EQ(0, stream->read(&c, 1));
}

TEST_F(ReadonlyFileTest, TestWrite) {
  scoped_refptr<FileStream> stream =
      handler_->open(-1 /* fd */, kTestFiles[0].filename, O_RDONLY, 0);
  ASSERT_TRUE(stream);
  char c = 'a';
  EXPECT_EQ(-1, stream->write(&c, 1));
  EXPECT_EQ(EINVAL, errno);
  EXPECT_EQ(-1, stream->pwrite(&c, 1, 0));
  EXPECT_EQ(EINVAL, errno);
}

void ReadonlyFileTest::CallIoctl(
    scoped_refptr<FileStream> stream, int request, ...) {
  va_list ap;
  va_start(ap, request);
  EXPECT_EQ(0, stream->ioctl(request, ap));
  va_end(ap);
}

TEST_F(ReadonlyFileTest, TestIoctl) {
  scoped_refptr<FileStream> stream =
      handler_->open(-1 /* fd */, kTestFiles[1].filename, O_RDONLY, 0);
  ASSERT_TRUE(stream);
  int remain;
  CallIoctl(stream, FIONREAD, &remain);
  EXPECT_EQ(static_cast<int>(kTestFiles[1].size), remain);
  char c[kTestFiles[1].size];
  EXPECT_EQ(static_cast<ssize_t>(kTestFiles[1].size - 1),
            stream->read(c, kTestFiles[1].size - 1));
  CallIoctl(stream, FIONREAD, &remain);
  EXPECT_EQ(1, remain);
  EXPECT_EQ(1, stream->read(c, 1));
  CallIoctl(stream, FIONREAD, &remain);
  EXPECT_EQ(0, remain);
}

TEST_F(ReadonlyFileTest, TestGetStreamType) {
  scoped_refptr<FileStream> stream =
      handler_->open(-1 /* fd */, kTestFiles[1].filename, O_RDONLY, 0);
  ASSERT_TRUE(stream);
  EXPECT_NE(std::string("unknown"), stream->GetStreamType());
  EXPECT_NE(std::string(), stream->GetStreamType());
}

TEST_F(ReadonlyFileTest, TestGetSize) {
  scoped_refptr<FileStream> stream =
      handler_->open(-1 /* fd */, kTestFiles[1].filename, O_RDONLY, 0);
  ASSERT_TRUE(stream);
  EXPECT_EQ(kTestFiles[1].size, stream->GetSize());
}

}  // namespace posix_translation
