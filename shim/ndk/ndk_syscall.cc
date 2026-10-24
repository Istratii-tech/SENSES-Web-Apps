// Raw Linux EABI system calls (SVC #0 with the number in r7). Android NDK
// libraries normally reach the kernel through bionic, whose functions the
// rest of the bridge thunks; but some do not. TikTok 47.1.4's security
// libraries (libmetasec_ov.so, liboecsec_ov.so, libnpth*, libshadowhook,
// libbytehook) issue the syscalls themselves to keep bionic out of the
// loop -- openat/read/write/close/mmap2/mprotect/fstat64/getdents64/
// readlinkat/faccessat/clock_gettime/gettid/prctl and more. The ARM CPU
// (shim/ndk/cpu/) knows only a handful of numbers and otherwise returns
// -ENOSYS; this table, registered through ndk_set_syscall_handler, maps the
// common ARM EABI numbers (asm/unistd.h, __NR_SYSCALL_BASE == 0) to the
// bridge's existing implementations, using bionic's struct layouts and
// -Linux errno numbers as a raw syscall does.
//
// A handler runs before the CPU's own small table: it returns 1 when it
// handled the call (result, or a negative errno, already in r0) and 0 to
// let the CPU handle it (exit, write, getpid, gettid, gettimeofday,
// sched_yield, nanosleep, clock_gettime, set_tls) or report
// -ENOSYS once. Part of the arc-wasm shim layer; topic ndk-syscalls
// (docs/topics/ndk-imports.md).

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include <map>
#include <mutex>
#include <string>

#include <emscripten/threading.h>

// Signals to guest code (shim/ndk/cpu/ndk_cpu_ext.h; absent in the stub CPU).
struct NdkSigaction {
  uint32_t handler, flags;
  uint64_t mask;
};
extern "C" int ndk_sig_action(int sig, const NdkSigaction* act, NdkSigaction* old) __attribute__((weak));
extern "C" int ndk_sig_mask(int how, const uint64_t* set, uint64_t* old) __attribute__((weak));
extern "C" int ndk_sig_send(int tid, int sig) __attribute__((weak));
extern "C" int ndk_sig_deliver(void) __attribute__((weak));
extern "C" int ndk_sig_suspend(const uint64_t* mask) __attribute__((weak));
extern "C" int ndk_sig_wait_set(uint64_t set, double ms) __attribute__((weak));

#include "ndk_bridge.h"

extern "C" int __android_log_print(int prio, const char* tag, const char* fmt, ...);
// The CPU's raw-syscall hook (shim/ndk/cpu/ndk_cpu_ext.h); absent in the
// stub CPU, so it is referenced weakly.
typedef int (*NdkSyscallHandler)(NdkCpuState* state);
extern "C" void ndk_set_syscall_handler(NdkSyscallHandler fn) __attribute__((weak));

