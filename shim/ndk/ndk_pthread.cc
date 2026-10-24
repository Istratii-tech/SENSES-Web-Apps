// bionic pthreads for guest code. bionic's mutex, condition variable and
// once control are one 32-bit word each (rwlock: 40 bytes, its first word
// a mutex), so the guest word holds a pointer to a host object of ours (a
// futex-based mutex, condition variable or rwlock whose waits guest signals
// interrupt, as the kernel's: shim/ndk/cpu/signal.cc), created on first use (tagged with bit 0; untagged values are
// bionic's static initializers, which encode the mutex type). A host object
// starts with a magic word, checked before use: a guest word that does not
// lead to a live one (the mutex of an object freed meanwhile, now other
// data; a struct copied after its mutex was destroyed) is taken as a new
// mutex, as bionic would take that word, instead of writing a host mutex
// wherever the stale value points. Semaphores
// and once controls work on the guest word itself with futexes. Thread
// specific data is kept per thread here, so key destructors (guest code)
// can run at thread exit. Error codes are translated to Linux numbers.
// Part of the arc-wasm shim layer.

#include <emscripten/heap.h>
#include <emscripten/threading.h>
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <unistd.h>

#include <atomic>
#include <map>
#include <mutex>
#include <utility>

#include "../multiproc/include/arc_lproc.h"
#include "ndk_bridge.h"

extern "C" int __android_log_print(int prio, const char* tag, const char* fmt, ...);
// Optional CPU extensions (shim/ndk/cpu/ndk_cpu_ext.h); absent in the stub CPU.
extern "C" void ndk_set_thread_guest_stack_size(size_t bytes) __attribute__((weak));
extern "C" void ndk_cpu_thread_exit(void) __attribute__((weak));
extern "C" void ndk_guest_stack(uint32_t* base, uint32_t* size) __attribute__((weak));
// The AArch64 CPU (ndk_cpu64.h): optional.
#pragma weak ndk64_set_thread_guest_stack_size
#pragma weak ndk64_sig_thread_created
#pragma weak ndk64_guest_stack
extern "C" int ndk_sig_wait(uint32_t addr, uint32_t val, double ms) __attribute__((weak));
extern "C" int ndk_sig_sleep(double ms, double* left_ms) __attribute__((weak));
extern "C" int ndk_sig_send_pthread(uint32_t thread, int sig) __attribute__((weak));
extern "C" int ndk_sig_mask(int how, const uint64_t* set, uint64_t* old) __attribute__((weak));
extern "C" int ndk_sig_deliver(void) __attribute__((weak));
extern "C" int ndk_sig_check(void) __attribute__((weak));
extern "C" void ndk_sig_self(uint32_t* thread, int* tid) __attribute__((weak));

