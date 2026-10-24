// android.app.NativeActivity for guest code (docs/topics/native-activity.md):
// the natives of frameworks/base/core/jni/android_app_NativeActivity.cpp
// (register_android_app_NativeActivity, which gRegJNI refers to weakly) on
// the NDK bridge, and what libandroid gives such apps: ANativeActivity_*,
// AInputQueue/AInputEvent (input.h) and AConfiguration (configuration.h).
//
//   loadNativeCode   dlopens the app's ARM library through the bridge (as
//       System.load does: ndk_dalvik_dlopen), finds its entry point
//       (ANativeActivity_onCreate or the android.app.func_name meta-data,
//       which NativeActivity.java passes) and calls it with an
//       ANativeActivity laid out for the guest's ABI (armeabi(-v7a) or
//       arm64-v8a): its JavaVM and JNIEnv are the bridge's guest-visible
//       ones, |clazz| a global reference, |assetManager| the AAssetManager
//       the bridge's AAsset_* take. The guest fills the callbacks table;
//       the other natives call its entries (guest functions) on the main
//       thread.
//   the window       the ANativeWindow of the Surface of NativeActivity's
//       SurfaceHolder (the same pointer ANativeWindow_fromSurface gives:
//       guest EGL and ANativeWindow_lock draw into it).
//   the input queue  android_view_InputQueue's InputQueue (the AInputQueue
//       of a device) and its InputEvents, AOSP's libandroid input.cpp built
//       for the host; only attachLooper differs (a guest callback).
//   main-thread work ANativeActivity_finish/setWindowFlags/... from any
//       thread: a pipe on the main Looper, as on a device.
//
// Compiled with the framework's headers like ndk_android_fw.cc, for 8.1, 10
// and (NA_FW6) 6.0.1, whose loadNativeCode takes no class loader
// (mk/topic-native-activity.mk). Part of the arc-wasm shim layer.

#undef LOG_TAG
#define LOG_TAG "NativeActivity"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <android/configuration.h>
#include <android/input.h>
#include <android/native_activity.h>
#include <android_runtime/android_content_res_Configuration.h>
#include <android_runtime/android_view_InputQueue.h>
#include <android_runtime/android_view_Surface.h>
#include <input/Input.h>
#include <jni.h>
#include <system/window.h>
#include <utils/Log.h>
#include <utils/Looper.h>
#include <utils/String8.h>

#include "android_os_MessageQueue.h"
#include "ndk_android_fw.h"
#include "ndk_bridge.h"
#include "ndk_host_libs.h"
#include "ndk_jni.h"

extern "C" int property_get(const char* key, char* value, const char* default_value);
// The bridge's dlopen for app libraries (shim/fw6-system/ndk_art.cc).
extern "C" int ndk_dalvik_dlopen(const char* path, int flags, void** handle)
    __attribute__((weak));
extern "C" const char* ndk_dalvik_dlerror(void) __attribute__((weak));

namespace ndk {
// The libandroid types guest code holds pointers to: host objects as they
// are, for both ABIs (AConfiguration is ResTable_config: fixed-size fields).
template <> struct Opaque64<AInputEvent> : std::true_type {};
template <> struct Opaque64<AInputQueue> : std::true_type {};
template <> struct Opaque64<AConfiguration> : std::true_type {};
template <> struct Opaque64<AAssetManager> : std::true_type {};
}  // namespace ndk

