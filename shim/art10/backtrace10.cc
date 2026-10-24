// libbacktrace (system/core/libbacktrace) for the Android 10 stack in
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
  bool Unwind(size_t, void*) override {
    error_.error_code = BACKTRACE_UNWIND_ERROR_UNSUPPORTED_OPERATION;
    return false;
  }
  bool ReadWord(uint64_t, word_t*) override { return false; }
  size_t Read(uint64_t, uint8_t*, size_t) override { return 0; }

 protected:
  std::string GetFunctionNameRaw(uint64_t, uint64_t*) override { return ""; }
};

class NoMap : public BacktraceMap {
 public:
  explicit NoMap(pid_t pid) : BacktraceMap(pid) {}
  bool Build() override { return true; }
};

}  // namespace

Backtrace::Backtrace(pid_t pid, pid_t tid, BacktraceMap* map)
    : pid_(pid), tid_(tid), map_(map), map_shared_(true) {}

Backtrace::~Backtrace() {}

Backtrace* Backtrace::Create(pid_t pid, pid_t tid, BacktraceMap* map) {
  return new NoBacktrace(pid, tid, map);
}

Backtrace* Backtrace::CreateOffline(pid_t pid, pid_t tid, BacktraceMap* map,
                                    const backtrace_stackinfo_t&, bool) {
  return new NoBacktrace(pid, tid, map);
}

std::string Backtrace::GetFunctionName(uint64_t pc, uint64_t* offset, const backtrace_map_t*) {
  *offset = 0;
  return "";
}

void Backtrace::FillInMap(uint64_t, backtrace_map_t* map) {
  *map = backtrace_map_t();
}

bool Backtrace::VerifyReadWordArgs(uint64_t, word_t*) { return false; }

std::string Backtrace::FormatFrameData(size_t frame_num) {
  return frame_num < frames_.size() ? FormatFrameData(&frames_[frame_num]) : "";
}

std::string Backtrace::FormatFrameData(const backtrace_frame_data_t* frame) {
  char buf[64];
  snprintf(buf, sizeof(buf), "#%02zu pc %08" PRIx64, frame->num, frame->rel_pc);
  return buf;
}

std::string Backtrace::GetErrorString(BacktraceUnwindError error) {
  return error.error_code == BACKTRACE_UNWIND_NO_ERROR ? "No error" : "Unwinding is not supported in wasm";
}

BacktraceMap::BacktraceMap(pid_t pid) : pid_(pid) {}

BacktraceMap::~BacktraceMap() {}

BacktraceMap* BacktraceMap::Create(pid_t pid, bool) {
  return new NoMap(pid);
}

BacktraceMap* BacktraceMap::CreateOffline(pid_t pid, const std::vector<backtrace_map_t>& maps) {
  NoMap* map = new NoMap(pid);
  map->maps_.assign(maps.begin(), maps.end());
  return map;
}

void BacktraceMap::FillIn(uint64_t addr, backtrace_map_t* map) {
  *map = backtrace_map_t();
  for (const backtrace_map_t& m : maps_) {
    if (addr >= m.start && addr < m.end) {
      *map = m;
      return;
    }
  }
}

bool BacktraceMap::Build() { return true; }

