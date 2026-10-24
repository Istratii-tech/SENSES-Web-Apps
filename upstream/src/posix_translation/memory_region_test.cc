// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <sys/mman.h>

#include "base/compiler_specific.h"
#include "gtest/gtest.h"
#include "posix_translation/memory_region.h"
#include "posix_translation/test_util/file_system_test_common.h"

// A macro for testing MemoryRegion whose public functions only accept addresses
// aligned to 2-byte boundary.
#define ALIGN_(n) __attribute__((__aligned__(n)))

namespace posix_translation {

class MemoryRegionTest : public FileSystemTestCommon {
 protected:
  virtual void SetUp() OVERRIDE {
    FileSystemTestCommon::SetUp();
    file_system_->memory_region_->abort_on_unexpected_memory_maps_ = false;
  }
  std::string GetMemoryMapAsString() {
    return file_system_->GetMemoryMapAsStringLocked();
  }
  bool IsMemoryRangeAvailable(void* addr, size_t length) {
    return file_system_->IsMemoryRangeAvailableLocked(addr, length);
  }
  bool AddFileStreamByAddr(void* addr, size_t length,
                           scoped_refptr<FileStream> stream) {
    return file_system_->memory_region_->AddFileStreamByAddr(
        addr, length, 0 /* offset */, PROT_READ, 0 /* flags */, stream);
  }
  bool AddFileStreamByAddrWithProt(void* addr, size_t length, int prot,
                                   scoped_refptr<FileStream> stream) {
    return file_system_->memory_region_->AddFileStreamByAddr(
        addr, length, 0 /* offset */, prot, 0 /* flags */, stream);
  }
  bool RemoveFileStreamsByAddr(void* addr, size_t length) {
    const int result = file_system_->memory_region_->RemoveFileStreamsByAddr(
        addr, length, true);
    if (result == -1 && errno == ENOSYS)
      return false;
    EXPECT_EQ(0, result);
    return true;
  }
  bool RemoveFileStreamsByAddrWithoutMunmap(void* addr, size_t length) {
    const int result = file_system_->memory_region_->RemoveFileStreamsByAddr(
        addr, length, false);
    if (result == -1 && errno == ENOSYS)
      return false;
    EXPECT_EQ(0, result);
    return true;
  }
  bool SetAdviceByAddr(void* addr, size_t length, int advice) {
    const int result = file_system_->memory_region_->SetAdviceByAddr(
        addr, length, advice);
    if (result == -1)
      return false;
    EXPECT_EQ(0, result);
    return true;
  }
  bool ChangeProtectionModeByAddr(void* addr, size_t length, int prot) {
    const int result = file_system_->memory_region_->ChangeProtectionModeByAddr(
        addr, length, prot);
    if (result == -1 && errno == ENOSYS)
      return false;
    EXPECT_EQ(0, result);
    return true;
  }
  bool IsWriteMapped(ino_t inode) {
    return file_system_->memory_region_->IsWriteMapped(inode);
  }
  bool IsCurrentlyMapped(ino_t inode) {
    return file_system_->memory_region_->IsCurrentlyMapped(inode);
  }
  bool IsPageEndAddress(const void* addr) {
    return MemoryRegion::IsPageEndAddress(addr);
  }
  void ClearAddrMap() {
    file_system_->memory_region_->map_.clear();
  }
  size_t GetAddrMapSize() const {
    return file_system_->memory_region_->map_.size();
  }

  // Returns true if a memory region [addr, addr+length) exists in the map.
  bool HasMemoryRegion(void* addr, size_t length) const {
    typedef MemoryRegion::PageToStream::iterator Iterator;
    char* addr_start = static_cast<char*>(addr);
    char* addr_end = static_cast<char*>(addr) + length - 1;
    Iterator it_start = file_system_->memory_region_->map_.find(addr_start);
    Iterator it_end = file_system_->memory_region_->map_.find(addr_end);
    Iterator not_found = file_system_->memory_region_->map_.end();
    return (it_start != not_found) && (it_end != not_found) &&
        (it_start->first < it_end->first) &&
        (std::distance(it_start, it_end) == 1);
  }
};

// A class template for TYPED_TEST_F. This template is instantiated N times
// with each type in |TestTypes| below.
template <typename T>
class MemoryRegionTypedTest : public MemoryRegionTest {
 protected:
  void TestAddStreamByAddr();
  void TestRemoveStreamByAddr();
  void TestModifyStreamByAddr();
};

namespace {

class StubFileStream : public FileStream {
 public:
  explicit StubFileStream(bool emulate_memory_file)
      : FileStream(0, ""),
        emulate_memory_file_(emulate_memory_file) {
    Reset();
  }

  // When you need FileStream::inode() to return a valid value, use this
  // constructor.
  explicit StubFileStream(const std::string& pathname)
      : FileStream(0, pathname),
        emulate_memory_file_(true) {
    Reset();
  }

  virtual bool ReturnsSameAddressForMultipleMmaps() const OVERRIDE {
    return emulate_memory_file_;
  }

  virtual int munmap(void* addr, size_t length) OVERRIDE {
    // Records the last |addr| and |length| of the unmapped region, that can
    // be referred from tests.
    last_munmap_addr = addr;
    last_munmap_length = length;
    ++munmap_count;
    return 0;
  }
  virtual int mprotect(void *addr, size_t length, int prot) OVERRIDE {
    // The same as above.
    last_mprotect_addr = addr;
    last_mprotect_length = length;
    last_mprotect_prot = prot;
    ++mprotect_count;
    return 0;
  }
  virtual ssize_t read(void*, size_t) OVERRIDE { return -1; }
  virtual ssize_t write(const void*, size_t) OVERRIDE { return -1; }
  virtual const char* GetStreamType() const OVERRIDE { return "stub"; }

  void Reset() {
    last_munmap_addr = MAP_FAILED;
    last_munmap_length = 0;
    munmap_count = 0;

    last_mprotect_addr = MAP_FAILED;
    last_mprotect_length = 0;
    last_mprotect_prot = 0;
    mprotect_count = 0;
  }

  void* last_munmap_addr;
  size_t last_munmap_length;
  size_t munmap_count;

  const void* last_mprotect_addr;
  size_t last_mprotect_length;
  int last_mprotect_prot;
  size_t mprotect_count;

 private:
  const bool emulate_memory_file_;
};

}  // namespace

// The size of the array must be >=2 and must be even.
typedef ::testing::Types<char[2], char[4], char[6], char[4096]> TestTypes;
TYPED_TEST_CASE(MemoryRegionTypedTest, TestTypes);

#define TYPED_TEST_F(_fixture, _test)                        \
  TYPED_TEST(_fixture, _test) {                              \
    this->_test();                                           \
  }                                                          \
  template <typename TypeParam>                              \
  void _fixture<TypeParam>::_test()

// Tests all corner cases of AddStreamByAddr.
TYPED_TEST_F(MemoryRegionTypedTest, TestAddStreamByAddr) {
  static const size_t kSize = sizeof(TypeParam);
  struct TestAddresses {
    TypeParam region0;  // TypeParam is char[N] (N = 2,4,..).
    TypeParam region1;
    TypeParam region2;
    TypeParam region3;
    TypeParam region4;
    TypeParam region5;
  } addresses ALIGN_(2);

  scoped_refptr<StubFileStream> stream = new StubFileStream(true);
  // First, insert region2.
  EXPECT_TRUE(AddFileStreamByAddr(addresses.region2, kSize, stream));
  EXPECT_TRUE(HasMemoryRegion(addresses.region2, kSize));

  // Try to insert regions which overlap |region2|. They should all fail.
  // except the "exactly the same" cases.

  // Exactly the same.
  EXPECT_TRUE(AddFileStreamByAddr(addresses.region2, kSize, stream));
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region2, kSize));
  // Left aligned.
  EXPECT_FALSE(AddFileStreamByAddr(addresses.region2, kSize - 2, stream));
  // Right aligned.
  if (kSize == 2) {
    EXPECT_TRUE(AddFileStreamByAddr(addresses.region2 + kSize - 2, 2, stream));
    EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region2 + kSize - 2, 2));
  } else {
    EXPECT_FALSE(AddFileStreamByAddr(addresses.region2 + kSize - 2, 2, stream));
  }
  // Overlaps left, right aligned.
  EXPECT_FALSE(AddFileStreamByAddr(
      addresses.region1 + (kSize - 2), kSize + 2, stream));
  // Overlaps right, left aligned.
  EXPECT_FALSE(AddFileStreamByAddr(addresses.region2, kSize + 2, stream));
  // Overlaps both.
  EXPECT_FALSE(AddFileStreamByAddr(
      addresses.region1 + (kSize - 2), kSize + 4, stream));
  if (kSize > 2) {
    // Overlaps left.
    EXPECT_FALSE(AddFileStreamByAddr(
        addresses.region1 + (kSize - 2), kSize, stream));
    // Overlaps right.
    EXPECT_FALSE(AddFileStreamByAddr(addresses.region2 + 2, kSize, stream));
    if (kSize > 4) {
      // Contained.
      EXPECT_FALSE(AddFileStreamByAddr(addresses.region2 + 2,
                                       kSize - 4, stream));
    }
  }
  // Confirm that AddFileStreamByAddr failures don't corrupt the tree.
  EXPECT_TRUE(HasMemoryRegion(addresses.region2, kSize));

  // Try to insert regions that don't overlap |region2|.
  EXPECT_TRUE(AddFileStreamByAddr(addresses.region0, kSize, stream));
  EXPECT_TRUE(HasMemoryRegion(addresses.region0, kSize));
  EXPECT_TRUE(AddFileStreamByAddr(addresses.region4, kSize, stream));
  EXPECT_TRUE(HasMemoryRegion(addresses.region4, kSize));
  // Add with length==0 should always fail.
  EXPECT_FALSE(AddFileStreamByAddr(addresses.region5, 0, stream));
  EXPECT_TRUE(AddFileStreamByAddr(addresses.region5, 2, stream));
  EXPECT_TRUE(HasMemoryRegion(addresses.region5, 2));
  EXPECT_TRUE(AddFileStreamByAddr(addresses.region1, kSize, stream));
  EXPECT_TRUE(HasMemoryRegion(addresses.region1, kSize));
  EXPECT_TRUE(AddFileStreamByAddr(addresses.region3, kSize, stream));
  EXPECT_TRUE(HasMemoryRegion(addresses.region3, kSize));

  // Check the tree status again, just in case.
  EXPECT_TRUE(HasMemoryRegion(addresses.region2, kSize));

  // Remove all regions.
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region0, kSize));
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region1, kSize));
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region2, kSize));
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region3, kSize));
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region4, kSize));
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region5, 2));
}

// Resets the map, then adds |stream1| and |stream2| to the map in the
// following way:
//
// * region[0] to [1] are unused.
// * region[2] to [4] are backed by |stream1|.
// * region[5] to [7] are unused.
// * region[8] to [10] are backed by |stream2|.
// * region[11] to [12] are unused.
//
//  0 1 2 3 4 5 6 7 8 9 A B C
// |E|E|1|1|1|E|E|E|2|2|2|E|E|
#define RESET()                                                                \
  do {                                                                         \
    ClearAddrMap();                                                            \
    stream1->Reset();                                                          \
    stream2->Reset();                                                          \
    stream3->Reset();                                                          \
    expected_count1 = expected_count2 = expected_count3 = 0;                   \
    EXPECT_TRUE(AddFileStreamByAddr(addresses.region[2], kSize * 3, stream1)); \
    EXPECT_TRUE(AddFileStreamByAddr(addresses.region[8], kSize * 3, stream2)); \
  } while (false)

