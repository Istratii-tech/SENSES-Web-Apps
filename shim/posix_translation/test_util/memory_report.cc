// gtest listener that reports memory growth per test (heap via mallinfo and
// pages held by the wasm mmap layer). Prints only tests that grow memory by
// at least kReportThreshold, plus a summary after each full iteration, so
// leaks show up when running with --gtest_repeat.

#include <malloc.h>
#include <stdio.h>

#include "gtest/gtest.h"
#include "wasm_mm/wasm_mmap.h"

namespace {

const long kReportThreshold = 256 * 1024;  // NOLINT

long HeapInUse() {  // NOLINT
  struct mallinfo info = mallinfo();
  return info.uordblks;
}

class MemoryReport : public ::testing::EmptyTestEventListener {
 public:
  virtual void OnTestStart(const ::testing::TestInfo& info) override {
    heap_before_ = HeapInUse();
    blocks_before_ = static_cast<long>(wasm_mm_block_bytes());  // NOLINT
  }
  virtual void OnTestEnd(const ::testing::TestInfo& info) override {
    const long heap = HeapInUse() - heap_before_;  // NOLINT
    const long blocks =  // NOLINT
        static_cast<long>(wasm_mm_block_bytes()) - blocks_before_;  // NOLINT
    if (heap >= kReportThreshold || blocks >= kReportThreshold) {
      printf("[ MEMORY   ] %s.%s heap %+ld KiB, mmap blocks %+ld KiB\n",
             info.test_suite_name(), info.name(), heap / 1024, blocks / 1024);
    }
  }
  virtual void OnTestIterationEnd(const ::testing::UnitTest&,
                                  int iteration) override {
    printf("[ MEMORY   ] after iteration %d: heap %ld KiB, mmap mapped %zu KiB"
           ", mmap blocks %zu KiB\n", iteration, HeapInUse() / 1024,
           wasm_mm_mapped_bytes() / 1024, wasm_mm_block_bytes() / 1024);
  }

 private:
  long heap_before_;  // NOLINT
  long blocks_before_;  // NOLINT
};

struct Registrar {
  Registrar() {
    ::testing::UnitTest::GetInstance()->listeners().Append(new MemoryReport);
  }
} g_registrar;

}  // namespace
