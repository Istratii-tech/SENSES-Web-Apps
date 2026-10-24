// A stand-in for the ARM CPU (ndk_cpu.h) that runs no ARM code, for
// building and unit-testing the bridge without topic "arm-cpu". Host stubs
// get unique addresses in a private arena; ndk_call_guest runs a host stub
// directly (as if guest code had branched to it) or a "fake guest
// function" a test registered with ndk_stub_fake_guest, and aborts for
// anything else. Never linked into a real build. Part of the arc-wasm shim
// layer.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <mutex>

#include "../ndk_cpu.h"

namespace {

struct Stub {
  NdkHostFn fn;
  void* user;
  const char* name;
};
std::mutex g_lock;
std::map<uint32_t, Stub> g_stubs;
std::map<uint32_t, void (*)(NdkCpuState*)> g_fakes;
char* g_arena;
size_t g_used;
NdkSymbolizer g_symbolizer;
thread_local NdkCpuState* t_state;
const size_t kStack = 256 * 1024;

}  // namespace

extern "C" {

// Registers a host function that plays the guest function at |addr|.
void ndk_stub_fake_guest(uint32_t addr, void (*fn)(NdkCpuState*)) {
  std::lock_guard<std::mutex> l(g_lock);
  g_fakes[addr & ~1u] = fn;
}

// The host function behind a stub address (tests call it with their own
// NdkCpuState).
NdkHostFn ndk_stub_lookup(uint32_t addr, void** user) {
  std::lock_guard<std::mutex> l(g_lock);
  auto it = g_stubs.find(addr & ~1u);
  if (it == g_stubs.end())
    return nullptr;
  *user = it->second.user;
  return it->second.fn;
}

uint32_t ndk_host_stub(NdkHostFn fn, void* user, const char* name) {
  std::lock_guard<std::mutex> l(g_lock);
  if (!g_arena)
    g_arena = static_cast<char*>(calloc(1, 1 << 20));
  uint32_t a = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_arena + g_used));
  g_used += 8;
  g_stubs[a] = {fn, user, name};
  return a;
}

NdkCpuState* ndk_current_state(void) {
  if (!t_state) {
    t_state = static_cast<NdkCpuState*>(calloc(1, sizeof(NdkCpuState)));
    char* stack = static_cast<char*>(malloc(kStack));
    t_state->r[13] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(stack + kStack));
  }
  return t_state;
}

uint64_t ndk_call_guest(uint32_t addr, const uint32_t* args, int nargs) {
  NdkCpuState* s = ndk_current_state();
  NdkCpuState saved = *s;
  // Arguments per AAPCS: r0-r3, then the stack (kept 8-aligned).
  int extra = nargs > 4 ? nargs - 4 : 0;
  s->r[13] = (s->r[13] - 4 * extra) & ~7u;
  for (int i = 0; i < nargs; i++) {
    if (i < 4)
      s->r[i] = args[i];
    else
      memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(s->r[13] + 4 * (i - 4))), &args[i], 4);
  }
  s->r[14] = 0xfffffff0;  // "return to the host"
  Stub stub = {};
  void (*fake)(NdkCpuState*) = nullptr;
  {
    std::lock_guard<std::mutex> l(g_lock);
    auto it = g_stubs.find(addr & ~1u);
    if (it != g_stubs.end())
      stub = it->second;
    auto f = g_fakes.find(addr & ~1u);
    if (f != g_fakes.end())
      fake = f->second;
  }
  if (stub.fn) {
    stub.fn(s, stub.user);
  } else if (fake) {
    fake(s);
  } else {
    uint32_t off = 0;
    const char* sym = g_symbolizer ? g_symbolizer(addr, &off) : nullptr;
    fprintf(stderr, "ndk_cpu_stub: no ARM CPU in this build; cannot run 0x%x (%s+0x%x)\n",
            addr, sym ? sym : "?", off);
    abort();
  }
  uint64_t r = s->r[0] | static_cast<uint64_t>(s->r[1]) << 32;
  *s = saved;
  return r;
}

// The guarded call (cpu/ndk_cpu_ext.h): no guest code to leave here, so
// leaving it ends the test.
int ndk_call_guest_guarded(uint32_t addr, const uint32_t* args, int nargs, uint64_t* result,
                           char*, size_t) {
  *result = ndk_call_guest(addr, args, nargs);
  return 1;
}
void ndk_leave_guest(const char* why) {
  fprintf(stderr, "ndk_cpu_stub: guest code ended: %s\n", why);
  abort();
}

void ndk_flush_code(uint32_t, size_t) {}

void ndk_set_symbolizer(NdkSymbolizer fn) { g_symbolizer = fn; }

}  // extern "C"
