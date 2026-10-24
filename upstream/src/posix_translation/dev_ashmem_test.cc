// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <asm/types.h>  // for __u32 used in linux/ashmem.h
#include <errno.h>
#include <stdarg.h>
#include <inttypes.h>
#include <linux/ashmem.h>
#include <sys/sysmacros.h>

#include "base/compiler_specific.h"
#include "base/memory/scoped_ptr.h"
#include "gtest/gtest.h"
#include "posix_translation/dev_ashmem.h"
#include "posix_translation/test_util/file_system_test_common.h"

// We use random numbers for this test.
const int kNullMajorId = 42;
const int kNullMinorId = 43;

namespace posix_translation {

class DevAshmemTest : public FileSystemTestCommon {
 protected:
  DevAshmemTest() : handler_(new DevAshmemHandler) {
  }

  int CallIoctl(scoped_refptr<FileStream> stream, int request, ...) {
    va_list ap;
    va_start(ap, request);
    const int ret = stream->ioctl(request, ap);
    va_end(ap);
    return ret;
  }

  scoped_ptr<FileSystemHandler> handler_;

 private:
  virtual void SetUp() OVERRIDE {
    FileSystemTestCommon::SetUp();
    DeviceHandler::AddDeviceId("/dev/ashmem", kNullMajorId, kNullMinorId);
  }

