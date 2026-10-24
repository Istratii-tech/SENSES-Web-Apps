// Replacement for libcore/luni/src/main/native/Register.cpp (libjavacore's
// JNI_OnLoad) for libcore 10 on ART 10. The library is linked statically,
// so its JNI_OnLoad gets a unique name and is found through
// shim/art10/static_jni_libs_art10.cc instead of dlopen/dlsym. (NativeBN is
// on BoringSSL's libcrypto, ExpatParser on expat: mk/topic-art10.mk builds
// both.) register_org_apache_harmony_dalvik_NativeTestTarget returns int, unlike
// the others; Register.cpp declares it void, which native ABIs tolerate but
// WebAssembly does not. As shim/art8/javacore_register8.cc for libcore 8.1.
// Part of the arc-wasm shim layer.

#define LOG_TAG "libcore"

#include <stdlib.h>

#include <log/log.h>
#include <nativehelper/JNIHelp.h>
#include <nativehelper/ScopedLocalFrame.h>

#include "JniConstants.h"

#define STRONG(FN) extern void FN(JNIEnv*);
STRONG(register_android_system_OsConstants)
STRONG(register_java_lang_invoke_MethodHandle)
STRONG(register_java_lang_invoke_VarHandle)
STRONG(register_java_math_NativeBN)
STRONG(register_java_util_regex_Matcher)
STRONG(register_java_util_regex_Pattern)
STRONG(register_libcore_icu_ICU)
STRONG(register_libcore_icu_NativeConverter)
STRONG(register_libcore_icu_TimeZoneNames)
STRONG(register_libcore_io_AsynchronousCloseMonitor)
STRONG(register_libcore_io_Linux)
STRONG(register_libcore_io_Memory)
STRONG(register_libcore_util_NativeAllocationRegistry)
STRONG(register_org_apache_harmony_xml_ExpatParser)
STRONG(register_sun_misc_Unsafe)
#undef STRONG
extern int register_org_apache_harmony_dalvik_NativeTestTarget(JNIEnv*);

extern "C" jint arc_wasm_javacore10_JNI_OnLoad(JavaVM* vm, void*) {
  JNIEnv* env;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
    ALOGE("JavaVM::GetEnv() failed");
    abort();
  }

  ScopedLocalFrame localFrame(env);

#define REGISTER(FN) FN(env)
  REGISTER(register_android_system_OsConstants);
  REGISTER(register_java_lang_invoke_MethodHandle);
  REGISTER(register_java_lang_invoke_VarHandle);
  REGISTER(register_java_math_NativeBN);
  REGISTER(register_java_util_regex_Matcher);
  REGISTER(register_java_util_regex_Pattern);
  REGISTER(register_libcore_icu_ICU);
  REGISTER(register_libcore_icu_NativeConverter);
  REGISTER(register_libcore_icu_TimeZoneNames);
  REGISTER(register_libcore_io_AsynchronousCloseMonitor);
  REGISTER(register_libcore_io_Linux);
  REGISTER(register_libcore_io_Memory);
  REGISTER(register_libcore_util_NativeAllocationRegistry);
  register_org_apache_harmony_dalvik_NativeTestTarget(env);
  REGISTER(register_org_apache_harmony_xml_ExpatParser);
  REGISTER(register_sun_misc_Unsafe);
#undef REGISTER

  JniConstants::Initialize(env);
  return JNI_VERSION_1_6;
}
