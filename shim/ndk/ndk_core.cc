// The NDK bridge's core: the host symbol table guest imports bind to, host
// stubs, errno translation, per-thread guest state (bionic TLS) and
// initialization. Part of the arc-wasm shim layer.

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "ndk_bridge.h"

extern "C" int property_get(const char* key, char* value, const char* default_value);
extern "C" int __android_log_print(int prio, const char* tag, const char* fmt, ...);
// Optional CPU extension (shim/ndk/cpu/ndk_cpu_ext.h); absent in the stub CPU.
extern "C" void ndk_cpu_watch_host_calls(const char* words) __attribute__((weak));
extern "C" void ndk_set_jit(int threshold) __attribute__((weak));
extern "C" void ndk_set_jit_max(int max) __attribute__((weak));
extern "C" void ndk_cpu_dump_threads(void) __attribute__((weak));
extern "C" void ndk_cpu_watch_strings(const char* text) __attribute__((weak));
extern "C" void ndk_sig_self(uint32_t* thread, int* tid) __attribute__((weak));
// The AArch64 CPU (shim/ndk/cpu64): optional, as builds without it link.
#pragma weak ndk64_host_stub
#pragma weak ndk64_current_state
#pragma weak ndk64_call_guest
#pragma weak ndk64_call_guest_args
#pragma weak ndk64_call_guest_guarded
#pragma weak ndk64_leave_guest
#pragma weak ndk64_in_guarded_call
#pragma weak ndk64_set_jit
#pragma weak ndk64_set_jit_max
#pragma weak ndk64_watch_host_calls

