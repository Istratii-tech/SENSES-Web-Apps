// The dex JIT on ART 10 (topic "dex-jit", docs/topics/dex-jit.md): hot
// methods compiled into wasm by shim/dexjit (dex_jit.h), installed per
// thread in the function table, called by the flat interpreter loop, by
// Execute, by the compiled code's invoke helper and by on-stack replacement;
// the runtime helpers the compiled code imports.
//
// Switch: ARC_JIT (environment) or the system property ro.arcwasm.jit, 1 by
// default; 0 turns it off. Tuning (environment): ARC_JIT_THRESHOLD (calls
// before a method is compiled), ARC_JIT_OSR=0, ARC_JIT_THREAD_KB (compiled
// wasm one thread instantiates at most; least recently used are dropped),
// ARC_JIT_CODE_KB (compiled wasm kept in all), ARC_JIT_DEPTH (nested
// compiled calls), ARC_JIT_LOG=1|2, ARC_JIT_DUMP=dir (Node: the modules).
// Part of the arc-wasm shim layer.
#include "dex_jit_art10.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include "art_method-inl.h"
#include "class_linker-inl.h"
#include "class_root.h"
#include "common_throws.h"
#include "dex/dex_file_exception_helpers.h"
#include "dex/code_item_accessors-inl.h"
#include "dex/dex_file-inl.h"
#include "dex/dex_instruction-inl.h"
#include "entrypoints/entrypoint_utils-inl.h"
#include "gc/accounting/card_table.h"
#include "gc/allocator/rosalloc.h"
#include "gc/heap.h"
#include "imtable-inl.h"
#include "instrumentation.h"
#include "interpreter/interpreter_common.h"
#include "interpreter/interpreter_intrinsics.h"
#include "interpreter/interpreter_switch_impl-inl.h"
#include "interpreter/shadow_frame.h"
#include "jvalue-inl.h"
#include "mirror/array-inl.h"
#include "mirror/class-inl.h"
#include "mirror/dex_cache-inl.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/string-inl.h"
#include "runtime.h"
#include "thread-inl.h"

#include "../dexjit/dex_jit.h"
#include "../membudget/mem_budget.h"

extern "C" int __system_property_get(const char* name, char* value) __attribute__((weak));

#ifndef EMSCRIPTEN_KEEPALIVE
#define EMSCRIPTEN_KEEPALIVE
#endif
// For the page's log (demo/senses-apps-vm/run.html, every 5 s): modules
// compiled, instances made on all threads, instances dropped (eviction), KB
// compiled (topic jit-module-heap: what the JIT's modules cost a browser).
extern "C" EMSCRIPTEN_KEEPALIVE uint32_t arc_dexjit_counts[4] = {0, 0, 0, 0};

namespace art {
namespace interpreter {

uint8_t g_art_jit_off = 0;
uint16_t g_art_jit_threshold = 1000;
uint8_t g_art_jit_osr = 0;
uint32_t g_art_wasm_yield_branches = 65536;
__thread ArtJitThread t_art_jit = {nullptr, 0, nullptr, 0, nullptr};
uint32_t g_art_jit_depth_cap = 48;
// 1 while compiled code may call compiled code directly (no method entry or
// exit listeners); compiled code reads it (dex_jit.h, Layout::calls_ok).
uint8_t g_art_jit_calls_ok = 1;
// 1 while compiled code may allocate small objects itself (RosAlloc the
// allocator, allocations not instrumented; dex_jit.h, Layout::alloc_ok).
uint8_t g_art_jit_alloc_ok = 0;

namespace {

struct JitCode {
  std::vector<uint8_t> bytes;
  ArtMethod* method;
  uint32_t entry_pc;
};

std::mutex g_mu;
std::vector<JitCode*> g_codes(1, nullptr);   // index = code id (0 unused)
dexjit::Layout g_layout;
bool g_inited = false;
int g_log = 0;
const char* g_dump = nullptr;
// Debugging: compile only methods whose name contains one of these (','
// separated), or none that contains one of those.
std::vector<std::string> g_only, g_skip;
uint32_t g_thread_cap = 8u << 20;
// The module of $enter/$invoke (Layout::shared_calls), or of $mat and $pop
// (Layout::direct_calls); empty without.
std::vector<uint8_t> g_calls;
uint32_t g_code_cap = 32u << 20;
uint32_t g_code_bytes = 0;
uint32_t g_compiled = 0, g_refused = 0, g_osr_compiled = 0;
bool g_code_full = false;
bool g_alloc_inline = false;   // ARC_JIT_ALLOC (1): allocation in compiled code
bool g_handlers = true;        // ARC_JIT_HANDLERS (1): catch handlers continue compiled
// Debugging (ARC_JIT_PROF=N): every N calls of the invoke helper, the
// call sites that used it most and their targets.
uint32_t g_prof = 0;
std::mutex g_prof_mu;
std::map<uint64_t, uint32_t> g_prof_sites;
uint64_t g_prof_calls = 0;

__thread uint32_t t_bytes = 0;
__thread uint32_t t_instances = 0;
__thread double t_instantiate_ms = 0;
// Calls on a thread before it instantiates code another thread compiled
// (each thread compiles and instantiates the modules it runs: only for
// what is hot on it too).
uint32_t g_thread_threshold = 200;
// A call from compiled code into a method still interpreted counts this many
// times towards compiling it: such calls cost more than the interpreter's
// own (a nested interpreter loop), so a hot compiled caller pulls its
// callees in.
uint32_t g_caller_weight = 8;

#ifdef __EMSCRIPTEN__
// The function's table index on this thread; -1 when the module fails to
// compile or link (a bug: the code is dropped), -2 when the table cannot
// grow (the JIT is turned off). Each thread (emscripten pthread) is its own
// instance with its own table: every thread instantiates what it runs.
// |calls|: the module of $enter/$invoke or $mat/$pop (Layout::shared_calls,
// direct_calls; else 0), instantiated once per thread before the first method.
EM_JS(int, art_jit_instantiate, (const uint8_t* bytes, int len, const uint8_t* calls, int calls_len), {
  // A free slot of the function table, or -2 if it cannot grow.
  var artJitSlot = globalThis.artJitSlot || (globalThis.artJitSlot = function() {
    var free = globalThis.artJitFree || (globalThis.artJitFree = []);
    if (free.length) return free.pop();
    // Many slots at a time (Safari copies the table on every grow).
    if (!globalThis.artJitNext || globalThis.artJitNext >= globalThis.artJitEnd ||
        globalThis.artJitEnd != wasmTable.length) {
      globalThis.artJitNext = wasmTable.length;
      try {
        wasmTable.grow(256);
      } catch (g) {
        err('dex-jit: the function table cannot grow: ' + g);
        return -2;
      }
      globalThis.artJitEnd = wasmTable.length;
    }
    return globalThis.artJitNext++;
  });
  try {
    var imports = globalThis.artJitImports;
    if (!imports) {
      var e = {M: wasmMemory};
      ['art_jit_invoke', 'art_jit_interpret', 'art_jit_new_instance', 'art_jit_new_array',
       'art_jit_const_string', 'art_jit_const_class', 'art_jit_check_cast', 'art_jit_instance_of',
       'art_jit_monitor', 'art_jit_throw', 'art_jit_suspend', 'art_jit_aput_object',
       'art_jit_fmod', 'art_jit_enter', 'art_jit_leave', 'art_jit_frame', 'art_jit_finish',
       'art_jit_pop', 'art_jit_call'].forEach(function(n, i) {
        e[String.fromCharCode(97 + i)] = wasmExports[n];   // (dexjit::Helper order)
      });
      e.T = wasmTable;
      if (calls_len > 0) {
        var c = new WebAssembly.Instance(new WebAssembly.Module(
            new Uint8Array(wasmMemory.buffer, calls >>> 0, calls_len).slice()), {e: e});
        if (c.exports.F) {
          // Direct calls: $mat, $pop, $alloc.
          e.F = c.exports.F;
          e.P = c.exports.P;
          e.A = c.exports.A;
        } else {
          e.E = c.exports.E;
          e.I = c.exports.I;
        }
      }
      imports = globalThis.artJitImports = {e: e};
    }
    // WebAssembly.Module takes no views of shared memory: a copy, in a kept
    // buffer (a new one per module is garbage for the thread's GC).
    var scratch = globalThis.artJitScratch;
    if (!scratch || scratch.length < len)
      scratch = globalThis.artJitScratch = new Uint8Array(Math.max(len, 2 * (scratch ? scratch.length : 8192)));
    var code = scratch.subarray(0, len);
    code.set(new Uint8Array(wasmMemory.buffer, bytes >>> 0, len));
    var inst = new WebAssembly.Instance(new WebAssembly.Module(code), imports);
    var index = artJitSlot();
    if (index < 0) return index;
    wasmTable.set(index, inst.exports.f);
    return index;
  } catch (e) {
    err('dex-jit: ' + e);
    return -1;
  }
});
// A dropped instance (eviction, the thread's end) goes only at a full
// collection of the thread's heap: JS garbage now and then makes one
// (shim/ndk/jit_pack.cc, arcJitDrop: the same code; topic jit-module-heap).
EM_JS(void, art_jit_release, (int index), {
  wasmTable.set(index, null);
  (globalThis.artJitFree || (globalThis.artJitFree = [])).push(index);
  var drop = globalThis.arcJitDrop || (globalThis.arcJitDrop = function (n, mb) {
    var dropped = globalThis.arcJitDropped = (globalThis.arcJitDropped || 0) + n;
    if (mb <= 0 || dropped < 256) return;
    globalThis.arcJitDropped = 0;
    var keep = [];
    for (var i = 0; i < mb * 16; i++) {
      var a = new Array(1024);
      for (var j = 0; j < 1024; j++) a[j] = {x: j};
      keep.push(a);
      if (keep.length > 64) keep.shift();
    }
  });
  drop(1, 4);
});
// ARC_JIT_DUMP (Node): a module to a file of the host (the VM's own files
// are not the host's).
EM_JS(void, art_jit_dump, (const char* path, const uint8_t* bytes, int len), {
  try {
    require('fs').writeFileSync(UTF8ToString(path), new Uint8Array(wasmMemory.buffer, bytes, len).slice());
  } catch (e) {
    err('dex-jit: dump: ' + e);
  }
});
EM_JS(int, art_jit_ios, (), {
  var ua = (globalThis.navigator && navigator.userAgent) || "";
  return /iPhone|iPad|iPod/.test(ua) || (/Macintosh/.test(ua) && navigator.maxTouchPoints > 1) ? 1 : 0;
});
// JavaScriptCore (Safari, every browser on iOS): its wasm atomic loads cost
// several plain ones (an iPhone X, iOS 16: ~12 ns).
EM_JS(int, art_jit_jsc, (), {
  var ua = (globalThis.navigator && navigator.userAgent) || "";
  return /iPhone|iPad|iPod/.test(ua) || (/AppleWebKit/.test(ua) && !/Chrome|Chromium|Edg|Android/.test(ua)) ? 1 : 0;
});
EM_JS(int, art_jit_memory_shared, (), {
  return typeof SharedArrayBuffer != 'undefined' && wasmMemory.buffer instanceof SharedArrayBuffer ? 1 : 0;
});
#else
static int art_jit_instantiate(const uint8_t*, int, const uint8_t*, int) { return -2; }
static void art_jit_release(int) {}
static void art_jit_dump(const char*, const uint8_t*, int) {}
static int art_jit_ios() { return 0; }
static int art_jit_memory_shared() { return 0; }
static int art_jit_jsc() { return 0; }
#endif

double NowMs() {
#ifdef __EMSCRIPTEN__
  return emscripten_get_now();
#else
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
#endif
}

uint32_t EnvU32(const char* name, uint32_t def) {
  const char* v = getenv(name);
  return (v && *v) ? static_cast<uint32_t>(strtoul(v, nullptr, 10)) : def;
}

uint32_t ImtIndex(ArtMethod* interface_method) REQUIRES_SHARED(Locks::mutator_lock_);

// What the compiler needs to put |m| in place of a call to it: its intrinsic,
// or its quickened code (only once its class is initialized: verified and
// quickened, nothing rewrites the code any more).
void FillTarget(ArtMethod* m, dexjit::Callee* out) REQUIRES_SHARED(Locks::mutator_lock_) {
  out->intrinsic = dexjit::IntrinsicOf(m->GetDeclaringClassDescriptor(), m->GetName(),
                                       m->GetSignature().ToString().c_str());
  if (m->IsNative() || m->IsAbstract() || m->IsProxyMethod() || !m->SkipAccessChecks() ||
      !m->GetDeclaringClass()->IsInitialized()) {
    return;
  }
  const CodeItemDataAccessor ci(m->DexInstructionData());
  if (!ci.HasCodeItem()) return;
  out->insns = ci.Insns();
  out->insns_size = ci.InsnsSizeInCodeUnits();
  out->registers = ci.RegistersSize();
  out->ins = ci.InsSize();
}

bool Compilable(ArtMethod* m) REQUIRES_SHARED(Locks::mutator_lock_);
ArtJitMethod* GetOrCreateLocked(ArtMethod* m) REQUIRES_SHARED(Locks::mutator_lock_);

// The code of |m| for the compiler.
void FillMethod(ArtMethod* m, dexjit::Method* dm) REQUIRES_SHARED(Locks::mutator_lock_) {
  const CodeItemDataAccessor accessor(m->DexInstructionData());
  dm->art_method = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(m));
  dm->insns = accessor.Insns();
  dm->insns_size = accessor.InsnsSizeInCodeUnits();
  dm->registers = accessor.RegistersSize();
  dm->ins = accessor.InsSize();
  uint32_t shorty_len;
  dm->shorty = m->GetShorty(&shorty_len);
  dm->is_static = m->IsStatic();
  for (const dex::TryItem& ti : accessor.TryItems()) {
    dexjit::TryItem item;
    item.start = ti.start_addr_;
    item.count = ti.insn_count_;
    for (CatchHandlerIterator it(accessor, ti); it.HasNext(); it.Next()) {
      item.handlers.push_back(it.GetHandlerAddress());
    }
    dm->tries.push_back(std::move(item));
  }
}

// Compile-time answers from the method's dex cache.
class Resolver8 : public dexjit::Resolver {
 public:
  explicit Resolver8(ArtMethod* m) REQUIRES_SHARED(Locks::mutator_lock_)
      : m_(m), dex_cache_(m->GetDexCache()) {}

