// Green threads: the threads 32-bit guest code starts (pthread_create) share
// a few host threads, "carriers", instead of having one each (topic
// threads-per-worker, docs/topics/threads-per-worker.md). A host thread is a
// Web Worker with its own JavaScriptCore VM and wasm instance, 4-10 MB of an
// iPhone's tab (ios-threads.md), and a Unity game starts 40-50 guest threads
// that mostly wait: Subway Surfers' 16 "Background Job.Worker", its job
// workers and il2cpp's pool sleep in futex and condition variable waits.
//
// A green thread is a CPU state of its own (ndk_cpu_state_new: registers,
// guest stack) that a carrier runs (ndk_cpu_green_run) until it waits at the
// outermost level of a host call. There it parks instead of waiting: no host
// stack is kept (wasm on iOS 16 cannot switch stacks); the waits below are
// written so that the call runs again from its start when the thread
// resumes, the record it left (ndk_cpu_park / ndk_cpu_resumed) saying what
// it had done. The carrier meanwhile runs the next runnable green thread.
// Wakes are the wakers' job: every wake of a guest futex word goes through
// GuestFutexWake, which also wakes green threads parked on that word;
// timers end timed waits, and the CPU's hook a signal's (signal.cc).
//
// The bridge's per-thread state (thread_locals of the host thread that runs
// guest code) is swapped by the carrier (RegisterGreenSlot), the light
// process is set for each thread. JNI (AttachCurrentThread), EGL and ALooper
// bind a thread to its host thread: such a green thread is pinned to its
// carrier, which then runs nothing else. A wait that cannot park (poll, a
// wait under a nested guest call) keeps the carrier ("blocked"): it does not
// count against the limit of carriers, and another one starts if green
// threads wait. A green thread that runs long while others wait is asked to
// yield (at a block boundary); a tick on the page's thread watches that.
//
// ARC_NDK_GREEN=0 turns this off (a host thread per guest thread, as
// before); ARC_NDK_GREEN_CARRIERS=N sets the limit (default: the browser's
// cores, 2-4).
// Part of the arc-wasm shim layer.

#include <emscripten.h>
#include <emscripten/eventloop.h>
#include <emscripten/threading.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <mutex>
#include <vector>

#include "../multiproc/include/arc_lproc.h"
#include "ndk_bridge.h"

extern "C" int __android_log_print(int prio, const char* tag, const char* fmt, ...);
// The CPU's side (shim/ndk/cpu/ndk_cpu_ext.h); absent in the stub CPU (no
// green threads then).
extern "C" {
NdkCpuState* ndk_cpu_state_new(size_t stack_size, int tid, uint32_t self, void* owner) __attribute__((weak));
void ndk_cpu_state_free(NdkCpuState* s) __attribute__((weak));
void* ndk_cpu_state_owner(const NdkCpuState* s) __attribute__((weak));
void ndk_cpu_green_start(NdkCpuState* s, uint32_t fn, uint32_t arg) __attribute__((weak));
void ndk_cpu_green_enter(NdkCpuState* s) __attribute__((weak));
int ndk_cpu_green_run(NdkCpuState* s) __attribute__((weak));
int ndk_cpu_green_leave(NdkCpuState* s, int parked) __attribute__((weak));
void ndk_cpu_set_green_hook(void (*hook)(NdkCpuState* s)) __attribute__((weak));
void ndk_cpu_green_yield(NdkCpuState* s) __attribute__((weak));
NdkResume* ndk_cpu_parked_record(NdkCpuState* s) __attribute__((weak));
uint32_t ndk_cpu_carrier_translations(void) __attribute__((weak));
int ndk_cpu_can_park(void) __attribute__((weak));
int ndk_cpu_park(const NdkResume* rec) __attribute__((weak));
NdkResume* ndk_cpu_resumed(void) __attribute__((weak));
int ndk_cpu_green_end(void) __attribute__((weak));
}
#pragma weak arc_lproc_get_thread
#pragma weak arc_lproc_set_thread

