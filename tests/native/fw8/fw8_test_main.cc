// The native tests of topic fw8-core (mk/topic-fw8-core.mk, make
// fw8-native-test): libutils, libbinder, servicemanager and libandroidfw of
// AOSP 8.1 in WebAssembly, before ART 8.1 (topic art8) runs the framework's
// Java on them. Boots posix_translation's kernel as artvm does
// (art/vfs_setup.cc): /dev/binder, /system from the image packaged with the
// test (framework-res.apk 8.1, build.prop), then loads /system/build.prop
// into the property store as init would. Compiled with posix_translation's
// flags (the Makefile's), the tests with the framework's (FW8_INCLUDES).

#include <stdio.h>

#include "binder/dev_binder.h"
#include "gtest/gtest.h"
#include "kernel/arc_kernel.h"
#include "posix_translation/host_file.h"
#include "posix_translation/readonly_file.h"
#include "posix_translation/virtual_file_system.h"

// shim/fw6/system_properties.cc (libprops8.a).
extern "C" int arc_wasm_load_properties(const char* path);

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  posix_translation::VirtualFileSystem* vfs = arc_kernel::Boot();
  vfs->Mount("/dev/binder", new posix_translation::DevBinderHandler);
  vfs->Mount("/system/", new posix_translation::ReadonlyFileHandler(
      "/arc/system.img", 64 * 1024, new posix_translation::HostFileHandler()));
  int n = arc_wasm_load_properties("/system/build.prop");
  printf("loaded %d properties from /system/build.prop\n", n);
  return RUN_ALL_TESTS();
}