// Resets the map, then adds |stream1| and |stream2| to the map in the
// following way:
//
// * region[0] to [1] are unused.
// * region[2] to [4] are backed by |stream1|.
// * region[5] to [7] are backed by |stream2|. No gap between the two streams.
// * region[8] to [12] are unused.
//
//  0 1 2 3 4 5 6 7 8 9 A B C
// |E|E|1|1|1|2|2|2|E|E|E|E|E|
#define RESET2()                                                               \
  do {                                                                         \
    ClearAddrMap();                                                            \
    stream1->Reset();                                                          \
    stream2->Reset();                                                          \
    stream3->Reset();                                                          \
    expected_count1 = expected_count2 = expected_count3 = 0;                   \
    EXPECT_TRUE(AddFileStreamByAddr(addresses.region[2], kSize * 3, stream1)); \
    EXPECT_TRUE(AddFileStreamByAddr(addresses.region[5], kSize * 3, stream2)); \
  } while (false)

// Resets the map, then adds |stream1|, |stream2|, and |stream3| to the map in
// the following way:
//
// * region[0] to [1] are unused.
// * region[2] to [4] are backed by |stream1|.
// * region[5] to [7] are backed by |stream2|.
// * region[8] to [108] are backed by |stream3|.
// * region[11] to [12] are unused.
//
//  0 1 2 3 4 5 6 7 8 9 A B C
// |E|E|1|1|1|2|2|2|3|3|3|E|E|
#define RESET3()                                                               \
  do {                                                                         \
    ClearAddrMap();                                                            \
    stream1->Reset();                                                          \
    stream2->Reset();                                                          \
    stream3->Reset();                                                          \
    expected_count1 = expected_count2 = expected_count3 = 0;                   \
    EXPECT_TRUE(AddFileStreamByAddr(addresses.region[2], kSize * 3, stream1)); \
    EXPECT_TRUE(AddFileStreamByAddr(addresses.region[5], kSize * 3, stream2)); \
    EXPECT_TRUE(AddFileStreamByAddr(addresses.region[8], kSize * 3, stream3)); \
  } while (false)

#define CHECK_MUNMAP_COUNT()                                                   \
  do {                                                                         \
    EXPECT_EQ(expected_count1, stream1->munmap_count);                         \
    EXPECT_EQ(expected_count2, stream2->munmap_count);                         \
    EXPECT_EQ(expected_count3, stream3->munmap_count);                         \
  } while (false)

#define CHECK_MPROTECT_COUNT()                                                 \
  do {                                                                         \
    EXPECT_EQ(expected_count1, stream1->mprotect_count);                       \
    EXPECT_EQ(expected_count2, stream2->mprotect_count);                       \
    EXPECT_EQ(expected_count3, stream3->mprotect_count);                       \
  } while (false)

