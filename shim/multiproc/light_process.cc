// Light processes (shim/multiproc/include/arc_lproc.h; topic multiproc,
// docs/topics/multiproc.md): the pid registry, the calling thread's process,
// and the wrappers that make threads inherit it and keep a light process's
// exit(), _exit() and abort() to that process.
//
// The wrappers (light_process_wrap.cc) take effect where the VM is linked
// with them and -Wl,--wrap=SYMBOL (art8, art10: mk/topic-multiproc.mk);
// elsewhere no thread ever joins a light process.
// Part of the arc-wasm shim layer.

#include "include/arc_lproc.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <deque>
#include <map>
#include <string>

extern "C" int __android_log_print(int prio, const char* tag, const char* fmt, ...);

namespace {

// Emscripten's getpid() (emscripten_syscall_stubs.c: g_pid). __syscall_getpid
// is ours now (shim/kernel/arc_kernel.cc), so this is the VM's pid.
const int kMainPid = 42;
const int kFirstLightPid = 2001;
const int kMaxHooks = 4;

struct Process {
  std::string name;
  int uid = 0;
  bool dead = false;
  bool native_shared = false;
  // Spawned processes (topic native-exec): parent, wait status.
  bool spawned = false;
  int parent = 0;
  bool exited = false;
  bool reaped = false;
  int wstatus = 0;
};

pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_killed_cond = PTHREAD_COND_INITIALIZER;
pthread_cond_t g_exit_cond = PTHREAD_COND_INITIALIZER;  // a spawned process ended
void (*g_exit_hook)(int, int) = nullptr;
// Never freed (a dead process keeps its record: its zombie threads still ask).
std::map<int, Process>* g_procs = new std::map<int, Process>;
std::string* g_main_name = new std::string("app_process");
std::deque<int>* g_killed = new std::deque<int>;
int g_next_pid = kFirstLightPid;
volatile int g_any = 0;
void (*g_kill_hooks[kMaxHooks])(int) = {};
void (*g_create_hooks[kMaxHooks])(int) = {};

void AddHook(void (**hooks)(int), void (*hook)(int)) {
  for (int i = 0; i < kMaxHooks; i++) {
    if (!hooks[i] || hooks[i] == hook) {
      hooks[i] = hook;
      return;
    }
  }
}

// The calling thread's light process; 0: the VM's.
thread_local int t_pid = 0;
thread_local int t_uid = 0;


struct Lock {
  Lock() { pthread_mutex_lock(&g_lock); }
  ~Lock() { pthread_mutex_unlock(&g_lock); }
};

// A thread of a killed process never runs again.
[[noreturn]] void Park(const char* why) {
  __android_log_print(4, "LightProcess", "pid %d: %s; thread parked", t_pid, why);
  pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
  pthread_cond_t c = PTHREAD_COND_INITIALIZER;
  pthread_mutex_lock(&m);
  while (true)
    pthread_cond_wait(&c, &m);
}

}  // namespace

