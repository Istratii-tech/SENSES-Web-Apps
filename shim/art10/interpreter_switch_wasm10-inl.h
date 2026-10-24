// The switch interpreter of ART 10 on WebAssembly, with flat calls: included
// by art/runtime/interpreter/interpreter_switch_impl-inl.h in place of AOSP's
// ExecuteSwitchImplCpp (patches/aosp10/art10.patch), inside namespace
// art::interpreter, after the class InstructionHandler.
//
// The design is ART 8.1's (topic art8, docs/topics/art8.md "Плоские вызовы и
// ускорение"; from ART 6.0.1, topics art and art-speed):
//
// * Flat calls. A call from an interpreted method to an interpreted method
//   does not nest C++ frames (WebAssembly's stack is small and a deep Java
//   recursion would overflow it; each nested ExecuteSwitchImpl also costs
//   time). The callee's frame comes from the thread's frame stack
//   (shim/art/interpreter_frames_wasm.cc, with its code item accessor in
//   front of it: shim/art10/interpreter_frames_wasm10.h) and this loop
//   continues in it; its return continues the caller after the invoke, an
//   exception it does not catch unwinds into the caller's handler.
// * The frame's dex pc is stored by the instructions that may look at it
//   (anything that can throw, call, allocate or suspend the thread), not by
//   moves, constants, arithmetic, comparisons and branches.
// * Branches and returns have no calls in the loop: a backward branch with
//   the thread's suspend flags set, and every kYieldBranches-th one, leaves
//   the loop ("yields"); ExecuteSwitchImplCpp runs the suspend check and
//   enters it again. V8 has no on-stack replacement for WebAssembly: a call
//   that started in baseline code stays there until it returns, and with
//   flat calls the outermost loop runs a thread's whole Java call tree.
// * Listeners (a debugger: dex pc and branch events) are not called from the
//   flat loop: when there are some it yields, and the rest of the call (its
//   flat frames too) runs in the loop with access checks, which calls them
//   for every instruction as AOSP does.
// * Fast paths: invoke-* of a resolved method (no DoInvoke, no
//   DoCallCommon), intrinsics of the interpreter (MterpHandleIntrinsic),
//   natives (ArtWasmCallNativeFromLoop), empty direct methods, quickened and
//   static field access.
//
// Unlike 8.1's, which rewrote AOSP's loop, this one keeps ART 10's
// InstructionHandler for the semantics of every instruction: the loop runs a
// handler for the current frame (its accessor and result register are the
// loop's) and only adds the above around it. New instructions (dex 039:
// const-method-handle, const-method-type, invoke-custom, invoke-polymorphic)
// are AOSP's code as they are.
//
// Part of the arc-wasm shim layer.

#ifndef ARC_WASM_SHIM_ART10_INTERPRETER_SWITCH_WASM10_INL_H_
#define ARC_WASM_SHIM_ART10_INTERPRETER_SWITCH_WASM10_INL_H_

#include <atomic>

#include "imtable-inl.h"
#include "interpreter/interpreter_intrinsics.h"
#include "intrinsics_enum.h"

// --- helpers --------------------------------------------------------------------

// ArtWasmTryFlatInvoke's results besides a frame (small values, never frame
// addresses):
// - a native method (|*native| is set): ArtWasmCallNativeFromLoop runs it;
static ShadowFrame* const kArtWasmNativeCall = reinterpret_cast<ShadowFrame*>(16);
// - a method marked intrinsic (|*native| is set; non-range invokes only):
//   MterpHandleIntrinsic (interpreter_intrinsics.cc) computes it from the
//   caller's registers, or punts (an exception to throw, a supplementary
//   character) to the usual call;
static ShadowFrame* const kArtWasmIntrinsicCall = reinterpret_cast<ShadowFrame*>(24);
// - invoke-direct of a method whose code is one return-void (Object.<init>
//   of every object allocation, empty constructors and private methods):
//   nothing to run. As Dalvik's invoke-object-init: in the flat loop nobody
//   listens to the call (ArtWasmCanCallFlat), and ART registers finalizable
//   objects when it allocates them, not in Object.<init>.
static ShadowFrame* const kArtWasmEmptyCall = reinterpret_cast<ShadowFrame*>(8);
// - the class initialization of a static method threw (the frame is gone,
//   the exception pending).
static ShadowFrame* const kArtWasmEnterFailed = reinterpret_cast<ShadowFrame*>(32);

// AssignRegister of interpreter_common.cc.
ALWAYS_INLINE static void ArtWasmAssignRegister(ShadowFrame* new_shadow_frame,
                                                const ShadowFrame& shadow_frame,
                                                size_t dest_reg, size_t src_reg)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  uint32_t src_value = shadow_frame.GetVReg(src_reg);
  mirror::Object* o = shadow_frame.GetVRegReference<kVerifyNone>(src_reg);
  if (src_value == reinterpret_cast<uintptr_t>(o)) {
    new_shadow_frame->SetVRegReference(dest_reg, o);
  } else {
    new_shadow_frame->SetVReg(dest_reg, src_value);
  }
}

// The IMT index of an interface method. ART 10 caches it in abstract
// methods (ArtMethod::imt_index_); a default method's is computed from
// hashes of its class descriptor, name and signature on every
// invoke-interface (compiled code has it built in): cached here per method
// (lock-free, direct-mapped; a 64-bit atomic holds method and index
// together, so a reader never sees one with the other's partner).
ALWAYS_INLINE static uint32_t ArtWasmImtIndex(ArtMethod* interface_method)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (LIKELY(interface_method->IsAbstract())) {
    return interface_method->GetImtIndex();
  }
  constexpr size_t kBits = 12;
  static std::atomic<uint64_t> cache[1u << kBits];
  const uint32_t key = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(interface_method));
  std::atomic<uint64_t>& slot = cache[(key * 2654435761u) >> (32 - kBits)];
  const uint64_t v = slot.load(std::memory_order_relaxed);
  if (LIKELY(static_cast<uint32_t>(v) == key)) {
    return static_cast<uint32_t>(v >> 32);
  }
  const uint32_t index = ImTable::GetImtIndex(interface_method);
  slot.store((static_cast<uint64_t>(index) << 32) | key, std::memory_order_relaxed);
  return index;
}

// What ArtInterpreterToInterpreterBridge does before running a method: push
// its frame, initialize the class of a static method. On failure the frame
// is gone and an exception is pending.
NO_INLINE static bool ArtWasmFlatEnterInitialize(Thread* self, ShadowFrame* callee,
                                                 ObjPtr<mirror::Class> declaring_class)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  StackHandleScope<1> hs(self);
  HandleWrapperObjPtr<mirror::Class> h_declaring_class(hs.NewHandleWrapper(&declaring_class));
  if (UNLIKELY(!Runtime::Current()->GetClassLinker()->EnsureInitialized(
      self, h_declaring_class, true, true))) {
    DCHECK(self->IsExceptionPending());
    self->PopShadowFrame();
    ArtWasmFreeFlatFrame(callee);
    return false;
  }
  return true;
}

// A frame DoCallCommon built for a flat call: push it, initialize the class
// of a static method. False with an exception pending (the frame is gone).
ALWAYS_INLINE static bool ArtWasmFlatEnter(Thread* self, ShadowFrame* callee)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  self->PushShadowFrame(callee);
  ArtMethod* const method = callee->GetMethod();
  if (method->IsStatic()) {
    ObjPtr<mirror::Class> declaring_class = method->GetDeclaringClass();
    if (UNLIKELY(!declaring_class->IsInitialized())) {
      return ArtWasmFlatEnterInitialize(self, callee, declaring_class);
    }
  }
  return true;
}

// The intrinsics MterpHandleIntrinsic (interpreter_intrinsics.cc) computes
// from the caller's registers. ART 10 marks every method of intrinsics_list.h
// intrinsic (InitializeIntrinsics, as a boot image would have them); the
// others (Integer.valueOf, StringBuilder.append ...) are run as the methods
// they are. (8.1's ArtWasmInstallIntrinsics marked only these.)
constexpr bool ArtWasmIsMterpIntrinsic(Intrinsics intrinsic) {
  switch (intrinsic) {
    case Intrinsics::kIntegerReverse:
    case Intrinsics::kIntegerReverseBytes:
    case Intrinsics::kIntegerBitCount:
    case Intrinsics::kIntegerCompare:
    case Intrinsics::kIntegerHighestOneBit:
    case Intrinsics::kIntegerLowestOneBit:
    case Intrinsics::kIntegerNumberOfLeadingZeros:
    case Intrinsics::kIntegerNumberOfTrailingZeros:
    case Intrinsics::kIntegerRotateRight:
    case Intrinsics::kIntegerRotateLeft:
    case Intrinsics::kIntegerSignum:
    case Intrinsics::kLongReverse:
    case Intrinsics::kLongReverseBytes:
    case Intrinsics::kLongBitCount:
    case Intrinsics::kLongCompare:
    case Intrinsics::kLongHighestOneBit:
    case Intrinsics::kLongLowestOneBit:
    case Intrinsics::kLongNumberOfLeadingZeros:
    case Intrinsics::kLongNumberOfTrailingZeros:
    case Intrinsics::kLongRotateRight:
    case Intrinsics::kLongRotateLeft:
    case Intrinsics::kLongSignum:
    case Intrinsics::kShortReverseBytes:
    case Intrinsics::kMathAbsDouble:
    case Intrinsics::kMathAbsFloat:
    case Intrinsics::kMathAbsLong:
    case Intrinsics::kMathAbsInt:
    case Intrinsics::kMathMinLongLong:
    case Intrinsics::kMathMinIntInt:
    case Intrinsics::kMathMaxLongLong:
    case Intrinsics::kMathMaxIntInt:
    case Intrinsics::kMathCos:
    case Intrinsics::kMathSin:
    case Intrinsics::kMathAcos:
    case Intrinsics::kMathAsin:
    case Intrinsics::kMathAtan:
    case Intrinsics::kMathTan:
    case Intrinsics::kMathSqrt:
    case Intrinsics::kMathCeil:
    case Intrinsics::kMathFloor:
    case Intrinsics::kStringCharAt:
    case Intrinsics::kStringCompareTo:
    case Intrinsics::kStringEquals:
    case Intrinsics::kStringGetCharsNoCheck:
    case Intrinsics::kStringIndexOf:
    case Intrinsics::kStringIndexOfAfter:
    case Intrinsics::kStringIsEmpty:
    case Intrinsics::kStringLength:
    case Intrinsics::kVarHandleFullFence:
    case Intrinsics::kVarHandleAcquireFence:
    case Intrinsics::kVarHandleReleaseFence:
    case Intrinsics::kVarHandleLoadLoadFence:
    case Intrinsics::kVarHandleStoreStoreFence:
      return true;
    default:
      return false;
  }
}

