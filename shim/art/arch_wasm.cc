// The architecture layer of ART (art/runtime/arch/<isa>/) for WebAssembly.
//
// ART on a device runs compiled ("quick") code and enters it, and leaves
// it, through assembly stubs: the invoke stub, trampolines (resolution, JNI
// lookup, quick-to-interpreter, generic JNI, proxy, IMT conflict),
// instrumentation, allocation and other quick entry points, all reached by
// address from the ArtMethod entry points and the Thread's entry point
// tables. In this port nothing is compiled: every method runs on the
// interpreter (see ArtMethod::Invoke and interpreter.cc in
// patches/aosp60/art.patch), natives are called from C++ through JavaScript
// (shim/art/jni_call_wasm.cc). The stubs below exist only so that the
// runtime can install and compare their addresses; each is a distinct
// function (WebAssembly gives each one its own table slot) that aborts if it
// is ever called.
//
// Also here: what arch/<isa>/{entrypoints_init,thread,fault_handler}_<isa>.cc
// provide. There are no signals in a browser, so the fault handlers (implicit
// null/suspend/stack overflow checks of compiled code) are never installed.
// Part of the arc-wasm shim layer.

#include "arch/context.h"
#include "base/logging.h"
#include "entrypoints/interpreter/interpreter_entrypoints.h"
#include "entrypoints/jni/jni_entrypoints.h"
#include "entrypoints/quick/quick_alloc_entrypoints.h"
#include "entrypoints/quick/quick_entrypoints.h"
#include "fault_handler.h"
#include "interpreter/interpreter.h"
#include "thread.h"

namespace {

[[noreturn]] void Unreachable(const char* stub) {
  LOG(art::FATAL) << "arc-wasm: " << stub
             << " called: there is no compiled code; everything runs on the interpreter";
  __builtin_trap();
}

}  // namespace