namespace ndk {
namespace {

// A negative guest (bionic) errno, as a raw syscall returns on failure.
int32_t GErr(int host_errno) { return -ToGuestErrno(host_errno); }

// The result of a host call that returns -1 with errno on failure.
int32_t Ret(long r) { return r < 0 ? GErr(errno) : (int32_t)r; }

// bionic's struct stat on ARM is the kernel's stat64 (104 bytes); fill it
// through the bridge's converter.
int32_t DoStat(const char* path, void* buf, bool follow) {
  struct stat h;
  if ((follow ? stat(path, &h) : lstat(path, &h)) != 0)
    return GErr(errno);
  ToGuestStatBuf(h, buf);
  return 0;
}

// --- getdents64: read a directory fd as linux_dirent64 records -----------------------------
//
// The kernel layer has no raw getdents, so the directory is read through a
// DIR opened on a dup of the guest fd (its own offset), cached per fd and
// closed at end of stream. Guest code that reads a directory to the end
// (as /proc scanners do) leaves nothing behind.

struct GDirent64 {  // bionic struct dirent (== Linux dirent64)
  uint64_t d_ino;
  int64_t d_off;
  uint16_t d_reclen;
  uint8_t d_type;
  char d_name[1];
};

std::mutex g_dir_lock;
std::map<int, DIR*>* g_dirs = new std::map<int, DIR*>;

int32_t GetDents64(int fd, void* buf, uint32_t count) {
  std::lock_guard<std::mutex> l(g_dir_lock);
  auto it = g_dirs->find(fd);
  if (it == g_dirs->end()) {
    int dup_fd = dup(fd);
    if (dup_fd < 0)
      return GErr(errno);
    DIR* d = fdopendir(dup_fd);
    if (!d) {
      int e = errno;
      close(dup_fd);
      return GErr(e);
    }
    it = g_dirs->emplace(fd, d).first;
  }
  DIR* d = it->second;
  uint8_t* out = static_cast<uint8_t*>(buf);
  uint32_t off = 0;
  for (;;) {
    long pos = telldir(d);
    errno = 0;
    struct dirent* e = readdir(d);
    if (!e) {
      if (errno && off == 0)
        return GErr(errno);  // a real error, nothing read yet
      break;                 // end of directory
    }
    size_t namelen = strlen(e->d_name);
    uint32_t reclen = (offsetof(GDirent64, d_name) + namelen + 1 + 7) & ~7u;
    if (off + reclen > count) {
      if (off == 0) {
        seekdir(d, pos);
        return GErr(EINVAL);  // the entry does not fit the buffer
      }
      seekdir(d, pos);  // next call starts here
      break;
    }
    GDirent64* g = reinterpret_cast<GDirent64*>(out + off);
    g->d_ino = e->d_ino;
    g->d_off = telldir(d);
    g->d_reclen = (uint16_t)reclen;
    g->d_type = e->d_type;
    memcpy(g->d_name, e->d_name, namelen + 1);
    off += reclen;
  }
  if (off == 0) {  // stream drained: drop the cached DIR
    closedir(d);
    g_dirs->erase(it);
  }
  return (int32_t)off;
}

void ForgetDir(int fd) {
  if (g_dirs->empty())
    return;
  std::lock_guard<std::mutex> l(g_dir_lock);
  auto it = g_dirs->find(fd);
  if (it != g_dirs->end()) {
    closedir(it->second);
    g_dirs->erase(it);
  }
}

// --- things the one process in wasm cannot do: logged once -----------------------------------

void LogOnce(uint32_t nr, const char* what) {
  static std::mutex m;
  static std::map<uint32_t, bool> seen;
  std::lock_guard<std::mutex> l(m);
  if (seen.emplace(nr, true).second)
    __android_log_print(5, "ndk", "syscall %u (%s): not supported", nr, what);
}

// mremap of a private mapping: grow by a fresh anonymous mapping with the
// contents (as ndk_fd.cc's mremap does); the address space cannot grow in
// place. MREMAP_MAYMOVE is bit 0.
int32_t DoMremap(uint32_t old_addr, uint32_t old_len, uint32_t new_len, int flags) {
  void* old = Ptr<void>(old_addr);
  if (new_len <= old_len) {
    if (new_len < old_len)
      munmap(static_cast<uint8_t*>(old) + new_len, old_len - new_len);
    return (int32_t)old_addr;
  }
  if (!(flags & 1))  // must move but may not
    return GErr(ENOMEM);
  void* n = mmap(nullptr, new_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (n == MAP_FAILED)
    return GErr(errno);
  memcpy(n, old, old_len);
  munmap(old, old_len);
  return (int32_t)Addr(n);
}

int32_t DoFcntl(int fd, int cmd, uint32_t arg) {
  switch (cmd) {
    case 3: {  // F_GETFL
      int f = fcntl(fd, F_GETFL);
      return f < 0 ? GErr(errno) : GuestOpenFlags(f);
    }
    case 4:  // F_SETFL
      return Ret(fcntl(fd, F_SETFL, HostOpenFlags(arg)));
    case 0:   // F_DUPFD
    case 1:   // F_GETFD
    case 2:   // F_SETFD
    case 1030:  // F_DUPFD_CLOEXEC
      return Ret(fcntl(fd, cmd, arg));
    default: {
      int r = FcntlLock(fd, cmd, arg);
      return r == -2 ? GErr(EINVAL) : Ret(r);
    }
  }
}

// futex: the operations guest code uses, on top of the host's. syscall()
// (ndk_libc.cc) comes here too. FUTEX_WAIT_BITSET / FUTEX_WAKE_BITSET are
// Rust std's Mutex, Condvar and thread parking (Gecko's Rust, app-services):
// without them every waiting Rust thread spun on ENOSYS. The bitset is not
// kept (every waiter matches: a spurious wakeup at most).
int32_t DoFutex(uint32_t addr, int op_flags, uint32_t val, uint32_t timeout_addr) {
  int op = op_flags & 0x7f;     // without FUTEX_PRIVATE_FLAG / FUTEX_CLOCK_REALTIME
  if (op == 0 || op == 9) {     // FUTEX_WAIT, FUTEX_WAIT_BITSET
    double ms = INFINITY;
    if (timeout_addr) {
      uint32_t sec = Load32(timeout_addr), nsec = Load32(timeout_addr + 4);
      ms = sec * 1000.0 + nsec / 1e6;
      if (op == 9) {            // an absolute time
        struct timespec now;
        clock_gettime((op_flags & 256) ? CLOCK_REALTIME : CLOCK_MONOTONIC, &now);
        ms -= now.tv_sec * 1000.0 + now.tv_nsec / 1e6;
        if (ms < 0) ms = 0;
      }
    }
    // 0 / -ETIMEDOUT / -EAGAIN / -EINTR (Linux); a green thread parks (kParked).
    return GuestFutexWaitAt(kSiteFutex, Ptr<void>(addr), val, ms);
  }
  if (op == 1 || op == 10) {    // FUTEX_WAKE, FUTEX_WAKE_BITSET
    int r = GuestFutexWake(Ptr<void>(addr), (int)val);
    return r < 0 ? 0 : r;
  }
  return GErr(ENOSYS);
}

// prctl: the advisory options guest code sets (seccomp, dumpable, VMA
// naming, ...). There is no kernel to keep them; the thread name lives in
// the guest (bionic's prctl thunk stores it), so the raw syscall need only
// accept them, as a locked-down device's SELinux would.
int32_t DoPrctl(int option, uint32_t a2) {
  switch (option) {
    case 3:  // PR_GET_DUMPABLE
      return 0;
    case 16:  // PR_GET_NAME: an empty name
      *Ptr<char>(a2) = 0;
      return 0;
    default:
      return 0;  // accept PR_SET_NAME, PR_SET_SECCOMP, PR_SET_VMA, ...
  }
}

// One call: r[0..5] the arguments; false = not in the table.
bool SyscallNr(uint32_t nr, uint32_t* r, int32_t* out) {
  std::string path;
  int32_t res;
  switch (nr) {
    // --- files -------------------------------------------------------------------------
    case 3:  res = Ret(FdRead(r[0], Ptr<void>(r[1]), r[2])); break;
    case 4:  res = Ret(FdWrite(r[0], Ptr<const void>(r[1]), r[2])); break;
    case 5:  res = Ret(FdOpen(Ptr<const char>(r[0]), r[1], r[2])); break;
    case 6:  ForgetDir(r[0]); res = Ret(FdClose(r[0])); break;
    case 10: res = Ret(unlink(Ptr<const char>(r[0]))); break;
    case 19: res = Ret((long)lseek(r[0], (int32_t)r[1], r[2])); break;
    case 33: res = Ret(access(Ptr<const char>(r[0]), r[1])); break;
    case 38: res = Ret(rename(Ptr<const char>(r[0]), Ptr<const char>(r[1]))); break;
    case 39: res = Ret(mkdir(Ptr<const char>(r[0]), r[1])); break;
    case 40: res = Ret(rmdir(Ptr<const char>(r[0]))); break;
    case 41: res = Ret(dup(r[0])); break;
    case 54: res = Ret(ioctl(r[0], r[1], Ptr<void>(r[2]))); break;
    case 55:                 // fcntl
    case 221: res = DoFcntl(r[0], r[1], r[2]); break;  // fcntl64
    case 63: res = Ret(dup2(r[0], r[1])); break;
    case 145: res = Ret(readv(r[0], Ptr<const struct iovec>(r[1]), r[2])); break;
    case 146: res = Ret(writev(r[0], Ptr<const struct iovec>(r[1]), r[2])); break;
    case 195: res = DoStat(Ptr<const char>(r[0]), Ptr<void>(r[1]), true); break;   // stat64
    case 196: res = DoStat(Ptr<const char>(r[0]), Ptr<void>(r[1]), false); break;  // lstat64
    case 197: {  // fstat64
      struct stat h;
      res = fstat(r[0], &h) == 0 ? (ToGuestStatBuf(h, Ptr<void>(r[1])), 0) : GErr(errno);
      break;
    }
    case 217: res = GetDents64(r[0], Ptr<void>(r[1]), r[2]); break;  // getdents64

    // --- *at (the kernel layer resolves only AT_FDCWD) ----------------------------------
    case 322:  // openat
      res = ResolveAtPath((int)r[0], Ptr<const char>(r[1]), &path)
                ? Ret(FdOpen(path.c_str(), r[2], r[3]))
                : GErr(errno);
      break;
    case 323:  // mkdirat
      res = ResolveAtPath((int)r[0], Ptr<const char>(r[1]), &path) ? Ret(mkdir(path.c_str(), r[2]))
                                                                   : GErr(errno);
      break;
    case 328:  // unlinkat
      if (ResolveAtPath((int)r[0], Ptr<const char>(r[1]), &path))
        res = Ret((r[2] & 0x200) ? rmdir(path.c_str()) : unlink(path.c_str()));  // AT_REMOVEDIR
      else
        res = GErr(errno);
      break;
    case 327:  // fstatat64: r2 buf, r3 flags (AT_SYMLINK_NOFOLLOW == 0x100)
      res = ResolveAtPath((int)r[0], Ptr<const char>(r[1]), &path)
                ? DoStat(path.c_str(), Ptr<void>(r[2]), !(r[3] & 0x100))
                : GErr(errno);
      break;
    case 332:  // readlinkat
      res = ResolveAtPath((int)r[0], Ptr<const char>(r[1]), &path)
                ? Ret(readlink(path.c_str(), Ptr<char>(r[2]), r[3]))
                : GErr(errno);
      break;
    case 334:  // faccessat: r2 mode
      res = ResolveAtPath((int)r[0], Ptr<const char>(r[1]), &path) ? Ret(access(path.c_str(), r[2]))
                                                                   : GErr(errno);
      break;

    // --- memory ------------------------------------------------------------------------
    case 192:  // mmap2: offset in 4 KiB pages
      res = (int32_t)Addr(GuestMmap(Ptr<void>(r[0]), r[1], r[2], r[3], (int)r[4],
                                    (int64_t)r[5] * 4096));
      if (res == (int32_t)Addr(MAP_FAILED))
        res = GErr(errno);
      break;
    case 91: res = Ret(GuestMunmap(Ptr<void>(r[0]), r[1])); break;
    case 125:  // mprotect: wasm memory has no protection; executable code is noted and flushed
      res = Ret(GuestMprotect(Ptr<void>(r[0]), r[1], (int)r[2]));
      break;
    case 163: res = DoMremap(r[0], r[1], r[2], (int)r[3]); break;  // mremap
    case 45: {  // brk: no heap break; allocation goes through mmap
      static uint32_t cur = 0;
      uint32_t want = r[0];
      if (want == 0)
        res = (int32_t)cur;
      else if (want <= cur)
        res = (int32_t)(cur = want);
      else
        res = GErr(ENOMEM);
      break;
    }

    // --- process and identity ----------------------------------------------------------
    case 24:   // getuid (old)
    case 199: res = getuid(); break;  // getuid32
    case 47:   // getgid (old)
    case 200: res = getgid(); break;  // getgid32
    case 49:   // geteuid (old)
    case 201: res = geteuid(); break;  // geteuid32
    case 50:   // getegid (old)
    case 202: res = getegid(); break;  // getegid32
    case 64: res = getppid(); break;
    case 172: res = DoPrctl((int)r[0], r[1]); break;  // prctl
    case 116: {  // sysinfo: a single 2 GiB machine
      memset(Ptr<void>(r[0]), 0, 64);
      uint64_t two_gb = 2ull << 30;
      memcpy(static_cast<uint8_t*>(Ptr<void>(r[0])) + 16, &two_gb, 4);  // totalram
      res = 0;
      break;
    }
    case 186: res = GuestSigaltstack(r[0], r[1]); break;  // sigaltstack: noted (ndk_bionic.cc)
    case 369: res = Ret(GuestPrlimit64((int)r[1], r[2], r[3])); break;  // prlimit64(pid, ...)
    case 220: res = Ret(GuestMadvise(Ptr<void>(r[0]), r[1], (int)r[2])); break;  // madvise: as libc's
    case 219: {  // mincore: every page of the address space is resident (Go finds the
                 // page size so when /proc/self/auxv is not there, as on Android)
      if (r[0] & 4095) {
        res = GErr(EINVAL);
        break;
      }
      memset(Ptr<void>(r[2]), 1, (r[1] + 4095) / 4096);
      res = 0;
      break;
    }
    case 122: {  // uname: what libc's uname says (ndk_bionic.cc); 6 fields of 65 (Go's runtime)
      char* u = Ptr<char>(r[0]);
      memset(u, 0, 6 * 65);
      const char* f[6] = {"Linux", "localhost", "3.18.71-arcwasm", "#1 SMP PREEMPT", "armv7l",
                          "localdomain"};
      for (int i = 0; i < 6; i++) strcpy(u + 65 * i, f[i]);
      res = 0;
      break;
    }
    case 2:   LogOnce(nr, "fork"); res = GErr(ENOSYS); break;
    case 11:  LogOnce(nr, "execve"); res = GErr(ENOSYS); break;
    case 120: LogOnce(nr, "clone"); res = GErr(ENOSYS); break;
    case 190: LogOnce(nr, "vfork"); res = GErr(ENOSYS); break;
    case 114: res = GErr(ECHILD); break;  // wait4: no children

    // --- scheduling and futex ----------------------------------------------------------
    case 241: res = 0; break;  // sched_setaffinity: any mask is fine
    case 242:                  // sched_getaffinity: GuestCpuCount CPUs
      if (r[1] < 4) {
        res = GErr(EINVAL);
      } else {
        GuestCpuMask(Ptr<void>(r[2]), r[1]);
        res = r[1] < 128 ? (int32_t)r[1] : 128;
      }
      break;
    case 240: res = DoFutex(r[0], (int)r[1], r[2], r[3]); break;  // futex

    // --- signals: guest handlers run on guest threads (shim/ndk/cpu/signal.cc) ---------
    case 67:     // sigaction(sig, act, oact): handler, mask (32), flags, restorer
    case 174: {  // rt_sigaction(sig, act, oact, size): handler, flags, restorer, mask (64)
      if (!ndk_sig_action) {
        if (r[2]) memset(Ptr<void>(r[2]), 0, nr == 174 ? 20 : 16);
        res = 0;
        break;
      }
      NdkSigaction a, old;
      if (r[1]) {
        const uint32_t* p = Ptr<uint32_t>(r[1]);
        a = nr == 174 ? NdkSigaction{p[0], p[1], p[3] | (uint64_t)p[4] << 32} : NdkSigaction{p[0], p[2], p[1]};
      }
      res = ndk_sig_action((int)r[0], r[1] ? &a : nullptr, &old);
      if (res == 0 && r[2]) {
        uint32_t* p = Ptr<uint32_t>(r[2]);
        if (nr == 174) {
          p[0] = old.handler; p[1] = old.flags; p[2] = 0;
          p[3] = (uint32_t)old.mask; p[4] = (uint32_t)(old.mask >> 32);
        } else {
          p[0] = old.handler; p[1] = (uint32_t)old.mask; p[2] = old.flags; p[3] = 0;
        }
      }
      break;
    }
    case 119:  // sigreturn
    case 173:  // rt_sigreturn: handlers return to the CPU's own stub instead
      res = 0;
      break;
    case 126:    // sigprocmask(how, set, oset): 32-bit sets
    case 175: {  // rt_sigprocmask(how, set, oset, size): 64-bit sets
      if (!ndk_sig_mask) {
        if (r[2]) memset(Ptr<void>(r[2]), 0, nr == 175 ? 8 : 4);
        res = 0;
        break;
      }
      uint64_t set = 0, old = 0;
      if (r[1]) set = nr == 175 ? (Load32(r[1]) | (uint64_t)Load32(r[1] + 4) << 32) : Load32(r[1]);
      res = ndk_sig_mask((int)r[0], r[1] ? &set : nullptr, &old);
      if (res == 0 && r[2]) {
        Store32(r[2], (uint32_t)old);
        if (nr == 175) Store32(r[2] + 4, (uint32_t)(old >> 32));
      }
      if (ndk_sig_deliver) ndk_sig_deliver();
      break;
    }
    case 72:     // sigsuspend(0, 0, mask)
    case 179: {  // rt_sigsuspend(set, size)
      if (!ndk_sig_suspend) {
        res = GErr(ENOSYS);
        break;
      }
      uint64_t mask = nr == 72 ? r[2] : (Load32(r[0]) | (uint64_t)Load32(r[0] + 4) << 32);
      res = GuestSigsuspend(&mask);
      break;
    }
    case 177: {  // rt_sigtimedwait(set, info, timeout, size)
      double ms = INFINITY;
      if (r[2]) ms = (int32_t)Load32(r[2]) * 1000.0 + (int32_t)Load32(r[2] + 4) / 1e6;
      if (!ndk_sig_wait_set) {
        if (!isinf(ms)) {  // no signal ever arrives: sleep out the timeout, then time out
          struct timespec ts = {(time_t)(ms / 1000), (long)fmod(ms, 1000.0) * 1000000};
          nanosleep(&ts, nullptr);
        }
        res = GErr(EAGAIN);
        break;
      }
      uint64_t set = Load32(r[0]) | (uint64_t)Load32(r[0] + 4) << 32;
      res = ndk_sig_wait_set(set, ms);
      if (res > 0 && r[1]) {  // siginfo: signo, errno, code (SI_TKILL), pid
        memset(Ptr<void>(r[1]), 0, 128);
        Store32(r[1], (uint32_t)res);
        Store32(r[1] + 8, (uint32_t)-6);
        Store32(r[1] + 12, (uint32_t)getpid());
      }
      break;
    }

    // --- time --------------------------------------------------------------------------
    case 266: {  // clock_nanosleep(clock, flags, request, remain)
      struct timespec req = {(time_t)(int32_t)Load32(r[2]), (long)(int32_t)Load32(r[2] + 4)}, rem;
      if (IsGreenThread()) {   // through the bridge's sleep (a green thread parks)
        double ms = req.tv_sec * 1000.0 + req.tv_nsec / 1e6, left = 0;
        if (r[1]) {   // TIMER_ABSTIME
          struct timespec now;
          clock_gettime((clockid_t)r[0], &now);
          ms -= now.tv_sec * 1000.0 + now.tv_nsec / 1e6;
        }
        int e = ms > 0 ? GuestSleep(ms, &left) : 0;
        if (e == kParked) { res = kParked; break; }
        if (e != 0 && !r[1] && r[3]) {
          Store32(r[3], (uint32_t)(left / 1000));
          Store32(r[3] + 4, (uint32_t)(fmod(left, 1000.0) * 1e6));
        }
        res = e;   // 0 or -EINTR (Linux)
        break;
      }
      int e = clock_nanosleep((clockid_t)r[0], r[1] ? TIMER_ABSTIME : 0, &req, &rem);
      if (e == 0) {
        res = 0;
      } else if (e == EINTR && !(r[1]) && r[3]) {
        Store32(r[3], (uint32_t)rem.tv_sec);
        Store32(r[3] + 4, (uint32_t)rem.tv_nsec);
        res = GErr(EINTR);
      } else {
        res = GErr(e);
      }
      break;
    }

    case 263: {  // clock_gettime(clock, ts) (the CPU does it for svc; this is syscall())
      struct timespec ts;
      if (clock_gettime((clockid_t)HostClockId((int)r[0]), &ts) != 0) {
        res = GErr(errno);
        break;
      }
      Store32(r[1], (uint32_t)ts.tv_sec);
      Store32(r[1] + 4, (uint32_t)ts.tv_nsec);
      res = 0;
      break;
    }
    case 264: {  // clock_getres(clock, res)
      if (r[1]) {
        Store32(r[1], 0);
        Store32(r[1] + 4, 1000);  // 1 us
      }
      res = 0;
      break;
    }

    // --- descriptors beyond dup2 -------------------------------------------------------
    case 358: res = Ret(dup3(r[0], r[1], HostOpenFlags(r[2]))); break;  // dup3
    case 359: res = Ret(pipe2(Ptr<int>(r[0]), HostOpenFlags(r[1]))); break;  // pipe2

    // --- random and signals to threads ------------------------------------------------
    case 0xf0002:  // ARM cacheflush(start, end, flags), also through libc's syscall()
      if (r[1] > r[0])
        FlushGuestCode(r[0], r[1] - r[0]);
      res = 0;
      break;
    case 385:  // memfd_create(name, flags)
      res = Ret(GuestMemfdCreate(Ptr<const char>(r[0]), r[1]));
      break;
    case 384: {  // getrandom(buf, len, flags): never blocks here
      uint8_t* p = Ptr<uint8_t>(r[0]);
      uint32_t n = r[1];
      for (uint32_t done = 0; done < n;) {
        uint32_t chunk = n - done < 256 ? n - done : 256;  // getentropy's limit
        if (getentropy(p + done, chunk) != 0) {
          res = GErr(errno);
          goto out;
        }
        done += chunk;
      }
      res = (int32_t)n;
      break;
    }
    case 268:    // tgkill(tgid, tid, sig)
    case 238: {  // tkill(tid, sig)
      int tid = (int)(nr == 268 ? r[1] : r[0]), sig = (int)(nr == 268 ? r[2] : r[1]);
      if (nr == 268 && (int)r[0] != getpid()) {
        res = GErr(ESRCH);
        break;
      }
      if (sig == 6 || sig == 9 || sig == 11) {  // as raise(): fatal
        if (tid == gettid()) {
          __android_log_print(6, "ndk", "guest sent itself signal %d", sig);
          abort();
        }
      }
      res = ndk_sig_send ? ndk_sig_send(tid, sig) : 0;
      if (res == 0 && tid == gettid() && ndk_sig_deliver) ndk_sig_deliver();
      break;
    }

    // --- sockets: as the libc thunks (Go's net package issues these itself) -------------
    // Same layouts on both sides (sockaddr, 32-bit msghdr), the same SOCK_* and MSG_* bits.
    case 281: res = Ret(socket((int)r[0], (int)r[1], (int)r[2])); break;
    case 282: res = Ret(bind(r[0], Ptr<const struct sockaddr>(r[1]), r[2])); break;
    case 283: res = Ret(connect(r[0], Ptr<const struct sockaddr>(r[1]), r[2])); break;
    case 284: res = Ret(listen(r[0], (int)r[1])); break;
    case 285: res = Ret(accept(r[0], Ptr<struct sockaddr>(r[1]), Ptr<socklen_t>(r[2]))); break;
    case 366: res = Ret(GuestAccept4(r[0], Ptr<struct sockaddr>(r[1]), Ptr<socklen_t>(r[2]), (int)r[3])); break;
    case 286: res = Ret(getsockname(r[0], Ptr<struct sockaddr>(r[1]), Ptr<socklen_t>(r[2]))); break;
    case 287: res = Ret(getpeername(r[0], Ptr<struct sockaddr>(r[1]), Ptr<socklen_t>(r[2]))); break;
    case 288: res = Ret(socketpair((int)r[0], (int)r[1], (int)r[2], Ptr<int>(r[3]))); break;
    case 289: res = Ret(send(r[0], Ptr<const void>(r[1]), r[2], (int)r[3])); break;
    case 290:
      res = Ret(sendto(r[0], Ptr<const void>(r[1]), r[2], (int)r[3], Ptr<const struct sockaddr>(r[4]), r[5]));
      break;
    case 291: res = Ret(recv(r[0], Ptr<void>(r[1]), r[2], (int)r[3])); break;
    case 292:
      res = Ret(recvfrom(r[0], Ptr<void>(r[1]), r[2], (int)r[3], Ptr<struct sockaddr>(r[4]), Ptr<socklen_t>(r[5])));
      break;
    case 293: res = Ret(shutdown(r[0], (int)r[1])); break;
    case 294: res = Ret(GuestSetsockopt(r[0], (int)r[1], (int)r[2], Ptr<const void>(r[3]), r[4])); break;
    case 295: res = Ret(GuestGetsockopt(r[0], (int)r[1], (int)r[2], Ptr<void>(r[3]), Ptr<socklen_t>(r[4]))); break;
    case 296: res = Ret(sendmsg(r[0], Ptr<const struct msghdr>(r[1]), (int)r[2])); break;
    case 297: res = Ret(recvmsg(r[0], Ptr<struct msghdr>(r[1]), (int)r[2])); break;
    case 376: LogOnce(nr, "process_vm_readv"); res = GErr(ENOSYS); break;

    default: {
      int fr;
      if (FdSyscall(nr, r, &fr)) {  // eventfd, timerfd, epoll
        res = Ret(fr);
        break;
      }
      return false;  // let the CPU handle it or report -ENOSYS once
    }
  }
out:
  *out = res;
  return true;
}

int Syscall(NdkCpuState* s) {
  int32_t res;
  if (!SyscallNr(s->r[7], s->r, &res))
    return 0;
  s->r[0] = (uint32_t)res;
  return 1;
}

}  // namespace

bool RawSyscall(uint32_t nr, uint32_t* args, int32_t* guest_res) {
  return SyscallNr(nr, args, guest_res);
}

void RegisterSyscalls() {
  if (ndk_set_syscall_handler)  // the real CPU has it; the stub does not
    ndk_set_syscall_handler(Syscall);
}

}  // namespace ndk