namespace ndk {

// ndk_linker64.cc: the ELF TLS blocks of loaded libraries into a new thread's area.
void InitThreadTls64(uint32_t tp) __attribute__((weak));

namespace {

struct HostEntry {
  const char* lib;
  HostSymbol sym;   // both halves merged (fn/data: 32-bit guests, fn64/data64: arm64)
  uint32_t addr;    // stub or data address, once resolved
  uint32_t addr64;
};

std::mutex g_sym_lock;
std::map<std::string, HostEntry>* g_syms;
std::map<uint32_t, const char*>* g_stub_names;
std::map<uint32_t, const char*>* g_stub_names64;
std::set<std::string>* g_host_libs;

// Linux errno numbers by name (bionic uses the kernel's). Entries for
// errors the host does not define are skipped by the preprocessor.
struct ErrnoPair {
  int host, linux_;
};
const ErrnoPair kErrno[] = {
    {EPERM, 1}, {ENOENT, 2}, {ESRCH, 3}, {EINTR, 4}, {EIO, 5}, {ENXIO, 6},
    {E2BIG, 7}, {ENOEXEC, 8}, {EBADF, 9}, {ECHILD, 10}, {EAGAIN, 11},
    {ENOMEM, 12}, {EACCES, 13}, {EFAULT, 14}, {ENOTBLK, 15}, {EBUSY, 16},
    {EEXIST, 17}, {EXDEV, 18}, {ENODEV, 19}, {ENOTDIR, 20}, {EISDIR, 21},
    {EINVAL, 22}, {ENFILE, 23}, {EMFILE, 24}, {ENOTTY, 25}, {ETXTBSY, 26},
    {EFBIG, 27}, {ENOSPC, 28}, {ESPIPE, 29}, {EROFS, 30}, {EMLINK, 31},
    {EPIPE, 32}, {EDOM, 33}, {ERANGE, 34}, {EDEADLK, 35}, {ENAMETOOLONG, 36},
    {ENOLCK, 37}, {ENOSYS, 38}, {ENOTEMPTY, 39}, {ELOOP, 40}, {ENOMSG, 42},
    {EIDRM, 43}, {ENOSTR, 60}, {ENODATA, 61}, {ETIME, 62}, {ENOSR, 63},
    {ENOLINK, 67}, {EPROTO, 71}, {EMULTIHOP, 72}, {EBADMSG, 74},
    {EOVERFLOW, 75}, {EILSEQ, 84}, {EUSERS, 87}, {ENOTSOCK, 88},
    {EDESTADDRREQ, 89}, {EMSGSIZE, 90}, {EPROTOTYPE, 91}, {ENOPROTOOPT, 92},
    {EPROTONOSUPPORT, 93}, {ESOCKTNOSUPPORT, 94}, {EOPNOTSUPP, 95},
    {EPFNOSUPPORT, 96}, {EAFNOSUPPORT, 97}, {EADDRINUSE, 98},
    {EADDRNOTAVAIL, 99}, {ENETDOWN, 100}, {ENETUNREACH, 101},
    {ENETRESET, 102}, {ECONNABORTED, 103}, {ECONNRESET, 104}, {ENOBUFS, 105},
    {EISCONN, 106}, {ENOTCONN, 107}, {ESHUTDOWN, 108}, {ETOOMANYREFS, 109},
    {ETIMEDOUT, 110}, {ECONNREFUSED, 111}, {EHOSTDOWN, 112},
    {EHOSTUNREACH, 113}, {EALREADY, 114}, {EINPROGRESS, 115}, {ESTALE, 116},
    {EDQUOT, 122}, {ECANCELED, 125}, {EOWNERDEAD, 130},
    {ENOTRECOVERABLE, 131},
};

// bionic's TLS area (Android 4.4 bionic/libc/private/bionic_tls.h): what
// __get_tls() returns, BIONIC_TLS_SLOTS words, then the thread's static TLS
// area, where the linker puts the ELF TLS blocks of libraries
// (ndk_linker.cc). Slot 2 (4.4's errno pointer; guests call __errno())
// stays 0 in every thread: since Android 10 it is TLS_SLOT_APP, Go's g
// (runtime/cgo reads it on threads that call into Go: non-zero = a Go thread).
enum { kTlsSelf = 0, kTlsThreadId = 1, kTlsApp = 2, kTlsSlots = 64 };
struct GuestThread {
  uint32_t tls[kTlsSlots];
  int guest_errno;
  uint8_t static_tls[kGuestTlsAreaBytes - kTlsSlots * 4 - sizeof(int)];
};
static_assert(offsetof(GuestThread, static_tls) <= kGuestStaticTlsStart &&
              sizeof(GuestThread) >= kGuestTlsAreaBytes, "the static TLS area");
thread_local GuestThread* t_thread;

// bionic's arm64 TLS (TPIDR_EL0 points at 64-bit slots): the compiler reads
// the stack protector's guard from slot 5 (TLS_SLOT_STACK_GUARD). The slots
// are followed by the thread's static TLS area, where the 64-bit linker puts
// the ELF TLS blocks of libraries (ndk_linker64.cc).
enum { kTls64StackGuard = 5 };
struct GuestThread64 {
  uint64_t tls[kTlsSlots];
  uint8_t static_tls[16 * 1024 - kTlsSlots * 8];
};
thread_local GuestThread64* t_thread64;
uint64_t StackGuard64() {
  static uint64_t guard = [] {
    uint64_t g = 0;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
      if (read(fd, &g, sizeof g) != sizeof g) g = 0;
      close(fd);
    }
    if (g == 0) g = 0x2545f4914f6cdd1dull ^ reinterpret_cast<uintptr_t>(&g);
    return g & ~0xffull;   // a zero byte, as bionic's, stops string overruns
  }();
  return guard;
}

// A green thread's own (ndk_green.cc swaps it on its carriers).
void* ThreadSlot() { return &t_thread; }
[[maybe_unused]] const bool kThreadSlot = (RegisterGreenSlot(ThreadSlot, sizeof(t_thread)), true);

GuestThread* Thread() {
  if (!t_thread) {
    t_thread = static_cast<GuestThread*>(aligned_alloc(64, sizeof(GuestThread)));
    memset(t_thread, 0, sizeof(GuestThread));
    t_thread->tls[kTlsSelf] = Addr(t_thread->tls);
    uint32_t self = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pthread_self()));
    if (ndk_sig_self) ndk_sig_self(&self, nullptr);   // a green thread's own
    t_thread->tls[kTlsThreadId] = self;
    InitThreadTls(Addr(t_thread->tls));
  }
  return t_thread;
}

}  // namespace