  uint32_t String(uint32_t idx) override NO_THREAD_SAFETY_ANALYSIS {
    ObjPtr<mirror::String> s = dex_cache_->GetResolvedString(dex::StringIndex(idx));
    if (s == nullptr) {
      // (The dex cache is a small hash array: the string may be resolved
      // and evicted; the intern table has it then.)
      s = Runtime::Current()->GetClassLinker()->LookupString(dex::StringIndex(idx), dex_cache_);
    }
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s.Ptr()));
  }
  uint32_t Class(uint32_t idx) override NO_THREAD_SAFETY_ANALYSIS {
    ObjPtr<mirror::Class> c = Runtime::Current()->GetClassLinker()->LookupResolvedType(
        dex::TypeIndex(idx), dex_cache_, m_->GetDeclaringClass()->GetClassLoader());
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(c.Ptr()));
  }
  uint32_t NewInstanceClass(uint32_t idx) override NO_THREAD_SAFETY_ANALYSIS {
    ObjPtr<mirror::Class> c = Runtime::Current()->GetClassLinker()->LookupResolvedType(
        dex::TypeIndex(idx), dex_cache_, m_->GetDeclaringClass()->GetClassLoader());
    if (c == nullptr || c->IsArrayClass() || c->IsStringClass() || !c->IsInstantiable() ||
        !c->IsInitialized()) {
      return 0;
    }
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(c.Ptr()));
  }
  uint32_t NewArrayClass(uint32_t idx, uint32_t* shift) override NO_THREAD_SAFETY_ANALYSIS {
    ObjPtr<mirror::Class> c = Runtime::Current()->GetClassLinker()->LookupResolvedType(
        dex::TypeIndex(idx), dex_cache_, m_->GetDeclaringClass()->GetClassLoader());
    if (c == nullptr || !c->IsArrayClass()) return 0;
    *shift = c->GetComponentSizeShift();
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(c.Ptr()));
  }
  bool StaticField(uint32_t idx, uint32_t* object, uint32_t* offset, bool* is_volatile) override
      NO_THREAD_SAFETY_ANALYSIS {
    ArtField* f = Runtime::Current()->GetClassLinker()->LookupResolvedField(idx, m_, true);
    if (f == nullptr || !f->IsStatic()) return false;
    ObjPtr<mirror::Class> c = f->GetDeclaringClass();
    if (!c->IsInitialized()) return false;
    *object = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(c.Ptr()));
    *offset = f->GetOffset().Uint32Value();
    *is_volatile = f->IsVolatile();
    return true;
  }
  bool InstanceField(uint32_t idx, uint32_t* offset, bool* is_volatile) override
      NO_THREAD_SAFETY_ANALYSIS {
    ArtField* f = Runtime::Current()->GetClassLinker()->LookupResolvedField(idx, m_, false);
    if (f == nullptr || f->IsStatic()) return false;
    *offset = f->GetOffset().Uint32Value();
    *is_volatile = f->IsVolatile();
    return true;
  }
  void Invoke(uint8_t opcode, uint32_t idx, dexjit::Callee* out) override NO_THREAD_SAFETY_ANALYSIS {
    out->kind = dexjit::Callee::kNone;
    ArtMethod* m = Runtime::Current()->GetClassLinker()->LookupResolvedMethod(
        idx, dex_cache_, m_->GetDeclaringClass()->GetClassLoader());
    if (m == nullptr || m->IsRuntimeMethod()) return;
    switch (opcode) {
      case Instruction::INVOKE_STATIC:
      case Instruction::INVOKE_STATIC_RANGE:
        if (!m->IsStatic()) return;
        out->kind = dexjit::Callee::kKnown;
        out->method = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(m));
        out->is_static = true;
        out->initialized = m->GetDeclaringClass()->IsInitialized();
        if (out->initialized) FillTarget(m, out);
        return;
      case Instruction::INVOKE_DIRECT:
      case Instruction::INVOKE_DIRECT_RANGE:
        if (m->IsStatic()) return;
        out->kind = dexjit::Callee::kKnown;
        out->method = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(m));
        FillTarget(m, out);
        return;
      case Instruction::INVOKE_VIRTUAL:
      case Instruction::INVOKE_VIRTUAL_RANGE:
        if (m->IsStatic() || m->GetDeclaringClass()->IsInterface()) return;
        if (m->IsFinal() || m->GetDeclaringClass()->IsFinal()) {
          out->kind = dexjit::Callee::kKnown;   // (never overridden)
          out->method = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(m));
          FillTarget(m, out);
        } else {
          out->kind = dexjit::Callee::kVirtual;
          out->index = m->GetMethodIndex();
          // Inlined under a check that the vtable holds |m| itself.
          if (!m->IsAbstract()) {
            out->method = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(m));
            FillTarget(m, out);
            out->intrinsic = dexjit::kIntrinsicNone;
          }
        }
        return;
      case Instruction::INVOKE_SUPER:
      case Instruction::INVOKE_SUPER_RANGE: {
        // A method of a class (not an interface's default method): the
        // caller's superclass's vtable entry (FindMethodToCall<kSuper>).
        if (m->IsStatic() || m->GetDeclaringClass()->IsInterface()) return;
        ObjPtr<mirror::Class> super_class = m_->GetDeclaringClass()->GetSuperClass();
        if (super_class == nullptr || m->GetMethodIndex() >= super_class->GetVTableLength()) return;
        ArtMethod* target = super_class->GetVTableEntry(m->GetMethodIndex(), kRuntimePointerSize);
        if (target == nullptr || target->IsAbstract()) return;
        out->kind = dexjit::Callee::kKnown;
        out->method = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(target));
        FillTarget(target, out);
        return;
      }
      case Instruction::INVOKE_INTERFACE:
      case Instruction::INVOKE_INTERFACE_RANGE:
        if (m->IsStatic()) return;
        if (m->GetDeclaringClass()->IsObjectClass()) {
          // An Object method through an interface type: the vtable.
          out->kind = dexjit::Callee::kVirtual;
          out->index = m->GetMethodIndex();
        } else if (m->GetDeclaringClass()->IsInterface()) {
          out->kind = dexjit::Callee::kInterface;
          out->index = ImtIndex(m);
        }
        return;
      default:
        return;
    }
  }
  dexjit::Resolver* Local(uint32_t method, dexjit::Method* out) override NO_THREAD_SAFETY_ANALYSIS {
    ArtMethod* m = reinterpret_cast<ArtMethod*>(static_cast<uintptr_t>(method));
    if (!Compilable(m) || !m->GetDeclaringClass()->IsInitialized()) return nullptr;
    // (Under g_mu. The copy's frames take the code item from the record, 8.1.)
    ArtJitMethod* jm = GetOrCreateLocked(m);
    if (jm == nullptr) return nullptr;
    FillMethod(m, out);
    jm->code_item = out->insns;
    jm->local = true;
    locals_.emplace_back(new Resolver8(m));
    return locals_.back().get();
  }
  bool IsStringInit(uint32_t idx) override NO_THREAD_SAFETY_ANALYSIS {
    const DexFile* df = m_->GetDexFile();
    const dex::MethodId& mid = df->GetMethodId(idx);
    return strcmp(df->GetMethodName(mid), "<init>") == 0 &&
           strcmp(df->StringByTypeIdx(mid.class_idx_), "Ljava/lang/String;") == 0;
  }

 private:
  ArtMethod* const m_;
  ObjPtr<mirror::DexCache> dex_cache_;
  std::vector<std::unique_ptr<Resolver8>> locals_;   // of local copies (Local)
};

