// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <arpa/inet.h>  // htonl

#include <string>

#include "base/compiler_specific.h"
#include "base/memory/scoped_ptr.h"
#include "base/strings/string_util.h"
#include "gtest/gtest.h"
#include "posix_translation/dir.h"
#include "posix_translation/readonly_fs_reader.h"
#include "posix_translation/readonly_fs_reader_test.h"
#include "posix_translation/test_util/file_system_test_common.h"
#include "posix_translation/test_util/mmap_util.h"

namespace posix_translation {

class ReadonlyFsReaderTest : public FileSystemTestCommon {
 protected:
  ReadonlyFsReaderTest() : cc_factory_(this) {
  }

  virtual ~ReadonlyFsReaderTest() {
  }

  virtual void SetUp() OVERRIDE {
    FileSystemTestCommon::SetUp();

    std::string test_filename = ARC_TARGET_PATH
        "/posix_translation_fs_images/test_readonly_fs_image.img";
    ASSERT_TRUE(test_image_.Init(test_filename));
    reader_.reset(new ReadonlyFsReader(
        reinterpret_cast<const unsigned char*>(test_image_.data())));

#if defined(PROD_READONLY_FS_IMAGE)
    std::string prod_filename = PROD_READONLY_FS_IMAGE;
    ASSERT_TRUE(prod_image_.Init(prod_filename));
    reader_prod_.reset(new ReadonlyFsReader(
        reinterpret_cast<const unsigned char*>(prod_image_.data())));
#endif
  }

  const ReadonlyFsReader::Metadata* FindFile(
      const ReadonlyFsReader::FileToMemory& files,
      const std::string& file_to_find) {
    for (ReadonlyFsReader::FileToMemory::const_iterator it = files.begin();
         it != files.end(); ++it) {
      if (it->first.find(file_to_find) != std::string::npos)
        return &it->second;
    }
    return NULL;
  }

  const ReadonlyFsReader::Metadata* FindNonVendorLibraries(
      const ReadonlyFsReader::FileToMemory& files) {
    const std::string kSo(".so");
    const std::string kVendorLibPrefix("/vendor/lib");
    for (ReadonlyFsReader::FileToMemory::const_iterator it = files.begin();
         it != files.end(); ++it) {
      if (EndsWith(it->first, kSo, true) &&
          !StartsWithASCII(it->first, kVendorLibPrefix, true))
        return &it->second;
    }
    return NULL;
  }

  static const unsigned char* ReadUInt32BE(const unsigned char* p,
                                           uint32_t* out_result) {
    return ReadonlyFsReader::ReadUInt32BE(p, out_result);
  }

  pp::CompletionCallbackFactory<ReadonlyFsReaderTest> cc_factory_;
  scoped_ptr<ReadonlyFsReader> reader_;
  scoped_ptr<ReadonlyFsReader> reader_prod_;
  MmappedFile prod_image_;
  MmappedFile test_image_;

