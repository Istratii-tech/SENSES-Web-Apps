// JNI between Dalvik and guest code.
//
// Guest code gets a guest-visible JNIEnv and JavaVM: a pointer to a struct
// whose first word is a function table of host stubs (ndk_host_stub), so
// "(*env)->FindClass(env, ...)" in ARM code branches to a stub, which finds
// the host JNIEnv behind the guest one and calls Dalvik's function. Fixed
// signatures are marshalled by the generic argument reader; Call*Method and
// NewObject (variadic, or with a guest va_list) are rebuilt as the "A"
// variants from the method's shorty (default promotions: float arguments
// arrive as doubles). RegisterNatives passes the guest function pointers
// on unchanged: Dalvik stores them like any native's, and calls them back
// through dvmPlatformInvoke, which hands guest addresses to
// ndk_dalvik_invoke (ndk_dalvik.cc). Part of the arc-wasm shim layer.

#include <jni.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <map>
#include <mutex>

#include "ndk_bridge.h"
#include "ndk_jni.h"

extern "C" int __android_log_print(int prio, const char* tag, const char* fmt, ...);
extern "C" int property_get(const char* key, char* value, const char* default_value);

namespace ndk {
namespace {

struct GuestEnv {
  uint32_t functions;  // what (*env) reads
  JNIEnv* host;
};
struct GuestVm {
  uint32_t functions;
  JavaVM* host;
};

const int kEnvSlots = sizeof(JNINativeInterface) / 4;
const int kVmSlots = sizeof(JNIInvokeInterface) / 4;
uint32_t g_env_table[kEnvSlots];
uint32_t g_vm_table[kVmSlots];
GuestVm g_vm;

std::mutex g_env_lock;
std::map<JNIEnv*, GuestEnv*>* g_envs;

JNIEnv* HostEnv(uint32_t guest_env) { return Ptr<GuestEnv>(guest_env)->host; }

void Set(uint32_t* table, size_t offset, NdkHostFn fn, void* user, const char* name) {
  table[offset / 4] = MakeStub(fn, user, name);
}

// --- functions with fixed signatures -----------------------------------------------

template <typename T>
struct JniTraits;
template <typename R, typename... A>
struct JniTraits<R (*)(JNIEnv*, A...)> {
  template <R (*JNINativeInterface::*M)(JNIEnv*, A...)>
  static void Call(NdkCpuState* s, void*) {
    JNIEnv* env = HostEnv(s->r[0]);
    RegArgs a(s, 1);
    std::tuple<A...> args{GetArg<A>(a)...};
    auto fn = env->functions->*M;
    if constexpr (std::is_void<R>::value) {
      std::apply([&](A... x) { fn(env, x...); }, args);
    } else {
      R r = std::apply([&](A... x) { return fn(env, x...); }, args);
      SetResult(s, r);
    }
  }
};

void GetJavaVM(NdkCpuState* s, void*) {
  JNIEnv* env = HostEnv(s->r[0]);
  JavaVM* vm = nullptr;
  jint r = env->GetJavaVM(&vm);
  if (r == JNI_OK)
    Store32(s->r[1], Addr(GuestVmFor(vm)));
  SetResult(s, r);
}

// Google Play services. There are none here, and the Firebase C++ SDK
// (Unity games) gates its modules on
// GoogleApiAvailability.isGooglePlayServicesAvailable(Context), which it
// calls from native code: with SERVICE_INVALID (9, "requires the Google Play
// Store, but it is missing") CheckAndFixDependenciesAsync reports
// UnavailableInvalid and RemoteConfig/Messaging GetInstance return null, so
// a game that uses RemoteConfig anyway stops (Subway Surfers' bootstrap,
// docs/topics/install8.md). Guest code asking gets SUCCESS (0); Java code
// asking still gets the real answer, and the Java libraries fail their own
// Play services calls as on a phone without them. debug.ndk.gms=0: off.
std::atomic<jmethodID> g_gms_check[2];

bool GmsAnswerOn() {
  static int on = -1;
  if (on < 0) {
    char v[92];
    property_get("debug.ndk.gms", v, "1");
    on = v[0] != '0';
  }
  return on;
}

void NoteMethodID(jmethodID mid, const char* name, const char* sig) {
  if (!mid || strcmp(name, "isGooglePlayServicesAvailable") != 0 ||
      strncmp(sig, "(Landroid/content/Context;", 26) != 0 || !GmsAnswerOn())
    return;
  for (std::atomic<jmethodID>& m : g_gms_check) {
    jmethodID expected = nullptr;
    if (m.load() == mid || m.compare_exchange_strong(expected, mid))
      return;
  }
}

bool IsGmsCheck(jmethodID mid) {
  for (std::atomic<jmethodID>& m : g_gms_check)
    if (mid && m.load(std::memory_order_relaxed) == mid)
      return true;
  return false;
}

void GetMethodIDStub(NdkCpuState* s, void*) {
  JNIEnv* env = HostEnv(s->r[0]);
  const char* name = Ptr<const char>(s->r[2]);
  const char* sig = Ptr<const char>(s->r[3]);
  jmethodID r = env->GetMethodID(Ptr<_jclass>(s->r[1]), name, sig);
  NoteMethodID(r, name, sig);
  SetResult(s, r);
}

// Lookups guest code makes, for debug.ndk.trace=2.
void TraceFindClass(NdkCpuState* s, void*) {
  JNIEnv* env = HostEnv(s->r[0]);
  const char* name = Ptr<const char>(s->r[1]);
  jclass r = env->FindClass(name);
  __android_log_print(4, "ndk", "JNI FindClass %s -> %p", name, r);
  SetResult(s, r);
}
void TraceGetMethodID(NdkCpuState* s, void* is_static) {
  JNIEnv* env = HostEnv(s->r[0]);
  jclass cls = Ptr<_jclass>(s->r[1]);
  const char* name = Ptr<const char>(s->r[2]);
  const char* sig = Ptr<const char>(s->r[3]);
  jmethodID r = is_static ? env->GetStaticMethodID(cls, name, sig) : env->GetMethodID(cls, name, sig);
  __android_log_print(4, "ndk", "JNI Get%sMethodID %s%s -> %p", is_static ? "Static" : "", name,
                      sig, r);
  if (!is_static)
    NoteMethodID(r, name, sig);
  SetResult(s, r);
}

// --- Call<type>Method[V] and NewObject[V] -------------------------------------------

enum Kind { kVirtual, kNonvirtual, kStatic, kNewObject };
struct Variadic {
  Kind kind;
  char ret;           // shorty of the result
  size_t a_offset;    // the "A" variant in JNINativeInterface
  const char* name;
};

template <typename R>
R InvokeA(JNIEnv* env, const Variadic& v, jobject obj, jclass cls, jmethodID mid,
          const jvalue* args) {
  void* fn = *reinterpret_cast<void* const*>(
      reinterpret_cast<const char*>(env->functions) + v.a_offset);
  switch (v.kind) {
    case kVirtual:
      return reinterpret_cast<R (*)(JNIEnv*, jobject, jmethodID, const jvalue*)>(fn)(
          env, obj, mid, args);
    case kNonvirtual:
      return reinterpret_cast<R (*)(JNIEnv*, jobject, jclass, jmethodID, const jvalue*)>(fn)(
          env, obj, cls, mid, args);
    default:
      return reinterpret_cast<R (*)(JNIEnv*, jclass, jmethodID, const jvalue*)>(fn)(
          env, cls, mid, args);
  }
}

// Reads the method's arguments (C default promotions) into |out|.
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
      case 'F': v.f = (jfloat)BitCast<double>(a.U64()); break;
      case 'D': v.d = BitCast<double>(a.U64()); break;
      default: v.l = Ptr<_jobject>(a.U32()); break;  // L, [
    }
  }
}

