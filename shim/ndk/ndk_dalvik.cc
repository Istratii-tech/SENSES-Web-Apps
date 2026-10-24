// Dalvik's side of the NDK bridge: dlopen of ARM libraries for
// System.load/loadLibrary, and calls from Dalvik into native methods
// implemented in guest code (dvmPlatformInvoke hands them over when the
// function address is guest code). Compiled with Dalvik's headers.
// Part of the arc-wasm shim layer.

#include <fcntl.h>
#include <unistd.h>

#include "Dalvik.h"
#include "ndk_bridge.h"
#include "ndk_jni.h"

extern "C" int property_get(const char* key, char* value, const char* default_value);

#undef LOG_TAG
#define LOG_TAG "ndk"

namespace {

bool Tracing() { return ndk::TraceLevel() >= 1; }

// Lenient mode (arcwasm.server.lenient, topic userspace): a fatal error of an
// app's native code (abort(), an unresolved symbol, a CPU fault) ends only
// the native method, with a Java exception; CrashRecovery closes the app.
// Otherwise it aborts the VM, every app with it.
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
  snprintf(buf, size, "%s+0x%x", sym ? sym : "?", off);
  return buf;
}

}  // namespace

extern "C" {

int ndk_dalvik_dlopen(const char* path, int flags, void** handle) {
  // An ARM ELF file? (e_ident, then e_type and e_machine at 16 and 18.)
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return 0;
  unsigned char h[20];
  bool arm = read(fd, h, sizeof h) == (ssize_t)sizeof h && memcmp(h, "\177ELF", 4) == 0 &&
             h[4] == 1 /* ELFCLASS32 */ && (h[18] | h[19] << 8) == 40 /* EM_ARM */;
  close(fd);
  if (!arm)
    return 0;
  *handle = ndk::Dlopen(path, flags);
  return 1;
}

void ndk_update_ld_library_path(const char* path) { ndk::SetLibraryPath(path); }

int ndk_dalvik_is_handle(void* handle) { return ndk::IsLibrary(handle); }

void* ndk_dalvik_dlsym(void* handle, const char* symbol) {
  return ndk::Ptr<void>(ndk::Dlsym(static_cast<ndk::Library*>(handle), symbol));
}

const char* ndk_dalvik_dlerror(void) { return ndk::Dlerror(); }

int ndk_dalvik_jni_onload(void* fn, JavaVM* vm) {
  char buf[256];
  uint32_t args[2] = {ndk::Addr(ndk::GuestVmFor(vm)), 0};
  int r = (int32_t)ndk::CallGuest(ndk::Addr(fn), args, 2);
  ALOGI("%s returned 0x%x", Name(ndk::Addr(fn), buf, sizeof buf), r);
  return r;
}

const char* ndk_method_shorty(jmethodID mid) {
  return reinterpret_cast<const Method*>(mid)->shorty;
}

// dvmPlatformInvoke for a guest function: the AAPCS soft-float arguments
// are JNIEnv*, this-or-class, then Dalvik's argument words, 64-bit values
// on an even word (registers r0-r3, then the stack).
bool arc_ndk_platform_invoke(void* pEnv, ClassObject* clazz, int argInfo, int argc,
                             const u4* argv, const char* shorty, void* func, JValue* pReturn) {
  uint32_t addr = ndk::Addr(func);
  if (!ndk::IsGuestCode(addr))
    return false;
  uint32_t words[2 * argc + 4];
  int n = 0;
  words[n++] = ndk::Addr(ndk::GuestEnvFor(static_cast<JNIEnv*>(pEnv)));
  words[n++] = clazz != NULL ? ndk::Addr(clazz) : *argv++;
  for (const char* s = shorty + 1; *s != '\0'; s++) {
    if (*s == 'J' || *s == 'D') {
      if (n & 1)
        words[n++] = 0;
      words[n++] = *argv++;
      words[n++] = *argv++;
    } else {
      words[n++] = *argv++;
    }
  }
  char buf[256];
  if (Tracing())
    ALOGI("-> %s (%s)", Name(addr, buf, sizeof buf), shorty);
  uint64_t r;
  if (Lenient()) {
    char why[256];
    if (!ndk::CallGuestGuarded(addr, words, n, &r, why, sizeof why)) {
      char msg[600];
      snprintf(msg, sizeof msg, "native code of the app ended in %s: %s",
               Name(addr, buf, sizeof buf), why);
      ALOGE("%s", msg);
      JNIEnv* env = static_cast<JNIEnv*>(pEnv);
      env->ThrowNew(env->FindClass("java/lang/RuntimeException"), msg);
      pReturn->j = 0;
      return true;
    }
  } else {
    r = ndk::CallGuest(addr, words, n);
  }
  if (Tracing())
    ALOGI("<- %s", Name(addr, buf, sizeof buf));
  switch (shorty[0]) {
    case 'V': break;
    case 'J': case 'D': pReturn->j = (s8)r; break;
    case 'Z': pReturn->i = (u1)r; break;
    case 'B': pReturn->i = (s1)r; break;
    case 'C': pReturn->i = (u2)r; break;
    case 'S': pReturn->i = (s2)r; break;
    default: pReturn->i = (s4)(u4)r; break;  // I F L [
  }
  return true;
}

}  // extern "C"