bool Compilable(ArtMethod* m) REQUIRES_SHARED(Locks::mutator_lock_) {
  return !m->IsNative() && !m->IsAbstract() && !m->IsProxyMethod() && m->SkipAccessChecks() &&
         !m->MustCountLocks() && !m->IsObsolete() && m->GetCodeItem() != nullptr;
}

// The method's code item accessor, from the flat loop's per-thread cache. A
// copy: the cache entry is overwritten by the next method that maps to it
// (any call made while the accessor is in use).
inline CodeItemDataAccessor AccessorOf(ArtMethod* m) REQUIRES_SHARED(Locks::mutator_lock_) {
  return ArtWasmCodeEntryAccessor(ArtWasmMethodCodeEntry(m));
}

// A new code id for |m| at |entry_pc| (|osr|: an on-stack replacement
// entry, an f even at 0), or -1. Under g_mu.
int32_t CompileLocked(ArtMethod* m, uint32_t entry_pc, bool osr) REQUIRES_SHARED(Locks::mutator_lock_) {
  if (g_code_full || !Compilable(m)) return -1;
  dexjit::Method dm;
  FillMethod(m, &dm);
  if (!g_only.empty() || !g_skip.empty()) {
    const std::string name = m->PrettyMethod();
    bool only = g_only.empty();
    for (const std::string& o : g_only) only |= name.find(o) != std::string::npos;
    bool skip = false;
    for (const std::string& k : g_skip) skip |= name.find(k) != std::string::npos;
    if (!only || skip) return -1;
  }
  Resolver8 resolver(m);
  dexjit::Options options;
  options.entry_pc = entry_pc;
  options.osr = osr;
  std::vector<uint8_t> bytes;
  std::string why;
  dexjit::Stats stats;
  const double t0 = g_log ? NowMs() : 0;
  if (!dexjit::Compile(dm, g_layout, &resolver, options, &bytes, &why, &stats)) {
    ++g_refused;
    if (g_log >= 2) {
      fprintf(stderr, "dex-jit: not compiled %s @%u: %s\n", m->PrettyMethod().c_str(), entry_pc,
              why.c_str());
    }
    return -1;
  }
  if (g_code_bytes + bytes.size() > g_code_cap) {
    g_code_full = true;
    fprintf(stderr, "dex-jit: %u KB of compiled code: no more methods are compiled\n",
            g_code_bytes >> 10);
    return -1;
  }
  g_code_bytes += bytes.size();
  __atomic_fetch_add(&arc_dexjit_counts[0], 1, __ATOMIC_RELAXED);
  __atomic_store_n(&arc_dexjit_counts[3], g_code_bytes >> 10, __ATOMIC_RELAXED);
  JitCode* code = new JitCode;
  code->bytes = std::move(bytes);
  code->method = m;
  code->entry_pc = entry_pc;
  g_codes.push_back(code);
  const int32_t id = static_cast<int32_t>(g_codes.size() - 1);
  if (entry_pc == 0) ++g_compiled; else ++g_osr_compiled;
  if (g_log) {
    fprintf(stderr, "dex-jit: #%d %s%s%u: %u code units, %u blocks, %u interpreted, %u inlined, %u local, %zu bytes, %.2f ms\n",
            id, m->PrettyMethod().c_str(), entry_pc ? " osr@" : " ", entry_pc,
            dm.insns_size, stats.blocks, stats.interpreted, stats.inlined, stats.locals, code->bytes.size(),
            NowMs() - t0);
  }
  if (g_dump) {
    char path[512];
    snprintf(path, sizeof(path), "%s/jit%d.wasm", g_dump, id);
    art_jit_dump(path, code->bytes.data(), static_cast<int>(code->bytes.size()));
    if (id == 1 && !g_calls.empty()) {
      snprintf(path, sizeof(path), "%s/calls.wasm", g_dump);
      art_jit_dump(path, g_calls.data(), static_cast<int>(g_calls.size()));
    }
  }
  return id;
}

ArtJitMethod* GetOrCreateLocked(ArtMethod* m) REQUIRES_SHARED(Locks::mutator_lock_) {
  ArtJitMethod* jm = ArtJitDataOf(m);
  if (ArtJitForeign(m, jm)) {
    return nullptr;   // (data_ is not ours: never compiled)
  }
  if (jm == nullptr) {
    jm = new ArtJitMethod();
    jm->method = m;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    m->SetDataPtrSize(jm, kRuntimePointerSize);
  }
  return jm;
}

// Drops this thread's least recently used instances until |need| more
// bytes fit (a second chance for those used since the last pass).
void Evict(uint32_t need) {
  const uint32_t goal = g_thread_cap - g_thread_cap / 4;
  for (int pass = 0; pass < 3 && t_bytes + need > goal; ++pass) {
    for (uint32_t id = 1; id < t_art_jit.cap && t_bytes + need > goal; ++id) {
      const int32_t fi = t_art_jit.fns[id];
      if (fi <= 0) continue;
      if (t_art_jit.used[id] && pass == 0) {
        t_art_jit.used[id] = 0;
        continue;
      }
      art_jit_release(fi);
      __atomic_fetch_add(&arc_dexjit_counts[2], 1, __ATOMIC_RELAXED);
      t_art_jit.fns[id] = -64;   // instantiated again after 64 more calls
      t_art_jit.used[id] = 0;
      uint32_t size;
      {
        std::lock_guard<std::mutex> lock(g_mu);
        size = static_cast<uint32_t>(g_codes[id]->bytes.size());
      }
      t_bytes -= std::min(t_bytes, size);
      --t_instances;
    }
  }
}

// A thread's instances go when it ends (topic jit-module-heap): its
// worker keeps the table for the next thread on it, and a pool's threads
// that come and go each instantiated the hot methods again. Code that runs
// on the thread after this (pthread key destructors come later on
// Emscripten) interprets.
__thread bool t_gone = false;
struct ThreadEnd {
  bool armed = false;
  ~ThreadEnd() {
    t_gone = true;
    uint32_t n = 0;
    for (uint32_t id = 1; id < t_art_jit.cap; ++id) {
      if (t_art_jit.fns[id] <= 0) continue;
      art_jit_release(t_art_jit.fns[id]);
      ++n;
    }
    __atomic_fetch_add(&arc_dexjit_counts[2], n, __ATOMIC_RELAXED);
    int32_t* fns = t_art_jit.fns;
    uint8_t* used = t_art_jit.used;
    t_art_jit.cap = 0;
    t_art_jit.fns = nullptr;
    t_art_jit.used = nullptr;
    free(fns);
    free(used);
    t_bytes = 0;
    t_instances = 0;
  }
};
thread_local ThreadEnd t_end;

// The function of code |id| on this thread, instantiated if need be (at
// once if |hot_here|: this thread made it hot; else after
// g_thread_threshold more calls on this thread).
ArtJitFn Instantiate(uint32_t id, bool hot_here) {
  if (t_gone) return nullptr;
  if (!t_end.armed) t_end.armed = true;   // (its destructor at the thread's end)
  if (t_art_jit.frame_chunk == nullptr) {
    // The thread's block for compiled code (its free mterp ibase slot; set
    // before any compiled code runs on the thread).
    t_art_jit.frame_chunk = &t_art_frame_chunk;
    Thread::Current()->SetMterpCurrentIBase(&t_art_jit);
  }
  if (id >= t_art_jit.cap) {
    {
      std::lock_guard<std::mutex> lock(g_mu);
      if (id >= g_codes.size()) {
        fprintf(stderr, "dex-jit: no code #%u\n", id);
        return nullptr;
      }
    }
    uint32_t cap = t_art_jit.cap ? t_art_jit.cap : 256;
    while (cap <= id) cap *= 2;
    t_art_jit.fns = static_cast<int32_t*>(realloc(t_art_jit.fns, cap * sizeof(int32_t)));
    t_art_jit.used = static_cast<uint8_t*>(realloc(t_art_jit.used, cap));
    memset(t_art_jit.fns + t_art_jit.cap, 0, (cap - t_art_jit.cap) * sizeof(int32_t));
    memset(t_art_jit.used + t_art_jit.cap, 0, cap - t_art_jit.cap);
    t_art_jit.cap = cap;
  }
  int32_t fi = t_art_jit.fns[id];
  if (fi > 0) {
    t_art_jit.used[id] = 1;
    return reinterpret_cast<ArtJitFn>(static_cast<uintptr_t>(fi));
  }
  if (fi == 0 && !hot_here && g_thread_threshold > 0) {
    t_art_jit.fns[id] = -static_cast<int32_t>(g_thread_threshold);
    return nullptr;
  }
  if (fi < 0) {
    if (fi == INT32_MIN || ++t_art_jit.fns[id] < 0) return nullptr;
  }
  const JitCode* code;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    code = id < g_codes.size() ? g_codes[id] : nullptr;
  }
  if (code == nullptr || code->bytes.empty()) {
    // (Not a code id: never instantiated, the caller interprets.)
    fprintf(stderr, "dex-jit: no code #%u\n", id);
    t_art_jit.fns[id] = INT32_MIN;
    return nullptr;
  }
  const uint32_t size = static_cast<uint32_t>(code->bytes.size());
  if (t_bytes + size > g_thread_cap) Evict(size);
  const double t0 = NowMs();
  fi = art_jit_instantiate(code->bytes.data(), static_cast<int>(size), g_calls.data(),
                           static_cast<int>(g_calls.size()));
  t_instantiate_ms += NowMs() - t0;
  if (fi < 0) {
    t_art_jit.fns[id] = INT32_MIN;
    if (fi == -2) {
      g_art_jit_off = 1;
      g_art_jit_osr = 0;
    } else {
      fprintf(stderr, "dex-jit: module #%u (%s @%u) did not instantiate\n", id,
              code->method->PrettyMethod().c_str(), code->entry_pc);
    }
    return nullptr;
  }
  t_art_jit.fns[id] = fi;
  t_art_jit.used[id] = 1;
  t_bytes += size;
  ++t_instances;
  __atomic_fetch_add(&arc_dexjit_counts[1], 1, __ATOMIC_RELAXED);
  if (g_log && (t_instances & (t_instances - 1)) == 0 && t_instances >= 64) {
    fprintf(stderr, "dex-jit: %u modules (%u KB, %.0f ms) on thread %d\n", t_instances, t_bytes >> 10,
            t_instantiate_ms,
            static_cast<int>(GetTid()));
  }
  return reinterpret_cast<ArtJitFn>(static_cast<uintptr_t>(fi));
}

// Whether there is room on the native stacks for another compiled frame.
inline bool StackRoom(Thread* self) {
  if (t_art_jit.depth >= g_art_jit_depth_cap) return false;
  const bool implicit_check = !Runtime::Current()->ExplicitStackOverflowChecks();
  return reinterpret_cast<uint8_t*>(__builtin_frame_address(0)) >=
         self->GetStackEndForInterpreter(implicit_check) + 16 * 1024;
}