int ToGuestErrno(int host) {
  for (const ErrnoPair& e : kErrno) {
    if (e.host == host)
      return e.linux_;
  }
  return host == 0 ? 0 : 22;  // EINVAL
}

int ToHostErrno(int guest) {
  for (const ErrnoPair& e : kErrno) {
    if (e.linux_ == guest)
      return e.host;
  }
  return guest == 0 ? 0 : EINVAL;
}

int* GuestErrno() { return &Thread()->guest_errno; }

void FreeGuestThread() {
  if (!t_thread) return;
  ForgetThreadTls(Addr(t_thread->tls));   // (the linker writes new libraries' TLS into live areas)
  free(t_thread);
  t_thread = nullptr;
}

void ThunkErrnoBegin() { errno = 0; }
void ThunkErrnoEnd() {
  if (errno != 0)
    *GuestErrno() = ToGuestErrno(errno);
}

NdkCpuState* CurrentState() {
  NdkCpuState* s = ndk_current_state();
  if (s->tls == 0)
    s->tls = Addr(Thread()->tls);
  return s;
}

uint64_t CallGuest(uint32_t addr, const uint32_t* args, int nargs) {
  CurrentState();
  return ndk_call_guest(addr, args, nargs);
}

bool CallGuestGuarded(uint32_t addr, const uint32_t* args, int nargs, uint64_t* result,
                      char* why, size_t why_size) {
  CurrentState();
  return ndk_call_guest_guarded(addr, args, nargs, result, why, why_size) != 0;
}

void RegisterHostSymbols(const char* lib, const HostSymbol* syms, size_t n) {
  std::lock_guard<std::mutex> l(g_sym_lock);
  if (!g_syms) {
    g_syms = new std::map<std::string, HostEntry>;
    g_host_libs = new std::set<std::string>;
  }
  if (!g_stub_names)
    g_stub_names = new std::map<uint32_t, const char*>;
  if (!g_stub_names64)
    g_stub_names64 = new std::map<uint32_t, const char*>;
  g_host_libs->insert(lib);
  for (size_t i = 0; i < n; i++) {
    const HostSymbol& sym = syms[i];
    auto it = g_syms->find(sym.name);
    if (it == g_syms->end()) {
      (*g_syms)[sym.name] = HostEntry{lib, sym, 0, 0};
      continue;
    }
    // Merge: each half present in |sym| replaces that half (a later
    // registration wins, per ABI).
    HostEntry& e = it->second;
    e.lib = lib;
    e.sym.name = sym.name;
    if (sym.fn || sym.data) {
      e.sym.fn = sym.fn;
      e.sym.user = sym.user;
      e.sym.data = sym.data;
      e.addr = 0;
    }
    if (sym.fn64 || sym.data64) {
      e.sym.fn64 = sym.fn64;
      e.sym.data64 = sym.data64;
      if (!sym.fn) e.sym.user = sym.user;
      e.addr64 = 0;
    }
  }
}

void RegisterDefaultHostSymbols(const char* lib, const HostSymbol* syms, size_t n) {
  std::vector<HostSymbol> absent;
  {
    std::lock_guard<std::mutex> l(g_sym_lock);
    for (size_t i = 0; i < n; i++) {
      if (!g_syms) {
        absent.push_back(syms[i]);
        continue;
      }
      auto it = g_syms->find(syms[i].name);
      if (it == g_syms->end()) {
        absent.push_back(syms[i]);
        continue;
      }
      // Present: only the halves nobody registered.
      HostSymbol h = syms[i];
      const HostSymbol& have = it->second.sym;
      if (have.fn || have.data) h.fn = nullptr, h.data = 0;
      if (have.fn64 || have.data64) h.fn64 = nullptr, h.data64 = 0;
      if (h.fn || h.data || h.fn64 || h.data64)
        absent.push_back(h);
    }
  }
  RegisterHostSymbols(lib, absent.data(), absent.size());
}

bool AliasHostSymbol(const char* lib, const char* alias, const char* original) {
  std::lock_guard<std::mutex> l(g_sym_lock);
  auto it = g_syms->find(original);
  if (it == g_syms->end() || g_syms->count(alias))
    return false;
  HostEntry e = it->second;
  e.lib = lib;
  e.addr = 0;  // a stub of its own (named for the alias)
  e.addr64 = 0;
  e.sym.name = alias;
  (*g_syms)[alias] = e;
  g_host_libs->insert(lib);
  return true;
}

