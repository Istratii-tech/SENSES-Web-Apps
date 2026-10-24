// Signals to AArch64 guest threads (topic "unity2022-arm64"), as the ARMv7
// CPU's (shim/ndk/cpu/signal.cc, which the bridge's API functions forward
// here for a thread running arm64 code): a signal is a pending bit on the
// target thread's Cpu that the thread acts on at safe points --
//
//  - at a block boundary of the interpreter (exec64.cc Run; translations
//    return to it at the end of a region): the handler starts in the same
//    Run, on the thread's guest stack below a Linux-style rt_sigframe
//    (siginfo + ucontext with the interrupted registers, which a
//    conservative GC such as Boehm's scans), returning to a host stub that
//    does rt_sigreturn;
//  - inside a host function that waits (the bridge's futex, mutexes,
//    condition variables, semaphores, sleeps, sigsuspend: ndk64_sig_wait
//    and the others below): the sender wakes the futex the thread sleeps
//    on, and the handler runs nested from the host function, after which
//    the wait returns EINTR or goes on;
//  - a thread outside guest code altogether (a Java thread back in Java):
//    a helper thread ("proxy") runs the handler with that thread's CPU, on
//    its guest stack, as that thread (ndk64_sig_self), while the thread
//    waits at its next entry into guest code (Cpu::gate).
//
// Without this, Boehm's GC in il2cpp (Unity on arm64) got ESRCH from
// pthread_kill for every other thread, took them for gone and collected
// while they ran: objects still in use were freed (Subway Surfers 3.70:
// crashes in il2cpp with a code address where an object was).
//
// A thread in host code below guest code that does not wait here (a JNI
// call into Java, a blocking read) gets its signals when it returns to
// guest code; a translated loop that never leaves its region, when it
// does. Alternate signal stacks are not used (SA_ONSTACK is ignored).
// Numbers are Linux's (1..64), errno values Linux's.
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten/threading.h>
#endif

#include "ndk/cpu64/cpu64_internal.h"