// Compiled code finds the JIT's block of the thread (ArtJitThread) through
// its Thread (the free mterp ibase slot, dex_jit.h Layout::thread_jit); the
// block is the worker's (__thread). A guest thread of the NDK bridge's green
// threads may come into Java on another carrier worker than before: the slot
// is set again whenever the runtime enters compiled code.
inline void EnterCompiled(Thread* self) REQUIRES_SHARED(Locks::mutator_lock_) {
  if (UNLIKELY(self->GetMterpCurrentIBase() != &t_art_jit)) {
    if (t_art_jit.frame_chunk == nullptr) t_art_jit.frame_chunk = &t_art_frame_chunk;
    self->SetMterpCurrentIBase(&t_art_jit);
  }
}

// The interpreter continues |sf| at its dex pc (the compiled code flushed
// the frame); a pending exception is handled there first.
// The compiled entry of compiled |m| at |pc| (a catch handler the
// interpreter moved the frame to), compiled the first time (an on-stack
// replacement entry: the frame is complete), or nullptr.
ArtJitFn HandlerEntry(ArtMethod* m, uint32_t pc) REQUIRES_SHARED(Locks::mutator_lock_) {
  if (g_art_jit_off || !g_handlers) return nullptr;
  ArtJitMethod* jm = ArtJitMethodOf(m);
  if (jm == nullptr || jm->id <= 0) return nullptr;   // (only in hot methods)
  int32_t id = 0;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    for (int i = 0; i < jm->num_osr; ++i) {
      if (jm->osr_pc[i] == pc) id = jm->osr_id[i];
    }
    if (id == 0) {
      if (jm->num_osr == ArtJitMethod::kMaxOsr) return nullptr;
      id = CompileLocked(m, pc, true);
      const int k = jm->num_osr++;
      jm->osr_pc[k] = pc;
      jm->osr_id[k] = id > 0 ? id : -1;
    }
  }
  return id > 0 ? Instantiate(static_cast<uint32_t>(id), true) : nullptr;
}

JValue Finish(Thread* self, ShadowFrame* sf, const CodeItemDataAccessor& accessor)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  while (self->IsExceptionPending()) {
    if (!MoveToExceptionHandler(self, *sf, Runtime::Current()->GetInstrumentation())) {
      return JValue();
    }
    // A handler of this method: compiled code from there (topic dex-jit-speed).
    ArtJitFn fn = HandlerEntry(sf->GetMethod(), sf->GetDexPC());
    if (fn == nullptr || !StackRoom(self)) break;
    JValue result;
    EnterCompiled(self);
    ++t_art_jit.depth;
    const int32_t status = fn(sf, self, &result);
    --t_art_jit.depth;
    if (status == 0) return result;
  }
  return ExecuteSwitchImpl<false, false>(self, accessor, *sf, JValue(), false);
}

// AssignRegister of interpreter_common.cc (as ArtWasmAssignRegister).
ALWAYS_INLINE void AssignRegister(ShadowFrame* callee, const ShadowFrame& caller, size_t dest,
                                  size_t src) REQUIRES_SHARED(Locks::mutator_lock_) {
  const uint32_t value = caller.GetVReg(src);
  mirror::Object* o = caller.GetVRegReference<kVerifyNone>(src);
  if (value == reinterpret_cast<uintptr_t>(o)) {
    callee->SetVRegReference(dest, o);
  } else {
    callee->SetVReg(dest, value);
  }
}

// The callee's frame for invoke |inst| of |caller| (|num_regs| registers,
// the arguments in the last ones), not pushed; nullptr if the frame stack
// is full.
template <bool is_range>
ShadowFrame* BuildFrame(const ShadowFrame& caller, const Instruction* inst, uint16_t inst_data,
                        ArtMethod* m, uint16_t num_regs) REQUIRES_SHARED(Locks::mutator_lock_) {
  const uint16_t num_ins = is_range ? inst->VRegA_3rc(inst_data) : inst->VRegA_35c(inst_data);
  void* memory = ArtWasmFlatFrameAlloc(ShadowFrame::ComputeSize(num_regs));
  if (UNLIKELY(memory == nullptr)) {
    return nullptr;
  }
  ShadowFrame* callee = ShadowFrame::CreateForCall(num_regs, num_ins,
                                                   const_cast<ShadowFrame*>(&caller), m, memory);
  const size_t first = num_regs - num_ins;
  if (is_range) {
    const uint32_t vregC = inst->VRegC_3rc();
    for (size_t i = 0; i < num_ins; ++i) AssignRegister(callee, caller, first + i, vregC + i);
  } else {
    uint16_t reg_list = inst->Fetch16(2);
    uint16_t count = num_ins;
    if (count == 5) {
      AssignRegister(callee, caller, first + 4U, (inst_data >> 8) & 0x0f);
      --count;
    }
    for (size_t i = 0; i < count; ++i, reg_list >>= 4) {
      AssignRegister(callee, caller, first + i, reg_list & 0x0f);
    }
  }
  return callee;
}

// Calls interpreted-or-compiled |m| (code item |ci|) for invoke |inst| of
// |caller|: frame, push, class initialization, run, pop.
template <bool is_range>
bool CallWithFrame(Thread* self, ShadowFrame* caller, const Instruction* inst, uint16_t inst_data,
                   bool from_compiled,
                   ArtMethod* m, const CodeItemDataAccessor& ci, JValue* result, bool* done)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  ShadowFrame* callee = BuildFrame<is_range>(*caller, inst, inst_data, m, ci.RegistersSize());
  if (callee == nullptr) {
    *done = false;   // (DoInvoke reports the full frame stack)
    return false;
  }
  *done = true;
  self->PushShadowFrame(callee);
  if (m->IsStatic()) {
    ObjPtr<mirror::Class> declaring_class = m->GetDeclaringClass();
    if (UNLIKELY(!declaring_class->IsInitialized())) {
      StackHandleScope<1> hs(self);
      HandleWrapperObjPtr<mirror::Class> h(hs.NewHandleWrapper(&declaring_class));
      if (UNLIKELY(!Runtime::Current()->GetClassLinker()->EnsureInitialized(self, h, true, true))) {
        self->PopShadowFrame();
        ArtWasmFlatFrameFree(callee);
        return false;
      }
    }
  }
  // (Looked up after the class initialization: that may have run code
  // that evicted instances on this thread.)
  if (from_compiled && g_caller_weight > 1 && !g_art_jit_off) {
    ArtJitMethod* jm = ArtJitMethodOf(m);
    if (jm == nullptr || jm->id == 0) {
      const uint32_t c = m->GetCounter() + g_caller_weight - 1;
      m->SetCounter(static_cast<int16_t>(std::min<uint32_t>(c, 0xfff0)));
    }
  }
  ArtJitFn fn = ArtJitLookup(self, m);
  if (fn != nullptr && StackRoom(self)) {
    *result = ArtJitRun(self, callee, fn);
  } else {
    const bool implicit_check = !Runtime::Current()->ExplicitStackOverflowChecks();
    if (UNLIKELY(reinterpret_cast<uint8_t*>(__builtin_frame_address(0)) <
                 self->GetStackEndForInterpreter(implicit_check))) {
      ThrowStackOverflowError(self);
    } else {
      *result = ExecuteSwitchImpl<false, false>(self, ci, *callee, JValue(), false);
    }
  }
  self->PopShadowFrame();
  ArtWasmFlatFrameFree(callee);
  return !self->IsExceptionPending();
}

// A native method's call (ArtWasmCallNativeFromLoop of the flat loop).
template <bool is_range>
bool CallNative(Thread* self, ShadowFrame* caller, const Instruction* inst, uint16_t inst_data,
                ArtMethod* m, JValue* result, bool* done) REQUIRES_SHARED(Locks::mutator_lock_) {
  const uint16_t num_ins = is_range ? inst->VRegA_3rc(inst_data) : inst->VRegA_35c(inst_data);
  ShadowFrame* frame = BuildFrame<is_range>(*caller, inst, inst_data, m, num_ins);
  if (frame == nullptr) {
    *done = false;
    return false;
  }
  *done = true;
  ArtWasmCallNative(self, frame, result);
  ArtWasmFlatFrameFree(frame);
  return !self->IsExceptionPending();
}

uint32_t ImtIndex(ArtMethod* interface_method) REQUIRES_SHARED(Locks::mutator_lock_) {
  if (LIKELY(interface_method->IsAbstract())) {
    return interface_method->GetImtIndex();
  }
  constexpr size_t kBits = 10;
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

// The call of |m|, the target of invoke |inst| of |sf| (dispatched): an
// intrinsic of the interpreter, a native method, or a method with code
// called with a frame (compiled or interpreted); *done false when none of
// these applies (DoInvoke then). False with an exception pending.
template <bool is_range>
bool CallTarget(Thread* self, ShadowFrame* sf, const Instruction* inst, uint16_t inst_data,
                bool invoke_direct, ArtMethod* m, JValue* result, bool* done)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (!is_range && m->IsIntrinsic() && MterpHandleIntrinsic(sf, m, inst, inst_data, result)) {
    *done = true;
    return !self->IsExceptionPending();
  }
  if (m->IsNative()) {
    if (!m->IsConstructor() && Runtime::Current()->IsStarted()) {
      return CallNative<is_range>(self, sf, inst, inst_data, m, result, done);
    }
  } else if (m->IsInvokable() &&
             !(invoke_direct && m->IsConstructor() && m->GetDeclaringClass()->IsStringClass()) &&
             ArtWasmCanCallFlat(self, m)) {
    const CodeItemDataAccessor ci = AccessorOf(m);
    if (ci.HasCodeItem()) {
      if (invoke_direct && ci.InsnsSizeInCodeUnits() == 1 &&
          ((ci.Insns()[0] & 0xff) == Instruction::RETURN_VOID ||
           (ci.Insns()[0] & 0xff) == Instruction::RETURN_VOID_NO_BARRIER)) {
        QuasiAtomic::ThreadFenceForConstructor();
        *done = true;
        return true;
      }
      return CallWithFrame<is_range>(self, sf, inst, inst_data, true, m, ci, result, done);
    }
  }
  return false;
}

