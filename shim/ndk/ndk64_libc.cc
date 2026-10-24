// bionic's libc for arm64-v8a guests (topic "ndk-arm64"): the functions whose
// 32-bit thunks (ndk_libc.cc and friends) do not fit an LP64 guest because a
// structure, a pointer to a pointer, a long or a callback differs: time and
// struct tm, struct stat, strto*, qsort/bsearch, atexit, setjmp/longjmp,
// open/fcntl/ioctl/syscall (arm64 numbers, also SVC #0 in guest code),
// iovec/msghdr, addrinfo, passwd, signals, getauxval. The rest of libc is
// the same thunks as for 32-bit guests (NDK_FNX in their tables).
//
// Layouts are bionic's for LP64 (bionic/libc/include, asm-generic): long,
// size_t, pointers and time_t are 8 bytes.

#include <dirent.h>
#include <emscripten/threading.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netdb.h>
#include <poll.h>
#include <pwd.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

#include "ndk_bridge.h"

extern "C" int __android_log_print(int prio, const char* tag, const char* fmt, ...);
// A signal to another thread (either ABI's: shim/ndk/cpu/signal.cc).
extern "C" int ndk_sig_send(int tid, int sig) __attribute__((weak));

namespace ndk {

namespace {

// --- guest structures (LP64 bionic) -------------------------------------------------------------
struct G64Timeval : GuestLayout64 {
  int64_t tv_sec, tv_usec;
};
struct G64Timespec : GuestLayout64 {
  int64_t tv_sec, tv_nsec;
};
struct G64Tm : GuestLayout64 {
  int32_t tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
  int32_t pad;
  int64_t tm_gmtoff;
  uint64_t tm_zone;
};
static_assert(sizeof(G64Tm) == 56, "bionic LP64 struct tm");
struct G64Stat : GuestLayout64 {   // asm-generic struct stat (arm64)
  uint64_t st_dev, st_ino;
  uint32_t st_mode, st_nlink, st_uid, st_gid;
  uint64_t st_rdev, pad1;
  int64_t st_size;
  int32_t st_blksize, pad2;
  int64_t st_blocks;
  int64_t st_atime_, st_atime_nsec, st_mtime_, st_mtime_nsec, st_ctime_, st_ctime_nsec;
  uint32_t unused4, unused5;
};
static_assert(sizeof(G64Stat) == 128, "bionic arm64 struct stat");
struct G64Iovec {
  uint64_t base, len;
};
struct G64Msghdr : GuestLayout64 {
  uint64_t msg_name;
  uint32_t msg_namelen, pad0;
  uint64_t msg_iov;
  uint64_t msg_iovlen;
  uint64_t msg_control;
  uint64_t msg_controllen;
  int32_t msg_flags, pad1;
};
static_assert(sizeof(G64Msghdr) == 56, "bionic LP64 struct msghdr");
struct G64Mmsghdr : GuestLayout64 {
  G64Msghdr hdr;
  uint32_t len, pad;
};
struct G64Addrinfo : GuestLayout64 {
  int32_t ai_flags, ai_family, ai_socktype, ai_protocol;
  uint32_t ai_addrlen, pad;
  uint64_t ai_canonname, ai_addr, ai_next;
};
static_assert(sizeof(G64Addrinfo) == 48, "bionic LP64 struct addrinfo");
struct G64Passwd : GuestLayout64 {
  uint64_t pw_name, pw_passwd;
  uint32_t pw_uid, pw_gid;
  uint64_t pw_dir, pw_shell;
};
static_assert(sizeof(G64Passwd) == 40, "bionic LP64 struct passwd");

void ToTm(const G64Tm* g, struct tm* h) {
  memset(h, 0, sizeof *h);
  h->tm_sec = g->tm_sec; h->tm_min = g->tm_min; h->tm_hour = g->tm_hour; h->tm_mday = g->tm_mday;
  h->tm_mon = g->tm_mon; h->tm_year = g->tm_year; h->tm_wday = g->tm_wday; h->tm_yday = g->tm_yday;
  h->tm_isdst = g->tm_isdst;
  h->tm_gmtoff = static_cast<long>(g->tm_gmtoff);
  h->tm_zone = Ptr<const char>(GuestAddr64(g->tm_zone));
}
void FromTm(const struct tm* h, G64Tm* g) {
  g->tm_sec = h->tm_sec; g->tm_min = h->tm_min; g->tm_hour = h->tm_hour; g->tm_mday = h->tm_mday;
  g->tm_mon = h->tm_mon; g->tm_year = h->tm_year; g->tm_wday = h->tm_wday; g->tm_yday = h->tm_yday;
  g->tm_isdst = h->tm_isdst;
  g->pad = 0;
  g->tm_gmtoff = h->tm_gmtoff;
  g->tm_zone = GuestPtr64(h->tm_zone);
}

void ToStat(const struct stat& h, G64Stat* g) {
  memset(g, 0, sizeof *g);
  g->st_dev = h.st_dev;
  g->st_ino = h.st_ino;
  g->st_mode = h.st_mode;
  g->st_nlink = h.st_nlink;
  g->st_uid = h.st_uid;
  g->st_gid = h.st_gid;
  g->st_rdev = h.st_rdev;
  g->st_size = h.st_size;
  g->st_blksize = h.st_blksize;
  g->st_blocks = h.st_blocks;
  g->st_atime_ = h.st_atim.tv_sec;
  g->st_atime_nsec = h.st_atim.tv_nsec;
  g->st_mtime_ = h.st_mtim.tv_sec;
  g->st_mtime_nsec = h.st_mtim.tv_nsec;
  g->st_ctime_ = h.st_ctim.tv_sec;
  g->st_ctime_nsec = h.st_ctim.tv_nsec;
}

// Host iovecs for |n| guest ones (truncated lengths: wasm buffers are < 4 GB).
std::vector<struct iovec> Iovecs(uint32_t guest, int n) {
  std::vector<struct iovec> v(n > 0 ? n : 0);
  const G64Iovec* g = Ptr<const G64Iovec>(guest);
  for (int i = 0; i < n; i++) {
    v[i].iov_base = Ptr<void>(GuestAddr64(g[i].base));
    v[i].iov_len = g[i].len > 0xffffffffu ? 0xffffffffu : static_cast<size_t>(g[i].len);
  }
  return v;
}

// --- time --------------------------------------------------------------------------------------
int G64_gettimeofday(G64Timeval* tv, void* tz) {
  struct timeval h;
  int r = gettimeofday(&h, nullptr);
  if (tv) {
    tv->tv_sec = h.tv_sec;
    tv->tv_usec = h.tv_usec;
  }
  if (tz) memset(tz, 0, 8);
  return r;
}
int G64_clock_gettime(int clk, G64Timespec* ts) {
  struct timespec h;
  int r = clock_gettime(HostClockId(clk), &h);
  if (r == 0 && ts) {
    ts->tv_sec = h.tv_sec;
    ts->tv_nsec = h.tv_nsec;
  }
  return r;
}
int G64_clock_getres(int clk, G64Timespec* ts) {
  struct timespec h;
  int r = clock_getres(HostClockId(clk), &h);
  if (r == 0 && ts) {
    ts->tv_sec = h.tv_sec;
    ts->tv_nsec = h.tv_nsec;
  }
  return r;
}
int G64_nanosleep(const G64Timespec* req, G64Timespec* rem) {
  if (req->tv_nsec < 0 || req->tv_nsec >= 1000000000 || req->tv_sec < 0) {
    errno = EINVAL;
    return -1;
  }
  // (a guest signal's handler ends it: EINTR with the time left)
  double left = 0;
  if (GuestSleep(req->tv_sec * 1000.0 + req->tv_nsec / 1e6, &left) == 0)
    return 0;
  if (rem) {
    rem->tv_sec = static_cast<int64_t>(left / 1000);
    rem->tv_nsec = static_cast<int64_t>((left - rem->tv_sec * 1000.0) * 1e6);
  }
  errno = EINTR;
  return -1;
}
// Returns an error number (POSIX), not -1/errno.
int G64_clock_nanosleep(int clk, int flags, const G64Timespec* req, G64Timespec* rem) {
  struct timespec h = {static_cast<time_t>(req->tv_sec), static_cast<long>(req->tv_nsec)};
  if (flags & 1 /* TIMER_ABSTIME */) {
    struct timespec now;
    if (clock_gettime(clk, &now) != 0) return ToGuestErrno(errno);
    int64_t ns = (int64_t)(h.tv_sec - now.tv_sec) * 1000000000 + (h.tv_nsec - now.tv_nsec);
    if (ns <= 0) return 0;
    h.tv_sec = ns / 1000000000;
    h.tv_nsec = ns % 1000000000;
    rem = nullptr;
  }
  // (as nanosleep: a guest signal's handler ends it)
  double left = 0;
  if (GuestSleep(h.tv_sec * 1000.0 + h.tv_nsec / 1e6, &left) == 0) return 0;
  if (rem) {
    rem->tv_sec = static_cast<int64_t>(left / 1000);
    rem->tv_nsec = static_cast<int64_t>((left - rem->tv_sec * 1000.0) * 1e6);
  }
  return 4;  // EINTR (Linux)
}
int64_t G64_clock() {
  struct timespec ts;
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;   // CLOCKS_PER_SEC 1000000
}

thread_local G64Tm t_tm;
G64Tm* G64_gmtime_r(const time_t* t, G64Tm* out) {
  struct tm h;
  if (!gmtime_r(t, &h)) return nullptr;
  FromTm(&h, out);
  return out;
}
G64Tm* G64_gmtime(const time_t* t) { return G64_gmtime_r(t, &t_tm); }
G64Tm* G64_localtime_r(const time_t* t, G64Tm* out) {
  struct tm h;
  if (!localtime_r(t, &h)) return nullptr;
  FromTm(&h, out);
  return out;
}
G64Tm* G64_localtime(const time_t* t) { return G64_localtime_r(t, &t_tm); }
time_t G64_mktime(G64Tm* g) {
  struct tm h;
  ToTm(g, &h);
  time_t r = mktime(&h);
  FromTm(&h, g);
  return r;
}
time_t G64_timegm(G64Tm* g) {
  struct tm h;
  ToTm(g, &h);
  time_t r = timegm(&h);
  FromTm(&h, g);
  return r;
}
size_t G64_strftime(char* s, size_t max, const char* fmt, const G64Tm* g) {
  struct tm h;
  ToTm(g, &h);
  return strftime(s, max, fmt, &h);
}
char* G64_asctime(const G64Tm* g) {
  struct tm h;
  ToTm(g, &h);
  return asctime(&h);
}
char* G64_strptime(const char* s, const char* fmt, G64Tm* g) {
  struct tm h;
  ToTm(g, &h);
  char* r = strptime(s, fmt, &h);
  FromTm(&h, g);
  return r;
}
int G64_utimes(const char* path, const G64Timeval* tv) {
  if (!tv) return utimes(path, nullptr);
  struct timeval h[2] = {{static_cast<time_t>(tv[0].tv_sec), static_cast<suseconds_t>(tv[0].tv_usec)},
                         {static_cast<time_t>(tv[1].tv_sec), static_cast<suseconds_t>(tv[1].tv_usec)}};
  return utimes(path, h);
}
int G64_utimensat(int dirfd, const char* path, const G64Timespec* ts, int flags) {
  std::string full;
  if (!ResolveAtPath(dirfd, path, &full)) return -1;
  if (!ts) return utimensat(AT_FDCWD, full.c_str(), nullptr, flags);
  struct timespec h[2] = {{static_cast<time_t>(ts[0].tv_sec), static_cast<long>(ts[0].tv_nsec)},
                          {static_cast<time_t>(ts[1].tv_sec), static_cast<long>(ts[1].tv_nsec)}};
  return utimensat(AT_FDCWD, full.c_str(), h, flags);
}
int G64_futimens(int fd, const G64Timespec* ts) {
  if (!ts) return futimens(fd, nullptr);
  struct timespec h[2] = {{static_cast<time_t>(ts[0].tv_sec), static_cast<long>(ts[0].tv_nsec)},
                          {static_cast<time_t>(ts[1].tv_sec), static_cast<long>(ts[1].tv_nsec)}};
  return futimens(fd, h);
}

// --- stat ---------------------------------------------------------------------------------------------
int G64_stat(const char* p, G64Stat* g) {
  struct stat h;
  int r = stat(p, &h);
  if (r == 0) ToStat(h, g);
  return r;
}
int G64_lstat(const char* p, G64Stat* g) {
  struct stat h;
  int r = lstat(p, &h);
  if (r == 0) ToStat(h, g);
  return r;
}
int G64_fstat(int fd, G64Stat* g) {
  struct stat h;
  int r = fstat(fd, &h);
  if (r == 0) ToStat(h, g);
  return r;
}
int G64_fstatat(int dirfd, const char* path, G64Stat* g, int flags) {
  struct stat h;
  int r;
  if (path && path[0] == '\0' && (flags & 0x1000 /* AT_EMPTY_PATH */)) {
    r = fstat(dirfd, &h);
  } else {
    std::string full;
    if (!ResolveAtPath(dirfd, path, &full)) return -1;
    r = (flags & 0x100 /* AT_SYMLINK_NOFOLLOW */) ? lstat(full.c_str(), &h) : stat(full.c_str(), &h);
  }
  if (r == 0) ToStat(h, g);
  return r;
}

// --- stdlib ------------------------------------------------------------------------------------------
int64_t G64_atol(const char* s) { return atoll(s); }
int64_t G64_labs(int64_t v) { return v < 0 ? -v : v; }
int64_t G64_strtol(const char* s, uint64_t* end, int base) {
  char* e;
  int64_t r = strtoll(s, &e, base);
  if (end) *end = GuestPtr64(e);
  return r;
}
uint64_t G64_strtoul(const char* s, uint64_t* end, int base) {
  char* e;
  uint64_t r = strtoull(s, &e, base);
  if (end) *end = GuestPtr64(e);
  return r;
}
double G64_strtod(const char* s, uint64_t* end) {
  char* e;
  double r = strtod(s, &e);
  if (end) *end = GuestPtr64(e);
  return r;
}
float G64_strtof(const char* s, uint64_t* end) {
  char* e;
  float r = strtof(s, &e);
  if (end) *end = GuestPtr64(e);
  return r;
}
long double G64_strtold(const char* s, uint64_t* end) {
  char* e;
  long double r = strtold(s, &e);
  if (end) *end = GuestPtr64(e);
  return r;
}
char* G64_strtok_r(char* s, const char* delim, uint64_t* save) {
  char* p = Ptr<char>(GuestAddr64(*save));
  char* r = strtok_r(s, delim, &p);
  *save = GuestPtr64(p);
  return r;
}
char* G64_strsep(uint64_t* sp, const char* delim) {
  char* p = Ptr<char>(GuestAddr64(*sp));
  char* r = strsep(&p, delim);
  *sp = GuestPtr64(p);
  return r;
}
int G64_posix_memalign(uint64_t* out, size_t align, size_t size) {
  void* p = nullptr;
  int r = posix_memalign(&p, align, size);
  if (r == 0) *out = Addr(p);
  return ToGuestErrno(r);
}
void* G64_aligned_alloc(size_t align, size_t size) { return aligned_alloc(align, size); }

// div_t (8 bytes) comes back in x0; ldiv_t/lldiv_t/imaxdiv_t (16) in x0, x1.
void G64_div(NdkCpu64State* s, void*) {
  int32_t n = static_cast<int32_t>(s->x[0]), d = static_cast<int32_t>(s->x[1]);
  int32_t q = d ? n / d : 0, r = d ? n % d : 0;
  s->x[0] = static_cast<uint32_t>(q) | static_cast<uint64_t>(static_cast<uint32_t>(r)) << 32;
}
void G64_ldiv(NdkCpu64State* s, void*) {
  int64_t n = static_cast<int64_t>(s->x[0]), d = static_cast<int64_t>(s->x[1]);
  s->x[0] = static_cast<uint64_t>(d ? n / d : 0);
  s->x[1] = static_cast<uint64_t>(d ? n % d : 0);
}

// qsort/bsearch with a guest comparator. Sorting runs on the host (a merge
// of index order, stable enough for a comparator): elements are compared in
// place through pointers into the guest array.
struct GuestCmp {
  uint32_t fn;
  int operator()(const void* a, const void* b) const {
    uint64_t args[2] = {Addr(a), Addr(b)};
    return static_cast<int32_t>(CallGuest64(fn, args, 2));
  }
};
void G64_qsort(void* base, size_t n, size_t size, uint64_t cmp) {
  if (n < 2 || size == 0) return;
  GuestCmp c = {static_cast<uint32_t>(cmp)};
  // Insertion sort for small arrays, else heap-free merge sort with a buffer.
  char* a = static_cast<char*>(base);
  std::vector<char> tmp(size);
  if (n <= 16) {
    for (size_t i = 1; i < n; i++) {
      memcpy(tmp.data(), a + i * size, size);
      size_t j = i;
      while (j > 0 && c(a + (j - 1) * size, tmp.data()) > 0) {
        memcpy(a + j * size, a + (j - 1) * size, size);
        j--;
      }
      memcpy(a + j * size, tmp.data(), size);
    }
    return;
  }
  std::vector<char> buf(n * size);
  for (size_t width = 1; width < n; width *= 2) {
    for (size_t lo = 0; lo < n; lo += 2 * width) {
      size_t mid = lo + width < n ? lo + width : n, hi = lo + 2 * width < n ? lo + 2 * width : n;
      size_t i = lo, j = mid, k = lo;
      while (i < mid && j < hi) {
        if (c(a + j * size, a + i * size) < 0) memcpy(&buf[k++ * size], a + j++ * size, size);
        else memcpy(&buf[k++ * size], a + i++ * size, size);
      }
      while (i < mid) memcpy(&buf[k++ * size], a + i++ * size, size);
      while (j < hi) memcpy(&buf[k++ * size], a + j++ * size, size);
    }
    memcpy(a, buf.data(), n * size);
  }
}
void* G64_bsearch(const void* key, const void* base, size_t n, size_t size, uint64_t cmp) {
  GuestCmp c = {static_cast<uint32_t>(cmp)};
  const char* a = static_cast<const char*>(base);
  size_t lo = 0, hi = n;
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    int r = c(key, a + mid * size);
    if (r == 0) return const_cast<char*>(a + mid * size);
    if (r < 0) hi = mid;
    else lo = mid + 1;
  }
  return nullptr;
}

// --- atexit --------------------------------------------------------------------------------------------
struct Atexit64 {
  uint64_t fn, arg, dso;
  bool cxa;
};
std::mutex g_atexit_lock;
std::vector<Atexit64> g_atexit;
int G64_cxa_atexit(uint64_t fn, uint64_t arg, uint64_t dso) {
  std::lock_guard<std::mutex> l(g_atexit_lock);
  g_atexit.push_back({fn, arg, dso, true});
  return 0;
}
int G64_atexit(uint64_t fn) {
  std::lock_guard<std::mutex> l(g_atexit_lock);
  g_atexit.push_back({fn, 0, 0, false});
  return 0;
}
void RunAtexit64(uint64_t dso) {
  for (;;) {
    Atexit64 e;
    {
      std::lock_guard<std::mutex> l(g_atexit_lock);
      size_t i = g_atexit.size();
      while (i > 0 && dso != 0 && g_atexit[i - 1].dso != dso) i--;
      if (i == 0) return;
      e = g_atexit[i - 1];
      g_atexit.erase(g_atexit.begin() + (i - 1));
    }
    uint64_t arg = e.arg;
    CallGuest64(static_cast<uint32_t>(e.fn), e.cxa ? &arg : nullptr, e.cxa ? 1 : 0);
  }
}
void G64_cxa_finalize(uint64_t dso) { RunAtexit64(dso); }
void G64_exit(int status) {
  RunAtexit64(0);
  exit(status);
}

// C++ thread_local destructors (__cxa_thread_atexit_impl): run, last first,
// when the thread's guest state goes (a host thread exit).
struct ThreadDtor64 {
  uint64_t fn, obj;
};
struct ThreadDtors64 {
  std::vector<ThreadDtor64> list;
  ~ThreadDtors64() {
    while (!list.empty()) {
      ThreadDtor64 d = list.back();
      list.pop_back();
      uint64_t a = d.obj;
      CallGuest64(static_cast<uint32_t>(d.fn), &a, 1);
    }
  }
};
thread_local ThreadDtors64 t_dtors;
int G64_cxa_thread_atexit_impl(uint64_t fn, uint64_t obj, uint64_t) {
  t_dtors.list.push_back({fn, obj});
  return 0;
}

uint64_t g_stack_chk_guard64;

// --- setjmp/longjmp (bionic arm64 jmp_buf: 32 longs) -----------------------------------------------------
// Our layout: magic, x19-x30, sp, d8-d15.
enum { kJb64Magic = 0, kJb64Regs = 1, kJb64Sp = 13, kJb64Fp = 14 };
const uint64_t kJmp64Magic = 0x4e444b4a36340001ull;
void G64_setjmp(NdkCpu64State* s, void*) {
  uint64_t* jb = Ptr<uint64_t>(GuestAddr64(s->x[0]));
  jb[kJb64Magic] = kJmp64Magic;
  for (int i = 0; i < 12; i++) jb[kJb64Regs + i] = s->x[19 + i];
  jb[kJb64Sp] = s->sp;
  for (int i = 0; i < 8; i++) jb[kJb64Fp + i] = s->v[8 + i][0];
  s->x[0] = 0;
}
void G64_longjmp(NdkCpu64State* s, void*) {
  const uint64_t* jb = Ptr<const uint64_t>(GuestAddr64(s->x[0]));
  uint32_t val = static_cast<uint32_t>(s->x[1]);
  if (jb[kJb64Magic] != kJmp64Magic) {
    __android_log_print(7, "ndk", "longjmp with a bad jmp_buf 0x%llx", (unsigned long long)s->x[0]);
    LeaveGuest("longjmp with a bad jmp_buf");
    abort();
  }
  for (int i = 0; i < 12; i++) s->x[19 + i] = jb[kJb64Regs + i];
  s->sp = jb[kJb64Sp];
  for (int i = 0; i < 8; i++) {
    s->v[8 + i][0] = jb[kJb64Fp + i];
    s->v[8 + i][1] = 0;
  }
  s->x[0] = val ? val : 1;   // the CPU returns to x30: the setjmp call site
}

// --- files --------------------------------------------------------------------------------------------
void G64_open(NdkCpu64State* s, void*) {
  const char* path = Ptr<const char>(GuestAddr64(s->x[0]));
  int flags = static_cast<int>(s->x[1]);
  int mode = static_cast<int>(s->x[2]);
  ThunkErrnoBegin();
  int r = FdOpen(path, flags, mode);
  ThunkErrnoEnd();
  s->x[0] = static_cast<uint64_t>(static_cast<int64_t>(r));
}
void G64_openat(NdkCpu64State* s, void*) {
  int dirfd = static_cast<int>(s->x[0]);
  const char* path = Ptr<const char>(GuestAddr64(s->x[1]));
  int flags = static_cast<int>(s->x[2]);
  int mode = static_cast<int>(s->x[3]);
  ThunkErrnoBegin();
  std::string full;
  int r = ResolveAtPath(dirfd, path, &full) ? FdOpen(full.c_str(), flags, mode) : -1;
  ThunkErrnoEnd();
  s->x[0] = static_cast<uint64_t>(static_cast<int64_t>(r));
}
int64_t Fcntl64(int fd, int cmd, uint64_t arg) {
  switch (cmd) {
    case 3: {   // F_GETFL
      int r = fcntl(fd, F_GETFL);
      return r < 0 ? r : GuestOpenFlags(r);
    }
    case 4: return fcntl(fd, F_SETFL, HostOpenFlags(static_cast<int>(arg)));
    case 0: case 1: case 2: case 1030:   // F_DUPFD, F_GETFD, F_SETFD, F_DUPFD_CLOEXEC
      return fcntl(fd, cmd, static_cast<int>(arg));
    case 5: case 6: case 7: {   // F_GETLK, F_SETLK, F_SETLKW: struct flock is the same in LP64 and musl
      int r = FcntlLock(fd, cmd == 5 ? 12 : cmd == 6 ? 13 : 14, static_cast<uint32_t>(arg));   // as F_*LK64
      if (r == -2) {
        errno = EINVAL;
        return -1;
      }
      return r;
    }
    default:
      __android_log_print(5, "ndk", "fcntl(%d, %d): unsupported command (arm64)", fd, cmd);
      errno = EINVAL;
      return -1;
  }
}
void G64_fcntl(NdkCpu64State* s, void*) {
  ThunkErrnoBegin();
  int64_t r = Fcntl64(static_cast<int>(s->x[0]), static_cast<int>(s->x[1]), s->x[2]);
  ThunkErrnoEnd();
  s->x[0] = static_cast<uint64_t>(r);
}
void G64_ioctl(NdkCpu64State* s, void*) {
  ThunkErrnoBegin();
  int r = ioctl(static_cast<int>(s->x[0]), static_cast<int>(s->x[1]), Ptr<void>(GuestAddr64(s->x[2])));
  ThunkErrnoEnd();
  s->x[0] = static_cast<uint64_t>(static_cast<int64_t>(r));
}
ssize_t G64_readv(int fd, uint64_t iov, int n) {
  std::vector<struct iovec> v = Iovecs(GuestAddr64(iov), n);
  return readv(fd, v.data(), n);
}
ssize_t G64_writev(int fd, uint64_t iov, int n) {
  std::vector<struct iovec> v = Iovecs(GuestAddr64(iov), n);
  return writev(fd, v.data(), n);
}
ssize_t G64_preadv(int fd, uint64_t iov, int n, off_t off) {
  std::vector<struct iovec> v = Iovecs(GuestAddr64(iov), n);
  ssize_t total = 0;
  for (auto& e : v) {
    ssize_t r = pread(fd, e.iov_base, e.iov_len, off + total);
    if (r < 0) return total ? total : r;
    total += r;
    if ((size_t)r < e.iov_len) break;
  }
  return total;
}
ssize_t G64_pwritev(int fd, uint64_t iov, int n, off_t off) {
  std::vector<struct iovec> v = Iovecs(GuestAddr64(iov), n);
  ssize_t total = 0;
  for (auto& e : v) {
    ssize_t r = pwrite(fd, e.iov_base, e.iov_len, off + total);
    if (r < 0) return total ? total : r;
    total += r;
    if ((size_t)r < e.iov_len) break;
  }
  return total;
}
// The memory calls take the guest's 64-bit addresses and lengths: a large
// reservation lives in a window above 4 GB (GuestMmap64, ndk_libc.cc).
void G64_mmap(NdkCpu64State* s, void*) {
  ThunkErrnoBegin();
  uint64_t r = GuestMmap64(s->x[0], s->x[1], static_cast<int>(s->x[2]), static_cast<int>(s->x[3]),
                           static_cast<int>(s->x[4]), static_cast<int64_t>(s->x[5]));
  ThunkErrnoEnd();
  s->x[0] = r;
}
void G64_munmap(NdkCpu64State* s, void*) {
  ThunkErrnoBegin();
  int r = GuestMunmap64(s->x[0], s->x[1]);
  ThunkErrnoEnd();
  s->x[0] = static_cast<uint64_t>(static_cast<int64_t>(r));
}
// |prot| -1: madvise(|advice|).
int MemCall64(uint64_t addr, uint64_t len, int prot, int advice) {
  std::vector<std::pair<uint32_t, uint32_t>> low;
  if (!GuestMemRange64(addr, len, prot, &low))
    return -1;
  int r = 0;
  for (const auto& part : low) {
    void* p = Ptr<void>(part.first);
    if ((prot < 0 ? GuestMadvise(p, part.second - part.first, advice)
                  : GuestMprotect(p, part.second - part.first, prot)) != 0)
      r = -1;
  }
  return r;
}
void G64_mprotect(NdkCpu64State* s, void*) {
  ThunkErrnoBegin();
  int r = MemCall64(s->x[0], s->x[1], static_cast<int>(s->x[2]), 0);
  ThunkErrnoEnd();
  s->x[0] = static_cast<uint64_t>(static_cast<int64_t>(r));
}
void G64_madvise(NdkCpu64State* s, void*) {
  ThunkErrnoBegin();
  int r = MemCall64(s->x[0], s->x[1], -1, static_cast<int>(s->x[2]));
  ThunkErrnoEnd();
  s->x[0] = static_cast<uint64_t>(static_cast<int64_t>(r));
}
int G64_readdir_r(DIR* d, struct dirent* e, uint64_t* result) {
  struct dirent* r = nullptr;
  int rc = readdir_r(d, e, &r);
  *result = Addr(r);
  return rc;
}
int G64_select(int n, fd_set* r, fd_set* w, fd_set* e, G64Timeval* tv) {
  struct timeval h, *hp = nullptr;
  if (tv) {
    h.tv_sec = tv->tv_sec;
    h.tv_usec = tv->tv_usec;
    hp = &h;
  }
  int rc = select(n, r, w, e, hp);
  if (tv) {
    tv->tv_sec = h.tv_sec;
    tv->tv_usec = h.tv_usec;
  }
  return rc;
}
int G64_pselect(int n, fd_set* r, fd_set* w, fd_set* e, const G64Timespec* ts, const void*) {
  struct timeval h, *hp = nullptr;
  if (ts) {
    h.tv_sec = ts->tv_sec;
    h.tv_usec = ts->tv_nsec / 1000;
    hp = &h;
  }
  return select(n, r, w, e, hp);
}
int G64_ppoll(struct pollfd* fds, uint64_t n, const G64Timespec* ts, const void*) {
  int ms = ts ? static_cast<int>(ts->tv_sec * 1000 + ts->tv_nsec / 1000000) : -1;
  return poll(fds, static_cast<nfds_t>(n), ms);
}

// --- sockets ---------------------------------------------------------------------------------------------
// struct cmsghdr: LP64 { size_t len; int level, type; } (16, aligned 8) against
// the host's { socklen_t len; int level, type; } (12, aligned 4).
size_t GCmsgAlign(size_t n) { return (n + 7) & ~size_t(7); }
size_t HCmsgAlign(size_t n) { return (n + 3) & ~size_t(3); }

struct HostMsg {
  struct msghdr h;
  std::vector<struct iovec> iov;
  std::vector<uint8_t> control;
};
void ToHostMsg(const G64Msghdr* g, HostMsg* m, bool sending) {
  memset(&m->h, 0, sizeof m->h);
  m->h.msg_name = Ptr<void>(GuestAddr64(g->msg_name));
  m->h.msg_namelen = g->msg_namelen;
  m->iov = Iovecs(GuestAddr64(g->msg_iov), static_cast<int>(g->msg_iovlen));
  m->h.msg_iov = m->iov.data();
  m->h.msg_iovlen = static_cast<int>(g->msg_iovlen);
  m->h.msg_flags = g->msg_flags;
  if (g->msg_control && g->msg_controllen) {
    m->control.assign(g->msg_controllen + 64, 0);
    if (sending) {
      // Repack each guest control message.
      uint32_t p = static_cast<uint32_t>(g->msg_control), end = p + static_cast<uint32_t>(g->msg_controllen);
      size_t out = 0;
      while (p + 16 <= end) {
        uint64_t len = Load64(p);
        if (len < 16 || p + len > end) break;
        size_t data = len - 16;
        struct cmsghdr* c = reinterpret_cast<struct cmsghdr*>(&m->control[out]);
        c->cmsg_len = static_cast<socklen_t>(12 + data);
        c->cmsg_level = static_cast<int32_t>(Load32(p + 8));
        c->cmsg_type = static_cast<int32_t>(Load32(p + 12));
        memcpy(reinterpret_cast<uint8_t*>(c) + 12, Ptr<void>(p + 16), data);
        out += HCmsgAlign(12 + data);
        p += static_cast<uint32_t>(GCmsgAlign(len));
      }
      m->h.msg_controllen = static_cast<socklen_t>(out);
    } else {
      m->h.msg_controllen = static_cast<socklen_t>(m->control.size());
    }
    m->h.msg_control = m->control.data();
  }
}
// Received control messages back into the guest's buffer.
void FromHostMsg(const HostMsg& m, G64Msghdr* g) {
  g->msg_namelen = m.h.msg_namelen;
  g->msg_flags = m.h.msg_flags;
  if (!g->msg_control || !g->msg_controllen) return;
  uint32_t p = static_cast<uint32_t>(g->msg_control), cap = static_cast<uint32_t>(g->msg_controllen), out = 0;
  const uint8_t* h = static_cast<const uint8_t*>(m.h.msg_control);
  size_t hp = 0;
  while (h && hp + 12 <= m.h.msg_controllen) {
    const struct cmsghdr* c = reinterpret_cast<const struct cmsghdr*>(h + hp);
    if (c->cmsg_len < 12) break;
    size_t data = c->cmsg_len - 12;
    if (out + 16 + data > cap) {
      g->msg_flags |= MSG_CTRUNC;
      break;
    }
    Store64(p + out, 16 + data);
    Store32(p + out + 8, static_cast<uint32_t>(c->cmsg_level));
    Store32(p + out + 12, static_cast<uint32_t>(c->cmsg_type));
    memcpy(Ptr<void>(p + out + 16), reinterpret_cast<const uint8_t*>(c) + 12, data);
    out += static_cast<uint32_t>(GCmsgAlign(16 + data));
    hp += HCmsgAlign(c->cmsg_len);
  }
  g->msg_controllen = out;
}
ssize_t G64_sendmsg(int fd, const G64Msghdr* g, int flags) {
  HostMsg m;
  ToHostMsg(g, &m, true);
  return sendmsg(fd, &m.h, flags);
}
ssize_t G64_recvmsg(int fd, G64Msghdr* g, int flags) {
  HostMsg m;
  ToHostMsg(g, &m, false);
  ssize_t r = recvmsg(fd, &m.h, flags);
  if (r >= 0) FromHostMsg(m, g);
  return r;
}
int G64_recvmmsg(int fd, G64Mmsghdr* v, unsigned n, int flags, G64Timespec*) {
  unsigned i = 0;
  for (; i < n; i++) {
    ssize_t r = G64_recvmsg(fd, &v[i].hdr, i == 0 ? flags & ~0x10000 /* MSG_WAITFORONE */ : flags | MSG_DONTWAIT);
    if (r < 0) {
      if (i == 0) return -1;
      break;
    }
    v[i].len = static_cast<uint32_t>(r);
  }
  return static_cast<int>(i);
}
int G64_sendmmsg(int fd, G64Mmsghdr* v, unsigned n, int flags) {
  unsigned i = 0;
  for (; i < n; i++) {
    ssize_t r = G64_sendmsg(fd, &v[i].hdr, flags);
    if (r < 0) {
      if (i == 0) return -1;
      break;
    }
    v[i].len = static_cast<uint32_t>(r);
  }
  return static_cast<int>(i);
}
int G64_accept4(int fd, struct sockaddr* addr, socklen_t* len, int flags) {
  int r = accept(fd, addr, len);
  if (r >= 0 && flags) {
    if (flags & 04000 /* SOCK_NONBLOCK */) fcntl(r, F_SETFL, fcntl(r, F_GETFL) | O_NONBLOCK);
    if (flags & 02000000 /* SOCK_CLOEXEC */) fcntl(r, F_SETFD, FD_CLOEXEC);
  }
  return r;
}

int GaiError(int r) {
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
int G64_getaddrinfo(const char* node, const char* service, const G64Addrinfo* hints, uint64_t* res) {
  struct addrinfo h, *hr = nullptr;
  memset(&h, 0, sizeof h);
  if (hints) {
    h.ai_flags = hints->ai_flags;
    h.ai_family = hints->ai_family;
    h.ai_socktype = hints->ai_socktype;
    h.ai_protocol = hints->ai_protocol;
  }
  int r = getaddrinfo(node, service, hints ? &h : nullptr, &hr);
  if (r != 0) return GaiError(r);
  uint64_t head = 0;
  G64Addrinfo* prev = nullptr;
  for (struct addrinfo* p = hr; p; p = p->ai_next) {
    G64Addrinfo* g = static_cast<G64Addrinfo*>(calloc(1, sizeof(G64Addrinfo) + p->ai_addrlen));
    g->ai_flags = p->ai_flags;
    g->ai_family = p->ai_family;
    g->ai_socktype = p->ai_socktype;
    g->ai_protocol = p->ai_protocol;
    g->ai_addrlen = p->ai_addrlen;
    memcpy(g + 1, p->ai_addr, p->ai_addrlen);
    g->ai_addr = Addr(g + 1);
    g->ai_canonname = p->ai_canonname ? Addr(strdup(p->ai_canonname)) : 0;
    if (prev) prev->ai_next = Addr(g);
    else head = Addr(g);
    prev = g;
  }
  freeaddrinfo(hr);
  *res = head;
  return 0;
}
void G64_freeaddrinfo(G64Addrinfo* g) {
  while (g) {
    G64Addrinfo* next = Ptr<G64Addrinfo>(GuestAddr64(g->ai_next));
    free(Ptr<void>(GuestAddr64(g->ai_canonname)));
    free(g);
    g = next;
  }
}

// --- users ------------------------------------------------------------------------------------------------
thread_local G64Passwd t_pw;
G64Passwd* ToPasswd(struct passwd* h) {
  if (!h) return nullptr;
  t_pw.pw_name = Addr(h->pw_name);
  t_pw.pw_passwd = Addr(h->pw_passwd);
  t_pw.pw_uid = h->pw_uid;
  t_pw.pw_gid = h->pw_gid;
  t_pw.pw_dir = Addr(h->pw_dir);
  t_pw.pw_shell = Addr(h->pw_shell);
  return &t_pw;
}
G64Passwd* G64_getpwuid(uint32_t uid) { return ToPasswd(NdkAndroidPasswd(uid)); }
// getgrgid/getgrnam: Android's ids (ndk_libc8.cc), bionic's LP64 struct group.
struct G64Group : GuestLayout64 {
  uint64_t gr_name, gr_passwd;
  uint32_t gr_gid, pad;
  uint64_t gr_mem;
};
G64Group* G64Group_of(uint32_t gid) {
  static thread_local G64Group g;
  static thread_local char name[32];
  static thread_local uint64_t members[1];
  NdkAndroidIdName(gid, name, sizeof(name));
  members[0] = 0;
  g.gr_name = Addr(name);
  g.gr_passwd = 0;
  g.gr_gid = gid;
  g.pad = 0;
  g.gr_mem = Addr(members);
  return &g;
}
G64Group* G64_getgrgid(uint32_t gid) { return G64Group_of(gid); }
G64Group* G64_getgrnam(const char* name) {
  const int64_t id = NdkAndroidIdOf(name);
  return id < 0 ? nullptr : G64Group_of(static_cast<uint32_t>(id));
}
G64Passwd* G64_getpwnam(const char* name) {
  const int64_t id = NdkAndroidIdOf(name);
  return id < 0 ? nullptr : ToPasswd(NdkAndroidPasswd(static_cast<uint32_t>(id)));
}
int G64_getpw_r(uint32_t, void*, char*, size_t, uint64_t* result) {
  *result = 0;
  return 2 /* ENOENT */;
}
int G64_getpwnam_r(const char*, void*, char*, size_t, uint64_t* result) {
  *result = 0;
  return 2 /* ENOENT */;
}

// --- signals: delivered at the guest threads' safe points (shim/ndk/cpu64/signal64.cc) -----------------
// bionic's LP64 struct sigaction: sa_flags (int) at 0, the handler at 8, sa_mask at
// 16, sa_restorer at 24. The kernel's (rt_sigaction): handler 0, flags 8, restorer
// 16, mask 24.
void ToSigaction64(const uint8_t* a, bool kernel, Ndk64Sigaction* out) {
  uint64_t w[4];
  memcpy(w, a, sizeof w);
  out->handler = kernel ? w[0] : w[1];
  out->flags = kernel ? w[1] : static_cast<uint32_t>(w[0]);
  out->mask = kernel ? w[3] : w[2];
}
void FromSigaction64(const Ndk64Sigaction& in, bool kernel, uint8_t* a) {
  uint64_t w[4] = {0, 0, 0, 0};
  if (kernel) {
    w[0] = in.handler;
    w[1] = in.flags;
    w[3] = in.mask;
  } else {
    w[0] = static_cast<uint32_t>(in.flags);
    w[1] = in.handler;
    w[2] = in.mask;
  }
  memcpy(a, w, sizeof w);
}
int Sigaction64(int sig, const void* act, void* oact, bool kernel) {
  Ndk64Sigaction n, o;
  if (act) ToSigaction64(static_cast<const uint8_t*>(act), kernel, &n);
  if (ndk64_sig_action(sig, act ? &n : nullptr, &o) < 0) {
    errno = EINVAL;
    return -1;
  }
  if (oact) FromSigaction64(o, kernel, static_cast<uint8_t*>(oact));
  return 0;
}
int G64_sigaction(int sig, const void* act, void* oact) { return Sigaction64(sig, act, oact, false); }
// signal(): the handler with SA_RESTART (bionic's); the old handler.
uint64_t G64_signal(int sig, uint64_t handler) {
  Ndk64Sigaction n = {handler, 0x10000000 /* SA_RESTART */, 0}, o;
  if (ndk64_sig_action(sig, &n, &o) < 0) {
    errno = EINVAL;
    return ~0ull;   // SIG_ERR
  }
  return o.handler;
}
int G64_sigemptyset(uint64_t* set) { *set = 0; return 0; }
int G64_sigfillset(uint64_t* set) { *set = ~0ull; return 0; }
int G64_sigaddset(uint64_t* set, int sig) {
  if (sig < 1 || sig > 64) { errno = EINVAL; return -1; }
  *set |= 1ull << (sig - 1);
  return 0;
}
int G64_sigdelset(uint64_t* set, int sig) {
  if (sig < 1 || sig > 64) { errno = EINVAL; return -1; }
  *set &= ~(1ull << (sig - 1));
  return 0;
}
int G64_sigismember(const uint64_t* set, int sig) {
  if (sig < 1 || sig > 64) { errno = EINVAL; return -1; }
  return (*set >> (sig - 1)) & 1;
}
int G64_pthread_sigmask(int how, const uint64_t* set, uint64_t* old) {
  uint64_t o = 0;
  if (ndk64_sig_mask(how, set, &o) < 0) return EINVAL;   // an error number
  if (old) *old = o;
  ndk64_sig_deliver();   // what this unblocked arrives now
  return 0;
}
int G64_sigprocmask(int how, const uint64_t* set, uint64_t* old) {
  int r = G64_pthread_sigmask(how, set, old);
  if (r == 0) return 0;
  errno = r;
  return -1;
}
int G64_raise(int sig) {
  if (sig == 6 || sig == 9 || sig == 11) {
    __android_log_print(6, "ndk", "arm64 guest raised signal %d", sig);
    LeaveGuest("raise()");
    abort();
  }
  if (ndk64_sig_raise(sig) < 0) {
    errno = EINVAL;
    return -1;
  }
  return 0;
}

// pthread_cleanup_push/pop (the macros call these): { prev, routine, arg }.
struct G64Cleanup : GuestLayout64 {
  uint64_t prev, routine, arg;
};
thread_local uint64_t t_cleanup64;
void G64_cleanup_push(G64Cleanup* c, uint64_t routine, uint64_t arg) {
  c->prev = t_cleanup64;
  c->routine = routine;
  c->arg = arg;
  t_cleanup64 = Addr(c);
}
void G64_cleanup_pop(G64Cleanup* c, int execute) {
  t_cleanup64 = c->prev;
  if (execute) {
    uint64_t a = c->arg;
    CallGuest64(static_cast<uint32_t>(c->routine), &a, 1);
  }
}

// --- getauxval ------------------------------------------------------------------------------------------
// AT_HWCAP: what the AArch64 CPU runs (FP, ASIMD, CRC32, LSE atomics, LRCPC);
// no AES/PMULL/SHA (BoringSSL and friends then use their portable code).
uint64_t G64_getauxval(uint64_t type) {
  static uint8_t random_bytes[16] = {0x5a, 0x17, 0xc0, 0xde, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
  switch (type) {
    case 16: return (1u << 0) | (1u << 1) | (1u << 7) | (1u << 8) | (1u << 15);   // AT_HWCAP
    case 26: return 0;                                                       // AT_HWCAP2
    case 6: return 4096;                                                     // AT_PAGESZ
    case 15: return Addr("aarch64");                                         // AT_PLATFORM
    case 25: return Addr(random_bytes);                                      // AT_RANDOM
    case 17: return 100;                                                     // AT_CLKTCK
    case 11: case 12: return getuid();                                       // AT_UID, AT_EUID
    case 13: case 14: return getgid();                                       // AT_GID, AT_EGID
    case 23: return 0;                                                       // AT_SECURE
  }
  errno = ENOENT;
  return 0;
}

// --- raw syscalls (arm64 numbers) --------------------------------------------------------------------------
// For syscall() and SVC #0 in guest code. Returns the result or -(bionic errno).
int64_t SysErr() { return -ToGuestErrno(errno); }
int64_t SysRet(int64_t r) { return r < 0 ? SysErr() : r; }

int64_t DoFutex64(uint32_t addr, int op, uint32_t val, uint32_t timeout) {
  int cmd = op & 0x7f;   // without FUTEX_PRIVATE_FLAG / FUTEX_CLOCK_REALTIME
  if (cmd == 0 || cmd == 9) {   // FUTEX_WAIT, FUTEX_WAIT_BITSET
    double ms = INFINITY;
    if (timeout) {
      const G64Timespec* ts = Ptr<const G64Timespec>(timeout);
      ms = ts->tv_sec * 1000.0 + ts->tv_nsec / 1e6;
      if (cmd == 9) {   // an absolute time
        struct timespec now;
        clock_gettime((op & 256) ? CLOCK_REALTIME : CLOCK_MONOTONIC, &now);
        ms -= now.tv_sec * 1000.0 + now.tv_nsec / 1e6;
        if (ms < 0) ms = 0;
      }
    }
    // (a guest signal's handler runs inside the wait: -EINTR then, as on Linux)
    int r = GuestFutexWait(Ptr<void>(addr), val, ms);
    return r == -110 || r == -11 || r == -4 ? r : 0;
  }
  if (cmd == 1 || cmd == 10) {   // FUTEX_WAKE, FUTEX_WAKE_BITSET
    int r = GuestFutexWake(Ptr<void>(addr), static_cast<int>(val));
    return r < 0 ? 0 : r;
  }
  return -38;
}

int64_t Syscall64(uint64_t nr, const uint64_t* a) {
  auto P = [&](int i) { return Ptr<void>(GuestAddr64(a[i])); };
  auto S = [&](int i) { return Ptr<const char>(GuestAddr64(a[i])); };
  auto I = [&](int i) { return static_cast<int>(a[i]); };
  errno = 0;
  switch (nr) {
    case 63: return SysRet(FdRead(I(0), P(1), static_cast<size_t>(a[2])));
    case 64: return SysRet(FdWrite(I(0), P(1), static_cast<size_t>(a[2])));
    case 56: {   // openat
      std::string full;
      if (!ResolveAtPath(I(0), S(1), &full)) return SysErr();
      return SysRet(FdOpen(full.c_str(), I(2), I(3)));
    }
    case 57: return SysRet(FdClose(I(0)));
    case 62: return SysRet(lseek(I(0), static_cast<off_t>(a[1]), I(2)));
    case 67: return SysRet(pread(I(0), P(1), static_cast<size_t>(a[2]), static_cast<off_t>(a[3])));
    case 68: return SysRet(pwrite(I(0), P(1), static_cast<size_t>(a[2]), static_cast<off_t>(a[3])));
    case 65: return SysRet(G64_readv(I(0), a[1], I(2)));
    case 66: return SysRet(G64_writev(I(0), a[1], I(2)));
    case 29: return SysRet(ioctl(I(0), I(1), P(2)));
    case 25: return SysRet(Fcntl64(I(0), I(1), a[2]));
    case 23: return SysRet(dup(I(0)));
    case 24: return SysRet(dup3(I(0), I(1), HostOpenFlags(I(2))));
    case 59: return SysRet(pipe2(static_cast<int*>(P(0)), HostOpenFlags(I(1))));
    case 79: return SysRet(G64_fstatat(I(0), S(1), static_cast<G64Stat*>(P(2)), I(3)));
    case 80: return SysRet(G64_fstat(I(0), static_cast<G64Stat*>(P(1))));
    case 61: {   // getdents64: the layout (linux_dirent64) is the same as on ARM
      uint32_t args[6] = {static_cast<uint32_t>(a[0]), GuestAddr64(a[1]), static_cast<uint32_t>(a[2])};
      int32_t r;
      if (RawSyscall(217, args, &r)) return r;
      return -38;
    }
    case 78: case 48: case 34: case 35: case 38: case 53: case 54: case 36: case 37: {
      // readlinkat, faccessat, mkdirat, unlinkat, renameat, fchmodat, fchownat, symlinkat, linkat:
      // the ARM numbers take the same arguments.
      // |ptrs|: which arguments are pointers (paths, readlinkat's buffer).
      static const struct { uint32_t a64, arm, ptrs; } kMap[] = {
          {78, 332, 6}, {48, 334, 2}, {34, 323, 2}, {35, 328, 2}, {38, 329, 10},
          {53, 333, 2}, {54, 325, 2}, {36, 331, 5}, {37, 330, 10}};
      for (auto& m : kMap) {
        if (m.a64 != nr) continue;
        uint32_t args[6];
        for (int i = 0; i < 6; i++) args[i] = (m.ptrs >> i & 1) ? GuestAddr64(a[i]) : static_cast<uint32_t>(a[i]);
        int32_t r;
        if (RawSyscall(m.arm, args, &r)) return r;
      }
      return -38;
    }
    case 17: return getcwd(static_cast<char*>(P(0)), static_cast<size_t>(a[1])) ? static_cast<int64_t>(strlen(S(0)) + 1) : SysErr();
    case 46: return SysRet(ftruncate(I(0), static_cast<off_t>(a[1])));
    case 82: return SysRet(fsync(I(0)));
    case 83: return SysRet(fdatasync(I(0)));
    case 222: {   // mmap (offset in bytes)
      uint64_t r = GuestMmap64(a[0], a[1], I(2), I(3), I(4), static_cast<int64_t>(a[5]));
      return r == ~0ull ? SysErr() : static_cast<int64_t>(r);
    }
    case 215: return SysRet(GuestMunmap64(a[0], a[1]));
    case 226: {   // mprotect: no protection in wasm; executable code is noted
      std::vector<std::pair<uint32_t, uint32_t>> low;
      if (!GuestMemRange64(a[0], a[1], I(2), &low))
        return SysErr();
      for (const auto& part : low) {
        // (GuestMprotect: what the mmap allocator keeps to zero once accessible.)
        GuestMprotect(Ptr<void>(part.first), part.second - part.first, I(2));
        if (a[2] & PROT_EXEC)
          NoteExecRegion64(part.first, part.second);
      }
      return 0;
    }
    case 233: case 227: case 228: case 229: return 0;   // madvise, msync, mlock, munlock
    case 232:     // mincore: every page resident (Go's page-size probe, as ndk_syscall.cc)
      if (a[0] & 4095) return -ToGuestErrno(EINVAL);
      memset(P(2), 1, (a[1] + 4095) / 4096);
      return 0;
    case 98: return DoFutex64(GuestAddr64(a[0]), I(1), static_cast<uint32_t>(a[2]), a[3] ? GuestAddr64(a[3]) : 0);
    case 101: return G64_nanosleep(static_cast<const G64Timespec*>(P(0)), static_cast<G64Timespec*>(P(1))) == 0 ? 0 : SysErr();
    case 113: return G64_clock_gettime(I(0), static_cast<G64Timespec*>(P(1))) == 0 ? 0 : SysErr();
    case 114: return G64_clock_getres(I(0), static_cast<G64Timespec*>(P(1))) == 0 ? 0 : SysErr();
    case 115: return -G64_clock_nanosleep(I(0), I(1), static_cast<const G64Timespec*>(P(2)), static_cast<G64Timespec*>(P(3)));
    case 169: return G64_gettimeofday(static_cast<G64Timeval*>(P(0)), P(1)) == 0 ? 0 : SysErr();
    case 124: return sched_yield();
    case 122: return 0;   // sched_setaffinity: any mask is fine
    case 123: {           // sched_getaffinity: GuestCpuCount CPUs
      size_t n = static_cast<size_t>(a[1]);
      if (n < 8) return -22;
      GuestCpuMask(P(2), n);
      return 8;
    }
    case 172: return getpid();
    case 173: return getppid();
    case 174: return getuid();
    case 175: return geteuid();
    case 176: return getgid();
    case 177: return getegid();
    case 178: case 96: return GuestTid();   // gettid, set_tid_address (a helper running a handler: that thread)
    case 168: {   // getcpu
      if (a[0]) Store32(GuestAddr64(a[0]), 0);
      if (a[1]) Store32(GuestAddr64(a[1]), 0);
      return 0;
    }
    case 129: case 130: case 131: {   // kill, tkill, tgkill
      int sig = I(nr == 131 ? 2 : 1);
      int tid = nr == 129 ? GuestTid() : I(nr == 131 ? 1 : 0);   // kill: this process, this thread
      if (tid == GuestTid() && (sig == 6 || sig == 9 || sig == 11)) return G64_raise(sig);
      if (tid == GuestTid()) return ndk64_sig_raise(sig) < 0 ? -22 : 0;
      int r = ndk_sig_send ? ndk_sig_send(tid, sig) : -3;
      return r < 0 ? r : 0;
    }
    case 134: {   // rt_sigaction
      return Sigaction64(I(0), a[1] ? P(1) : nullptr, a[2] ? P(2) : nullptr, true) == 0 ? 0 : -22;
    }
    case 135: {   // rt_sigprocmask
      return G64_pthread_sigmask(I(0), a[1] ? static_cast<const uint64_t*>(P(1)) : nullptr,
                                 a[2] ? static_cast<uint64_t*>(P(2)) : nullptr) == 0 ? 0 : -22;
    }
    case 133: {   // rt_sigsuspend
      uint64_t mask = Load64(GuestAddr64(a[0]));
      return GuestSigsuspend(&mask) == kParked ? 0 : -4;
    }
    case 132: return GuestSigaltstack64(a[0] ? GuestAddr64(a[0]) : 0, a[1] ? GuestAddr64(a[1]) : 0);
    case 167: {   // prctl: names and dumpability are kept nowhere
      uint32_t args[6];
      for (int i = 0; i < 6; i++) args[i] = static_cast<uint32_t>(a[i]);
      if (a[0] == 15 || a[0] == 16) args[1] = GuestAddr64(a[1]);   // PR_SET_NAME, PR_GET_NAME
      int32_t r;
      if (RawSyscall(172, args, &r)) return r;
      return 0;
    }
    case 279: return SysRet(GuestMemfdCreate(static_cast<const char*>(P(0)), I(1)));   // memfd_create
    case 270: case 271: {   // process_vm_readv, process_vm_writev
      // Of the guest's own process (crash reporters and memory probes read
      // their memory "safely" this way): a copy, EFAULT at the first remote
      // range that is not memory. {base, len} iovecs of 16 bytes.
      if (static_cast<int>(a[0]) != getpid()) return -3;   // ESRCH
      const bool write = nr == 271;
      uint64_t done = 0, li = 0, loff = 0;
      for (uint64_t ri = 0; ri < a[4]; ri++) {
        const uint32_t riov = GuestAddr64(a[3]) + 16 * static_cast<uint32_t>(ri);
        uint64_t rbase = Load64(riov), rlen = Load64(riov + 8);
        if (rlen && !GuestHasMemory64(rbase, rlen)) return done ? static_cast<int64_t>(done) : -14;   // EFAULT
        for (uint64_t roff = 0; roff < rlen && li < a[2];) {
          const uint32_t liov = GuestAddr64(a[1]) + 16 * static_cast<uint32_t>(li);
          const uint64_t lbase = Load64(liov), llen = Load64(liov + 8);
          const uint64_t n = std::min(rlen - roff, llen - loff);
          char* l = Ptr<char>(lbase + loff);
          char* r = Ptr<char>(rbase + roff);
          if (n) memmove(write ? r : l, write ? l : r, static_cast<size_t>(n));
          roff += n;
          loff += n;
          done += n;
          if (loff == llen) {
            li++;
            loff = 0;
          }
        }
      }
      return static_cast<int64_t>(done);
    }
    case 278: {   // getrandom
      uint32_t args[6] = {GuestAddr64(a[0]), static_cast<uint32_t>(a[1]), static_cast<uint32_t>(a[2])};
      int32_t r;
      if (RawSyscall(384, args, &r)) return r;
      return -38;
    }
    case 19: case 20: case 21: case 22: case 26: case 27: case 28: {
      // eventfd2, epoll_create1, epoll_ctl, epoll_pwait, inotify_init1, inotify_add_watch,
      // inotify_rm_watch: as the ARM numbers (struct epoll_event is 16 bytes on both).
      static const struct { uint32_t a64, arm; } kMap[] = {{19, 356}, {20, 357}, {21, 251}, {22, 346},
                                                            {26, 360}, {27, 317}, {28, 318}};
      uint32_t arm = 0;
      for (auto& m : kMap) if (m.a64 == nr) arm = m.arm;
      uint32_t args[6];
      for (int i = 0; i < 6; i++) args[i] = static_cast<uint32_t>(a[i]);
      if (nr == 21) args[3] = a[3] ? GuestAddr64(a[3]) : 0;
      if (nr == 22) {
        args[1] = a[1] ? GuestAddr64(a[1]) : 0;
        args[4] = a[4] ? GuestAddr64(a[4]) : 0;
      }
      if (nr == 27) args[1] = GuestAddr64(a[1]);
      int r;
      if (!FdSyscall(arm, args, &r)) return -38;
      return SysRet(r);
    }
    // Sockets, as the libc thunks (Go's net package issues these itself).
    case 198: return SysRet(socket(I(0), I(1), I(2)));
    case 199: return SysRet(socketpair(I(0), I(1), I(2), static_cast<int*>(P(3))));
    case 200: return SysRet(bind(I(0), static_cast<const struct sockaddr*>(P(1)), static_cast<socklen_t>(a[2])));
    case 201: return SysRet(listen(I(0), I(1)));
    case 202: return SysRet(accept(I(0), static_cast<struct sockaddr*>(P(1)), static_cast<socklen_t*>(P(2))));
    case 242: return SysRet(G64_accept4(I(0), static_cast<struct sockaddr*>(P(1)), static_cast<socklen_t*>(P(2)), I(3)));
    case 203: return SysRet(connect(I(0), static_cast<const struct sockaddr*>(P(1)), static_cast<socklen_t>(a[2])));
    case 204: return SysRet(getsockname(I(0), static_cast<struct sockaddr*>(P(1)), static_cast<socklen_t*>(P(2))));
    case 205: return SysRet(getpeername(I(0), static_cast<struct sockaddr*>(P(1)), static_cast<socklen_t*>(P(2))));
    case 206:
      return SysRet(sendto(I(0), P(1), static_cast<size_t>(a[2]), I(3), static_cast<const struct sockaddr*>(P(4)),
                           static_cast<socklen_t>(a[5])));
    case 207:
      return SysRet(recvfrom(I(0), P(1), static_cast<size_t>(a[2]), I(3), static_cast<struct sockaddr*>(P(4)),
                             static_cast<socklen_t*>(P(5))));
    case 208: return SysRet(setsockopt(I(0), I(1), I(2), P(3), static_cast<socklen_t>(a[4])));
    case 209: return SysRet(getsockopt(I(0), I(1), I(2), P(3), static_cast<socklen_t*>(P(4))));
    case 210: return SysRet(shutdown(I(0), I(1)));
    case 211: return SysRet(G64_sendmsg(I(0), static_cast<const G64Msghdr*>(P(1)), I(2)));
    case 212: return SysRet(G64_recvmsg(I(0), static_cast<G64Msghdr*>(P(1)), I(2)));
    case 283: return 0;   // membarrier: no commands (MEMBARRIER_CMD_QUERY)
    case 160: return SysRet(uname(static_cast<struct utsname*>(P(0))));
    case 163: return SysRet(GuestPrlimit64(I(0), 0, a[1] ? GuestAddr64(a[1]) : 0));  // getrlimit
    case 261:   // prlimit64(pid, resource, new, old): struct rlimit is 2 x 64 bits (ndk_bionic.cc's limits)
      return SysRet(GuestPrlimit64(I(1), a[2] ? GuestAddr64(a[2]) : 0, a[3] ? GuestAddr64(a[3]) : 0));
    case 164: return 0;   // setrlimit
    case 214: return 0;   // brk: no heap break
    case 220: case 221: case 260:   // clone, execve, wait4
      return nr == 260 ? -10 /* ECHILD */ : -38;
    case 93: case 94:
      fflush(stdout);
      exit(I(0));
  }
  return -38 - 1000;   // unknown: the caller decides
}

// The CPU's raw-syscall hook: SVC #0 in guest code.
int SvcHandler64(NdkCpu64State* s) {
  int64_t r = Syscall64(s->x[8], s->x);
  if (r == -38 - 1000) return 0;   // the CPU's own table, then -ENOSYS with a message
  s->x[0] = static_cast<uint64_t>(r);
  return 1;
}

// syscall(nr, ...): -1 with errno on failure.
void G64_syscall(NdkCpu64State* s, void*) {
  uint64_t a[6] = {s->x[1], s->x[2], s->x[3], s->x[4], s->x[5], s->x[6]};
  int64_t r = Syscall64(s->x[0], a);
  if (r == -38 - 1000) {
    __android_log_print(5, "ndk", "syscall(%llu): unsupported (arm64)", (unsigned long long)s->x[0]);
    r = -38;
  }
  if (r < 0 && r > -4096) {
    *GuestErrno() = static_cast<int>(-r);
    s->x[0] = ~0ull;
  } else {
    s->x[0] = static_cast<uint64_t>(r);
  }
}

uint64_t g_ctype64, g_tolower64, g_toupper64;

const HostSymbol kLibc64[] = {
    // strings and stdlib
    NDK_FN64("strtok_r", G64_strtok_r), NDK_FN64("strsep", G64_strsep),
    NDK_DATA64("_ctype_", Addr(&g_ctype64)), NDK_DATA64("_tolower_tab_", Addr(&g_tolower64)),
    NDK_DATA64("_toupper_tab_", Addr(&g_toupper64)),
    NDK_FN64("posix_memalign", G64_posix_memalign), NDK_FN64("aligned_alloc", G64_aligned_alloc),
    NDK_FN64("atol", G64_atol), NDK_FN64("labs", G64_labs), NDK_FN64("strtol", G64_strtol),
    NDK_FN64("strtoul", G64_strtoul), NDK_FN64("strtoll", G64_strtol), NDK_FN64("strtoull", G64_strtoul),
    NDK_FN64("strtoimax", G64_strtol), NDK_FN64("strtoumax", G64_strtoul), NDK_FN64("strtoq", G64_strtol),
    NDK_FN64("strtouq", G64_strtoul), NDK_FN64("strtod", G64_strtod), NDK_FN64("strtof", G64_strtof),
    NDK_FN64("strtold", G64_strtold),
    NDK_RAW64("div", G64_div), NDK_RAW64("ldiv", G64_ldiv), NDK_RAW64("lldiv", G64_ldiv),
    NDK_RAW64("imaxdiv", G64_ldiv),
    NDK_FN64("qsort", G64_qsort), NDK_FN64("bsearch", G64_bsearch),
    NDK_FN64("exit", G64_exit), NDK_FN64("atexit", G64_atexit), NDK_FN64("__cxa_atexit", G64_cxa_atexit),
    NDK_FN64("__cxa_finalize", G64_cxa_finalize), NDK_FN64("__cxa_thread_atexit_impl", G64_cxa_thread_atexit_impl),
    NDK_DATA64("__stack_chk_guard", Addr(&g_stack_chk_guard64)),
    NDK_RAW64("setjmp", G64_setjmp), NDK_RAW64("_setjmp", G64_setjmp), NDK_RAW64("sigsetjmp", G64_setjmp),
    NDK_RAW64("longjmp", G64_longjmp), NDK_RAW64("_longjmp", G64_longjmp), NDK_RAW64("siglongjmp", G64_longjmp),
    // time (time_t is 64 bits on both sides)
    NDK_FN64("time", ::time), NDK_FN64("difftime", ::difftime), NDK_FN64("ctime", ::ctime),
    NDK_FN64("ctime_r", ::ctime_r),
    NDK_FN64("gettimeofday", G64_gettimeofday), NDK_FN64("clock_gettime", G64_clock_gettime),
    NDK_FN64("clock_getres", G64_clock_getres), NDK_FN64("nanosleep", G64_nanosleep),
    NDK_FN64("clock_nanosleep", G64_clock_nanosleep), NDK_FN64("clock", G64_clock),
    NDK_FN64("gmtime", G64_gmtime), NDK_FN64("gmtime_r", G64_gmtime_r), NDK_FN64("localtime", G64_localtime),
    NDK_FN64("localtime_r", G64_localtime_r), NDK_FN64("mktime", G64_mktime), NDK_FN64("timegm", G64_timegm),
    NDK_FN64("timelocal", G64_mktime), NDK_FN64("strftime", G64_strftime), NDK_FN64("asctime", G64_asctime),
    NDK_FN64("strptime", G64_strptime), NDK_FN64("utime", ::utime), NDK_FN64("utimes", G64_utimes),
    NDK_FN64("utimensat", G64_utimensat), NDK_FN64("futimens", G64_futimens),
    // files (off_t is 64 bits on both sides)
    NDK_RAW64("open", G64_open), NDK_RAW64("open64", G64_open), NDK_RAW64("__open_2", G64_open),
    NDK_RAW64("openat", G64_openat), NDK_RAW64("openat64", G64_openat), NDK_RAW64("__openat_2", G64_openat),
    NDK_RAW64("fcntl", G64_fcntl), NDK_RAW64("ioctl", G64_ioctl),
    NDK_FN64("lseek", ::lseek), NDK_FN64("lseek64", ::lseek), NDK_FN64("pread", ::pread),
    NDK_FN64("pwrite", ::pwrite), NDK_FN64("pread64", ::pread), NDK_FN64("pwrite64", ::pwrite),
    NDK_FN64("ftruncate", ::ftruncate), NDK_FN64("ftruncate64", ::ftruncate), NDK_FN64("truncate", ::truncate),
    NDK_FN64("truncate64", ::truncate),
    NDK_FN64("readv", G64_readv), NDK_FN64("writev", G64_writev), NDK_FN64("preadv", G64_preadv),
    NDK_FN64("pwritev", G64_pwritev), NDK_FN64("preadv64", G64_preadv), NDK_FN64("pwritev64", G64_pwritev),
    NDK_FN64("stat", G64_stat), NDK_FN64("lstat", G64_lstat), NDK_FN64("fstat", G64_fstat),
    NDK_FN64("stat64", G64_stat), NDK_FN64("lstat64", G64_lstat), NDK_FN64("fstat64", G64_fstat),
    NDK_FN64("fstatat", G64_fstatat), NDK_FN64("fstatat64", G64_fstatat),
    NDK_RAW64("mmap", G64_mmap), NDK_RAW64("mmap64", G64_mmap), NDK_RAW64("munmap", G64_munmap),
    NDK_RAW64("mprotect", G64_mprotect), NDK_RAW64("madvise", G64_madvise),
    NDK_FN64("readdir_r", G64_readdir_r), NDK_FN64("readdir64", ::readdir), NDK_FN64("readdir64_r", G64_readdir_r),
    NDK_FN64("select", G64_select), NDK_FN64("pselect", G64_pselect), NDK_FN64("ppoll", G64_ppoll),
    // sockets
    NDK_FN64("sendmsg", G64_sendmsg), NDK_FN64("recvmsg", G64_recvmsg), NDK_FN64("recvmmsg", G64_recvmmsg),
    NDK_FN64("sendmmsg", G64_sendmmsg), NDK_FN64("accept4", G64_accept4),
    NDK_FN64("getaddrinfo", G64_getaddrinfo), NDK_FN64("freeaddrinfo", G64_freeaddrinfo),
    // users, signals, process
    NDK_FN64("getpwuid", G64_getpwuid), NDK_FN64("getpwnam", G64_getpwnam), NDK_FN64("getpwuid_r", G64_getpw_r),
    NDK_FN64("getgrgid", G64_getgrgid), NDK_FN64("getgrnam", G64_getgrnam),
    NDK_FN64("getgrgid_r", G64_getpw_r), NDK_FN64("getpwnam_r", G64_getpwnam_r), NDK_FN64("getgrnam_r", G64_getpwnam_r),
    NDK_FN64("sigaction", G64_sigaction), NDK_FN64("signal", G64_signal), NDK_FN64("bsd_signal", G64_signal),
    NDK_FN64("sigemptyset", G64_sigemptyset), NDK_FN64("sigfillset", G64_sigfillset),
    NDK_FN64("sigaddset", G64_sigaddset), NDK_FN64("sigdelset", G64_sigdelset),
    NDK_FN64("sigismember", G64_sigismember), NDK_FN64("sigprocmask", G64_sigprocmask),
    NDK_FN64("pthread_sigmask", G64_pthread_sigmask), NDK_FN64("raise", G64_raise),
    NDK_FN64("__pthread_cleanup_push", G64_cleanup_push), NDK_FN64("__pthread_cleanup_pop", G64_cleanup_pop),
    NDK_FN64("getauxval", G64_getauxval), NDK_RAW64("syscall", G64_syscall),
};

}  // namespace

// GuestProcessArgv (ndk_bionic.cc) for arm64 guests: 64-bit words; envp is
// argv + 16.
uint32_t GuestProcessArgv64() {
  static uint32_t argv = [] {
    static const uint64_t kAux[] = {6, 16, 26, 15, 25, 17, 11, 12, 13, 14, 23};
    const size_t naux = sizeof kAux / sizeof kAux[0];
    size_t envc = 0;
    while (::environ && ::environ[envc]) envc++;
    uint64_t* b = static_cast<uint64_t*>(calloc(2 + envc + 1 + 2 * (naux + 1), 8));
    size_t i = 0;
    b[i++] = Addr("app_process");
    b[i++] = 0;
    for (size_t e = 0; e < envc; e++) b[i++] = Addr(::environ[e]);
    b[i++] = 0;
    for (uint64_t t : kAux) {
      b[i++] = t;
      b[i++] = G64_getauxval(t);
    }
    return Addr(b);
  }();
  return argv;
}

void RegisterLibc64() {
  // The ctype table pointers: the same tables as the 32-bit guests'.
  if (uint32_t a = HostDataSymbol("_ctype_")) g_ctype64 = Load32(a);
  if (uint32_t a = HostDataSymbol("_tolower_tab_")) g_tolower64 = Load32(a);
  if (uint32_t a = HostDataSymbol("_toupper_tab_")) g_toupper64 = Load32(a);
  uint64_t guard = 0;
  int fd = open("/dev/urandom", O_RDONLY);
  if (fd >= 0) {
    if (read(fd, &guard, sizeof guard) != sizeof guard) guard = 0;
    close(fd);
  }
  g_stack_chk_guard64 = (guard ? guard : 0x5a17c0de5a17c0deull ^ Addr(&guard)) & ~0xffull;
  RegisterHostSymbols("libc.so", kLibc64);
  ndk64_set_syscall_handler(SvcHandler64);
}

}  // namespace ndk

// Keeps this file in the link (-Wl,-u,ndk64_libc_linked, mk/topic-ndk-arm64.mk): the
// shared bridge files refer to it only weakly.
extern "C" int ndk64_libc_linked = 1;