namespace ndk {

// Linux errno values of GuestFutexWait.
const int kLinuxEINTR = 4, kLinuxEAGAIN = 11, kLinuxETIMEDOUT = 110;

// A wait of a green thread that does not park (a nested guest call, or a
// caller that cannot run again from its start) keeps its carrier.
struct Blocking {
  const bool on;
  Blocking() : on(IsGreenThread()) { if (on) GreenBlocking(true); }
  ~Blocking() { if (on) GreenBlocking(false); }
};

int GuestFutexWait(void* addr, uint32_t val, double ms) {
  Blocking b;
  if (ndk_sig_wait) {
    int saved = errno;  // handlers that run inside may leave the host's errno changed
    int r = ndk_sig_wait(Addr(addr), val, ms);
    errno = saved;
    return r;
  }
  int e = emscripten_futex_wait(addr, val, ms);
  if (e == 0) return 0;
  if (e == -ETIMEDOUT) return -kLinuxETIMEDOUT;
  if (e == -EWOULDBLOCK) return -kLinuxEAGAIN;
  return -ToGuestErrno(-e);
}

int GuestFutexWaitAt(int site, void* addr, uint32_t val, double ms, const uint32_t* u) {
  if (NdkResume* r = GreenResumed(site)) return r->result;
  if (ms > 0 && GreenCanPark())
    return GreenParkFutex(addr, val, isinf(ms) ? INFINITY : GreenNowMs() + ms, site, u);
  return GuestFutexWait(addr, val, ms);
}

// --- time stretch (topic timing-races) ---------------------------------------------------
// Guest code that sleeps a fixed time and counts on another part being ready by
// then (a mod's thread: "library loaded? sleep(3), then il2cpp" while il2cpp_init
// still runs here; SDK and anti-cheat threads that wait "long enough") is right on
// a phone and early here, where starting up takes several times longer. While the
// app has a surface on screen that has not shown its first frame yet (the
// display's SurfaceFlinger tells: ndk_time_stretch_screen), a sleep of at least
// kStretchMinMs by a thread guest code made (pthread_create: not a Java thread,
// which may be the one drawing) lasts up to ARC_TIME_STRETCH times as long, and
// ends as soon as that screen is ready, never before the time asked. Clocks are
// not touched (only a sleep returns later, as on a busy CPU). ARC_TIME_STRETCH=0:
// sleeps as asked.
namespace {
const double kStretchMinMs = 100, kStretchSliceMs = 100;
const double kStretchDefault = 8;
// A screen that has not been ready for this long (a surface that never gets a
// frame) no longer stretches sleeps.
const double kStretchPendingMaxMs = 180000;
thread_local bool t_guest_made = false;   // pthread_create of guest code made it
// -1: no screen here (no SurfaceFlinger in this process), 0: ready, 1: waiting.
std::atomic<int> g_screen{-1};
std::atomic<int64_t> g_screen_since_us{0};
std::atomic<int> g_stretch_logs{0};

double StretchNowMs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
double StretchFactor() {
  static const double k = [] {
    const char* e = getenv("ARC_TIME_STRETCH");
    return e && *e ? atof(e) : kStretchDefault;
  }();
  return k;
}
bool StretchActive(double now) {
  return g_screen.load() == 1 && now - g_screen_since_us.load() / 1000.0 < kStretchPendingMaxMs;
}
// The end of a stretched sleep from |t0| (0: not stretched).
double StretchEnd(double t0, double ms) {
  const double k = StretchFactor();
  if (k <= 1 || ms < kStretchMinMs || !(t_guest_made || IsGreenThread()) || !StretchActive(t0))
    return 0;
  if (g_stretch_logs.fetch_add(1) < 20)
    __android_log_print(4, "ndk", "time stretch: thread %d sleeps %.0f ms, up to %.0f ms while a "
                        "surface waits for its first frame", GuestTid(), ms, k * ms);
  return t0 + k * ms;
}
// When a sleep asked to end at |end| and stretched to |ext| (0: not) wakes next,
// from |now|; 0: it is over.
double StretchWake(double now, double end, double ext) {
  if (now < end) return end;
  if (ext <= 0 || now >= ext || !StretchActive(now)) return 0;
  return now + kStretchSliceMs < ext ? now + kStretchSliceMs : ext;
}
void StretchDone(double t0, double ms, double ext) {
  if (ext > 0 && g_stretch_logs.fetch_add(1) < 20)
    __android_log_print(4, "ndk", "time stretch: thread %d slept %.0f ms (asked %.0f)", GuestTid(),
                        StretchNowMs() - t0, ms);
}
}  // namespace

// The display's SurfaceFlinger, after each composition: whether a surface on
// screen waits for its first frame (or nothing has been shown yet).
extern "C" void ndk_time_stretch_screen(int waiting) {
  const int now = waiting ? 1 : 0;
  const int old = g_screen.exchange(now);
  if (old == now) return;
  if (now) g_screen_since_us.store(static_cast<int64_t>(StretchNowMs() * 1000));
}

// A green thread parks (kParked); its call, run again, finds the deadline
// (and the stretched one: u[2..3]).
int GuestSleep(double ms, double* left_ms) {
  if (NdkResume* r = GreenResumed(kSiteSleep)) {
    double deadline, ext;
    memcpy(&deadline, r->u, sizeof deadline);
    memcpy(&ext, r->u + 2, sizeof ext);
    if (r->result != -kLinuxEINTR) {
      const double wake = StretchWake(GreenNowMs(), deadline, ext);
      if (wake > 0 && GreenCanPark()) return GreenParkTimed(wake, kSiteSleep, r->u);
      if (ext > 0) {
        const double asked = (ext - deadline) / (StretchFactor() - 1);
        StretchDone(deadline - asked, asked, ext);
      }
      return 0;
    }
    if (left_ms) *left_ms = deadline - GreenNowMs() > 0 ? deadline - GreenNowMs() : 0;
    return -kLinuxEINTR;
  }
  if (ms > 0 && GreenCanPark()) {
    const double t0 = GreenNowMs(), deadline = t0 + ms, ext = StretchEnd(t0, ms);
    uint32_t u[4] = {0, 0, 0, 0};
    memcpy(u, &deadline, sizeof deadline);
    memcpy(u + 2, &ext, sizeof ext);
    return GreenParkTimed(deadline, kSiteSleep, u);
  }
  Blocking b;
  if (ndk_sig_sleep) {
    int saved = errno;
    const double t0 = StretchNowMs(), end = t0 + ms, ext = ms > 0 ? StretchEnd(t0, ms) : 0;
    int r = ndk_sig_sleep(ms, left_ms);
    for (double now = StretchNowMs(), wake; r == 0 && (wake = StretchWake(now, end, ext)) > 0;
         now = StretchNowMs()) {
      double left = 0;
      r = ndk_sig_sleep(wake - now, &left);
      if (r != 0 && left_ms) *left_ms = end - StretchNowMs() > 0 ? end - StretchNowMs() : 0;
    }
    if (r == 0) StretchDone(t0, ms, ext);
    errno = saved;
    return r;
  }
  if (left_ms) *left_ms = 0;
  if (ms > 0) {
    struct timespec ts = {static_cast<time_t>(ms / 1000), static_cast<long>(fmod(ms, 1000.0) * 1e6)};
    nanosleep(&ts, nullptr);
  }
  return 0;
}

extern "C" int ndk_sig_suspend(const uint64_t* mask) __attribute__((weak));
int GuestSigsuspend(const uint64_t* mask) {
  if (!ndk_sig_suspend || !ndk_sig_mask) return -kLinuxEINTR;
  if (NdkResume* r = GreenResumed(kSiteSuspend)) {   // a handler ran: the mask as before
    uint64_t old = r->u[0] | (uint64_t)r->u[1] << 32;
    ndk_sig_mask(2, &old, nullptr);
    return -kLinuxEINTR;
  }
  if (GreenCanPark()) {
    // The mask is |mask| while it waits (its handler runs with it, when the
    // thread resumes); a signal that is pending already runs at once.
    uint64_t old = 0;
    ndk_sig_mask(2, mask, &old);
    if (ndk_sig_check && ndk_sig_check()) {
      ndk_sig_mask(2, &old, nullptr);
    } else {
      uint32_t u[4] = {static_cast<uint32_t>(old), static_cast<uint32_t>(old >> 32), 0, 0};
      return GreenParkTimed(INFINITY, kSiteSuspend, u);
    }
  }
  Blocking b;
  return ndk_sig_suspend(mask);
}

int GuestTid() {
  int tid = gettid();
  if (ndk_sig_self) ndk_sig_self(nullptr, &tid);
  return tid;
}

int SignalSlicedWait(int timeout_ms, int (*wait)(void* ctx, int ms), void* ctx) {
  if (timeout_ms == 0)
    return wait(ctx, timeout_ms);
  Blocking b;
  if (!ndk_sig_check)
    return wait(ctx, timeout_ms);
  const int kSliceMs = 50;
  struct timespec t0;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  for (;;) {
    if (ndk_sig_check() && ndk_sig_deliver()) {
      errno = EINTR;
      return -1;
    }
    int slice = kSliceMs;
    if (timeout_ms > 0) {
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      int left = timeout_ms - (int)((now.tv_sec - t0.tv_sec) * 1000 + (now.tv_nsec - t0.tv_nsec) / 1000000);
      if (left <= 0) return 0;
      if (left < slice) slice = left;
    }
    int r = wait(ctx, slice);
    if (r != 0) return r;
  }
}

// ARC_NDK_THREADS_LOG=1 (&env= of the test page): every guest thread's start
// routine and the names threads give themselves, to stderr (which threads an
// app makes: each is a worker, ~6-10 MB of an iPhone's tab; topic ios-threads).
bool ThreadsLog() {
  static const bool on = getenv("ARC_NDK_THREADS_LOG") && *getenv("ARC_NDK_THREADS_LOG") == '1';
  return on;
}

namespace {
int PollSlice(void* p, int ms) {
  const std::pair<struct pollfd*, nfds_t>* a = static_cast<std::pair<struct pollfd*, nfds_t>*>(p);
  return poll(a->first, a->second, ms);
}
}  // namespace

int GuestPoll(struct pollfd* fds, nfds_t n, int timeout_ms) {
  if (n == 0 && timeout_ms > 0) {   // a sleep
    double left = 0;
    int r = GuestSleep(timeout_ms, &left);
    if (r == kParked) return kParked;
    if (r == 0) return 0;
    errno = EINTR;
    return -1;
  }
  std::pair<struct pollfd*, nfds_t> a(fds, n);
  return SignalSlicedWait(timeout_ms, PollSlice, &a);
}

namespace {

struct GTimespec {
  int32_t tv_sec, tv_nsec;
};

int Err(int host) { return ToGuestErrno(host); }

std::atomic<uint32_t>* Word(void* p) { return reinterpret_cast<std::atomic<uint32_t>*>(p); }

// A host object of a guest word: |magic| while it lives, 0 once destroyed.
template <typename T>
struct Box {
  uint32_t magic;
  T obj;
};

// The live Box at |addr| (a guest word's value without the tag), or null.
template <typename T>
Box<T>* LiveBox(uint32_t addr, uint32_t magic) {
  if (addr < 0x10000 || (addr & 7) != 0 || addr + sizeof(Box<T>) > emscripten_get_heap_size())
    return nullptr;
  Box<T>* b = Ptr<Box<T>>(addr);
  return __atomic_load_n(&b->magic, __ATOMIC_ACQUIRE) == magic ? b : nullptr;
}

std::atomic<int> g_stale_reports{0};
void ReportStale(const char* kind, void* guest, uint32_t v) {
  int n = g_stale_reports.fetch_add(1);
  if (n < 20)
    __android_log_print(5, "ndk", "guest %s at 0x%x holds 0x%x, not a live one: taken as a new %s"
                        " (used after its memory was freed?)", kind, Addr(guest), v, kind);
}

// Returns the host object behind a guest word, creating it with |init|
// (given the untagged initial value) if needed.
template <typename T, uint32_t kMagic, typename Init, typename Fini>
T* Hosted(void* guest, const char* kind, Init init, Fini fini) {
  std::atomic<uint32_t>* w = Word(guest);
  for (;;) {
    uint32_t v = w->load(std::memory_order_acquire);
    if (v & 1) {
      if (Box<T>* live = LiveBox<T>(v & ~1u, kMagic))
        return &live->obj;
      ReportStale(kind, guest, v);
      if (!w->compare_exchange_strong(v, 0, std::memory_order_acq_rel))
        continue;
      v = 0;
    }
    Box<T>* b = new Box<T>;
    init(&b->obj, v);
    __atomic_store_n(&b->magic, kMagic, __ATOMIC_RELEASE);
    if (w->compare_exchange_strong(v, Addr(b) | 1, std::memory_order_acq_rel))
      return &b->obj;
    fini(&b->obj);  // another thread won; take its object
    delete b;
  }
}

// Clears a guest word and destroys its host object, if it has a live one.
template <typename T, uint32_t kMagic, typename Fini>
void Unhost(void* guest, Fini fini) {
  uint32_t v = Word(guest)->exchange(0);
  if (!(v & 1))
    return;
  Box<T>* b = LiveBox<T>(v & ~1u, kMagic);
  uint32_t expected = kMagic;
  if (!b || !__atomic_compare_exchange_n(&b->magic, &expected, 0u, false, __ATOMIC_ACQ_REL,
                                         __ATOMIC_ACQUIRE))
    return;
  fini(&b->obj);
  delete b;
}

const uint32_t kMutexMagic = 0x6d757478, kCondMagic = 0x636f6e64, kRwlockMagic = 0x72776c6b;

// --- mutex ----------------------------------------------------------------

// A futex mutex (Drepper's "mutex 2"): state 0 free, 1 locked, 2 locked
// with waiters. Recursive and error-checking ones note their owner.
struct GMutex {
  std::atomic<uint32_t> state;
  int type;  // 0 normal, 1 recursive, 2 error checking (bionic's attribute types)
  std::atomic<pid_t> owner;
  uint32_t count;
};

// bionic's initializers: 0 normal, 0x4000 recursive, 0x8000 error checking.
void InitMutex(GMutex* m, uint32_t init) {
  m->state.store(0);
  m->type = (init & 0x4000) ? 1 : (init & 0x8000) ? 2 : 0;
  m->owner.store(0);
  m->count = 0;
}
void FiniMutex(GMutex*) {}
GMutex* Mutex(void* g) {
  return Hosted<GMutex, kMutexMagic>(g, "mutex", InitMutex, FiniMutex);
}

double MsSince(const struct timespec& t0) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (now.tv_sec - t0.tv_sec) * 1000.0 + (now.tv_nsec - t0.tv_nsec) / 1e6;
}

// Locks |m| within |ms| (INFINITY: no limit). A guest signal that comes
// while it waits runs its handler, and the wait goes on (as SA_RESTART).
// |waiter|: the call of a green thread that parked in the wait below runs
// again (GreenResumed); it was woken as a waiter and takes the lock as one
// (state 2), or the wake of the next waiter would be lost.
int LockMutex(GMutex* m, double ms, int site = kSiteMutex, const uint32_t* u = nullptr,
              bool waiter = false) {
  const pid_t self = GuestTid();
  if (!waiter && GreenResumed(site)) waiter = true;
  if (!waiter && m->type != 0 && m->owner.load(std::memory_order_relaxed) == self) {
    if (m->type == 2) return EDEADLK;
    if (m->count == UINT32_MAX) return EAGAIN;
    m->count++;
    return 0;
  }
  uint32_t expected = 0;
  if (waiter || !m->state.compare_exchange_strong(expected, 1)) {
    struct timespec t0;
    if (!isinf(ms)) clock_gettime(CLOCK_MONOTONIC, &t0);
    while (m->state.exchange(2) != 0) {
      double left = ms;
      if (!isinf(ms) && (left = ms - MsSince(t0)) <= 0) return ETIMEDOUT;
      if (GuestFutexWaitAt(site, &m->state, 2, left, u) == kParked) return kParked;
    }
  }
  m->owner.store(self, std::memory_order_relaxed);
  m->count = 1;
  return 0;
}
int TrylockMutex(GMutex* m) {
  if (m->type != 0 && m->owner.load(std::memory_order_relaxed) == GuestTid()) {
    if (m->type == 2) return EBUSY;
    if (m->count == UINT32_MAX) return EAGAIN;
    m->count++;
    return 0;
  }
  uint32_t expected = 0;
  if (!m->state.compare_exchange_strong(expected, 1)) return EBUSY;
  m->owner.store(GuestTid(), std::memory_order_relaxed);
  m->count = 1;
  return 0;
}
// Releases |m| entirely (a recursive one too); the caller checked ownership.
void ReleaseMutex(GMutex* m) {
  m->count = 0;
  m->owner.store(0, std::memory_order_relaxed);
  if (m->state.exchange(0) == 2) GuestFutexWake(&m->state, 1);
}
int UnlockMutex(GMutex* m) {
  if (m->type != 0) {
    if (m->owner.load(std::memory_order_relaxed) != GuestTid()) return EPERM;
    if (--m->count > 0) return 0;
  }
  ReleaseMutex(m);
  return 0;
}

// bionic's pthread_mutexattr_t: a long, the type in its low bits.
int G_mutexattr_init(int32_t* a) { *a = 0; return 0; }
int G_mutexattr_destroy(int32_t* a) { *a = -1; return 0; }
int G_mutexattr_settype(int32_t* a, int type) {
  if (type < 0 || type > 2) return Err(EINVAL);
  *a = (*a & ~3) | type;
  return 0;
}
int G_mutexattr_gettype(const int32_t* a, int* type) { *type = *a & 3; return 0; }
int G_mutexattr_setpshared(int32_t*, int) { return 0; }
int G_mutexattr_getpshared(const int32_t*, int* p) { *p = 0; return 0; }

int G_mutex_init(uint32_t* m, const int32_t* attr) {
  int type = attr ? (*attr & 3) : 0;
  *m = type == 1 ? 0x4000 : type == 2 ? 0x8000 : 0;
  Mutex(m);
  return 0;
}
int G_mutex_destroy(uint32_t* m) {
  Unhost<GMutex, kMutexMagic>(m, FiniMutex);
  return 0;
}
int G_mutex_lock(void* m) {
  int e = LockMutex(Mutex(m), INFINITY);
  return e == kParked ? 0 : Err(e);
}
int G_mutex_trylock(void* m) { return Err(TrylockMutex(Mutex(m))); }
int G_mutex_unlock(void* m) { return Err(UnlockMutex(Mutex(m))); }

// --- condition variables ------------------------------------------------------

// bionic's pthread_condattr_t: a long, bit 0 process-shared, bit 1
// CLOCK_MONOTONIC (Android 5+: pthread_condattr_setclock). An initialized
// guest word holds the clock as its untagged value until first use.
// A wait sleeps on a sequence number that every signal/broadcast advances;
// a guest signal ends the wait early (a spurious wakeup, which POSIX allows)
// after its handler ran, before the mutex is taken again.
const int32_t kCondMonotonic = 2;
struct GCond {
  std::atomic<uint32_t> seq;
  clockid_t clock;  // what pthread_cond_timedwait's deadline is measured on
};
void InitCond(GCond* c, uint32_t init) {
  c->seq.store(0);
  c->clock = (init & kCondMonotonic) ? CLOCK_MONOTONIC : CLOCK_REALTIME;
}
void FiniCond(GCond*) {}
GCond* Cond(void* g) { return Hosted<GCond, kCondMagic>(g, "condition variable", InitCond, FiniCond); }

int G_condattr_init(int32_t* a) { *a = 0; return 0; }
int G_condattr_destroy(int32_t* a) { *a = -1; return 0; }
int G_condattr_setpshared(int32_t*, int) { return 0; }
int G_condattr_getpshared(const int32_t*, int* p) { *p = 0; return 0; }
int G_condattr_setclock(int32_t* a, int clock) {
  if (clock == 1)  // CLOCK_MONOTONIC
    *a |= kCondMonotonic;
  else if (clock == 0)  // CLOCK_REALTIME
    *a &= ~kCondMonotonic;
  else
    return Err(EINVAL);
  return 0;
}
int G_condattr_getclock(const int32_t* a, int* clock) {
  *clock = (*a & kCondMonotonic) ? 1 : 0;
  return 0;
}
int G_cond_init(uint32_t* c, const int32_t* attr) {
  *c = (attr && (*attr & kCondMonotonic)) ? kCondMonotonic : 0;
  Cond(c);
  return 0;
}
int G_cond_destroy(uint32_t* c) {
  Unhost<GCond, kCondMagic>(c, FiniCond);
  return 0;
}
int G_cond_signal(void* c) {
  GCond* hc = Cond(c);
  hc->seq.fetch_add(1);
  GuestFutexWake(&hc->seq, 1);
  return 0;
}
int G_cond_broadcast(void* c) {
  GCond* hc = Cond(c);
  hc->seq.fetch_add(1);
  GuestFutexWake(&hc->seq, INT32_MAX);
  return 0;
}

// Waits at most |ms| (INFINITY: no limit); a host errno, or kParked. A
// green thread parks in the wait (record: the mutex's count) or in taking
// the mutex again (the count and the wait's result); run again, it goes on
// from there.
int CondWait(void* c, void* m, double ms) {
  GCond* hc = Cond(c);
  GMutex* hm = Mutex(m);
  uint32_t count;
  int r;
  bool waiter = false;   // it parked taking the mutex again
  if (NdkResume* rec = GreenResumed(kSiteCond)) {
    count = rec->u[0];
    r = rec->result;
  } else if (NdkResume* rec = GreenResumed(kSiteCondRelock)) {
    count = rec->u[0];
    r = static_cast<int>(rec->u[1]);
    waiter = true;
  } else {
    if (hm->type != 0 && hm->owner.load(std::memory_order_relaxed) != GuestTid()) return EPERM;
    if (ms <= 0) return ETIMEDOUT;
    const uint32_t seq = hc->seq.load();
    count = hm->count;
    ReleaseMutex(hm);
    const uint32_t u[4] = {count, 0, 0, 0};
    r = GuestFutexWaitAt(kSiteCond, &hc->seq, seq, ms, u);
    if (r == kParked) return kParked;
  }
  const uint32_t u[4] = {count, static_cast<uint32_t>(r), 0, 0};
  if (LockMutex(hm, INFINITY, kSiteCondRelock, u, waiter) == kParked) return kParked;
  hm->count = count ? count : 1;
  return r == -kLinuxETIMEDOUT ? ETIMEDOUT : 0;
}
int G_cond_wait(void* c, void* m) {
  int e = CondWait(c, m, INFINITY);
  return e == kParked ? 0 : Err(e);
}

int64_t NsUntil(clockid_t clk, const GTimespec* abs) {
  struct timespec now;
  clock_gettime(clk, &now);
  return (abs->tv_sec - (int64_t)now.tv_sec) * 1000000000LL + (abs->tv_nsec - now.tv_nsec);
}
int CondErr(int e) { return e == kParked ? 0 : Err(e); }
// The deadline is on the condition variable's clock.
int G_cond_timedwait(void* c, void* m, const GTimespec* abs) {
  return CondErr(CondWait(c, m, NsUntil(Cond(c)->clock, abs) / 1e6));
}
int G_cond_timedwait_monotonic(void* c, void* m, const GTimespec* abs) {
  return CondErr(CondWait(c, m, NsUntil(CLOCK_MONOTONIC, abs) / 1e6));
}
int G_cond_timedwait_relative_np(void* c, void* m, const GTimespec* rel) {
  return CondErr(CondWait(c, m, rel->tv_sec * 1000.0 + rel->tv_nsec / 1e6));
}
int G_cond_timeout_np(void* c, void* m, uint32_t ms) { return CondErr(CondWait(c, m, ms)); }

// --- rwlock (40 bytes in bionic; the first word holds our pointer) --------------

// state: readers holding it, -1 for a writer. Waiters sleep on the state.
struct GRwlock {
  std::atomic<int32_t> state;
  std::atomic<uint32_t> waiters;
  std::atomic<pid_t> writer;
};
void InitRwlock(GRwlock* l, uint32_t) {
  l->state.store(0);
  l->waiters.store(0);
  l->writer.store(0);
}
void FiniRwlock(GRwlock*) {}
GRwlock* Rwlock(void* g) {
  return Hosted<GRwlock, kRwlockMagic>(g, "rwlock", InitRwlock, FiniRwlock);
}
int G_rwlock_init(uint32_t* l, const void*) {
  memset(l, 0, 40);
  Rwlock(l);
  return 0;
}
int G_rwlock_destroy(uint32_t* l) {
  Unhost<GRwlock, kRwlockMagic>(l, FiniRwlock);
  return 0;
}
// Returns false if a green thread parked (|waiters| keeps it counted: its
// call, run again, takes it off first, RwResumed).
bool RwWait(GRwlock* l, int32_t seen) {
  l->waiters.fetch_add(1);
  if (GuestFutexWaitAt(kSiteRwlock, &l->state, static_cast<uint32_t>(seen), INFINITY) == kParked)
    return false;
  l->waiters.fetch_sub(1);
  return true;
}
void RwResumed(GRwlock* l) {
  if (GreenResumed(kSiteRwlock)) l->waiters.fetch_sub(1);
}
int RdTry(GRwlock* l) {
  int32_t s = l->state.load();
  while (s >= 0)
    if (l->state.compare_exchange_weak(s, s + 1)) return 0;
  return EBUSY;
}
int G_rwlock_rdlock(void* g) {
  GRwlock* l = Rwlock(g);
  RwResumed(l);
  for (;;) {
    if (RdTry(l) == 0) return 0;
    if (l->writer.load(std::memory_order_relaxed) == GuestTid()) return Err(EDEADLK);
    if (!RwWait(l, -1)) return 0;
  }
}
int G_rwlock_tryrdlock(void* g) { return Err(RdTry(Rwlock(g))); }
int WrTry(GRwlock* l, int32_t* seen) {
  int32_t expected = 0;
  if (l->state.compare_exchange_strong(expected, -1)) {
    l->writer.store(GuestTid(), std::memory_order_relaxed);
    return 0;
  }
  *seen = expected;
  return EBUSY;
}
int G_rwlock_wrlock(void* g) {
  GRwlock* l = Rwlock(g);
  RwResumed(l);
  for (;;) {
    int32_t seen;
    if (WrTry(l, &seen) == 0) return 0;
    if (seen < 0 && l->writer.load(std::memory_order_relaxed) == GuestTid()) return Err(EDEADLK);
    if (!RwWait(l, seen)) return 0;
  }
}
int G_rwlock_trywrlock(void* g) {
  int32_t seen;
  return Err(WrTry(Rwlock(g), &seen));
}
int G_rwlock_unlock(void* g) {
  GRwlock* l = Rwlock(g);
  int32_t s = l->state.load();
  if (s == -1) {
    l->writer.store(0, std::memory_order_relaxed);
    l->state.store(0);
  } else if (s > 0) {
    l->state.fetch_sub(1);
  } else {
    return Err(EPERM);
  }
  if (l->waiters.load()) GuestFutexWake(&l->state, INT32_MAX);
  return 0;
}
// bionic's pthread_rwlockattr_t: a long (process-shared flag).
int G_rwlockattr_init(int32_t* a) { *a = 0; return 0; }
int G_rwlockattr_destroy(int32_t* a) { *a = -1; return 0; }
int G_rwlockattr_setpshared(int32_t* a, int p) { *a = p; return 0; }
int G_rwlockattr_getpshared(const int32_t* a, int* p) { *p = *a; return 0; }

// --- once ------------------------------------------------------------------------
// 0: not run, 1: running, 2: done (waiters sleep on the word).
int G_once(uint32_t* ctl, uint32_t init) {
  std::atomic<uint32_t>* w = Word(ctl);
  for (;;) {
    uint32_t v = w->load(std::memory_order_acquire);
    if (v == 2)
      return 0;
    if (v == 0 && w->compare_exchange_strong(v, 1)) {
      CallGuest0(init);
      w->store(2, std::memory_order_release);
      GuestFutexWake(ctl, INT32_MAX);
      return 0;
    }
    if (GuestFutexWaitAt(kSiteFutex, ctl, 1, 10) == kParked)   // (runs again from the start)
      return 0;
  }
}

// --- semaphores (bionic sem_t: one word, the count) ------------------------------

int G_sem_init(uint32_t* s, int, uint32_t value) {
  Word(s)->store(value);
  return 0;
}
int G_sem_destroy(uint32_t*) { return 0; }
int G_sem_post(uint32_t* s) {
  Word(s)->fetch_add(1);
  GuestFutexWake(s, 1);
  return 0;
}
bool TryDown(uint32_t* s) {
  uint32_t v = Word(s)->load();
  while (v > 0) {
    if (Word(s)->compare_exchange_weak(v, v - 1))
      return true;
  }
  return false;
}
int G_sem_trywait(uint32_t* s) {
  if (TryDown(s))
    return 0;
  errno = EAGAIN;
  return -1;
}
// A guest signal's handler runs while it waits; the wait goes on.
int G_sem_wait(uint32_t* s) {
  while (!TryDown(s))
    if (GuestFutexWaitAt(kSiteSem, s, 0, INFINITY) == kParked) return 0;
  return 0;
}
int G_sem_timedwait(uint32_t* s, const GTimespec* abs) {
  while (!TryDown(s)) {
    double ms = NsUntil(CLOCK_REALTIME, abs) / 1e6;
    if (ms <= 0) {
      errno = ETIMEDOUT;
      return -1;
    }
    if (GuestFutexWaitAt(kSiteSem, s, 0, ms) == kParked) return 0;
  }
  return 0;
}
int G_sem_getvalue(uint32_t* s, int* v) {
  *v = (int)Word(s)->load();
  return 0;
}

// --- thread specific data ---------------------------------------------------------

// Keys are one table for all of the VM's guest code: light processes
// (multiproc) each load their own copy of an app's libraries, and Gecko alone
// creates a few dozen keys per copy (bionic's 128 ran out at the third
// content process: "Could not initialize FlushRejections::sDispatched"). So
// more keys, each remembering the process (native namespace) that created
// it; a killed process's keys are freed. A key's generation tags the values
// threads set: a key used again (after pthread_key_delete or its process's
// death) reads 0 in every thread, as bionic's sequence numbers make it.
const int kMaxKeys = 512;
struct KeyInfo {
  bool used;
  uint32_t dtor;
  int ns;  // arc_lproc_native_ns() of the creator (0: the VM's)
};
std::mutex g_key_lock;
KeyInfo g_keys[kMaxKeys];
std::atomic<uint32_t> g_key_gen[kMaxKeys];
struct Tsd {
  uint32_t value[kMaxKeys];
  uint32_t gen[kMaxKeys];
};
thread_local Tsd* t_tsd;
pthread_key_t g_exit_key;  // host key whose destructor runs the guest ones

void RunGuestDestructors(void* p) {
  Tsd* tsd = static_cast<Tsd*>(p);
  // As POSIX: up to PTHREAD_DESTRUCTOR_ITERATIONS rounds.
  for (int round = 0; round < 4; round++) {
    bool any = false;
    for (int k = 0; k < kMaxKeys; k++) {
      uint32_t v = tsd->value[k], dtor;
      {
        std::lock_guard<std::mutex> l(g_key_lock);
        dtor = g_keys[k].used && tsd->gen[k] == g_key_gen[k].load() ? g_keys[k].dtor : 0;
      }
      if (v && dtor) {
        tsd->value[k] = 0;
        CallGuest1(dtor, v);
        any = true;
      }
    }
    if (!any)
      break;
  }
  t_tsd = nullptr;
  free(tsd);
}

Tsd* ThreadTsd() {
  if (!t_tsd) {
    t_tsd = static_cast<Tsd*>(calloc(1, sizeof(Tsd)));
    // (A green thread's run at its end, GreenThreadExit; not its carrier's.)
    if (!IsGreenThread()) pthread_setspecific(g_exit_key, t_tsd);
  }
  return t_tsd;
}
void* TsdSlot() { return &t_tsd; }
[[maybe_unused]] const bool kTsdSlot = (RegisterGreenSlot(TsdSlot, sizeof(t_tsd)), true);

int G_key_create(int32_t* key, uint32_t dtor) {
  std::lock_guard<std::mutex> l(g_key_lock);
  for (int k = 0; k < kMaxKeys; k++) {
    if (!g_keys[k].used) {
      g_keys[k] = {true, dtor, arc_lproc_native_ns()};
      g_key_gen[k].fetch_add(1);
      *key = k;
      return 0;
    }
  }
  return Err(EAGAIN);
}
int G_key_delete(int32_t key) {
  std::lock_guard<std::mutex> l(g_key_lock);
  if (key < 0 || key >= kMaxKeys || !g_keys[key].used)
    return Err(EINVAL);
  g_keys[key].used = false;
  return 0;
}
uint32_t G_getspecific(int32_t key) {
  if (key < 0 || key >= kMaxKeys)
    return 0;
  Tsd* tsd = ThreadTsd();
  return tsd->gen[key] == g_key_gen[key].load(std::memory_order_relaxed) ? tsd->value[key] : 0;
}
int G_setspecific(int32_t key, uint32_t value) {
  if (key < 0 || key >= kMaxKeys)
    return Err(EINVAL);
  Tsd* tsd = ThreadTsd();
  tsd->gen[key] = g_key_gen[key].load(std::memory_order_relaxed);
  tsd->value[key] = value;
  return 0;
}

// --- threads -------------------------------------------------------------------

// bionic's pthread_attr_t.
struct GAttr {
  uint32_t flags, stack_base, stack_size, guard_size;
  int32_t sched_policy, sched_priority;
};
const uint32_t kDetached = 1;

int G_attr_init(GAttr* a) {
  *a = {0, 0, 1024 * 1024, 4096, 0, 0};
  return 0;
}
int G_attr_destroy(GAttr* a) {
  memset(a, 0x42, sizeof *a);
  return 0;
}
int G_attr_setdetachstate(GAttr* a, int s) {
  a->flags = s ? (a->flags | kDetached) : (a->flags & ~kDetached);
  return 0;
}
int G_attr_getdetachstate(const GAttr* a, int* s) {
  *s = (a->flags & kDetached) ? 1 : 0;
  return 0;
}
int G_attr_setstacksize(GAttr* a, uint32_t size) {
  if (size < 8192) return Err(EINVAL);
  a->stack_size = size;
  return 0;
}
int G_attr_getstacksize(const GAttr* a, uint32_t* size) {
  *size = a->stack_size;
  return 0;
}
int G_attr_setschedpolicy(GAttr* a, int p) { a->sched_policy = p; return 0; }
int G_attr_getschedpolicy(const GAttr* a, int* p) { *p = a->sched_policy; return 0; }
int G_attr_setschedparam(GAttr* a, const int32_t* prio) { a->sched_priority = *prio; return 0; }
int G_attr_getschedparam(const GAttr* a, int32_t* prio) { *prio = a->sched_priority; return 0; }
int G_attr_setscope(GAttr*, int) { return 0; }
int G_attr_getscope(const GAttr*, int* s) { *s = 0; return 0; }
int G_attr_setguardsize(GAttr* a, uint32_t g) { a->guard_size = g; return 0; }
int G_attr_getguardsize(const GAttr* a, uint32_t* g) { *g = a->guard_size; return 0; }

// What is known of guest-visible threads: the guest stack the CPU gave each
// (pthread_getattr_np) and its kernel-style thread id (pthread_gettid_np).
struct ThreadInfo {
  uint32_t stack_base, stack_size;
  pid_t tid;
};
std::mutex g_threads_lock;
std::map<uint32_t, ThreadInfo> g_threads;  // by pthread_t

uint32_t G_self();

// The calling thread's entry, noted on first use.
ThreadInfo NoteSelf() {
  uint32_t self = G_self();
  {
    std::lock_guard<std::mutex> l(g_threads_lock);
    auto it = g_threads.find(self);
    if (it != g_threads.end())
      return it->second;
  }
  ThreadInfo info = {0, 0, GuestTid()};
  if (ndk_guest_stack) {
    CurrentState();  // the guest state (and stack) of this thread
    ndk_guest_stack(&info.stack_base, &info.stack_size);
  }
  std::lock_guard<std::mutex> l(g_threads_lock);
  g_threads[self] = info;
  return info;
}
void ForgetSelf(void*) {
  std::lock_guard<std::mutex> l(g_threads_lock);
  g_threads.erase(G_self());
}
pthread_key_t g_forget_key;

struct Start {
  uint32_t fn, arg, stack_size;
};
void* ThreadMain(void* p) {
  Start s = *static_cast<Start*>(p);
  delete static_cast<Start*>(p);
  // The guest's stack has the size it asked for (the CPU allocates it; a
  // stack the guest supplied with pthread_attr_setstack is not used).
  if (ndk_set_thread_guest_stack_size)
    ndk_set_thread_guest_stack_size(s.stack_size);
  NoteSelf();
  pthread_setspecific(g_forget_key, &g_forget_key);
  t_guest_made = true;
  return Ptr<void>(CallGuest1(s.fn, s.arg));
}

// A green thread's start and end, on its carrier as it (ndk_green.cc).
}  // namespace
void GreenThreadStart(uint32_t stack_size) {
  NoteSelf();
}
void GreenThreadExit() {
  if (t_tsd) RunGuestDestructors(t_tsd);
  ForgetSelf(nullptr);
  FreeGuestThread();
}
namespace {

int G_create(uint32_t* thread, const GAttr* ga, uint32_t fn, uint32_t arg) {
  uint32_t stack_size = ga ? ga->stack_size : 1024 * 1024;
  if (stack_size < 64 * 1024)
    stack_size = 64 * 1024;
  // A green thread (ndk_green.cc): a CPU state that carriers run.
  if (GreenCreate(thread, fn, arg, stack_size, ga && (ga->flags & kDetached)) == 0) {
    if (ThreadsLog()) {
      static std::atomic<int> n{0};
      uint32_t off = 0;
      const char* sym = Symbolize(fn, &off);
      fprintf(stderr, "ndk: guest thread %d (green %08x) starts at %s+0x%x\n", ++n, *thread, sym ? sym : "?", off);
    }
    return 0;
  }
  pthread_attr_t a;
  pthread_attr_init(&a);
  // Host frames of guest calls (thunks, JNI) need more than the guest's
  // stack size suggests; the guest's own stack belongs to the CPU.
  pthread_attr_setstacksize(&a, 1024 * 1024);
  if (ga && (ga->flags & kDetached))
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
  pthread_t t;
  Start* s = new Start{fn, arg, stack_size};
  int r = pthread_create(&t, &a, ThreadMain, s);
  pthread_attr_destroy(&a);
  if (ThreadsLog()) {
    static std::atomic<int> n{0};
    uint32_t off = 0;
    const char* sym = Symbolize(fn, &off);
    fprintf(stderr, "ndk: guest thread %d (r=%d) starts at %s+0x%x\n", ++n, r, sym ? sym : "?", off);
  }
  if (r != 0) {
    delete s;
    return Err(r);
  }
  *thread = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(t));
  return 0;
}
pthread_t HostThread(uint32_t t) { return reinterpret_cast<pthread_t>(static_cast<uintptr_t>(t)); }
// Struct Blocking above: a green thread joining a host thread keeps its carrier.
int G_join(uint32_t t, uint32_t* ret) {
  if (GreenEnabled() && IsGreen(t)) {
    int e = GreenJoin(t, ret);
    return e == kParked ? 0 : Err(e);
  }
  void* r = nullptr;
  Blocking b;
  int e = pthread_join(HostThread(t), &r);
  if (e == 0 && ret)
    *ret = Addr(r);
  return Err(e);
}
int G_detach(uint32_t t) {
  if (GreenEnabled() && IsGreen(t)) return Err(GreenDetach(t));
  return Err(pthread_detach(HostThread(t)));
}
// The thread guest code runs as (a signal handler a helper runs for a
// thread outside guest code runs as that thread: shim/ndk/cpu/signal.cc).
uint32_t G_self() {
  uint32_t self;
  if (ndk_sig_self) {
    ndk_sig_self(&self, nullptr);
    return self;
  }
  return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pthread_self()));
}
int G_equal(uint32_t a, uint32_t b) { return a == b; }
void G_exit(uint32_t ret) {
  if (GreenExit(ret)) return;   // a green thread ends when this call returns
  // The guest calls of this thread end here without returning: the CPU
  // lets go of its state (else its destructor waited for itself, and a
  // pthread_join for the thread forever).
  if (ndk_cpu_thread_exit) ndk_cpu_thread_exit();
  pthread_exit(Ptr<void>(ret));
}
// Signals to guest threads (shim/ndk/cpu/signal.cc): pending on the target's
// CPU, run by it at its next safe point; one sent to the calling thread runs
// before pthread_kill returns. A thread that never ran guest code has no
// handler to run: ESRCH (Boehm's GC then takes the thread for gone).
int G_kill(uint32_t t, int sig) {
  if (sig == 0 || !ndk_sig_send_pthread) return 0;
  int r = ndk_sig_send_pthread(t, sig);
  if (r == -3) return Err(ESRCH);
  if (r < 0) return Err(EINVAL);
  if (t == G_self() && ndk_sig_deliver) ndk_sig_deliver();
  return 0;
}
// bionic's 32-bit sigset_t: signals 1-32 (SIG_SETMASK clears 33-64, as
// bionic's LP32 pthread_sigmask).
int G_sigmask(int how, const uint32_t* set, uint32_t* old) {
  if (!ndk_sig_mask) {
    if (old) *old = 0;
    return 0;
  }
  uint64_t s64 = set ? *set : 0, o64 = 0;
  if (ndk_sig_mask(how, set ? &s64 : nullptr, &o64) != 0) return Err(EINVAL);
  if (old) *old = static_cast<uint32_t>(o64);
  if (ndk_sig_deliver) ndk_sig_deliver();  // what this unblocked arrives now
  return 0;
}
int G_getschedparam(uint32_t, int* policy, int32_t* prio) {
  *policy = 0;  // SCHED_OTHER
  *prio = 0;
  return 0;
}
int G_setschedparam(uint32_t, int, const int32_t*) { return 0; }
int G_setname_np(uint32_t t, const char* name) {
  if (ThreadsLog()) fprintf(stderr, "ndk: guest thread %08x named \"%.15s\"\n", t, name ? name : "");
  return 0;
}
// Android 5+: the stack of a running thread (bionic fills the whole
// pthread_attr_t). Threads other than the caller report what the CPU gave
// them if they ran guest code, else bionic's default.
int G_getattr_np(uint32_t t, GAttr* a) {
  G_attr_init(a);
  ThreadInfo info = {0, 0, 0};
  if (t == G_self()) {
    info = NoteSelf();
  } else {
    std::lock_guard<std::mutex> l(g_threads_lock);
    auto it = g_threads.find(t);
    if (it != g_threads.end())
      info = it->second;
  }
  if (info.stack_size) {
    a->stack_base = info.stack_base;
    a->stack_size = info.stack_size;
  }
  a->guard_size = 0;
  return 0;
}
int G_attr_getstack(const GAttr* a, uint32_t* base, uint32_t* size) {
  *base = a->stack_base;
  *size = a->stack_size;
  return 0;
}
int G_attr_setstack(GAttr* a, uint32_t base, uint32_t size) {
  if ((size & 15) || size < 16384)
    return Err(EINVAL);
  a->stack_base = base;
  a->stack_size = size;
  return 0;
}
int G_attr_getstackaddr(const GAttr* a, uint32_t* addr) {
  *addr = a->stack_base + a->stack_size;  // the top, as bionic's
  return 0;
}
pid_t G_gettid_np(uint32_t t) {
  if (t == G_self())
    return GuestTid();
  std::lock_guard<std::mutex> l(g_threads_lock);
  auto it = g_threads.find(t);
  return it != g_threads.end() ? it->second.tid : -1;
}
// No fork(), so fork handlers never run.
int G_atfork(uint32_t, uint32_t, uint32_t) { return 0; }
int G_register_atfork(uint32_t, uint32_t, uint32_t, uint32_t) { return 0; }