// invoke-* of compiled code: the interpreter's fast paths (as
// ArtWasmTryFlatInvoke), otherwise DoInvoke.
template <InvokeType type, bool is_range, bool is_quick = false>
bool Invoke(Thread* self, ShadowFrame* sf, const Instruction* inst, uint16_t inst_data,
            JValue* result) REQUIRES_SHARED(Locks::mutator_lock_) {
  if (type != kSuper && !is_quick) {
    const uint32_t method_idx = is_range ? inst->VRegB_3rc() : inst->VRegB_35c();
    const uint32_t vregC = is_range ? inst->VRegC_3rc() : inst->VRegC_35c();
    mirror::Object* receiver = type == kStatic ? nullptr : sf->GetVRegReference(vregC);
    ArtMethod* resolved =
        sf->GetMethod()->GetDexCache()->GetResolvedMethod(method_idx, kRuntimePointerSize);
    ArtMethod* m = nullptr;
    if ((type == kStatic || receiver != nullptr) && resolved != nullptr &&
        !resolved->IsRuntimeMethod()) {
      switch (type) {
        case kStatic:
        case kDirect:
          m = resolved;
          break;
        case kVirtual:
          m = receiver->GetClass()->GetEmbeddedVTableEntry(resolved->GetMethodIndex(),
                                                            kRuntimePointerSize);
          break;
        case kInterface:
          if (resolved->GetDeclaringClass()->IsObjectClass()) {
            // (An Object method through an interface type: the vtable.)
            m = receiver->GetClass()->GetEmbeddedVTableEntry(resolved->GetMethodIndex(),
                                                              kRuntimePointerSize);
            break;
          }
          m = receiver->GetClass()->GetImt(kRuntimePointerSize)->Get(ImtIndex(resolved),
                                                                     kRuntimePointerSize);
          if (m->IsRuntimeMethod()) m = nullptr;
          break;
        default:
          break;
      }
    }
    if (m != nullptr) {
      bool done = false;
      const bool ok = CallTarget<is_range>(self, sf, inst, inst_data, type == kDirect, m, result, &done);
      if (done) return ok;
    }
  }
  ShadowFrame** const saved = t_art_flat_out;
  t_art_flat_out = nullptr;
  const bool ok = DoInvoke<type, is_range, false, false, is_quick>(self, *sf, inst, inst_data, result);
  t_art_flat_out = saved;
  return ok;
}

// Compiled code calls compiled code directly only while nobody listens to
// method entries and exits (refreshed on the runtime's paths into compiled
// code; listeners appear only while threads are suspended).
void RefreshCallsOk() REQUIRES_SHARED(Locks::mutator_lock_) {
  const instrumentation::Instrumentation* in = Runtime::Current()->GetInstrumentation();
  g_art_jit_calls_ok = !in->HasMethodEntryListeners() && !in->HasMethodExitListeners() &&
                       !in->HasWatchedFramePopListeners();
  const uint8_t alloc_ok = g_alloc_inline && !in->AllocEntrypointsInstrumented() &&
      Runtime::Current()->GetHeap()->GetCurrentAllocator() == gc::kAllocatorTypeRosAlloc;
  if (UNLIKELY(alloc_ok != g_art_jit_alloc_ok) && g_log) {
    fprintf(stderr, "dex-jit: allocation in compiled code %s (allocator %d, instrumented %d)\n",
            alloc_ok ? "on" : "off",
            static_cast<int>(Runtime::Current()->GetHeap()->GetCurrentAllocator()),
            in->AllocEntrypointsInstrumented() ? 1 : 0);
  }
  g_art_jit_alloc_ok = alloc_ok;
}

}  // namespace

ArtJitFn ArtJitLookupSlow(Thread* self, ArtMethod* m) {
  RefreshCallsOk();
  if (g_art_jit_off || !g_inited) {
    return nullptr;
  }
  ArtJitMethod* jm = ArtJitDataOf(m);
  if (ArtJitForeign(m, jm)) {
    return nullptr;
  }
  bool compiled_here = false;
  if (jm == nullptr || jm->id == 0) {
    std::lock_guard<std::mutex> lock(g_mu);
    jm = GetOrCreateLocked(m);
    if (jm == nullptr) {
      return nullptr;
    }
    if (jm->id == 0) {
      compiled_here = true;
      const int32_t id = CompileLocked(m, 0, false);
      if (id > 0) {
        const CodeItemDataAccessor ci(m->DexInstructionData());
        jm->code_item = ci.Insns();
        jm->registers = ci.RegistersSize();
        jm->ins = ci.InsSize();
        jm->direct = g_layout.direct_calls && jm->ins <= kArtJitMaxDirect;
      }
      __atomic_store_n(&jm->id, id > 0 ? id : -1, __ATOMIC_RELEASE);
    }
  }
  if (jm->id <= 0) {
    return nullptr;
  }
  return Instantiate(static_cast<uint32_t>(jm->id), compiled_here);
}

namespace {

// g of code |fn| (dex_jit.h, Layout::direct_calls) of |ins| argument
// registers, called with |sf|, which the runtime made and pushed.
int64_t CallDirect(Thread* self, ShadowFrame* sf, uint32_t ins, uintptr_t fn)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  const uint32_t fp = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(sf)) | 1;
  const uint32_t* a = reinterpret_cast<const uint32_t*>(sf->GetVRegArgs(sf->NumberOfVRegs() - ins));
  typedef uint32_t U;
  switch (ins) {
    case 0: return reinterpret_cast<int64_t (*)(Thread*, U)>(fn)(self, fp);
    case 1: return reinterpret_cast<int64_t (*)(Thread*, U, U)>(fn)(self, fp, a[0]);
    case 2: return reinterpret_cast<int64_t (*)(Thread*, U, U, U)>(fn)(self, fp, a[0], a[1]);
    case 3: return reinterpret_cast<int64_t (*)(Thread*, U, U, U, U)>(fn)(self, fp, a[0], a[1], a[2]);
    case 4:
      return reinterpret_cast<int64_t (*)(Thread*, U, U, U, U, U)>(fn)(self, fp, a[0], a[1], a[2],
                                                                        a[3]);
    case 5:
      return reinterpret_cast<int64_t (*)(Thread*, U, U, U, U, U, U)>(fn)(self, fp, a[0], a[1],
                                                                           a[2], a[3], a[4]);
    case 6:
      return reinterpret_cast<int64_t (*)(Thread*, U, U, U, U, U, U, U)>(fn)(
          self, fp, a[0], a[1], a[2], a[3], a[4], a[5]);
    case 7:
      return reinterpret_cast<int64_t (*)(Thread*, U, U, U, U, U, U, U, U)>(fn)(
          self, fp, a[0], a[1], a[2], a[3], a[4], a[5], a[6]);
    default:
      static_assert(kArtJitMaxDirect == 8, "CallDirect's cases");
      return reinterpret_cast<int64_t (*)(Thread*, U, U, U, U, U, U, U, U, U)>(fn)(
          self, fp, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
  }
}

}  // namespace

JValue ArtJitRun(Thread* self, ShadowFrame* sf, ArtJitFn fn) {
  RefreshCallsOk();
  const CodeItemDataAccessor ci = AccessorOf(sf->GetMethod());
  if (!StackRoom(self)) {
    return ExecuteSwitchImpl<false, false>(self, ci, *sf, JValue(), false);
  }
  JValue result;
  ArtJitMethod* jm = ArtJitMethodOf(sf->GetMethod());
  if (jm != nullptr && jm->direct) {
    // g with this frame (| 1) and the arguments from it (it finishes the
    // frame in the interpreter itself if it has to).
    EnterCompiled(self);
    ++t_art_jit.depth;
    result.SetJ(CallDirect(self, sf, jm->ins, reinterpret_cast<uintptr_t>(fn)));
    --t_art_jit.depth;
    return result;
  }
  EnterCompiled(self);
  ++t_art_jit.depth;
  const int32_t status = fn(sf, self, &result);
  --t_art_jit.depth;
  if (LIKELY(status == 0)) {
    return result;
  }
  return Finish(self, sf, ci);
}

bool ArtJitCallFromLoop(Thread* self, ShadowFrame* caller, const Instruction* inst,
                        uint16_t inst_data, ArtMethod* m, ArtJitFn fn, JValue* result,
                        bool is_range) {
  UNUSED(fn);
  bool done = false;
  bool ok;
  const CodeItemDataAccessor ci = AccessorOf(m);
  if (is_range) {
    ok = CallWithFrame<true>(self, caller, inst, inst_data, false, m, ci, result, &done);
  } else {
    ok = CallWithFrame<false>(self, caller, inst, inst_data, false, m, ci, result, &done);
  }
  if (!done) {
    ThrowStackOverflowError(self);
    return false;
  }
  return ok;
}

int ArtJitOsr(Thread* self, ShadowFrame** psf, size_t* depth, JValue* result_register,
              JValue* out) {
  ShadowFrame* sf = *psf;
  ArtMethod* const m = sf->GetMethod();
  const uint32_t pc = sf->GetDexPC();
  ArtJitMethod* jm = ArtJitMethodOf(m);
  if (jm != nullptr && jm->id < 0 && jm->num_osr == ArtJitMethod::kMaxOsr) {
    return 0;
  }
  int32_t id = 0;
  if (jm != nullptr) {
    for (int i = 0; i < jm->num_osr; ++i) {
      if (jm->osr_pc[i] == pc) {
        id = jm->osr_id[i];
        break;
      }
    }
  }
  if (id < 0) {
    return 0;
  }
  if (id == 0) {
    if (!g_inited || !Compilable(m) || !ArtWasmCanCallFlat(self, m)) {
      return 0;
    }
    std::lock_guard<std::mutex> lock(g_mu);
    jm = GetOrCreateLocked(m);
    if (jm == nullptr) {
      return 0;
    }
    // Compiled at the second sample in this method (the loop ran
    // 2 * g_art_wasm_yield_branches backward branches).
    if (++jm->osr_samples < 2 || jm->num_osr == ArtJitMethod::kMaxOsr) {
      return 0;
    }
    for (int i = 0; i < jm->num_osr; ++i) {
      if (jm->osr_pc[i] == pc) return 0;
    }
    id = CompileLocked(m, pc, true);
    const int k = jm->num_osr++;
    jm->osr_pc[k] = pc;
    jm->osr_id[k] = id > 0 ? id : -1;
    if (id <= 0) {
      return 0;
    }
  }
  ArtJitFn fn = Instantiate(static_cast<uint32_t>(id), true);
  if (fn == nullptr || !StackRoom(self)) {
    return 0;
  }
  JValue result;
  EnterCompiled(self);
  ++t_art_jit.depth;
  const int32_t status = fn(sf, self, &result);
  --t_art_jit.depth;
  if (status == 0) {
    if (*depth == 0) {
      *out = result;
      return 1;
    }
    // A flat callee returned: its caller continues after the invoke (3
    // code units, like every flat invoke) with the result.
    ShadowFrame* caller = sf->GetLink();
    self->PopShadowFrame();
    ArtWasmFreeFlatFrame(sf);
    --*depth;
    caller->SetDexPC(caller->GetDexPC() + 3);
    *result_register = result;
    *psf = caller;
    return 2;
  }
  // The interpreter continues the frame (after the handler search, which
  // may unwind flat frames).
  *result_register = JValue();
  while (self->IsExceptionPending()) {
    if (MoveToExceptionHandler(self, *sf, Runtime::Current()->GetInstrumentation())) {
      break;
    }
    if (*depth == 0) {
      *out = JValue();
      return 1;
    }
    ShadowFrame* caller = sf->GetLink();
    self->PopShadowFrame();
    ArtWasmFreeFlatFrame(sf);
    --*depth;
    sf = caller;
  }
  *psf = sf;
  return 2;
}