void CallVariadic(NdkCpuState* s, const Variadic& v, bool va_list) {
  JNIEnv* env = HostEnv(s->r[0]);
  RegArgs regs(s, 1);
  jobject obj = nullptr;
  jclass cls = nullptr;
  if (v.kind == kVirtual || v.kind == kNonvirtual)
    obj = GetArg<jobject>(regs);
  if (v.kind != kVirtual)
    cls = GetArg<jclass>(regs);
  jmethodID mid = GetArg<jmethodID>(regs);
  if (v.ret == 'I' && v.kind != kStatic && IsGmsCheck(mid)) {
    static std::atomic<bool> said{false};
    if (!said.exchange(true))
      __android_log_print(4, "ndk", "isGooglePlayServicesAvailable from native code: 0 (SUCCESS)");
    SetResult(s, (jint)0);
    return;
  }
  jvalue args[256];
  if (va_list) {
    VaArgs va(regs.U32());
    ReadArgs(mid, va, args, 256);
  } else {
    ReadArgs(mid, regs, args, 256);
  }
  switch (v.ret) {
    case 'V': InvokeA<void>(env, v, obj, cls, mid, args); break;
    case 'Z': SetResult(s, InvokeA<jboolean>(env, v, obj, cls, mid, args)); break;
    case 'B': SetResult(s, InvokeA<jbyte>(env, v, obj, cls, mid, args)); break;
    case 'C': SetResult(s, InvokeA<jchar>(env, v, obj, cls, mid, args)); break;
    case 'S': SetResult(s, InvokeA<jshort>(env, v, obj, cls, mid, args)); break;
    case 'I': SetResult(s, InvokeA<jint>(env, v, obj, cls, mid, args)); break;
    case 'J': SetResult(s, InvokeA<jlong>(env, v, obj, cls, mid, args)); break;
    case 'F': SetResult(s, InvokeA<jfloat>(env, v, obj, cls, mid, args)); break;
    case 'D': SetResult(s, InvokeA<jdouble>(env, v, obj, cls, mid, args)); break;
    default: SetResult(s, InvokeA<jobject>(env, v, obj, cls, mid, args)); break;
  }
}
void CallVariadicStub(NdkCpuState* s, void* user) {
  CallVariadic(s, *static_cast<const Variadic*>(user), false);
}
void CallVaListStub(NdkCpuState* s, void* user) {
  CallVariadic(s, *static_cast<const Variadic*>(user), true);
}

