// Dormant waits of Java threads on ART 10 (topic java-pool-park,
// docs/topics/java-pool-park.md); see dormant_wasm10.h.
//
// Every Android thread is a pthread, and every pthread a Web Worker with a
// JavaScriptCore heap of its own (~5 MB on an iPhone). Most Java threads of
// a big app wait most of the time: idle pool threads in LockSupport.park,
// daemons and watchdogs in Object.wait / Thread.sleep, HandlerThreads in an
// empty Looper. Such a wait cannot give the worker back with its stack (no
// stack switching in WebAssembly on iOS 16-18), but the interpreter's frames
// are data (ShadowFrame): a thread can be continued from them, as ART
// continues deoptimized frames.
//
// So a thread that has waited longer than the grace period goes dormant:
//  1. Capture: its Java frames (flat frames of the interpreter, frames of
//     compiled code, which write their registers back before any call) are
//     copied to the heap as deoptimized frames and become the thread's whole
//     managed stack (the GC sees them as roots, stack traces show them). The
//     C++ frames on the way (the call of run(), the native that waits) are
//     dropped; only stacks that can be continued qualify: one invocation from
//     Thread.run (no JNI, reflection, class initializer or proxy in between),
//     every frame at an invoke, no native code of the app on the thread (the
//     guest CPU), no WebGL context of the thread, no lock counting.
//  2. The thread stays in the thread list, suspended in its wait state
//     (WAITING / TIMED_WAITING / SLEEPING / NATIVE); its pthread exits and the
//     worker goes back to Emscripten's pool (the page terminates spare ones).
//  3. What ends the wait (Thread::Unpark, the monitor's notify, an interrupt,
//     a ready file descriptor, the timeout, on the page's thread) claims the
//     record and starts a new pthread. It takes over the art::Thread (TLS,
//     pthread, tid, stack bounds), finishes the wait as the native would
//     (the park's permit; the monitor reacquired, InterruptedException), and
//     continues the frames after the call that waited; when the bottom one
//     (run()) returns, the thread ends as Thread::CreateCallback ends it.
// A thread may go dormant again on its new pthread (the frames not yet
// continued are reused as they are).
//
// Switches: ARC_JAVA_DORMANT=0 (off), ARC_JAVA_DORMANT_GRACE_MS (1500),
// ARC_JAVA_DORMANT_LOG=1 (each dormancy and wake to stderr). Part of the
// arc-wasm shim layer.

#include "dormant_wasm10.h"

#include <errno.h>
#include <math.h>
#include <sys/epoll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <emscripten.h>
#include <emscripten/threading.h>

#include "art_method-inl.h"
#include "base/mutex.h"
#include "dex/code_item_accessors-inl.h"
#include "dex/dex_instruction-inl.h"
#include "interpreter/interpreter_common.h"
#include "interpreter/shadow_frame.h"
#include "jni/jni_env_ext.h"
#include "managed_stack.h"
#include "monitor.h"
#include "runtime.h"
#include "runtime_callbacks.h"
#include "thread-inl.h"
#include "thread_list.h"

// What else ties a pthread to its worker (weak: not every program links them).
extern "C" int ndk_cpu_thread_has_state(void) __attribute__((weak));
extern "C" int ndk_cpu64_thread_has_state(void) __attribute__((weak));
extern "C" int arc_gl_thread_bound(void) __attribute__((weak));
extern "C" void arc_lproc_get_thread(int* pid, int* uid) __attribute__((weak));
extern "C" void arc_lproc_set_thread(int pid, int uid) __attribute__((weak));

// For the page (run.html's 5-second line) and tests: dormant now, went
// dormant, woken, not eligible when the grace ended, new pthreads that failed.
extern "C" EMSCRIPTEN_KEEPALIVE uint32_t arc_dormant_counts[5] = {0, 0, 0, 0, 0};

