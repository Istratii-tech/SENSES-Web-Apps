// Calls into native code for ART on WebAssembly, where nothing is compiled:
//
// * ArtWasmInterpreterJni: a JNI native method, with any signature. On a
//   device the method's compiled JNI stub (or the generic JNI trampoline)
//   builds the native call; the interpreter has only a handful of shorties
//   hard-coded (InterpreterJni in interpreter.cc). WebAssembly cannot call a
//   function pointer through a mismatched type (call_indirect traps), so the
//   call goes through JavaScript, as Dalvik's does (shim/dalvik/wasm_call.cc):
//   the function is fetched from the module's table and called with an
//   argument array built from the shorty; JS-to-wasm calls coerce arguments.
//   Around it, what JniMethodStart/JniMethodEnd do for compiled stubs: a
//   local reference segment, the transition to kNative (except for fast
//   natives), the monitor of synchronized methods, the reference result.
//
// * ArtWasmInvokeProxy: a method of a java.lang.reflect.Proxy class (on a
//   device: art_quick_proxy_invoke_handler and artQuickProxyInvokeHandler).
//
// Both are called from ArtMethod::Invoke / EnterInterpreterFromInvoke
// (patches/aosp10/art10.patch). The ART 10 version of
// shim/art/jni_call_wasm.cc; besides the API, natives annotated
// @CriticalNative get no JNIEnv* and no class here, as on a device. Part of
// the arc-wasm shim layer.

#include <emscripten/emscripten.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <utility>
#include <vector>

#include "class_linker.h"

#include "nativehelper/ScopedLocalFrame.h"
#include "art_method-inl.h"
#include "entrypoints/entrypoint_utils.h"
#include "entrypoints/runtime_asm_entrypoints.h"
#include "jni/java_vm_ext.h"
#include "jni/jni_env_ext-inl.h"
#include "jvalue-inl.h"
#include "mirror/class-inl.h"
#include "mirror/method.h"
#include "mirror/object-inl.h"
#include "android-base/stringprintf.h"
#include "runtime.h"
#include "scoped_thread_state_change-inl.h"
#include "stack.h"
#include "thread-inl.h"
#include "base/utils.h"

// |kinds| is a NUL-terminated string: the return kind, then one kind per
// argument: 'i' (32-bit int or pointer), 'j' (64-bit int), 'f' (float),
// 'd' (double), 'v' (void, return only). |args| holds the arguments as
// consecutive 32-bit slots; 'j' and 'd' take two (low word first). The
// result is stored at |ret| (8 bytes). Addresses are unsigned (>>> 0): the
// heap may be larger than 2 GB. Returns what it saw of the function's
// signature: 4 if the function takes as many parameters as |kinds| has
// arguments, plus 0 if it returned nothing, 1 a number, 2 a BigInt (see
// TypedNativeCall).
EM_JS(int, arc_art_call_native, (const void* fn, const char* kinds, const uint32_t* args,
                                 void* ret), {
  if (!Module.arcArtScratch) {
    var b = new ArrayBuffer(8);
    Module.arcArtScratch = { u32: new Uint32Array(b), f32: new Float32Array(b),
                             f64: new Float64Array(b) };
  }
  var s = Module.arcArtScratch;
  var a = [];
  var p = args >>> 2;
  for (var k = (kinds >>> 0) + 1; HEAPU8[k]; k++) {
    switch (HEAPU8[k]) {
      case 105: a.push(HEAP32[p++]); break;  // 'i'
      case 102: a.push(HEAPF32[p++]); break;  // 'f'
      case 106:  // 'j'
        a.push(BigInt.asIntN(64, BigInt(HEAPU32[p]) | (BigInt(HEAPU32[p + 1]) << 32n)));
        p += 2;
        break;
      case 100:  // 'd'
        s.u32[0] = HEAPU32[p];
        s.u32[1] = HEAPU32[p + 1];
        a.push(s.f64[0]);
        p += 2;
        break;
    }
  }
  var f = wasmTable.get(fn >>> 0);
  var r;
  try {
    r = f.apply(null, a);
  } catch (e) {
    // A Java signature that does not match the C function (e.g. long vs
    // int) shows up here: name the call before rethrowing.
    if (e instanceof TypeError) {
      err('arc_art_call_native: JNI call failed: kinds ' + UTF8ToString(kinds) +
          ', wasm function ' + f.name + ' (' + f.length + ' params): ' + e);
    }
    throw e;
  }
  var q = ret >>> 0;
  switch (HEAPU8[kinds >>> 0]) {
    case 105: HEAP32[q >>> 2] = r; HEAP32[(q >>> 2) + 1] = 0; break;
    case 102: HEAPF32[q >>> 2] = r; break;
    case 106: HEAP64[q >>> 3] = r; break;
    case 100: HEAPF64[q >>> 3] = r; break;
  }
  return (f.length === a.length ? 4 : 0) |
         (r === undefined ? 0 : typeof r === 'bigint' ? 2 : 1);
});