int G_getcpuclockid(uint32_t, int32_t* clk) {
  *clk = CLOCK_THREAD_CPUTIME_ID;
  return 0;
}


// --- arm64-v8a guests (topic ndk-arm64) -----------------------------------------
// Mutexes, condition variables, rwlocks, semaphores and once controls are the
// same words at the start of bionic's (larger) LP64 objects, so most of the
// above serves both ABIs (NDK_FNX). What differs: pthread_t (a long), the
// 56-byte pthread_attr_t, struct timespec, thread-specific values (64 bits)
// and guest callbacks (the AArch64 CPU).

struct G64Timespec : GuestLayout64 {
  int64_t tv_sec, tv_nsec;
};
GTimespec To32(const G64Timespec* t) {
  return {static_cast<int32_t>(t->tv_sec), static_cast<int32_t>(t->tv_nsec)};
}
// The 32-bit waits (CondWait: guest signals interrupt them) on converted times.
int G64_cond_timedwait(void* c, void* m, const G64Timespec* abs) {
  GTimespec t = To32(abs);
  return G_cond_timedwait(c, m, &t);
}
int G64_cond_timedwait_monotonic(void* c, void* m, const G64Timespec* abs) {
  GTimespec t = To32(abs);
  return G_cond_timedwait_monotonic(c, m, &t);
}
int G64_cond_timedwait_relative_np(void* c, void* m, const G64Timespec* rel) {
  GTimespec t = To32(rel);
  return G_cond_timedwait_relative_np(c, m, &t);
}
int G64_sem_timedwait(uint32_t* s, const G64Timespec* abs) {
  GTimespec t = To32(abs);
  return G_sem_timedwait(s, &t);
}
int G64_once(uint32_t* ctl, uint64_t init) {
  std::atomic<uint32_t>* w = Word(ctl);
  for (;;) {
    uint32_t v = w->load(std::memory_order_acquire);
    if (v == 2)
      return 0;
    if (v == 0 && w->compare_exchange_strong(v, 1)) {
      CallGuest64(static_cast<uint32_t>(init), nullptr, 0);
      w->store(2, std::memory_order_release);
      emscripten_futex_wake(ctl, INT32_MAX);
      return 0;
    }
    emscripten_futex_wait(ctl, 1, 10);
  }
}

