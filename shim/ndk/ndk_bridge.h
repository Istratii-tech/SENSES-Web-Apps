// The NDK bridge: everything around the ARM CPU that lets Dalvik load an
// Android NDK library (an ARM ELF .so) and lets Java and guest code call
// each other. Contract with the CPU: ndk_cpu.h (guest address == host
// address, AAPCS soft-float).
//
//   ndk_linker.cc   ELF loader + dynamic linker, guest dlopen/dlsym/...
//   ndk_libc.cc     bionic libc ABI on top of musl and the arc-wasm kernel
//   ndk_stdio.cc    bionic FILE (__sF, inline getc/putc), printf engine
//   ndk_pthread.cc  bionic pthread objects mapped to host ones
//   ndk_libm.cc     libm (soft-float: doubles in core register pairs)
//   ndk_misc.cc     libstdc++ (new/delete, __cxa_guard_*), liblog, libz
//   ndk_gles.cc     libGLESv2 -> host gl* of the current thread's context
//   ndk_jni.cc      guest-visible JNIEnv/JavaVM, Dalvik <-> guest calls
//
// Part of the arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_NDK_NDK_BRIDGE_H_
#define ARC_WASM_SHIM_NDK_NDK_BRIDGE_H_

#include <stddef.h>
#include <dirent.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <utime.h>
#include <wchar.h>
#include <jni.h>

struct stat;
struct rlimit;
struct utsname;
struct in_addr;
struct in6_addr;
struct pollfd;

#include <atomic>
#include <string>
#include <tuple>
#include <utility>
#include <vector>
#include <type_traits>
#include <utility>

#include "ndk_cpu.h"
#include "ndk_cpu64.h"

// From the CPU's extensions (cpu/ndk_cpu_ext.h, which this include path
// does not reach).
extern "C" int ndk_call_guest_guarded(uint32_t addr, const uint32_t* args, int nargs,
                                      uint64_t* result, char* why, size_t why_size);
extern "C" void ndk_leave_guest(const char* why);

namespace ndk {

// --- guest memory -------------------------------------------------------------

// The wasm address of an arm64 guest's address: its low 32 bits, plus, in a
// reservation's window above 4 GB, its 64 KB's entry in the window's table
// (ndk_libc.cc; the CPU's tables, shim/ndk/cpu64).
extern "C" uint32_t* ndk64_window_table[64] __attribute__((weak));
inline uint32_t GuestAddr64(uint64_t a) {
  const uint32_t low = static_cast<uint32_t>(a);
  return (a >> 32) && ndk64_window_table ? low + ndk64_window_table[(a >> 36) & 63][(a >> 16) & 0xfffff] : low;
}

template <typename T>
inline T* Ptr(uint32_t addr) {
  return reinterpret_cast<T*>(static_cast<uintptr_t>(addr));
}
// A 64-bit value -- an arm64 guest's pointer, a field of its structures --
// is its memory in a reservation's window too (the same as the 32-bit one
// below 4 GB).
template <typename T, typename A, typename std::enable_if<std::is_integral<A>::value && sizeof(A) == 8, int>::type = 0>
inline T* Ptr(A addr) {
  return Ptr<T>(GuestAddr64(static_cast<uint64_t>(addr)));
}
inline uint32_t Addr(const void* p) {
  return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p));
}
inline uint32_t Load32(uint32_t addr) {
  uint32_t v;
  memcpy(&v, Ptr<void>(addr), 4);
  return v;
}
inline void Store32(uint32_t addr, uint32_t v) { memcpy(Ptr<void>(addr), &v, 4); }

template <typename To, typename From>
inline To BitCast(From f) {
  static_assert(sizeof(To) == sizeof(From), "size");
  To t;
  memcpy(&t, &f, sizeof t);
  return t;
}

// --- arguments ------------------------------------------------------------------

// A source of AAPCS arguments: 32-bit words, 64-bit values on an even word.
// The arm64-v8a sources (RegArgs64, VaArgs64) read integer and
// floating-point arguments from separate registers, and their long and
// pointers are 64 bits wide (Lp64()).
class ArgSource {
 public:
  virtual ~ArgSource() {}
  virtual uint32_t U32() = 0;
  virtual uint64_t U64() = 0;
  // A double (a variadic float arrives as one).
  virtual double F64() { return BitCast<double>(U64()); }
  // A long double: the same as double on ARM, IEEE quad on AArch64.
  virtual long double F128() { return F64(); }
  virtual bool Lp64() const { return false; }
  // A pointer (for Ptr: 64 bits from an arm64 guest, see GuestAddr64).
  virtual uint64_t Pointer() { return U32(); }
};

// The arguments of a call that just entered a host stub: r0-r3, then the
// stack at sp. |first| skips leading words (e.g. the JNIEnv). Word n of the
// stack is 8-aligned exactly when n is even (sp is 8-aligned at a call), so
// 64-bit values just start on an even word in both places.
class RegArgs : public ArgSource {
 public:
  explicit RegArgs(const NdkCpuState* s, int first = 0) : s_(s), n_(first) {}
  uint32_t U32() override { return Word(n_++); }
  uint64_t U64() override {
    n_ = (n_ + 1) & ~1;
    uint64_t lo = Word(n_++);
    return lo | static_cast<uint64_t>(Word(n_++)) << 32;
  }
  uint32_t Word(int n) const {
    return n < 4 ? s_->r[n] : Load32(s_->r[13] + 4 * (n - 4));
  }
  int next() const { return n_; }

 private:
  const NdkCpuState* s_;
  int n_;
};

// A guest va_list (ARM EABI: a pointer into the argument area, where 64-bit
// values are 8-aligned).
class VaArgs : public ArgSource {
 public:
  explicit VaArgs(uint32_t ap) : p_(ap) {}
  uint32_t U32() override {
    uint32_t v = Load32(p_);
    p_ += 4;
    return v;
  }
  uint64_t U64() override {
    p_ = (p_ + 7) & ~7u;
    uint64_t lo = Load32(p_);
    uint64_t hi = Load32(p_ + 4);
    p_ += 8;
    return lo | hi << 32;
  }

