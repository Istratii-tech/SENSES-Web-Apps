// libjavacrypto.so on ART 10: conscrypt's natives are linked into artvm10
// (mk/topic-fw10-crypto-media.mk), and this adds the library to the table of
// statically linked JNI libraries of topic art10
// (shim/art10/static_jni_libs_art10.cc) before main(), so that
// System.loadLibrary("javacrypto") (NativeCryptoJni) runs its JNI_OnLoad.
// Part of the arc-wasm shim layer (topic fw10-crypto-media).

#include <jni.h>

extern "C" int arc_wasm_javacrypto10_JNI_OnLoad(JavaVM* vm, void* reserved);
extern "C" void arc_wasm_art10_add_static_jni_lib(const char* name,
                                                 int (*on_load)(JavaVM*, void*));

namespace {

struct AddJavacryptoLib {
  AddJavacryptoLib() {
    arc_wasm_art10_add_static_jni_lib("libjavacrypto.so", arc_wasm_javacrypto10_JNI_OnLoad);
  }
} gAddJavacryptoLib;

}  // namespace