namespace ndk {

namespace {

const int kLinuxEINTR = 4, kLinuxEAGAIN = 11, kLinuxETIMEDOUT = 110;
const uint32_t kGreenMagic = 0x67726e74;   // "grnt"
// Green thread ids, above the host threads' (emscripten counts those up
// from 1): kernel-style tids for gettid, signals and /proc.
const int kFirstTid = 0x200000;
const double kIdleExitMs = 5000;    // an idle carrier ends after this (one stays)
const double kSliceMs = 20;         // runnable threads wait: one that ran this long yields
const double kMinSliceMs = 2;       // a woken thread takes the carrier of one that ran this long
const double kStuckMs = 200;        // asked to yield this long ago, still not back: stuck
const int kMaxCarriers = 64;        // even with every carrier blocked

double MonoMs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

// --- the bridge's per-thread state ------------------------------------------------------

struct Slot {
  void* (*addr)();
  uint32_t size, offset;
};
std::vector<Slot>& Slots() {
  static std::vector<Slot>* slots = new std::vector<Slot>;
  return *slots;
}
uint32_t SlotBytes() {
  const std::vector<Slot>& s = Slots();
  return s.empty() ? 0 : s.back().offset + s.back().size;
}

struct Carrier;

enum Sched { kSchedNew, kQueued, kRunning, kSchedParked, kDone };

struct Green {
  uint32_t magic;
  NdkCpuState* cpu;
  int tid;
  uint32_t fn, arg, stack_size;
  int lpid, luid;           // light process (multiproc)
  uint8_t* slots;           // its share of the bridge's thread_locals (Slots)
  bool slots_set;           // (a new thread starts with a carrier's defaults)
  // Scheduling (under g_lock).
  int sched;
  bool requeue;             // woken while its carrier still had it
  bool started;
  bool exiting;             // ending: its destructors run, nothing parks
  Carrier* pinned;          // runs only there
  uint32_t pins;            // why (GreenPinReason)
  const Carrier* last;      // where it ran last (its translations are there; never dereferenced)
  Green* qnext;
  double queued_at;
  // A wait: futex word (0: none; in a bucket while |waiting|) and deadline;
  // what ended it (the result its record gets when it runs again).
  std::atomic<int> waiting;
  int wake_result;
  bool parked_call;         // it last parked in a call (it has a record)
  uint32_t wait_addr;
  double deadline;
  Green* bnext;
  // pthread_join / pthread_detach.
  std::atomic<uint32_t> exit_word;   // 1 once it ended
  std::atomic<uint32_t> ends;        // kDetached | kExited
  uint32_t retval;
};
const uint32_t kDetached = 1, kExited = 2;

struct Carrier {
  Green* running;
  Green* pinned;            // its only thread
  bool counted;             // in g_active (unpinned, running, not blocked)
  bool blocked;             // its thread waits where it cannot park
  bool called;              // taken off the idle list for a thread
  bool retire;              // its pinned thread ended: it ends too (ART, GL of that thread)
  double since;             // when it took |running|
  double yield_asked;       // when the tick asked |running| to yield (0: not)
  bool from_yielded;        // |running| came from the queue of yielded ones
  uint32_t translations;    // ARM translations in its table (after its last slice)
  std::atomic<uint32_t> word;   // it sleeps on it
  uint8_t* defaults;        // its thread_locals while it runs no green thread
};

std::mutex g_lock;
// Runnable threads: woken ones (they mostly run a little and wait again)
// before those that yielded a carrier after running a slice; a thread that
// never waits so goes behind every woken one (else each wake of the others
// waited for a spinning one's slice). The tick moves yielded threads that
// waited long to the front queue.
struct Queue {
  Green* head = nullptr;
  Green* tail = nullptr;
};
Queue g_woken, g_yielded;
Green* QueueHead() { return g_woken.head ? g_woken.head : g_yielded.head; }
std::vector<Green*>* g_greens;      // live ones
std::vector<Carrier*>* g_carriers;
// Unpinned carriers waiting for work, not called yet. One of them keeps the
// time: it wakes for the next deadline (g_keeper_until), the others only to
// end when idle long.
std::vector<Carrier*>* g_idle;
Carrier* g_keeper = nullptr;
double g_keeper_until = INFINITY;
int g_active = 0;        // unpinned carriers running a thread, not blocked
int g_starting = 0;      // carriers started, not yet in their loop
int g_cap = 4;
int g_started_total = 0;  // carriers started so far (for measurements)
std::atomic<int> g_next_tid{kFirstTid};
thread_local Carrier* t_carrier;
thread_local Green* t_green;       // the thread this carrier runs now

// --- futex words green threads wait on -------------------------------------------------

const int kBuckets = 256;
struct Bucket {
  std::mutex mu;
  Green* head;
};
Bucket g_buckets[kBuckets];
std::atomic<int> g_word_waiters{0};   // green threads in buckets
Bucket& BucketOf(uint32_t addr) { return g_buckets[(addr >> 2) * 2654435761u >> 24 & (kBuckets - 1)]; }

void BucketRemoveLocked(Bucket& b, Green* g) {
  for (Green** p = &b.head; *p; p = &(*p)->bnext) {
    if (*p == g) {
      *p = g->bnext;
      g->bnext = nullptr;
      g_word_waiters.fetch_sub(1);
      return;
    }
  }
}

// --- the run queue ------------------------------------------------------------------------

void StartCarrierLocked();

// No carrier for a runnable thread: the one that has run longest yields at
// its next block boundary, if it ran a little (as a kernel lets a woken
// thread in; else a thread that never waits keeps its carrier until the
// tick). One request at a time per carrier.
void PreemptLocked(double now) {
  Carrier* longest = nullptr;
  for (Carrier* c : *g_carriers) {
    if (!c->running || c->pinned || c->blocked || c->yield_asked || c->running->exiting) continue;
    // One that yielded before (it runs on and on) at once; others after a little.
    if (!c->from_yielded && now - c->since < kMinSliceMs) continue;
    if (!longest || (c->from_yielded && !longest->from_yielded) ||
        (c->from_yielded == longest->from_yielded && c->since < longest->since))
      longest = c;
  }
  if (longest) {
    longest->yield_asked = now;
    ndk_cpu_green_yield(longest->running->cpu);
  }
}

void Poke(Carrier* c) {
  c->word.fetch_add(1);
  emscripten_futex_wake(&c->word, 1);
}

bool IdleLocked(Carrier* c) {
  for (Carrier* o : *g_idle) if (o == c) return true;
  return false;
}

// |c| stops waiting for work; another idle one keeps the time if it did.
void LeaveIdleLocked(Carrier* c) {
  for (size_t i = 0; i < g_idle->size(); ++i)
    if ((*g_idle)[i] == c) { g_idle->erase(g_idle->begin() + i); break; }
  if (g_keeper == c) {
    g_keeper = g_idle->empty() ? nullptr : g_idle->front();
    g_keeper_until = INFINITY;
    if (g_keeper) Poke(g_keeper);
  }
}

void Push(Queue& q, Green* g) {
  g->qnext = nullptr;
  if (q.tail) q.tail->qnext = g; else q.head = g;
  q.tail = g;
}
Green* Pop(Queue& q) {
  Green* g = q.head;
  if (g) {
    q.head = g->qnext;
    if (!q.head) q.tail = nullptr;
    g->qnext = nullptr;
  }
  return g;
}

// |yielded|: it ran a slice and gave its carrier back (not woken).
void EnqueueLocked(Green* g, bool yielded = false) {
  g->sched = kQueued;
  g->queued_at = MonoMs();
  if (g->pinned) {
    Poke(g->pinned);
    return;
  }
  Push(yielded ? g_yielded : g_woken, g);
  // At most g_cap carriers run threads (idle ones beyond that end after a
  // while): a called one counts at once. The one it ran on before if that
  // is idle (its translations of the thread's code are there).
  if (g_active + g_starting < g_cap) {
    if (!g_idle->empty()) {
      Carrier* c = g_idle->back();
      for (Carrier* o : *g_idle)
        if (o == g->last) c = o;
      LeaveIdleLocked(c);
      c->called = true;
      c->counted = true;
      g_active++;
      Poke(c);
    } else {
      StartCarrierLocked();
    }
  } else if (!yielded) {
    PreemptLocked(MonoMs());
  }
}

// Room for one more running carrier (one blocked or pinned meanwhile):
// a waiting thread gets an idle carrier or a new one.
void KickLocked() {
  if (!QueueHead() || g_active + g_starting >= g_cap) return;
  if (!g_idle->empty()) {
    Carrier* c = g_idle->back();
    LeaveIdleLocked(c);
    c->called = true;
    c->counted = true;
    g_active++;
    Poke(c);
  } else {
    StartCarrierLocked();
  }
}

// The first of |q| that last ran on |c| among the first few, else the head.
Green* PopFor(Queue& q, const Carrier* c) {
  Green* prev = nullptr;
  int n = 0;
  for (Green* g = q.head; g && n < 16; prev = g, g = g->qnext, ++n) {
    if (g->last != c) continue;
    if (n == 0) return Pop(q);
    prev->qnext = g->qnext;
    if (q.tail == g) q.tail = prev;
    g->qnext = nullptr;
    return g;
  }
  return Pop(q);
}

Green* DequeueLocked(const Carrier* c, bool* yielded) {
  Green* g = PopFor(g_woken, c);
  *yielded = !g;
  return g ? g : PopFor(g_yielded, c);
}

// A parked thread can run again (its wait ended); its carrier may still be
// finishing the park.
void MakeRunnableLocked(Green* g) {
  if (g->sched == kRunning) g->requeue = true;
  else if (g->sched == kSchedParked) EnqueueLocked(g);
}

// Ends |g|'s wait with |result| (for its record), if it still waits: wakes,
// timers and signals race, one wins. g_lock held.
bool WakeLocked(Green* g, int result) {
  int expected = 1;
  if (!g->waiting.compare_exchange_strong(expected, 0)) return false;
  g->wake_result = result;
  if (g->wait_addr) {
    Bucket& b = BucketOf(g->wait_addr);
    std::lock_guard<std::mutex> l(b.mu);
    BucketRemoveLocked(b, g);
  }
  MakeRunnableLocked(g);
  return true;
}

// The CPU's hook: a deliverable signal for a parked thread (CPU list lock held).
void SignalHook(NdkCpuState* s) {
  Green* g = static_cast<Green*>(ndk_cpu_state_owner(s));
  std::lock_guard<std::mutex> l(g_lock);
  WakeLocked(g, -kLinuxEINTR);
}

// --- carriers --------------------------------------------------------------------------

void SwitchIn(Carrier* c, Green* g) {
  t_green = g;
  for (const Slot& s : Slots()) {
    void* p = s.addr();
    memcpy(c->defaults + s.offset, p, s.size);
    if (g->slots_set) memcpy(p, g->slots + s.offset, s.size);
  }
  g->slots_set = true;
  if (arc_lproc_set_thread) arc_lproc_set_thread(g->lpid, g->luid);
}

void SwitchOut(Carrier* c, Green* g) {
  for (const Slot& s : Slots()) {
    void* p = s.addr();
    memcpy(g->slots + s.offset, p, s.size);
    memcpy(p, c->defaults + s.offset, s.size);
  }
  if (arc_lproc_set_thread) arc_lproc_set_thread(0, 0);
  t_green = nullptr;
}

void FreeGreen(Green* g) {
  g->magic = 0;
  free(g->slots);
  delete g;
}

// The end of |g| on carrier |c| (its function returned or it called
// pthread_exit): its thread-specific data destructors run as it (no wait
// parks now), then everything of it goes.
void Finish(Carrier* c, Green* g, uint32_t retval) {
  g->exiting = true;
  g->retval = retval;
  GreenThreadExit();   // ndk_pthread.cc: TSD destructors, its entry; ndk_core.cc: its TLS
  ndk_cpu_green_leave(g->cpu, 0);
  SwitchOut(c, g);
  ndk_cpu_state_free(g->cpu);
  g->cpu = nullptr;
  {
    std::lock_guard<std::mutex> l(g_lock);
    g->sched = kDone;
    for (size_t i = 0; i < g_greens->size(); ++i)
      if ((*g_greens)[i] == g) { (*g_greens)[i] = g_greens->back(); g_greens->pop_back(); break; }
    if (g->pinned) {
      c->pinned = nullptr;
      g->pinned = nullptr;
      c->retire = true;
    }
  }
  g->exit_word.store(1);
  GuestFutexWake(&g->exit_word, INT32_MAX);
  if (g->ends.fetch_or(kExited) & kDetached) FreeGreen(g);
}

// Runs |g| until it parks, yields or ends.
void RunSlice(Carrier* c, Green* g) {
  SwitchIn(c, g);
  ndk_cpu_green_enter(g->cpu);
  if (!g->started) {
    g->started = true;
    CurrentState();       // bionic's TLS for it (ndk_core.cc)
    GreenThreadStart(g->stack_size);   // ndk_pthread.cc: its entry (stack, tid)
  }
  if (g->parked_call) {   // what ended its wait, for the call that runs again
    g->parked_call = false;
    if (NdkResume* rec = ndk_cpu_parked_record(g->cpu)) rec->result = g->wake_result;
  }
  g->wake_result = 0;
  int why = ndk_cpu_green_run(g->cpu);
  if (why == kNdkParkNone || why == kNdkParkExit) {
    Finish(c, g, why == kNdkParkNone ? g->cpu->r[0] : g->retval);
    return;
  }
  g->parked_call = why == kNdkParkCall;
  const bool sig = ndk_cpu_green_leave(g->cpu, why == kNdkParkCall);
  SwitchOut(c, g);
  if (ndk_cpu_carrier_translations) c->translations = ndk_cpu_carrier_translations();
  std::lock_guard<std::mutex> l(g_lock);
  if (sig) WakeLocked(g, -kLinuxEINTR);   // a signal came while it parked
  if (why == kNdkParkYield || g->requeue) {
    g->requeue = false;
    EnqueueLocked(g, why == kNdkParkYield);
  } else {
    g->sched = kSchedParked;
    // A deadline before the time keeper's: it looks again.
    if (g->waiting.load() && g->deadline < g_keeper_until && g_keeper) Poke(g_keeper);
  }
}

// Ends the waits whose time is up; the next deadline (INFINITY: none).
double FireTimersLocked(double now) {
  double next = INFINITY;
  for (Green* g : *g_greens) {
    if (g->sched == kDone || isinf(g->deadline) || g->waiting.load() == 0) continue;
    if (g->deadline <= now) {
      WakeLocked(g, -kLinuxETIMEDOUT);
    } else if (g->deadline < next) {
      next = g->deadline;
    }
  }
  return next;
}

void* CarrierMain(void* p) {
  Carrier* c = static_cast<Carrier*>(p);
  t_carrier = c;
  if (arc_lproc_set_thread) arc_lproc_set_thread(0, 0);
  std::unique_lock<std::mutex> l(g_lock);
  g_starting--;
  double idle_since = MonoMs();
  for (;;) {
    double now = MonoMs();
    double next = FireTimersLocked(now);
    Green* g = nullptr;
    bool yielded = false;
    if (c->pinned) {
      if (c->pinned->sched == kQueued) g = c->pinned;
    } else {
      g = DequeueLocked(c, &yielded);
    }
    if (g) {
      LeaveIdleLocked(c);
      c->called = false;
      g->sched = kRunning;
      g->last = c;
      c->running = g;
      c->since = now;
      c->yield_asked = 0;
      c->from_yielded = yielded;
      if (!c->pinned && !c->counted) {
        c->counted = true;
        g_active++;
      }
      l.unlock();
      RunSlice(c, g);
      l.lock();
      if (c->counted) g_active--;
      c->counted = false;
      c->running = nullptr;
      c->blocked = false;
      idle_since = MonoMs();
      if (c->retire) break;
      continue;
    }
    // Nothing to run (called, but another carrier took it): not counted.
    if (c->counted) {
      c->counted = false;
      g_active--;
    }
    // Nothing to run: wait for work, the next deadline, or the idle limit
    // (the last unpinned carrier stays while there are green threads).
    if (!c->pinned && now - idle_since >= kIdleExitMs) {
      int unpinned = 0;
      for (Carrier* o : *g_carriers) if (!o->pinned) unpinned++;
      if (unpinned > 1 || g_greens->empty()) {
        LeaveIdleLocked(c);
        break;
      }
    }
    double until = now + kIdleExitMs;
    if (!c->pinned) {
      if (!IdleLocked(c)) g_idle->push_back(c);
      if (!g_keeper) g_keeper = c;
    }
    const bool keeper = g_keeper == c;
    if ((keeper || c->pinned) && next < until) until = next;
    if (keeper) g_keeper_until = until;
    const uint32_t seen = c->word.load();
    l.unlock();
    emscripten_futex_wait(&c->word, seen, until > now ? until - now : 0);
    l.lock();
    c->called = false;   // (EnqueueLocked took it off the idle list then)
  }
  for (size_t i = 0; i < g_carriers->size(); ++i)
    if ((*g_carriers)[i] == c) { (*g_carriers)[i] = g_carriers->back(); g_carriers->pop_back(); break; }
  l.unlock();
  free(c->defaults);
  delete c;
  return nullptr;
}

void StartCarrierLocked() {
  if ((int)g_carriers->size() >= kMaxCarriers) return;
  Carrier* c = new Carrier();
  c->defaults = static_cast<uint8_t*>(calloc(1, SlotBytes() + 1));
  pthread_attr_t a;
  pthread_attr_init(&a);
  // Host frames of guest calls (thunks, JNI) need more than the guest's
  // stacks suggest; the guest's own stacks belong to the CPU.
  pthread_attr_setstacksize(&a, 1024 * 1024);
  pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
  pthread_t t;
  // (A light process's pthread_create wrapper would make it that
  // process's: the carrier sets each thread's process itself.)
  int pid = 0, uid = 0;
  if (arc_lproc_get_thread) arc_lproc_get_thread(&pid, &uid);
  if (arc_lproc_set_thread) arc_lproc_set_thread(0, 0);
  if (pthread_create(&t, &a, CarrierMain, c) == 0) {
    g_carriers->push_back(c);
    g_starting++;
    g_started_total++;
  } else {
    free(c->defaults);
    delete c;
    __android_log_print(6, "ndk", "green threads: no carrier thread could start");
  }
  if (arc_lproc_set_thread) arc_lproc_set_thread(pid, uid);
  pthread_attr_destroy(&a);
}

// On the page's thread every 10 ms: deadlines when no carrier is idle to
// keep the time; and when runnable threads wait for a
// carrier: those that ran a slice yield (a thread that never waits, a spin,
// keeps its carrier otherwise). A carrier whose thread was asked and is not
// back after a while waits in the host where the bridge cannot see it (a
// blocking read): one more carrier then starts, beyond the limit.
void Tick(void*) {
  std::unique_lock<std::mutex> l(g_lock, std::try_to_lock);
  if (!l.owns_lock()) return;
  const double now = MonoMs();
  // Deadlines when no carrier is idle to keep the time.
  if (g_idle->empty()) FireTimersLocked(now);
  // Yielded threads that waited long go in front (a spinning one gets its
  // turn while others keep waking).
  while (g_yielded.head && now - g_yielded.head->queued_at >= kStuckMs) Push(g_woken, Pop(g_yielded));
  Green* head = QueueHead();
  if (!head || now - head->queued_at < kSliceMs) return;
  for (Carrier* c : *g_carriers) {
    if (!c->running || c->pinned || c->blocked || c->running->exiting) continue;
    if (!c->yield_asked) {
      if (now - c->since >= kSliceMs) {
        c->yield_asked = now;
        ndk_cpu_green_yield(c->running->cpu);
      }
    } else if (now - c->yield_asked > kStuckMs) {
      // Taken for blocked (until its thread comes back): not counted.
      c->blocked = true;
      if (c->counted) {
        c->counted = false;
        g_active--;
      }
    }
  }
  KickLocked();
}

extern "C" void ndk_cpu_dump_threads(void) __attribute__((weak));
// ARC_NDK_GREEN_DUMP=SECS: every guest thread's place every SECS seconds
// (from a host thread of its own; for hangs under Node).
void* DumpMain(void* arg) {
  const unsigned secs = static_cast<unsigned>(reinterpret_cast<uintptr_t>(arg));
  for (;;) {
    sleep(secs);
    if (ndk_cpu_dump_threads) ndk_cpu_dump_threads();
  }
  return nullptr;
}

// The page's thread calls ndk_green_tick (below) every 10 ms; the timer
// neither keeps Node's process alive nor outlives the runtime.
EM_JS(void, ndk_green_start_tick, (), {
  var id = setInterval(function() {
    if (ABORT || (typeof runtimeExited != 'undefined' && runtimeExited)) {
      clearInterval(id);
      return;
    }
    _ndk_green_tick();
  }, 10);
  if (id && id.unref) id.unref();
});
void StartTick() { ndk_green_start_tick(); }

bool Enabled() {
  static const bool on = [] {
    if (const char* d = getenv("ARC_NDK_GREEN_DUMP")) {
      pthread_t t;
      if (atoi(d) > 0 && pthread_create(&t, nullptr, DumpMain, reinterpret_cast<void*>((uintptr_t)atoi(d))) == 0)
        pthread_detach(t);
    }
    if (!ndk_cpu_state_new || !ndk_cpu_park) return false;
    const char* e = getenv("ARC_NDK_GREEN");
    if (e && *e == '0') return false;
    int cores = emscripten_num_logical_cores();
    g_cap = cores < 2 ? 2 : cores > 4 ? 4 : cores;
    if (const char* n = getenv("ARC_NDK_GREEN_CARRIERS")) {
      int v = atoi(n);
      if (v > 0) g_cap = v;
    }
    g_greens = new std::vector<Green*>;
    g_carriers = new std::vector<Carrier*>;
    g_idle = new std::vector<Carrier*>;
    ndk_cpu_set_green_hook(SignalHook);
    emscripten_async_run_in_main_runtime_thread(EM_FUNC_SIG_V, reinterpret_cast<void*>(StartTick));
    if (ThreadsLog())
      fprintf(stderr, "ndk: green threads on, %d carriers at most\n", g_cap);
    return true;
  }();
  return on;
}

Green* LiveGreen(uint32_t t) {
  if (t < 0x10000 || (t & 3) || !g_greens) return nullptr;
  std::lock_guard<std::mutex> l(g_lock);
  for (Green* g : *g_greens)
    if (Addr(g) == t) return g;
  return nullptr;
}

}  // namespace

// --- the bridge's interface (ndk_bridge.h) --------------------------------------------

void RegisterGreenSlot(void* (*addr)(), uint32_t size) {
  std::vector<Slot>& s = Slots();
  uint32_t offset = (SlotBytes() + 7) & ~7u;
  s.push_back({addr, size, offset});
}

bool GreenEnabled() { return Enabled(); }
bool IsGreenThread() { return t_green != nullptr; }

int GreenCreate(uint32_t* thread, uint32_t fn, uint32_t arg, uint32_t stack_size, bool detached) {
  if (!Enabled()) return -1;
  Green* g = new Green();
  g->magic = kGreenMagic;
  g->tid = g_next_tid.fetch_add(1);
  g->fn = fn;
  g->arg = arg;
  g->stack_size = stack_size;
  if (arc_lproc_get_thread) arc_lproc_get_thread(&g->lpid, &g->luid);
  g->slots = static_cast<uint8_t*>(calloc(1, SlotBytes() + 1));
  g->deadline = INFINITY;
  if (detached) g->ends.store(kDetached);
  g->cpu = ndk_cpu_state_new(stack_size, g->tid, Addr(g), g);
  ndk_cpu_green_start(g->cpu, fn, arg);
  *thread = Addr(g);
  std::lock_guard<std::mutex> l(g_lock);
  g_greens->push_back(g);
  g->sched = kSchedParked;
  EnqueueLocked(g);
  return 0;
}

// A live green thread, or one that ended and is not joined or detached yet.
bool IsGreen(uint32_t t) {
  if (LiveGreen(t)) return true;
  if (t < 0x10000 || (t & 3)) return false;
  const Green* d = reinterpret_cast<const Green*>(static_cast<uintptr_t>(t));
  return d->magic == kGreenMagic && d->exit_word.load() == 1;
}

int GreenJoin(uint32_t t, uint32_t* ret) {
  Green* g = LiveGreen(t);
  if (!g) {
    // Ended already? Its record stays until joined.
    if (t >= 0x10000 && !(t & 3)) {
      Green* d = reinterpret_cast<Green*>(static_cast<uintptr_t>(t));
      if (d->magic == kGreenMagic && d->exit_word.load() == 1) g = d;
    }
    if (!g) return ESRCH;
  }
  if (g->ends.load() & kDetached) return EINVAL;
  if (g == t_green) return EDEADLK;
  while (g->exit_word.load() == 0) {
    int e = GuestFutexWaitAt(kSiteJoin, &g->exit_word, 0, INFINITY);
    if (e == kParked) return kParked;
  }
  if (ret) *ret = g->retval;
  FreeGreen(g);
  return 0;
}

int GreenDetach(uint32_t t) {
  Green* g = LiveGreen(t);
  if (!g) {
    if (t >= 0x10000 && !(t & 3)) {
      Green* d = reinterpret_cast<Green*>(static_cast<uintptr_t>(t));
      if (d->magic == kGreenMagic && d->exit_word.load() == 1) {
        if (!(d->ends.fetch_or(kDetached) & kDetached)) FreeGreen(d);
        return 0;
      }
    }
    return ESRCH;
  }
  if (g->ends.fetch_or(kDetached) & kExited) FreeGreen(g);
  return 0;
}

bool GreenExit(uint32_t ret) {
  if (!t_green || t_green->exiting || ndk_cpu_green_end() != 0) return false;
  t_green->retval = ret;
  return true;
}

bool GreenCanPark() {
  return t_green && !t_green->exiting && ndk_cpu_can_park();
}

NdkResume* GreenResumed(int site) {
  if (!t_green) return nullptr;
  NdkResume* r = ndk_cpu_resumed();
  if (!r || r->site != site) return nullptr;
  static thread_local NdkResume taken;
  taken = *r;
  r->site = 0;   // once
  return &taken;
}

namespace {
void ParkRecord(int site, const uint32_t* u) {
  NdkResume rec = {site, 0, {0, 0, 0, 0}};
  if (u) memcpy(rec.u, u, sizeof rec.u);
  ndk_cpu_park(&rec);
}
}  // namespace

int GreenParkFutex(void* addr, uint32_t val, double deadline, int site, const uint32_t* u) {
  Green* g = t_green;
  Bucket& b = BucketOf(Addr(addr));
  // Counted before the word is read: a waker changes the word, then reads
  // the count (GuestFutexWake); one of the two sees the other's write.
  g_word_waiters.fetch_add(1);
  {
    std::lock_guard<std::mutex> l(b.mu);
    if (__atomic_load_n(static_cast<uint32_t*>(addr), __ATOMIC_SEQ_CST) != val) {
      g_word_waiters.fetch_sub(1);
      return -kLinuxEAGAIN;
    }
    g->wait_addr = Addr(addr);
    g->deadline = deadline;
    g->waiting.store(1);
    g->bnext = b.head;
    b.head = g;
  }
  ParkRecord(site, u);
  return kParked;
}

int GreenParkTimed(double deadline, int site, const uint32_t* u) {
  Green* g = t_green;
  g->wait_addr = 0;
  g->deadline = deadline;
  g->waiting.store(1);
  ParkRecord(site, u);
  return kParked;
}

int GreenParkYield() {
  Green* g = t_green;
  g->wait_addr = 0;
  g->deadline = INFINITY;
  g->waiting.store(0);
  ParkRecord(kSiteYield, nullptr);
  std::lock_guard<std::mutex> l(g_lock);
  g->requeue = true;   // runnable again at once, behind the others
  return kParked;
}

int GuestFutexWake(void* addr, int n) {
  int woken = emscripten_futex_wake(addr, n);
  if (woken < 0) woken = 0;
  if (woken >= n) return woken;
  std::atomic_thread_fence(std::memory_order_seq_cst);
  if (g_word_waiters.load() == 0) return woken;
  const uint32_t a = Addr(addr);
  Green* found[16];
  int nfound = 0;
  Bucket& b = BucketOf(a);
  {
    std::lock_guard<std::mutex> l(b.mu);
    for (Green* g = b.head; g && woken + nfound < n && nfound < 16; g = g->bnext) {
      int expected = 1;
      if (g->wait_addr == a && g->waiting.compare_exchange_strong(expected, 0)) found[nfound++] = g;
    }
    for (int i = 0; i < nfound; ++i) BucketRemoveLocked(b, found[i]);
  }
  if (nfound) {
    std::lock_guard<std::mutex> l(g_lock);
    for (int i = 0; i < nfound; ++i) {
      found[i]->wake_result = 0;
      MakeRunnableLocked(found[i]);
    }
  }
  // More than 16 to wake (a broadcast): the rest in another round.
  if (nfound == 16 && woken + nfound < n) return woken + nfound + GuestFutexWake(addr, n - woken - nfound);
  return woken + nfound;
}

void GreenBlocking(bool begin) {
  Carrier* c = t_carrier;
  if (!c || !t_green || c->pinned) return;
  std::lock_guard<std::mutex> l(g_lock);
  if (begin == c->blocked) return;
  c->blocked = begin;
  // (Not another carrier at once: most such waits are short, a new carrier
  // is a new worker. The tick calls one if runnable threads wait a slice.)
  if (begin) {
    if (c->counted) g_active--;
    c->counted = false;
  } else {
    c->counted = true;
    g_active++;
  }
}

void GreenPin(int reason, const char* why) {
  Green* g = t_green;
  Carrier* c = t_carrier;
  if (!g) return;
  g->pins |= reason;
  if (g->pinned) return;
  std::lock_guard<std::mutex> l(g_lock);
  g->pinned = c;
  c->pinned = g;
  if (c->counted) g_active--;
  c->counted = false;
  c->blocked = false;
  KickLocked();
  if (ThreadsLog()) fprintf(stderr, "ndk: green thread %d stays on its carrier (%s)\n", g->tid, why);
}

void GreenUnpin(int reason) {
  Green* g = t_green;
  Carrier* c = t_carrier;
  if (!g || !(g->pins & reason)) return;
  g->pins &= ~reason;
  if (g->pins || !g->pinned) return;
  std::lock_guard<std::mutex> l(g_lock);
  g->pinned = nullptr;
  c->pinned = nullptr;
  c->counted = true;   // (it runs g now, as any carrier)
  g_active++;
  if (ThreadsLog()) fprintf(stderr, "ndk: green thread %d may change carriers again\n", g->tid);
}

double GreenNowMs() { return MonoMs(); }

}  // namespace ndk

