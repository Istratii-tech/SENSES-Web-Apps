// What bionic gained in Android 5-6 that NDK libraries built for API 21+
// import (docs/topics/ndk-imports.md): FORTIFY checks (__*_chk), system
// properties, getauxval, processes and signals (with honest refusals
// where wasm has no such thing: fork, exec, ptrace), scheduling, resource
// limits and system information, 64-bit time, syslog, a few libm and
// ARM EABI helpers. File descriptors are in ndk_fd.cc, locales and wide
// characters in ndk_locale.cc. Part of the arc-wasm shim layer.

#include <emscripten/threading.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <math.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

#include <atomic>
#include <map>
#include <mutex>
#include <string>

#include <sys/resource.h>
#include <sys/utsname.h>

#include "ndk_bridge.h"

extern "C" int __android_log_print(int prio, const char* tag, const char* fmt, ...);
extern "C" int __android_log_write(int prio, const char* tag, const char* text);
extern "C" int property_get(const char* key, char* value, const char* default_value);
// Signals to guest code (shim/ndk/cpu/ndk_cpu_ext.h; absent in the stub CPU).
extern "C" int ndk_sig_send(int tid, int sig) __attribute__((weak));
extern "C" int ndk_sig_mask(int how, const uint64_t* set, uint64_t* old) __attribute__((weak));
extern "C" int ndk_sig_deliver(void) __attribute__((weak));
extern "C" int ndk_sig_suspend(const uint64_t* mask) __attribute__((weak));
extern "C" int ndk_sig_wait_set(uint64_t set, double ms) __attribute__((weak));
extern "C" int ndk_sig_altstack(const uint32_t* ss, uint32_t* old) __attribute__((weak));