// What DoInvoke (FindMethodToCall without access checks, then DoCallCommon's
// fast path) does for a call to an interpreted method resolved before (by
// DoInvoke, which keeps it in the thread's interpreter cache), when it can be
// flat: returns the callee's frame with the arguments, pushed (as
// ArtInterpreterToInterpreterBridge does, with the class of a static method
// initialized). nullptr
// for everything else (not in the cache, null receiver, not invokable,
// String.<init>, IMT conflict, invoke-super of an interface method, frame
// stack full): DoInvoke handles those as before. Not inlined: the loop function must stay small
// enough for Safari's optimizing tier (topic art8: 69 KB of wasm was too
// much, 58 KB was fine).
template<InvokeType type, bool is_range, bool is_quick>
NO_INLINE static ShadowFrame* ArtWasmTryFlatInvoke(Thread* self, ShadowFrame* caller,
                                                   const Instruction* inst, uint16_t inst_data,
                                                   ArtMethod** native)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  const uint32_t method_idx = is_range ? inst->VRegB_3rc() : inst->VRegB_35c();
  const uint32_t vregC = is_range ? inst->VRegC_3rc() : inst->VRegC_35c();
  ObjPtr<mirror::Object> receiver = nullptr;
  if (type != kStatic) {
    receiver = caller->GetVRegReference(vregC);
    if (UNLIKELY(receiver == nullptr)) {
      return nullptr;
    }
  }
  ArtMethod* m;
  if (is_quick) {
    // invoke-virtual-quick: the vtable index is in the instruction.
    m = receiver->GetClass()->GetEmbeddedVTableEntry(method_idx, kRuntimePointerSize);
  } else {
    size_t tls_value;
    ArtMethod* resolved;
    if (LIKELY(self->GetInterpreterCache()->Get(inst, &tls_value))) {
      resolved = reinterpret_cast<ArtMethod*>(tls_value);
    } else {
      // Not in the thread's small interpreter cache (256 entries by
      // instruction address: a loop body with many instructions evicts the
      // invokes around it): the method resolved before, from the dex cache
      // (as 8.1's flat loop); else DoInvoke.
      resolved = caller->GetMethod()->GetDexCache()->GetResolvedMethod(method_idx,
                                                                       kRuntimePointerSize);
      if (UNLIKELY(resolved == nullptr || resolved->IsRuntimeMethod())) {
        return nullptr;
      }
    }
    switch (type) {
      case kStatic:
      case kDirect:
        m = resolved;
        break;
      case kVirtual:
        // The class of an object is instantiable: its vtable is embedded.
        m = receiver->GetClass()->GetEmbeddedVTableEntry(resolved->GetMethodIndex(),
                                                          kRuntimePointerSize);
        break;
      case kInterface:
        if (UNLIKELY(resolved->GetDeclaringClass()->IsObjectClass())) {
          // An Object method through an interface type (FindMethodToCall:
          // the vtable; the IMT has only interface methods).
          m = receiver->GetClass()->GetEmbeddedVTableEntry(resolved->GetMethodIndex(),
                                                            kRuntimePointerSize);
          break;
        }
        m = receiver->GetClass()->GetImt(kRuntimePointerSize)->Get(
            ArtWasmImtIndex(resolved), kRuntimePointerSize);
        if (UNLIKELY(m->IsRuntimeMethod())) {  // conflict or unimplemented
          return nullptr;
        }
        break;
      case kSuper: {
        // FindMethodToCall<kSuper>: a method of a class (not a default method
        // of an interface, which DoInvoke looks up) is the referrer's
        // superclass's vtable entry. (libcore's StringBuilder.append calls
        // AbstractStringBuilder's this way.)
        if (UNLIKELY(resolved->GetDeclaringClass()->IsInterface())) {
          return nullptr;
        }
        ObjPtr<mirror::Class> super_class = caller->GetMethod()->GetDeclaringClass()->GetSuperClass();
        if (UNLIKELY(super_class == nullptr ||
                     resolved->GetMethodIndex() >= super_class->GetVTableLength())) {
          return nullptr;
        }
        m = super_class->GetVTableEntry(resolved->GetMethodIndex(), kRuntimePointerSize);
        break;
      }
      default:
        return nullptr;
    }
  }
  // The access flags once (an atomic load each time otherwise), with the
  // tests of IsIntrinsic, IsNative, IsInvokable, SkipAccessChecks,
  // MustCountLocks (ArtWasmCanCallFlat) on them.
  const uint32_t flags = m->GetAccessFlags();
  const bool intrinsic = (flags & kAccIntrinsic) != 0;
  if (!is_range && UNLIKELY(intrinsic) &&
      ArtWasmIsMterpIntrinsic(static_cast<Intrinsics>(
          (flags & kAccIntrinsicBits) >> CTZ(kAccIntrinsicBits)))) {
    *native = m;
    return kArtWasmIntrinsicCall;
  }
  if ((flags & kAccNative) != 0) {
    if (LIKELY((flags & kAccConstructor) == 0 && Runtime::Current()->IsStarted())) {
      *native = m;
      return kArtWasmNativeCall;
    }
    return nullptr;
  }
  const bool invokable =
      (flags & kAccAbstract) == 0 && (intrinsic || (flags & kAccDefaultConflict) == 0);
  const bool counts_locks = !intrinsic && (flags & kAccMustCountLocks) != 0;
  if (UNLIKELY(!invokable || (flags & kAccSkipAccessChecks) == 0 || counts_locks ||
               (type == kDirect && (flags & kAccConstructor) != 0 &&
                m->GetDeclaringClass()->IsStringClass()) ||
               !ArtWasmCanCallFlatRuntime(self, m))) {
    return nullptr;
  }
  ArtWasmCodeCacheEntry* const code_entry = ArtWasmMethodCodeEntry(m);
  const CodeItemDataAccessor& accessor = ArtWasmCodeEntryAccessor(code_entry);
  if (UNLIKELY(!accessor.HasCodeItem())) {
    return nullptr;
  }
  if (type == kDirect && accessor.InsnsSizeInCodeUnits() == 1 &&
      ((accessor.Insns()[0] & 0xff) == Instruction::RETURN_VOID ||
       (accessor.Insns()[0] & 0xff) == Instruction::RETURN_VOID_NO_BARRIER)) {
    return kArtWasmEmptyCall;
  }
  // A method with compiled code (topic dex-jit) runs out of the loop, like a
  // native one (ArtWasmCallNativeFromLoop); this counts calls towards
  // compiling it.
  if (UNLIKELY(ArtJitLookup(self, m) != nullptr) && ArtJitHasRoom()) {
    *native = m;
    return kArtWasmNativeCall;
  }
  const uint16_t num_regs = accessor.RegistersSize();
  const uint16_t num_ins = is_range ? inst->VRegA_3rc(inst_data) : inst->VRegA_35c(inst_data);
  DCHECK_EQ(num_ins, accessor.InsSize());
  ShadowFrame* callee = ArtWasmNewFlatFrame(accessor, num_regs, num_ins, caller, m);
  if (UNLIKELY(callee == nullptr)) {
    return nullptr;  // (DoInvoke throws StackOverflowError)
  }
  const size_t first_dest_reg = num_regs - num_ins;
  if (is_range) {
    for (size_t src_reg = vregC, dest_reg = first_dest_reg; dest_reg < num_regs;
         ++dest_reg, ++src_reg) {
      ArtWasmAssignRegister(callee, *caller, dest_reg, src_reg);
    }
  } else {
    uint16_t reg_list = inst->Fetch16(2);
    uint16_t count = num_ins;
    if (count == 5) {
      ArtWasmAssignRegister(callee, *caller, first_dest_reg + 4U, (inst_data >> 8) & 0x0f);
      --count;
    }
    for (size_t arg_index = 0; arg_index < count; ++arg_index, reg_list >>= 4) {
      ArtWasmAssignRegister(callee, *caller, first_dest_reg + arg_index, reg_list & 0x0f);
    }
  }
  // What ArtInterpreterToInterpreterBridge does before running a method:
  // push its frame, initialize the class of a static method.
  self->PushShadowFrame(callee);
  if ((flags & kAccStatic) != 0 && !code_entry->class_initialized) {
    ObjPtr<mirror::Class> declaring_class = m->GetDeclaringClass();
    if (UNLIKELY(!declaring_class->IsInitialized())) {
      if (!ArtWasmFlatEnterInitialize(self, callee, declaring_class)) {
        return kArtWasmEnterFailed;
      }
      // (Initializing: its <clinit> may be running on this thread. The class
      // is read again: the thread may have been suspended, and |declaring_class|
      // is not a handle.)
      if (!m->GetDeclaringClass()->IsInitialized()) {
        return callee;
      }
    }
    code_entry->class_initialized = true;
  }
  return callee;
}

