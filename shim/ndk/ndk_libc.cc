// bionic's libc ABI (Android 4.4, ARM) for guest code, on top of musl and
// the arc-wasm kernel. Most functions take the same arguments on both
// sides and are called directly; the rest convert what differs: errno
// numbers, 32-bit time_t/off_t, struct stat/timeval/timespec/addrinfo/
// passwd layouts, open() flags, fnmatch flags, ctype tables, callbacks into
// guest code (qsort, atexit), setjmp/longjmp. Stdio is in ndk_stdio.cc,
// pthread in ndk_pthread.cc. Part of the arc-wasm shim layer.

#include <ctype.h>
#include <dirent.h>
#include <emscripten/emscripten.h>
#include <emscripten/heap.h>
#include <emscripten/threading.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <locale.h>
#include <malloc.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <sched.h>
#include <search.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <vector>

#include "../membudget/mem_budget.h"
#include "../wasm_mm/wasm_mmap.h"
#include "../multiproc/include/arc_lproc.h"
#include "ndk_bridge.h"

extern "C" int __android_log_print(int prio, const char* tag, const char* fmt, ...);
extern "C" int property_get(const char* key, char* value, const char* default_value);
// Signals to guest code (shim/ndk/cpu/ndk_cpu_ext.h; absent in the stub CPU).
struct NdkSigaction {
  uint32_t handler, flags;
  uint64_t mask;
};
extern "C" int ndk_sig_action(int sig, const NdkSigaction* act, NdkSigaction* old) __attribute__((weak));
extern "C" void ndk64_flush_code(uint32_t start, size_t size) __attribute__((weak));
extern "C" void ndk64_windows_on(void) __attribute__((weak));
extern "C" int ndk_sig_mask(int how, const uint64_t* set, uint64_t* old) __attribute__((weak));
extern "C" int ndk_sig_raise(int sig) __attribute__((weak));
extern "C" int ndk_sig_deliver(void) __attribute__((weak));

