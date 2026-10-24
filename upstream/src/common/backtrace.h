// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// This file provides a utility to output backtrace.

#ifndef COMMON_BACKTRACE_H_
#define COMMON_BACKTRACE_H_

#include <string>

namespace arc {

class BacktraceInterface {
 public:
  static BacktraceInterface* Get();
  static void Print();
  virtual ~BacktraceInterface() {}
  virtual int Backtrace(void** buffer, int size) = 0;
  virtual char** BacktraceSymbols(void* const* buffer, int size) = 0;

  static std::string Demangle(const std::string& str);
  static std::string DemangleAll(const std::string& str);
};

}  // namespace arc

#endif  // COMMON_BACKTRACE_H_