  DISALLOW_COPY_AND_ASSIGN(DevAshmemTest);
};

TEST_F(DevAshmemTest, TestInit) {
}

TEST_F(DevAshmemTest, TestStat) {
  struct stat st = {};
  EXPECT_EQ(0, handler_->stat("/dev/ashmem", &st));
  EXPECT_NE(0U, st.st_ino);
  EXPECT_EQ(S_IFCHR | 0666U, st.st_mode);
}

TEST_F(DevAshmemTest, TestOpenClose) {
  scoped_refptr<FileStream> stream =
      handler_->open(512, "/dev/ashmem", O_RDONLY, 0);
  ASSERT_TRUE(stream != NULL);
}

TEST_F(DevAshmemTest, TestOpenFail) {
  errno = 0;
  scoped_refptr<FileStream> stream =
      handler_->open(512, "/dev/ashmem", O_RDONLY | O_DIRECTORY, 0);
  EXPECT_TRUE(stream == NULL);
  EXPECT_EQ(ENOTDIR, errno);
}

TEST_F(DevAshmemTest, TestFstat) {
  scoped_refptr<FileStream> stream =
      handler_->open(512, "/dev/ashmem", O_RDONLY, 0);
  ASSERT_TRUE(stream != NULL);
  struct stat st = {};
  stream->fstat(&st);
  EXPECT_NE(0U, st.st_ino);
  EXPECT_EQ(S_IFCHR | 0666U, st.st_mode);
  EXPECT_EQ(makedev(kNullMajorId, kNullMinorId), st.st_rdev);
}

TEST_F(DevAshmemTest, TestIoctl) {
  scoped_refptr<FileStream> stream =
      handler_->open(512, "/dev/ashmem", O_RDONLY, 0);
  ASSERT_TRUE(stream != NULL);

  EXPECT_EQ(0, CallIoctl(stream, ASHMEM_SET_SIZE, 123));
  EXPECT_EQ(123, CallIoctl(stream, ASHMEM_GET_SIZE, 123));

  EXPECT_EQ(0, CallIoctl(stream, ASHMEM_SET_NAME, "123"));
  char buf[ASHMEM_NAME_LEN];
  ASSERT_EQ(0, CallIoctl(stream, ASHMEM_GET_NAME, buf));
  EXPECT_STREQ("123", buf);

  // Confirm that ASHMEM_SET_SIZE/NAME fails once the device is mapped.
  void* mapped = stream->mmap(NULL, 0x10000, PROT_READ, MAP_PRIVATE, 0);
  ASSERT_NE(MAP_FAILED, mapped);
  EXPECT_EQ(0, stream->munmap(mapped, 0x10000));
  errno = 0;
  EXPECT_EQ(-1, CallIoctl(stream, ASHMEM_SET_SIZE, 1234));
  EXPECT_EQ(EINVAL, errno);
  errno = 0;
  EXPECT_EQ(-1, CallIoctl(stream, ASHMEM_SET_NAME, "willfail"));
  EXPECT_EQ(EINVAL, errno);

  int dummy_prot = 0;
  EXPECT_EQ(0, CallIoctl(stream, ASHMEM_SET_PROT_MASK, dummy_prot));
  EXPECT_EQ(ASHMEM_NOT_PURGED, CallIoctl(stream, ASHMEM_PIN));
  EXPECT_EQ(ASHMEM_IS_UNPINNED, CallIoctl(stream, ASHMEM_UNPIN));
}

TEST_F(DevAshmemTest, TestLseek) {
  scoped_refptr<FileStream> stream =
      handler_->open(512, "/dev/ashmem", O_RDONLY, 0);
  ASSERT_TRUE(stream != NULL);
  errno = 0;
  // When the size is not set, it should return EINVAL.
  EXPECT_EQ(-1, stream->lseek(0, SEEK_SET));
  EXPECT_EQ(EINVAL, errno);
  CallIoctl(stream, ASHMEM_SET_SIZE, 1);

  // After setting the size, read() returns EBADF if the device is not yet
  // mapped to the memory.
  EXPECT_EQ(-1, stream->lseek(0, SEEK_SET));
  EXPECT_EQ(EBADF, errno);

  // Once it is mapped (even if the mapping is private), lseek succeeds.
  void* mapped = stream->mmap(NULL, 0x10000, PROT_READ, MAP_PRIVATE, 0);
  ASSERT_NE(MAP_FAILED, mapped);
  EXPECT_EQ(0, stream->munmap(mapped, 0x10000));
  EXPECT_EQ(123456, stream->lseek(123456, SEEK_SET));
}

// Confirm read() behavior with and without mmap().
TEST_F(DevAshmemTest, TestRead) {
  scoped_refptr<FileStream> stream =
      handler_->open(512, "/dev/ashmem", O_RDONLY, 0);
  ASSERT_TRUE(stream != NULL);

  // read() should return EOF if ASHMEM_SET_SIZE has not been called.
  char c;
  EXPECT_EQ(0, stream->read(&c, 1));  // EOF
  CallIoctl(stream, ASHMEM_SET_SIZE, 1);

  // After setting the size, read() returns EBADF if the device is not yet
  // mapped to the memory.
  errno = 0;
  EXPECT_EQ(-1, stream->read(&c, 1));
  EXPECT_EQ(EBADF, errno);

  // Once it is mapped (even if the mapping is private), read succeeds.
  void* mapped = stream->mmap(NULL, 0x10000, PROT_READ, MAP_PRIVATE, 0);
  ASSERT_NE(MAP_FAILED, mapped);
  EXPECT_EQ(0, stream->munmap(mapped, 0x10000));
  c = 42;
  EXPECT_EQ(1, stream->read(&c, 1));
  EXPECT_EQ(0, c);
}

TEST_F(DevAshmemTest, TestWrite) {
  scoped_refptr<FileStream> stream =
      handler_->open(512, "/dev/ashmem", O_RDWR, 0);
  ASSERT_TRUE(stream != NULL);
  // Write to the device should always fail.
  errno = 0;
  char c = 1;
  EXPECT_EQ(-1, stream->write(&c, 1));
  EXPECT_EQ(EINVAL, errno);
}

TEST_F(DevAshmemTest, TestAshmemMapSizeUnknown) {
  scoped_refptr<FileStream> stream =
      handler_->open(512, "/dev/ashmem", O_RDWR, 0);
  ASSERT_TRUE(stream != NULL);

  // mmap fails with EINVAL is ASHMEM_SET_SIZE has not been called yet.
  errno = 0;
  void* mapped =
      stream->mmap(NULL, 0x10000, PROT_READ | PROT_WRITE, MAP_SHARED, 0);
  EXPECT_EQ(MAP_FAILED, mapped);
  EXPECT_EQ(EINVAL, errno);
}

// Test the usual mmap then fully munmap case.
TEST_F(DevAshmemTest, TestAshmemMapUnmap) {
  scoped_refptr<FileStream> stream =
      handler_->open(512, "/dev/ashmem", O_RDWR, 0);
  ASSERT_TRUE(stream != NULL);

  int result = CallIoctl(stream, ASHMEM_SET_NAME, "arc-ashmem-abc");
  EXPECT_EQ(0, result);
  const size_t size = 0x200000;  // 2MB
  result = CallIoctl(stream, ASHMEM_SET_SIZE, size);
  EXPECT_EQ(0, result);
  void* mapped =
      stream->mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, 0);
  ASSERT_NE(MAP_FAILED, mapped);
  // Touch the memory
  static_cast<uint8_t*>(mapped)[0] = 0x1;
  static_cast<uint8_t*>(mapped)[size - 1] = 0x1;