namespace ndk {
namespace {

typedef int32_t gtime_t;  // bionic time_t, off_t, long, clock_t
typedef int32_t goff_t;

// --- memory and strings --------------------------------------------------------------

char* G_strchr(const char* s, int c) { return const_cast<char*>(::strchr(s, c)); }
char* G_strrchr(const char* s, int c) { return const_cast<char*>(::strrchr(s, c)); }
char* G_strstr(const char* s, const char* n) { return const_cast<char*>(::strstr(s, n)); }
char* G_strpbrk(const char* s, const char* a) { return const_cast<char*>(::strpbrk(s, a)); }
void* G_memchr(const void* s, int c, size_t n) { return const_cast<void*>(::memchr(s, c, n)); }
void* G_memrchr(const void* s, int c, size_t n) {
  const unsigned char* p = static_cast<const unsigned char*>(s);
  while (n--) {
    if (p[n] == (unsigned char)c)
      return const_cast<unsigned char*>(p + n);
  }
  return nullptr;
}
void* G_memmem(const void* h, size_t hl, const void* n, size_t nl) {
  return ::memmem(h, hl, n, nl);
}
size_t G_strlcpy(char* d, const char* s, size_t n) {
  size_t l = strlen(s);
  if (n) {
    size_t c = l < n - 1 ? l : n - 1;
    memcpy(d, s, c);
    d[c] = 0;
  }
  return l;
}
size_t G_strlcat(char* d, const char* s, size_t n) {
  size_t dl = strnlen(d, n);
  if (dl == n)
    return n + strlen(s);
  return dl + G_strlcpy(d + dl, s, n - dl);
}
wchar_t* G_wcschr(const wchar_t* s, wchar_t c) { return const_cast<wchar_t*>(::wcschr(s, c)); }
wchar_t* G_wcsrchr(const wchar_t* s, wchar_t c) { return const_cast<wchar_t*>(::wcsrchr(s, c)); }
wchar_t* G_wcsstr(const wchar_t* s, const wchar_t* n) { return const_cast<wchar_t*>(::wcsstr(s, n)); }
wchar_t* G_wmemchr(const wchar_t* s, wchar_t c, size_t n) {
  return const_cast<wchar_t*>(::wmemchr(s, c, n));
}
// The ARM EABI helpers bionic's libc exports (note the argument order).
void G_aeabi_memcpy(void* d, const void* s, size_t n) { memcpy(d, s, n); }
void G_aeabi_memmove(void* d, const void* s, size_t n) { memmove(d, s, n); }
void G_aeabi_memset(void* d, size_t n, int c) { memset(d, c, n); }
void G_aeabi_memclr(void* d, size_t n) { memset(d, 0, n); }

char* G_strerror(int e) { return ::strerror(ToHostErrno(e)); }
int G_strerror_r(int e, char* buf, size_t n) {
  if (n == 0)
    return ERANGE;
  G_strlcpy(buf, ::strerror(ToHostErrno(e)), n);
  return 0;
}

char* G_basename(const char* path) {
  static thread_local char buf[4096];
  if (!path || !*path)
    return strcpy(buf, ".");
  size_t end = strlen(path);
  while (end > 1 && path[end - 1] == '/') end--;
  size_t start = end;
  while (start > 0 && path[start - 1] != '/') start--;
  size_t n = end - start;
  if (n >= sizeof buf) {
    errno = ENAMETOOLONG;
    return nullptr;
  }
  memcpy(buf, path + start, n);
  buf[n] = 0;
  return buf;
}
char* G_dirname(const char* path) {
  static thread_local char buf[4096];
  if (!path || !strchr(path, '/'))
    return strcpy(buf, ".");
  size_t end = strlen(path);
  while (end > 1 && path[end - 1] == '/') end--;
  while (end > 0 && path[end - 1] != '/') end--;
  while (end > 1 && path[end - 1] == '/') end--;
  if (end == 0)
    return strcpy(buf, ".");
  if (end >= sizeof buf) {
    errno = ENAMETOOLONG;
    return nullptr;
  }
  memcpy(buf, path, end);
  buf[end] = 0;
  return buf;
}

// --- ctype: bionic's tables (libc/stdlib/ctype_.c, tolower_.c, toupper_.c) ---

enum { kU = 0x01, kL = 0x02, kN = 0x04, kS = 0x08, kP = 0x10, kC = 0x20, kX = 0x40, kB = 0x80 };
char g_ctype[1 + 256];
int16_t g_tolower[1 + 256], g_toupper[1 + 256];
uint32_t g_ctype_ptr, g_tolower_ptr, g_toupper_ptr;  // the exported pointers

void InitCtype() {
  for (int c = 0; c < 256; c++) {
    int f;
    if (c < 0x20 || (c >= 0x7f && c < 0xa0)) {
      f = kC | (c >= 9 && c <= 13 ? kS : 0);
    } else if (c == ' ') {
      f = kS | kB;
    } else if (c >= '0' && c <= '9') {
      f = kN;
    } else if (c >= 'A' && c <= 'Z') {
      f = kU | (c <= 'F' ? kX : 0);
    } else if (c >= 'a' && c <= 'z') {
      f = kL | (c <= 'f' ? kX : 0);
    } else {
      f = kP;
    }
    g_ctype[1 + c] = static_cast<char>(f);
    g_tolower[1 + c] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
    g_toupper[1 + c] = (c >= 'a' && c <= 'z') ? c - 32 : c;
  }
  g_tolower[0] = g_toupper[0] = -1;  // EOF
  g_ctype_ptr = Addr(g_ctype);
  g_tolower_ptr = Addr(g_tolower);
  g_toupper_ptr = Addr(g_toupper);
}

// ctype functions, as bionic's: the C locale, -1..255.
bool InCtype(int c) { return c >= -1 && c <= 255; }
int CtypeFlags(int c) { return InCtype(c) ? (unsigned char)g_ctype[1 + c] : 0; }
int G_isalnum(int c) { return CtypeFlags(c) & (kU | kL | kN); }
int G_isalpha(int c) { return CtypeFlags(c) & (kU | kL); }
int G_iscntrl(int c) { return CtypeFlags(c) & kC; }
int G_isdigit(int c) { return CtypeFlags(c) & kN; }
int G_isgraph(int c) { return CtypeFlags(c) & (kP | kU | kL | kN); }
int G_islower(int c) { return CtypeFlags(c) & kL; }
int G_isprint(int c) { return CtypeFlags(c) & (kP | kU | kL | kN | kB); }
int G_ispunct(int c) { return CtypeFlags(c) & kP; }
int G_isspace(int c) { return CtypeFlags(c) & kS; }
int G_isupper(int c) { return CtypeFlags(c) & kU; }
int G_isxdigit(int c) { return CtypeFlags(c) & (kN | kX); }
int G_isblank(int c) { return c == ' ' || c == '\t'; }
int G_isascii(int c) { return (unsigned)c <= 0x7f; }
int G_tolower(int c) { return InCtype(c) ? g_tolower[1 + c] : c; }
int G_toupper(int c) { return InCtype(c) ? g_toupper[1 + c] : c; }

// Byte order of the C locale.
int G_wcscoll(const wchar_t* a, const wchar_t* b) { return wcscmp(a, b); }
size_t G_wcsxfrm(wchar_t* d, const wchar_t* s, size_t n) {
  size_t l = wcslen(s);
  if (n > 0) {
    size_t c = l < n - 1 ? l : n - 1;
    wmemcpy(d, s, c);
    d[c] = 0;
  }
  return l;
}
int G_strcoll(const char* a, const char* b) { return strcmp(a, b); }
size_t G_strxfrm(char* d, const char* s, size_t n) { return G_strlcpy(d, s, n); }

// --- stdlib ------------------------------------------------------------------------

// Comparators are guest functions; the current one per thread.
thread_local uint32_t t_cmp;
int CallCmp(const void* a, const void* b) {
  return static_cast<int32_t>(CallGuest2(t_cmp, Addr(a), Addr(b)));
}
void G_qsort(void* base, size_t n, size_t size, uint32_t cmp) {
  uint32_t saved = t_cmp;
  t_cmp = cmp;
  qsort(base, n, size, CallCmp);
  t_cmp = saved;
}
void* G_bsearch(const void* key, const void* base, size_t n, size_t size, uint32_t cmp) {
  uint32_t saved = t_cmp;
  t_cmp = cmp;
  void* r = bsearch(key, base, n, size, CallCmp);
  t_cmp = saved;
  return r;
}

// <search.h> binary trees (musl's): the comparator, the walk action and the
// free function are guest functions. A node starts with its key pointer, as
// bionic's (what tsearch/tfind return is read by the guest as const void**).
void* G_tsearch(const void* key, void** root, uint32_t cmp) {
  uint32_t saved = t_cmp;
  t_cmp = cmp;
  void* r = tsearch(key, root, CallCmp);
  t_cmp = saved;
  return r;
}
void* G_tfind(const void* key, void* const* root, uint32_t cmp) {
  uint32_t saved = t_cmp;
  t_cmp = cmp;
  void* r = tfind(key, root, CallCmp);
  t_cmp = saved;
  return r;
}
void* G_tdelete(const void* key, void** root, uint32_t cmp) {
  uint32_t saved = t_cmp;
  t_cmp = cmp;
  void* r = tdelete(key, root, CallCmp);
  t_cmp = saved;
  return r;
}
thread_local uint32_t t_walk;
void CallWalk(const void* node, VISIT v, int depth) {
  uint32_t a[3] = {Addr(node), static_cast<uint32_t>(v), static_cast<uint32_t>(depth)};
  CallGuest(t_walk, a, 3);
}
void G_twalk(const void* root, uint32_t action) {
  uint32_t saved = t_walk;
  t_walk = action;
  twalk(root, CallWalk);
  t_walk = saved;
}
thread_local uint32_t t_free_node;
void CallFreeNode(void* key) { CallGuest1(t_free_node, Addr(key)); }
void G_tdestroy(void* root, uint32_t free_node) {
  uint32_t saved = t_free_node;
  t_free_node = free_node;
  tdestroy(root, CallFreeNode);
  t_free_node = saved;
}

int32_t G_strtol(const char* s, char** e, int b) { return (int32_t)strtol(s, e, b); }
uint32_t G_strtoul(const char* s, char** e, int b) { return (uint32_t)strtoul(s, e, b); }
int32_t G_atol(const char* s) { return (int32_t)atol(s); }
int32_t G_labs(int32_t v) { return v < 0 ? -v : v; }
int G_abs(int v) { return v < 0 ? -v : v; }
long long G_llabs(long long v) { return v < 0 ? -v : v; }
double G_strtold(const char* s, char** e) { return strtod(s, e); }
double G_atof(const char* s) { return strtod(s, nullptr); }
int32_t G_lrand48() { return (int32_t)lrand48(); }
int32_t G_mrand48() { return (int32_t)mrand48(); }
void G_srand48(int32_t seed) { srand48(seed); }
int32_t G_random() { return (int32_t)random(); }

// div_t/ldiv_t: AAPCS returns a composite larger than 4 bytes in memory, at
// an address passed in r0 (the arguments follow in r1, r2).
void G_div(NdkCpuState* s, void*) {
  int32_t n = s->r[1], d = s->r[2];
  Store32(s->r[0], n / d);
  Store32(s->r[0] + 4, n % d);
}

void G_abort() {
  __android_log_print(6, "ndk", "guest called abort()");
  // A spawned program ends with SIGABRT (topic native-exec, ndk_exec.cc).
  arc_lproc_run_exit_hook(arc_lproc_current(), SIGABRT);
  LeaveGuest("abort()");  // to the app's JNI call, where the bridge guards it
  abort();
}

// --- atexit -------------------------------------------------------------------------

struct AtexitEntry {
  uint32_t fn, arg, dso;
  bool cxa;
};
std::mutex g_atexit_lock;
std::vector<AtexitEntry> g_atexit;

int G_cxa_atexit(uint32_t fn, uint32_t arg, uint32_t dso) {
  std::lock_guard<std::mutex> l(g_atexit_lock);
  g_atexit.push_back({fn, arg, dso, true});
  return 0;
}
int G_aeabi_atexit(uint32_t arg, uint32_t fn, uint32_t dso) { return G_cxa_atexit(fn, arg, dso); }
int G_atexit(uint32_t fn) {
  std::lock_guard<std::mutex> l(g_atexit_lock);
  g_atexit.push_back({fn, 0, 0, false});
  return 0;
}
void G_cxa_finalize(uint32_t dso) { RunAtexit(dso); }
void G_exit(int status) {
  // A spawned program (topic native-exec): its end, not the app's atexit
  // handlers (the handlers are the VM's: shim/ndk/ndk_exec.cc).
  arc_lproc_run_exit_hook(arc_lproc_current(), (status & 0xff) << 8);
  RunAtexit(0);
  exit(status);
}

// setenv/unsetenv: a spawned program's own environment (ndk_exec.cc).
int G_setenv(const char* name, const char* value, int overwrite) {
  int r;
  if (ExecSetenv && ExecSetenv(name, value ? value : "", overwrite, &r)) return r;
  return setenv(name, value, overwrite);
}
int G_unsetenv(const char* name) {
  int r;
  if (ExecSetenv && ExecSetenv(name, nullptr, 1, &r)) return r;
  return unsetenv(name);
}

// getenv: a spawned program has an environment of its own.
const char* G_getenv(const char* name) {
  if (ExecGetenv) {
    bool handled = false;
    const char* v = ExecGetenv(name, &handled);
    if (handled) return v;
  }
  return getenv(name);
}

// --- __stack_chk ------------------------------------------------------------------

uint32_t g_stack_chk_guard;
void G_stack_chk_fail() {
  __android_log_print(7, "ndk", "stack corruption detected (__stack_chk_fail)");
  abort();
}

void G_assert(const char* file, int line, const char* expr) {
  __android_log_print(7, "ndk", "%s:%d: assertion \"%s\" failed", file, line, expr);
  abort();
}
void G_assert2(const char* file, int line, const char* func, const char* expr) {
  __android_log_print(7, "ndk", "%s:%d: %s: assertion \"%s\" failed", file, line, func, expr);
  abort();
}

// --- setjmp/longjmp ------------------------------------------------------------------
// Our own layout inside bionic's jmp_buf (64 words): a magic word, r4-r11,
// sp, lr, then d8-d15 (VFP callee-saved; armeabi code leaves them alone),
// then whether the signal mask was saved and the mask (as bionic: setjmp
// and sigsetjmp(env, 1) save it, _setjmp does not; longjmp restores it).
enum { kJbMagic = 0, kJbCore = 1, kJbSp = 9, kJbLr = 10, kJbVfp = 12, kJbMaskSaved = 28, kJbMask = 29 };
const uint32_t kJmpMagic = 0x4e444b4a;  // "NDKJ"

void SetJmp(NdkCpuState* s, bool save_mask) {
  uint32_t* jb = Ptr<uint32_t>(s->r[0]);
  jb[kJbMagic] = kJmpMagic;
  for (int i = 0; i < 8; i++) jb[kJbCore + i] = s->r[4 + i];
  jb[kJbSp] = s->r[13];
  jb[kJbLr] = s->r[14];
  memcpy(&jb[kJbVfp], &s->d[8], 8 * sizeof(uint64_t));
  jb[kJbMaskSaved] = save_mask && ndk_sig_mask;
  if (jb[kJbMaskSaved]) {
    uint64_t mask = 0;
    ndk_sig_mask(0, nullptr, &mask);
    memcpy(&jb[kJbMask], &mask, 8);
  }
  s->r[0] = 0;
}
void G_setjmp(NdkCpuState* s, void*) { SetJmp(s, true); }
void G_setjmp_nomask(NdkCpuState* s, void*) { SetJmp(s, false); }
void G_sigsetjmp(NdkCpuState* s, void*) { SetJmp(s, s->r[1] != 0); }

void G_longjmp(NdkCpuState* s, void*) {
  const uint32_t* jb = Ptr<const uint32_t>(s->r[0]);
  uint32_t val = s->r[1];
  if (jb[kJbMagic] != kJmpMagic) {
    __android_log_print(7, "ndk", "longjmp with a bad jmp_buf 0x%x", s->r[0]);
    abort();
  }
  for (int i = 0; i < 8; i++) s->r[4 + i] = jb[kJbCore + i];
  s->r[13] = jb[kJbSp];
  s->r[14] = jb[kJbLr];  // the CPU "returns" to the setjmp call site
  memcpy(&s->d[8], &jb[kJbVfp], 8 * sizeof(uint64_t));
  if (jb[kJbMaskSaved] == 1 && ndk_sig_mask) {  // (out of a signal handler, say)
    uint64_t mask;
    memcpy(&mask, &jb[kJbMask], 8);
    ndk_sig_mask(2, &mask, nullptr);
  }
  s->r[0] = val ? val : 1;
}

// --- time (bionic: 32-bit time_t and long) --------------------------------------------

struct GTimeval {
  int32_t tv_sec, tv_usec;
};
struct GTimespec {
  int32_t tv_sec, tv_nsec;
};

gtime_t G_time(gtime_t* t) {
  gtime_t now = (gtime_t)time(nullptr);
  if (t)
    *t = now;
  return now;
}
int G_gettimeofday(GTimeval* tv, void* tz) {
  struct timeval h;
  int r = gettimeofday(&h, nullptr);
  if (tv) {
    tv->tv_sec = (int32_t)h.tv_sec;
    tv->tv_usec = (int32_t)h.tv_usec;
  }
  if (tz)
    memset(tz, 0, 8);
  return r;
}
int G_clock_gettime(int clk, GTimespec* ts) {
  struct timespec h;
  int r = clock_gettime(HostClockId(clk), &h);
  if (r == 0 && ts) {
    ts->tv_sec = (int32_t)h.tv_sec;
    ts->tv_nsec = (int32_t)h.tv_nsec;
  }
  return r;
}
int G_clock_getres(int clk, GTimespec* ts) {
  struct timespec h;
  int r = clock_getres(HostClockId(clk), &h);
  if (r == 0 && ts) {
    ts->tv_sec = (int32_t)h.tv_sec;
    ts->tv_nsec = (int32_t)h.tv_nsec;
  }
  return r;
}
// Sleeps that a guest signal interrupts (EINTR, with the time left).
void MsToTimespec(double ms, GTimespec* ts) {
  if (ms < 0) ms = 0;
  ts->tv_sec = (int32_t)(ms / 1000);
  ts->tv_nsec = (int32_t)((ms - ts->tv_sec * 1000.0) * 1e6);
}
int G_nanosleep(const GTimespec* req, GTimespec* rem) {
  if (req->tv_nsec < 0 || req->tv_nsec >= 1000000000 || req->tv_sec < 0) {
    errno = EINVAL;
    return -1;
  }
  double left;
  int r = GuestSleep(req->tv_sec * 1000.0 + req->tv_nsec / 1e6, &left);
  if (r == 0 || r == kParked)   // (kParked: a green thread's call runs again)
    return 0;
  if (rem) MsToTimespec(left, rem);
  errno = EINTR;
  return -1;
}
int G_usleep(uint32_t us) {
  int r = GuestSleep(us / 1000.0, nullptr);
  if (r == 0 || r == kParked)
    return 0;
  errno = EINTR;
  return -1;
}
uint32_t G_sleep(uint32_t s) {
  double left;
  int r = GuestSleep(s * 1000.0, &left);
  if (r == 0 || r == kParked)
    return 0;
  return (uint32_t)((left + 999) / 1000);
}
// A green thread gives its carrier to the next runnable one.
int G_sched_yield() {
  if (GreenResumed(kSiteYield)) return 0;
  if (GreenCanPark()) return GreenParkYield() == kParked ? 0 : 0;
  return sched_yield();
}
int32_t G_clock() { return (int32_t)clock(); }
double G_difftime(gtime_t a, gtime_t b) { return (double)a - (double)b; }
// struct tm has the same layout (bionic has tm_gmtoff and tm_zone too).
struct tm* G_gmtime(const gtime_t* t) {
  time_t h = *t;
  return gmtime(&h);
}
struct tm* G_gmtime_r(const gtime_t* t, struct tm* out) {
  time_t h = *t;
  return gmtime_r(&h, out);
}
struct tm* G_localtime(const gtime_t* t) {
  time_t h = *t;
  return localtime(&h);
}
struct tm* G_localtime_r(const gtime_t* t, struct tm* out) {
  time_t h = *t;
  return localtime_r(&h, out);
}
gtime_t G_mktime(struct tm* tm) { return (gtime_t)mktime(tm); }
gtime_t G_timegm(struct tm* tm) { return (gtime_t)timegm(tm); }
char* G_ctime(const gtime_t* t) {
  time_t h = *t;
  return ctime(&h);
}

// --- files ------------------------------------------------------------------------

// bionic's open() flags (ARM's asm/fcntl.h moves four of the generic ones).
int ToHostOpenFlags(int g) {
  // O_CREAT, O_EXCL, O_NOCTTY, O_TRUNC, O_APPEND, O_NONBLOCK, O_CLOEXEC
  // have the same values on both sides.
  int h = g & (O_ACCMODE | 0100 | 0200 | 0400 | 01000 | 02000 | 04000 | 02000000);
  if (g & 010000) h |= O_SYNC;
  if (g & 020000) h |= O_ASYNC;
  if (g & 040000) h |= O_DIRECTORY;
  if (g & 0100000) h |= O_NOFOLLOW;
  if (g & 0200000) h |= O_DIRECT;
  if (g & 0400000) h |= O_LARGEFILE;
  if (g & 01000000) h |= O_NOATIME;
  return h;
}
int ToGuestOpenFlags(int h) {
  int g = h & (O_ACCMODE | O_CREAT | O_EXCL | O_NOCTTY | O_TRUNC | O_APPEND | O_NONBLOCK |
               O_CLOEXEC);
  if ((h & O_SYNC) == O_SYNC) g |= 010000;
  if (h & O_ASYNC) g |= 020000;
  if (h & O_DIRECTORY) g |= 040000;
  if (h & O_NOFOLLOW) g |= 0100000;
  if (h & O_DIRECT) g |= 0200000;
  if (h & O_LARGEFILE) g |= 0400000;
  return g;
}

void G_open(NdkCpuState* s, void*) {
  RegArgs a(s);
  const char* path = GetArg<const char*>(a);
  int flags = GetArg<int>(a);
  int mode = GetArg<int>(a);  // only meaningful with O_CREAT
  ThunkErrnoBegin();
  int r = FdOpen(path, flags, mode);
  ThunkErrnoEnd();
  SetResult(s, r);
}
DIR* G_opendir(const char* path) {
  DIR* d = opendir(path);
  if (d)
    NoteFdPath(dirfd(d), path);
  return d;
}

void G_fcntl(NdkCpuState* s, void*) {
  RegArgs a(s);
  int fd = GetArg<int>(a);
  int cmd = GetArg<int>(a);
  uint32_t arg = a.U32();
  ThunkErrnoBegin();
  int r;
  switch (cmd) {
    case 3:  // F_GETFL
      r = fcntl(fd, F_GETFL);
      if (r >= 0) r = ToGuestOpenFlags(r);
      break;
    case 4:  // F_SETFL
      r = fcntl(fd, F_SETFL, ToHostOpenFlags(arg));
      break;
    case 0: case 1: case 2: case 1030:  // F_DUPFD, F_GETFD, F_SETFD, F_DUPFD_CLOEXEC
      r = fcntl(fd, cmd, arg);
      break;
    default:
      r = FcntlLock(fd, cmd, arg);
      if (r == -2) {
        __android_log_print(5, "ndk", "fcntl(%d, %d): unsupported command", fd, cmd);
        errno = EINVAL;
        r = -1;
      }
  }
  ThunkErrnoEnd();
  SetResult(s, r);
}

void G_ioctl(NdkCpuState* s, void*) {
  RegArgs a(s);
  int fd = GetArg<int>(a);
  uint32_t req = a.U32();
  uint32_t arg = a.U32();
  ThunkErrnoBegin();
  int r = ioctl(fd, req, Ptr<void>(arg));
  ThunkErrnoEnd();
  SetResult(s, r);
}

goff_t G_lseek(int fd, goff_t off, int whence) {
  off_t r = lseek(fd, off, whence);
  if (r > INT32_MAX) {
    errno = EOVERFLOW;
    return -1;
  }
  return (goff_t)r;
}
int64_t G_lseek64(int fd, int64_t off, int whence) { return lseek(fd, off, whence); }
ssize_t G_pread(int fd, void* b, size_t n, goff_t off) { return pread(fd, b, n, off); }
ssize_t G_pwrite(int fd, const void* b, size_t n, goff_t off) { return pwrite(fd, b, n, off); }
int G_ftruncate(int fd, goff_t len) { return ftruncate(fd, len); }
int G_truncate(const char* p, goff_t len) { return truncate(p, len); }

// bionic's struct stat on ARM (the kernel's stat64).
struct GStat {
  uint64_t st_dev;
  uint8_t pad0[4];
  uint32_t st_ino32;
  uint32_t st_mode, st_nlink, st_uid, st_gid;
  uint64_t st_rdev;
  uint8_t pad3[4];
  int64_t st_size;
  uint32_t st_blksize;
  uint64_t st_blocks;
  uint32_t st_atime_, st_atime_nsec, st_mtime_, st_mtime_nsec, st_ctime_, st_ctime_nsec;
  uint64_t st_ino;
};
static_assert(sizeof(GStat) == 104, "bionic struct stat");
static_assert(offsetof(GStat, st_size) == 48, "bionic struct stat");

void ToGuestStat(const struct stat& h, GStat* g) {
  memset(g, 0, sizeof *g);
  g->st_dev = h.st_dev;
  g->st_ino32 = (uint32_t)h.st_ino;
  g->st_ino = h.st_ino;
  g->st_mode = h.st_mode;
  g->st_nlink = h.st_nlink;
  g->st_uid = h.st_uid;
  g->st_gid = h.st_gid;
  g->st_rdev = h.st_rdev;
  g->st_size = h.st_size;
  g->st_blksize = h.st_blksize;
  g->st_blocks = h.st_blocks;
  g->st_atime_ = (uint32_t)h.st_atim.tv_sec;
  g->st_atime_nsec = h.st_atim.tv_nsec;
  g->st_mtime_ = (uint32_t)h.st_mtim.tv_sec;
  g->st_mtime_nsec = h.st_mtim.tv_nsec;
  g->st_ctime_ = (uint32_t)h.st_ctim.tv_sec;
  g->st_ctime_nsec = h.st_ctim.tv_nsec;
}
int G_stat(const char* p, GStat* g) {
  struct stat h;
  int r = stat(p, &h);
  if (r == 0) ToGuestStat(h, g);
  return r;
}
int G_lstat(const char* p, GStat* g) {
  struct stat h;
  int r = lstat(p, &h);
  if (r == 0) ToGuestStat(h, g);
  return r;
}
int G_fstat(int fd, GStat* g) {
  struct stat h;
  int r = fstat(fd, &h);
  if (r == 0) ToGuestStat(h, g);
  return r;
}

// struct dirent: identical layouts (checked below); DIR* is opaque.
static_assert(sizeof(struct dirent) == 280 && offsetof(struct dirent, d_name) == 19,
              "bionic struct dirent");

// No descriptor is a terminal for guest code: an app's 0-2 are /dev/null and
// tcgetattr is ENOTTY (ndk_libc8.cc). Emscripten's console said yes, and
// libuv took stdio for a TTY, whose tcgetattr then failed a CHECK in Node.
int G_isatty(int fd) {
  struct stat h;
  errno = fstat(fd, &h) == 0 ? ENOTTY : EBADF;
  return 0;
}

// fnmatch: bionic swaps FNM_NOESCAPE and FNM_PATHNAME relative to musl.
int G_fnmatch(const char* pat, const char* s, int gf) {
  int hf = 0;
  if (gf & 0x01) hf |= FNM_NOESCAPE;
  if (gf & 0x02) hf |= FNM_PATHNAME;
  if (gf & 0x04) hf |= FNM_PERIOD;
  if (gf & 0x08) hf |= FNM_LEADING_DIR;
  if (gf & 0x10) hf |= FNM_CASEFOLD;
  return fnmatch(pat, s, hf) == 0 ? 0 : 1;
}

// bionic's struct passwd (no pw_gecos).
struct GPasswd {
  uint32_t pw_name, pw_passwd, pw_uid, pw_gid, pw_dir, pw_shell;
};
GPasswd* ToGuestPasswd(struct passwd* h) {
  static thread_local GPasswd g;
  if (!h)
    return nullptr;
  g = {Addr(h->pw_name), Addr(h->pw_passwd), h->pw_uid, h->pw_gid, Addr(h->pw_dir),
       Addr(h->pw_shell)};
  return &g;
}
// Android's users (PhysFS takes its user directory from getpwuid(getuid())).
GPasswd* G_getpwuid(uid_t uid) { return ToGuestPasswd(NdkAndroidPasswd(uid)); }
GPasswd* G_getpwnam(const char* n) {
  const int64_t id = NdkAndroidIdOf(n);
  return id < 0 ? nullptr : ToGuestPasswd(NdkAndroidPasswd(static_cast<uint32_t>(id)));
}

// Zeros over guest memory for a mapping of |prot| in 4 KB pages (-1: madvise):
// through the mmap allocator, which knows pages that are zeros already and
// pages to zero once accessible (topic wasm-heap-trim), when it is linked.
extern "C" void wasm_mm_guest_zero(void* addr, size_t len, int prot) __attribute__((weak));
extern "C" void wasm_mm_guest_access(void* addr, size_t len, int prot) __attribute__((weak));
void GuestZero(void* p, size_t n, int prot) {
  if (wasm_mm_guest_zero)
    wasm_mm_guest_zero(p, n, prot);
  else
    memset(p, 0, n);
}

void* G_mmap(void* addr, size_t len, int prot, int flags, int fd, goff_t off) {
  return GuestMmap(addr, len, prot, flags, fd, off);
}
}  // namespace

// Guest mappings whose length is not whole host pages, by address: the host
// mapped the rounded-up length, which a munmap of exactly them frees. A
// multimap: an ashmem region's MAP_SHARED mappings are all at one address,
// and light processes (multiproc) each map and unmap it (Gecko's shared
// memory): every mapping keeps its record until its own munmap.
std::mutex g_odd_maps_lock;
std::multimap<uint32_t, size_t> g_odd_maps;

// The host's pages are larger than the guest's 4 KB ones (PAGE_SIZE): only
// the host pages wholly inside [addr, addr + len) are unmapped (the host
// would round the length up and free live guest memory after it); the
// rest stays mapped, as the guest may map it again (MAP_FIXED) or not.
int GuestMunmap(void* addr, size_t len) {
  if ((Addr(addr) & 4095) != 0 || len == 0) {
    errno = EINVAL;
    return -1;
  }
  FlushGuestCode(Addr(addr), len);
  ForgetExecRegion(Addr(addr), Addr(addr) + len);
  {
    // Exactly a mapping of the guest's that the host rounded up: all of it.
    std::lock_guard<std::mutex> l(g_odd_maps_lock);
    auto range = g_odd_maps.equal_range(Addr(addr));
    for (auto it = range.first; it != range.second; ++it) {
      if (it->second == len) {
        g_odd_maps.erase(it);
        return munmap(addr, len);
      }
    }
  }
  uint32_t lo = (Addr(addr) + PAGE_SIZE - 1) & ~(uint32_t)(PAGE_SIZE - 1);
  uint32_t hi = (Addr(addr) + len) & ~(uint32_t)(PAGE_SIZE - 1);
  if (lo == Addr(addr) && hi == Addr(addr) + len)
    return munmap(addr, len);
  return hi > lo ? munmap(Ptr<void>(lo), hi - lo) : 0;
}

// One CPU was too few: Dart's worker_manager (Saber) sizes its isolate pool
// as Platform.numberOfProcessors - 1, and with no worker its scheduler
// re-enters itself through microtasks forever (no first frame).
int GuestCpuCount() {
  static const int n = [] {
    const int c = emscripten_num_logical_cores();
    return c < 1 ? 1 : c > 8 ? 8 : c;  // (shim/kernel/arc_kernel.cc: the same)
  }();
  return n;
}

void GuestCpuMask(void* set, size_t n) {
  uint8_t* p = static_cast<uint8_t*>(set);
  memset(p, 0, n);
  for (int i = 0; i < GuestCpuCount() && static_cast<size_t>(i / 8) < n; ++i)
    p[i / 8] |= static_cast<uint8_t>(1u << (i % 8));
}

namespace {
int G_munmap(void* addr, size_t len) { return GuestMunmap(addr, len); }
// madvise in 4 KB pages: MADV_DONTNEED of part of a host page zeroes just
// that part (private anonymous memory reads back as zeros; the host would
// zero whole host pages); whole host pages go to the host. Other advice is
// only advice.
int G_madvise(void* addr, size_t len, int advice) {
  if ((Addr(addr) & 4095) != 0) {
    errno = EINVAL;
    return -1;
  }
  if ((Addr(addr) & (PAGE_SIZE - 1)) == 0 && (len & (PAGE_SIZE - 1)) == 0)
    return madvise(addr, len, advice);
  if (advice == MADV_DONTNEED)
    GuestZero(addr, len, -1);
  return 0;
}
// wasm memory has no protection; code the guest makes executable is noted
// (natives may point into it) and flushed: a JIT with W^X writes, then
// makes the code executable (it flushes too on ARM; the flush here only
// costs the blocks of the range, see FlushGuestCode).
int G_mprotect(void* addr, size_t len, int prot) {
  // Pages the mmap allocator keeps as zeros to write once accessible get them now.
  if (wasm_mm_guest_access)
    wasm_mm_guest_access(addr, len, prot);
  if (prot & PROT_EXEC) {
    NoteExecRegion(Addr(addr), Addr(addr) + len);
    FlushGuestCode(Addr(addr), len);
  }
  return 0;
}
}  // namespace

int GuestMadvise(void* addr, size_t len, int advice) { return G_madvise(addr, len, advice); }
int GuestMprotect(void* addr, size_t len, int prot) { return G_mprotect(addr, len, prot); }

namespace {

// poll and select wait in slices that guest signals interrupt (EINTR).
int G_poll(struct pollfd* fds, nfds_t n, int timeout) { return GuestPoll(fds, n, timeout); }
struct SelectArgs {
  int n;
  fd_set *r, *w, *e;
  fd_set r0, w0, e0;  // the sets asked for (select clears them on a timeout)
};
int SelectSlice(void* p, int ms) {
  SelectArgs* a = static_cast<SelectArgs*>(p);
  if (a->r) *a->r = a->r0;
  if (a->w) *a->w = a->w0;
  if (a->e) *a->e = a->e0;
  struct timeval tv = {ms / 1000, (ms % 1000) * 1000};
  return select(a->n, a->r, a->w, a->e, ms < 0 ? nullptr : &tv);
}
int G_select(int n, fd_set* r, fd_set* w, fd_set* e, GTimeval* tv) {
  SelectArgs a = {n, r, w, e, {}, {}, {}};
  if (r) a.r0 = *r;
  if (w) a.w0 = *w;
  if (e) a.e0 = *e;
  int ms = -1;
  if (tv) {
    int64_t t = (int64_t)tv->tv_sec * 1000 + (tv->tv_usec + 999) / 1000;
    ms = t > INT32_MAX ? INT32_MAX : (int)t;
  }
  return SignalSlicedWait(ms, SelectSlice, &a);
}

struct GUtimbuf {
  int32_t actime, modtime;
};
int G_utime(const char*, const GUtimbuf*) { return 0; }

// sysconf: bionic's _SC_ numbers; the guest sees 4 KB pages, as on ARM
// (its ELF segments are laid out for them).
int32_t G_sysconf(int name) {
  switch (name) {
    case 0x0006: return 100;    // _SC_CLK_TCK
    case 0x000b: return 1024;   // _SC_OPEN_MAX
    case 0x0027:                // _SC_PAGESIZE
    case 0x0028: return 4096;   // _SC_PAGE_SIZE
    case 0x0060:                // _SC_NPROCESSORS_CONF
    case 0x0061: return GuestCpuCount();  // _SC_NPROCESSORS_ONLN
    case 0x0062: return 512 * 256;  // _SC_PHYS_PAGES: 512 MB
    default:
      __android_log_print(5, "ndk", "sysconf(0x%x): unknown name", name);
      errno = EINVAL;
      return -1;
  }
}
int G_getpagesize() { return 4096; }

// --- sockets and names ----------------------------------------------------------

// SO_RCVTIMEO/SO_SNDTIMEO: bionic (32-bit ARM) has 20/21 with its 8-byte
// timeval; the host (musl, 64-bit time_t) names these SO_RCVTIMEO_OLD and
// knows only 66/67 (SO_*TIMEO_NEW) with a 16-byte timeval. Passed through, the
// guest's setsockopt failed with EINVAL (il2cpp's Socket.ReceiveTimeout throws:
// every HttpClient request of a Unity game died right after its first write).
constexpr int kGSoRcvTimeo = 20, kGSoSndTimeo = 21;
int HostTimeoOption(int level, int optname) {
  if (level != SOL_SOCKET) return 0;
  if (optname == kGSoRcvTimeo) return SO_RCVTIMEO;
  if (optname == kGSoSndTimeo) return SO_SNDTIMEO;
  return 0;
}
int G_setsockopt(int fd, int level, int optname, const void* optval, socklen_t optlen) {
  if (int host = HostTimeoOption(level, optname)) {
    if (!optval || optlen < sizeof(GTimeval)) {
      errno = optval ? EINVAL : EFAULT;
      return -1;
    }
    const GTimeval* g = static_cast<const GTimeval*>(optval);
    struct timeval tv = {g->tv_sec, g->tv_usec};
    return setsockopt(fd, level, host, &tv, sizeof tv);
  }
  return setsockopt(fd, level, optname, optval, optlen);
}
int G_getsockopt(int fd, int level, int optname, void* optval, socklen_t* optlen) {
  if (int host = HostTimeoOption(level, optname)) {
    if (!optval || !optlen) {
      errno = EFAULT;
      return -1;
    }
    struct timeval tv;
    socklen_t len = sizeof tv;
    if (getsockopt(fd, level, host, &tv, &len) != 0) return -1;
    GTimeval g = {(int32_t)tv.tv_sec, (int32_t)tv.tv_usec};
    *optlen = std::min<socklen_t>(*optlen, sizeof g);
    memcpy(optval, &g, *optlen);
    return 0;
  }
  return getsockopt(fd, level, optname, optval, optlen);
}

// bionic's struct addrinfo has ai_canonname before ai_addr (musl: after).
struct GAddrinfo {
  int32_t ai_flags, ai_family, ai_socktype, ai_protocol, ai_addrlen;
  uint32_t ai_canonname, ai_addr, ai_next;
};
int G_getaddrinfo(const char* node, const char* service, const GAddrinfo* hints,
                  GAddrinfo** res) {
  struct addrinfo h, *hr = nullptr;
  memset(&h, 0, sizeof h);
  if (hints) {
    h.ai_flags = hints->ai_flags;
    h.ai_family = hints->ai_family;
    h.ai_socktype = hints->ai_socktype;
    h.ai_protocol = hints->ai_protocol;
  }
  int r = getaddrinfo(node, service, hints ? &h : nullptr, &hr);
  if (r != 0) {
    // bionic EAI_* (BSD): AGAIN 2, FAIL 4, FAMILY 5, MEMORY 6, NONAME 8,
    // SERVICE 9, SOCKTYPE 10, SYSTEM 11.
    switch (r) {
      case EAI_AGAIN: return 2;
      case EAI_FAIL: return 4;
      case EAI_FAMILY: return 5;
      case EAI_MEMORY: return 6;
      case EAI_SERVICE: return 9;
      case EAI_SOCKTYPE: return 10;
      case EAI_SYSTEM: return 11;
      default: return 8;
    }
  }
  GAddrinfo* head = nullptr;
  GAddrinfo** tail = &head;
  for (struct addrinfo* p = hr; p; p = p->ai_next) {
    GAddrinfo* g = static_cast<GAddrinfo*>(calloc(1, sizeof(GAddrinfo) + p->ai_addrlen));
    g->ai_flags = p->ai_flags;
    g->ai_family = p->ai_family;
    g->ai_socktype = p->ai_socktype;
    g->ai_protocol = p->ai_protocol;
    g->ai_addrlen = p->ai_addrlen;
    memcpy(g + 1, p->ai_addr, p->ai_addrlen);
    g->ai_addr = Addr(g + 1);
    g->ai_canonname = p->ai_canonname ? Addr(strdup(p->ai_canonname)) : 0;
    *tail = g;
    tail = Ptr<GAddrinfo*>(Addr(&g->ai_next));
  }
  freeaddrinfo(hr);
  *res = head;
  return 0;
}
void G_freeaddrinfo(GAddrinfo* g) {
  while (g) {
    GAddrinfo* next = Ptr<GAddrinfo>(g->ai_next);
    free(Ptr<void>(g->ai_canonname));
    free(g);
    g = next;
  }
}
char* G_inet_ntoa(uint32_t a) {
  struct in_addr x;
  x.s_addr = a;
  return inet_ntoa(x);
}
const char* G_gai_strerror(int e) { return e == 0 ? "Success" : "Name resolution failed"; }

// --- process, signals --------------------------------------------------------

// Guest handlers are kept by the CPU (shim/ndk/cpu/signal.cc), which runs
// them for signals guest code sends to its threads (pthread_kill, tgkill,
// raise) and for SIGILL from an undefined instruction. Faults of guest code
// stay fatal (the CPU's report), and so do raise(SIGABRT) and the like.
// bionic's 32-bit struct sigaction on ARM: handler, mask, flags, restorer.
struct GSigaction {
  uint32_t handler, mask;
  int32_t flags;
  uint32_t restorer;
};
int G_sigaction(int sig, const GSigaction* act, GSigaction* oact) {
  if (!ndk_sig_action) {
    if (oact)
      memset(oact, 0, sizeof(*oact));
    return 0;
  }
  NdkSigaction a, old;
  if (act) a = {act->handler, (uint32_t)act->flags, act->mask};
  if (ndk_sig_action(sig, act ? &a : nullptr, &old) != 0) {
    errno = EINVAL;
    return -1;
  }
  if (oact)
    *oact = {old.handler, (uint32_t)old.mask, (int32_t)old.flags, 0};
  return 0;
}
// signal(): BSD semantics (SA_RESTART); returns the old handler.
uint32_t G_bsd_signal(int sig, uint32_t handler) {
  if (!ndk_sig_action)
    return 0;  // SIG_DFL
  NdkSigaction a = {handler, 0x10000000 /* SA_RESTART */, 0}, old;
  if (ndk_sig_action(sig, &a, &old) != 0) {
    errno = EINVAL;
    return ~0u;  // SIG_ERR
  }
  return old.handler;
}
bool FatalSignal(int sig) {
  // SIGABRT, SIGKILL, SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGTRAP, SIGSYS
  return sig == 6 || sig == 9 || sig == 11 || sig == 7 || sig == 8 || sig == 4 || sig == 5 || sig == 31;
}
int G_raise(int sig) {
  if (sig == 6 || sig == 9 || sig == 11) {  // SIGABRT, SIGKILL, SIGSEGV
    __android_log_print(6, "ndk", "guest raised signal %d", sig);
    abort();
  }
  if (sig > 0 && !FatalSignal(sig) && ndk_sig_raise)
    ndk_sig_raise(sig);
  return 0;
}
int G_kill(int pid, int sig) {
  if (pid == getpid())
    return G_raise(sig);
  // Another light process of the app (topic multiproc): a parent killing
  // its child process (kill() of the VM knows them, light_process_wrap.cc).
  if (pid > 0 && arc_lproc_is_light(pid))
    return kill(pid, sig);
  errno = EPERM;
  return -1;
}
int G_sigemptyset(uint32_t* set) { *set = 0; return 0; }
int G_sigfillset(uint32_t* set) { *set = ~0u; return 0; }
int G_sigaddset(uint32_t* set, int sig) { *set |= 1u << (sig - 1); return 0; }
int G_sigdelset(uint32_t* set, int sig) { *set &= ~(1u << (sig - 1)); return 0; }
int G_sigismember(const uint32_t* set, int sig) { return (*set >> (sig - 1)) & 1; }
int G_sigprocmask(int how, const uint32_t* set, uint32_t* old) {
  if (!ndk_sig_mask) {
    if (old) *old = 0;
    return 0;
  }
  uint64_t s64 = set ? *set : 0, o64 = 0;
  if (ndk_sig_mask(how, set ? &s64 : nullptr, &o64) != 0) {
    errno = EINVAL;
    return -1;
  }
  if (old) *old = (uint32_t)o64;
  if (ndk_sig_deliver) ndk_sig_deliver();
  return 0;
}

int G_gettid() { return GuestTid(); }

// syscall(2) with the ARM EABI numbers guest code is known to use.
void G_syscall(NdkCpuState* s, void*) {
  RegArgs a(s);
  uint32_t nr = a.U32();
  uint32_t x[6];
  for (uint32_t& v : x) v = a.U32();
  ThunkErrnoBegin();
  int32_t r;
  switch (nr) {
    case 20: r = getpid(); break;
    case 224: r = GuestTid(); break;
    case 158: r = G_sched_yield(); break;
    case 241:  // sched_setaffinity(pid, len, mask): any mask is fine
      r = 0;
      break;
    case 242:  // sched_getaffinity(pid, len, mask): GuestCpuCount CPUs; bytes written
      if (x[1] < 4) {
        errno = EINVAL;
        r = -1;
      } else {
        GuestCpuMask(Ptr<void>(x[2]), x[1]);
        r = x[1] < 128 ? x[1] : 128;
      }
      break;
    // 240 futex: the raw svc table (ndk_syscall.cc DoFutex).
    default: {
      // The rest of the raw svc table (ndk_syscall.cc), which answers -errno.
      int32_t g;
      if (RawSyscall(nr, x, &g)) {
        if (g < 0 && g > -4096) {
          errno = ToHostErrno(-g);
          r = -1;
        } else {
          r = g;
        }
        break;
      }
      __android_log_print(5, "ndk", "syscall(%u): not supported", nr);
      errno = ENOSYS;
      r = -1;
    }
  }
  ThunkErrnoEnd();
  SetResult(s, r);
}

int G_system_property_get(const char* key, char* value) {
  return property_get(key, value, "");
}

int* G_errno() { return GuestErrno(); }

const HostSymbol kLibc[] = {
    // memory and strings
    NDK_FNX("memcpy", ::memcpy), NDK_FNX("memmove", ::memmove), NDK_FNX("memset", ::memset),
    NDK_FNX("memcmp", ::memcmp), NDK_FNX("memchr", G_memchr), NDK_FNX("memrchr", G_memrchr),
    NDK_FNX("memmem", G_memmem), NDK_FNX("bcopy", ::bcopy), NDK_FNX("bzero", ::bzero),
    NDK_FNX("strlen", ::strlen), NDK_FNX("strnlen", ::strnlen), NDK_FNX("strcmp", ::strcmp),
    NDK_FNX("strncmp", ::strncmp), NDK_FNX("strcpy", ::strcpy), NDK_FNX("strncpy", ::strncpy),
    NDK_FNX("strcat", ::strcat), NDK_FNX("strncat", ::strncat), NDK_FNX("strlcpy", G_strlcpy),
    NDK_FNX("strlcat", G_strlcat), NDK_FNX("strchr", G_strchr), NDK_FNX("strrchr", G_strrchr),
    NDK_FNX("strstr", G_strstr), NDK_FNX("strpbrk", G_strpbrk), NDK_FNX("strspn", ::strspn),
    NDK_FNX("strcspn", ::strcspn), NDK_FNX("strdup", ::strdup), NDK_FNX("strndup", ::strndup),
    NDK_FNX("strcasecmp", ::strcasecmp), NDK_FNX("strncasecmp", ::strncasecmp),
    NDK_FNX("strtok", ::strtok), NDK_FN("strtok_r", ::strtok_r), NDK_FN("strsep", ::strsep),
    NDK_FNX("strerror", G_strerror), NDK_FNX("strerror_r", G_strerror_r),
    NDK_FNX("strcoll", G_strcoll), NDK_FNX("strxfrm", G_strxfrm),
    NDK_FNX("basename", G_basename), NDK_FNX("dirname", G_dirname),
    NDK_FN("__aeabi_memcpy", G_aeabi_memcpy), NDK_FN("__aeabi_memcpy4", G_aeabi_memcpy),
    NDK_FN("__aeabi_memcpy8", G_aeabi_memcpy), NDK_FN("__aeabi_memmove", G_aeabi_memmove),
    NDK_FN("__aeabi_memmove4", G_aeabi_memmove), NDK_FN("__aeabi_memmove8", G_aeabi_memmove),
    NDK_FN("__aeabi_memset", G_aeabi_memset), NDK_FN("__aeabi_memset4", G_aeabi_memset),
    NDK_FN("__aeabi_memset8", G_aeabi_memset), NDK_FN("__aeabi_memclr", G_aeabi_memclr),
    NDK_FN("__aeabi_memclr4", G_aeabi_memclr), NDK_FN("__aeabi_memclr8", G_aeabi_memclr),
    // ctype
    NDK_DATA("_ctype_", Addr(&g_ctype_ptr)), NDK_DATA("_tolower_tab_", Addr(&g_tolower_ptr)),
    NDK_DATA("_toupper_tab_", Addr(&g_toupper_ptr)),
    NDK_FNX("isalnum", G_isalnum), NDK_FNX("isalpha", G_isalpha), NDK_FNX("iscntrl", G_iscntrl),
    NDK_FNX("isdigit", G_isdigit), NDK_FNX("isgraph", G_isgraph), NDK_FNX("islower", G_islower),
    NDK_FNX("isprint", G_isprint), NDK_FNX("ispunct", G_ispunct), NDK_FNX("isspace", G_isspace),
    NDK_FNX("isupper", G_isupper), NDK_FNX("isxdigit", G_isxdigit), NDK_FNX("isblank", G_isblank),
    NDK_FNX("isascii", G_isascii), NDK_FNX("tolower", G_tolower), NDK_FNX("toupper", G_toupper),
    // wide characters and locale
    NDK_FNX("wcslen", ::wcslen), NDK_FNX("wcscmp", ::wcscmp), NDK_FNX("wcsncmp", ::wcsncmp),
    NDK_FNX("wcscpy", ::wcscpy), NDK_FNX("wcsncpy", ::wcsncpy), NDK_FNX("wcscat", ::wcscat),
    NDK_FNX("wcschr", G_wcschr), NDK_FNX("wcsrchr", G_wcsrchr), NDK_FNX("wcsstr", G_wcsstr),
    NDK_FNX("wmemchr", G_wmemchr), NDK_FNX("wmemcmp", ::wmemcmp), NDK_FNX("wmemcpy", ::wmemcpy),
    NDK_FNX("wmemmove", ::wmemmove), NDK_FNX("wmemset", ::wmemset),
    NDK_FNX("wcscoll", G_wcscoll), NDK_FNX("wcsxfrm", G_wcsxfrm),
    NDK_FNX("iswctype", ::iswctype), NDK_FNX("wctype", ::wctype), NDK_FNX("towlower", ::towlower),
    NDK_FNX("towupper", ::towupper), NDK_FNX("iswalpha", ::iswalpha), NDK_FNX("iswdigit", ::iswdigit),
    NDK_FNX("iswspace", ::iswspace), NDK_FNX("iswupper", ::iswupper), NDK_FNX("iswlower", ::iswlower),
    NDK_FNX("iswalnum", ::iswalnum), NDK_FNX("iswpunct", ::iswpunct), NDK_FNX("iswprint", ::iswprint),
    NDK_FNX("iswxdigit", ::iswxdigit), NDK_FNX("iswcntrl", ::iswcntrl),
    // stdlib
    NDK_FNX("malloc", ::malloc), NDK_FNX("calloc", ::calloc), NDK_FNX("realloc", ::realloc),
    NDK_FNX("free", ::free), NDK_FNX("memalign", ::memalign), NDK_FNX("valloc", ::valloc),
    NDK_FN("posix_memalign", ::posix_memalign),
    NDK_FNX("malloc_usable_size", ::malloc_usable_size),
    NDK_FNX("atoi", ::atoi), NDK_FN("atol", G_atol), NDK_FNX("atoll", ::atoll), NDK_FNX("atof", G_atof),
    NDK_FN("strtol", G_strtol), NDK_FN("strtoul", G_strtoul), NDK_FN("strtoll", ::strtoll),
    NDK_FN("strtoull", ::strtoull), NDK_FN("strtod", ::strtod), NDK_FN("strtof", ::strtof),
    NDK_FN("strtold", G_strtold), NDK_FN("strtoimax", ::strtoimax), NDK_FN("strtoumax", ::strtoumax),
    NDK_FNX("abs", G_abs), NDK_FN("labs", G_labs), NDK_FNX("llabs", G_llabs),
    NDK_RAW("div", G_div), NDK_RAW("ldiv", G_div),
    NDK_FN("qsort", G_qsort), NDK_FN("bsearch", G_bsearch),
    NDK_FN("tsearch", G_tsearch), NDK_FN("tfind", G_tfind), NDK_FN("tdelete", G_tdelete),
    NDK_FN("twalk", G_twalk), NDK_FN("tdestroy", G_tdestroy),
    NDK_FNX("rand", ::rand), NDK_FNX("srand", ::srand), NDK_FNX("rand_r", ::rand_r),
    NDK_FNX("random", G_random), NDK_FNX("srandom", ::srandom), NDK_FNX("lrand48", G_lrand48),
    NDK_FNX("mrand48", G_mrand48), NDK_FNX("srand48", G_srand48), NDK_FNX("drand48", ::drand48),
    NDK_FNX("getenv", G_getenv), NDK_FN("setenv", G_setenv), NDK_FN("unsetenv", G_unsetenv),
    NDK_FN("putenv", ::putenv),  // (arm64: ndk_bionic.cc keeps its environ in step)
    NDK_FNX("abort", G_abort), NDK_FN("exit", G_exit), NDK_FNX("_exit", ::_exit),
    NDK_FN("atexit", G_atexit), NDK_FN("__cxa_atexit", G_cxa_atexit),
    NDK_FN("__aeabi_atexit", G_aeabi_atexit), NDK_FN("__cxa_finalize", G_cxa_finalize),
    NDK_DATA("__stack_chk_guard", Addr(&g_stack_chk_guard)),
    NDK_FNX("__stack_chk_fail", G_stack_chk_fail),
    NDK_FNX("__assert", G_assert), NDK_FNX("__assert2", G_assert2),
    NDK_FNX("__errno", G_errno),
    NDK_RAW("setjmp", G_setjmp), NDK_RAW("_setjmp", G_setjmp_nomask), NDK_RAW("sigsetjmp", G_sigsetjmp),
    NDK_RAW("longjmp", G_longjmp), NDK_RAW("_longjmp", G_longjmp),
    NDK_RAW("siglongjmp", G_longjmp),
    // time
    NDK_FN("time", G_time), NDK_FN("gettimeofday", G_gettimeofday),
    NDK_FN("clock_gettime", G_clock_gettime), NDK_FN("clock_getres", G_clock_getres),
    NDK_FN("nanosleep", G_nanosleep), NDK_FNX("usleep", G_usleep), NDK_FNX("sleep", G_sleep),
    NDK_FNX("clock", G_clock), NDK_FN("difftime", G_difftime), NDK_FN("gmtime", G_gmtime),
    NDK_FN("gmtime_r", G_gmtime_r), NDK_FN("localtime", G_localtime),
    NDK_FN("localtime_r", G_localtime_r), NDK_FN("mktime", G_mktime),
    NDK_FN("timegm", G_timegm), NDK_FNX("ctime", G_ctime), NDK_FN("strftime", ::strftime),
    NDK_FNX("tzset", ::tzset),
    // files and directories
    NDK_RAW("open", G_open), NDK_FNX("close", FdClose), NDK_FNX("read", FdRead),
    NDK_FNX("write", FdWrite), NDK_FN("readv", ::readv), NDK_FN("writev", ::writev),
    NDK_FN("lseek", G_lseek), NDK_FN("lseek64", G_lseek64), NDK_FN("pread", G_pread),
    NDK_FN("pwrite", G_pwrite), NDK_FN("ftruncate", G_ftruncate), NDK_FN("truncate", G_truncate),
    NDK_FNX("fsync", ::fsync), NDK_FNX("fdatasync", ::fdatasync), NDK_RAW("fcntl", G_fcntl),
    NDK_RAW("ioctl", G_ioctl), NDK_FNX("dup", ::dup), NDK_FNX("dup2", ::dup2),
    NDK_FNX("pipe", ::pipe), NDK_FNX("poll", G_poll), NDK_FN("select", G_select),
    NDK_FN("stat", G_stat), NDK_FN("lstat", G_lstat), NDK_FN("fstat", G_fstat),
    NDK_FNX("access", ::access), NDK_FNX("chmod", ::chmod), NDK_FNX("fchmod", ::fchmod),
    NDK_FNX("chown", ::chown), NDK_FNX("umask", ::umask), NDK_FNX("mkdir", ::mkdir),
    NDK_FNX("rmdir", ::rmdir), NDK_FNX("unlink", ::unlink), NDK_FNX("rename", ::rename),
    NDK_FNX("link", ::link), NDK_FNX("symlink", ::symlink), NDK_FNX("readlink", ::readlink),
    NDK_FNX("chdir", ::chdir), NDK_FNX("getcwd", ::getcwd), NDK_FN("utime", G_utime),
    NDK_FNX("isatty", G_isatty), NDK_FNX("fnmatch", G_fnmatch),
    NDK_FNX("opendir", G_opendir), NDK_FNX("fdopendir", ::fdopendir), NDK_FNX("readdir", ::readdir),
    NDK_FN("readdir_r", ::readdir_r), NDK_FNX("closedir", ::closedir),
    NDK_FNX("rewinddir", ::rewinddir), NDK_FNX("dirfd", ::dirfd),
    NDK_FNX("telldir", ::telldir), NDK_FNX("seekdir", ::seekdir),
    NDK_FN("mmap", G_mmap), NDK_FN("munmap", G_munmap), NDK_FN("mprotect", G_mprotect),
    NDK_FNX("memfd_create", GuestMemfdCreate),
    NDK_FNX("msync", ::msync), NDK_FN("madvise", G_madvise),
    NDK_FNX("sysconf", G_sysconf), NDK_FNX("getpagesize", G_getpagesize),
    NDK_FNX("pathconf", ::pathconf), NDK_FNX("fpathconf", ::fpathconf),
    // sockets
    NDK_FNX("socket", ::socket), NDK_FNX("bind", ::bind), NDK_FNX("connect", ::connect),
    NDK_FNX("listen", ::listen), NDK_FNX("accept", ::accept), NDK_FNX("shutdown", ::shutdown),
    NDK_FNX("send", ::send), NDK_FNX("recv", ::recv), NDK_FNX("sendto", ::sendto),
    NDK_FNX("recvfrom", ::recvfrom), NDK_FN("sendmsg", ::sendmsg), NDK_FN("recvmsg", ::recvmsg),
    // 32-bit guests: bionic's 8-byte timeval of SO_RCVTIMEO/SO_SNDTIMEO converted;
    // arm64's 16-byte one goes through as it was.
    NDK_FN("getsockopt", G_getsockopt), NDK_FN("setsockopt", G_setsockopt),
    NDK_FN64("getsockopt", ::getsockopt), NDK_FN64("setsockopt", ::setsockopt),
    NDK_FNX("getsockname", ::getsockname), NDK_FNX("getpeername", ::getpeername),
    NDK_FNX("socketpair", ::socketpair),
    NDK_FN("getaddrinfo", G_getaddrinfo), NDK_FN("freeaddrinfo", G_freeaddrinfo),
    NDK_FNX("gai_strerror", G_gai_strerror), NDK_FNX("inet_ntop", ::inet_ntop),
    NDK_FNX("inet_pton", ::inet_pton), NDK_FNX("inet_addr", ::inet_addr),
    NDK_FNX("inet_ntoa", G_inet_ntoa), NDK_FNX("htonl", ::htonl), NDK_FNX("htons", ::htons),
    NDK_FNX("ntohl", ::ntohl), NDK_FNX("ntohs", ::ntohs), NDK_FNX("gethostname", ::gethostname),
    // process and signals
    NDK_FNX("getpid", ::getpid), NDK_FNX("getppid", ::getppid), NDK_FNX("gettid", G_gettid),
    NDK_FNX("getuid", ::getuid), NDK_FNX("geteuid", ::geteuid), NDK_FNX("getgid", ::getgid),
    NDK_FNX("getegid", ::getegid), NDK_FN("getpwuid", G_getpwuid), NDK_FN("getpwnam", G_getpwnam),
    NDK_FNX("sched_yield", G_sched_yield), NDK_FN("sigaction", G_sigaction),
    NDK_FN("bsd_signal", G_bsd_signal), NDK_FN("signal", G_bsd_signal), NDK_FNX("raise", G_raise),
    NDK_FNX("kill", G_kill), NDK_FN("sigemptyset", G_sigemptyset),
    NDK_FN("sigfillset", G_sigfillset), NDK_FN("sigaddset", G_sigaddset),
    NDK_FN("sigdelset", G_sigdelset), NDK_FN("sigismember", G_sigismember),
    NDK_FN("sigprocmask", G_sigprocmask), NDK_RAW("syscall", G_syscall),
    NDK_FNX("__system_property_get", G_system_property_get),
};

// --- what API 24-27 apps (and Firefox 158) import besides --------------------------------
// docs/topics/apps8-survey.md, docs/topics/fw8-system.md.

// TIMER_ABSTIME: sleep until an absolute time of the clock. Returns an
// error number (not -1/errno), as POSIX says.
int G_clock_nanosleep(int clk, int flags, const GTimespec* req, GTimespec* rem) {
  struct timespec h = {req->tv_sec, req->tv_nsec};
  if (flags & 1 /* TIMER_ABSTIME */) {
    struct timespec now;
    if (clock_gettime(clk, &now) != 0)
      return ToGuestErrno(errno);
    int64_t ns = (int64_t)(h.tv_sec - now.tv_sec) * 1000000000 + (h.tv_nsec - now.tv_nsec);
    if (ns <= 0)
      return 0;
    h.tv_sec = ns / 1000000000;
    h.tv_nsec = ns % 1000000000;
    rem = nullptr;
  }
  double left;
  int r = GuestSleep(h.tv_sec * 1000.0 + h.tv_nsec / 1e6, &left);
  if (r == 0 || r == kParked)
    return 0;
  if (rem) MsToTimespec(left, rem);
  return 4;  // EINTR (Linux)
}

// Advice only (wasm memory is never paged out): POSIX's form returns 0.
int G_posix_madvise(void*, size_t, int) { return 0; }

// GNU strerror_r: the message, maybe not in |buf|.
char* G_gnu_strerror_r(int e, char* buf, size_t n) {
  if (n > 0)
    G_strlcpy(buf, ::strerror(ToHostErrno(e)), n);
  return buf;
}

// recvmmsg (QUIC): recvmsg until the first that would block. bionic's and
// musl's struct msghdr/mmsghdr have the same 32-bit layout.
struct GMmsghdr {
  struct msghdr hdr;
  uint32_t len;
};
static_assert(sizeof(GMmsghdr) == 32, "bionic's struct mmsghdr");
int G_recvmmsg(int fd, GMmsghdr* v, unsigned n, int flags, GTimespec*) {
  unsigned i = 0;
  for (; i < n; i++) {
    ssize_t r = recvmsg(fd, &v[i].hdr, i == 0 ? flags & ~0x10000 /* MSG_WAITFORONE */
                                              : flags | MSG_DONTWAIT);
    if (r < 0) {
      if (i == 0)
        return -1;
      break;
    }
    v[i].len = (uint32_t)r;
  }
  return (int)i;
}

// The identity calls of a crash reporter (Breakpad, Firefox's crashhelper):
// one uid (0) that cannot become another.
int G_setid(uint32_t id) {
  if (id == 0)
    return 0;
  errno = EPERM;
  return -1;
}
int G_setsid() {
  errno = EPERM;
  return -1;
}
int G_setpgid(int, int) { return 0; }
int G_eperm_path(const char*) {
  errno = EPERM;
  return -1;
}
int G_daemon(int, int) {
  errno = ENOSYS;
  return -1;
}
int G_setgroups(size_t, const uint32_t*) {
  errno = EPERM;
  return -1;
}
int G_initgroups(const char*, uint32_t) {
  errno = EPERM;
  return -1;
}
int G_getgroups(int, uint32_t*) { return 0; }
int G_getgrouplist(const char*, uint32_t gid, uint32_t* groups, int* n) {
  if (*n < 1) {
    *n = 1;
    return -1;
  }
  groups[0] = gid;
  *n = 1;
  return 1;
}
int G_setfsid(uint32_t) { return 0; }  // the previous id: 0
void G_sync() {}
ssize_t G_process_vm_rw(int, const void*, unsigned long, const void*, unsigned long,
                        unsigned long) {
  errno = ENOSYS;
  return -1;
}
// getpw*_r, getgr*_r (API 24): no entry (bionic's for an unknown id).
int G_getpw_r(uint32_t, void*, char*, size_t, uint32_t* result) {
  *result = 0;
  return 2 /* ENOENT */;
}
int G_getpwnam_r(const char*, void*, char*, size_t, uint32_t* result) {
  *result = 0;
  return 2 /* ENOENT */;
}

// bionic's pthread_cleanup_push/pop (the macros call these): a per-thread
// stack of { prev, routine, arg }.
struct GCleanup {
  uint32_t prev, routine, arg;
};
thread_local uint32_t t_cleanup;
void* CleanupSlot() { return &t_cleanup; }   // a green thread's own (ndk_green.cc)
[[maybe_unused]] const bool kCleanupSlot = (RegisterGreenSlot(CleanupSlot, sizeof t_cleanup), true);
void G_cleanup_push(GCleanup* c, uint32_t routine, uint32_t arg) {
  c->prev = t_cleanup;
  c->routine = routine;
  c->arg = arg;
  t_cleanup = Addr(c);
}
void G_cleanup_pop(GCleanup* c, int execute) {
  t_cleanup = c->prev;
  if (execute)
    CallGuest1(c->routine, c->arg);
}

int G_fegetround() { return 0; }  // FE_TONEAREST: wasm rounds to nearest

const HostSymbol kLibcApi24to27[] = {
    NDK_FN("clock_nanosleep", G_clock_nanosleep), NDK_FNX("posix_madvise", G_posix_madvise),
    NDK_FNX("__gnu_strerror_r", G_gnu_strerror_r), NDK_FN("recvmmsg", G_recvmmsg),
    NDK_FNX("setuid", G_setid), NDK_FNX("setgid", G_setid), NDK_FNX("setsid", G_setsid),
    NDK_FNX("setpgid", G_setpgid), NDK_FNX("chroot", G_eperm_path), NDK_FNX("daemon", G_daemon),
    NDK_FNX("setgroups", G_setgroups), NDK_FNX("initgroups", G_initgroups),
    NDK_FNX("getgroups", G_getgroups), NDK_FNX("getgrouplist", G_getgrouplist),
    NDK_FNX("setfsuid", G_setfsid), NDK_FNX("setfsgid", G_setfsid), NDK_FNX("sync", G_sync),
    NDK_FNX("process_vm_readv", G_process_vm_rw), NDK_FNX("process_vm_writev", G_process_vm_rw),
    NDK_FN("getpwuid_r", G_getpw_r), NDK_FN("getgrgid_r", G_getpw_r),
    NDK_FN("getpwnam_r", G_getpwnam_r), NDK_FN("getgrnam_r", G_getpwnam_r),
    NDK_FN("__pthread_cleanup_push", G_cleanup_push),
    NDK_FN("__pthread_cleanup_pop", G_cleanup_pop),
    // bionic's struct stat64 is struct stat; off64_t paths are open()'s.
    NDK_FN("stat64", G_stat), NDK_FN("lstat64", G_lstat), NDK_FN("fstat64", G_fstat),
    NDK_RAW("open64", G_open),
};

const HostSymbol kLibmApi24to27[] = {
    NDK_FN("fegetround", G_fegetround),
};

}  // namespace

int GuestSetsockopt(int fd, int level, int optname, const void* optval, socklen_t optlen) {
  return G_setsockopt(fd, level, optname, optval, optlen);
}
int GuestGetsockopt(int fd, int level, int optname, void* optval, socklen_t* optlen) {
  return G_getsockopt(fd, level, optname, optval, optlen);
}

int HostOpenFlags(int guest_flags) { return ToHostOpenFlags(guest_flags); }
int GuestOpenFlags(int host_flags) { return ToGuestOpenFlags(host_flags); }

void ToGuestStatBuf(const struct stat& host, void* guest) {
  ToGuestStat(host, static_cast<GStat*>(guest));
}

void* GuestMmap(void* addr, size_t len, int prot, int flags, int fd, int64_t off) {
  // Guest code thinks in 4 KB pages (sysconf(_SC_PAGESIZE)); the host's
  // mappings are made of larger ones (PAGE_SIZE). An anonymous MAP_FIXED
  // inside memory the guest has, at an address the host cannot map at (an
  // allocator committing or decommitting 4 KB pages, as mozjemalloc does),
  // is what it means: fresh zero pages there. Protection is not enforced.
  // The same for a length that is not whole host pages: the host would round
  // it up and zero the guest's live data after it.
  if ((flags & MAP_FIXED) && (flags & MAP_ANONYMOUS) && addr != nullptr &&
      ((Addr(addr) & (PAGE_SIZE - 1)) != 0 || (len & (PAGE_SIZE - 1)) != 0) &&
      (Addr(addr) & 4095) == 0) {
    GuestZero(addr, len, prot);
    FlushGuestCode(Addr(addr), len);
    if (prot & PROT_EXEC)
      NoteExecRegion(Addr(addr), Addr(addr) + len);
    return addr;
  }
  // The same for a file (topic native-exec: qemu-user maps a guest's ELF
  // segments and files page by page, 4 KB, inside its reservation): the
  // file's bytes copied into place, zeros after its end. Writes to a shared
  // one stay in memory (logged once).
  if ((flags & MAP_FIXED) && !(flags & MAP_ANONYMOUS) && addr != nullptr && fd >= 0 &&
      ((Addr(addr) & (PAGE_SIZE - 1)) != 0 || (len & (PAGE_SIZE - 1)) != 0) &&
      (Addr(addr) & 4095) == 0 && (off & 4095) == 0) {
    struct stat st;
    if (fstat(fd, &st) != 0)
      return MAP_FAILED;
    GuestZero(addr, len, prot | PROT_WRITE);
    size_t done = 0;
    while (done < len) {
      const ssize_t n = pread(fd, static_cast<char*>(addr) + done, len - done, off + static_cast<int64_t>(done));
      if (n <= 0)
        break;
      done += static_cast<size_t>(n);
    }
    if ((flags & MAP_SHARED) && (prot & PROT_WRITE)) {
      static std::atomic<bool> told{false};
      if (!told.exchange(true))
        __android_log_print(5, "ndk", "mmap: a shared writable file mapping at 4 KB pages is a copy");
    }
    FlushGuestCode(Addr(addr), len);
    if (prot & PROT_EXEC)
      NoteExecRegion(Addr(addr), Addr(addr) + len);
    return addr;
  }
  void* r = mmap(addr, len, prot, flags, fd, off);
  if (r != MAP_FAILED && (len & (PAGE_SIZE - 1)) != 0) {
    std::lock_guard<std::mutex> l(g_odd_maps_lock);
    g_odd_maps.emplace(Addr(r), len);
  }
  if (r != MAP_FAILED && (prot & PROT_EXEC)) {
    FlushGuestCode(Addr(r), len);
    NoteExecRegion(Addr(r), Addr(r) + len);
  }
  return r;
}

void FlushGuestCode(uint32_t addr, size_t len) {
  ndk_flush_code(addr, len);
  if (ndk64_flush_code)
    ndk64_flush_code(addr, len);
}

// --- reservations beyond the wasm memory (arm64 guests) ------------------------------
// An LP64 guest reserves address space as if it had 2^47 bytes of it and
// commits what it uses: Hermes (React Native) reserves 3 GB per runtime and
// commits 4 MB segments from its start (MAP_FIXED); Dart with compressed
// pointers reserves 8 GB and keeps the half aligned to 4 GB; Unity's Dynamic
// Heap (Subway Surfers 3.70) reserves 511 MB fifteen times and commits blocks
// of it. A guest address is a wasm address (the A64 CPU uses its low 32
// bits), 4 GB in all -- 1.5 GB on a phone. So a PROT_NONE reservation from the
// budget's guest_reserve_mb up is a window above 4 GB of its own (W = k << 36:
// the guest's addresses W + 2 GB .. W + 2 GB + len, kWindowLow) without
// memory; what the guest commits (MAP_FIXED with access, mprotect with access)
// gets wasm memory then, in 64 KB pieces. A window has a table, an entry per 64 KB of its addresses:
// guest address a is wasm address low32(a) + T[a >> 16 & 0xfffff], T =
// ndk64_window_table[a >> 36 & 63] (the A64 CPU, shim/ndk/cpu64; the host's
// thunks, GuestAddr64). The memory of a commit is one run of wasm memory (a
// host function given a pointer and a length sees it whole), with room after
// it for the commits that follow (nothing touches the room until then: fresh
// memory is no footprint). Addresses not committed are a scratch page (on a
// device: SIGSEGV). Decommitting (PROT_NONE, madvise) keeps the memory -- a
// touched wasm page is the tab's for good (wasm-heap-trim.md); munmap gives it
// back. Out of wasm memory a commit fails with ENOMEM, as on a device out of
// memory. A pointer into a window's memory the host hands back is in the
// window again (GuestPtr64: a table from each 64 KB of wasm memory to its
// window address).
std::atomic<uint32_t> g_reserve_windows{0};

namespace {

const uint64_t k4G = 1ull << 32;
const int kWindowShift = 36;  // a window: 64 GB of addresses
const int kMaxWindows = 63;   // windows 1-63: the CPU has 64 tables
const int kGranShift = 16;    // a table entry: 64 KB (the host's page)
const uint64_t kGran = 1ull << kGranShift;
// The room after a commit's memory: as much as the commit, 1-32 MB.
const uint64_t kRoomMin = 1ull << 20, kRoomMax = 32ull << 20;
// A window's addresses start this far into its 64 GB: a host function that
// takes only the low 32 bits of one (a guest pointer it truncates) is past
// a phone's wasm memory (1.5 GB) and traps there, instead of reading or
// writing the VM's own data at the bottom of the memory.
const uint64_t kWindowLow = 2ull << 30;
// A run with up to this much data moves when a commit grows past it.
const uint64_t kMoveMax = 1ull << 20;
// A reservation of a spawned program (topic native-exec: qemu-user's TCG code
// buffer, ~128 MB) up to this size stays in the low 4 GB, where the A64 CPU
// can run the code it will hold; larger ones (Unity's 511 MB, a 32-bit
// guest's 4 GB) are windows.
const uint64_t kLowReserveMax = 256ull << 20;

uint64_t GranDown(uint64_t a) { return a & ~(kGran - 1); }
uint64_t GranUp(uint64_t a) { return (a + kGran - 1) & ~(kGran - 1); }

// Address ranges [lo, hi), by start.
template <typename T>
class Ranges {
 public:
  void Add(T lo, T hi) {
    if (lo >= hi)
      return;
    Remove(lo, hi);
    typename std::map<T, T>::iterator next = r_.lower_bound(lo);
    if (next != r_.end() && next->first == hi) {
      hi = next->second;
      r_.erase(next);
    }
    typename std::map<T, T>::iterator prev = r_.lower_bound(lo);
    if (prev != r_.begin() && (--prev)->second == lo) {
      prev->second = hi;
      return;
    }
    r_[lo] = hi;
  }
  void Remove(T lo, T hi) {
    if (lo >= hi)
      return;
    typename std::map<T, T>::iterator it = r_.lower_bound(lo);
    if (it != r_.begin()) {
      typename std::map<T, T>::iterator prev = it;
      --prev;
      if (prev->second > lo) {
        const T end = prev->second;
        prev->second = lo;
        if (end > hi)
          r_[hi] = end;
      }
    }
    while (it != r_.end() && it->first < hi) {
      const T end = it->second;
      it = r_.erase(it);
      if (end > hi) {
        r_[hi] = end;
        break;
      }
    }
  }
  // The parts of [lo, hi) in these ranges.
  std::vector<std::pair<T, T>> Within(T lo, T hi) const {
    std::vector<std::pair<T, T>> out;
    typename std::map<T, T>::const_iterator it = r_.upper_bound(lo);
    if (it != r_.begin())
      --it;
    for (; it != r_.end() && it->first < hi; ++it) {
      const T a = std::max(lo, it->first), b = std::min(hi, it->second);
      if (a < b)
        out.push_back(std::make_pair(a, b));
    }
    return out;
  }
  bool Overlaps(T lo, T hi) const { return !Within(lo, hi).empty(); }
  uint64_t Bytes() const {
    uint64_t n = 0;
    for (const auto& e : r_)
      n += e.second - e.first;
    return n;
  }
  bool Empty() const { return r_.empty(); }

