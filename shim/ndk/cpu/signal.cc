// Signals to guest threads (topic "arm-cpu", docs/topics/arm-cpu.md,
// "Сигналы гостевому коду"). There is no kernel to interrupt a wasm thread,
// so a signal is a pending bit on the target thread's Cpu that the thread
// itself acts on at safe points:
//
//  - in the interpreter, at a block boundary (exec.cc Run, and the back
//    edge of translated loops, jit.cc): the handler starts in the same Run,
//    on the thread's guest stack below a Linux-style frame (siginfo +
//    ucontext with the interrupted registers, which a conservative GC such
//    as Boehm's scans), returning to a host stub that does rt_sigreturn;
//  - inside a host function that waits (ndk_sig_wait and the others below,
//    used by the bridge's futex, mutexes, condition variables, semaphores,
//    sleeps, sigsuspend): the sender wakes the futex the thread sleeps on,
//    and the handler runs nested from the host function (a nested Run on
//    the same guest stack), after which the wait returns EINTR or goes on;
//  - a thread outside guest code altogether (a Java thread that called
//    into native code before and is back in Java now): a helper thread
//    ("proxy") runs the handler with that thread's CPU, on its guest stack,
//    as that thread (pthread_self/gettid of the bridge ask the CPU), while
//    the thread itself waits at its next entry into guest code (Cpu::gate);
//  - an undefined instruction with a guest SIGILL handler (SignalFault).
//
// A thread that is in host code below guest code that does not wait here (a
// JNI call into Java, a blocking read) gets its signals when it returns to
// guest code.
// Numbers are Linux ARM's (1..64), errno values Linux's.
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <condition_variable>
#include <mutex>
#include <set>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten/threading.h>
#endif

#include "ndk/cpu/cpu_internal.h"
#include "ndk/cpu/ndk_cpu_ext.h"