  // Clean up.
  EXPECT_EQ(0, stream->munmap(mapped, size));
}

// Confirm that read() still succeeds even after full munmap.
// Both Binder and CTS require this.
TEST_F(DevAshmemTest, TestAshmemReadAfterUnmap) {
  scoped_refptr<FileStream> stream =
      handler_->open(512, "/dev/ashmem", O_RDWR, 0);
  ASSERT_TRUE(stream != NULL);

  const size_t size = 0x10000;  // 64kb
  int result = CallIoctl(stream, ASHMEM_SET_SIZE, size);
  EXPECT_EQ(0, result);

  void* mapped =
      stream->mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, 0);
  ASSERT_NE(MAP_FAILED, mapped);
  // Touch the memory, then full-unmap.
  static_cast<uint8_t*>(mapped)[0] = 0x1;
  static_cast<uint8_t*>(mapped)[size - 1] = 0x1;
  EXPECT_EQ(0, stream->munmap(mapped, size));

  // Confirm that read still succeeds.
  char buf[0x10000] = {};
  EXPECT_EQ(static_cast<ssize_t>(sizeof(buf)),
            stream->read(buf, sizeof(buf) * 2));
  EXPECT_EQ(0x1, buf[0]);
  EXPECT_EQ(0x0, buf[1]);
  EXPECT_EQ(0x0, buf[size - 2]);
  EXPECT_EQ(0x1, buf[size - 1]);
  EXPECT_EQ(0, stream->read(buf, 1));
}

// Confirm that read() fails after partial unmap. This is just a
// limitation of the current DevAshmem implementation. The real
// /dev/ashmem in the kernel does not have the limitation.
TEST_F(DevAshmemTest, TestAshmemReadAfterPartialUnmap) {
  scoped_refptr<FileStream> stream =
      handler_->open(512, "/dev/ashmem", O_RDWR, 0);
  ASSERT_TRUE(stream != NULL);

  const size_t size = 0x20000;  // 128kb
  int result = CallIoctl(stream, ASHMEM_SET_SIZE, size);
  EXPECT_EQ(0, result);

  void* mapped =
      stream->mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, 0);
  ASSERT_NE(MAP_FAILED, mapped);
  EXPECT_EQ(0, stream->munmap(mapped, size / 2));  // partial unmap

  // Confirm that read fails.
  char buf[0x10000] = {};
  errno = 0;
  EXPECT_EQ(-1, stream->read(buf, sizeof(buf)));
  EXPECT_EQ(EBADF, errno);

  // Clean up.
  EXPECT_EQ(0, stream->munmap(static_cast<uint8_t*>(mapped) + (size / 2),
                              size / 2));
}

// Test mmap-write-munmap-mmap-read case.
TEST_F(DevAshmemTest, TestAshmemMapUnmapMap) {
  scoped_refptr<FileStream> stream =
      handler_->open(512, "/dev/ashmem", O_RDWR, 0);
  ASSERT_TRUE(stream != NULL);

  const size_t size = 0x10000;  // 64kb
  int result = CallIoctl(stream, ASHMEM_SET_SIZE, size);
  EXPECT_EQ(0, result);

  void* mapped = stream->mmap(NULL, size, PROT_WRITE, MAP_SHARED, 0);
  ASSERT_NE(MAP_FAILED, mapped);
  // Touch the memory, then full-unmap.
  static_cast<uint8_t*>(mapped)[0] = 0x1;
  static_cast<uint8_t*>(mapped)[size - 1] = 0x1;
  EXPECT_EQ(0, stream->munmap(mapped, size));

  mapped = stream->mmap(NULL, size, PROT_READ, MAP_SHARED, 0);
  ASSERT_NE(MAP_FAILED, mapped);
  // Confirm that the 0x1 writes are still visible.
  EXPECT_EQ(0x1, static_cast<uint8_t*>(mapped)[0]);
  EXPECT_EQ(0x1, static_cast<uint8_t*>(mapped)[size - 1]);
  EXPECT_EQ(0, stream->munmap(mapped, size));
}

