// dalvikvm for arc-wasm: boots the kernel layer, starts Dalvik through the
// JNI invocation API and runs a class's static main(String[]), like AOSP's
// dalvikvm command:
//
//   dalvikvm [VM options] -cp CLASSPATH CLASS [ARGS...]
//
// VM options are passed to JNI_CreateJavaVM unchanged (e.g. -Xmx64m,
// -verbose:class). BOOTCLASSPATH defaults to libcore, the framework
// (framework.jar, framework2.jar) and the libraries it needs; the framework's
// natives are registered before CLASS runs, as app_process does.

#include <cutils/properties.h>
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

extern "C" int arc_dalvik_boot_kernel();
extern "C" int arc_wasm_register_framework_natives(JNIEnv* env);
extern "C" int arc_wasm_load_properties(const char* path);
extern "C" void arc_wasm_set_browser_timezone();  // shim/userspace/timezone.cc

namespace {

int Usage() {
  fprintf(stderr, "usage: dalvikvm [VM options] -cp CLASSPATH CLASS [ARGS...]\n");
  return 2;
}

// Runs CLASS.main(ARGS); returns the process exit status.
int RunMain(JNIEnv* env, const char* class_name, int argc, char** argv) {
  std::string slashed(class_name);
  for (char& c : slashed) {
    if (c == '.')
      c = '/';
  }
  jclass main_class = env->FindClass(slashed.c_str());
  if (main_class == NULL) {
    fprintf(stderr, "dalvikvm: class not found: %s\n", class_name);
    env->ExceptionDescribe();
    return 1;
  }
  jmethodID main = env->GetStaticMethodID(main_class, "main",
                                          "([Ljava/lang/String;)V");
  if (main == NULL) {
    fprintf(stderr, "dalvikvm: no static main(String[]) in %s\n", class_name);
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

}  // namespace

int main(int argc, char** argv) {
  const bool persistent_data = arc_dalvik_boot_kernel();
  setenv("ANDROID_ROOT", "/system", 0);
  setenv("ANDROID_DATA", "/data", 0);
  // Primary external storage ("sdcard"): a directory in /data (OPFS in
  // browsers). Without it Environment builds File(null) (android.os.Debug's
  // static initializer, for one).
  setenv("EXTERNAL_STORAGE", "/data/media/0", 0);
  // java.library.path (System.loadLibrary's search path), as init's
  // environment on a device.
  setenv("LD_LIBRARY_PATH", "/vendor/lib:/system/lib", 0);
  // As init does before starting anything (shim/framework/load_properties.cc).
  arc_wasm_load_properties("/system/build.prop");
  arc_wasm_set_browser_timezone();
  // AOSP 4.4.4's order (FW_BOOT_JARS in mk/framework.mk; android.policy and
  // services: mk/topic-activity.mk).
  // An image may append its own boot jars with ro.arcwasm.bootclasspath.append
  // in its build.prop (topic userspace: HTC Sense's framework libraries).
  std::string boot_class_path =
      "/system/framework/core.jar:/system/framework/conscrypt.jar:"
      "/system/framework/okhttp.jar:/system/framework/core-junit.jar:"
      "/system/framework/bouncycastle.jar:/system/framework/ext.jar:"
      "/system/framework/framework.jar:/system/framework/framework2.jar:"
      "/system/framework/android.policy.jar:/system/framework/services.jar";
  char append[PROPERTY_VALUE_MAX];
  if (property_get("ro.arcwasm.bootclasspath.append", append, "") > 0) {
    boot_class_path += ":";
    boot_class_path += append;
  }
  setenv("BOOTCLASSPATH", boot_class_path.c_str(), 0);
  fprintf(stderr, "dalvikvm: kernel up, /data %s\n",
          persistent_data ? "on OPFS" : "not mounted");

  // -Xrs: no signal catcher thread (it waits for SIGQUIT/SIGUSR1 with
  // sigwait, and a browser delivers no signals).
  std::vector<std::string> vm_args = {"-Xrs"};
  int i = 1;
  for (; i < argc && argv[i][0] == '-'; ++i) {
    if (strcmp(argv[i], "-cp") == 0 || strcmp(argv[i], "-classpath") == 0) {
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
    fprintf(stderr, "dalvikvm: JNI_CreateJavaVM failed\n");
    return 1;
  }
  if (arc_wasm_register_framework_natives(env) != 0)
    fprintf(stderr, "dalvikvm: some framework natives are not registered\n");
  const int status = RunMain(env, class_name, argc - i, argv + i);
  if (vm->DetachCurrentThread() != JNI_OK)
    fprintf(stderr, "dalvikvm: could not detach the main thread\n");
  vm->DestroyJavaVM();
  return status;
}