// Thread-specific data of 64-bit guests: own keys (bionic's are small ints in
// both), 64-bit values, destructors run on the AArch64 CPU at thread exit.
// Owners and generations as for the 32-bit keys.
struct KeyInfo64 {
  bool used;
  uint64_t dtor;
  int ns;
};
KeyInfo64 g_keys64[kMaxKeys];
std::atomic<uint32_t> g_key_gen64[kMaxKeys];
struct Tsd64 {
  uint64_t value[kMaxKeys];
  uint32_t gen[kMaxKeys];
};
thread_local Tsd64* t_tsd64;
pthread_key_t g_exit_key64;

void RunGuestDestructors64(void* p) {
  Tsd64* tsd = static_cast<Tsd64*>(p);
  for (int round = 0; round < 4; round++) {
    bool any = false;
    for (int k = 0; k < kMaxKeys; k++) {
      uint64_t v = tsd->value[k], dtor;
      {
        std::lock_guard<std::mutex> l(g_key_lock);
        dtor = g_keys64[k].used && tsd->gen[k] == g_key_gen64[k].load() ? g_keys64[k].dtor : 0;
      }
      if (v && dtor) {
        tsd->value[k] = 0;
        CallGuest64(static_cast<uint32_t>(dtor), &v, 1);
        any = true;
      }
    }
    if (!any)
      break;
  }
  t_tsd64 = nullptr;
  free(tsd);
}
Tsd64* ThreadTsd64() {
  if (!t_tsd64) {
    t_tsd64 = static_cast<Tsd64*>(calloc(1, sizeof(Tsd64)));
    pthread_setspecific(g_exit_key64, t_tsd64);
  }
  return t_tsd64;
}
int G64_key_create(int32_t* key, uint64_t dtor) {
  std::lock_guard<std::mutex> l(g_key_lock);
  for (int k = 0; k < kMaxKeys; k++) {
    if (!g_keys64[k].used) {
      g_keys64[k] = {true, dtor, arc_lproc_native_ns()};
      g_key_gen64[k].fetch_add(1);
      *key = k;
      return 0;
    }
  }
  return Err(EAGAIN);
}
int G64_key_delete(int32_t key) {
  std::lock_guard<std::mutex> l(g_key_lock);
  if (key < 0 || key >= kMaxKeys || !g_keys64[key].used)
    return Err(EINVAL);
  g_keys64[key].used = false;
  return 0;
}
uint64_t G64_getspecific(int32_t key) {
  if (key < 0 || key >= kMaxKeys)
    return 0;
  Tsd64* tsd = ThreadTsd64();
  return tsd->gen[key] == g_key_gen64[key].load(std::memory_order_relaxed) ? tsd->value[key] : 0;
}
int G64_setspecific(int32_t key, uint64_t value) {
  if (key < 0 || key >= kMaxKeys)
    return Err(EINVAL);
  Tsd64* tsd = ThreadTsd64();
  tsd->gen[key] = g_key_gen64[key].load(std::memory_order_relaxed);
  tsd->value[key] = value;
  return 0;
}