namespace {
HostLibraryProvider* g_providers;
}  // namespace

void AddHostLibraryProvider(HostLibraryProvider* p) {
  // Kept sorted by priority (a static constructor's order is the link's).
  HostLibraryProvider** at = &g_providers;
  while (*at && (*at)->priority <= p->priority)
    at = &(*at)->next;
  p->next = *at;
  *at = p;
}

uint32_t ResolveHostSymbol(const char* name) {
  EnsureInit();
  std::lock_guard<std::mutex> l(g_sym_lock);
  auto it = g_syms->find(name);
  if (it == g_syms->end())
    return 0;
  HostEntry& e = it->second;
  if (!e.sym.fn && !e.sym.data)
    return 0;   // an arm64-only symbol
  if (e.addr == 0) {
    if (e.sym.fn) {
      e.addr = ndk_host_stub(e.sym.fn, e.sym.user, e.sym.name);
      (*g_stub_names)[e.addr & ~1u] = e.sym.name;
      NoteGuestRange(e.addr & ~1u, (e.addr & ~1u) + 4);
    } else {
      e.addr = e.sym.data;
    }
  }
  return e.addr;
}

uint32_t HostDataSymbol(const char* name) {
  std::lock_guard<std::mutex> l(g_sym_lock);
  if (!g_syms)
    return 0;
  auto it = g_syms->find(name);
  return it == g_syms->end() ? 0 : it->second.sym.data;
}

namespace {
std::atomic<uint32_t> g_guest_lo{UINT32_MAX}, g_guest_hi{0};
}  // namespace

void NoteGuestRange(uint32_t lo, uint32_t hi) {
  uint32_t v = g_guest_lo.load();
  while (lo < v && !g_guest_lo.compare_exchange_weak(v, lo)) {}
  v = g_guest_hi.load();
  while (hi > v && !g_guest_hi.compare_exchange_weak(v, hi)) {}
}

bool MaybeGuest(uint32_t addr) { return addr >= g_guest_lo.load() && addr < g_guest_hi.load(); }

uint32_t MakeStub(NdkHostFn fn, void* user, const char* name) {
  uint32_t a = ndk_host_stub(fn, user, name);
  std::lock_guard<std::mutex> l(g_sym_lock);
  if (!g_stub_names)
    g_stub_names = new std::map<uint32_t, const char*>;
  (*g_stub_names)[a & ~1u] = name;
  NoteGuestRange(a & ~1u, (a & ~1u) + 4);
  return a;
}

const char* HostStubName(uint32_t addr) {
  std::lock_guard<std::mutex> l(g_sym_lock);
  if (!g_stub_names)
    return nullptr;
  auto it = g_stub_names->find(addr & ~1u);
  return it == g_stub_names->end() ? nullptr : it->second;
}

// --- arm64-v8a guests -----------------------------------------------------------------------

bool HaveCpu64() { return ndk64_host_stub != nullptr; }

// bionic's libdl functions are wrappers that call the linker's __loader_*
// with the caller's address (bl __loader_dlopen). Code that bypasses linker
// namespaces (libadrenotools, used by emulators to load Vulkan drivers) reads
// dlopen's instructions for that BL and calls its target: a stub's word (its
// contents are never executed) holds that BL to the __loader_ stub.
const struct { const char* wrapper; const char* loader; } kLoaderWrappers64[] = {
    {"dlopen", "__loader_dlopen"}, {"dlsym", "__loader_dlsym"},
    {"android_dlopen_ext", "__loader_android_dlopen_ext"}, {"dlclose", "__loader_dlclose"},
    {"dlerror", "__loader_dlerror"}, {"dladdr", "__loader_dladdr"},
    {"dl_iterate_phdr", "__loader_dl_iterate_phdr"},
};