extern "C" EMSCRIPTEN_KEEPALIVE void ndk_green_tick(void) { ndk::Tick(nullptr); }

// For measurements (the page): green threads | carriers << 8 | idle carriers
// << 16 | pinned << 24, and the carriers started so far.
extern "C" EMSCRIPTEN_KEEPALIVE uint32_t ndk_green_counts(void) {
  using namespace ndk;
  if (!g_greens) return 0;
  std::unique_lock<std::mutex> l(g_lock, std::try_to_lock);
  if (!l.owns_lock()) return 0;
  uint32_t pinned = 0;
  for (Green* g : *g_greens) if (g->pinned) pinned++;
  auto b = [](size_t v) { return static_cast<uint32_t>(v > 255 ? 255 : v); };
  return b(g_greens->size()) | b(g_carriers->size()) << 8 | b(g_idle->size()) << 16 | b(pinned) << 24;
}
extern "C" EMSCRIPTEN_KEEPALIVE int ndk_green_started(void) { return ndk::g_started_total; }
// ARM translations instantiated on all carriers (each carrier its own table).
extern "C" EMSCRIPTEN_KEEPALIVE int ndk_green_translations(void) {
  using namespace ndk;
  if (!g_carriers) return 0;
  std::unique_lock<std::mutex> l(g_lock, std::try_to_lock);
  if (!l.owns_lock()) return -1;
  int n = 0;
  for (Carrier* c : *g_carriers) n += c->translations;
  return n;
}