// A killed light process's keys (both kinds) are free again; values its
// threads left are not seen through the keys' next generation.
void FreeProcessKeys(int pid) {
  std::lock_guard<std::mutex> l(g_key_lock);
  int n = 0;
  for (int k = 0; k < kMaxKeys; k++) {
    if (g_keys[k].used && g_keys[k].ns == pid) {
      g_keys[k].used = false;
      n++;
    }
    if (g_keys64[k].used && g_keys64[k].ns == pid) {
      g_keys64[k].used = false;
      n++;
    }
  }
  if (n)
    __android_log_print(4, "ndk", "pid %d: %d pthread keys freed", pid, n);
}
struct KeyKillHook {
  KeyKillHook() { arc_lproc_set_kill_hook(FreeProcessKeys); }
} g_key_kill_hook;

// bionic's LP64 pthread_attr_t (56 bytes).
struct GAttr64 : GuestLayout64 {
  uint32_t flags, pad0;
  uint64_t stack_base, stack_size, guard_size;
  int32_t sched_policy, sched_priority;
  uint8_t reserved[16];
};
static_assert(sizeof(GAttr64) == 56, "bionic LP64 pthread_attr_t");
int G64_attr_init(GAttr64* a) {
  memset(a, 0, sizeof *a);
  a->stack_size = 1024 * 1024;
  a->guard_size = 4096;
  return 0;
}
int G64_attr_destroy(GAttr64* a) {
  memset(a, 0x42, sizeof *a);
  return 0;
}
int G64_attr_setdetachstate(GAttr64* a, int s) {
  a->flags = s ? (a->flags | kDetached) : (a->flags & ~kDetached);
  return 0;
}
int G64_attr_getdetachstate(const GAttr64* a, int* s) {
  *s = (a->flags & kDetached) ? 1 : 0;
  return 0;
}
int G64_attr_setstacksize(GAttr64* a, uint64_t size) {
  if (size < 16384) return Err(EINVAL);
  a->stack_size = size;
  return 0;
}
int G64_attr_getstacksize(const GAttr64* a, uint64_t* size) {
  *size = a->stack_size;
  return 0;
}
int G64_attr_setschedpolicy(GAttr64* a, int p) { a->sched_policy = p; return 0; }
int G64_attr_getschedpolicy(const GAttr64* a, int* p) { *p = a->sched_policy; return 0; }
int G64_attr_setschedparam(GAttr64* a, const int32_t* prio) { a->sched_priority = *prio; return 0; }
int G64_attr_getschedparam(const GAttr64* a, int32_t* prio) { *prio = a->sched_priority; return 0; }
int G64_attr_setscope(GAttr64*, int) { return 0; }
int G64_attr_getscope(const GAttr64*, int* s) { *s = 0; return 0; }
int G64_attr_setguardsize(GAttr64* a, uint64_t g) { a->guard_size = g; return 0; }
int G64_attr_getguardsize(const GAttr64* a, uint64_t* g) { *g = a->guard_size; return 0; }
int G64_attr_getstack(const GAttr64* a, uint64_t* base, uint64_t* size) {
  *base = a->stack_base;
  *size = a->stack_size;
  return 0;
}
int G64_attr_setstack(GAttr64* a, uint64_t base, uint64_t size) {
  if ((size & 15) || size < 16384)
    return Err(EINVAL);
  a->stack_base = base;
  a->stack_size = size;
  return 0;
}
int G64_attr_getstackaddr(const GAttr64* a, uint64_t* addr) {
  *addr = a->stack_base + a->stack_size;
  return 0;
}