// --- JavaVM ------------------------------------------------------------------------

JavaVM* HostVm(uint32_t guest_vm) { return Ptr<GuestVm>(guest_vm)->host; }

void VmDestroy(NdkCpuState* s, void*) { SetResult(s, (jint)JNI_ERR); }
void VmAttach(NdkCpuState* s, void* daemon) {
  GreenPin(kPinJni, "JNI");   // ART's thread is its host thread's (ndk_green.cc)
  JavaVM* vm = HostVm(s->r[0]);
  JNIEnv* env = nullptr;
  void* args = Ptr<void>(s->r[2]);
  jint r = daemon ? vm->AttachCurrentThreadAsDaemon(&env, args)
                  : vm->AttachCurrentThread(&env, args);
  if (r == JNI_OK && s->r[1])
    Store32(s->r[1], Addr(GuestEnvFor(env)));
  SetResult(s, r);
}
void VmDetach(NdkCpuState* s, void*) {
  jint r = HostVm(s->r[0])->DetachCurrentThread();
  if (r == JNI_OK) GreenUnpin(kPinJni);   // a green thread may change carriers again
  SetResult(s, r);
}
void VmGetEnv(NdkCpuState* s, void*) {
  void* env = nullptr;
  jint r = HostVm(s->r[0])->GetEnv(&env, (jint)s->r[2]);
  Store32(s->r[1], r == JNI_OK ? Addr(GuestEnvFor(static_cast<JNIEnv*>(env))) : 0);
  SetResult(s, r);
}

