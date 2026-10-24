// The contract between the ARM CPU (topic "arm-cpu", shim/ndk/cpu/) and the
// NDK bridge (topic "ndk-bridge": ELF loader, bionic/GLES/JNI thunks).
// Together they run Android NDK libraries built for ARM (armeabi first,
// then armeabi-v7a) inside the wasm process, the role ARC's closed
// ndk_translation played. Part of the arc-wasm shim layer.
//
// Memory model: guest addresses ARE host addresses. Guest code and data are
// loaded into ordinary wasm linear memory (mmap through the kernel layer),
// so a guest pointer can be passed to a host function (strlen, glTexImage2D,
// a JNI call) unchanged. Only ABI differences need converting (bionic vs
// musl struct layouts, errno, stdio, va_list, soft-float).
//
// Calling convention: AAPCS, little-endian. armeabi is soft-float (double
// in register pairs r0:r1 / r2:r3, aligned); armeabi-v7a "softfp" passes
// floats the same way, in core registers.
#ifndef ARC_WASM_SHIM_NDK_NDK_CPU_H_
#define ARC_WASM_SHIM_NDK_NDK_CPU_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The guest-visible CPU state of one guest thread. Fields other code may
// read or write are listed; the CPU may keep more after them.
typedef struct NdkCpuState {
  uint32_t r[16];      // r0-r12, sp (r13), lr (r14), pc (r15)
  uint32_t cpsr;       // N Z C V Q, T (Thumb) bit, GE, ...
  uint64_t d[32];      // VFP/NEON registers (armeabi-v7a)
  uint32_t fpscr;
  uint32_t tls;        // TPIDRURO: what bionic's __get_tls() returns
  // (CPU-private state follows.)
} NdkCpuState;

// A green thread's parked host call (shim/ndk/cpu/ndk_cpu_ext.h, topic
// threads-per-worker): what the host function left for when the call runs
// again.
typedef struct NdkResume {
  int32_t site;     // which wait (the bridge's numbering)
  int32_t result;   // set when it resumes (the wait's result: 0, -ETIMEDOUT, -EINTR, ...)
  uint32_t u[4];    // the host function's own
} NdkResume;
enum NdkPark { kNdkParkNone = 0, kNdkParkCall = 1, kNdkParkYield = 2, kNdkParkExit = 3 };

// A host function guest code can call. It reads its arguments from
// state->r[0..3] and the guest stack (state->r[13]) per AAPCS and stores
// its result in r[0] (and r[1] for 64-bit results). The CPU then returns
// to the guest at lr, as if a guest function had returned.
typedef void (*NdkHostFn)(NdkCpuState* state, void* user);

// Returns a guest code address that, when the guest branches to it (BL,
// BLX, BX, a load into pc; ARM or Thumb state), runs |fn| on the calling
// guest thread and returns to lr. Used for imported functions (PLT/GOT
// entries), JNIEnv function tables and callbacks. Stable for the process
// lifetime; |name| is for traces and crash reports. Thread-safe.
uint32_t ndk_host_stub(NdkHostFn fn, void* user, const char* name);

// Runs the guest function at |addr| (bit 0 set: Thumb) on the calling host
// thread's guest context, with |nargs| 32-bit arguments placed per AAPCS
// (r0-r3, then the stack), until it returns. Returns r0 | (uint64_t)r1 << 32.
// Re-entrant: guest -> host stub -> ndk_call_guest -> ... nests on the same
// guest stack.
uint64_t ndk_call_guest(uint32_t addr, const uint32_t* args, int nargs);

// The calling host thread's guest context, created on first use with its
// own guest stack (and tls slot, which the bridge sets up for bionic).
NdkCpuState* ndk_current_state(void);

// Tells the CPU that guest code in [start, start + size) changed (loaded,
// relocated, patched), so cached decodings/translations of it are dropped.
void ndk_flush_code(uint32_t start, size_t size);

// Called by the CPU for faults it cannot handle (undefined instruction,
// unaligned access it does not support, a branch to address 0, ...).
// The default prints the guest pc, registers and the nearest symbol known
// through ndk_set_symbolizer, then aborts.
typedef const char* (*NdkSymbolizer)(uint32_t addr, uint32_t* offset);
void ndk_set_symbolizer(NdkSymbolizer fn);

#ifdef __cplusplus
}
#endif

#endif  // ARC_WASM_SHIM_NDK_NDK_CPU_H_