void ArtJitInit() {
  if (g_inited) return;
  // The switch of topic guest-jit: ro.arcwasm.jit (from $ARC_JIT on every
  // stack), else $ARC_JIT itself (no properties loaded yet), on by default.
  int on = 1;
  char value[96] = "";
  if (&__system_property_get != nullptr && __system_property_get("ro.arcwasm.jit", value) > 0) {
    on = strcmp(value, "0") != 0;
  } else if (const char* e = getenv("ARC_JIT")) {
    on = strcmp(e, "0") != 0;
  }
  gc::Heap* heap = Runtime::Current()->GetHeap();
  const gc::CollectorType collector = heap->CurrentCollectorType();
  if (on && collector != gc::kCollectorTypeMS && collector != gc::kCollectorTypeCMS) {
    // Compiled code keeps references in wasm locals between safepoints.
    fprintf(stderr, "dex-jit: off (the heap moves objects)\n");
    on = 0;
  }
  if (!on) {
    g_art_jit_off = 1;
    g_inited = true;
    return;
  }
  const bool ios = art_jit_ios() != 0;
  g_art_jit_threshold = static_cast<uint16_t>(std::min<uint32_t>(EnvU32("ARC_JIT_THRESHOLD", 1000), 60000));
  if (g_art_jit_threshold == 0) g_art_jit_threshold = 1;
  g_art_jit_osr = EnvU32("ARC_JIT_OSR", 1) != 0;
  // The caps of the memory budget's device class (an iPhone: 1 MB a thread,
  // 4 MB in all), else by the browser.
  const ArcMemConfig* mc = arc_mem_config ? arc_mem_config() : nullptr;
  g_thread_cap = EnvU32("ARC_JIT_THREAD_KB", mc ? mc->dex_jit_thread_kb : ios ? 1024 : 8192) << 10;
  g_code_cap = EnvU32("ARC_JIT_CODE_KB", mc ? mc->dex_jit_kb : ios ? 4096 : 32768) << 10;
  g_art_jit_depth_cap = EnvU32("ARC_JIT_DEPTH", 48);
  g_thread_threshold = EnvU32("ARC_JIT_THREAD_THRESHOLD", 200);
  g_caller_weight = EnvU32("ARC_JIT_CALLER_WEIGHT", 8);
  g_log = static_cast<int>(EnvU32("ARC_JIT_LOG", 0));
  g_prof = EnvU32("ARC_JIT_PROF", 0);
  g_handlers = EnvU32("ARC_JIT_HANDLERS", 1) != 0;
  g_dump = getenv("ARC_JIT_DUMP");
  for (int k = 0; k < 2; ++k) {
    const char* v = getenv(k == 0 ? "ARC_JIT_ONLY" : "ARC_JIT_SKIP");
    std::vector<std::string>& list = k == 0 ? g_only : g_skip;
    for (const char* p = v; p && *p;) {
      const char* e = strchr(p, ',');
      list.emplace_back(p, e ? e - p : strlen(p));
      p = e ? e + 1 : nullptr;
    }
  }
  if (g_art_jit_osr) {
    g_art_wasm_yield_branches = EnvU32("ARC_JIT_YIELD", 4096);
  }
  dexjit::Layout& l = g_layout;
  l.vregs = ShadowFrame::VRegsOffset();
  l.dex_pc = ShadowFrame::DexPCOffset();
  l.thread_flags = Thread::ThreadFlagsOffset<kRuntimePointerSize>().Uint32Value();
  l.thread_exception = Thread::ExceptionOffset<kRuntimePointerSize>().Uint32Value();
  l.object_class = mirror::Object::ClassOffset().Uint32Value();
  l.class_component = mirror::Class::ComponentTypeOffset().Uint32Value();
  l.array_length = mirror::Array::LengthOffset().Uint32Value();
  l.array_data[0] = mirror::Array::DataOffset(1).Uint32Value();
  l.array_data[1] = mirror::Array::DataOffset(2).Uint32Value();
  l.array_data[2] = mirror::Array::DataOffset(4).Uint32Value();
  l.array_data[3] = mirror::Array::DataOffset(8).Uint32Value();
  l.object_class_ptr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(
      GetClassRoot<mirror::Object>().Ptr()));
  l.card_table = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(heap->GetCardTable()->GetBiasedBegin()));
  l.card_shift = gc::accounting::CardTable::kCardShift;
  l.card_dirty = gc::accounting::CardTable::kCardDirty;
  l.memory_shared = art_jit_memory_shared() != 0;
  l.fast_calls = EnvU32("ARC_JIT_FAST_CALLS", 1) != 0;
  // Direct calls (topic dex-jit-speed): g with the arguments as parameters,
  // its frame made only when needed; ARC_JIT_DIRECT=0: art_jit_enter/leave.
  l.direct_calls = l.fast_calls && EnvU32("ARC_JIT_DIRECT", 1) != 0;
  l.direct_max_ins = kArtJitMaxDirect;
  l.jit_method_method = offsetof(ArtJitMethod, method);
  l.wasm_frames = !l.direct_calls && EnvU32("ARC_JIT_WASM_FRAMES", 0) != 0;
  // Local copies of small call targets in the caller's module (ARC_JIT_LOCAL
  // of them a module, 0: none; ARC_JIT_LOCAL_INSNS code units each at most).
  // Not on a phone's budget (4 MB of JIT code): Bench ~8% faster, but the
  // compiled code ~40% more (Telegram X: 423 -> 598 KB; topic dex-jit-speed).
  // Loops poll the thread's flags every ARC_JIT_POLL iterations (an atomic
  // load): every 64th in JavaScriptCore (iPhone X: Bench's loops 2-5 times
  // faster), each one in V8 (cheap there).
  l.suspend_poll = std::max<uint32_t>(1, EnvU32("ARC_JIT_POLL", art_jit_jsc() ? 64 : 1));
  if (l.direct_calls) {
    const bool phone = mc ? mc->dex_jit_kb <= 4096 : ios;
    l.local_max = EnvU32("ARC_JIT_LOCAL", phone ? 0 : 4);
    l.local_max_insns = EnvU32("ARC_JIT_LOCAL_INSNS", 32);
  }
  // Small objects allocated in compiled code (ARC_JIT_ALLOC=0: by the runtime).
  g_alloc_inline = EnvU32("ARC_JIT_ALLOC", 1) != 0 && gc::kUseRosAlloc;
  if (g_alloc_inline) {
    using gc::allocator::RosAlloc;
    static_assert(RosAlloc::kThreadLocalBracketQuantumSize == 8, "quantum");
    CHECK_EQ(RosAlloc::RunSlotNextOffset(), mirror::Object::ClassOffset().Uint32Value());
    l.alloc_ok = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_art_jit_alloc_ok));
    l.class_object_size_fast = mirror::Class::ObjectSizeAllocFastPathOffset().Uint32Value();
    l.thread_rosalloc_runs = Thread::RosAllocRunsOffset<kRuntimePointerSize>().Uint32Value();
    l.thread_alloc_stack_top = Thread::ThreadLocalAllocStackTopOffset<kRuntimePointerSize>().Uint32Value();
    l.thread_alloc_stack_end = Thread::ThreadLocalAllocStackEndOffset<kRuntimePointerSize>().Uint32Value();
    l.rosalloc_max_size = RosAlloc::kMaxThreadLocalBracketSize;
    l.rosalloc_quantum_shift = 3;
    l.run_free_list_head = RosAlloc::RunFreeListOffset() + RosAlloc::RunFreeListHeadOffset();
    l.run_free_list_size = RosAlloc::RunFreeListOffset() + RosAlloc::RunFreeListSizeOffset();
  }   // (measured: not faster than art_jit_enter/leave, +20% code)
  l.frame_num_vregs = ShadowFrame::NumberOfVRegsOffset();
  l.frame_link = ShadowFrame::LinkOffset();
  l.frame_method = ShadowFrame::MethodOffset();
  l.frame_code_item = 0;   // (no code item in 10's frames; dex_instructions_ stays null)
  l.thread_top_shadow_frame = Thread::TopShadowFrameOffset<kRuntimePointerSize>().Uint32Value();
  l.thread_jit = Thread::MterpCurrentIBaseOffset<kRuntimePointerSize>().Uint32Value();
  l.jit_fns = offsetof(ArtJitThread, fns);
  l.jit_cap = offsetof(ArtJitThread, cap);
  l.jit_used = offsetof(ArtJitThread, used);
  l.jit_depth = offsetof(ArtJitThread, depth);
  l.jit_chunk = offsetof(ArtJitThread, frame_chunk);
  l.jit_depth_cap = g_art_jit_depth_cap;
  l.method_data = ArtMethod::DataOffset(kRuntimePointerSize).Uint32Value();
  l.jit_method_id = offsetof(ArtJitMethod, id);
  l.jit_method_code_item = offsetof(ArtJitMethod, code_item);
  l.jit_method_registers = offsetof(ArtJitMethod, registers);
  l.chunk_top = offsetof(ArtWasmFrameChunk, top);
  l.chunk_end = offsetof(ArtWasmFrameChunk, end);
  l.chunk_data = sizeof(ArtWasmFrameChunk);
  l.calls_ok = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_art_jit_calls_ok));
  l.method_declaring_class = ArtMethod::DeclaringClassOffset().Uint32Value();
  l.method_dex_index = ArtMethod::DexMethodIndexOffset().Uint32Value();
  l.class_status = mirror::Class::StatusOffset().Uint32Value();
  l.class_status_shift = 32 - 4;   // (the status is in the top bits)
  l.class_initialized = static_cast<uint32_t>(ClassStatus::kInitialized);
  l.class_vtable = mirror::Class::EmbeddedVTableEntryOffset(0, kRuntimePointerSize).Uint32Value();
  l.class_imt = mirror::Class::ImtPtrOffset(kRuntimePointerSize).Uint32Value();
  l.string_count = mirror::String::CountOffset().Uint32Value();
  l.string_value = mirror::String::ValueOffset().Uint32Value();
  l.string_compression = mirror::kUseStringCompression;
  l.thread_peer = Thread::PeerOffset<kRuntimePointerSize>().Uint32Value();
  // $enter/$invoke once per thread (ARC_JIT_SHARED_CALLS=0: in every
  // method's module, as before topic jit-module-heap).
  l.shared_calls = !l.direct_calls && l.fast_calls && EnvU32("ARC_JIT_SHARED_CALLS", 1) != 0;
  if (l.shared_calls || l.direct_calls) dexjit::BuildCallsModule(l, &g_calls);
  if (g_log) {
    fprintf(stderr, "dex-jit: %s\n",
            l.direct_calls ? "direct calls" :
            l.shared_calls ? "$enter/$invoke in a module of their own, one a thread" :
                             "$enter/$invoke in each method's module");
  }
  g_inited = true;
  if (g_log) {
    fprintf(stderr, "dex-jit: on (threshold %u, osr %d, %u KB per thread, %u KB in all)\n",
            g_art_jit_threshold, g_art_jit_osr, g_thread_cap >> 10, g_code_cap >> 10);
  }
}

}  // namespace interpreter
}  // namespace art