uint32_t ResolveHostSymbol64Locked(const char* name) {
  auto it = g_syms->find(name);
  if (it == g_syms->end())
    return 0;
  HostEntry& e = it->second;
  if (!e.sym.fn64 && !e.sym.data64)
    return 0;   // a 32-bit-only symbol
  if (e.addr64 == 0) {
    if (e.sym.fn64) {
      e.addr64 = ndk64_host_stub(e.sym.fn64, e.sym.user, e.sym.name);
      (*g_stub_names64)[e.addr64] = e.sym.name;
      for (const auto& w : kLoaderWrappers64) {
        if (strcmp(w.wrapper, name) != 0) continue;
        if (uint32_t target = ResolveHostSymbol64Locked(w.loader))
          Store32(e.addr64, 0x94000000u | (((target - e.addr64) >> 2) & 0x3ffffffu));   // BL
      }
    } else {
      e.addr64 = e.sym.data64;
    }
  }
  return e.addr64;
}

uint32_t ResolveHostSymbol64(const char* name) {
  EnsureInit();
  if (!HaveCpu64())
    return 0;
  std::lock_guard<std::mutex> l(g_sym_lock);
  return ResolveHostSymbol64Locked(name);
}

uint32_t MakeStub64(Ndk64HostFn fn, void* user, const char* name) {
  uint32_t a = ndk64_host_stub(fn, user, name);
  std::lock_guard<std::mutex> l(g_sym_lock);
  if (!g_stub_names64)
    g_stub_names64 = new std::map<uint32_t, const char*>;
  (*g_stub_names64)[a] = name;
  return a;
}

const char* HostStubName64(uint32_t addr) {
  std::lock_guard<std::mutex> l(g_sym_lock);
  if (!g_stub_names64)
    return nullptr;
  auto it = g_stub_names64->find(addr);
  return it == g_stub_names64->end() ? nullptr : it->second;
}

NdkCpu64State* CurrentState64() {
  NdkCpu64State* s = ndk64_current_state();
  if (s->tpidr == 0) {
    if (!t_thread64) {
      GuestThread* t = Thread();
      t_thread64 = static_cast<GuestThread64*>(aligned_alloc(64, sizeof(GuestThread64)));
      memset(t_thread64, 0, sizeof(GuestThread64));
      t_thread64->tls[kTlsSelf] = Addr(t_thread64->tls);
      t_thread64->tls[kTlsThreadId] = t->tls[kTlsThreadId];
      t_thread64->tls[kTls64StackGuard] = StackGuard64();
      if (InitThreadTls64)
        InitThreadTls64(Addr(t_thread64->tls));
    }
    s->tpidr = Addr(t_thread64->tls);
  }
  return s;
}

uint64_t CallGuest64(uint32_t addr, const uint64_t* args, int nargs) {
  CurrentState64();
  return ndk64_call_guest(addr, args, nargs);
}

void CallGuest64Args(uint32_t addr, const Ndk64Args* args, Ndk64Result* result) {
  CurrentState64();
  ndk64_call_guest_args(addr, args, result);
}

bool CallGuest64Guarded(uint32_t addr, const Ndk64Args* args, Ndk64Result* result, char* why, size_t why_size) {
  CurrentState64();
  return ndk64_call_guest_guarded(addr, args, result, why, why_size) != 0;
}

void LeaveGuest(const char* why) {
  // A thread runs one guest ABI at a time: the 64-bit guard if there is one.
  if (ndk64_in_guarded_call && ndk64_in_guarded_call())
    ndk64_leave_guest(why);
  ndk_leave_guest(why);
}

long double RegArgs64::F128() {
  if (nv_ < 8) {
    long double v;
    memcpy(&v, s_->v[nv_++], 16);
    return v;
  }
  ns_ = (ns_ + 1) & ~1;   // 16-byte aligned on the stack
  long double v;
  memcpy(&v, Ptr<void>(static_cast<uint32_t>(s_->sp) + 8 * ns_), 16);
  ns_ += 2;
  return v;
}

long double VaArgs64::F128() {
  long double v;
  if (vr_offs_ < 0) {
    memcpy(&v, Ptr<void>(vr_top_ + vr_offs_), 16);
    vr_offs_ += 16;
    return v;
  }
  stack_ = (stack_ + 15) & ~15u;
  memcpy(&v, Ptr<void>(stack_), 16);
  stack_ += 16;
  return v;
}

