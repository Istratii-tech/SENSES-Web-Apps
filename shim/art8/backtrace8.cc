// libbacktrace (system/core/libbacktrace) for the Android 8.1 stack in
// WebAssembly: nothing can unwind a wasm stack from C++ (the engine owns
// it; Runtime::Abort prints it through emscripten_log instead), and there
// is no /proc/self/maps to parse. So Backtrace::Unwind finds no frames and
// BacktraceMap knows no mappings; ART then prints "(no native stack)" in its
// thread dumps and skips its /proc/self/maps checks. Part of the arc-wasm
// shim layer.

#include <backtrace/Backtrace.h>
#include <backtrace/BacktraceMap.h>
#include <inttypes.h>
#include <stdio.h>

namespace {

class NoBacktrace : public Backtrace {
 public:
  NoBacktrace(pid_t pid, pid_t tid, BacktraceMap* map) : Backtrace(pid, tid, map) {}
  bool Unwind(size_t, ucontext_t*) override {
    error_ = BACKTRACE_UNWIND_ERROR_UNSUPPORTED_OPERATION;
    return false;
  }
  bool ReadWord(uintptr_t, word_t*) override { return false; }
  size_t Read(uintptr_t, uint8_t*, size_t) override { return 0; }

 protected:
  std::string GetFunctionNameRaw(uintptr_t, uintptr_t*) override { return ""; }
};

class NoMap : public BacktraceMap {
 public:
  explicit NoMap(pid_t pid) : BacktraceMap(pid) {}
  bool Build() override { return true; }
};

}  // namespace

Backtrace::Backtrace(pid_t pid, pid_t tid, BacktraceMap* map)
    : pid_(pid), tid_(tid), map_(map), map_shared_(true), error_(BACKTRACE_UNWIND_NO_ERROR) {}

Backtrace::~Backtrace() {}

Backtrace* Backtrace::Create(pid_t pid, pid_t tid, BacktraceMap* map) {
  return new NoBacktrace(pid, tid, map);
}

Backtrace* Backtrace::CreateOffline(pid_t pid, pid_t tid, BacktraceMap* map,
                                    const backtrace_stackinfo_t&, bool) {
  return new NoBacktrace(pid, tid, map);
}

std::string Backtrace::GetFunctionName(uintptr_t pc, uintptr_t* offset, const backtrace_map_t*) {
  *offset = 0;
  return "";
}

void Backtrace::FillInMap(uintptr_t, backtrace_map_t* map) {
  *map = backtrace_map_t();
}

bool Backtrace::VerifyReadWordArgs(uintptr_t, word_t*) { return false; }

std::string Backtrace::FormatFrameData(size_t frame_num) {
  return frame_num < frames_.size() ? FormatFrameData(&frames_[frame_num]) : "";
}

std::string Backtrace::FormatFrameData(const backtrace_frame_data_t* frame) {
  char buf[64];
  snprintf(buf, sizeof(buf), "#%02zu pc %" PRIPTR, frame->num, frame->rel_pc);
  return buf;
}

std::string Backtrace::GetErrorString(BacktraceUnwindError error) {
  return error == BACKTRACE_UNWIND_NO_ERROR ? "No error" : "Unwinding is not supported in wasm";
}

BacktraceMap::BacktraceMap(pid_t pid) : pid_(pid) {}

BacktraceMap::~BacktraceMap() {}

BacktraceMap* BacktraceMap::Create(pid_t pid, bool) {
  return new NoMap(pid);
}

BacktraceMap* BacktraceMap::Create(pid_t pid, const std::vector<backtrace_map_t>& maps) {
  NoMap* map = new NoMap(pid);
  map->maps_.assign(maps.begin(), maps.end());
  return map;
}

void BacktraceMap::FillIn(uintptr_t addr, backtrace_map_t* map) {
  *map = backtrace_map_t();
  for (const backtrace_map_t& m : maps_) {
    if (addr >= m.start && addr < m.end) {
      *map = m;
      return;
    }
  }
}

bool BacktraceMap::Build() { return true; }

bool BacktraceMap::ParseLine(const char*, backtrace_map_t*) { return false; }