namespace ndkcpu64 {
namespace {

constexpr int kEINTR = 4, kEAGAIN = 11, kESRCH = 3, kEINVAL = 22, kETIMEDOUT = 110;
constexpr int kSIGKILL = 9, kSIGSTOP = 19;
constexpr uint64_t kSA_NODEFER = 0x40000000, kSA_RESETHAND = 0x80000000u, kSA_RESTART = 0x10000000,
                   kSA_SIGINFO = 4;
constexpr int kSI_TKILL = -6;

inline uint64_t Bit(int sig) { return 1ull << (sig - 1); }
constexpr uint64_t kUnblockable = (1ull << (kSIGKILL - 1)) | (1ull << (kSIGSTOP - 1));

std::mutex& CpusLock() {
  static std::mutex* mu = new std::mutex;
  return *mu;
}
std::set<Cpu*>& Cpus() {
  static std::set<Cpu*>* cpus = new std::set<Cpu*>;
  return *cpus;
}
// Threads pthread_create made that have no CPU yet (it comes with their first guest
// code): the signals sent to them meanwhile. CpusLock.
std::map<uint32_t, uint64_t>& Starting() {
  static std::map<uint32_t, uint64_t>* starting = new std::map<uint32_t, uint64_t>;
  return *starting;
}

// The process's actions (handler 0: SIG_DFL, 1: SIG_IGN).
std::mutex g_action_mu;
Ndk64Sigaction g_actions[65];

Ndk64Sigaction Action(int sig) {
  std::lock_guard<std::mutex> lock(g_action_mu);
  return g_actions[sig];
}
bool Deliverable(const Ndk64Sigaction& a) { return a.handler > 1; }

// --- the frame on the guest stack: Linux's arm64 rt_sigframe ----------------------------
struct GSiginfo {
  int32_t signo, err, code, pad;
  int32_t pid, uid;   // _kill
  uint8_t rest[128 - 24];
};
static_assert(sizeof(GSiginfo) == 128, "siginfo");
struct GUcontext {
  uint64_t flags, link;
  uint64_t ss_sp;
  int32_t ss_flags, ss_pad;
  uint64_t ss_size;
  uint64_t sigmask;
  uint8_t sigmask_rest[128 - 8];
  uint64_t pad_;   // uc_mcontext is 16-byte aligned
  // struct sigcontext
  uint64_t fault_address;
  uint64_t regs[31];
  uint64_t sp, pc, pstate;
  uint64_t pad2_;
  // __reserved[4096]: a struct fpsimd_context, then a terminator
  uint32_t fp_magic, fp_size, fpsr, fpcr;
  uint64_t vregs[32][2];
  uint8_t reserved_rest[4096 - 528];
};
static_assert(offsetof(GUcontext, sigmask) == 40, "uc_sigmask");
static_assert(offsetof(GUcontext, fault_address) == 176, "uc_mcontext");
static_assert(offsetof(GUcontext, fp_magic) == 176 + 288, "__reserved");
static_assert(sizeof(GUcontext) == 176 + 288 + 4096, "ucontext");
struct GFrame {
  GSiginfo info;
  GUcontext uc;
  uint64_t fp, lr;     // the frame record x29 points at in the handler (unwinders)
  uint32_t magic, sig;
  uint64_t pad_;
};
static_assert(sizeof(GFrame) % 16 == 0, "frame");
constexpr uint32_t kFrameMagic = 0x4e534947;   // "NSIG"

uint32_t g_sigreturn_stub;

void Recompute(Cpu* c) {
  c->sig_attention.store((c->sig_pending.load() & ~c->sig_blocked) ? 1 : 0);
}

int TakeSignal(Cpu* c, uint64_t allowed) {
  for (;;) {
    uint64_t p = c->sig_pending.load();
    uint64_t ready = p & allowed;
    if (!ready) return 0;
    int sig = __builtin_ctzll(ready) + 1;
    if (c->sig_pending.compare_exchange_weak(p, p & ~Bit(sig))) return sig;
  }
}

// Builds the frame below the guest sp for |sig| with the interrupted state
// (x[], sp, flags and V registers; pc |pc|) and points the CPU at the
// handler with x30 = |lr|. Returns the frame's address.
uint64_t EnterHandler(Cpu* c, int sig, const Ndk64Sigaction& a, uint64_t pc, uint64_t lr) {
  const uint64_t sp = (c->sp - sizeof(GFrame)) & ~15ull;
  GFrame* f = static_cast<GFrame*>(HostPtr(sp));
  memset(f, 0, sizeof(*f));
  f->info.signo = sig;
  f->info.code = kSI_TKILL;
  f->info.pid = getpid();
  GUcontext& uc = f->uc;
  uc.ss_flags = 2;   // SS_DISABLE
  uc.sigmask = c->sig_blocked;
  memcpy(uc.regs, c->x, sizeof(uc.regs));
  uc.sp = c->sp;
  uc.pc = pc;
  uc.pstate = PackNzcv(c);
  uc.fp_magic = 0x46508001;
  uc.fp_size = 528;
  uc.fpsr = c->fpsr;
  uc.fpcr = c->fpcr;
  memcpy(uc.vregs, c->v, sizeof(uc.vregs));
  f->fp = c->x[29];
  f->lr = pc;
  f->magic = kFrameMagic;
  f->sig = static_cast<uint32_t>(sig);

  c->sig_blocked |= (a.mask | ((a.flags & kSA_NODEFER) ? 0 : Bit(sig))) & ~kUnblockable;
  if (a.flags & kSA_RESETHAND) {
    std::lock_guard<std::mutex> lock(g_action_mu);
    g_actions[sig].handler = 0;
    g_actions[sig].flags &= ~kSA_SIGINFO;
  }
  Recompute(c);
  c->x[0] = static_cast<uint64_t>(sig);
  c->x[1] = sp + offsetof(GFrame, info);
  c->x[2] = sp + offsetof(GFrame, uc);
  c->x[29] = sp + offsetof(GFrame, fp);
  c->x[30] = lr;
  c->sp = sp;
  c->pc = a.handler;
  c->ex_valid = 0;
  return sp;
}

// rt_sigreturn: the state from the frame at |sp| (the handler may have changed it).
void LeaveHandler(Cpu* c, uint64_t sp) {
  GFrame* f = static_cast<GFrame*>(HostPtr(sp));
  if (f->magic != kFrameMagic)
    Fault(c, static_cast<uint32_t>(c->x[30]), "signal return with a damaged frame at sp=%llx",
          static_cast<unsigned long long>(sp));
  const GUcontext& uc = f->uc;
  memcpy(c->x, uc.regs, sizeof(uc.regs));
  c->sp = uc.sp;
  c->pc = uc.pc;
  c->nzcv = static_cast<uint32_t>(uc.pstate) & 0xf0000000u;
  UnpackNzcv(c, c->nzcv);
  c->fpsr = uc.fpsr;
  memcpy(c->v, uc.vregs, sizeof(uc.vregs));
  c->sig_blocked = uc.sigmask & ~kUnblockable;
  c->ex_valid = 0;
  Recompute(c);
}

// The handler of a safe point returns here (x30): back to the interrupted code.
// (HostCall goes on at the pc this sets.)
void SigreturnStub(NdkCpu64State* s, void*) {
  Cpu* c = static_cast<Cpu*>(s);
  LeaveHandler(c, c->sp);
}

void EnsureSigreturnStub() {
  static std::once_flag once;
  std::call_once(once, [] { g_sigreturn_stub = ndk64_host_stub(SigreturnStub, nullptr, "<signal return>"); });
}

// Runs the handlers of the pending unblocked signals from a host function
// (|c| is in a host call): each in a nested Run that returns to the "return
// to host" stub. Returns 0 if none ran, 1 if all that ran had SA_RESTART, 2
// otherwise.
int DeliverInHost(Cpu* c) {
  int result = 0;
  for (;;) {
    int sig = TakeSignal(c, ~c->sig_blocked);
    if (!sig) break;
    Ndk64Sigaction a = Action(sig);
    if (!Deliverable(a)) continue;
    const uint64_t pc = c->pc;
    const uint32_t nzcv = c->nzcv;
    const uint64_t sp = EnterHandler(c, sig, a, pc, ReturnStubAddr());
    Run(c);
    LeaveHandler(c, sp);
    c->pc = pc;
    c->nzcv = nzcv;
    result = (result == 2 || !(a.flags & kSA_RESTART)) ? 2 : 1;
  }
  Recompute(c);
  return result;
}

bool Attention(Cpu* c) { return c->sig_attention.load() != 0; }

#ifdef __EMSCRIPTEN__
int FutexWait(volatile void* addr, uint32_t val, double ms) {
  int e = emscripten_futex_wait(addr, val, ms);
  if (e == 0) return 0;
  if (e == -ETIMEDOUT) return -kETIMEDOUT;
  if (e == -EWOULDBLOCK || e == -EAGAIN) return -kEAGAIN;
  return -kEINVAL;
}
void FutexWake(volatile void* addr, int n) { emscripten_futex_wake(addr, n); }
#else
int FutexWait(volatile void* addr, uint32_t val, double ms) {
  if (*static_cast<volatile uint32_t*>(addr) != val) return -kEAGAIN;
  struct timespec ts = {0, 1000000};
  nanosleep(&ts, nullptr);
  return 0;
}
void FutexWake(volatile void*, int) {}
#endif

double NowMs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

// A wake between the check of sig_attention and the futex wait would be lost:
// waits are cut into slices.
constexpr double kSliceMs = 50;

// The interruptible futex wait under the API functions. |stop|: which pending
// signals end it (beside unblocked ones, for sigwait). Returns 0 (woken),
// -ETIMEDOUT, -EAGAIN (value differs) or 1 (a signal is there).
int WaitInterruptible(Cpu* c, volatile uint32_t* addr, uint32_t val, double ms, uint64_t stop) {
  const double deadline = isinf(ms) ? INFINITY : NowMs() + ms;
  const uint32_t a = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(addr));
  for (;;) {
    c->sig_wait_addr.store(a);
    if (Attention(c) || (c->sig_pending.load() & stop)) {
      c->sig_wait_addr.store(0);
      return 1;
    }
    double left = isinf(deadline) ? INFINITY : deadline - NowMs();
    if (left <= 0) {
      c->sig_wait_addr.store(0);
      return -kETIMEDOUT;
    }
    int e = FutexWait(addr, val, left < kSliceMs ? left : kSliceMs);
    c->sig_wait_addr.store(0);
    if (e == -kETIMEDOUT) continue;
    if (e == 0 && (Attention(c) || (c->sig_pending.load() & stop))) return 1;
    return e;
  }
}

// --- proxies: handlers of threads outside guest code ---------------------------------
std::mutex& g_proxy_mu = *new std::mutex;
std::condition_variable& g_proxy_cv = *new std::condition_variable;
std::vector<Cpu*>& g_proxy_queue = *new std::vector<Cpu*>;
int g_proxy_idle;

void StartProxy(Cpu* t);

void RunProxy(Cpu* t, ndkjit::ThreadJit* jit) {
  for (;;) {
    Cpu* own = SetCurrentCpu(t);
    // Translations are instantiated per thread: this helper's table, not t's.
    ndkjit::ThreadJit theirs = t->jit;
    t->jit = *jit;
    t->pc = ReturnStubAddr();   // (as if in a host call: the handler returns there)
    DeliverInHost(t);
    *jit = t->jit;
    t->jit = theirs;
    SetCurrentCpu(own);
    // Release; again if a signal came meanwhile and the thread is still outside guest code.
    std::lock_guard<std::mutex> lock(CpusLock());
    t->gate.store(0);
    FutexWake(&t->gate, 0x7fffffff);
    uint32_t expected = 0;
    if (!Cpus().count(t) || !Attention(t) || !t->gate.compare_exchange_strong(expected, 2)) return;
  }
}

void* ProxyMain(void*) {
  ndkjit::ThreadJit jit;
  memset(&jit, 0, sizeof(jit));
  std::unique_lock<std::mutex> lock(g_proxy_mu);
  for (;;) {
    while (g_proxy_queue.empty()) g_proxy_cv.wait(lock);
    Cpu* t = g_proxy_queue.back();
    g_proxy_queue.pop_back();
    g_proxy_idle--;
    lock.unlock();
    RunProxy(t, &jit);
    lock.lock();
    g_proxy_idle++;
  }
}

// |t| (gate won by the caller) to a helper.
void StartProxy(Cpu* t) {
  std::lock_guard<std::mutex> lock(g_proxy_mu);
  g_proxy_queue.push_back(t);
  if (g_proxy_idle < static_cast<int>(g_proxy_queue.size())) {
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 1024 * 1024);   // host frames of nested guest calls
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_t th;
    static int created;
    if (pthread_create(&th, &a, ProxyMain, nullptr) == 0) {
      g_proxy_idle++;
      if (++created % 8 == 1)
        fprintf(stderr, "ndk_cpu64: %d helper threads for signal handlers (now for tid %d)\n", created, t->tid);
    }
    else fprintf(stderr, "ndk_cpu64: no thread for a signal handler of a thread outside guest code\n");
    pthread_attr_destroy(&a);
  }
  g_proxy_cv.notify_one();
}

