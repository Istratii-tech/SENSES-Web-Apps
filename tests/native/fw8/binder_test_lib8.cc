// libarcwasm_binder_test.so on ART 8.1: arcwasm.BinderTest System.load()s
// it; artvm8 links it in (mk/topic-fw8-core.mk), and this adds it to the
// table of statically linked JNI libraries of topic art8
// (shim/art8/static_jni_libs_art8.cc) before main(), so that its natives
// (tests/native/binder_test_jni.cc) are registered on load.

#include <jni.h>

extern "C" int arc_wasm_binder_test_JNI_OnLoad(JavaVM* vm, void* reserved);
extern "C" void arc_wasm_art8_add_static_jni_lib(const char* name,
                                                int (*on_load)(JavaVM*, void*));

namespace {

struct AddBinderTestLib {
  AddBinderTestLib() {
    arc_wasm_art8_add_static_jni_lib("libarcwasm_binder_test.so", arc_wasm_binder_test_JNI_OnLoad);
  }
} gAddBinderTestLib;

}  // namespace
