// Replacement for libcore/luni/src/main/native/Register.cpp (libjavacore's
// JNI_OnLoad) for libcore 6.0 on ART. The library is linked statically, so
// its JNI_OnLoad gets a unique name and is found through
// shim/art/static_jni_libs_art.cc instead of dlopen/dlsym. Modules built
// elsewhere (NativeBN needs BoringSSL, ExpatParser needs expat: topic
// fw6-crypto, mk/topic-fw6-crypto.mk) are declared weak and skipped when
// they are not linked in; calls to their native methods throw
// UnsatisfiedLinkError. (The others stay strong references: a weak one does
// not pull the object file out of the static library.)
// register_org_apache_harmony_dalvik_NativeTestTarget returns int, unlike
// the others; Register.cpp declares it void, which native ABIs tolerate but
// WebAssembly does not. Part of the arc-wasm shim layer.

#define LOG_TAG "libcore"

#include <stdlib.h>

#include "JniConstants.h"
#include "ScopedLocalFrame.h"
#include "cutils/log.h"

#define STRONG(FN) extern void FN(JNIEnv*);
#define WEAK(FN) extern void FN(JNIEnv*) __attribute__((weak));
STRONG(register_android_system_OsConstants)
STRONG(register_java_io_File)
STRONG(register_java_io_FileDescriptor)
STRONG(register_java_io_ObjectStreamClass)
STRONG(register_java_lang_Character)
STRONG(register_java_lang_Double)
STRONG(register_java_lang_Float)
STRONG(register_java_lang_Math)
STRONG(register_java_lang_ProcessManager)
STRONG(register_java_lang_RealToString)
STRONG(register_java_lang_StrictMath)
STRONG(register_java_lang_StringToReal)
STRONG(register_java_lang_System)
WEAK(register_java_math_NativeBN)
STRONG(register_java_nio_ByteOrder)
STRONG(register_java_text_Bidi)
STRONG(register_java_util_jar_StrictJarFile)
STRONG(register_java_util_regex_Matcher)
STRONG(register_java_util_regex_Pattern)
STRONG(register_java_util_zip_Adler32)
STRONG(register_java_util_zip_CRC32)
STRONG(register_java_util_zip_Deflater)
STRONG(register_java_util_zip_Inflater)
STRONG(register_libcore_icu_AlphabeticIndex)
STRONG(register_libcore_icu_ICU)
STRONG(register_libcore_icu_NativeCollation)
STRONG(register_libcore_icu_NativeConverter)
STRONG(register_libcore_icu_NativeDecimalFormat)
STRONG(register_libcore_icu_NativeIDN)
STRONG(register_libcore_icu_NativeNormalizer)
STRONG(register_libcore_icu_NativePluralRules)
STRONG(register_libcore_icu_TimeZoneNames)
STRONG(register_libcore_icu_Transliterator)
STRONG(register_libcore_io_AsynchronousCloseMonitor)
STRONG(register_libcore_io_Memory)
STRONG(register_libcore_io_Posix)
WEAK(register_org_apache_harmony_xml_ExpatParser)
STRONG(register_sun_misc_Unsafe)
#undef STRONG
#undef WEAK
extern int register_org_apache_harmony_dalvik_NativeTestTarget(JNIEnv*);

extern "C" jint arc_wasm_javacore_JNI_OnLoad(JavaVM* vm, void*) {
  JNIEnv* env;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
    ALOGE("JavaVM::GetEnv() failed");
    abort();
  }

  ScopedLocalFrame localFrame(env);

#define REGISTER(FN) \
  if (FN)            \
    FN(env);         \
  else               \
    ALOGW("libjavacore: %s is not built; its natives are missing", #FN)
  REGISTER(register_android_system_OsConstants);
  REGISTER(register_java_io_File);
  REGISTER(register_java_io_FileDescriptor);
  REGISTER(register_java_io_ObjectStreamClass);
  REGISTER(register_java_lang_Character);
  REGISTER(register_java_lang_Double);
  REGISTER(register_java_lang_Float);
  REGISTER(register_java_lang_Math);
  REGISTER(register_java_lang_ProcessManager);
  REGISTER(register_java_lang_RealToString);
  REGISTER(register_java_lang_StrictMath);
  REGISTER(register_java_lang_StringToReal);
  REGISTER(register_java_lang_System);
  REGISTER(register_java_math_NativeBN);
  REGISTER(register_java_nio_ByteOrder);
  REGISTER(register_java_text_Bidi);
  REGISTER(register_java_util_jar_StrictJarFile);
  REGISTER(register_java_util_regex_Matcher);
  REGISTER(register_java_util_regex_Pattern);
  REGISTER(register_java_util_zip_Adler32);
  REGISTER(register_java_util_zip_CRC32);
  REGISTER(register_java_util_zip_Deflater);
  REGISTER(register_java_util_zip_Inflater);
  REGISTER(register_libcore_icu_AlphabeticIndex);
  REGISTER(register_libcore_icu_ICU);
  REGISTER(register_libcore_icu_NativeCollation);
  REGISTER(register_libcore_icu_NativeConverter);
  REGISTER(register_libcore_icu_NativeDecimalFormat);
  REGISTER(register_libcore_icu_NativeIDN);
  REGISTER(register_libcore_icu_NativeNormalizer);
  REGISTER(register_libcore_icu_NativePluralRules);
  REGISTER(register_libcore_icu_TimeZoneNames);
  REGISTER(register_libcore_icu_Transliterator);
  REGISTER(register_libcore_io_AsynchronousCloseMonitor);
  REGISTER(register_libcore_io_Memory);
  REGISTER(register_libcore_io_Posix);
  register_org_apache_harmony_dalvik_NativeTestTarget(env);
  REGISTER(register_org_apache_harmony_xml_ExpatParser);
  REGISTER(register_sun_misc_Unsafe);
#undef REGISTER

  return JNI_VERSION_1_6;
}
