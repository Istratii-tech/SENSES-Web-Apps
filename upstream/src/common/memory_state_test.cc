// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Test memory state related classes.

#include "base/compiler_specific.h"
#include "common/backtrace.h"
#include "common/memory_state.h"
#include "gtest/gtest.h"
#include "gmock/gmock.h"

namespace arc {

using ::testing::Ge;
using ::testing::NotNull;
using ::testing::Return;
using ::testing::_;

class ProcessMapHeaderTest : public testing::Test {
 public:
  virtual void SetUp() OVERRIDE;
 protected:
  struct dl_phdr_info info_;
  ElfW(Phdr) phdr_[3];
};

void ProcessMapHeaderTest::SetUp() {
  memset(&info_, 0, sizeof(info_));
  memset(phdr_, 0, sizeof(phdr_));

  info_.dlpi_addr = 1000000;
  info_.dlpi_name = "foo.so";
  info_.dlpi_phdr = phdr_;
  info_.dlpi_phnum = sizeof(phdr_) / sizeof(*phdr_);

  static const int types[] = {PT_LOAD, PT_DYNAMIC, PT_SHLIB};
  for (int i = 0; i < info_.dlpi_phnum; ++i) {
    phdr_[i].p_type = types[i];
    phdr_[i].p_vaddr = 2000000 + i;
    phdr_[i].p_memsz = 3000000 + i;
    phdr_[i].p_offset = 4000000 + i;
    phdr_[i].p_flags = 5000000 + i;
  }
}

TEST_F(ProcessMapHeaderTest, ConvertToJSON) {
  ProcessMapHeader pmh(&info_, 1);

  EXPECT_EQ(
      "{"
          "\"type\":\"DYNAMIC\","
          "\"library\":\"foo.so\","
          "\"baseAddress\":1000000,"
          "\"objectAddress\":2000001,"
          "\"memorySize\":3000001,"
          "\"fileOffset\":4000001,"
          "\"flags\":5000001"
      "}", pmh.ConvertToJSON());
}

TEST_F(ProcessMapHeaderTest, ConvertListToJSON) {
  ProcessMapHeader::List list;
  for (int i = 0; i < info_.dlpi_phnum; ++i) {
    list.push_back(ProcessMapHeader(&info_, i));
  }

  EXPECT_EQ(
      "["
          "{"
              "\"type\":\"LOAD\","
              "\"library\":\"foo.so\","
              "\"baseAddress\":1000000,"
              "\"objectAddress\":2000000,"
              "\"memorySize\":3000000,"
              "\"fileOffset\":4000000,"
              "\"flags\":5000000"
          "},"
          "{"
              "\"type\":\"DYNAMIC\","
              "\"library\":\"foo.so\","
              "\"baseAddress\":1000000,"
              "\"objectAddress\":2000001,"
              "\"memorySize\":3000001,"
              "\"fileOffset\":4000001,"
              "\"flags\":5000001"
          "},"
          "{"
              "\"type\":\"SHLIB\","
              "\"library\":\"foo.so\","
              "\"baseAddress\":1000000,"
              "\"objectAddress\":2000002,"
              "\"memorySize\":3000002,"
              "\"fileOffset\":4000002,"
              "\"flags\":5000002"
          "}"
      "]", ProcessMapHeader::ConvertListToJSON(list));
}

class MemoryMappingInfoTest : public testing::Test {
 public:
  virtual void SetUp() OVERRIDE;
 protected:
  struct NaClMemMappingInfo info_[3];
};

void MemoryMappingInfoTest::SetUp() {
  for (uint i = 0; i < sizeof(info_) / sizeof(*info_); ++i) {
    info_[i].start = 1000 + i;
    info_[i].size = 2000 + i;
    info_[i].prot = 3000 + i;
    info_[i].max_prot = 4000 + i;
    info_[i].vmmap_type = 5000 + i;
  }
}

TEST_F(MemoryMappingInfoTest, ConvertToJSON) {
  MemoryMappingInfo mmi(&info_[1]);

  EXPECT_EQ(
      "{"
          "\"start\":1001,"
          "\"size\":2001,"
          "\"prot\":3001,"
          "\"maxProt\":4001,"
          "\"vmmapType\":5001,"
          "\"backtrace\":[]"
      "}", mmi.ConvertToJSON());
}

TEST_F(MemoryMappingInfoTest, ConvertListToJSON) {
  MemoryMappingInfo::List list;
  for (uint i = 0; i < sizeof(info_) / sizeof(*info_); ++i) {
    list.push_back(MemoryMappingInfo(&info_[i]));
  }

  EXPECT_EQ(
      "["
          "{"
          "\"start\":1000,"
          "\"size\":2000,"
          "\"prot\":3000,"
          "\"maxProt\":4000,"
          "\"vmmapType\":5000,"
          "\"backtrace\":[]"
          "},"
          "{"
          "\"start\":1001,"
          "\"size\":2001,"
          "\"prot\":3001,"
          "\"maxProt\":4001,"
          "\"vmmapType\":5001,"
          "\"backtrace\":[]"
          "},"
          "{"
          "\"start\":1002,"
          "\"size\":2002,"
          "\"prot\":3002,"
          "\"maxProt\":4002,"
          "\"vmmapType\":5002,"
          "\"backtrace\":[]"
          "}"
      "]", MemoryMappingInfo::ConvertListToJSON(list));
}

class MockBacktracer : public BacktraceInterface {
 public:
  MOCK_METHOD2(Backtrace, int(void**buffer, int size));
  MOCK_METHOD2(BacktraceSymbols, char**(void*const* buffer, int size));
};

class MemoryMappingBacktraceMapTest : public testing::Test {
 public:
  virtual void SetUp() OVERRIDE;
  virtual void TearDown() OVERRIDE;
 protected:
  MemoryMappingBacktraceMap* map_;
  MockBacktracer* backtracer_;

