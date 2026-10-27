// The contract between the AArch64 CPU (topic "ndk-arm64", shim/ndk/cpu64/)
// and the arm64-v8a half of the NDK bridge (shim/ndk: ELF64 loader, bionic
// LP64 thunks, the 64-bit JNIEnv). The 64-bit counterpart of ndk_cpu.h; both
// CPUs live in one process (a VM runs 32-bit and 64-bit apps side by side).
//
// Memory model: guest addresses ARE wasm addresses, below 4 GB. Pointers in
// X registers are 64 bits wide with the upper 32 bits zero; the CPU uses the
// low 32 bits of every data address (so a tag in the top byte, as with
// TBI, is ignored too). Host pointers passed to the guest are zero-extended.
//
// Calling convention: AAPCS64 (Linux/Android flavour), little-endian:
// integer and pointer arguments in x0-x7, floating-point ones in v0-v7,
// the rest in 8-byte stack slots; results in x0 (x1) or v0.
#ifndef ARC_WASM_SHIM_NDK_NDK_CPU64_H_
#define ARC_WASM_SHIM_NDK_NDK_CPU64_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The guest-visible CPU state of one guest thread. The CPU may keep more
// after these fields.
typedef struct NdkCpu64State {
  uint64_t x[31];          // x0-x30 (x29 = fp, x30 = lr)
  uint64_t sp;             // follows x30: the CPU indexes it as register 31
  uint64_t reserved_[2];   // CPU-internal (the zero register, a sink for writes to it)
  uint64_t pc;
  uint32_t nzcv;           // N Z C V in bits 31..28: current while a host function runs
  uint32_t fpcr;
  uint32_t fpsr;
  uint32_t pad_;
  uint64_t tpidr;          // TPIDR_EL0: what bionic's __get_tls() returns
  uint64_t v[32][2];       // V0-V31, [0] = low 64 bits (D/S/H/B views), [1] = high
} NdkCpu64State;

// A host function guest code can call. It reads its arguments from x0-x7,
// v0-v7 and the stack (sp) and stores its result in x0 (x1) or v[0]. The
// CPU then returns to the guest at x30, unless the function changed
// state->pc (it holds the stub's address when the function is called):
// then execution continues there (longjmp, thread exit).
typedef void (*Ndk64HostFn)(NdkCpu64State* state, void* user);

// Returns a guest code address that runs |fn| when guest code branches to
// it (BL, BLR, BR, RET). Stable for the process lifetime; |name| is for
// traces and crash reports. Thread-safe.
uint32_t ndk64_host_stub(Ndk64HostFn fn, void* user, const char* name);
// The name of the host stub at |addr|, or null.
const char* ndk64_host_stub_name(uint32_t addr);
// Whether |addr| is one of the host stubs.
int ndk64_is_host_stub(uint32_t addr);

// The arguments of a guest call.
typedef struct Ndk64Args {
  uint64_t x[8];           // x0-x7
  uint64_t v[8];           // v0-v7, low 64 bits (a float in the low 32)
  const uint64_t* stack;   // 8-byte stack slots, the first at the call's sp
  int nstack;
} Ndk64Args;
typedef struct Ndk64Result {
  uint64_t x0, x1;
  uint64_t v0, v0_hi;
} Ndk64Result;

// Runs the guest function at |addr| on the calling host thread's guest
// context until it returns. Re-entrant (guest -> host stub -> guest -> ...
// nests on the same guest stack); x0-x30, sp, flags and v0-v7 of the
// interrupted guest code are restored afterwards, so a host function that
// calls back into the guest still finds its own arguments.
void ndk64_call_guest_args(uint32_t addr, const Ndk64Args* args, Ndk64Result* result);
// Integer arguments only: x0-x7 then the stack; returns x0.
uint64_t ndk64_call_guest(uint32_t addr, const uint64_t* args, int nargs);
// ndk64_call_guest_args that a fatal guest error can end (abort(), a call to
// an unresolved symbol, a CPU fault: ndk64_leave_guest). Returns 1 when the
// call returned, 0 (with |why|) when it was left; the CPU state is then as
// before the call.
int ndk64_call_guest_guarded(uint32_t addr, const Ndk64Args* args, Ndk64Result* result, char* why,
                             size_t why_size);
// Leaves the innermost ndk64_call_guest_guarded of the calling thread;
// returns if there is none (the caller then aborts).
void ndk64_leave_guest(const char* why);
// Whether the calling thread is inside a guarded 64-bit guest call.
int ndk64_in_guarded_call(void);
// Continues the guest at a full saved state (not a fresh call): a vfork child
// resumes the parent's code after the clone SVC (topic native-exec). 1
// normally, 0 with |why| on a fault.
int ndk64_resume_guest(const NdkCpu64State* st, char* why, size_t why_size);

// The calling host thread's guest context, created on first use with its
// own guest stack.
NdkCpu64State* ndk64_current_state(void);

// Guest code in [start, start + size) changed: cached decodings and
// translations of it are dropped.
void ndk64_flush_code(uint32_t start, size_t size);
// The bridge made its first reservation window (ndk64_window_table,
// shim/ndk/ndk_libc.cc): all blocks go, translations look windows up.
void ndk64_windows_on(void);