 private:
  uint32_t p_;
};

// A guest pointer a host function takes as its wasm address: an arm64
// guest's is translated as a pointer (GuestAddr64), where a uint32_t
// argument would be its low 32 bits.
struct GuestAddrArg {
  uint32_t a;
  operator uint32_t() const { return a; }
};

template <typename T>
inline T GetArg(ArgSource& a) {
  if constexpr (std::is_same<T, GuestAddrArg>::value) {
    return GuestAddrArg{a.U32()};
  } else if constexpr (std::is_same<T, double>::value) {
    return BitCast<double>(a.U64());
  } else if constexpr (std::is_same<T, float>::value) {
    return BitCast<float>(a.U32());
  } else if constexpr (std::is_pointer<T>::value) {
    return reinterpret_cast<T>(static_cast<uintptr_t>(a.U32()));
  } else if constexpr (sizeof(T) == 8) {
    return static_cast<T>(a.U64());
  } else {
    return static_cast<T>(a.U32());  // narrower types: low bits, as AAPCS
  }
}

template <typename T>
inline void SetResult(NdkCpuState* s, T v) {
  if constexpr (std::is_same<T, double>::value) {
    uint64_t b = BitCast<uint64_t>(v);
    s->r[0] = static_cast<uint32_t>(b);
    s->r[1] = static_cast<uint32_t>(b >> 32);
  } else if constexpr (std::is_same<T, float>::value) {
    s->r[0] = BitCast<uint32_t>(v);
  } else if constexpr (std::is_pointer<T>::value) {
    s->r[0] = Addr(v);
  } else if constexpr (sizeof(T) == 8) {
    s->r[0] = static_cast<uint32_t>(v);
    s->r[1] = static_cast<uint32_t>(static_cast<uint64_t>(v) >> 32);
  } else if constexpr (std::is_signed<T>::value) {
    s->r[0] = static_cast<uint32_t>(static_cast<int32_t>(v));  // sign-extend
  } else {
    s->r[0] = static_cast<uint32_t>(v);  // zero-extend
  }
}

// --- errno --------------------------------------------------------------------

// Linux (bionic) errno numbers <-> the host's (Emscripten: WASI numbering).
int ToGuestErrno(int host);
int ToHostErrno(int guest);
// The calling thread's guest errno (what bionic's __errno() points to).
int* GuestErrno();
inline void SetGuestErrno(int host_errno) { *GuestErrno() = ToGuestErrno(host_errno); }

