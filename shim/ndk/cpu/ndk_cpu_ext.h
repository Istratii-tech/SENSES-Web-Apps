// Optional extensions of the ARM CPU beyond the contract in
// shim/ndk/ndk_cpu.h (topic "arm-cpu", docs/topics/arm-cpu.md). Nothing
// here is required by the bridge; it is for tests, tracing and tuning.
#ifndef ARC_WASM_SHIM_NDK_CPU_NDK_CPU_EXT_H_
#define ARC_WASM_SHIM_NDK_CPU_NDK_CPU_EXT_H_

#include <stddef.h>
#include <stdint.h>

#include "ndk/ndk_cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

// Raw Linux EABI syscalls (SVC with the number in r7). The handler returns
// nonzero if it handled the call (result in state->r[0], negative errno on
// failure); otherwise the CPU's own small table runs (exit, write, getpid,
// gettid, clock_gettime, gettimeofday, cacheflush, set_tls), and anything
// else returns -ENOSYS with a one-time message.
typedef int (*NdkSyscallHandler)(NdkCpuState* state);
void ndk_set_syscall_handler(NdkSyscallHandler fn);

// Called instead of the default report-and-abort for faults. |pc| is the
// faulting guest address, |message| a description. The handler must not
// return to the CPU (longjmp/throw out of it, or exit).
typedef void (*NdkFaultHandler)(NdkCpuState* state, uint32_t pc, const char* message);
void ndk_set_fault_handler(NdkFaultHandler fn);

// ndk_call_guest that a fatal guest error can end (topic userspace): abort(),
// a call to an unresolved symbol, a CPU fault. ndk_leave_guest(why) on the
// calling thread then returns here, the CPU state as before the call: 0, with
// |why| (the guest's stack and heap stay as the error left them, so the
// caller should give up on the guest code's work, as a crashed app). 1 and
// *result when the call returned.
int ndk_call_guest_guarded(uint32_t addr, const uint32_t* args, int nargs, uint64_t* result,
                           char* why, size_t why_size);
// Leaves the innermost ndk_call_guest_guarded of the calling thread; returns
// (and the caller aborts) if there is none.
void ndk_leave_guest(const char* why);

// Size of guest stacks created by ndk_current_state() from now on
// (default 1 MiB).
void ndk_set_guest_stack_size(size_t bytes);
// The same for the calling thread only (a guest pthread_create's stack
// size), if its guest state does not exist yet; 0 = the default above.
void ndk_set_thread_guest_stack_size(size_t bytes);
// The calling thread's guest stack: [*base, *base + *size) (creates the
// thread's guest state if needed).
void ndk_guest_stack(uint32_t* base, uint32_t* size);

// Guest instructions executed on the calling thread so far (conditional
// instructions that did not pass count, as on hardware) and global counters.
uint64_t ndk_instruction_count(void);
typedef struct NdkCpuStats {
  uint64_t blocks_decoded;
  uint64_t insns_decoded;
  uint64_t blocks_flushed;
  uint64_t host_calls;
  uint64_t blocks_translated;        // to wasm (ndk_set_jit)
  uint64_t translated_bytes;
  uint64_t blocks_not_translatable;  // hot blocks with operations the translator lacks
  uint64_t flush_calls;              // ndk_flush_code (cacheflush, mprotect, loads ...)
  uint64_t blocks_kept;              // blocks in flushed ranges whose code was the same
} NdkCpuStats;
void ndk_cpu_stats(NdkCpuStats* out);
// Debugging: prints the calling thread's last host calls to stderr.
void ndk_cpu_report_recent_calls(void);
// Debugging: logs calls of host functions whose name contains one of the
// comma-separated |words| (the first 400 of each and every 1000th).
void ndk_cpu_watch_host_calls(const char* words);
// Debugging: logs (the first 100) host calls whose first three arguments
// point at memory holding |text| in its first 256 bytes, with tid and pid.
void ndk_cpu_watch_strings(const char* text);
// Debugging: |hook| is called on the executing thread, after the block lock is
// released, for every guest block decoded from now on ([start, end), Thumb
// bit not set); null removes it.
typedef void (*NdkBlockHook)(uint32_t start, uint32_t end);
void ndk_cpu_set_block_hook(NdkBlockHook hook);