  char** NewTrace(int interesting_items, int* out_skipped);

  char** NewHalfMangledTrace();
  int GetHalfMangledTraceSize();

  char** NewSimpleTrace();
  int GetSimpleTraceSize();

  void ClippingTest(
      int test_range,
      int map_start, int map_size, int unmap_start, int unmap_size,
      int range1_start, int range1_size, int range2_start, int range2_size);
};

void MemoryMappingBacktraceMapTest::SetUp() {
  backtracer_ = new MockBacktracer;
  map_ = new MemoryMappingBacktraceMap();
  delete map_->backtracer_;
  map_->backtracer_ = backtracer_;
}

void MemoryMappingBacktraceMapTest::TearDown() {
  delete map_;
}

char** MemoryMappingBacktraceMapTest::NewTrace(
    int interesting_items, int* out_skipped) {
  // Using malloc because that's what backtrace_symbols uses.
  int skipped = MemoryMappingBacktraceMap::GetUninterestingLayers();
  char** trace = static_cast<char**>(malloc(
      sizeof(*trace) * (interesting_items + skipped)));
  for (int i = 0; i < skipped; ++i) {
    trace[i] = const_cast<char*>("skipped");
  }
  *out_skipped = skipped;
  return trace;
}

int MemoryMappingBacktraceMapTest::GetHalfMangledTraceSize() {
  return MemoryMappingBacktraceMap::GetUninterestingLayers() + 2;
}

char** MemoryMappingBacktraceMapTest::NewHalfMangledTrace() {
  int skipped;
  char** trace = NewTrace(2, &skipped);
  trace[skipped + 0] = const_cast<char*>("/lib/foo.so (_Z1fv) [0x12345678]");
  trace[skipped + 1] = const_cast<char*>("/lib/bar.so _Z1fv [0x12345679]");
  return trace;
}

int MemoryMappingBacktraceMapTest::GetSimpleTraceSize() {
  return MemoryMappingBacktraceMap::GetUninterestingLayers() + 1;
}

char** MemoryMappingBacktraceMapTest::NewSimpleTrace() {
  int skipped;
  char** trace = NewTrace(1, &skipped);
  trace[skipped + 0] = const_cast<char*>("OK");
  return trace;
}

TEST_F(MemoryMappingBacktraceMapTest, Demangling) {
  EXPECT_CALL(*backtracer_, Backtrace(NotNull(),
      Ge(GetHalfMangledTraceSize()))).WillOnce(
      Return(GetHalfMangledTraceSize()));
  map_->MapCurrentStackFrame(reinterpret_cast<void*>(0x12345678), 0x10000);

  EXPECT_CALL(*backtracer_,
      BacktraceSymbols(NotNull(), GetHalfMangledTraceSize())).WillOnce(
      Return(NewHalfMangledTrace()));
  EXPECT_EQ(
      "["
          "\"/lib/foo.so (f()) [0x12345678]\","
          "\"/lib/bar.so _Z1fv [0x12345679]\""
      "]",
      map_->ConvertBacktraceToJSON(reinterpret_cast<void*>(0x12345678)));
}

// These tests match the cases described in memory_state.cc (i - vi).

void MemoryMappingBacktraceMapTest::ClippingTest(
      int test_range,
      int map_start, int map_size, int unmap_start, int unmap_size,
      int range1_start, int range1_size, int range2_start, int range2_size) {
  EXPECT_CALL(*backtracer_, Backtrace(NotNull(),
      Ge(GetSimpleTraceSize()))).WillOnce(
      Return(GetSimpleTraceSize()));
  map_->MapCurrentStackFrame(reinterpret_cast<void*>(map_start), map_size);
  map_->Unmap(reinterpret_cast<void*>(unmap_start), unmap_size);

  for (int i = 0; i < test_range; ++i) {
    if ((i >= range1_start && i < range1_start + range1_size) ||
        (i >= range2_start && i < range2_start + range2_size)) {
      EXPECT_CALL(*backtracer_,
         BacktraceSymbols(NotNull(), GetSimpleTraceSize())).WillOnce(
         Return(NewSimpleTrace()));
    }
    std::string trace = map_->ConvertBacktraceToJSON(
        reinterpret_cast<void*>(i));
    if ((i >= range1_start && i < range1_start + range1_size) ||
        (i >= range2_start && i < range2_start + range2_size)) {
      EXPECT_EQ("[\"OK\"]", trace);
    } else {
      EXPECT_EQ("[]", trace);
    }
  }
}

TEST_F(MemoryMappingBacktraceMapTest, ClippingI) {
  // a    b             c    d
  // |-----xxxxxxxxxxxxx-----|
  //      (-------------)
  ClippingTest(
      1000, 100, 400, 200, 200,
      100, 100, 400, 100);
}

TEST_F(MemoryMappingBacktraceMapTest, ClippingII) {
  // b    a        c    d
  //      |xxxxxxxx-----|
  // (-------------)
  ClippingTest(
      1000, 200, 300, 100, 300,
      400, 100, 0, 0);
}

TEST_F(MemoryMappingBacktraceMapTest, ClippingIII) {
  // a    b        d    c
  // |-----xxxxxxxx|
  //      (-------------)
  ClippingTest(
      1000, 100, 300, 200, 300,
      100, 100, 0, 0);
}

TEST_F(MemoryMappingBacktraceMapTest, ClippingIV) {
  // b    a             d    c
  //      |xxxxxxxxxxxxx|
  // (-----------------------)
  ClippingTest(
      1000, 200, 200, 100, 400,
      0, 0, 0, 0);
}

TEST_F(MemoryMappingBacktraceMapTest, ClippingV) {
  // a     d    b     c
  // |-----|
  //            (-----)
  ClippingTest(
      1000, 100, 100, 400, 100,
      100, 100, 0, 0);
}

TEST_F(MemoryMappingBacktraceMapTest, ClippingVI) {
    // b     c    a     d
    //            |-----|
    // (-----)
  ClippingTest(
      1000, 400, 100, 100, 100,
      400, 100, 0, 0);
}

}  // namespace arc