// The NDK bridge runs natives implemented in ARM code (same arguments as
// arc_art_call_native); weak, so artvm builds without it.
extern "C" bool arc_ndk_art_call_native(const void* fn, const char* kinds,
                                        const uint32_t* args, void* ret)
    __attribute__((weak));

namespace art {
namespace interpreter {

namespace {

// ARC_ART_NO_TYPED_JNI=1: every native through the JavaScript bridge (for
// comparison).
bool TypedCallsEnabled() {
  static const bool enabled = getenv("ARC_ART_NO_TYPED_JNI") == nullptr;
  return enabled;
}

// --- Typed calls ---------------------------------------------------------------
//
// The JavaScript bridge costs far more than most natives. A native whose
// C signature is known can be called from C directly instead: a cast of
// the function pointer to that signature (call_indirect). WebAssembly traps
// if the types do not match exactly, where the bridge would convert (an
// extra or missing parameter, a result the Java declaration does not have,
// an int where Java has a float), so the first call of each method goes
// through the bridge, which reports the function's parameter count and the
// kind of its result; the method is called directly from then on only if
// they match its shorty and its arguments and result are 32/64-bit
// integers (pointers, references, Z B C S I J). Everything else, and
// natives of the NDK bridge, keep using the bridge.

constexpr size_t kMaxTypedJavaArgs = 6;  // besides JNIEnv* and this/class

template <bool kLong, typename R, int N, unsigned Mask, typename... Args> struct TypedStep;

// Calls |fn| with Args..., then N more arguments from |s|: bit i of Mask
// says whether the i-th of those is a long (two slots).
template <typename R, int N, unsigned Mask, typename... Args>
struct TypedCaller {
  static R Call(const void* fn, const uint32_t* s, Args... args) {
    return TypedStep<(Mask & 1) != 0, R, N, Mask, Args...>::Call(fn, s, args...);
  }
};
template <typename R, unsigned Mask, typename... Args>
struct TypedCaller<R, 0, Mask, Args...> {
  static R Call(const void* fn, const uint32_t*, Args... args) {
    return reinterpret_cast<R (*)(Args...)>(const_cast<void*>(fn))(args...);
  }
};
template <typename R, int N, unsigned Mask, typename... Args>
struct TypedStep<false, R, N, Mask, Args...> {
  static R Call(const void* fn, const uint32_t* s, Args... args) {
    return TypedCaller<R, N - 1, (Mask >> 1), Args..., uint32_t>::Call(fn, s + 1, args..., s[0]);
  }
};
template <typename R, int N, unsigned Mask, typename... Args>
struct TypedStep<true, R, N, Mask, Args...> {
  static R Call(const void* fn, const uint32_t* s, Args... args) {
    return TypedCaller<R, N - 1, (Mask >> 1), Args..., uint64_t>::Call(
        fn, s + 2, args..., static_cast<uint64_t>(s[0]) | (static_cast<uint64_t>(s[1]) << 32));
  }
};

// The slots start with JNIEnv* and this/class.
template <typename R, int N, unsigned Mask>
R TypedEntry(const void* fn, const uint32_t* s) {
  return TypedCaller<R, N, Mask, uint32_t, uint32_t>::Call(fn, s + 2, s[0], s[1]);
}

template <typename R> using TypedFn = R (*)(const void*, const uint32_t*);

template <typename R, int N, unsigned... M>
struct TypedTableOf {
  static constexpr TypedFn<R> kFns[sizeof...(M)] = {&TypedEntry<R, N, M>...};
};
template <typename R, int N, unsigned... M>
constexpr TypedFn<R> TypedTableOf<R, N, M...>::kFns[sizeof...(M)];

template <typename R, int N, unsigned... M>
const TypedFn<R>* MakeTypedTable(std::integer_sequence<unsigned, M...>) {
  return TypedTableOf<R, N, M...>::kFns;
}

// TypedTables<R>(n)[mask]: n Java arguments, mask of the longs among them.
template <typename R>
const TypedFn<R>* TypedTables(size_t n) {
  static const TypedFn<R>* const tables[kMaxTypedJavaArgs + 1] = {
    MakeTypedTable<R, 0>(std::make_integer_sequence<unsigned, 1>()),
    MakeTypedTable<R, 1>(std::make_integer_sequence<unsigned, 2>()),
    MakeTypedTable<R, 2>(std::make_integer_sequence<unsigned, 4>()),
    MakeTypedTable<R, 3>(std::make_integer_sequence<unsigned, 8>()),
    MakeTypedTable<R, 4>(std::make_integer_sequence<unsigned, 16>()),
    MakeTypedTable<R, 5>(std::make_integer_sequence<unsigned, 32>()),
    MakeTypedTable<R, 6>(std::make_integer_sequence<unsigned, 64>()),
  };
  return tables[n];
}

// What is known about calling a method's native: one entry per method
// (and function: RegisterNatives may change it). Lock-free; never removed
// (ART 6.0 does not unload classes); when the table is full, natives just
// keep using the bridge.
enum : uint32_t { kCallUnknown = 0, kCallTyped = 1, kCallBridge = 2 };
struct CallKindSlot {
  std::atomic<ArtMethod*> method;
  std::atomic<uint64_t> fn_and_kind;  // function << 32 | kind
};
constexpr size_t kCallKindBits = 14;
constexpr size_t kCallKindProbes = 32;
CallKindSlot g_call_kinds[1u << kCallKindBits];

inline size_t CallKindHash(ArtMethod* m) {
  return (static_cast<uint32_t>(reinterpret_cast<uintptr_t>(m)) * 2654435761u) >>
         (32 - kCallKindBits);
}

uint32_t LookupCallKind(ArtMethod* m, const void* fn) {
  size_t i = CallKindHash(m);
  for (size_t n = 0; n < kCallKindProbes; ++n, i = (i + 1) & ((1u << kCallKindBits) - 1)) {
    ArtMethod* slot_method = g_call_kinds[i].method.load(std::memory_order_acquire);
    if (slot_method == m) {
      const uint64_t v = g_call_kinds[i].fn_and_kind.load(std::memory_order_acquire);
      return static_cast<uint32_t>(v >> 32) == reinterpret_cast<uintptr_t>(fn)
          ? static_cast<uint32_t>(v) : kCallUnknown;
    }
    if (slot_method == nullptr) {
      return kCallUnknown;
    }
  }
  return kCallUnknown;
}

void StoreCallKind(ArtMethod* m, const void* fn, uint32_t kind) {
  const uint64_t v = (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(fn)) << 32) | kind;
  size_t i = CallKindHash(m);
  for (size_t n = 0; n < kCallKindProbes; ++n, i = (i + 1) & ((1u << kCallKindBits) - 1)) {
    ArtMethod* expected = nullptr;
    if (g_call_kinds[i].method.compare_exchange_strong(expected, m, std::memory_order_acq_rel) ||
        expected == m) {
      g_call_kinds[i].fn_and_kind.store(v, std::memory_order_release);
      return;
    }
  }
}

// Whether a native with these |kinds| may be called directly, given what
// the bridge saw of the function (|seen|, from arc_art_call_native).
bool CanCallTyped(const char* kinds, int seen) {
  int expected_result;
  switch (kinds[0]) {
    case 'v': expected_result = 0; break;
    case 'i': expected_result = 1; break;
    case 'j': expected_result = 2; break;
    default: return false;  // float, double
  }
  if ((seen & 4) == 0 || (seen & 3) != expected_result) {
    return false;
  }
  size_t n = 0;
  for (const char* k = kinds + 3; *k != '\0'; ++k, ++n) {
    if ((*k != 'i' && *k != 'j') || n >= kMaxTypedJavaArgs) {
      return false;
    }
  }
  return true;
}

// Calls |fn| directly (CanCallTyped said so); the result as the bridge
// stores it.
void CallTyped(const void* fn, const char* kinds, const uint32_t* slots, JValue* ret) {
  size_t n = 0;
  unsigned mask = 0;
  for (const char* k = kinds + 3; *k != '\0'; ++k, ++n) {
    if (*k == 'j') {
      mask |= 1u << n;
    }
  }
  switch (kinds[0]) {
    case 'v':
      TypedTables<void>(n)[mask](fn, slots);
      break;
    case 'i':
      ret->SetJ(TypedTables<uint32_t>(n)[mask](fn, slots));
      break;
    default:  // 'j'
      ret->SetJ(static_cast<int64_t>(TypedTables<uint64_t>(n)[mask](fn, slots)));
      break;
  }
}

char KindOf(char shorty_type) {
  switch (shorty_type) {
    case 'V': return 'v';
    case 'F': return 'f';
    case 'D': return 'd';
    case 'J': return 'j';
    default: return 'i';  // Z B C S I L
  }
}

// Unlocks a synchronized native's monitor, keeping a pending exception
// (as UnlockJniSynchronizedMethod in quick_jni_entrypoints.cc).
void UnlockSynchronized(Thread* self, jobject locked) REQUIRES_SHARED(Locks::mutator_lock_) {
  StackHandleScope<1> hs(self);
  Handle<mirror::Throwable> saved(hs.NewHandle(self->GetException()));
  self->ClearException();
  self->DecodeJObject(locked)->MonitorExit(self);
  if (UNLIKELY(self->IsExceptionPending())) {
    LOG(FATAL) << "Synchronized JNI code returning with an exception:\n"
               << saved->Dump() << "\nEncountered second exception during implicit MonitorExit:\n"
               << self->GetException()->Dump();
  }
  if (saved.Get() != nullptr) {
    self->SetException(saved.Get());
  }
}

// @FastNative (the native keeps the thread runnable) or @CriticalNative
// (static, primitive arguments and result: no JNIEnv* and no class either).
// Since 9 the class linker keeps both in the method's access flags (from the
// annotations, as it loads the method), so this is a flag test.
enum : uintptr_t { kNativeFast = 1, kNativeCritical = 2 };
inline uintptr_t NativeKind(ArtMethod* m) REQUIRES_SHARED(Locks::mutator_lock_) {
  return m->IsCriticalNative() ? kNativeCritical : m->IsFastNative() ? kNativeFast : 0;
}

}  // namespace

// |args| are the method's arguments without the receiver, in the shadow
// frame of the native method (pushed by EnterInterpreterFromInvoke).
void ArtWasmInterpreterJni(Thread* self, ArtMethod* method, mirror::Object* receiver,
                           uint32_t* args, JValue* result) {
  const void* fn = method->GetEntryPointFromJni();
  if (fn == nullptr || fn == GetJniDlsymLookupStub()) {
    // artFindNativeMethod: the native method is at the top of the stack.
    fn = Runtime::Current()->GetJavaVM()->FindCodeForNativeMethod(method);
    if (fn == nullptr) {
      DCHECK(self->IsExceptionPending());
      return;
    }
    method->RegisterNative(fn);
  }

  uint32_t shorty_len = 0;
  const char* shorty = method->GetShorty(&shorty_len);
  JNIEnvExt* env = self->GetJniEnv();
  const IRTSegmentState saved_cookie = env->GetLocalRefCookie();
  env->SetLocalRefCookie(env->GetLocalsSegmentState());

  // @FastNative / @CriticalNative (see NativeKind): as
  // artQuickGenericJniTrampoline treats them.
  const uintptr_t native_kind = NativeKind(method);
  const bool critical = native_kind == kNativeCritical;

  // env, this or the class, then the arguments (two slots for J and D).
  uint32_t slots[2 + 2 * shorty_len];
  size_t n = 0;
  char kinds[shorty_len + 3];
  size_t k = 0;
  kinds[k++] = KindOf(shorty[0]);
  jobject this_or_class = nullptr;
  if (!critical) {
    kinds[k++] = 'i';
    slots[n++] = reinterpret_cast<uintptr_t>(static_cast<JNIEnv*>(env));
    kinds[k++] = 'i';
    this_or_class = method->IsStatic()
        ? env->AddLocalReference<jobject>(method->GetDeclaringClass())
        : env->AddLocalReference<jobject>(receiver);
    slots[n++] = reinterpret_cast<uintptr_t>(this_or_class);
  }
  size_t a = 0;
  for (uint32_t i = 1; i < shorty_len; ++i) {
    kinds[k++] = KindOf(shorty[i]);
    switch (shorty[i]) {
      case 'L': {
        mirror::Object* o = reinterpret_cast<StackReference<mirror::Object>*>(&args[a++])->AsMirrorPtr();
        slots[n++] = o == nullptr ? 0 : reinterpret_cast<uintptr_t>(env->AddLocalReference<jobject>(o));
        break;
      }
      case 'J':
      case 'D':
        slots[n++] = args[a++];
        slots[n++] = args[a++];
        break;
      default:
        slots[n++] = args[a++];
        break;
    }
  }
  kinds[k] = '\0';

  static const bool trace = getenv("ARC_ART_TRACE_JNI") != nullptr;
  if (UNLIKELY(trace)) {
    std::string a;
    for (size_t i = critical ? 0 : 2; i < n; ++i) {
      a += android::base::StringPrintf(" %#x", slots[i]);
    }
    LOG(INFO) << "JNI call " << method->PrettyMethod() << " kinds " << kinds << " fn " << fn
              << (native_kind == kNativeFast ? " (fast)" : "") << (critical ? " (critical)" : "")
              << " args" << a;
  }
  const bool synchronized = method->IsSynchronized();
  if (synchronized) {
    self->DecodeJObject(this_or_class)->MonitorEnter(self);
    if (UNLIKELY(self->IsExceptionPending())) {
      env->SetLocalSegmentState(env->GetLocalRefCookie());
      env->SetLocalRefCookie(saved_cookie);
      return;
    }
  }
  const bool fast = native_kind != 0;
  if (!fast) {
    self->TransitionFromRunnableToSuspended(kNative);
  }
  JValue ret;
  ret.SetJ(0);
  if (!arc_ndk_art_call_native ||
      !arc_ndk_art_call_native(fn, kinds, slots, &ret)) {
    const uint32_t call_kind = critical ? kCallBridge : LookupCallKind(method, fn);
    if (call_kind == kCallTyped) {
      CallTyped(fn, kinds, slots, &ret);
    } else {
      const int seen = arc_art_call_native(fn, kinds, slots, &ret);
      if (call_kind == kCallUnknown && TypedCallsEnabled()) {
        StoreCallKind(method, fn, CanCallTyped(kinds, seen) ? kCallTyped : kCallBridge);
      }
    }
  }
  if (UNLIKELY(trace)) {
    LOG(INFO) << "JNI return "
              << android::base::StringPrintf("%#llx", static_cast<unsigned long long>(ret.GetJ()));
  }
  if (!fast) {
    self->TransitionFromSuspendedToRunnable();
  } else if (UNLIKELY(self->TestAllFlags())) {
    self->CheckSuspend();
  }
  if (synchronized) {
    UnlockSynchronized(self, this_or_class);
  }

  if (result != nullptr) {
    // Widen sub-word results to the whole int, the way the native ABIs do
    // (the interpreter's move-result reads 32 bits; JValue::SetC and the
    // like would leave the upper bits as they were).
    switch (shorty[0]) {
      case 'L':
        // Decode before the local references are popped. The result may be
        // garbage if an exception is pending.
        result->SetL(self->IsExceptionPending()
                         ? nullptr
                         : self->DecodeJObject(reinterpret_cast<jobject>(ret.GetI())).Ptr());
        break;
      case 'Z': result->SetI(static_cast<uint8_t>(ret.GetI())); break;
      case 'B': result->SetI(static_cast<int8_t>(ret.GetI())); break;
      case 'C': result->SetI(static_cast<uint16_t>(ret.GetI())); break;
      case 'S': result->SetI(static_cast<int16_t>(ret.GetI())); break;
      case 'I': result->SetI(ret.GetI()); break;
      case 'F': result->SetF(ret.GetF()); break;
      case 'J': result->SetJ(ret.GetJ()); break;
      case 'D': result->SetD(ret.GetD()); break;
      default: break;
    }
  }
  env->SetLocalSegmentState(env->GetLocalRefCookie());
  env->SetLocalRefCookie(saved_cookie);
}

typedef bool (*ArtWasmIntrinsic)(Thread* self, ShadowFrame* sf, JValue* result);
ArtWasmIntrinsic ArtWasmFindIntrinsic(ArtMethod* method);  // intrinsics_wasm10.cc

// A native method called by the interpreter (DoCallCommon): |sf| is its
// frame, built by DoCallCommon with the arguments from vreg 0, not pushed
// yet. On a device the interpreter calls the method's compiled JNI stub
// through ArtInterpreterToCompiledCodeBridge and ArtMethod::Invoke; here the
// native runs in this frame directly (an intrinsic, or the JNI call), which
// is what EnterInterpreterFromInvoke would do after copying the arguments to
// a second frame.
void ArtWasmCallNative(Thread* self, ShadowFrame* sf, JValue* result) {
  ArtMethod* method = sf->GetMethod();
  const bool is_static = method->IsStatic();
  if (is_static) {
    // As ArtInterpreterToCompiledCodeBridge: initialize the class first.
    ObjPtr<mirror::Class> declaring_class = method->GetDeclaringClass();
    if (UNLIKELY(!declaring_class->IsInitialized())) {
      self->PushShadowFrame(sf);
      StackHandleScope<1> hs(self);
      Handle<mirror::Class> h_class(hs.NewHandle(declaring_class));
      const bool ok = Runtime::Current()->GetClassLinker()->EnsureInitialized(self, h_class, true,
                                                                               true);
      self->PopShadowFrame();
      if (UNLIKELY(!ok)) {
        DCHECK(self->IsExceptionPending());
        return;
      }
    }
  }
  self->PushShadowFrame(sf);
  ArtWasmIntrinsic intrinsic = ArtWasmFindIntrinsic(method);
  if (intrinsic == nullptr || !intrinsic(self, sf, result)) {
    mirror::Object* receiver = is_static ? nullptr : sf->GetVRegReference(0);
    ArtWasmInterpreterJni(self, method, receiver, sf->GetVRegArgs(is_static ? 0 : 1), result);
  }
  self->PopShadowFrame();
}

// |args| are as ArtMethod::Invoke gets them: the receiver (the proxy
// object), then the arguments.
void ArtWasmInvokeProxy(Thread* self, ArtMethod* proxy_method, uint32_t* args, JValue* result,
                        const char* shorty) {
  ScopedObjectAccessUnchecked soa(self);
  JNIEnvExt* env = self->GetJniEnv();
  ScopedLocalFrame frame(env);  // pops the references made below
  mirror::Object* receiver = reinterpret_cast<StackReference<mirror::Object>*>(&args[0])->AsMirrorPtr();
  jobject rcvr_jobj = soa.AddLocalReference<jobject>(receiver);
  std::vector<jvalue> jargs;
  size_t a = 1;
  for (const char* s = shorty + 1; *s != '\0'; ++s) {
    jvalue v;
    v.j = 0;
    switch (*s) {
      case 'L':
        v.l = soa.AddLocalReference<jobject>(
            reinterpret_cast<StackReference<mirror::Object>*>(&args[a++])->AsMirrorPtr());
        break;
      case 'J':
      case 'D':
        v.j = static_cast<int64_t>(args[a]) | (static_cast<int64_t>(args[a + 1]) << 32);
        a += 2;
        break;
      case 'F':
        memcpy(&v.f, &args[a++], sizeof(float));
        break;
      default:
        v.i = static_cast<int32_t>(args[a++]);
        break;
    }
    jargs.push_back(v);
  }
  ArtMethod* interface_method = proxy_method->GetInterfaceMethodIfProxy(kRuntimePointerSize);
  jobject interface_method_jobj = soa.AddLocalReference<jobject>(
      mirror::Method::CreateFromArtMethod<kRuntimePointerSize, false>(soa.Self(),
                                                                       interface_method));
  JValue r = InvokeProxyInvocationHandler(soa, shorty, rcvr_jobj, interface_method_jobj, jargs);
  if (result != nullptr) {
    *result = r;
  }
}

}  // namespace interpreter
}  // namespace art