namespace android {
namespace {

struct {
  jmethodID finish;
  jmethodID setWindowFlags;
  jmethodID setWindowFormat;
  jmethodID showIme;
  jmethodID hideIme;
} gNativeActivityClassInfo;

// The entries of ANativeActivityCallbacks, in order.
enum Callback {
  kOnStart, kOnResume, kOnSaveInstanceState, kOnPause, kOnStop, kOnDestroy,
  kOnWindowFocusChanged, kOnNativeWindowCreated, kOnNativeWindowResized,
  kOnNativeWindowRedrawNeeded, kOnNativeWindowDestroyed, kOnInputQueueCreated,
  kOnInputQueueDestroyed, kOnContentRectChanged, kOnConfigurationChanged, kOnLowMemory,
  kNumCallbacks
};
const char* const kCallbackNames[kNumCallbacks] = {
    "onStart", "onResume", "onSaveInstanceState", "onPause", "onStop", "onDestroy",
    "onWindowFocusChanged", "onNativeWindowCreated", "onNativeWindowResized",
    "onNativeWindowRedrawNeeded", "onNativeWindowDestroyed", "onInputQueueCreated",
    "onInputQueueDestroyed", "onContentRectChanged", "onConfigurationChanged", "onLowMemory"};

// ANativeActivity as an armeabi(-v7a) guest sees it (and the host: wasm32
// has the same 4-byte pointers) and as an arm64-v8a one.
struct GuestActivity32 {
  uint32_t callbacks, vm, env, clazz, internal_data_path, external_data_path;
  int32_t sdk_version;
  uint32_t instance, asset_manager, obb_path;
};
static_assert(sizeof(GuestActivity32) == sizeof(ANativeActivity), "ANativeActivity");
struct GuestActivity64 {
  uint64_t callbacks, vm, env, clazz, internal_data_path, external_data_path;
  int32_t sdk_version;
  uint32_t pad;
  uint64_t instance, asset_manager, obb_path;
};
static_assert(sizeof(GuestActivity64) == 80, "ANativeActivity on LP64");

bool Lenient() {
  static int lenient = -1;
  if (lenient < 0) {
    char value[92];
    property_get("arcwasm.server.lenient", value, "0");
    lenient = value[0] == '1';
  }
  return lenient == 1;
}

const char* Name(uint32_t addr, char* buf, size_t size) {
  uint32_t off = 0;
  const char* sym = ndk::Symbolize(addr, &off);
  if (!sym && ndk::Symbolize64)
    sym = ndk::Symbolize64(addr, &off);
  snprintf(buf, size, "%s+0x%x", sym ? sym : "?", off);
  return buf;
}

struct ActivityWork {
  int32_t cmd;
  int32_t arg1;
  int32_t arg2;
};

enum {
  CMD_FINISH = 1,
  CMD_SET_WINDOW_FORMAT,
  CMD_SET_WINDOW_FLAGS,
  CMD_SHOW_SOFT_INPUT,
  CMD_HIDE_SOFT_INPUT,
};

class NativeCode;

// What the guest's ANativeActivity* points to: the structure for its ABI,
// then (beyond what the guest reads) the NativeCode it belongs to.
struct GuestBlock {
  union {
    GuestActivity32 a32;
    GuestActivity64 a64;
  };
  NativeCode* owner;
};

// The native state of one NativeActivity.
class NativeCode {
 public:
  NativeCode(bool arm64, uint32_t create) : arm64_(arm64), create_(create) {
    memset(&guest_, 0, sizeof guest_);
    guest_.owner = this;
    memset(cb32_, 0, sizeof cb32_);
    memset(cb64_, 0, sizeof cb64_);
  }

  ~NativeCode() {
    Call(kOnDestroy, {});
    if (env_ != NULL) {
      if (clazz_ != NULL)
        env_->DeleteGlobalRef(clazz_);
      if (java_asset_manager_ != NULL)
        env_->DeleteGlobalRef(java_asset_manager_);
    }
    if (message_queue_ != NULL && main_work_read_ >= 0)
      message_queue_->getLooper()->removeFd(main_work_read_);
    window_ = NULL;
    if (main_work_read_ >= 0)
      close(main_work_read_);
    if (main_work_write_ >= 0)
      close(main_work_write_);
    // (The library stays loaded, as on a device.)
  }

  // Fills the guest's ANativeActivity.
  void Init(JNIEnv* env, jobject clazz, jobject assets, int32_t sdk) {
    env_ = env;
    clazz_ = env->NewGlobalRef(clazz);
    java_asset_manager_ = env->NewGlobalRef(assets);
    // (the AAssetManager* the bridge's AAsset_* take: ndk_android_fw.cc's)
    void* asset_manager = ndkfw_asset_manager_from_java(env, assets);
    JavaVM* vm = NULL;
    env->GetJavaVM(&vm);
    if (arm64_) {
      GuestActivity64* a = &guest_.a64;
      a->callbacks = ndk::Addr(cb64_);
      a->vm = ndk::Addr(ndk::GuestVmFor64(vm));
      a->env = ndk::Addr(ndk::GuestEnvFor64(env));
      a->clazz = ndk::Addr(clazz_);
      a->internal_data_path = ndk::Addr(internal_.string());
      a->external_data_path = ndk::Addr(external_.string());
      a->sdk_version = sdk;
      a->asset_manager = ndk::Addr(asset_manager);
      a->obb_path = ndk::Addr(obb_.string());
    } else {
      GuestActivity32* a = &guest_.a32;
      a->callbacks = ndk::Addr(cb32_);
      a->vm = ndk::Addr(ndk::GuestVmFor(vm));
      a->env = ndk::Addr(ndk::GuestEnvFor(env));
      a->clazz = ndk::Addr(clazz_);
      a->internal_data_path = ndk::Addr(internal_.string());
      a->external_data_path = ndk::Addr(external_.string());
      a->sdk_version = sdk;
      a->asset_manager = ndk::Addr(asset_manager);
      a->obb_path = ndk::Addr(obb_.string());
    }
  }

