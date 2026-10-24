// What a Vulkan SPIR-V module needs before Tint turns it into WGSL
// (vk_icd.h; topic vulkan, docs/topics/vulkan.md):
//
//   * specialization constants become constants with the pipeline's values
//     (VkSpecializationInfo), defaults where it gives none;
//   * push constants (WebGPU has none) become a read-only storage buffer at
//     (push_group, push_binding), which the page fills per draw;
//   * vertex shaders get Vulkan's y axis: Vulkan's clip space has y down,
//     WebGPU's y up, so the entry point is wrapped in one that negates
//     gl_Position.y after the original returns.
#ifndef ARC_WASM_SHIM_VULKAN_VK_SPIRV_H_
#define ARC_WASM_SHIM_VULKAN_VK_SPIRV_H_

#include <stdint.h>

#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace arcvk {

struct SpirvOptions {
  VkShaderStageFlagBits stage = VK_SHADER_STAGE_VERTEX_BIT;
  std::string entry = "main";
  const VkSpecializationInfo* spec = nullptr;
  int push_group = -1;            // -1: leave push constants alone
  int push_binding = 999;
  bool flip_y = true;             // vertex shaders only
};

// What the module declares that the pipeline needs to know.
struct SpirvInfo {
  bool has_push_constants = false;
  // (set, binding) of every combined image sampler variable.
  std::vector<std::pair<uint32_t, uint32_t>> combined;
  // (set, binding) of storage buffers the module writes (none NonWritable).
  std::vector<std::pair<uint32_t, uint32_t>> written_storage;
};

// False with |error| set if |in| is not SPIR-V this understands.
bool PrepareSpirv(const std::vector<uint32_t>& in, const SpirvOptions& options, std::vector<uint32_t>* out,
                  SpirvInfo* info, std::string* error);

}  // namespace arcvk

#endif  // ARC_WASM_SHIM_VULKAN_VK_SPIRV_H_