namespace art {

namespace {

enum Kind : int { kPark = 1, kMonitor = 2, kPoll = 3 };
// Record states.
enum : int { kAwake = 0, kDormant = 1, kClaimed = 2 };
// Thread::park_state_ (thread.cc).
enum : int { kParkPermitAvailable = 0, kParkNoPermit = 1, kParkWaiterWaiting = 2 };

double MonoMs() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

int EnvInt(const char* name, int dflt) {
  const char* v = getenv(name);
  return v && *v ? atoi(v) : dflt;
}

// Which waits may go dormant: ARC_JAVA_DORMANT_KINDS, a mask of 1 park, 2
// Object.wait/sleep, 4 an idle Looper (7, all).
bool KindOn(int kind) {
  static const int mask = EnvInt("ARC_JAVA_DORMANT_KINDS", 7);
  return (mask >> (kind - 1)) & 1;
}

bool Enabled() {
  // (Compiled code with its frames in wasm locals, ARC_JIT_WASM_FRAMES, does
  // not write them all back before calls.)
  static const bool on = EnvInt("ARC_JAVA_DORMANT", 1) != 0 &&
                         EnvInt("ARC_JIT_WASM_FRAMES", 0) == 0;
  return on;
}
// (-1: not read yet; tests set it: art_dormant_set_grace)
std::atomic<int> g_grace_ms(-1);
double GraceMs() {
  int g = g_grace_ms.load(std::memory_order_relaxed);
  if (g < 0) {
    g = EnvInt("ARC_JAVA_DORMANT_GRACE_MS", 1500);
    g_grace_ms.store(g, std::memory_order_relaxed);
  }
  return g;
}
// A timed wait goes dormant only with at least this much of it left.
double MinRestMs() { return std::min(300.0, GraceMs()); }
bool Log() {
  static const bool on = EnvInt("ARC_JAVA_DORMANT_LOG", 0) != 0;
  return on;
}

const char* KindName(int k) {
  return k == kPark ? "park" : k == kMonitor ? "wait" : "poll";
}

// What ties the calling pthread to its worker besides ART, or null.
const char* NativeTie() {
  if (ndk_cpu_thread_has_state && ndk_cpu_thread_has_state()) return "guest code";
  if (ndk_cpu64_thread_has_state && ndk_cpu64_thread_has_state()) return "guest code";
  if (arc_gl_thread_bound && arc_gl_thread_bound()) return "WebGL";
  return nullptr;
}

// Whether nothing outside ART ties it (said once per thread in the log).
__thread bool t_tie_told = false;
bool NativeStateFree() {
  const char* tie = NativeTie();
  if (tie == nullptr) return true;
  if (Log() && !t_tie_told) {
    t_tie_told = true;
    std::string name;
    if (Thread* self = Thread::Current()) self->GetThreadName(name);
    fprintf(stderr, "art dormant: \"%s\" stays on its worker (%s)\n", name.c_str(), tie);
  }
  return false;
}

bool ResumableInvoke(const Instruction* inst) {
  switch (inst->Opcode()) {
    case Instruction::INVOKE_POLYMORPHIC:
    case Instruction::INVOKE_POLYMORPHIC_RANGE:
    case Instruction::INVOKE_CUSTOM:
    case Instruction::INVOKE_CUSTOM_RANGE:
      return false;
    default:
      return inst->IsInvoke();
  }
}

// The heap frames the current pthread is continuing (ContinueFrames): from
// here on, a capture reuses them instead of copying.
__thread ShadowFrame* t_floor = nullptr;

}  // namespace

struct ArtWasmDormant::Rec {
  Thread* thread = nullptr;
  int id = 0;
  std::atomic<int> state{kAwake};
  uint32_t gen = 0;            // dormancy number (older timers are stale)
  // The thread's base (CreateCallback's, or Resume's): JNI local references,
  // handle scopes.
  IRTSegmentState base_cookie;
  IRTSegmentState base_segment;
  BaseHandleScope* base_handles = nullptr;
  // The frames to continue: the innermost, linked to the outer ones.
  ShadowFrame* frames = nullptr;
  int kind = kPark;
  double deadline = INFINITY;  // monotonic ms
  bool timed_out = false;
  bool signalled = false;      // (monitor) a notify/interrupt came (wait mutex held)
  size_t stack_size = 0;
  int lproc_pid = 0, lproc_uid = 0;
  char name[48] = "";          // (log)
  // kMonitor: what Monitor::Wait restores.
  Monitor* monitor = nullptr;
  int prev_lock_count = 0;
  ArtMethod* saved_method = nullptr;
  uintptr_t saved_dex_pc = 0;
  bool interrupt_should_throw = false;
  // kPoll
  int fd = -1;
  void (*on_resume)(void*) = nullptr;
  void* resume_arg = nullptr;
  const char* why = "";        // what woke it (log)
  // Its new pthread could not be started: the page's timer tries again
  // (only then: a timer that finds the record claimed otherwise is late).
  std::atomic<bool> respawn{false};
  double since = 0;            // when it went dormant (log)
};

namespace {

typedef ArtWasmDormant::Rec Rec;

std::mutex g_mu;  // g_recs, g_pollers
std::unordered_map<int, Rec*>* g_recs;
std::vector<Rec*>* g_pollers;  // kPoll records while dormant
int g_next_id = 1;

// Claims a dormant record for its wake (one waker wins).
bool Claim(Rec* r) {
  int s = kDormant;
  return r->state.compare_exchange_strong(s, kClaimed, std::memory_order_seq_cst);
}

std::atomic<int> g_npollers(0);  // g_pollers' size, read without the lock

void ForgetPollerLocked(Rec* r) {
  for (size_t i = 0; i < g_pollers->size(); ++i) {
    if ((*g_pollers)[i] == r) {
      g_pollers->erase(g_pollers->begin() + i);
      break;
    }
  }
  g_npollers.store(static_cast<int>(g_pollers->size()));
}

// --- the page's timers --------------------------------------------------------------

EM_JS(void, art_dormant_js_timeout, (int id, int gen, int ms), {
  var t = setTimeout(function() {
    if (ABORT || (typeof runtimeExited != 'undefined' && runtimeExited)) return;
    _art_dormant_timeout(id, gen);
  }, ms);
  if (t && t.unref) t.unref();
});

void ScheduleOnPage(int id, int gen, int ms) { art_dormant_js_timeout(id, gen, ms); }

void Schedule(int id, uint32_t gen, double ms) {
  int m = ms < 1 ? 1 : ms > 2e9 ? 2000000000 : static_cast<int>(ceil(ms));
  emscripten_async_run_in_main_runtime_thread(EM_FUNC_SIG_VIII, reinterpret_cast<void*>(ScheduleOnPage),
                                              id, static_cast<int>(gen), m);
}

// --- spare carriers ---------------------------------------------------------------
// A new pthread needs the page's thread (Emscripten starts workers there),
// and the page may be waiting for this very Java thread: a WebView's
// synchronous @JavascriptInterface call spins on the page until a Java
// thread answers, and the answer may need a dormant thread (a Handler of a
// sleeping HandlerThread) - which then waits for the page. So a few pthreads
// stay ready (ARC_JAVA_DORMANT_SPARES, 2) once threads go dormant: a wake
// hands the thread to one of them without the page, and a new spare is
// asked for (that one may wait). A spare whose thread ends goes back to
// waiting; one whose thread goes dormant again exits with it.
struct Spare {
  std::atomic<int32_t> has_job{0};
  Thread* job = nullptr;
};
std::mutex g_spare_mu;
std::vector<Spare*>* g_idle_spares;   // waiting for a job
int g_spares = 0;                     // alive (idle, busy) or starting
size_t g_spare_stack = 0;             // their stack size (the first dormant thread's)
__thread Spare* t_spare = nullptr;    // the spare this pthread is

int SparesWanted() {
  static const int n = EnvInt("ARC_JAVA_DORMANT_SPARES", 2);
  return n;
}

void* SpareMain(void* arg);

// Starts spares up to the number wanted (asynchronously: the page's thread
// gives them workers when it can).
void EnsureSpares() {
  for (;;) {
    size_t stack;
    {
      std::lock_guard<std::mutex> l(g_spare_mu);
      if (g_spares >= SparesWanted() || g_spare_stack == 0) return;
      g_spares++;
      stack = g_spare_stack;
    }
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, stack);
    pthread_t t;
    const int rc = pthread_create(&t, &attr, SpareMain, new Spare());
    pthread_attr_destroy(&attr);
    if (rc != 0) {
      std::lock_guard<std::mutex> l(g_spare_mu);
      g_spares--;
      return;
    }
  }
}

// The spare is gone (its thread went dormant again, or it quit).
void SpareGone(Spare* s) {
  {
    std::lock_guard<std::mutex> l(g_spare_mu);
    g_spares--;
  }
  delete s;
}

// An idle spare for a thread with |stack_size|, taken, or null.
Spare* TakeSpare(size_t stack_size) {
  std::lock_guard<std::mutex> l(g_spare_mu);
  if (!g_idle_spares || g_idle_spares->empty() || stack_size > g_spare_stack) return nullptr;
  Spare* s = g_idle_spares->back();
  g_idle_spares->pop_back();
  return s;
}

void* SpareMain(void* arg) {
  Spare* s = static_cast<Spare*>(arg);
  t_spare = s;
  for (;;) {
    {
      std::lock_guard<std::mutex> l(g_spare_mu);
      if (!g_idle_spares) g_idle_spares = new std::vector<Spare*>;
      g_idle_spares->push_back(s);
    }
    while (s->has_job.load() == 0) {
      emscripten_futex_wait(reinterpret_cast<volatile void*>(&s->has_job), 0, INFINITY);
    }
    s->has_job.store(0);
    ArtWasmDormant::Resume(s->job);   // (exits the pthread if its thread goes dormant again)
    // Its thread ended. Native state of it (guest CPU, WebGL) must not reach the next one.
    if (NativeTie() != nullptr) break;
  }
  t_spare = nullptr;
  SpareGone(s);
  return nullptr;
}

// Starts the pthread that continues |r|'s thread (claimed): a spare, or a
// new one. A failure (no worker now) is retried from the page's timer.
void Spawn(Rec* r, const char* why) {
  r->why = why;
  if (Spare* s = TakeSpare(r->stack_size)) {
    s->job = r->thread;
    s->has_job.store(1);
    emscripten_futex_wake(reinterpret_cast<volatile void*>(&s->has_job), 1);
    EnsureSpares();
    return;
  }
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  if (r->stack_size) pthread_attr_setstacksize(&attr, r->stack_size);
  pthread_t t;
  int rc = pthread_create(&t, &attr, &ArtWasmDormant::Resume, r->thread);
  pthread_attr_destroy(&attr);
  if (rc != 0) {
    __atomic_fetch_add(&arc_dormant_counts[4], 1, __ATOMIC_RELAXED);
    fprintf(stderr, "art dormant: no pthread for \"%s\" (%d): again in 50 ms\n", r->name, rc);
    r->respawn.store(true);
    Schedule(r->id, r->gen, 50);
  }
}

}  // namespace

}  // namespace art