// Confirm mmap with two partial munmap calls works.
TEST_F(DevAshmemTest, TestAshmemMapUnmapTwice) {
  scoped_refptr<FileStream> stream =
      handler_->open(512, "/dev/ashmem", O_RDWR, 0);
  ASSERT_TRUE(stream != NULL);

  int result = CallIoctl(stream, ASHMEM_SET_NAME, "arc-ashmem");
  EXPECT_EQ(0, result);
  const size_t size = 0x200000;  // 2MB
  result = CallIoctl(stream, ASHMEM_SET_SIZE, size);
  EXPECT_EQ(0, result);
  void* mapped =
      stream->mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, 0);
  ASSERT_NE(MAP_FAILED, mapped);
  // Touch the memory
  static_cast<uint8_t*>(mapped)[0] = 0x1;
  static_cast<uint8_t*>(mapped)[size - 1] = 0x1;

  // Clean up.
  EXPECT_EQ(0, stream->munmap(mapped, size / 2));
  // Touch the memory again to confirm that the last 1MB region has not been
  // unmapped yet
  static_cast<uint8_t*>(mapped)[size - 1] = 0x1;
  EXPECT_EQ(0, stream->munmap(
      static_cast<uint8_t*>(mapped) + (size / 2), size / 2));
}

// Confirm that two memory regions returned from two mmap(MAP_SHARED) calls
// actually point to the same physical memory.
TEST_F(DevAshmemTest, TestAshmemSharedMmapTwice) {
  scoped_refptr<FileStream> stream =
      handler_->open(512, "/dev/ashmem", O_RDWR, 0);
  ASSERT_TRUE(stream != NULL);

  int result = CallIoctl(stream, ASHMEM_SET_NAME, "arc-ashmem");
  EXPECT_EQ(0, result);
  const size_t size = 0x200000;  // 2MB
  result = CallIoctl(stream, ASHMEM_SET_SIZE, size);
  EXPECT_EQ(0, result);
  void* mapped =
      stream->mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, 0);
  ASSERT_NE(MAP_FAILED, mapped);
  static_cast<uint8_t*>(mapped)[0] = 0x1;  // write to |mapped|

  // Call mmap again.
  void* mapped2 =
      stream->mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, 0);
  ASSERT_NE(MAP_FAILED, mapped2);

  // Two mmap calls must not return the same address.
  // Note: Currently this test fails because stream->mmap() returns the same
  //       address twice which is not POSIX compatible.
  // EXPECT_NE(mapped, mapped2);

  // Two memory regions should share the content.
  EXPECT_EQ(0x1U, static_cast<uint8_t*>(mapped2)[0]);  // read from mapped2
  static_cast<uint8_t*>(mapped2)[1] = 0xf;  // write to mapped2
  EXPECT_EQ(0xfU, static_cast<uint8_t*>(mapped)[1]);  // read from mapped

  // Clean up.
  EXPECT_EQ(0, stream->munmap(mapped2, size));
  EXPECT_EQ(0, stream->munmap(mapped, size));
}

// Confirm that two memory regions returned from two mmap(MAP_PRIVATE) calls
// do NOT point to the same physical memory.
TEST_F(DevAshmemTest, TestAshmemPrivateMmapTwice) {
  scoped_refptr<FileStream> stream =
      handler_->open(512, "/dev/ashmem", O_RDWR, 0);
  ASSERT_TRUE(stream != NULL);

  const size_t size = 0x10000;  // 64k
  int result = CallIoctl(stream, ASHMEM_SET_SIZE, size);
  EXPECT_EQ(0, result);
  void* mapped =
      stream->mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE, 0);
  ASSERT_NE(MAP_FAILED, mapped);
  static_cast<uint8_t*>(mapped)[0] = 0x1;  // write to |mapped|

  // Call mmap again.
  void* mapped2 =
      stream->mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE, 0);
  ASSERT_NE(MAP_FAILED, mapped2);

  EXPECT_NE(mapped, mapped2);

  // Two memory regions should NOT share the content.
  EXPECT_EQ(0x0U, static_cast<uint8_t*>(mapped2)[0]);  // read from mapped2
  static_cast<uint8_t*>(mapped2)[1] = 0xf;  // write to mapped2
  EXPECT_EQ(0x0U, static_cast<uint8_t*>(mapped)[1]);  // read from mapped

  // Clean up.
  EXPECT_EQ(0, stream->munmap(mapped2, size));
  EXPECT_EQ(0, stream->munmap(mapped, size));
}