#define JNI_PLAIN(n)                                                                    \
  Set(g_env_table, offsetof(JNINativeInterface, n),                                     \
      &JniTraits<decltype(JNINativeInterface::n)>::template Call<&JNINativeInterface::n>, \
      nullptr, "JNIEnv::" #n)
#define JNI_VARIADIC(n, kind, ret)                                                           \
  do {                                                                                       \
    static const Variadic v = {kind, ret, offsetof(JNINativeInterface, n##A), "JNIEnv::" #n}; \
    Set(g_env_table, offsetof(JNINativeInterface, n), CallVariadicStub,                      \
        const_cast<Variadic*>(&v), "JNIEnv::" #n);                                           \
    Set(g_env_table, offsetof(JNINativeInterface, n##V), CallVaListStub,                     \
        const_cast<Variadic*>(&v), "JNIEnv::" #n "V");                                       \
  } while (0)

void BuildTables() {
    JNI_PLAIN(GetVersion);
    JNI_PLAIN(DefineClass);
    JNI_PLAIN(FindClass);
    JNI_PLAIN(FromReflectedMethod);
    JNI_PLAIN(FromReflectedField);
    JNI_PLAIN(ToReflectedMethod);
    JNI_PLAIN(GetSuperclass);
    JNI_PLAIN(IsAssignableFrom);
    JNI_PLAIN(ToReflectedField);
    JNI_PLAIN(Throw);
    JNI_PLAIN(ThrowNew);
    JNI_PLAIN(ExceptionOccurred);
    JNI_PLAIN(ExceptionDescribe);
    JNI_PLAIN(ExceptionClear);
    JNI_PLAIN(FatalError);
    JNI_PLAIN(PushLocalFrame);
    JNI_PLAIN(PopLocalFrame);
    JNI_PLAIN(NewGlobalRef);
    JNI_PLAIN(DeleteGlobalRef);
    JNI_PLAIN(DeleteLocalRef);
    JNI_PLAIN(IsSameObject);
    JNI_PLAIN(NewLocalRef);
    JNI_PLAIN(EnsureLocalCapacity);
    JNI_PLAIN(AllocObject);
    JNI_PLAIN(NewObjectA);
    JNI_PLAIN(GetObjectClass);
    JNI_PLAIN(IsInstanceOf);
    JNI_PLAIN(GetMethodID);
    JNI_PLAIN(CallObjectMethodA);
    JNI_PLAIN(CallBooleanMethodA);
    JNI_PLAIN(CallByteMethodA);
    JNI_PLAIN(CallCharMethodA);
    JNI_PLAIN(CallShortMethodA);
    JNI_PLAIN(CallIntMethodA);
    JNI_PLAIN(CallLongMethodA);
    JNI_PLAIN(CallFloatMethodA);
    JNI_PLAIN(CallDoubleMethodA);
    JNI_PLAIN(CallVoidMethodA);
    JNI_PLAIN(CallNonvirtualObjectMethodA);
    JNI_PLAIN(CallNonvirtualBooleanMethodA);
    JNI_PLAIN(CallNonvirtualByteMethodA);
    JNI_PLAIN(CallNonvirtualCharMethodA);
    JNI_PLAIN(CallNonvirtualShortMethodA);
    JNI_PLAIN(CallNonvirtualIntMethodA);
    JNI_PLAIN(CallNonvirtualLongMethodA);
    JNI_PLAIN(CallNonvirtualFloatMethodA);
    JNI_PLAIN(CallNonvirtualDoubleMethodA);
    JNI_PLAIN(CallNonvirtualVoidMethodA);
    JNI_PLAIN(GetFieldID);
    JNI_PLAIN(GetObjectField);
    JNI_PLAIN(GetBooleanField);
    JNI_PLAIN(GetByteField);
    JNI_PLAIN(GetCharField);
    JNI_PLAIN(GetShortField);
    JNI_PLAIN(GetIntField);
    JNI_PLAIN(GetLongField);
    JNI_PLAIN(GetFloatField);
    JNI_PLAIN(GetDoubleField);
    JNI_PLAIN(SetObjectField);
    JNI_PLAIN(SetBooleanField);
    JNI_PLAIN(SetByteField);
    JNI_PLAIN(SetCharField);
    JNI_PLAIN(SetShortField);
    JNI_PLAIN(SetIntField);
    JNI_PLAIN(SetLongField);
    JNI_PLAIN(SetFloatField);
    JNI_PLAIN(SetDoubleField);
    JNI_PLAIN(GetStaticMethodID);
    JNI_PLAIN(CallStaticObjectMethodA);
    JNI_PLAIN(CallStaticBooleanMethodA);
    JNI_PLAIN(CallStaticByteMethodA);
    JNI_PLAIN(CallStaticCharMethodA);
    JNI_PLAIN(CallStaticShortMethodA);
    JNI_PLAIN(CallStaticIntMethodA);
    JNI_PLAIN(CallStaticLongMethodA);
    JNI_PLAIN(CallStaticFloatMethodA);
    JNI_PLAIN(CallStaticDoubleMethodA);
    JNI_PLAIN(CallStaticVoidMethodA);
    JNI_PLAIN(GetStaticFieldID);
    JNI_PLAIN(GetStaticObjectField);
    JNI_PLAIN(GetStaticBooleanField);
    JNI_PLAIN(GetStaticByteField);
    JNI_PLAIN(GetStaticCharField);
    JNI_PLAIN(GetStaticShortField);
    JNI_PLAIN(GetStaticIntField);
    JNI_PLAIN(GetStaticLongField);
    JNI_PLAIN(GetStaticFloatField);
    JNI_PLAIN(GetStaticDoubleField);
    JNI_PLAIN(SetStaticObjectField);
    JNI_PLAIN(SetStaticBooleanField);
    JNI_PLAIN(SetStaticByteField);
    JNI_PLAIN(SetStaticCharField);
    JNI_PLAIN(SetStaticShortField);
    JNI_PLAIN(SetStaticIntField);
    JNI_PLAIN(SetStaticLongField);
    JNI_PLAIN(SetStaticFloatField);
    JNI_PLAIN(SetStaticDoubleField);
    JNI_PLAIN(NewString);
    JNI_PLAIN(GetStringLength);
    JNI_PLAIN(GetStringChars);
    JNI_PLAIN(ReleaseStringChars);
    JNI_PLAIN(NewStringUTF);
    JNI_PLAIN(GetStringUTFLength);
    JNI_PLAIN(GetStringUTFChars);
    JNI_PLAIN(ReleaseStringUTFChars);
    JNI_PLAIN(GetArrayLength);
    JNI_PLAIN(NewObjectArray);
    JNI_PLAIN(GetObjectArrayElement);
    JNI_PLAIN(SetObjectArrayElement);
    JNI_PLAIN(NewBooleanArray);
    JNI_PLAIN(NewByteArray);
    JNI_PLAIN(NewCharArray);
    JNI_PLAIN(NewShortArray);
    JNI_PLAIN(NewIntArray);
    JNI_PLAIN(NewLongArray);
    JNI_PLAIN(NewFloatArray);
    JNI_PLAIN(NewDoubleArray);
    JNI_PLAIN(GetBooleanArrayElements);
    JNI_PLAIN(GetByteArrayElements);
    JNI_PLAIN(GetCharArrayElements);
    JNI_PLAIN(GetShortArrayElements);
    JNI_PLAIN(GetIntArrayElements);
    JNI_PLAIN(GetLongArrayElements);
    JNI_PLAIN(GetFloatArrayElements);
    JNI_PLAIN(GetDoubleArrayElements);
    JNI_PLAIN(ReleaseBooleanArrayElements);
    JNI_PLAIN(ReleaseByteArrayElements);
    JNI_PLAIN(ReleaseCharArrayElements);
    JNI_PLAIN(ReleaseShortArrayElements);
    JNI_PLAIN(ReleaseIntArrayElements);
    JNI_PLAIN(ReleaseLongArrayElements);
    JNI_PLAIN(ReleaseFloatArrayElements);
    JNI_PLAIN(ReleaseDoubleArrayElements);
    JNI_PLAIN(GetBooleanArrayRegion);
    JNI_PLAIN(GetByteArrayRegion);
    JNI_PLAIN(GetCharArrayRegion);
    JNI_PLAIN(GetShortArrayRegion);
    JNI_PLAIN(GetIntArrayRegion);
    JNI_PLAIN(GetLongArrayRegion);
    JNI_PLAIN(GetFloatArrayRegion);
    JNI_PLAIN(GetDoubleArrayRegion);
    JNI_PLAIN(SetBooleanArrayRegion);
    JNI_PLAIN(SetByteArrayRegion);
    JNI_PLAIN(SetCharArrayRegion);
    JNI_PLAIN(SetShortArrayRegion);
    JNI_PLAIN(SetIntArrayRegion);
    JNI_PLAIN(SetLongArrayRegion);
    JNI_PLAIN(SetFloatArrayRegion);
    JNI_PLAIN(SetDoubleArrayRegion);
    JNI_PLAIN(RegisterNatives);
    JNI_PLAIN(UnregisterNatives);
    JNI_PLAIN(MonitorEnter);
    JNI_PLAIN(MonitorExit);
    JNI_PLAIN(GetStringRegion);
    JNI_PLAIN(GetStringUTFRegion);
    JNI_PLAIN(GetPrimitiveArrayCritical);
    JNI_PLAIN(ReleasePrimitiveArrayCritical);
    JNI_PLAIN(GetStringCritical);
    JNI_PLAIN(ReleaseStringCritical);
    JNI_PLAIN(NewWeakGlobalRef);
    JNI_PLAIN(DeleteWeakGlobalRef);
    JNI_PLAIN(ExceptionCheck);
    JNI_PLAIN(NewDirectByteBuffer);
    JNI_PLAIN(GetDirectBufferAddress);
    JNI_PLAIN(GetDirectBufferCapacity);
    JNI_PLAIN(GetObjectRefType);
    JNI_VARIADIC(CallObjectMethod, kVirtual, 'L');
    JNI_VARIADIC(CallBooleanMethod, kVirtual, 'Z');
    JNI_VARIADIC(CallByteMethod, kVirtual, 'B');
    JNI_VARIADIC(CallCharMethod, kVirtual, 'C');
    JNI_VARIADIC(CallShortMethod, kVirtual, 'S');
    JNI_VARIADIC(CallIntMethod, kVirtual, 'I');
    JNI_VARIADIC(CallLongMethod, kVirtual, 'J');
    JNI_VARIADIC(CallFloatMethod, kVirtual, 'F');
    JNI_VARIADIC(CallDoubleMethod, kVirtual, 'D');
    JNI_VARIADIC(CallVoidMethod, kVirtual, 'V');
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
    JNI_VARIADIC(CallStaticObjectMethod, kStatic, 'L');
    JNI_VARIADIC(CallStaticBooleanMethod, kStatic, 'Z');
    JNI_VARIADIC(CallStaticByteMethod, kStatic, 'B');
    JNI_VARIADIC(CallStaticCharMethod, kStatic, 'C');
    JNI_VARIADIC(CallStaticShortMethod, kStatic, 'S');
    JNI_VARIADIC(CallStaticIntMethod, kStatic, 'I');
    JNI_VARIADIC(CallStaticLongMethod, kStatic, 'J');
    JNI_VARIADIC(CallStaticFloatMethod, kStatic, 'F');
    JNI_VARIADIC(CallStaticDoubleMethod, kStatic, 'D');
    JNI_VARIADIC(CallStaticVoidMethod, kStatic, 'V');
    JNI_VARIADIC(NewObject, kNewObject, 'L');
  Set(g_env_table, offsetof(JNINativeInterface, GetJavaVM), GetJavaVM, nullptr,
      "JNIEnv::GetJavaVM");
  Set(g_env_table, offsetof(JNINativeInterface, GetMethodID), GetMethodIDStub, nullptr,
      "JNIEnv::GetMethodID");
  if (TraceLevel() >= 2) {
    Set(g_env_table, offsetof(JNINativeInterface, FindClass), TraceFindClass, nullptr,
        "JNIEnv::FindClass");
    Set(g_env_table, offsetof(JNINativeInterface, GetMethodID), TraceGetMethodID, nullptr,
        "JNIEnv::GetMethodID");
    Set(g_env_table, offsetof(JNINativeInterface, GetStaticMethodID), TraceGetMethodID,
        reinterpret_cast<void*>(1), "JNIEnv::GetStaticMethodID");
  }

  Set(g_vm_table, offsetof(JNIInvokeInterface, DestroyJavaVM), VmDestroy, nullptr,
      "JavaVM::DestroyJavaVM");
  Set(g_vm_table, offsetof(JNIInvokeInterface, AttachCurrentThread), VmAttach, nullptr,
      "JavaVM::AttachCurrentThread");
  Set(g_vm_table, offsetof(JNIInvokeInterface, DetachCurrentThread), VmDetach, nullptr,
      "JavaVM::DetachCurrentThread");
  Set(g_vm_table, offsetof(JNIInvokeInterface, GetEnv), VmGetEnv, nullptr, "JavaVM::GetEnv");
  Set(g_vm_table, offsetof(JNIInvokeInterface, AttachCurrentThreadAsDaemon), VmAttach,
      reinterpret_cast<void*>(1), "JavaVM::AttachCurrentThreadAsDaemon");
}

}  // namespace

JNIEnv* HostEnvOf(uint32_t guest_env) {
  if (!guest_env)
    return nullptr;
  GuestEnv* g = Ptr<GuestEnv>(guest_env);
  return g->functions == Addr(g_env_table) ? g->host : nullptr;
}

void* GuestEnvFor(JNIEnv* env) {
  if (!env)
    return nullptr;
  std::lock_guard<std::mutex> l(g_env_lock);
  if (!g_envs)
    g_envs = new std::map<JNIEnv*, GuestEnv*>;
  GuestEnv*& g = (*g_envs)[env];
  if (!g)
    g = new GuestEnv{Addr(g_env_table), env};
  return g;
}

void* GuestVmFor(JavaVM* vm) {
  g_vm.functions = Addr(g_vm_table);
  g_vm.host = vm;
  return &g_vm;
}

void RegisterJni() { BuildTables(); }

}  // namespace ndk