bool IsHostLibrary(const char* name) {
  EnsureInit();
  // Android system libraries an NDK app may link against; the ones without
  // registered symbols load as empty host libraries.
  static const char* const kSystem[] = {
      "libc.so", "libm.so", "libdl.so", "liblog.so", "libstdc++.so", "libz.so",
      "libGLESv1_CM.so", "libGLESv2.so", "libGLESv3.so", "libEGL.so",
      "libandroid.so", "libjnigraphics.so", "libOpenSLES.so", "libOpenMAXAL.so",
      // Android 7.0-8.1 (API 24-27): libsync, libnativewindow, libmediandk and
      // libcamera2ndk have stubs or real ones (ndk_android8.cc, other topics);
      // libaaudio is not there before topic fw8-crypto-media (cubeb falls back
      // to OpenSL ES when dlopen fails).
      "libsync.so", "libnativewindow.so", "libmediandk.so",
  };
  for (const char* s : kSystem) {
    if (strcmp(name, s) == 0)
      return true;
  }
  std::lock_guard<std::mutex> l(g_sym_lock);
  return g_host_libs->count(name) != 0;
}

// ro.arcwasm.jit=0 -- SENSES Web Apps' "without JIT" for an app (the page's
// ARC_JIT; docs/topics/guest-jit.md): no ARM or arm64 block is translated
// to wasm, all native code (an app's own generated code too) is
// interpreted. Unset or "1": translation on.
//
// Debugging switches, asked at each dlopen until set (the bridge starts
// before an app's properties), over the above: debug.ndk.watch=WORD,... logs calls of host
// functions so named; debug.ndk.jit=N translates ARM and arm64 blocks to wasm after N
// executions (0: never); debug.ndk.jit_max=N instantiates at most N of
// them per thread, ARM and arm64 (0: no limit; unset: the memory budget's,
// 1024 on an iPhone); debug.ndk.watch_pc=lib.so+0xVADDR,... reports the
// guest stack when that code first runs; debug.ndk.dump_threads=SECS prints every
// guest thread's place (ndk_cpu_dump_threads: pc, lr, host call, stack) every SECS
// seconds (hangs in Node, where no page can ask for it); debug.ndk.watch_str=TEXT logs
// host calls whose arguments point at TEXT, with the caller's pid (which light process
// handles a page's text). A property value is at most 91
// characters; the environment variables NDK_WATCH and NDK_WATCH_PC (the
// test page's ?env=) take longer lists and win over the properties.
void MaybeWatchHostCalls() {
  static std::atomic<bool> watch_done{false}, jit_done{false}, mode_done{false};
  char v[92];
  if (!mode_done.exchange(true)) {
    property_get("ro.arcwasm.jit", v, "");
    const char* e = v[0] ? v : getenv("ARC_JIT");   // (a stack that loads no properties)
    if (e && strcmp(e, "0") == 0) {
      __android_log_print(4, "ndk", "no JIT (ro.arcwasm.jit=0): native code is interpreted");
      if (ndk_set_jit) ndk_set_jit(0);
      if (ndk64_set_jit) ndk64_set_jit(0);
    }
  }
  if (!watch_done.load() && ndk_cpu_watch_host_calls) {
    const char* e = getenv("NDK_WATCH");
    if (!e || !*e) {
      property_get("debug.ndk.watch", v, "");
      e = v;
    }
    if (e[0]) {
      watch_done.store(true);
      __android_log_print(4, "ndk", "watching host calls: %s", e);
      ndk_cpu_watch_host_calls(e);
    }
  }
  static std::atomic<bool> pc_done{false};
  if (!pc_done.load()) {
    const char* e = getenv("NDK_WATCH_PC");
    if (!e || !*e) {
      property_get("debug.ndk.watch_pc", v, "");
      e = v;
    }
    if (e[0]) {
      pc_done.store(true);
      __android_log_print(4, "ndk", "watching guest code: %s", e);
      WatchGuestPcs(e);
    }
  }
  static std::atomic<bool> str_done{false};
  if (!str_done.load() && ndk_cpu_watch_strings) {
    property_get("debug.ndk.watch_str", v, "");
    if (v[0]) {
      str_done.store(true);
      __android_log_print(4, "ndk", "watching host calls for \"%s\"", v);
      ndk_cpu_watch_strings(v);
    }
  }
  static std::atomic<bool> dump_done{false};
  if (!dump_done.load() && ndk_cpu_dump_threads) {
    property_get("debug.ndk.dump_threads", v, "");
    if (v[0] && atoi(v) > 0) {
      dump_done.store(true);
      __android_log_print(4, "ndk", "dumping guest threads every %s s", v);
      pthread_t t;
      pthread_create(&t, nullptr, [](void* arg) -> void* {
        const unsigned secs = static_cast<unsigned>(reinterpret_cast<uintptr_t>(arg));
        for (;;) {
          sleep(secs);
          ndk_cpu_dump_threads();
        }
        return nullptr;
      }, reinterpret_cast<void*>(static_cast<uintptr_t>(atoi(v))));
      pthread_detach(t);
    }
  }
  static std::atomic<bool> jit_max_done{false};
  if (!jit_max_done.load() && ndk_set_jit_max) {
    property_get("debug.ndk.jit_max", v, "");
    if (v[0]) {
      jit_max_done.store(true);
      __android_log_print(4, "ndk", "translations per thread: at most %s", v);
      ndk_set_jit_max(atoi(v));
      if (ndk64_set_jit_max) ndk64_set_jit_max(atoi(v));
    }
  }
  if (!jit_done.load() && ndk_set_jit) {
    property_get("debug.ndk.jit", v, "");
    if (v[0]) {
      jit_done.store(true);
      __android_log_print(4, "ndk", "translation threshold: %s", v);
      ndk_set_jit(atoi(v));
      if (ndk64_set_jit) ndk64_set_jit(atoi(v));
    }
  }
}