  // Runs guest function |fn|(this, |args|...) on this thread. A fatal error
  // of the guest ends the VM, or in lenient mode only this call (with a
  // Java exception, as ndk_dalvik.cc's native methods). False then.
  bool CallFn(uint32_t fn, std::initializer_list<uint32_t> args, uint64_t* result,
              const char* what) {
    char why[256];
    bool ok = true;
    uint64_t r = 0;
    if (arm64_) {
      Ndk64Args a;
      memset(&a, 0, sizeof a);
      int n = 0;
      a.x[n++] = ndk::Addr(&guest_);
      for (uint32_t v : args)
        a.x[n++] = v;
      Ndk64Result res;
      if (Lenient()) {
        ok = ndk::CallGuest64Guarded(fn, &a, &res, why, sizeof why);
      } else {
        ndk::CallGuest64Args(fn, &a, &res);
      }
      r = res.x0;
    } else {
      uint32_t words[8];
      int n = 0;
      words[n++] = ndk::Addr(&guest_);
      for (uint32_t v : args)
        words[n++] = v;
      if (Lenient()) {
        ok = ndk::CallGuestGuarded(fn, words, n, &r, why, sizeof why);
      } else {
        r = ndk::CallGuest(fn, words, n);
      }
    }
    if (!ok) {
      char buf[256], msg[600];
      snprintf(msg, sizeof msg, "native code of the app ended in %s (%s): %s",
               Name(fn, buf, sizeof buf), what, why);
      ALOGE("%s", msg);
      if (env_ != NULL && !env_->ExceptionCheck())
        env_->ThrowNew(env_->FindClass("java/lang/RuntimeException"), msg);
    }
    if (result)
      *result = r;
    return ok;
  }

  // The guest's callback |which|, if it set one.
  uint32_t CallbackFn(Callback which) const {
    return arm64_ ? static_cast<uint32_t>(cb64_[which]) : cb32_[which];
  }
  bool Call(Callback which, std::initializer_list<uint32_t> args, uint64_t* result = NULL) {
    uint32_t fn = CallbackFn(which);
    if (fn == 0)
      return false;
    if (ndk::TraceLevel() >= 1)
      ALOGI("-> %s", kCallbackNames[which]);
    CallFn(fn, args, result, kCallbackNames[which]);
    return true;
  }

  void SetSurface(jobject surface) {
    window_ = surface != NULL ? android_view_Surface_getNativeWindow(env_, surface) : NULL;
  }

  bool arm64_;
  uint32_t create_;  // the guest's ANativeActivity_onCreate
  GuestBlock guest_;
  uint32_t cb32_[kNumCallbacks];
  uint64_t cb64_[kNumCallbacks];