// Threads: a host thread whose guest state is the AArch64 CPU's; its 64-bit
// result goes through a box (pthread_exit/join pass a host pointer).
struct Result64 {
  uint64_t value;
};
ThreadInfo NoteSelf64() {
  uint32_t self = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pthread_self()));
  {
    std::lock_guard<std::mutex> l(g_threads_lock);
    auto it = g_threads.find(self);
    if (it != g_threads.end() && it->second.stack_size)
      return it->second;
  }
  ThreadInfo info = {0, 0, gettid()};
  if (ndk64_guest_stack) {
    CurrentState64();
    ndk64_guest_stack(&info.stack_base, &info.stack_size);
  }
  std::lock_guard<std::mutex> l(g_threads_lock);
  g_threads[self] = info;
  return info;
}
struct Start64 {
  uint64_t fn, arg;
  uint64_t stack_size;
};
// ARC_NDK_THREAD_DELAY=LIB:MS (diagnostics, off by default): a thread whose start
// routine is in LIB begins MS later -- to see whether a crash is a race of that
// library's thread with the rest (an injected mod calling il2cpp before il2cpp_init
// is done, topic unity2022-arm64).
void DelayThreadStart64(uint32_t fn) {
  const char* e = getenv("ARC_NDK_THREAD_DELAY");
  const char* colon = e ? strchr(e, ':') : nullptr;
  if (!colon || !Symbolize64)
    return;
  uint32_t off = 0;
  const char* sym = Symbolize64(fn, &off);
  const size_t n = static_cast<size_t>(colon - e);
  if (!sym || strncmp(sym, e, n) != 0 || sym[n] != '!')
    return;
  const int ms = atoi(colon + 1);
  __android_log_print(4, "ndk", "thread of %s+0x%x starts %d ms later (ARC_NDK_THREAD_DELAY)", sym, off, ms);
  usleep(static_cast<useconds_t>(ms) * 1000);
}
void* ThreadMain64(void* p) {
  Start64 s = *static_cast<Start64*>(p);
  delete static_cast<Start64*>(p);
  DelayThreadStart64(static_cast<uint32_t>(s.fn));
  if (ndk64_set_thread_guest_stack_size)
    ndk64_set_thread_guest_stack_size(static_cast<size_t>(s.stack_size));
  NoteSelf64();
  pthread_setspecific(g_forget_key, &g_forget_key);
  t_guest_made = true;
  uint64_t arg = s.arg;
  uint64_t r = CallGuest64(static_cast<uint32_t>(s.fn), &arg, 1);
  return new Result64{r};
}
int G64_create(uint64_t* thread, const GAttr64* ga, uint64_t fn, uint64_t arg) {
  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setstacksize(&a, 1024 * 1024);
  if (ga && (ga->flags & kDetached))
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
  uint64_t stack_size = ga ? ga->stack_size : 1024 * 1024;
  if (stack_size < 64 * 1024) stack_size = 64 * 1024;
  if (stack_size > 64 * 1024 * 1024) stack_size = 64 * 1024 * 1024;
  Start64* s = new Start64{fn, arg, stack_size};
  pthread_t t;
  int r = pthread_create(&t, &a, ThreadMain64, s);
  pthread_attr_destroy(&a);
  if (r != 0) {
    delete s;
    return Err(r);
  }
  *thread = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(t));
  if (ndk64_sig_thread_created) ndk64_sig_thread_created(*thread);
  return 0;
}
int G64_join(uint64_t t, uint64_t* ret) {
  void* r = nullptr;
  int e = pthread_join(HostThread(static_cast<uint32_t>(t)), &r);
  if (e == 0) {
    Result64* box = static_cast<Result64*>(r);
    if (ret) *ret = box && r != PTHREAD_CANCELED ? box->value : 0;
    if (box && r != PTHREAD_CANCELED) delete box;
  }
  return Err(e);
}
void G64_exit(uint64_t ret) { pthread_exit(new Result64{ret}); }
int G64_getattr_np(uint64_t t, GAttr64* a) {
  G64_attr_init(a);
  ThreadInfo info = {0, 0, 0};
  if (static_cast<uint32_t>(t) == static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pthread_self()))) {
    info = NoteSelf64();
  } else {
    std::lock_guard<std::mutex> l(g_threads_lock);
    auto it = g_threads.find(static_cast<uint32_t>(t));
    if (it != g_threads.end())
      info = it->second;
  }
  if (info.stack_size) {
    a->stack_base = info.stack_base;
    a->stack_size = info.stack_size;
  }
  a->guard_size = 0;
  return 0;
}
int G64_getcpuclockid(uint64_t, int32_t* clk) {
  *clk = CLOCK_THREAD_CPUTIME_ID;
  return 0;
}