// For tests (art/test_natives.cc): arc_dormant_counts[what] (-1 past them),
// and the grace period.
extern "C" int art_dormant_stat(int what) {
  return what >= 0 && what < 5 ? static_cast<int>(__atomic_load_n(&arc_dormant_counts[what], __ATOMIC_RELAXED)) : -1;
}
extern "C" void art_dormant_set_grace(int ms) { art::g_grace_ms.store(ms, std::memory_order_relaxed); }

// For the framework's MessageQueue (patches/aosp10/zz-pool-park.patch,
// android_os_MessageQueue.cpp): an idle Looper's wait on its epoll, called in
// the native state; the grace period, ms.
extern "C" int arc_java_dormant_epoll(int epfd, int timeout_ms, void (*on_resume)(void*), void* arg) {
  art::Thread* self = art::Thread::Current();
  return self == nullptr ? 0 : art::ArtWasmDormant::EpollDormant(self, epfd, timeout_ms, on_resume, arg);
}
extern "C" int arc_java_dormant_grace_ms(void) {
  return art::Enabled() ? static_cast<int>(art::GraceMs()) : -1;
}
// For the file system (upstream posix_translation, epoll_stream.cc).
extern "C" int arc_dormant_pollers_now(void) { return art::g_npollers.load(std::memory_order_relaxed); }
extern "C" void arc_dormant_fd_ready(int epfd) { art::ArtWasmDormant::EpollReady(epfd); }