// Tests if RemoveStreamByAddr removes one or more memory regions correctly.
TYPED_TEST_F(MemoryRegionTypedTest, TestRemoveStreamByAddr) {
  static const size_t kSize = sizeof(TypeParam);
  struct TestAddresses {
    TypeParam region[13];  // TypeParam is char[N] (N = 2,4,..).
  } addresses ALIGN_(2);

  scoped_refptr<StubFileStream> stream1 = new StubFileStream(true);
  scoped_refptr<StubFileStream> stream2 = new StubFileStream(true);
  scoped_refptr<StubFileStream> stream3 = new StubFileStream(true);
  size_t expected_count1 = 0;  // for |stream1|
  size_t expected_count2 = 0;  // for |stream2|
  size_t expected_count3 = 0;  // for |stream3|

  // Delete [0]. This should be no-op.
  RESET();
  EXPECT_FALSE(RemoveFileStreamsByAddr(addresses.region[0], kSize));
  CHECK_MUNMAP_COUNT();
  EXPECT_TRUE(HasMemoryRegion(addresses.region[2], kSize * 3));
  EXPECT_TRUE(HasMemoryRegion(addresses.region[8], kSize * 3));
  EXPECT_EQ(4U, GetAddrMapSize());

  // Delete [0]-[1]. no-op.
  EXPECT_FALSE(RemoveFileStreamsByAddr(addresses.region[0], kSize * 2));
  CHECK_MUNMAP_COUNT();
  EXPECT_TRUE(HasMemoryRegion(addresses.region[2], kSize * 3));
  EXPECT_TRUE(HasMemoryRegion(addresses.region[8], kSize * 3));
  EXPECT_EQ(4U, GetAddrMapSize());

  // Delete [0]-[2]. The first block of |stream1| should be removed.
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region[0], kSize * 3));
  ++expected_count1;
  CHECK_MUNMAP_COUNT();
  EXPECT_EQ(addresses.region[2], stream1->last_munmap_addr);
  EXPECT_EQ(kSize, stream1->last_munmap_length);
  EXPECT_TRUE(HasMemoryRegion(addresses.region[3], kSize * 2));
  EXPECT_TRUE(HasMemoryRegion(addresses.region[8], kSize * 3));
  EXPECT_EQ(4U, GetAddrMapSize());

  // Delete [0]-[4]. |stream1| should be removed.
  RESET();
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region[0], kSize * 5));
  ++expected_count1;
  CHECK_MUNMAP_COUNT();
  EXPECT_EQ(addresses.region[2], stream1->last_munmap_addr);
  EXPECT_EQ(kSize * 3, stream1->last_munmap_length);
  EXPECT_TRUE(HasMemoryRegion(addresses.region[8], kSize * 3));
  EXPECT_EQ(2U, GetAddrMapSize());

  // Delete [0]-[5]. |stream1| should be removed.
  RESET();
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region[0], kSize * 6));
  ++expected_count1;
  CHECK_MUNMAP_COUNT();
  EXPECT_EQ(addresses.region[2], stream1->last_munmap_addr);
  EXPECT_EQ(kSize * 3, stream1->last_munmap_length);
  EXPECT_TRUE(HasMemoryRegion(addresses.region[8], kSize * 3));
  EXPECT_EQ(2U, GetAddrMapSize());

  // Delete [0]-[7]. |stream1| should be removed.
  RESET();
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region[0], kSize * 8));
  ++expected_count1;
  CHECK_MUNMAP_COUNT();
  EXPECT_EQ(addresses.region[2], stream1->last_munmap_addr);
  EXPECT_EQ(kSize * 3, stream1->last_munmap_length);
  EXPECT_TRUE(HasMemoryRegion(addresses.region[8], kSize * 3));
  EXPECT_EQ(2U, GetAddrMapSize());

  // Delete [0]-[8]. |stream1| and the first block of |stream2| should be
  // removed.
  RESET();
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region[0], kSize * 9));
  ++expected_count1;
  ++expected_count2;
  CHECK_MUNMAP_COUNT();
  EXPECT_EQ(addresses.region[2], stream1->last_munmap_addr);
  EXPECT_EQ(kSize * 3, stream1->last_munmap_length);
  EXPECT_EQ(addresses.region[8], stream2->last_munmap_addr);
  EXPECT_EQ(kSize, stream2->last_munmap_length);
  EXPECT_TRUE(HasMemoryRegion(addresses.region[9], kSize * 2));
  EXPECT_EQ(2U, GetAddrMapSize());

  // Delete [0]-[10]. Both |stream1| and |stream2| should be removed.
  RESET();
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region[0], kSize * 11));
  ++expected_count1;
  ++expected_count2;
  CHECK_MUNMAP_COUNT();
  EXPECT_EQ(addresses.region[2], stream1->last_munmap_addr);
  EXPECT_EQ(kSize * 3, stream1->last_munmap_length);
  EXPECT_EQ(addresses.region[8], stream2->last_munmap_addr);
  EXPECT_EQ(kSize * 3, stream2->last_munmap_length);
  EXPECT_EQ(0U, GetAddrMapSize());

  // Delete [0]-[11]. Both |stream1| and |stream2| should be removed.
  RESET();
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region[0], kSize * 12));
  ++expected_count1;
  ++expected_count2;
  CHECK_MUNMAP_COUNT();
  EXPECT_EQ(addresses.region[2], stream1->last_munmap_addr);
  EXPECT_EQ(kSize * 3, stream1->last_munmap_length);
  EXPECT_EQ(addresses.region[8], stream2->last_munmap_addr);
  EXPECT_EQ(kSize * 3, stream2->last_munmap_length);
  EXPECT_EQ(0U, GetAddrMapSize());

  // Change the base position to [1].

  // Delete [1]. This should be no-op.
  RESET();
  EXPECT_FALSE(RemoveFileStreamsByAddr(addresses.region[1], kSize));
  CHECK_MUNMAP_COUNT();
  EXPECT_TRUE(HasMemoryRegion(addresses.region[2], kSize * 3));
  EXPECT_TRUE(HasMemoryRegion(addresses.region[8], kSize * 3));
  EXPECT_EQ(4U, GetAddrMapSize());

  // Change the base position to [2].

  // Delete [2]. The first block of |stream1| should be removed.
  RESET();
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region[2], kSize));
  ++expected_count1;
  CHECK_MUNMAP_COUNT();
  EXPECT_EQ(addresses.region[2], stream1->last_munmap_addr);
  EXPECT_EQ(kSize, stream1->last_munmap_length);
  EXPECT_TRUE(HasMemoryRegion(addresses.region[3], kSize * 2));
  EXPECT_TRUE(HasMemoryRegion(addresses.region[8], kSize * 3));
  EXPECT_EQ(4U, GetAddrMapSize());

  // Delete [2]-[4]. |stream1| should be removed.
  RESET();
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region[2], kSize * 3));
  ++expected_count1;
  CHECK_MUNMAP_COUNT();
  EXPECT_EQ(addresses.region[2], stream1->last_munmap_addr);
  EXPECT_EQ(kSize * 3, stream1->last_munmap_length);
  EXPECT_TRUE(HasMemoryRegion(addresses.region[8], kSize * 3));
  EXPECT_EQ(2U, GetAddrMapSize());

  // Delete [2]-[5]. |stream1| should be removed.
  RESET();
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region[2], kSize * 4));
  ++expected_count1;
  CHECK_MUNMAP_COUNT();
  EXPECT_EQ(addresses.region[2], stream1->last_munmap_addr);
  EXPECT_EQ(kSize * 3, stream1->last_munmap_length);
  EXPECT_TRUE(HasMemoryRegion(addresses.region[8], kSize * 3));
  EXPECT_EQ(2U, GetAddrMapSize());

  // Delete [2]-[7]. |stream1| should be removed.
  RESET();
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region[2], kSize * 6));
  ++expected_count1;
  CHECK_MUNMAP_COUNT();
  EXPECT_EQ(addresses.region[2], stream1->last_munmap_addr);
  EXPECT_EQ(kSize * 3, stream1->last_munmap_length);
  EXPECT_TRUE(HasMemoryRegion(addresses.region[8], kSize * 3));
  EXPECT_EQ(2U, GetAddrMapSize());

  // Delete [2]-[8]. |stream1| and the first block of |stream2| should be
  // removed.
  RESET();
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region[2], kSize * 7));
  ++expected_count1;
  ++expected_count2;
  CHECK_MUNMAP_COUNT();
  EXPECT_EQ(addresses.region[2], stream1->last_munmap_addr);
  EXPECT_EQ(kSize * 3, stream1->last_munmap_length);
  EXPECT_EQ(addresses.region[8], stream2->last_munmap_addr);
  EXPECT_EQ(kSize, stream2->last_munmap_length);
  EXPECT_TRUE(HasMemoryRegion(addresses.region[9], kSize * 2));
  EXPECT_EQ(2U, GetAddrMapSize());

  // Delete [2]-[10]. Both |stream1| and |stream2| should be removed.
  RESET();
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region[2], kSize * 9));
  ++expected_count1;
  ++expected_count2;
  CHECK_MUNMAP_COUNT();
  EXPECT_EQ(addresses.region[2], stream1->last_munmap_addr);
  EXPECT_EQ(kSize * 3, stream1->last_munmap_length);
  EXPECT_EQ(addresses.region[8], stream2->last_munmap_addr);
  EXPECT_EQ(kSize * 3, stream2->last_munmap_length);
  EXPECT_EQ(0U, GetAddrMapSize());

  // Delete [2]-[11]. Both |stream1| and |stream2| should be removed.
  RESET();
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region[2], kSize * 10));
  ++expected_count1;
  ++expected_count2;
  CHECK_MUNMAP_COUNT();
  EXPECT_EQ(addresses.region[2], stream1->last_munmap_addr);
  EXPECT_EQ(kSize * 3, stream1->last_munmap_length);
  EXPECT_EQ(addresses.region[8], stream2->last_munmap_addr);
  EXPECT_EQ(kSize * 3, stream2->last_munmap_length);
  EXPECT_EQ(0U, GetAddrMapSize());

  // Change the base position to [3].

  // Delete [3]. The second block of |stream1| should be removed.
  RESET();
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region[3], kSize));
  ++expected_count1;
  CHECK_MUNMAP_COUNT();
  EXPECT_EQ(addresses.region[3], stream1->last_munmap_addr);
  EXPECT_EQ(kSize, stream1->last_munmap_length);
  EXPECT_TRUE(HasMemoryRegion(addresses.region[2], kSize));
  EXPECT_TRUE(HasMemoryRegion(addresses.region[4], kSize));
  EXPECT_TRUE(HasMemoryRegion(addresses.region[8], kSize * 3));
  EXPECT_EQ(6U, GetAddrMapSize());  // The first region should split.

  // Change the base position to [4].

  // Delete [4]. The last block of |stream1| should be removed.
  RESET();
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region[4], kSize));
  ++expected_count1;
  CHECK_MUNMAP_COUNT();
  EXPECT_EQ(addresses.region[4], stream1->last_munmap_addr);
  EXPECT_EQ(kSize, stream1->last_munmap_length);
  EXPECT_TRUE(HasMemoryRegion(addresses.region[2], kSize * 2));
  EXPECT_TRUE(HasMemoryRegion(addresses.region[8], kSize * 3));
  EXPECT_EQ(4U, GetAddrMapSize());

  // Delete [4]-[5]. The last block of |stream1| should be removed.
  RESET();
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region[4], kSize * 2));
  ++expected_count1;
  CHECK_MUNMAP_COUNT();
  EXPECT_EQ(addresses.region[4], stream1->last_munmap_addr);
  EXPECT_EQ(kSize, stream1->last_munmap_length);
  EXPECT_TRUE(HasMemoryRegion(addresses.region[2], kSize * 2));
  EXPECT_TRUE(HasMemoryRegion(addresses.region[8], kSize * 3));
  EXPECT_EQ(4U, GetAddrMapSize());

  // Delete [4]-[7]. The last block of |stream1| should be removed.
  RESET();
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region[4], kSize * 4));
  ++expected_count1;
  CHECK_MUNMAP_COUNT();
  EXPECT_EQ(addresses.region[4], stream1->last_munmap_addr);
  EXPECT_EQ(kSize, stream1->last_munmap_length);
  EXPECT_TRUE(HasMemoryRegion(addresses.region[2], kSize * 2));
  EXPECT_TRUE(HasMemoryRegion(addresses.region[8], kSize * 3));
  EXPECT_EQ(4U, GetAddrMapSize());

  // Delete [4]-[8]. The last block of |stream1| and the first block of
  // |stream2| should be removed.
  RESET();
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region[4], kSize * 5));
  ++expected_count1;
  ++expected_count2;
  CHECK_MUNMAP_COUNT();
  EXPECT_EQ(addresses.region[4], stream1->last_munmap_addr);
  EXPECT_EQ(kSize, stream1->last_munmap_length);
  EXPECT_EQ(addresses.region[8], stream2->last_munmap_addr);
  EXPECT_EQ(kSize, stream2->last_munmap_length);
  EXPECT_TRUE(HasMemoryRegion(addresses.region[2], kSize * 2));
  EXPECT_TRUE(HasMemoryRegion(addresses.region[9], kSize * 2));
  EXPECT_EQ(4U, GetAddrMapSize());

  // Delete [4]-[10]. The last block of |stream1| and all blocks of |stream2|
  // should be removed.
  RESET();
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region[4], kSize * 7));
  ++expected_count1;
  ++expected_count2;
  CHECK_MUNMAP_COUNT();
  EXPECT_EQ(addresses.region[4], stream1->last_munmap_addr);
  EXPECT_EQ(kSize, stream1->last_munmap_length);
  EXPECT_EQ(addresses.region[8], stream2->last_munmap_addr);
  EXPECT_EQ(kSize * 3, stream2->last_munmap_length);
  EXPECT_TRUE(HasMemoryRegion(addresses.region[2], kSize * 2));
  EXPECT_EQ(2U, GetAddrMapSize());

  // Delete [4]-[11]. The last block of |stream1| and all blocks of |stream2|
  // should be removed.
  RESET();
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region[4], kSize * 8));
  ++expected_count1;
  ++expected_count2;
  CHECK_MUNMAP_COUNT();
  EXPECT_EQ(addresses.region[4], stream1->last_munmap_addr);
  EXPECT_EQ(kSize, stream1->last_munmap_length);
  EXPECT_EQ(addresses.region[8], stream2->last_munmap_addr);
  EXPECT_EQ(kSize * 3, stream2->last_munmap_length);
  EXPECT_TRUE(HasMemoryRegion(addresses.region[2], kSize * 2));
  EXPECT_EQ(2U, GetAddrMapSize());

  // Change the base position to [5].

  // Delete [5]. This should be no-op.
  RESET();
  EXPECT_FALSE(RemoveFileStreamsByAddr(addresses.region[5], kSize));
  CHECK_MUNMAP_COUNT();
  EXPECT_TRUE(HasMemoryRegion(addresses.region[2], kSize * 3));
  EXPECT_TRUE(HasMemoryRegion(addresses.region[8], kSize * 3));
  EXPECT_EQ(4U, GetAddrMapSize());

  // Delete [5]-[6]. no-op.
  EXPECT_FALSE(RemoveFileStreamsByAddr(addresses.region[5], kSize * 2));
  CHECK_MUNMAP_COUNT();
  EXPECT_TRUE(HasMemoryRegion(addresses.region[2], kSize * 3));
  EXPECT_TRUE(HasMemoryRegion(addresses.region[8], kSize * 3));
  EXPECT_EQ(4U, GetAddrMapSize());

  // Delete [5]-[7]. no-op.
  EXPECT_FALSE(RemoveFileStreamsByAddr(addresses.region[5], kSize * 3));
  CHECK_MUNMAP_COUNT();
  EXPECT_TRUE(HasMemoryRegion(addresses.region[2], kSize * 3));
  EXPECT_TRUE(HasMemoryRegion(addresses.region[8], kSize * 3));
  EXPECT_EQ(4U, GetAddrMapSize());

  // Delete [5]-[8]. The first block of |stream2| should be removed.
  RESET();
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region[5], kSize * 4));
  ++expected_count2;
  CHECK_MUNMAP_COUNT();
  EXPECT_EQ(addresses.region[8], stream2->last_munmap_addr);
  EXPECT_EQ(kSize, stream2->last_munmap_length);
  EXPECT_TRUE(HasMemoryRegion(addresses.region[2], kSize * 3));
  EXPECT_TRUE(HasMemoryRegion(addresses.region[9], kSize * 2));
  EXPECT_EQ(4U, GetAddrMapSize());

  // Delete [5]-[10]. |stream2| should be removed.
  RESET();
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region[5], kSize * 6));
  ++expected_count2;
  CHECK_MUNMAP_COUNT();
  EXPECT_EQ(addresses.region[8], stream2->last_munmap_addr);
  EXPECT_EQ(kSize * 3, stream2->last_munmap_length);
  EXPECT_TRUE(HasMemoryRegion(addresses.region[2], kSize * 3));
  EXPECT_EQ(2U, GetAddrMapSize());

  // Delete [5]-[11]. |stream2| should be removed.
  RESET();
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region[5], kSize * 7));
  ++expected_count2;
  CHECK_MUNMAP_COUNT();
  EXPECT_EQ(addresses.region[8], stream2->last_munmap_addr);
  EXPECT_EQ(kSize * 3, stream2->last_munmap_length);
  EXPECT_TRUE(HasMemoryRegion(addresses.region[2], kSize * 3));
  EXPECT_EQ(2U, GetAddrMapSize());

  // Change the base position to [6].

  // Delete [6]. This should be no-op.
  RESET();
  EXPECT_FALSE(RemoveFileStreamsByAddr(addresses.region[6], kSize));
  CHECK_MUNMAP_COUNT();
  EXPECT_TRUE(HasMemoryRegion(addresses.region[2], kSize * 3));
  EXPECT_TRUE(HasMemoryRegion(addresses.region[8], kSize * 3));
  EXPECT_EQ(4U, GetAddrMapSize());

  // Delete [6]-[7]. no-op.
  EXPECT_FALSE(RemoveFileStreamsByAddr(addresses.region[6], kSize * 2));
  CHECK_MUNMAP_COUNT();
  EXPECT_TRUE(HasMemoryRegion(addresses.region[2], kSize * 3));
  EXPECT_TRUE(HasMemoryRegion(addresses.region[8], kSize * 3));
  EXPECT_EQ(4U, GetAddrMapSize());

  // Change the base position to [7].

  // Delete [7]. This should be no-op.
  EXPECT_FALSE(RemoveFileStreamsByAddr(addresses.region[7], kSize));
  CHECK_MUNMAP_COUNT();
  EXPECT_TRUE(HasMemoryRegion(addresses.region[2], kSize * 3));
  EXPECT_TRUE(HasMemoryRegion(addresses.region[8], kSize * 3));
  EXPECT_EQ(4U, GetAddrMapSize());

  // Delete [7]-[8]. The first block of |stream2| should be removed.
  RESET();
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region[7], kSize * 2));
  ++expected_count2;
  CHECK_MUNMAP_COUNT();
  EXPECT_EQ(addresses.region[8], stream2->last_munmap_addr);
  EXPECT_EQ(kSize, stream2->last_munmap_length);
  EXPECT_TRUE(HasMemoryRegion(addresses.region[2], kSize * 3));
  EXPECT_TRUE(HasMemoryRegion(addresses.region[9], kSize * 2));
  EXPECT_EQ(4U, GetAddrMapSize());

  // Delete [7]-[10]. |stream2| should be removed.
  RESET();
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region[7], kSize * 4));
  ++expected_count2;
  CHECK_MUNMAP_COUNT();
  EXPECT_EQ(addresses.region[8], stream2->last_munmap_addr);
  EXPECT_EQ(kSize * 3, stream2->last_munmap_length);
  EXPECT_TRUE(HasMemoryRegion(addresses.region[2], kSize * 3));
  EXPECT_EQ(2U, GetAddrMapSize());

  // Delete [7]-[11]. |stream2| should be removed.
  RESET();
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region[7], kSize * 5));
  ++expected_count2;
  CHECK_MUNMAP_COUNT();
  EXPECT_EQ(addresses.region[8], stream2->last_munmap_addr);
  EXPECT_EQ(kSize * 3, stream2->last_munmap_length);
  EXPECT_TRUE(HasMemoryRegion(addresses.region[2], kSize * 3));
  EXPECT_EQ(2U, GetAddrMapSize());

  // Change the base position to [8].

  // Delete [8]. The first block of |stream2| should be removed.
  RESET();
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region[8], kSize));
  ++expected_count2;
  CHECK_MUNMAP_COUNT();
  EXPECT_EQ(addresses.region[8], stream2->last_munmap_addr);
  EXPECT_EQ(kSize, stream2->last_munmap_length);
  EXPECT_TRUE(HasMemoryRegion(addresses.region[2], kSize * 3));
  EXPECT_TRUE(HasMemoryRegion(addresses.region[9], kSize * 2));
  EXPECT_EQ(4U, GetAddrMapSize());

  // Delete [8]-[10]. |stream2| should be removed.
  RESET();
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region[8], kSize * 3));
  ++expected_count2;
  CHECK_MUNMAP_COUNT();
  EXPECT_EQ(addresses.region[8], stream2->last_munmap_addr);
  EXPECT_EQ(kSize * 3, stream2->last_munmap_length);
  EXPECT_TRUE(HasMemoryRegion(addresses.region[2], kSize * 3));
  EXPECT_EQ(2U, GetAddrMapSize());

  // Delete [8]-[11]. |stream2| should be removed.
  RESET();
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region[8], kSize * 4));
  ++expected_count2;
  CHECK_MUNMAP_COUNT();
  EXPECT_EQ(addresses.region[8], stream2->last_munmap_addr);
  EXPECT_EQ(kSize * 3, stream2->last_munmap_length);
  EXPECT_TRUE(HasMemoryRegion(addresses.region[2], kSize * 3));
  EXPECT_EQ(2U, GetAddrMapSize());

  // Change the base position to [9].

  // Delete [9]. The second block of |stream2| should be removed.
  RESET();
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region[9], kSize));
  ++expected_count2;
  CHECK_MUNMAP_COUNT();
  EXPECT_EQ(addresses.region[9], stream2->last_munmap_addr);
  EXPECT_EQ(kSize, stream2->last_munmap_length);
  EXPECT_TRUE(HasMemoryRegion(addresses.region[2], kSize * 3));
  EXPECT_TRUE(HasMemoryRegion(addresses.region[8], kSize));
  EXPECT_TRUE(HasMemoryRegion(addresses.region[10], kSize));
  EXPECT_EQ(6U, GetAddrMapSize());  // split

  // Change the base position to [10].

  // Delete [10]. The last block of |stream2| should be removed.
  RESET();
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region[10], kSize));
  ++expected_count2;
  CHECK_MUNMAP_COUNT();
  EXPECT_EQ(addresses.region[10], stream2->last_munmap_addr);
  EXPECT_EQ(kSize, stream2->last_munmap_length);
  EXPECT_TRUE(HasMemoryRegion(addresses.region[2], kSize * 3));
  EXPECT_TRUE(HasMemoryRegion(addresses.region[8], kSize * 2));
  EXPECT_EQ(4U, GetAddrMapSize());

  // Delete [10]-[11]. The last block of |stream2| should be removed.
  RESET();
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region[10], kSize * 2));
  ++expected_count2;
  CHECK_MUNMAP_COUNT();
  EXPECT_EQ(addresses.region[10], stream2->last_munmap_addr);
  EXPECT_EQ(kSize, stream2->last_munmap_length);
  EXPECT_TRUE(HasMemoryRegion(addresses.region[2], kSize * 3));
  EXPECT_TRUE(HasMemoryRegion(addresses.region[8], kSize * 2));
  EXPECT_EQ(4U, GetAddrMapSize());

  // Change the base position to [11].

  // Delete [11]. This should be no-op.
  RESET();
  EXPECT_FALSE(RemoveFileStreamsByAddr(addresses.region[11], kSize));
  CHECK_MUNMAP_COUNT();
  EXPECT_TRUE(HasMemoryRegion(addresses.region[2], kSize * 3));
  EXPECT_TRUE(HasMemoryRegion(addresses.region[8], kSize * 3));
  EXPECT_EQ(4U, GetAddrMapSize());

  // Delete [11]-[12]. This should be no-op.
  RESET();
  EXPECT_FALSE(RemoveFileStreamsByAddr(addresses.region[11], kSize * 2));
  CHECK_MUNMAP_COUNT();
  EXPECT_TRUE(HasMemoryRegion(addresses.region[2], kSize * 3));
  EXPECT_TRUE(HasMemoryRegion(addresses.region[8], kSize * 3));
  EXPECT_EQ(4U, GetAddrMapSize());

  // Change the base position to [12].

  // Delete [12]. This should be no-op.
  EXPECT_FALSE(RemoveFileStreamsByAddr(addresses.region[12], kSize));
  CHECK_MUNMAP_COUNT();
  EXPECT_TRUE(HasMemoryRegion(addresses.region[2], kSize * 3));
  EXPECT_TRUE(HasMemoryRegion(addresses.region[8], kSize * 3));
  EXPECT_EQ(4U, GetAddrMapSize());

  // Delete [4]-[5]. The last block of |stream1| and the first block of
  // |stream2| should be removed.
  RESET2();
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region[4], kSize * 2));
  ++expected_count1;
  ++expected_count2;
  CHECK_MUNMAP_COUNT();
  EXPECT_EQ(addresses.region[4], stream1->last_munmap_addr);
  EXPECT_EQ(kSize, stream1->last_munmap_length);
  EXPECT_EQ(addresses.region[5], stream2->last_munmap_addr);
  EXPECT_EQ(kSize, stream2->last_munmap_length);
  EXPECT_TRUE(HasMemoryRegion(addresses.region[2], kSize * 2));
  EXPECT_TRUE(HasMemoryRegion(addresses.region[6], kSize * 2));
  EXPECT_EQ(4U, GetAddrMapSize());

  // Delete [2]-[7]. Both streams should be removed.
  RESET2();
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region[2], kSize * 6));
  ++expected_count1;
  ++expected_count2;
  CHECK_MUNMAP_COUNT();
  EXPECT_EQ(addresses.region[2], stream1->last_munmap_addr);
  EXPECT_EQ(kSize * 3, stream1->last_munmap_length);
  EXPECT_EQ(addresses.region[5], stream2->last_munmap_addr);
  EXPECT_EQ(kSize * 3, stream2->last_munmap_length);
  EXPECT_EQ(0U, GetAddrMapSize());

  // Delete [1]-[8]. Both streams should be removed.
  RESET2();
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region[1], kSize * 8));
  ++expected_count1;
  ++expected_count2;
  CHECK_MUNMAP_COUNT();
  EXPECT_EQ(addresses.region[2], stream1->last_munmap_addr);
  EXPECT_EQ(kSize * 3, stream1->last_munmap_length);
  EXPECT_EQ(addresses.region[5], stream2->last_munmap_addr);
  EXPECT_EQ(kSize * 3, stream2->last_munmap_length);
  EXPECT_EQ(0U, GetAddrMapSize());

  // Delete [6]. The second block of |stream2| should be removed.
  RESET3();
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region[6], kSize));
  ++expected_count2;
  CHECK_MUNMAP_COUNT();
  EXPECT_EQ(addresses.region[6], stream2->last_munmap_addr);
  EXPECT_EQ(kSize, stream2->last_munmap_length);
  EXPECT_TRUE(HasMemoryRegion(addresses.region[2], kSize * 3));
  EXPECT_TRUE(HasMemoryRegion(addresses.region[5], kSize));
  EXPECT_TRUE(HasMemoryRegion(addresses.region[7], kSize));
  EXPECT_TRUE(HasMemoryRegion(addresses.region[8], kSize * 3));
  EXPECT_EQ(8U, GetAddrMapSize());  // split

  // Delete [7]-[8]. The last block of |stream2| and the first block of
  // |stream3| should be removed.
  RESET3();
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region[7], kSize * 2));
  ++expected_count2;
  ++expected_count3;
  CHECK_MUNMAP_COUNT();
  EXPECT_EQ(addresses.region[7], stream2->last_munmap_addr);
  EXPECT_EQ(kSize, stream2->last_munmap_length);
  EXPECT_EQ(addresses.region[8], stream3->last_munmap_addr);
  EXPECT_EQ(kSize, stream3->last_munmap_length);
  EXPECT_TRUE(HasMemoryRegion(addresses.region[2], kSize * 3));
  EXPECT_TRUE(HasMemoryRegion(addresses.region[5], kSize * 2));
  EXPECT_TRUE(HasMemoryRegion(addresses.region[9], kSize * 2));
  EXPECT_EQ(6U, GetAddrMapSize());

  // Delete [4]-[8]. The last block of |stream1| and the first block of
  // |stream3| should be removed. |stream2| should be gone.
  RESET3();
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region[4], kSize * 5));
  ++expected_count1;
  ++expected_count2;
  ++expected_count3;
  CHECK_MUNMAP_COUNT();
  EXPECT_EQ(addresses.region[4], stream1->last_munmap_addr);
  EXPECT_EQ(kSize, stream1->last_munmap_length);
  EXPECT_EQ(addresses.region[5], stream2->last_munmap_addr);
  EXPECT_EQ(kSize * 3, stream2->last_munmap_length);
  EXPECT_EQ(addresses.region[8], stream3->last_munmap_addr);
  EXPECT_EQ(kSize, stream3->last_munmap_length);
  EXPECT_TRUE(HasMemoryRegion(addresses.region[2], kSize * 2));
  EXPECT_TRUE(HasMemoryRegion(addresses.region[9], kSize * 2));
  EXPECT_EQ(4U, GetAddrMapSize());

  // Delete [1]-[11]. |stream1|, |stream2|, and |stream3| should be gone.
  RESET3();
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region[1], kSize * 11));
  ++expected_count1;
  ++expected_count2;
  ++expected_count3;
  CHECK_MUNMAP_COUNT();
  EXPECT_EQ(addresses.region[2], stream1->last_munmap_addr);
  EXPECT_EQ(kSize * 3, stream1->last_munmap_length);
  EXPECT_EQ(addresses.region[5], stream2->last_munmap_addr);
  EXPECT_EQ(kSize * 3, stream2->last_munmap_length);
  EXPECT_EQ(addresses.region[8], stream3->last_munmap_addr);
  EXPECT_EQ(kSize * 3, stream3->last_munmap_length);
  EXPECT_EQ(0U, GetAddrMapSize());
}