namespace ndk {

void FortifyFail(const char* fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  __android_log_print(7, "libc", "FORTIFY: %s", buf);
  abort();
}

namespace {

// --- FORTIFY (bionic/libc/bionic/__*_chk.cpp) ----------------------------------------
// The compiler passes the size of the destination it can see; a call that
// would write (or read) past it aborts, as on a device.

void* G_memcpy_chk(void* d, const void* s, size_t n, size_t dlen) {
  if (n > dlen)
    FortifyFail("memcpy: prevented %zu-byte write into %zu-byte buffer", n, dlen);
  return memcpy(d, s, n);
}
void* G_memmove_chk(void* d, const void* s, size_t n, size_t dlen) {
  if (n > dlen)
    FortifyFail("memmove: prevented %zu-byte write into %zu-byte buffer", n, dlen);
  return memmove(d, s, n);
}
void* G_memset_chk(void* d, int c, size_t n, size_t dlen) {
  if (n > dlen)
    FortifyFail("memset: prevented %zu-byte write into %zu-byte buffer", n, dlen);
  return memset(d, c, n);
}
void* G_memchr_chk(const void* s, int c, size_t n, size_t slen) {
  if (n > slen)
    FortifyFail("memchr: prevented read past end of buffer");
  return const_cast<void*>(memchr(s, c, n));
}
void* G_memrchr_chk(const void* s, int c, size_t n, size_t slen) {
  if (n > slen)
    FortifyFail("memrchr: prevented read past end of buffer");
  const unsigned char* p = static_cast<const unsigned char*>(s);
  while (n--) {
    if (p[n] == (unsigned char)c)
      return const_cast<unsigned char*>(p + n);
  }
  return nullptr;
}
char* G_strcpy_chk(char* d, const char* s, size_t dlen) {
  size_t n = strlen(s) + 1;
  if (n > dlen)
    FortifyFail("strcpy: prevented %zu-byte write into %zu-byte buffer", n, dlen);
  return static_cast<char*>(memcpy(d, s, n));
}
char* G_stpcpy_chk(char* d, const char* s, size_t dlen) {
  size_t n = strlen(s);
  if (n + 1 > dlen)
    FortifyFail("stpcpy: prevented %zu-byte write into %zu-byte buffer", n + 1, dlen);
  memcpy(d, s, n + 1);
  return d + n;
}
char* G_strncpy_chk(char* d, const char* s, size_t n, size_t dlen) {
  if (n > dlen)
    FortifyFail("strncpy: prevented %zu-byte write into %zu-byte buffer", n, dlen);
  return strncpy(d, s, n);
}
char* G_strncpy_chk2(char* d, const char* s, size_t n, size_t dlen, size_t slen) {
  if (n > dlen)
    FortifyFail("strncpy: prevented %zu-byte write into %zu-byte buffer", n, dlen);
  if (n > slen) {
    size_t l = strnlen(s, slen);
    if (l >= slen)
      FortifyFail("strncpy: detected read past end of %zu-byte buffer", slen);
  }
  return strncpy(d, s, n);
}
char* G_stpncpy_chk(char* d, const char* s, size_t n, size_t dlen) {
  if (n > dlen)
    FortifyFail("stpncpy: prevented %zu-byte write into %zu-byte buffer", n, dlen);
  return stpncpy(d, s, n);
}
char* G_stpncpy_chk2(char* d, const char* s, size_t n, size_t dlen, size_t slen) {
  if (n > dlen)
    FortifyFail("stpncpy: prevented %zu-byte write into %zu-byte buffer", n, dlen);
  if (n > slen && strnlen(s, slen) >= slen)
    FortifyFail("stpncpy: detected read past end of %zu-byte buffer", slen);
  return stpncpy(d, s, n);
}
char* G_strcat_chk(char* d, const char* s, size_t dlen) {
  size_t dl = strnlen(d, dlen);
  if (dl >= dlen)
    FortifyFail("strcat: detected read past end of buffer");
  size_t sl = strlen(s);
  if (dl + sl + 1 > dlen)
    FortifyFail("strcat: prevented write past end of %zu-byte buffer", dlen);
  memcpy(d + dl, s, sl + 1);
  return d;
}
char* G_strncat_chk(char* d, const char* s, size_t n, size_t dlen) {
  size_t dl = strnlen(d, dlen);
  if (dl >= dlen)
    FortifyFail("strncat: detected read past end of buffer");
  size_t sl = strnlen(s, n);
  if (dl + sl + 1 > dlen)
    FortifyFail("strncat: prevented write past end of %zu-byte buffer", dlen);
  memcpy(d + dl, s, sl);
  d[dl + sl] = 0;
  return d;
}
size_t G_strlen_chk(const char* s, size_t slen) {
  size_t n = strlen(s);
  if (n >= slen)
    FortifyFail("strlen: detected read past end of %zu-byte buffer", slen);
  return n;
}
char* G_strchr_chk(const char* s, int c, size_t slen) {
  for (size_t i = 0; i < slen; i++) {
    if (s[i] == (char)c)
      return const_cast<char*>(s + i);
    if (s[i] == 0)
      return nullptr;
  }
  FortifyFail("strchr: prevented read past end of buffer");
}
char* G_strrchr_chk(const char* s, int c, size_t slen) {
  const char* found = nullptr;
  for (size_t i = 0; i < slen; i++) {
    if (s[i] == (char)c)
      found = s + i;
    if (s[i] == 0)
      return const_cast<char*>(found);
  }
  FortifyFail("strrchr: prevented read past end of buffer");
}
size_t G_strlcpy_chk(char* d, const char* s, size_t n, size_t dlen) {
  if (n > dlen)
    FortifyFail("strlcpy: prevented %zu-byte write into %zu-byte buffer", n, dlen);
  size_t l = strlen(s);
  if (n) {
    size_t c = l < n - 1 ? l : n - 1;
    memcpy(d, s, c);
    d[c] = 0;
  }
  return l;
}
size_t G_strlcat_chk(char* d, const char* s, size_t n, size_t dlen) {
  if (n > dlen)
    FortifyFail("strlcat: prevented %zu-byte write into %zu-byte buffer", n, dlen);
  size_t dl = strnlen(d, n);
  if (dl == n)
    return n + strlen(s);
  size_t l = strlen(s), room = n - dl - 1, c = l < room ? l : room;
  memcpy(d + dl, s, c);
  d[dl + c] = 0;
  return dl + l;
}
// printf family: the format runs through the bridge's guest formatter.
int G_vsnprintf_chk(char* d, size_t n, int, size_t dlen, const char* fmt, uint32_t ap) {
  if (n > dlen)
    FortifyFail("vsnprintf: prevented %zu-byte write into %zu-byte buffer", n, dlen);
  VaArgs a(ap);
  return GuestFormat(d, n, fmt, a);
}
int G_vsprintf_chk(char* d, int, size_t dlen, const char* fmt, uint32_t ap) {
  VaArgs a(ap);
  int r = GuestFormat(d, dlen, fmt, a);
  if (r >= 0 && (size_t)r >= dlen)
    FortifyFail("vsprintf: prevented write past end of %zu-byte buffer", dlen);
  return r;
}
void G_snprintf_chk(NdkCpuState* s, void*) {
  char* d = Ptr<char>(s->r[0]);
  size_t n = s->r[1], dlen = s->r[3];
  RegArgs a(s, 5);
  if (n > dlen)
    FortifyFail("snprintf: prevented %zu-byte write into %zu-byte buffer", n, dlen);
  SetResult(s, GuestFormat(d, n, Ptr<const char>(a.Word(4)), a));
}
void G_sprintf_chk(NdkCpuState* s, void*) {
  char* d = Ptr<char>(s->r[0]);
  size_t dlen = s->r[2];
  RegArgs a(s, 4);
  int r = GuestFormat(d, dlen, Ptr<const char>(s->r[3]), a);
  if (r >= 0 && (size_t)r >= dlen)
    FortifyFail("sprintf: prevented write past end of %zu-byte buffer", dlen);
  SetResult(s, r);
}
ssize_t G_read_chk(int fd, void* buf, size_t n, size_t blen) {
  if (n > blen)
    FortifyFail("read: prevented %zu-byte write into %zu-byte buffer", n, blen);
  return FdRead(fd, buf, n);
}
ssize_t G_pread_chk(int fd, void* buf, size_t n, int32_t off, size_t blen) {
  if (n > blen)
    FortifyFail("pread: prevented %zu-byte write into %zu-byte buffer", n, blen);
  return pread(fd, buf, n, off);
}
ssize_t G_pread64_chk(int fd, void* buf, size_t n, int64_t off, size_t blen) {
  if (n > blen)
    FortifyFail("pread64: prevented %zu-byte write into %zu-byte buffer", n, blen);
  return pread(fd, buf, n, off);
}
ssize_t G_write_chk(int fd, const void* buf, size_t n, size_t blen) {
  if (n > blen)
    FortifyFail("write: prevented %zu-byte read from %zu-byte buffer", n, blen);
  return FdWrite(fd, buf, n);
}
ssize_t G_readlink_chk(const char* p, char* buf, size_t n, size_t blen) {
  if (n > blen)
    FortifyFail("readlink: prevented %zu-byte write into %zu-byte buffer", n, blen);
  return readlink(p, buf, n);
}
char* G_getcwd_chk(char* buf, size_t n, size_t blen) {
  if (n > blen)
    FortifyFail("getcwd: prevented %zu-byte write into %zu-byte buffer", n, blen);
  return getcwd(buf, n);
}
int G_poll_chk(struct pollfd* fds, uint32_t nfds, int timeout, size_t flen) {
  if (nfds * sizeof(struct pollfd) > flen)
    FortifyFail("poll: %u fds in a %zu-byte buffer", nfds, flen);
  return GuestPoll(fds, nfds, timeout);
}
ssize_t G_recvfrom_chk(int fd, void* buf, size_t n, size_t blen, int flags, struct sockaddr* a,
                       socklen_t* al) {
  if (n > blen)
    FortifyFail("recvfrom: prevented %zu-byte write into %zu-byte buffer", n, blen);
  return recvfrom(fd, buf, n, flags, a, al);
}
ssize_t G_sendto_chk(int fd, const void* buf, size_t n, size_t blen, int flags,
                     const struct sockaddr* a, socklen_t al) {
  if (n > blen)
    FortifyFail("sendto: prevented %zu-byte read from %zu-byte buffer", n, blen);
  return sendto(fd, buf, n, flags, a, al);
}
uint32_t G_umask_chk(uint32_t mode) {
  if (mode & ~0777u)
    FortifyFail("umask: called with invalid mask %o", mode);
  return umask(mode);
}
// fd_set: 1024 bits, as the host's.
void CheckFd(const char* fn, int fd, size_t set_size) {
  if (fd < 0)
    FortifyFail("%s: file descriptor %d < 0", fn, fd);
  if (fd >= 1024)
    FortifyFail("%s: file descriptor %d >= FD_SETSIZE", fn, fd);
  if (set_size < 128)
    FortifyFail("%s: set size %zu is too small to be an fd_set", fn, set_size);
}
void G_FD_SET_chk(int fd, uint32_t* set, size_t n) {
  CheckFd("FD_SET", fd, n);
  set[fd / 32] |= 1u << (fd % 32);
}
void G_FD_CLR_chk(int fd, uint32_t* set, size_t n) {
  CheckFd("FD_CLR", fd, n);
  set[fd / 32] &= ~(1u << (fd % 32));
}
int G_FD_ISSET_chk(int fd, const uint32_t* set, size_t n) {
  CheckFd("FD_ISSET", fd, n);
  return (set[fd / 32] >> (fd % 32)) & 1;
}

// --- system properties (__system_property_find/read) ------------------------------

// prop_info is opaque to callers: one interned record per name looked up.
struct PropInfo : GuestLayout64 {   // (opaque to the guest)
  char name[32];
  std::string key;
};
std::mutex g_prop_lock;
std::map<std::string, PropInfo*> g_props;

const PropInfo* G_system_property_find(const char* name) {
  char v[92];
  if (property_get(name, v, "") <= 0)
    return nullptr;
  std::lock_guard<std::mutex> l(g_prop_lock);
  PropInfo*& p = g_props[name];
  if (!p) {
    p = new PropInfo;
    snprintf(p->name, sizeof p->name, "%s", name);
    p->key = name;
  }
  return p;
}
int G_system_property_read(const PropInfo* pi, char* name, char* value) {
  if (!pi)
    return 0;
  if (name)
    snprintf(name, 32, "%s", pi->name);  // PROP_NAME_MAX
  char v[92];
  int n = property_get(pi->key.c_str(), v, "");
  if (value)
    memcpy(value, v, n + 1);  // PROP_VALUE_MAX (92)
  return n;
}
void G_system_property_read_callback(const PropInfo* pi, uint32_t cb, uint32_t cookie) {
  char v[92];
  property_get(pi->key.c_str(), v, "");
  uint32_t args[4] = {cookie, Addr(pi->name), Addr(v), 0};
  CallGuest(cb, args, 4);
}
uint32_t G_system_property_serial(const PropInfo*) { return 0; }

// --- getauxval: what a Linux kernel puts into the auxiliary vector -----------------

uint8_t g_at_random[16];
char g_at_platform[] = "v7l";
uint32_t G_getauxval(uint32_t type) {
  switch (type) {
    case 6: return 4096;  // AT_PAGESZ
    case 15: return Addr(g_at_platform);  // AT_PLATFORM
    case 16:  // AT_HWCAP: what the ARM CPU (shim/ndk/cpu) executes
      return (1 << 0) | (1 << 1) | (1 << 2) | (1 << 4) |  // SWP HALF THUMB FAST_MULT
             (1 << 6) | (1 << 7) |                        // VFP EDSP
             (1 << 12) | (1 << 13) | (1 << 15) |          // NEON VFPv3 TLS
             (1 << 16) | (1 << 17) | (1 << 18) | (1 << 19);  // VFPv4 IDIVA IDIVT VFPD32
    case 26: return 0;  // AT_HWCAP2: no AES/PMULL/SHA/CRC32
    case 17: return 100;  // AT_CLKTCK
    case 11: return getuid();  // AT_UID
    case 12: return geteuid();
    case 13: return getgid();
    case 14: return getegid();
    case 23: return 0;  // AT_SECURE
    case 25: return Addr(g_at_random);  // AT_RANDOM
    default:
      errno = ENOENT;
      return 0;
  }
}

// --- processes: one process, no fork/exec ---------------------------------------------

int Refuse(const char* what, int err) {
  __android_log_print(5, "ndk", "%s: not available (no processes in arc-wasm)", what);
  errno = err;
  return -1;
}
int G_fork() { return Refuse("fork", ENOSYS); }
int G_vfork() { return Refuse("vfork", ENOSYS); }
int G_execv(const char* path, uint32_t) { return Refuse(path ? path : "exec", ENOSYS); }
int G_execve(const char* path, uint32_t, uint32_t) { return Refuse(path ? path : "exec", ENOSYS); }
void G_execl(NdkCpuState* s, void*) {
  ThunkErrnoBegin();
  int r = Refuse(Ptr<const char>(s->r[0]) ? Ptr<const char>(s->r[0]) : "exec", ENOSYS);
  ThunkErrnoEnd();
  SetResult(s, r);
}
int G_wait(int32_t*) {
  errno = ECHILD;
  return -1;
}
int G_waitpid(int, int32_t*, int) {
  errno = ECHILD;
  return -1;
}
int G_waitid(int, int, void*, int) {
  errno = ECHILD;
  return -1;
}
int G_wait4(int, int32_t*, int, void*) {
  errno = ECHILD;
  return -1;
}
// system(NULL) asks whether there is a shell: no.
int G_system(const char* cmd) {
  if (!cmd)
    return 0;
  return Refuse("system", ENOSYS);
}
// ptrace: as when a seccomp policy or SELinux forbids it.
void G_ptrace(NdkCpuState* s, void*) {
  ThunkErrnoBegin();
  errno = EPERM;
  ThunkErrnoEnd();
  SetResult(s, -1);
}

// prctl(option, ...): names and the few flags apps set; the rest EINVAL.
thread_local char t_thread_name[16] = "";
void* NameSlot() { return t_thread_name; }   // a green thread's own (ndk_green.cc)
[[maybe_unused]] const bool kNameSlot = (RegisterGreenSlot(NameSlot, sizeof t_thread_name), true);
int DoPrctl(int option, uint32_t a2) {
  int r = 0;
  switch (option) {
    case 15:  // PR_SET_NAME
      snprintf(t_thread_name, sizeof t_thread_name, "%s", Ptr<const char>(a2));
      if (ThreadsLog()) fprintf(stderr, "ndk: guest thread named \"%s\" (prctl)\n", t_thread_name);
      break;
    case 16:  // PR_GET_NAME
      memcpy(Ptr<char>(a2), t_thread_name, sizeof t_thread_name);
      break;
    case 3:  // PR_GET_DUMPABLE
      r = 1;
      break;
    case 4:   // PR_SET_DUMPABLE
    case 1:   // PR_SET_PDEATHSIG
    case 8:   // PR_SET_KEEPCAPS
    case 22:  // PR_SET_SECCOMP
    case 29:  // PR_SET_TIMERSLACK
    case 36:  // PR_SET_CHILD_SUBREAPER
    case 38:  // PR_SET_NO_NEW_PRIVS
    case 0x53564d41:  // PR_SET_VMA (naming anonymous memory)
      break;
    case 2:  // PR_GET_PDEATHSIG
      if (a2) Store32(a2, 0);
      break;
    case 21:  // PR_GET_SECCOMP
    case 39:  // PR_GET_NO_NEW_PRIVS
      r = 0;
      break;
    default:
      errno = EINVAL;
      r = -1;
  }
  return r;
}
void G_prctl(NdkCpuState* s, void*) {
  ThunkErrnoBegin();
  int r = DoPrctl(s->r[0], s->r[1]);
  ThunkErrnoEnd();
  SetResult(s, r);
}

// Signals: guest handlers run on guest threads (shim/ndk/cpu/signal.cc); the
// CPU keeps each thread's alternate stack and starts SA_ONSTACK handlers
// there (here only without that CPU: noted).
struct GStack {
  uint32_t ss_sp;
  int32_t ss_flags;
  uint32_t ss_size;
};
thread_local GStack t_altstack = {0, 2 /* SS_DISABLE */, 0};
void* AltstackSlot() { return &t_altstack; }
[[maybe_unused]] const bool kAltstackSlot = (RegisterGreenSlot(AltstackSlot, sizeof t_altstack), true);
int G_sigaltstack(const GStack* ss, GStack* old) {
  static_assert(sizeof(GStack) == 12, "stack_t: ss_sp, ss_flags, ss_size");
  if (ndk_sig_altstack) {
    uint32_t prev[3];
    if (ndk_sig_altstack(ss ? reinterpret_cast<const uint32_t*>(ss) : nullptr, prev) < 0) {
      errno = EPERM;   // on that stack now
      return -1;
    }
    if (old) memcpy(old, prev, sizeof prev);
    return 0;
  }
  if (old)
    *old = t_altstack;
  if (ss)
    t_altstack = *ss;
  return 0;
}
uint32_t G_alarm(uint32_t) { return 0; }
// Interval timers deliver signals, which never come here (as alarm): set and
// get succeed, the old and current values read as disarmed (struct itimerval:
// four longs, 32 bits for armeabi guests).
int G_setitimer(int, const void*, uint32_t* old) {
  if (old)
    memset(old, 0, 16);
  return 0;
}
int G_getitimer(int, uint32_t* cur) {
  if (cur)
    memset(cur, 0, 16);
  return 0;
}
// pause/sigsuspend/sigwait wait for a signal (from another guest thread).
[[noreturn]] void SleepForever(const char* what) {
  __android_log_print(4, "ndk", "%s: no signals in arc-wasm, the thread sleeps for good", what);
  for (;;) sleep(3600);
}
int G_sigsuspend(const uint32_t* mask) {
  if (!ndk_sig_suspend) SleepForever("sigsuspend");
  uint64_t m = *mask;  // bionic's 32-bit sigset_t
  if (GuestSigsuspend(&m) == kParked) return 0;   // (a green thread: the call runs again)
  errno = EINTR;
  return -1;
}
int G_pause() {
  if (!ndk_sig_suspend || !ndk_sig_mask) SleepForever("pause");
  uint64_t m;
  ndk_sig_mask(0, nullptr, &m);
  if (GuestSigsuspend(&m) == kParked) return 0;
  errno = EINTR;
  return -1;
}
int G_sigwait(const uint32_t* set, int32_t* sig) {
  if (!ndk_sig_wait_set) SleepForever("sigwait");
  int r;
  while ((r = ndk_sig_wait_set(*set, INFINITY)) == -4 /* EINTR */) {}
  if (r < 0) return ToGuestErrno(EINVAL);
  *sig = r;
  return 0;
}
int G_tgkill(int tgid, int tid, int sig) {
  if (tgid == getpid() && (sig == 6 || sig == 9) && tid == GuestTid()) {
    __android_log_print(6, "ndk", "guest sent itself signal %d", sig);
    abort();
  }
  if (tgid != getpid() || !ndk_sig_send) {
    if (sig == 0 || !ndk_sig_send) return 0;
    errno = ESRCH;
    return -1;
  }
  int r = ndk_sig_send(tid, sig);
  if (r < 0) {
    errno = r == -3 ? ESRCH : EINVAL;
    return -1;
  }
  if (tid == GuestTid() && ndk_sig_deliver) ndk_sig_deliver();
  return 0;
}
int G_libc_current_sigrtmin() { return 32 + 3; }  // bionic 6: __SIGRTMIN + 3
int G_libc_current_sigrtmax() { return 64; }

std::string g_abort_message;
void G_android_set_abort_message(const char* msg) {
  if (msg && g_abort_message.empty()) {
    g_abort_message = msg;
    __android_log_print(6, "ndk", "abort message: %s", msg);
  }
}

// An executable's entry (__libc_init from crtbegin_dynamic.o): runs main
// with the kernel's argument block and exits, if a guest ever runs one.
void G_libc_init(uint32_t raw_args, uint32_t, uint32_t main_fn, uint32_t) {
  uint32_t argc = Load32(raw_args);
  uint32_t args[3] = {argc, raw_args + 4, raw_args + 4 + 4 * (argc + 1)};
  __android_log_print(4, "ndk", "running an executable's main (argc %u)", argc);
  exit((int32_t)CallGuest(main_fn, args, 3));
}

// --- scheduling --------------------------------------------------------------------

int G_sched_get_priority_max(int policy) { return (policy == 1 || policy == 2) ? 99 : 0; }
int G_sched_get_priority_min(int policy) { return (policy == 1 || policy == 2) ? 1 : 0; }
int G_sched_getscheduler(int) { return 0; }  // SCHED_OTHER
int G_sched_setscheduler(int, int, const int32_t*) { return 0; }
int G_sched_getparam(int, int32_t* p) {
  *p = 0;
  return 0;
}
int G_sched_setparam(int, const int32_t*) { return 0; }
int G_sched_getcpu() { return 0; }
// The mask is GuestCpuCount's CPUs (the kernel call returns the bytes
// written; the libc function returns 0).
int G_sched_getaffinity(int, size_t n, uint8_t* set) {
  if (n < 4) {
    errno = EINVAL;
    return -1;
  }
  GuestCpuMask(set, n);
  return 0;
}
int G_sched_setaffinity(int, size_t, const void*) { return 0; }
int G_sched_cpucount(size_t n, const uint8_t* set) {
  int c = 0;
  for (size_t i = 0; i < n; i++) c += __builtin_popcount(set[i]);
  return c;
}
int G_sched_rr_get_interval(int, int32_t* ts) {
  ts[0] = 0;
  ts[1] = 100000000;
  return 0;
}

// Nice values: remembered per thread id, nothing else.
std::mutex g_nice_lock;
std::map<int, int> g_nice;
int G_getpriority(int which, int who) {
  std::lock_guard<std::mutex> l(g_nice_lock);
  if (which == 0 /* PRIO_PROCESS */) {
    auto it = g_nice.find(who ? who : gettid());
    return it == g_nice.end() ? 0 : it->second;
  }
  return 0;
}
int G_setpriority(int which, int who, int prio) {
  std::lock_guard<std::mutex> l(g_nice_lock);
  if (which == 0)
    g_nice[who ? who : gettid()] = prio < -20 ? -20 : prio > 19 ? 19 : prio;
  return 0;
}

// --- resources and system information -------------------------------------------------

// bionic's struct rlimit: two unsigned longs (RLIM_INFINITY ~0UL).
struct GRlimit {
  uint32_t cur, max;
};
std::mutex g_rlimit_lock;
GRlimit g_rlimits[16] = {
    {~0u, ~0u}, {~0u, ~0u}, {~0u, ~0u},            // CPU FSIZE DATA
    {8 << 20, ~0u}, {0, ~0u}, {~0u, ~0u},          // STACK CORE RSS
    {~0u, ~0u}, {1024, 4096}, {64 << 10, 64 << 10},  // NPROC NOFILE MEMLOCK
    {~0u, ~0u}, {~0u, ~0u}, {~0u, ~0u},            // AS LOCKS SIGPENDING
    {819200, 819200}, {40, 40}, {0, 0}, {~0u, ~0u},  // MSGQUEUE NICE RTPRIO RTTIME
};
int G_getrlimit(int r, GRlimit* out) {
  if (r < 0 || r >= 16) {
    errno = EINVAL;
    return -1;
  }
  std::lock_guard<std::mutex> l(g_rlimit_lock);
  *out = g_rlimits[r];
  return 0;
}
int G_setrlimit(int r, const GRlimit* in) {
  if (r < 0 || r >= 16 || in->cur > in->max) {
    errno = EINVAL;
    return -1;
  }
  std::lock_guard<std::mutex> l(g_rlimit_lock);
  g_rlimits[r] = *in;
  return 0;
}
struct GRlimit64 {
  uint64_t cur, max;
};
uint64_t Wide(uint32_t v) { return v == ~0u ? ~0ull : v; }
int G_getrlimit64(int r, GRlimit64* out) {
  GRlimit g;
  if (G_getrlimit(r, &g) != 0)
    return -1;
  *out = {Wide(g.cur), Wide(g.max)};
  return 0;
}
int G_prlimit(int pid, int r, const GRlimit* in, GRlimit* old) {
  if (old && G_getrlimit(r, old) != 0)
    return -1;
  return in ? G_setrlimit(r, in) : 0;
}

// Heap in use, for ru_maxrss and sysinfo.
size_t HeapBytes() {
  struct mallinfo mi = mallinfo();
  return (size_t)mi.uordblks + (size_t)mi.hblkhd;
}
// bionic's struct rusage: two 32-bit timevals and 14 longs.
struct GRusage {
  int32_t utime_sec, utime_usec, stime_sec, stime_usec;
  int32_t maxrss, ixrss, idrss, isrss, minflt, majflt, nswap, inblock, oublock, msgsnd, msgrcv,
      nsignals, nvcsw, nivcsw;
};
static_assert(sizeof(GRusage) == 72, "bionic struct rusage");
int G_getrusage(int who, GRusage* r) {
  memset(r, 0, sizeof *r);
  struct timespec ts;
  clock_gettime(who == 1 /* RUSAGE_THREAD */ ? CLOCK_THREAD_CPUTIME_ID : CLOCK_PROCESS_CPUTIME_ID,
                &ts);
  r->utime_sec = (int32_t)ts.tv_sec;
  r->utime_usec = (int32_t)(ts.tv_nsec / 1000);
  r->maxrss = (int32_t)(HeapBytes() / 1024);
  return 0;
}
// bionic's struct tms: four 32-bit clock_t, in sysconf(_SC_CLK_TCK) units.
int32_t G_times(int32_t* t) {
  struct timespec ts;
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
  if (t) {
    t[0] = (int32_t)(ts.tv_sec * 100 + ts.tv_nsec / 10000000);
    t[1] = t[2] = t[3] = 0;
  }
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int32_t)(ts.tv_sec * 100 + ts.tv_nsec / 10000000);
}

// The device the guest sees: 2 GB of RAM (what an app may plan for).
const uint32_t kTotalRam = 2u << 30;
struct GSysinfo {
  int32_t uptime;
  uint32_t loads[3];
  uint32_t totalram, freeram, sharedram, bufferram, totalswap, freeswap;
  uint16_t procs, pad;
  uint32_t totalhigh, freehigh, mem_unit;
  char f[8];
};
static_assert(sizeof(GSysinfo) == 64, "bionic struct sysinfo");
int G_sysinfo(GSysinfo* si) {
  memset(si, 0, sizeof *si);
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  si->uptime = (int32_t)ts.tv_sec;
  si->totalram = kTotalRam;
  size_t used = HeapBytes();
  si->freeram = used < kTotalRam ? kTotalRam - (uint32_t)used : 0;
  si->procs = 1;
  si->mem_unit = 1;
  return 0;
}
struct GMallinfo {
  uint32_t arena, ordblks, smblks, hblks, hblkhd, usmblks, fsmblks, uordblks, fordblks, keepcost;
};
// A struct returned in memory: the address comes in r0.
void G_mallinfo(NdkCpuState* s, void*) {
  struct mallinfo mi = mallinfo();
  GMallinfo* g = Ptr<GMallinfo>(s->r[0]);
  *g = {(uint32_t)mi.arena, (uint32_t)mi.ordblks, (uint32_t)mi.smblks, (uint32_t)mi.hblks,
        (uint32_t)mi.hblkhd, (uint32_t)mi.usmblks, (uint32_t)mi.fsmblks, (uint32_t)mi.uordblks,
        (uint32_t)mi.fordblks, (uint32_t)mi.keepcost};
}

// struct utsname: six 65-byte fields on both sides.
struct GUtsname {
  char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65];
};
int G_uname(GUtsname* u) {
  memset(u, 0, sizeof *u);
  strcpy(u->sysname, "Linux");
  strcpy(u->nodename, "localhost");
  strcpy(u->release, "3.18.71-arcwasm");
  strcpy(u->version, "#1 SMP PREEMPT");
  strcpy(u->machine, "armv7l");
  strcpy(u->domainname, "localdomain");
  return 0;
}

