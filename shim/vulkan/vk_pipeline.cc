// Shader modules, descriptor sets and their layouts, pipeline layouts,
// render passes, framebuffers and pipelines (vk_icd.h).
//
// Bindings as WebGPU sees them (the page builds bind group layouts from the
// pipeline's description and the WGSL's declarations):
//
//   descriptor (set s, binding b)        @group(s) @binding(b)
//   the sampler of a combined image
//   sampler at (s, b)                    @group(s) @binding(b + kSamplerBindingOffset)
//   push constants                       @group(P) @binding(kPushBinding), a read-only
//                                        storage buffer; P = the layout's set count,
//                                        or the last set when there are 4
//
// so a pipeline's WGSL uses at most 4 bind groups (WebGPU's default limit).

#include <stdio.h>
#include <string.h>

#include <algorithm>

#include "vk_icd.h"
#include "vk_spirv.h"

namespace arcvk {

const char* CompareFunction(VkCompareOp op);  // vk_device.cc

namespace {

constexpr uint32_t kSamplerBindingOffset = 500;
constexpr uint32_t kPushBinding = 999;

Device* Dev(VkDevice d) { return reinterpret_cast<Device*>(d); }

// A JSON text built piece by piece.
class Json {
 public:
  Json& Raw(const std::string& s) {
    Sep();
    s_ += s;
    return *this;
  }
  Json& Key(const char* k) {
    Sep();
    s_ += '"';
    s_ += k;
    s_ += "\":";
    fresh_ = true;
    return *this;
  }
  Json& Str(const char* v) {
    Sep();
    s_ += '"';
    for (; v && *v; v++) {
      if (*v == '"' || *v == '\\') s_ += '\\';
      if (static_cast<unsigned char>(*v) >= 0x20) s_ += *v;
    }
    s_ += '"';
    return *this;
  }
  Json& Num(double v) {
    Sep();
    char b[40];
    snprintf(b, sizeof b, "%.9g", JsonNum(v));
    s_ += b;
    return *this;
  }
  Json& Int(long long v) {
    Sep();
    s_ += std::to_string(v);
    return *this;
  }
  Json& Bool(bool v) {
    Sep();
    s_ += v ? "true" : "false";
    return *this;
  }
  Json& Null() {
    Sep();
    s_ += "null";
    return *this;
  }
  Json& Open(char c) {
    Sep();
    s_ += c;
    fresh_ = true;
    return *this;
  }
  Json& Close(char c) {
    s_ += c;
    fresh_ = false;
    return *this;
  }
  const std::string& str() const { return s_; }

