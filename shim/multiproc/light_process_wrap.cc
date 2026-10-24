// Wrappers that keep light processes (shim/multiproc/include/arc_lproc.h;
// topic multiproc) together: a new thread belongs to its creator's process;
// exit(), _exit() and abort() in a light process end that process, not the
// VM; kill() of a light process's pid (Process.killProcess) kills it.
// Linked into art8 and art10 with -Wl,--wrap= for each of pthread_create,
// exit, _exit, abort and kill (mk/topic-multiproc.mk).
// Part of the arc-wasm shim layer.

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>

#include "include/arc_lproc.h"

extern "C" int __android_log_print(int prio, const char* tag, const char* fmt, ...);

namespace {

struct Start {
  void* (*fn)(void*);
  void* arg;
  int pid;
  int uid;
};

void* StartInProcess(void* p) {
  Start s = *static_cast<Start*>(p);
  free(p);
  arc_lproc_set_thread(s.pid, s.uid);
  return s.fn(s.arg);
}

int LightPid() {
  int pid, uid;
  arc_lproc_get_thread(&pid, &uid);
  return pid;
}

}  // namespace

extern "C" {

int __real_pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                          void* (*fn)(void*), void* arg);
void __real_exit(int status) __attribute__((noreturn));
void __real__exit(int status) __attribute__((noreturn));
void __real_abort(void) __attribute__((noreturn));
int __real_kill(pid_t pid, int sig);

int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                          void* (*fn)(void*), void* arg) {
  int pid, uid;
  arc_lproc_get_thread(&pid, &uid);
  if (!pid)
    return __real_pthread_create(thread, attr, fn, arg);
  Start* s = static_cast<Start*>(malloc(sizeof(Start)));
  s->fn = fn;
  s->arg = arg;
  s->pid = pid;
  s->uid = uid;
  const int r = __real_pthread_create(thread, attr, StartInProcess, s);
  if (r != 0)
    free(s);
  return r;
}

// Runtime.exit's nativeExit or a library's own exit(): no atexit handlers
// (they are the VM's), the process's binder process dies, the thread stops.
void __wrap_exit(int status) {
  const int pid = LightPid();
  if (pid) {
    arc_lproc_kill(pid, 0);
    arc_lproc_park("exit");
  }
  __real_exit(status);
}

void __wrap__exit(int status) {
  const int pid = LightPid();
  if (pid) {
    arc_lproc_kill(pid, 0);
    arc_lproc_park("_exit");
  }
  __real__exit(status);
}

void __wrap_abort(void) {
  const int pid = LightPid();
  if (pid) {
    __android_log_print(6, "LightProcess", "abort() in light process %d", pid);
    arc_lproc_kill(pid, SIGABRT);
    arc_lproc_park("abort");
  }
  __real_abort();
}

// Emscripten's kill() knows one pid (getpid(), now the caller's process) and
// raises the signal there. A light process's pid: 0 asks whether it lives,
// a fatal signal kills it (Process.killProcess, a parent killing its
// child), others are dropped unless it is the caller's own process.
int __wrap_kill(pid_t pid, int sig) {
  if (pid > 0 && arc_lproc_is_light(pid)) {
    if (!arc_lproc_alive(pid)) {
      errno = ESRCH;
      return -1;
    }
    if (sig == 0)
      return 0;
    if (sig == SIGKILL || sig == SIGTERM || sig == SIGABRT || sig == SIGSEGV ||
        sig == SIGQUIT || sig == SIGINT || sig == SIGHUP)
      return arc_lproc_kill(pid, sig);
    if (pid != LightPid())
      return 0;
  }
  return __real_kill(pid, sig);
}

}  // extern "C"