// --- time ---------------------------------------------------------------------------

// time64.h: 64-bit time_t versions of the 32-bit-time functions.
struct tm* G_gmtime64_r(const int64_t* t, struct tm* out) {
  time_t h = *t;
  return gmtime_r(&h, out);
}
struct tm* G_localtime64_r(const int64_t* t, struct tm* out) {
  time_t h = *t;
  return localtime_r(&h, out);
}
struct tm* G_gmtime64(const int64_t* t) {
  time_t h = *t;
  return gmtime(&h);
}
struct tm* G_localtime64(const int64_t* t) {
  time_t h = *t;
  return localtime(&h);
}
int64_t G_mktime64(struct tm* tm) { return mktime(tm); }
int64_t G_timegm64(struct tm* tm) { return timegm(tm); }
int64_t G_timelocal64(struct tm* tm) { return mktime(tm); }
char* G_asctime64(const struct tm* tm) { return asctime(tm); }
char* G_ctime64(const int64_t* t) {
  time_t h = *t;
  return ctime(&h);
}
int32_t G_timelocal(struct tm* tm) { return (int32_t)mktime(tm); }

// --- syslog -> the Android log --------------------------------------------------------

std::mutex g_syslog_lock;
std::string g_syslog_ident;
int SyslogPrio(int p) {
  switch (p & 7) {
    case 0: case 1: case 2: return 7;  // EMERG ALERT CRIT -> FATAL
    case 3: return 6;                  // ERR
    case 4: return 5;                  // WARNING
    case 5: case 6: return 4;          // NOTICE INFO
    default: return 3;                 // DEBUG
  }
}
void G_openlog(const char* ident, int, int) {
  std::lock_guard<std::mutex> l(g_syslog_lock);
  g_syslog_ident = ident ? ident : "";
}
void G_closelog() {
  std::lock_guard<std::mutex> l(g_syslog_lock);
  g_syslog_ident.clear();
}
void Syslog(int prio, const char* fmt, ArgSource& a) {
  char* text = GuestFormatAlloc(fmt, a);
  std::string tag;
  {
    std::lock_guard<std::mutex> l(g_syslog_lock);
    tag = g_syslog_ident.empty() ? "syslog" : g_syslog_ident;
  }
  __android_log_write(SyslogPrio(prio), tag.c_str(), text);
  free(text);
}
void G_syslog(NdkCpuState* s, void*) {
  RegArgs a(s, 2);
  Syslog(s->r[0], Ptr<const char>(s->r[1]), a);
}
void G_vsyslog(int prio, const char* fmt, uint32_t ap) {
  VaArgs a(ap);
  Syslog(prio, fmt, a);
}
int G_setlogmask(int) { return 0xff; }

