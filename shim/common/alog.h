// Replacement for ARC's common/alog.h, which was not part of the open-source
// release. Provides the Android cutils/log.h macro surface used by ARC, writing
// to stderr. Assertions stay enabled (ARC's tests rely on them). Part of the
// arc-wasm shim layer.
#ifndef COMMON_ALOG_H_
#define COMMON_ALOG_H_

#include <stdarg.h>
#include <stddef.h>

#ifndef LOG_TAG
#define LOG_TAG NULL
#endif

typedef enum android_LogPriority {
  ANDROID_LOG_UNKNOWN = 0,
  ANDROID_LOG_DEFAULT,
  ANDROID_LOG_VERBOSE,
  ANDROID_LOG_DEBUG,
  ANDROID_LOG_INFO,
  ANDROID_LOG_WARN,
  ANDROID_LOG_ERROR,
  ANDROID_LOG_FATAL,
  ANDROID_LOG_SILENT,
} android_LogPriority;

#ifdef __cplusplus
extern "C" {
#endif

int __android_log_print(int prio, const char* tag, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));
int __android_log_vprint(int prio, const char* tag, const char* fmt,
                         va_list ap);
void __android_log_assert(const char* cond, const char* tag,
                          const char* fmt, ...)
    __attribute__((noreturn, format(printf, 3, 4)));
void __android_log_assert_with_source(const char* cond, const char* tag,
                                      const char* file, int line,
                                      const char* fmt, ...)
    __attribute__((noreturn, format(printf, 5, 6)));

#ifdef __cplusplus
}
#endif

#define __android_second(dummy, second, ...) second
#define __android_rest(first, ...) , ##__VA_ARGS__

#define ALOG(priority, tag, ...) \
  __android_log_print(ANDROID_##priority, tag, __VA_ARGS__)

#define ALOGV(...) ((void)0)
#define ALOGD(...) ((void)__android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__))
#define ALOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__))
#define ALOGW(...) ((void)__android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__))
#define ALOGE(...) ((void)__android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__))

#define ALOGV_IF(cond, ...) ((void)0)
#define ALOGD_IF(cond, ...) ((cond) ? (void)ALOGD(__VA_ARGS__) : (void)0)
#define ALOGI_IF(cond, ...) ((cond) ? (void)ALOGI(__VA_ARGS__) : (void)0)
#define ALOGW_IF(cond, ...) ((cond) ? (void)ALOGW(__VA_ARGS__) : (void)0)
#define ALOGE_IF(cond, ...) ((cond) ? (void)ALOGE(__VA_ARGS__) : (void)0)

// The "## __VA_ARGS__" form lets callers omit the message entirely.
#define LOG_ALWAYS_FATAL_IF(cond, ...)                                   \
  ((cond) ? __android_log_assert_with_source(#cond, LOG_TAG, __FILE__,   \
                                             __LINE__, "" __VA_ARGS__)   \
          : (void)0)
#define LOG_ALWAYS_FATAL(...)                                            \
  __android_log_assert_with_source(NULL, LOG_TAG, __FILE__, __LINE__,    \
                                   "" __VA_ARGS__)
#define LOG_FATAL_IF(cond, ...) LOG_ALWAYS_FATAL_IF(cond, ## __VA_ARGS__)
#define LOG_FATAL(...) LOG_ALWAYS_FATAL(__VA_ARGS__)
#define ALOG_ASSERT(cond, ...) LOG_FATAL_IF(!(cond), ## __VA_ARGS__)

#ifdef __cplusplus
#include <string>
namespace arc {

typedef void (*LogWriter)(const void* buf, size_t count);

// Mirrors ARC's crash-report hook; unused in the wasm port.
enum CrashLogMessageKind {
  ReportableOnlyForTesters,
  ReportableForAllUsers,
};

void SetLogWriter(LogWriter writer);
void WriteLog(const std::string& log);
void WriteLog(const char* log, size_t log_size);
bool ShouldLog(int priority);

}  // namespace arc
#endif

#endif  // COMMON_ALOG_H_
