// A host program around shim/vulkan/vk_spirv.cc (topic vulkan): runs the
// driver's SPIR-V pass on a module, for spirv-val and Tint to check.
//
//   spirv_prepass IN.spv OUT.spv vertex|fragment|compute [PUSH_GROUP]

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "vk_spirv.h"

int main(int argc, char** argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: %s IN.spv OUT.spv vertex|fragment|compute [PUSH_GROUP]\n", argv[0]);
    return 2;
  }
  FILE* f = fopen(argv[1], "rb");
  if (!f) return 2;
  std::vector<uint32_t> in;
  uint32_t w;
  while (fread(&w, 4, 1, f) == 1) in.push_back(w);
  fclose(f);
  arcvk::SpirvOptions o;
  o.stage = !strcmp(argv[3], "vertex")     ? VK_SHADER_STAGE_VERTEX_BIT
            : !strcmp(argv[3], "fragment") ? VK_SHADER_STAGE_FRAGMENT_BIT
                                           : VK_SHADER_STAGE_COMPUTE_BIT;
  o.push_group = argc > 4 ? atoi(argv[4]) : -1;
  std::vector<uint32_t> out;
  arcvk::SpirvInfo info;
  std::string error;
  if (!arcvk::PrepareSpirv(in, o, &out, &info, &error)) {
    fprintf(stderr, "error: %s\n", error.c_str());
    return 1;
  }
  printf("push constants: %d, combined samplers: %zu, written storage buffers: %zu\n", info.has_push_constants,
         info.combined.size(), info.written_storage.size());
  f = fopen(argv[2], "wb");
  fwrite(out.data(), 4, out.size(), f);
  fclose(f);
  return 0;
}
