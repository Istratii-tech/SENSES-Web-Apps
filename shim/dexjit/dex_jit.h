// dex-to-wasm compiler of the ART JIT (topic "dex-jit", docs/topics/dex-jit.md).
//
// Compiles one method of verified, quickened dex code into a small wasm
// module that imports the process's memory and the runtime helpers below
// and exports one function
//
//   f(ShadowFrame* frame, Thread* self, JValue* result) -> i32
//
// which runs the method in its interpreter frame: dex registers live in
// wasm locals and are written back to the frame ("flushed") before every
// call into the runtime that may look at the frame (GC, stack walks,
// exceptions), references to both halves of the frame (the GC reads the
// reference half). It returns 0 when the method returned (the value in
// *result), 1 when the interpreter has to take over the frame at its dex pc
// (the frame is complete; an exception may be pending there).
//
// The compiler knows nothing of ART's classes: the VM side (one per ART
// version) gives the layout constants and answers questions about the
// constant pool. Part of the arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_DEXJIT_DEX_JIT_H_
#define ARC_WASM_SHIM_DEXJIT_DEX_JIT_H_

#include <stdint.h>

#include <string>
#include <vector>

namespace dexjit {

// Offsets and constants the generated code uses.
struct Layout {
  uint32_t vregs = 0;             // ShadowFrame::VRegsOffset()
  uint32_t dex_pc = 0;            // ShadowFrame::DexPCOffset()
  uint32_t thread_flags = 0;      // the 16 flag bits of Thread::state_and_flags
  uint32_t thread_exception = 0;  // Thread::ExceptionOffset
  uint32_t object_class = 0;      // mirror::Object::ClassOffset
  uint32_t class_component = 0;   // mirror::Class::ComponentTypeOffset
  uint32_t array_length = 0;      // mirror::Array::LengthOffset
  uint32_t array_data[4] = {};    // mirror::Array::DataOffset of 1, 2, 4, 8-byte components
  uint32_t object_class_ptr = 0;  // java.lang.Object's class (aput-object of Object[])
  uint32_t card_table = 0;        // CardTable::GetBiasedBegin (0: no card marking)
  uint32_t card_shift = 0;
  uint32_t card_dirty = 0;
  bool memory_shared = true;      // the process's memory is a SharedArrayBuffer