namespace ndkcpu {

std::mutex& CpusLock();
std::set<Cpu*>& Cpus();

namespace {

// Linux errno values (the API reports them; the host's differ).
constexpr int kEINTR = 4, kEAGAIN = 11, kESRCH = 3, kEINVAL = 22, kETIMEDOUT = 110;
constexpr int kSIGILL = 4, kSIGKILL = 9, kSIGSTOP = 19;
constexpr uint32_t kSA_SIGINFO = 4, kSA_NODEFER = 0x40000000, kSA_RESETHAND = 0x80000000u,
                   kSA_RESTART = 0x10000000, kSA_ONSTACK = 0x08000000;
constexpr uint32_t kSS_ONSTACK = 1, kSS_DISABLE = 2;
constexpr int kSI_TKILL = -6;

inline uint64_t Bit(int sig) { return 1ull << (sig - 1); }
// Signals that cannot be blocked.
constexpr uint64_t kUnblockable = (1ull << (kSIGKILL - 1)) | (1ull << (kSIGSTOP - 1));

// The process's actions. Read without the lock by delivering threads (a
// torn read of a changing action delivers either one, as on Linux).
std::mutex g_action_mu;
NdkSigaction g_actions[65];

NdkSigaction Action(int sig) {
  std::lock_guard<std::mutex> lock(g_action_mu);
  return g_actions[sig];
}

// --- the frame on the guest stack ---------------------------------------------------
// As Linux's rt_sigframe on ARM: siginfo, then the ucontext (struct
// sigcontext with r0-r15 and cpsr, the 64-bit mask, VFP registers in
// uc_regspace), then our check word.
struct GSiginfo {
  int32_t signo, err, code;
  uint32_t a, b, c;   // _kill: pid, uid; _sigfault: addr
  uint32_t pad[26];
};
static_assert(sizeof(GSiginfo) == 128, "siginfo");
struct GUcontext {
  uint32_t flags, link;
  uint32_t ss_sp, ss_flags, ss_size;
  uint32_t trap_no, error_code, oldmask;
  uint32_t r[16];   // arm_r0..arm_r10, fp, ip, sp, lr, pc
  uint32_t cpsr, fault_address;
  uint32_t sigmask[2];
  uint32_t unused[30];
  // uc_regspace: struct vfp_sigframe
  uint32_t vfp_magic, vfp_size;
  uint64_t fpregs[32];
  uint32_t fpscr, fpexc, fpinst, fpinst2;
  uint32_t regspace_rest[128 - 2 - 64 - 4];
};
static_assert(offsetof(GUcontext, r) == 32, "sigcontext");
static_assert(offsetof(GUcontext, sigmask) == 104, "uc_sigmask");
static_assert(offsetof(GUcontext, vfp_magic) == 232, "uc_regspace");
static_assert(sizeof(GUcontext) == 744, "ucontext");
struct GFrame {
  GSiginfo info;
  GUcontext uc;
  uint32_t magic, sig;
};
constexpr uint32_t kFrameMagic = 0x4e534947;   // "NSIG"

uint32_t g_sigreturn_stub;

void Recompute(Cpu* c) {
  c->sig_attention.store(0);
  if ((c->sig_pending.load() & ~c->sig_blocked) || c->yield_req.load()) c->sig_attention.store(1);
}

// Takes the lowest pending unblocked signal off the thread (0: none).
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
// (r[] with r[15] = |pc| and c->thumb) and points the CPU at the handler
// with lr = |lr|. Returns the frame address.
bool OnAltStack(const Cpu* c) {
  return !(c->alt_flags & kSS_DISABLE) && c->alt_size && c->r[13] - c->alt_sp < c->alt_size;
}

uint32_t EnterHandler(Cpu* c, int sig, const NdkSigaction& a, uint32_t pc, int code, uint32_t addr,
                      uint32_t lr) {
  // SA_ONSTACK with an alternate stack: the frame goes on top of it (unless
  // the thread is there already: a nested signal).
  const bool alt = (a.flags & kSA_ONSTACK) && !(c->alt_flags & kSS_DISABLE) && c->alt_size;
  const uint32_t top = alt && !OnAltStack(c) ? c->alt_sp + c->alt_size : c->r[13];
  uint32_t sp = (top - sizeof(GFrame)) & ~7u;
  GFrame* f = static_cast<GFrame*>(HostPtr(sp));
  memset(f, 0, sizeof(*f));
  f->info.signo = sig;
  f->info.code = code;
  if (code == kSI_TKILL) {
    f->info.a = getpid();
  } else {
    f->info.a = addr;
  }
  GUcontext& uc = f->uc;
  memcpy(uc.r, c->r, sizeof(uc.r));
  uc.r[15] = pc;
  uc.cpsr = PackCpsr(c);
  uc.fault_address = addr;
  uc.trap_no = sig == kSIGILL ? 6 : 0;
  uc.sigmask[0] = (uint32_t)c->sig_blocked;
  uc.sigmask[1] = (uint32_t)(c->sig_blocked >> 32);
  uc.oldmask = uc.sigmask[0];
  uc.ss_sp = c->alt_sp;
  uc.ss_size = c->alt_size;
  uc.ss_flags = c->alt_size ? c->alt_flags : kSS_DISABLE;
  uc.vfp_magic = 0x56465001;
  uc.vfp_size = 4 * 2 + 8 * 32 + 4 * 4;
  memcpy(uc.fpregs, c->d, sizeof(uc.fpregs));
  uc.fpscr = c->fpscr;
  uc.fpexc = 0x40000000;
  f->magic = kFrameMagic;
  f->sig = sig;

  c->sig_blocked |= (a.mask | ((a.flags & kSA_NODEFER) ? 0 : Bit(sig))) & ~kUnblockable;
  if (a.flags & kSA_RESETHAND) {
    std::lock_guard<std::mutex> lock(g_action_mu);
    g_actions[sig].handler = 0;
    g_actions[sig].flags &= ~kSA_SIGINFO;
  }
  Recompute(c);
  c->r[0] = sig;
  c->r[1] = sp + offsetof(GFrame, info);
  c->r[2] = sp + offsetof(GFrame, uc);
  c->r[13] = sp;
  c->r[14] = lr;
  c->r[15] = a.handler & ~1u;
  c->thumb = a.handler & 1;
  c->ex_valid = false;
  return sp;
}

// rt_sigreturn: the state from the frame at |sp| (the handler may have
// changed it). r[15] gets the resume pc with bit 0 = Thumb.
void LeaveHandler(Cpu* c, uint32_t sp) {
  GFrame* f = static_cast<GFrame*>(HostPtr(sp));
  if (f->magic != kFrameMagic) Fault(c, c->r[14], "signal return with a damaged frame at sp=%08x", sp);
  const GUcontext& uc = f->uc;
  memcpy(c->r, uc.r, sizeof(c->r));
  UnpackCpsr(c, uc.cpsr);
  c->cpsr = uc.cpsr;
  memcpy(c->d, uc.fpregs, sizeof(uc.fpregs));
  c->fpscr = uc.fpscr;
  c->sig_blocked = (uc.sigmask[0] | (uint64_t)uc.sigmask[1] << 32) & ~kUnblockable;
  c->ex_valid = false;
  c->r[15] = (c->r[15] & ~1u) | (c->thumb ? 1 : 0);
  Recompute(c);
}

void SigreturnStub(NdkCpuState* s, void*) {
  Cpu* c = static_cast<Cpu*>(s);
  LeaveHandler(c, c->r[13]);
  // OP_HOSTCALL continues at r[15] (it differs from the stub's address).
}

void EnsureSigreturnStub() {
  static std::once_flag once;
  std::call_once(once, [] { g_sigreturn_stub = ndk_host_stub(SigreturnStub, nullptr, "<signal return>"); });
}

bool Deliverable(const NdkSigaction& a) { return a.handler > 1; }   // not SIG_DFL / SIG_IGN

// Runs the handlers of the pending unblocked signals from a host function
// (|c| is in a host call: r[15] is the stub's address). Each handler runs in
// a nested Run and returns to the "return to host" stub. Returns 0 if none
// ran, 1 if all that ran had SA_RESTART, 2 otherwise.
int DeliverInHost(Cpu* c) {
  int result = 0;
  for (;;) {
    int sig = TakeSignal(c, ~c->sig_blocked);
    if (!sig) break;
    NdkSigaction a = Action(sig);
    if (!Deliverable(a)) continue;
    EnsureSigreturnStub();
    const uint32_t pc = c->r[15] | (c->thumb ? 1 : 0);
    const uint32_t in_host = c->in_host;
    const bool ex = c->ex_valid;
    uint32_t sp = EnterHandler(c, sig, a, pc, kSI_TKILL, 0, ReturnStubAddr());
    Run(c);
    LeaveHandler(c, sp);
    c->thumb = c->r[15] & 1;
    c->r[15] &= ~1u;
    c->in_host = in_host;
    c->ex_valid = ex;
    c->cpsr = PackCpsr(c);
    result = (result == 2 || !(a.flags & kSA_RESTART)) ? 2 : 1;
  }
  Recompute(c);
  return result;
}

bool Attention(Cpu* c) { return c->sig_attention.load() != 0; }

// A parked green thread's hook into its scheduler (ndk_cpu_set_green_hook).
std::atomic<void (*)(NdkCpuState*)> g_green_hook{nullptr};

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

// A wake from a sender can come between the check of sig_attention and the
// futex wait and be lost; waits are cut into slices so such a signal waits
// one slice at most.
constexpr double kSliceMs = 50;

// The interruptible futex wait under the API functions. |stop|: which
// pending signals end it (beside unblocked ones, for sigwait). Returns 0
// (woken), -ETIMEDOUT, -EAGAIN (value differs) or 1 (a signal is there:
// the caller delivers or takes it).
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
    if (e == -kETIMEDOUT) continue;   // a slice (or the deadline: checked above)
    if (e == 0 && (Attention(c) || (c->sig_pending.load() & stop))) return 1;
    return e;
  }
}

