// The VM side of the host WebView's channel to the page (topic webview8,
// docs/topics/webview8.md): the natives of arcwasm.webview.HostBridge, in
// the static JNI library libarcwasm_webview.so (System.load of
// /system/lib/libarcwasm_webview.so finds it in the VM's table).
//
//   VM -> page   a JSON command (UTF-8) is handed to Module.arcWebView.command
//                on the page's main thread (emscripten_async_run_in_main_
//                runtime_thread: queued, the VM thread does not wait)
//   page -> VM   the page writes events into a mailbox in the wasm memory and
//                bumps its first word; nativeWait sleeps on that word
//                (memory.atomic.wait32, woken by the page's Atomics.notify).
//                A @JavascriptInterface call comes through the mailbox's
//                sync slot while the page's thread spins for the answer
//                (nativeReply).
//
// The mailbox's layout is shim/webview/webview_host.js's (its header
// comment). Nothing here is Android-version specific: the same file serves
// ART 10's WebView later (its own static-library table).

#include <emscripten.h>
#include <emscripten/atomic.h>
#include <emscripten/threading.h>
#include <jni.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

enum {
  kSeq = 0, kLock = 1, kUsed = 2, kCap = 3, kSyncState = 4, kSyncLen = 5, kSyncCap = 6,
  kPagePending = 7, kHeaderWords = 8,
};
enum { kSyncIdle = 0, kSyncAsked = 1, kSyncAnswered = 2, kSyncTaken = 3, kSyncAbandoned = 4 };

const int kEventBytes = 256 << 10;
const int kSyncBytes = 256 << 10;

int32_t* g_box;

uint8_t* Events() { return reinterpret_cast<uint8_t*>(g_box + kHeaderWords); }
uint8_t* SyncSlot() { return Events() + g_box[kCap]; }

void Lock() {
  while (!__sync_bool_compare_and_swap(&g_box[kLock], 0, 1)) {
  }
}
void Unlock() { __atomic_store_n(&g_box[kLock], 0, __ATOMIC_SEQ_CST); }

// On the page's main thread.
void RunCommand(char* json) {
  EM_ASM({
    var api = Module['arcWebView'];
    if (api) api.command(UTF8ToString($0));
  }, json);
  free(json);
}

void Send(const char* bytes, size_t n) {
  char* copy = static_cast<char*>(malloc(n + 1));
  if (!copy) return;
  memcpy(copy, bytes, n);
  copy[n] = 0;
  emscripten_async_run_in_main_runtime_thread(EM_FUNC_SIG_VI, reinterpret_cast<void*>(RunCommand),
                                              copy);
}

jboolean Available(JNIEnv*, jclass) {
  // A page with the host (webview_host.js, a --pre-js of the page builds);
  // Node and pages without it have none.
  return MAIN_THREAD_EM_ASM_INT({
    return (typeof document == 'object' && Module['arcWebView']) ? 1 : 0;
  }) ? JNI_TRUE : JNI_FALSE;
}

jboolean Init(JNIEnv*, jclass) {
  if (g_box) return JNI_TRUE;
  size_t size = kHeaderWords * 4 + kEventBytes + kSyncBytes;
  int32_t* box = static_cast<int32_t*>(calloc(1, size));
  if (!box) return JNI_FALSE;
  box[kCap] = kEventBytes;
  box[kSyncCap] = kSyncBytes;
  g_box = box;
  char json[128];
  int n = snprintf(json, sizeof json, "{\"op\":\"init\",\"box\":%u,\"cap\":%d,\"sync\":%d}",
                   static_cast<unsigned>(reinterpret_cast<uintptr_t>(box)), kEventBytes, kSyncBytes);
  Send(json, n);
  return JNI_TRUE;
}

void SendBytes(JNIEnv* env, jclass, jbyteArray json) {
  jsize n = env->GetArrayLength(json);
  jbyte* p = env->GetByteArrayElements(json, nullptr);
  Send(reinterpret_cast<const char*>(p), n);
  env->ReleaseByteArrayElements(json, p, JNI_ABORT);
}

jbyteArray Tagged(JNIEnv* env, char tag, const uint8_t* bytes, int n) {
  jbyteArray out = env->NewByteArray(n + 1);
  if (!out) return nullptr;
  jbyte t = tag;
  env->SetByteArrayRegion(out, 0, 1, &t);
  env->SetByteArrayRegion(out, 1, n, reinterpret_cast<const jbyte*>(bytes));
  return out;
}