// Test the case where mmap(MAP_SHARED) and mmap(MAP_PRIVATE) are mixed.
TEST_F(DevAshmemTest, TestAshmemSharedMmapAndPrivateMmapMixed) {
  scoped_refptr<FileStream> stream =
      handler_->open(512, "/dev/ashmem", O_RDWR, 0);
  ASSERT_TRUE(stream != NULL);

  const size_t size = 0x10000;  // 64k
  int result = CallIoctl(stream, ASHMEM_SET_SIZE, size);
  EXPECT_EQ(0, result);
  void* mapped =
      stream->mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, 0);
  ASSERT_NE(MAP_FAILED, mapped);
  static_cast<uint8_t*>(mapped)[0] = 0x1;  // write to |mapped|

  // Call mmap again. The 0x1 write should NOT be visible to the private
  // mapping to emulate the kernel.
  void* mapped2 =
      stream->mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE, 0);
  ASSERT_NE(MAP_FAILED, mapped2);

  EXPECT_NE(mapped, mapped2);

  // Two memory regions should NOT share the content.
  EXPECT_EQ(0x0U, static_cast<uint8_t*>(mapped2)[0]);  // read from mapped2
  static_cast<uint8_t*>(mapped2)[1] = 0xf;  // write to mapped2
  EXPECT_EQ(0x0U, static_cast<uint8_t*>(mapped)[1]);  // read from mapped

  // Clean up.
  EXPECT_EQ(0, stream->munmap(mapped2, size));
  EXPECT_EQ(0, stream->munmap(mapped, size));
}

// Confirm that mmap(MAP_PRIVATE) with a page-aligned offset succeeds.
TEST_F(DevAshmemTest, TestAshmemSPrivateMmapWithOffset) {
  scoped_refptr<FileStream> stream =
      handler_->open(512, "/dev/ashmem", O_RDWR, 0);
  ASSERT_TRUE(stream != NULL);

  const size_t size = 0x20000;  // 128k
  int result = CallIoctl(stream, ASHMEM_SET_SIZE, size);
  EXPECT_EQ(0, result);
  void* mapped =
      stream->mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE, size / 2);
  ASSERT_NE(MAP_FAILED, mapped);
  static_cast<uint8_t*>(mapped)[0] = 0x1;  // write to |mapped|

  // Clean up.
  EXPECT_EQ(0, stream->munmap(mapped, size / 2));
}

// Confirm that mmap(MAP_SHARED) with a page-aligned offset fails. This is just
// a limitation of the current DevAshmem implementation. The real /dev/ashmem
// in the kernel does not have the limitation.
TEST_F(DevAshmemTest, TestAshmemSSharedMmapWithOffset) {
  scoped_refptr<FileStream> stream =
      handler_->open(512, "/dev/ashmem", O_RDWR, 0);
  ASSERT_TRUE(stream != NULL);

  const size_t size = 0x20000;  // 128k
  int result = CallIoctl(stream, ASHMEM_SET_SIZE, size);
  EXPECT_EQ(0, result);
  void* mapped =
      stream->mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, size / 2);
  EXPECT_EQ(MAP_FAILED, mapped);
}

// Call mmap(MAP_PRIVATE) twice with different sizes.
TEST_F(DevAshmemTest, TestAshmemPrivateMmapTwiceDifferentSizes) {
  scoped_refptr<FileStream> stream =
      handler_->open(512, "/dev/ashmem", O_RDWR, 0);
  ASSERT_TRUE(stream != NULL);

  const size_t size = 0x20000;  // 128k
  int result = CallIoctl(stream, ASHMEM_SET_SIZE, size);
  EXPECT_EQ(0, result);
  void* mapped =
      stream->mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE, 0);
  ASSERT_NE(MAP_FAILED, mapped);

  // Call mmap again.
  void* mapped2 =
      stream->mmap(NULL, size / 2, PROT_READ | PROT_WRITE, MAP_PRIVATE, 0);
  ASSERT_NE(MAP_FAILED, mapped2);

  EXPECT_NE(mapped, mapped2);

  // Clean up.
  EXPECT_EQ(0, stream->munmap(mapped2, size / 2));
  EXPECT_EQ(0, stream->munmap(mapped, size));
}