namespace art {
namespace interpreter {

void ProfInvokeSite(ShadowFrame* sf, uint32_t pc) REQUIRES_SHARED(Locks::mutator_lock_) {
  std::lock_guard<std::mutex> lock(g_prof_mu);
  ++g_prof_sites[(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(sf->GetMethod())) << 32) | pc];
  if (++g_prof_calls % g_prof != 0) return;
  std::vector<std::pair<uint32_t, uint64_t>> top;
  for (const auto& e : g_prof_sites) top.push_back({e.second, e.first});
  std::sort(top.rbegin(), top.rend());
  fprintf(stderr, "dex-jit: invoke helper, %llu calls; the sites most:\n",
          static_cast<unsigned long long>(g_prof_calls));
  for (size_t i = 0; i < top.size() && i < 20; ++i) {
    ArtMethod* m = reinterpret_cast<ArtMethod*>(static_cast<uintptr_t>(top[i].second >> 32));
    const uint32_t at = static_cast<uint32_t>(top[i].second);
    const Instruction* inst = Instruction::At(m->DexInstructions().Insns() + at);
    const bool quick = inst->Opcode() == Instruction::INVOKE_VIRTUAL_QUICK ||
                       inst->Opcode() == Instruction::INVOKE_VIRTUAL_RANGE_QUICK;
    fprintf(stderr, "  %8u  %s @%u -> %s\n", top[i].first, m->PrettyMethod().c_str(), at,
            quick ? "(quickened virtual)" : m->GetDexFile()->PrettyMethod(inst->VRegB()).c_str());
  }
}

}  // namespace interpreter
}  // namespace art

// --- the helpers compiled code imports (dex_jit.h, Helper) -------------------------------

using art::ArtMethod;
using art::Instruction;
using art::JValue;
using art::ShadowFrame;
using art::Thread;
using art::ObjPtr;
namespace mirror = art::mirror;
namespace interp = art::interpreter;

#define ART_JIT_EXPORT extern "C" EMSCRIPTEN_KEEPALIVE NO_THREAD_SAFETY_ANALYSIS

namespace {

inline const Instruction* At(ShadowFrame* sf, uint32_t pc) NO_THREAD_SAFETY_ANALYSIS {
  sf->SetDexPC(pc);
  return Instruction::At(interp::AccessorOf(sf->GetMethod()).Insns() + pc);
}

inline mirror::Object* Obj(uint32_t p) {
  return reinterpret_cast<mirror::Object*>(static_cast<uintptr_t>(p));
}

inline uint32_t Ptr(ObjPtr<mirror::Object> o) NO_THREAD_SAFETY_ANALYSIS {
  return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(o.Ptr()));
}

}  // namespace

ART_JIT_EXPORT int64_t art_jit_invoke(Thread* self, ShadowFrame* sf, uint32_t pc) {
  if (UNLIKELY(interp::g_prof != 0)) interp::ProfInvokeSite(sf, pc);
  const Instruction* inst = At(sf, pc);
  const uint16_t inst_data = inst->Fetch16(0);
  JValue result;
  using art::InvokeType;
  switch (inst->Opcode(inst_data)) {
    case Instruction::INVOKE_VIRTUAL: interp::Invoke<art::kVirtual, false>(self, sf, inst, inst_data, &result); break;
    case Instruction::INVOKE_SUPER: interp::Invoke<art::kSuper, false>(self, sf, inst, inst_data, &result); break;
    case Instruction::INVOKE_DIRECT: interp::Invoke<art::kDirect, false>(self, sf, inst, inst_data, &result); break;
    case Instruction::INVOKE_STATIC: interp::Invoke<art::kStatic, false>(self, sf, inst, inst_data, &result); break;
    case Instruction::INVOKE_INTERFACE: interp::Invoke<art::kInterface, false>(self, sf, inst, inst_data, &result); break;
    case Instruction::INVOKE_VIRTUAL_RANGE: interp::Invoke<art::kVirtual, true>(self, sf, inst, inst_data, &result); break;
    case Instruction::INVOKE_SUPER_RANGE: interp::Invoke<art::kSuper, true>(self, sf, inst, inst_data, &result); break;
    case Instruction::INVOKE_DIRECT_RANGE: interp::Invoke<art::kDirect, true>(self, sf, inst, inst_data, &result); break;
    case Instruction::INVOKE_STATIC_RANGE: interp::Invoke<art::kStatic, true>(self, sf, inst, inst_data, &result); break;
    case Instruction::INVOKE_INTERFACE_RANGE: interp::Invoke<art::kInterface, true>(self, sf, inst, inst_data, &result); break;
    case Instruction::INVOKE_VIRTUAL_QUICK: interp::Invoke<art::kVirtual, false, true>(self, sf, inst, inst_data, &result); break;
    case Instruction::INVOKE_VIRTUAL_RANGE_QUICK: interp::Invoke<art::kVirtual, true, true>(self, sf, inst, inst_data, &result); break;
    default: LOG(FATAL) << "dex-jit: not an invoke at " << pc; break;
  }
  return result.GetJ();
}

// One instruction in the interpreter (with access checks: that variant
// executes single instructions). The compiled code then compares the
// frame's dex pc with the next instruction's: an exception (pending, dex pc
// back at this instruction), or one handled by a catch in this method, hands
// the frame to the interpreter.
ART_JIT_EXPORT int64_t art_jit_interpret(Thread* self, ShadowFrame* sf, uint32_t pc) {
  const art::CodeItemDataAccessor accessor = interp::AccessorOf(sf->GetMethod());
  const Instruction* inst = At(sf, pc);
  const uint16_t inst_data = inst->Fetch16(0);
  JValue result_register;
  interp::SwitchImplContext ctx{self, accessor, *sf, result_register, false, JValue()};
  bool exit_loop = false;
  interp::InstructionHandler<false, false> h(&ctx, art::Runtime::Current()->GetInstrumentation(),
                                             self, *sf, pc, inst, inst_data, exit_loop);
  switch (inst->Opcode(inst_data)) {
    // (The instructions the compiler leaves to the interpreter.)
#define ART_JIT_INTERPRET(OP) case Instruction::OP: h.OP(); break;
    ART_JIT_INTERPRET(FILLED_NEW_ARRAY) ART_JIT_INTERPRET(FILLED_NEW_ARRAY_RANGE)
    ART_JIT_INTERPRET(FILL_ARRAY_DATA)
    ART_JIT_INTERPRET(INVOKE_POLYMORPHIC) ART_JIT_INTERPRET(INVOKE_POLYMORPHIC_RANGE)
    ART_JIT_INTERPRET(INVOKE_CUSTOM) ART_JIT_INTERPRET(INVOKE_CUSTOM_RANGE)
    ART_JIT_INTERPRET(CONST_METHOD_HANDLE) ART_JIT_INTERPRET(CONST_METHOD_TYPE)
    ART_JIT_INTERPRET(IGET) ART_JIT_INTERPRET(IGET_WIDE) ART_JIT_INTERPRET(IGET_OBJECT)
    ART_JIT_INTERPRET(IGET_BOOLEAN) ART_JIT_INTERPRET(IGET_BYTE) ART_JIT_INTERPRET(IGET_CHAR)
    ART_JIT_INTERPRET(IGET_SHORT)
    ART_JIT_INTERPRET(IPUT) ART_JIT_INTERPRET(IPUT_WIDE) ART_JIT_INTERPRET(IPUT_OBJECT)
    ART_JIT_INTERPRET(IPUT_BOOLEAN) ART_JIT_INTERPRET(IPUT_BYTE) ART_JIT_INTERPRET(IPUT_CHAR)
    ART_JIT_INTERPRET(IPUT_SHORT)
    ART_JIT_INTERPRET(SGET) ART_JIT_INTERPRET(SGET_WIDE) ART_JIT_INTERPRET(SGET_OBJECT)
    ART_JIT_INTERPRET(SGET_BOOLEAN) ART_JIT_INTERPRET(SGET_BYTE) ART_JIT_INTERPRET(SGET_CHAR)
    ART_JIT_INTERPRET(SGET_SHORT)
    ART_JIT_INTERPRET(SPUT) ART_JIT_INTERPRET(SPUT_WIDE) ART_JIT_INTERPRET(SPUT_OBJECT)
    ART_JIT_INTERPRET(SPUT_BOOLEAN) ART_JIT_INTERPRET(SPUT_BYTE) ART_JIT_INTERPRET(SPUT_CHAR)
    ART_JIT_INTERPRET(SPUT_SHORT)
#undef ART_JIT_INTERPRET
    default:
      LOG(FATAL) << "dex-jit: not an instruction for the interpreter helper at " << pc;
  }
  // The compiled code continues if the frame is at the next instruction;
  // an exception (handled or not) leaves the frame to the interpreter, which
  // finds the handler again from this instruction.
  sf->SetDexPC(self->IsExceptionPending() || exit_loop ? pc : inst->GetDexPc(accessor.Insns()));
  return result_register.GetJ();
}

ART_JIT_EXPORT uint32_t art_jit_new_instance(Thread* self, ShadowFrame* sf, uint32_t pc) {
  const Instruction* inst = At(sf, pc);
  ObjPtr<mirror::Class> c = art::ResolveVerifyAndClinit(art::dex::TypeIndex(inst->VRegB_21c()),
                                                        sf->GetMethod(), self, false, false);
  if (c == nullptr) return 0;
  const art::gc::AllocatorType allocator = art::Runtime::Current()->GetHeap()->GetCurrentAllocator();
  ObjPtr<mirror::Object> obj;
  if (UNLIKELY(c->IsStringClass())) {
    obj = mirror::String::AllocEmptyString<true>(self, allocator);
  } else if (art::Runtime::Current()->GetInstrumentation()->AllocEntrypointsInstrumented()) {
    obj = art::AllocObjectFromCode<true>(c.Ptr(), self, allocator);
  } else {
    obj = art::AllocObjectFromCode<false>(c.Ptr(), self, allocator);
  }
  return Ptr(obj);
}

ART_JIT_EXPORT uint32_t art_jit_new_array(Thread* self, ShadowFrame* sf, uint32_t pc, int32_t length) {
  const Instruction* inst = At(sf, pc);
  const art::dex::TypeIndex type(inst->VRegC_22c());
  const art::gc::AllocatorType allocator = art::Runtime::Current()->GetHeap()->GetCurrentAllocator();
  ObjPtr<mirror::Object> obj;
  if (art::Runtime::Current()->GetInstrumentation()->AllocEntrypointsInstrumented()) {
    obj = art::AllocArrayFromCode<false, true>(type, length, sf->GetMethod(), self, allocator);
  } else {
    obj = art::AllocArrayFromCode<false, false>(type, length, sf->GetMethod(), self, allocator);
  }
  return Ptr(obj);
}