// Tests if ModifyStreamByAddr modifies one or more memory regions properly.
TYPED_TEST_F(MemoryRegionTypedTest, TestModifyStreamByAddr) {
  static const size_t kSize = sizeof(TypeParam);
  struct TestAddresses {
    TypeParam region[13];  // TypeParam is char[N] (N = 2,4,..).
  } addresses ALIGN_(2);

  scoped_refptr<StubFileStream> stream1 = new StubFileStream(true);
  scoped_refptr<StubFileStream> stream2 = new StubFileStream(true);
  scoped_refptr<StubFileStream> stream3 = new StubFileStream(true);
  size_t expected_count1 = 0;  // for |stream1|
  size_t expected_count2 = 0;  // for |stream2|
  size_t expected_count3 = 0;  // for |stream3|

  // Test modifications to |stream1|.
  RESET();

  // Modify [0]. This should be no-op.
  EXPECT_FALSE(ChangeProtectionModeByAddr(
      addresses.region[0], kSize, PROT_READ));
  CHECK_MPROTECT_COUNT();

  // Modify [0]-[1]. no-op.
  EXPECT_FALSE(ChangeProtectionModeByAddr(
      addresses.region[0], kSize * 2, PROT_READ));
  CHECK_MPROTECT_COUNT();

  // Modify [0]-[2]. The first block of |stream1| should be modified.
  EXPECT_TRUE(ChangeProtectionModeByAddr(
      addresses.region[0], kSize * 3, PROT_READ));
  ++expected_count1;
  CHECK_MPROTECT_COUNT();
  EXPECT_EQ(addresses.region[2], stream1->last_mprotect_addr);
  EXPECT_EQ(kSize, stream1->last_mprotect_length);
  EXPECT_EQ(PROT_READ, stream1->last_mprotect_prot);

  // Modify [0]-[4]. |stream1| should be modified.
  EXPECT_TRUE(ChangeProtectionModeByAddr(
      addresses.region[0], kSize * 5, PROT_READ));
  ++expected_count1;
  CHECK_MPROTECT_COUNT();
  EXPECT_EQ(addresses.region[2], stream1->last_mprotect_addr);
  EXPECT_EQ(kSize * 3, stream1->last_mprotect_length);
  EXPECT_EQ(PROT_READ, stream1->last_mprotect_prot);

  // Modify [1]-[2]. |stream1| should be modified.
  EXPECT_TRUE(ChangeProtectionModeByAddr(
      addresses.region[1], kSize * 2, PROT_READ));
  ++expected_count1;
  CHECK_MPROTECT_COUNT();
  EXPECT_EQ(addresses.region[2], stream1->last_mprotect_addr);
  EXPECT_EQ(kSize, stream1->last_mprotect_length);
  EXPECT_EQ(PROT_READ, stream1->last_mprotect_prot);

  // Modify [2]-[3]. |stream1| should be modified.
  EXPECT_TRUE(ChangeProtectionModeByAddr(
      addresses.region[2], kSize * 2, PROT_READ));
  ++expected_count1;
  CHECK_MPROTECT_COUNT();
  EXPECT_EQ(addresses.region[2], stream1->last_mprotect_addr);
  EXPECT_EQ(kSize * 2, stream1->last_mprotect_length);
  EXPECT_EQ(PROT_READ, stream1->last_mprotect_prot);

  // Modify [3]. |stream1| should be modified.
  EXPECT_TRUE(ChangeProtectionModeByAddr(
      addresses.region[3], kSize, PROT_READ));
  ++expected_count1;
  CHECK_MPROTECT_COUNT();
  EXPECT_EQ(addresses.region[3], stream1->last_mprotect_addr);
  EXPECT_EQ(kSize, stream1->last_mprotect_length);
  EXPECT_EQ(PROT_READ, stream1->last_mprotect_prot);

  // Modify [2]-[4]. |stream1| should be modified.
  EXPECT_TRUE(ChangeProtectionModeByAddr(
      addresses.region[2], kSize * 3, PROT_READ));
  ++expected_count1;
  CHECK_MPROTECT_COUNT();
  EXPECT_EQ(addresses.region[2], stream1->last_mprotect_addr);
  EXPECT_EQ(kSize * 3, stream1->last_mprotect_length);
  EXPECT_EQ(PROT_READ, stream1->last_mprotect_prot);

  // Modify [3]-[4]. |stream1| should be modified.
  EXPECT_TRUE(ChangeProtectionModeByAddr(
      addresses.region[3], kSize * 2, PROT_READ));
  ++expected_count1;
  CHECK_MPROTECT_COUNT();
  EXPECT_EQ(addresses.region[3], stream1->last_mprotect_addr);
  EXPECT_EQ(kSize * 2, stream1->last_mprotect_length);
  EXPECT_EQ(PROT_READ, stream1->last_mprotect_prot);

  // Modify [4]-[6]. |stream1| should be modified.
  EXPECT_TRUE(ChangeProtectionModeByAddr(
      addresses.region[4], kSize * 3, PROT_READ));
  ++expected_count1;
  CHECK_MPROTECT_COUNT();
  EXPECT_EQ(addresses.region[4], stream1->last_mprotect_addr);
  EXPECT_EQ(kSize, stream1->last_mprotect_length);
  EXPECT_EQ(PROT_READ, stream1->last_mprotect_prot);

  // Modify [5]-[6]. no-op.
  EXPECT_FALSE(ChangeProtectionModeByAddr(
      addresses.region[5], kSize * 2, PROT_READ));
  CHECK_MPROTECT_COUNT();

  // Test modifications to |stream2|.

  // Modify [6]. This should be no-op.
  EXPECT_FALSE(ChangeProtectionModeByAddr(
      addresses.region[6], kSize, PROT_READ));
  CHECK_MPROTECT_COUNT();

  // Modify [6]-[7]. no-op.
  EXPECT_FALSE(ChangeProtectionModeByAddr(
      addresses.region[6], kSize * 2, PROT_READ));
  CHECK_MPROTECT_COUNT();

  // Modify [6]-[8]. The first block of |stream2| should be modified.
  EXPECT_TRUE(ChangeProtectionModeByAddr(
      addresses.region[6], kSize * 3, PROT_READ));
  ++expected_count2;
  CHECK_MPROTECT_COUNT();
  EXPECT_EQ(addresses.region[8], stream2->last_mprotect_addr);
  EXPECT_EQ(kSize, stream2->last_mprotect_length);
  EXPECT_EQ(PROT_READ, stream2->last_mprotect_prot);

  // Modify [6]-[A]. |stream2| should be modified.
  EXPECT_TRUE(ChangeProtectionModeByAddr(
      addresses.region[6], kSize * 5, PROT_READ));
  ++expected_count2;
  CHECK_MPROTECT_COUNT();
  EXPECT_EQ(addresses.region[8], stream2->last_mprotect_addr);
  EXPECT_EQ(kSize * 3, stream2->last_mprotect_length);
  EXPECT_EQ(PROT_READ, stream2->last_mprotect_prot);

  // Modify [7]-[8]. |stream2| should be modified.
  EXPECT_TRUE(ChangeProtectionModeByAddr(
      addresses.region[7], kSize * 2, PROT_READ));
  ++expected_count2;
  CHECK_MPROTECT_COUNT();
  EXPECT_EQ(addresses.region[8], stream2->last_mprotect_addr);
  EXPECT_EQ(kSize, stream2->last_mprotect_length);
  EXPECT_EQ(PROT_READ, stream2->last_mprotect_prot);

  // Modify [8]-[9]. |stream2| should be modified.
  EXPECT_TRUE(ChangeProtectionModeByAddr(
      addresses.region[8], kSize * 2, PROT_READ));
  ++expected_count2;
  CHECK_MPROTECT_COUNT();
  EXPECT_EQ(addresses.region[8], stream2->last_mprotect_addr);
  EXPECT_EQ(kSize * 2, stream2->last_mprotect_length);
  EXPECT_EQ(PROT_READ, stream2->last_mprotect_prot);

  // Modify [9]. |stream2| should be modified.
  EXPECT_TRUE(ChangeProtectionModeByAddr(
      addresses.region[9], kSize, PROT_READ));
  ++expected_count2;
  CHECK_MPROTECT_COUNT();
  EXPECT_EQ(addresses.region[9], stream2->last_mprotect_addr);
  EXPECT_EQ(kSize, stream2->last_mprotect_length);
  EXPECT_EQ(PROT_READ, stream2->last_mprotect_prot);

  // Modify [8]-[A]. |stream2| should be modified.
  EXPECT_TRUE(ChangeProtectionModeByAddr(
      addresses.region[8], kSize * 3, PROT_READ));
  ++expected_count2;
  CHECK_MPROTECT_COUNT();
  EXPECT_EQ(addresses.region[8], stream2->last_mprotect_addr);
  EXPECT_EQ(kSize * 3, stream2->last_mprotect_length);
  EXPECT_EQ(PROT_READ, stream2->last_mprotect_prot);

  // Modify [9]-[A]. |stream2| should be modified.
  EXPECT_TRUE(ChangeProtectionModeByAddr(
      addresses.region[9], kSize * 2, PROT_READ));
  ++expected_count2;
  CHECK_MPROTECT_COUNT();
  EXPECT_EQ(addresses.region[9], stream2->last_mprotect_addr);
  EXPECT_EQ(kSize * 2, stream2->last_mprotect_length);
  EXPECT_EQ(PROT_READ, stream2->last_mprotect_prot);

  // Modify [A]-[C]. |stream2| should be modified.
  EXPECT_TRUE(ChangeProtectionModeByAddr(
      addresses.region[0xA], kSize * 3, PROT_READ));
  ++expected_count2;
  CHECK_MPROTECT_COUNT();
  EXPECT_EQ(addresses.region[0xA], stream2->last_mprotect_addr);
  EXPECT_EQ(kSize, stream2->last_mprotect_length);
  EXPECT_EQ(PROT_READ, stream2->last_mprotect_prot);

  // Modify [B]-[C]. no-op.
  EXPECT_FALSE(ChangeProtectionModeByAddr(
      addresses.region[0xB], kSize * 2, PROT_READ));
  CHECK_MPROTECT_COUNT();

  // Modify |stream1| and |stream2| at the same time.

  // Modify [1]-[B].
  EXPECT_TRUE(ChangeProtectionModeByAddr(
      addresses.region[1], kSize * 11, PROT_READ));
  ++expected_count1;
  ++expected_count2;
  CHECK_MPROTECT_COUNT();
  EXPECT_EQ(addresses.region[2], stream1->last_mprotect_addr);
  EXPECT_EQ(kSize * 3, stream1->last_mprotect_length);
  EXPECT_EQ(PROT_READ, stream1->last_mprotect_prot);
  EXPECT_EQ(addresses.region[8], stream2->last_mprotect_addr);
  EXPECT_EQ(kSize * 3, stream2->last_mprotect_length);
  EXPECT_EQ(PROT_READ, stream2->last_mprotect_prot);

  // Modify [2]-[A].
  EXPECT_TRUE(ChangeProtectionModeByAddr(
      addresses.region[2], kSize * 9, PROT_READ));
  ++expected_count1;
  ++expected_count2;
  CHECK_MPROTECT_COUNT();
  EXPECT_EQ(addresses.region[2], stream1->last_mprotect_addr);
  EXPECT_EQ(kSize * 3, stream1->last_mprotect_length);
  EXPECT_EQ(PROT_READ, stream1->last_mprotect_prot);
  EXPECT_EQ(addresses.region[8], stream2->last_mprotect_addr);
  EXPECT_EQ(kSize * 3, stream2->last_mprotect_length);
  EXPECT_EQ(PROT_READ, stream2->last_mprotect_prot);

  // Modify [3]-[9].
  EXPECT_TRUE(ChangeProtectionModeByAddr(
      addresses.region[3], kSize * 7, PROT_READ));
  ++expected_count1;
  ++expected_count2;
  CHECK_MPROTECT_COUNT();
  EXPECT_EQ(addresses.region[3], stream1->last_mprotect_addr);
  EXPECT_EQ(kSize * 2, stream1->last_mprotect_length);
  EXPECT_EQ(PROT_READ, stream1->last_mprotect_prot);
  EXPECT_EQ(addresses.region[8], stream2->last_mprotect_addr);
  EXPECT_EQ(kSize * 2, stream2->last_mprotect_length);
  EXPECT_EQ(PROT_READ, stream2->last_mprotect_prot);

  // Modify [4]-[8].
  EXPECT_TRUE(ChangeProtectionModeByAddr(
      addresses.region[4], kSize * 5, PROT_READ));
  ++expected_count1;
  ++expected_count2;
  CHECK_MPROTECT_COUNT();
  EXPECT_EQ(addresses.region[4], stream1->last_mprotect_addr);
  EXPECT_EQ(kSize, stream1->last_mprotect_length);
  EXPECT_EQ(PROT_READ, stream1->last_mprotect_prot);
  EXPECT_EQ(addresses.region[8], stream2->last_mprotect_addr);
  EXPECT_EQ(kSize, stream2->last_mprotect_length);
  EXPECT_EQ(PROT_READ, stream2->last_mprotect_prot);

  // Modify |stream1| and |stream2| at the same time.
  RESET2();

  // Modify [1]-[8].
  EXPECT_TRUE(ChangeProtectionModeByAddr(
      addresses.region[1], kSize * 8, PROT_READ));
  ++expected_count1;
  ++expected_count2;
  CHECK_MPROTECT_COUNT();
  EXPECT_EQ(addresses.region[2], stream1->last_mprotect_addr);
  EXPECT_EQ(kSize * 3, stream1->last_mprotect_length);
  EXPECT_EQ(PROT_READ, stream1->last_mprotect_prot);
  EXPECT_EQ(addresses.region[5], stream2->last_mprotect_addr);
  EXPECT_EQ(kSize * 3, stream2->last_mprotect_length);
  EXPECT_EQ(PROT_READ, stream2->last_mprotect_prot);

  // Modify [2]-[7].
  EXPECT_TRUE(ChangeProtectionModeByAddr(
      addresses.region[2], kSize * 6, PROT_READ));
  ++expected_count1;
  ++expected_count2;
  CHECK_MPROTECT_COUNT();
  EXPECT_EQ(addresses.region[2], stream1->last_mprotect_addr);
  EXPECT_EQ(kSize * 3, stream1->last_mprotect_length);
  EXPECT_EQ(PROT_READ, stream1->last_mprotect_prot);
  EXPECT_EQ(addresses.region[5], stream2->last_mprotect_addr);
  EXPECT_EQ(kSize * 3, stream2->last_mprotect_length);
  EXPECT_EQ(PROT_READ, stream2->last_mprotect_prot);

  // Modify [3]-[6].
  EXPECT_TRUE(ChangeProtectionModeByAddr(
      addresses.region[3], kSize * 4, PROT_READ));
  ++expected_count1;
  ++expected_count2;
  CHECK_MPROTECT_COUNT();
  EXPECT_EQ(addresses.region[3], stream1->last_mprotect_addr);
  EXPECT_EQ(kSize * 2, stream1->last_mprotect_length);
  EXPECT_EQ(PROT_READ, stream1->last_mprotect_prot);
  EXPECT_EQ(addresses.region[5], stream2->last_mprotect_addr);
  EXPECT_EQ(kSize * 2, stream2->last_mprotect_length);
  EXPECT_EQ(PROT_READ, stream2->last_mprotect_prot);

  // Modify [4]-[5].
  EXPECT_TRUE(ChangeProtectionModeByAddr(
      addresses.region[4], kSize * 2, PROT_READ));
  ++expected_count1;
  ++expected_count2;
  CHECK_MPROTECT_COUNT();
  EXPECT_EQ(addresses.region[4], stream1->last_mprotect_addr);
  EXPECT_EQ(kSize, stream1->last_mprotect_length);
  EXPECT_EQ(PROT_READ, stream1->last_mprotect_prot);
  EXPECT_EQ(addresses.region[5], stream2->last_mprotect_addr);
  EXPECT_EQ(kSize, stream2->last_mprotect_length);
  EXPECT_EQ(PROT_READ, stream2->last_mprotect_prot);

  // Test zero-length modify. It should always succeed.
  RESET();
  EXPECT_TRUE(ChangeProtectionModeByAddr(addresses.region[4], 0, PROT_READ));
  CHECK_MPROTECT_COUNT();
}