// The page's next message: 'S' + a @JavascriptInterface call (answer it with
// nativeReply), 'E' + event lines, or null after |timeout_ms| without one.
jbyteArray Wait(JNIEnv* env, jclass, jint timeout_ms) {
  if (!g_box) return nullptr;
  for (;;) {
    int32_t seq = __atomic_load_n(&g_box[kSeq], __ATOMIC_SEQ_CST);
    if (__sync_bool_compare_and_swap(&g_box[kSyncState], kSyncAsked, kSyncTaken)) {
      int n = g_box[kSyncLen];
      if (n < 0 || n > g_box[kSyncCap]) n = 0;
      return Tagged(env, 'S', SyncSlot(), n);
    }
    uint8_t* copy = nullptr;
    int used = 0, more = 0;
    Lock();
    used = g_box[kUsed];
    if (used > 0) {
      copy = static_cast<uint8_t*>(malloc(used));
      if (copy) {
        memcpy(copy, Events(), used);
        g_box[kUsed] = 0;
        more = g_box[kPagePending];
      } else {
        used = 0;
      }
    }
    Unlock();
    if (used > 0) {
      jbyteArray out = Tagged(env, 'E', copy, used);
      free(copy);
      if (more) {
        static const char kPump[] = "{\"op\":\"pump\"}";
        Send(kPump, sizeof kPump - 1);
      }
      return out;
    }
    int r = emscripten_atomic_wait_u32(&g_box[kSeq], seq,
                                       static_cast<int64_t>(timeout_ms) * 1000000);
    if (r == ATOMICS_WAIT_TIMED_OUT) return nullptr;
  }
}

// The answer to the call nativeWait gave (JSON), for the page that waits.
void Reply(JNIEnv* env, jclass, jbyteArray json) {
  if (!g_box) return;
  jsize n = json ? env->GetArrayLength(json) : 0;
  if (n > g_box[kSyncCap]) n = 0;
  if (n > 0) {
    env->GetByteArrayRegion(json, 0, n, reinterpret_cast<jbyte*>(SyncSlot()));
  } else {
    memcpy(SyncSlot(), "null", 4);
    n = 4;
  }
  g_box[kSyncLen] = n;
  if (!__sync_bool_compare_and_swap(&g_box[kSyncState], kSyncTaken, kSyncAnswered)) {
    // The page gave up waiting.
    __atomic_store_n(&g_box[kSyncState], kSyncIdle, __ATOMIC_SEQ_CST);
  }
}

const JNINativeMethod kHostBridge[] = {
    {"nativeAvailable", "()Z", reinterpret_cast<void*>(Available)},
    {"nativeInit", "()Z", reinterpret_cast<void*>(Init)},
    {"nativeSend", "([B)V", reinterpret_cast<void*>(SendBytes)},
    {"nativeWait", "(I)[B", reinterpret_cast<void*>(Wait)},
    {"nativeReply", "([B)V", reinterpret_cast<void*>(Reply)},
};

}  // namespace

extern "C" int arc_wasm_webview_JNI_OnLoad(JavaVM* vm, void*) {
  JNIEnv* env;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return -1;
  jclass cls = env->FindClass("arcwasm/webview/HostBridge");
  if (!cls) return -1;
  if (env->RegisterNatives(cls, kHostBridge, sizeof kHostBridge / sizeof kHostBridge[0]) < 0)
    return -1;
  env->DeleteLocalRef(cls);
  return JNI_VERSION_1_6;
}

// Into the VM's table of statically linked JNI libraries: ART 8.1's, or
// ART 10's when that is the VM linked.
extern "C" void arc_wasm_art8_add_static_jni_lib(const char* name, int (*on_load)(JavaVM*, void*))
    __attribute__((weak));
extern "C" void arc_wasm_art10_add_static_jni_lib(const char* name, int (*on_load)(JavaVM*, void*))
    __attribute__((weak));
namespace {
struct AddWebViewLib {
  AddWebViewLib() {
    if (arc_wasm_art8_add_static_jni_lib) {
      arc_wasm_art8_add_static_jni_lib("libarcwasm_webview.so", arc_wasm_webview_JNI_OnLoad);
    } else if (arc_wasm_art10_add_static_jni_lib) {
      arc_wasm_art10_add_static_jni_lib("libarcwasm_webview.so", arc_wasm_webview_JNI_OnLoad);
    }
  }
} g_add_webview_lib;
}  // namespace