int G_log_buf_print(NdkCpuState* s) {
  RegArgs a(s, 4);
  char* text = GuestFormatAlloc(Ptr<const char>(s->r[3]), a);
  int r = __android_log_write(s->r[1], Ptr<const char>(s->r[2]) ? Ptr<const char>(s->r[2]) : "",
                              text);
  free(text);
  return r;
}
void G_android_log_buf_print(NdkCpuState* s, void*) { SetResult(s, G_log_buf_print(s)); }

// --- random -------------------------------------------------------------------------

void FillRandom(void* buf, size_t n) {
  if (getentropy(buf, n < 256 ? n : 256) == 0 && n <= 256)
    return;
  uint8_t* p = static_cast<uint8_t*>(buf);
  for (size_t i = 0; i < n; i += 256) {
    size_t c = n - i < 256 ? n - i : 256;
    if (getentropy(p + i, c) != 0) {
      for (size_t k = 0; k < c; k++) p[i + k] = (uint8_t)lrand48();
    }
  }
}
uint32_t G_arc4random() {
  uint32_t v;
  FillRandom(&v, sizeof v);
  return v;
}
void G_arc4random_buf(void* buf, size_t n) { FillRandom(buf, n); }
uint32_t G_arc4random_uniform(uint32_t upper) {
  if (upper < 2)
    return 0;
  uint32_t min = -upper % upper, r;
  do r = G_arc4random(); while (r < min);
  return r % upper;
}
int G_getentropy(void* buf, size_t n) {
  if (n > 256) {
    errno = EIO;
    return -1;
  }
  FillRandom(buf, n);
  return 0;
}