TEST_F(MemoryRegionTest, TestGetMemoryMapAsString) {
  // The debug function should return something non-empty.
  EXPECT_NE(std::string(), GetMemoryMapAsString());
}

// Tests IsWriteMapped and IsCurrentlyMapped.
TEST_F(MemoryRegionTest, TestIsMappedFunctions) {
  static const size_t kSize = 8;
  struct {
    char addr1[kSize];
    char addr2[kSize];
    char addr3[kSize];
    char addr4[kSize];
  } m ALIGN_(2);

  scoped_refptr<StubFileStream> stream1 =
      new StubFileStream(std::string("/path/1"));
  scoped_refptr<StubFileStream> stream2 =
      new StubFileStream(std::string("/path/2"));
  scoped_refptr<StubFileStream> stream3 =
      new StubFileStream(std::string("/path/3"));
  scoped_refptr<StubFileStream> stream4 =
      new StubFileStream(std::string("/path/4"));

  // Initially IsWriteMapped and IsCurrentlyMapped should both return false.
  EXPECT_FALSE(IsWriteMapped(stream1->inode()));
  EXPECT_FALSE(IsWriteMapped(stream2->inode()));
  EXPECT_FALSE(IsWriteMapped(stream3->inode()));
  EXPECT_FALSE(IsWriteMapped(stream4->inode()));
  EXPECT_FALSE(IsCurrentlyMapped(stream1->inode()));
  EXPECT_FALSE(IsCurrentlyMapped(stream2->inode()));
  EXPECT_FALSE(IsCurrentlyMapped(stream3->inode()));
  EXPECT_FALSE(IsCurrentlyMapped(stream4->inode()));

  // Map files.
  EXPECT_TRUE(AddFileStreamByAddrWithProt(m.addr1, kSize, PROT_READ, stream1));
  EXPECT_TRUE(AddFileStreamByAddrWithProt(m.addr2, kSize, PROT_WRITE, stream2));
  EXPECT_TRUE(RemoveFileStreamsByAddr(m.addr2, kSize));
  EXPECT_TRUE(AddFileStreamByAddrWithProt(m.addr2, kSize, PROT_WRITE, stream2));
  EXPECT_TRUE(IsCurrentlyMapped(stream2->inode()));
  EXPECT_TRUE(AddFileStreamByAddrWithProt(
      m.addr3, kSize, PROT_READ | PROT_WRITE, stream3));
  EXPECT_TRUE(AddFileStreamByAddrWithProt(m.addr4, kSize, PROT_NONE, stream4));

  // Test the functions again.
  EXPECT_FALSE(IsWriteMapped(stream1->inode()));
  EXPECT_TRUE(IsWriteMapped(stream2->inode()));
  EXPECT_TRUE(IsWriteMapped(stream3->inode()));
  EXPECT_FALSE(IsWriteMapped(stream4->inode()));
  EXPECT_TRUE(IsCurrentlyMapped(stream1->inode()));
  EXPECT_TRUE(IsCurrentlyMapped(stream2->inode()));
  EXPECT_TRUE(IsCurrentlyMapped(stream3->inode()));
  EXPECT_TRUE(IsCurrentlyMapped(stream4->inode()));

  // Change from PROT_READ to PROT_WRITE.
  EXPECT_TRUE(ChangeProtectionModeByAddr(m.addr1, kSize, PROT_WRITE));
  // Change from PROT_WRITE to PROT_READ.
  EXPECT_TRUE(ChangeProtectionModeByAddr(m.addr1, kSize, PROT_WRITE));

  // Test the functions again.
  EXPECT_TRUE(IsWriteMapped(stream1->inode()));
  EXPECT_TRUE(IsWriteMapped(stream2->inode()));  // still return true
  EXPECT_TRUE(IsWriteMapped(stream3->inode()));
  EXPECT_FALSE(IsWriteMapped(stream4->inode()));
  EXPECT_TRUE(IsCurrentlyMapped(stream1->inode()));
  EXPECT_TRUE(IsCurrentlyMapped(stream2->inode()));
  EXPECT_TRUE(IsCurrentlyMapped(stream3->inode()));
  EXPECT_TRUE(IsCurrentlyMapped(stream4->inode()));

  // Partially unmap |m.addr1| and |m.addr2|, then confirm IsXXXMapped still
  // returns true.
  EXPECT_TRUE(RemoveFileStreamsByAddr(m.addr1, kSize / 2));
  EXPECT_TRUE(IsWriteMapped(stream1->inode()));
  EXPECT_TRUE(IsCurrentlyMapped(stream1->inode()));
  EXPECT_TRUE(RemoveFileStreamsByAddr(
      m.addr2 + 2, kSize / 2));  // split the region into two
  EXPECT_TRUE(IsWriteMapped(stream2->inode()));
  EXPECT_TRUE(IsCurrentlyMapped(stream2->inode()));

  // Unmap all memory regions, then test the functions again. IsWriteMapped
  // should still return true for |stream1|, |stream2|, and |stream3|.
  // Note: Removing the same address twice or more is safe.
  EXPECT_TRUE(RemoveFileStreamsByAddr(m.addr1, kSize));
  EXPECT_TRUE(IsWriteMapped(stream1->inode()));
  EXPECT_FALSE(IsCurrentlyMapped(stream1->inode()));

  EXPECT_TRUE(RemoveFileStreamsByAddr(m.addr2, kSize));
  EXPECT_TRUE(IsWriteMapped(stream2->inode()));
  EXPECT_FALSE(IsCurrentlyMapped(stream2->inode()));

  EXPECT_TRUE(RemoveFileStreamsByAddr(m.addr3, kSize));
  EXPECT_TRUE(IsWriteMapped(stream3->inode()));
  EXPECT_FALSE(IsCurrentlyMapped(stream3->inode()));

  EXPECT_TRUE(RemoveFileStreamsByAddr(m.addr4, kSize));
  EXPECT_FALSE(IsWriteMapped(stream4->inode()));
  EXPECT_FALSE(IsCurrentlyMapped(stream4->inode()));

  // Final sanity checks.
  EXPECT_TRUE(IsWriteMapped(stream1->inode()));
  EXPECT_TRUE(IsWriteMapped(stream2->inode()));
  EXPECT_TRUE(IsWriteMapped(stream3->inode()));
  EXPECT_FALSE(IsCurrentlyMapped(stream1->inode()));
  EXPECT_FALSE(IsCurrentlyMapped(stream2->inode()));
  EXPECT_FALSE(IsCurrentlyMapped(stream3->inode()));
}