const HostSymbol kPthread64[] = {
    NDK_FN64("pthread_cond_timedwait", G64_cond_timedwait),
    NDK_FN64("pthread_cond_timedwait_monotonic", G64_cond_timedwait_monotonic),
    NDK_FN64("pthread_cond_timedwait_monotonic_np", G64_cond_timedwait_monotonic),
    NDK_FN64("pthread_cond_timedwait_relative_np", G64_cond_timedwait_relative_np),
    NDK_FN64("sem_timedwait", G64_sem_timedwait), NDK_FN64("pthread_once", G64_once),
    NDK_FN64("pthread_key_create", G64_key_create), NDK_FN64("pthread_key_delete", G64_key_delete),
    NDK_FN64("pthread_getspecific", G64_getspecific), NDK_FN64("pthread_setspecific", G64_setspecific),
    NDK_FN64("pthread_attr_init", G64_attr_init), NDK_FN64("pthread_attr_destroy", G64_attr_destroy),
    NDK_FN64("pthread_attr_setdetachstate", G64_attr_setdetachstate),
    NDK_FN64("pthread_attr_getdetachstate", G64_attr_getdetachstate),
    NDK_FN64("pthread_attr_setstacksize", G64_attr_setstacksize),
    NDK_FN64("pthread_attr_getstacksize", G64_attr_getstacksize),
    NDK_FN64("pthread_attr_setschedpolicy", G64_attr_setschedpolicy),
    NDK_FN64("pthread_attr_getschedpolicy", G64_attr_getschedpolicy),
    NDK_FN64("pthread_attr_setschedparam", G64_attr_setschedparam),
    NDK_FN64("pthread_attr_getschedparam", G64_attr_getschedparam),
    NDK_FN64("pthread_attr_setscope", G64_attr_setscope), NDK_FN64("pthread_attr_getscope", G64_attr_getscope),
    NDK_FN64("pthread_attr_setguardsize", G64_attr_setguardsize),
    NDK_FN64("pthread_attr_getguardsize", G64_attr_getguardsize),
    NDK_FN64("pthread_attr_getstack", G64_attr_getstack), NDK_FN64("pthread_attr_setstack", G64_attr_setstack),
    NDK_FN64("pthread_attr_getstackaddr", G64_attr_getstackaddr),
    NDK_FN64("pthread_create", G64_create), NDK_FN64("pthread_join", G64_join),
    NDK_FN64("pthread_exit", G64_exit), NDK_FN64("pthread_getattr_np", G64_getattr_np),
    NDK_FN64("pthread_getcpuclockid", G64_getcpuclockid),
};

