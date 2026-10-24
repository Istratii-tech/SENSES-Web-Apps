// libvulkan.so as both guest ABIs see it (topic vulkan, docs/topics/vulkan.md):
// the entry points tools/vulkan/gen_vk.py generates (vk_thunks_gen.inc) and
// the two it does not, vkGetInstanceProcAddr and vkGetDeviceProcAddr,
// which answer with the guest addresses of the same host stubs (the 32-bit
// or the 64-bit one, by the caller's ABI). A host library of the NDK
// bridge (shim/ndk/ndk_host_libs.h): guest dlopen("libvulkan.so") finds it.

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "ndk_bridge.h"
#include "ndk_host_libs.h"
#include "vk_icd.h"

namespace ndk {
namespace vkthunk {

// Host copies of arm64 guests' structures live in a per-thread arena for the
// length of one call (ArenaScope); nested calls (a callback into guest code
// that calls Vulkan) release in LIFO order.
class Arena {
 public:
  void* Alloc(size_t n, size_t align) {
    used_ = (used_ + align - 1) & ~(align - 1);
    if (block_ >= blocks_.size() || used_ + n > sizes_[block_]) {
      // The next block (a fresh one if none is big enough).
      block_ = blocks_.empty() ? 0 : block_ + (block_ < blocks_.size() ? 1 : 0);
      while (block_ < blocks_.size() && sizes_[block_] < n) block_++;
      if (block_ >= blocks_.size()) {
        size_t size = n > kBlock ? n : kBlock;
        blocks_.emplace_back(new uint8_t[size]);
        sizes_.push_back(size);
        block_ = blocks_.size() - 1;
      }
      used_ = 0;
    }
    void* p = blocks_[block_].get() + used_;
    used_ += n;
    memset(p, 0, n);
    return p;
  }
  template <typename T>
  T* New() {
    return static_cast<T*>(Alloc(sizeof(T), alignof(T)));
  }
  template <typename T>
  T* NewArray(uint32_t n) {
    return static_cast<T*>(Alloc(sizeof(T) * (n ? n : 1), alignof(T)));
  }
  std::pair<size_t, size_t> Mark() const { return {block_, used_}; }
  void Release(std::pair<size_t, size_t> m) {
    block_ = m.first;
    used_ = m.second;
  }

 private:
  static constexpr size_t kBlock = 64 << 10;
  std::vector<std::unique_ptr<uint8_t[]>> blocks_;
  std::vector<size_t> sizes_;
  size_t block_ = 0, used_ = 0;
};

thread_local Arena* t_arena;

class ArenaScope {
 public:
  ArenaScope() {
    if (!t_arena) t_arena = new Arena();
    mark_ = t_arena->Mark();
  }
  ~ArenaScope() { t_arena->Release(mark_); }
  Arena& arena() { return *t_arena; }

 private:
  std::pair<size_t, size_t> mark_;
};

inline void* GuestPtr(uint32_t g) { return reinterpret_cast<void*>(static_cast<uintptr_t>(g)); }

void NoteUnknownSType(VkStructureType st) {
  arcvk::Unsupported("pNext structure type %d from an arm64 guest (dropped)", st);
}

// An aapcs-vfp float argument: s0-s15 are the halves of d0-d7.
inline float F32(NdkCpuState* s, int n) {
  uint64_t d = s->d[n / 2];
  return BitCast<float>(static_cast<uint32_t>(n & 1 ? d >> 32 : d));
}

#include "vk_thunks_gen.inc"

// --- vkGet*ProcAddr ------------------------------------------------------------------------

const std::set<std::string>& Commands() {
  static const std::set<std::string>* names = [] {
    auto* s = new std::set<std::string>;
#define X(name, impl, level) s->insert(#name);
    ARCVK_COMMANDS(X)
#undef X
    return s;
  }();
  return *names;
}

void* GetProcAddr32(void*, const char* name) {
  if (!name || !Commands().count(name)) return nullptr;
  return reinterpret_cast<void*>(static_cast<uintptr_t>(ResolveHostSymbol(name)));
}

void* GetProcAddr64(void*, const char* name) {
  if (!name || !Commands().count(name)) return nullptr;
  return reinterpret_cast<void*>(static_cast<uintptr_t>(ResolveHostSymbol64(name)));
}

const HostSymbol kProcAddr[] = {
    NDK_FN("vkGetInstanceProcAddr", GetProcAddr32), NDK_FN("vkGetDeviceProcAddr", GetProcAddr32),
    NDK_FN64("vkGetInstanceProcAddr", GetProcAddr64), NDK_FN64("vkGetDeviceProcAddr", GetProcAddr64),
};

void Register() {
  RegisterHostSymbols("libvulkan.so", kVulkanSymbols);
  RegisterHostSymbols("libvulkan.so", kProcAddr);
}

}  // namespace vkthunk
}  // namespace ndk

NDK_HOST_LIBRARY(vulkan, 10, ::ndk::vkthunk::Register)

// For the host side of tests and the system server (arcwasm.vulkan.VulkanSupport):
// the Vulkan version the system reports (0: none, no WebGPU).
extern "C" uint32_t arcvk_system_version() {
  return arcvk::HaveGpu() ? VK_MAKE_API_VERSION(0, 1, 1, 0) : 0;
}