// --- libm and ARM EABI helpers bionic exports -----------------------------------------
// (long double is double on ARM.)

int G_ilogbf(float x) { return ilogbf(x); }
double G_nexttoward(double x, double y) { return nextafter(x, y); }
float G_nexttowardf(float x, double y) {
  return (double)x == y ? (float)y : nextafterf(x, x < y ? INFINITY : -INFINITY);
}
float G_remquof(float x, float y, int* q) { return remquof(x, y, q); }
double G_roundl(double x) { return round(x); }
double G_scalbln(double x, int32_t e) { return scalbln(x, e); }
float G_scalblnf(float x, int32_t e) { return scalblnf(x, e); }
long long G_llrintf(float x) { return llrintf(x); }
long long G_llroundf(float x) { return llroundf(x); }
// lldiv_t (16 bytes) is returned in memory at r0; the numerator is in r2:r3
// (an even register pair), the denominator on the stack.
void G_lldiv(NdkCpuState* s, void*) {
  RegArgs a(s, 1);
  int64_t n = (int64_t)a.U64(), d = (int64_t)a.U64();
  int64_t q = n / d, r = n % d;
  memcpy(Ptr<void>(s->r[0]), &q, 8);
  memcpy(Ptr<void>(s->r[0] + 8), &r, 8);
}
// __aeabi_idiv: r0 / r1 (division by zero gives 0, as libgcc's).
int32_t G_aeabi_idiv(int32_t n, int32_t d) {
  if (d == 0) return 0;
  if (n == INT32_MIN && d == -1) return INT32_MIN;
  return n / d;
}
uint32_t G_aeabi_uidiv(uint32_t n, uint32_t d) { return d ? n / d : 0; }
float G_aeabi_l2f(int64_t v) { return (float)v; }
float G_aeabi_ul2f(uint64_t v) { return (float)v; }
double G_aeabi_l2d(int64_t v) { return (double)v; }
double G_aeabi_ul2d(uint64_t v) { return (double)v; }

// --- misc ---------------------------------------------------------------------------

thread_local int t_h_errno;
int* G_get_h_errno() { return &t_h_errno; }
struct hostent* G_gethostbyname(const char* name) {
  struct hostent* h = gethostbyname(name);
  t_h_errno = h ? 0 : 1;  // HOST_NOT_FOUND
  return h;  // same layout: five pointer-sized fields
}
int G_inet_aton(const char* s, struct in_addr* a) { return inet_aton(s, a); }
// gethostbyname2: IPv4 through gethostbyname; no IPv6 host table.
struct hostent* G_gethostbyname2(const char* name, int af) {
  if (af == 2 /* AF_INET */)
    return G_gethostbyname(name);
  t_h_errno = 1;  // HOST_NOT_FOUND
  return nullptr;
}
// No reverse lookups (the relay resolves names only).
struct hostent* G_gethostbyaddr(const void*, socklen_t, int) {
  t_h_errno = 1;
  return nullptr;
}
// /etc/protocols' three entries NSS asks for (bionic's struct protoent:
// name, aliases, number).
struct GProtoent {
  uint32_t p_name, p_aliases;
  int32_t p_proto;
};
uint32_t g_no_aliases;
GProtoent g_protocols[3];
void InitProtocols() {
  static const char* const names[] = {"ip", "tcp", "udp"};
  static const int numbers[] = {0, 6, 17};
  for (int i = 0; i < 3; i++)
    g_protocols[i] = {Addr(names[i]), Addr(&g_no_aliases), numbers[i]};
}
GProtoent* G_getprotobyname(const char* name) {
  for (GProtoent& p : g_protocols) {
    if (name && strcmp(Ptr<const char>(p.p_name), name) == 0)
      return &p;
  }
  return nullptr;
}
GProtoent* G_getprotobynumber(int proto) {
  for (GProtoent& p : g_protocols) {
    if (p.p_proto == proto)
      return &p;
  }
  return nullptr;
}
// <netinet/in.h> data (API 24 for apps).
const uint8_t g_in6addr_any[16] = {0};
const uint8_t g_in6addr_loopback[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};

// getnameinfo: bionic's (BSD) NI_* bits differ from musl's.
int G_getnameinfo(const struct sockaddr* sa, socklen_t salen, char* host, socklen_t hostlen,
                  char* serv, socklen_t servlen, int gflags) {
  int flags = 0;
  if (gflags & 0x01) flags |= NI_NOFQDN;
  if (gflags & 0x02) flags |= NI_NUMERICHOST;
  if (gflags & 0x04) flags |= NI_NAMEREQD;
  if (gflags & 0x08) flags |= NI_NUMERICSERV;
  if (gflags & 0x10) flags |= NI_DGRAM;
  int r = getnameinfo(sa, salen, host, hostlen, serv, servlen, flags);
  switch (r) {  // bionic EAI_*
    case 0: return 0;
    case EAI_AGAIN: return 2;
    case EAI_FAIL: return 4;
    case EAI_FAMILY: return 5;
    case EAI_MEMORY: return 6;
    case EAI_OVERFLOW: return 14;
    case EAI_SYSTEM: return 11;
    default: return 8;  // EAI_NONAME
  }
}
// Network interfaces: only the loopback one.
uint32_t G_if_nametoindex(const char* name) { return strcmp(name, "lo") == 0 ? 1 : 0; }
char* G_if_indextoname(uint32_t index, char* name) {
  if (index != 1) {
    errno = ENXIO;
    return nullptr;
  }
  return strcpy(name, "lo");
}
// struct msghdr/cmsghdr have the same layout; CMSG_ALIGN is 4 bytes.
uint32_t G_cmsg_nxthdr(const uint32_t* msg, uint32_t cmsg) {
  uint32_t next = cmsg + ((Load32(cmsg) + 3) & ~3u);
  uint32_t control = msg[4], controllen = msg[5];  // msg_control, msg_controllen
  if (next + 12 > control + controllen)  // sizeof(struct cmsghdr)
    return 0;
  return next;
}
int G_cacheflush(uint32_t start, uint32_t end, int) {
  if (end > start)
    ndk_flush_code(start, end - start);
  return 0;
}


// --- arm64-v8a guests (topic ndk-arm64) -----------------------------------------
// What differs for an LP64 guest: callbacks on the AArch64 CPU, structures with
// longs and pointers (rusage, tms, sysinfo, mallinfo, hostent, protoent,
// stack_t, rlimit as is), variadic syslog/prctl/execl, long double in libm.