// Tests IsCurrentlyMapped more.
TEST_F(MemoryRegionTest, TestIsCurrentlyMapped) {
  static const size_t kSize = 2;
  struct {
    char addr1[kSize];
    char addr2[kSize];
    char addr3[kSize];
  } m ALIGN_(2);

  scoped_refptr<StubFileStream> stream1 =
      new StubFileStream(std::string("/path/1"));
  scoped_refptr<StubFileStream> stream2 =
      new StubFileStream(std::string("/path/2"));
  scoped_refptr<StubFileStream> stream3 =
      new StubFileStream(std::string("/path/3"));

  EXPECT_FALSE(IsCurrentlyMapped(stream1->inode()));
  EXPECT_FALSE(IsCurrentlyMapped(stream2->inode()));
  EXPECT_FALSE(IsCurrentlyMapped(stream3->inode()));

  EXPECT_TRUE(AddFileStreamByAddrWithProt(m.addr1, kSize, PROT_READ, stream1));
  EXPECT_TRUE(IsCurrentlyMapped(stream1->inode()));
  EXPECT_TRUE(AddFileStreamByAddrWithProt(m.addr2, kSize, PROT_WRITE, stream2));
  EXPECT_TRUE(IsCurrentlyMapped(stream2->inode()));
  EXPECT_TRUE(RemoveFileStreamsByAddr(m.addr2, kSize));
  EXPECT_TRUE(AddFileStreamByAddrWithProt(m.addr2, kSize, PROT_WRITE, stream2));
  EXPECT_TRUE(IsCurrentlyMapped(stream1->inode()));
  EXPECT_TRUE(IsCurrentlyMapped(stream2->inode()));
  EXPECT_TRUE(AddFileStreamByAddrWithProt(m.addr3, kSize, PROT_NONE, stream3));

  EXPECT_TRUE(IsCurrentlyMapped(stream1->inode()));
  EXPECT_TRUE(IsCurrentlyMapped(stream2->inode()));
  EXPECT_TRUE(IsCurrentlyMapped(stream3->inode()));

  EXPECT_TRUE(RemoveFileStreamsByAddr(m.addr1, kSize));
  EXPECT_TRUE(RemoveFileStreamsByAddr(m.addr2, kSize));
  EXPECT_FALSE(IsCurrentlyMapped(stream1->inode()));
  EXPECT_FALSE(IsCurrentlyMapped(stream2->inode()));
  EXPECT_TRUE(IsCurrentlyMapped(stream3->inode()));
  EXPECT_TRUE(RemoveFileStreamsByAddr(m.addr3, kSize));
  EXPECT_FALSE(RemoveFileStreamsByAddr(m.addr3, kSize));
  EXPECT_FALSE(IsCurrentlyMapped(stream3->inode()));
}