// The page's timer: a dormant thread's timeout (or a retry of its pthread).
extern "C" EMSCRIPTEN_KEEPALIVE void art_dormant_timeout(int id, int gen) {
  using namespace art;
  Rec* r = nullptr;
  {
    std::lock_guard<std::mutex> l(g_mu);
    if (!g_recs) return;
    auto it = g_recs->find(id);
    if (it == g_recs->end()) return;
    r = it->second;
    if (r->gen != static_cast<uint32_t>(gen)) return;
    const int s = r->state.load();
    if (s == kClaimed) {
      // A retry of the new pthread; otherwise the thread is already woken
      // and its pthread on the way (this timeout came late).
      if (!r->respawn.exchange(false)) return;
    } else if (s != kDormant || !Claim(r)) {
      return;
    } else {
      r->timed_out = true;
      r->why = "timeout";
      if (r->kind == kPoll) ForgetPollerLocked(r);
    }
  }
  Spawn(r, r->why);
}

namespace art {

ArtWasmDormant::Rec* ArtWasmDormant::RecOf(Thread* t) {
  return static_cast<Rec*>(t->tlsPtr_.arc_dormant);
}

void ArtWasmDormant::ThreadStart(Thread* self) {
  if (!Enabled()) return;
  Rec* r = RecOf(self);
  if (r == nullptr) {
    r = new Rec();
    r->thread = self;
    std::lock_guard<std::mutex> l(g_mu);
    if (!g_recs) {
      g_recs = new std::unordered_map<int, Rec*>;
      g_pollers = new std::vector<Rec*>;
    }
    r->id = g_next_id++;
    (*g_recs)[r->id] = r;
    self->tlsPtr_.arc_dormant = r;
  }
  JNIEnvExt* env = self->GetJniEnv();
  r->base_cookie = env->GetLocalRefCookie();
  r->base_segment = env->GetLocalsSegmentState();
  r->base_handles = self->tlsPtr_.top_handle_scope;
  // The spares start with the first threads (the runtime's daemons), while
  // the page's thread is surely free to give them workers.
  {
    std::lock_guard<std::mutex> l(g_spare_mu);
    if (g_spare_stack == 0) g_spare_stack = self->tlsPtr_.stack_size;
  }
  EnsureSpares();
}

void ArtWasmDormant::ThreadGone(Thread* t) {
  Rec* r = RecOf(t);
  if (r == nullptr) return;
  t->tlsPtr_.arc_dormant = nullptr;
  std::lock_guard<std::mutex> l(g_mu);
  g_recs->erase(r->id);
  ForgetPollerLocked(r);
  delete r;
}

// Captures the thread's frames, makes them its stack, publishes the record
// (kDormant). The thread is suspended (in its wait state); false, nothing
// changed, if it cannot go dormant now.
bool ArtWasmDormant::Commit(Thread* self, Rec* r, int kind, double deadline, uint32_t* gen,
                            bool* retry) {
  // Nobody walks this stack while the suspend count is 0 and its lock held
  // (a stack walk of a suspended thread raises its count first).
  MutexLock mu(self, *Locks::thread_suspend_count_lock_);
  const char* refused = nullptr;
  *retry = false;
  if (self->GetSuspendCount() != 0) {
    refused = "suspend";  // (a stack walk, the GC: again after another grace period)
    *retry = true;
  }
  else if (self->tlsPtr_.stacked_shadow_frame_record != nullptr ||
           self->tlsPtr_.deoptimization_context_stack != nullptr ||
           self->tlsPtr_.frame_id_to_shadow_frame != nullptr ||
           self->tlsPtr_.exception != nullptr) refused = "state";
  const ManagedStack* ms = &self->tlsPtr_.managed_stack;
  const ManagedStack* base = ms->GetLink();
  // One fragment above the empty base: the invocation of run() by
  // CreateCallback (or of the frames by Resume). A native, reflection, a
  // class initializer or a proxy in between would have pushed another.
  if (!refused && (ms->GetTopQuickFrame() != nullptr || base == nullptr || base->GetLink() != nullptr ||
                   base->GetTopShadowFrame() != nullptr || base->GetTopQuickFrame() != nullptr)) {
    refused = "stack";
  }
  std::vector<ShadowFrame*> copy;
  ShadowFrame* tail = nullptr;
  if (!refused) {
    ShadowFrame* f = ms->GetTopShadowFrame();
    if (f != nullptr && f->GetMethod()->IsNative()) f = f->GetLink();  // the native that waits
    for (; f != nullptr && !refused; f = f->GetLink()) {
      if (f == t_floor) {
        tail = f;
        break;
      }
      ArtMethod* m = f->GetMethod();
      if (m->IsNative() || m->IsProxyMethod() || m->MustCountLocks()) {
        refused = "frame";
        break;
      }
      CodeItemDataAccessor accessor(m->DexInstructionData());
      const uint32_t pc = f->GetDexPC();
      if (!accessor.HasCodeItem() || pc >= accessor.InsnsSizeInCodeUnits()) {
        refused = "pc";
        break;
      }
      const Instruction* inst = &accessor.InstructionAt(pc);
      if (!ResumableInvoke(inst) || interpreter::IsStringInit(inst, m)) {
        refused = "invoke";
        break;
      }
      if (copy.size() >= 1024) {
        refused = "deep";
        break;
      }
      copy.push_back(f);
    }
    if (!refused && copy.empty() && tail == nullptr) refused = "empty";
  }
  if (refused) {
    __atomic_fetch_add(&arc_dormant_counts[3], 1, __ATOMIC_RELAXED);
    if (Log()) {
      std::string n;
      self->GetThreadName(n);
      fprintf(stderr, "art dormant: \"%s\" stays (%s, %s)\n", n.c_str(), KindName(kind), refused);
    }
    return false;
  }
  ShadowFrame* link = tail;
  for (size_t i = copy.size(); i-- > 0;) {
    ShadowFrame* s = copy[i];
    const uint32_t n = s->NumberOfVRegs();
    ShadowFrame* d = ShadowFrame::CreateDeoptimizedFrame(n, link, s->GetMethod(), s->GetDexPC());
    if (n != 0) {
      memcpy(d->GetVRegArgs(0), s->GetVRegArgs(0), n * sizeof(uint32_t));
      memcpy(d->GetShadowRefAddr(0), s->GetShadowRefAddr(0), n * sizeof(uint32_t));
    }
    link = d;
  }
  // The frames are the stack now; what the dropped C++ frames held goes.
  self->tlsPtr_.managed_stack = ManagedStack();
  self->SetTopOfShadowStack(link);
  self->tlsPtr_.top_handle_scope = r->base_handles;
  JNIEnvExt* env = self->GetJniEnv();
  env->SetLocalRefCookie(r->base_cookie);
  env->SetLocalSegmentState(r->base_segment);
  r->frames = link;
  r->kind = kind;
  r->deadline = deadline;
  r->timed_out = false;
  r->stack_size = self->tlsPtr_.stack_size;
  if (arc_lproc_get_thread) arc_lproc_get_thread(&r->lproc_pid, &r->lproc_uid);
  snprintf(r->name, sizeof(r->name), "%s", self->tlsPtr_.name != nullptr ? self->tlsPtr_.name->c_str() : "");
  r->since = MonoMs();
  *gen = ++r->gen;
  __atomic_fetch_add(&arc_dormant_counts[0], 1, __ATOMIC_RELAXED);
  __atomic_fetch_add(&arc_dormant_counts[1], 1, __ATOMIC_RELAXED);
  if (Log()) {
    fprintf(stderr, "art dormant: \"%s\" leaves its worker (%s, %zu frames copied%s%s)\n", r->name,
            KindName(kind), copy.size(), tail ? " + continued ones" : "",
            isinf(deadline) ? "" : ", timed");
  }
  r->state.store(kDormant, std::memory_order_seq_cst);
  return true;
}

void ArtWasmDormant::ExitPthread(Thread* self) {
  UNUSED(self);
  pthread_setspecific(Thread::pthread_key_self_, nullptr);
  if (Spare* s = t_spare) {
    t_spare = nullptr;
    SpareGone(s);
  }
  EnsureSpares();
  pthread_exit(nullptr);
}

bool ArtWasmDormant::ParkWait(Thread* self, bool is_absolute, int64_t time, bool* timed_out) {
  Rec* r = RecOf(self);
  const bool untimed = !is_absolute && time == 0;
  if (r == nullptr || (!untimed && time <= 0) || !KindOn(kPark)) return false;
  const double now = MonoMs();
  double deadline = INFINITY;
  if (is_absolute) {
    timespec wall;
    clock_gettime(CLOCK_REALTIME, &wall);
    deadline = now + (static_cast<double>(time) - (wall.tv_sec * 1000.0 + wall.tv_nsec / 1e6));
  } else if (!untimed) {
    deadline = now + static_cast<double>(time) / 1e6;
  }
  if (deadline - now < GraceMs() + MinRestMs() || !NativeStateFree()) return false;
  const ThreadState st = untimed ? kWaiting : kTimedWaiting;
  volatile void* addr = self->tls32_.park_state_.Address();
  ScopedThreadSuspension sts(self, st);
  for (;;) {
    emscripten_futex_wait(addr, kParkWaiterWaiting, std::min(GraceMs(), deadline - MonoMs()));
    if (self->tls32_.park_state_.load() != kParkWaiterWaiting) {
      *timed_out = false;
      return true;
    }
    if (deadline - MonoMs() < MinRestMs()) break;
    uint32_t gen;
    bool retry;
    if (Commit(self, r, kPark, deadline, &gen, &retry)) {
      // The permit may have come before the record was out (Unparked saw none).
      if (self->tls32_.park_state_.load(std::memory_order_seq_cst) != kParkWaiterWaiting) {
        if (Claim(r)) Spawn(r, "unpark");
      } else if (!untimed) {
        Schedule(r->id, gen, deadline - MonoMs());
      }
      ExitPthread(self);
    }
    if (!retry) break;
  }
  const double rest = deadline - MonoMs();
  if (rest <= 0) {
    *timed_out = true;
    return true;
  }
  *timed_out = emscripten_futex_wait(addr, kParkWaiterWaiting, untimed ? INFINITY : rest) == -ETIMEDOUT;
  return true;
}

void ArtWasmDormant::Unparked(Thread* t) {
  std::atomic_thread_fence(std::memory_order_seq_cst);
  Rec* r = RecOf(t);
  if (r != nullptr && r->kind == kPark && r->state.load(std::memory_order_seq_cst) == kDormant &&
      Claim(r)) {
    Spawn(r, "unpark");
  }
}

bool ArtWasmDormant::MonitorWait(Thread* self, Monitor* monitor, ThreadState why, int64_t ms,
                                 int32_t ns, int prev_lock_count, ArtMethod* saved_method,
                                 uintptr_t saved_dex_pc, bool interrupt_should_throw,
                                 bool* timed_out) {
  Rec* r = RecOf(self);
  if (r == nullptr || !KindOn(kMonitor)) return false;
  const bool untimed = why == kWaiting;
  const double now = MonoMs();
  const double deadline = untimed ? INFINITY : now + ms + ns / 1e6;
  if (deadline - now < GraceMs() + MinRestMs() || !NativeStateFree()) return false;
  ConditionVariable* cv = self->GetWaitConditionVariable();
  r->signalled = false;
  for (;;) {
    const double wait = std::min(GraceMs(), deadline - MonoMs());
    if (!cv->TimedWait(self, static_cast<int64_t>(wait), 0) || r->signalled ||
        self->IsInterrupted()) {
      *timed_out = false;
      return true;
    }
    if (deadline - MonoMs() < MinRestMs()) break;
    r->monitor = monitor;
    r->prev_lock_count = prev_lock_count;
    r->saved_method = saved_method;
    r->saved_dex_pc = saved_dex_pc;
    r->interrupt_should_throw = interrupt_should_throw;
    uint32_t gen;
    bool retry;
    if (Commit(self, r, kMonitor, deadline, &gen, &retry)) {
      if (!untimed) Schedule(r->id, gen, deadline - MonoMs());
      // (Wakers decide holding this mutex: SignalWaiter.)
      self->GetWaitMutex()->ExclusiveUnlock(self);
      ExitPthread(self);
    }
    if (!retry) break;
  }
  const double rest = deadline - MonoMs();
  if (untimed) {
    cv->Wait(self);
    *timed_out = false;
  } else if (rest <= 0) {
    *timed_out = true;
  } else {
    const int64_t rest_ms = static_cast<int64_t>(rest);
    *timed_out = cv->TimedWait(self, rest_ms, static_cast<int32_t>((rest - rest_ms) * 1e6));
  }
  return true;
}

void ArtWasmDormant::SignalWaiter(Thread* self, Thread* waiter) {
  Rec* r = RecOf(waiter);
  if (r != nullptr) {
    r->signalled = true;
    if (r->kind == kMonitor && r->state.load() == kDormant && Claim(r)) {
      Spawn(r, "notify");
      return;
    }
  }
  waiter->GetWaitConditionVariable()->Signal(self);
}

int ArtWasmDormant::EpollDormant(Thread* self, int epfd, int timeout_ms, void (*on_resume)(void*),
                                 void* arg) {
  Rec* r = RecOf(self);
  if (r == nullptr || !KindOn(kPoll) || (timeout_ms >= 0 && timeout_ms < MinRestMs()) ||
      !NativeStateFree()) {
    return 0;
  }
  const double deadline = timeout_ms < 0 ? INFINITY : MonoMs() + timeout_ms;
  r->fd = epfd;
  r->on_resume = on_resume;
  r->resume_arg = arg;
  uint32_t gen;
  for (;;) {
    bool retry;
    {
      std::lock_guard<std::mutex> l(g_mu);
      if (Commit(self, r, kPoll, deadline, &gen, &retry)) {
        g_pollers->push_back(r);
        g_npollers.store(static_cast<int>(g_pollers->size()));
        break;
      }
    }
    // (Level-triggered: looking does not take the events from the Looper.)
    struct epoll_event ev;
    if (!retry || deadline - MonoMs() < GraceMs() + MinRestMs() ||
        ::epoll_wait(epfd, &ev, 1, static_cast<int>(GraceMs())) != 0) {
      return 0;
    }
  }
  // Events before the record was in the list (EpollReady did not see it)?
  // (A level-triggered epoll: looking does not take them.)
  struct epoll_event ev;
  const bool ready = ::epoll_wait(epfd, &ev, 1, 0) != 0;
  bool spawn = false;
  if (ready) {
    std::lock_guard<std::mutex> l(g_mu);
    if (Claim(r)) {
      ForgetPollerLocked(r);
      spawn = true;
    }
  }
  if (spawn) {
    Spawn(r, "fd");
  } else if (timeout_ms >= 0) {
    Schedule(r->id, gen, deadline - MonoMs());
  }
  ExitPthread(self);
}

void ArtWasmDormant::EpollReady(int epfd) {
  std::vector<Rec*> woken;
  {
    std::lock_guard<std::mutex> l(g_mu);
    if (!g_pollers) return;
    for (size_t i = 0; i < g_pollers->size();) {
      Rec* r = (*g_pollers)[i];
      if (r->fd == epfd && r->state.load() == kDormant && Claim(r)) {
        g_pollers->erase(g_pollers->begin() + i);
        woken.push_back(r);
      } else {
        ++i;
      }
    }
    g_npollers.store(static_cast<int>(g_pollers->size()));
  }
  for (Rec* r : woken) Spawn(r, "fd");
}

void ArtWasmDormant::FinishMonitorWait(Thread* self, Rec* r) {
  r->monitor->ArcWasmEndDormantWait(self, r->prev_lock_count, r->saved_method, r->saved_dex_pc,
                                    r->interrupt_should_throw, r->timed_out);
}

void* ArtWasmDormant::Resume(void* arg) {
  Thread* self = static_cast<Thread*>(arg);
  Rec* r = RecOf(self);
  // Take over the thread (as Thread::Init does for a new one).
  self->tlsPtr_.pthread_self = pthread_self();
  self->InitTid();
  CHECK(self->InitStackHwm());
  CHECK_PTHREAD_CALL(pthread_setspecific, (Thread::pthread_key_self_, self), "dormant resume");
  if (arc_lproc_set_thread) arc_lproc_set_thread(r->lproc_pid, r->lproc_uid);
  __atomic_fetch_sub(&arc_dormant_counts[0], 1, __ATOMIC_RELAXED);
  __atomic_fetch_add(&arc_dormant_counts[2], 1, __ATOMIC_RELAXED);
  if (Log()) {
    fprintf(stderr, "art dormant: \"%s\" back after %.0f ms (%s)\n", r->name, MonoMs() - r->since,
            r->why);
  }
  self->TransitionFromSuspendedToRunnable();
  // The rest of the wait, as the native would have finished it.
  switch (r->kind) {
    case kPark:
      self->tls32_.park_state_.store(kParkNoPermit, std::memory_order_relaxed);
      Runtime::Current()->GetRuntimeCallbacks()->ThreadParkFinished(r->timed_out);
      break;
    case kMonitor:
      FinishMonitorWait(self, r);
      break;
    case kPoll:
      if (r->on_resume != nullptr) r->on_resume(r->resume_arg);
      break;
  }
  ShadowFrame* frames = r->frames;
  r->frames = nullptr;
  r->state.store(kAwake, std::memory_order_seq_cst);
  ContinueFrames(self, frames);
  // As at the end of Thread::CreateCallback.
  self->TransitionFromRunnableToSuspended(kNative);
  Runtime::Current()->GetThreadList()->Unregister(self);
  return nullptr;
}

// Runs the heap frames (innermost first) to the end of the bottom one, each
// after the call it made (or at its handler of the pending exception).
void ArtWasmDormant::ContinueFrames(Thread* self, ShadowFrame* frames) {
  Rec* r = RecOf(self);
  // An empty base below one fragment, as ArtMethod::Invoke's for run().
  self->tlsPtr_.managed_stack = ManagedStack();
  ManagedStack fragment;
  self->PushManagedStackFragment(&fragment);
  JNIEnvExt* env = self->GetJniEnv();
  r->base_cookie = env->GetLocalRefCookie();
  r->base_segment = env->GetLocalsSegmentState();
  r->base_handles = self->tlsPtr_.top_handle_scope;
  const instrumentation::Instrumentation* instrumentation = Runtime::Current()->GetInstrumentation();
  JValue value;
  value.SetJ(0);
  bool first = true;
  for (ShadowFrame* sf = frames; sf != nullptr;) {
    self->SetTopOfShadowStack(sf);
    t_floor = sf;
    CodeItemDataAccessor accessor(sf->GetMethod()->DexInstructionData());
    uint32_t pc = sf->GetDexPC();
    if (self->IsExceptionPending()) {
      pc = interpreter::MoveToExceptionHandler(self, *sf, first ? nullptr : instrumentation)
          ? sf->GetDexPC() : dex::kDexNoIndex;
    } else {
      pc += accessor.InstructionAt(pc).SizeInCodeUnits();
    }
    if (pc != dex::kDexNoIndex) {
      sf->SetDexPC(pc);
      value = interpreter::ArtWasmResumeFrame(self, sf, value);
    }
    ShadowFrame* next = sf->GetLink();
    ShadowFrame::DeleteDeoptimizedFrame(sf);
    sf = next;
    first = false;
  }
  t_floor = nullptr;
  self->SetTopOfShadowStack(nullptr);
  self->PopManagedStackFragment(fragment);
}

}  // namespace art
