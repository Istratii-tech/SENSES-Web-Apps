// Replacement for ARC's posix_translation/test_util/sysconf_util.cc.
//
// Upstream overrides libc's sysconf() and falls back to the real one via
// dlopen("libc.so"). A statically linked wasm module has no libc.so, so the
// fallback answers the queries posix_translation makes itself.

#include "posix_translation/test_util/sysconf_util.h"

#include <errno.h>
#include <limits.h>
#include <unistd.h>

#if defined(__EMSCRIPTEN__)
#include <emscripten/threading.h>
#endif

namespace posix_translation {

namespace {

int g_online = -1;
int g_configured = -1;

long NumProcessors() {  // NOLINT
#if defined(__EMSCRIPTEN__)
  return emscripten_num_logical_cores();
#else
  return 1;
#endif
}

}  // namespace

ScopedNumProcessorsOnlineSetting::ScopedNumProcessorsOnlineSetting(int num) {
  g_online = num;
}

ScopedNumProcessorsOnlineSetting::~ScopedNumProcessorsOnlineSetting() {
  g_online = -1;
}

ScopedNumProcessorsConfiguredSetting::ScopedNumProcessorsConfiguredSetting(
    int num) {
  g_configured = num;
}

ScopedNumProcessorsConfiguredSetting::~ScopedNumProcessorsConfiguredSetting() {
  g_configured = -1;
}

}  // namespace posix_translation

// Overrides libc's sysconf(). Static linking means libc's object is simply
// not pulled in, so this definition wins.
extern "C" long sysconf(int name) {  // NOLINT
  using posix_translation::g_configured;
  using posix_translation::g_online;
  switch (name) {
    case _SC_NPROCESSORS_ONLN:
      return g_online == -1 ? posix_translation::NumProcessors() : g_online;
    case _SC_NPROCESSORS_CONF:
      return g_configured == -1 ? posix_translation::NumProcessors()
                                : g_configured;
    case _SC_PAGESIZE:
      return PAGE_SIZE;
    case _SC_OPEN_MAX:
      return 1024;
    case _SC_CLK_TCK:
      return 100;
    default:
      errno = EINVAL;
      return -1;
  }
}
