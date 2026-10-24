// The libandroid_runtime 8.1 frame (shim/fw8/register_fw8.cc): which of
// gRegJNI's 150 registrations libandroid_runtime8.a links in (weak
// references, the library linked whole). Running them needs ART 8.1
// (topic art8): the Java tests do. Part of make fw8-native-test.

#include <stdio.h>
#include <string.h>

#include <set>
#include <string>

#include "gtest/gtest.h"

namespace android {
int arc_wasm_fw8_jni_count(int* linked);
const char* arc_wasm_fw8_jni_name(int i, bool* linked);
}

namespace {

TEST(Runtime8, CoreRegistrationsLinked) {
  int linked = 0;
  const int total = android::arc_wasm_fw8_jni_count(&linked);
  EXPECT_EQ(150 + 4, total);  // gRegJNI of 8.1 and arc-wasm's four
  std::set<std::string> in;
  for (int i = 0; i < total; i++) {
    bool l = false;
    const char* name = android::arc_wasm_fw8_jni_name(i, &l);
    if (l) in.insert(name);
  }
  for (const char* core : {"register_com_android_internal_os_RuntimeInit",
                           "register_com_android_internal_os_ZygoteInit_nativeZygoteInit",
                           "register_android_util_Log", "register_android_os_SystemClock",
                           "register_android_util_EventLog", "register_android_os_Trace",
                           "register_android_os_MessageQueue", "register_android_os_Binder",
                           "register_android_os_Parcel", "register_android_content_AssetManager",
                           "register_android_content_StringBlock",
                           "register_android_content_XmlBlock", "register_android_os_Process",
                           "register_android_os_SystemProperties", "register_android_nio_utils",
                           "register_android_content_res_Configuration",
                           "register_com_android_internal_util_VirtualRefBasePtr",
                           "register_android_os_HwBinder", "register_android_os_HwBlob",
                           "register_android_os_HwParcel", "register_android_os_HwRemoteBinder",
                           "register_arc_wasm_binder"}) {
    EXPECT_EQ(1u, in.count(core)) << core << " is not linked in";
  }
  printf("%d of %d JNI registrations linked in\n", linked, total);
  EXPECT_EQ(static_cast<int>(in.size()), linked);
}

}  // namespace