  JNIEnv* env_ = NULL;
  jobject clazz_ = NULL;
  jobject java_asset_manager_ = NULL;
  String8 internal_, external_, obb_;
  sp<ANativeWindow> window_;
  int32_t last_width_ = 0, last_height_ = 0;
  // Wake the main thread for ANativeActivity_finish and the like.
  int main_work_read_ = -1;
  int main_work_write_ = -1;
  sp<MessageQueue> message_queue_;
};

NativeCode* CodeOf(void* activity) { return static_cast<GuestBlock*>(activity)->owner; }

void write_work(int fd, int32_t cmd, int32_t arg1 = 0, int32_t arg2 = 0) {
  ActivityWork work = {cmd, arg1, arg2};
  int res;
  do {
    res = write(fd, &work, sizeof(work));
  } while (res < 0 && errno == EINTR);
  if (res == sizeof(work))
    return;
  if (res < 0)
    ALOGW("Failed writing to work fd: %s", strerror(errno));
  else
    ALOGW("Truncated writing to work fd: %d", res);
}

int mainWorkCallback(int fd, int events, void* data) {
  NativeCode* code = static_cast<NativeCode*>(data);
  if ((events & POLLIN) == 0)
    return 1;
  ActivityWork work;
  int res = read(code->main_work_read_, &work, sizeof(work));
  if (res != sizeof(work)) {
    ALOGW("Failed reading work fd: %d", res);
    return 1;
  }
  JNIEnv* env = code->env_;
  switch (work.cmd) {
    case CMD_FINISH:
      env->CallVoidMethod(code->clazz_, gNativeActivityClassInfo.finish);
      code->message_queue_->raiseAndClearException(env, "finish");
      break;
    case CMD_SET_WINDOW_FORMAT:
      env->CallVoidMethod(code->clazz_, gNativeActivityClassInfo.setWindowFormat, work.arg1);
      code->message_queue_->raiseAndClearException(env, "setWindowFormat");
      break;
    case CMD_SET_WINDOW_FLAGS:
      env->CallVoidMethod(code->clazz_, gNativeActivityClassInfo.setWindowFlags, work.arg1,
                          work.arg2);
      code->message_queue_->raiseAndClearException(env, "setWindowFlags");
      break;
    case CMD_SHOW_SOFT_INPUT:
      env->CallVoidMethod(code->clazz_, gNativeActivityClassInfo.showIme, work.arg1);
      code->message_queue_->raiseAndClearException(env, "showIme");
      break;
    case CMD_HIDE_SOFT_INPUT:
      env->CallVoidMethod(code->clazz_, gNativeActivityClassInfo.hideIme, work.arg1);
      code->message_queue_->raiseAndClearException(env, "hideIme");
      break;
    default:
      ALOGW("Unknown work command: %d", work.cmd);
      break;
  }
  return 1;
}

thread_local std::string g_dlerror;

void SetString(JNIEnv* env, jstring s, String8* out) {
  if (s == NULL)
    return;
  const char* chars = env->GetStringUTFChars(s, NULL);
  *out = chars;
  env->ReleaseStringUTFChars(s, chars);
}

jlong loadNativeCode_native(JNIEnv* env, jobject clazz, jstring path, jstring funcName,
                            jobject messageQueue, jstring internalDataDir, jstring obbDir,
                            jstring externalDataDir, jint sdkVersion, jobject jAssetMgr,
                            jbyteArray savedState, jobject classLoader, jstring libraryPath) {
  String8 lib, func;
  SetString(env, path, &lib);
  SetString(env, funcName, &func);
  void* handle = NULL;
  if (!ndk_dalvik_dlopen || !ndk_dalvik_dlopen(lib.string(), RTLD_NOW, &handle)) {
    g_dlerror = std::string(lib.string()) + ": not an ARM or AArch64 library";
    ALOGW("NativeActivity LoadNativeLibrary(\"%s\") failed: %s", lib.string(), g_dlerror.c_str());
    return 0;
  }
  if (handle == NULL) {
    const char* e = ndk_dalvik_dlerror ? ndk_dalvik_dlerror() : NULL;
    g_dlerror = e ? e : "dlopen failed";
    ALOGW("NativeActivity LoadNativeLibrary(\"%s\") failed: %s", lib.string(), g_dlerror.c_str());
    return 0;
  }
  bool arm64 = ndk::IsLibrary64 && ndk::IsLibrary64(handle);
  uint32_t create = arm64 ? ndk::Dlsym64(static_cast<ndk::Library64*>(handle), func.string())
                          : ndk::Dlsym(static_cast<ndk::Library*>(handle), func.string());
  if (create == 0) {
    g_dlerror = std::string(func.string()) + " not found in " + lib.string();
    ALOGW("%s", g_dlerror.c_str());
    return 0;
  }
  std::unique_ptr<NativeCode> code(new NativeCode(arm64, create));

  code->message_queue_ = android_os_MessageQueue_getMessageQueue(env, messageQueue);
  if (code->message_queue_ == NULL) {
    ALOGW("Unable to retrieve native MessageQueue");
    return 0;
  }
  int msgpipe[2];
  if (pipe(msgpipe)) {
    ALOGW("could not create pipe: %s", strerror(errno));
    return 0;
  }
  code->main_work_read_ = msgpipe[0];
  code->main_work_write_ = msgpipe[1];
  fcntl(code->main_work_read_, F_SETFL, O_NONBLOCK);
  fcntl(code->main_work_write_, F_SETFL, O_NONBLOCK);
  code->message_queue_->getLooper()->addFd(code->main_work_read_, 0, ALOOPER_EVENT_INPUT,
                                           mainWorkCallback, code.get());

  SetString(env, internalDataDir, &code->internal_);
  SetString(env, externalDataDir, &code->external_);
  SetString(env, obbDir, &code->obb_);
  code->Init(env, clazz, jAssetMgr, sdkVersion);

  jbyte* raw = NULL;
  jsize size = 0;
  if (savedState != NULL) {
    raw = env->GetByteArrayElements(savedState, NULL);
    size = env->GetArrayLength(savedState);
  }
  char buf[256];
  ALOGI("%s: %s (%s)", lib.string(), Name(create, buf, sizeof buf), arm64 ? "arm64-v8a" : "arm");
  bool ok = code->CallFn(create, {ndk::Addr(raw), static_cast<uint32_t>(size)}, NULL,
                         func.string());
  if (raw != NULL)
    env->ReleaseByteArrayElements(savedState, raw, 0);
  if (!ok)
    return 0;
  return reinterpret_cast<jlong>(code.release());
}

#if NA_FW6
// Android 6.0's NativeActivity passes no class loader or library path.
jlong loadNativeCode6_native(JNIEnv* env, jobject clazz, jstring path, jstring funcName,
                             jobject messageQueue, jstring internalDataDir, jstring obbDir,
                             jstring externalDataDir, jint sdkVersion, jobject jAssetMgr,
                             jbyteArray savedState) {
  return loadNativeCode_native(env, clazz, path, funcName, messageQueue, internalDataDir, obbDir,
                               externalDataDir, sdkVersion, jAssetMgr, savedState, NULL, NULL);
}
#endif

jstring getDlError_native(JNIEnv* env, jobject) {
  return env->NewStringUTF(g_dlerror.c_str());
}

void unloadNativeCode_native(JNIEnv*, jobject, jlong handle) {
  delete reinterpret_cast<NativeCode*>(handle);
}

#define SIMPLE(name, cb)                                         \
  void name(JNIEnv*, jobject, jlong handle) {                    \
    if (handle != 0)                                             \
      reinterpret_cast<NativeCode*>(handle)->Call(cb, {});       \
  }
SIMPLE(onStart_native, kOnStart)
SIMPLE(onResume_native, kOnResume)
SIMPLE(onPause_native, kOnPause)
SIMPLE(onStop_native, kOnStop)
SIMPLE(onConfigurationChanged_native, kOnConfigurationChanged)
SIMPLE(onLowMemory_native, kOnLowMemory)
#undef SIMPLE

jbyteArray onSaveInstanceState_native(JNIEnv* env, jobject, jlong handle) {
  if (handle == 0)
    return NULL;
  NativeCode* code = reinterpret_cast<NativeCode*>(handle);
  uint64_t len = 0;  // size_t* of either ABI (little-endian: the low word first)
  uint64_t state = 0;
  if (!code->Call(kOnSaveInstanceState, {ndk::Addr(&len)}, &state))
    return NULL;
  size_t n = code->arm64_ ? static_cast<size_t>(len) : static_cast<uint32_t>(len);
  jbyteArray array = NULL;
  void* p = code->arm64_ ? ndk::Ptr<void>(state) : ndk::Ptr<void>(static_cast<uint32_t>(state));
  if (n > 0 && p != NULL) {
    array = env->NewByteArray(n);
    if (array != NULL)
      env->SetByteArrayRegion(array, 0, n, static_cast<jbyte*>(p));
  }
  free(p);  // the guest's malloc is the host's
  return array;
}

void onWindowFocusChanged_native(JNIEnv*, jobject, jlong handle, jboolean focused) {
  if (handle != 0)
    reinterpret_cast<NativeCode*>(handle)->Call(kOnWindowFocusChanged, {focused ? 1u : 0u});
}

int32_t getWindowProp(ANativeWindow* window, int what) {
  int value;
  int res = window->query(window, what, &value);
  return res < 0 ? res : value;
}

void onSurfaceCreated_native(JNIEnv*, jobject, jlong handle, jobject surface) {
  if (handle == 0)
    return;
  NativeCode* code = reinterpret_cast<NativeCode*>(handle);
  code->SetSurface(surface);
  if (code->window_ != NULL) {
    code->last_width_ = getWindowProp(code->window_.get(), NATIVE_WINDOW_WIDTH);
    code->last_height_ = getWindowProp(code->window_.get(), NATIVE_WINDOW_HEIGHT);
    ALOGI("native window %p %dx%d", code->window_.get(), code->last_width_, code->last_height_);
    code->Call(kOnNativeWindowCreated, {ndk::Addr(code->window_.get())});
  }
}

void onSurfaceChanged_native(JNIEnv*, jobject, jlong handle, jobject surface, jint, jint, jint) {
  if (handle == 0)
    return;
  NativeCode* code = reinterpret_cast<NativeCode*>(handle);
  sp<ANativeWindow> old = code->window_;
  code->SetSurface(surface);
  if (old != code->window_) {
    if (old != NULL)
      code->Call(kOnNativeWindowDestroyed, {ndk::Addr(old.get())});
    if (code->window_ != NULL) {
      code->Call(kOnNativeWindowCreated, {ndk::Addr(code->window_.get())});
      code->last_width_ = getWindowProp(code->window_.get(), NATIVE_WINDOW_WIDTH);
      code->last_height_ = getWindowProp(code->window_.get(), NATIVE_WINDOW_HEIGHT);
    }
  } else if (code->window_ != NULL) {
    int32_t w = getWindowProp(code->window_.get(), NATIVE_WINDOW_WIDTH);
    int32_t h = getWindowProp(code->window_.get(), NATIVE_WINDOW_HEIGHT);
    if (w != code->last_width_ || h != code->last_height_) {
      code->last_width_ = w;
      code->last_height_ = h;
      code->Call(kOnNativeWindowResized, {ndk::Addr(code->window_.get())});
    }
  }
}

void onSurfaceRedrawNeeded_native(JNIEnv*, jobject, jlong handle) {
  if (handle == 0)
    return;
  NativeCode* code = reinterpret_cast<NativeCode*>(handle);
  if (code->window_ != NULL)
    code->Call(kOnNativeWindowRedrawNeeded, {ndk::Addr(code->window_.get())});
}

void onSurfaceDestroyed_native(JNIEnv*, jobject, jlong handle) {
  if (handle == 0)
    return;
  NativeCode* code = reinterpret_cast<NativeCode*>(handle);
  if (code->window_ != NULL)
    code->Call(kOnNativeWindowDestroyed, {ndk::Addr(code->window_.get())});
  code->SetSurface(NULL);
}

// The AInputQueue* of a device is the InputQueue* (its first, empty base).
uint32_t GuestQueue(jlong ptr) {
  return ndk::Addr(static_cast<AInputQueue*>(reinterpret_cast<InputQueue*>(ptr)));
}

void onInputQueueCreated_native(JNIEnv*, jobject, jlong handle, jlong queuePtr) {
  if (handle != 0)
    reinterpret_cast<NativeCode*>(handle)->Call(kOnInputQueueCreated, {GuestQueue(queuePtr)});
}

void onInputQueueDestroyed_native(JNIEnv*, jobject, jlong handle, jlong queuePtr) {
  if (handle != 0)
    reinterpret_cast<NativeCode*>(handle)->Call(kOnInputQueueDestroyed, {GuestQueue(queuePtr)});
}

void onContentRectChanged_native(JNIEnv*, jobject, jlong handle, jint x, jint y, jint w, jint h) {
  if (handle == 0)
    return;
  ARect rect = {x, y, x + w, y + h};
  reinterpret_cast<NativeCode*>(handle)->Call(kOnContentRectChanged, {ndk::Addr(&rect)});
}

const JNINativeMethod g_methods[] = {
#if NA_FW6
    {"loadNativeCode",
     "(Ljava/lang/String;Ljava/lang/String;Landroid/os/MessageQueue;Ljava/lang/String;Ljava/lang/"
     "String;Ljava/lang/String;ILandroid/content/res/AssetManager;[B)J",
     (void*)loadNativeCode6_native},
#else
    {"loadNativeCode",
     "(Ljava/lang/String;Ljava/lang/String;Landroid/os/MessageQueue;Ljava/lang/String;Ljava/lang/"
     "String;Ljava/lang/String;ILandroid/content/res/AssetManager;[BLjava/lang/ClassLoader;Ljava/"
     "lang/String;)J",
     (void*)loadNativeCode_native},
    {"getDlError", "()Ljava/lang/String;", (void*)getDlError_native},
#endif
    {"unloadNativeCode", "(J)V", (void*)unloadNativeCode_native},
    {"onStartNative", "(J)V", (void*)onStart_native},
    {"onResumeNative", "(J)V", (void*)onResume_native},
    {"onSaveInstanceStateNative", "(J)[B", (void*)onSaveInstanceState_native},
    {"onPauseNative", "(J)V", (void*)onPause_native},
    {"onStopNative", "(J)V", (void*)onStop_native},
    {"onConfigurationChangedNative", "(J)V", (void*)onConfigurationChanged_native},
    {"onLowMemoryNative", "(J)V", (void*)onLowMemory_native},
    {"onWindowFocusChangedNative", "(JZ)V", (void*)onWindowFocusChanged_native},
    {"onSurfaceCreatedNative", "(JLandroid/view/Surface;)V", (void*)onSurfaceCreated_native},
    {"onSurfaceChangedNative", "(JLandroid/view/Surface;III)V", (void*)onSurfaceChanged_native},
    {"onSurfaceRedrawNeededNative", "(JLandroid/view/Surface;)V",
     (void*)onSurfaceRedrawNeeded_native},
    {"onSurfaceDestroyedNative", "(J)V", (void*)onSurfaceDestroyed_native},
    {"onInputQueueCreatedNative", "(JJ)V", (void*)onInputQueueCreated_native},
    {"onInputQueueDestroyedNative", "(JJ)V", (void*)onInputQueueDestroyed_native},
    {"onContentRectChangedNative", "(JIIII)V", (void*)onContentRectChanged_native},
};

// --- libandroid for the guest: native_activity.h --------------------------------------

void G_ANativeActivity_finish(void* activity) {
  write_work(CodeOf(activity)->main_work_write_, CMD_FINISH);
}
void G_ANativeActivity_setWindowFormat(void* activity, int32_t format) {
  write_work(CodeOf(activity)->main_work_write_, CMD_SET_WINDOW_FORMAT, format);
}
void G_ANativeActivity_setWindowFlags(void* activity, uint32_t add, uint32_t remove) {
  // (NativeActivity.setWindowFlags(flags, mask): the flags to add, the ones changed)
  write_work(CodeOf(activity)->main_work_write_, CMD_SET_WINDOW_FLAGS, add, add | remove);
}
void G_ANativeActivity_showSoftInput(void* activity, uint32_t flags) {
  write_work(CodeOf(activity)->main_work_write_, CMD_SHOW_SOFT_INPUT, flags);
}
void G_ANativeActivity_hideSoftInput(void* activity, uint32_t flags) {
  write_work(CodeOf(activity)->main_work_write_, CMD_HIDE_SOFT_INPUT, flags);
}

// --- input.h: AInputQueue with a guest callback ------------------------------------------

// A guest's ALooper_callbackFunc and its data, for InputQueue::attachLooper.
struct GuestLooperCallback {
  uint32_t fn;
  uint32_t data;
  bool arm64;
};
int CallGuestLooperCallback(int fd, int events, void* data) {
  GuestLooperCallback* cb = static_cast<GuestLooperCallback*>(data);
  if (cb->arm64) {
    uint64_t args[3] = {(uint64_t)(uint32_t)fd, (uint64_t)(uint32_t)events, cb->data};
    return (int32_t)ndk::CallGuest64(cb->fn, args, 3);
  }
  uint32_t args[3] = {(uint32_t)fd, (uint32_t)events, cb->data};
  return (int32_t)ndk::CallGuest(cb->fn, args, 3);
}
void AttachLooper(AInputQueue* queue, void* looper, int ident, uint32_t callback, void* data,
                  bool arm64) {
  InputQueue* iq = static_cast<InputQueue*>(queue);
  Looper* l = static_cast<Looper*>(looper);
  if (callback == 0) {
    iq->attachLooper(l, ident, NULL, data);
  } else {
    // (kept: the looper may call it until detachLooper, which does not say)
    iq->attachLooper(l, ident, CallGuestLooperCallback,
                     new GuestLooperCallback{callback, ndk::Addr(data), arm64});
  }
}
void G_AInputQueue_attachLooper(AInputQueue* queue, void* looper, int ident, uint32_t callback,
                                void* data) {
  AttachLooper(queue, looper, ident, callback, data, false);
}
void G64_AInputQueue_attachLooper(AInputQueue* queue, void* looper, int ident,
                                  uint32_t callback, void* data) {
  AttachLooper(queue, looper, ident, callback, data, true);
}
int32_t G64_AInputQueue_getEvent(AInputQueue* queue, uint64_t* out) {
  AInputEvent* e = NULL;
  int32_t r = AInputQueue_getEvent(queue, &e);
  *out = ndk::Addr(e);
  return r;
}

}  // namespace
}  // namespace android