// FORTIFY (_FORTIFY_SOURCE) failure: logs like bionic's __fortify_fatal and
// aborts.
[[noreturn]] void FortifyFail(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// --- file descriptors (ndk_fd.cc) and helpers of ndk_libc.cc -----------------

// read/write/close as guest code sees them (eventfd/timerfd are emulated).
ssize_t FdRead(int fd, void* buf, size_t n);
ssize_t FdWrite(int fd, const void* buf, size_t n);
int FdClose(int fd);
// open() with bionic's flags; remembers the path for the *at() functions.
int FdOpen(const char* path, int guest_flags, int mode);
void NoteFdPath(int fd, const char* path);
// A descriptor open for writing (stdio's fopen): IN_CLOSE_WRITE when closed.
void NoteFdWrite(int fd);
// Before closing fd outside FdClose (fclose): forgets its path, inotify events.
void FdForget(int fd);
int HostOpenFlags(int guest_flags);
int GuestOpenFlags(int host_flags);
// fcntl record locks (ndk_fd.cc): 0 / -1 with errno, -2 = not a lock command.
int FcntlLock(int fd, int cmd, uint32_t arg);
// eventfd/timerfd/epoll by ARM system call number (ndk_fd.cc); result -1/errno.
bool FdSyscall(uint32_t nr, const uint32_t* args, int* result);
// The raw svc table (ndk_syscall.cc) for libc's syscall(): args[0..5];
// *guest_res = result or -(bionic errno). false = not in the table.
bool RawSyscall(uint32_t nr, uint32_t* args, int32_t* guest_res);
// The process's argv as bionic's linker gives it to library constructors
// (argc 1; envp and the auxiliary vector follow): ndk_bionic.cc, ndk64_libc.cc.
// A Linux clock id as the host's clock_gettime takes it: Emscripten has
// REALTIME, MONOTONIC and the CPU-time clocks; the raw, coarse and boot-time
// ones (Kivy's clock asks for MONOTONIC_RAW) are their nearest.
inline int HostClockId(int clk) {
  switch (clk) {
    case 4: case 6: case 7: case 9: return 1;  // MONOTONIC_RAW/_COARSE, BOOTTIME(_ALARM)
    case 5: case 8: case 11: return 0;         // REALTIME_COARSE/_ALARM, TAI
    default: return clk;
  }
}
uint32_t GuestProcessArgv();
uint32_t GuestProcessArgv64() __attribute__((weak));  // (no arm64 bridge on 4.4/6.0.1)
bool GuestThreadLp64();
int GuestSigaltstack(uint32_t ss, uint32_t old);     // ndk_bionic.cc
int GuestSigaltstack64(uint32_t ss, uint32_t old);
int GuestPrlimit64(int resource, uint32_t in, uint32_t old);
// Socket calls as the 32-bit libc thunks make them (ndk_libc.cc: bionic's
// SO_RCVTIMEO/SO_SNDTIMEO; accept4's flags), for raw syscalls.
int GuestSetsockopt(int fd, int level, int optname, const void* optval, socklen_t optlen);
int GuestGetsockopt(int fd, int level, int optname, void* optval, socklen_t* optlen);
int GuestAccept4(int fd, struct sockaddr* a, socklen_t* al, int flags);
// The path a *at(dirfd, path) call means (joins the directory fd's path for
// the raw-syscall handler); false with errno set for an unknown fd.
bool ResolveAtPath(int dirfd, const char* path, std::string* out);
// Copies a host struct stat into bionic's layout (104 bytes on ARM).
void ToGuestStatBuf(const struct stat& host, void* guest);
// Android's names of user and group ids (root, system ..., u0_aNN) and back,
// as bionic's getgrgid/getgrnam answer (ndk_libc8.cc; -1: not one).
void NdkAndroidIdName(uint32_t id, char* buf, size_t len);
int64_t NdkAndroidIdOf(const char* name);
// bionic's getpwuid for such an id (no /etc/passwd on Android): the name,
// gid = uid, home "/" (an app's: "/data"), shell /system/bin/sh. Per thread.
struct passwd* NdkAndroidPasswd(uint32_t uid);
// mmap as guest code sees it (code mappings flushed for the CPU).
void* GuestMmap(void* addr, size_t len, int prot, int flags, int fd, int64_t off);
// The same for an arm64 guest, with its 64-bit addresses and lengths: a
// PROT_NONE reservation from the budget's guest_reserve_mb up is a window of
// addresses above 4 GB, its wasm memory comes as the guest commits it
// (ndk_libc.cc). ~0 and errno on failure.
uint64_t GuestMmap64(uint64_t addr, uint64_t len, int prot, int flags, int fd, int64_t off);
int GuestMunmap64(uint64_t addr, uint64_t len);
// The wasm memory mprotect (|prot|) or madvise (|prot| -1) of an arm64 guest
// acts on (in a window, access gets memory first); false (errno) where there
// is none or no more.
bool GuestMemRange64(uint64_t addr, uint64_t len, int prot, std::vector<std::pair<uint32_t, uint32_t>>* low);
// Whether all of an arm64 guest's [addr, addr + len) is memory (ndk_libc.cc).
bool GuestHasMemory64(uint64_t addr, uint64_t len);
// The guest's madvise and mprotect (4 KB pages; protection is not enforced).
int GuestMadvise(void* addr, size_t len, int advice);
int GuestMprotect(void* addr, size_t len, int prot);
// munmap for guest code, in 4 KB pages (ndk_libc.cc).
int GuestMunmap(void* addr, size_t len);
// Code in [addr, addr + len) may have changed (cacheflush, mprotect to
// PROT_EXEC, mappings): both CPUs retire their blocks there whose code did
// (topic guest-jit, shim/ndk/ndk_code_index.h).
void FlushGuestCode(uint32_t addr, size_t len);
// memfd_create: a region of /dev/ashmem (ndk_libc.cc).
int GuestMemfdCreate(const char* name, unsigned flags);

// --- thunks ------------------------------------------------------------------------

// Calls host function F with the guest's arguments read per its C type and
// stores its result. Host errno set by F becomes the guest's errno.
template <auto F>
struct Thunk;
template <typename R, typename... A, R (*F)(A...)>
struct Thunk<F> {
  static void Call(NdkCpuState* s, void*);
};
void ThunkErrnoBegin();
void ThunkErrnoEnd();

template <typename R, typename... A, R (*F)(A...)>
void Thunk<F>::Call(NdkCpuState* s, void*) {
  RegArgs a(s);
  std::tuple<A...> args{GetArg<A>(a)...};  // braced list: left to right
  ThunkErrnoBegin();
  if constexpr (std::is_void<R>::value) {
    std::apply(F, args);
    ThunkErrnoEnd();
  } else {
    R r = std::apply(F, args);
    ThunkErrnoEnd();
    SetResult(s, r);
  }
}

// A symbol a host library exports to guest code: a function (|fn|) or data
// at a fixed address (|data|), for armeabi(-v7a) guests, and the same for
// arm64-v8a guests (|fn64|, |data64|; see "arm64-v8a" below). An entry may
// have either half or both; the halves of entries with the same name merge.
struct HostSymbol {
  const char* name;
  NdkHostFn fn;
  void* user;
  uint32_t data;
  Ndk64HostFn fn64;
  uint32_t data64;
};
#define NDK_FN(name, f) {name, &::ndk::Thunk<f>::Call, nullptr, 0}
#define NDK_RAW(name, fn) {name, fn, nullptr, 0}
#define NDK_DATA(name, addr) {name, nullptr, nullptr, (addr)}
// Both ABIs: F's C types mean the same for an LP64 guest (Thunk64 converts
// widths: pointers, long and size_t; see CheckedLp64 for what it refuses).
#define NDK_FNX(name, f) {name, &::ndk::Thunk<f>::Call, nullptr, 0, &::ndk::Thunk64<f>::Call, 0}
// arm64-v8a only.
#define NDK_FN64(name, f) {name, nullptr, nullptr, 0, &::ndk::Thunk64<f>::Call, 0}
#define NDK_RAW64(name, fn) {name, nullptr, nullptr, 0, fn, 0}
#define NDK_DATA64(name, addr) {name, nullptr, nullptr, 0, nullptr, (addr)}

// Registers |syms| as exports of the host library |lib| ("libc.so", ...).
void RegisterHostSymbols(const char* lib, const HostSymbol* syms, size_t n);
template <size_t N>
inline void RegisterHostSymbols(const char* lib, const HostSymbol (&syms)[N]) {
  RegisterHostSymbols(lib, syms, N);
}

// As RegisterHostSymbols, but only the names nobody registered yet: the
// bridge's stand-ins for libraries other topics implement for real, in
// files that register themselves from a static constructor (fw8-graphics'
// ndk_sync.cc and ndk_hardware_buffer.cc, fw8-crypto-media's libmediandk).
void RegisterDefaultHostSymbols(const char* lib, const HostSymbol* syms, size_t n);
template <size_t N>
inline void RegisterDefaultHostSymbols(const char* lib, const HostSymbol (&syms)[N]) {
  RegisterDefaultHostSymbols(lib, syms, N);
}

// |alias| becomes an export of |lib| with the same code or data as the host
// symbol |original| (bionic's foo64 for foo where the types agree, the long
// double math of ARM, where long double is double). False if |original| is
// unknown or |alias| exists already.
bool AliasHostSymbol(const char* lib, const char* alias, const char* original);

// Host libraries beyond the bridge's own (shim/ndk/ndk_host_libs.h): other
// topics' (libmediandk, libaaudio...) register here before EnsureInit.
struct HostLibraryProvider {
  const char* name;
  void (*register_symbols)();
  int priority;  // higher registers later: its symbols replace a stub's
  HostLibraryProvider* next;
};
void AddHostLibraryProvider(HostLibraryProvider* provider);

// Registration functions of the thunk files (called once by EnsureInit).
void RegisterLibc();
void RegisterStdio();
void RegisterPthread();
void RegisterLibm();
void RegisterMisc();
void RegisterGles();
void RegisterLibdl();
void RegisterJni();
void RegisterBionic();
void RegisterFd();
void RegisterLocale();
void RegisterZlib();
void RegisterEgl();
void RegisterAndroid();
// ndk_android.cc: debug.ndk.window_probe -- logs a few pixels of a window's
// frame (RGBA, top row first; stride in pixels) and the frame rate.
void ProbeWindowPixels(const char* what, const uint8_t* rgba, int w, int h, int stride,
                       double cost_ms);
double NowMs();
void RegisterOpenSles();
void RegisterSyscalls();
void RegisterLibc8();     // ndk_libc8.cc: libc/libm of API 21-27
void RegisterAndroid8();  // ndk_android8.cc: libsync, libnativewindow, libmediandk stub, ASharedMemory
void InstallSymbolizer();
void EnsureInit();
// debug.ndk.trace: 0 off, 1 calls from Dalvik into guest code, 2 also the
// classes and methods guest code looks up through JNI.
int TraceLevel();
void MaybeWatchHostCalls();

// The guest address of host symbol |name| (a stub or data), 0 if unknown.
uint32_t ResolveHostSymbol(const char* name);
// The address of 32-bit data symbol |name| registered so far, 0 if none;
// for registration functions, which run inside the bridge's initialization
// (ResolveHostSymbol waits for it to finish).
uint32_t HostDataSymbol(const char* name);
bool IsHostLibrary(const char* name);
// The name of the host stub at |addr|, or nullptr.
const char* HostStubName(uint32_t addr);
// ndk_host_stub, remembering the stub's name (for HostStubName).
uint32_t MakeStub(NdkHostFn fn, void* user, const char* name);
// Guest code lives in [lo, hi) of all ranges noted so far: a cheap first
// test that keeps wasm function pointers (small table indices) out.
void NoteGuestRange(uint32_t lo, uint32_t hi);
bool MaybeGuest(uint32_t addr);

// --- guest signals (ndk_pthread.cc on the CPU's shim/ndk/cpu/signal.cc) ---------------

// FUTEX_WAIT on a guest word that guest signals interrupt: a pending
// signal's handler runs inside, then -EINTR. Otherwise 0 when woken
// (spuriously too), -EAGAIN when *addr != val, -ETIMEDOUT after |ms|
// (INFINITY: no limit). Linux errno values.
int GuestFutexWait(void* addr, uint32_t val, double ms);
// Sleeps |ms|; -EINTR (Linux) with the time left if a signal handler ran.
// A green thread parks (kParked: return at once).
int GuestSleep(double ms, double* left_ms);
// sigsuspend with |mask|: -EINTR (Linux) once a handler ran; a green
// thread parks (kParked).
int GuestSigsuspend(const uint64_t* mask);
// A wait no sender can wake (poll, select, epoll_wait): |wait|(ctx, ms) is
// called in slices of at most 50 ms (|timeout_ms| < 0: no limit), and a
// guest signal pending in between runs its handler and ends the wait with
// -1 / EINTR. |wait| returns 0 when its slice timed out.
int SignalSlicedWait(int timeout_ms, int (*wait)(void* ctx, int ms), void* ctx);
// poll() through SignalSlicedWait.
int GuestPoll(struct pollfd* fds, unsigned int n, int timeout_ms);  // n: nfds_t
// The kernel thread id guest code runs as (gettid).
int GuestTid();

// --- green threads (ndk_green.cc, topic threads-per-worker) ------------------------------
// Guest threads that share carrier host threads. A wait of one, at the
// outermost level of its host call, parks the call: the host function
// returns kParked at once (its result is not used: the call runs again from
// the start when the thread resumes) and, running again, finds the record
// it left (GreenResumed with its site) to go on from there.
constexpr int kParked = -0x7ffffff0;
enum GreenSite {
  kSiteFutex = 1, kSiteMutex, kSiteCond, kSiteCondRelock, kSiteRwlock, kSiteSem,
  kSiteSleep, kSiteSuspend, kSiteJoin, kSiteYield,
};
bool GreenEnabled();
bool IsGreenThread();   // the calling thread is a green thread on its carrier
// pthread_create of a green thread: 0, or -1 (none: make a host thread).
int GreenCreate(uint32_t* thread, uint32_t fn, uint32_t arg, uint32_t stack_size, bool detached);
bool IsGreen(uint32_t pthread);
// Host errno values (ESRCH, EINVAL, EDEADLK), or kParked.
int GreenJoin(uint32_t pthread, uint32_t* ret);
int GreenDetach(uint32_t pthread);
// pthread_exit of a green thread at the outermost level of the call: true
// (the thread ends when the call returns); false: not here.
bool GreenExit(uint32_t ret);
// Whether the calling thread's wait may park now.
bool GreenCanPark();
// The record of this call if it parked at |site| before (taken: once).
NdkResume* GreenResumed(int site);
// Parks on the guest futex word |addr| while *addr == |val| (else -EAGAIN,
// Linux), until a wake (result 0), |deadline| (GreenNowMs; INFINITY: none:
// -ETIMEDOUT) or a signal (-EINTR). |u|: 4 words for the record, or null.
int GreenParkFutex(void* addr, uint32_t val, double deadline, int site, const uint32_t* u);
// Parks until |deadline| (-ETIMEDOUT) or a signal (-EINTR).
int GreenParkTimed(double deadline, int site, const uint32_t* u);
// sched_yield: runnable again at once, after the others.
int GreenParkYield();
double GreenNowMs();
// GuestFutexWait (below) that parks a green thread (at |site|, with |u|);
// the call that resumes gets the result it was woken with.
int GuestFutexWaitAt(int site, void* addr, uint32_t val, double ms, const uint32_t* u = nullptr);
// Wakes up to |n| waiters of the guest futex word |addr|: host threads
// (emscripten_futex_wake), then parked green threads. Every wake of a word
// guest threads may wait on goes through this.
int GuestFutexWake(void* addr, int n);
// Around a wait of a green thread that cannot park: its carrier is not
// counted as running meanwhile (another one may start).
void GreenBlocking(bool begin);
// The calling green thread stays on its carrier (JNI, EGL, ALooper tie it to
// the host thread) until every reason it had is gone (GreenUnpin:
// DetachCurrentThread, eglMakeCurrent without a context).
enum GreenPinReason { kPinJni = 1, kPinEgl = 2, kPinLooper = 4 };
void GreenPin(int reason, const char* why);
void GreenUnpin(int reason);
// A thread_local of the bridge that is a guest thread's own: carriers swap
// its |size| bytes at |addr()| (registered from static constructors).
void RegisterGreenSlot(void* (*addr)(), uint32_t size);
// ndk_pthread.cc / ndk_core.cc: a green thread's start (on its carrier, as
// it) and end (its thread-specific data destructors, its TLS).
void GreenThreadStart(uint32_t stack_size);
void GreenThreadExit();
void FreeGuestThread();   // ndk_core.cc
// The CPUs guest code sees: the browser's cores (1-8), as the kernel's
// /proc/cpuinfo and /sys/devices/system/cpu/possible say (sysconf,
// get_nprocs, sched_getaffinity agree).
int GuestCpuCount();
// sched_getaffinity's mask of those CPUs in |set| (|n| bytes, at least 4).
void GuestCpuMask(void* set, size_t n);

// --- calling guest code --------------------------------------------------------------

// ndk_current_state() with bionic's TLS set up on first use.
NdkCpuState* CurrentState();
// ELF TLS of 32-bit guest libraries (ndk_linker.cc): each thread's static
// TLS area runs from this offset from TPIDRURO (after bionic's slots) to
// kGuestTlsAreaBytes; ndk_core.cc reserves it and calls InitThreadTls
// for every new thread.
constexpr uint32_t kGuestStaticTlsStart = 512;
constexpr uint32_t kGuestTlsAreaBytes = 16 * 1024;
void InitThreadTls(uint32_t tp);
void ForgetThreadTls(uint32_t tp);   // before the area is freed
uint64_t CallGuest(uint32_t addr, const uint32_t* args, int nargs);
// CallGuest that a fatal guest error ends instead of the VM: false and |why|
// then (ndk_call_guest_guarded).
bool CallGuestGuarded(uint32_t addr, const uint32_t* args, int nargs, uint64_t* result,
                      char* why, size_t why_size);
inline uint32_t CallGuest0(uint32_t addr) { return (uint32_t)CallGuest(addr, nullptr, 0); }
inline uint32_t CallGuest1(uint32_t addr, uint32_t a) { return (uint32_t)CallGuest(addr, &a, 1); }
inline uint32_t CallGuest2(uint32_t addr, uint32_t a, uint32_t b) {
  uint32_t v[2] = {a, b};
  return (uint32_t)CallGuest(addr, v, 2);
}

// Formats like printf with arguments from |args| (bionic types: long is 32
// bits, long double is double). Returns the full length, as vsnprintf.
int GuestFormat(char* out, size_t size, const char* fmt, ArgSource& args);
// Same, into a malloc'ed string.
char* GuestFormatAlloc(const char* fmt, ArgSource& args);
// scanf with arguments from |args| (pointers): from |str|, or |file| if null.
int GuestScan(const char* str, FILE* file, const char* fmt, ArgSource& args);

// --- linker ---------------------------------------------------------------------

struct Library;
// True for a guest code address in a loaded library, a host stub or
// executable memory the guest mapped (NoteExecRegion).
bool IsGuestCode(uint32_t addr);
// [lo, hi) holds code the guest made (mmap/mprotect with PROT_EXEC: JITs,
// libffi trampolines); munmap forgets it.
void NoteExecRegion(uint32_t lo, uint32_t hi);
void ForgetExecRegion(uint32_t lo, uint32_t hi);
Library* Dlopen(const char* path, int flags);
// "lib.so!symbol" (or a host stub's name) for a guest address, as the
// CPU's fault reports print it; nullptr if unknown.
const char* Symbolize(uint32_t addr, uint32_t* offset);
// ARC_NDK_THREADS_LOG=1: guest threads' start routines and names to stderr.
bool ThreadsLog();
// Debugging: logs the calling thread's guest pc, lr and the words on its
// guest stack that point into loaded libraries (likely return addresses),
// as "lib.so+0xOFFSET", innermost first. |why| starts the report.
void ReportGuestStack(const char* why);
// Debugging (debug.ndk.watch_pc): reports the guest stack when code at one of
// the comma-separated "lib.so+0xVADDR" addresses is first decoded.
void WatchGuestPcs(const char* list);
// Debugging: from TraceNewBlocks(true) on, the guest blocks the calling thread
// decodes (code that runs for the first time) are collected; TraceNewBlocks(
// false, why, n) logs the last n of them as "lib.so+0xVADDR".
void TraceNewBlocks(bool on, const char* why = nullptr, size_t last = 0);
// LD_LIBRARY_PATH: ':'-separated directories searched first.
void SetLibraryPath(const char* path);
// Loads and relocates without running constructors (import checks).
Library* LoadNoInit(const char* path);
uint32_t Dlsym(Library* lib, const char* name);
int Dlclose(Library* lib);
const char* Dlerror();
bool IsLibrary(void* handle);
// The ARM exception index table of the library containing |pc|.
uint32_t FindExidx(uint32_t pc, int* count);
// Runs the __cxa_atexit handlers registered for |dso| (all if 0).
void RunAtexit(uint32_t dso);
// The undefined symbols that resolved to nothing at load time.
const char* const* UnresolvedSymbols(Library* lib, size_t* n);
// Import checks: a DT_NEEDED library that is not found is skipped (and
// listed by MissingNeeded) instead of failing the load.
void SetSkipMissingNeeded(bool skip);
size_t MissingNeededCount(Library* lib);
const char* MissingNeededName(Library* lib, size_t i);


// --- arm64-v8a (LP64) guests ----------------------------------------------------------
// The AArch64 CPU (ndk_cpu64.h) runs libraries of arm64-v8a apps; their host
// functions are the same ones, with another ABI wrapper (AAPCS64: integers and
// pointers in x0-x7, floating point in v0-v7, then 8-byte stack slots; long,
// size_t and pointers are 64 bits). A guest pointer is a wasm address
// zero-extended; host pointers given to the guest are zero-extended too
// (0xffffffff, as MAP_FAILED or SIG_ERR, becomes ~0). The 64-bit CPU and the
// 64-bit linker (ndk_linker64.cc) are reached through weak references, so
// builds without the AArch64 CPU link (and load no arm64 library).

inline uint64_t Load64(uint32_t addr) {
  uint64_t v;
  memcpy(&v, Ptr<void>(addr), 8);
  return v;
}
inline void Store64(uint32_t addr, uint64_t v) { memcpy(Ptr<void>(addr), &v, 8); }
// Reservations of an arm64 guest larger than its memory (ndk_libc.cc): how
// many there are, and the guest's address of wasm address |a| (in a
// reservation's window above 4 GB, or |a|).
extern std::atomic<uint32_t> g_reserve_windows;
uint64_t ReserveWindowAddr64(uint32_t a);
// A pointer as a 64-bit guest stores it.
inline uint64_t GuestPtr64(const void* p) {
  uint32_t a = Addr(p);
  if (a == 0xffffffffu)
    return ~0ull;
  return g_reserve_windows.load(std::memory_order_relaxed) ? ReserveWindowAddr64(a) : a;
}

// The arguments of a call that entered a 64-bit host stub.
class RegArgs64 : public ArgSource {
 public:
  explicit RegArgs64(const NdkCpu64State* s, int first_x = 0, int first_v = 0)
      : s_(s), nx_(first_x), nv_(first_v), ns_(0) {}
  uint32_t U32() override { return static_cast<uint32_t>(X()); }
  uint64_t U64() override { return X(); }
  double F64() override { return BitCast<double>(V()); }
  long double F128() override;
  bool Lp64() const override { return true; }
  uint64_t Pointer() override { return X(); }
  uint64_t X() { return nx_ < 8 ? s_->x[nx_++] : Stack(); }
  uint64_t V() { return nv_ < 8 ? s_->v[nv_++][0] : Stack(); }
  uint64_t Stack() { return Load64(static_cast<uint32_t>(s_->sp) + 8 * ns_++); }
  int next_x() const { return nx_; }

 private:
  const NdkCpu64State* s_;
  int nx_, nv_, ns_;
};

// A guest va_list of AAPCS64: struct { void* __stack, *__gr_top, *__vr_top;
// int __gr_offs, __vr_offs; }, passed by its address.
class VaArgs64 : public ArgSource {
 public:
  explicit VaArgs64(uint32_t va)
      : stack_(static_cast<uint32_t>(Load64(va))), gr_top_(static_cast<uint32_t>(Load64(va + 8))),
        vr_top_(static_cast<uint32_t>(Load64(va + 16))), gr_offs_(static_cast<int32_t>(Load32(va + 24))),
        vr_offs_(static_cast<int32_t>(Load32(va + 28))) {}
  uint32_t U32() override { return static_cast<uint32_t>(X()); }
  uint64_t U64() override { return X(); }
  double F64() override { return BitCast<double>(V()); }
  long double F128() override;
  bool Lp64() const override { return true; }
  uint64_t Pointer() override { return X(); }
  uint64_t X() {
    if (gr_offs_ < 0) {
      uint64_t v = Load64(gr_top_ + gr_offs_);
      gr_offs_ += 8;
      return v;
    }
    uint64_t v = Load64(stack_);
    stack_ += 8;
    return v;
  }
  uint64_t V() {
    if (vr_offs_ < 0) {
      uint64_t v = Load64(vr_top_ + vr_offs_);
      vr_offs_ += 16;
      return v;
    }
    uint64_t v = Load64(stack_);
    stack_ += 8;
    return v;
  }

 private:
  uint32_t stack_, gr_top_, vr_top_;
  int32_t gr_offs_, vr_offs_;
};

// The guest FILE* of arm64 bionic: opaque, so a host FILE* as it is, except
// the standard streams, which old NDK headers address as &__sF[0..2]
// (152-byte entries); ndk_stdio64.cc.
FILE* HostFile64(uint32_t guest) __attribute__((weak));
uint32_t GuestFile64(FILE* host) __attribute__((weak));

// Guest-layout structures (marker base): a pointer to one is a safe
// parameter of an arm64 thunk.
struct GuestLayout64 {};
// Opaque handles, and structures laid out alike in LP64 bionic and in the
// host's musl, a pointer to which an arm64 thunk may pass as it is.
template <typename T> struct Opaque64 : std::false_type {};
template <> struct Opaque64<FILE> : std::true_type {};
template <> struct Opaque64<DIR> : std::true_type {};
template <> struct Opaque64<struct dirent> : std::true_type {};    // d_ino, d_off 64-bit; d_name[256]
template <> struct Opaque64<struct pollfd> : std::true_type {};
template <> struct Opaque64<struct sockaddr> : std::true_type {};
template <> struct Opaque64<fd_set> : std::true_type {};           // 1024 bits, little-endian words
template <> struct Opaque64<struct utimbuf> : std::true_type {};   // two time_t
template <> struct Opaque64<struct rlimit> : std::true_type {};    // two 64-bit rlim_t
template <> struct Opaque64<struct utsname> : std::true_type {};   // 6 x 65 chars
template <> struct Opaque64<struct in_addr> : std::true_type {};
template <> struct Opaque64<struct in6_addr> : std::true_type {};
template <> struct Opaque64<mbstate_t> : std::true_type {};        // 8 bytes on both sides
// JNI's references, IDs and values: host objects (or unions) passed as they are.
template <> struct Opaque64<_jobject> : std::true_type {};
template <> struct Opaque64<_jclass> : std::true_type {};
template <> struct Opaque64<_jstring> : std::true_type {};
template <> struct Opaque64<_jarray> : std::true_type {};
template <> struct Opaque64<_jobjectArray> : std::true_type {};
template <> struct Opaque64<_jbooleanArray> : std::true_type {};
template <> struct Opaque64<_jbyteArray> : std::true_type {};
template <> struct Opaque64<_jcharArray> : std::true_type {};
template <> struct Opaque64<_jshortArray> : std::true_type {};
template <> struct Opaque64<_jintArray> : std::true_type {};
template <> struct Opaque64<_jlongArray> : std::true_type {};
template <> struct Opaque64<_jfloatArray> : std::true_type {};
template <> struct Opaque64<_jdoubleArray> : std::true_type {};
template <> struct Opaque64<_jthrowable> : std::true_type {};
template <> struct Opaque64<_jmethodID> : std::true_type {};
template <> struct Opaque64<_jfieldID> : std::true_type {};
template <> struct Opaque64<jvalue> : std::true_type {};
template <typename T, typename = void> struct Complete64 : std::false_type {};
template <typename T> struct Complete64<T, std::void_t<decltype(sizeof(T))>> : std::true_type {};

// Which C types of a host function mean the same to an LP64 guest. Pointers
// to long, size_t or pointers are 4 bytes on the host and 8 in the guest;
// pointers to functions would be guest code; structures differ unless they
// are marked (GuestLayout64, Opaque64).
template <typename P> struct Lp64Pointee {
  static constexpr bool value =
      std::is_void<P>::value ||
      (std::is_arithmetic<P>::value && !std::is_same<P, long>::value && !std::is_same<P, unsigned long>::value) ||
      std::is_enum<P>::value || Opaque64<P>::value ||
      std::conjunction<Complete64<P>, std::is_base_of<GuestLayout64, P>>::value;
};
template <typename T> struct Lp64Type {
  static constexpr bool value =
      std::is_arithmetic<T>::value || std::is_enum<T>::value || std::is_same<T, GuestAddrArg>::value ||
      (std::is_pointer<T>::value && Lp64Pointee<typename std::remove_cv<typename std::remove_pointer<T>::type>::type>::value);
};

template <typename T>
inline T GetArg64(RegArgs64& a) {
  static_assert(Lp64Type<T>::value, "this C type does not mean the same to an arm64 guest");
  if constexpr (std::is_same<T, GuestAddrArg>::value) {
    return GuestAddrArg{GuestAddr64(a.X())};
  } else if constexpr (std::is_same<T, double>::value) {
    return a.F64();
  } else if constexpr (std::is_same<T, float>::value) {
    return BitCast<float>(static_cast<uint32_t>(a.V()));
  } else if constexpr (std::is_same<T, long double>::value) {
    return a.F128();
  } else if constexpr (std::is_same<T, bool>::value) {   // AAPCS64: only the low 8 bits are defined
    return (a.X() & 0xff) != 0;
  } else if constexpr (std::is_same<T, FILE*>::value) {
    return HostFile64(static_cast<uint32_t>(a.X()));
  } else if constexpr (std::is_pointer<T>::value) {
    return reinterpret_cast<T>(static_cast<uintptr_t>(GuestAddr64(a.X())));
  } else if constexpr (std::is_same<T, unsigned long>::value) {   // size_t: SIZE_MAX and more saturate
    uint64_t v = a.X();
    return v > 0xffffffffu ? 0xffffffffu : static_cast<unsigned long>(v);
  } else {
    return static_cast<T>(a.X());   // narrower types: the low bits
  }
}

template <typename T>
inline void SetResult64(NdkCpu64State* s, T v) {
  if constexpr (std::is_same<T, double>::value) {
    s->v[0][0] = BitCast<uint64_t>(v);
    s->v[0][1] = 0;
  } else if constexpr (std::is_same<T, float>::value) {
    s->v[0][0] = BitCast<uint32_t>(v);
    s->v[0][1] = 0;
  } else if constexpr (std::is_same<T, long double>::value) {
    memcpy(s->v[0], &v, 16);
  } else if constexpr (std::is_same<T, FILE*>::value) {
    s->x[0] = GuestFile64(v);
  } else if constexpr (std::is_pointer<T>::value) {
    s->x[0] = GuestPtr64(reinterpret_cast<const void*>(v));
  } else if constexpr (std::is_same<T, unsigned long>::value) {   // size_t: (size_t)-1, -2 ... stay negative
    uint32_t u = static_cast<uint32_t>(v);
    s->x[0] = u >= 0xfffff000u ? static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(u))) : u;
  } else if constexpr (sizeof(T) == 8) {
    s->x[0] = static_cast<uint64_t>(v);
  } else if constexpr (std::is_signed<T>::value) {
    s->x[0] = static_cast<uint64_t>(static_cast<int64_t>(v));
  } else {
    s->x[0] = static_cast<uint64_t>(v);
  }
}