extern "C" {

int arc_lproc_main_pid(void) { return kMainPid; }

int arc_lproc_current(void) { return t_pid ? t_pid : kMainPid; }

int arc_lproc_current_uid(void) { return t_uid; }

int arc_lproc_uid(int pid) {
  Lock l;
  std::map<int, Process>::iterator it = g_procs->find(pid);
  return it != g_procs->end() ? it->second.uid : 0;
}

int arc_lproc_any(void) { return g_any; }

int arc_lproc_create(const char* name, int uid) {
  int pid;
  void (*hooks[kMaxHooks])(int);
  {
    Lock l;
    pid = g_next_pid++;
    Process& p = (*g_procs)[pid];
    p.name = name ? name : "";
    p.uid = uid;
    g_any = 1;
    memcpy(hooks, g_create_hooks, sizeof(hooks));
    __android_log_print(4, "LightProcess", "created pid %d uid %d \"%s\"", pid, uid,
                        p.name.c_str());
  }
  for (int i = 0; i < kMaxHooks; i++) {
    if (hooks[i])
      hooks[i](pid);
  }
  return pid;
}

int arc_lproc_enter(int pid) {
  if (pid == kMainPid) {
    t_pid = 0;
    t_uid = 0;
    return 0;
  }
  Lock l;
  std::map<int, Process>::iterator it = g_procs->find(pid);
  if (it == g_procs->end() || it->second.dead)
    return -1;
  t_pid = pid;
  t_uid = it->second.uid;
  return 0;
}

int arc_lproc_alive(int pid) {
  if (pid == kMainPid)
    return 1;
  Lock l;
  std::map<int, Process>::iterator it = g_procs->find(pid);
  return it != g_procs->end() && !it->second.dead;
}

int arc_lproc_is_light(int pid) {
  Lock l;
  return g_procs->count(pid) != 0;
}

size_t arc_lproc_name(int pid, char* buf, size_t size) {
  std::string name;
  {
    Lock l;
    if (pid == kMainPid) {
      name = *g_main_name;
    } else {
      std::map<int, Process>::iterator it = g_procs->find(pid);
      if (it != g_procs->end())
        name = it->second.name;
    }
  }
  if (buf && size) {
    const size_t n = name.size() < size - 1 ? name.size() : size - 1;
    memcpy(buf, name.data(), n);
    buf[n] = '\0';
  }
  return name.size();
}

void arc_lproc_set_name(int pid, const char* name) {
  Lock l;
  if (pid == kMainPid) {
    *g_main_name = name ? name : "";
    return;
  }
  std::map<int, Process>::iterator it = g_procs->find(pid);
  if (it != g_procs->end())
    it->second.name = name ? name : "";
}

int arc_lproc_kill(int pid, int signal) {
  void (*hooks[kMaxHooks])(int);
  {
    Lock l;
    std::map<int, Process>::iterator it = g_procs->find(pid);
    if (it == g_procs->end() || it->second.dead)
      return -1;
    it->second.dead = true;
    if (it->second.spawned && !it->second.exited) {
      it->second.exited = true;
      it->second.wstatus = signal ? (signal & 0x7f) : 0;
      pthread_cond_broadcast(&g_exit_cond);
    }
    memcpy(hooks, g_kill_hooks, sizeof(hooks));
    __android_log_print(4, "LightProcess", "killed pid %d \"%s\" (signal %d, from pid %d)",
                        pid, it->second.name.c_str(), signal, arc_lproc_current());
  }
  for (int i = 0; i < kMaxHooks; i++) {
    if (hooks[i])
      hooks[i](pid);
  }
  {
    Lock l;
    g_killed->push_back(pid);
    pthread_cond_broadcast(&g_killed_cond);
  }
  if (pid == t_pid)
    Park("killed");
  return 0;
}

int arc_lproc_wait_killed(int timeout_ms) {
  Lock l;
  if (g_killed->empty()) {
    if (timeout_ms < 0) {
      while (g_killed->empty())
        pthread_cond_wait(&g_killed_cond, &g_lock);
    } else {
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_sec += timeout_ms / 1000;
      ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
      if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
      }
      while (g_killed->empty()) {
        if (pthread_cond_timedwait(&g_killed_cond, &g_lock, &ts) == ETIMEDOUT)
          break;
      }
    }
  }
  if (g_killed->empty())
    return 0;
  const int pid = g_killed->front();
  g_killed->pop_front();
  return pid;
}

int arc_lproc_list(int* pids, int max) {
  Lock l;
  int n = 0;
  for (std::map<int, Process>::iterator it = g_procs->begin(); it != g_procs->end(); ++it) {
    if (it->second.dead)
      continue;
    if (n < max && pids)
      pids[n] = it->first;
    n++;
  }
  return n;
}

void arc_lproc_set_kill_hook(void (*hook)(int pid)) {
  Lock l;
  AddHook(g_kill_hooks, hook);
}

void arc_lproc_set_create_hook(void (*hook)(int pid)) {
  Lock l;
  AddHook(g_create_hooks, hook);
}

