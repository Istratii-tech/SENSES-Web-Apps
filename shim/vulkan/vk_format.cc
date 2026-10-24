// VkFormat <-> WebGPU formats (vk_icd.h).

#include "vk_icd.h"

namespace arcvk {
namespace {

constexpr uint8_t C = VK_IMAGE_ASPECT_COLOR_BIT;
constexpr uint8_t D = VK_IMAGE_ASPECT_DEPTH_BIT;
constexpr uint8_t S = VK_IMAGE_ASPECT_STENCIL_BIT;

// {vk, gpu, bytes, bw, bh, aspects, feature, renderable, blendable, storage, filterable, copyable}
#define PLAIN(vk, gpu, bytes, rend, blend, stor, filt) \
  {vk, gpu, bytes, 1, 1, C, nullptr, rend, blend, stor, filt, true}
#define BLOCK(vk, gpu, bytes, w, h, feat) {vk, gpu, bytes, w, h, C, feat, false, false, false, true, true}

const FormatInfo kFormats[] = {
    PLAIN(VK_FORMAT_R8_UNORM, "r8unorm", 1, true, true, false, true),
    PLAIN(VK_FORMAT_R8_SNORM, "r8snorm", 1, false, false, false, true),
    PLAIN(VK_FORMAT_R8_UINT, "r8uint", 1, true, false, false, false),
    PLAIN(VK_FORMAT_R8_SINT, "r8sint", 1, true, false, false, false),
    PLAIN(VK_FORMAT_R8G8_UNORM, "rg8unorm", 2, true, true, false, true),
    PLAIN(VK_FORMAT_R8G8_SNORM, "rg8snorm", 2, false, false, false, true),
    PLAIN(VK_FORMAT_R8G8_UINT, "rg8uint", 2, true, false, false, false),
    PLAIN(VK_FORMAT_R8G8_SINT, "rg8sint", 2, true, false, false, false),
    PLAIN(VK_FORMAT_R8G8B8A8_UNORM, "rgba8unorm", 4, true, true, true, true),
    PLAIN(VK_FORMAT_R8G8B8A8_SRGB, "rgba8unorm-srgb", 4, true, true, false, true),
    PLAIN(VK_FORMAT_R8G8B8A8_SNORM, "rgba8snorm", 4, false, false, true, true),
    PLAIN(VK_FORMAT_R8G8B8A8_UINT, "rgba8uint", 4, true, false, true, false),
    PLAIN(VK_FORMAT_R8G8B8A8_SINT, "rgba8sint", 4, true, false, true, false),
    PLAIN(VK_FORMAT_A8B8G8R8_UNORM_PACK32, "rgba8unorm", 4, true, true, true, true),
    PLAIN(VK_FORMAT_A8B8G8R8_SRGB_PACK32, "rgba8unorm-srgb", 4, true, true, false, true),
    PLAIN(VK_FORMAT_A8B8G8R8_SNORM_PACK32, "rgba8snorm", 4, false, false, true, true),
    PLAIN(VK_FORMAT_A8B8G8R8_UINT_PACK32, "rgba8uint", 4, true, false, true, false),
    PLAIN(VK_FORMAT_A8B8G8R8_SINT_PACK32, "rgba8sint", 4, true, false, true, false),
    PLAIN(VK_FORMAT_B8G8R8A8_UNORM, "bgra8unorm", 4, true, true, false, true),
    PLAIN(VK_FORMAT_B8G8R8A8_SRGB, "bgra8unorm-srgb", 4, true, true, false, true),
    PLAIN(VK_FORMAT_A2B10G10R10_UNORM_PACK32, "rgb10a2unorm", 4, true, true, false, true),
    PLAIN(VK_FORMAT_A2B10G10R10_UINT_PACK32, "rgb10a2uint", 4, true, false, false, false),
    PLAIN(VK_FORMAT_B10G11R11_UFLOAT_PACK32, "rg11b10ufloat", 4, false, false, false, true),
    PLAIN(VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, "rgb9e5ufloat", 4, false, false, false, true),
    PLAIN(VK_FORMAT_R16_UINT, "r16uint", 2, true, false, false, false),
    PLAIN(VK_FORMAT_R16_SINT, "r16sint", 2, true, false, false, false),
    PLAIN(VK_FORMAT_R16_SFLOAT, "r16float", 2, true, true, false, true),
    PLAIN(VK_FORMAT_R16G16_UINT, "rg16uint", 4, true, false, false, false),
    PLAIN(VK_FORMAT_R16G16_SINT, "rg16sint", 4, true, false, false, false),
    PLAIN(VK_FORMAT_R16G16_SFLOAT, "rg16float", 4, true, true, false, true),
    PLAIN(VK_FORMAT_R16G16B16A16_UINT, "rgba16uint", 8, true, false, true, false),
    PLAIN(VK_FORMAT_R16G16B16A16_SINT, "rgba16sint", 8, true, false, true, false),
    PLAIN(VK_FORMAT_R16G16B16A16_SFLOAT, "rgba16float", 8, true, true, true, true),
    PLAIN(VK_FORMAT_R32_UINT, "r32uint", 4, true, false, true, false),
    PLAIN(VK_FORMAT_R32_SINT, "r32sint", 4, true, false, true, false),
    PLAIN(VK_FORMAT_R32_SFLOAT, "r32float", 4, true, false, true, false),
    PLAIN(VK_FORMAT_R32G32_UINT, "rg32uint", 8, true, false, true, false),
    PLAIN(VK_FORMAT_R32G32_SINT, "rg32sint", 8, true, false, true, false),
    PLAIN(VK_FORMAT_R32G32_SFLOAT, "rg32float", 8, true, false, true, false),
    PLAIN(VK_FORMAT_R32G32B32A32_UINT, "rgba32uint", 16, true, false, true, false),
    PLAIN(VK_FORMAT_R32G32B32A32_SINT, "rgba32sint", 16, true, false, true, false),
    PLAIN(VK_FORMAT_R32G32B32A32_SFLOAT, "rgba32float", 16, true, false, true, false),
    {VK_FORMAT_D16_UNORM, "depth16unorm", 2, 1, 1, D, nullptr, true, false, false, false, true},
    {VK_FORMAT_X8_D24_UNORM_PACK32, "depth24plus", 4, 1, 1, D, nullptr, true, false, false, false, false},
    {VK_FORMAT_D32_SFLOAT, "depth32float", 4, 1, 1, D, nullptr, true, false, false, false, true},
    {VK_FORMAT_D24_UNORM_S8_UINT, "depth24plus-stencil8", 4, 1, 1, D | S, nullptr, true, false,
     false, false, false},
    {VK_FORMAT_D32_SFLOAT_S8_UINT, "depth32float-stencil8", 8, 1, 1, D | S, "depth32float-stencil8",
     true, false, false, false, false},
    {VK_FORMAT_S8_UINT, "stencil8", 1, 1, 1, S, nullptr, true, false, false, false, true},
    BLOCK(VK_FORMAT_BC1_RGB_UNORM_BLOCK, "bc1-rgba-unorm", 8, 4, 4, "texture-compression-bc"),
    BLOCK(VK_FORMAT_BC1_RGB_SRGB_BLOCK, "bc1-rgba-unorm-srgb", 8, 4, 4, "texture-compression-bc"),
    BLOCK(VK_FORMAT_BC1_RGBA_UNORM_BLOCK, "bc1-rgba-unorm", 8, 4, 4, "texture-compression-bc"),
    BLOCK(VK_FORMAT_BC1_RGBA_SRGB_BLOCK, "bc1-rgba-unorm-srgb", 8, 4, 4, "texture-compression-bc"),
    BLOCK(VK_FORMAT_BC2_UNORM_BLOCK, "bc2-rgba-unorm", 16, 4, 4, "texture-compression-bc"),
    BLOCK(VK_FORMAT_BC2_SRGB_BLOCK, "bc2-rgba-unorm-srgb", 16, 4, 4, "texture-compression-bc"),
    BLOCK(VK_FORMAT_BC3_UNORM_BLOCK, "bc3-rgba-unorm", 16, 4, 4, "texture-compression-bc"),
    BLOCK(VK_FORMAT_BC3_SRGB_BLOCK, "bc3-rgba-unorm-srgb", 16, 4, 4, "texture-compression-bc"),
    BLOCK(VK_FORMAT_BC4_UNORM_BLOCK, "bc4-r-unorm", 8, 4, 4, "texture-compression-bc"),
    BLOCK(VK_FORMAT_BC4_SNORM_BLOCK, "bc4-r-snorm", 8, 4, 4, "texture-compression-bc"),
    BLOCK(VK_FORMAT_BC5_UNORM_BLOCK, "bc5-rg-unorm", 16, 4, 4, "texture-compression-bc"),
    BLOCK(VK_FORMAT_BC5_SNORM_BLOCK, "bc5-rg-snorm", 16, 4, 4, "texture-compression-bc"),
    BLOCK(VK_FORMAT_BC6H_UFLOAT_BLOCK, "bc6h-rgb-ufloat", 16, 4, 4, "texture-compression-bc"),
    BLOCK(VK_FORMAT_BC6H_SFLOAT_BLOCK, "bc6h-rgb-float", 16, 4, 4, "texture-compression-bc"),
    BLOCK(VK_FORMAT_BC7_UNORM_BLOCK, "bc7-rgba-unorm", 16, 4, 4, "texture-compression-bc"),
    BLOCK(VK_FORMAT_BC7_SRGB_BLOCK, "bc7-rgba-unorm-srgb", 16, 4, 4, "texture-compression-bc"),
    BLOCK(VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, "etc2-rgb8unorm", 8, 4, 4, "texture-compression-etc2"),
    BLOCK(VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK, "etc2-rgb8unorm-srgb", 8, 4, 4, "texture-compression-etc2"),
    BLOCK(VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK, "etc2-rgb8a1unorm", 8, 4, 4, "texture-compression-etc2"),
    BLOCK(VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK, "etc2-rgb8a1unorm-srgb", 8, 4, 4,
          "texture-compression-etc2"),
    BLOCK(VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK, "etc2-rgba8unorm", 16, 4, 4, "texture-compression-etc2"),
    BLOCK(VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK, "etc2-rgba8unorm-srgb", 16, 4, 4,
          "texture-compression-etc2"),
    BLOCK(VK_FORMAT_EAC_R11_UNORM_BLOCK, "eac-r11unorm", 8, 4, 4, "texture-compression-etc2"),
    BLOCK(VK_FORMAT_EAC_R11_SNORM_BLOCK, "eac-r11snorm", 8, 4, 4, "texture-compression-etc2"),
    BLOCK(VK_FORMAT_EAC_R11G11_UNORM_BLOCK, "eac-rg11unorm", 16, 4, 4, "texture-compression-etc2"),
    BLOCK(VK_FORMAT_EAC_R11G11_SNORM_BLOCK, "eac-rg11snorm", 16, 4, 4, "texture-compression-etc2"),
#define ASTC(w, h)                                                                              \
  BLOCK(VK_FORMAT_ASTC_##w##x##h##_UNORM_BLOCK, "astc-" #w "x" #h "-unorm", 16, w, h,         \
        "texture-compression-astc"),                                                          \
      BLOCK(VK_FORMAT_ASTC_##w##x##h##_SRGB_BLOCK, "astc-" #w "x" #h "-unorm-srgb", 16, w, h, \
            "texture-compression-astc")
    ASTC(4, 4), ASTC(5, 4), ASTC(5, 5), ASTC(6, 5), ASTC(6, 6), ASTC(8, 5), ASTC(8, 6), ASTC(8, 8),
    ASTC(10, 5), ASTC(10, 6), ASTC(10, 8), ASTC(10, 10), ASTC(12, 10), ASTC(12, 12),
#undef ASTC
};

const struct {
  VkFormat vk;
  const char* gpu;
} kVertexFormats[] = {
    {VK_FORMAT_R8_UNORM, "unorm8"}, {VK_FORMAT_R8_SNORM, "snorm8"},
    {VK_FORMAT_R8_UINT, "uint8"}, {VK_FORMAT_R8_SINT, "sint8"},
    {VK_FORMAT_R8G8_UNORM, "unorm8x2"}, {VK_FORMAT_R8G8_SNORM, "snorm8x2"},
    {VK_FORMAT_R8G8_UINT, "uint8x2"}, {VK_FORMAT_R8G8_SINT, "sint8x2"},
    {VK_FORMAT_R8G8B8A8_UNORM, "unorm8x4"}, {VK_FORMAT_R8G8B8A8_SNORM, "snorm8x4"},
    {VK_FORMAT_R8G8B8A8_UINT, "uint8x4"}, {VK_FORMAT_R8G8B8A8_SINT, "sint8x4"},
    {VK_FORMAT_A8B8G8R8_UNORM_PACK32, "unorm8x4"}, {VK_FORMAT_A8B8G8R8_SNORM_PACK32, "snorm8x4"},
    {VK_FORMAT_A8B8G8R8_UINT_PACK32, "uint8x4"}, {VK_FORMAT_A8B8G8R8_SINT_PACK32, "sint8x4"},
    {VK_FORMAT_B8G8R8A8_UNORM, "unorm8x4-bgra"},
    {VK_FORMAT_R16_UNORM, "unorm16"}, {VK_FORMAT_R16_SNORM, "snorm16"},
    {VK_FORMAT_R16_UINT, "uint16"}, {VK_FORMAT_R16_SINT, "sint16"},
    {VK_FORMAT_R16_SFLOAT, "float16"},
    {VK_FORMAT_R16G16_UNORM, "unorm16x2"}, {VK_FORMAT_R16G16_SNORM, "snorm16x2"},
    {VK_FORMAT_R16G16_UINT, "uint16x2"}, {VK_FORMAT_R16G16_SINT, "sint16x2"},
    {VK_FORMAT_R16G16_SFLOAT, "float16x2"},
    {VK_FORMAT_R16G16B16A16_UNORM, "unorm16x4"}, {VK_FORMAT_R16G16B16A16_SNORM, "snorm16x4"},
    {VK_FORMAT_R16G16B16A16_UINT, "uint16x4"}, {VK_FORMAT_R16G16B16A16_SINT, "sint16x4"},
    {VK_FORMAT_R16G16B16A16_SFLOAT, "float16x4"},
    {VK_FORMAT_R32_SFLOAT, "float32"}, {VK_FORMAT_R32G32_SFLOAT, "float32x2"},
    {VK_FORMAT_R32G32B32_SFLOAT, "float32x3"}, {VK_FORMAT_R32G32B32A32_SFLOAT, "float32x4"},
    {VK_FORMAT_R32_UINT, "uint32"}, {VK_FORMAT_R32G32_UINT, "uint32x2"},
    {VK_FORMAT_R32G32B32_UINT, "uint32x3"}, {VK_FORMAT_R32G32B32A32_UINT, "uint32x4"},
    {VK_FORMAT_R32_SINT, "sint32"}, {VK_FORMAT_R32G32_SINT, "sint32x2"},
    {VK_FORMAT_R32G32B32_SINT, "sint32x3"}, {VK_FORMAT_R32G32B32A32_SINT, "sint32x4"},
    {VK_FORMAT_A2B10G10R10_UNORM_PACK32, "unorm10-10-10-2"},
};

}  // namespace

const FormatInfo* GetFormat(VkFormat f) {
  for (const auto& i : kFormats)
    if (i.vk == f) return &i;
  return nullptr;
}

const char* VertexFormat(VkFormat f) {
  for (const auto& v : kVertexFormats)
    if (v.vk == f) return v.gpu;
  return nullptr;
}

}  // namespace arcvk