// --- proxies: handlers for threads outside guest code -------------------------------
// A helper thread takes a CPU whose gate it won (0 -> 2) and runs that
// thread's pending handlers on it. Helpers stay for the next request; a new
// one is started when none is free (a stopped world keeps one helper per
// thread outside guest code, waiting in the handler's sigsuspend).
// (Never destroyed: helpers wait on them while the process exits.)
std::mutex& g_proxy_mu = *new std::mutex;
std::condition_variable& g_proxy_cv = *new std::condition_variable;
std::vector<Cpu*>& g_proxy_queue = *new std::vector<Cpu*>;
int g_proxy_idle;

void RunProxy(Cpu* t, ndkjit::ThreadJit* jit) {
  for (;;) {
    Cpu* own = SetCurrentCpu(t);
    GuestNesting()++;   // guest calls from the handler's host calls: no gate
    // Translations are instantiated per thread: this helper's table, not t's.
    ndkjit::ThreadJit theirs = t->jit;
    t->jit = *jit;
    DeliverInHost(t);
    *jit = t->jit;
    t->jit = theirs;
    GuestNesting()--;
    SetCurrentCpu(own);
    // Release; again if a signal came meanwhile (its sender found the gate
    // taken) and the thread is still outside guest code.
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
  if (g_proxy_idle < (int)g_proxy_queue.size()) {
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 1024 * 1024);   // host frames of nested guest calls
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_t th;
    if (pthread_create(&th, &a, ProxyMain, nullptr) == 0) g_proxy_idle++;
    else fprintf(stderr, "ndk_cpu: no thread for a signal handler of a thread outside guest code\n");
    pthread_attr_destroy(&a);
  }
  g_proxy_cv.notify_one();
}

}  // namespace