// Translation of hot blocks to wasm functions: a block is translated after
// |threshold| executions (default 50; 0 = off, interpreter only). Needs the
// final link to allow table growth (-sALLOW_TABLE_GROWTH); otherwise the
// first attempt reports it once and translation stays off.
void ndk_set_jit(int threshold);
// At most |max| translations instantiated per thread (0: no limit, -1: by
// the browser: 1024 on iOS).
void ndk_set_jit_max(int max);

// Whether |addr| is a host stub returned by ndk_host_stub, and its name.
const char* ndk_host_stub_name(uint32_t addr);

// --- Signals to guest threads (shim/ndk/cpu/signal.cc) ---------------------------
// Linux ARM signal numbers 1..64; results are 0 or a negative Linux errno
// (EINTR 4, EAGAIN 11, ESRCH 3, EINVAL 22, ETIMEDOUT 110). A handler runs on
// the target thread at a safe point: a block boundary of guest code, or
// inside one of the waits below. Signals without a guest handler (SIG_DFL,
// SIG_IGN) are dropped when they would be delivered.
typedef struct NdkSigaction {
  uint32_t handler;   // guest address (bit 0: Thumb); 0 SIG_DFL, 1 SIG_IGN
  uint32_t flags;     // SA_SIGINFO, SA_RESTART, SA_NODEFER, SA_RESETHAND (others kept, unused)
  uint64_t mask;      // blocked while the handler runs; bit (sig - 1)
} NdkSigaction;
int ndk_sig_action(int sig, const NdkSigaction* act, NdkSigaction* old);
int ndk_sig_has_handler(int sig);
// Makes |sig| pending on the guest thread with kernel thread id |tid| /
// host pthread_t |thread| and wakes it from a wait here. -ESRCH: no such
// thread runs guest code. sig 0 only checks.
int ndk_sig_send(int tid, int sig);
int ndk_sig_send_pthread(uint32_t thread, int sig);
// The calling thread's mask: how 0 SIG_BLOCK, 1 SIG_UNBLOCK, 2 SIG_SETMASK.
int ndk_sig_mask(int how, const uint64_t* set, uint64_t* old);
uint64_t ndk_sig_pending(void);
// The calling thread sends itself |sig| and, from a host function, runs
// the handler before returning (raise).
int ndk_sig_raise(int sig);
// From a host function: runs the handlers of the calling thread's pending
// unblocked signals now. 0 none ran, 1 all had SA_RESTART, 2 otherwise.
int ndk_sig_deliver(void);
// futex(FUTEX_WAIT) on the guest word at |addr| for at most |ms| (INFINITY:
// no limit) that a signal interrupts: its handler runs, then -EINTR. 0 when
// woken (spuriously too), -ETIMEDOUT, -EAGAIN if *addr != val.
int ndk_sig_wait(uint32_t addr, uint32_t val, double ms);
// Sleeps |ms|; -EINTR (and the time left) if a handler ran.
int ndk_sig_sleep(double ms, double* left_ms);
// sigsuspend: the mask is |mask| until a handler has run; -EINTR.
int ndk_sig_suspend(const uint64_t* mask);
// Whether the calling thread (in a host function under guest code) has a
// pending unblocked signal: for waits that cannot be woken (poll), which
// then call ndk_sig_deliver and return EINTR.
int ndk_sig_check(void);
// The thread guest code runs as: the calling thread, or the one a signal
// proxy runs a handler for (pthread_self / gettid for guest code).
void ndk_sig_self(uint32_t* thread, int* tid);
// sigaltstack for the calling thread: |ss| and |old| are {ss_sp, ss_flags,
// ss_size} (either may be null); old's flags have SS_ONSTACK (1) while the
// thread runs on it. -EPERM to change it from there, as Linux.
int ndk_sig_altstack(const uint32_t* ss, uint32_t* old);
// sigwait / sigtimedwait: takes a pending signal of |set| (the signal
// number), -EAGAIN after |ms|, -EINTR if a handler of another one ran.
int ndk_sig_wait_set(uint64_t set, double ms);

