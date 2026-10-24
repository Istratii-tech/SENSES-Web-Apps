// bionic libc/libm functions of API levels 21-27 (Android 5.0-8.1) the
// bridge did not have yet: what NDK code built for minSdk 21-27 imports
// (tests/ndk/android81-ndk-symbols.txt has the levels; topic fw8-system,
// docs/topics/fw8-system.md). Where bionic's types and the host's agree the
// export is an alias of an existing thunk (foo64 of foo: off_t and off64_t
// paths that bionic routes to the same code; the long double math of ARM,
// where long double is double); the rest is here. What has no meaning in
// one wasm process (SysV IPC, ttys, namespaces, signals queued between
// processes) fails with ENOSYS, as a kernel without it. Part of the arc-wasm
// shim layer.

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <search.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <uchar.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>

#include "ndk_bridge.h"

extern "C" int property_get(const char* key, char* value, const char* default_value);

namespace ndk {
namespace {

int Enosys() {
  errno = ENOSYS;
  return -1;
}

// --- strings, stdio on fds ---------------------------------------------------------------

int Dprintf(int fd, const char* fmt, ArgSource& args) {
  char* text = GuestFormatAlloc(fmt, args);
  int n = (int)strlen(text);
  int r = (int)FdWrite(fd, text, n);
  free(text);
  return r < 0 ? -1 : n;
}
void G_dprintf(NdkCpuState* s, void*) {
  RegArgs a(s, 2);
  SetResult(s, Dprintf((int)s->r[0], Ptr<const char>(s->r[1]), a));
}
int G_vdprintf(int fd, const char* fmt, uint32_t ap) {
  VaArgs a(ap);
  return Dprintf(fd, fmt, a);
}
wchar_t* G_wmempcpy(wchar_t* d, const wchar_t* s, size_t n) { return wmemcpy(d, s, n) + n; }
char* G_strerror_l(int e, void*) { return ::strerror(ToHostErrno(e)); }
// API 23 GNU basename: the part after the last '/' (no modification).
const char* G_gnu_basename(const char* path) {
  const char* slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}
int G_tolower_raw(int c) { return c - 'A' + 'a'; }
int G_toupper_raw(int c) { return c - 'a' + 'A'; }

// --- process, program name -----------------------------------------------------------
const char* g_progname = "app_process";
const char* G_getprogname() { return g_progname; }
void G_setprogname(const char* name) {
  const char* slash = name ? strrchr(name, '/') : nullptr;
  g_progname = slash ? slash + 1 : name;
}
// The browser's cores (as /proc and sysconf say), the wasm heap as memory.
int G_get_nprocs() { return GuestCpuCount(); }
int32_t G_get_phys_pages() { return (int32_t)(4096LL * 1024 * 1024 / 4096); }
int32_t G_get_avphys_pages() { return (int32_t)(2048LL * 1024 * 1024 / 4096); }
void G_quick_exit(int status) { _Exit(status); }
int G_at_quick_exit(uint32_t) { return 0; }  // never called: quick_exit does not run them
int G_mallopt(int, int) { return 1; }

// C++ thread_local destructors (__cxa_thread_atexit_impl, API 23): the
// objects are kept; their destructors do not run when the thread exits
// (a leak per exiting thread, as bionic before API 23 had).
int G_cxa_thread_atexit_impl(uint32_t, uint32_t, uint32_t) { return 0; }

// API 27 Control Flow Integrity runtime: every check passes.
void G_cfi_slowpath(uint64_t, uint32_t) {}
void G_cfi_slowpath_diag(uint64_t, uint32_t, uint32_t) {}
uint32_t G_cfi_shadow_size() { return 0; }

// --- floating-point environment: wasm rounds to nearest, raises nothing ----------------
int G_fe_zero() { return 0; }
int G_fe_zero_p(void*) { return 0; }
int G_fe_zero_i(int) { return 0; }
int G_fe_zero_pi(void*, int) { return 0; }
int G_fesetround(int r) { return r == 0 ? 0 : -1; }
int G_feenableexcept(int e) { return e ? -1 : 0; }

// bionic's fpclassify constants (FP_INFINITE 0x01 ... FP_ZERO 0x10).
int GuestFpclass(int c) {
  switch (c) {
    case FP_INFINITE: return 0x01;
    case FP_NAN: return 0x02;
    case FP_NORMAL: return 0x04;
    case FP_SUBNORMAL: return 0x08;
    default: return 0x10;  // FP_ZERO
  }
}
int G_fpclassify(double d) { return GuestFpclass(fpclassify(d)); }
int G_isfinite(double d) { return isfinite(d) ? 1 : 0; }
int G_isfinitef(float f) { return isfinite(f) ? 1 : 0; }
int G_isnormal(double d) { return isnormal(d) ? 1 : 0; }
int G_isnormalf(float f) { return isnormal(f) ? 1 : 0; }
int G_isinf(double d) { return isinf(d) ? 1 : 0; }
int G_isnan(double d) { return isnan(d) ? 1 : 0; }
double G_significand(double d) { return scalbn(d, -ilogb(d)); }

// --- sockets, interfaces -------------------------------------------------------------

// sendmmsg: sendmsg each (bionic's and musl's mmsghdr agree, see recvmmsg).
struct GMmsghdr {
  struct msghdr hdr;
  uint32_t len;
};
int G_sendmmsg(int fd, GMmsghdr* v, unsigned n, int flags) {
  unsigned i = 0;
  for (; i < n; i++) {
    ssize_t r = sendmsg(fd, &v[i].hdr, flags);
    if (r < 0) {
      if (i == 0)
        return -1;
      break;
    }
    v[i].len = (uint32_t)r;
  }
  return (int)i;
}

// ppoll: struct pollfd agrees; bionic's timespec has a 32-bit time_t. The
// signal mask argument is not applied.
struct GTimespec {
  int32_t tv_sec, tv_nsec;
};
// Guest signals interrupt it (EINTR), as poll.
int G_ppoll(struct pollfd* fds, nfds_t n, const GTimespec* ts, const void*) {
  int ms = ts ? (int)(ts->tv_sec * 1000 + ts->tv_nsec / 1000000) : -1;
  return GuestPoll(fds, n, ms);
}
int G_ppoll_chk(struct pollfd* fds, nfds_t n, const GTimespec* ts, const void* mask,
                size_t fds_size) {
  if (fds_size / sizeof(struct pollfd) < n)
    FortifyFail("ppoll: %u-element pollfd array too small for %u", (unsigned)(fds_size / 8),
                (unsigned)n);
  return G_ppoll(fds, n, ts, mask);
}

// getifaddrs (API 24): the kernel's two interfaces, as net_interfaces.cc
// and the connectivity service describe them (lo, and eth0 10.0.2.15/24).
// bionic's struct ifaddrs: next, name, flags, addr, netmask, broadaddr, data.
struct GIfaddrs {
  uint32_t next, name, flags, addr, netmask, broadaddr, data;
};
struct IfaddrsBlock {
  GIfaddrs ifa[2];
  struct sockaddr_in addr[2], mask[2], bcast[2];
  char names[2][8];
};
int G_getifaddrs(uint32_t* out) {
  IfaddrsBlock* b = static_cast<IfaddrsBlock*>(calloc(1, sizeof(IfaddrsBlock)));
  if (!b) {
    errno = ENOMEM;  // (out of memory: no list, as bionic)
    return -1;
  }
  const char* names[2] = {"lo", "eth0"};
  const char* addrs[2] = {"127.0.0.1", "10.0.2.15"};
  const char* masks[2] = {"255.0.0.0", "255.255.255.0"};
  const char* bcasts[2] = {"127.255.255.255", "10.0.2.255"};
  const uint32_t flags[2] = {0x1 | 0x8 | 0x40 /* UP LOOPBACK RUNNING */,
                             0x1 | 0x2 | 0x40 | 0x1000 /* UP BROADCAST RUNNING MULTICAST */};
  for (int i = 0; i < 2; i++) {
    strcpy(b->names[i], names[i]);
    for (struct sockaddr_in* sa : {&b->addr[i], &b->mask[i], &b->bcast[i]})
      sa->sin_family = 2;  // AF_INET (bionic's number; musl's is the same)
    inet_aton(addrs[i], &b->addr[i].sin_addr);
    inet_aton(masks[i], &b->mask[i].sin_addr);
    inet_aton(bcasts[i], &b->bcast[i].sin_addr);
    b->ifa[i] = {i == 0 ? Addr(&b->ifa[1]) : 0, Addr(b->names[i]), flags[i], Addr(&b->addr[i]),
                 Addr(&b->mask[i]), i == 0 ? 0 : Addr(&b->bcast[i]), 0};
  }
  *out = Addr(&b->ifa[0]);
  return 0;
}
void G_freeifaddrs(void* p) { free(p); }  // the block starts with the first entry

// if_nameindex: { index, name } pairs ending in { 0, NULL }.
struct GIfNameIndex {
  uint32_t index, name;
};
GIfNameIndex* G_if_nameindex() {
  static const char* const names[] = {"lo", "eth0"};
  GIfNameIndex* v = static_cast<GIfNameIndex*>(calloc(3, sizeof(GIfNameIndex)));
  if (!v) {
    errno = ENOBUFS;
    return nullptr;
  }
  for (int i = 0; i < 2; i++)
    v[i] = {(uint32_t)i + 1, Addr(names[i])};
  return v;
}
void G_if_freenameindex(void* p) { free(p); }

// --- files ------------------------------------------------------------------------------
ssize_t G_preadv(int fd, const struct iovec* iov, int n, int32_t off) {
  return preadv(fd, iov, n, off);
}
ssize_t G_preadv64(int fd, const struct iovec* iov, int n, int64_t off) {
  return preadv(fd, iov, n, off);
}
ssize_t G_pwritev(int fd, const struct iovec* iov, int n, int32_t off) {
  return pwritev(fd, iov, n, off);
}
ssize_t G_pwritev64(int fd, const struct iovec* iov, int n, int64_t off) {
  return pwritev(fd, iov, n, off);
}
ssize_t G_pwrite_chk(int fd, const void* buf, size_t n, int32_t off, size_t blen) {
  if (n > blen)
    FortifyFail("pwrite: prevented %zu-byte read from %zu-byte buffer", n, blen);
  return pwrite(fd, buf, n, off);
}
ssize_t G_pwrite64_chk(int fd, const void* buf, size_t n, int64_t off, size_t blen) {
  if (n > blen)
    FortifyFail("pwrite64: prevented %zu-byte read from %zu-byte buffer", n, blen);
  return pwrite(fd, buf, n, off);
}
ssize_t G_readlinkat_chk(int dirfd, const char* path, char* buf, size_t n, size_t blen) {
  if (n > blen)
    FortifyFail("readlinkat: prevented %zu-byte write into %zu-byte buffer", n, blen);
  std::string full;
  if (!ResolveAtPath(dirfd, path, &full))
    return -1;
  return readlink(full.c_str(), buf, n);
}
int G_lockf(int, int, int32_t) { return 0; }  // one process: nothing to lock out
int G_lockf64(int, int, int64_t) { return 0; }
int G_sync_file_range(int, int64_t, int64_t, unsigned) { return 0; }
int G_dup3(int oldfd, int newfd, int flags) {
  if (oldfd == newfd) {
    errno = EINVAL;
    return -1;
  }
  int r = dup2(oldfd, newfd);
  if (r >= 0 && (flags & 0x80000 /* O_CLOEXEC */))
    fcntl(r, F_SETFD, FD_CLOEXEC);
  return r;
}
// (inotify: ndk_fd.cc, a descriptor without events.)

// --- threads: barriers and spin locks (API 24) -------------------------------------------
// A guest pthread_spinlock_t is an int: the lock word, used atomically in
// place. Barriers are host objects found by the guest object's address.
int G_spin_init(int32_t* lock, int) {
  *lock = 0;
  return 0;
}
int G_spin_destroy(int32_t*) { return 0; }
int G_spin_lock(int32_t* lock) {
  std::atomic<int32_t>* a = reinterpret_cast<std::atomic<int32_t>*>(lock);
  int32_t expected = 0;
  while (!a->compare_exchange_weak(expected, 1, std::memory_order_acquire)) {
    expected = 0;
    sched_yield();
  }
  return 0;
}
int G_spin_trylock(int32_t* lock) {
  int32_t expected = 0;
  return reinterpret_cast<std::atomic<int32_t>*>(lock)->compare_exchange_strong(
             expected, 1, std::memory_order_acquire) ? 0 : 16 /* EBUSY */;
}
int G_spin_unlock(int32_t* lock) {
  reinterpret_cast<std::atomic<int32_t>*>(lock)->store(0, std::memory_order_release);
  return 0;
}

struct Barrier {
  std::mutex m;
  std::condition_variable cv;
  unsigned count, waiting = 0, generation = 0;
};
std::mutex g_barriers_lock;
std::map<uint32_t, Barrier*> g_barriers;
int G_barrier_init(void* b, const void*, unsigned count) {
  if (count == 0)
    return 22;  // EINVAL
  std::lock_guard<std::mutex> l(g_barriers_lock);
  Barrier*& slot = g_barriers[Addr(b)];
  delete slot;
  slot = new Barrier;
  slot->count = count;
  return 0;
}
int G_barrier_destroy(void* b) {
  std::lock_guard<std::mutex> l(g_barriers_lock);
  auto it = g_barriers.find(Addr(b));
  if (it != g_barriers.end()) {
    delete it->second;
    g_barriers.erase(it);
  }
  return 0;
}
int G_barrier_wait(void* b) {
  Barrier* bar;
  {
    std::lock_guard<std::mutex> l(g_barriers_lock);
    auto it = g_barriers.find(Addr(b));
    if (it == g_barriers.end())
      return 22;
    bar = it->second;
  }
  std::unique_lock<std::mutex> l(bar->m);
  unsigned gen = bar->generation;
  if (++bar->waiting == bar->count) {
    bar->waiting = 0;
    bar->generation++;
    bar->cv.notify_all();
    return -1;  // PTHREAD_BARRIER_SERIAL_THREAD
  }
  bar->cv.wait(l, [&] { return bar->generation != gen; });
  return 0;
}
int G_barrierattr(void*) { return 0; }
int G_barrierattr_getpshared(const void*, int* shared) {
  *shared = 0;
  return 0;
}
int G_barrierattr_setpshared(void*, int) { return 0; }
// pthread_getname_np (API 26): bionic keeps the name in the kernel (comm).
int G_getname_np(uint32_t, char* buf, size_t n) {
  if (n == 0)
    return 34;  // ERANGE
  buf[0] = '\0';
  return 0;
}

// --- users, locale, misc (API 26) ---------------------------------------------------------
void* G_null_entry() { return nullptr; }

// getgrgid/getgrnam: bionic's groups are Android's ids (root, system ...,
// u0_aNN for apps), no /etc/group. Material Files names a file's group with
// it. The struct is bionic's 32-bit one (4 words); the name lives per thread.
struct GGroup {
  uint32_t gr_name, gr_passwd, gr_gid, gr_mem;
};
}  // namespace

// The Android name of group or user |id| (bionic's android_ids and
// u0_aNN), into |buf|; for the 64-bit bridge too (ndk64_libc.cc).
void NdkAndroidIdName(uint32_t id, char* buf, size_t len) {
  static const struct { uint32_t id; const char* name; } kIds[] = {
      {0, "root"}, {1000, "system"}, {1001, "radio"}, {1003, "graphics"}, {1004, "input"},
      {1005, "audio"}, {1006, "camera"}, {1013, "media"}, {1015, "sdcard_rw"},
      {1023, "media_rw"}, {1028, "sdcard_r"}, {2000, "shell"}, {3003, "inet"},
      {9997, "everybody"}, {9999, "nobody"},
  };
  for (const auto& e : kIds) {
    if (e.id == id) {
      snprintf(buf, len, "%s", e.name);
      return;
    }
  }
  const uint32_t app = id % 100000;
  if (app >= 10000 && app < 20000)
    snprintf(buf, len, "u%u_a%u", id / 100000, app - 10000);
  else if (app >= 50000 && app < 60000)
    snprintf(buf, len, "u%u_a%u_cache", id / 100000, app - 50000);
  else
    snprintf(buf, len, "%u", id);
}

// The id of an Android name (NdkAndroidIdName's inverse), or -1.
int64_t NdkAndroidIdOf(const char* name) {
  if (name == nullptr)
    return -1;
  for (uint32_t id : {0u, 1000u, 1001u, 1003u, 1004u, 1005u, 1006u, 1013u, 1015u, 1023u,
                      1028u, 2000u, 3003u, 9997u, 9999u}) {
    char buf[32];
    NdkAndroidIdName(id, buf, sizeof(buf));
    if (strcmp(buf, name) == 0)
      return id;
  }
  unsigned user = 0, app = 0;
  char tail = 0;
  if (sscanf(name, "u%u_a%u%c", &user, &app, &tail) == 2 && app < 10000)
    return static_cast<int64_t>(user) * 100000 + 10000 + app;
  return -1;
}

struct passwd* NdkAndroidPasswd(uint32_t uid) {
  static thread_local struct passwd pw;
  static thread_local char name[32];
  static char empty[] = "", root_dir[] = "/", app_dir[] = "/data", shell[] = "/system/bin/sh";
  NdkAndroidIdName(uid, name, sizeof(name));
  const uint32_t app = uid % 100000;
  pw.pw_name = name;
  pw.pw_passwd = nullptr;
  pw.pw_uid = uid;
  pw.pw_gid = uid;
  pw.pw_gecos = empty;
  pw.pw_dir = app >= 10000 ? app_dir : root_dir;
  pw.pw_shell = shell;
  return &pw;
}

namespace {

GGroup* GuestGroup(uint32_t gid) {
  static thread_local GGroup g;
  static thread_local char name[32];
  static thread_local uint32_t members[1];
  NdkAndroidIdName(gid, name, sizeof(name));
  members[0] = 0;
  g = {Addr(name), 0, gid, Addr(members)};
  return &g;
}
// Extended attributes: the file systems here keep none (ENOTSUP, as a file
// system without xattr support answers). Material Files reads a file's
// SELinux label (getxattr "security.selinux") when it lists a directory.
ssize_t G_getxattr(const char*, const char*, void*, size_t) {
  errno = ENOTSUP;
  return -1;
}
ssize_t G_fgetxattr(int, const char*, void*, size_t) {
  errno = ENOTSUP;
  return -1;
}
ssize_t G_listxattr(const char*, char*, size_t) {
  errno = ENOTSUP;
  return -1;
}
ssize_t G_flistxattr(int, char*, size_t) {
  errno = ENOTSUP;
  return -1;
}
int G_setxattr(const char*, const char*, const void*, size_t, int) {
  errno = ENOTSUP;
  return -1;
}
int G_fsetxattr(int, const char*, const void*, size_t, int) {
  errno = ENOTSUP;
  return -1;
}
int G_removexattr(const char*, const char*) {
  errno = ENOTSUP;
  return -1;
}
int G_fremovexattr(int, const char*) {
  errno = ENOTSUP;
  return -1;
}
GGroup* G_getgrgid(uint32_t gid) { return GuestGroup(gid); }
GGroup* G_getgrnam(const char* name) {
  const int64_t id = NdkAndroidIdOf(name);
  return id < 0 ? nullptr : GuestGroup(static_cast<uint32_t>(id));
}
void G_noop() {}
// <langinfo.h>: CODESET (1 in bionic) is UTF-8; the rest of the C locale.
const char* G_nl_langinfo(int item) { return item == 1 ? "UTF-8" : ""; }
const char* G_nl_langinfo_l(int item, void*) { return G_nl_langinfo(item); }
int G_getdomainname(char* buf, size_t n) {
  if (n < 7) {
    errno = EINVAL;
    return -1;
  }
  strcpy(buf, "(none)");
  return 0;
}
int G_eperm() {
  errno = EPERM;
  return -1;
}
const char* G_ctermid(char* s) {
  if (s) {
    strcpy(s, "/dev/tty");
    return s;
  }
  return "/dev/tty";
}
// catopen: no message catalogs; catgets answers the default.
uint32_t G_catopen(const char*, int) {
  errno = ENOENT;
  return UINT32_MAX;
}
const char* G_catgets(uint32_t, int, int, const char* dflt) { return dflt; }
int G_catclose(uint32_t) { return 0; }
int G_sig_zero(int) { return 0; }  // sighold, sigrelse, sigignore: no signals arrive
int G_tc_enotty() {
  errno = ENOTTY;
  return -1;
}
// One process, its own group and session leader.
int G_getpgrp() { return getpid(); }
int G_getpgid(int pid) { return pid == 0 || pid == getpid() ? getpid() : (errno = ESRCH, -1); }
// What else apps' libraries import (topic compat-fixes): no services
// database, terminals, mounts or named semaphores; one uid, which cannot
// become another (as setuid); a device answers so without them.
void* G_null_enoent() {
  errno = ENOENT;
  return nullptr;
}
void* G_null_enosys() {
  errno = ENOSYS;
  return nullptr;
}
void* G_shmat() {
  errno = ENOSYS;
  return reinterpret_cast<void*>(-1);
}
int G_ttyname_r() { return ENOTTY; }  // (returns the error)
int G_zero() { return 0; }
int G_one() { return 1; }
int G_setid8(uint32_t id) {  // (as ndk_libc.cc's setuid)
  if (id == 0)
    return 0;
  errno = EPERM;
  return -1;
}
int G_getresid(uint32_t* r, uint32_t* e, uint32_t* s) {
  *r = *e = *s = getuid();
  return 0;
}
int G_sigpending(uint32_t* set) {  // nothing pending: guests get signals as they are sent
  *set = 0;
  return 0;
}
int G64_sigpending(uint64_t* set) {
  *set = 0;
  return 0;
}
void* G_sched_cpualloc(uint32_t count) { return calloc(1, (count + 63) / 64 * 8); }
char g_login[] = "u0_app";
char* G_getlogin() { return g_login; }
int G_res_search() {
  h_errno = HOST_NOT_FOUND;
  return -1;
}
// API 29 (an inline function of the NDK's headers before): the system's API
// level. Go's runtime/cgo asks it through dlsym to find its TLS slot (on 29+,
// TLS_SLOT_APP) and aborts without it ("inittls: could not find pthread key").
int G_android_get_device_api_level() {
  char v[92];
  property_get("ro.build.version.sdk", v, "");
  int level = atoi(v);
  return level > 0 ? level : -1;
}

// --- dynamic linking ----------------------------------------------------------------------
// android_dlopen_ext (API 21) and dlvsym (24) are aliases of dlopen and
// dlsym below: the extinfo and version arguments are ignored.


// --- arm64-v8a guests (topic ndk-arm64) -----------------------------------------
// The structures with pointers (ifaddrs, if_nameindex), off_t of lockf and
// __pwrite_chk (64 bits), wide-string conversions with an end pointer.
struct G64Ifaddrs : GuestLayout64 {
  uint64_t next, name;
  uint32_t flags, pad;
  uint64_t addr, netmask, broadaddr, data;
};
struct IfaddrsBlock64 {
  G64Ifaddrs ifa[2];
  struct sockaddr_in addr[2], mask[2], bcast[2];
  char names[2][8];
};
int G64_getifaddrs(uint64_t* out) {
  IfaddrsBlock64* b = static_cast<IfaddrsBlock64*>(calloc(1, sizeof(IfaddrsBlock64)));
  if (!b) {
    errno = ENOMEM;  // (out of memory: no list, as bionic)
    return -1;
  }
  const char* names[2] = {"lo", "eth0"};
  const char* addrs[2] = {"127.0.0.1", "10.0.2.15"};
  const char* masks[2] = {"255.0.0.0", "255.255.255.0"};
  const char* bcasts[2] = {"127.255.255.255", "10.0.2.255"};
  const uint32_t flags[2] = {0x1 | 0x8 | 0x40, 0x1 | 0x2 | 0x40 | 0x1000};
  for (int i = 0; i < 2; i++) {
    strcpy(b->names[i], names[i]);
    for (struct sockaddr_in* sa : {&b->addr[i], &b->mask[i], &b->bcast[i]})
      sa->sin_family = 2;
    inet_aton(addrs[i], &b->addr[i].sin_addr);
    inet_aton(masks[i], &b->mask[i].sin_addr);
    inet_aton(bcasts[i], &b->bcast[i].sin_addr);
    G64Ifaddrs& e = b->ifa[i];
    e.next = i == 0 ? Addr(&b->ifa[1]) : 0;
    e.name = Addr(b->names[i]);
    e.flags = flags[i];
    e.addr = Addr(&b->addr[i]);
    e.netmask = Addr(&b->mask[i]);
    e.broadaddr = i == 0 ? 0 : Addr(&b->bcast[i]);
    e.data = 0;
  }
  *out = Addr(&b->ifa[0]);
  return 0;
}
struct G64IfNameIndex : GuestLayout64 {
  uint32_t index, pad;
  uint64_t name;
};
G64IfNameIndex* G64_if_nameindex() {
  static const char* const names[] = {"lo", "eth0"};
  G64IfNameIndex* v = static_cast<G64IfNameIndex*>(calloc(3, sizeof(G64IfNameIndex)));
  if (!v) {
    errno = ENOBUFS;
    return nullptr;
  }
  for (int i = 0; i < 2; i++) {
    v[i].index = i + 1;
    v[i].name = Addr(names[i]);
  }
  return v;
}
intmax_t G64_wcstoimax(const wchar_t* s, uint64_t* end, int base) {
  wchar_t* e;
  intmax_t r = wcstoimax(s, &e, base);
  if (end) *end = Addr(e);
  return r;
}
uintmax_t G64_wcstoumax(const wchar_t* s, uint64_t* end, int base) {
  wchar_t* e;
  uintmax_t r = wcstoumax(s, &e, base);
  if (end) *end = Addr(e);
  return r;
}
int G64_ppoll_chk(struct pollfd* fds, uint64_t n, const void* ts, const void* mask, size_t fds_size) {
  if (fds_size / sizeof(struct pollfd) < n)
    FortifyFail("ppoll: %u-element pollfd array too small for %u", (unsigned)(fds_size / 8), (unsigned)n);
  int ms = -1;
  if (ts) {
    const int64_t* t = static_cast<const int64_t*>(ts);
    ms = static_cast<int>(t[0] * 1000 + t[1] / 1000000);
  }
  (void)mask;
  return poll(fds, static_cast<nfds_t>(n), ms);
}

const HostSymbol kLibc8_64[] = {
    NDK_FN64("getifaddrs", G64_getifaddrs), NDK_FN64("if_nameindex", G64_if_nameindex),
    NDK_FN64("lockf", G_lockf64), NDK_FN64("__pwrite_chk", G_pwrite64_chk),
    NDK_FN64("wcstoimax", G64_wcstoimax), NDK_FN64("wcstoumax", G64_wcstoumax),
    NDK_FN64("__ppoll_chk", G64_ppoll_chk),
};

const HostSymbol kLibc8[] = {
    NDK_RAW("dprintf", G_dprintf), NDK_FN("vdprintf", G_vdprintf),
    NDK_FNX("stpcpy", ::stpcpy), NDK_FNX("stpncpy", ::stpncpy), NDK_FNX("mempcpy", ::mempcpy),
    NDK_FNX("wmempcpy", G_wmempcpy), NDK_FNX("strchrnul", ::strchrnul),
    NDK_FNX("strerror_l", G_strerror_l), NDK_FNX("__gnu_basename", G_gnu_basename),
    NDK_FNX("_tolower", G_tolower_raw), NDK_FNX("_toupper", G_toupper_raw),
    NDK_FNX("getprogname", G_getprogname), NDK_FNX("setprogname", G_setprogname),
    NDK_FNX("get_nprocs", G_get_nprocs), NDK_FNX("get_nprocs_conf", G_get_nprocs),
    NDK_FNX("get_phys_pages", G_get_phys_pages), NDK_FNX("get_avphys_pages", G_get_avphys_pages),
    NDK_FNX("quick_exit", G_quick_exit), NDK_FN("at_quick_exit", G_at_quick_exit),
    NDK_FNX("_Exit", ::_Exit), NDK_FNX("mallopt", G_mallopt),
    NDK_FN("__cxa_thread_atexit_impl", G_cxa_thread_atexit_impl),
    NDK_FN("__cfi_slowpath", G_cfi_slowpath), NDK_FN("__cfi_slowpath_diag", G_cfi_slowpath_diag),
    NDK_FNX("__cfi_shadow_size", G_cfi_shadow_size),
    NDK_FN("sendmmsg", G_sendmmsg), NDK_FN("ppoll", G_ppoll), NDK_FN("__ppoll_chk", G_ppoll_chk),
    NDK_FN("getifaddrs", G_getifaddrs), NDK_FNX("freeifaddrs", G_freeifaddrs),
    NDK_FN("if_nameindex", G_if_nameindex), NDK_FNX("if_freenameindex", G_if_freenameindex),
    NDK_FNX("inet_network", ::inet_network),
    NDK_FN("preadv", G_preadv), NDK_FN("preadv64", G_preadv64), NDK_FN("pwritev", G_pwritev),
    NDK_FN("pwritev64", G_pwritev64), NDK_FN("__pwrite_chk", G_pwrite_chk),
    NDK_FNX("__pwrite64_chk", G_pwrite64_chk), NDK_FNX("__readlinkat_chk", G_readlinkat_chk),
    NDK_FN("lockf", G_lockf), NDK_FNX("lockf64", G_lockf64),
    NDK_FNX("sync_file_range", G_sync_file_range), NDK_FNX("dup3", G_dup3),
    NDK_FNX("pthread_spin_init", G_spin_init), NDK_FNX("pthread_spin_destroy", G_spin_destroy),
    NDK_FNX("pthread_spin_lock", G_spin_lock), NDK_FNX("pthread_spin_trylock", G_spin_trylock),
    NDK_FNX("pthread_spin_unlock", G_spin_unlock),
    NDK_FNX("pthread_barrier_init", G_barrier_init),
    NDK_FNX("pthread_barrier_destroy", G_barrier_destroy),
    NDK_FNX("pthread_barrier_wait", G_barrier_wait),
    NDK_FNX("pthread_barrierattr_init", G_barrierattr),
    NDK_FNX("pthread_barrierattr_destroy", G_barrierattr),
    NDK_FNX("pthread_barrierattr_getpshared", G_barrierattr_getpshared),
    NDK_FNX("pthread_barrierattr_setpshared", G_barrierattr_setpshared),
    NDK_FNX("pthread_getname_np", G_getname_np),
    NDK_FNX("getpwent", G_null_entry), NDK_FNX("getgrent", G_null_entry),
    NDK_FN("getgrgid", G_getgrgid), NDK_FN("getgrnam", G_getgrnam),
    NDK_FNX("getxattr", G_getxattr), NDK_FNX("lgetxattr", G_getxattr),
    NDK_FNX("fgetxattr", G_fgetxattr), NDK_FNX("listxattr", G_listxattr),
    NDK_FNX("llistxattr", G_listxattr), NDK_FNX("flistxattr", G_flistxattr),
    NDK_FNX("setxattr", G_setxattr), NDK_FNX("lsetxattr", G_setxattr),
    NDK_FNX("fsetxattr", G_fsetxattr), NDK_FNX("removexattr", G_removexattr),
    NDK_FNX("lremovexattr", G_removexattr), NDK_FNX("fremovexattr", G_fremovexattr),
    NDK_FNX("setpwent", G_noop), NDK_FNX("endpwent", G_noop), NDK_FNX("setgrent", G_noop),
    NDK_FNX("endgrent", G_noop),
    NDK_FNX("nl_langinfo", G_nl_langinfo), NDK_FNX("nl_langinfo_l", G_nl_langinfo_l),
    NDK_FNX("getdomainname", G_getdomainname), NDK_FNX("setdomainname", G_eperm),
    NDK_FNX("sethostname", G_eperm), NDK_FNX("ctermid", G_ctermid),
    NDK_FNX("catopen", G_catopen), NDK_FNX("catgets", G_catgets), NDK_FNX("catclose", G_catclose),
    NDK_FNX("sighold", G_sig_zero), NDK_FNX("sigrelse", G_sig_zero),
    NDK_FNX("sigignore", G_sig_zero), NDK_FNX("sigpause", G_sig_zero),
    NDK_FNX("tcgetattr", G_tc_enotty), NDK_FNX("tcsetattr", G_tc_enotty),
    NDK_FNX("tcdrain", G_tc_enotty), NDK_FNX("tcflow", G_tc_enotty), NDK_FNX("tcflush", G_tc_enotty),
    NDK_FNX("tcsendbreak", G_tc_enotty), NDK_FNX("tcgetsid", G_tc_enotty),
    NDK_FNX("tcgetpgrp", G_tc_enotty), NDK_FNX("tcsetpgrp", G_tc_enotty),
    NDK_FNX("android_get_device_api_level", G_android_get_device_api_level),
    NDK_FNX("getpgrp", G_getpgrp), NDK_FNX("getpgid", G_getpgid), NDK_FNX("getsid", G_getpgid),
    NDK_FNX("setpgrp", G_zero), NDK_FNX("nice", G_zero), NDK_FNX("killpg", G_eperm),
    NDK_FNX("getservbyname", G_null_enoent), NDK_FNX("getservbyport", G_null_enoent),
    NDK_FNX("res_search", G_res_search), NDK_FNX("hstrerror", ::hstrerror),
    NDK_FNX("ttyname_r", G_ttyname_r), NDK_FNX("ptsname", G_null_enosys),
    NDK_FNX("ptsname_r", G_ttyname_r), NDK_FNX("grantpt", Enosys), NDK_FNX("unlockpt", Enosys),
    NDK_FNX("seteuid", G_setid8), NDK_FNX("setegid", G_setid8), NDK_FNX("setreuid", G_setid8),
    NDK_FNX("setregid", G_setid8), NDK_FNX("setresuid", G_setid8), NDK_FNX("setresgid", G_setid8),
    NDK_FNX("getresuid", G_getresid), NDK_FNX("getresgid", G_getresid),
    NDK_FN("sigpending", G_sigpending), NDK_FN64("sigpending", G64_sigpending),
    NDK_FNX("strsignal", ::strsignal), NDK_FN("clearenv", ::clearenv),
    NDK_FNX("tempnam", ::tempnam), NDK_FNX("erand48", ::erand48), NDK_FNX("nrand48", ::nrand48),
    NDK_FNX("jrand48", ::jrand48), NDK_FNX("__sched_cpualloc", G_sched_cpualloc),
    NDK_FNX("__sched_cpufree", ::free), NDK_FNX("getlogin", G_getlogin),
    NDK_FNX("setmntent", G_null_enoent), NDK_FNX("getmntent", G_null_enoent),
    NDK_FNX("endmntent", G_one), NDK_FNX("mount", G_eperm), NDK_FNX("umount", G_eperm),
    NDK_FNX("umount2", G_eperm), NDK_FNX("unshare", G_eperm), NDK_FNX("clock_settime", G_eperm),
    NDK_FNX("sem_open", G_null_enosys), NDK_FNX("sem_close", Enosys), NDK_FNX("sem_unlink", Enosys),
    NDK_FNX("shmat", G_shmat), NDK_FNX("ftok", Enosys),
    // No SysV IPC, namespaces, splicing, pseudo-terminals, queued signals.
    NDK_FNX("msgget", Enosys), NDK_FNX("msgctl", Enosys), NDK_FNX("msgrcv", Enosys),
    NDK_FNX("msgsnd", Enosys), NDK_FNX("semget", Enosys), NDK_FNX("semctl", Enosys),
    NDK_FNX("semop", Enosys), NDK_FNX("semtimedop", Enosys), NDK_FNX("shmget", Enosys),
    NDK_FNX("shmctl", Enosys), NDK_FNX("shmdt", Enosys), NDK_FNX("setns", Enosys),
    NDK_FNX("splice", Enosys), NDK_FNX("tee", Enosys), NDK_FNX("vmsplice", Enosys),
    NDK_FNX("posix_openpt", Enosys), NDK_FNX("openpty", Enosys), NDK_FNX("forkpty", Enosys),
    NDK_FNX("login_tty", Enosys), NDK_FNX("sigqueue", Enosys), NDK_FNX("sigtimedwait", Enosys),
    NDK_FNX("sigwaitinfo", Enosys), NDK_FNX("quotactl", Enosys), NDK_FNX("adjtimex", Enosys),
    NDK_FNX("clock_adjtime", Enosys), NDK_FNX("futimesat", Enosys),
    NDK_FNX("malloc_info", Enosys),
    NDK_FNX("initstate", ::initstate), NDK_FNX("setstate", ::setstate),
    NDK_FNX("lcong48", ::lcong48), NDK_FN("insque", ::insque), NDK_FN("remque", ::remque),
    NDK_FN("wcstoimax", ::wcstoimax), NDK_FN("wcstoumax", ::wcstoumax),
    NDK_FNX("c16rtomb", ::c16rtomb), NDK_FNX("mbrtoc16", ::mbrtoc16), NDK_FNX("mblen", ::mblen),
};

const HostSymbol kLibm8[] = {
    NDK_FNX("__fpclassify", G_fpclassify), NDK_FNX("isfinite", G_isfinite),
    NDK_FNX("isfinitef", G_isfinitef), NDK_FN("isfinitel", G_isfinite),
    NDK_FNX("isnormal", G_isnormal), NDK_FNX("isnormalf", G_isnormalf),
    NDK_FN("isnormall", G_isnormal), NDK_FN("isinfl", G_isinf), NDK_FN("isnanl", G_isnan),
    NDK_FN("significandl", G_significand),
    NDK_FNX("feclearexcept", G_fe_zero_i), NDK_FNX("feraiseexcept", G_fe_zero_i),
    NDK_FNX("fetestexcept", G_fe_zero_i), NDK_FNX("fegetenv", G_fe_zero_p),
    NDK_FNX("fesetenv", G_fe_zero_p), NDK_FNX("feholdexcept", G_fe_zero_p),
    NDK_FNX("feupdateenv", G_fe_zero_p), NDK_FNX("fegetexceptflag", G_fe_zero_pi),
    NDK_FNX("fesetexceptflag", G_fe_zero_pi), NDK_FNX("fesetround", G_fesetround),
    NDK_FNX("feenableexcept", G_feenableexcept), NDK_FNX("fedisableexcept", G_fe_zero_i),
    NDK_FNX("fegetexcept", G_fe_zero),
};

// Same code, other name: bionic's foo64 of foo where off_t and the
// structures are the same for both (struct dirent64 is struct dirent, fpos_t
// stays 32-bit for fgetpos64's off_t paths in bionic... except where noted),
// libm's long double functions (ARM: long double is double), BSD names.
const char* const kAliases[][3] = {
    {"libc.so", "readdir64", "readdir"}, {"libc.so", "readdir64_r", "readdir_r"},
    {"libc.so", "alphasort64", "alphasort"}, {"libc.so", "scandir64", "scandir"},
    {"libc.so", "scandirat64", "scandirat"}, {"libc.so", "fallocate64", "fallocate"},
    {"libc.so", "ftw64", "ftw"}, {"libc.so", "nftw64", "nftw"},
    {"libc.so", "prlimit64", "prlimit"}, {"libc.so", "setrlimit64", "setrlimit"},
    {"libc.so", "getrlimit64", "getrlimit"}, {"libc.so", "mkostemp64", "mkostemp"},
    {"libc.so", "mkostemps64", "mkostemps"}, {"libc.so", "mkstemps64", "mkstemps"},
    {"libc.so", "fopen64", "fopen"}, {"libc.so", "freopen64", "freopen"},
    {"libc.so", "tmpfile64", "tmpfile"}, {"libc.so", "fileno_unlocked", "fileno"},
    {"libc.so", "clearerr_unlocked", "clearerr"}, {"libc.so", "feof_unlocked", "feof"},
    {"libc.so", "ferror_unlocked", "ferror"},
    {"libc.so", "strtoq", "strtoll"}, {"libc.so", "strtouq", "strtoull"},
    {"libc.so", "towctrans_l", "towctrans"}, {"libc.so", "wctrans_l", "wctrans"},
    {"libc.so", "pthread_mutex_timedlock", "pthread_mutex_lock_timeout_np"},
    {"libdl.so", "android_dlopen_ext", "dlopen"}, {"libdl.so", "dlvsym", "dlsym"},
    {"libm.so", "acosl", "acos"}, {"libm.so", "acoshl", "acosh"}, {"libm.so", "asinl", "asin"},
    {"libm.so", "asinhl", "asinh"}, {"libm.so", "atan2l", "atan2"}, {"libm.so", "atanhl", "atanh"},
    {"libm.so", "atanl", "atan"}, {"libm.so", "cbrtl", "cbrt"}, {"libm.so", "coshl", "cosh"},
    {"libm.so", "cosl", "cos"}, {"libm.so", "erfcl", "erfc"}, {"libm.so", "erfl", "erf"},
    {"libm.so", "exp2l", "exp2"}, {"libm.so", "expl", "exp"}, {"libm.so", "expm1l", "expm1"},
    {"libm.so", "fmal", "fma"}, {"libm.so", "fmodl", "fmod"}, {"libm.so", "frexpl", "frexp"},
    {"libm.so", "hypotl", "hypot"}, {"libm.so", "lgammal", "lgamma"},
    {"libm.so", "lgammal_r", "lgamma_r"}, {"libm.so", "llrintl", "llrint"},
    {"libm.so", "log10l", "log10"}, {"libm.so", "log1pl", "log1p"}, {"libm.so", "logl", "log"},
    {"libm.so", "lrintl", "lrint"}, {"libm.so", "modfl", "modf"},
    {"libm.so", "nearbyintl", "nearbyint"}, {"libm.so", "nextafterl", "nextafter"},
    {"libm.so", "powl", "pow"}, {"libm.so", "remainderl", "remainder"},
    {"libm.so", "remquol", "remquo"}, {"libm.so", "rintl", "rint"}, {"libm.so", "sinhl", "sinh"},
    {"libm.so", "sinl", "sin"}, {"libm.so", "sqrtl", "sqrt"}, {"libm.so", "tanhl", "tanh"},
    {"libm.so", "tanl", "tan"}, {"libm.so", "tgammal", "tgamma"},
};

}  // namespace

void RegisterLibc8() {
  RegisterHostSymbols("libc.so", kLibc8);
  RegisterHostSymbols("libm.so", kLibm8);
  for (const auto& a : kAliases)
    AliasHostSymbol(a[0], a[1], a[2]);
  RegisterHostSymbols("libc.so", kLibc8_64);
}

}  // namespace ndk
