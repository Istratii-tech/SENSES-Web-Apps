// JNI for arm64-v8a guests (topic "ndk-arm64"): the counterpart of ndk_jni.cc
// with 64-bit tables. A guest JNIEnv is { functions (8 bytes), host JNIEnv* };
// the table holds AArch64 host stubs, one per JNINativeInterface slot (the
// host's slot index: offset / 4). References and IDs are host pointers,
// zero-extended for the guest; jvalue is 8 bytes on both sides. What differs:
// JNINativeMethod (24 bytes), JavaVMAttachArgs, the JNIEnv**/JavaVM**
// outputs, and the arguments of Call*Method[V] (AAPCS64 registers or the
// AArch64 va_list), read by the method's shorty.

#include <jni.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <mutex>
#include <vector>

#include "ndk_bridge.h"
#include "ndk_jni.h"

extern "C" int __android_log_print(int prio, const char* tag, const char* fmt, ...);

namespace ndk {

namespace {

struct GuestEnv64 {
  uint64_t functions;
  JNIEnv* host;
  uint32_t pad;
};
struct GuestVm64 {
  uint64_t functions;
  JavaVM* host;
  uint32_t pad;
};

const int kEnvSlots = sizeof(JNINativeInterface) / sizeof(void*);
const int kVmSlots = sizeof(JNIInvokeInterface) / sizeof(void*);
uint64_t g_env_table[kEnvSlots];
uint64_t g_vm_table[kVmSlots];
GuestVm64 g_vm;
std::mutex g_env_lock;
std::map<JNIEnv*, GuestEnv64*>* g_envs;

JNIEnv* HostEnv(uint64_t guest_env) { return Ptr<GuestEnv64>(GuestAddr64(guest_env))->host; }
JavaVM* HostVm(uint64_t guest_vm) { return Ptr<GuestVm64>(GuestAddr64(guest_vm))->host; }

void Set(uint64_t* table, size_t host_offset, Ndk64HostFn fn, void* user, const char* name) {
  table[host_offset / sizeof(void*)] = MakeStub64(fn, user, name);
}

template <typename T>
struct JniTraits64;
template <typename R, typename... A>
struct JniTraits64<R (*)(JNIEnv*, A...)> {
  template <R (*JNINativeInterface::*M)(JNIEnv*, A...)>
  static void Call(NdkCpu64State* s, void*) {
    JNIEnv* env = HostEnv(s->x[0]);
    RegArgs64 a(s, 1);
    std::tuple<A...> args{GetArg64<A>(a)...};
    auto fn = env->functions->*M;
    if constexpr (std::is_void<R>::value) {
      std::apply([&](A... x) { fn(env, x...); }, args);
    } else {
      R r = std::apply([&](A... x) { return fn(env, x...); }, args);
      SetResult64(s, r);
    }
  }
};

void GetJavaVM(NdkCpu64State* s, void*) {
  JNIEnv* env = HostEnv(s->x[0]);
  JavaVM* vm = nullptr;
  jint r = env->GetJavaVM(&vm);
  if (r == JNI_OK) Store64(GuestAddr64(s->x[1]), Addr(GuestVmFor64(vm)));
  SetResult64(s, r);
}

// RegisterNatives: JNINativeMethod is { name, signature, fnPtr } of 8-byte
// pointers. The function pointers are guest addresses, which ART stores as
// any native's and calls back through arc_ndk_art_call_native.
void RegisterNatives(NdkCpu64State* s, void*) {
  JNIEnv* env = HostEnv(s->x[0]);
  jclass cls = Ptr<_jclass>(GuestAddr64(s->x[1]));
  uint32_t methods = GuestAddr64(s->x[2]);
  jint n = static_cast<jint>(s->x[3]);
  std::vector<JNINativeMethod> host(n > 0 ? n : 0);
  for (jint i = 0; i < n; i++) {
    host[i].name = Ptr<const char>(GuestAddr64(Load64(methods + 24 * i)));
    host[i].signature = Ptr<const char>(GuestAddr64(Load64(methods + 24 * i + 8)));
    host[i].fnPtr = Ptr<void>(GuestAddr64(Load64(methods + 24 * i + 16)));
  }
  if (TraceLevel() >= 1) {
    for (jint i = 0; i < n; i++) {
      uint32_t off = 0;
      const char* sym = Symbolize64 ? Symbolize64(Addr(host[i].fnPtr), &off) : nullptr;
      __android_log_print(4, "ndk", "RegisterNatives %s%s -> %s+0x%x", host[i].name, host[i].signature,
                          sym ? sym : "?", off);
    }
  }
  SetResult64(s, env->RegisterNatives(cls, host.data(), n));
}

void TraceFindClass(NdkCpu64State* s, void*) {
  JNIEnv* env = HostEnv(s->x[0]);
  const char* name = Ptr<const char>(GuestAddr64(s->x[1]));
  jclass r = env->FindClass(name);
  __android_log_print(4, "ndk", "JNI FindClass %s -> %p (arm64)", name, r);
  SetResult64(s, r);
}
void TraceGetMethodID(NdkCpu64State* s, void* is_static) {
  JNIEnv* env = HostEnv(s->x[0]);
  jclass cls = Ptr<_jclass>(GuestAddr64(s->x[1]));
  const char* name = Ptr<const char>(GuestAddr64(s->x[2]));
  const char* sig = Ptr<const char>(GuestAddr64(s->x[3]));
  jmethodID r = is_static ? env->GetStaticMethodID(cls, name, sig) : env->GetMethodID(cls, name, sig);
  __android_log_print(4, "ndk", "JNI Get%sMethodID %s%s -> %p (arm64)", is_static ? "Static" : "", name, sig, r);
  SetResult64(s, r);
}

// --- Call<type>Method[V], NewObject[V] -----------------------------------------------------------
enum Kind { kVirtual, kNonvirtual, kStatic, kNewObject };
struct Variadic {
  Kind kind;
  char ret;
  size_t a_offset;
  const char* name;
};

template <typename R>
R InvokeA(JNIEnv* env, const Variadic& v, jobject obj, jclass cls, jmethodID mid, const jvalue* args) {
  void* fn = *reinterpret_cast<void* const*>(reinterpret_cast<const char*>(env->functions) + v.a_offset);
  switch (v.kind) {
    case kVirtual:
      return reinterpret_cast<R (*)(JNIEnv*, jobject, jmethodID, const jvalue*)>(fn)(env, obj, mid, args);
    case kNonvirtual:
      return reinterpret_cast<R (*)(JNIEnv*, jobject, jclass, jmethodID, const jvalue*)>(fn)(env, obj, cls, mid, args);
    default:
      return reinterpret_cast<R (*)(JNIEnv*, jclass, jmethodID, const jvalue*)>(fn)(env, cls, mid, args);
  }
}

// The method's arguments (default promotions: float as double) into |out|.
void ReadArgs(jmethodID mid, ArgSource& a, jvalue* out, int max) {
  const char* shorty = ndk_method_shorty(mid);
  int n = 0;
  for (const char* p = shorty + 1; *p && n < max; p++, n++) {
    jvalue& v = out[n];
    v.j = 0;
    switch (*p) {
      case 'Z': v.z = (jboolean)a.U32(); break;
      case 'B': v.b = (jbyte)a.U32(); break;
      case 'C': v.c = (jchar)a.U32(); break;
      case 'S': v.s = (jshort)a.U32(); break;
      case 'I': v.i = (jint)a.U32(); break;
      case 'J': v.j = (jlong)a.U64(); break;
      case 'F': v.f = (jfloat)a.F64(); break;
      case 'D': v.d = a.F64(); break;
      default: v.l = Ptr<_jobject>(a.U32()); break;   // L, [
    }
  }
}

void CallVariadic(NdkCpu64State* s, const Variadic& v, bool va_list) {
  JNIEnv* env = HostEnv(s->x[0]);
  RegArgs64 regs(s, 1);
  jobject obj = nullptr;
  jclass cls = nullptr;
  if (v.kind == kVirtual || v.kind == kNonvirtual) obj = Ptr<_jobject>(regs.U32());
  if (v.kind != kVirtual) cls = Ptr<_jclass>(regs.U32());
  jmethodID mid = Ptr<_jmethodID>(regs.U32());
  jvalue args[256];
  if (va_list) {
    VaArgs64 va(regs.U32());
    ReadArgs(mid, va, args, 256);
  } else {
    ReadArgs(mid, regs, args, 256);
  }
  switch (v.ret) {
    case 'V': InvokeA<void>(env, v, obj, cls, mid, args); break;
    case 'Z': SetResult64(s, InvokeA<jboolean>(env, v, obj, cls, mid, args)); break;
    case 'B': SetResult64(s, InvokeA<jbyte>(env, v, obj, cls, mid, args)); break;
    case 'C': SetResult64(s, InvokeA<jchar>(env, v, obj, cls, mid, args)); break;
    case 'S': SetResult64(s, InvokeA<jshort>(env, v, obj, cls, mid, args)); break;
    case 'I': SetResult64(s, InvokeA<jint>(env, v, obj, cls, mid, args)); break;
    case 'J': SetResult64(s, InvokeA<jlong>(env, v, obj, cls, mid, args)); break;
    case 'F': SetResult64(s, InvokeA<jfloat>(env, v, obj, cls, mid, args)); break;
    case 'D': SetResult64(s, InvokeA<jdouble>(env, v, obj, cls, mid, args)); break;
    default: SetResult64(s, InvokeA<jobject>(env, v, obj, cls, mid, args)); break;
  }
}
void CallVariadicStub(NdkCpu64State* s, void* user) { CallVariadic(s, *static_cast<const Variadic*>(user), false); }
void CallVaListStub(NdkCpu64State* s, void* user) { CallVariadic(s, *static_cast<const Variadic*>(user), true); }

// --- JavaVM -----------------------------------------------------------------------------------------
void VmDestroy(NdkCpu64State* s, void*) { SetResult64(s, (jint)JNI_ERR); }
void VmAttach(NdkCpu64State* s, void* daemon) {
  JavaVM* vm = HostVm(s->x[0]);
  JNIEnv* env = nullptr;
  // JavaVMAttachArgs: { jint version; char* name; jobject group } (LP64: 24 bytes).
  JavaVMAttachArgs host_args, *ap = nullptr;
  if (uint32_t g = GuestAddr64(s->x[2])) {
    host_args.version = static_cast<jint>(Load32(g));
    host_args.name = Ptr<char>(GuestAddr64(Load64(g + 8)));
    host_args.group = Ptr<_jobject>(GuestAddr64(Load64(g + 16)));
    ap = &host_args;
  }
  jint r = daemon ? vm->AttachCurrentThreadAsDaemon(&env, ap) : vm->AttachCurrentThread(&env, ap);
  if (r == JNI_OK && s->x[1]) Store64(GuestAddr64(s->x[1]), Addr(GuestEnvFor64(env)));
  SetResult64(s, r);
}
void VmDetach(NdkCpu64State* s, void*) { SetResult64(s, HostVm(s->x[0])->DetachCurrentThread()); }
void VmGetEnv(NdkCpu64State* s, void*) {
  void* env = nullptr;
  jint r = HostVm(s->x[0])->GetEnv(&env, static_cast<jint>(s->x[2]));
  Store64(GuestAddr64(s->x[1]), r == JNI_OK ? Addr(GuestEnvFor64(static_cast<JNIEnv*>(env))) : 0);
  SetResult64(s, r);
}

#define JNI_PLAIN(n)                                                                                \
  Set(g_env_table, offsetof(JNINativeInterface, n),                                                 \
      &JniTraits64<decltype(JNINativeInterface::n)>::template Call<&JNINativeInterface::n>, nullptr, \
      "JNIEnv::" #n)
#define JNI_VARIADIC(n, kind, ret)                                                                     \
  do {                                                                                                 \
    static const Variadic v = {kind, ret, offsetof(JNINativeInterface, n##A), "JNIEnv::" #n};          \
    Set(g_env_table, offsetof(JNINativeInterface, n), CallVariadicStub, const_cast<Variadic*>(&v),     \
        "JNIEnv::" #n);                                                                                \
    Set(g_env_table, offsetof(JNINativeInterface, n##V), CallVaListStub, const_cast<Variadic*>(&v),    \
        "JNIEnv::" #n "V");                                                                            \
  } while (0)

void BuildTables() {
  JNI_PLAIN(GetVersion); JNI_PLAIN(DefineClass); JNI_PLAIN(FindClass); JNI_PLAIN(FromReflectedMethod);
  JNI_PLAIN(FromReflectedField); JNI_PLAIN(ToReflectedMethod); JNI_PLAIN(GetSuperclass);
  JNI_PLAIN(IsAssignableFrom); JNI_PLAIN(ToReflectedField); JNI_PLAIN(Throw); JNI_PLAIN(ThrowNew);
  JNI_PLAIN(ExceptionOccurred); JNI_PLAIN(ExceptionDescribe); JNI_PLAIN(ExceptionClear); JNI_PLAIN(FatalError);
  JNI_PLAIN(PushLocalFrame); JNI_PLAIN(PopLocalFrame); JNI_PLAIN(NewGlobalRef); JNI_PLAIN(DeleteGlobalRef);
  JNI_PLAIN(DeleteLocalRef); JNI_PLAIN(IsSameObject); JNI_PLAIN(NewLocalRef); JNI_PLAIN(EnsureLocalCapacity);
  JNI_PLAIN(AllocObject); JNI_PLAIN(NewObjectA); JNI_PLAIN(GetObjectClass); JNI_PLAIN(IsInstanceOf);
  JNI_PLAIN(GetMethodID);
  JNI_PLAIN(CallObjectMethodA); JNI_PLAIN(CallBooleanMethodA); JNI_PLAIN(CallByteMethodA);
  JNI_PLAIN(CallCharMethodA); JNI_PLAIN(CallShortMethodA); JNI_PLAIN(CallIntMethodA); JNI_PLAIN(CallLongMethodA);
  JNI_PLAIN(CallFloatMethodA); JNI_PLAIN(CallDoubleMethodA); JNI_PLAIN(CallVoidMethodA);
  JNI_PLAIN(CallNonvirtualObjectMethodA); JNI_PLAIN(CallNonvirtualBooleanMethodA);
  JNI_PLAIN(CallNonvirtualByteMethodA); JNI_PLAIN(CallNonvirtualCharMethodA);
  JNI_PLAIN(CallNonvirtualShortMethodA); JNI_PLAIN(CallNonvirtualIntMethodA);
  JNI_PLAIN(CallNonvirtualLongMethodA); JNI_PLAIN(CallNonvirtualFloatMethodA);
  JNI_PLAIN(CallNonvirtualDoubleMethodA); JNI_PLAIN(CallNonvirtualVoidMethodA);
  JNI_PLAIN(GetFieldID); JNI_PLAIN(GetObjectField); JNI_PLAIN(GetBooleanField); JNI_PLAIN(GetByteField);
  JNI_PLAIN(GetCharField); JNI_PLAIN(GetShortField); JNI_PLAIN(GetIntField); JNI_PLAIN(GetLongField);
  JNI_PLAIN(GetFloatField); JNI_PLAIN(GetDoubleField); JNI_PLAIN(SetObjectField); JNI_PLAIN(SetBooleanField);
  JNI_PLAIN(SetByteField); JNI_PLAIN(SetCharField); JNI_PLAIN(SetShortField); JNI_PLAIN(SetIntField);
  JNI_PLAIN(SetLongField); JNI_PLAIN(SetFloatField); JNI_PLAIN(SetDoubleField); JNI_PLAIN(GetStaticMethodID);
  JNI_PLAIN(CallStaticObjectMethodA); JNI_PLAIN(CallStaticBooleanMethodA); JNI_PLAIN(CallStaticByteMethodA);
  JNI_PLAIN(CallStaticCharMethodA); JNI_PLAIN(CallStaticShortMethodA); JNI_PLAIN(CallStaticIntMethodA);
  JNI_PLAIN(CallStaticLongMethodA); JNI_PLAIN(CallStaticFloatMethodA); JNI_PLAIN(CallStaticDoubleMethodA);
  JNI_PLAIN(CallStaticVoidMethodA);
  JNI_PLAIN(GetStaticFieldID); JNI_PLAIN(GetStaticObjectField); JNI_PLAIN(GetStaticBooleanField);
  JNI_PLAIN(GetStaticByteField); JNI_PLAIN(GetStaticCharField); JNI_PLAIN(GetStaticShortField);
  JNI_PLAIN(GetStaticIntField); JNI_PLAIN(GetStaticLongField); JNI_PLAIN(GetStaticFloatField);
  JNI_PLAIN(GetStaticDoubleField); JNI_PLAIN(SetStaticObjectField); JNI_PLAIN(SetStaticBooleanField);
  JNI_PLAIN(SetStaticByteField); JNI_PLAIN(SetStaticCharField); JNI_PLAIN(SetStaticShortField);
  JNI_PLAIN(SetStaticIntField); JNI_PLAIN(SetStaticLongField); JNI_PLAIN(SetStaticFloatField);
  JNI_PLAIN(SetStaticDoubleField);
  JNI_PLAIN(NewString); JNI_PLAIN(GetStringLength); JNI_PLAIN(GetStringChars); JNI_PLAIN(ReleaseStringChars);
  JNI_PLAIN(NewStringUTF); JNI_PLAIN(GetStringUTFLength); JNI_PLAIN(GetStringUTFChars);
  JNI_PLAIN(ReleaseStringUTFChars); JNI_PLAIN(GetArrayLength); JNI_PLAIN(NewObjectArray);
  JNI_PLAIN(GetObjectArrayElement); JNI_PLAIN(SetObjectArrayElement);
  JNI_PLAIN(NewBooleanArray); JNI_PLAIN(NewByteArray); JNI_PLAIN(NewCharArray); JNI_PLAIN(NewShortArray);
  JNI_PLAIN(NewIntArray); JNI_PLAIN(NewLongArray); JNI_PLAIN(NewFloatArray); JNI_PLAIN(NewDoubleArray);
  JNI_PLAIN(GetBooleanArrayElements); JNI_PLAIN(GetByteArrayElements); JNI_PLAIN(GetCharArrayElements);
  JNI_PLAIN(GetShortArrayElements); JNI_PLAIN(GetIntArrayElements); JNI_PLAIN(GetLongArrayElements);
  JNI_PLAIN(GetFloatArrayElements); JNI_PLAIN(GetDoubleArrayElements);
  JNI_PLAIN(ReleaseBooleanArrayElements); JNI_PLAIN(ReleaseByteArrayElements);
  JNI_PLAIN(ReleaseCharArrayElements); JNI_PLAIN(ReleaseShortArrayElements); JNI_PLAIN(ReleaseIntArrayElements);
  JNI_PLAIN(ReleaseLongArrayElements); JNI_PLAIN(ReleaseFloatArrayElements); JNI_PLAIN(ReleaseDoubleArrayElements);
  JNI_PLAIN(GetBooleanArrayRegion); JNI_PLAIN(GetByteArrayRegion); JNI_PLAIN(GetCharArrayRegion);
  JNI_PLAIN(GetShortArrayRegion); JNI_PLAIN(GetIntArrayRegion); JNI_PLAIN(GetLongArrayRegion);
  JNI_PLAIN(GetFloatArrayRegion); JNI_PLAIN(GetDoubleArrayRegion);
  JNI_PLAIN(SetBooleanArrayRegion); JNI_PLAIN(SetByteArrayRegion); JNI_PLAIN(SetCharArrayRegion);
  JNI_PLAIN(SetShortArrayRegion); JNI_PLAIN(SetIntArrayRegion); JNI_PLAIN(SetLongArrayRegion);
  JNI_PLAIN(SetFloatArrayRegion); JNI_PLAIN(SetDoubleArrayRegion);
  JNI_PLAIN(UnregisterNatives); JNI_PLAIN(MonitorEnter); JNI_PLAIN(MonitorExit);
  JNI_PLAIN(GetStringRegion); JNI_PLAIN(GetStringUTFRegion); JNI_PLAIN(GetPrimitiveArrayCritical);
  JNI_PLAIN(ReleasePrimitiveArrayCritical); JNI_PLAIN(GetStringCritical); JNI_PLAIN(ReleaseStringCritical);
  JNI_PLAIN(NewWeakGlobalRef); JNI_PLAIN(DeleteWeakGlobalRef); JNI_PLAIN(ExceptionCheck);
  JNI_PLAIN(NewDirectByteBuffer); JNI_PLAIN(GetDirectBufferAddress); JNI_PLAIN(GetDirectBufferCapacity);
  JNI_PLAIN(GetObjectRefType);
  JNI_VARIADIC(CallObjectMethod, kVirtual, 'L'); JNI_VARIADIC(CallBooleanMethod, kVirtual, 'Z');
  JNI_VARIADIC(CallByteMethod, kVirtual, 'B'); JNI_VARIADIC(CallCharMethod, kVirtual, 'C');
  JNI_VARIADIC(CallShortMethod, kVirtual, 'S'); JNI_VARIADIC(CallIntMethod, kVirtual, 'I');
  JNI_VARIADIC(CallLongMethod, kVirtual, 'J'); JNI_VARIADIC(CallFloatMethod, kVirtual, 'F');
  JNI_VARIADIC(CallDoubleMethod, kVirtual, 'D'); JNI_VARIADIC(CallVoidMethod, kVirtual, 'V');
  JNI_VARIADIC(CallNonvirtualObjectMethod, kNonvirtual, 'L');
  JNI_VARIADIC(CallNonvirtualBooleanMethod, kNonvirtual, 'Z');
  JNI_VARIADIC(CallNonvirtualByteMethod, kNonvirtual, 'B');
  JNI_VARIADIC(CallNonvirtualCharMethod, kNonvirtual, 'C');
  JNI_VARIADIC(CallNonvirtualShortMethod, kNonvirtual, 'S');
  JNI_VARIADIC(CallNonvirtualIntMethod, kNonvirtual, 'I');
  JNI_VARIADIC(CallNonvirtualLongMethod, kNonvirtual, 'J');
  JNI_VARIADIC(CallNonvirtualFloatMethod, kNonvirtual, 'F');
  JNI_VARIADIC(CallNonvirtualDoubleMethod, kNonvirtual, 'D');
  JNI_VARIADIC(CallNonvirtualVoidMethod, kNonvirtual, 'V');
  JNI_VARIADIC(CallStaticObjectMethod, kStatic, 'L'); JNI_VARIADIC(CallStaticBooleanMethod, kStatic, 'Z');
  JNI_VARIADIC(CallStaticByteMethod, kStatic, 'B'); JNI_VARIADIC(CallStaticCharMethod, kStatic, 'C');
  JNI_VARIADIC(CallStaticShortMethod, kStatic, 'S'); JNI_VARIADIC(CallStaticIntMethod, kStatic, 'I');
  JNI_VARIADIC(CallStaticLongMethod, kStatic, 'J'); JNI_VARIADIC(CallStaticFloatMethod, kStatic, 'F');
  JNI_VARIADIC(CallStaticDoubleMethod, kStatic, 'D'); JNI_VARIADIC(CallStaticVoidMethod, kStatic, 'V');
  JNI_VARIADIC(NewObject, kNewObject, 'L');
  Set(g_env_table, offsetof(JNINativeInterface, GetJavaVM), GetJavaVM, nullptr, "JNIEnv::GetJavaVM");
  Set(g_env_table, offsetof(JNINativeInterface, RegisterNatives), RegisterNatives, nullptr,
      "JNIEnv::RegisterNatives");
  if (TraceLevel() >= 2) {
    Set(g_env_table, offsetof(JNINativeInterface, FindClass), TraceFindClass, nullptr, "JNIEnv::FindClass");
    Set(g_env_table, offsetof(JNINativeInterface, GetMethodID), TraceGetMethodID, nullptr, "JNIEnv::GetMethodID");
    Set(g_env_table, offsetof(JNINativeInterface, GetStaticMethodID), TraceGetMethodID, reinterpret_cast<void*>(1),
        "JNIEnv::GetStaticMethodID");
  }
  Set(g_vm_table, offsetof(JNIInvokeInterface, DestroyJavaVM), VmDestroy, nullptr, "JavaVM::DestroyJavaVM");
  Set(g_vm_table, offsetof(JNIInvokeInterface, AttachCurrentThread), VmAttach, nullptr,
      "JavaVM::AttachCurrentThread");
  Set(g_vm_table, offsetof(JNIInvokeInterface, DetachCurrentThread), VmDetach, nullptr,
      "JavaVM::DetachCurrentThread");
  Set(g_vm_table, offsetof(JNIInvokeInterface, GetEnv), VmGetEnv, nullptr, "JavaVM::GetEnv");
  Set(g_vm_table, offsetof(JNIInvokeInterface, AttachCurrentThreadAsDaemon), VmAttach, reinterpret_cast<void*>(1),
      "JavaVM::AttachCurrentThreadAsDaemon");
}

}  // namespace

JNIEnv* HostEnvOf64(uint32_t guest_env) {
  if (!guest_env) return nullptr;
  GuestEnv64* g = Ptr<GuestEnv64>(guest_env);
  return g->functions == Addr(g_env_table) ? g->host : nullptr;
}

void* GuestEnvFor64(JNIEnv* env) {
  if (!env) return nullptr;
  std::lock_guard<std::mutex> l(g_env_lock);
  if (!g_envs) g_envs = new std::map<JNIEnv*, GuestEnv64*>;
  GuestEnv64*& g = (*g_envs)[env];
  if (!g) g = new GuestEnv64{Addr(g_env_table), env, 0};
  return g;
}

void* GuestVmFor64(JavaVM* vm) {
  g_vm.functions = Addr(g_vm_table);
  g_vm.host = vm;
  return &g_vm;
}

void RegisterJni64() { BuildTables(); }

}  // namespace ndk

// Keeps this file in the link (-Wl,-u,ndk64_jni_linked, mk/topic-ndk-arm64.mk): the
// shared bridge files refer to it only weakly.
extern "C" int ndk64_jni_linked = 1;