// Marks |sig| pending on |c| and wakes it if it waits; a thread outside guest
// code gets a proxy. CpusLock held.
void Post(Cpu* c, int sig) {
  c->sig_pending.fetch_or(Bit(sig));
  if (c->sig_blocked & Bit(sig)) return;
  c->sig_attention.store(1);
  uint32_t w = c->sig_wait_addr.load();
  if (w) FutexWake(reinterpret_cast<volatile void*>(static_cast<uintptr_t>(w)), 0x7fffffff);
  uint32_t expected = 0;
  if (Deliverable(Action(sig)) && c->gate.compare_exchange_strong(expected, 2)) StartProxy(c);
}

// ARC_NDK64_SIGNALS=0: signals to other threads are ESRCH, as before them (to compare).
bool SignalsOff() {
  static const bool off = [] {
    const char* e = getenv("ARC_NDK64_SIGNALS");
    return e && *e == '0';
  }();
  return off;
}

int Send(bool by_tid, uint32_t who, int sig) {
  if (sig < 0 || sig > 64) return -kEINVAL;
  if (SignalsOff()) return -kESRCH;
  std::lock_guard<std::mutex> lock(CpusLock());
  for (Cpu* c : Cpus()) {
    if (by_tid ? static_cast<uint32_t>(c->tid) != who : c->self != who) continue;
    if (sig) Post(c, sig);
    return 0;
  }
  if (!by_tid) {
    auto it = Starting().find(who);
    if (it != Starting().end()) {
      if (sig) it->second |= Bit(sig);
      return 0;
    }
  }
  return -kESRCH;
}

}  // namespace