// A native method from the flat loop (ArtWasmTryFlatInvoke found |m|): what
// DoCallCommon does for it (a frame with the arguments, ArtWasmCallNative),
// without FindMethodToCall and DoCallCommon's generality. The frame comes
// from the frame stack (LIFO, freed right after). False with an exception
// pending.
template<bool is_range>
NO_INLINE static bool ArtWasmCallNativeFromLoop(Thread* self, ShadowFrame* caller,
                                                const Instruction* inst, uint16_t inst_data,
                                                ArtMethod* m, JValue* result)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (!m->IsNative()) {
    // Compiled code (topic dex-jit; ArtWasmTryFlatInvoke).
    return ArtJitCallFromLoop(self, caller, inst, inst_data, m, nullptr, result, is_range);
  }
  const uint32_t vregC = is_range ? inst->VRegC_3rc() : inst->VRegC_35c();
  const uint16_t num_ins = is_range ? inst->VRegA_3rc(inst_data) : inst->VRegA_35c(inst_data);
  void* memory = ArtWasmFlatFrameAlloc(ShadowFrame::ComputeSize(num_ins));
  if (UNLIKELY(memory == nullptr)) {
    ThrowStackOverflowError(self);
    return false;
  }
  ShadowFrame* frame = ShadowFrame::CreateForCall(num_ins, num_ins, caller, m, memory);
  if (is_range) {
    for (size_t i = 0; i < num_ins; ++i) {
      ArtWasmAssignRegister(frame, *caller, i, vregC + i);
    }
  } else {
    uint16_t reg_list = inst->Fetch16(2);
    uint16_t count = num_ins;
    if (count == 5) {
      ArtWasmAssignRegister(frame, *caller, 4U, (inst_data >> 8) & 0x0f);
      --count;
    }
    for (size_t arg_index = 0; arg_index < count; ++arg_index, reg_list >>= 4) {
      ArtWasmAssignRegister(frame, *caller, arg_index, reg_list & 0x0f);
    }
  }
  ArtWasmCallNative(self, frame, result);
  ArtWasmFlatFrameFree(frame);
  return !self->IsExceptionPending();
}


// arc-wasm (art-speed): sget/sput (DoFieldGet/DoFieldPut of static fields)
// with the common case inline: the field is resolved (in the method's dex
// cache), not volatile, its class initialized; FindFieldFromCode without
// access checks would return the same field. (Field listeners: checked by
// the caller.)
ALWAYS_INLINE static ArtField* ArtWasmResolvedStaticField(const ShadowFrame* sf,
                                                          const Instruction* inst)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  ArtField* f = sf->GetMethod()->GetDexCache()->GetResolvedField(inst->VRegB_21c(),
                                                                 kRuntimePointerSize);
  if (UNLIKELY(f == nullptr || !f->IsStatic() || f->IsVolatile() ||
               !f->GetDeclaringClass()->IsInitialized())) {
    return nullptr;
  }
  return f;
}

// --- classification of instructions -----------------------------------------

enum class ArtWasmKind {
  kOther,      // the handler, after storing the dex pc
  kCold,       // the same out of the loop (ArtWasmColdInstruction)
  kReturn,     // return-*
  kArray,      // aget-*, aput-*, array-length
  kDiv,        // div-int, rem-int, div-long, rem-long (all formats)
  kNew,        // new-instance, new-array
  kConstString,  // const-string
  kTypeCheck,  // check-cast, instance-of
  kPure,       // the handler, without: cannot throw, call, allocate or suspend
  kIf22t,      // if-eq ... if-le
  kIf21z,      // if-eqz ... if-lez
  kGoto,       // goto, goto/16, goto/32
  kInvoke,     // invoke-* (format 35c/3rc, flat calls)
  kIGetQuick,  // iget-*-quick
  kIPutQuick,  // iput-*-quick
  kSGet,       // sget-*
  kSPut,       // sput-*
};

constexpr ArtWasmKind ArtWasmKindOf(Instruction::Code op) {
  switch (op) {
    case Instruction::NOP:
    case Instruction::MOVE: case Instruction::MOVE_FROM16: case Instruction::MOVE_16:
    case Instruction::MOVE_WIDE: case Instruction::MOVE_WIDE_FROM16: case Instruction::MOVE_WIDE_16:
    case Instruction::MOVE_OBJECT: case Instruction::MOVE_OBJECT_FROM16:
    case Instruction::MOVE_OBJECT_16:
    case Instruction::MOVE_RESULT: case Instruction::MOVE_RESULT_WIDE:
    case Instruction::MOVE_RESULT_OBJECT: case Instruction::MOVE_EXCEPTION:
    case Instruction::CONST_4: case Instruction::CONST_16: case Instruction::CONST:
    case Instruction::CONST_HIGH16: case Instruction::CONST_WIDE_16:
    case Instruction::CONST_WIDE_32: case Instruction::CONST_WIDE:
    case Instruction::CONST_WIDE_HIGH16:
    case Instruction::CMPL_FLOAT: case Instruction::CMPG_FLOAT: case Instruction::CMPL_DOUBLE:
    case Instruction::CMPG_DOUBLE: case Instruction::CMP_LONG:
    case Instruction::NEG_INT: case Instruction::NOT_INT: case Instruction::NEG_LONG:
    case Instruction::NOT_LONG: case Instruction::NEG_FLOAT: case Instruction::NEG_DOUBLE:
    case Instruction::INT_TO_LONG: case Instruction::INT_TO_FLOAT: case Instruction::INT_TO_DOUBLE:
    case Instruction::LONG_TO_INT: case Instruction::LONG_TO_FLOAT:
    case Instruction::LONG_TO_DOUBLE: case Instruction::FLOAT_TO_INT:
    case Instruction::FLOAT_TO_LONG: case Instruction::FLOAT_TO_DOUBLE:
    case Instruction::DOUBLE_TO_INT: case Instruction::DOUBLE_TO_LONG:
    case Instruction::DOUBLE_TO_FLOAT: case Instruction::INT_TO_BYTE: case Instruction::INT_TO_CHAR:
    case Instruction::INT_TO_SHORT:
    case Instruction::ADD_INT: case Instruction::SUB_INT: case Instruction::MUL_INT:
    case Instruction::AND_INT: case Instruction::OR_INT: case Instruction::XOR_INT:
    case Instruction::SHL_INT: case Instruction::SHR_INT: case Instruction::USHR_INT:
    case Instruction::ADD_LONG: case Instruction::SUB_LONG: case Instruction::MUL_LONG:
    case Instruction::AND_LONG: case Instruction::OR_LONG: case Instruction::XOR_LONG:
    case Instruction::SHL_LONG: case Instruction::SHR_LONG: case Instruction::USHR_LONG:
    case Instruction::ADD_FLOAT: case Instruction::SUB_FLOAT: case Instruction::MUL_FLOAT:
    case Instruction::DIV_FLOAT: case Instruction::REM_FLOAT:
    case Instruction::ADD_DOUBLE: case Instruction::SUB_DOUBLE: case Instruction::MUL_DOUBLE:
    case Instruction::DIV_DOUBLE: case Instruction::REM_DOUBLE:
    case Instruction::ADD_INT_2ADDR: case Instruction::SUB_INT_2ADDR:
    case Instruction::MUL_INT_2ADDR: case Instruction::AND_INT_2ADDR:
    case Instruction::OR_INT_2ADDR: case Instruction::XOR_INT_2ADDR:
    case Instruction::SHL_INT_2ADDR: case Instruction::SHR_INT_2ADDR:
    case Instruction::USHR_INT_2ADDR:
    case Instruction::ADD_LONG_2ADDR: case Instruction::SUB_LONG_2ADDR:
    case Instruction::MUL_LONG_2ADDR: case Instruction::AND_LONG_2ADDR:
    case Instruction::OR_LONG_2ADDR: case Instruction::XOR_LONG_2ADDR:
    case Instruction::SHL_LONG_2ADDR: case Instruction::SHR_LONG_2ADDR:
    case Instruction::USHR_LONG_2ADDR:
    case Instruction::ADD_FLOAT_2ADDR: case Instruction::SUB_FLOAT_2ADDR:
    case Instruction::MUL_FLOAT_2ADDR: case Instruction::DIV_FLOAT_2ADDR:
    case Instruction::REM_FLOAT_2ADDR:
    case Instruction::ADD_DOUBLE_2ADDR: case Instruction::SUB_DOUBLE_2ADDR:
    case Instruction::MUL_DOUBLE_2ADDR: case Instruction::DIV_DOUBLE_2ADDR:
    case Instruction::REM_DOUBLE_2ADDR:
    case Instruction::ADD_INT_LIT16: case Instruction::RSUB_INT: case Instruction::MUL_INT_LIT16:
    case Instruction::AND_INT_LIT16: case Instruction::OR_INT_LIT16:
    case Instruction::XOR_INT_LIT16:
    case Instruction::ADD_INT_LIT8: case Instruction::RSUB_INT_LIT8:
    case Instruction::MUL_INT_LIT8: case Instruction::AND_INT_LIT8: case Instruction::OR_INT_LIT8:
    case Instruction::XOR_INT_LIT8: case Instruction::SHL_INT_LIT8:
    case Instruction::SHR_INT_LIT8: case Instruction::USHR_INT_LIT8:
      return ArtWasmKind::kPure;
    case Instruction::IF_EQ: case Instruction::IF_NE: case Instruction::IF_LT:
    case Instruction::IF_GE: case Instruction::IF_GT: case Instruction::IF_LE:
      return ArtWasmKind::kIf22t;
    case Instruction::IF_EQZ: case Instruction::IF_NEZ: case Instruction::IF_LTZ:
    case Instruction::IF_GEZ: case Instruction::IF_GTZ: case Instruction::IF_LEZ:
      return ArtWasmKind::kIf21z;
    case Instruction::GOTO: case Instruction::GOTO_16: case Instruction::GOTO_32:
      return ArtWasmKind::kGoto;
    case Instruction::INVOKE_VIRTUAL: case Instruction::INVOKE_VIRTUAL_RANGE:
    case Instruction::INVOKE_SUPER: case Instruction::INVOKE_SUPER_RANGE:
    case Instruction::INVOKE_DIRECT: case Instruction::INVOKE_DIRECT_RANGE:
    case Instruction::INVOKE_STATIC: case Instruction::INVOKE_STATIC_RANGE:
    case Instruction::INVOKE_INTERFACE: case Instruction::INVOKE_INTERFACE_RANGE:
    case Instruction::INVOKE_VIRTUAL_QUICK: case Instruction::INVOKE_VIRTUAL_RANGE_QUICK:
      return ArtWasmKind::kInvoke;
    case Instruction::IGET_QUICK: case Instruction::IGET_WIDE_QUICK:
    case Instruction::IGET_OBJECT_QUICK: case Instruction::IGET_BOOLEAN_QUICK:
    case Instruction::IGET_BYTE_QUICK: case Instruction::IGET_CHAR_QUICK:
    case Instruction::IGET_SHORT_QUICK:
      return ArtWasmKind::kIGetQuick;
    case Instruction::IPUT_QUICK: case Instruction::IPUT_WIDE_QUICK:
    case Instruction::IPUT_OBJECT_QUICK: case Instruction::IPUT_BOOLEAN_QUICK:
    case Instruction::IPUT_BYTE_QUICK: case Instruction::IPUT_CHAR_QUICK:
    case Instruction::IPUT_SHORT_QUICK:
      return ArtWasmKind::kIPutQuick;
    case Instruction::SGET: case Instruction::SGET_WIDE: case Instruction::SGET_OBJECT:
    case Instruction::SGET_BOOLEAN: case Instruction::SGET_BYTE: case Instruction::SGET_CHAR:
    case Instruction::SGET_SHORT:
      return ArtWasmKind::kSGet;
    case Instruction::SPUT: case Instruction::SPUT_WIDE: case Instruction::SPUT_OBJECT:
    case Instruction::SPUT_BOOLEAN: case Instruction::SPUT_BYTE: case Instruction::SPUT_CHAR:
    case Instruction::SPUT_SHORT:
      return ArtWasmKind::kSPut;
    case Instruction::RETURN_VOID: case Instruction::RETURN_VOID_NO_BARRIER:
    case Instruction::RETURN: case Instruction::RETURN_WIDE: case Instruction::RETURN_OBJECT:
      return ArtWasmKind::kReturn;
    // Common in loops and allocation: fast paths in the loop.
    case Instruction::AGET: case Instruction::AGET_WIDE: case Instruction::AGET_OBJECT:
    case Instruction::AGET_BOOLEAN: case Instruction::AGET_BYTE: case Instruction::AGET_CHAR:
    case Instruction::AGET_SHORT:
    case Instruction::APUT: case Instruction::APUT_WIDE: case Instruction::APUT_OBJECT:
    case Instruction::APUT_BOOLEAN: case Instruction::APUT_BYTE: case Instruction::APUT_CHAR:
    case Instruction::APUT_SHORT: case Instruction::ARRAY_LENGTH:
      return ArtWasmKind::kArray;
    case Instruction::DIV_INT: case Instruction::REM_INT: case Instruction::DIV_INT_2ADDR:
    case Instruction::REM_INT_2ADDR: case Instruction::DIV_INT_LIT16:
    case Instruction::REM_INT_LIT16: case Instruction::DIV_INT_LIT8: case Instruction::REM_INT_LIT8:
    case Instruction::DIV_LONG: case Instruction::REM_LONG: case Instruction::DIV_LONG_2ADDR:
    case Instruction::REM_LONG_2ADDR:
      return ArtWasmKind::kDiv;
    case Instruction::NEW_INSTANCE: case Instruction::NEW_ARRAY:
      return ArtWasmKind::kNew;
    case Instruction::CONST_STRING:
      return ArtWasmKind::kConstString;
    case Instruction::CHECK_CAST: case Instruction::INSTANCE_OF:
      return ArtWasmKind::kTypeCheck;
    // Everything else (field access before quickening, monitors, switches,
    // throw, array initialization, method handles and types,
    // invoke-polymorphic and invoke-custom, constants that resolve
    // classes): out of the loop, which must stay small enough for Safari's
    // optimizing tier.
    default:
      return ArtWasmKind::kCold;
  }
}