namespace ndk {
namespace {

using namespace android;

// (AInputQueue_attachLooper and _getEvent aside, AOSP's libandroid
// functions as they are: their types mean the same to both ABIs.)
#define NA_FN(name) NDK_FNX(#name, ::name)
const HostSymbol kNativeActivity[] = {
    NDK_FNX("ANativeActivity_finish", G_ANativeActivity_finish),
    NDK_FNX("ANativeActivity_setWindowFormat", G_ANativeActivity_setWindowFormat),
    NDK_FNX("ANativeActivity_setWindowFlags", G_ANativeActivity_setWindowFlags),
    NDK_FNX("ANativeActivity_showSoftInput", G_ANativeActivity_showSoftInput),
    NDK_FNX("ANativeActivity_hideSoftInput", G_ANativeActivity_hideSoftInput),
    // input.h
    NDK_FN("AInputQueue_attachLooper", G_AInputQueue_attachLooper),
    NDK_FN64("AInputQueue_attachLooper", G64_AInputQueue_attachLooper),
    NDK_FN("AInputQueue_getEvent", ::AInputQueue_getEvent),
    NDK_FN64("AInputQueue_getEvent", G64_AInputQueue_getEvent),
    NA_FN(AInputQueue_detachLooper), NA_FN(AInputQueue_hasEvents),
    NA_FN(AInputQueue_preDispatchEvent), NA_FN(AInputQueue_finishEvent),
    NA_FN(AInputEvent_getType), NA_FN(AInputEvent_getDeviceId), NA_FN(AInputEvent_getSource),
    NA_FN(AKeyEvent_getAction), NA_FN(AKeyEvent_getFlags), NA_FN(AKeyEvent_getKeyCode),
    NA_FN(AKeyEvent_getScanCode), NA_FN(AKeyEvent_getMetaState), NA_FN(AKeyEvent_getRepeatCount),
    NA_FN(AKeyEvent_getDownTime), NA_FN(AKeyEvent_getEventTime),
    NA_FN(AMotionEvent_getAction), NA_FN(AMotionEvent_getFlags), NA_FN(AMotionEvent_getMetaState),
    NA_FN(AMotionEvent_getButtonState), NA_FN(AMotionEvent_getEdgeFlags),
    NA_FN(AMotionEvent_getDownTime), NA_FN(AMotionEvent_getEventTime),
    NA_FN(AMotionEvent_getXOffset), NA_FN(AMotionEvent_getYOffset),
    NA_FN(AMotionEvent_getXPrecision), NA_FN(AMotionEvent_getYPrecision),
    NA_FN(AMotionEvent_getPointerCount), NA_FN(AMotionEvent_getPointerId),
    NA_FN(AMotionEvent_getToolType), NA_FN(AMotionEvent_getRawX), NA_FN(AMotionEvent_getRawY),
    NA_FN(AMotionEvent_getX), NA_FN(AMotionEvent_getY), NA_FN(AMotionEvent_getPressure),
    NA_FN(AMotionEvent_getSize), NA_FN(AMotionEvent_getTouchMajor),
    NA_FN(AMotionEvent_getTouchMinor), NA_FN(AMotionEvent_getToolMajor),
    NA_FN(AMotionEvent_getToolMinor), NA_FN(AMotionEvent_getOrientation),
    NA_FN(AMotionEvent_getAxisValue), NA_FN(AMotionEvent_getHistorySize),
    NA_FN(AMotionEvent_getHistoricalEventTime), NA_FN(AMotionEvent_getHistoricalRawX),
    NA_FN(AMotionEvent_getHistoricalRawY), NA_FN(AMotionEvent_getHistoricalX),
    NA_FN(AMotionEvent_getHistoricalY), NA_FN(AMotionEvent_getHistoricalPressure),
    NA_FN(AMotionEvent_getHistoricalSize), NA_FN(AMotionEvent_getHistoricalTouchMajor),
    NA_FN(AMotionEvent_getHistoricalTouchMinor), NA_FN(AMotionEvent_getHistoricalToolMajor),
    NA_FN(AMotionEvent_getHistoricalToolMinor), NA_FN(AMotionEvent_getHistoricalOrientation),
    NA_FN(AMotionEvent_getHistoricalAxisValue),
    // configuration.h
    NA_FN(AConfiguration_new), NA_FN(AConfiguration_delete), NA_FN(AConfiguration_fromAssetManager),
    NA_FN(AConfiguration_copy), NA_FN(AConfiguration_getMcc), NA_FN(AConfiguration_getMnc),
    NA_FN(AConfiguration_getLanguage), NA_FN(AConfiguration_getCountry),
    NA_FN(AConfiguration_getOrientation), NA_FN(AConfiguration_getTouchscreen),
    NA_FN(AConfiguration_getDensity), NA_FN(AConfiguration_getKeyboard),
    NA_FN(AConfiguration_getNavigation), NA_FN(AConfiguration_getKeysHidden),
    NA_FN(AConfiguration_getNavHidden), NA_FN(AConfiguration_getSdkVersion),
    NA_FN(AConfiguration_getScreenSize), NA_FN(AConfiguration_getScreenLong),
    NA_FN(AConfiguration_getScreenRound), NA_FN(AConfiguration_getUiModeType),
    NA_FN(AConfiguration_getUiModeNight), NA_FN(AConfiguration_getScreenWidthDp),
    NA_FN(AConfiguration_getScreenHeightDp), NA_FN(AConfiguration_getSmallestScreenWidthDp),
    NA_FN(AConfiguration_getLayoutDirection), NA_FN(AConfiguration_setMcc),
    NA_FN(AConfiguration_setMnc), NA_FN(AConfiguration_setLanguage),
    NA_FN(AConfiguration_setCountry), NA_FN(AConfiguration_setOrientation),
    NA_FN(AConfiguration_setTouchscreen), NA_FN(AConfiguration_setDensity),
    NA_FN(AConfiguration_setKeyboard), NA_FN(AConfiguration_setNavigation),
    NA_FN(AConfiguration_setKeysHidden), NA_FN(AConfiguration_setNavHidden),
    NA_FN(AConfiguration_setSdkVersion), NA_FN(AConfiguration_setScreenSize),
    NA_FN(AConfiguration_setScreenLong), NA_FN(AConfiguration_setScreenRound),
    NA_FN(AConfiguration_setUiModeType), NA_FN(AConfiguration_setUiModeNight),
    NA_FN(AConfiguration_setScreenWidthDp), NA_FN(AConfiguration_setScreenHeightDp),
    NA_FN(AConfiguration_setSmallestScreenWidthDp), NA_FN(AConfiguration_setLayoutDirection),
    NA_FN(AConfiguration_diff), NA_FN(AConfiguration_match), NA_FN(AConfiguration_isBetterThan),
};
#undef NA_FN

void RegisterNativeActivity() { RegisterHostSymbols("libandroid.so", kNativeActivity); }

}  // namespace
}  // namespace ndk