void G64_system_property_read_callback(const PropInfo* pi, uint64_t cb, uint64_t cookie) {
  char v[92];
  property_get(pi->key.c_str(), v, "");
  uint64_t args[4] = {cookie, Addr(pi->name), Addr(v), 0};
  CallGuest64(static_cast<uint32_t>(cb), args, 4);
}
void G64_prctl(NdkCpu64State* s, void*) {
  ThunkErrnoBegin();
  int r = DoPrctl(static_cast<int>(s->x[0]), GuestAddr64(s->x[1]));
  ThunkErrnoEnd();
  SetResult64(s, r);
}
void G64_ptrace(NdkCpu64State* s, void*) {
  ThunkErrnoBegin();
  errno = EPERM;
  ThunkErrnoEnd();
  SetResult64(s, (int64_t)-1);
}
void G64_execl(NdkCpu64State* s, void*) {
  ThunkErrnoBegin();
  int r = Refuse("execl", ENOSYS);
  ThunkErrnoEnd();
  SetResult64(s, r);
}
struct G64Stack : GuestLayout64 {
  uint64_t ss_sp;
  int32_t ss_flags, pad;
  uint64_t ss_size;
};
thread_local G64Stack t_altstack64 = {{}, 0, 2 /* SS_DISABLE */, 0, 0};
int G64_sigaltstack(const G64Stack* ss, G64Stack* old) {
  if (old) *old = t_altstack64;
  if (ss) t_altstack64 = *ss;
  return 0;
}
int G64_sched_rr_get_interval(int, int64_t* ts) {
  ts[0] = 0;
  ts[1] = 10000000;   // 10 ms
  return 0;
}
// struct rusage: two timevals, 14 longs.
int G64_getrusage(int who, int64_t* r) {
  struct rusage h;
  int rc = getrusage(who, &h);
  if (rc != 0) return rc;
  memset(r, 0, 144);
  r[0] = h.ru_utime.tv_sec; r[1] = h.ru_utime.tv_usec;
  r[2] = h.ru_stime.tv_sec; r[3] = h.ru_stime.tv_usec;
  r[4] = h.ru_maxrss;
  return 0;
}
// struct tms: four clock_t (long); the result is clock ticks since boot.
int64_t G64_times(int64_t* t) {
  struct timespec ts;
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
  int64_t cpu = ts.tv_sec * 100 + ts.tv_nsec / 10000000;
  if (t) {
    t[0] = cpu;
    t[1] = t[2] = t[3] = 0;
  }
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 100 + ts.tv_nsec / 10000000;
}
// struct sysinfo (LP64): a single 2 GiB machine, as the 32-bit one.
int G64_sysinfo(uint8_t* si) {
  memset(si, 0, 112);
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  int64_t uptime = ts.tv_sec;
  uint64_t total = 2048ull << 20, free_ram = 1024ull << 20;
  memcpy(si, &uptime, 8);                // uptime
  memcpy(si + 32, &total, 8);            // totalram
  memcpy(si + 40, &free_ram, 8);         // freeram
  uint16_t procs = 1;
  memcpy(si + 80, &procs, 2);            // procs
  uint32_t unit = 1;
  memcpy(si + 104, &unit, 4);            // mem_unit
  return 0;
}
// mallinfo returns 10 size_t: memory, at the address in x8 (AAPCS64).
void G64_mallinfo(NdkCpu64State* s, void*) {
  struct mallinfo h = mallinfo();
  uint64_t v[10] = {(uint64_t)h.arena, (uint64_t)h.ordblks, (uint64_t)h.smblks, (uint64_t)h.hblks,
                    (uint64_t)h.hblkhd, (uint64_t)h.usmblks, (uint64_t)h.fsmblks, (uint64_t)h.uordblks,
                    (uint64_t)h.fordblks, (uint64_t)h.keepcost};
  memcpy(Ptr<void>(s->x[8]), v, sizeof v);
}
int G64_uname(struct utsname* u) { return uname(u); }
int G64_getrlimit(int r, struct rlimit* out) { return getrlimit(r, out); }
int G64_setrlimit(int r, const struct rlimit*) { (void)r; return 0; }
int G64_prlimit(int, int r, const struct rlimit*, struct rlimit* old) { return old ? getrlimit(r, old) : 0; }
char* G64_asctime_r(const int32_t* tm, char* buf) {
  struct tm h;
  memset(&h, 0, sizeof h);
  memcpy(&h, tm, 9 * sizeof(int32_t));   // the first nine ints are the same
  return asctime_r(&h, buf);
}

// hostent/protoent with 8-byte pointers: built per thread.
struct G64Hostent : GuestLayout64 {
  uint64_t h_name, h_aliases;
  int32_t h_addrtype, h_length;
  uint64_t h_addr_list;
};
thread_local G64Hostent t_hostent;
thread_local uint64_t t_host_aliases[1];
thread_local uint64_t t_host_addrs[17];
G64Hostent* ToHostent64(struct hostent* h) {
  t_h_errno = h ? 0 : 1;
  if (!h) return nullptr;
  t_hostent.h_name = Addr(h->h_name);
  t_host_aliases[0] = 0;
  t_hostent.h_aliases = Addr(t_host_aliases);
  t_hostent.h_addrtype = h->h_addrtype;
  t_hostent.h_length = h->h_length;
  int i = 0;
  for (; h->h_addr_list && h->h_addr_list[i] && i < 16; i++) t_host_addrs[i] = Addr(h->h_addr_list[i]);
  t_host_addrs[i] = 0;
  t_hostent.h_addr_list = Addr(t_host_addrs);
  return &t_hostent;
}
G64Hostent* G64_gethostbyname(const char* name) { return ToHostent64(gethostbyname(name)); }
G64Hostent* G64_gethostbyname2(const char* name, int af) {
  if (af == 2) return G64_gethostbyname(name);
  t_h_errno = 1;
  return nullptr;
}
G64Hostent* G64_gethostbyaddr(const void*, socklen_t, int) {
  t_h_errno = 1;
  return nullptr;
}
struct G64Protoent : GuestLayout64 {
  uint64_t p_name, p_aliases;
  int32_t p_proto, pad;
};
uint64_t g_no_aliases64;
G64Protoent g_protocols64[3];
G64Protoent* G64_getprotobyname(const char* name) {
  for (G64Protoent& p : g_protocols64)
    if (name && strcmp(Ptr<const char>(p.p_name), name) == 0) return &p;
  return nullptr;
}
G64Protoent* G64_getprotobynumber(int proto) {
  for (G64Protoent& p : g_protocols64)
    if (p.p_proto == proto) return &p;
  return nullptr;
}
// CMSG_NXTHDR of LP64: struct msghdr (msg_control at 32, msg_controllen at
// 40), struct cmsghdr { size_t len; int level, type; } aligned to 8.
uint64_t G64_cmsg_nxthdr(const uint64_t* msg, uint64_t cmsg) {
  uint64_t next = cmsg + ((Load64(static_cast<uint32_t>(cmsg)) + 7) & ~7ull);
  uint64_t control = msg[4], controllen = msg[5];
  if (next + 16 > control + controllen) return 0;
  return next;
}
// libm functions with long double (IEEE quad on AArch64, as on wasm32).
double G64_nexttoward(double x, long double y) { return nexttoward(x, y); }
float G64_nexttowardf(float x, long double y) { return nexttowardf(x, y); }
long double G64_nexttowardl(long double x, long double y) { return nexttowardl(x, y); }
long double G64_roundl(long double x) { return roundl(x); }
long double G64_scalblnl(long double x, long e) { return scalblnl(x, e); }
int G64_vsyslog(int prio, const char* fmt, uint64_t ap) {
  VaArgs64 a(GuestAddr64(ap));
  char* text = GuestFormatAlloc(fmt, a);
  __android_log_write(prio <= 3 ? 6 : prio == 4 ? 5 : 4, "syslog", text);
  free(text);
  return 0;
}
void G64_syslog(NdkCpu64State* s, void*) {
  RegArgs64 a(s, 2);
  int prio = static_cast<int>(s->x[0]);
  char* text = GuestFormatAlloc(Ptr<const char>(s->x[1]), a);
  __android_log_write(prio <= 3 ? 6 : prio == 4 ? 5 : 4, "syslog", text);
  free(text);
}
void G64_android_log_buf_print(NdkCpu64State* s, void*) {
  RegArgs64 a(s, 4);
  int prio = static_cast<int>(s->x[1]);
  const char* tag = Ptr<const char>(s->x[2]);
  char* text = GuestFormatAlloc(Ptr<const char>(s->x[3]), a);
  int r = __android_log_write(prio, tag ? tag : "", text);
  free(text);
  SetResult64(s, r);
}

// Data: long timezone, char* tzname[2], char** environ (a 64-bit snapshot).
int64_t g_timezone64;
uint64_t g_tzname64[2];
uint64_t g_environ64;