// Of kind kOther: may suspend the thread (allocate, resolve) other than by
// throwing. The others (array access, array-length, division) go to the next
// instruction without looking for listeners, unless they threw.
constexpr bool ArtWasmMaySuspend(Instruction::Code op) {
  return op == Instruction::NEW_INSTANCE || op == Instruction::NEW_ARRAY ||
      op == Instruction::CONST_STRING || op == Instruction::CHECK_CAST ||
      op == Instruction::INSTANCE_OF;
}

constexpr InvokeType ArtWasmInvokeTypeOf(Instruction::Code op) {
  switch (op) {
    case Instruction::INVOKE_SUPER: case Instruction::INVOKE_SUPER_RANGE: return kSuper;
    case Instruction::INVOKE_DIRECT: case Instruction::INVOKE_DIRECT_RANGE: return kDirect;
    case Instruction::INVOKE_STATIC: case Instruction::INVOKE_STATIC_RANGE: return kStatic;
    case Instruction::INVOKE_INTERFACE: case Instruction::INVOKE_INTERFACE_RANGE:
      return kInterface;
    default: return kVirtual;
  }
}

constexpr bool ArtWasmIsRange(Instruction::Code op) {
  return op == Instruction::INVOKE_VIRTUAL_RANGE || op == Instruction::INVOKE_SUPER_RANGE ||
      op == Instruction::INVOKE_DIRECT_RANGE || op == Instruction::INVOKE_STATIC_RANGE ||
      op == Instruction::INVOKE_INTERFACE_RANGE || op == Instruction::INVOKE_VIRTUAL_RANGE_QUICK;
}

constexpr bool ArtWasmIsQuickInvoke(Instruction::Code op) {
  return op == Instruction::INVOKE_VIRTUAL_QUICK || op == Instruction::INVOKE_VIRTUAL_RANGE_QUICK;
}

constexpr Primitive::Type ArtWasmFieldTypeOf(Instruction::Code op) {
  switch (op) {
    case Instruction::IGET_WIDE_QUICK: case Instruction::IPUT_WIDE_QUICK:
    case Instruction::SGET_WIDE: case Instruction::SPUT_WIDE:
      return Primitive::kPrimLong;
    case Instruction::IGET_OBJECT_QUICK: case Instruction::IPUT_OBJECT_QUICK:
    case Instruction::SGET_OBJECT: case Instruction::SPUT_OBJECT:
      return Primitive::kPrimNot;
    case Instruction::IGET_BOOLEAN_QUICK: case Instruction::IPUT_BOOLEAN_QUICK:
    case Instruction::SGET_BOOLEAN: case Instruction::SPUT_BOOLEAN:
      return Primitive::kPrimBoolean;
    case Instruction::IGET_BYTE_QUICK: case Instruction::IPUT_BYTE_QUICK:
    case Instruction::SGET_BYTE: case Instruction::SPUT_BYTE:
      return Primitive::kPrimByte;
    case Instruction::IGET_CHAR_QUICK: case Instruction::IPUT_CHAR_QUICK:
    case Instruction::SGET_CHAR: case Instruction::SPUT_CHAR:
      return Primitive::kPrimChar;
    case Instruction::IGET_SHORT_QUICK: case Instruction::IPUT_SHORT_QUICK:
    case Instruction::SGET_SHORT: case Instruction::SPUT_SHORT:
      return Primitive::kPrimShort;
    default:
      return Primitive::kPrimInt;
  }
}

template <Instruction::Code op>
ALWAYS_INLINE static bool ArtWasmCompare(int32_t a, int32_t b) {
  switch (op) {
    case Instruction::IF_EQ: case Instruction::IF_EQZ: return a == b;
    case Instruction::IF_NE: case Instruction::IF_NEZ: return a != b;
    case Instruction::IF_LT: case Instruction::IF_LTZ: return a < b;
    case Instruction::IF_GE: case Instruction::IF_GEZ: return a >= b;
    case Instruction::IF_GT: case Instruction::IF_GTZ: return a > b;
    default: return a <= b;  // IF_LE, IF_LEZ
  }
}

// Field access of a quickened or static field (|obj| is the object, or the
// declaring class), its vreg vregA.
template <Primitive::Type field_type>
ALWAYS_INLINE static void ArtWasmGetField(ShadowFrame* sf, uint32_t vregA,
                                          ObjPtr<mirror::Object> obj, MemberOffset offset)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  switch (field_type) {
    case Primitive::kPrimInt: sf->SetVReg(vregA, obj->GetField32(offset)); break;
    case Primitive::kPrimBoolean: sf->SetVReg(vregA, obj->GetFieldBoolean(offset)); break;
    case Primitive::kPrimByte: sf->SetVReg(vregA, obj->GetFieldByte(offset)); break;
    case Primitive::kPrimChar: sf->SetVReg(vregA, obj->GetFieldChar(offset)); break;
    case Primitive::kPrimShort: sf->SetVReg(vregA, obj->GetFieldShort(offset)); break;
    case Primitive::kPrimLong: sf->SetVRegLong(vregA, obj->GetField64(offset)); break;
    default:  // kPrimNot
      sf->SetVRegReference(vregA, obj->GetFieldObject<mirror::Object>(offset));
      break;
  }
}

