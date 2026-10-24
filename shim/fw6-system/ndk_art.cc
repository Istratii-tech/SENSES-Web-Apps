// ART's side of the NDK bridge (shim/ndk), as shim/ndk/ndk_dalvik.cc is
// Dalvik's: dlopen of ARM libraries for System.load/loadLibrary (the
// ndk_dalvik_* functions shim/art/static_jni_libs_art.cc refers to, named
// for the bridge's interface, shim/ndk/ndk_jni.h), the guest JNI_OnLoad,
// android_update_LD_LIBRARY_PATH, the shorty of a jmethodID (an
// ArtMethod*), and calls from ART into native methods implemented in guest
// code: arc_ndk_art_call_native, which shim/art/jni_call_wasm.cc asks
// before calling a native through JS. Compiled with ART's headers.
// Part of the arc-wasm shim layer (topic fw6-system).

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include <cutils/log.h>
// (libcutils; its header needs bionic's sys/system_properties.h)
extern "C" int property_get(const char* key, char* value, const char* default_value);

#include <mutex>
#include <string>

#include "art_method-inl.h"
#include "ndk_bridge.h"
#if __has_include("scoped_thread_state_change-inl.h")  // ART 8
#include "scoped_thread_state_change-inl.h"
#else
#include "scoped_thread_state_change.h"
#include "utils.h"
#endif
#include "thread.h"
#include "ndk_jni.h"

#undef LOG_TAG
#define LOG_TAG "ndk"

namespace {

bool Tracing() { return ndk::TraceLevel() >= 1; }

// debug.ndk.trace_jni=TEXT: logs calls of native methods whose Java name
// (e.g. "org.mozilla.geckoview.GeckoSession$Compositor.") contains TEXT.
const char* TraceJniFilter() {
  static char filter[92];
  static std::once_flag once;
  std::call_once(once, [] { property_get("debug.ndk.trace_jni", filter, ""); });
  return filter;
}

std::string TracedMethod() {
  const char* filter = TraceJniFilter();
  if (filter[0] == '\0')
    return std::string();
  art::ScopedObjectAccess soa(art::Thread::Current());
  art::ArtMethod* m = soa.Self()->GetCurrentMethod(nullptr);
  if (m == nullptr)
    return std::string();
#if __has_include("obj_ptr.h")  // ART 8
  std::string name = m->PrettyMethod();
#else
  std::string name = art::PrettyMethod(m);
#endif
  return name.find(filter) != std::string::npos ? name : std::string();
}

const char* Name(uint32_t addr, char* buf, size_t size) {
  uint32_t off = 0;
  const char* sym = ndk::Symbolize(addr, &off);
  if (!sym && ndk::Symbolize64)
    sym = ndk::Symbolize64(addr, &off);
  snprintf(buf, size, "%s+0x%x", sym ? sym : "?", off);
  return buf;
}

bool IsHandle64(void* handle) { return ndk::IsLibrary64 && ndk::IsLibrary64(handle); }
bool IsGuest64(uint32_t addr) { return ndk::IsGuestCode64 && ndk::IsGuestCode64(addr); }

// A native method in arm64 guest code (AAPCS64): JNIEnv* and this-or-class in
// x0, x1, then the Java arguments: integers and references in x2-x7, float
// and double in v0-v7, the rest in 8-byte stack slots in order. |kinds| and
// |args| as for arc_ndk_art_call_native below.
bool CallNative64(uint32_t addr, const char* kinds, const uint32_t* args, void* ret) {
  size_t nkinds = strlen(kinds);
  Ndk64Args a;
  memset(&a, 0, sizeof a);
  uint64_t stack[nkinds + 2];
  int nx = 0, nv = 0, ns = 0;
  a.x[nx++] = ndk::Addr(ndk::GuestEnvFor64(reinterpret_cast<JNIEnv*>(args[0])));
  const uint32_t* p = args + 1;
  for (const char* k = kinds + 2; *k != '\0'; ++k) {
    uint64_t v;
    bool fp = *k == 'f' || *k == 'd';
    if (*k == 'j' || *k == 'd') {
      v = p[0] | (uint64_t)p[1] << 32;
      p += 2;
    } else {
      v = *p++;   // int, reference (zero-extended), float bits
    }
    if (fp && nv < 8) a.v[nv++] = v;
    else if (!fp && nx < 8) a.x[nx++] = v;
    else stack[ns++] = v;
  }
  a.stack = stack;
  a.nstack = ns;
  char buf[256];
  if (Tracing())
    ALOGI("-> %s (%s, arm64)", Name(addr, buf, sizeof buf), kinds);
  std::string traced = TracedMethod();
  if (!traced.empty())
    ALOGI("jni -> %s", traced.c_str());
  Ndk64Result r;
  ndk::CallGuest64Args(addr, &a, &r);
  if (Tracing())
    ALOGI("<- %s", Name(addr, buf, sizeof buf));
  if (!traced.empty())
    ALOGI("jni <- %s", traced.c_str());
  art::JValue* result = static_cast<art::JValue*>(ret);
  switch (kinds[0]) {
    case 'v': break;
    case 'j': result->SetJ(static_cast<int64_t>(r.x0)); break;
    case 'd': result->SetJ(static_cast<int64_t>(r.v0)); break;
    case 'f': result->SetI(static_cast<int32_t>(static_cast<uint32_t>(r.v0))); break;
    default: result->SetI(static_cast<int32_t>(static_cast<uint32_t>(r.x0))); break;  // i, references
  }
  return true;
}

}  // namespace