// Each stub has the C signature its declaration in ART has (WebAssembly
// checks signatures at link time, even of functions only used by address).
#define ART_WASM_STUB(ret, name, ...) \
  extern "C" ret name(__VA_ARGS__) { Unreachable(#name); }

namespace art {
class ArtMethod;
namespace mirror {
class Class;
}
}  // namespace art
using art::ArtMethod;
using art::mirror::Class;

ART_WASM_STUB(void*, art_jni_dlsym_lookup_stub, JNIEnv*, jobject)
ART_WASM_STUB(void, art_quick_imt_conflict_trampoline, ArtMethod*)
ART_WASM_STUB(void, art_quick_to_interpreter_bridge, ArtMethod*)
ART_WASM_STUB(void, art_quick_generic_jni_trampoline, ArtMethod*)
ART_WASM_STUB(void, art_quick_proxy_invoke_handler)
ART_WASM_STUB(void, art_quick_resolution_trampoline, ArtMethod*)
ART_WASM_STUB(void, art_quick_deoptimize)
ART_WASM_STUB(void, art_quick_deoptimize_from_compiled_slow_path)
ART_WASM_STUB(void, art_quick_instrumentation_entry, void*)
ART_WASM_STUB(void, art_quick_instrumentation_exit)
ART_WASM_STUB(void, art_quick_invoke_stub, ArtMethod*, uint32_t*, uint32_t, art::Thread*,
              art::JValue*, const char*)
ART_WASM_STUB(void, art_quick_invoke_static_stub, ArtMethod*, uint32_t*, uint32_t, art::Thread*,
              art::JValue*, const char*)
ART_WASM_STUB(void, art_quick_entrypoint)

// arch/quick_alloc_entrypoints.S: every allocation entry point for every
// allocator (only installed in the Thread's quick entry point table).
#define ART_WASM_ALLOC_STUBS(suffix)                                                      \
  ART_WASM_STUB(void*, art_quick_alloc_array##suffix, uint32_t, int32_t, ArtMethod*)      \
  ART_WASM_STUB(void*, art_quick_alloc_array_resolved##suffix, Class*, int32_t, ArtMethod*) \
  ART_WASM_STUB(void*, art_quick_alloc_array_with_access_check##suffix, uint32_t, int32_t, \
                ArtMethod*)                                                               \
  ART_WASM_STUB(void*, art_quick_alloc_object##suffix, uint32_t, ArtMethod*)              \
  ART_WASM_STUB(void*, art_quick_alloc_object_resolved##suffix, Class*, ArtMethod*)       \
  ART_WASM_STUB(void*, art_quick_alloc_object_initialized##suffix, Class*, ArtMethod*)    \
  ART_WASM_STUB(void*, art_quick_alloc_object_with_access_check##suffix, uint32_t, ArtMethod*) \
  ART_WASM_STUB(void*, art_quick_check_and_alloc_array##suffix, uint32_t, int32_t, ArtMethod*) \
  ART_WASM_STUB(void*, art_quick_check_and_alloc_array_with_access_check##suffix, uint32_t, \
                int32_t, ArtMethod*)                                                      \
  ART_WASM_STUB(void*, art_quick_alloc_string_from_bytes##suffix, void*, int32_t, int32_t, \
                int32_t)                                                                  \
  ART_WASM_STUB(void*, art_quick_alloc_string_from_chars##suffix, int32_t, int32_t, void*) \
  ART_WASM_STUB(void*, art_quick_alloc_string_from_string##suffix, void*)
#define ART_WASM_ALLOC_STUBS_BOTH(suffix) \
  ART_WASM_ALLOC_STUBS(suffix)            \
  ART_WASM_ALLOC_STUBS(suffix##_instrumented)
ART_WASM_ALLOC_STUBS_BOTH(_dlmalloc)
ART_WASM_ALLOC_STUBS_BOTH(_rosalloc)
ART_WASM_ALLOC_STUBS_BOTH(_bump_pointer)
ART_WASM_ALLOC_STUBS_BOTH(_tlab)
ART_WASM_ALLOC_STUBS_BOTH(_region)
ART_WASM_ALLOC_STUBS_BOTH(_region_tlab)

namespace art {

// arch/<isa>/entrypoints_init_<isa>.cc. The interpreter's entry points are
// real; the quick ones all point at art_quick_entrypoint (unreachable).
void InitEntryPoints(InterpreterEntryPoints* ipoints, JniEntryPoints* jpoints,
                     QuickEntryPoints* qpoints) {
  ipoints->pInterpreterToInterpreterBridge = artInterpreterToInterpreterBridge;
  ipoints->pInterpreterToCompiledCodeBridge = artInterpreterToCompiledCodeBridge;
  jpoints->pDlsymLookup = art_jni_dlsym_lookup_stub;
  void** q = reinterpret_cast<void**>(qpoints);
  for (size_t i = 0; i < sizeof(*qpoints) / sizeof(void*); ++i) {
    q[i] = reinterpret_cast<void*>(art_quick_entrypoint);
  }
  ResetQuickAllocEntryPoints(qpoints);
}

// arch/context.cc + arch/<isa>/context_<isa>.cc: the register context of
// a quick frame, used to long-jump into compiled code (exception delivery,
// deoptimization) and handed to stack walks (Thread::VisitRoots). Shadow
// frames have no registers, so this context has none either.
namespace {

class WasmContext FINAL : public Context {
 public:
  void Reset() OVERRIDE {}
  void FillCalleeSaves(const StackVisitor&) OVERRIDE {}
  void SetSP(uintptr_t) OVERRIDE {}
  void SetPC(uintptr_t) OVERRIDE {}
  bool IsAccessibleGPR(uint32_t) OVERRIDE { return false; }
  uintptr_t* GetGPRAddress(uint32_t) OVERRIDE { return nullptr; }
  uintptr_t GetGPR(uint32_t) OVERRIDE { return kBadGprBase; }
  void SetGPR(uint32_t, uintptr_t) OVERRIDE {}
  bool IsAccessibleFPR(uint32_t) OVERRIDE { return false; }
  uintptr_t GetFPR(uint32_t) OVERRIDE { return kBadFprBase; }
  void SetFPR(uint32_t, uintptr_t) OVERRIDE {}
  void SmashCallerSaves() OVERRIDE {}
  NO_RETURN void DoLongJump() OVERRIDE { Unreachable("Context::DoLongJump"); }
};

}  // namespace

Context* CreateWasmContext() {
  return new WasmContext();
}

// arch/<isa>/thread_<isa>.cc: nothing to set up (no segment registers or
// thread pointer register for compiled code).
void Thread::InitCpu() {}
void Thread::CleanupCpu() {}

// arch/<isa>/fault_handler_<isa>.cc: the handlers of the implicit checks.
// Runtime::Init does not enable implicit checks on wasm, so the fault
// manager is never initialized and none of these runs.
void FaultManager::HandleNestedSignal(int, siginfo_t*, void*) {
  Unreachable("FaultManager::HandleNestedSignal");
}

void FaultManager::GetMethodAndReturnPcAndSp(siginfo_t*, void*, ArtMethod** out_method,
                                             uintptr_t* out_return_pc, uintptr_t* out_sp) {
  *out_method = nullptr;
  *out_return_pc = 0;
  *out_sp = 0;
}

bool NullPointerHandler::Action(int, siginfo_t*, void*) { return false; }
bool SuspensionHandler::Action(int, siginfo_t*, void*) { return false; }
bool StackOverflowHandler::Action(int, siginfo_t*, void*) { return false; }

}  // namespace art
