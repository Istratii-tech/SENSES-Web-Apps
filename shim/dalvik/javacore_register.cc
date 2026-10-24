// Replacement for libcore/luni/src/main/native/Register.cpp (libjavacore's
// JNI_OnLoad). The library is linked statically, so its JNI_OnLoad gets a
// unique name and is found through shim/dalvik/static_jni_libs.h instead of
// dlopen/dlsym. Modules that are not built (ExpatParser needs expat,
// RawSocket has no browser equivalent; NativeBN is built with OpenSSL in
// mk/topic-crypto.mk) are
// declared weak and skipped; calls to their native methods throw
// UnsatisfiedLinkError. (The others must stay strong references: a weak one
// does not pull the object file out of the static library.)
// Three register_* functions return int; Register.cpp declares them void,
// which native ABIs tolerate but a WebAssembly indirect call does not.
// Part of the arc-wasm shim layer.

#define LOG_TAG "libcore"

#include <stdlib.h>

#include "JniConstants.h"
#include "ScopedLocalFrame.h"
#include "cutils/log.h"

extern void register_java_io_Console(JNIEnv*);
extern void register_java_io_File(JNIEnv*);
extern void register_java_io_ObjectStreamClass(JNIEnv*);
extern void register_java_lang_Character(JNIEnv*);
extern int register_java_lang_Double(JNIEnv*);  // returns int, unlike the others
extern int register_java_lang_Float(JNIEnv*);  // returns int, unlike the others
extern void register_java_lang_Math(JNIEnv*);
extern void register_java_lang_ProcessManager(JNIEnv*);
extern void register_java_lang_RealToString(JNIEnv*);
extern void register_java_lang_StrictMath(JNIEnv*);
extern void register_java_lang_StringToReal(JNIEnv*);
extern void register_java_lang_System(JNIEnv*);
extern void register_java_math_NativeBN(JNIEnv*);
extern void register_java_nio_ByteOrder(JNIEnv*);
extern void register_java_nio_charset_Charsets(JNIEnv*);
extern void register_java_text_Bidi(JNIEnv*);
extern void register_java_util_regex_Matcher(JNIEnv*);
extern void register_java_util_regex_Pattern(JNIEnv*);
extern void register_java_util_zip_Adler32(JNIEnv*);
extern void register_java_util_zip_CRC32(JNIEnv*);
extern void register_java_util_zip_Deflater(JNIEnv*);
extern void register_java_util_zip_Inflater(JNIEnv*);
extern void register_libcore_icu_AlphabeticIndex(JNIEnv*);
extern void register_libcore_icu_DateIntervalFormat(JNIEnv*);
extern void register_libcore_icu_ICU(JNIEnv*);
extern void register_libcore_icu_NativeBreakIterator(JNIEnv*);
extern void register_libcore_icu_NativeCollation(JNIEnv*);
extern void register_libcore_icu_NativeConverter(JNIEnv*);
extern void register_libcore_icu_NativeDecimalFormat(JNIEnv*);
extern void register_libcore_icu_NativeIDN(JNIEnv*);
extern void register_libcore_icu_NativeNormalizer(JNIEnv*);
extern void register_libcore_icu_NativePluralRules(JNIEnv*);
extern void register_libcore_icu_TimeZoneNames(JNIEnv*);
extern void register_libcore_icu_Transliterator(JNIEnv*);
extern void register_libcore_io_AsynchronousCloseMonitor(JNIEnv*);
extern void register_libcore_io_Memory(JNIEnv*);
extern void register_libcore_io_OsConstants(JNIEnv*);
extern void register_libcore_io_Posix(JNIEnv*);
extern void register_libcore_net_RawSocket(JNIEnv*) __attribute__((weak));
extern int register_org_apache_harmony_dalvik_NativeTestTarget(JNIEnv*);  // returns int, unlike the others
extern void register_org_apache_harmony_xml_ExpatParser(JNIEnv*) __attribute__((weak));
extern void register_sun_misc_Unsafe(JNIEnv*);