 private:
  std::map<T, T> r_;
};

struct Run {
  uint64_t n;  // bytes, whole 64 KB
  uint32_t w;  // its wasm memory: the guest's [g, g + n) is wasm [w, w + n)
};

struct ReserveWindow {
  uint64_t base = 0, end = 0;     // the window's addresses [base, end); 0: a free slot
  Ranges<uint64_t> reserved;      // of them, still the guest's (munmap takes them away)
  std::map<uint64_t, Run> runs;   // its memory, by guest start
  Ranges<uint64_t> committed;     // mapped with access now
  Ranges<uint64_t> used;          // committed at some time: the memory holds data
  uint32_t* table = nullptr;      // an entry per 64 KB of [base, end)
  bool refused = false;           // a commit found no memory (logged once)
  bool split = false;             // a commit's memory is not one run (logged once)
};
std::mutex g_windows_lock;
ReserveWindow g_windows[kMaxWindows];  // slot k: window k + 1
uint32_t g_scratch = 0;                // the 64 KB of wasm memory uncommitted addresses are
// Each 64 KB of wasm memory in a window's run: its guest address >> 16 (0:
// none), read without the lock (GuestPtr64 on every pointer a host function
// returns).
uint32_t g_window_rev[1u << (32 - kGranShift)];

uint64_t WindowW(int slot) { return static_cast<uint64_t>(slot + 1) << kWindowShift; }

// ARC_RESERVE_LOG=1: every call on a window's addresses (diagnostics; read
// each time: they are few, and the environment comes late).
bool ReserveLogOn() {
  const char* e = getenv("ARC_RESERVE_LOG");
  return e && *e == '1';
}
void ReserveLog(const char* what, int slot, uint64_t addr, uint64_t len, int prot) {
  if (!ReserveLogOn())
    return;
  const uint64_t base = slot >= 0 ? g_windows[slot].base : 0;
  __android_log_print(4, "ndk", "reserve-log %s w%d +%#llx %#llx prot %d", what, slot + 1,
                      static_cast<unsigned long long>(addr - base), static_cast<unsigned long long>(len), prot);
}

// The window whose addresses hold |addr| (under the lock), or -1.
int WindowOf(uint64_t addr) {
  const uint64_t k = addr >> kWindowShift;
  if (k == 0 || k > kMaxWindows)
    return -1;
  const ReserveWindow& w = g_windows[k - 1];
  return w.base && addr >= w.base && addr < w.end ? static_cast<int>(k - 1) : -1;
}

// The table entries of the guest's [g, g + n) (whole 64 KB): wasm memory
// from |wasm|, or (0) the scratch page.
void SetEntriesLocked(ReserveWindow& win, uint64_t g, uint64_t n, uint32_t wasm) {
  for (uint64_t o = 0; o < n; o += kGran) {
    const uint64_t i = (g + o - win.base) >> kGranShift;
    const uint32_t at = wasm ? wasm + static_cast<uint32_t>(o) : g_scratch;
    __atomic_store_n(&win.table[i], at - static_cast<uint32_t>(g + o), __ATOMIC_RELEASE);
  }
  if (wasm) {
    for (uint64_t o = 0; o < n; o += kGran)
      __atomic_store_n(&g_window_rev[(wasm + o) >> kGranShift], static_cast<uint32_t>((g + o) >> kGranShift),
                       __ATOMIC_RELEASE);
  }
}

// Memory for the guest's [g, g + n) (whole 64 KB): a run of fresh wasm memory.
bool AddRunLocked(ReserveWindow& win, uint64_t g, uint64_t n) {
  if (n >= k4G)
    return false;
  void* r = GuestMmap(nullptr, static_cast<size_t>(n), PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (r == MAP_FAILED)
    return false;
  win.runs[g] = Run{n, Addr(r)};
  SetEntriesLocked(win, g, n, Addr(r));
  return true;
}

// Memory for the guest's [lo, hi) (whole 64 KB, |room| more after it) continuing the run
// that ends at |lo| in wasm memory too, if nobody has the wasm memory after that run
// (MAP_FIXED_NOREPLACE, shim/wasm_mm): the commit is one piece with the run, and no
// data moves. False when there is no such run or the memory there is taken.
bool ExtendRunLocked(ReserveWindow& win, uint64_t lo, uint64_t hi, uint64_t room) {
  std::map<uint64_t, Run>::iterator prev = win.runs.lower_bound(lo);
  if (prev == win.runs.begin())
    return false;
  --prev;
  if (prev->first + prev->second.n != lo)
    return false;
  const uint64_t at = static_cast<uint64_t>(prev->second.w) + prev->second.n;
  for (uint64_t extra : {room, uint64_t{0}}) {
    const uint64_t n = hi - lo + extra;
    if (at + n > k4G)
      continue;
    void* m = GuestMmap(Ptr<void>(static_cast<uint32_t>(at)), static_cast<size_t>(n), PROT_NONE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (m == MAP_FAILED)
      continue;
    if (Addr(m) != at) {  // (an mmap that took it as a hint)
      GuestMunmap(m, static_cast<size_t>(n));
      return false;
    }
    win.runs[lo] = Run{n, static_cast<uint32_t>(at)};
    SetEntriesLocked(win, lo, n, static_cast<uint32_t>(at));
    if (ReserveLogOn())
      __android_log_print(4, "ndk", "reservation at %#llx (arm64): a run grows in place at +%#llx by %llu KB",
                          static_cast<unsigned long long>(win.base), static_cast<unsigned long long>(lo - win.base),
                          static_cast<unsigned long long>(n >> 10));
    return true;
  }
  return false;
}

// The memory of the guest's [lo, hi) (whole 64 KB) goes back.
void DropRunsLocked(ReserveWindow& win, uint64_t lo, uint64_t hi) {
  std::map<uint64_t, Run>::iterator it = win.runs.upper_bound(lo);
  if (it != win.runs.begin())
    --it;
  while (it != win.runs.end() && it->first < hi) {
    const uint64_t g = it->first, n = it->second.n;
    const uint32_t w = it->second.w;
    const uint64_t a = std::max(lo, g), b = std::min(hi, g + n);
    if (a >= b) {
      ++it;
      continue;
    }
    const uint32_t wa = w + static_cast<uint32_t>(a - g);
    GuestMunmap(Ptr<void>(wa), static_cast<size_t>(b - a));
    for (uint64_t o = 0; o < b - a; o += kGran)
      __atomic_store_n(&g_window_rev[(wa + o) >> kGranShift], 0u, __ATOMIC_RELEASE);
    SetEntriesLocked(win, a, b - a, 0);
    it = win.runs.erase(it);
    if (g < a)
      win.runs[g] = Run{a - g, w};
    if (b < g + n)
      it = win.runs.insert(std::make_pair(b, Run{g + n - b, w + static_cast<uint32_t>(b - g)})).first, ++it;
  }
}

// Moves the data of run |r| to a new run for the guest's [lo, hi) (whole
// 64 KB, all of |r| in it) with |room| after it, so that a commit growing
// past a run that holds a little data is one run too (Unity's Dynamic Heap
// commits a 4 KB header, then the block after it). Its pages are made
// accessible (zeros a decommit left are written), copied, given their
// protection, then the table points to the new memory and the old goes
// back. Other runs in [lo, hi) are room only. False (nothing changed) when
// the wasm memory has no more.
bool RegrowRunLocked(ReserveWindow& win, std::map<uint64_t, Run>::iterator r, uint64_t lo, uint64_t hi,
                     uint64_t room) {
  void* m = MAP_FAILED;
  for (uint64_t extra : {room, uint64_t{0}}) {
    if (hi + extra - lo < k4G)
      m = GuestMmap(nullptr, static_cast<size_t>(hi + extra - lo), PROT_NONE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (m != MAP_FAILED) {
      hi += extra;
      break;
    }
  }
  if (m == MAP_FAILED)
    return false;
  const uint32_t w = Addr(m), rw = r->second.w;
  const uint64_t rg = r->first, rn = r->second.n;
  for (const auto& u : win.used.Within(rg, rg + rn)) {
    void* from = Ptr<void>(rw + static_cast<uint32_t>(u.first - rg));
    void* to = Ptr<void>(w + static_cast<uint32_t>(u.first - lo));
    const size_t n = static_cast<size_t>(u.second - u.first);
    GuestMprotect(from, n, PROT_READ | PROT_WRITE);
    GuestMprotect(to, n, PROT_READ | PROT_WRITE);
    memcpy(to, from, n);
  }
  // What was decommitted is decommitted again.
  for (const auto& u : win.used.Within(rg, rg + rn)) {
    uint64_t at = u.first;
    for (const auto& c : win.committed.Within(u.first, u.second)) {
      if (c.first > at)
        GuestMprotect(Ptr<void>(w + static_cast<uint32_t>(at - lo)), static_cast<size_t>(c.first - at), PROT_NONE);
      at = c.second;
    }
    if (at < u.second)
      GuestMprotect(Ptr<void>(w + static_cast<uint32_t>(at - lo)), static_cast<size_t>(u.second - at), PROT_NONE);
  }
  {
    uint64_t data = 0;
    for (const auto& u : win.used.Within(rg, rg + rn))
      data += u.second - u.first;
    __android_log_print(4, "ndk", "reservation at %#llx (arm64): %llu KB of data at +%#llx moved for a commit up to +%#llx",
                        static_cast<unsigned long long>(win.base), static_cast<unsigned long long>(data >> 10),
                        static_cast<unsigned long long>(rg - win.base), static_cast<unsigned long long>(hi - win.base));
  }
  win.runs.erase(r);
  DropRunsLocked(win, lo, hi);  // (room only)
  win.runs[lo] = Run{hi - lo, w};
  SetEntriesLocked(win, lo, hi - lo, w);
  for (uint64_t o = 0; o < rn; o += kGran)
    __atomic_store_n(&g_window_rev[(rw + o) >> kGranShift], 0u, __ATOMIC_RELEASE);
  GuestMunmap(Ptr<void>(rw), static_cast<size_t>(rn));
  return true;
}

// The room after the guest's [lo, hi): as much as it, kRoomMin-kRoomMax,
// up to the next run and the window's end.
uint64_t RoomAfterLocked(const ReserveWindow& win, uint64_t lo, uint64_t hi) {
  std::map<uint64_t, Run>::const_iterator next = win.runs.lower_bound(hi);
  const uint64_t limit = std::min(next == win.runs.end() ? GranUp(win.end) : next->first, GranUp(win.end));
  return std::min(std::min(std::max(hi - lo, kRoomMin), kRoomMax), limit > hi ? limit - hi : 0);
}

// Wasm memory for the guest's [lo, hi) in a window (a commit). Its memory
// is one run, with room after it: the room of earlier commits is given up
// for it; a run before it with data grows in place if the wasm memory after
// it is free (ExtendRunLocked), else, holding little data, moves
// (RegrowRunLocked); otherwise (more data around) each missing part is a run
// of its own (logged once). False when the wasm memory has no more.
bool EnsureMemoryLocked(ReserveWindow& win, uint64_t lo, uint64_t hi) {
  const uint64_t glo = GranDown(lo), ghi = std::min(GranUp(hi), GranUp(win.end));
  // The parts without memory, and the runs in [glo, ghi) that hold data.
  std::vector<std::pair<uint64_t, uint64_t>> gaps;
  std::vector<std::map<uint64_t, Run>::iterator> held;
  uint64_t at = glo;
  std::map<uint64_t, Run>::iterator it = win.runs.upper_bound(glo);
  if (it != win.runs.begin())
    --it;
  for (; it != win.runs.end() && it->first < ghi; ++it) {
    const uint64_t g = it->first, e = g + it->second.n;
    if (e <= glo)
      continue;
    if (g > at)
      gaps.push_back(std::make_pair(at, g));
    if (win.used.Overlaps(std::max(g, glo), std::min(e, ghi)))
      held.push_back(it);
    at = std::max(at, e);
  }
  if (at < ghi)
    gaps.push_back(std::make_pair(at, ghi));
  if (gaps.empty())
    return true;
  if (held.empty()) {
    // Only the room of earlier commits: one run for all of it.
    DropRunsLocked(win, glo, ghi);
    gaps.assign(1, std::make_pair(glo, ghi));
  } else {
    if (held.size() == 1) {
      std::map<uint64_t, Run>::iterator r = held[0];
      // The commit continues the run: in place if the wasm memory after it is free.
      if (gaps.size() == 1 && gaps[0].first == r->first + r->second.n &&
          ExtendRunLocked(win, gaps[0].first, gaps[0].second, RoomAfterLocked(win, gaps[0].first, gaps[0].second)))
        return true;
      uint64_t data = 0;
      for (const auto& u : win.used.Within(r->first, r->first + r->second.n))
        data += u.second - u.first;
      const uint64_t lo2 = std::min(r->first, glo), hi2 = std::max(ghi, r->first + r->second.n);
      if (data <= kMoveMax && RegrowRunLocked(win, r, lo2, hi2, RoomAfterLocked(win, lo2, hi2)))
        return true;
    }
  }
  for (const auto& gap : gaps) {
    const uint64_t want = gap.second - gap.first, room = RoomAfterLocked(win, gap.first, gap.second);
    if (ExtendRunLocked(win, gap.first, gap.second, room))
      continue;
    if (!held.empty() && !win.split) {
      win.split = true;
      __android_log_print(5, "ndk", "reservation at %#llx (arm64): the memory of a commit at +%#llx is in parts",
                          static_cast<unsigned long long>(win.base), static_cast<unsigned long long>(lo - win.base));
    }
    if (!(room && AddRunLocked(win, gap.first, want + room)) && !AddRunLocked(win, gap.first, want))
      return false;
  }
  return true;
}

// The wasm memory of the guest's [lo, hi): (guest start, wasm start, bytes).
struct Part {
  uint64_t g;
  uint32_t w;
  uint64_t n;
};
std::vector<Part> PartsLocked(const ReserveWindow& win, uint64_t lo, uint64_t hi) {
  std::vector<Part> out;
  std::map<uint64_t, Run>::const_iterator it = win.runs.upper_bound(lo);
  if (it != win.runs.begin())
    --it;
  for (; it != win.runs.end() && it->first < hi; ++it) {
    const uint64_t a = std::max(lo, it->first), b = std::min(hi, it->first + it->second.n);
    if (a < b)
      out.push_back(Part{a, it->second.w + static_cast<uint32_t>(a - it->first), b - a});
  }
  return out;
}

void RefusedLocked(ReserveWindow& win, uint64_t addr, uint64_t len) {
  if (win.refused)
    return;
  win.refused = true;
  uint64_t n = 0;
  for (const auto& r : win.runs)
    n += r.second.n;
  __android_log_print(5, "ndk", "reservation at %#llx (arm64): no memory for %#llx-%#llx (%llu MB of it have memory)",
                      static_cast<unsigned long long>(win.base), static_cast<unsigned long long>(addr),
                      static_cast<unsigned long long>(addr + len), static_cast<unsigned long long>(n >> 20));
}

void FreeWindowLocked(int slot) {
  ReserveWindow& win = g_windows[slot];
  DropRunsLocked(win, win.base, GranUp(win.end));
  if (ndk64_window_table)
    ndk64_window_table[slot + 1] = nullptr;
  free(win.table);
  win = ReserveWindow();
  g_reserve_windows.fetch_sub(1);
}

// Reservations from this size up are windows (the budget's guest_reserve_mb).
size_t WindowMinBytes() {
  static const size_t bytes = [] {
    uint32_t mb = arc_mem_config ? arc_mem_config()->guest_reserve_mb : 0;
    if (mb == 0)
      mb = 64;
    return static_cast<size_t>(mb) << 20;
  }();
  return bytes;
}

// A new window for a reservation of |len|: its address, or ~0 (errno).
// |want|: the address asked for (a hint): honoured when its window is free,
// 64 KB aligned and the reservation fits there (qemu-user picks a hole from
// /proc/self/maps and takes nothing else: ProcMapsWindows); 0, or one that
// cannot be had: the first free window, at W + kWindowLow -- or, with
// |exact|, EEXIST (MAP_FIXED_NOREPLACE).
uint64_t NewWindow(uint64_t len, uint64_t want = 0, bool exact = false) {
  if (len > (1ull << kWindowShift) - kWindowLow) {
    errno = ENOMEM;
    return ~0ull;
  }
  uint64_t base = 0;
  bool first = false;
  {
    std::lock_guard<std::mutex> l(g_windows_lock);
    int slot = 0;
    uint64_t at = 0;
    if (want) {
      const uint64_t k = want >> kWindowShift;
      const uint64_t offset = want & ((1ull << kWindowShift) - 1);
      if (k >= 1 && k <= kMaxWindows && !g_windows[k - 1].base && (want & (kGran - 1)) == 0 &&
          offset + len <= (1ull << kWindowShift)) {
        slot = static_cast<int>(k - 1);
        at = want;
      }
    }
    if (!at) {
      if (exact) {
        errno = EEXIST;
        return ~0ull;
      }
      while (slot < kMaxWindows && g_windows[slot].base)
        ++slot;
      if (slot == kMaxWindows) {
        errno = ENOMEM;
        return ~0ull;
      }
      at = WindowW(slot) + kWindowLow;
    }
    if (!g_scratch) {
      void* s = GuestMmap(nullptr, kGran, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if (s == MAP_FAILED)
        return ~0ull;
      g_scratch = Addr(s);
    }
    const size_t entries = static_cast<size_t>(GranUp(len) >> kGranShift);
    uint32_t* table = static_cast<uint32_t*>(malloc(entries * sizeof(uint32_t)));
    if (!table) {
      errno = ENOMEM;
      return ~0ull;
    }
    ReserveWindow& win = g_windows[slot];
    win.base = base = at;
    win.end = base + len;
    win.table = table;
    for (size_t i = 0; i < entries; ++i)
      table[i] = g_scratch - static_cast<uint32_t>(base + (static_cast<uint64_t>(i) << kGranShift));
    win.reserved.Add(base, win.end);
    if (ndk64_window_table)
      ndk64_window_table[slot + 1] = table - ((base - WindowW(slot)) >> kGranShift);  // (indexed by a >> 16 & 0xfffff)
    first = g_reserve_windows.fetch_add(1) == 0;
  }
  // Translations made so far take the low 32 bits of an address as it is.
  static std::atomic<bool> on{false};
  if (first && !on.exchange(true) && ndk64_windows_on)
    ndk64_windows_on();
  __android_log_print(4, "ndk", "reservation of %llu MB (arm64) at %#llx: memory as it is committed",
                      static_cast<unsigned long long>(len >> 20), static_cast<unsigned long long>(base));
  return base;
}

}  // namespace

// /proc/self/maps lines of the address space an arm64 guest has beyond the
// libraries: the wasm memory (and what aliases it, below the first window),
// each window in use whole, the low guard of each free one -- so that a hole
// found there is the start of a free window (qemu-user's guest_base).
std::string ProcMapsWindows() {
  std::string text;
  char line[160];
  auto add = [&](uint64_t lo, uint64_t hi, const char* name) {
    snprintf(line, sizeof line, "%010llx-%010llx ---p 00000000 00:00 0          %s\n",
             static_cast<unsigned long long>(lo), static_cast<unsigned long long>(hi), name);
    text += line;
  };
  add(0, 1ull << kWindowShift, "[anon:arc-wasm memory]");
  std::lock_guard<std::mutex> l(g_windows_lock);
  for (int slot = 0; slot < kMaxWindows; ++slot) {
    if (g_windows[slot].base)
      add(WindowW(slot), WindowW(slot + 1), "[anon:arc-wasm reservation]");
    else
      add(WindowW(slot), WindowW(slot) + kWindowLow, "[anon:arc-wasm guard]");
  }
  add(WindowW(kMaxWindows), 1ull << 47, "[anon:arc-wasm beyond]");
  return text;
}

uint64_t ReserveWindowAddr64(uint32_t a) {
  const uint32_t g = __atomic_load_n(&g_window_rev[a >> kGranShift], __ATOMIC_ACQUIRE);
  return g ? (static_cast<uint64_t>(g) << kGranShift) | (a & (kGran - 1)) : a;
}

// Bytes of the windows' memory not committed: not in use (the budget,
// shim/membudget/mem_budget.cc).
extern "C" size_t wasm_mm_large_free_bytes();
extern "C" size_t wasm_mm_large_hole_bytes();
extern "C" uint64_t ndk_reserve_idle_bytes() {
  if (g_reserve_windows.load() == 0)
    return 0;
  std::lock_guard<std::mutex> l(g_windows_lock);
  uint64_t n = 0, mem_all = 0, committed_all = 0, used_all = 0;
  int windows = 0;
  for (const ReserveWindow& w : g_windows) {
    if (!w.base)
      continue;
    uint64_t mem = 0;
    for (const auto& r : w.runs)
      mem += r.second.n;
    const uint64_t c = w.committed.Bytes();
    n += mem > c ? mem - c : 0;
    mem_all += mem;
    committed_all += c;
    used_all += w.used.Bytes();
    ++windows;
  }
  // (the budget asks every second: a summary every 30 s)
  static double last = 0;
  const double now = emscripten_get_now();
  if (now - last >= 30000) {
    last = now;
    __android_log_print(4, "ndk", "reservations (arm64): %d windows, memory %llu MB: committed %llu, "
                        "decommitted %llu, room %llu; wasm_mm free %zu MB, holes %zu MB, malloc top %u MB",
                        windows,
                        static_cast<unsigned long long>(mem_all >> 20),
                        static_cast<unsigned long long>(committed_all >> 20),
                        static_cast<unsigned long long>((used_all - std::min(used_all, committed_all)) >> 20),
                        static_cast<unsigned long long>((mem_all - std::min(mem_all, used_all)) >> 20),
                        wasm_mm_large_free_bytes() >> 20, wasm_mm_large_hole_bytes() >> 20,
                        static_cast<unsigned>(reinterpret_cast<uintptr_t>(sbrk(0)) >> 20));
  }
  return n;
}

uint64_t GuestMmap64(uint64_t addr, uint64_t len, int prot, int flags, int fd, int64_t off) {
  const bool fixed = (flags & MAP_FIXED) != 0;
  if (len == 0) {
    errno = EINVAL;
    return ~0ull;
  }
  // A reservation: a window (at the hint, if it can be had there). A spawned
  // program's mid-size reservation with no window hint stays in the low 4 GB
  // instead: qemu-user's code buffer must be executable there (topic
  // native-exec; the A64 CPU's PC is 32-bit).
  if (!fixed && (flags & MAP_ANONYMOUS) && prot == PROT_NONE && len >= WindowMinBytes()) {
    const bool window_hint = addr + len > k4G;
    const bool spawned = arc_lproc_any() && arc_lproc_is_spawned(arc_lproc_current());
    if (spawned && !window_hint && len <= kLowReserveMax && len < k4G) {
      void* r = GuestMmap(Ptr<void>(0u), static_cast<size_t>(len), prot,
                          flags & ~MAP_FIXED_NOREPLACE, fd, off);
      if (r != MAP_FAILED)
        return Addr(r);
      // The low 4 GB is full: a window after all.
    }
    return NewWindow((len + 4095) & ~4095ull, window_hint ? addr : 0, (flags & MAP_FIXED_NOREPLACE) != 0);
  }
  if (!fixed && addr + len > k4G) {
    if (flags & MAP_FIXED_NOREPLACE) {  // what is asked for is not to be had
      errno = EEXIST;
      return ~0ull;
    }
    addr = 0;  // a hint where there is nothing
  }
  if (addr + len <= k4G && len < k4G) {
    void* r = GuestMmap(Ptr<void>(static_cast<uint32_t>(addr)), static_cast<size_t>(len), prot, flags, fd, off);
    return r == MAP_FAILED ? ~0ull : Addr(r);
  }
  if (!fixed) {
    errno = ENOMEM;
    return ~0ull;
  }
  // MAP_FIXED in a window: with access (or a file's), memory for it first;
  // PROT_NONE, anonymous: what memory there is stays, decommitted.
  std::lock_guard<std::mutex> l(g_windows_lock);
  const int slot = WindowOf(addr);
  if (slot < 0 || addr + len > g_windows[slot].end || (addr & 4095) != 0) {
    errno = slot < 0 || addr + len > g_windows[slot].end ? ENOMEM : EINVAL;
    return ~0ull;
  }
  ReserveWindow& w = g_windows[slot];
  ReserveLog("mmap", slot, addr, len, prot);
  const bool memory = prot != PROT_NONE || !(flags & MAP_ANONYMOUS);
  if (memory && !EnsureMemoryLocked(w, addr, addr + len)) {
    RefusedLocked(w, addr, len);
    errno = ENOMEM;
    return ~0ull;
  }
  for (const Part& p : PartsLocked(w, addr, addr + len)) {
    if (GuestMmap(Ptr<void>(p.w), static_cast<size_t>(p.n), prot, flags, fd, off + static_cast<int64_t>(p.g - addr)) ==
        MAP_FAILED)
      return ~0ull;
  }
  w.reserved.Add(addr, addr + len);
  if (prot != PROT_NONE) {
    w.committed.Add(addr, addr + len);
    w.used.Add(addr, addr + len);
  } else {
    w.committed.Remove(addr, addr + len);
  }
  return addr;
}

int GuestMunmap64(uint64_t addr, uint64_t len) {
  if ((addr & 4095) != 0 || len == 0) {
    errno = EINVAL;
    return -1;
  }
  if (addr + len <= k4G)
    return GuestMunmap(Ptr<void>(static_cast<uint32_t>(addr)), static_cast<size_t>(len));
  if (addr < k4G) {
    GuestMunmap(Ptr<void>(static_cast<uint32_t>(addr)), static_cast<size_t>(k4G - addr));
    len -= k4G - addr;
    addr = k4G;
  }
  len = (len + 4095) & ~4095ull;
  std::lock_guard<std::mutex> l(g_windows_lock);
  for (int slot = 0; slot < kMaxWindows; ++slot) {
    ReserveWindow& w = g_windows[slot];
    if (!w.base || addr >= w.end || addr + len <= w.base)
      continue;
    ReserveLog("munmap", slot, addr, len, 0);
    const uint64_t lo = std::max(addr, w.base), hi = std::min(addr + len, w.end);
    // Whole 64 KB go back; the guest's 4 KB pages of partly unmapped ones
    // are decommitted (zeros if mapped again).
    const uint64_t glo = GranUp(lo), ghi = hi == w.end ? GranUp(hi) : GranDown(hi);
    for (const Part& p : PartsLocked(w, lo, hi)) {
      const uint64_t a = p.g, b = p.g + p.n;
      if (a >= glo && b <= ghi)
        continue;
      for (const auto& edge : {std::make_pair(a, std::min(b, glo)), std::make_pair(std::max(a, ghi), b)})
        if (edge.first < edge.second)
          GuestMmap(Ptr<void>(p.w + static_cast<uint32_t>(edge.first - p.g)), static_cast<size_t>(edge.second - edge.first),
                    PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    }
    if (glo < ghi)
      DropRunsLocked(w, glo, ghi);
    w.reserved.Remove(lo, hi);
    w.committed.Remove(lo, hi);
    w.used.Remove(lo, hi);
    if (w.reserved.Empty())
      FreeWindowLocked(slot);
  }
  return 0;  // (unmapping what is not mapped is no error)
}

// What mprotect (|prot|) and madvise (|prot| -1) act on: |*low|, wasm
// memory (all of [addr, addr + len) under 4 GB, or a window's memory in it;
// access gets memory first); false with ENOMEM where there is nothing, and
// when the wasm memory has no more.
bool GuestMemRange64(uint64_t addr, uint64_t len, int prot, std::vector<std::pair<uint32_t, uint32_t>>* low) {
  low->clear();
  if (addr + len <= k4G) {
    low->push_back(std::make_pair(static_cast<uint32_t>(addr), static_cast<uint32_t>(addr + len)));
    return true;
  }
  std::lock_guard<std::mutex> l(g_windows_lock);
  const int slot = WindowOf(addr);
  if (slot < 0 || addr + len > g_windows[slot].end) {
    errno = ENOMEM;
    return false;
  }
  ReserveWindow& w = g_windows[slot];
  ReserveLog(prot < 0 ? "madvise" : "mprotect", slot, addr, len, prot);
  const uint64_t end = (addr + len + 4095) & ~4095ull;
  if (prot > 0) {
    if (!EnsureMemoryLocked(w, addr, end)) {
      RefusedLocked(w, addr, len);
      errno = ENOMEM;
      return false;
    }
    w.committed.Add(addr, end);
    w.used.Add(addr, end);
  } else if (prot == PROT_NONE) {
    w.committed.Remove(addr, end);
  }
  for (const Part& p : PartsLocked(w, addr, end))
    low->push_back(std::make_pair(p.w, p.w + static_cast<uint32_t>(p.n)));
  return true;
}

// Whether all of the guest's [addr, addr + len) is memory: in the wasm
// memory, or committed in a window (process_vm_readv's EFAULT).
bool GuestHasMemory64(uint64_t addr, uint64_t len) {
  if (addr + len <= k4G)
    return addr >= 4096 && addr + len <= emscripten_get_heap_size();
  std::lock_guard<std::mutex> l(g_windows_lock);
  const int slot = WindowOf(addr);
  if (slot < 0 || addr + len > g_windows[slot].end)
    return false;
  for (const auto& part : g_windows[slot].committed.Within(addr, addr + len))
    if (part.first == addr && part.second == addr + len)
      return true;
  return false;
}

// memfd_create: an ashmem region (posix_translation's DevAshmem), whose
// MAP_SHARED mappings are all the same memory at the same address -- in one
// wasm memory, that is what a JIT's double mapping (a writable view and an
// executable view of one region) needs; ftruncate sets its size there.
int GuestMemfdCreate(const char* name, unsigned flags) {
  int fd = open("/dev/ashmem", O_RDWR | ((flags & 1 /* MFD_CLOEXEC */) ? O_CLOEXEC : 0));
  if (fd < 0)
    return -1;
  char buf[256] = {0};
  snprintf(buf, sizeof buf, "memfd:%s", name ? name : "");
  ioctl(fd, _IOW(0x77, 1, char[256]) /* ASHMEM_SET_NAME */, buf);
  return fd;
}

void RunAtexit(uint32_t dso) {
  for (;;) {
    AtexitEntry e;
    {
      std::lock_guard<std::mutex> l(g_atexit_lock);
      size_t i = g_atexit.size();
      while (i > 0 && dso != 0 && g_atexit[i - 1].dso != dso) i--;
      if (i == 0)
        return;
      e = g_atexit[i - 1];
      g_atexit.erase(g_atexit.begin() + (i - 1));
    }
    if (e.cxa)
      CallGuest1(e.fn, e.arg);
    else
      CallGuest0(e.fn);
  }
}

void RegisterLibc() {
  InitCtype();
  g_stack_chk_guard = (uint32_t)(lrand48() ^ (uintptr_t)&g_stack_chk_guard ^ 0x5a17c0deu);
  if (g_stack_chk_guard == 0)
    g_stack_chk_guard = 0x5a17c0de;
  RegisterHostSymbols("libc.so", kLibc);
  RegisterHostSymbols("libc.so", kLibcApi24to27);
  RegisterHostSymbols("libm.so", kLibmApi24to27);
}

}  // namespace ndk