ART_JIT_EXPORT uint32_t art_jit_const_string(Thread* self, ShadowFrame* sf, uint32_t pc) {
  const Instruction* inst = At(sf, pc);
  const uint32_t idx = inst->Opcode() == Instruction::CONST_STRING ? inst->VRegB_21c() : inst->VRegB_31c();
  return Ptr(interp::ResolveString(self, *sf, art::dex::StringIndex(idx)));
}

ART_JIT_EXPORT uint32_t art_jit_const_class(Thread* self, ShadowFrame* sf, uint32_t pc) {
  const Instruction* inst = At(sf, pc);
  return Ptr(art::ResolveVerifyAndClinit(art::dex::TypeIndex(inst->VRegB_21c()), sf->GetMethod(),
                                         self, false, false));
}

ART_JIT_EXPORT void art_jit_check_cast(Thread* self, ShadowFrame* sf, uint32_t pc, uint32_t obj) {
  const Instruction* inst = At(sf, pc);
  ObjPtr<mirror::Class> c = art::ResolveVerifyAndClinit(art::dex::TypeIndex(inst->VRegB_21c()),
                                                        sf->GetMethod(), self, false, false);
  if (c == nullptr) return;
  mirror::Object* o = Obj(obj);
  if (o != nullptr && !o->InstanceOf(c)) {
    art::ThrowClassCastException(c, o->GetClass());
  }
}

ART_JIT_EXPORT uint32_t art_jit_instance_of(Thread* self, ShadowFrame* sf, uint32_t pc, uint32_t obj) {
  const Instruction* inst = At(sf, pc);
  ObjPtr<mirror::Class> c = art::ResolveVerifyAndClinit(art::dex::TypeIndex(inst->VRegC_22c()),
                                                        sf->GetMethod(), self, false, false);
  if (c == nullptr) return 0;
  mirror::Object* o = Obj(obj);
  return (o != nullptr && o->InstanceOf(c)) ? 1 : 0;
}

ART_JIT_EXPORT void art_jit_monitor(Thread* self, ShadowFrame* sf, uint32_t pc, uint32_t obj,
                                    int32_t enter) {
  At(sf, pc);
  ObjPtr<mirror::Object> o = Obj(obj);
  if (o == nullptr) {
    interp::ThrowNullPointerExceptionFromInterpreter();
  } else if (enter) {
    interp::DoMonitorEnter<false>(self, sf, o);
  } else {
    interp::DoMonitorExit<false>(self, sf, o);
  }
}

ART_JIT_EXPORT void art_jit_throw(Thread* self, ShadowFrame* sf, uint32_t pc, int32_t kind,
                                  int32_t a, int32_t b) {
  At(sf, pc);
  switch (kind) {
    case dexjit::kThrowNullPointer:
      interp::ThrowNullPointerExceptionFromInterpreter();
      break;
    case dexjit::kThrowArrayIndex:
      art::ThrowArrayIndexOutOfBoundsException(a, b);
      break;
    case dexjit::kThrowDivideByZero:
      art::ThrowArithmeticExceptionDivideByZero();
      break;
    default: {
      mirror::Object* o = Obj(static_cast<uint32_t>(a));
      if (o == nullptr) {
        art::ThrowNullPointerException("throw with null exception");
      } else {
        self->SetException(o->AsThrowable());
      }
      break;
    }
  }
}

ART_JIT_EXPORT void art_jit_suspend(Thread* self, ShadowFrame* sf, uint32_t pc) {
  sf->SetDexPC(pc);
  self->AllowThreadSuspension();
}

ART_JIT_EXPORT void art_jit_aput_object(Thread* self, ShadowFrame* sf, uint32_t pc, uint32_t array,
                                        int32_t index, uint32_t value) {
  At(sf, pc);
  UNUSED(self);
  mirror::Object* a = Obj(array);
  if (a == nullptr) {
    interp::ThrowNullPointerExceptionFromInterpreter();
    return;
  }
  ObjPtr<mirror::ObjectArray<mirror::Object>> arr = a->AsObjectArray<mirror::Object>();
  ObjPtr<mirror::Object> val = Obj(value);
  if (arr->CheckIsValidIndex(index) && arr->CheckAssignable(val)) {
    arr->SetWithoutChecks<false>(index, val);
  }
}

ART_JIT_EXPORT double art_jit_fmod(double a, double b) {
  return fmod(a, b);
}

// A compiled caller's call of |m| (dispatched): when |m| has compiled code
// instantiated on this thread and there is room, its frame (arguments to be
// copied by the caller, the other references cleared), pushed; returns the
// frame | the function's table index << 32. 0: the caller uses the invoke
// helper.
ART_JIT_EXPORT int64_t art_jit_enter(Thread* self, ShadowFrame* caller, ArtMethod* m) {
  if (!interp::g_art_jit_calls_ok || interp::t_art_jit.depth >= interp::g_art_jit_depth_cap) {
    return 0;
  }
  interp::ArtJitMethod* jm = interp::ArtJitMethodOf(m);
  if (jm == nullptr || jm->id <= 0) return 0;
  const uint32_t id = static_cast<uint32_t>(jm->id);
  if (id >= interp::t_art_jit.cap) return 0;
  const int32_t fi = interp::t_art_jit.fns[id];
  if (fi <= 0) return 0;
  const size_t size = ShadowFrame::ComputeSize(jm->registers);
  void* memory = interp::ArtWasmFlatFrameAlloc(size + 8);   // + the result
  if (memory == nullptr) return 0;
  ShadowFrame* f = ShadowFrame::CreateForCall(jm->registers, jm->ins, caller, m, memory);
  self->PushShadowFrame(f);
  ++interp::t_art_jit.depth;
  interp::t_art_jit.used[id] = 1;
  return (static_cast<int64_t>(fi) << 32) | static_cast<uint32_t>(reinterpret_cast<uintptr_t>(f));
}

// After the call of art_jit_enter's frame |f|: pops it, finishing it in the
// interpreter if the compiled code handed it over (status 1).
ART_JIT_EXPORT int64_t art_jit_leave(Thread* self, ShadowFrame* f, int32_t status) {
  --interp::t_art_jit.depth;
  JValue result;
  if (LIKELY(status == 0)) {
    memcpy(&result, reinterpret_cast<uint8_t*>(f) + ShadowFrame::ComputeSize(f->NumberOfVRegs()), 8);
  } else {
    result = interp::Finish(self, f, interp::AccessorOf(f->GetMethod()));
  }
  self->PopShadowFrame();
  interp::ArtWasmFlatFrameFree(f);
  return result.GetJ();
}

// invoke |inst| of compiled code (the dex pc stored) whose target |m| the
// code found but did not call itself; 0 (a null receiver, ...) or an IMT
// conflict method: the invoke helper.
ART_JIT_EXPORT int64_t art_jit_call(Thread* self, ShadowFrame* sf, const Instruction* inst,
                                    ArtMethod* m) {
  const uint16_t inst_data = inst->Fetch16(0);
  const Instruction::Code op = inst->Opcode(inst_data);
  JValue result;
  if (m != nullptr && !m->IsRuntimeMethod() &&
      (!m->IsStatic() || m->GetDeclaringClass()->IsInitialized())) {
    bool done = false;
    const bool direct = op == Instruction::INVOKE_DIRECT || op == Instruction::INVOKE_DIRECT_RANGE;
    if (Instruction::FormatOf(op) == Instruction::k3rc) {
      interp::CallTarget<true>(self, sf, inst, inst_data, direct, m, &result, &done);
    } else {
      interp::CallTarget<false>(self, sf, inst, inst_data, direct, m, &result, &done);
    }
    if (done) return result.GetJ();
  }
  return art_jit_invoke(self, sf, sf->GetDexPC());
}

// Direct calls: g's frame when the current chunk of the frame stack has no
// room (its references cleared), pushed; 0 with StackOverflowError pending
// when the frame stack is full.
ART_JIT_EXPORT uint32_t art_jit_frame(Thread* self, ShadowFrame* link, ArtMethod* m,
                                      uint32_t registers) {
  void* memory = interp::ArtWasmFlatFrameAlloc(ShadowFrame::ComputeSize(registers));
  if (memory == nullptr) {
    art::ThrowStackOverflowError(self);
    return 0;
  }
  ShadowFrame* f = ShadowFrame::CreateForCall(registers, 0, link, m, memory);
  self->PushShadowFrame(f);
  return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(f));
}

// Direct calls: the interpreter runs g's frame |f| from its dex pc (a
// pending exception is handled first) to the end; the frame is popped and
// freed unless the runtime made it (|given|).
ART_JIT_EXPORT int64_t art_jit_finish(Thread* self, ShadowFrame* f, int32_t given) {
  const JValue result = interp::Finish(self, f, interp::AccessorOf(f->GetMethod()));
  if (!given) {
    self->PopShadowFrame();
    interp::ArtWasmFlatFrameFree(f);
  }
  return result.GetJ();
}

// Direct calls: frees g's frame (popped already) when it is the first of
// its chunk.
ART_JIT_EXPORT void art_jit_pop(Thread* self, ShadowFrame* f) {
  UNUSED(self);
  interp::ArtWasmFlatFrameFree(f);
}

// For tests (art/test_natives.cc): 1 compiled, 0 not (yet), -1 refused, 2
// compiled into its callers' modules only (local copies).
ART_JIT_EXPORT int art_jit_method_state(void* method) {
  interp::ArtJitMethod* jm = interp::ArtJitMethodOf(reinterpret_cast<ArtMethod*>(method));
  if (jm == nullptr) return 0;
  return jm->id > 0 ? 1 : jm->id < 0 ? -1 : jm->local ? 2 : 0;
}

// The memory budget (shim/membudget): wasm compiled, kept in the wasm memory
// (and instantiated outside it by each thread that runs it).
extern "C" void art_jit_mem_stats(uint64_t* code_bytes) {
  *code_bytes = __atomic_load_n(&interp::g_code_bytes, __ATOMIC_RELAXED);
}

// For tests: compiled methods, OSR entries, refused, KB of code; -1 when off.
ART_JIT_EXPORT int art_jit_stat(int what) {
  if (interp::g_art_jit_off) return -1;
  switch (what) {
    case 0: return static_cast<int>(interp::g_compiled);
    case 1: return static_cast<int>(interp::g_osr_compiled);
    case 2: return static_cast<int>(interp::g_refused);
    case 3: return static_cast<int>(interp::g_code_bytes >> 10);
    case 4: return static_cast<int>(interp::t_instances);
    default: return 0;
  }
}