// Call mmap(MAP_SHARED) twice with different sizes. This fails, but this is
// just a limitation of the current DevAshmem implementation. The real
// /dev/ashmem in the kernel does not have the limitation.
TEST_F(DevAshmemTest, TestAshmemSharedMmapTwiceDifferentSizes) {
  scoped_refptr<FileStream> stream =
      handler_->open(512, "/dev/ashmem", O_RDWR, 0);
  ASSERT_TRUE(stream != NULL);

  const size_t size = 0x20000;  // 128k
  int result = CallIoctl(stream, ASHMEM_SET_SIZE, size);
  EXPECT_EQ(0, result);
  void* mapped =
      stream->mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, 0);
  ASSERT_NE(MAP_FAILED, mapped);

  // Call mmap again.
  void* mapped2 =
      stream->mmap(NULL, size / 2, PROT_READ | PROT_WRITE, MAP_SHARED, 0);
  EXPECT_EQ(MAP_FAILED, mapped2);

  // Clean up.
  EXPECT_EQ(0, stream->munmap(mapped, size));
}

// Call mmap(MAP_SHARED) twice with and without MAP_FIXED.
TEST_F(DevAshmemTest, TestAshmemSharedMmapTwiceWithAndWithoutMapFixed) {
  scoped_refptr<FileStream> stream =
      handler_->open(512, "/dev/ashmem", O_RDWR, 0);
  ASSERT_TRUE(stream != NULL);

  const size_t size = 0x20000;  // 128k
  int result = CallIoctl(stream, ASHMEM_SET_SIZE, size);
  EXPECT_EQ(0, result);
  void* mapped =
      stream->mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, 0);
  ASSERT_NE(MAP_FAILED, mapped);

  // Call mmap again with MAP_FIXED and with the same address, |mapped|.
  void* mapped2 = stream->mmap(
      mapped, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, 0);
  ASSERT_NE(MAP_FAILED, mapped2);

  // Clean up.
  EXPECT_EQ(0, stream->munmap(mapped, size));
}

// Call mmap(MAP_SHARED) twice with and without MAP_FIXED. This time,
// use a different address with MAP_FIXED. This fails, but this is
// just a limitation of the current DevAshmem implementation. The real
// /dev/ashmem in the kernel does not have the limitation.
TEST_F(DevAshmemTest, TestAshmemSharedMmapTwiceWithAndWithoutMapFixed2) {
  scoped_refptr<FileStream> stream =
      handler_->open(512, "/dev/ashmem", O_RDWR, 0);
  ASSERT_TRUE(stream != NULL);

  const size_t size = 0x20000;  // 128k
  int result = CallIoctl(stream, ASHMEM_SET_SIZE, size);
  EXPECT_EQ(0, result);
  void* mapped =
      stream->mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, 0);
  ASSERT_NE(MAP_FAILED, mapped);

  // Call mmap again with MAP_FIXED and with the same address, |mapped|.
  void* mapped2 = stream->mmap(
      static_cast<uint8_t*>(mapped) + 0x10000,
      size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, 0);
  ASSERT_EQ(MAP_FAILED, mapped2);

  // Clean up.
  EXPECT_EQ(0, stream->munmap(mapped, size));
}

// Call mmap(MAP_SHARED) after partial munmap. This fails, but this is
// just a limitation of the current DevAshmem implementation. The real
// /dev/ashmem in the kernel does not have the limitation.
TEST_F(DevAshmemTest, TestAshmemSharedMmapAfterPartialMunmap) {
  scoped_refptr<FileStream> stream =
      handler_->open(512, "/dev/ashmem", O_RDWR, 0);
  ASSERT_TRUE(stream != NULL);

  const size_t size = 0x20000;  // 128k
  int result = CallIoctl(stream, ASHMEM_SET_SIZE, size);
  EXPECT_EQ(0, result);
  void* mapped =
      stream->mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, 0);
  ASSERT_NE(MAP_FAILED, mapped);
  EXPECT_EQ(0, stream->munmap(mapped, size / 2));

  // Call mmap again.
  void* mapped2 =
      stream->mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, 0);
  ASSERT_EQ(MAP_FAILED, mapped2);

  // Clean up.
  EXPECT_EQ(0, stream->munmap(static_cast<uint8_t*>(mapped) + (size / 2),
                              size / 2));
}

// Other MAP_FIXED cases are tested in
// src/integration_tests/posix_translation_ndk/jni/mmap_tests.cc.
// because we need to test the interaction between memory_region.cc and
// FileStreams too.

}  // namespace posix_translation