template <Primitive::Type field_type>
ALWAYS_INLINE static void ArtWasmSetField(const ShadowFrame* sf, uint32_t vregA,
                                          ObjPtr<mirror::Object> obj, MemberOffset offset)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  switch (field_type) {
    case Primitive::kPrimInt: obj->SetField32<false>(offset, sf->GetVReg(vregA)); break;
    case Primitive::kPrimBoolean: obj->SetFieldBoolean<false>(offset, sf->GetVReg(vregA)); break;
    case Primitive::kPrimByte: obj->SetFieldByte<false>(offset, sf->GetVReg(vregA)); break;
    case Primitive::kPrimChar: obj->SetFieldChar<false>(offset, sf->GetVReg(vregA)); break;
    case Primitive::kPrimShort: obj->SetFieldShort<false>(offset, sf->GetVReg(vregA)); break;
    case Primitive::kPrimLong: obj->SetField64<false>(offset, sf->GetVRegLong(vregA)); break;
    default:  // kPrimNot
      obj->SetFieldObject<false>(offset, sf->GetVRegReference(vregA));
      break;
  }
}

constexpr bool ArtWasmHasColdPath(Instruction::Code op) {
  const ArtWasmKind kind = ArtWasmKindOf(op);
  return kind != ArtWasmKind::kPure && kind != ArtWasmKind::kIf22t &&
      kind != ArtWasmKind::kIf21z && kind != ArtWasmKind::kGoto && kind != ArtWasmKind::kOther &&
      kind != ArtWasmKind::kArray && kind != ArtWasmKind::kDiv;
}

// The instructions of kind kCold, and the slow paths of the others (invoke
// without a fast path, quick field access of null, return with listeners):
// the handler, in a function of its own. Returns the next instruction, or
// nullptr if the handler left the loop (*exit_result: its result). (The loop's
// |inst| is not passed by address: it would have to live in memory, and
// every instruction would load and store it.)
template<bool do_access_check, bool transaction_active>
NO_INLINE static const Instruction* ArtWasmColdInstruction(
    Thread* self, ShadowFrame* sf, const CodeItemDataAccessor& accessor, JValue* result_register,
    uint32_t dex_pc, const Instruction* inst, uint16_t inst_data, JValue* exit_result)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  SwitchImplContext ctx{self, accessor, *sf, *result_register, false, JValue()};
  bool exit_loop = false;
  InstructionHandler<do_access_check, transaction_active> h(
      &ctx, Runtime::Current()->GetInstrumentation(), self, *sf, dex_pc, inst, inst_data,
      exit_loop);
  switch (inst->Opcode(inst_data)) {
#define ART_WASM_COLD_CASE(OPCODE, OPCODE_NAME, pname, f, i, a, e, v)                          \
    case OPCODE:                                                                               \
      if constexpr (ArtWasmHasColdPath(static_cast<Instruction::Code>(OPCODE))) {              \
        h.OPCODE_NAME();                                                                       \
      }                                                                                        \
      break;
DEX_INSTRUCTION_LIST(ART_WASM_COLD_CASE)
#undef ART_WASM_COLD_CASE
  }
  if (UNLIKELY(exit_loop)) {
    *exit_result = ctx.result;
    return nullptr;
  }
  return inst;
}

// Array access (kArray) without the handler: false with an exception
// pending (the caller stored the dex pc before it throws).
template <Instruction::Code op>
ALWAYS_INLINE static bool ArtWasmArrayOp(ShadowFrame* sf, const Instruction* inst,
                                         uint16_t inst_data)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if constexpr (op == Instruction::ARRAY_LENGTH) {
    ObjPtr<mirror::Object> array = sf->GetVRegReference(inst->VRegB_12x(inst_data));
    if (UNLIKELY(array == nullptr)) {
      ThrowNullPointerExceptionFromInterpreter();
      return false;
    }
    sf->SetVReg(inst->VRegA_12x(inst_data), array->AsArray()->GetLength());
    return true;
  } else {
    ObjPtr<mirror::Object> a = sf->GetVRegReference(inst->VRegB_23x());
    if (UNLIKELY(a == nullptr)) {
      ThrowNullPointerExceptionFromInterpreter();
      return false;
    }
    const int32_t index = sf->GetVReg(inst->VRegC_23x());
    const uint32_t vA = inst->VRegA_23x(inst_data);
#define ART_WASM_ARRAY(T)                                                         \
    ObjPtr<T> array = ObjPtr<T>::DownCast(a);                                     \
    if (UNLIKELY(!array->CheckIsValidIndex(index))) {                             \
      return false;                                                               \
    }
    switch (op) {
      case Instruction::AGET: { ART_WASM_ARRAY(mirror::IntArray)
        sf->SetVReg(vA, array->GetWithoutChecks(index)); return true; }
      case Instruction::AGET_WIDE: { ART_WASM_ARRAY(mirror::LongArray)
        sf->SetVRegLong(vA, array->GetWithoutChecks(index)); return true; }
      case Instruction::AGET_BOOLEAN: { ART_WASM_ARRAY(mirror::BooleanArray)
        sf->SetVReg(vA, array->GetWithoutChecks(index)); return true; }
      case Instruction::AGET_BYTE: { ART_WASM_ARRAY(mirror::ByteArray)
        sf->SetVReg(vA, array->GetWithoutChecks(index)); return true; }
      case Instruction::AGET_CHAR: { ART_WASM_ARRAY(mirror::CharArray)
        sf->SetVReg(vA, array->GetWithoutChecks(index)); return true; }
      case Instruction::AGET_SHORT: { ART_WASM_ARRAY(mirror::ShortArray)
        sf->SetVReg(vA, array->GetWithoutChecks(index)); return true; }
      case Instruction::AGET_OBJECT: {
        ObjPtr<mirror::ObjectArray<mirror::Object>> array = a->AsObjectArray<mirror::Object>();
        if (UNLIKELY(!array->CheckIsValidIndex(index))) {
          return false;
        }
        sf->SetVRegReference(vA, array->GetWithoutChecks(index));
        return true;
      }
      case Instruction::APUT: { ART_WASM_ARRAY(mirror::IntArray)
        array->SetWithoutChecks<false>(index, sf->GetVReg(vA)); return true; }
      case Instruction::APUT_WIDE: { ART_WASM_ARRAY(mirror::LongArray)
        array->SetWithoutChecks<false>(index, sf->GetVRegLong(vA)); return true; }
      case Instruction::APUT_BOOLEAN: { ART_WASM_ARRAY(mirror::BooleanArray)
        array->SetWithoutChecks<false>(index, sf->GetVReg(vA)); return true; }
      case Instruction::APUT_BYTE: { ART_WASM_ARRAY(mirror::ByteArray)
        array->SetWithoutChecks<false>(index, sf->GetVReg(vA)); return true; }
      case Instruction::APUT_CHAR: { ART_WASM_ARRAY(mirror::CharArray)
        array->SetWithoutChecks<false>(index, sf->GetVReg(vA)); return true; }
      case Instruction::APUT_SHORT: { ART_WASM_ARRAY(mirror::ShortArray)
        array->SetWithoutChecks<false>(index, sf->GetVReg(vA)); return true; }
      default: {  // APUT_OBJECT
        ObjPtr<mirror::ObjectArray<mirror::Object>> array = a->AsObjectArray<mirror::Object>();
        ObjPtr<mirror::Object> val = sf->GetVRegReference(vA);
        if (UNLIKELY(!array->CheckIsValidIndex(index) || !array->CheckAssignable(val))) {
          return false;
        }
        array->SetWithoutChecks<false>(index, val);
        return true;
      }
    }
#undef ART_WASM_ARRAY
  }
}

// div/rem (kDiv) without the handler (DoIntDivide and the like throw
// ArithmeticException): false with an exception pending.
template <Instruction::Code op>
ALWAYS_INLINE static bool ArtWasmDivOp(ShadowFrame* sf, const Instruction* inst,
                                       uint16_t inst_data)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  switch (op) {
    case Instruction::DIV_INT:
      return DoIntDivide(*sf, inst->VRegA_23x(inst_data), sf->GetVReg(inst->VRegB_23x()),
                         sf->GetVReg(inst->VRegC_23x()));
    case Instruction::REM_INT:
      return DoIntRemainder(*sf, inst->VRegA_23x(inst_data), sf->GetVReg(inst->VRegB_23x()),
                            sf->GetVReg(inst->VRegC_23x()));
    case Instruction::DIV_LONG:
      return DoLongDivide(*sf, inst->VRegA_23x(inst_data), sf->GetVRegLong(inst->VRegB_23x()),
                          sf->GetVRegLong(inst->VRegC_23x()));
    case Instruction::REM_LONG:
      return DoLongRemainder(*sf, inst->VRegA_23x(inst_data),
                             sf->GetVRegLong(inst->VRegB_23x()),
                             sf->GetVRegLong(inst->VRegC_23x()));
    case Instruction::DIV_INT_2ADDR: {
      const uint32_t vA = inst->VRegA_12x(inst_data);
      return DoIntDivide(*sf, vA, sf->GetVReg(vA), sf->GetVReg(inst->VRegB_12x(inst_data)));
    }
    case Instruction::REM_INT_2ADDR: {
      const uint32_t vA = inst->VRegA_12x(inst_data);
      return DoIntRemainder(*sf, vA, sf->GetVReg(vA), sf->GetVReg(inst->VRegB_12x(inst_data)));
    }
    case Instruction::DIV_LONG_2ADDR: {
      const uint32_t vA = inst->VRegA_12x(inst_data);
      return DoLongDivide(*sf, vA, sf->GetVRegLong(vA),
                          sf->GetVRegLong(inst->VRegB_12x(inst_data)));
    }
    case Instruction::REM_LONG_2ADDR: {
      const uint32_t vA = inst->VRegA_12x(inst_data);
      return DoLongRemainder(*sf, vA, sf->GetVRegLong(vA),
                             sf->GetVRegLong(inst->VRegB_12x(inst_data)));
    }
    case Instruction::DIV_INT_LIT16:
      return DoIntDivide(*sf, inst->VRegA_22s(inst_data), sf->GetVReg(inst->VRegB_22s(inst_data)),
                         inst->VRegC_22s());
    case Instruction::REM_INT_LIT16:
      return DoIntRemainder(*sf, inst->VRegA_22s(inst_data),
                            sf->GetVReg(inst->VRegB_22s(inst_data)), inst->VRegC_22s());
    case Instruction::DIV_INT_LIT8:
      return DoIntDivide(*sf, inst->VRegA_22b(inst_data), sf->GetVReg(inst->VRegB_22b()),
                         inst->VRegC_22b());
    default:  // REM_INT_LIT8
      return DoIntRemainder(*sf, inst->VRegA_22b(inst_data), sf->GetVReg(inst->VRegB_22b()),
                            inst->VRegC_22b());
  }
}

