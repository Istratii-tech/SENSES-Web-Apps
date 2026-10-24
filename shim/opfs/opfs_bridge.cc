// See opfs_bridge.h.

#include "opfs/opfs_bridge.h"

#include <atomic>

#include <emscripten/emscripten.h>
#include <emscripten/eventloop.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "base/synchronization/lock.h"
#include "common/alog.h"

extern "C" {
// library_arc_opfs.js
void arc_opfs_start(em_proxying_ctx* ctx, arc_opfs::Request* request);
int arc_opfs_available();
}

static_assert(offsetof(arc_opfs::Request, result) == 28, "layout");
static_assert(offsetof(arc_opfs::Request, offset) == 32, "layout");
static_assert(offsetof(arc_opfs::Request, size) == 40, "layout");
static_assert(offsetof(arc_opfs::Request, mtime) == 48, "layout");
static_assert(offsetof(arc_opfs::Request, type) == 56, "layout");
static_assert(offsetof(arc_opfs::Request, listing) == 60, "layout");

namespace arc_opfs {

namespace {

pthread_once_t g_once = PTHREAD_ONCE_INIT;
pthread_t g_thread;
bool g_started = false;
pthread_mutex_t g_ready_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_ready_cond = PTHREAD_COND_INITIALIZER;
bool g_ready = false;
bool g_available = false;
std::atomic<base::Lock*> g_released_lock(NULL);
thread_local int t_keep_lock = 0;  // ScopedKeepLock depth

void* ThreadMain(void*) {
  pthread_mutex_lock(&g_ready_mutex);
  g_available = arc_opfs_available() != 0;
  g_ready = true;
  pthread_cond_broadcast(&g_ready_cond);
  pthread_mutex_unlock(&g_ready_mutex);
  // Stay alive in the worker's event loop, serving proxied requests and the
  // promises they start. Without the keepalive the thread would count as
  // exited once it unwinds, and Emscripten would cancel requests in flight.
  emscripten_runtime_keepalive_push();
  emscripten_unwind_to_js_event_loop();
  return NULL;
}

void StartOnce() {
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  g_started = pthread_create(&g_thread, &attr, ThreadMain, NULL) == 0;
  pthread_attr_destroy(&attr);
  if (!g_started)
    return;
  pthread_mutex_lock(&g_ready_mutex);
  while (!g_ready)
    pthread_cond_wait(&g_ready_cond, &g_ready_mutex);
  pthread_mutex_unlock(&g_ready_mutex);
}

void Dispatch(em_proxying_ctx* ctx, void* arg) {
  arc_opfs_start(ctx, static_cast<Request*>(arg));
}

}  // namespace

void SetLockReleasedWhileWaiting(base::Lock* lock) {
  g_released_lock.store(lock, std::memory_order_relaxed);
}

ScopedKeepLock::ScopedKeepLock() { ++t_keep_lock; }
ScopedKeepLock::~ScopedKeepLock() { --t_keep_lock; }

bool Start() {
  pthread_once(&g_once, StartOnce);
  return g_started && g_available;
}

// Requests in flight, for diagnostics (arc_opfs_dump_requests: artvm
// --watchdog): which thread waits for which operation, since when.
struct InFlight {
  int tid;
  int op;
  const Request* request;
  double since_ms;
};
pthread_mutex_t g_flight_mutex = PTHREAD_MUTEX_INITIALIZER;
InFlight g_flight[64];

int Call(Request* request) {
  LOG_ALWAYS_FATAL_IF(!g_started || !g_available, "OPFS bridge not started");
  LOG_ALWAYS_FATAL_IF(emscripten_is_main_browser_thread(),
                      "OPFS calls block; not allowed on the main thread");
  request->result = -EIO;
  int slot = -1;
  pthread_mutex_lock(&g_flight_mutex);
  for (int i = 0; i < 64; ++i) {
    if (!g_flight[i].request) {
      g_flight[i] = {gettid(), request->op, request, emscripten_get_now()};
      slot = i;
      break;
    }
  }
  pthread_mutex_unlock(&g_flight_mutex);
  const double start_ms = emscripten_get_now();
  // The VFS mutex is not held across the wait: the browser can take seconds
  // to answer (a busy machine), and every file operation of every thread,
  // mmap and munmap included, would wait behind this one -- a collection of
  // ART's freeing a large object among them, with all of ART's allocating
  // threads waiting for that collection.
  base::Lock* held = g_released_lock.load(std::memory_order_relaxed);
  if (held && (t_keep_lock > 0 || !held->IsAcquiredByCurrentThread()))
    held = NULL;
  if (held)
    held->Release();
  const bool ok = emscripten_proxy_sync_with_ctx(emscripten_proxy_get_system_queue(),
                                                 g_thread, Dispatch, request);
  if (held)
    held->Acquire();
  // A slow request in the log (topic gc-pause: what a start waits for when
  // the machine is busy).
  const double took_ms = emscripten_get_now() - start_ms;
  if (took_ms >= 1000) {
    ALOGW("OPFS: request %d (%s) took %.0f ms (tid %d)", request->op,
          request->path ? request->path : "", took_ms, gettid());
  }
  if (slot >= 0) {
    pthread_mutex_lock(&g_flight_mutex);
    g_flight[slot].request = NULL;
    pthread_mutex_unlock(&g_flight_mutex);
  }
  return ok ? request->result : -EIO;
}

int Stat(const std::string& path, EntryType* type, double* size,
         double* mtime) {
  Request r = {};
  r.op = kStat;
  r.path = path.c_str();
  const int result = Call(&r);
  if (result >= 0) {
    *type = static_cast<EntryType>(r.type);
    *size = r.size;
    *mtime = r.mtime;
  }
  return result;
}

int List(const std::string& path, std::vector<Entry>* entries) {
  Request r = {};
  r.op = kList;
  r.path = path.c_str();
  const int result = Call(&r);
  if (result < 0)
    return result;
  // "<d|f><name>\0" ... "\0"
  for (const char* p = r.listing; p && *p; p += strlen(p) + 1) {
    Entry e;
    e.is_directory = (*p == 'd');
    e.name = p + 1;
    entries->push_back(e);
  }
  free(r.listing);
  return result;
}

int PathOp(Op op, const std::string& path, const std::string& path2) {
  Request r = {};
  r.op = op;
  r.path = path.c_str();
  r.path2 = path2.c_str();
  return Call(&r);
}

}  // namespace arc_opfs