// For dumps (the page: Module._ndk_green_dump()).
extern "C" EMSCRIPTEN_KEEPALIVE void ndk_green_dump(void) {
  using namespace ndk;
  if (!g_greens) return;
  std::lock_guard<std::mutex> l(g_lock);
  int parked = 0, queued = 0, running = 0, pinned = 0;
  for (Green* g : *g_greens) {
    if (g->sched == kSchedParked) parked++;
    else if (g->sched == kQueued) queued++;
    else if (g->sched == kRunning) running++;
    if (g->pinned) pinned++;
  }
  int blocked = 0;
  for (Carrier* c : *g_carriers) if (c->blocked) blocked++;
  if (getenv("ARC_NDK_GREEN_DUMP")) {
    for (Green* g : *g_greens) {
      NdkResume* rec = g->cpu ? ndk_cpu_parked_record(g->cpu) : nullptr;
      fprintf(stderr, "  green %d: sched %d waiting %d word %08x (=%08x) deadline %.0f record %d/%d%s%s\n", g->tid,
              g->sched, g->waiting.load(), g->wait_addr,
              g->wait_addr ? *reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(g->wait_addr)) : 0,
              isinf(g->deadline) ? -1.0 : g->deadline - MonoMs(), rec ? rec->site : -1, rec ? rec->result : 0,
              g->requeue ? " requeue" : "", g->pinned ? " pinned" : "");
    }
  }
  fprintf(stderr, "ndk: green threads %zu (running %d, runnable %d, parked %d, pinned %d); carriers %zu "
          "(idle %zu, active %d, blocked %d, starting %d, limit %d)\n", g_greens->size(), running, queued,
          parked, pinned, g_carriers->size(), g_idle->size(), g_active, blocked, g_starting, g_cap);
}