void GateLeave(Cpu* c) {
  c->gate.store(0);
  if (!Attention(c)) return;
  // A signal came while the thread was in guest code but after its last
  // safe point: its handler runs now, by a proxy.
  std::lock_guard<std::mutex> lock(CpusLock());
  uint32_t expected = 0;
  if (Cpus().count(c) && c->gate.compare_exchange_strong(expected, 2)) StartProxy(c);
}

// --- safe points of the interpreter --------------------------------------------------

bool SignalSafePoint(Cpu* c) {
  for (;;) {
    int sig = TakeSignal(c, ~c->sig_blocked);
    if (!sig) {
      Recompute(c);
      return false;
    }
    NdkSigaction a = Action(sig);
    if (!Deliverable(a)) continue;
    EnsureSigreturnStub();
    const uint32_t pc = c->r[15] | (c->thumb ? 1 : 0);
    EnterHandler(c, sig, a, pc, kSI_TKILL, 0, g_sigreturn_stub);
    return true;
  }
}

bool GreenYieldPoint(Cpu* c) {
  if (!c->green || c->depth != 1 || !c->yield_req.load(std::memory_order_relaxed)) return false;
  c->yield_req.store(0);
  Recompute(c);
  c->park = kNdkParkYield;
  return true;
}

// Whether a pending signal of |c| would run a handler now (a parked green
// thread is resumed for it).
bool GreenSignalDeliverable(Cpu* c) {
  uint64_t ready = c->sig_pending.load() & ~c->sig_blocked;
  for (int sig = 1; ready; ++sig, ready >>= 1)
    if ((ready & 1) && Deliverable(Action(sig))) return true;
  return false;
}

bool SignalFault(Cpu* c, int sig, int code, uint32_t pc) {
  if (c->sig_blocked & Bit(sig)) return false;
  NdkSigaction a = Action(sig);
  if (!Deliverable(a)) return false;
  EnsureSigreturnStub();
  c->r[15] = pc;
  EnterHandler(c, sig, a, pc | (c->thumb ? 1 : 0), code, pc, g_sigreturn_stub);
  return true;
}

}  // namespace ndkcpu

using namespace ndkcpu;

// A thread running arm64 code: the AArch64 CPU's signals (shim/ndk/cpu64/signal64.cc;
// the bridge's API functions are shared by both ABIs).
extern "C" {
int ndk64_sig_active(void) __attribute__((weak));
int ndk64_sig_send(int tid, int sig) __attribute__((weak));
int ndk64_sig_send_pthread(uint32_t thread, int sig) __attribute__((weak));
int ndk64_sig_mask(int how, const uint64_t* set, uint64_t* old) __attribute__((weak));
uint64_t ndk64_sig_pending(void) __attribute__((weak));
int ndk64_sig_deliver(void) __attribute__((weak));
int ndk64_sig_wait(uint32_t addr, uint32_t val, double ms) __attribute__((weak));
int ndk64_sig_sleep(double ms, double* left_ms) __attribute__((weak));
int ndk64_sig_suspend(const uint64_t* mask) __attribute__((weak));
int ndk64_sig_wait_set(uint64_t set, double ms) __attribute__((weak));
int ndk64_sig_check(void) __attribute__((weak));
void ndk64_sig_self(uint32_t* thread, int* tid) __attribute__((weak));
int ndk64_sig_raise(int sig) __attribute__((weak));
}
static bool Use64() {
  Cpu* c = CurrentCpuIfAny();
  if (c && c->depth) return false;
  return ndk64_sig_active && ndk64_sig_active();
}

