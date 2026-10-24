// android::CallStack (system/core/libutils/CallStack.cpp) without
// libbacktrace: there is no way to unwind native frames in wasm from C++,
// so a call stack is always empty. Code that records or logs stacks for
// debugging (libui's Region, RefBase's DEBUG_REFS) links and does nothing.
// Part of the arc-wasm shim layer.

#include <utils/CallStack.h>
#include <utils/Printer.h>

namespace android {

CallStack::CallStack() {}

CallStack::CallStack(const char* logtag, int32_t ignoreDepth) {
  update(ignoreDepth + 1);
  log(logtag);
}

CallStack::~CallStack() {}

void CallStack::update(int32_t /*ignoreDepth*/, pid_t /*tid*/) { mFrameLines.clear(); }

void CallStack::log(const char* logtag, android_LogPriority priority, const char* prefix) const {
  LogPrinter printer(logtag, priority, prefix, /*ignoreBlankLines*/ false);
  print(printer);
}

void CallStack::dump(int fd, int indent, const char* prefix) const {
  FdPrinter printer(fd, indent, prefix);
  print(printer);
}

String8 CallStack::toString(const char* prefix) const {
  String8 str;
  String8Printer printer(&str, prefix);
  print(printer);
  return str;
}

void CallStack::print(Printer& printer) const {
  for (size_t i = 0; i < mFrameLines.size(); i++)
    printer.printLine(mFrameLines[i].string());
}

}  // namespace android
