// artvm --watchdog=SECONDS: a plain pthread (not attached to the runtime)
// that every SECONDS prints what each ART thread is doing: its state, the
// Java frames on top of its stack (of a thread that is not running Java
// code), the runtime locks it holds, and who owns
// the global runtime locks. For hangs (docs/topics/tiktok.md): it takes no
// lock it might wait for (a try-lock of the thread list, else a racy walk),
// allocates nothing in the Java heap, and writes with fs.writeSync from its
// own worker under Node (a write through the kernel, or console.log from a
// worker, waits for whatever the hang holds: the VFS mutex, Node's main
// thread). Debugging aid only; off unless asked for.
// Part of the arc-wasm shim layer.

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sstream>
#include <string>

#include <emscripten.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>

#include "art_method-inl.h"
#include "base/mutex.h"
#include "runtime.h"
#include "stack.h"
#include "thread.h"
#include "thread_list.h"
#include "utils.h"

extern "C" int arc_kernel_vfs_owner_tid() __attribute__((weak));
extern "C" size_t arc_opfs_dump_requests(char* buf, size_t size) __attribute__((weak));

namespace {

// Seconds (CLOCK_MONOTONIC) at which the main runtime thread last ran a
// no-op the watchdog sent it: whether it still serves proxied calls (stdio
// writes, host file reads).
volatile long gMainAnswered;

long Now() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec;
}

void MainPing(void*) { gMainAnswered = Now(); }

EM_JS(void, arc_watchdog_write, (const char* s), {
  var text = UTF8ToString(s);
  if (typeof process === 'object' && typeof require === 'function') {
    try { require('fs').writeSync(2, text); return; } catch (e) {}
  }
  err(text);  // a browser: to the page's output (posted from this worker)
});

struct Dump {
  std::string out;
};

void DumpThread(art::Thread* t, void* arg) NO_THREAD_SAFETY_ANALYSIS {
  Dump* d = static_cast<Dump*>(arg);
  std::string name;
  t->GetThreadName(name);
  char line[256];
  snprintf(line, sizeof(line), "  \"%s\" tid=%d", name.c_str(), t->GetTid());
  d->out += line;
  std::ostringstream state;
  state << t->GetState();
  d->out += " " + state.str();
  for (int level = 0; level < art::kLockLevelCount; ++level) {
    art::BaseMutex* m = t->GetHeldMutex(static_cast<art::LockLevel>(level));
    if (m != nullptr) {
      d->out += " holds ";
      d->out += m->GetName();
    }
  }
  d->out += "\n";
  // Frames only of threads that are not running Java code: a runnable
  // thread pushes and frees frames under the walk (no lock keeps them).
  if (t->GetState() == art::kRunnable) return;
  const art::ManagedStack* ms = t->GetManagedStack();
  art::ShadowFrame* sf = ms != nullptr ? ms->GetTopShadowFrame() : nullptr;
  for (int i = 0; sf != nullptr && i < 6; ++i, sf = sf->GetLink()) {
    art::ArtMethod* m = sf->GetMethod();
    if (m == nullptr) continue;
    snprintf(line, sizeof(line), "    at %s (dex pc %u)\n", art::PrettyMethod(m).c_str(),
             sf->GetDexPC());
    d->out += line;
  }
}

void* Run(void* arg) NO_THREAD_SAFETY_ANALYSIS {
  const unsigned seconds = static_cast<unsigned>(reinterpret_cast<uintptr_t>(arg));
  for (int round = 1;; ++round) {
    sleep(seconds);
    art::Runtime* rt = art::Runtime::Current();
    if (rt == nullptr || rt->GetThreadList() == nullptr) continue;
    Dump d;
    char head[256];
    const long now = Now();
    snprintf(head, sizeof(head), "artvm watchdog #%d (t=%ld s): main thread answered %ld s ago, "
             "VFS mutex owner %d, mutator lock owner %llu, thread list lock owner %llu\n", round,
             now, gMainAnswered ? now - gMainAnswered : -1L,
             arc_kernel_vfs_owner_tid ? arc_kernel_vfs_owner_tid() : -1,
             static_cast<unsigned long long>(art::Locks::mutator_lock_->GetExclusiveOwnerTid()),
             static_cast<unsigned long long>(
                 art::Locks::thread_list_lock_->GetExclusiveOwnerTid()));
    d.out = head;
    const bool locked = art::Locks::thread_list_lock_->ExclusiveTryLock(nullptr);
    if (!locked) d.out += "  (thread list lock busy: unlocked walk)\n";
    if (arc_opfs_dump_requests) {
      char opfs[4096];
      arc_opfs_dump_requests(opfs, sizeof(opfs));
      d.out += opfs;
    }
    rt->GetThreadList()->ForEach(DumpThread, &d);
    if (locked) art::Locks::thread_list_lock_->ExclusiveUnlock(nullptr);
    arc_watchdog_write(d.out.c_str());
    emscripten_proxy_async(emscripten_proxy_get_system_queue(),
                           emscripten_main_runtime_thread_id(), MainPing, nullptr);
  }
  return nullptr;
}

}  // namespace

// Starts the watchdog (artvm.cc, for --watchdog=SECONDS).
void arc_art_start_watchdog(unsigned seconds) {
  pthread_t t;
  if (pthread_create(&t, nullptr, Run, reinterpret_cast<void*>(static_cast<uintptr_t>(seconds))) ==
      0) {
    pthread_detach(t);
  }
}