namespace {

struct Module {
  const char* name;
  void (*fn)(JNIEnv*);
};

const Module kModules[] = {
    {"register_java_io_Console", register_java_io_Console},
    {"register_java_io_File", register_java_io_File},
    {"register_java_io_ObjectStreamClass", register_java_io_ObjectStreamClass},
    {"register_java_lang_Character", register_java_lang_Character},
    {"register_java_lang_Double", [](JNIEnv* env) { register_java_lang_Double(env); }},
    {"register_java_lang_Float", [](JNIEnv* env) { register_java_lang_Float(env); }},
    {"register_java_lang_Math", register_java_lang_Math},
    {"register_java_lang_ProcessManager", register_java_lang_ProcessManager},
    {"register_java_lang_RealToString", register_java_lang_RealToString},
    {"register_java_lang_StrictMath", register_java_lang_StrictMath},
    {"register_java_lang_StringToReal", register_java_lang_StringToReal},
    {"register_java_lang_System", register_java_lang_System},
    {"register_java_math_NativeBN", register_java_math_NativeBN},
    {"register_java_nio_ByteOrder", register_java_nio_ByteOrder},
    {"register_java_nio_charset_Charsets", register_java_nio_charset_Charsets},
    {"register_java_text_Bidi", register_java_text_Bidi},
    {"register_java_util_regex_Matcher", register_java_util_regex_Matcher},
    {"register_java_util_regex_Pattern", register_java_util_regex_Pattern},
    {"register_java_util_zip_Adler32", register_java_util_zip_Adler32},
    {"register_java_util_zip_CRC32", register_java_util_zip_CRC32},
    {"register_java_util_zip_Deflater", register_java_util_zip_Deflater},
    {"register_java_util_zip_Inflater", register_java_util_zip_Inflater},
    {"register_libcore_icu_AlphabeticIndex", register_libcore_icu_AlphabeticIndex},
    {"register_libcore_icu_DateIntervalFormat", register_libcore_icu_DateIntervalFormat},
    {"register_libcore_icu_ICU", register_libcore_icu_ICU},
    {"register_libcore_icu_NativeBreakIterator", register_libcore_icu_NativeBreakIterator},
    {"register_libcore_icu_NativeCollation", register_libcore_icu_NativeCollation},
    {"register_libcore_icu_NativeConverter", register_libcore_icu_NativeConverter},
    {"register_libcore_icu_NativeDecimalFormat", register_libcore_icu_NativeDecimalFormat},
    {"register_libcore_icu_NativeIDN", register_libcore_icu_NativeIDN},
    {"register_libcore_icu_NativeNormalizer", register_libcore_icu_NativeNormalizer},
    {"register_libcore_icu_NativePluralRules", register_libcore_icu_NativePluralRules},
    {"register_libcore_icu_TimeZoneNames", register_libcore_icu_TimeZoneNames},
    {"register_libcore_icu_Transliterator", register_libcore_icu_Transliterator},
    {"register_libcore_io_AsynchronousCloseMonitor", register_libcore_io_AsynchronousCloseMonitor},
    {"register_libcore_io_Memory", register_libcore_io_Memory},
    {"register_libcore_io_OsConstants", register_libcore_io_OsConstants},
    {"register_libcore_io_Posix", register_libcore_io_Posix},
    {"register_libcore_net_RawSocket", register_libcore_net_RawSocket},
    {"register_org_apache_harmony_dalvik_NativeTestTarget", [](JNIEnv* env) { register_org_apache_harmony_dalvik_NativeTestTarget(env); }},
    {"register_org_apache_harmony_xml_ExpatParser", register_org_apache_harmony_xml_ExpatParser},
    {"register_sun_misc_Unsafe", register_sun_misc_Unsafe},
};

}  // namespace

extern "C" int arc_wasm_javacore_JNI_OnLoad(JavaVM* vm, void*) {
  JNIEnv* env;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
    ALOGE("JavaVM::GetEnv() failed");
    abort();
  }
  ScopedLocalFrame localFrame(env);
  for (const Module& m : kModules) {
    if (m.fn)
      m.fn(env);
    else
      ALOGD("libjavacore: %s not built; its natives are unavailable", m.name);
  }
  return JNI_VERSION_1_6;
}