extern "C" {

int ndk_sig_action(int sig, const NdkSigaction* act, NdkSigaction* old) {
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

// Marks |sig| pending on |c| and wakes it if it waits; a thread outside
// guest code gets a proxy. CpusLock held.
static void Post(Cpu* c, int sig) {
  c->sig_pending.fetch_or(Bit(sig));
  if (c->sig_blocked & Bit(sig)) return;
  c->sig_attention.store(1);
  uint32_t w = c->sig_wait_addr.load();
  if (w) FutexWake(HostPtr(w), 0x7fffffff);
  uint32_t expected = 0;
  if (Deliverable(Action(sig)) && c->gate.compare_exchange_strong(expected, 2)) {
    StartProxy(c);
  } else if (expected == 3 && Deliverable(Action(sig))) {   // a parked green thread
    if (void (*hook)(NdkCpuState*) = g_green_hook.load()) hook(c);
  }
}

void ndk_cpu_set_green_hook(void (*hook)(NdkCpuState* s)) { g_green_hook.store(hook); }

static int Send(bool by_tid, uint32_t who, int sig) {
  if (sig < 0 || sig > 64) return -kEINVAL;
  std::lock_guard<std::mutex> lock(CpusLock());
  for (Cpu* c : Cpus()) {
    if (by_tid ? (uint32_t)c->tid != who : c->self != who) continue;
    if (sig) Post(c, sig);
    return 0;
  }
  return -kESRCH;
}

int ndk_sig_send(int tid, int sig) {
  int r = Send(true, (uint32_t)tid, sig);
  if (r == -kESRCH && ndk64_sig_send) r = ndk64_sig_send(tid, sig);
  return r;
}
int ndk_sig_send_pthread(uint32_t thread, int sig) {
  int r = Send(false, thread, sig);
  if (r == -kESRCH && ndk64_sig_send_pthread) r = ndk64_sig_send_pthread(thread, sig);
  return r;
}

int ndk_sig_has_handler(int sig) {
  if (sig < 1 || sig > 64) return 0;
  return Deliverable(Action(sig));
}

int ndk_sig_altstack(const uint32_t* ss, uint32_t* old) {
  Cpu* c = static_cast<Cpu*>(ndk_current_state());
  const bool on = OnAltStack(c);
  uint32_t prev[3] = {c->alt_sp, c->alt_size ? c->alt_flags | (on ? kSS_ONSTACK : 0) : kSS_DISABLE,
                      c->alt_size};
  if (ss) {
    if (on) return -1;   // EPERM
    if (ss[1] & kSS_DISABLE) {
      c->alt_sp = c->alt_size = 0;
      c->alt_flags = kSS_DISABLE;
    } else {
      c->alt_sp = ss[0];
      c->alt_size = ss[2];
      c->alt_flags = 0;
    }
  }
  if (old) memcpy(old, prev, sizeof prev);
  return 0;
}

int ndk_sig_mask(int how, const uint64_t* set, uint64_t* old) {
  if (Use64()) return ndk64_sig_mask(how, set, old);
  Cpu* c = static_cast<Cpu*>(ndk_current_state());
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

uint64_t ndk_sig_pending(void) {
  if (Use64()) return ndk64_sig_pending();
  Cpu* c = CurrentCpuIfAny();
  return c ? c->sig_pending.load() : 0;
}

int ndk_sig_deliver(void) {
  if (Use64()) return ndk64_sig_deliver();
  Cpu* c = CurrentCpuIfAny();
  if (!c || !Attention(c) || c->depth == 0) return 0;
  return DeliverInHost(c);
}

int ndk_sig_wait(uint32_t addr, uint32_t val, double ms) {
  if (Use64()) return ndk64_sig_wait(addr, val, ms);
  volatile uint32_t* p = static_cast<volatile uint32_t*>(HostPtr(addr));
  Cpu* c = CurrentCpuIfAny();
  if (!c || c->depth == 0) return FutexWait(p, val, ms);
  int e = WaitInterruptible(c, p, val, ms, 0);
  if (e != 1) return e;
  DeliverInHost(c);
  return -kEINTR;
}

int ndk_sig_sleep(double ms, double* left_ms) {
  if (Use64()) return ndk64_sig_sleep(ms, left_ms);
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

int ndk_sig_suspend(const uint64_t* mask) {
  if (Use64()) return ndk64_sig_suspend(mask);
  Cpu* c = static_cast<Cpu*>(ndk_current_state());
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

int ndk_sig_wait_set(uint64_t set, double ms) {
  if (Use64()) return ndk64_sig_wait_set(set, ms);
  Cpu* c = static_cast<Cpu*>(ndk_current_state());
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

int ndk_sig_check(void) {
  if (Use64()) return ndk64_sig_check();
  Cpu* c = CurrentCpuIfAny();
  return c && c->depth && Attention(c);
}

void ndk_sig_self(uint32_t* thread, int* tid) {
  if (Use64()) {
    ndk64_sig_self(thread, tid);
    return;
  }
  if (Cpu* c = CurrentCpuIfAny()) {
    if (thread) *thread = c->self;
    if (tid) *tid = c->tid;
    return;
  }
  if (thread) *thread = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pthread_self()));
  if (tid) *tid = gettid();
}

int ndk_sig_raise(int sig) {
  if (Use64()) return ndk64_sig_raise(sig);
  if (sig < 1 || sig > 64) return -kEINVAL;
  Cpu* c = static_cast<Cpu*>(ndk_current_state());
  c->sig_pending.fetch_or(Bit(sig));
  Recompute(c);
  if (c->depth) DeliverInHost(c);
  return 0;
}

}  // extern "C"