template <auto F>
struct Thunk64;
template <typename R, typename... A, R (*F)(A...)>
struct Thunk64<F> {
  static void Call(NdkCpu64State* s, void*) {
    static_assert(std::is_void<R>::value || Lp64Type<R>::value || std::is_same<R, long double>::value,
                  "this result type does not mean the same to an arm64 guest");
    RegArgs64 a(s);
    std::tuple<A...> args{GetArg64<A>(a)...};  // braced list: left to right
    ThunkErrnoBegin();
    if constexpr (std::is_void<R>::value) {
      std::apply(F, args);
      ThunkErrnoEnd();
    } else {
      R r = std::apply(F, args);
      ThunkErrnoEnd();
      SetResult64(s, r);
    }
  }
};

// The guest address of the arm64 host symbol |name| (a 64-bit stub or data),
// 0 if unknown; ndk_core.cc.
uint32_t ResolveHostSymbol64(const char* name);
// ndk64_host_stub, remembering the name.
uint32_t MakeStub64(Ndk64HostFn fn, void* user, const char* name);
const char* HostStubName64(uint32_t addr);
// Whether this build has the AArch64 CPU.
bool HaveCpu64();
// ndk64_current_state() with bionic's arm64 TLS (TPIDR_EL0) set up.
NdkCpu64State* CurrentState64();
uint64_t CallGuest64(uint32_t addr, const uint64_t* args, int nargs);
void CallGuest64Args(uint32_t addr, const Ndk64Args* args, Ndk64Result* result);
bool CallGuest64Guarded(uint32_t addr, const Ndk64Args* args, Ndk64Result* result, char* why, size_t why_size);
// Leaves a guarded guest call of either CPU (the innermost); returns if none.
void LeaveGuest(const char* why);

