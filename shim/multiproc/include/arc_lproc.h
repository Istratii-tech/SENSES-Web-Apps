// Light processes: Android processes of one app as groups of threads of the
// one WebAssembly process (topic multiproc, docs/topics/multiproc.md).
// Part of the arc-wasm shim layer.
//
// The VM is one process (getpid() 42 in Emscripten): the system server, the
// app's main process and the test harness run in it. A component of another
// android:process runs in a light process: a pid of its own, a name (what
// /proc/PID/cmdline and Process.setArgV0 say), a uid (the VM's, or an
// isolated one), and the threads that belong to it. A thread belongs to the
// light process of the thread that created it (pthread_create is wrapped:
// -Wl,--wrap=pthread_create at the VM's link), or to the one it joined with
// arc_lproc_enter(). getpid() and getuid() answer the calling thread's
// process; so do the binder driver (shim/binder/dev_binder.cc: one binder
// process per light process) and /proc/self (shim/app6/proc_self.cc).
//
// Killing a light process (arc_lproc_kill) marks it dead, kills its binder
// process (callers get DeadObjectException, holders of its objects death
// notices), wakes the system server's reaper (arc_lproc_wait_killed) and,
// when a thread kills its own process (Process.killProcess(myPid()),
// System.exit, exit(), abort() in a light process), parks that thread for
// good, as a killed process's threads never run again. The other threads of
// the dead process cannot be stopped from outside in WebAssembly: they stay
// as zombies, but their process is gone for everyone else.
#ifndef ARC_WASM_LPROC_H_
#define ARC_WASM_LPROC_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// The VM's own pid (its first process: system server + the app's main process).
int arc_lproc_main_pid(void);

// The pid of the calling thread's process (arc_lproc_main_pid() unless it
// belongs to a light process). What getpid() returns.
int arc_lproc_current(void);

// The uid of the calling thread's process. What getuid() returns.
int arc_lproc_current_uid(void);

// The uid of process |pid| (the VM's: 0).
int arc_lproc_uid(int pid);

// Whether any light process was ever created (cheap; for fast paths).
int arc_lproc_any(void);

// Creates a light process: a new pid (from 2001 on), its name and uid.
// No thread belongs to it yet.
int arc_lproc_create(const char* name, int uid);

// The calling thread joins process |pid| (a fresh thread, before it uses
// binder). Returns 0, or -1 if there is no such live process.
int arc_lproc_enter(int pid);

// 1 if |pid| is the VM's pid or a live light process, else 0.
int arc_lproc_alive(int pid);

// 1 if |pid| is a light process (alive or dead), else 0.
int arc_lproc_is_light(int pid);

// The name of process |pid| into |buf| (empty if unknown); its length.
size_t arc_lproc_name(int pid, char* buf, size_t size);

// Names process |pid| (Process.setArgV0 of a thread of it).
void arc_lproc_set_name(int pid, const char* name);

// Kills light process |pid| (see above). Returns 0, -1 if it is not a live
// light process. Never returns when it is the caller's own process.
int arc_lproc_kill(int pid, int signal);

// Blocks up to |timeout_ms| (< 0: forever) for a light process killed since
// the last call; returns its pid, or 0 on timeout.
int arc_lproc_wait_killed(int timeout_ms);

// The live light processes' pids into |pids| (at most |max|); their count.
int arc_lproc_list(int* pids, int max);

// Called with the pid of each light process right after it is killed, on
// the killing thread, before the reaper hears of it. The binder driver
// registers one (it is in another library).
void arc_lproc_set_kill_hook(void (*hook)(int pid));

// Called with the pid of each light process right after it is created
// (/proc/PID/ is mounted by one).
void arc_lproc_set_create_hook(void (*hook)(int pid));

// The namespace of the NDK bridge's loader (shim/ndk/ndk_linker*.cc) for
// the calling thread: 0 for the VM's process, else its light process's pid,
// so that each process has its own copy of an app's native libraries (their
// statics), as on a device. 0 too for a light process that shares the
// main process's copies (arc_lproc_set_native_shared).
int arc_lproc_native_ns(void);
void arc_lproc_set_native_shared(int pid, int shared);

// The calling thread's light process (0: the VM's) and uid, as raw
// thread state; for the wrappers of light_process_wrap.cc.
void arc_lproc_get_thread(int* pid, int* uid);
void arc_lproc_set_thread(int pid, int uid);

// Blocks the calling thread for good (a thread of a killed process).
void arc_lproc_park(const char* why) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif  // ARC_WASM_LPROC_H_