std::vector<Cpu*> CpusSnapshot() {
  std::lock_guard<std::mutex> lock(CpusLock());
  return std::vector<Cpu*>(Cpus().begin(), Cpus().end());
}

void RegisterCpu(Cpu* c) {
  std::lock_guard<std::mutex> lock(CpusLock());
  Cpus().insert(c);
  auto it = Starting().find(c->self);
  if (it != Starting().end()) {
    c->sig_pending.fetch_or(it->second);
    Starting().erase(it);
    Recompute(c);
  }
}

void UnregisterCpu(Cpu* c) {
  {
    std::lock_guard<std::mutex> lock(CpusLock());
    Cpus().erase(c);
  }
  for (uint32_t g; (g = c->gate.load()) == 2;) FutexWait(&c->gate, g, 50);
}

void GateEnter(Cpu* c) {
  for (;;) {
    uint32_t expected = 0;
    if (c->gate.compare_exchange_strong(expected, 1)) return;
    FutexWait(&c->gate, expected, 50);   // a helper runs this thread's handlers
  }
}

void GateLeave(Cpu* c) {
  c->gate.store(0);
  if (!Attention(c)) return;
  // A signal came while the thread was in guest code but after its last safe
  // point: its handler runs now, by a proxy.
  std::lock_guard<std::mutex> lock(CpusLock());
  uint32_t expected = 0;
  if (Cpus().count(c) && c->gate.compare_exchange_strong(expected, 2)) StartProxy(c);
}