// new-instance and new-array out of the loop, with the allocation code
// compiled code would use: uninstrumented (the thread-local RosAlloc path)
// unless the allocation entrypoints are instrumented (AOSP's interpreter
// always takes the instrumented path). nullptr with an exception pending.
NO_INLINE static mirror::Object* ArtWasmNewInstance(Thread* self, ArtMethod* method,
                                                    dex::TypeIndex type_idx)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  ObjPtr<mirror::Class> c = ResolveVerifyAndClinit(type_idx, method, self, false, false);
  if (UNLIKELY(c == nullptr)) {
    return nullptr;
  }
  gc::AllocatorType allocator_type = Runtime::Current()->GetHeap()->GetCurrentAllocator();
  if (UNLIKELY(c->IsStringClass())) {
    return mirror::String::AllocEmptyString<true>(self, allocator_type).Ptr();
  }
  if (Runtime::Current()->GetInstrumentation()->AllocEntrypointsInstrumented()) {
    return AllocObjectFromCode<true>(c, self, allocator_type).Ptr();
  }
  return AllocObjectFromCode<false>(c, self, allocator_type).Ptr();
}

NO_INLINE static mirror::Object* ArtWasmNewArray(Thread* self, ArtMethod* method,
                                                 dex::TypeIndex type_idx, int32_t length)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  gc::AllocatorType allocator_type = Runtime::Current()->GetHeap()->GetCurrentAllocator();
  if (Runtime::Current()->GetInstrumentation()->AllocEntrypointsInstrumented()) {
    return AllocArrayFromCode<false, true>(type_idx, length, method, self, allocator_type).Ptr();
  }
  return AllocArrayFromCode<false, false>(type_idx, length, method, self, allocator_type).Ptr();
}

NO_INLINE static mirror::Object* ArtWasmResolveString(Thread* self, ShadowFrame* sf,
                                                      dex::StringIndex string_idx)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  return ResolveString(self, *sf, string_idx).Ptr();
}

// Where ArtWasmSwitchLoop continues after it yielded.
struct ArtWasmResume {
  ShadowFrame* sf;  // the current frame (a flat callee, or the method's own); null: not yet
  size_t depth;     // flat frames above the method's own
  JValue result_register;
  bool yielded;
  bool listeners;   // continue with the loop that calls listeners
  bool suspend;     // run the thread's suspend check, then continue
};

static constexpr uint32_t kArtWasmYieldBranches = 65536;

// --- the loop -----------------------------------------------------------------

template<bool do_access_check, bool transaction_active>
NO_INLINE static void ArtWasmSwitchLoop(SwitchImplContext* ctx, ArtWasmResume* resume)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  // Flat calls only without access checks and transactions; the loop with
  // access checks (also: with listeners) runs flat frames made before.
  constexpr bool kFlat = !do_access_check && !transaction_active;
  using Handler = InstructionHandler<do_access_check, transaction_active>;
  Thread* const self = ctx->self;
  const instrumentation::Instrumentation* const instrumentation =
      Runtime::Current()->GetInstrumentation();
  ShadowFrame* sf;
  size_t depth;
  JValue result_register;
  if (resume->sf != nullptr) {
    sf = resume->sf;
    depth = resume->depth;
    result_register = resume->result_register;
  } else {
    sf = &ctx->shadow_frame;
    depth = 0;
    result_register = ctx->result_register;
    if (UNLIKELY(!sf->HasReferenceArray())) {
      LOG(FATAL) << "Invalid shadow frame for interpreter use";
    }
    self->VerifyStack();
    DCHECK(!sf->GetForceRetryInstruction())
        << "Entered interpreter from invoke without retry instruction being handled!";
  }
  CodeItemDataAccessor accessor = depth == 0 ? ctx->accessor : ArtWasmFlatFrameAccessor(sf);
  const uint16_t* insns = accessor.Insns();
  const Instruction* inst = Instruction::At(insns + sf->GetDexPC());
  uint32_t yield_countdown = g_art_wasm_yield_branches;  // (kArtWasmYieldBranches, or the JIT's)
  JValue flat_result;
  uint32_t dex_pc;
  uint16_t inst_data;

#define ART_WASM_YIELD(_suspend, _listeners) \
  do {                                       \
    resume->sf = sf;                         \
    resume->depth = depth;                   \
    resume->result_register = result_register; \
    resume->yielded = true;                  \
    resume->suspend = (_suspend);            \
    resume->listeners = (_listeners);        \
    return;                                  \
  } while (false)

// Dex pc and branch listeners can only be added while this thread is
// suspended, so the flat loop checks for them where it may have been: when
// it is entered, after an instruction that may call out, and after an
// exception is handled.
#define ART_WASM_LISTENERS_CHECK()                                                    \
  do {                                                                                \
    if (kFlat && UNLIKELY(instrumentation->HasDexPcListeners() ||                     \
                          instrumentation->HasBranchListeners())) {                   \
      sf->SetDexPC(inst->GetDexPc(insns));                                            \
      ART_WASM_YIELD(false, true);                                                    \
    }                                                                                 \
  } while (false)

