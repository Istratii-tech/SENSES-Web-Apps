// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "common/backtrace.h"

#include <stdlib.h>
#include <string.h>

#include "base/logging.h"
#include "base/macros.h"
#include "base/memory/scoped_ptr.h"
#include "gtest/gtest.h"

namespace arc {

namespace {

#if !defined(NDEBUG)
std::string ConcatSymbolsToString(char** symbols, size_t size) {
  size_t total_size = 0;
  for (int i = 0; i < size; ++i)
    total_size += strlen(symbols[i]) + 1;

  std::string result;
  result.reserve(total_size);
  for (int i = 0; i < size; ++i) {
    result += symbols[i];
    result += "\n";
  }
  return result;
}
#endif

}  // namespace

TEST(Backtrace, Backtrace) {
#if defined(__arm__) && defined(NDEBUG)
  LOG(WARNING) << "Backtrace is not available on ARM release build.";
#else
  void* buffer[100];
  scoped_ptr<BacktraceInterface> backtracer(BacktraceInterface::Get());
  EXPECT_LT(0, backtracer->Backtrace(buffer, arraysize(buffer)));
#endif
}

TEST(Backtrace, Symbolize) {
#if defined(NDEBUG)
  LOG(WARNING) << "Symbolization does not work on release build.";
#else
  void* buffer[100];
  scoped_ptr<BacktraceInterface> backtracer(BacktraceInterface::Get());
  size_t size = backtracer->Backtrace(buffer, arraysize(buffer));
  ASSERT_LT(0, size);
  ASSERT_GE(arraysize(buffer), size);

  scoped_ptr<char*, base::FreeDeleter> symbols(
      backtracer->BacktraceSymbols(buffer, size));

  bool found = false;
  for (int i = 0; i < size && !found; ++i)
    found = strstr(symbols.get()[i], "Backtrace_Symbolize_Test") != nullptr;
  EXPECT_TRUE(found)
      << "The function name that we are running as a test case is"
      " arc::Backtrace_Symbolize_Test::TestBody, and that should be included "
      " in the symbolized backtrace.\n"
      << ConcatSymbolsToString(symbols.get(), size);
#endif
}

}  // namespace arc