// arm64 guests' environ: 64-bit pointers to the host's strings, made again
// after each change (the old arrays stay: a guest may hold one). Was made
// once: python-for-android's setenv (ANDROID_ARGUMENT...) before Python read
// os.environ went unseen, Kivy took itself for a desktop (Midistrum).
std::mutex g_environ64_lock;
void SyncEnviron64() {
  std::lock_guard<std::mutex> l(g_environ64_lock);
  size_t n = 0;
  while (::environ && ::environ[n]) n++;
  uint64_t* env = static_cast<uint64_t*>(calloc(n + 1, 8));
  for (size_t i = 0; i < n; i++) env[i] = Addr(::environ[i]);
  g_environ64 = Addr(env);
}
int G64_setenv(const char* name, const char* value, int overwrite) {
  int r = setenv(name, value, overwrite);
  SyncEnviron64();
  return r;
}
int G64_unsetenv(const char* name) {
  int r = unsetenv(name);
  SyncEnviron64();
  return r;
}
int G64_putenv(char* s) {
  int r = putenv(s);
  SyncEnviron64();
  return r;
}
int G64_clearenv() {
  int r = clearenv();
  SyncEnviron64();
  return r;
}

const HostSymbol kBionic64[] = {
    NDK_FN64("setenv", G64_setenv), NDK_FN64("unsetenv", G64_unsetenv),
    NDK_FN64("putenv", G64_putenv), NDK_FN64("clearenv", G64_clearenv),
    NDK_FN64("__system_property_read_callback", G64_system_property_read_callback),
    NDK_RAW64("prctl", G64_prctl), NDK_RAW64("ptrace", G64_ptrace),
    NDK_RAW64("execl", G64_execl), NDK_RAW64("execlp", G64_execl), NDK_RAW64("execle", G64_execl),
    NDK_FN64("sigaltstack", G64_sigaltstack), NDK_FN64("sched_rr_get_interval", G64_sched_rr_get_interval),
    NDK_FN64("getrusage", G64_getrusage), NDK_FN64("times", G64_times), NDK_FN64("sysinfo", G64_sysinfo),
    NDK_RAW64("mallinfo", G64_mallinfo), NDK_FN64("uname", G64_uname),
    NDK_FN64("getrlimit", G64_getrlimit), NDK_FN64("setrlimit", G64_setrlimit),
    NDK_FN64("getrlimit64", G64_getrlimit), NDK_FN64("setrlimit64", G64_setrlimit),
    NDK_FN64("prlimit", G64_prlimit), NDK_FN64("prlimit64", G64_prlimit),
    NDK_FN64("asctime_r", G64_asctime_r),
    NDK_DATA64("timezone", Addr(&g_timezone64)), NDK_DATA64("tzname", Addr(g_tzname64)),
    NDK_DATA64("environ", Addr(&g_environ64)),
    NDK_DATA64("in6addr_any", Addr(g_in6addr_any)), NDK_DATA64("in6addr_loopback", Addr(g_in6addr_loopback)),
    NDK_FN64("gethostbyname", G64_gethostbyname), NDK_FN64("gethostbyname2", G64_gethostbyname2),
    NDK_FN64("gethostbyaddr", G64_gethostbyaddr), NDK_FN64("getprotobyname", G64_getprotobyname),
    NDK_FN64("getprotobynumber", G64_getprotobynumber), NDK_FN64("__cmsg_nxthdr", G64_cmsg_nxthdr),
};
const HostSymbol kBionic64Raw[] = {
    NDK_RAW64("syslog", G64_syslog), NDK_FN64("vsyslog", G64_vsyslog),
};
const HostSymbol kBionicLibm64[] = {
    NDK_FN64("nexttoward", G64_nexttoward), NDK_FN64("nexttowardf", G64_nexttowardf),
    NDK_FN64("nexttowardl", G64_nexttowardl), NDK_FN64("roundl", G64_roundl), NDK_FN64("scalblnl", G64_scalblnl),
};
const HostSymbol kBionicLiblog64[] = {
    NDK_RAW64("__android_log_buf_print", G64_android_log_buf_print),
};

const HostSymbol kBionic[] = {
    // FORTIFY
    NDK_FNX("__memcpy_chk", G_memcpy_chk), NDK_FNX("__memmove_chk", G_memmove_chk),
    NDK_FNX("__memset_chk", G_memset_chk), NDK_FNX("__memchr_chk", G_memchr_chk),
    NDK_FNX("__memrchr_chk", G_memrchr_chk), NDK_FNX("__strcpy_chk", G_strcpy_chk),
    NDK_FNX("__stpcpy_chk", G_stpcpy_chk), NDK_FNX("__strncpy_chk", G_strncpy_chk),
    NDK_FNX("__strncpy_chk2", G_strncpy_chk2), NDK_FNX("__stpncpy_chk", G_stpncpy_chk),
    NDK_FNX("__stpncpy_chk2", G_stpncpy_chk2), NDK_FNX("__strcat_chk", G_strcat_chk),
    NDK_FNX("__strncat_chk", G_strncat_chk), NDK_FNX("__strlen_chk", G_strlen_chk),
    NDK_FNX("__strchr_chk", G_strchr_chk), NDK_FNX("__strrchr_chk", G_strrchr_chk),
    NDK_FNX("__strlcpy_chk", G_strlcpy_chk), NDK_FNX("__strlcat_chk", G_strlcat_chk),
    NDK_FN("__vsnprintf_chk", G_vsnprintf_chk), NDK_FN("__vsprintf_chk", G_vsprintf_chk),
    NDK_RAW("__snprintf_chk", G_snprintf_chk), NDK_RAW("__sprintf_chk", G_sprintf_chk),
    NDK_FNX("__read_chk", G_read_chk), NDK_FN("__pread_chk", G_pread_chk),
    // (LP64: off_t is 64 bits, __pread_chk is __pread64_chk; Kazumi's Flutter plugins)
    NDK_FN64("__pread_chk", G_pread64_chk),
    NDK_FNX("__pread64_chk", G_pread64_chk), NDK_FNX("__write_chk", G_write_chk),
    NDK_FNX("__readlink_chk", G_readlink_chk), NDK_FNX("__getcwd_chk", G_getcwd_chk),
    NDK_FNX("__poll_chk", G_poll_chk), NDK_FNX("__recvfrom_chk", G_recvfrom_chk),
    NDK_FNX("__sendto_chk", G_sendto_chk), NDK_FNX("__umask_chk", G_umask_chk),
    NDK_FNX("__FD_SET_chk", G_FD_SET_chk), NDK_FNX("__FD_CLR_chk", G_FD_CLR_chk),
    NDK_FNX("__FD_ISSET_chk", G_FD_ISSET_chk),
    // properties, auxv
    NDK_FNX("__system_property_find", G_system_property_find),
    NDK_FNX("__system_property_read", G_system_property_read),
    NDK_FN("__system_property_read_callback", G_system_property_read_callback),
    NDK_FNX("__system_property_serial", G_system_property_serial),
    NDK_FN("getauxval", G_getauxval),
    // processes and signals
    NDK_FNX("fork", G_fork), NDK_FNX("vfork", G_vfork), NDK_FNX("execv", G_execv),
    NDK_FNX("execvp", G_execv), NDK_FNX("execve", G_execve), NDK_FNX("execvpe", G_execve),
    NDK_RAW("execl", G_execl), NDK_RAW("execlp", G_execl), NDK_RAW("execle", G_execl),
    NDK_FNX("wait", G_wait), NDK_FNX("waitpid", G_waitpid), NDK_FNX("waitid", G_waitid),
    NDK_FNX("wait4", G_wait4), NDK_FNX("system", G_system), NDK_RAW("ptrace", G_ptrace),
    NDK_RAW("prctl", G_prctl), NDK_FN("sigaltstack", G_sigaltstack), NDK_FNX("alarm", G_alarm),
    NDK_FN("setitimer", G_setitimer), NDK_FN("getitimer", G_getitimer),
    NDK_FNX("pause", G_pause), NDK_FNX("sigwait", G_sigwait), NDK_FNX("sigsuspend", G_sigsuspend),
    NDK_FNX("tgkill", G_tgkill), NDK_FNX("__libc_current_sigrtmin", G_libc_current_sigrtmin),
    NDK_FNX("__libc_current_sigrtmax", G_libc_current_sigrtmax),
    NDK_FNX("android_set_abort_message", G_android_set_abort_message),
    NDK_FN("__libc_init", G_libc_init),
    // scheduling
    NDK_FNX("sched_get_priority_max", G_sched_get_priority_max),
    NDK_FNX("sched_get_priority_min", G_sched_get_priority_min),
    NDK_FNX("sched_getscheduler", G_sched_getscheduler),
    NDK_FNX("sched_setscheduler", G_sched_setscheduler),
    NDK_FNX("sched_getparam", G_sched_getparam), NDK_FNX("sched_setparam", G_sched_setparam),
    NDK_FNX("sched_getcpu", G_sched_getcpu), NDK_FNX("sched_getaffinity", G_sched_getaffinity),
    NDK_FNX("sched_setaffinity", G_sched_setaffinity), NDK_FNX("__sched_cpucount", G_sched_cpucount),
    NDK_FN("sched_rr_get_interval", G_sched_rr_get_interval),
    NDK_FNX("getpriority", G_getpriority), NDK_FNX("setpriority", G_setpriority),
    // resources, system information
    NDK_FN("getrlimit", G_getrlimit), NDK_FN("setrlimit", G_setrlimit),
    NDK_FN("getrlimit64", G_getrlimit64), NDK_FN("prlimit", G_prlimit),
    NDK_FN("getrusage", G_getrusage), NDK_FN("times", G_times), NDK_FN("sysinfo", G_sysinfo),
    NDK_RAW("mallinfo", G_mallinfo), NDK_FN("uname", G_uname),
    // time
    NDK_FN("gmtime64_r", G_gmtime64_r), NDK_FN("localtime64_r", G_localtime64_r),
    NDK_FN("gmtime64", G_gmtime64), NDK_FN("localtime64", G_localtime64),
    NDK_FN("mktime64", G_mktime64), NDK_FN("timegm64", G_timegm64),
    NDK_FN("timelocal64", G_timelocal64), NDK_FN("asctime64", G_asctime64),
    NDK_FN("ctime64", G_ctime64), NDK_FN("timelocal", G_timelocal),
    NDK_FN("asctime", ::asctime), NDK_FN("asctime_r", ::asctime_r), NDK_FN("strptime", ::strptime),
    NDK_DATA("timezone", Addr(&::timezone)), NDK_DATA("daylight", Addr(&::daylight)),
    NDK_DATA("tzname", Addr(&::tzname)), NDK_DATA("environ", Addr(&::environ)),
    // syslog
    NDK_FNX("openlog", G_openlog), NDK_FNX("closelog", G_closelog), NDK_RAW("syslog", G_syslog),
    NDK_FN("vsyslog", G_vsyslog), NDK_FNX("setlogmask", G_setlogmask),
    // random
    NDK_FNX("arc4random", G_arc4random), NDK_FNX("arc4random_buf", G_arc4random_buf),
    NDK_FNX("arc4random_uniform", G_arc4random_uniform), NDK_FNX("getentropy", G_getentropy),
    // misc
    NDK_FNX("__get_h_errno", G_get_h_errno), NDK_FN("gethostbyname", G_gethostbyname),
    NDK_FN("gethostbyname2", G_gethostbyname2), NDK_FN("gethostbyaddr", G_gethostbyaddr),
    NDK_FN("getprotobyname", G_getprotobyname), NDK_FN("getprotobynumber", G_getprotobynumber),
    NDK_DATA("in6addr_any", Addr(g_in6addr_any)),
    NDK_DATA("in6addr_loopback", Addr(g_in6addr_loopback)),
    NDK_FNX("inet_aton", G_inet_aton), NDK_FNX("getnameinfo", G_getnameinfo),
    NDK_FNX("if_nametoindex", G_if_nametoindex), NDK_FNX("if_indextoname", G_if_indextoname),
    NDK_FN("__cmsg_nxthdr", G_cmsg_nxthdr), NDK_FN("cacheflush", G_cacheflush),
    NDK_FNX("strcasestr", ::strcasestr),
    // ARM EABI helpers (bionic's libc.so exports libgcc's)
    NDK_FN("__aeabi_idiv", G_aeabi_idiv), NDK_FN("__aeabi_uidiv", G_aeabi_uidiv),
    NDK_FN("__aeabi_l2f", G_aeabi_l2f), NDK_FN("__aeabi_ul2f", G_aeabi_ul2f),
    NDK_FN("__aeabi_l2d", G_aeabi_l2d), NDK_FN("__aeabi_ul2d", G_aeabi_ul2d),
    NDK_RAW("lldiv", G_lldiv), NDK_RAW("imaxdiv", G_lldiv),
};