// A handler for the current instruction of the current frame.
#define ART_WASM_HANDLER(_h, _exit)                                                   \
  SwitchImplContext _h##_ctx{self, accessor, *sf, result_register, false, JValue()};  \
  bool _exit = false;                                                                 \
  Handler _h(&_h##_ctx, instrumentation, self, *sf, dex_pc, inst, inst_data, _exit)

// The current instruction out of the loop (ArtWasmColdInstruction).
#define ART_WASM_COLD()                                                               \
  do {                                                                                \
    JValue cold_result;                                                               \
    const Instruction* cold_next = ArtWasmColdInstruction<do_access_check, transaction_active>( \
        self, sf, accessor, &result_register, dex_pc, inst, inst_data, &cold_result); \
    if (LIKELY(cold_next != nullptr)) {                                               \
      inst = cold_next;                                                               \
    } else {                                                                          \
      if (self->IsExceptionPending()) {                                               \
        goto unwind;                                                                  \
      }                                                                               \
      flat_result = cold_result;                                                      \
      goto do_return;                                                                 \
    }                                                                                 \
  } while (false)

// After a handler that left the loop (_exit): a return (the result in its
// context) or an exception the frame does not handle.
#define ART_WASM_AFTER_EXIT(_h)                                                       \
  do {                                                                                \
    if (self->IsExceptionPending()) {                                                 \
      goto unwind;                                                                    \
    }                                                                                 \
    flat_result = _h##_ctx.result;                                                    \
    goto do_return;                                                                   \
  } while (false)

  ART_WASM_LISTENERS_CHECK();
  while (true) {
    dex_pc = inst->GetDexPc(insns);
    inst_data = inst->Fetch16(0);
    if constexpr (kFlat) {
      switch (inst->Opcode(inst_data)) {
#define ART_WASM_FLAT_CASE(OPCODE, OPCODE_NAME, pname, f, i, a, e, v)                           \
        case OPCODE: {                                                                          \
          constexpr Instruction::Code kOp = static_cast<Instruction::Code>(OPCODE);             \
          constexpr ArtWasmKind kKind = ArtWasmKindOf(kOp);                                     \
          if constexpr (kKind == ArtWasmKind::kPure) {                                          \
            ART_WASM_HANDLER(h, exit_loop);                                                     \
            h.OPCODE_NAME();                                                                    \
            continue;                                                                           \
          } else if constexpr (kKind == ArtWasmKind::kIf22t || kKind == ArtWasmKind::kIf21z ||  \
                               kKind == ArtWasmKind::kGoto) {                                   \
            int32_t offset;                                                                     \
            if constexpr (kKind == ArtWasmKind::kGoto) {                                        \
              offset = kOp == Instruction::GOTO ? inst->VRegA_10t(inst_data)                    \
                     : kOp == Instruction::GOTO_16 ? inst->VRegA_20t() : inst->VRegA_30t();     \
            } else if constexpr (kKind == ArtWasmKind::kIf22t) {                                \
              if (!ArtWasmCompare<kOp>(sf->GetVReg(inst->VRegA_22t(inst_data)),                 \
                                       sf->GetVReg(inst->VRegB_22t(inst_data)))) {              \
                inst = inst->Next_2xx();                                                        \
                continue;                                                                       \
              }                                                                                 \
              offset = inst->VRegC_22t();                                                       \
            } else {                                                                            \
              if (!ArtWasmCompare<kOp>(sf->GetVReg(inst->VRegA_21t(inst_data)), 0)) {           \
                inst = inst->Next_2xx();                                                        \
                continue;                                                                       \
              }                                                                                 \
              offset = inst->VRegB_21t();                                                       \
            }                                                                                   \
            inst = inst->RelativeAt(offset);                                                    \
            if (offset <= 0) {                                                                  \
              /* A backward branch: the suspend check, with the target as the dex pc. */        \
              sf->SetDexPC(inst->GetDexPc(insns));                                              \
              if (UNLIKELY(self->TestAllFlags())) {                                             \
                ART_WASM_YIELD(true, false);                                                    \
              }                                                                                 \
              if (UNLIKELY(--yield_countdown == 0)) {                                           \
                ART_WASM_YIELD(false, false);                                                   \
              }                                                                                 \
            }                                                                                   \
            continue;                                                                           \
          } else if constexpr (kKind == ArtWasmKind::kIGetQuick) {                              \
            ObjPtr<mirror::Object> obj = sf->GetVRegReference(inst->VRegB_22c(inst_data));      \
            if (LIKELY(obj != nullptr && !instrumentation->HasFieldReadListeners())) {          \
              ArtWasmGetField<ArtWasmFieldTypeOf(kOp)>(sf, inst->VRegA_22c(inst_data), obj,     \
                                                      MemberOffset(inst->VRegC_22c()));         \
              inst = inst->Next_2xx();                                                          \
              continue;                                                                         \
            }                                                                                   \
            sf->SetDexPC(dex_pc);                                                               \
            ART_WASM_COLD();                                                                    \
          } else if constexpr (kKind == ArtWasmKind::kIPutQuick) {                              \
            ObjPtr<mirror::Object> obj = sf->GetVRegReference(inst->VRegB_22c(inst_data));      \
            if (LIKELY(obj != nullptr && !instrumentation->HasFieldWriteListeners())) {         \
              ArtWasmSetField<ArtWasmFieldTypeOf(kOp)>(sf, inst->VRegA_22c(inst_data), obj,     \
                                                      MemberOffset(inst->VRegC_22c()));         \
              inst = inst->Next_2xx();                                                          \
              continue;                                                                         \
            }                                                                                   \
            sf->SetDexPC(dex_pc);                                                               \
            ART_WASM_COLD();                                                                    \
          } else if constexpr (kKind == ArtWasmKind::kSGet) {                                   \
            ArtField* f = ArtWasmResolvedStaticField(sf, inst);                                 \
            if (LIKELY(f != nullptr && !instrumentation->HasFieldReadListeners())) {            \
              ArtWasmGetField<ArtWasmFieldTypeOf(kOp)>(sf, inst->VRegA_21c(inst_data),          \
                                                      f->GetDeclaringClass(), f->GetOffset());  \
              inst = inst->Next_2xx();                                                          \
              continue;                                                                         \
            }                                                                                   \
            sf->SetDexPC(dex_pc);                                                               \
            ART_WASM_COLD();                                                                    \
          } else if constexpr (kKind == ArtWasmKind::kSPut) {                                   \
            ArtField* f = ArtWasmResolvedStaticField(sf, inst);                                 \
            if (LIKELY(f != nullptr && !instrumentation->HasFieldWriteListeners())) {           \
              ArtWasmSetField<ArtWasmFieldTypeOf(kOp)>(sf, inst->VRegA_21c(inst_data),          \
                                                      f->GetDeclaringClass(), f->GetOffset());  \
              inst = inst->Next_2xx();                                                          \
              continue;                                                                         \
            }                                                                                   \
            sf->SetDexPC(dex_pc);                                                               \
            ART_WASM_COLD();                                                                    \
          } else if constexpr (kKind == ArtWasmKind::kInvoke) {                                 \
            sf->SetDexPC(dex_pc);                                                               \
            ArtMethod* fast_native = nullptr;                                                   \
            ShadowFrame* callee = ArtWasmTryFlatInvoke<ArtWasmInvokeTypeOf(kOp),                \
                                                       ArtWasmIsRange(kOp),                     \
                                                       ArtWasmIsQuickInvoke(kOp)>(              \
                self, sf, inst, inst_data, &fast_native);                                       \
            if (UNLIKELY(callee == kArtWasmEnterFailed)) {                                      \
              goto handle_exception;                                                            \
            }                                                                                   \
            if (callee == kArtWasmEmptyCall) {                                                  \
              QuasiAtomic::ThreadFenceForConstructor(); /* its return-void */                   \
              inst = inst->Next_3xx();                                                          \
              continue;                                                                         \
            }                                                                                   \
            if (callee == kArtWasmIntrinsicCall) {                                              \
              if (MterpHandleIntrinsic(sf, fast_native, inst, inst_data, &result_register)) {   \
                inst = inst->Next_3xx();                                                        \
                continue;                                                                       \
              }                                                                                 \
              callee = nullptr;                                                                 \
            } else if (callee == kArtWasmNativeCall) {                                          \
              if (LIKELY(ArtWasmCallNativeFromLoop<ArtWasmIsRange(kOp)>(                        \
                      self, sf, inst, inst_data, fast_native, &result_register))) {             \
                inst = inst->Next_3xx();                                                        \
                ART_WASM_LISTENERS_CHECK();                                                     \
                continue;                                                                       \
              }                                                                                 \
              goto handle_exception;                                                            \
            }                                                                                   \
            if (callee == nullptr) {                                                            \
              /* DoInvoke (through the handler) may still make the call flat. */                \
              t_art_flat_out = &callee;                                                         \
              t_art_flat_caller = sf;                                                           \
              JValue cold_result;                                                               \
              const Instruction* cold_next =                                                    \
                  ArtWasmColdInstruction<do_access_check, transaction_active>(                  \
                      self, sf, accessor, &result_register, dex_pc, inst, inst_data, &cold_result); \
              t_art_flat_out = nullptr;                                                         \
              if (callee != nullptr) {                                                          \
                /* Built by DoCallCommon, not pushed. */                                        \
                if (UNLIKELY(!ArtWasmFlatEnter(self, callee))) {                                \
                  goto handle_exception;                                                        \
                }                                                                               \
              } else {                                                                          \
                if (LIKELY(cold_next != nullptr)) {                                             \
                  inst = cold_next;                                                             \
                } else {                                                                        \
                  if (self->IsExceptionPending()) {                                             \
                    goto unwind;                                                                \
                  }                                                                             \
                  flat_result = cold_result;                                                    \
                  goto do_return;                                                               \
                }                                                                               \
                ART_WASM_LISTENERS_CHECK();                                                     \
                continue;                                                                       \
              }                                                                                 \
            }                                                                                   \
            /* Continue in the callee's frame (pushed). */                                      \
            sf = callee;                                                                        \
            ++depth;                                                                            \
            accessor = ArtWasmFlatFrameAccessor(sf);                                            \
            insns = accessor.Insns();                                                           \
            inst = Instruction::At(insns);                                                      \
            result_register = JValue();                                                         \
            continue;                                                                           \
          } else if constexpr (kKind == ArtWasmKind::kArray) {                                  \
            sf->SetDexPC(dex_pc); /* (the exceptions' stack traces and messages) */            \
            if (LIKELY((ArtWasmArrayOp<kOp>(sf, inst, inst_data)))) {                           \
              inst = kOp == Instruction::ARRAY_LENGTH ? inst->Next_1xx() : inst->Next_2xx();    \
              continue;                                                                         \
            }                                                                                   \
            goto handle_exception;                                                              \
          } else if constexpr (kKind == ArtWasmKind::kDiv) {                                    \
            sf->SetDexPC(dex_pc); /* (the exceptions' stack traces and messages) */            \
            if (LIKELY((ArtWasmDivOp<kOp>(sf, inst, inst_data)))) {                             \
              inst = (kOp == Instruction::DIV_INT_2ADDR || kOp == Instruction::REM_INT_2ADDR ||  \
                      kOp == Instruction::DIV_LONG_2ADDR || kOp == Instruction::REM_LONG_2ADDR) \
                  ? inst->Next_1xx() : inst->Next_2xx();                                        \
              continue;                                                                         \
            }                                                                                   \
            goto handle_exception;                                                              \
          } else if constexpr (kKind == ArtWasmKind::kNew) {                                    \
            sf->SetDexPC(dex_pc);                                                               \
            mirror::Object* obj;                                                                \
            uint32_t vA;                                                                        \
            if constexpr (kOp == Instruction::NEW_INSTANCE) {                                   \
              obj = ArtWasmNewInstance(self, sf->GetMethod(), dex::TypeIndex(inst->VRegB_21c())); \
              vA = inst->VRegA_21c(inst_data);                                                  \
            } else {                                                                            \
              obj = ArtWasmNewArray(self, sf->GetMethod(), dex::TypeIndex(inst->VRegC_22c()),   \
                                    sf->GetVReg(inst->VRegB_22c(inst_data)));                   \
              vA = inst->VRegA_22c(inst_data);                                                  \
            }                                                                                   \
            if (UNLIKELY(obj == nullptr)) {                                                     \
              goto handle_exception;                                                            \
            }                                                                                   \
            sf->SetVRegReference(vA, obj);                                                      \
            inst = inst->Next_2xx();                                                            \
          } else if constexpr (kKind == ArtWasmKind::kConstString) {                            \
            const dex::StringIndex string_idx(inst->VRegB_21c());                               \
            mirror::Object* str = sf->GetMethod()->GetDexCache()->GetResolvedString(string_idx); \
            if (UNLIKELY(str == nullptr)) {                                                     \
              sf->SetDexPC(dex_pc);                                                             \
              str = ArtWasmResolveString(self, sf, string_idx);                                 \
              if (UNLIKELY(str == nullptr)) {                                                   \
                goto handle_exception;                                                          \
              }                                                                                 \
            }                                                                                   \
            sf->SetVRegReference(inst->VRegA_21c(inst_data), str);                              \
            inst = inst->Next_2xx();                                                            \
            continue;                                                                           \
          } else if constexpr (kKind == ArtWasmKind::kTypeCheck) {                              \
            /* The common cases: null, or the class resolved and the test passing; else */      \
            /* the handler (resolution, ClassCastException). */                                 \
            if constexpr (kOp == Instruction::CHECK_CAST) {                                     \
              ObjPtr<mirror::Object> obj = sf->GetVRegReference(inst->VRegA_21c(inst_data));    \
              if (obj == nullptr) {                                                             \
                inst = inst->Next_2xx();                                                        \
                continue;                                                                       \
              }                                                                                 \
              ObjPtr<mirror::Class> c = sf->GetMethod()->GetDexCache()->GetResolvedType(        \
                  dex::TypeIndex(inst->VRegB_21c()));                                           \
              if (LIKELY(c != nullptr && obj->InstanceOf(c))) {                                 \
                inst = inst->Next_2xx();                                                        \
                continue;                                                                       \
              }                                                                                 \
            } else {                                                                            \
              ObjPtr<mirror::Object> obj = sf->GetVRegReference(inst->VRegB_22c(inst_data));    \
              if (obj == nullptr) {                                                             \
                sf->SetVReg(inst->VRegA_22c(inst_data), 0);                                     \
                inst = inst->Next_2xx();                                                        \
                continue;                                                                       \
              }                                                                                 \
              ObjPtr<mirror::Class> c = sf->GetMethod()->GetDexCache()->GetResolvedType(        \
                  dex::TypeIndex(inst->VRegC_22c()));                                           \
              if (LIKELY(c != nullptr)) {                                                       \
                sf->SetVReg(inst->VRegA_22c(inst_data), obj->InstanceOf(c) ? 1 : 0);            \
                inst = inst->Next_2xx();                                                        \
                continue;                                                                       \
              }                                                                                 \
            }                                                                                   \
            sf->SetDexPC(dex_pc);                                                               \
            ART_WASM_COLD();                                                                    \
          } else if constexpr (kKind == ArtWasmKind::kReturn) {                                 \
            /* As the handler's return-*, without its suspend check (a call) here: */           \
            /* with the suspend flags set, or listeners to tell, the handler's. */              \
            if (LIKELY(!self->TestAllFlags() &&                                                 \
                       !Handler::NeedsMethodExitEvent(instrumentation))) {                       \
              flat_result.SetJ(0);                                                              \
              if constexpr (kOp == Instruction::RETURN_VOID) {                                  \
                QuasiAtomic::ThreadFenceForConstructor();                                       \
              } else if constexpr (kOp == Instruction::RETURN) {                                \
                flat_result.SetI(sf->GetVReg(inst->VRegA_11x(inst_data)));                      \
              } else if constexpr (kOp == Instruction::RETURN_WIDE) {                           \
                flat_result.SetJ(sf->GetVRegLong(inst->VRegA_11x(inst_data)));                  \
              } else if constexpr (kOp == Instruction::RETURN_OBJECT) {                         \
                flat_result.SetL(sf->GetVRegReference(inst->VRegA_11x(inst_data)));             \
              }                                                                                 \
              goto do_return;                                                                   \
            }                                                                                   \
            sf->SetDexPC(dex_pc);                                                               \
            ART_WASM_COLD();                                                                    \
          } else if constexpr (kKind == ArtWasmKind::kCold) {                                   \
            sf->SetDexPC(dex_pc);                                                               \
            ART_WASM_COLD();                                                                    \
          } else {                                                                              \
            sf->SetDexPC(dex_pc);                                                               \
            ART_WASM_HANDLER(h, exit_loop);                                                     \
            h.OPCODE_NAME();                                                                    \
            if (UNLIKELY(exit_loop)) ART_WASM_AFTER_EXIT(h);                                    \
            if constexpr (!ArtWasmMaySuspend(kOp)) {                                            \
              /* (A thrown exception is pending until its handler's move-exception.) */         \
              if (LIKELY(!self->IsExceptionPending())) {                                        \
                continue;                                                                       \
              }                                                                                 \
            }                                                                                   \
          }                                                                                     \
          break;                                                                                \
        }
DEX_INSTRUCTION_LIST(ART_WASM_FLAT_CASE)
#undef ART_WASM_FLAT_CASE
      }
      ART_WASM_LISTENERS_CHECK();
      continue;
    } else {
      // As AOSP's loop: the dex pc, then listeners, for every instruction.
      sf->SetDexPC(dex_pc);
      TraceExecution(*sf, inst, dex_pc);
      {
        ART_WASM_HANDLER(h, exit_loop);
        if (!h.Preamble()) {
          if (UNLIKELY(exit_loop)) ART_WASM_AFTER_EXIT(h);
          continue;
        }
      }
      switch (inst->Opcode(inst_data)) {
#define ART_WASM_CASE(OPCODE, OPCODE_NAME, pname, f, i, a, e, v)                                \
        case OPCODE: {                                                                          \
          ART_WASM_HANDLER(h, exit_loop);                                                       \
          h.OPCODE_NAME();                                                                      \
          if (UNLIKELY(exit_loop)) ART_WASM_AFTER_EXIT(h);                                      \
          break;                                                                                \
        }
DEX_INSTRUCTION_LIST(ART_WASM_CASE)
#undef ART_WASM_CASE
      }
      continue;
    }

  handle_exception:
    // An exception from the instruction at dex_pc of the current frame
    // (a native, the class initialization of a flat call, a fast path that
    // threw before storing its dex pc): its handler here, or unwind.
    sf->SetDexPC(dex_pc);
    {
      ART_WASM_HANDLER(h, exit_loop);
      (void) h.HandlePendingException();
      if (exit_loop) {
        goto unwind;
      }
    }
    ART_WASM_LISTENERS_CHECK();
    continue;

  do_return:
    // The current frame returned |flat_result|.
    if (depth == 0) {
      ctx->result = flat_result;
      resume->yielded = false;
      return;
    }
    // The method of a flat call returned: continue its caller after the
    // invoke (all invoke formats that make flat calls are 3 code units).
    {
      ShadowFrame* const callee = sf;
      sf = callee->GetLink();
      self->PopShadowFrame();
      // (No ~ShadowFrame: its LockCountData holds monitors only for methods
      // that must count locks, which are never flat calls.)
      ArtWasmFreeFlatFrame(callee);
      --depth;
      accessor = depth == 0 ? ctx->accessor : ArtWasmFlatFrameAccessor(sf);
      insns = accessor.Insns();
      result_register = flat_result;
      inst = Instruction::At(insns + sf->GetDexPC())->Next_3xx();
    }
    continue;

  unwind:
    // The current frame has no handler for the pending exception.
    if (depth == 0) {
      ctx->result = JValue();
      resume->yielded = false;
      return;
    }
    // A flat call: look for one at the caller's invoke.
    {
      ShadowFrame* const callee = sf;
      sf = callee->GetLink();
      self->PopShadowFrame();
      ArtWasmFreeFlatFrame(callee);
      --depth;
      accessor = depth == 0 ? ctx->accessor : ArtWasmFlatFrameAccessor(sf);
      insns = accessor.Insns();
      dex_pc = sf->GetDexPC();
      inst = Instruction::At(insns + dex_pc);
      inst_data = inst->Fetch16(0);
    }
    goto handle_exception;
  }
#undef ART_WASM_YIELD
#undef ART_WASM_LISTENERS_CHECK
#undef ART_WASM_HANDLER
#undef ART_WASM_AFTER_EXIT
#undef ART_WASM_COLD
}

