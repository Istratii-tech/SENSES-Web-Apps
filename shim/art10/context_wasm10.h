// The register context of ART 10 on WebAssembly (arch/<isa>/context_<isa>.h):
// used to long-jump into compiled code (exception delivery, deoptimization)
// and handed to stack walks (Thread::VisitRoots). Nothing is compiled and
// shadow frames have no registers, so this context has none either.
// Included by arch/context-inl.h (patches/aosp10/art10.patch). Part of the
// arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_ART10_CONTEXT_WASM10_H_
#define ARC_WASM_SHIM_ART10_CONTEXT_WASM10_H_

#include "arch/context.h"

namespace art {

[[noreturn]] void ArtWasmUnreachable(const char* what);  // arch_wasm10.cc

class WasmContext final : public Context {
 public:
  void Reset() override {}
  void FillCalleeSaves(uint8_t*, const QuickMethodFrameInfo&) override {}
  void SetSP(uintptr_t) override {}
  void SetPC(uintptr_t) override {}
  void SetArg0(uintptr_t) override {}
  bool IsAccessibleGPR(uint32_t) override { return false; }
  uintptr_t* GetGPRAddress(uint32_t) override { return nullptr; }
  uintptr_t GetGPR(uint32_t) override { return kBadGprBase; }
  void SetGPR(uint32_t, uintptr_t) override {}
  bool IsAccessibleFPR(uint32_t) override { return false; }
  uintptr_t GetFPR(uint32_t) override { return kBadFprBase; }
  void SetFPR(uint32_t, uintptr_t) override {}
  void SmashCallerSaves() override {}
  NO_RETURN void DoLongJump() override { ArtWasmUnreachable("Context::DoLongJump"); }
};

}  // namespace art

#endif  // ARC_WASM_SHIM_ART10_CONTEXT_WASM10_H_
