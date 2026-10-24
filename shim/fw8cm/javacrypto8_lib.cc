// libjavacrypto.so on ART 8.1: conscrypt's natives are linked into artvm8
// (mk/topic-fw8-crypto-media.mk), and this adds the library to the table of
// statically linked JNI libraries of topic art8
// (shim/art8/static_jni_libs_art8.cc) before main(), so that
// System.loadLibrary("javacrypto") (NativeCryptoJni) runs its JNI_OnLoad.
// Part of the arc-wasm shim layer (topic fw8-crypto-media).

#include <jni.h>

extern "C" int arc_wasm_javacrypto8_JNI_OnLoad(JavaVM* vm, void* reserved);
extern "C" void arc_wasm_art8_add_static_jni_lib(const char* name,
                                                int (*on_load)(JavaVM*, void*));

namespace {

struct AddJavacryptoLib {
  AddJavacryptoLib() {
    arc_wasm_art8_add_static_jni_lib("libjavacrypto.so", arc_wasm_javacrypto8_JNI_OnLoad);
  }
} gAddJavacryptoLib;

}  // namespace