// The 64-bit linker (ndk_linker64.cc).
struct Library64;
Library64* Dlopen64(const char* path, int flags) __attribute__((weak));
Library64* LoadNoInit64(const char* path) __attribute__((weak));
uint32_t Dlsym64(Library64* lib, const char* name) __attribute__((weak));
const char* Dlerror64() __attribute__((weak));
bool IsLibrary64(void* handle) __attribute__((weak));
bool IsGuestCode64(uint32_t addr) __attribute__((weak));
const char* Symbolize64(uint32_t addr, uint32_t* offset) __attribute__((weak));
const char* const* UnresolvedSymbols64(Library64* lib, size_t* n) __attribute__((weak));
size_t MissingNeededCount64(Library64* lib) __attribute__((weak));
const char* MissingNeededName64(Library64* lib, size_t i) __attribute__((weak));
void NoteExecRegion64(uint32_t lo, uint32_t hi) __attribute__((weak));
// "lib.so" lines of /proc/self/maps for the 64-bit libraries.
std::string Maps64() __attribute__((weak));
// Whether |path| is an AArch64 ELF shared object.
bool IsElf64File(const char* path) __attribute__((weak));
// Where a loaded library (an executable loaded as one: ndk_exec.cc) lies:
// its load bias and program headers in memory. False for a host library.
bool LibraryImage64(Library64* lib, uint32_t* bias, uint32_t* phdr, uint32_t* phnum)
    __attribute__((weak));
