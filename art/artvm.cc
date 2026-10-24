// artvm for arc-wasm: boots the kernel layer, starts ART (AOSP 6.0.1,
// interpreter only, no boot image) through the JNI invocation API and runs a
// class's static main(String[]), like AOSP's dalvikvm command does on ART:
//
//   artvm [VM options] -cp CLASSPATH CLASS [ARGS...]
//
// VM options are passed to JNI_CreateJavaVM unchanged (e.g. -Xmx256m,
// -verbose:class). The options ART needs here come first and can be
// overridden by later ones: -Xint, no dex2oat and no image (the boot class
// path is loaded from the jars' classes*.dex), the non-concurrent
// mark-sweep GC. BOOTCLASSPATH defaults to AOSP 6.0.1's (the framework's
// jars, topic fw6-core); with the framework on it, the framework's natives
// are registered before main() runs, as app_process does
// (arc_wasm_fw6_start, shim/fw6/android_runtime.cc).

#include <emscripten.h>
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

extern "C" int arc_art_boot_kernel();
extern "C" void arc_art_register_test_natives(JNIEnv* env);
// shim/fw6/android_runtime.cc (topic fw6-core), when linked in.
extern "C" int arc_wasm_fw6_start(JNIEnv* env) __attribute__((weak));
// art/watchdog.cc (topic tiktok): --watchdog=SECONDS.
void arc_art_start_watchdog(unsigned seconds) __attribute__((weak));
// The memory budget (shim/membudget, topic mem-budget): the heap options
// it gives, and the dalvik.vm.* properties the framework reads it from.
extern "C" int arc_mem_budget_vm_options(char (*out)[48], int max) __attribute__((weak));
extern "C" void arc_mem_budget_publish_properties(void) __attribute__((weak));

namespace {

int Usage() {
  fprintf(stderr, "usage: artvm [VM options] -cp CLASSPATH CLASS [ARGS...]\n");
  return 2;
}

// Runs CLASS.main(ARGS); returns the process exit status.
int RunMain(JNIEnv* env, const char* class_name, int argc, char** argv) {
  std::string slashed(class_name);
  for (char& c : slashed) {
    if (c == '.')
      c = '/';
  }
  // FindClass from a thread with no Java frames uses the system class
  // loader (java.class.path), as on a device.
  jclass main_class = env->FindClass(slashed.c_str());
  if (main_class == NULL) {
    fprintf(stderr, "artvm: class not found: %s\n", class_name);
    env->ExceptionDescribe();
    return 1;
  }
  jmethodID main = env->GetStaticMethodID(main_class, "main", "([Ljava/lang/String;)V");
  if (main == NULL) {
    fprintf(stderr, "artvm: no static main(String[]) in %s\n", class_name);
    env->ExceptionDescribe();
    return 1;
  }
  jclass string_class = env->FindClass("java/lang/String");
  jobjectArray args = env->NewObjectArray(argc, string_class, NULL);
  for (int i = 0; i < argc; ++i) {
    jstring s = env->NewStringUTF(argv[i]);
    env->SetObjectArrayElement(args, i, s);
    env->DeleteLocalRef(s);
  }
  env->CallStaticVoidMethod(main_class, main, args);
  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    return 1;
  }
  return 0;
}

// The device time zone is the browser's (topic userspace): what Intl reports,
// e.g. "Europe/Moscow", as persist.sys.timezone, which SystemServer hands to
// libcore (TimezoneGetter, as RuntimeInit does). Without it, apps run in GMT.
EM_JS(char*, arc_art_browser_timezone, (), {
  var tz = "";
  try {
    tz = Intl.DateTimeFormat().resolvedOptions().timeZone || "";
  } catch (e) {
  }
  return stringToNewUTF8(tz);
});

}  // namespace

extern "C" int __system_property_set(const char* key, const char* value);