const HostSymbol kBionicLibm[] = {
    NDK_FNX("ilogbf", G_ilogbf), NDK_FN("nexttoward", G_nexttoward),
    NDK_FN("nexttowardf", G_nexttowardf), NDK_FN("nexttowardl", G_nexttoward),
    NDK_FNX("remquof", G_remquof), NDK_FN("roundl", G_roundl), NDK_FNX("scalbln", G_scalbln),
    NDK_FNX("scalblnf", G_scalblnf), NDK_FN("scalblnl", G_scalbln), NDK_FNX("llrintf", G_llrintf),
    NDK_FNX("llroundf", G_llroundf),
};

const HostSymbol kBionicLiblog[] = {
    NDK_RAW("__android_log_buf_print", G_android_log_buf_print),
};

}  // namespace

// Raw-syscall forms (ndk_syscall.cc, ndk64_libc.cc) of sigaltstack and
// prlimit64: Go's runtime makes these calls itself, and takes an "old" it
// did not get filled for a signal stack that is there.
int GuestSigaltstack(uint32_t ss, uint32_t old) {
  return G_sigaltstack(ss ? Ptr<const GStack>(ss) : nullptr, old ? Ptr<GStack>(old) : nullptr);
}
int GuestSigaltstack64(uint32_t ss, uint32_t old) {
  return G64_sigaltstack(ss ? Ptr<const G64Stack>(ss) : nullptr, old ? Ptr<G64Stack>(old) : nullptr);
}
// prlimit64(0, resource, new, old) with struct rlimit64: 0 or -1 and errno.
int GuestPrlimit64(int resource, uint32_t in, uint32_t old) {
  if (old && G_getrlimit64(resource, Ptr<GRlimit64>(old)) != 0)
    return -1;
  if (in) {
    const GRlimit64* n = Ptr<const GRlimit64>(in);
    GRlimit g = {n->cur > 0xffffffffull ? ~0u : (uint32_t)n->cur,
                 n->max > 0xffffffffull ? ~0u : (uint32_t)n->max};
    return G_setrlimit(resource, &g);
  }
  return 0;
}

// argc/argv/envp and the auxiliary vector after them, laid out as the kernel
// lays them out on a new process's stack: bionic's linker passes them to
// DT_INIT and DT_INIT_ARRAY functions (argc, argv, envp). Go's runtime, as a
// library, finds its auxv (AT_PAGESZ) past the end of envp. The 32-bit block
// (argv[0], NULL, envp..., NULL, auxv pairs, AT_NULL); envp is argv + 8.
uint32_t GuestProcessArgv() {
  static uint32_t argv = [] {
    static const uint32_t kAux[] = {6, 16, 26, 15, 25, 17, 11, 12, 13, 14, 23};
    const size_t naux = sizeof kAux / sizeof kAux[0];
    size_t envc = 0;
    while (::environ && ::environ[envc]) envc++;
    uint32_t* b = static_cast<uint32_t*>(calloc(2 + envc + 1 + 2 * (naux + 1), 4));
    size_t i = 0;
    b[i++] = Addr("app_process");
    b[i++] = 0;
    for (size_t e = 0; e < envc; e++) b[i++] = Addr(::environ[e]);
    b[i++] = 0;
    for (uint32_t t : kAux) {
      b[i++] = t;
      b[i++] = G_getauxval(t);
    }
    return Addr(b);  // (AT_NULL: the zeroed pair at the end)
  }();
  return argv;
}

void RegisterBionic() {
  FillRandom(g_at_random, sizeof g_at_random);
  InitProtocols();
  RegisterHostSymbols("libc.so", kBionic);
  RegisterHostSymbols("libm.so", kBionicLibm);
  RegisterHostSymbols("liblog.so", kBionicLiblog);
  // arm64-v8a halves.
  static const char* const kProtoNames[] = {"ip", "tcp", "udp"};
  static const int kProtoNumbers[] = {0, 6, 17};
  for (int i = 0; i < 3; i++) {
    g_protocols64[i].p_name = Addr(kProtoNames[i]);
    g_protocols64[i].p_aliases = Addr(&g_no_aliases64);
    g_protocols64[i].p_proto = kProtoNumbers[i];
  }
  g_timezone64 = ::timezone;
  g_tzname64[0] = Addr(::tzname[0]);
  g_tzname64[1] = Addr(::tzname[1]);
  // Emscripten's made-up user (HOME=/home/web_user, USER, LOGNAME) is not
  // Android's: an app process has none of them (init.environ.rc), and a
  // directory that is not there sent Python's expanduser astray.
  for (const char* name : {"HOME", "USER", "LOGNAME"}) {
    const char* v = getenv(name);
    if (v && strstr(v, "web_user"))
      unsetenv(name);
  }
  SyncEnviron64();
  RegisterHostSymbols("libc.so", kBionic64);
  RegisterHostSymbols("libc.so", kBionic64Raw);
  RegisterHostSymbols("libm.so", kBionicLibm64);
  RegisterHostSymbols("liblog.so", kBionicLiblog64);
}

}  // namespace ndk