extern "C" {

int ndk_dalvik_dlopen(const char* path, int flags, void** handle) {
  // An ARM or AArch64 ELF file? (e_ident, then e_type and e_machine at 16 and 18.)
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return 0;
  unsigned char h[20];
  bool elf = read(fd, h, sizeof h) == (ssize_t)sizeof h && memcmp(h, "\177ELF", 4) == 0;
  close(fd);
  int machine = elf ? (h[18] | h[19] << 8) : 0;
  if (elf && h[4] == 1 /* ELFCLASS32 */ && machine == 40 /* EM_ARM */) {
    *handle = ndk::Dlopen(path, flags);
    return 1;
  }
  // arm64-v8a (topic ndk-arm64), in builds with the AArch64 CPU.
  if (elf && h[4] == 2 /* ELFCLASS64 */ && machine == 183 /* EM_AARCH64 */ && ndk::Dlopen64) {
    *handle = ndk::Dlopen64(path, flags);
    return 1;
  }
  return 0;
}

void ndk_update_ld_library_path(const char* path) { ndk::SetLibraryPath(path); }

int ndk_dalvik_is_handle(void* handle) { return ndk::IsLibrary(handle) || IsHandle64(handle); }

void* ndk_dalvik_dlsym(void* handle, const char* symbol) {
  if (IsHandle64(handle))
    return ndk::Ptr<void>(ndk::Dlsym64(static_cast<ndk::Library64*>(handle), symbol));
  return ndk::Ptr<void>(ndk::Dlsym(static_cast<ndk::Library*>(handle), symbol));
}

const char* ndk_dalvik_dlerror(void) {
  if (ndk::Dlerror64) {
    if (const char* e = ndk::Dlerror64())
      return e;
  }
  return ndk::Dlerror();
}



int ndk_dalvik_jni_onload(void* fn, JavaVM* vm) {
  char buf[256];
  if (IsGuest64(ndk::Addr(fn))) {
    uint64_t args[2] = {ndk::Addr(ndk::GuestVmFor64(vm)), 0};
    int r = (int32_t)ndk::CallGuest64(ndk::Addr(fn), args, 2);
    ALOGI("%s returned 0x%x", Name(ndk::Addr(fn), buf, sizeof buf), r);
    return r;
  }
  uint32_t args[2] = {ndk::Addr(ndk::GuestVmFor(vm)), 0};
  int r = (int32_t)ndk::CallGuest(ndk::Addr(fn), args, 2);
  ALOGI("%s returned 0x%x", Name(ndk::Addr(fn), buf, sizeof buf), r);
  return r;
}

// In ART 6.0 a jmethodID is the ArtMethod*.
const char* ndk_method_shorty(jmethodID mid) {
  return reinterpret_cast<art::ArtMethod*>(mid)->GetShorty();
}

// A native method in guest code: the AAPCS soft-float arguments are
// JNIEnv*, this-or-class, then the Java arguments, 64-bit values on an even
// word (registers r0-r3, then the stack). |kinds| is the return kind then
// one per argument ('i' 32 bits, 'j' long, 'f' float, 'd' double), |args|
// their 32-bit slots, longs and doubles in two (shim/art/jni_call_wasm.cc).
bool arc_ndk_art_call_native(const void* fn, const char* kinds, const uint32_t* args,
                             void* ret) {
  uint32_t addr = ndk::Addr(fn);
  if (IsGuest64(addr))
    return CallNative64(addr, kinds, args, ret);
  if (!ndk::IsGuestCode(addr))
    return false;
  size_t nkinds = strlen(kinds);
  uint32_t words[2 * nkinds + 4];
  int n = 0;
  // env: the guest-visible JNIEnv for the host one.
  words[n++] = ndk::Addr(ndk::GuestEnvFor(reinterpret_cast<JNIEnv*>(args[0])));
  const uint32_t* a = args + 1;
  for (const char* k = kinds + 2; *k != '\0'; ++k) {
    // kinds[1] is env (done above); kinds[2] the this-or-class slot, then
    // the arguments.
    if (*k == 'j' || *k == 'd') {
      if (n & 1)
        words[n++] = 0;
      words[n++] = *a++;
      words[n++] = *a++;
    } else {
      words[n++] = *a++;
    }
  }
  char buf[256];
  if (Tracing())
    ALOGI("-> %s (%s)", Name(addr, buf, sizeof buf), kinds);
  std::string traced = TracedMethod();
  if (!traced.empty())
    ALOGI("jni -> %s", traced.c_str());
  uint64_t r = ndk::CallGuest(addr, words, n);
  if (Tracing())
    ALOGI("<- %s", Name(addr, buf, sizeof buf));
  if (!traced.empty())
    ALOGI("jni <- %s", traced.c_str());
  art::JValue* result = static_cast<art::JValue*>(ret);
  switch (kinds[0]) {
    case 'v': break;
    case 'j': case 'd': result->SetJ(static_cast<int64_t>(r)); break;
    default: result->SetI(static_cast<int32_t>(static_cast<uint32_t>(r))); break;  // i f
  }
  return true;
}

}  // extern "C"
