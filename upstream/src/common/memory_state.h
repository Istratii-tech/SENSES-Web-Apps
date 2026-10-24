// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// This file defines utility functions for working with process data.

#ifndef COMMON_MEMORY_STATE_H_
#define COMMON_MEMORY_STATE_H_

#include <link.h>
#include <sys/types.h>

#include <map>
#include <string>
#include <vector>

#include "common/private/minimal_base.h"
#include "gtest/gtest_prod.h"

template <typename T> struct DefaultSingletonTraits;

namespace base {
class Lock;
}  // namespace base

namespace arc {

class BacktraceInterface;

// Describes a single segment from an ELF file in memory.
class ProcessMapHeader {
 public:
  uint32_t GetType() const { return type_; }
  std::string GetLibrary() const { return library_; }
  uintptr_t GetBaseAddress() const { return base_address_; }
  uintptr_t GetObjectAddress() const { return object_address_; }
  uintptr_t GetVirtualAddress() const {
    return base_address_ + object_address_;
  }
  uintptr_t GetMemorySize() const { return memory_size_; }
  uintptr_t GetFileOffset() const { return file_offset_; }
  uint32_t GetFlags() const { return flags_; }

  std::string GetTypeStr() const;
  std::string GetFlagsStr() const;

  std::string ConvertToString() const;
  std::string ConvertToJSON() const;

  typedef std::vector<ProcessMapHeader> List;

  // Dumps layout of the current process.
  static void DumpLayout(List* list);
  static void SortByVirtualAddress(List* list);
  static void AddSyntheticLibraries(
      size_t loader_size, size_t irt_size, List* list);

  // Prints layout of the current process to stderr.
  // To dump code segments call PrintLayout(PT_LOAD).
  static void PrintLayout(int type_filter);

  static std::string ConvertListToJSON(const List& list);

 private:
  FRIEND_TEST(ProcessMapHeaderTest, ConvertToJSON);
  FRIEND_TEST(ProcessMapHeaderTest, ConvertListToJSON);

  ProcessMapHeader();  // No default constructor.
  ProcessMapHeader(const struct dl_phdr_info* info, int idx);

  static int DumpPhdrCallback(
      struct dl_phdr_info* info, size_t size, void *data);

  static bool CompareByVirtualAddress(
      ProcessMapHeader i, ProcessMapHeader j);

  int type_;
  std::string library_;
  uintptr_t base_address_;
  uintptr_t object_address_;
  uintptr_t memory_size_;
  uintptr_t file_offset_;
  uint32_t flags_;
};

// TODO(crbug.com/238463): Drop this.
// This structure matches the current unstable abi of the NaCl list_mappings
// syscall. Eventually a public header will be provided when the interface is
// stable and we should use that instead.
struct NaClMemMappingInfo {
  uint32_t start;
  uint32_t size;
  uint32_t prot;
  uint32_t max_prot;
  uint32_t vmmap_type;
};

// Records backtraces when mmap is done.
struct MemoryMappingBacktrace;
class MemoryMappingBacktraceMap {
 public:
  static MemoryMappingBacktraceMap* GetInstance();

  void MapCurrentStackFrame(void *addr, size_t length);
  void Unmap(void *addr, size_t length);
  std::string ConvertBacktraceToJSON(void *addr);

  static int GetUninterestingLayers();

 private:
  friend struct DefaultSingletonTraits<MemoryMappingBacktraceMap>;
  friend class MemoryMappingBacktraceMapTest;

  MemoryMappingBacktraceMap();
  ~MemoryMappingBacktraceMap();

  // Hook to allow mocking of backtracing.
  BacktraceInterface* backtracer_;

  typedef std::map<uintptr_t, MemoryMappingBacktrace*> Map;

  Map memory_;
  // TODO(crbug.com/391661): Use std::unique_ptr once we completely migrate to
  // clang.
  base::Lock* mu_;

  COMMON_DISALLOW_COPY_AND_ASSIGN(MemoryMappingBacktraceMap);
};

// Describes a single mmap memory mapping region.
class MemoryMappingInfo {
 public:
  uint32_t GetStart() const { return info_.start; }
  uint32_t GetSize() const { return info_.size; }
  uint32_t GetProtection() const { return info_.prot; }
  uint32_t GetMaximumProtection() const { return info_.max_prot; }
  uint32_t GetVmmapType() const { return info_.vmmap_type; }

  std::string ConvertToJSON() const;

  typedef std::vector<MemoryMappingInfo> List;

  // Dump mappings in the current process (sorted by start address).
  static void DumpRegions(List* list);

  static std::string ConvertListToJSON(const List& list);
  static void ExtractNaClSizes(
      const List& list, size_t* loader_size, size_t* irt_size);

 private:
  FRIEND_TEST(MemoryMappingInfoTest, ConvertToJSON);
  FRIEND_TEST(MemoryMappingInfoTest, ConvertListToJSON);

  MemoryMappingInfo();  // No default constructor.
  explicit MemoryMappingInfo(const struct NaClMemMappingInfo* info);

  NaClMemMappingInfo info_;
};

// Place to collect all interesting memory state snapshot related functionality.
class MemoryState {
 public:
  static std::string DumpAsJSON();
 private:
  MemoryState();

  static std::string DumpMallocInfoAsJSON();

  COMMON_DISALLOW_COPY_AND_ASSIGN(MemoryState);
};

}  // namespace arc

#endif  // COMMON_MEMORY_STATE_H_
