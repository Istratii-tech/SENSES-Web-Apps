// Replacement for ARC's common/trace_event.cc, which forwarded trace events to
// PPB_Trace_Event_Dev. Tracing is disabled in the wasm port: every category
// reports "not enabled", so TRACE_EVENT* macros reduce to a load and a branch.

#include "common/trace_event.h"

namespace arc {
namespace trace {

void SetThreadName(const char* name) {}

const unsigned char* GetCategoryEnabled(const char* category_name) {
  static const unsigned char kDisabled = 0;
  return &kDisabled;
}

void AddTraceEvent(char phase,
                   const unsigned char* category_enabled,
                   const char* name,
                   uint64_t id,
                   int num_args,
                   const char** arg_names,
                   const unsigned char* arg_types,
                   const uint64_t* arg_values,
                   unsigned char flags) {}

}  // namespace trace
}  // namespace arc