 private:
  void Sep() {
    if (!fresh_ && !s_.empty()) s_ += ',';
    fresh_ = false;
  }
  std::string s_;
  bool fresh_ = true;
};

const char* DescriptorKind(VkDescriptorType t) {
  switch (t) {
    case VK_DESCRIPTOR_TYPE_SAMPLER: return "sampler";
    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: return "combined";
    case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: return "texture";
    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: return "storageTexture";
    case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: return "uniformTexel";
    case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: return "storageTexel";
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC: return "uniform";
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: return "storage";
    case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: return "input";
    default: return "unknown";
  }
}

bool IsDynamic(VkDescriptorType t) {
  return t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
}

bool IsBuffer(VkDescriptorType t) {
  return t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
         t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
}

// GPUShaderStage bits of Vulkan stages.
uint32_t Visibility(VkShaderStageFlags s) {
  uint32_t v = 0;
  if (s & VK_SHADER_STAGE_VERTEX_BIT) v |= 1;
  if (s & VK_SHADER_STAGE_FRAGMENT_BIT) v |= 2;
  if (s & VK_SHADER_STAGE_COMPUTE_BIT) v |= 4;
  return v;
}

}  // namespace

// --- shader modules and pipeline caches ---------------------------------------------------

VkResult vkCreateShaderModule(VkDevice, const VkShaderModuleCreateInfo* info, const VkAllocationCallbacks*,
                              VkShaderModule* pModule) {
  ShaderModule* m = new ShaderModule();
  m->code.assign(info->pCode, info->pCode + info->codeSize / 4);
  *pModule = Handle<VkShaderModule>(m);
  return VK_SUCCESS;
}

void vkDestroyShaderModule(VkDevice, VkShaderModule module, const VkAllocationCallbacks*) {
  delete As<ShaderModule>(module);
}

struct PipelineCache {
  int unused;
};

VkResult vkCreatePipelineCache(VkDevice, const VkPipelineCacheCreateInfo*, const VkAllocationCallbacks*,
                               VkPipelineCache* pCache) {
  *pCache = Handle<VkPipelineCache>(new PipelineCache());
  return VK_SUCCESS;
}

void vkDestroyPipelineCache(VkDevice, VkPipelineCache cache, const VkAllocationCallbacks*) {
  delete As<PipelineCache>(cache);
}

VkResult vkGetPipelineCacheData(VkDevice device, VkPipelineCache, size_t* pSize, void* pData) {
  // Only the header (VkPipelineCacheHeaderVersionOne): nothing is cached.
  const size_t kSize = 32;
  if (!pData) {
    *pSize = kSize;
    return VK_SUCCESS;
  }
  if (*pSize < kSize) {
    *pSize = 0;
    return VK_INCOMPLETE;
  }
  const VkPhysicalDeviceProperties& p = Dev(device)->physical->props;
  uint32_t h[4] = {32, VK_PIPELINE_CACHE_HEADER_VERSION_ONE, p.vendorID, p.deviceID};
  memcpy(pData, h, 16);
  memcpy(static_cast<uint8_t*>(pData) + 16, p.pipelineCacheUUID, VK_UUID_SIZE);
  *pSize = kSize;
  return VK_SUCCESS;
}

VkResult vkMergePipelineCaches(VkDevice, VkPipelineCache, uint32_t, const VkPipelineCache*) { return VK_SUCCESS; }

// --- descriptor set layouts and pipeline layouts --------------------------------------------

const DescriptorSetLayout::Binding* DescriptorSetLayout::Find(uint32_t b) const {
  for (const auto& x : bindings)
    if (x.binding == b) return &x;
  return nullptr;
}

VkResult vkCreateDescriptorSetLayout(VkDevice, const VkDescriptorSetLayoutCreateInfo* info,
                                     const VkAllocationCallbacks*, VkDescriptorSetLayout* pLayout) {
  DescriptorSetLayout* l = new DescriptorSetLayout();
  for (uint32_t i = 0; i < info->bindingCount; i++) {
    const VkDescriptorSetLayoutBinding& b = info->pBindings[i];
    if (b.descriptorCount == 0) continue;
    DescriptorSetLayout::Binding x{b.binding, b.descriptorType, b.descriptorCount, b.stageFlags, {}};
    if (b.pImmutableSamplers &&
        (b.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER ||
         b.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)) {
      for (uint32_t k = 0; k < b.descriptorCount; k++) x.immutable.push_back(As<Sampler>(b.pImmutableSamplers[k]));
    }
    if (b.descriptorCount > 1 && !IsBuffer(b.descriptorType))
      Unsupported("arrays of textures or samplers in a descriptor set (WebGPU has no binding arrays)");
    l->bindings.push_back(x);
  }
  std::sort(l->bindings.begin(), l->bindings.end(),
            [](const DescriptorSetLayout::Binding& a, const DescriptorSetLayout::Binding& b) {
              return a.binding < b.binding;
            });
  for (const auto& b : l->bindings)
    if (IsDynamic(b.type)) l->dynamic_count += b.count;
  *pLayout = Handle<VkDescriptorSetLayout>(l);
  return VK_SUCCESS;
}

void vkDestroyDescriptorSetLayout(VkDevice, VkDescriptorSetLayout layout, const VkAllocationCallbacks*) {
  // Sets and pipeline layouts made from it may outlive it in Vulkan: kept.
  (void)layout;
}

void vkGetDescriptorSetLayoutSupport(VkDevice, const VkDescriptorSetLayoutCreateInfo* info,
                                     VkDescriptorSetLayoutSupport* out) {
  out->supported = info->bindingCount <= 64;
}

VkResult vkCreatePipelineLayout(VkDevice, const VkPipelineLayoutCreateInfo* info, const VkAllocationCallbacks*,
                                VkPipelineLayout* pLayout) {
  PipelineLayout* l = new PipelineLayout();
  for (uint32_t i = 0; i < info->setLayoutCount; i++) l->sets.push_back(As<DescriptorSetLayout>(info->pSetLayouts[i]));
  uint32_t end = 0;
  for (uint32_t i = 0; i < info->pushConstantRangeCount; i++) {
    l->push.push_back(info->pPushConstantRanges[i]);
    end = std::max(end, info->pPushConstantRanges[i].offset + info->pPushConstantRanges[i].size);
  }
  l->push_size = (end + 15) & ~15u;
  *pLayout = Handle<VkPipelineLayout>(l);
  return VK_SUCCESS;
}

void vkDestroyPipelineLayout(VkDevice, VkPipelineLayout layout, const VkAllocationCallbacks*) {
  (void)layout;  // pipelines keep pointing at it
}

// --- descriptor pools and sets --------------------------------------------------------------

DescriptorSet::Slot* DescriptorSet::Find(uint32_t binding, uint32_t index) {
  for (auto& s : slots)
    if (s.binding == binding && s.index == index) return &s;
  return nullptr;
}

VkResult vkCreateDescriptorPool(VkDevice, const VkDescriptorPoolCreateInfo*, const VkAllocationCallbacks*,
                                VkDescriptorPool* pPool) {
  *pPool = Handle<VkDescriptorPool>(new DescriptorPool());
  return VK_SUCCESS;
}

static void FreeSet(Device* dev, DescriptorSet* s) {
  if (s->page_id) dev->gpu->stream().Op(OP_DESTROY, {s->page_id});
  delete s;
}

VkResult vkResetDescriptorPool(VkDevice device, VkDescriptorPool pool, VkDescriptorPoolResetFlags) {
  Device* dev = Dev(device);
  DescriptorPool* p = As<DescriptorPool>(pool);
  std::lock_guard<std::mutex> l(dev->lock);
  for (DescriptorSet* s : p->sets) FreeSet(dev, s);
  p->sets.clear();
  return VK_SUCCESS;
}

void vkDestroyDescriptorPool(VkDevice device, VkDescriptorPool pool, const VkAllocationCallbacks*) {
  if (!pool) return;
  vkResetDescriptorPool(device, pool, 0);
  delete As<DescriptorPool>(pool);
}

VkResult vkAllocateDescriptorSets(VkDevice device, const VkDescriptorSetAllocateInfo* info, VkDescriptorSet* out) {
  Device* dev = Dev(device);
  DescriptorPool* p = As<DescriptorPool>(info->descriptorPool);
  std::lock_guard<std::mutex> l(dev->lock);
  for (uint32_t i = 0; i < info->descriptorSetCount; i++) {
    DescriptorSet* s = new DescriptorSet();
    s->layout = As<DescriptorSetLayout>(info->pSetLayouts[i]);
    for (const auto& b : s->layout->bindings) {
      for (uint32_t k = 0; k < b.count; k++) {
        DescriptorSet::Slot slot{b.binding, k, {}};
        slot.d.type = b.type;
        if (k < b.immutable.size()) slot.d.sampler = b.immutable[k];
        s->slots.push_back(slot);
      }
    }
    p->sets.push_back(s);
    out[i] = Handle<VkDescriptorSet>(s);
  }
  return VK_SUCCESS;
}

VkResult vkFreeDescriptorSets(VkDevice device, VkDescriptorPool pool, uint32_t n, const VkDescriptorSet* sets) {
  Device* dev = Dev(device);
  DescriptorPool* p = As<DescriptorPool>(pool);
  std::lock_guard<std::mutex> l(dev->lock);
  for (uint32_t i = 0; i < n; i++) {
    DescriptorSet* s = As<DescriptorSet>(sets[i]);
    if (!s) continue;
    p->sets.erase(std::remove(p->sets.begin(), p->sets.end(), s), p->sets.end());
    FreeSet(dev, s);
  }
  return VK_SUCCESS;
}

namespace {

// Writes one descriptor at (binding, element), moving on to the next binding
// when the element runs past the end (the spec's consecutive updates).
void WriteDescriptor(DescriptorSet* s, uint32_t* binding, uint32_t* element, const Descriptor& d) {
  const DescriptorSetLayout::Binding* b = s->layout->Find(*binding);
  while (b && *element >= b->count) {
    *element -= b->count;
    ++*binding;
    b = s->layout->Find(*binding);
  }
  DescriptorSet::Slot* slot = b ? s->Find(*binding, *element) : nullptr;
  if (slot) {
    Sampler* immutable = slot->d.sampler && *element < b->immutable.size() ? b->immutable[*element] : nullptr;
    slot->d = d;
    slot->d.type = b->type;
    if (immutable) slot->d.sampler = immutable;
  }
  ++*element;
  s->dirty = true;
}

}  // namespace

void vkUpdateDescriptorSets(VkDevice device, uint32_t nw, const VkWriteDescriptorSet* writes, uint32_t nc,
                            const VkCopyDescriptorSet* copies) {
  Device* dev = Dev(device);
  std::lock_guard<std::mutex> l(dev->lock);
  for (uint32_t i = 0; i < nw; i++) {
    const VkWriteDescriptorSet& w = writes[i];
    DescriptorSet* s = As<DescriptorSet>(w.dstSet);
    uint32_t binding = w.dstBinding, element = w.dstArrayElement;
    for (uint32_t k = 0; k < w.descriptorCount; k++) {
      Descriptor d;
      switch (w.descriptorType) {
        case VK_DESCRIPTOR_TYPE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
        case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
          d.view = As<ImageView>(w.pImageInfo[k].imageView);
          d.sampler = As<Sampler>(w.pImageInfo[k].sampler);
          break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
          d.texel = As<BufferView>(w.pTexelBufferView[k]);
          break;
        default:
          d.buffer = As<Buffer>(w.pBufferInfo[k].buffer);
          d.offset = w.pBufferInfo[k].offset;
          d.range = w.pBufferInfo[k].range;
          break;
      }
      WriteDescriptor(s, &binding, &element, d);
    }
  }
  for (uint32_t i = 0; i < nc; i++) {
    const VkCopyDescriptorSet& c = copies[i];
    DescriptorSet* src = As<DescriptorSet>(c.srcSet);
    DescriptorSet* dst = As<DescriptorSet>(c.dstSet);
    uint32_t sb = c.srcBinding, se = c.srcArrayElement, db = c.dstBinding, de = c.dstArrayElement;
    for (uint32_t k = 0; k < c.descriptorCount; k++) {
      const DescriptorSetLayout::Binding* b = src->layout->Find(sb);
      while (b && se >= b->count) {
        se -= b->count;
        b = src->layout->Find(++sb);
      }
      DescriptorSet::Slot* from = b ? src->Find(sb, se) : nullptr;
      se++;
      if (from) WriteDescriptor(dst, &db, &de, from->d);
    }
  }
}

VkResult vkCreateDescriptorUpdateTemplate(VkDevice, const VkDescriptorUpdateTemplateCreateInfo* info,
                                          const VkAllocationCallbacks*, VkDescriptorUpdateTemplate* out) {
  DescriptorUpdateTemplate* t = new DescriptorUpdateTemplate();
  t->entries.assign(info->pDescriptorUpdateEntries, info->pDescriptorUpdateEntries + info->descriptorUpdateEntryCount);
  if (info->templateType == VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR)
    Unsupported("push descriptor templates");
  *out = Handle<VkDescriptorUpdateTemplate>(t);
  return VK_SUCCESS;
}

void vkDestroyDescriptorUpdateTemplate(VkDevice, VkDescriptorUpdateTemplate t, const VkAllocationCallbacks*) {
  delete As<DescriptorUpdateTemplate>(t);
}

void vkUpdateDescriptorSetWithTemplate(VkDevice device, VkDescriptorSet set, VkDescriptorUpdateTemplate tmpl,
                                       const void* pData) {
  // The template's data holds VkDescriptorImageInfo, VkDescriptorBufferInfo
  // and VkBufferView, laid out alike in both guest ABIs.
  DescriptorUpdateTemplate* t = As<DescriptorUpdateTemplate>(tmpl);
  std::vector<VkWriteDescriptorSet> writes;
  std::vector<std::vector<uint8_t>> storage;
  for (const auto& e : t->entries) {
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.dstBinding = e.dstBinding;
    w.dstArrayElement = e.dstArrayElement;
    w.descriptorCount = e.descriptorCount;
    w.descriptorType = e.descriptorType;
    const uint8_t* base = static_cast<const uint8_t*>(pData) + e.offset;
    size_t elem = IsBuffer(e.descriptorType) ? sizeof(VkDescriptorBufferInfo)
                  : (e.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER ||
                     e.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER)
                      ? sizeof(VkBufferView)
                      : sizeof(VkDescriptorImageInfo);
    storage.emplace_back(elem * e.descriptorCount);
    for (uint32_t k = 0; k < e.descriptorCount; k++)
      memcpy(storage.back().data() + k * elem, base + k * e.stride, elem);
    const void* p = storage.back().data();
    if (IsBuffer(e.descriptorType)) w.pBufferInfo = static_cast<const VkDescriptorBufferInfo*>(p);
    else if (elem == sizeof(VkBufferView)) w.pTexelBufferView = static_cast<const VkBufferView*>(p);
    else w.pImageInfo = static_cast<const VkDescriptorImageInfo*>(p);
    writes.push_back(w);
  }
  vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

uint32_t SyncDescriptorSet(Device* dev, DescriptorSet* s) {
  if (!s->dirty && s->page_id) return s->page_id;
  OpList& st = dev->gpu->stream();
  if (s->page_id) st.Op(OP_DESTROY, {s->page_id});
  s->page_id = dev->NewId();
  s->dirty = false;
  Json j;
  j.Open('[');
  for (auto& slot : s->slots) {
    if (slot.index != 0) continue;  // arrays: the first element only (see the layout's note)
    const Descriptor& d = slot.d;
    j.Open('{').Key("binding").Int(slot.binding).Key("kind").Str(DescriptorKind(d.type));
    if (IsBuffer(d.type)) {
      if (d.buffer && d.buffer->memory) {
        VkDeviceSize base = 0;
        uint32_t id = BufferId(d.buffer, &base);
        VkDeviceSize size = d.range == VK_WHOLE_SIZE ? d.buffer->size - d.offset : d.range;
        j.Key("buffer").Int(id).Key("offset").Int(static_cast<long long>(base + d.offset));
        j.Key("size").Int(static_cast<long long>(size));
      }
    } else {
      if (d.view) j.Key("view").Int(d.view->view_id);
      if (d.sampler) j.Key("sampler").Int(d.sampler->sampler_id);
    }
    j.Close('}');
  }
  j.Close(']');
  st.Begin(OP_SET_DESCRIPTORS);
  st.U32(s->page_id);
  st.Str(j.str());
  st.End();
  return s->page_id;
}

// --- render passes and framebuffers -------------------------------------------------------------

VkResult vkCreateRenderPass(VkDevice, const VkRenderPassCreateInfo* info, const VkAllocationCallbacks*,
                            VkRenderPass* pPass) {
  RenderPass* rp = new RenderPass();
  rp->attachments.assign(info->pAttachments, info->pAttachments + info->attachmentCount);
  rp->first_use.assign(info->attachmentCount, -1);
  rp->last_use.assign(info->attachmentCount, -1);
  for (uint32_t i = 0; i < info->subpassCount; i++) {
    const VkSubpassDescription& sd = info->pSubpasses[i];
    RenderPass::Subpass sp;
    sp.color.assign(sd.pColorAttachments, sd.pColorAttachments + sd.colorAttachmentCount);
    if (sd.pResolveAttachments)
      sp.resolve.assign(sd.pResolveAttachments, sd.pResolveAttachments + sd.colorAttachmentCount);
    sp.input.assign(sd.pInputAttachments, sd.pInputAttachments + sd.inputAttachmentCount);
    if (sd.pDepthStencilAttachment) sp.depth = *sd.pDepthStencilAttachment;
    auto use = [&](uint32_t a) {
      if (a == VK_ATTACHMENT_UNUSED || a >= info->attachmentCount) return;
      if (rp->first_use[a] < 0) rp->first_use[a] = static_cast<int>(i);
      rp->last_use[a] = static_cast<int>(i);
    };
    for (auto& r : sp.color) use(r.attachment);
    for (auto& r : sp.resolve) use(r.attachment);
    for (auto& r : sp.input) use(r.attachment);
    use(sp.depth.attachment);
    rp->subpasses.push_back(sp);
  }
  *pPass = Handle<VkRenderPass>(rp);
  return VK_SUCCESS;
}

void vkDestroyRenderPass(VkDevice, VkRenderPass pass, const VkAllocationCallbacks*) {
  (void)pass;  // pipelines made for it keep reading its formats
}

void vkGetRenderAreaGranularity(VkDevice, VkRenderPass, VkExtent2D* out) { *out = {1, 1}; }

VkResult vkCreateFramebuffer(VkDevice, const VkFramebufferCreateInfo* info, const VkAllocationCallbacks*,
                             VkFramebuffer* pFb) {
  Framebuffer* fb = new Framebuffer();
  fb->pass = As<RenderPass>(info->renderPass);
  for (uint32_t i = 0; i < info->attachmentCount; i++) fb->views.push_back(As<ImageView>(info->pAttachments[i]));
  fb->width = info->width;
  fb->height = info->height;
  fb->layers = info->layers;
  *pFb = Handle<VkFramebuffer>(fb);
  return VK_SUCCESS;
}

void vkDestroyFramebuffer(VkDevice, VkFramebuffer fb, const VkAllocationCallbacks*) { delete As<Framebuffer>(fb); }

// --- pipelines -----------------------------------------------------------------------------

namespace {

const char* Topology(VkPrimitiveTopology t, bool* fan) {
  *fan = false;
  switch (t) {
    case VK_PRIMITIVE_TOPOLOGY_POINT_LIST: return "point-list";
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST: return "line-list";
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP: return "line-strip";
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP: return "triangle-strip";
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:
      *fan = true;  // drawn as a list through an index list of the page's
      return "triangle-list";
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST: return "triangle-list";
    default:
      Unsupported("primitive topology %d", t);
      return "triangle-list";
  }
}

const char* BlendFactor(VkBlendFactor f) {
  switch (f) {
    case VK_BLEND_FACTOR_ZERO: return "zero";
    case VK_BLEND_FACTOR_ONE: return "one";
    case VK_BLEND_FACTOR_SRC_COLOR: return "src";
    case VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR: return "one-minus-src";
    case VK_BLEND_FACTOR_DST_COLOR: return "dst";
    case VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR: return "one-minus-dst";
    case VK_BLEND_FACTOR_SRC_ALPHA: return "src-alpha";
    case VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA: return "one-minus-src-alpha";
    case VK_BLEND_FACTOR_DST_ALPHA: return "dst-alpha";
    case VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA: return "one-minus-dst-alpha";
    case VK_BLEND_FACTOR_CONSTANT_COLOR: return "constant";
    case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR: return "one-minus-constant";
    case VK_BLEND_FACTOR_CONSTANT_ALPHA: return "constant";
    case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA: return "one-minus-constant";
    case VK_BLEND_FACTOR_SRC_ALPHA_SATURATE: return "src-alpha-saturated";
    case VK_BLEND_FACTOR_SRC1_COLOR: return "src1";
    case VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR: return "one-minus-src1";
    case VK_BLEND_FACTOR_SRC1_ALPHA: return "src1-alpha";
    case VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA: return "one-minus-src1-alpha";
    default: return "one";
  }
}

const char* BlendOp(VkBlendOp op) {
  switch (op) {
    case VK_BLEND_OP_SUBTRACT: return "subtract";
    case VK_BLEND_OP_REVERSE_SUBTRACT: return "reverse-subtract";
    case VK_BLEND_OP_MIN: return "min";
    case VK_BLEND_OP_MAX: return "max";
    default: return "add";
  }
}

const char* StencilOp(VkStencilOp op) {
  static const char* kNames[] = {"keep", "zero", "replace", "increment-clamp", "decrement-clamp", "invert",
                                 "increment-wrap", "decrement-wrap"};
  return op <= VK_STENCIL_OP_DECREMENT_AND_WRAP ? kNames[op] : "keep";
}

void StencilFace(Json& j, const VkStencilOpState& s, bool enabled) {
  j.Open('{');
  j.Key("compare").Str(enabled ? CompareFunction(s.compareOp) : "always");
  j.Key("failOp").Str(enabled ? StencilOp(s.failOp) : "keep");
  j.Key("depthFailOp").Str(enabled ? StencilOp(s.depthFailOp) : "keep");
  j.Key("passOp").Str(enabled ? StencilOp(s.passOp) : "keep");
  j.Close('}');
}

// The bind group plan of |layout|: per group, the bindings and their kinds
// (what the page joins with the WGSL's declarations).
void LayoutJson(Json& j, const PipelineLayout* layout, int push_group) {
  j.Key("groups").Open('[');
  size_t groups = layout->sets.size();
  if (push_group >= 0) groups = std::max(groups, static_cast<size_t>(push_group) + 1);
  for (size_t g = 0; g < groups; g++) {
    j.Open('[');
    if (g < layout->sets.size() && layout->sets[g]) {
      for (const auto& b : layout->sets[g]->bindings) {
        j.Open('{').Key("binding").Int(b.binding).Key("kind").Str(DescriptorKind(b.type));
        j.Key("dynamic").Bool(IsDynamic(b.type)).Key("visibility").Int(Visibility(b.stages));
        if (b.type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
          j.Key("samplerBinding").Int(b.binding + kSamplerBindingOffset);
        j.Close('}');
      }
    }
    if (static_cast<int>(g) == push_group) {
      j.Open('{').Key("binding").Int(kPushBinding).Key("kind").Str("push");
      j.Key("size").Int(layout->push_size).Key("visibility").Int(7).Close('}');
    }
    j.Close(']');
  }
  j.Close(']');
  j.Key("pushGroup").Int(push_group);
}

int PushGroup(const PipelineLayout* layout) {
  if (!layout->push_size) return -1;
  return static_cast<int>(std::min<size_t>(layout->sets.size(), 3));
}

// Prepares a stage's SPIR-V and sends it as a shader; its id, 0 on failure.
uint32_t MakeShader(Device* dev, const VkPipelineShaderStageCreateInfo& st, const PipelineLayout* layout,
                    std::string* entry_out, bool flip_y = true) {
  ShaderModule* mod = As<ShaderModule>(st.module);
  SpirvOptions o;
  o.stage = st.stage;
  o.entry = st.pName ? st.pName : "main";
  o.spec = st.pSpecializationInfo;
  o.push_group = PushGroup(layout);
  o.push_binding = kPushBinding;
  o.flip_y = flip_y;
  std::vector<uint32_t> spirv;
  SpirvInfo info;
  std::string error;
  if (!PrepareSpirv(mod->code, o, &spirv, &info, &error)) {
    Log("shader (%s): %s", o.entry.c_str(), error.c_str());
    return 0;
  }
  *entry_out = o.entry;
  Json j;
  j.Open('{');
  j.Key("stage").Str(st.stage == VK_SHADER_STAGE_VERTEX_BIT     ? "vertex"
                     : st.stage == VK_SHADER_STAGE_FRAGMENT_BIT ? "fragment"
                                                                : "compute");
  j.Key("entry").Str(o.entry.c_str());
  // Where Tint puts the sampler half of each combined image sampler.
  j.Key("samplers").Open('[');
  for (auto& c : info.combined) {
    j.Open('[').Int(c.first).Int(c.second).Int(c.first).Int(c.second + kSamplerBindingOffset).Close(']');
  }
  j.Close(']');
  j.Key("written").Open('[');
  for (auto& w : info.written_storage) j.Open('[').Int(w.first).Int(w.second).Close(']');
  j.Close(']');
  j.Close('}');
  uint32_t id = dev->NewId();
  OpList& s = dev->gpu->stream();
  s.Begin(OP_CREATE_SHADER);
  s.U32(id);
  s.Str(j.str());
  s.Bytes(spirv.data(), spirv.size() * 4);
  s.End();
  return id;
}

}  // namespace

VkResult vkCreateGraphicsPipelines(VkDevice device, VkPipelineCache, uint32_t n,
                                   const VkGraphicsPipelineCreateInfo* infos, const VkAllocationCallbacks*,
                                   VkPipeline* out) {
  Device* dev = Dev(device);
  VkResult result = VK_SUCCESS;
  std::lock_guard<std::mutex> l(dev->lock);
  for (uint32_t i = 0; i < n; i++) {
    const VkGraphicsPipelineCreateInfo& ci = infos[i];
    Pipeline* p = new Pipeline();
    p->device = dev;
    p->bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS;
    p->layout = As<PipelineLayout>(ci.layout);
    if (ci.pDynamicState) {
      for (uint32_t k = 0; k < ci.pDynamicState->dynamicStateCount; k++) {
        VkDynamicState d = ci.pDynamicState->pDynamicStates[k];
        if (d <= VK_DYNAMIC_STATE_STENCIL_REFERENCE) p->dynamic |= 1u << d;
        else Unsupported("dynamic state %d", d);
      }
    }
    Json j;
    j.Open('{').Key("kind").Str("render");
    int push_group = PushGroup(p->layout);
    LayoutJson(j, p->layout, push_group);
    bool ok = true;
    for (uint32_t s = 0; s < ci.stageCount; s++) {
      const VkPipelineShaderStageCreateInfo& st = ci.pStages[s];
      if (st.stage != VK_SHADER_STAGE_VERTEX_BIT && st.stage != VK_SHADER_STAGE_FRAGMENT_BIT) {
        Unsupported("shader stage 0x%x (geometry and tessellation)", st.stage);
        continue;
      }
      std::string entry;
      uint32_t id = MakeShader(dev, st, p->layout, &entry);
      if (!id) ok = false;
      j.Key(st.stage == VK_SHADER_STAGE_VERTEX_BIT ? "vertex" : "fragment").Open('{');
      j.Key("shader").Int(id).Key("entry").Str(entry.c_str());
      // Without the y flip, for negative viewport heights (VK_KHR_maintenance1:
      // the viewport flips instead).
      if (st.stage == VK_SHADER_STAGE_VERTEX_BIT) j.Key("unflipped").Int(MakeShader(dev, st, p->layout, &entry, false));
      j.Close('}');
    }
    // Vertex input: Vulkan binding n is WebGPU's buffer slot n.
    const VkPipelineVertexInputStateCreateInfo* vi = ci.pVertexInputState;
    j.Key("buffers").Open('[');
    uint32_t slots = 0;
    if (vi) {
      for (uint32_t b = 0; b < vi->vertexBindingDescriptionCount; b++)
        slots = std::max(slots, vi->pVertexBindingDescriptions[b].binding + 1);
    }
    p->vertex_buffer_count = slots;
    for (uint32_t slot = 0; slot < slots; slot++) {
      const VkVertexInputBindingDescription* bd = nullptr;
      for (uint32_t b = 0; b < vi->vertexBindingDescriptionCount; b++)
        if (vi->pVertexBindingDescriptions[b].binding == slot) bd = &vi->pVertexBindingDescriptions[b];
      if (!bd) {
        j.Null();
        continue;
      }
      if (bd->stride % 4) Unsupported("vertex strides that are not a multiple of 4");
      if (slot < 16) p->vertex_strides[slot] = bd->stride;
      j.Open('{').Key("arrayStride").Int(bd->stride);
      j.Key("stepMode").Str(bd->inputRate == VK_VERTEX_INPUT_RATE_INSTANCE ? "instance" : "vertex");
      j.Key("attributes").Open('[');
      for (uint32_t a = 0; a < vi->vertexAttributeDescriptionCount; a++) {
        const VkVertexInputAttributeDescription& ad = vi->pVertexAttributeDescriptions[a];
        if (ad.binding != slot) continue;
        const char* f = VertexFormat(ad.format);
        if (!f) {
          Unsupported("vertex format %d", ad.format);
          f = "float32x4";
        }
        j.Open('{').Key("format").Str(f).Key("offset").Int(ad.offset).Key("shaderLocation").Int(ad.location);
        j.Close('}');
      }
      j.Close(']').Close('}');
    }
    j.Close(']');
    // Primitive and rasterization.
    const VkPipelineInputAssemblyStateCreateInfo* ia = ci.pInputAssemblyState;
    const VkPipelineRasterizationStateCreateInfo* rs = ci.pRasterizationState;
    bool fan = false;
    j.Key("primitive").Open('{');
    j.Key("topology").Str(Topology(ia ? ia->topology : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, &fan));
    p->triangle_fan = fan;
    j.Key("fan").Bool(fan);
    j.Key("restart").Bool(ia && ia->primitiveRestartEnable);
    if (rs) {
      j.Key("frontFace").Str(rs->frontFace == VK_FRONT_FACE_CLOCKWISE ? "cw" : "ccw");
      j.Key("cullMode").Str(rs->cullMode == VK_CULL_MODE_FRONT_BIT ? "front"
                            : rs->cullMode == VK_CULL_MODE_BACK_BIT ? "back"
                                                                    : "none");
      if (rs->cullMode == VK_CULL_MODE_FRONT_AND_BACK) Unsupported("culling both faces");
      if (rs->depthClampEnable && dev->physical->HasFeature("depth-clip-control"))
        j.Key("unclippedDepth").Bool(true);
      if (rs->polygonMode != VK_POLYGON_MODE_FILL) Unsupported("polygon modes other than fill");
    }
    j.Close('}');
    j.Key("discard").Bool(rs && rs->rasterizerDiscardEnable);
    // The subpass: attachment formats.
    RenderPass* rp = As<RenderPass>(ci.renderPass);
    const RenderPass::Subpass* sp = rp && ci.subpass < rp->subpasses.size() ? &rp->subpasses[ci.subpass] : nullptr;
    uint32_t samples = 1;
    j.Key("colors").Open('[');
    if (sp) {
      for (const auto& c : sp->color) {
        if (c.attachment == VK_ATTACHMENT_UNUSED) {
          j.Null();
          continue;
        }
        const FormatInfo* fi = GetFormat(rp->attachments[c.attachment].format);
        samples = rp->attachments[c.attachment].samples;
        j.Str(fi && fi->gpu ? fi->gpu : "rgba8unorm");
      }
    }
    j.Close(']');
    const FormatInfo* depth_fmt = nullptr;
    if (sp && sp->depth.attachment != VK_ATTACHMENT_UNUSED) {
      depth_fmt = GetFormat(rp->attachments[sp->depth.attachment].format);
      samples = rp->attachments[sp->depth.attachment].samples;
    }
    // Depth and stencil.
    const VkPipelineDepthStencilStateCreateInfo* ds = ci.pDepthStencilState;
    if (depth_fmt && depth_fmt->gpu) {
      j.Key("depthStencil").Open('{').Key("format").Str(depth_fmt->gpu);
      bool test = ds && ds->depthTestEnable;
      j.Key("depthWriteEnabled").Bool(ds && ds->depthTestEnable && ds->depthWriteEnable);
      j.Key("depthCompare").Str(test ? CompareFunction(ds->depthCompareOp) : "always");
      bool stencil = ds && ds->stencilTestEnable;
      j.Key("stencilFront");
      StencilFace(j, ds ? ds->front : VkStencilOpState{}, stencil);
      j.Key("stencilBack");
      StencilFace(j, ds ? ds->back : VkStencilOpState{}, stencil);
      j.Key("stencilReadMask").Int(stencil ? ds->front.compareMask : 0xff);
      j.Key("stencilWriteMask").Int(stencil ? ds->front.writeMask : 0);
      if (rs && rs->depthBiasEnable) {
        j.Key("depthBias").Num(rs->depthBiasConstantFactor).Key("depthBiasSlopeScale").Num(rs->depthBiasSlopeFactor);
        j.Key("depthBiasClamp").Num(rs->depthBiasClamp);
        j.Key("biasEnabled").Bool(true);
      }
      if (ds && ds->depthBoundsTestEnable) Unsupported("depth bounds tests");
      j.Close('}');
    }
    // Multisampling.
    const VkPipelineMultisampleStateCreateInfo* ms = ci.pMultisampleState;
    j.Key("multisample").Open('{').Key("count").Int(samples);
    if (ms && ms->pSampleMask) j.Key("mask").Int(ms->pSampleMask[0]);
    j.Key("alphaToCoverageEnabled").Bool(ms && ms->alphaToCoverageEnable && samples > 1);
    j.Close('}');
    // Blending.
    const VkPipelineColorBlendStateCreateInfo* cb = ci.pColorBlendState;
    j.Key("targets").Open('[');
    if (sp) {
      for (size_t c = 0; c < sp->color.size(); c++) {
        if (sp->color[c].attachment == VK_ATTACHMENT_UNUSED) {
          j.Null();
          continue;
        }
        const VkPipelineColorBlendAttachmentState* a =
            cb && c < cb->attachmentCount ? &cb->pAttachments[c] : nullptr;
        j.Open('{').Key("writeMask").Int(a ? a->colorWriteMask : 0xf);
        if (a && a->blendEnable) {
          bool cmm = a->colorBlendOp == VK_BLEND_OP_MIN || a->colorBlendOp == VK_BLEND_OP_MAX;
          bool amm = a->alphaBlendOp == VK_BLEND_OP_MIN || a->alphaBlendOp == VK_BLEND_OP_MAX;
          j.Key("blend").Open('{');
          j.Key("color").Open('{').Key("operation").Str(BlendOp(a->colorBlendOp));
          j.Key("srcFactor").Str(cmm ? "one" : BlendFactor(a->srcColorBlendFactor));
          j.Key("dstFactor").Str(cmm ? "one" : BlendFactor(a->dstColorBlendFactor)).Close('}');
          j.Key("alpha").Open('{').Key("operation").Str(BlendOp(a->alphaBlendOp));
          j.Key("srcFactor").Str(amm ? "one" : BlendFactor(a->srcAlphaBlendFactor));
          j.Key("dstFactor").Str(amm ? "one" : BlendFactor(a->dstAlphaBlendFactor)).Close('}');
          j.Close('}');
        }
        j.Close('}');
      }
    }
    j.Close(']');
    if (cb && cb->logicOpEnable) Unsupported("blend logic ops");
    // State the pipeline fixes where it is not dynamic.
    const VkPipelineViewportStateCreateInfo* vp = ci.pViewportState;
    if (vp && vp->pViewports && vp->viewportCount && !(p->dynamic & (1u << VK_DYNAMIC_STATE_VIEWPORT))) {
      p->viewport = vp->pViewports[0];
      p->has_viewport = true;
    }
    if (vp && vp->pScissors && vp->scissorCount && !(p->dynamic & (1u << VK_DYNAMIC_STATE_SCISSOR))) {
      p->scissor = vp->pScissors[0];
      p->has_scissor = true;
    }
    if (vp && vp->viewportCount > 1) Unsupported("more than one viewport");
    if (cb) memcpy(p->blend_constants, cb->blendConstants, sizeof p->blend_constants);
    if (ds) {
      p->stencil_ref = ds->front.reference;
      p->stencil_compare[0] = ds->front.compareMask;
      p->stencil_compare[1] = ds->back.compareMask;
      p->stencil_write[0] = ds->front.writeMask;
      p->stencil_write[1] = ds->back.writeMask;
    }
    if (rs) {
      p->depth_bias[0] = rs->depthBiasEnable ? rs->depthBiasConstantFactor : 0;
      p->depth_bias[1] = rs->depthBiasEnable ? rs->depthBiasClamp : 0;
      p->depth_bias[2] = rs->depthBiasEnable ? rs->depthBiasSlopeFactor : 0;
    }
    j.Key("dynamic").Int(p->dynamic);
    j.Close('}');
    p->pipeline_id = dev->NewId();
    OpList& s = dev->gpu->stream();
    s.Begin(OP_CREATE_PIPELINE);
    s.U32(p->pipeline_id);
    s.Str(j.str());
    s.End();
    if (!ok) result = VK_SUCCESS;  // the page logs; Vulkan apps rarely survive a failed create
    out[i] = Handle<VkPipeline>(p);
    if (TraceLevel()) Log("graphics pipeline %u: %s", p->pipeline_id, j.str().c_str());
  }
  return result;
}

VkResult vkCreateComputePipelines(VkDevice device, VkPipelineCache, uint32_t n,
                                  const VkComputePipelineCreateInfo* infos, const VkAllocationCallbacks*,
                                  VkPipeline* out) {
  Device* dev = Dev(device);
  std::lock_guard<std::mutex> l(dev->lock);
  for (uint32_t i = 0; i < n; i++) {
    const VkComputePipelineCreateInfo& ci = infos[i];
    Pipeline* p = new Pipeline();
    p->device = dev;
    p->bind_point = VK_PIPELINE_BIND_POINT_COMPUTE;
    p->layout = As<PipelineLayout>(ci.layout);
    Json j;
    j.Open('{').Key("kind").Str("compute");
    LayoutJson(j, p->layout, PushGroup(p->layout));
    std::string entry;
    uint32_t id = MakeShader(dev, ci.stage, p->layout, &entry);
    j.Key("compute").Open('{').Key("shader").Int(id).Key("entry").Str(entry.c_str()).Close('}');
    j.Close('}');
    p->pipeline_id = dev->NewId();
    OpList& s = dev->gpu->stream();
    s.Begin(OP_CREATE_PIPELINE);
    s.U32(p->pipeline_id);
    s.Str(j.str());
    s.End();
    out[i] = Handle<VkPipeline>(p);
  }
  return VK_SUCCESS;
}

void vkDestroyPipeline(VkDevice device, VkPipeline pipeline, const VkAllocationCallbacks*) {
  Pipeline* p = As<Pipeline>(pipeline);
  if (!p) return;
  Device* dev = Dev(device);
  {
    std::lock_guard<std::mutex> l(dev->lock);
    dev->gpu->stream().Op(OP_DESTROY, {p->pipeline_id});
  }
  delete p;
}

}  // namespace arcvk