NDK_HOST_LIBRARY(native_activity, 10, ::ndk::RegisterNativeActivity)

namespace android {

int register_android_app_NativeActivity(JNIEnv* env) {
  const char* const kName = "android/app/NativeActivity";
  jclass clazz = env->FindClass(kName);
  LOG_ALWAYS_FATAL_IF(clazz == NULL, "no class %s", kName);
  gNativeActivityClassInfo.finish = env->GetMethodID(clazz, "finish", "()V");
  gNativeActivityClassInfo.setWindowFlags = env->GetMethodID(clazz, "setWindowFlags", "(II)V");
  gNativeActivityClassInfo.setWindowFormat = env->GetMethodID(clazz, "setWindowFormat", "(I)V");
  gNativeActivityClassInfo.showIme = env->GetMethodID(clazz, "showIme", "(I)V");
  gNativeActivityClassInfo.hideIme = env->GetMethodID(clazz, "hideIme", "(I)V");
  LOG_ALWAYS_FATAL_IF(!gNativeActivityClassInfo.finish || !gNativeActivityClassInfo.hideIme,
                      "NativeActivity's methods");
  return env->RegisterNatives(clazz, g_methods, sizeof g_methods / sizeof g_methods[0]);
}

}  // namespace android

// Keeps this file in the link (-Wl,-u,ndk_native_activity_linked).
extern "C" int ndk_native_activity_linked = 1;