 private:
  DISALLOW_COPY_AND_ASSIGN(ReadonlyFsReaderTest);
};

TEST_F(ReadonlyFsReaderTest, TestReadUInt32BE) {
  static const uint32_t kValue = 0x12345678;

  const uint32_t value_big_endian = htonl(kValue);
  const unsigned char* p =
    // Casting to char* does not violate the -fstrict-aliasing rule.
    reinterpret_cast<const unsigned char*>(&value_big_endian);

  uint32_t result = 0;
  EXPECT_EQ(p + sizeof(uint32_t), ReadUInt32BE(p, &result));
  EXPECT_EQ(kValue, result);
}

TEST_F(ReadonlyFsReaderTest, TestReadUInt32BEUnaligned) {
  static const unsigned char buf[7] = {};
  for (size_t i = 0; i < 4; ++i) {
    uint32_t result = 0x12345678;
    // Make sure ReadUInt32BE() does not crash.
    ReadUInt32BE(&buf[i], &result);
    EXPECT_EQ(0U, result);
  }
}

TEST_F(ReadonlyFsReaderTest, TestAlignTo) {
  static const size_t kNaCl64PageSize = 64 * 1024;

  uintptr_t p = 0;
  uintptr_t p_aligned = 0;
  EXPECT_EQ(reinterpret_cast<void*>(p_aligned),
            reader_->AlignTo(reinterpret_cast<void*>(p), kNaCl64PageSize));
  p = 1;
  p_aligned = kNaCl64PageSize;
  EXPECT_EQ(reinterpret_cast<void*>(p_aligned),
            reader_->AlignTo(reinterpret_cast<void*>(p), kNaCl64PageSize));
  p = p_aligned - 1;
  EXPECT_EQ(reinterpret_cast<void*>(p_aligned),
            reader_->AlignTo(reinterpret_cast<void*>(p), kNaCl64PageSize));
  p = p_aligned;
  EXPECT_EQ(reinterpret_cast<void*>(p_aligned),
            reader_->AlignTo(reinterpret_cast<void*>(p), kNaCl64PageSize));
  p = p_aligned + 1;
  EXPECT_EQ(reinterpret_cast<void*>(p_aligned * 2),
            reader_->AlignTo(reinterpret_cast<void*>(p), kNaCl64PageSize));
  p = p_aligned * 2 - 1;
  EXPECT_EQ(reinterpret_cast<void*>(p_aligned * 2),
            reader_->AlignTo(reinterpret_cast<void*>(p), kNaCl64PageSize));
  p = p_aligned * 2;
  EXPECT_EQ(reinterpret_cast<void*>(p_aligned * 2),
            reader_->AlignTo(reinterpret_cast<void*>(p), kNaCl64PageSize));
  p = p_aligned * 2 + 1;
  EXPECT_EQ(reinterpret_cast<void*>(p_aligned * 3),
            reader_->AlignTo(reinterpret_cast<void*>(p), kNaCl64PageSize));
}

TEST_F(ReadonlyFsReaderTest, TestExist) {
  EXPECT_TRUE(reader_->Exist(""));
  EXPECT_TRUE(reader_->Exist("/"));
  EXPECT_FALSE(reader_->Exist("/tes"));
  EXPECT_TRUE(reader_->Exist("/test"));
  EXPECT_FALSE(reader_->Exist("/testa"));
  EXPECT_TRUE(reader_->Exist("/test/"));
  EXPECT_FALSE(reader_->Exist("/test/a.ode"));
  EXPECT_TRUE(reader_->Exist("/test/a.odex"));
  EXPECT_FALSE(reader_->Exist("/test/a.odexa"));
  EXPECT_TRUE(reader_->Exist("/test/c.odex"));
  EXPECT_TRUE(reader_->Exist("/test/c0.odex"));
  EXPECT_FALSE(reader_->Exist("/test/c1.odex"));
  EXPECT_TRUE(reader_->Exist("/test/dir"));
  EXPECT_TRUE(reader_->Exist("/test/dir/"));
  EXPECT_TRUE(reader_->Exist("/test/dir/empty.odex"));
  EXPECT_FALSE(reader_->Exist("/test/dir/empty.odexa"));
  EXPECT_TRUE(reader_->Exist("/test/emptydir"));
  EXPECT_TRUE(reader_->Exist("/test/emptydir/"));
  EXPECT_TRUE(reader_->Exist("/test/emptyfile"));
  EXPECT_TRUE(reader_->Exist("/test/symlink1"));
  EXPECT_TRUE(reader_->Exist("/test/symlink2"));
  EXPECT_FALSE(reader_->Exist("/test/symlink3"));
}

TEST_F(ReadonlyFsReaderTest, TestIsDirectory) {
  EXPECT_TRUE(reader_->IsDirectory(""));
  EXPECT_TRUE(reader_->IsDirectory("/"));
  EXPECT_TRUE(reader_->IsDirectory("/test"));
  EXPECT_TRUE(reader_->IsDirectory("/test/"));
  EXPECT_FALSE(reader_->IsDirectory("/test/a.odex"));
  EXPECT_TRUE(reader_->IsDirectory("/test/dir"));
  EXPECT_TRUE(reader_->IsDirectory("/test/dir/"));
  EXPECT_TRUE(reader_->IsDirectory("/test/emptydir"));
  EXPECT_TRUE(reader_->IsDirectory("/test/emptydir/"));
  EXPECT_FALSE(reader_->IsDirectory("/test/emptyfile"));
  EXPECT_FALSE(reader_->IsDirectory("/test/dir/empty.odex"));
  EXPECT_FALSE(reader_->IsDirectory("/test/symlink1"));
}

TEST_F(ReadonlyFsReaderTest, TestGetMetadata) {
  ReadonlyFsReader::Metadata metadata;
  EXPECT_FALSE(reader_->GetMetadata("", &metadata));
  EXPECT_FALSE(reader_->GetMetadata("/", &metadata));
  EXPECT_FALSE(reader_->GetMetadata("/tes", &metadata));
  EXPECT_FALSE(reader_->GetMetadata("/test", &metadata));
  EXPECT_FALSE(reader_->GetMetadata("/testa", &metadata));
  EXPECT_FALSE(reader_->GetMetadata("/test/", &metadata));
  EXPECT_FALSE(reader_->GetMetadata("/test/a.ode", &metadata));

  EXPECT_TRUE(reader_->GetMetadata("/test/a.odex", &metadata));
  EXPECT_EQ(4U, metadata.size);
  EXPECT_LT(0L, metadata.mtime);
  EXPECT_EQ(ReadonlyFsReader::kRegularFile, metadata.file_type);

  EXPECT_FALSE(reader_->GetMetadata("/test/a.odexa", &metadata));

  EXPECT_TRUE(reader_->GetMetadata("/test/c.odex", &metadata));
  EXPECT_EQ(0U, metadata.size);
  EXPECT_LT(0L, metadata.mtime);
  EXPECT_EQ(ReadonlyFsReader::kRegularFile, metadata.file_type);

  EXPECT_TRUE(reader_->GetMetadata("/test/c0.odex", &metadata));
  EXPECT_EQ(0U, metadata.size);
  EXPECT_LT(0L, metadata.mtime);
  EXPECT_EQ(ReadonlyFsReader::kRegularFile, metadata.file_type);

  EXPECT_FALSE(reader_->GetMetadata("/test/c1.odex", &metadata));
  EXPECT_FALSE(reader_->GetMetadata("/test/dir", &metadata));
  EXPECT_FALSE(reader_->GetMetadata("/test/dir/", &metadata));

  EXPECT_TRUE(reader_->GetMetadata("/test/dir/empty.odex", &metadata));
  EXPECT_EQ(0U, metadata.size);
  EXPECT_LT(0L, metadata.mtime);
  EXPECT_EQ(ReadonlyFsReader::kRegularFile, metadata.file_type);

  EXPECT_FALSE(reader_->GetMetadata("/test/dir/empty.odexa", &metadata));

  EXPECT_FALSE(reader_->GetMetadata("/test/emptydir", &metadata));
  EXPECT_FALSE(reader_->GetMetadata("/test/emptydir/", &metadata));

  EXPECT_TRUE(reader_->GetMetadata("/test/emptyfile", &metadata));
  EXPECT_EQ(0U, metadata.size);
  EXPECT_LT(0L, metadata.mtime);
  EXPECT_EQ(ReadonlyFsReader::kRegularFile, metadata.file_type);

  EXPECT_TRUE(reader_->GetMetadata("/test/symlink1", &metadata));
  EXPECT_EQ(0U, metadata.size);
  EXPECT_LT(0L, metadata.mtime);
  EXPECT_EQ(ReadonlyFsReader::kSymbolicLink, metadata.file_type);
  EXPECT_EQ("/test/a.odex", metadata.link_target);

  EXPECT_TRUE(reader_->GetMetadata("/test/symlink2", &metadata));
  EXPECT_EQ(0U, metadata.size);
  EXPECT_LT(0L, metadata.mtime);
  EXPECT_EQ(ReadonlyFsReader::kSymbolicLink, metadata.file_type);
  EXPECT_EQ("/test/b.odex", metadata.link_target);
}

TEST_F(ReadonlyFsReaderTest, TestOpenReadCloseDir) {
  static const Dir* kNullDir = NULL;

  // Try to scan "/".
  scoped_ptr<Dir> dirp(reader_->OpenDirectory("/"));
  ASSERT_NE(kNullDir, dirp.get());
  dirent entry;
  ASSERT_TRUE(dirp->GetNext(&entry));
  EXPECT_EQ(DT_DIR, entry.d_type);
  EXPECT_EQ(std::string("."), entry.d_name);
  EXPECT_NE(0U, entry.d_ino);
  ASSERT_TRUE(dirp->GetNext(&entry));
  EXPECT_EQ(DT_DIR, entry.d_type);
  EXPECT_EQ(std::string(".."), entry.d_name);
  EXPECT_NE(0U, entry.d_ino);
  ASSERT_TRUE(dirp->GetNext(&entry));
  EXPECT_EQ(DT_DIR, entry.d_type);
  EXPECT_EQ(std::string("test"), entry.d_name);
  EXPECT_NE(0U, entry.d_ino);
  ASSERT_FALSE(dirp->GetNext(&entry));

  // Try to scan "/test".
  dirp.reset(reader_->OpenDirectory("/test"));
  ASSERT_NE(kNullDir, dirp.get());
  ASSERT_TRUE(dirp->GetNext(&entry));
  EXPECT_EQ(DT_DIR, entry.d_type);
  EXPECT_EQ(std::string("."), entry.d_name);
  EXPECT_NE(0U, entry.d_ino);
  ASSERT_TRUE(dirp->GetNext(&entry));
  EXPECT_EQ(DT_DIR, entry.d_type);
  EXPECT_EQ(std::string(".."), entry.d_name);
  EXPECT_NE(0U, entry.d_ino);
  ASSERT_TRUE(dirp->GetNext(&entry));
  EXPECT_EQ(DT_REG, entry.d_type);
  EXPECT_EQ(std::string("a.odex"), entry.d_name);
  EXPECT_NE(0U, entry.d_ino);
  ASSERT_TRUE(dirp->GetNext(&entry));
  EXPECT_EQ(DT_REG, entry.d_type);
  EXPECT_EQ(std::string("b.odex"), entry.d_name);
  EXPECT_NE(0U, entry.d_ino);
  ASSERT_TRUE(dirp->GetNext(&entry));
  EXPECT_EQ(DT_REG, entry.d_type);
  EXPECT_EQ(std::string("big.odex"), entry.d_name);
  EXPECT_NE(0U, entry.d_ino);
  ASSERT_TRUE(dirp->GetNext(&entry));
  EXPECT_EQ(DT_REG, entry.d_type);
  EXPECT_EQ(std::string("c.odex"), entry.d_name);
  EXPECT_NE(0U, entry.d_ino);
  ASSERT_TRUE(dirp->GetNext(&entry));
  EXPECT_EQ(DT_REG, entry.d_type);
  EXPECT_EQ(std::string("c0.odex"), entry.d_name);
  EXPECT_NE(0U, entry.d_ino);
  ASSERT_TRUE(dirp->GetNext(&entry));
  EXPECT_EQ(DT_DIR, entry.d_type);
  EXPECT_EQ(std::string("dir"), entry.d_name);
  EXPECT_NE(0U, entry.d_ino);
  ASSERT_TRUE(dirp->GetNext(&entry));
  EXPECT_EQ(std::string("emptydir"), entry.d_name);
  EXPECT_NE(0U, entry.d_ino);
  ASSERT_TRUE(dirp->GetNext(&entry));
  EXPECT_EQ(std::string("emptyfile"), entry.d_name);
  EXPECT_NE(0U, entry.d_ino);
  ASSERT_TRUE(dirp->GetNext(&entry));
  EXPECT_EQ(std::string("symlink1"), entry.d_name);
  EXPECT_NE(0U, entry.d_ino);
  ASSERT_TRUE(dirp->GetNext(&entry));
  EXPECT_EQ(std::string("symlink2"), entry.d_name);
  EXPECT_NE(0U, entry.d_ino);
  ASSERT_FALSE(dirp->GetNext(&entry));

  // Try to scan "/test/dir".
  dirp.reset(reader_->OpenDirectory("/test/dir/"));
  ASSERT_NE(kNullDir, dirp.get());
  ASSERT_TRUE(dirp->GetNext(&entry));
  EXPECT_EQ(DT_DIR, entry.d_type);
  EXPECT_EQ(std::string("."), entry.d_name);
  EXPECT_NE(0U, entry.d_ino);
  ASSERT_TRUE(dirp->GetNext(&entry));
  EXPECT_EQ(DT_DIR, entry.d_type);
  EXPECT_EQ(std::string(".."), entry.d_name);
  EXPECT_NE(0U, entry.d_ino);
  ASSERT_TRUE(dirp->GetNext(&entry));
  EXPECT_EQ(DT_REG, entry.d_type);
  EXPECT_EQ(std::string("c.odex"), entry.d_name);
  EXPECT_NE(0U, entry.d_ino);
  ASSERT_TRUE(dirp->GetNext(&entry));
  EXPECT_EQ(DT_REG, entry.d_type);
  EXPECT_EQ(std::string("empty.odex"), entry.d_name);
  EXPECT_NE(0U, entry.d_ino);
  ASSERT_FALSE(dirp->GetNext(&entry));

  // Try to scan unknown dir, "/test/dirX".
  dirp.reset(reader_->OpenDirectory("/test/dirX"));
  EXPECT_EQ(kNullDir, dirp.get());
}

TEST_F(ReadonlyFsReaderTest, TestParseImage) {
  EXPECT_EQ(kNumTestFiles, reader_->file_objects_.size());
  ReadonlyFsReader::FileToMemory::const_iterator it =
    reader_->file_objects_.find(kTestFiles[0].filename);
  ASSERT_TRUE(it != reader_->file_objects_.end());
  ASSERT_EQ(kTestFiles[0].size, it->second.size);
  EXPECT_LT(0L, it->second.mtime);
  const char* file_head = test_image_.data() + it->second.offset;
  EXPECT_EQ('1', file_head[0]);
  EXPECT_EQ('2', file_head[1]);
  EXPECT_EQ('3', file_head[2]);
  EXPECT_EQ('\n', file_head[3]);

  it = reader_->file_objects_.find(kTestFiles[1].filename);
  ASSERT_TRUE(it != reader_->file_objects_.end());
  ASSERT_EQ(kTestFiles[1].size, it->second.size);
  EXPECT_LT(0L, it->second.mtime);
  file_head = test_image_.data() + it->second.offset;
  for (size_t i = 0; i < kTestFiles[1].size; ++i) {
    // See scripts/create_test_fs_image.py for the magic numbers.
    if (i < 90000)
      ASSERT_EQ(0x0, file_head[i]) << i;
    else
      ASSERT_EQ('X', file_head[i]) << i;
  }
  it = reader_->file_objects_.find(kTestFiles[2].filename);
  ASSERT_TRUE(it != reader_->file_objects_.end());
  ASSERT_EQ(kTestFiles[2].size, it->second.size);
  EXPECT_LT(0L, it->second.mtime);
  file_head = test_image_.data() + it->second.offset;
  EXPECT_EQ('Z', file_head[0]);

  it = reader_->file_objects_.find(kTestFiles[3].filename);
  ASSERT_TRUE(it != reader_->file_objects_.end());
  ASSERT_EQ(kTestFiles[3].size, it->second.size);  // empty file
  EXPECT_LT(0L, it->second.mtime);

  it = reader_->file_objects_.find(kTestFiles[4].filename);
  ASSERT_TRUE(it != reader_->file_objects_.end());
  ASSERT_EQ(kTestFiles[4].size, it->second.size);  // empty file
  EXPECT_LT(0L, it->second.mtime);

  it = reader_->file_objects_.find(kTestFiles[5].filename);
  ASSERT_TRUE(it != reader_->file_objects_.end());
  ASSERT_EQ(kTestFiles[5].size, it->second.size);
  EXPECT_LT(0L, it->second.mtime);
  file_head = test_image_.data() + it->second.offset;
  EXPECT_EQ('A', file_head[0]);

  it = reader_->file_objects_.find(kTestFiles[6].filename);
  ASSERT_TRUE(it != reader_->file_objects_.end());
  ASSERT_EQ(kTestFiles[6].size, it->second.size);  // empty file
  EXPECT_LT(0L, it->second.mtime);

  it = reader_->file_objects_.find("test/a.odex");
  EXPECT_TRUE(it == reader_->file_objects_.end());
  it = reader_->file_objects_.find("/test/a.ode");
  EXPECT_TRUE(it == reader_->file_objects_.end());
  it = reader_->file_objects_.find("/test");
  EXPECT_TRUE(it == reader_->file_objects_.end());
  it = reader_->file_objects_.find("does_not_exist");
  EXPECT_TRUE(it == reader_->file_objects_.end());
}

// Test if the production rootfs img is valid.
TEST_F(ReadonlyFsReaderTest, TestParseImageProd) {
#if !defined(PROD_READONLY_FS_IMAGE)
  // arc-wasm: the production image is built from Android binaries that are
  // not part of the open-source ARC tree.
  GTEST_SKIP() << "PROD_READONLY_FS_IMAGE is not available";
#endif
  // Note: Do not check files that are not open sourced.
  // Otherwise nacl-i686-weird builder will fail.

  EXPECT_GT(reader_prod_->file_objects_.size(), 0U);
  // These files should exist in the image.
  EXPECT_TRUE(FindFile(reader_prod_->file_objects_, "/proc/version"));
  EXPECT_TRUE(FindFile(reader_prod_->file_objects_, "/proc/meminfo"));
  const ReadonlyFsReader::Metadata* metadata =
      FindFile(reader_prod_->file_objects_, "/proc/loadavg");
  ASSERT_TRUE(metadata);
  EXPECT_EQ(27U, metadata->size);

  const char* file_head = prod_image_.data() + metadata->offset;
  EXPECT_EQ(std::string("0.00 0.00 0.00 1/279 22477\n"),
            std::string(file_head, metadata->size));
  EXPECT_TRUE(FindFile(reader_prod_->file_objects_, "/system/bin/sh"));
  EXPECT_TRUE(FindFile(reader_prod_->file_objects_,
                       "/system/usr/share/zoneinfo/tzdata"));
  // libRS.so is a canned library not built at all by ARC.
  // - ARM version is always provided.
  // - x86 version is provided only when NDK direct execution is enabled
  //   under x86.
  EXPECT_TRUE(FindFile(reader_prod_->file_objects_,
                       "/vendor/lib-armeabi-v7a/libRS.so"));
#if defined(__i386__) && defined(USE_NDK_DIRECT_EXECUTION)
  EXPECT_TRUE(FindFile(reader_prod_->file_objects_,
                       "/vendor/lib-x86/libRS.so"));
#else
  EXPECT_FALSE(FindFile(reader_prod_->file_objects_,
                        "/vendor/lib-x86/libRS.so"));
#endif
  // libutils.so is needed by libRS.so. It is built by ARC, but we do not have
  // NDK trampolines yet.
  // - ARM version is provided only for non-ARM (x86) targets to satisfy
  //   dependency of libRS.so.
  // - x86 version is never provided in favor of a native version in
  //   /system/lib.
#if !defined(__arm__)
  EXPECT_TRUE(FindFile(reader_prod_->file_objects_,
                       "/vendor/lib-armeabi-v7a/libutils.so"));
#else
  EXPECT_FALSE(FindFile(reader_prod_->file_objects_,
                        "/vendor/lib-armeabi-v7a/libutils.so"));
#endif
  EXPECT_FALSE(FindFile(reader_prod_->file_objects_,
                        "/vendor/lib-x86/libutils.so"));
  // libstdc++.so is needed by libRS.so. Since it is built by ARC and NDK
  // trampolines for it exist, we do not need a canned binary in favor of
  // a native version in /system/lib.
  EXPECT_FALSE(FindFile(reader_prod_->file_objects_,
                        "/vendor/lib-armeabi-v7a/libstdc++.so"));
  EXPECT_FALSE(FindFile(reader_prod_->file_objects_,
                        "/vendor/lib-x86/libstdc++.so"));
  // These files should NOT exist in the image.
  EXPECT_FALSE(FindFile(reader_prod_->file_objects_, "root/proc/version"));
  EXPECT_FALSE(FindFile(reader_prod_->file_objects_, "intermediates/"));

  EXPECT_FALSE(FindFile(reader_prod_->file_objects_, "dexopt"));
  EXPECT_FALSE(FindNonVendorLibraries(reader_prod_->file_objects_));
  // These directories should exist in the image.
  EXPECT_TRUE(reader_prod_->IsDirectory("/cache"));
  EXPECT_TRUE(reader_prod_->IsDirectory("/data"));
  EXPECT_TRUE(reader_prod_->IsDirectory("/dev"));
  EXPECT_TRUE(reader_prod_->IsDirectory("/mnt"));
  EXPECT_TRUE(reader_prod_->IsDirectory("/proc"));
  EXPECT_TRUE(reader_prod_->IsDirectory("/sys"));
  EXPECT_TRUE(reader_prod_->IsDirectory("/sys/kernel/debug/tracing"));
  EXPECT_TRUE(reader_prod_->IsDirectory("/system"));
  EXPECT_TRUE(reader_prod_->IsDirectory("/system/bin"));
  EXPECT_TRUE(reader_prod_->IsDirectory("/system/lib"));
  EXPECT_TRUE(reader_prod_->IsDirectory("/usr/lib"));
  EXPECT_TRUE(reader_prod_->IsDirectory("/vendor/chromium/crx"));
  // These directories should NOT exist in the image.
  EXPECT_FALSE(reader_prod_->IsDirectory("/bin"));
  EXPECT_FALSE(reader_prod_->IsDirectory("/deva"));
  EXPECT_FALSE(reader_prod_->IsDirectory("/dev/foo"));
  EXPECT_FALSE(reader_prod_->IsDirectory("/syste"));
  EXPECT_FALSE(reader_prod_->IsDirectory("/usr/bin"));
  EXPECT_FALSE(reader_prod_->IsDirectory("/tmp"));
}

}  // namespace posix_translation