  // Calls between compiled methods: the compiled caller dispatches and
  // copies the arguments; art_jit_enter/art_jit_leave make the frame.
  bool fast_calls = false;
  bool wasm_frames = false;              // the frame in wasm, else art_jit_enter/leave
  uint32_t frame_num_vregs = 0;          // ShadowFrame::NumberOfVRegsOffset
  // The callee's frame is built in wasm (functions $enter/$invoke of the
  // module): ShadowFrame fields (the rest of its header is zeroed; code item:
  // 0 if none), the thread's top shadow frame, the JIT's block of the thread
  // (ArtJitThread: table indices, their capacity, use bits, nesting, frame
  // stack chunk), ArtJitMethod's fields, the frame stack chunk's.
  uint32_t frame_link = 0, frame_method = 0, frame_code_item = 0;
  uint32_t thread_top_shadow_frame = 0, thread_jit = 0;
  uint32_t jit_fns = 0, jit_cap = 0, jit_used = 0, jit_depth = 0, jit_chunk = 0;
  uint32_t jit_depth_cap = 0;
  uint32_t method_data = 0;              // ArtMethod: the ArtJitMethod
  uint32_t jit_method_id = 0, jit_method_code_item = 0, jit_method_registers = 0;
  uint32_t jit_method_method = 0;        // ArtJitMethod::method (whether the record is ours)
  uint32_t chunk_top = 0, chunk_end = 0, chunk_data = 0;
  uint32_t calls_ok = 0;                 // address of a byte: 0 = no direct calls now
  uint32_t method_declaring_class = 0;   // ArtMethod
  uint32_t method_dex_index = 0;         // (0xffffffff: a runtime method, e.g. an IMT conflict)
  uint32_t class_status = 0, class_status_shift = 0, class_initialized = 0;
  uint32_t class_vtable = 0;             // the embedded vtable
  uint32_t class_imt = 0;                // the IMT pointer
  // Intrinsics in place of calls.
  uint32_t string_count = 0, string_value = 0;   // mirror::String (count: length << 1 | uncompressed)
  bool string_compression = true;
  uint32_t thread_peer = 0;              // Thread: the java.lang.Thread (0: no intrinsic)
  // $enter/$invoke imported ("E", "I", after the helpers) from a module of
  // their own (BuildCallsModule), one per thread, instead of a copy in each
  // method's module (topic jit-module-heap: JavaScriptCore compiles each
  // copy, hot as they are, to its top tier).
  bool shared_calls = false;
  // Direct calls (topic dex-jit-speed): a method of at most direct_max_ins
  // argument registers is compiled as
  //   g(self, fp, a0, ..., a(n-1)) -> i64 (the result)
  // with the arguments as parameters; fp is the caller's frame (the link),
  // or the method's own frame | 1 when the runtime made it (and calls g with
  // the arguments from there).
  // g makes its frame only when it needs one (a call, a helper, an
  // exception: $mat "F" of the calls module) and pops it on return ($pop
  // "P"); when the interpreter has to take over, the finish helper runs the
  // frame to its end and g returns its result. Compiled callers call g with
  // call_indirect of its type; a pending exception afterwards is the callee's.
  bool direct_calls = false;
  uint32_t direct_max_ins = 8;
  // Local copies (with direct calls): small methods a call site names (a
  // known target, or the vtable's usual one under a check) compiled as g
  // into the caller's module too and called there with call, which engines
  // inline; at most local_max of them a module, each of at most
  // local_max_insns code units (Resolver::Local).
  uint32_t local_max = 0;
  uint32_t local_max_insns = 0;
  // Loops read the thread's flags (suspend requests) every this many
  // iterations (1: every one); an atomic load each time.
  uint32_t suspend_poll = 1;
  // Allocation in wasm (topic dex-jit-speed; with direct calls: $alloc of
  // the calls module): new-instance and new-array of small objects from the
  // thread's RosAlloc run, as ART's assembly fast paths do
  // (art_quick_alloc_object_rosalloc): the slot from the run's free list,
  // the class stored over its next pointer, the object pushed on the
  // thread's allocation stack. alloc_ok: address of a byte, 1 while this may
  // be used (RosAlloc the allocator, allocations not instrumented); 0: no
  // allocation in wasm.
  uint32_t alloc_ok = 0;
  uint32_t class_object_size_fast = 0;    // Class::object_size_alloc_fast_path_
  uint32_t thread_rosalloc_runs = 0;      // Thread: the runs of the small brackets
  uint32_t thread_alloc_stack_top = 0, thread_alloc_stack_end = 0;
  uint32_t rosalloc_max_size = 0;         // the largest bracket of thread-local runs
  uint32_t rosalloc_quantum_shift = 0;    // its quantum (log2)
  uint32_t run_free_list_head = 0, run_free_list_size = 0;   // RosAlloc::Run
};

// Methods the compiler computes in place of the call.
enum Intrinsic : uint8_t {
  kIntrinsicNone = 0,
  kStringLength, kStringCharAt, kStringIsEmpty,
  kMathAbsInt, kMathAbsLong, kMathAbsFloat, kMathAbsDouble,
  kMathMinIntInt, kMathMaxIntInt, kMathMinLongLong, kMathMaxLongLong,
  kMathMinFloatFloat, kMathMaxFloatFloat, kMathMinDoubleDouble, kMathMaxDoubleDouble,
  kMathSqrt, kMathCeil, kMathFloor, kMathRint,
  kFloatFloatToRawIntBits, kFloatIntBitsToFloat, kDoubleDoubleToRawLongBits,
  kDoubleLongBitsToDouble,
  kIntegerBitCount, kIntegerNumberOfLeadingZeros, kIntegerNumberOfTrailingZeros,
  kIntegerRotateLeft, kIntegerRotateRight, kIntegerSignum,
  kLongBitCount, kLongNumberOfLeadingZeros, kLongNumberOfTrailingZeros,
  kLongRotateLeft, kLongRotateRight, kLongSignum,
  kThreadCurrentThread,
  kIntrinsicCount
};
// The intrinsic of a method by its class descriptor, name and signature
// (kIntrinsicNone if none).
Intrinsic IntrinsicOf(const char* class_descriptor, const char* name, const char* signature);

// The target of an invoke, as far as the compiler can know it.
struct Callee {
  enum Kind { kNone, kKnown, kVirtual, kInterface } kind = kNone;
  uint32_t method = 0;       // kKnown: the method
  bool is_static = false;
  bool initialized = true;   // kKnown static: its class was initialized
  uint32_t index = 0;        // kVirtual: vtable index; kInterface: IMT index
  // kKnown: computed in place (intrinsic) or its code inlined, if small and
  // simple enough; kVirtual: |method| is the one the vtable usually holds,
  // inlined under a check of the vtable entry.
  Intrinsic intrinsic = kIntrinsicNone;
  const uint16_t* insns = nullptr;   // the target's quickened code (its class initialized)
  uint32_t insns_size = 0, registers = 0, ins = 0;
};

// A try item of the method (for the analysis of which registers hold
// references: handlers are entered with the state of their try range).
struct TryItem {
  uint32_t start = 0, count = 0;
  std::vector<uint32_t> handlers;  // handler dex pcs (catch-all included)
};

struct Method {
  uint32_t art_method = 0;      // the ArtMethod (direct calls: its frame's method)
  const uint16_t* insns = nullptr;
  uint32_t insns_size = 0;      // code units
  uint32_t registers = 0;
  uint32_t ins = 0;
  const char* shorty = "";      // return type first
  bool is_static = false;
  std::vector<TryItem> tries;
};

// Constant-pool questions, answered when the method is compiled. A "no"
// makes the instruction call the runtime instead.
class Resolver {
 public:
  virtual ~Resolver() {}
  // The resolved String of const-string, or 0.
  virtual uint32_t String(uint32_t string_idx) = 0;
  // The resolved class of a type index, or 0 (const-class, check-cast,
  // instance-of; the class must stay where it is).
  virtual uint32_t Class(uint32_t type_idx) = 0;
  // A static field that is resolved, of an initialized class: the class
  // object (which holds the field), the field's offset in it, whether it is
  // volatile (accessed with atomics then).
  virtual bool StaticField(uint32_t field_idx, uint32_t* object, uint32_t* offset,
                           bool* is_volatile) = 0;
  // An instance field that is resolved (iget/iput the verifier did not
  // quicken: volatile ones, mostly): its offset, whether it is volatile.
  virtual bool InstanceField(uint32_t field_idx, uint32_t* offset, bool* is_volatile) {
    return false;
  }
  // new-instance of a type: its resolved class, if instantiable and
  // initialized (whether it is finalizable the class says at run time: its
  // fast path size), or 0.
  virtual uint32_t NewInstanceClass(uint32_t type_idx) { return 0; }
  // new-array of a type: its resolved array class and the component size's
  // log2, or 0.
  virtual uint32_t NewArrayClass(uint32_t type_idx, uint32_t* shift) { return 0; }
  // invoke-direct of String.<init> (the runtime replaces every copy of the
  // receiver in the caller's frame with the new string).
  virtual bool IsStringInit(uint32_t method_idx) = 0;
  // How invoke |opcode| of |method_idx| finds its target (kNone: the invoke
  // helper does everything).
  virtual void Invoke(uint8_t opcode, uint32_t method_idx, Callee* out) { out->kind = Callee::kNone; }
  // Layout::local_max: |method| (a call's target) to be compiled into the
  // caller's module: its code in *out and the resolver of its constant pool
  // (alive while this one is), or nullptr when it may not be.
  virtual Resolver* Local(uint32_t method, Method* out) { return nullptr; }
};

// Runtime functions the generated code imports (module "e", fields "a",
// "b", ... in this order; the memory is "M", the function table "T"); the VM side exports functions of these names
// (kHelperNames) and signatures and maps them.
enum Helper {
  kHelperInvoke,       // (self, frame, pc) -> i64        invoke-* at pc
  kHelperInterpret,    // (self, frame, pc) -> i64        one instruction in the interpreter
  kHelperNewInstance,  // (self, frame, pc) -> i32
  kHelperNewArray,     // (self, frame, pc, length) -> i32
  kHelperConstString,  // (self, frame, pc) -> i32
  kHelperConstClass,   // (self, frame, pc) -> i32
  kHelperCheckCast,    // (self, frame, pc, obj)
  kHelperInstanceOf,   // (self, frame, pc, obj) -> i32
  kHelperMonitor,      // (self, frame, pc, obj, enter)
  kHelperThrow,        // (self, frame, pc, kind, a, b)  (ThrowKind)
  kHelperSuspend,      // (self, frame, pc)
  kHelperAputObject,   // (self, frame, pc, array, index, value)
  kHelperFmod,         // (f64, f64) -> f64
  kHelperEnter,        // (self, frame, method) -> i64  callee frame | table index << 32, or 0
  kHelperLeave,        // (self, callee frame, status) -> i64  pops it; the result
  // Direct calls (Layout::direct_calls):
  kHelperFrame,        // (self, link, method, registers) -> frame   made and pushed (no room in the chunk)
  kHelperFinish,       // (self, frame, given) -> i64  the interpreter runs it to the end; pops it unless given
  kHelperPop,          // (self, frame)  frees it (the first of its chunk)
  // A call whose target the compiled code found (dispatched) but cannot call
  // directly (native, intrinsic, interpreted, ...): the invoke instruction's
  // address and the target; the dex pc is in the frame.
  kHelperCall,         // (self, frame, insn, method) -> i64
  kHelperCount
};
extern const char* const kHelperNames[kHelperCount];

enum ThrowKind {
  kThrowNullPointer = 0,     // of the instruction at pc
  kThrowArrayIndex = 1,      // index a, length b
  kThrowDivideByZero = 2,
  kThrowObject = 3,          // the throw instruction; a = the object (null: NPE)
};

struct Options {
  // dex pc the function starts at: 0, or a loop header (on-stack
  // replacement from the interpreter, which hands over a complete frame).
  uint32_t entry_pc = 0;
  // On-stack replacement (f with the interpreter's frame, also at dex pc 0:
  // never g).
  bool osr = false;
  // Methods larger than this are not compiled (code units).
  uint32_t max_insns = 6000;
  uint32_t max_registers = 512;
};

struct Stats {
  uint32_t blocks = 0;
  uint32_t interpreted = 0;   // instructions left to the interpreter helper
  uint32_t inlined = 0;       // calls replaced by intrinsics or inlined bodies
  uint32_t locals = 0;        // local copies of call targets (Layout::local_max)
};

// Compiles |m| into a module's bytes. False with the reason in |why| when
// it cannot (irreducible control flow, size limits, unknown instructions).
bool Compile(const Method& m, const Layout& layout, Resolver* resolver, const Options& options,
             std::vector<uint8_t>* out, std::string* why, Stats* stats = nullptr);

// The module of $enter and $invoke for Layout::shared_calls: imports as a
// method's (the memory, the table, the helpers), exports "E" and "I"; with
// Layout::direct_calls instead $mat "F", $pop "P" and $alloc "A".
void BuildCallsModule(const Layout& layout, std::vector<uint8_t>* out);

// Targets of backward branches (where the interpreter offers on-stack
// replacement), for tests.
std::vector<uint32_t> BackwardBranchTargets(const Method& m);

}  // namespace dexjit

#endif  // ARC_WASM_SHIM_DEXJIT_DEX_JIT_H_
