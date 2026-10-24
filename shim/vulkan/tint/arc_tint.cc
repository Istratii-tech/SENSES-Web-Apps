// SPIR-V -> WGSL for arc-wasm's Vulkan driver (topic vulkan,
// docs/topics/vulkan.md): Tint (Dawn, Apache-2.0) built into a wasm module
// of its own (tools/vulkan/build_tint.sh) that the page loads next to the
// VM (shim/vulkan/webgpu_host.js). Tint's SPIR-V reader runs SPIRV-Tools'
// split-combined-image-sampler pass: combined image samplers become a
// texture and a sampler, the sampler at the binding |sampler_map| gives.

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "src/tint/lang/core/ir/module.h"
#include "src/tint/lang/spirv/reader/reader.h"
#include "src/tint/lang/wgsl/writer/writer.h"

namespace {

char* Answer(char kind, const std::string& text) {
  char* out = static_cast<char*>(malloc(text.size() + 2));
  out[0] = kind;
  memcpy(out + 1, text.data(), text.size());
  out[text.size() + 1] = 0;
  return out;
}

}  // namespace

extern "C" {

// |words|: the module; |sampler_map|: |n| entries of (group, binding,
// sampler's group, sampler's binding) for combined image samplers. Returns
// "W" + the WGSL or "E" + the error, to free with arc_tint_free.
__attribute__((used)) char* arc_tint_spirv_to_wgsl(const uint32_t* words, uint32_t nwords,
                                                   const uint32_t* sampler_map, uint32_t n) {
  std::vector<uint32_t> spirv(words, words + nwords);
  tint::spirv::reader::Options opts;
  for (uint32_t i = 0; i < n; i++) {
    const uint32_t* e = sampler_map + 4 * i;
    opts.sampler_mappings[tint::BindingPoint{e[0], e[1]}] = tint::BindingPoint{e[2], e[3]};
  }
  auto ir = tint::spirv::reader::ReadIR(spirv, opts);
  if (ir != tint::Success) return Answer('E', ir.Failure().reason);
  tint::wgsl::writer::Options wopts;
  wopts.allowed_features = tint::wgsl::AllowedFeatures::Everything();
  auto out = tint::wgsl::writer::WgslFromIR(ir.Get(), wopts);
  if (out != tint::Success) return Answer('E', out.Failure().reason);
  return Answer('W', out->wgsl);
}

__attribute__((used)) void arc_tint_free(char* p) { free(p); }

}  // extern "C"