int TraceLevel() {
  static int level = -1;
  if (level < 0) {
    char v[92];
    property_get("debug.ndk.trace", v, "0");
    level = atoi(v);
  }
  return level;
}

void EnsureInit() {
  static std::once_flag once;
  std::call_once(once, [] {

    RegisterLibc();
    RegisterBionic();
    RegisterFd();
    RegisterLocale();  // after RegisterLibc: replaces its C-locale functions
    RegisterStdio();
    RegisterPthread();
    RegisterLibm();
    RegisterMisc();
    RegisterZlib();
    RegisterGles();
    RegisterEgl();
    RegisterAndroid();
    RegisterOpenSles();
    RegisterSyscalls();  // raw svc handler: reuses the thunks registered above
    RegisterLibdl();
    RegisterJni();
    // API 21-27 (after everything they alias), then other topics' libraries,
    // lowest priority first (a later one replaces symbols of the same name).
    RegisterLibc8();
    RegisterAndroid8();
    for (HostLibraryProvider* p = g_providers; p; p = p->next)
      p->register_symbols();
    // arm64-v8a only (the ndk64_*.cc files, in builds with the AArch64 CPU):
    // the halves of symbols whose 32-bit thunks do not fit an LP64 guest.
    if (HaveCpu64()) {
      RegisterLibm64();
      for (void (*reg)() :
           {RegisterLibc64, RegisterStdio64, RegisterPthread64, RegisterLibdl64, RegisterJni64}) {
        if (reg) reg();
      }
    }
    // Native programs (ndk_exec.cc, Android 10): their thunks win.
    if (RegisterExec) RegisterExec();
    InstallSymbolizer();
    // OpenSSL built for API < 18 (no getauxval) finds the CPU's extensions
    // by running their instructions under a SIGILL handler, and guests get
    // no signals (ndk_libc.cc): it reads OPENSSL_armcap first. NEON only
    // (ARMV7_NEON), as AT_HWCAP2 says (ndk_bionic.cc: G_getauxval); Telegram
    // X's OpenSSL 3.5 aborted on its PMULL probe.
    setenv("OPENSSL_armcap", "1", 0);
  });
}

// Whether this thread runs arm64 guest code (and not ARM32: one ABI per app).
bool GuestThreadLp64() { return t_thread64 != nullptr && t_thread == nullptr; }

}  // namespace ndk
