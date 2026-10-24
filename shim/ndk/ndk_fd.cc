// File descriptors for guest code beyond Android 4.4's set: the *at()
// family (the kernel layer resolves only AT_FDCWD, so directory fds are
// mapped to the paths they were opened with), eventfd and timerfd
// (emulated on a pipe: the counter lives here, the pipe carries readiness
// for poll/epoll), POSIX timers, epoll, statfs/statvfs, scandir and fts,
// 64-bit offsets, mremap, sendfile and the rest. Part of the arc-wasm
// shim layer.

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "ndk_bridge.h"

// The AArch64 CPU is only in builds that run arm64 code (topic ndk-arm64).
#pragma weak ndk64_flush_code

namespace ndk {
template <> struct Opaque64<struct epoll_event> : std::true_type {};   // 16 bytes on both sides
}  // namespace ndk

extern "C" int __android_log_print(int prio, const char* tag, const char* fmt, ...);

namespace ndk {
namespace {

const int kAtFdcwd = -100;
const int kAtSymlinkNofollow = 0x100, kAtRemovedir = 0x200, kAtEmptyPath = 0x1000;

struct GTimespec {
  int32_t tv_sec, tv_nsec;
};
struct GItimerspec {
  GTimespec it_interval, it_value;
};

int64_t Ns(const GTimespec& t) { return t.tv_sec * 1000000000LL + t.tv_nsec; }
GTimespec FromNs(int64_t ns) {
  if (ns < 0) ns = 0;
  return {(int32_t)(ns / 1000000000), (int32_t)(ns % 1000000000)};
}
int64_t NowNs(clockid_t clk) {
  struct timespec ts;
  clock_gettime(clk, &ts);
  return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

// --- the paths of open descriptors (for *at) -------------------------------------------

std::mutex g_path_lock;
std::map<int, std::string>* g_paths = new std::map<int, std::string>;

std::string Absolute(const char* path) {
  if (path[0] == '/')
    return path;
  char cwd[PATH_MAX];
  if (!getcwd(cwd, sizeof cwd))
    return path;
  std::string r = cwd;
  if (r.empty() || r.back() != '/')
    r += '/';
  return r + path;
}

// The path *at(dirfd, path) means, or false with errno set.
bool AtPath(int dirfd, const char* path, std::string* out) {
  if (!path) {
    errno = EFAULT;
    return false;
  }
  if (path[0] == '/' || dirfd == kAtFdcwd) {
    *out = path;
    return true;
  }
  std::lock_guard<std::mutex> l(g_path_lock);
  auto it = g_paths->find(dirfd);
  if (it == g_paths->end()) {
    __android_log_print(5, "ndk", "*at(%d, \"%s\"): the descriptor's path is unknown", dirfd, path);
    errno = EBADF;
    return false;
  }
  *out = it->second;
  if (*path) {
    if (out->empty() || out->back() != '/')
      *out += '/';
    *out += path;
  }
  return true;
}

// --- eventfd and timerfd: a pipe for readiness, the value here ---------------------------

struct EmuFd {
  enum Kind { kEvent, kTimer, kInotify } kind;
  int wfd;               // the pipe's write end (the guest holds the read end)
  std::mutex m;
  uint64_t counter = 0;  // eventfd value / timer expirations
  bool semaphore = false;
  // timerfd
  clockid_t clock = CLOCK_MONOTONIC;
  int64_t next_ns = 0, interval_ns = 0;  // 0: disarmed
  uint64_t generation = 0;               // bumped by every settime
  // inotify: the watches by absolute path (descriptor, mask) and the events
  // not read yet, whole struct inotify_event each; the counter is their number
  struct Watch {
    int wd;
    uint32_t mask;
  };
  std::map<std::string, Watch> watches;
  int next_wd = 1;
  std::vector<std::string> events;
};
std::mutex g_emu_lock;
std::map<int, EmuFd*>* g_emu = new std::map<int, EmuFd*>;
std::atomic<int> g_emu_count{0};

EmuFd* Emu(int fd) {
  if (g_emu_count.load(std::memory_order_relaxed) == 0)
    return nullptr;
  std::lock_guard<std::mutex> l(g_emu_lock);
  auto it = g_emu->find(fd);
  return it == g_emu->end() ? nullptr : it->second;
}

// Called with e->m held: makes the read end readable iff the counter is set.
void SyncReadiness(int rfd, EmuFd* e, uint64_t before) {
  char b = 1;
  if (before == 0 && e->counter > 0) {
    (void)!::write(e->wfd, &b, 1);
  } else if (before > 0 && e->counter == 0) {
    struct pollfd p = {rfd, POLLIN, 0};
    if (poll(&p, 1, 0) == 1)
      (void)!::read(rfd, &b, 1);
  }
}

int NewEmu(EmuFd::Kind kind, int flags, EmuFd** out) {
  int fds[2];
  if (pipe(fds) != 0)
    return -1;
  fcntl(fds[1], F_SETFL, O_NONBLOCK);
  if (flags & 04000)  // O_NONBLOCK (EFD_/TFD_NONBLOCK)
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
  if (flags & 02000000) {  // O_CLOEXEC
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
  }
  static std::atomic<uint64_t> generations{1};
  EmuFd* e = new EmuFd;
  e->kind = kind;
  // Distinct per object, so a timer entry for a closed fd that was reused
  // never matches.
  e->generation = generations.fetch_add(1 << 20);
  e->wfd = fds[1];
  {
    std::lock_guard<std::mutex> l(g_emu_lock);
    (*g_emu)[fds[0]] = e;
    g_emu_count++;
  }
  *out = e;
  return fds[0];
}

bool NonBlocking(int fd) { return (fcntl(fd, F_GETFL) & O_NONBLOCK) != 0; }

ssize_t InotifyRead(int fd, EmuFd* e, void* buf, size_t n) {
  for (;;) {
    {
      std::lock_guard<std::mutex> l(e->m);
      if (!e->events.empty()) {
        if (e->events.front().size() > n) {  // as Linux: the buffer takes no event
          errno = EINVAL;
          return -1;
        }
        uint64_t before = e->counter;
        size_t done = 0, k = 0;
        for (; k < e->events.size() && done + e->events[k].size() <= n; k++) {
          memcpy(static_cast<char*>(buf) + done, e->events[k].data(), e->events[k].size());
          done += e->events[k].size();
        }
        e->events.erase(e->events.begin(), e->events.begin() + k);
        e->counter = e->events.size();
        SyncReadiness(fd, e, before);
        return done;
      }
    }
    if (NonBlocking(fd)) {
      errno = EAGAIN;
      return -1;
    }
    struct pollfd p = {fd, POLLIN, 0};
    if (GuestPoll(&p, 1, -1) < 0 && errno == EINTR)  // a guest signal's handler ran
      return -1;
  }
}

ssize_t EmuRead(int fd, EmuFd* e, void* buf, size_t n) {
  if (e->kind == EmuFd::kInotify)
    return InotifyRead(fd, e, buf, n);
  if (n < 8) {
    errno = EINVAL;
    return -1;
  }
  for (;;) {
    {
      std::lock_guard<std::mutex> l(e->m);
      if (e->counter > 0) {
        uint64_t before = e->counter, v;
        if (e->kind == EmuFd::kEvent && e->semaphore) {
          v = 1;
          e->counter--;
        } else {
          v = e->counter;
          e->counter = 0;
        }
        SyncReadiness(fd, e, before);
        memcpy(buf, &v, 8);
        return 8;
      }
    }
    if (NonBlocking(fd)) {
      errno = EAGAIN;
      return -1;
    }
    struct pollfd p = {fd, POLLIN, 0};
    if (GuestPoll(&p, 1, -1) < 0 && errno == EINTR)  // a guest signal's handler ran
      return -1;
  }
}

ssize_t EmuWrite(int fd, EmuFd* e, const void* buf, size_t n) {
  if (e->kind != EmuFd::kEvent || n < 8) {
    errno = EINVAL;
    return -1;
  }
  uint64_t v;
  memcpy(&v, buf, 8);
  if (v == ~0ull) {
    errno = EINVAL;
    return -1;
  }
  std::lock_guard<std::mutex> l(e->m);
  if (e->counter > ~0ull - 1 - v) {
    errno = EAGAIN;  // would overflow (a blocking writer would wait)
    return -1;
  }
  uint64_t before = e->counter;
  e->counter += v;
  SyncReadiness(fd, e, before);
  return 8;
}

// --- the timer thread (timerfd and POSIX timers) -------------------------------------------

struct PosixTimer;
struct TimerEntry {
  int64_t when;      // on CLOCK_MONOTONIC
  int fd;            // a timerfd, or -1
  PosixTimer* timer;  // or a POSIX timer
  uint64_t generation;
  // The generation is part of the key: a timer re-armed for the deadline it
  // already has (Flutter's message loops re-arm their timerfd for the
  // earliest task at every post) must get an entry of its own, or the old
  // one, now stale, would fire for nothing and the timer never would.
  bool operator<(const TimerEntry& o) const {
    if (when != o.when) return when < o.when;
    if (fd != o.fd) return fd < o.fd;
    if (timer != o.timer) return timer < o.timer;
    return generation < o.generation;
  }
};
std::mutex g_timer_lock;
std::condition_variable g_timer_cv;
std::set<TimerEntry>* g_timers = new std::set<TimerEntry>;
bool g_timer_thread_started;

void TimerFired(const TimerEntry& t);

void TimerThread() {
  std::unique_lock<std::mutex> l(g_timer_lock);
  for (;;) {
    if (g_timers->empty()) {
      g_timer_cv.wait(l);
      continue;
    }
    TimerEntry first = *g_timers->begin();
    int64_t now = NowNs(CLOCK_MONOTONIC);
    if (first.when > now) {
      g_timer_cv.wait_for(l, std::chrono::nanoseconds(first.when - now));
      continue;
    }
    g_timers->erase(g_timers->begin());
    l.unlock();
    TimerFired(first);
    l.lock();
  }
}

void Schedule(const TimerEntry& t) {
  std::lock_guard<std::mutex> l(g_timer_lock);
  if (!g_timer_thread_started) {
    g_timer_thread_started = true;
    std::thread(TimerThread).detach();
  }
  g_timers->insert(t);
  g_timer_cv.notify_all();
}

// A deadline on |clock| as CLOCK_MONOTONIC.
int64_t ToMonotonic(clockid_t clock, int64_t abs) {
  if (clock != CLOCK_REALTIME)
    return abs;
  return abs - NowNs(clock) + NowNs(CLOCK_MONOTONIC);
}

// POSIX timers: SIGEV_THREAD runs the guest function on the timer's own
// thread; signals cannot be delivered, so SIGEV_SIGNAL/THREAD_ID timers
// only count overruns.
struct PosixTimer {
  clockid_t clock;
  int notify;  // SIGEV_SIGNAL 0, SIGEV_NONE 1, SIGEV_THREAD 2, SIGEV_THREAD_ID 4
  uint32_t fn, value;
  std::mutex m;
  std::condition_variable cv;
  int64_t next_ns = 0, interval_ns = 0;
  uint64_t generation = 0;
  int pending = 0, overrun = 0;
  bool deleted = false, thread_started = false;
};

void TimerCallbackThread(PosixTimer* t) {
  std::unique_lock<std::mutex> l(t->m);
  for (;;) {
    t->cv.wait(l, [t] { return t->pending > 0 || t->deleted; });
    if (t->deleted)
      break;
    t->overrun = t->pending - 1;
    t->pending = 0;
    uint32_t fn = t->fn, value = t->value;
    l.unlock();
    CallGuest1(fn, value);
    l.lock();
  }
  l.unlock();
  delete t;
}

void TimerFired(const TimerEntry& te) {
  if (te.timer) {
    PosixTimer* t = te.timer;
    std::lock_guard<std::mutex> l(t->m);
    if (t->deleted || te.generation != t->generation)
      return;
    if (t->notify == 2) {
      t->pending++;
      t->cv.notify_all();
    } else {
      static bool reported;
      if (!reported) {
        reported = true;
        __android_log_print(5, "ndk", "POSIX timer expired: signals cannot be delivered");
      }
      t->overrun++;
    }
    if (t->interval_ns > 0) {
      t->next_ns += t->interval_ns;
      Schedule({t->next_ns, -1, t, t->generation});
    } else {
      t->next_ns = 0;
    }
    return;
  }
  EmuFd* e = Emu(te.fd);
  if (!e)
    return;
  std::lock_guard<std::mutex> l(e->m);
  if (te.generation != e->generation || e->next_ns == 0)
    return;
  uint64_t before = e->counter;
  int64_t now = NowNs(CLOCK_MONOTONIC);
  uint64_t n = 1;
  if (e->interval_ns > 0) {
    n += (uint64_t)((now - e->next_ns) / e->interval_ns);
    e->next_ns += (int64_t)n * e->interval_ns;
    Schedule({e->next_ns, te.fd, nullptr, e->generation});
  } else {
    e->next_ns = 0;
  }
  e->counter += n;
  SyncReadiness(te.fd, e, before);
}

// --- guest functions: descriptors --------------------------------------------------------

int G_eventfd(uint32_t initval, int flags) {
  EmuFd* e;
  int fd = NewEmu(EmuFd::kEvent, flags, &e);
  if (fd < 0)
    return -1;
  e->semaphore = flags & 1;  // EFD_SEMAPHORE
  std::lock_guard<std::mutex> l(e->m);
  e->counter = initval;
  SyncReadiness(fd, e, 0);
  return fd;
}
int G_eventfd_read(int fd, uint64_t* v) { return FdRead(fd, v, 8) == 8 ? 0 : -1; }

// inotify: the VFS (OPFS, the APK, memory) tells nobody of changes, so the
// events are the ones the bridge itself sees: IN_CLOSE_WRITE when a
// descriptor the app's native code opened for writing (open, fopen) is
// closed, on a watch of the file or of its directory. Writes from Java
// (libcore) and from other processes make none. Before, inotify_init1 failed
// with ENOSYS and Dart's Directory.watch (Saber) reported "Function not
// implemented"; then, with descriptors that never woke, Firebase Messaging's
// Terminate (it touches its storage file to wake its thread, then joins it)
// hung, and with it il2cpp's garbage collector and Subway Surfers on
// "Loading...".
int G_inotify_init1(int flags) {
  EmuFd* e;
  return NewEmu(EmuFd::kInotify, flags, &e);  // IN_NONBLOCK, IN_CLOEXEC: O_ values
}
int G_inotify_init() { return G_inotify_init1(0); }
int G_inotify_add_watch(int fd, const char* path, uint32_t mask) {
  EmuFd* e = Emu(fd);
  if (!e || e->kind != EmuFd::kInotify) {
    errno = e || fcntl(fd, F_GETFD) >= 0 ? EINVAL : EBADF;
    return -1;
  }
  if ((mask & 0xfff) == 0) {  // no event of IN_ALL_EVENTS asked
    errno = EINVAL;
    return -1;
  }
  struct stat st;
  int r = (mask & 0x02000000 /* IN_DONT_FOLLOW */) ? lstat(path, &st) : stat(path, &st);
  if (r != 0)
    return -1;
  if ((mask & 0x01000000 /* IN_ONLYDIR */) && !S_ISDIR(st.st_mode)) {
    errno = ENOTDIR;
    return -1;
  }
  std::lock_guard<std::mutex> l(e->m);
  auto it = e->watches.find(Absolute(path));
  if (it != e->watches.end()) {  // the same inode: its watch descriptor, as Linux
    it->second.mask = (mask & 0x20000000 /* IN_MASK_ADD */) ? it->second.mask | mask : mask;
    return it->second.wd;
  }
  int wd = e->next_wd++;
  e->watches[Absolute(path)] = {wd, mask};
  return wd;
}
int G_inotify_rm_watch(int fd, int wd) {
  EmuFd* e = Emu(fd);
  if (!e || e->kind != EmuFd::kInotify) {
    errno = e || fcntl(fd, F_GETFD) >= 0 ? EINVAL : EBADF;
    return -1;
  }
  std::lock_guard<std::mutex> l(e->m);
  for (auto it = e->watches.begin(); it != e->watches.end(); ++it) {
    if (it->second.wd == wd) {
      e->watches.erase(it);
      return 0;
    }
  }
  errno = EINVAL;
  return -1;
}
int G_eventfd_write(int fd, uint64_t v) { return FdWrite(fd, &v, 8) == 8 ? 0 : -1; }

int G_timerfd_create(int clock, int flags) {
  if (clock != CLOCK_REALTIME && clock != CLOCK_MONOTONIC && clock != CLOCK_BOOTTIME &&
      clock != 8 && clock != 9) {  // *_ALARM clocks
    errno = EINVAL;
    return -1;
  }
  EmuFd* e;
  int fd = NewEmu(EmuFd::kTimer, flags, &e);
  if (fd >= 0)  // (boot time = monotonic time here: nothing ever suspends)
    e->clock = (clock == CLOCK_REALTIME || clock == 8) ? CLOCK_REALTIME : CLOCK_MONOTONIC;
  return fd;
}
void TimerfdGet(EmuFd* e, GItimerspec* cur) {
  int64_t left = e->next_ns ? e->next_ns - NowNs(CLOCK_MONOTONIC) : 0;
  cur->it_value = e->next_ns ? FromNs(left > 0 ? left : 1) : FromNs(0);
  cur->it_interval = FromNs(e->interval_ns);
}
int G_timerfd_settime(int fd, int flags, const GItimerspec* nv, GItimerspec* old) {
  EmuFd* e = Emu(fd);
  if (!e || e->kind != EmuFd::kTimer) {
    errno = EINVAL;
    return -1;
  }
  std::lock_guard<std::mutex> l(e->m);
  if (old)
    TimerfdGet(e, old);
  e->generation++;
  uint64_t before = e->counter;
  e->counter = 0;
  SyncReadiness(fd, e, before);
  int64_t value = Ns(nv->it_value);
  e->interval_ns = Ns(nv->it_interval);
  if (value == 0) {
    e->next_ns = 0;
    return 0;
  }
  e->next_ns = (flags & 1) ? ToMonotonic(e->clock, value) : NowNs(CLOCK_MONOTONIC) + value;
  Schedule({e->next_ns, fd, nullptr, e->generation});
  return 0;
}
int G_timerfd_gettime(int fd, GItimerspec* cur) {
  EmuFd* e = Emu(fd);
  if (!e || e->kind != EmuFd::kTimer) {
    errno = EINVAL;
    return -1;
  }
  std::lock_guard<std::mutex> l(e->m);
  TimerfdGet(e, cur);
  return 0;
}

// bionic's struct sigevent (ARM): value, signo, notify, then the union
// (function and attributes for SIGEV_THREAD).
struct GSigevent {
  uint32_t value;
  int32_t signo, notify;
  uint32_t fn, attr;
};
int G_timer_create(int clock, const GSigevent* ev, uint32_t* out) {
  PosixTimer* t = new PosixTimer;
  t->clock = clock == CLOCK_REALTIME ? CLOCK_REALTIME : CLOCK_MONOTONIC;
  t->notify = ev ? ev->notify : 0;
  t->fn = ev ? ev->fn : 0;
  t->value = ev ? ev->value : 0;
  if (t->notify == 2 && !t->fn) {
    delete t;
    errno = EINVAL;
    return -1;
  }
  *out = Addr(t);
  return 0;
}
int G_timer_settime(uint32_t id, int flags, const GItimerspec* nv, GItimerspec* old) {
  PosixTimer* t = Ptr<PosixTimer>(id);
  std::unique_lock<std::mutex> l(t->m);
  if (old) {
    int64_t left = t->next_ns ? t->next_ns - NowNs(CLOCK_MONOTONIC) : 0;
    old->it_value = FromNs(t->next_ns ? (left > 0 ? left : 1) : 0);
    old->it_interval = FromNs(t->interval_ns);
  }
  t->generation++;
  int64_t value = Ns(nv->it_value);
  t->interval_ns = Ns(nv->it_interval);
  if (value == 0) {
    t->next_ns = 0;
    return 0;
  }
  if (t->notify == 2 && !t->thread_started) {
    t->thread_started = true;
    std::thread(TimerCallbackThread, t).detach();
  }
  t->next_ns = (flags & 1) ? ToMonotonic(t->clock, value) : NowNs(CLOCK_MONOTONIC) + value;
  uint64_t gen = t->generation;
  int64_t when = t->next_ns;
  l.unlock();
  Schedule({when, -1, t, gen});
  return 0;
}
int G_timer_gettime(uint32_t id, GItimerspec* cur) {
  PosixTimer* t = Ptr<PosixTimer>(id);
  std::lock_guard<std::mutex> l(t->m);
  int64_t left = t->next_ns ? t->next_ns - NowNs(CLOCK_MONOTONIC) : 0;
  cur->it_value = FromNs(t->next_ns ? (left > 0 ? left : 1) : 0);
  cur->it_interval = FromNs(t->interval_ns);
  return 0;
}
int G_timer_getoverrun(uint32_t id) {
  PosixTimer* t = Ptr<PosixTimer>(id);
  std::lock_guard<std::mutex> l(t->m);
  return t->overrun;
}
int G_timer_delete(uint32_t id) {
  PosixTimer* t = Ptr<PosixTimer>(id);
  std::unique_lock<std::mutex> l(t->m);
  t->generation++;
  t->deleted = true;
  if (t->thread_started) {
    t->cv.notify_all();  // the callback thread deletes it
  } else {
    l.unlock();
    // Entries still queued for it are skipped by generation, but they hold
    // the pointer: keep the object (a few bytes per deleted timer).
  }
  return 0;
}

// epoll: struct epoll_event is 16 bytes with the data at 8 on ARM EABI,
// as in wasm32 (only x86-64 packs it).
static_assert(sizeof(struct epoll_event) == 16 && offsetof(struct epoll_event, data) == 8,
              "struct epoll_event");
int G_epoll_create(int size) {
  if (size <= 0) {
    errno = EINVAL;
    return -1;
  }
  return epoll_create1(0);
}
int G_epoll_create1(int flags) { return epoll_create1(flags & 02000000 ? EPOLL_CLOEXEC : 0); }
int G_epoll_ctl(int ep, int op, int fd, struct epoll_event* ev) { return epoll_ctl(ep, op, fd, ev); }
// In slices that guest signals interrupt (EINTR), as poll.
struct EpollArgs {
  int ep;
  struct epoll_event* ev;
  int max;
};
int EpollSlice(void* p, int ms) {
  EpollArgs* a = static_cast<EpollArgs*>(p);
  return epoll_wait(a->ep, a->ev, a->max, ms);
}
int G_epoll_wait(int ep, struct epoll_event* ev, int max, int timeout) {
  EpollArgs a = {ep, ev, max};
  return SignalSlicedWait(timeout, EpollSlice, &a);
}
int G_epoll_pwait(int ep, struct epoll_event* ev, int max, int timeout, const void*) {
  return G_epoll_wait(ep, ev, max, timeout);
}

// --- *at ---------------------------------------------------------------------------------

void G_openat(NdkCpuState* s, void*) {
  RegArgs a(s);
  int dirfd = GetArg<int>(a);
  const char* path = GetArg<const char*>(a);
  int flags = GetArg<int>(a);
  int mode = GetArg<int>(a);
  ThunkErrnoBegin();
  std::string p;
  int r = AtPath(dirfd, path, &p) ? FdOpen(p.c_str(), flags, mode) : -1;
  ThunkErrnoEnd();
  SetResult(s, r);
}
int G_open_2(const char* path, int flags) {
  if (flags & 0100)  // O_CREAT needs a mode
    FortifyFail("open: called with O_CREAT, but missing mode");
  return FdOpen(path, flags, 0);
}
int G_openat_2(int dirfd, const char* path, int flags) {
  if (flags & 0100)
    FortifyFail("openat: called with O_CREAT, but missing mode");
  std::string p;
  return AtPath(dirfd, path, &p) ? FdOpen(p.c_str(), flags, 0) : -1;
}
int G_creat(const char* path, uint32_t mode) {
  return FdOpen(path, 01 | 0100 | 01000, mode);  // O_WRONLY | O_CREAT | O_TRUNC
}
int G_fstatat(int dirfd, const char* path, void* st, int flags) {
  struct stat h;
  int r;
  if ((flags & kAtEmptyPath) && path && !*path) {
    r = fstat(dirfd, &h);
  } else {
    std::string p;
    if (!AtPath(dirfd, path, &p))
      return -1;
    r = (flags & kAtSymlinkNofollow) ? lstat(p.c_str(), &h) : stat(p.c_str(), &h);
  }
  if (r == 0)
    ToGuestStatBuf(h, st);
  return r;
}
int G_faccessat(int dirfd, const char* path, int mode, int) {
  std::string p;
  return AtPath(dirfd, path, &p) ? access(p.c_str(), mode) : -1;
}
int G_mkdirat(int dirfd, const char* path, uint32_t mode) {
  std::string p;
  return AtPath(dirfd, path, &p) ? mkdir(p.c_str(), mode) : -1;
}
int G_unlinkat(int dirfd, const char* path, int flags) {
  std::string p;
  if (!AtPath(dirfd, path, &p))
    return -1;
  return (flags & kAtRemovedir) ? rmdir(p.c_str()) : unlink(p.c_str());
}
int G_renameat(int od, const char* op, int nd, const char* np) {
  std::string a, b;
  return AtPath(od, op, &a) && AtPath(nd, np, &b) ? rename(a.c_str(), b.c_str()) : -1;
}
int G_linkat(int od, const char* op, int nd, const char* np, int) {
  std::string a, b;
  return AtPath(od, op, &a) && AtPath(nd, np, &b) ? link(a.c_str(), b.c_str()) : -1;
}
int G_symlinkat(const char* target, int nd, const char* np) {
  std::string b;
  return AtPath(nd, np, &b) ? symlink(target, b.c_str()) : -1;
}
ssize_t G_readlinkat(int dirfd, const char* path, char* buf, size_t n) {
  std::string p;
  return AtPath(dirfd, path, &p) ? readlink(p.c_str(), buf, n) : -1;
}
// Ownership and modes: the kernel layer keeps neither; succeed where the
// file exists (as for an app's own files).
int Exists(const char* p) {
  struct stat h;
  return stat(p, &h);
}
int G_fchmodat(int dirfd, const char* path, uint32_t mode, int) {
  std::string p;
  if (!AtPath(dirfd, path, &p))
    return -1;
  if (chmod(p.c_str(), mode) == 0)
    return 0;
  return errno == ENOENT ? -1 : Exists(p.c_str());
}
int G_fchownat(int dirfd, const char* path, uint32_t, uint32_t, int) {
  std::string p;
  return AtPath(dirfd, path, &p) ? Exists(p.c_str()) : -1;
}
int G_fchown(int fd, uint32_t, uint32_t) {
  struct stat h;
  return fstat(fd, &h);
}
int G_lchown(const char* p, uint32_t, uint32_t) {
  struct stat h;
  return lstat(p, &h);
}
int G_chown(const char* p, uint32_t, uint32_t) { return Exists(p); }
// Timestamps are not kept either: accept for existing files.
int G_utimensat(int dirfd, const char* path, const void*, int) {
  if (!path)
    return 0;  // futimens form
  std::string p;
  return AtPath(dirfd, path, &p) ? Exists(p.c_str()) : -1;
}
int G_futimens(int fd, const void*) {
  struct stat h;
  return fstat(fd, &h);
}
int G_utimes(const char* p, const void*) { return Exists(p); }
int G_lutimes(const char* p, const void*) {
  struct stat h;
  return lstat(p, &h);
}
int G_futimes(int fd, const void*) {
  struct stat h;
  return fstat(fd, &h);
}
int G_fchdir(int fd) {
  std::string p;
  {
    std::lock_guard<std::mutex> l(g_path_lock);
    auto it = g_paths->find(fd);
    if (it == g_paths->end()) {
      errno = EBADF;
      return -1;
    }
    p = it->second;
  }
  return chdir(p.c_str());
}
// --- FIFOs: named pipes kept by path, in memory -------------------------------------------
// Realm (Element) wakes its notifier threads through a FIFO next to the
// database ("x.realm.note"): every open() of that path must reach one pipe.
// The kernel layer has no FIFO nodes, so the bridge keeps them: mkfifo
// registers the path (nothing on disk; the FIFO lives as long as the VM, as a
// FIFO's buffer does, and it is the same for the app's light processes);
// open() of it hands out a dup of the pipe's read end (O_RDONLY, O_RDWR) or of
// its write end (O_WRONLY), and write() to an O_RDWR descriptor goes to the
// write end. The write end is non-blocking: a full FIFO answers EAGAIN.
// The registry holds the write end too, so a reader blocks in read() (where
// Linux blocks it in open()) until a writer comes. When the last writer
// (O_WRONLY or O_RDWR) closes, the registry lets go of its write end as well:
// the readers get end of file, as on Linux, and the path gets a fresh pipe
// for the next opens. Firebase Messaging wakes its thread that way (open for
// writing, close) and joins it on Terminate; without the end of file the join
// never returned and Subway Surfers stood on "Loading...".
// stat() does not see FIFOs; unlink() leaves them (a stale wake-up at most).
struct Fifo {
  int rfd, wfd;
  int writers;
};
std::mutex g_fifo_lock;
std::map<std::string, Fifo>* g_fifos = new std::map<std::string, Fifo>;
struct FifoWriter {
  std::string path;
  int wfd;  // the write end write() takes for an O_RDWR descriptor, else -1
};
std::map<int, FifoWriter>* g_fifo_writers = new std::map<int, FifoWriter>;  // writer descriptors
std::atomic<int> g_fifo_count{0}, g_fifo_writer_count{0};

bool FifoPipe(Fifo* f) {
  int fds[2];
  if (pipe(fds) != 0)
    return false;
  fcntl(fds[0], F_SETFD, FD_CLOEXEC);
  fcntl(fds[1], F_SETFD, FD_CLOEXEC);
  fcntl(fds[1], F_SETFL, O_NONBLOCK);
  *f = {fds[0], fds[1], 0};
  return true;
}

int G_mkfifo(const char* path, uint32_t) {
  if (!path) {
    errno = EFAULT;
    return -1;
  }
  std::string p = Absolute(path);
  struct stat h;
  std::lock_guard<std::mutex> l(g_fifo_lock);
  if (g_fifos->count(p) || stat(p.c_str(), &h) == 0) {
    errno = EEXIST;
    return -1;
  }
  size_t slash = p.rfind('/');
  std::string dir = slash == 0 ? "/" : p.substr(0, slash);
  if (stat(dir.c_str(), &h) != 0)
    return -1;
  if (!S_ISDIR(h.st_mode)) {
    errno = ENOTDIR;
    return -1;
  }
  Fifo f;
  if (!FifoPipe(&f))
    return -1;
  (*g_fifos)[p] = f;
  g_fifo_count++;
  return 0;
}
// No device nodes in the app's data: as SELinux on a device.
int G_mknod(const char* path, uint32_t mode, uint32_t) {
  if ((mode & S_IFMT) == S_IFIFO)
    return G_mkfifo(path, mode);
  errno = EPERM;
  return -1;
}

// open() of a registered FIFO: true with *out the descriptor (or -1 and errno).
bool FifoOpen(const char* path, int guest_flags, int* out) {
  if (g_fifo_count.load(std::memory_order_relaxed) == 0 || !path)
    return false;
  std::string p = Absolute(path);
  std::lock_guard<std::mutex> l(g_fifo_lock);
  auto it = g_fifos->find(p);
  if (it == g_fifos->end())
    return false;
  if ((guest_flags & 0300) == 0300) {  // O_CREAT | O_EXCL
    errno = EEXIST;
    *out = -1;
    return true;
  }
  Fifo& f = it->second;
  if (f.wfd < 0 && !FifoPipe(&f)) {  // the last writer closed: a fresh pipe
    *out = -1;
    return true;
  }
  int access = guest_flags & 3;
  int fd = dup(access == 1 ? f.wfd : f.rfd);  // 1: O_WRONLY
  if (fd < 0) {
    *out = -1;
    return true;
  }
  if (guest_flags & 04000)  // O_NONBLOCK
    fcntl(fd, F_SETFL, O_NONBLOCK);
  if (guest_flags & 02000000)  // O_CLOEXEC (a dup has it clear)
    fcntl(fd, F_SETFD, FD_CLOEXEC);
  if (access == 1 || access == 2) {  // O_WRONLY, O_RDWR
    (*g_fifo_writers)[fd] = {p, access == 2 ? f.wfd : -1};
    g_fifo_writer_count++;
    f.writers++;
  }
  *out = fd;
  return true;
}

// The write end behind an O_RDWR FIFO descriptor, or -1.
int FifoWriteEnd(int fd) {
  if (g_fifo_writer_count.load(std::memory_order_relaxed) == 0)
    return -1;
  std::lock_guard<std::mutex> l(g_fifo_lock);
  auto it = g_fifo_writers->find(fd);
  return it == g_fifo_writers->end() ? -1 : it->second.wfd;
}

void FifoClose(int fd) {
  if (g_fifo_writer_count.load(std::memory_order_relaxed) == 0)
    return;
  std::lock_guard<std::mutex> l(g_fifo_lock);
  auto it = g_fifo_writers->find(fd);
  if (it == g_fifo_writers->end())
    return;
  auto f = g_fifos->find(it->second.path);
  g_fifo_writers->erase(it);
  g_fifo_writer_count--;
  if (f != g_fifos->end() && --f->second.writers == 0 && f->second.wfd >= 0) {
    // The readers' dups of the read end see end of file once this
    // descriptor closes too; the next open makes a new pipe.
    close(f->second.wfd);
    close(f->second.rfd);
    f->second.wfd = f->second.rfd = -1;
  }
}
// realpath without /proc: walks the components, following symlinks.
char* G_realpath(const char* path, char* resolved) {
  if (!path) {
    errno = EINVAL;
    return nullptr;
  }
  std::string todo = Absolute(path), out;
  int links = 0;
  size_t i = 0;
  while (i < todo.size()) {
    while (i < todo.size() && todo[i] == '/') i++;
    size_t j = todo.find('/', i);
    if (j == std::string::npos) j = todo.size();
    std::string comp = todo.substr(i, j - i);
    i = j;
    if (comp.empty() || comp == ".")
      continue;
    if (comp == "..") {
      size_t k = out.rfind('/');
      out = k == std::string::npos ? "" : out.substr(0, k);
      continue;
    }
    std::string next = out + "/" + comp;
    struct stat h;
    if (lstat(next.c_str(), &h) != 0)
      return nullptr;
    if (S_ISLNK(h.st_mode)) {
      if (++links > 40) {
        errno = ELOOP;
        return nullptr;
      }
      char target[PATH_MAX];
      ssize_t n = readlink(next.c_str(), target, sizeof target - 1);
      if (n < 0)
        return nullptr;
      target[n] = 0;
      // Both forms give an absolute path to walk again from the root.
      todo = (target[0] == '/' ? std::string(target) : out + "/" + target) + todo.substr(i);
      out.clear();
      i = 0;
      continue;
    }
    if (i < todo.size() && !S_ISDIR(h.st_mode)) {
      errno = ENOTDIR;
      return nullptr;
    }
    out = next;
  }
  if (out.empty())
    out = "/";
  if (out.size() >= PATH_MAX) {
    errno = ENAMETOOLONG;
    return nullptr;
  }
  if (!resolved)
    resolved = static_cast<char*>(malloc(PATH_MAX));
  strcpy(resolved, out.c_str());
  return resolved;
}
char* G_tmpnam(char* buf) {
  static char own[4096];
  static std::atomic<unsigned> n{0};
  char* out = buf ? buf : own;
  const char* dir = getenv("TMPDIR");
  snprintf(out, 4096, "%s/tmp.%d.%u", dir ? dir : "/data/local/tmp", getpid(), n++);
  return out;
}

// --- sizes, offsets, memory ------------------------------------------------------------

int G_ftruncate64(int fd, int64_t len) { return ftruncate(fd, len); }
int G_truncate64(const char* p, int64_t len) { return truncate(p, len); }
ssize_t G_pread64(int fd, void* b, size_t n, int64_t off) { return pread(fd, b, n, off); }
ssize_t G_pwrite64(int fd, const void* b, size_t n, int64_t off) { return pwrite(fd, b, n, off); }
// posix_fadvise/posix_fallocate return the error number (not -1/errno).
int G_posix_fadvise(int fd, int32_t, int32_t, int) {
  struct stat h;
  return fstat(fd, &h) == 0 ? 0 : ToGuestErrno(errno);
}
int G_posix_fadvise64(int fd, int64_t, int64_t, int a) { return G_posix_fadvise(fd, 0, 0, a); }
int Fallocate(int fd, int64_t off, int64_t len) {
  struct stat h;
  if (fstat(fd, &h) != 0)
    return ToGuestErrno(errno);
  if (off + len > h.st_size && ftruncate(fd, off + len) != 0)
    return ToGuestErrno(errno);
  return 0;
}
int G_posix_fallocate(int fd, int32_t off, int32_t len) { return Fallocate(fd, off, len); }
int G_posix_fallocate64(int fd, int64_t off, int64_t len) { return Fallocate(fd, off, len); }
int G_fallocate(int fd, int mode, int32_t off, int32_t len) {
  int e = mode ? EOPNOTSUPP : Fallocate(fd, off, len);
  if (e) {
    errno = mode ? EOPNOTSUPP : ToHostErrno(e);
    return -1;
  }
  return 0;
}
void* G_mmap64(void* addr, size_t len, int prot, int flags, int fd, int64_t off) {
  return GuestMmap(addr, len, prot, flags, fd, off);
}
void* G_mmap2(void* addr, size_t len, int prot, int flags, int fd, uint32_t pgoff) {
  return GuestMmap(addr, len, prot, flags, fd, (int64_t)pgoff * 4096);
}
// mremap of a private mapping: in place when shrinking, else a new
// anonymous mapping with the contents (MREMAP_MAYMOVE), as there is no
// address space to grow into.
void G_mremap(NdkCpuState* s, void*) {
  uint32_t old_addr = s->r[0], old_size = s->r[1], new_size = s->r[2], flags = s->r[3];
  ThunkErrnoBegin();
  void* r;
  if (new_size <= old_size) {
    if (new_size < old_size) {
      uint32_t keep = (new_size + 4095) & ~4095u;
      if (keep < old_size)
        munmap(Ptr<void>(old_addr + keep), old_size - keep);
    }
    r = Ptr<void>(old_addr);
  } else if (!(flags & 1)) {  // MREMAP_MAYMOVE
    errno = ENOMEM;
    r = MAP_FAILED;
  } else {
    r = mmap(nullptr, new_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (r != MAP_FAILED) {
      memcpy(r, Ptr<void>(old_addr), old_size);
      ndk_flush_code(old_addr, old_size);
      munmap(Ptr<void>(old_addr), old_size);
    }
  }
  ThunkErrnoEnd();
  s->r[0] = r == MAP_FAILED ? 0xffffffffu : Addr(r);
}
int G_mincore(void* addr, size_t len, uint8_t* vec) {
  memset(vec, 1, (len + 4095) / 4096);  // everything is resident
  return 0;
}
int G_mlock(const void*, size_t) { return 0; }
int G_munlock(const void*, size_t) { return 0; }
int G_mlockall(int) { return 0; }
int G_munlockall() { return 0; }

// sendfile: read and write through a buffer.
ssize_t SendFile(int out, int in, int64_t* off, size_t count) {
  char buf[65536];
  size_t done = 0;
  while (done < count) {
    size_t want = std::min(sizeof buf, count - done);
    ssize_t n = off ? pread(in, buf, want, *off) : FdRead(in, buf, want);
    if (n < 0)
      return done ? (ssize_t)done : -1;
    if (n == 0)
      break;
    ssize_t w = FdWrite(out, buf, n);
    if (w < 0)
      return done ? (ssize_t)done : -1;
    if (off)
      *off += w;
    done += w;
    if (w < n)
      break;
  }
  return done;
}
ssize_t G_sendfile(int out, int in, int32_t* off, size_t count) {
  int64_t o = off ? *off : 0;
  ssize_t r = SendFile(out, in, off ? &o : nullptr, count);
  if (off)
    *off = (int32_t)o;
  return r;
}
ssize_t G_sendfile64(int out, int in, int64_t* off, size_t count) {
  return SendFile(out, in, off, count);
}

int G_pipe2(int* fds, int flags) {
  int h = 0;
  if (flags & 04000) h |= O_NONBLOCK;
  if (flags & 02000000) h |= O_CLOEXEC;
  return pipe2(fds, h);
}
int G_accept4(int fd, struct sockaddr* a, socklen_t* al, int flags) {
  int r = accept(fd, a, al);
  if (r >= 0) {
    if (flags & 04000)  // SOCK_NONBLOCK
      fcntl(r, F_SETFL, fcntl(r, F_GETFL) | O_NONBLOCK);
    if (flags & 02000000)  // SOCK_CLOEXEC
      fcntl(r, F_SETFD, FD_CLOEXEC);
  }
  return r;
}
// One process: advisory locks never conflict.
int G_flock(int fd, int) {
  struct stat h;
  return fstat(fd, &h);
}

}  // namespace

// eventfd, timerfd and epoll by ARM EABI system call number, for the raw
// syscall table (ndk_syscall.cc): Cronet's base library calls
// syscall(__NR_timerfd_create, ...) itself. false = not one of them.
int GuestAccept4(int fd, struct sockaddr* a, socklen_t* al, int flags) { return G_accept4(fd, a, al, flags); }

bool FdSyscall(uint32_t nr, const uint32_t* a, int* result) {
  switch (nr) {
    case 350: *result = G_timerfd_create(a[0], a[1]); return true;
    case 353:
      *result = G_timerfd_settime(a[0], a[1], reinterpret_cast<const GItimerspec*>(a[2]),
                                  reinterpret_cast<GItimerspec*>(a[3]));
      return true;
    case 354: *result = G_timerfd_gettime(a[0], reinterpret_cast<GItimerspec*>(a[1])); return true;
    case 316: *result = G_inotify_init(); return true;
    case 317:
      *result = G_inotify_add_watch(a[0], reinterpret_cast<const char*>(a[1]), a[2]);
      return true;
    case 318: *result = G_inotify_rm_watch(a[0], a[1]); return true;
    case 360: *result = G_inotify_init1(a[0]); return true;
    case 351: *result = G_eventfd(a[0], 0); return true;      // eventfd
    case 356: *result = G_eventfd(a[0], a[1]); return true;   // eventfd2
    case 250: *result = G_epoll_create(a[0]); return true;
    case 357: *result = G_epoll_create1(a[0]); return true;
    case 251:
      *result = G_epoll_ctl(a[0], a[1], a[2], reinterpret_cast<struct epoll_event*>(a[3]));
      return true;
    case 252:
      *result = G_epoll_wait(a[0], reinterpret_cast<struct epoll_event*>(a[1]), a[2], a[3]);
      return true;
    case 346:  // epoll_pwait
      *result = G_epoll_pwait(a[0], reinterpret_cast<struct epoll_event*>(a[1]), a[2], a[3],
                              reinterpret_cast<const void*>(a[4]));
      return true;
    default:
      return false;
  }
}

// fcntl record locks (F_GETLK/F_SETLK/F_SETLKW, their 64-bit and OFD forms)
// for G_fcntl and the raw syscall table. One process: a lock is always
// granted, F_GETLK reports none (l_type, the first short of struct flock
// and flock64 alike, = F_UNLCK). -2 = not a lock command.
int FcntlLock(int fd, int cmd, uint32_t arg) {
  switch (cmd) {
    case 5: case 12: case 36:  // F_GETLK, F_GETLK64, F_OFD_GETLK
      if (arg == 0) {
        errno = EFAULT;
        return -1;
      }
      *reinterpret_cast<int16_t*>(static_cast<uintptr_t>(arg)) = 2;  // F_UNLCK
      [[fallthrough]];
    case 6: case 7: case 13: case 14: case 37: case 38: {  // F_SETLK(W)(64), F_OFD_SETLK(W)
      struct stat h;
      return fstat(fd, &h) < 0 ? -1 : 0;
    }
    default:
      return -2;
  }
}

namespace {
int G_mkstemp(char* t) { return mkstemp(t); }
int G_mkostemp(char* t, int flags) { return mkostemp(t, HostOpenFlags(flags)); }
char* G_mkdtemp(char* t) { return mkdtemp(t); }

// statfs/statvfs: the app's storage, as a 4 GB ext4 volume with 2 GB free.
struct GStatfs {
  uint32_t f_type, f_bsize;
  uint64_t f_blocks, f_bfree, f_bavail, f_files, f_ffree;
  int32_t f_fsid[2];
  uint32_t f_namelen, f_frsize, f_flags, f_spare[4];
};
static_assert(sizeof(GStatfs) == 88 && offsetof(GStatfs, f_blocks) == 8, "bionic struct statfs");
struct GStatvfs {
  uint32_t f_bsize, f_frsize, f_blocks, f_bfree, f_bavail, f_files, f_ffree, f_favail, f_fsid,
      f_flag, f_namemax;
};
void FillStatfs(GStatfs* s) {
  memset(s, 0, sizeof *s);
  s->f_type = 0xef53;  // EXT4_SUPER_MAGIC
  s->f_bsize = s->f_frsize = 4096;
  s->f_blocks = 1u << 20;
  s->f_bfree = s->f_bavail = 1u << 19;
  s->f_files = 1u << 18;
  s->f_ffree = 1u << 17;
  s->f_namelen = 255;
}
void FillStatvfs(GStatvfs* v) {
  GStatfs s;
  FillStatfs(&s);
  *v = {s.f_bsize, s.f_frsize, (uint32_t)s.f_blocks, (uint32_t)s.f_bfree, (uint32_t)s.f_bavail,
        (uint32_t)s.f_files, (uint32_t)s.f_ffree, (uint32_t)s.f_ffree, 0, 0, s.f_namelen};
}
int G_statfs(const char* p, GStatfs* s) {
  if (Exists(p) != 0)
    return -1;
  FillStatfs(s);
  return 0;
}
int G_fstatfs(int fd, GStatfs* s) {
  struct stat h;
  if (fstat(fd, &h) != 0)
    return -1;
  FillStatfs(s);
  return 0;
}
int G_statvfs(const char* p, GStatvfs* v) {
  if (Exists(p) != 0)
    return -1;
  FillStatvfs(v);
  return 0;
}
int G_fstatvfs(int fd, GStatvfs* v) {
  struct stat h;
  if (fstat(fd, &h) != 0)
    return -1;
  FillStatvfs(v);
  return 0;
}

// --- scandir/alphasort (struct dirent has the same layout on both sides) -----------------

int G_alphasort(const struct dirent** a, const struct dirent** b) {
  return strcmp((*a)->d_name, (*b)->d_name);
}
int G_versionsort(const struct dirent** a, const struct dirent** b) {
  return strverscmp((*a)->d_name, (*b)->d_name);
}
thread_local uint32_t t_scandir_cmp;
int ScandirCmp(const void* a, const void* b) {
  return (int32_t)CallGuest2(t_scandir_cmp, Addr(a), Addr(b));
}
int G_scandir(const char* dir, struct dirent*** list, uint32_t filter, uint32_t cmp) {
  DIR* d = opendir(dir);
  if (!d)
    return -1;
  std::vector<struct dirent*> v;
  while (struct dirent* e = readdir(d)) {
    if (filter && !(int32_t)CallGuest1(filter, Addr(e)))
      continue;
    struct dirent* c = static_cast<struct dirent*>(malloc(sizeof *e));
    memcpy(c, e, sizeof *e);
    v.push_back(c);
  }
  closedir(d);
  if (cmp) {
    uint32_t saved = t_scandir_cmp;
    t_scandir_cmp = cmp;
    qsort(v.data(), v.size(), sizeof v[0], ScandirCmp);
    t_scandir_cmp = saved;
  }
  struct dirent** out = static_cast<struct dirent**>(malloc(sizeof(void*) * (v.size() + 1)));
  memcpy(out, v.data(), sizeof(void*) * v.size());
  *list = out;
  return (int)v.size();
}

// --- fts (bionic's OpenBSD fts.h; physical walks, no chdir) ----------------------------------

struct GFtsent {
  uint32_t fts_cycle, fts_parent, fts_link;
  int32_t fts_number;
  uint32_t fts_pointer, fts_accpath, fts_path;
  int32_t fts_errno, fts_symfd;
  uint32_t fts_pathlen, fts_namelen;
  uint32_t fts_ino, fts_dev, fts_nlink;
  int16_t fts_level;
  uint16_t fts_info, fts_flags, fts_instr;
  uint32_t fts_statp;
  char fts_name[1];
};
static_assert(offsetof(GFtsent, fts_level) == 56 && offsetof(GFtsent, fts_statp) == 64 &&
                  offsetof(GFtsent, fts_name) == 68,
              "bionic FTSENT");
struct GFts {
  uint32_t fts_cur, fts_child, fts_array, fts_dev, fts_path;
  int32_t fts_rfd;
  uint32_t fts_pathlen;
  int32_t fts_nitems;
  uint32_t fts_compar;
  int32_t fts_options;
};
enum {
  FTS_D = 1, FTS_DC = 2, FTS_DEFAULT = 3, FTS_DNR = 4, FTS_DOT = 5, FTS_DP = 6, FTS_ERR = 7,
  FTS_F = 8, FTS_NS = 10, FTS_NSOK = 11, FTS_SL = 12, FTS_SLNONE = 13,
  FTS_AGAIN = 1, FTS_FOLLOW = 2, FTS_SKIP = 4,
  FTS_COMFOLLOW = 0x1, FTS_LOGICAL = 0x2, FTS_NOSTAT = 0x8, FTS_SEEDOT = 0x20,
};
// Our walk state behind the guest FTS.
struct FtsWalk {
  GFts fts;  // first: the guest's FTS* is this
  struct Frame {
    GFtsent* dir;                 // the directory being listed
    std::vector<GFtsent*> kids;   // its entries, in order
    size_t next;
  };
  std::vector<Frame> stack;
  std::vector<GFtsent*> roots;
  size_t next_root = 0;
  GFtsent* last = nullptr;  // returned by the last fts_read
  bool last_is_postorder = false;
  std::vector<GFtsent*> all;  // to free
};

GFtsent* NewEnt(FtsWalk* w, const std::string& path, const std::string& name, int level,
                GFtsent* parent) {
  GFtsent* e =
      static_cast<GFtsent*>(calloc(1, sizeof(GFtsent) + name.size() + 8 + 104 + path.size() + 1));
  memcpy(e->fts_name, name.c_str(), name.size() + 1);
  char* statbuf = reinterpret_cast<char*>(e) + sizeof(GFtsent) + name.size();
  statbuf = reinterpret_cast<char*>((reinterpret_cast<uintptr_t>(statbuf) + 7) & ~(uintptr_t)7);
  char* pathbuf = statbuf + 104;
  memcpy(pathbuf, path.c_str(), path.size() + 1);
  e->fts_statp = Addr(statbuf);
  e->fts_path = e->fts_accpath = Addr(pathbuf);
  e->fts_pathlen = path.size();
  e->fts_namelen = name.size();
  e->fts_level = level;
  e->fts_parent = Addr(parent);
  w->all.push_back(e);
  return e;
}

void StatEnt(FtsWalk* w, GFtsent* e, bool follow) {
  struct stat h;
  const char* p = Ptr<const char>(e->fts_path);
  int r = follow ? stat(p, &h) : lstat(p, &h);
  if (r != 0 && follow && lstat(p, &h) == 0) {
    e->fts_info = FTS_SLNONE;
    ToGuestStatBuf(h, Ptr<void>(e->fts_statp));
    return;
  }
  if (r != 0) {
    e->fts_info = FTS_NS;
    e->fts_errno = ToGuestErrno(errno);
    return;
  }
  ToGuestStatBuf(h, Ptr<void>(e->fts_statp));
  e->fts_ino = (uint32_t)h.st_ino;
  e->fts_dev = (uint32_t)h.st_dev;
  e->fts_nlink = h.st_nlink;
  if (S_ISDIR(h.st_mode)) {
    const char* n = e->fts_name;
    e->fts_info = (!strcmp(n, ".") || !strcmp(n, "..")) && e->fts_level > 0 ? FTS_DOT : FTS_D;
  } else if (S_ISLNK(h.st_mode)) {
    e->fts_info = FTS_SL;
  } else if (S_ISREG(h.st_mode)) {
    e->fts_info = FTS_F;
  } else {
    e->fts_info = FTS_DEFAULT;
  }
}

thread_local uint32_t t_fts_cmp;
int FtsCmp(const void* a, const void* b) {
  return (int32_t)CallGuest2(t_fts_cmp, Addr(a), Addr(b));
}
void ListDir(FtsWalk* w, GFtsent* dir) {
  FtsWalk::Frame f{dir, {}, 0};
  std::string base = Ptr<const char>(dir->fts_path);
  DIR* d = opendir(base.c_str());
  if (!d) {
    dir->fts_info = FTS_DNR;
    dir->fts_errno = ToGuestErrno(errno);
    return;
  }
  bool logical = w->fts.fts_options & FTS_LOGICAL;
  while (struct dirent* de = readdir(d)) {
    std::string n = de->d_name;
    if ((n == "." || n == "..") && !(w->fts.fts_options & FTS_SEEDOT))
      continue;
    std::string p = base + (base.back() == '/' ? "" : "/") + n;
    GFtsent* e = NewEnt(w, p, n, dir->fts_level + 1, dir);
    if (w->fts.fts_options & FTS_NOSTAT)
      e->fts_info = FTS_NSOK;
    else
      StatEnt(w, e, logical);
    f.kids.push_back(e);
  }
  closedir(d);
  if (w->fts.fts_compar && !f.kids.empty()) {
    uint32_t saved = t_fts_cmp;
    t_fts_cmp = w->fts.fts_compar;
    qsort(f.kids.data(), f.kids.size(), sizeof(GFtsent*), FtsCmp);  // (const FTSENT**, ...)
    t_fts_cmp = saved;
  }
  for (size_t i = 0; i + 1 < f.kids.size(); i++) f.kids[i]->fts_link = Addr(f.kids[i + 1]);
  w->stack.push_back(f);
}

GFts* G_fts_open(const uint32_t* argv, int options, uint32_t compar) {
  FtsWalk* w = new FtsWalk;
  memset(&w->fts, 0, sizeof w->fts);
  w->fts.fts_options = options;
  w->fts.fts_compar = compar;
  for (; *argv; argv++) {
    const char* p = Ptr<const char>(*argv);
    const char* slash = strrchr(p, '/');
    std::string name = slash && slash[1] ? slash + 1 : p;
    GFtsent* e = NewEnt(w, p, name, 0, nullptr);
    StatEnt(w, e, (options & (FTS_LOGICAL | FTS_COMFOLLOW)) != 0);
    w->roots.push_back(e);
  }
  return &w->fts;
}

GFtsent* G_fts_read(GFts* fts) {
  FtsWalk* w = reinterpret_cast<FtsWalk*>(fts);
  // What the caller asked about the entry returned last.
  if (w->last && !w->last_is_postorder) {
    GFtsent* e = w->last;
    int instr = e->fts_instr;
    e->fts_instr = 0;
    if (instr == FTS_AGAIN) {
      StatEnt(w, e, false);
      return e;
    }
    if (instr == FTS_FOLLOW && (e->fts_info == FTS_SL || e->fts_info == FTS_SLNONE)) {
      StatEnt(w, e, true);
      if (e->fts_info == FTS_D || e->fts_info == FTS_F)
        return e;
    }
    if (e->fts_info == FTS_D && instr != FTS_SKIP)
      ListDir(w, e);
    else if (e->fts_info == FTS_D)
      w->stack.push_back({e, {}, 0});  // skipped: only its postorder visit
  }
  for (;;) {
    if (!w->stack.empty()) {
      FtsWalk::Frame& f = w->stack.back();
      if (f.next < f.kids.size()) {
        GFtsent* e = f.kids[f.next++];
        w->last = e;
        w->last_is_postorder = false;
        fts->fts_cur = Addr(e);
        return e;
      }
      GFtsent* dir = f.dir;
      w->stack.pop_back();
      if (dir->fts_info == FTS_D)
        dir->fts_info = FTS_DP;
      w->last = dir;
      w->last_is_postorder = true;
      fts->fts_cur = Addr(dir);
      return dir;
    }
    if (w->next_root < w->roots.size()) {
      GFtsent* e = w->roots[w->next_root++];
      w->last = e;
      w->last_is_postorder = false;
      fts->fts_cur = Addr(e);
      return e;
    }
    errno = 0;
    w->last = nullptr;
    return nullptr;
  }
}
GFtsent* G_fts_children(GFts*, int) {
  errno = 0;
  return nullptr;  // not supported: callers fall back to fts_read
}
int G_fts_set(GFts*, GFtsent* e, int instr) {
  e->fts_instr = instr;
  return 0;
}
int G_fts_close(GFts* fts) {
  FtsWalk* w = reinterpret_cast<FtsWalk*>(fts);
  for (GFtsent* e : w->all) free(e);
  delete w;
  return 0;
}


// --- arm64-v8a guests (topic ndk-arm64) -----------------------------------------
// 64-bit offsets everywhere (posix_fadvise, posix_fallocate, fallocate,
// sendfile), struct timespec/itimerspec, statfs/statvfs with longs, and the
// dirent** of alphasort/scandir with guest callbacks on the AArch64 CPU.
int G64_fallocate(int fd, int, int64_t off, int64_t len) { return G_posix_fallocate64(fd, off, len); }
struct G64Itimerspec : GuestLayout64 {
  int64_t interval_sec, interval_nsec, value_sec, value_nsec;
};
GItimerspec To32(const G64Itimerspec* t) {
  GItimerspec r;
  r.it_interval = {static_cast<int32_t>(t->interval_sec), static_cast<int32_t>(t->interval_nsec)};
  r.it_value = {static_cast<int32_t>(t->value_sec), static_cast<int32_t>(t->value_nsec)};
  return r;
}
void From32(const GItimerspec& r, G64Itimerspec* t) {
  t->interval_sec = r.it_interval.tv_sec;
  t->interval_nsec = r.it_interval.tv_nsec;
  t->value_sec = r.it_value.tv_sec;
  t->value_nsec = r.it_value.tv_nsec;
}
int G64_timerfd_settime(int fd, int flags, const G64Itimerspec* nv, G64Itimerspec* old) {
  GItimerspec n = To32(nv), o;
  int r = G_timerfd_settime(fd, flags, &n, old ? &o : nullptr);
  if (r == 0 && old) From32(o, old);
  return r;
}
int G64_timerfd_gettime(int fd, G64Itimerspec* cur) {
  GItimerspec c;
  int r = G_timerfd_gettime(fd, &c);
  if (r == 0) From32(c, cur);
  return r;
}
void G64_mremap(NdkCpu64State* s, void*) {
  if ((s->x[0] | s->x[1] | s->x[2]) >> 32) {   // in a reservation's window (GuestMmap64), or nothing
    ThunkErrnoBegin();
    errno = EFAULT;
    ThunkErrnoEnd();
    s->x[0] = ~0ull;
    return;
  }
  uint32_t old_addr = static_cast<uint32_t>(s->x[0]), old_size = static_cast<uint32_t>(s->x[1]);
  uint32_t new_size = static_cast<uint32_t>(s->x[2]), flags = static_cast<uint32_t>(s->x[3]);
  ThunkErrnoBegin();
  void* r;
  if (new_size <= old_size) {
    if (new_size < old_size) {
      uint32_t keep = (new_size + 4095) & ~4095u;
      if (keep < old_size) munmap(Ptr<void>(old_addr + keep), old_size - keep);
    }
    r = Ptr<void>(old_addr);
  } else if (!(flags & 1)) {
    errno = ENOMEM;
    r = MAP_FAILED;
  } else {
    r = mmap(nullptr, new_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (r != MAP_FAILED) {
      memcpy(r, Ptr<void>(old_addr), old_size);
      if (ndk64_flush_code)
        ndk64_flush_code(old_addr, old_size);
      munmap(Ptr<void>(old_addr), old_size);
    }
  }
  ThunkErrnoEnd();
  s->x[0] = GuestPtr64(r);
}
// struct statfs (asm-generic, LP64): longs where the ARM one has 32 bits.
struct G64Statfs : GuestLayout64 {
  uint64_t f_type, f_bsize, f_blocks, f_bfree, f_bavail, f_files, f_ffree;
  int32_t f_fsid[2];
  uint64_t f_namelen, f_frsize, f_flags, f_spare[4];
};
void ToStatfs64(const GStatfs& s, G64Statfs* g) {
  memset(g, 0, sizeof *g);
  g->f_type = s.f_type;
  g->f_bsize = s.f_bsize;
  g->f_blocks = s.f_blocks;
  g->f_bfree = s.f_bfree;
  g->f_bavail = s.f_bavail;
  g->f_files = s.f_files;
  g->f_ffree = s.f_ffree;
  g->f_namelen = s.f_namelen;
  g->f_frsize = s.f_frsize;
}
int G64_statfs(const char* p, G64Statfs* g) {
  GStatfs s;
  if (G_statfs(p, &s) != 0) return -1;
  ToStatfs64(s, g);
  return 0;
}
int G64_fstatfs(int fd, G64Statfs* g) {
  GStatfs s;
  if (G_fstatfs(fd, &s) != 0) return -1;
  ToStatfs64(s, g);
  return 0;
}
// struct statvfs (bionic LP64): f_bsize, f_frsize (long); f_blocks..f_favail
// (64-bit); f_fsid, f_flag, f_namemax (long); 6 reserved ints.
struct G64Statvfs : GuestLayout64 {
  uint64_t f_bsize, f_frsize, f_blocks, f_bfree, f_bavail, f_files, f_ffree, f_favail, f_fsid, f_flag, f_namemax;
  uint32_t reserved[6];
};
void ToStatvfs64(G64Statvfs* g) {
  GStatfs s;
  FillStatfs(&s);
  memset(g, 0, sizeof *g);
  g->f_bsize = g->f_frsize = s.f_bsize;
  g->f_blocks = s.f_blocks;
  g->f_bfree = s.f_bfree;
  g->f_bavail = s.f_bavail;
  g->f_files = s.f_files;
  g->f_ffree = g->f_favail = s.f_ffree;
  g->f_namemax = s.f_namelen;
}
int G64_statvfs(const char* p, G64Statvfs* g) {
  if (Exists(p) != 0) return -1;
  ToStatvfs64(g);
  return 0;
}
int G64_fstatvfs(int fd, G64Statvfs* g) {
  struct stat h;
  if (fstat(fd, &h) != 0) return -1;
  ToStatvfs64(g);
  return 0;
}
int G64_alphasort(const uint64_t* a, const uint64_t* b) {
  return strcmp(Ptr<const struct dirent>(GuestAddr64(*a))->d_name,
                Ptr<const struct dirent>(GuestAddr64(*b))->d_name);
}
int G64_versionsort(const uint64_t* a, const uint64_t* b) {
  return strverscmp(Ptr<const struct dirent>(GuestAddr64(*a))->d_name,
                    Ptr<const struct dirent>(GuestAddr64(*b))->d_name);
}
int G64_scandir(const char* dir, uint64_t* list, uint64_t filter, uint64_t cmp) {
  DIR* d = opendir(dir);
  if (!d) return -1;
  std::vector<uint64_t> v;
  while (struct dirent* e = readdir(d)) {
    uint64_t arg = Addr(e);
    if (filter && !static_cast<int32_t>(CallGuest64(static_cast<uint32_t>(filter), &arg, 1))) continue;
    struct dirent* c = static_cast<struct dirent*>(malloc(sizeof *e));
    memcpy(c, e, sizeof *e);
    v.push_back(Addr(c));
  }
  closedir(d);
  if (cmp) {
    // The comparator gets pointers to the array's elements (const struct dirent**).
    std::sort(v.begin(), v.end(), [&](const uint64_t& x, const uint64_t& y) {
      uint64_t args[2] = {Addr(&x), Addr(&y)};
      return static_cast<int32_t>(CallGuest64(static_cast<uint32_t>(cmp), args, 2)) < 0;
    });
  }
  uint64_t* out = static_cast<uint64_t*>(malloc(8 * (v.size() + 1)));
  memcpy(out, v.data(), 8 * v.size());
  *list = Addr(out);
  return static_cast<int>(v.size());
}

const HostSymbol kFd64[] = {
    NDK_FN64("posix_fadvise", G_posix_fadvise64), NDK_FN64("posix_fallocate", G_posix_fallocate64),
    NDK_FN64("fallocate", G64_fallocate), NDK_FN64("fallocate64", G64_fallocate),
    NDK_FN64("sendfile", G_sendfile64),
    NDK_FN64("timerfd_settime", G64_timerfd_settime), NDK_FN64("timerfd_gettime", G64_timerfd_gettime),
    NDK_RAW64("mremap", G64_mremap),
    NDK_FN64("statfs", G64_statfs), NDK_FN64("statfs64", G64_statfs), NDK_FN64("fstatfs", G64_fstatfs),
    NDK_FN64("fstatfs64", G64_fstatfs), NDK_FN64("statvfs", G64_statvfs), NDK_FN64("statvfs64", G64_statvfs),
    NDK_FN64("fstatvfs", G64_fstatvfs), NDK_FN64("fstatvfs64", G64_fstatvfs),
    NDK_FN64("alphasort", G64_alphasort), NDK_FN64("alphasort64", G64_alphasort),
    NDK_FN64("versionsort", G64_versionsort), NDK_FN64("scandir", G64_scandir), NDK_FN64("scandir64", G64_scandir),
};

const HostSymbol kFd[] = {
    NDK_FNX("eventfd", G_eventfd), NDK_FNX("eventfd_read", G_eventfd_read),
    NDK_FNX("eventfd_write", G_eventfd_write), NDK_FNX("timerfd_create", G_timerfd_create),
    NDK_FNX("inotify_init1", G_inotify_init1), NDK_FNX("inotify_init", G_inotify_init),
    NDK_FNX("inotify_add_watch", G_inotify_add_watch),
    NDK_FNX("inotify_rm_watch", G_inotify_rm_watch),
    NDK_FN("timerfd_settime", G_timerfd_settime), NDK_FN("timerfd_gettime", G_timerfd_gettime),
    NDK_FN("timer_create", G_timer_create), NDK_FN("timer_settime", G_timer_settime),
    NDK_FN("timer_gettime", G_timer_gettime), NDK_FN("timer_getoverrun", G_timer_getoverrun),
    NDK_FN("timer_delete", G_timer_delete),
    NDK_FNX("epoll_create", G_epoll_create), NDK_FNX("epoll_create1", G_epoll_create1),
    NDK_FNX("epoll_ctl", G_epoll_ctl), NDK_FNX("epoll_wait", G_epoll_wait),
    NDK_FNX("epoll_pwait", G_epoll_pwait),
    NDK_RAW("openat", G_openat), NDK_RAW("openat64", G_openat), NDK_FNX("__open_2", G_open_2),
    NDK_FNX("__openat_2", G_openat_2), NDK_FNX("creat", G_creat), NDK_FNX("creat64", G_creat),
    NDK_FN("fstatat", G_fstatat), NDK_FN("fstatat64", G_fstatat),
    NDK_FNX("faccessat", G_faccessat), NDK_FNX("mkdirat", G_mkdirat),
    NDK_FNX("unlinkat", G_unlinkat), NDK_FNX("renameat", G_renameat), NDK_FNX("linkat", G_linkat),
    NDK_FNX("symlinkat", G_symlinkat), NDK_FNX("readlinkat", G_readlinkat),
    NDK_FNX("fchmodat", G_fchmodat), NDK_FNX("fchownat", G_fchownat), NDK_FNX("fchown", G_fchown),
    NDK_FNX("lchown", G_lchown), NDK_FNX("chown", G_chown), NDK_FN("utimensat", G_utimensat),
    NDK_FN("futimens", G_futimens), NDK_FN("utimes", G_utimes), NDK_FNX("lutimes", G_lutimes),
    NDK_FNX("futimes", G_futimes), NDK_FNX("fchdir", G_fchdir), NDK_FNX("mkfifo", G_mkfifo),
    NDK_FNX("mknod", G_mknod), NDK_FNX("realpath", G_realpath), NDK_FNX("tmpnam", G_tmpnam),
    NDK_FNX("ftruncate64", G_ftruncate64), NDK_FNX("truncate64", G_truncate64),
    NDK_FNX("pread64", G_pread64), NDK_FNX("pwrite64", G_pwrite64),
    NDK_FN("posix_fadvise", G_posix_fadvise), NDK_FNX("posix_fadvise64", G_posix_fadvise64),
    NDK_FN("posix_fallocate", G_posix_fallocate), NDK_FNX("posix_fallocate64", G_posix_fallocate64),
    NDK_FN("fallocate", G_fallocate),
    NDK_FNX("mmap64", G_mmap64), NDK_FN("__mmap2", G_mmap2), NDK_RAW("mremap", G_mremap),
    NDK_FNX("mincore", G_mincore), NDK_FNX("mlock", G_mlock), NDK_FNX("munlock", G_munlock),
    NDK_FNX("mlockall", G_mlockall), NDK_FNX("munlockall", G_munlockall),
    NDK_FN("sendfile", G_sendfile), NDK_FNX("sendfile64", G_sendfile64),
    NDK_FNX("pipe2", G_pipe2), NDK_FNX("accept4", G_accept4), NDK_FNX("flock", G_flock),
    NDK_FNX("mkstemp", G_mkstemp), NDK_FNX("mkstemp64", G_mkstemp), NDK_FNX("mkostemp", G_mkostemp),
    NDK_FNX("mkdtemp", G_mkdtemp),
    NDK_FN("statfs", G_statfs), NDK_FN("statfs64", G_statfs), NDK_FN("fstatfs", G_fstatfs),
    NDK_FN("fstatfs64", G_fstatfs), NDK_FN("statvfs", G_statvfs), NDK_FN("statvfs64", G_statvfs),
    NDK_FN("fstatvfs", G_fstatvfs), NDK_FN("fstatvfs64", G_fstatvfs),
    NDK_FN("alphasort", G_alphasort), NDK_FN("versionsort", G_versionsort),
    NDK_FN("scandir", G_scandir),
    NDK_FN("fts_open", G_fts_open), NDK_FN("fts_read", G_fts_read),
    NDK_FN("fts_children", G_fts_children), NDK_FN("fts_set", G_fts_set),
    NDK_FN("fts_close", G_fts_close),
};

// An event for every inotify watch on path or on its directory that asks for it.
void InotifyNotify(const std::string& path, uint32_t what) {
  size_t slash = path.rfind('/');
  std::string dir = slash == 0 ? "/" : path.substr(0, slash), name = path.substr(slash + 1);
  std::lock_guard<std::mutex> g(g_emu_lock);
  for (auto& [rfd, e] : *g_emu) {
    if (e->kind != EmuFd::kInotify)
      continue;
    std::lock_guard<std::mutex> l(e->m);
    uint64_t before = e->counter;
    for (const auto& [wpath, w] : e->watches) {
      bool self = wpath == path, in_dir = wpath == dir;
      if ((!self && !in_dir) || !(w.mask & what) || e->events.size() >= 4096)
        continue;
      // struct inotify_event: wd, mask, cookie, len, then the name padded with
      // zeros to a multiple of 16 bytes (only for a watched directory)
      uint32_t len = self ? 0 : (name.size() + 1 + 15) & ~15u;
      std::string ev(16 + len, '\0');
      int32_t head[4] = {w.wd, (int32_t)what, 0, (int32_t)len};
      memcpy(&ev[0], head, 16);
      if (!self)
        memcpy(&ev[16], name.data(), name.size());
      e->events.push_back(std::move(ev));
    }
    e->counter = e->events.size();
    SyncReadiness(rfd, e, before);
  }
}

std::set<int>* g_write_fds = new std::set<int>;  // opened for writing (under g_path_lock)

}  // namespace

// --- descriptors as the rest of the bridge sees them ---------------------------------------

void NoteFdWrite(int fd) {
  if (fd < 0)
    return;
  std::lock_guard<std::mutex> l(g_path_lock);
  g_write_fds->insert(fd);
}

// Forgets fd's path; IN_CLOSE_WRITE for its watchers when it was open for writing.
void FdForget(int fd) {
  std::string path;
  {
    std::lock_guard<std::mutex> l(g_path_lock);
    auto it = g_paths->find(fd);
    if (g_write_fds->erase(fd) && it != g_paths->end())
      path = it->second;
    if (it != g_paths->end())
      g_paths->erase(it);
  }
  if (!path.empty() && g_emu_count.load(std::memory_order_relaxed))
    InotifyNotify(path, 0x8 /* IN_CLOSE_WRITE */);
}

void NoteFdPath(int fd, const char* path) {
  if (fd < 0 || !path)
    return;
  std::lock_guard<std::mutex> l(g_path_lock);
  (*g_paths)[fd] = Absolute(path);
}

// /proc/self/auxv (the kernel layer's is empty): the auxiliary vector that
// follows GuestProcessArgv's envp, in the calling guest's word size. Go's
// runtime takes the page size from it (it gives library constructors no
// auxv on Android), and aborts without one. A pipe holds the bytes.
static bool OwnAuxv(const char* path) {
  char own[40];
  snprintf(own, sizeof own, "/proc/%d/auxv", getpid());
  return !strcmp(path, "/proc/self/auxv") || !strcmp(path, "/proc/thread-self/auxv") ||
         !strcmp(path, own);
}
static int AuxvFd() {
  const bool lp64 = GuestThreadLp64() && GuestProcessArgv64;
  const uint32_t w = lp64 ? 8 : 4;
  auto word = [&](uint32_t a) -> uint64_t { return lp64 ? Load64(a) : Load32(a); };
  uint32_t a = lp64 ? GuestProcessArgv64() : GuestProcessArgv();
  while (word(a)) a += w;  // argv
  a += w;
  while (word(a)) a += w;  // envp
  a += w;
  uint32_t end = a;
  while (word(end)) end += 2 * w;  // auxv up to AT_NULL
  end += 2 * w;
  int p[2];
  if (pipe(p) != 0)
    return -1;
  ssize_t n = write(p[1], Ptr<void>(a), end - a);
  close(p[1]);
  if (n != (ssize_t)(end - a)) {
    close(p[0]);
    errno = EIO;
    return -1;
  }
  return p[0];
}

int FdOpen(const char* path, int guest_flags, int mode) {
  int fd;
  if (OwnAuxv(path))
    return AuxvFd();
  if (FifoOpen(path, guest_flags, &fd)) {
    NoteFdPath(fd, path);
    return fd;
  }
  fd = open(path, HostOpenFlags(guest_flags), mode);
  NoteFdPath(fd, path);
  if ((guest_flags & 3) != 0)  // O_WRONLY, O_RDWR
    NoteFdWrite(fd);
  return fd;
}

ssize_t FdRead(int fd, void* buf, size_t n) {
  if (EmuFd* e = Emu(fd))
    return EmuRead(fd, e, buf, n);
  return read(fd, buf, n);
}

ssize_t FdWrite(int fd, const void* buf, size_t n) {
  if (EmuFd* e = Emu(fd))
    return EmuWrite(fd, e, buf, n);
  int wfd = FifoWriteEnd(fd);
  return write(wfd >= 0 ? wfd : fd, buf, n);
}

int FdClose(int fd) {
  FdForget(fd);
  FifoClose(fd);
  if (g_emu_count.load(std::memory_order_relaxed)) {
    EmuFd* e = nullptr;
    {
      std::lock_guard<std::mutex> l(g_emu_lock);
      auto it = g_emu->find(fd);
      if (it != g_emu->end()) {
        e = it->second;
        g_emu->erase(it);
        g_emu_count--;
      }
    }
    if (e) {
      close(e->wfd);
      delete e;  // queued timer entries no longer find it
    }
  }
  return close(fd);
}

void RegisterFd() { RegisterHostSymbols("libc.so", kFd);
  RegisterHostSymbols("libc.so", kFd64); }

// The *at() path table, for the raw-syscall handler (ndk_syscall.cc): the
// path openat(dirfd, path) means, joining the directory fd's remembered
// path. Returns false with errno set for an unknown directory fd.
bool ResolveAtPath(int dirfd, const char* path, std::string* out) {
  return AtPath(dirfd, path, out);
}

}  // namespace ndk
