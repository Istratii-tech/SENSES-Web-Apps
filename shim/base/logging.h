// Minimal stand-in for chromium base/logging.h. Part of the arc-wasm shim
// layer.
#ifndef ARC_WASM_SHIM_BASE_LOGGING_H_
#define ARC_WASM_SHIM_BASE_LOGGING_H_

#include <stdlib.h>

#include <iostream>
#include <sstream>

namespace logging {

class LogMessage {
 public:
  LogMessage(const char* file, int line, const char* severity, bool fatal)
      : fatal_(fatal) {
    stream_ << "[" << severity << ":" << file << ":" << line << "] ";
  }
  ~LogMessage() {
    stream_ << std::endl;
    std::cerr << stream_.str();
    if (fatal_)
      abort();
  }
  std::ostream& stream() { return stream_; }

 private:
  bool fatal_;
  std::ostringstream stream_;
};

class LogMessageVoidify {
 public:
  void operator&(std::ostream&) {}
};

}  // namespace logging

#define LOG(severity) \
  ::logging::LogMessage(__FILE__, __LINE__, #severity, \
                        std::string(#severity) == "FATAL").stream()
#define DLOG(severity) LOG(severity)
#define VLOG(level) LOG(VERBOSE)

#define CHECK(condition)                                         \
  (condition) ? (void)0 : ::logging::LogMessageVoidify() &       \
      ::logging::LogMessage(__FILE__, __LINE__, "FATAL", true)   \
          .stream() << "Check failed: " #condition ". "

#define CHECK_EQ(a, b) CHECK((a) == (b))
#define CHECK_NE(a, b) CHECK((a) != (b))
#define CHECK_LT(a, b) CHECK((a) < (b))
#define CHECK_LE(a, b) CHECK((a) <= (b))
#define CHECK_GT(a, b) CHECK((a) > (b))
#define CHECK_GE(a, b) CHECK((a) >= (b))

#define DCHECK(condition) CHECK(condition)
#define DCHECK_EQ(a, b) CHECK_EQ(a, b)
#define DCHECK_NE(a, b) CHECK_NE(a, b)
#define DCHECK_LT(a, b) CHECK_LT(a, b)
#define DCHECK_LE(a, b) CHECK_LE(a, b)
#define DCHECK_GT(a, b) CHECK_GT(a, b)
#define DCHECK_GE(a, b) CHECK_GE(a, b)
#define NOTREACHED() DCHECK(false)
#define NOTIMPLEMENTED() LOG(ERROR) << "Not implemented reached in " << __func__

#endif  // ARC_WASM_SHIM_BASE_LOGGING_H_