const HostSymbol kPthread[] = {
    NDK_FNX("pthread_mutexattr_init", G_mutexattr_init),
    NDK_FNX("pthread_mutexattr_destroy", G_mutexattr_destroy),
    NDK_FNX("pthread_mutexattr_settype", G_mutexattr_settype),
    NDK_FNX("pthread_mutexattr_gettype", G_mutexattr_gettype),
    NDK_FNX("pthread_mutexattr_setpshared", G_mutexattr_setpshared),
    NDK_FNX("pthread_mutexattr_getpshared", G_mutexattr_getpshared),
    NDK_FNX("pthread_mutex_init", G_mutex_init), NDK_FNX("pthread_mutex_destroy", G_mutex_destroy),
    NDK_FNX("pthread_mutex_lock", G_mutex_lock), NDK_FNX("pthread_mutex_trylock", G_mutex_trylock),
    NDK_FNX("pthread_mutex_unlock", G_mutex_unlock),
    NDK_FNX("pthread_condattr_init", G_condattr_init),
    NDK_FNX("pthread_condattr_destroy", G_condattr_destroy),
    NDK_FNX("pthread_condattr_setpshared", G_condattr_setpshared),
    NDK_FNX("pthread_condattr_getpshared", G_condattr_getpshared),
    NDK_FNX("pthread_condattr_setclock", G_condattr_setclock),
    NDK_FNX("pthread_condattr_getclock", G_condattr_getclock),
    NDK_FNX("pthread_cond_init", G_cond_init), NDK_FNX("pthread_cond_destroy", G_cond_destroy),
    NDK_FNX("pthread_cond_signal", G_cond_signal), NDK_FNX("pthread_cond_broadcast", G_cond_broadcast),
    NDK_FNX("pthread_cond_wait", G_cond_wait), NDK_FN("pthread_cond_timedwait", G_cond_timedwait),
    NDK_FN("pthread_cond_timedwait_monotonic", G_cond_timedwait_monotonic),
    NDK_FN("pthread_cond_timedwait_monotonic_np", G_cond_timedwait_monotonic),
    NDK_FN("pthread_cond_timedwait_relative_np", G_cond_timedwait_relative_np),
    NDK_FNX("pthread_cond_timeout_np", G_cond_timeout_np),
    NDK_FNX("pthread_rwlock_init", G_rwlock_init), NDK_FNX("pthread_rwlock_destroy", G_rwlock_destroy),
    NDK_FNX("pthread_rwlock_rdlock", G_rwlock_rdlock),
    NDK_FNX("pthread_rwlock_tryrdlock", G_rwlock_tryrdlock),
    NDK_FNX("pthread_rwlock_wrlock", G_rwlock_wrlock),
    NDK_FNX("pthread_rwlock_trywrlock", G_rwlock_trywrlock),
    NDK_FNX("pthread_rwlock_unlock", G_rwlock_unlock),
    NDK_FNX("pthread_rwlockattr_init", G_rwlockattr_init),
    NDK_FNX("pthread_rwlockattr_destroy", G_rwlockattr_destroy),
    NDK_FNX("pthread_rwlockattr_setpshared", G_rwlockattr_setpshared),
    NDK_FNX("pthread_rwlockattr_getpshared", G_rwlockattr_getpshared),
    NDK_FN("pthread_once", G_once),
    NDK_FNX("sem_init", G_sem_init), NDK_FNX("sem_destroy", G_sem_destroy),
    NDK_FNX("sem_post", G_sem_post), NDK_FNX("sem_wait", G_sem_wait),
    NDK_FNX("sem_trywait", G_sem_trywait), NDK_FN("sem_timedwait", G_sem_timedwait),
    NDK_FNX("sem_getvalue", G_sem_getvalue),
    NDK_FN("pthread_key_create", G_key_create), NDK_FN("pthread_key_delete", G_key_delete),
    NDK_FN("pthread_getspecific", G_getspecific), NDK_FN("pthread_setspecific", G_setspecific),
    NDK_FN("pthread_attr_init", G_attr_init), NDK_FN("pthread_attr_destroy", G_attr_destroy),
    NDK_FN("pthread_attr_setdetachstate", G_attr_setdetachstate),
    NDK_FN("pthread_attr_getdetachstate", G_attr_getdetachstate),
    NDK_FN("pthread_attr_setstacksize", G_attr_setstacksize),
    NDK_FN("pthread_attr_getstacksize", G_attr_getstacksize),
    NDK_FN("pthread_attr_setschedpolicy", G_attr_setschedpolicy),
    NDK_FN("pthread_attr_getschedpolicy", G_attr_getschedpolicy),
    NDK_FN("pthread_attr_setschedparam", G_attr_setschedparam),
    NDK_FN("pthread_attr_getschedparam", G_attr_getschedparam),
    NDK_FN("pthread_attr_setscope", G_attr_setscope), NDK_FN("pthread_attr_getscope", G_attr_getscope),
    NDK_FN("pthread_attr_setguardsize", G_attr_setguardsize),
    NDK_FN("pthread_attr_getguardsize", G_attr_getguardsize),
    NDK_FN("pthread_create", G_create), NDK_FN("pthread_join", G_join),
    NDK_FNX("pthread_detach", G_detach), NDK_FNX("pthread_self", G_self),
    NDK_FNX("pthread_equal", G_equal), NDK_FN("pthread_exit", G_exit),
    NDK_FNX("pthread_kill", G_kill), NDK_FN("pthread_sigmask", G_sigmask),
    NDK_FNX("pthread_getschedparam", G_getschedparam),
    NDK_FNX("pthread_setschedparam", G_setschedparam),
    NDK_FNX("pthread_setname_np", G_setname_np),
    NDK_FN("pthread_getcpuclockid", G_getcpuclockid),
    NDK_FN("pthread_getattr_np", G_getattr_np), NDK_FN("pthread_attr_getstack", G_attr_getstack),
    NDK_FN("pthread_attr_setstack", G_attr_setstack),
    NDK_FN("pthread_attr_getstackaddr", G_attr_getstackaddr),
    NDK_FNX("pthread_gettid_np", G_gettid_np), NDK_FNX("pthread_atfork", G_atfork),
    NDK_FNX("__register_atfork", G_register_atfork),
};

}  // namespace

void RegisterPthread() {
  pthread_key_create(&g_exit_key, RunGuestDestructors);
  pthread_key_create(&g_forget_key, ForgetSelf);
  RegisterHostSymbols("libc.so", kPthread);
}

void RegisterPthread64() {
  pthread_key_create(&g_exit_key64, RunGuestDestructors64);
  RegisterHostSymbols("libc.so", kPthread64);
}

}  // namespace ndk