int main(int argc, char** argv) {
  // --mem=MB: the memory budget (as ?mem= on a page; shim/membudget).
  for (int k = 1; k < argc && argv[k][0] == '-'; ++k) {
    if (strncmp(argv[k], "--mem=", 6) == 0)
      setenv("ARC_MEM_BUDGET_MB", argv[k] + 6, 1);
  }
  const bool persistent_data = arc_art_boot_kernel();
  setenv("ANDROID_ROOT", "/system", 0);
  setenv("ANDROID_DATA", "/data", 0);
  setenv("EXTERNAL_STORAGE", "/data/media/0", 0);
  setenv("LD_LIBRARY_PATH", "/vendor/lib:/system/lib", 0);
  char* tz = arc_art_browser_timezone();
  if (tz && *tz)
    __system_property_set("persist.sys.timezone", tz);
  free(tz);
  // system/core/rootdir/init.environ.rc of AOSP 6.0.1.
  setenv("BOOTCLASSPATH",
         "/system/framework/core-libart.jar:/system/framework/conscrypt.jar:"
         "/system/framework/okhttp.jar:/system/framework/core-junit.jar:"
         "/system/framework/bouncycastle.jar:/system/framework/ext.jar:"
         "/system/framework/framework.jar:/system/framework/telephony-common.jar:"
         "/system/framework/voip-common.jar:/system/framework/ims-common.jar:"
         "/system/framework/apache-xml.jar:/system/framework/org.apache.http.legacy.boot.jar:"
         // arc-wasm: the in-process system server (topic fw6-system), on the
         // boot class path as services.jar was in Android 4.4 (not in 6.0).
         "/system/framework/services.jar",
         0);
  fprintf(stderr, "artvm: kernel up, /data %s\n", persistent_data ? "on OPFS" : "not mounted");

  std::vector<std::string> vm_args = {
      // Interpreter only; no oat files and no boot image: the boot class
      // path is opened as plain dex files (ClassLinker::InitWithoutImage).
      "-Xint", "-Xnodex2oat", "-Xnoimage-dex2oat", "-Xnorelocate",
      // Stop-the-world mark-sweep, also in the "background" process state.
      "-Xgc:MS", "-XX:BackgroundGC=MS",
      "-XX:ParallelGCThreads=0", "-XX:ConcGCThreads=0",
      // No second main space for compaction on OOM: it would reserve (and,
      // in wasm, commit) another -Xmx of memory.
      "-XX:DisableHSpaceCompactForOOM",
      // Heap growth as on a 6.0 phone (dalvik.vm.heaptargetutilization 0.75,
      // heapmaxfree 8m in frameworks/native/build/phone-*-dalvik-heap.mk), but
      // up to 16 MB free after a GC: ART's defaults (0.5, 2 MB) collect every
      // 2 MB, which for a big app (TikTok: ~130 MB live) is a 30 ms pause of
      // the interpreter every ~0.1 s (topic tiktok).
      "-XX:HeapTargetUtilization=0.75", "-XX:HeapMinFree=2m", "-XX:HeapMaxFree=16m",
  };
  if (arc_mem_budget_vm_options) {
    // -Xms, -Xmx and the growth limit of the budget's device class. A heap
    // size on the command line replaces them (its growth limit is then its
    // -Xmx, as ART's default).
    bool own_heap = false;
    for (int k = 1; k < argc && argv[k][0] == '-'; ++k) {
      own_heap |= strncmp(argv[k], "-Xmx", 4) == 0 || strncmp(argv[k], "-XX:HeapGrowthLimit", 19) == 0;
    }
    char budget_opts[4][48];
    const int n = arc_mem_budget_vm_options(budget_opts, 4);
    for (int k = 0; k < n && !own_heap; ++k)
      vm_args.push_back(budget_opts[k]);
  }
  int i = 1;
  for (; i < argc && argv[i][0] == '-'; ++i) {
    if (strncmp(argv[i], "--mem=", 6) == 0) {
      // (Read above.)
    } else if (strncmp(argv[i], "--watchdog=", 11) == 0) {
      // Thread states every N s, for hangs (art/watchdog.cc, topic tiktok).
      if (arc_art_start_watchdog) arc_art_start_watchdog(atoi(argv[i] + 11));
    } else if (strcmp(argv[i], "-cp") == 0 || strcmp(argv[i], "-classpath") == 0) {
      if (++i == argc)
        return Usage();
      vm_args.push_back(std::string("-Djava.class.path=") + argv[i]);
    } else {
      vm_args.push_back(argv[i]);
    }
  }
  if (i == argc)
    return Usage();
  const char* class_name = argv[i++];

  std::vector<JavaVMOption> options(vm_args.size());
  for (size_t j = 0; j < vm_args.size(); ++j) {
    options[j].optionString = vm_args[j].c_str();
    options[j].extraInfo = NULL;
  }
  JavaVMInitArgs init_args;
  init_args.version = JNI_VERSION_1_6;
  init_args.nOptions = options.size();
  init_args.options = options.data();
  init_args.ignoreUnrecognized = JNI_FALSE;

  JavaVM* vm = NULL;
  JNIEnv* env = NULL;
  if (JNI_CreateJavaVM(&vm, &env, &init_args) != JNI_OK) {
    fprintf(stderr, "artvm: JNI_CreateJavaVM failed\n");
    return 1;
  }
  arc_art_register_test_natives(env);
  if (arc_wasm_fw6_start) {
    // Only with the framework on the boot class path (BOOTCLASSPATH may
    // have been set to core-libart.jar alone).
    jclass binder = env->FindClass("android/os/Binder");
    env->ExceptionClear();
    if (binder != NULL && arc_wasm_fw6_start(env) != 0)
      fprintf(stderr, "artvm: some framework natives did not register\n");
  }
  // After build.prop (arc_wasm_fw6_start loads it): the budget's heap sizes.
  if (arc_mem_budget_publish_properties)
    arc_mem_budget_publish_properties();
  const int status = RunMain(env, class_name, argc - i, argv + i);
  if (vm->DetachCurrentThread() != JNI_OK)
    fprintf(stderr, "artvm: could not detach the main thread\n");
  vm->DestroyJavaVM();
  return status;
}