// A file growing in OPFS, while the browser has not answered: [0] since when
// (ms since the epoch, 0: none), [1] the bytes it grows by; the file's path
// in arc_opfs_stall_path. Written by the OPFS thread (library_arc_opfs.js)
// around each write or truncate that makes a file larger -- the only calls
// that ask the browser for storage space, and on Safari 16, once the
// origin's quota is used up, wait for an answer that does not come while the
// page lives: the thread stops inside the call (topic opfs-after-kill,
// docs/topics/opfs-after-kill.md). The page reads it (demo/apps/run.html:
// "storage full").
extern "C" EMSCRIPTEN_KEEPALIVE double arc_opfs_stall[2] = {0, 0};
extern "C" EMSCRIPTEN_KEEPALIVE char arc_opfs_stall_path[256] = {0};

// One line per OPFS request in flight ("tid op path for N ms"); for
// diagnostics (artvm --watchdog). Returns the length written.
extern "C" size_t arc_opfs_dump_requests(char* buf, size_t size) {
  static const char* const kOps[] = {"?", "open", "close", "read", "write", "getsize", "truncate",
                                     "flush", "stat", "mkdir", "unlink", "rmdir", "rename", "list"};
  size_t n = 0;
  if (size) buf[0] = '\0';
  const double now = emscripten_get_now();
  pthread_mutex_lock(&arc_opfs::g_flight_mutex);
  for (const arc_opfs::InFlight& f : arc_opfs::g_flight) {
    if (!f.request || n >= size) continue;
    const char* path = f.request->path ? f.request->path : "";
    n += snprintf(buf + n, size - n, "  opfs: tid %d %s %s for %.0f ms\n", f.tid,
                  f.op >= 0 && f.op < 14 ? kOps[f.op] : "?", path, now - f.since_ms);
  }
  pthread_mutex_unlock(&arc_opfs::g_flight_mutex);
  return n < size ? n : size;
}