// ART 10's entry of the switch interpreter (interpreter_switch_impl.h): the
// loop, again after each time it yielded.
template<bool do_access_check, bool transaction_active>
ATTRIBUTE_NO_SANITIZE_ADDRESS void ExecuteSwitchImplCpp(SwitchImplContext* ctx) {
  DCHECK(!ctx->interpret_one_instruction);  // (mterp's)
  Thread* const self = ctx->self;
  ArtWasmResume resume = {nullptr, 0, JValue(), false, false, false};
  ArtWasmSwitchLoop<do_access_check, transaction_active>(ctx, &resume);
  while (UNLIKELY(resume.yielded)) {
    resume.yielded = false;
    const bool plain_yield = !resume.suspend && !resume.listeners;
    if (resume.suspend) {
      resume.suspend = false;
      self->CheckSuspend();
    }
    if (UNLIKELY(resume.listeners)) {
      // The rest of this call (its flat frames too) in the loop with access
      // checks: it calls the listeners and makes no flat calls.
      resume.listeners = false;
      ArtWasmSwitchLoop<true, transaction_active>(ctx, &resume);
    } else {
      // On-stack replacement (topic dex-jit) at the loop header where the
      // loop yielded (every g_art_wasm_yield_branches backward branches).
      if (!do_access_check && !transaction_active && plain_yield && g_art_jit_osr) {
        JValue osr_result;
        if (ArtJitOsr(self, &resume.sf, &resume.depth, &resume.result_register, &osr_result) == 1) {
          ctx->result = osr_result;
          return;
        }
      }
      ArtWasmSwitchLoop<do_access_check, transaction_active>(ctx, &resume);
    }
  }
}

#endif  // ARC_WASM_SHIM_ART10_INTERPRETER_SWITCH_WASM10_INL_H_
