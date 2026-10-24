// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "common/backtrace.h"

#include <dlfcn.h>
#include <unwind.h>
extern "C"
char* __cxa_demangle(const char* mangled, char* buf, size_t* len, int* status);

#include <stdio.h>
#include <stdlib.h>

#include <vector>

#include "base/compiler_specific.h"
#include "base/strings/stringprintf.h"
#include "common/alog.h"
#include "common/logd_write.h"

namespace arc {

class LibgccBacktracer : public BacktraceInterface {
 public:
  virtual int Backtrace(void** buffer, int size) OVERRIDE;
  virtual char** BacktraceSymbols(void* const* buffer, int size) OVERRIDE;

 private:
  struct BufferHolder {
    BufferHolder(void** b, int s) : buffer(b), max_size(s), cnt(0) {}

    void** buffer;
    const int max_size;
    int cnt;
  };

  static _Unwind_Reason_Code BacktraceCallback(struct _Unwind_Context* ctx,
                                               void* arg);
};

_Unwind_Reason_Code LibgccBacktracer::BacktraceCallback(
    struct _Unwind_Context* ctx, void* arg) {
  BufferHolder* buf_holder = static_cast<BufferHolder*>(arg);
  ALOG_ASSERT(buf_holder->cnt < buf_holder->max_size);
  buf_holder->buffer[buf_holder->cnt] =
      reinterpret_cast<void*>(_Unwind_GetIP(ctx));
  buf_holder->cnt++;
  return (buf_holder->cnt < buf_holder->max_size ?
          _URC_NO_REASON : _URC_END_OF_STACK);
}

int LibgccBacktracer::Backtrace(void** buffer, int size) {
  ALOG_ASSERT(size > 0);
  BufferHolder buf_holder(buffer, size);
  _Unwind_Backtrace(&LibgccBacktracer::BacktraceCallback, &buf_holder);

#if defined(__i386__)
  // _Unwind_Backtrace returns only one element when .eh_frame is not
  // available. We will try backtrace based on frame pointers.
  if (buf_holder.cnt > 1)
    return buf_holder.cnt;

  // This layout assumes functions push base pointers at the
  // beginning.
  struct frame {
    struct frame* prev;
    void* ret;
  };
  frame* bp = static_cast<frame*>(__builtin_frame_address(0));
  int i;
  for (i = 0; bp && i < size; i++) {
    buffer[i] = bp->ret;
    bp = bp->prev;
  }
  return i;
#else
  return buf_holder.cnt;
#endif
}

char** LibgccBacktracer::BacktraceSymbols(void* const* buffer, int size) {
  // Create strings for each stack frame.
  std::vector<std::string> symbols;
  for (int i = 0; i < size; i++) {
    Dl_info info;
    if (!dladdr(buffer[i], &info)) {
      symbols.push_back("");
      continue;
    }
    ptrdiff_t diff = (static_cast<char*>(buffer[i]) -
                      static_cast<char*>(info.dli_saddr));
    symbols.push_back(base::StringPrintf("%s(%s+0x%tx) [%p]",
                                         info.dli_fname,
                                         info.dli_sname,
                                         diff,
                                         buffer[i]));
  }

  // Write pointers and strings in a single buffer. So, the caller can
  // free all memory allocated in this function by a single free call.
  char** ret;
  size_t buf_size = sizeof(*ret) * size;
  for (int i = 0; i < size; i++)
    buf_size += symbols[i].size() + 1;
  ret = static_cast<char**>(malloc(buf_size));
  char* str = reinterpret_cast<char*>(ret + size);
  for (int i = 0; i < size; i++) {
    ALOG_ASSERT(str + symbols[i].size() <
                reinterpret_cast<char*>(ret) + buf_size);
    ret[i] = str;
    str += snprintf(str, symbols[i].size() + 1,
                    "%s", symbols[i].c_str()) + 1;
  }
  ALOG_ASSERT(str == reinterpret_cast<char*>(ret) + buf_size);
  return ret;
}

BacktraceInterface* BacktraceInterface::Get() {
  return new LibgccBacktracer;
}

void BacktraceInterface::Print() {
  BacktraceInterface* backtracer = BacktraceInterface::Get();
  static const int kBacktraceCapacity = 100;
  void* buf[kBacktraceCapacity];
  const int size = backtracer->Backtrace(buf, kBacktraceCapacity);
  char** names = backtracer->BacktraceSymbols(buf, size);
  for (int i = 0; i < size; i++)
    WriteLog(base::StringPrintf("%s\n", DemangleAll(names[i]).c_str()));
  delete backtracer;
}

std::string BacktraceInterface::Demangle(const std::string& str) {
  int status = -1;
  char* demangled = __cxa_demangle(str.c_str(), NULL, NULL, &status);
  std::string result;
  if (demangled && status == 0) {
    result = demangled;
  } else {
    result = str;
  }
  free(demangled);
  return result;
}

std::string BacktraceInterface::DemangleAll(
    const std::string& str) {
  size_t length;

  std::string result;
  length = str.size();
  size_t i = 0;
  while (i < length) {
    if (str[i] == '(') {
      result += "(";
      size_t j = i + 1;
      while (j < length && str[j] != ')' && str[j] != '+') {
        ++j;
      }
      result += Demangle(str.substr(i + 1, j - (i + 1)));
      i = j;
    } else {
      result += str.substr(i, 1);
      ++i;
    }
  }
  return result;
}

}  // namespace arc