TEST_F(MemoryRegionTest, TestIsPageEndAddress) {
  uintptr_t ptr = 0x0;
  EXPECT_FALSE(IsPageEndAddress(reinterpret_cast<void*>(ptr)));
  ++ptr;
  EXPECT_TRUE(IsPageEndAddress(reinterpret_cast<void*>(ptr)));
  ++ptr;
  EXPECT_FALSE(IsPageEndAddress(reinterpret_cast<void*>(ptr)));
  ++ptr;
  EXPECT_TRUE(IsPageEndAddress(reinterpret_cast<void*>(ptr)));
}

TEST_F(MemoryRegionTest, TestIsMemoryRangeAvailable) {
  static const size_t kPageSize = 4096;
  static const size_t kLength = kPageSize * 3;
  struct {
    char addr_before[kPageSize];
    char addr[kLength];
    char addr_after[kPageSize];
  } m ALIGN_(2);

  // Initially, the function should always return true.
  EXPECT_TRUE(IsMemoryRangeAvailable(m.addr, kLength));

  scoped_refptr<StubFileStream> stream = new StubFileStream(true);
  EXPECT_TRUE(AddFileStreamByAddr(m.addr, kLength, stream));
  EXPECT_FALSE(IsMemoryRangeAvailable(m.addr, kLength));
  EXPECT_TRUE(RemoveFileStreamsByAddr(m.addr, kLength));
  EXPECT_EQ(m.addr, stream->last_munmap_addr);
  EXPECT_EQ(kLength, stream->last_munmap_length);
  EXPECT_TRUE(IsMemoryRangeAvailable(m.addr, kLength));
  for (int i = 0; i < 3; ++i) {
    EXPECT_TRUE(AddFileStreamByAddr(m.addr + kPageSize * i, kPageSize, stream));
    EXPECT_FALSE(IsMemoryRangeAvailable(m.addr, kLength));
    EXPECT_TRUE(RemoveFileStreamsByAddr(m.addr + kPageSize * i, kPageSize));
    EXPECT_EQ(m.addr + kPageSize * i, stream->last_munmap_addr);
    EXPECT_EQ(kPageSize, stream->last_munmap_length);
    EXPECT_TRUE(IsMemoryRangeAvailable(m.addr, kLength));
  }
  // Out of range.
  EXPECT_TRUE(AddFileStreamByAddr(m.addr_before, kPageSize, stream));
  EXPECT_TRUE(IsMemoryRangeAvailable(m.addr, kLength));
  EXPECT_TRUE(RemoveFileStreamsByAddr(m.addr_before, kPageSize));
  EXPECT_EQ(m.addr_before, stream->last_munmap_addr);
  EXPECT_EQ(kPageSize, stream->last_munmap_length);
  // Out of range.
  EXPECT_TRUE(AddFileStreamByAddr(m.addr_after, kPageSize, stream));
  EXPECT_TRUE(IsMemoryRangeAvailable(m.addr, kLength));
  EXPECT_TRUE(RemoveFileStreamsByAddr(m.addr_after, kPageSize));
  EXPECT_EQ(m.addr_after, stream->last_munmap_addr);
  EXPECT_EQ(kPageSize, stream->last_munmap_length);
}

// Tests common Add/RemoveStreamByAddr usage.
TEST_F(MemoryRegionTest, TestAddRemoveStreamByAddr) {
  static const size_t kPageSize = 4096;
  static const size_t length = kPageSize * 5;
  char addr1[kPageSize * 5] ALIGN_(2);

  // Initially the tree is empty.
  EXPECT_EQ(0U, GetAddrMapSize());
  EXPECT_FALSE(RemoveFileStreamsByAddr(addr1, kPageSize));

  scoped_refptr<StubFileStream> stream = new StubFileStream(true);
  EXPECT_TRUE(AddFileStreamByAddr(addr1, length, stream));

  // Remove the first page.
  // Still possible to use the last 4 pages.
  EXPECT_TRUE(RemoveFileStreamsByAddr(addr1, kPageSize));
  EXPECT_EQ(addr1, stream->last_munmap_addr);
  EXPECT_EQ(kPageSize, stream->last_munmap_length);
  EXPECT_TRUE(HasMemoryRegion(addr1 + kPageSize, kPageSize * 4));

  // Remove the last page.
  // Still possible to use the 3 pages in the middle.
  EXPECT_TRUE(RemoveFileStreamsByAddr(addr1 + kPageSize * 4, kPageSize));
  EXPECT_EQ(addr1 + kPageSize * 4, stream->last_munmap_addr);
  EXPECT_EQ(kPageSize, stream->last_munmap_length);
  EXPECT_TRUE(HasMemoryRegion(addr1 + kPageSize, kPageSize * 3));
  // Remove the third page.
  EXPECT_TRUE(RemoveFileStreamsByAddr(addr1 + kPageSize * 2, kPageSize));
  EXPECT_EQ(addr1 + kPageSize * 2, stream->last_munmap_addr);
  EXPECT_EQ(kPageSize, stream->last_munmap_length);
  // Still possible to use the 2nd and 4th pages.
  EXPECT_TRUE(HasMemoryRegion(addr1 + kPageSize, kPageSize));
  EXPECT_TRUE(HasMemoryRegion(addr1 + kPageSize * 3, kPageSize));
  // It's possible to reuse the removed pages.
  EXPECT_TRUE(RemoveFileStreamsByAddr(addr1 + kPageSize, kPageSize));
  EXPECT_EQ(addr1 + kPageSize, stream->last_munmap_addr);
  EXPECT_EQ(kPageSize, stream->last_munmap_length);
  EXPECT_TRUE(AddFileStreamByAddr(addr1 + kPageSize / 2, kPageSize, stream));
  EXPECT_TRUE(AddFileStreamByAddr(addr1 + kPageSize * 4, kPageSize, stream));
  EXPECT_TRUE(RemoveFileStreamsByAddr(addr1 + kPageSize * 4, kPageSize));
  EXPECT_EQ(addr1 + kPageSize * 4, stream->last_munmap_addr);
  EXPECT_EQ(kPageSize, stream->last_munmap_length);
  EXPECT_TRUE(RemoveFileStreamsByAddr(addr1 + kPageSize / 2, kPageSize));
  EXPECT_EQ(addr1 + kPageSize / 2, stream->last_munmap_addr);
  EXPECT_EQ(kPageSize, stream->last_munmap_length);

  // Remove the 4th page.
  for (size_t i = kPageSize * 3; i < kPageSize * 4; ++i) {
    // Delete 0-1, 4-5, 8-9, .. elements.
    if (i % 4 == 0) {
      EXPECT_TRUE(RemoveFileStreamsByAddr(addr1 + i, 2)) << i;
      EXPECT_EQ(addr1 + i, stream->last_munmap_addr);
      EXPECT_EQ(2U, stream->last_munmap_length);
    }
  }
  for (size_t i = kPageSize * 4 - 1; i >= kPageSize * 3; --i) {
    // Delete 2, 3, 6, 7, .. elements.
    if (i % 4 == 2) {
      EXPECT_TRUE(RemoveFileStreamsByAddr(addr1 + i, 2)) << i;
      EXPECT_EQ(addr1 + i, stream->last_munmap_addr);
      EXPECT_EQ(2U, stream->last_munmap_length);
    }
  }

  // Confirm all elements are now gone.
  EXPECT_EQ(0U, GetAddrMapSize());
}