bool SignalSafePoint(Cpu* c) {
  for (;;) {
    int sig = TakeSignal(c, ~c->sig_blocked);
    if (!sig) {
      Recompute(c);
      return false;
    }
    Ndk64Sigaction a = Action(sig);
    if (!Deliverable(a)) continue;
    EnsureSigreturnStub();
    EnterHandler(c, sig, a, c->pc, g_sigreturn_stub);
    return true;
  }
}

}  // namespace ndkcpu64

using namespace ndkcpu64;

extern "C" {

int ndk64_sig_active(void) {
  Cpu* c = CurrentCpuIfAny();
  return c != nullptr;
}

int ndk64_sig_action(int sig, const Ndk64Sigaction* act, Ndk64Sigaction* old) {
  if (sig < 1 || sig > 64) return -kEINVAL;
  if (act && (sig == kSIGKILL || sig == kSIGSTOP)) return -kEINVAL;
  std::lock_guard<std::mutex> lock(g_action_mu);
  if (old) *old = g_actions[sig];
  if (act) {
    g_actions[sig] = *act;
    g_actions[sig].mask &= ~kUnblockable;
  }
  return 0;
}

void ndk64_sig_thread_created(uint32_t thread) {
  std::lock_guard<std::mutex> lock(CpusLock());
  for (Cpu* c : Cpus())
    if (c->self == thread) return;   // (it runs already)
  Starting().emplace(thread, 0);
}

int ndk64_sig_send(int tid, int sig) { return Send(true, static_cast<uint32_t>(tid), sig); }
int ndk64_sig_send_pthread(uint32_t thread, int sig) { return Send(false, thread, sig); }

int ndk64_sig_mask(int how, const uint64_t* set, uint64_t* old) {
  Cpu* c = static_cast<Cpu*>(ndk64_current_state());
  if (old) *old = c->sig_blocked;
  if (set) {
    switch (how) {
      case 0: c->sig_blocked |= *set; break;    // SIG_BLOCK
      case 1: c->sig_blocked &= ~*set; break;   // SIG_UNBLOCK
      case 2: c->sig_blocked = *set; break;     // SIG_SETMASK
      default: return -kEINVAL;
    }
    c->sig_blocked &= ~kUnblockable;
    Recompute(c);
  }
  return 0;
}

uint64_t ndk64_sig_pending(void) {
  Cpu* c = CurrentCpuIfAny();
  return c ? c->sig_pending.load() : 0;
}

int ndk64_sig_deliver(void) {
  Cpu* c = CurrentCpuIfAny();
  if (!c || !Attention(c) || c->depth == 0) return 0;
  return DeliverInHost(c);
}

int ndk64_sig_wait(uint32_t addr, uint32_t val, double ms) {
  volatile uint32_t* p = reinterpret_cast<volatile uint32_t*>(static_cast<uintptr_t>(addr));
  Cpu* c = CurrentCpuIfAny();
  if (!c || c->depth == 0) return FutexWait(p, val, ms);
  int e = WaitInterruptible(c, p, val, ms, 0);
  if (e != 1) return e;
  DeliverInHost(c);
  return -kEINTR;
}

int ndk64_sig_sleep(double ms, double* left_ms) {
  Cpu* c = CurrentCpuIfAny();
  const double end = NowMs() + ms;
  if (left_ms) *left_ms = 0;
  if (!c || c->depth == 0) {
    uint32_t word = 0;
    while (FutexWait(&word, 0, end - NowMs()) == 0 && NowMs() < end) {}
    return 0;
  }
  for (;;) {
    double left = end - NowMs();
    if (left <= 0) return 0;
    int e = WaitInterruptible(c, &c->sig_word, 0, left, 0);
    if (e == 1) {
      if (DeliverInHost(c)) {
        if (left_ms) *left_ms = end - NowMs() > 0 ? end - NowMs() : 0;
        return -kEINTR;
      }
    } else if (e == -kETIMEDOUT) {
      return 0;
    }
  }
}

int ndk64_sig_suspend(const uint64_t* mask) {
  Cpu* c = static_cast<Cpu*>(ndk64_current_state());
  const uint64_t old = c->sig_blocked;
  c->sig_blocked = *mask & ~kUnblockable;
  Recompute(c);
  for (;;) {
    if (Attention(c) && DeliverInHost(c)) break;
    WaitInterruptible(c, &c->sig_word, 0, INFINITY, 0);
  }
  c->sig_blocked = old;
  Recompute(c);
  return -kEINTR;
}

int ndk64_sig_wait_set(uint64_t set, double ms) {
  Cpu* c = static_cast<Cpu*>(ndk64_current_state());
  const double end = isinf(ms) ? INFINITY : NowMs() + ms;
  for (;;) {
    if (int sig = TakeSignal(c, set)) {
      Recompute(c);
      return sig;
    }
    if (Attention(c) && DeliverInHost(c)) return -kEINTR;
    double left = isinf(end) ? INFINITY : end - NowMs();
    if (left <= 0) return -kEAGAIN;
    WaitInterruptible(c, &c->sig_word, 0, left, set);
  }
}

int ndk64_sig_check(void) {
  Cpu* c = CurrentCpuIfAny();
  return c && c->depth && Attention(c);
}

void ndk64_sig_self(uint32_t* thread, int* tid) {
  if (Cpu* c = CurrentCpuIfAny()) {
    if (thread) *thread = c->self;
    if (tid) *tid = c->tid;
    return;
  }
  if (thread) *thread = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pthread_self()));
  if (tid) *tid = gettid();
}

int ndk64_sig_raise(int sig) {
  if (sig < 1 || sig > 64) return -kEINVAL;
  Cpu* c = static_cast<Cpu*>(ndk64_current_state());
  c->sig_pending.fetch_or(Bit(sig));
  Recompute(c);
  if (c->depth) DeliverInHost(c);
  return 0;
}

}  // extern "C"