// Names guest addresses in fault reports ("lib.so!symbol").
typedef const char* (*Ndk64Symbolizer)(uint32_t addr, uint32_t* offset);
void ndk64_set_symbolizer(Ndk64Symbolizer fn);

// Raw Linux syscalls (SVC #0, number in x8, arguments in x0-x5). The handler
// returns nonzero if it handled the call (result in x0, negative errno on
// failure); otherwise the CPU's own small table runs (exit, write, getpid,
// gettid, clock_gettime, nanosleep, sched_yield) and anything else returns
// -ENOSYS with a one-time message.
typedef int (*Ndk64SyscallHandler)(NdkCpu64State* state);
void ndk64_set_syscall_handler(Ndk64SyscallHandler fn);

// Replaces the default report-and-abort for faults; must not return.
typedef void (*Ndk64FaultHandler)(NdkCpu64State* state, uint32_t pc, const char* message);
void ndk64_set_fault_handler(Ndk64FaultHandler fn);

// Guest stacks of threads created from now on (default 1 MiB); the calling
// thread's only, if its guest state does not exist yet (0: the default).
void ndk64_set_guest_stack_size(size_t bytes);
void ndk64_set_thread_guest_stack_size(size_t bytes);
void ndk64_guest_stack(uint32_t* base, uint32_t* size);

// Translation of hot blocks to wasm after |threshold| executions (default
// 50; 0 = interpreter only). Needs -sALLOW_TABLE_GROWTH in the final link.
void ndk64_set_jit(int threshold);
// Translations one thread instantiates at most (-1: by the memory budget or
// the browser), as ndk_set_jit_max of the ARM CPU.
void ndk64_set_jit_max(int max);

// Statistics and debugging.
uint64_t ndk64_instruction_count(void);
typedef struct Ndk64CpuStats {
  uint64_t blocks_decoded;
  uint64_t insns_decoded;
  uint64_t blocks_flushed;
  uint64_t host_calls;
  uint64_t blocks_translated;
  uint64_t translated_bytes;
  uint64_t blocks_not_translatable;
  uint64_t flush_calls;   // ndk64_flush_code (IC IVAU, mprotect, loads ...)
  uint64_t blocks_kept;   // blocks in flushed ranges whose code was the same
} Ndk64CpuStats;
void ndk64_cpu_stats(Ndk64CpuStats* out);
// Logs calls of host functions whose name contains one of the
// comma-separated |words| (the first 400 of each and every 1000th).
void ndk64_watch_host_calls(const char* words);
// Prints the calling thread's last host calls to stderr.
void ndk64_report_recent_calls(void);
// Every thread with an AArch64 CPU (racy reads): where it is, its signals, last host calls.
void ndk64_cpu_dump_threads(void);
// |hook| runs for every guest block decoded from now on ([start, end)).
typedef void (*Ndk64BlockHook)(uint32_t start, uint32_t end);
void ndk64_set_block_hook(Ndk64BlockHook hook);
// Decodes the instruction word at |addr| without running it: 1 if the CPU
// knows it, 0 if it would fault (static coverage of a library).
int ndk64_decodable(uint32_t addr, char* why, size_t why_size);

// --- signals (shim/ndk/cpu64/signal64.cc) -------------------------------------------
// As the ARMv7 CPU's ndk_sig_* (shim/ndk/cpu/ndk_cpu_ext.h), for threads running
// arm64 code: Linux numbers (1..64), negative Linux errno values. The bridge's
// API functions (shim/ndk/cpu/signal.cc) forward here when ndk64_sig_active().
typedef struct Ndk64Sigaction {
  uint64_t handler;   // 0 SIG_DFL, 1 SIG_IGN, else a guest function
  uint64_t flags;     // SA_*
  uint64_t mask;      // signals blocked while it runs (bit sig - 1)
} Ndk64Sigaction;
int ndk64_sig_active(void);   // the calling thread runs (or ran) arm64 code
int ndk64_sig_action(int sig, const Ndk64Sigaction* act, Ndk64Sigaction* old);
// pthread_create made |thread|: it takes signals before its first guest code.
void ndk64_sig_thread_created(uint32_t thread);
int ndk64_sig_send(int tid, int sig);
int ndk64_sig_send_pthread(uint32_t thread, int sig);
int ndk64_sig_mask(int how, const uint64_t* set, uint64_t* old);
uint64_t ndk64_sig_pending(void);
int ndk64_sig_deliver(void);
int ndk64_sig_wait(uint32_t addr, uint32_t val, double ms);
int ndk64_sig_sleep(double ms, double* left_ms);
int ndk64_sig_suspend(const uint64_t* mask);
int ndk64_sig_wait_set(uint64_t set, double ms);
int ndk64_sig_check(void);
void ndk64_sig_self(uint32_t* thread, int* tid);
int ndk64_sig_raise(int sig);

#ifdef __cplusplus
}
#endif

#endif  // ARC_WASM_SHIM_NDK_NDK_CPU64_H_