// pthread_exit from a host function: the calling thread leaves its guest
// calls without returning (their frames are gone); it no longer runs guest
// code (the gate is free: its CPU's destructor takes it).
void ndk_cpu_thread_exit(void);
// Whether the calling host thread has a guest context (a Java thread with one
// keeps its worker: topic java-pool-park).
int ndk_cpu_thread_has_state(void);

// --- Green threads (shim/ndk/ndk_green.cc, topic threads-per-worker) ------------------
// A guest thread whose CPU state no host thread owns: carrier threads run it
// in turns, and it gives its carrier back when it waits at the outermost
// level of a host call (or when asked to yield). There is no switching of
// host stacks: the host function that would wait parks the call instead,
// and the call runs again from the start when the thread resumes, the
// record it left (NdkResume) telling it what it had done.
// (NdkResume and NdkPark: shim/ndk/ndk_cpu.h.)
// A CPU state without a thread: its own guest stack, kernel tid |tid| and
// pthread_t |self| for the guest, |owner| for the bridge. Listed for dumps
// and signals (by |tid| and |self|).
NdkCpuState* ndk_cpu_state_new(size_t stack_size, int tid, uint32_t self, void* owner);
void ndk_cpu_state_free(NdkCpuState* s);
void* ndk_cpu_state_owner(const NdkCpuState* s);
// Sets it to call |fn| (bit 0: Thumb) with |arg|, returning to the host.
void ndk_cpu_green_start(NdkCpuState* s, uint32_t fn, uint32_t arg);
// On a carrier thread: |s| becomes the calling thread's CPU (with the
// carrier's translations), runs until it returns from its function
// (kNdkParkNone, the result in r[0]), parks a call, yields or ends
// (ndk_cpu_green_end), and stops being it.
void ndk_cpu_green_enter(NdkCpuState* s);
int ndk_cpu_green_run(NdkCpuState* s);
// |parked|: no carrier will run it until it is resumed (gate 3); returns 1
// if a deliverable signal is pending (the bridge resumes it then).
int ndk_cpu_green_leave(NdkCpuState* s, int parked);
// Called (with the CPU list's lock held) when a signal is posted to a
// parked green thread that can take it.
void ndk_cpu_set_green_hook(void (*hook)(NdkCpuState* s));
// Translations instantiated on the calling carrier thread (its table).
uint32_t ndk_cpu_carrier_translations(void);
// Asks a running green thread to yield at its next block boundary.
void ndk_cpu_green_yield(NdkCpuState* s);
// The record of the call a parked green thread waits in (its result is
// what the call finds when it resumes), or null.
NdkResume* ndk_cpu_parked_record(NdkCpuState* s);
// From a host function: whether the calling green thread may park this call
// now (its outermost host call, no record slot short); then parks it (0) or
// not (-1). The record comes back to the host function when the call runs
// again (ndk_cpu_resumed, once per call: the host function takes it).
int ndk_cpu_can_park(void);
int ndk_cpu_park(const NdkResume* rec);
NdkResume* ndk_cpu_resumed(void);
// From a host function of a green thread at its outermost level: the thread
// ends when the call returns (pthread_exit). -1 if not possible here.
int ndk_cpu_green_end(void);

#ifdef __cplusplus
}
#endif

#endif  // ARC_WASM_SHIM_NDK_CPU_NDK_CPU_EXT_H_