bool LibraryImage(Library* lib, uint32_t* bias, uint32_t* phdr, uint32_t* phnum);

// /proc/self/maps lines of the wasm memory and the arm64 windows (ndk_libc.cc),
// for programs the VM spawned.
std::string ProcMapsWindows();

// --- native programs (ndk_exec.cc, topic native-exec) -------------------------------
// Only in builds that run them (Android 10: mk/topic-native-exec.mk); weak.
// The calling thread's spawned program's stdin/stdout/stderr (|i| 0-2) as
// host FILEs over its own descriptors 0-2, or null outside one.
FILE* ExecStdFile(int i) __attribute__((weak));
// getenv of a spawned program (its own environment); |*handled| false
// outside one.
const char* ExecGetenv(const char* name, bool* handled) __attribute__((weak));
// fork()/vfork() of an arm64 spawned program (ndk_exec.cc): child resumes at
// the caller with 0, parent gets the child pid; -1/errno on failure.
int arc_vfork64(void) __attribute__((weak));
// Raw execve (SVC) of a spawned program: qemu-user re-execs itself for the
// guest's execve (ndk_exec.cc). -1/errno on failure, no return on success.
int ExecRaw64(const char* path, uint64_t argv, uint64_t envp) __attribute__((weak));
// setenv (|value| non-null) / unsetenv (null) of a spawned program: its own
// environment; false outside one.
bool ExecSetenv(const char* name, const char* value, int overwrite, int* result) __attribute__((weak));
// Its thunks (__libc_init of arm64 executables, wait/spawn): last of all.
void RegisterExec() __attribute__((weak));
// Raw clone/clone3 and exit of a static arm64 program's threads (SVC).
bool ExecClone64(NdkCpu64State* s, int64_t* result) __attribute__((weak));
bool ExecThreadExit64() __attribute__((weak));

// Registration of the arm64-only thunk files (called by EnsureInit). Weak: the
// files are only in builds with the AArch64 CPU (mk/topic-ndk-arm64.mk), as
// are the linker's functions above.
void RegisterLibm64();   // ndk_libm.cc (every build)
void RegisterLibdl64() __attribute__((weak));
void RegisterLibc64() __attribute__((weak));
void RegisterStdio64() __attribute__((weak));
void RegisterPthread64() __attribute__((weak));
void RegisterJni64() __attribute__((weak));

}  // namespace ndk

#endif  // ARC_WASM_SHIM_NDK_NDK_BRIDGE_H_