int arc_lproc_native_ns(void) {
  if (!t_pid)
    return 0;
  Lock l;
  std::map<int, Process>::iterator it = g_procs->find(t_pid);
  return it != g_procs->end() && it->second.native_shared ? 0 : t_pid;
}

void arc_lproc_set_native_shared(int pid, int shared) {
  Lock l;
  std::map<int, Process>::iterator it = g_procs->find(pid);
  if (it != g_procs->end())
    it->second.native_shared = shared != 0;
}

// For shim/multiproc/light_process_wrap.cc (linked only where the VM wraps
// the symbols).
void arc_lproc_get_thread(int* pid, int* uid) {
  *pid = t_pid;
  *uid = t_uid;
}

void arc_lproc_set_thread(int pid, int uid) {
  t_pid = pid;
  t_uid = uid;
}

void arc_lproc_park(const char* why) { Park(why); }

int arc_lproc_spawn_create(const char* name, int uid) {
  const int parent = arc_lproc_current();
  const int pid = arc_lproc_create(name, uid);
  Lock l;
  Process& p = (*g_procs)[pid];
  p.spawned = true;
  p.parent = parent;
  return pid;
}

int arc_lproc_is_spawned(int pid) {
  Lock l;
  std::map<int, Process>::iterator it = g_procs->find(pid);
  return it != g_procs->end() && it->second.spawned;
}

int arc_lproc_parent(int pid) {
  Lock l;
  std::map<int, Process>::iterator it = g_procs->find(pid);
  return it != g_procs->end() && it->second.spawned ? it->second.parent : kMainPid;
}

void arc_lproc_exited(int pid, int wstatus) {
  void (*hooks[kMaxHooks])(int);
  bool was_dead;
  {
    Lock l;
    std::map<int, Process>::iterator it = g_procs->find(pid);
    if (it == g_procs->end())
      return;
    if (!it->second.exited) {
      it->second.exited = true;
      it->second.wstatus = wstatus;
    }
    was_dead = it->second.dead;
    it->second.dead = true;
    memcpy(hooks, g_kill_hooks, sizeof(hooks));
    pthread_cond_broadcast(&g_exit_cond);
    __android_log_print(4, "LightProcess", "pid %d \"%s\" ended (status 0x%x)", pid,
                        it->second.name.c_str(), it->second.wstatus);
  }
  if (was_dead)
    return;
  for (int i = 0; i < kMaxHooks; i++) {
    if (hooks[i])
      hooks[i](pid);
  }
}

void arc_lproc_set_exit_hook(void (*hook)(int pid, int wstatus)) {
  Lock l;
  g_exit_hook = hook;
}

void arc_lproc_run_exit_hook(int pid, int wstatus) {
  void (*hook)(int, int);
  {
    Lock l;
    hook = g_exit_hook;
    std::map<int, Process>::iterator it = g_procs->find(pid);
    if (it == g_procs->end() || !it->second.spawned)
      return;
  }
  if (hook)
    hook(pid, wstatus);
}

int arc_lproc_waitpid(int pid, int* wstatus, int options) {
  const int self = arc_lproc_current();
  Lock l;
  while (true) {
    bool any = false;
    for (std::map<int, Process>::iterator it = g_procs->begin(); it != g_procs->end(); ++it) {
      Process& p = it->second;
      // A given pid is waited for from any process (Java's reaper thread is
      // shared by the app's processes); "any child" means the caller's.
      if (!p.spawned || p.reaped || (pid > 0 ? it->first != pid : p.parent != self))
        continue;
      any = true;
      if (p.exited) {
        p.reaped = true;
        if (wstatus)
          *wstatus = p.wstatus;
        return it->first;
      }
    }
    if (!any) {
      errno = ECHILD;
      return -1;
    }
    if (options & 1 /* WNOHANG */)
      return 0;
    pthread_cond_wait(&g_exit_cond, &g_lock);
  }
}

}  // extern "C"