TEST_F(MemoryRegionTest, TestAddRemoveStreamByAddrDupRegion) {
  static const size_t kSize = 16;
  struct TestAddresses {
    char region0[kSize];
    char region1[kSize];
  } addresses ALIGN_(2);

  // Initially the tree is empty.
  EXPECT_EQ(0U, GetAddrMapSize());
  EXPECT_FALSE(RemoveFileStreamsByAddr(addresses.region1, kSize));
  // The second/third AddFileStreamByAddr calls should succeed.
  scoped_refptr<StubFileStream> stream = new StubFileStream(true);
  EXPECT_TRUE(AddFileStreamByAddr(addresses.region1, kSize, stream));
  EXPECT_TRUE(HasMemoryRegion(addresses.region1, kSize));
  EXPECT_TRUE(AddFileStreamByAddr(addresses.region1, kSize, stream));
  EXPECT_TRUE(HasMemoryRegion(addresses.region1, kSize));
  EXPECT_TRUE(AddFileStreamByAddr(addresses.region1, kSize, stream));
  EXPECT_TRUE(HasMemoryRegion(addresses.region1, kSize));
  EXPECT_EQ(2U, GetAddrMapSize());  // not 6. it's ref-counted.
  // Try to remove regions which overlap |region1| in many ways. They should all
  // fail except the "exactly the same" case.

  // Exactly the same. Ref count decreases to 2.
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region1, kSize));
  // Left aligned.
  EXPECT_FALSE(RemoveFileStreamsByAddr(addresses.region1, kSize - 2));
  // Right aligned.
  EXPECT_FALSE(RemoveFileStreamsByAddr(addresses.region1 + kSize - 2, 2));
  // Overlaps left, right aligned.
  EXPECT_FALSE(RemoveFileStreamsByAddr(addresses.region1 + (kSize - 2),
                                       kSize + 2));
  // Overlaps right, left aligned.
  EXPECT_FALSE(RemoveFileStreamsByAddr(addresses.region1, kSize + 2));
  // Overlaps both.
  EXPECT_FALSE(RemoveFileStreamsByAddr(addresses.region1 + (kSize - 2),
                                       kSize + 4));
  // Overlaps left.
  EXPECT_FALSE(RemoveFileStreamsByAddr(addresses.region1 + (kSize - 2), kSize));
  // Overlaps right.
  EXPECT_FALSE(RemoveFileStreamsByAddr(addresses.region1 + 2, kSize));
  // Contained.
  EXPECT_FALSE(RemoveFileStreamsByAddr(addresses.region1 + 2, kSize - 4));
  // Ref count should still be 2.
  EXPECT_TRUE(HasMemoryRegion(addresses.region1, kSize));
  EXPECT_EQ(2U, GetAddrMapSize());

  // Remove twice. Ref count goes down to 0.
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region1, kSize));
  EXPECT_TRUE(HasMemoryRegion(addresses.region1, kSize));
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region1, kSize));
  EXPECT_EQ(addresses.region1, stream->last_munmap_addr);
  EXPECT_EQ(kSize, stream->last_munmap_length);
  EXPECT_FALSE(HasMemoryRegion(addresses.region1, kSize));
  EXPECT_EQ(0U, GetAddrMapSize());
}

// Resets the map, then adds |stream| to the map in the following way:
//
// * region[0] to [3] and [8] to [11] are unused.
// * region[4] to [7] are allocated at once and backed by |stream|.
//
//  0 1 2 3 4 5 6 7 8 9 A B
// |E|E|E|E|S|S|S|S|E|E|E|E|
#define RESET4()                                                             \
  do {                                                                       \
    ClearAddrMap();                                                          \
    EXPECT_TRUE(AddFileStreamByAddr(addresses.region4, kBlockSize, stream)); \
    EXPECT_TRUE(HasMemoryRegion(addresses.region4, kBlockSize));             \
    EXPECT_EQ(2U, GetAddrMapSize());                                         \
  } while (false)

// Tests Add/RemoveStreamByAddr usage with FileStream that does not return
// the same address for multiple mmap() requests.
TEST_F(MemoryRegionTest, TestAddRemovePosixCompliantFileStream) {
  static const size_t kSize = 2;
  static const size_t kBlockSize = kSize * 4;
  struct TestAddresses {
    char region0[kSize];
    char region1[kSize];
    char region2[kSize];
    char region3[kSize];
    char region4[kSize];
    char region5[kSize];
    char region6[kSize];
    char region7[kSize];
    char region8[kSize];
    char region9[kSize];
    char region10[kSize];
    char region11[kSize];
  } addresses ALIGN_(2);

  // Initially the tree is empty.
  EXPECT_EQ(0U, GetAddrMapSize());
  EXPECT_FALSE(RemoveFileStreamsByAddr(addresses.region1, kSize));
  scoped_refptr<StubFileStream> stream = new StubFileStream(false);
  // Try to remove regions which overlap |region1| in many ways. They should all
  // pass.

  RESET4();
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region4, kBlockSize));
  EXPECT_EQ(0U, GetAddrMapSize());
  // Left aligned.
  RESET4();
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region4, kSize * 3));
  EXPECT_EQ(2U, GetAddrMapSize());
  // Right aligned.
  RESET4();
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region5, kSize * 3));
  EXPECT_EQ(2U, GetAddrMapSize());
  // Overlaps left, right aligned.
  RESET4();
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region3, kSize * 5));
  EXPECT_EQ(0U, GetAddrMapSize());
  // Overlaps right, left aligned.
  RESET4();
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region4, kSize * 5));
  EXPECT_EQ(0U, GetAddrMapSize());
  // Overlaps both.
  RESET4();
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region2, kSize * 7));
  EXPECT_EQ(0U, GetAddrMapSize());
  // Overlaps left.
  RESET4();
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region1, kSize * 4));
  EXPECT_EQ(2U, GetAddrMapSize());
  // Overlaps right.
  RESET4();
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region6, kSize * 4));
  EXPECT_EQ(2U, GetAddrMapSize());
  // Contained.
  RESET4();
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region5, kSize * 2));
  EXPECT_TRUE(HasMemoryRegion(addresses.region4, kSize));
  EXPECT_TRUE(HasMemoryRegion(addresses.region7, kSize));
  EXPECT_EQ(4U, GetAddrMapSize());

  // Remove twice. The second call should fail, but break nothing.
  RESET4();
  EXPECT_TRUE(RemoveFileStreamsByAddr(addresses.region4, kBlockSize));
  EXPECT_FALSE(HasMemoryRegion(addresses.region4, kBlockSize));
  EXPECT_EQ(0U, GetAddrMapSize());
  EXPECT_FALSE(RemoveFileStreamsByAddr(addresses.region4, kBlockSize));
  EXPECT_FALSE(HasMemoryRegion(addresses.region4, kBlockSize));
  EXPECT_EQ(0U, GetAddrMapSize());
}

// Tests RemoveStreamByAddr usage with |call_munmap| to check if underlying
// munmap() is called or not called.
TEST_F(MemoryRegionTest, TestRemoveStreamWithoutMunmap) {
  static const size_t kSize = 8;
  struct {
    char addr1[kSize];
    char addr2[kSize];
    char addr3[kSize];
  } m ALIGN_(2);

  scoped_refptr<StubFileStream> stream = new StubFileStream(true);

  EXPECT_TRUE(AddFileStreamByAddr(m.addr1, kSize, stream));
  EXPECT_TRUE(HasMemoryRegion(m.addr1, kSize));
  EXPECT_EQ(2U, GetAddrMapSize());
  EXPECT_TRUE(AddFileStreamByAddr(m.addr2, kSize, stream));
  EXPECT_TRUE(HasMemoryRegion(m.addr2, kSize));
  EXPECT_EQ(4U, GetAddrMapSize());
  EXPECT_TRUE(AddFileStreamByAddr(m.addr3, kSize, stream));
  EXPECT_TRUE(HasMemoryRegion(m.addr3, kSize));
  EXPECT_EQ(6U, GetAddrMapSize());
  EXPECT_EQ(0U, stream->munmap_count);
  EXPECT_TRUE(RemoveFileStreamsByAddr(m.addr1, kSize));
  EXPECT_EQ(4U, GetAddrMapSize());
  EXPECT_EQ(1U, stream->munmap_count);
  EXPECT_EQ(m.addr1, stream->last_munmap_addr);
  EXPECT_EQ(kSize, stream->last_munmap_length);

  // RemoveFileStreamByAddrWithoutMunmap() should not call underlying munmap()
  // implementation.
  EXPECT_TRUE(RemoveFileStreamsByAddrWithoutMunmap(m.addr2, kSize));
  EXPECT_EQ(2U, GetAddrMapSize());
  EXPECT_EQ(1U, stream->munmap_count);

  // And RemoveFileStreamByAddr() should call the munmap().
  EXPECT_TRUE(RemoveFileStreamsByAddr(m.addr3, kSize));
  EXPECT_EQ(0U, GetAddrMapSize());
  EXPECT_EQ(2U, stream->munmap_count);
  EXPECT_EQ(m.addr3, stream->last_munmap_addr);
  EXPECT_EQ(kSize, stream->last_munmap_length);
}

TEST_F(MemoryRegionTest, TestSetAdviceByAddr) {
  static const size_t kSize = 8;
  struct {
    char addr1[kSize];
    char addr2[kSize];
    char addr3[kSize];
  } m ALIGN_(2);

  scoped_refptr<StubFileStream> stream = new StubFileStream(false);
  EXPECT_TRUE(AddFileStreamByAddr(m.addr1, kSize, stream));
  EXPECT_TRUE(HasMemoryRegion(m.addr1, kSize));
  EXPECT_EQ(2U, GetAddrMapSize());
  EXPECT_TRUE(AddFileStreamByAddr(m.addr2, kSize, stream));
  EXPECT_TRUE(HasMemoryRegion(m.addr1, kSize));
  EXPECT_EQ(4U, GetAddrMapSize());

  // It always pass on zero length.
  EXPECT_TRUE(SetAdviceByAddr(NULL, 0, MADV_NORMAL));
  EXPECT_TRUE(SetAdviceByAddr(m.addr3, 0, MADV_NORMAL));

  // It passes on registered space.
  EXPECT_TRUE(SetAdviceByAddr(m.addr1, 2, MADV_NORMAL));
  EXPECT_TRUE(SetAdviceByAddr(m.addr1, kSize, MADV_NORMAL));
  EXPECT_TRUE(SetAdviceByAddr(m.addr2, kSize, MADV_NORMAL));
  EXPECT_TRUE(SetAdviceByAddr(m.addr1, kSize + 2, MADV_NORMAL));
  EXPECT_TRUE(SetAdviceByAddr(m.addr1, kSize * 2, MADV_NORMAL));

  // It fails on unmanaged space.
  // TODO(crbug.com/362862): This is not Linux compatible. Once MemoryRegion
  // can manage all regions, it should succeed even on unknown spaces.
  EXPECT_FALSE(SetAdviceByAddr(m.addr3, kSize, MADV_NORMAL));
  EXPECT_EQ(ENOSYS, errno);

  // MADV_REMOVE is not supported.
  EXPECT_FALSE(SetAdviceByAddr(m.addr1, kSize, MADV_REMOVE));
  EXPECT_EQ(ENOSYS, errno);
}

}  // namespace posix_translation
