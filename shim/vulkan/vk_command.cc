// Command pools and buffers (vk_icd.h). A command buffer records op lists
// (vk_gpu.h) that every submit replays on the page: WebGPU command buffers
// are single-use, Vulkan's are not. State the page's encoders forget at
// pass boundaries (pipeline, bindings, viewport...) is sent again lazily,
// just before the draw or dispatch that needs it.

#include <stdio.h>
#include <string.h>

#include <algorithm>

#include "vk_icd.h"

namespace arcvk {
namespace {

CommandBuffer* CB(VkCommandBuffer c) { return reinterpret_cast<CommandBuffer*>(c); }

uint32_t Bits(float f) {
  uint32_t v;
  memcpy(&v, &f, 4);
  return v;
}

const char* AspectName(VkImageAspectFlags a, const FormatInfo* fi) {
  if (a == VK_IMAGE_ASPECT_DEPTH_BIT && (fi->aspects & VK_IMAGE_ASPECT_STENCIL_BIT)) return "depth-only";
  if (a == VK_IMAGE_ASPECT_STENCIL_BIT && (fi->aspects & VK_IMAGE_ASPECT_DEPTH_BIT)) return "stencil-only";
  return "all";
}

void MarkAllDirty(CommandBuffer* cb) {
  cb->sent_pipeline = nullptr;
  cb->sets_dirty[0] = cb->sets_dirty[1] = 0xf;
  cb->vertex_dirty = 0xffff;
  cb->index_dirty = true;
  cb->push_dirty = true;
  cb->dyn_dirty = true;
  cb->viewport_dirty = cb->has_viewport;
  cb->scissor_dirty = cb->has_scissor;
  cb->blend_dirty = cb->stencil_ref_dirty = true;
}

// Notes what the descriptors of |set| read and write in host-visible memory.
void NoteSet(CommandBuffer* cb, DescriptorSet* set, const std::vector<uint32_t>& dynamic) {
  size_t dyn = 0;
  for (auto& slot : set->slots) {
    const Descriptor& d = slot.d;
    bool is_dyn = d.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
                  d.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
    VkDeviceSize extra = 0;
    if (is_dyn) extra = dyn < dynamic.size() ? dynamic[dyn++] : 0;
    if (!d.buffer || !d.buffer->memory || !d.buffer->memory->HostVisible()) continue;
    VkDeviceSize size = d.range == VK_WHOLE_SIZE ? d.buffer->size - d.offset : d.range;
    NoteRead(cb, d.buffer, d.offset + extra, size);
    if (d.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || d.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)
      NoteWrite(cb, d.buffer, d.offset + extra, size);
  }
}

// Sends what the next draw (bp 0) or dispatch (bp 1) needs.
void FlushState(CommandBuffer* cb, int bp) {
  Device* dev = cb->device;
  OpList& o = cb->ops;
  Pipeline* p = bp ? cb->compute : cb->graphics;
  if (!p) return;
  if (cb->sent_pipeline != p) {
    o.Op(CMD_SET_PIPELINE, {p->pipeline_id});
    cb->sent_pipeline = p;
    cb->sets_dirty[bp] = 0xf;
    cb->push_dirty = true;
  }
  if (bp == 0) {
    if (cb->viewport_dirty && cb->has_viewport) {
      const VkViewport& v = cb->viewport;
      float y = v.y, h = v.height;
      uint32_t flipped = 0;
      if (h < 0) {
        // The viewport turns the picture over: the vertex shader must not (the
        // page takes the pipeline variant without the y flip).
        y += h;
        h = -h;
        flipped = 1;
      }
      o.Op(CMD_SET_VIEWPORT, {Bits(v.x), Bits(y), Bits(v.width), Bits(h), Bits(v.minDepth), Bits(v.maxDepth),
                              flipped});
      cb->viewport_dirty = false;
    }
    if (cb->scissor_dirty && cb->has_scissor) {
      const VkRect2D& s = cb->scissor;
      o.Op(CMD_SET_SCISSOR, {static_cast<uint32_t>(std::max(0, s.offset.x)),
                             static_cast<uint32_t>(std::max(0, s.offset.y)), s.extent.width, s.extent.height});
      cb->scissor_dirty = false;
    }
    if (cb->blend_dirty) {
      o.Op(CMD_SET_BLEND_CONSTANT, {Bits(cb->blend_constants[0]), Bits(cb->blend_constants[1]),
                                    Bits(cb->blend_constants[2]), Bits(cb->blend_constants[3])});
      cb->blend_dirty = false;
    }
    if (cb->stencil_ref_dirty) {
      o.Op(CMD_SET_STENCIL_REF, {cb->stencil_ref[0], cb->stencil_ref[1]});
      cb->stencil_ref_dirty = false;
    }
    if (cb->dyn_dirty) {
      o.Op(CMD_DYNAMIC_STATE, {Bits(cb->depth_bias[0]), Bits(cb->depth_bias[1]), Bits(cb->depth_bias[2]),
                               cb->stencil_compare[0], cb->stencil_compare[1], cb->stencil_write[0],
                               cb->stencil_write[1]});
      cb->dyn_dirty = false;
    }
    for (uint32_t slot = 0; slot < p->vertex_buffer_count && slot < 16; slot++) {
      if (!(cb->vertex_dirty & (1u << slot)) || !cb->vertex[slot]) continue;
      Buffer* b = cb->vertex[slot];
      VkDeviceSize base = 0;
      uint32_t id;
      {
        std::lock_guard<std::mutex> l(dev->lock);
        id = BufferId(b, &base);
      }
      VkDeviceSize off = cb->vertex_offset[slot];
      VkDeviceSize size = off < b->size ? b->size - off : 0;
      o.Op(CMD_SET_VERTEX_BUFFER, {slot, id, static_cast<uint32_t>(base + off), static_cast<uint32_t>(size)});
      NoteRead(cb, b, off, size);
      cb->vertex_dirty &= ~(1u << slot);
    }
  }
  PipelineLayout* layout = p->layout;
  for (uint32_t s = 0; layout && s < layout->sets.size() && s < 4; s++) {
    if (!(cb->sets_dirty[bp] & (1u << s))) continue;
    CommandBuffer::BoundSet& bs = cb->sets[bp][s];
    if (!bs.set) continue;
    uint32_t id;
    {
      std::lock_guard<std::mutex> l(dev->lock);
      id = SyncDescriptorSet(dev, bs.set);
    }
    o.Begin(CMD_BIND_SET);
    o.U32(bp);
    o.U32(s);
    o.U32(id);
    o.U32(static_cast<uint32_t>(bs.dynamic.size()));
    for (uint32_t d : bs.dynamic) o.U32(d);
    o.End();
    NoteSet(cb, bs.set, bs.dynamic);
  }
  cb->sets_dirty[bp] = 0;
  if (cb->push_dirty && layout && layout->push_size) {
    o.Begin(CMD_PUSH_CONSTANTS);
    o.U32(0);
    o.U32(layout->push_size);
    for (uint32_t i = 0; i < layout->push_size / 4; i++) {
      uint32_t w;
      memcpy(&w, cb->push + 4 * i, 4);
      o.U32(w);
    }
    o.End();
    cb->push_dirty = false;
  }
}

void FlushIndex(CommandBuffer* cb) {
  if (!cb->index_dirty || !cb->index) return;
  Buffer* b = cb->index;
  VkDeviceSize base = 0;
  uint32_t id;
  {
    std::lock_guard<std::mutex> l(cb->device->lock);
    id = BufferId(b, &base);
  }
  VkDeviceSize size = cb->index_offset < b->size ? b->size - cb->index_offset : 0;
  cb->ops.Op(CMD_SET_INDEX_BUFFER, {id, cb->index_type == VK_INDEX_TYPE_UINT32 ? 32u : 16u,
                                    static_cast<uint32_t>(base + cb->index_offset), static_cast<uint32_t>(size)});
  NoteRead(cb, b, cb->index_offset, size);
  cb->index_dirty = false;
}

// The render pass op of |cb|'s current subpass.
void BeginSubpass(CommandBuffer* cb) {
  RenderPass* rp = cb->pass;
  Framebuffer* fb = cb->framebuffer;
  const RenderPass::Subpass& sp = rp->subpasses[cb->subpass];
  int sub = static_cast<int>(cb->subpass);
  std::string j = "{\"colors\":[";
  char buf[512];
  for (size_t c = 0; c < sp.color.size(); c++) {
    if (c) j += ",";
    uint32_t a = sp.color[c].attachment;
    if (a == VK_ATTACHMENT_UNUSED || a >= fb->views.size()) {
      j += "null";
      continue;
    }
    const VkAttachmentDescription& ad = rp->attachments[a];
    bool first = rp->first_use[a] == sub, last = rp->last_use[a] == sub;
    const char* load = !first || ad.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD ? "load" : "clear";
    const char* store = !last || ad.storeOp == VK_ATTACHMENT_STORE_OP_STORE ? "store" : "discard";
    float clear[4] = {0, 0, 0, 0};
    if (first && ad.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR && a < cb->clears.size())
      memcpy(clear, cb->clears[a].color.float32, sizeof clear);
    int resolve = -1;
    if (c < sp.resolve.size() && sp.resolve[c].attachment != VK_ATTACHMENT_UNUSED &&
        sp.resolve[c].attachment < fb->views.size())
      resolve = static_cast<int>(fb->views[sp.resolve[c].attachment]->view_id);
    const FormatInfo* fi = GetFormat(ad.format);
    bool integer = fi && fi->gpu && (strstr(fi->gpu, "uint") || strstr(fi->gpu, "sint"));
    if (integer && a < cb->clears.size()) {
      // Integer targets clear to the integer values.
      if (strstr(fi->gpu, "uint"))
        for (int k = 0; k < 4; k++) clear[k] = static_cast<float>(cb->clears[a].color.uint32[k]);
      else
        for (int k = 0; k < 4; k++) clear[k] = static_cast<float>(cb->clears[a].color.int32[k]);
    }
    snprintf(buf, sizeof buf,
             "{\"view\":%u,\"resolve\":%d,\"load\":\"%s\",\"store\":\"%s\",\"clear\":[%.9g,%.9g,%.9g,%.9g]}",
             fb->views[a]->view_id, resolve, load, store, JsonNum(clear[0]), JsonNum(clear[1]), JsonNum(clear[2]),
             JsonNum(clear[3]));
    j += buf;
  }
  j += "]";
  uint32_t d = sp.depth.attachment;
  if (d != VK_ATTACHMENT_UNUSED && d < fb->views.size()) {
    const VkAttachmentDescription& ad = rp->attachments[d];
    const FormatInfo* fi = GetFormat(ad.format);
    bool first = rp->first_use[d] == sub, last = rp->last_use[d] == sub;
    float depth = 1;
    uint32_t stencil = 0;
    if (d < cb->clears.size()) {
      depth = cb->clears[d].depthStencil.depth;
      stencil = cb->clears[d].depthStencil.stencil;
    }
    bool has_depth = fi && (fi->aspects & VK_IMAGE_ASPECT_DEPTH_BIT);
    bool has_stencil = fi && (fi->aspects & VK_IMAGE_ASPECT_STENCIL_BIT);
    auto load = [&](VkAttachmentLoadOp op) { return !first || op == VK_ATTACHMENT_LOAD_OP_LOAD ? "load" : "clear"; };
    auto store = [&](VkAttachmentStoreOp op) {
      return !last || op == VK_ATTACHMENT_STORE_OP_STORE ? "store" : "discard";
    };
    snprintf(buf, sizeof buf,
             ",\"depth\":{\"view\":%u,\"hasDepth\":%s,\"hasStencil\":%s,\"depthLoad\":\"%s\",\"depthStore\":\"%s\","
             "\"stencilLoad\":\"%s\",\"stencilStore\":\"%s\",\"clearDepth\":%.9g,\"clearStencil\":%u}",
             fb->views[d]->view_id, has_depth ? "true" : "false", has_stencil ? "true" : "false",
             load(ad.loadOp), store(ad.storeOp), load(ad.stencilLoadOp), store(ad.stencilStoreOp), JsonNum(depth), stencil);
    j += buf;
  }
  // The subpass's formats and sample count (vkCmdClearAttachments' pipelines).
  uint32_t samples = 1;
  j += ",\"formats\":[";
  for (size_t c = 0; c < sp.color.size(); c++) {
    uint32_t a = sp.color[c].attachment;
    const FormatInfo* fi = a < rp->attachments.size() ? GetFormat(rp->attachments[a].format) : nullptr;
    if (a < rp->attachments.size()) samples = rp->attachments[a].samples;
    j += c ? "," : "";
    j += fi && fi->gpu ? std::string("\"") + fi->gpu + "\"" : "null";
  }
  j += "]";
  if (d < rp->attachments.size()) {
    const FormatInfo* fi = GetFormat(rp->attachments[d].format);
    samples = rp->attachments[d].samples;
    if (fi && fi->gpu) j += std::string(",\"depthFormat\":\"") + fi->gpu + "\"";
  }
  snprintf(buf, sizeof buf, ",\"samples\":%u,\"width\":%u,\"height\":%u}", samples, fb->width, fb->height);
  j += buf;
  cb->ops.Begin(CMD_BEGIN_PASS);
  cb->ops.Str(j);
  cb->ops.End();
  MarkAllDirty(cb);
  if (cb->render_area.offset.x || cb->render_area.offset.y || cb->render_area.extent.width < fb->width ||
      cb->render_area.extent.height < fb->height)
    Unsupported("render areas smaller than the framebuffer (clears cover all of it)");
}

}  // namespace

void NoteRead(CommandBuffer* cb, Buffer* b, VkDeviceSize offset, VkDeviceSize size) {
  if (!b || !b->memory || !b->memory->HostVisible() || !size) return;
  cb->reads.push_back({b->memory, b->offset + offset, size});
}

void NoteWrite(CommandBuffer* cb, Buffer* b, VkDeviceSize offset, VkDeviceSize size) {
  if (!b || !b->memory || !b->memory->HostVisible() || !size) return;
  cb->writes.push_back({b->memory, b->offset + offset, size});
}

void CommandBuffer::Reset() {
  ops.Clear();
  reads.clear();
  writes.clear();
  queries.clear();
  recording = false;
  pass = nullptr;
  framebuffer = nullptr;
  graphics = compute = nullptr;
  for (auto& bp : sets)
    for (auto& s : bp) s = BoundSet();
  for (auto& v : vertex) v = nullptr;
  index = nullptr;
  has_viewport = has_scissor = false;
  in_compute = false;
  MarkAllDirty(this);
}

// --- pools -------------------------------------------------------------------------------

VkResult vkCreateCommandPool(VkDevice device, const VkCommandPoolCreateInfo*, const VkAllocationCallbacks*,
                             VkCommandPool* pPool) {
  CommandPool* p = new CommandPool();
  p->device = reinterpret_cast<Device*>(device);
  *pPool = Handle<VkCommandPool>(p);
  return VK_SUCCESS;
}

void vkDestroyCommandPool(VkDevice, VkCommandPool pool, const VkAllocationCallbacks*) {
  CommandPool* p = As<CommandPool>(pool);
  if (!p) return;
  for (CommandBuffer* cb : p->buffers) delete cb;
  delete p;
}

VkResult vkResetCommandPool(VkDevice, VkCommandPool pool, VkCommandPoolResetFlags) {
  for (CommandBuffer* cb : As<CommandPool>(pool)->buffers) cb->Reset();
  return VK_SUCCESS;
}

void vkTrimCommandPool(VkDevice, VkCommandPool, VkCommandPoolTrimFlags) {}

VkResult vkAllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo* info, VkCommandBuffer* out) {
  CommandPool* p = As<CommandPool>(info->commandPool);
  for (uint32_t i = 0; i < info->commandBufferCount; i++) {
    CommandBuffer* cb = new CommandBuffer();
    cb->device = reinterpret_cast<Device*>(device);
    cb->pool = p;
    cb->level = info->level;
    cb->Reset();
    p->buffers.push_back(cb);
    out[i] = reinterpret_cast<VkCommandBuffer>(cb);
  }
  return VK_SUCCESS;
}

void vkFreeCommandBuffers(VkDevice, VkCommandPool pool, uint32_t n, const VkCommandBuffer* buffers) {
  CommandPool* p = As<CommandPool>(pool);
  for (uint32_t i = 0; i < n; i++) {
    CommandBuffer* cb = CB(buffers[i]);
    if (!cb) continue;
    p->buffers.erase(std::remove(p->buffers.begin(), p->buffers.end(), cb), p->buffers.end());
    delete cb;
  }
}

VkResult vkBeginCommandBuffer(VkCommandBuffer c, const VkCommandBufferBeginInfo* info) {
  CommandBuffer* cb = CB(c);
  cb->Reset();
  cb->recording = true;
  if (cb->level == VK_COMMAND_BUFFER_LEVEL_SECONDARY && info->pInheritanceInfo &&
      (info->flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT)) {
    // Recorded inside its primary's pass: the pass is the primary's.
    cb->pass = As<RenderPass>(info->pInheritanceInfo->renderPass);
    cb->subpass = info->pInheritanceInfo->subpass;
  }
  return VK_SUCCESS;
}

VkResult vkEndCommandBuffer(VkCommandBuffer c) {
  CB(c)->recording = false;
  return VK_SUCCESS;
}

VkResult vkResetCommandBuffer(VkCommandBuffer c, VkCommandBufferResetFlags) {
  CB(c)->Reset();
  return VK_SUCCESS;
}

// --- state -------------------------------------------------------------------------------

void vkCmdBindPipeline(VkCommandBuffer c, VkPipelineBindPoint bp, VkPipeline pipeline) {
  CommandBuffer* cb = CB(c);
  Pipeline* p = As<Pipeline>(pipeline);
  if (bp == VK_PIPELINE_BIND_POINT_COMPUTE) {
    cb->compute = p;
    return;
  }
  cb->graphics = p;
  // What the pipeline fixes replaces the dynamic values.
  if (p->has_viewport) {
    cb->viewport = p->viewport;
    cb->has_viewport = cb->viewport_dirty = true;
  }
  if (p->has_scissor) {
    cb->scissor = p->scissor;
    cb->has_scissor = cb->scissor_dirty = true;
  }
  if (!(p->dynamic & (1u << VK_DYNAMIC_STATE_BLEND_CONSTANTS))) {
    memcpy(cb->blend_constants, p->blend_constants, sizeof cb->blend_constants);
    cb->blend_dirty = true;
  }
  if (!(p->dynamic & (1u << VK_DYNAMIC_STATE_STENCIL_REFERENCE))) {
    cb->stencil_ref[0] = cb->stencil_ref[1] = p->stencil_ref;
    cb->stencil_ref_dirty = true;
  }
  if (!(p->dynamic & (1u << VK_DYNAMIC_STATE_DEPTH_BIAS))) memcpy(cb->depth_bias, p->depth_bias, sizeof cb->depth_bias);
  if (!(p->dynamic & (1u << VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK))) {
    cb->stencil_compare[0] = p->stencil_compare[0];
    cb->stencil_compare[1] = p->stencil_compare[1];
  }
  if (!(p->dynamic & (1u << VK_DYNAMIC_STATE_STENCIL_WRITE_MASK))) {
    cb->stencil_write[0] = p->stencil_write[0];
    cb->stencil_write[1] = p->stencil_write[1];
  }
  cb->dyn_dirty = true;
  cb->vertex_dirty = 0xffff;  // strides may differ
}

void vkCmdSetViewport(VkCommandBuffer c, uint32_t first, uint32_t n, const VkViewport* v) {
  CommandBuffer* cb = CB(c);
  if (first == 0 && n > 0) {
    cb->viewport = v[0];
    cb->has_viewport = cb->viewport_dirty = true;
  }
}

void vkCmdSetScissor(VkCommandBuffer c, uint32_t first, uint32_t n, const VkRect2D* s) {
  CommandBuffer* cb = CB(c);
  if (first == 0 && n > 0) {
    cb->scissor = s[0];
    cb->has_scissor = cb->scissor_dirty = true;
  }
}

void vkCmdSetLineWidth(VkCommandBuffer, float) {}

void vkCmdSetDepthBias(VkCommandBuffer c, float constant, float clamp, float slope) {
  CommandBuffer* cb = CB(c);
  cb->depth_bias[0] = constant;
  cb->depth_bias[1] = clamp;
  cb->depth_bias[2] = slope;
  cb->dyn_dirty = true;
}

void vkCmdSetBlendConstants(VkCommandBuffer c, const float constants[4]) {
  CommandBuffer* cb = CB(c);
  memcpy(cb->blend_constants, constants, sizeof cb->blend_constants);
  cb->blend_dirty = true;
}

void vkCmdSetDepthBounds(VkCommandBuffer, float, float) { Unsupported("depth bounds"); }

void vkCmdSetStencilCompareMask(VkCommandBuffer c, VkStencilFaceFlags faces, uint32_t mask) {
  CommandBuffer* cb = CB(c);
  if (faces & VK_STENCIL_FACE_FRONT_BIT) cb->stencil_compare[0] = mask;
  if (faces & VK_STENCIL_FACE_BACK_BIT) cb->stencil_compare[1] = mask;
  cb->dyn_dirty = true;
}

void vkCmdSetStencilWriteMask(VkCommandBuffer c, VkStencilFaceFlags faces, uint32_t mask) {
  CommandBuffer* cb = CB(c);
  if (faces & VK_STENCIL_FACE_FRONT_BIT) cb->stencil_write[0] = mask;
  if (faces & VK_STENCIL_FACE_BACK_BIT) cb->stencil_write[1] = mask;
  cb->dyn_dirty = true;
}

void vkCmdSetStencilReference(VkCommandBuffer c, VkStencilFaceFlags faces, uint32_t ref) {
  CommandBuffer* cb = CB(c);
  if (faces & VK_STENCIL_FACE_FRONT_BIT) cb->stencil_ref[0] = ref;
  if (faces & VK_STENCIL_FACE_BACK_BIT) cb->stencil_ref[1] = ref;
  cb->stencil_ref_dirty = true;
}

void vkCmdBindDescriptorSets(VkCommandBuffer c, VkPipelineBindPoint bp, VkPipelineLayout, uint32_t first,
                             uint32_t n, const VkDescriptorSet* sets, uint32_t ndyn, const uint32_t* dyn) {
  CommandBuffer* cb = CB(c);
  int b = bp == VK_PIPELINE_BIND_POINT_COMPUTE ? 1 : 0;
  uint32_t used = 0;
  for (uint32_t i = 0; i < n && first + i < 4; i++) {
    DescriptorSet* s = As<DescriptorSet>(sets[i]);
    CommandBuffer::BoundSet& bs = cb->sets[b][first + i];
    bs.set = s;
    bs.dynamic.clear();
    uint32_t k = s ? s->layout->dynamic_count : 0;
    for (uint32_t d = 0; d < k && used < ndyn; d++) bs.dynamic.push_back(dyn[used++]);
    cb->sets_dirty[b] |= 1u << (first + i);
  }
}

void vkCmdBindIndexBuffer(VkCommandBuffer c, VkBuffer buffer, VkDeviceSize offset, VkIndexType type) {
  CommandBuffer* cb = CB(c);
  cb->index = As<Buffer>(buffer);
  cb->index_offset = offset;
  cb->index_type = type;
  cb->index_dirty = true;
}

void vkCmdBindVertexBuffers(VkCommandBuffer c, uint32_t first, uint32_t n, const VkBuffer* buffers,
                            const VkDeviceSize* offsets) {
  CommandBuffer* cb = CB(c);
  for (uint32_t i = 0; i < n && first + i < 16; i++) {
    cb->vertex[first + i] = As<Buffer>(buffers[i]);
    cb->vertex_offset[first + i] = offsets[i];
    cb->vertex_dirty |= 1u << (first + i);
  }
}

void vkCmdPushConstants(VkCommandBuffer c, VkPipelineLayout, VkShaderStageFlags, uint32_t offset, uint32_t size,
                        const void* values) {
  CommandBuffer* cb = CB(c);
  if (offset + size > sizeof cb->push) return;
  memcpy(cb->push + offset, values, size);
  cb->push_dirty = true;
}

// --- draws and dispatches -----------------------------------------------------------------

void vkCmdDraw(VkCommandBuffer c, uint32_t vertices, uint32_t instances, uint32_t first_vertex,
               uint32_t first_instance) {
  CommandBuffer* cb = CB(c);
  FlushState(cb, 0);
  cb->ops.Op(CMD_DRAW, {vertices, instances, first_vertex, first_instance});
}

void vkCmdDrawIndexed(VkCommandBuffer c, uint32_t indices, uint32_t instances, uint32_t first_index,
                      int32_t vertex_offset, uint32_t first_instance) {
  CommandBuffer* cb = CB(c);
  FlushState(cb, 0);
  FlushIndex(cb);
  cb->ops.Op(CMD_DRAW_INDEXED, {indices, instances, first_index, static_cast<uint32_t>(vertex_offset),
                                first_instance});
}

static void DrawIndirect(VkCommandBuffer c, VkBuffer buffer, VkDeviceSize offset, uint32_t count, uint32_t stride,
                         bool indexed) {
  CommandBuffer* cb = CB(c);
  FlushState(cb, 0);
  if (indexed) FlushIndex(cb);
  Buffer* b = As<Buffer>(buffer);
  VkDeviceSize base = 0;
  uint32_t id;
  {
    std::lock_guard<std::mutex> l(cb->device->lock);
    id = BufferId(b, &base);
  }
  uint32_t each = indexed ? 20 : 16;
  NoteRead(cb, b, offset, count ? static_cast<VkDeviceSize>(stride) * (count - 1) + each : 0);
  cb->ops.Op(indexed ? CMD_DRAW_INDEXED_INDIRECT : CMD_DRAW_INDIRECT,
             {id, static_cast<uint32_t>(base + offset), count, stride ? stride : each});
}

void vkCmdDrawIndirect(VkCommandBuffer c, VkBuffer buffer, VkDeviceSize offset, uint32_t count, uint32_t stride) {
  DrawIndirect(c, buffer, offset, count, stride, false);
}

void vkCmdDrawIndexedIndirect(VkCommandBuffer c, VkBuffer buffer, VkDeviceSize offset, uint32_t count,
                              uint32_t stride) {
  DrawIndirect(c, buffer, offset, count, stride, true);
}

void vkCmdDispatch(VkCommandBuffer c, uint32_t x, uint32_t y, uint32_t z) {
  CommandBuffer* cb = CB(c);
  FlushState(cb, 1);
  cb->ops.Op(CMD_DISPATCH, {x, y, z});
}

void vkCmdDispatchBase(VkCommandBuffer c, uint32_t bx, uint32_t by, uint32_t bz, uint32_t x, uint32_t y,
                       uint32_t z) {
  if (bx || by || bz) Unsupported("vkCmdDispatchBase with a base");
  vkCmdDispatch(c, x, y, z);
}

void vkCmdDispatchIndirect(VkCommandBuffer c, VkBuffer buffer, VkDeviceSize offset) {
  CommandBuffer* cb = CB(c);
  FlushState(cb, 1);
  Buffer* b = As<Buffer>(buffer);
  VkDeviceSize base = 0;
  uint32_t id;
  {
    std::lock_guard<std::mutex> l(cb->device->lock);
    id = BufferId(b, &base);
  }
  NoteRead(cb, b, offset, 12);
  cb->ops.Op(CMD_DISPATCH_INDIRECT, {id, static_cast<uint32_t>(base + offset)});
}

void vkCmdSetDeviceMask(VkCommandBuffer, uint32_t) {}

// --- transfers ------------------------------------------------------------------------------

namespace {

uint32_t Id(CommandBuffer* cb, Buffer* b, VkDeviceSize* base) {
  std::lock_guard<std::mutex> l(cb->device->lock);
  return BufferId(b, base);
}

// A buffer <-> image copy region as the page wants it.
std::string CopyJson(CommandBuffer* cb, Buffer* b, Image* img, const VkBufferImageCopy& r, bool to_image) {
  const FormatInfo* fi = img->info;
  VkDeviceSize base = 0;
  uint32_t id = Id(cb, b, &base);
  uint32_t row_texels = r.bufferRowLength ? r.bufferRowLength : r.imageExtent.width;
  uint32_t rows = r.bufferImageHeight ? r.bufferImageHeight : r.imageExtent.height;
  uint32_t bpr = (row_texels + fi->block_w - 1) / fi->block_w * fi->block_bytes;
  uint32_t rpi = (rows + fi->block_h - 1) / fi->block_h;
  uint32_t depth = img->type == VK_IMAGE_TYPE_3D ? r.imageExtent.depth : r.imageSubresource.layerCount;
  int32_t z = img->type == VK_IMAGE_TYPE_3D ? r.imageOffset.z : static_cast<int32_t>(r.imageSubresource.baseArrayLayer);
  VkDeviceSize bytes = static_cast<VkDeviceSize>(bpr) * rpi * (depth - 1) +
                       static_cast<VkDeviceSize>(bpr) * ((r.imageExtent.height + fi->block_h - 1) / fi->block_h - 1) +
                       (r.imageExtent.width + fi->block_w - 1) / fi->block_w * fi->block_bytes;
  // A host-visible source: the page may upload from the shadow instead
  // (copyBufferToTexture wants rows of a multiple of 256 bytes).
  uintptr_t host = 0;
  if (b->memory && b->memory->host) host = reinterpret_cast<uintptr_t>(b->memory->host + b->offset + r.bufferOffset);
  if (to_image) NoteRead(cb, b, r.bufferOffset, bytes);
  else NoteWrite(cb, b, r.bufferOffset, bytes);
  char json[600];
  snprintf(json, sizeof json,
           "{\"buffer\":%u,\"offset\":%llu,\"bytesPerRow\":%u,\"rowsPerImage\":%u,\"bytes\":%llu,\"host\":%lu,"
           "\"texture\":%u,\"mipLevel\":%u,\"origin\":[%d,%d,%d],\"size\":[%u,%u,%u],\"aspect\":\"%s\","
           "\"blockW\":%u,\"blockH\":%u,\"blockBytes\":%u}",
           id, static_cast<unsigned long long>(base + r.bufferOffset), bpr, rpi,
           static_cast<unsigned long long>(bytes), static_cast<unsigned long>(host), img->texture_id,
           r.imageSubresource.mipLevel, r.imageOffset.x, r.imageOffset.y, z, r.imageExtent.width,
           img->type == VK_IMAGE_TYPE_1D ? 1 : r.imageExtent.height, depth,
           AspectName(r.imageSubresource.aspectMask, fi), fi->block_w, fi->block_h, fi->block_bytes);
  return json;
}

}  // namespace

void vkCmdCopyBuffer(VkCommandBuffer c, VkBuffer src, VkBuffer dst, uint32_t n, const VkBufferCopy* regions) {
  CommandBuffer* cb = CB(c);
  Buffer* s = As<Buffer>(src);
  Buffer* d = As<Buffer>(dst);
  VkDeviceSize sb = 0, db = 0;
  uint32_t sid = Id(cb, s, &sb), did = Id(cb, d, &db);
  for (uint32_t i = 0; i < n; i++) {
    const VkBufferCopy& r = regions[i];
    NoteRead(cb, s, r.srcOffset, r.size);
    NoteWrite(cb, d, r.dstOffset, r.size);
    cb->ops.Op(CMD_COPY_BUFFER, {sid, static_cast<uint32_t>(sb + r.srcOffset), did,
                                 static_cast<uint32_t>(db + r.dstOffset), static_cast<uint32_t>(r.size)});
  }
}

void vkCmdCopyBufferToImage(VkCommandBuffer c, VkBuffer buffer, VkImage image, VkImageLayout, uint32_t n,
                            const VkBufferImageCopy* regions) {
  CommandBuffer* cb = CB(c);
  for (uint32_t i = 0; i < n; i++) {
    std::string j = CopyJson(cb, As<Buffer>(buffer), As<Image>(image), regions[i], true);
    cb->ops.Begin(CMD_COPY_BUFFER_TO_TEXTURE);
    cb->ops.Str(j);
    cb->ops.End();
  }
}

void vkCmdCopyImageToBuffer(VkCommandBuffer c, VkImage image, VkImageLayout, VkBuffer buffer, uint32_t n,
                            const VkBufferImageCopy* regions) {
  CommandBuffer* cb = CB(c);
  for (uint32_t i = 0; i < n; i++) {
    std::string j = CopyJson(cb, As<Buffer>(buffer), As<Image>(image), regions[i], false);
    cb->ops.Begin(CMD_COPY_TEXTURE_TO_BUFFER);
    cb->ops.Str(j);
    cb->ops.End();
  }
}

static std::string ImageRegion(Image* img, const VkImageSubresourceLayers& sub, VkOffset3D off) {
  char b[200];
  int32_t z = img->type == VK_IMAGE_TYPE_3D ? off.z : static_cast<int32_t>(sub.baseArrayLayer);
  snprintf(b, sizeof b, "{\"texture\":%u,\"mipLevel\":%u,\"origin\":[%d,%d,%d],\"aspect\":\"%s\"}",
           img->texture_id, sub.mipLevel, off.x, off.y, z, AspectName(sub.aspectMask, img->info));
  return b;
}

void vkCmdCopyImage(VkCommandBuffer c, VkImage src, VkImageLayout, VkImage dst, VkImageLayout, uint32_t n,
                    const VkImageCopy* regions) {
  CommandBuffer* cb = CB(c);
  Image* s = As<Image>(src);
  Image* d = As<Image>(dst);
  for (uint32_t i = 0; i < n; i++) {
    const VkImageCopy& r = regions[i];
    uint32_t depth = s->type == VK_IMAGE_TYPE_3D ? r.extent.depth : r.srcSubresource.layerCount;
    char size[80];
    snprintf(size, sizeof size, "[%u,%u,%u]", r.extent.width, r.extent.height, depth);
    std::string j = "{\"src\":" + ImageRegion(s, r.srcSubresource, r.srcOffset) +
                    ",\"dst\":" + ImageRegion(d, r.dstSubresource, r.dstOffset) + ",\"size\":" + size + "}";
    cb->ops.Begin(CMD_COPY_TEXTURE);
    cb->ops.Str(j);
    cb->ops.End();
  }
}

void vkCmdBlitImage(VkCommandBuffer c, VkImage src, VkImageLayout, VkImage dst, VkImageLayout, uint32_t n,
                    const VkImageBlit* regions, VkFilter filter) {
  CommandBuffer* cb = CB(c);
  Image* s = As<Image>(src);
  Image* d = As<Image>(dst);
  for (uint32_t i = 0; i < n; i++) {
    const VkImageBlit& r = regions[i];
    char j[700];
    snprintf(j, sizeof j,
             "{\"src\":%u,\"srcFormat\":\"%s\",\"srcMip\":%u,\"srcLayer\":%u,\"srcBox\":[%d,%d,%d,%d],"
             "\"dst\":%u,\"dstFormat\":\"%s\",\"dstMip\":%u,\"dstLayer\":%u,\"dstBox\":[%d,%d,%d,%d],"
             "\"filter\":\"%s\",\"srcSize\":[%u,%u]}",
             s->texture_id, s->info->gpu, r.srcSubresource.mipLevel, r.srcSubresource.baseArrayLayer,
             r.srcOffsets[0].x, r.srcOffsets[0].y, r.srcOffsets[1].x, r.srcOffsets[1].y, d->texture_id,
             d->info->gpu, r.dstSubresource.mipLevel, r.dstSubresource.baseArrayLayer, r.dstOffsets[0].x,
             r.dstOffsets[0].y, r.dstOffsets[1].x, r.dstOffsets[1].y,
             filter == VK_FILTER_LINEAR ? "linear" : "nearest", std::max(1u, s->extent.width >> r.srcSubresource.mipLevel),
             std::max(1u, s->extent.height >> r.srcSubresource.mipLevel));
    cb->ops.Begin(CMD_BLIT);
    cb->ops.Str(j);
    cb->ops.End();
  }
}

void vkCmdResolveImage(VkCommandBuffer c, VkImage src, VkImageLayout, VkImage dst, VkImageLayout, uint32_t n,
                       const VkImageResolve* regions) {
  CommandBuffer* cb = CB(c);
  Image* s = As<Image>(src);
  Image* d = As<Image>(dst);
  for (uint32_t i = 0; i < n; i++) {
    const VkImageResolve& r = regions[i];
    char j[300];
    snprintf(j, sizeof j,
             "{\"resolve\":true,\"src\":%u,\"srcMip\":%u,\"srcLayer\":%u,\"dst\":%u,\"dstMip\":%u,\"dstLayer\":%u,"
             "\"format\":\"%s\"}",
             s->texture_id, r.srcSubresource.mipLevel, r.srcSubresource.baseArrayLayer, d->texture_id,
             r.dstSubresource.mipLevel, r.dstSubresource.baseArrayLayer, s->info->gpu);
    cb->ops.Begin(CMD_BLIT);
    cb->ops.Str(j);
    cb->ops.End();
  }
}

void vkCmdUpdateBuffer(VkCommandBuffer c, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size, const void* data) {
  CommandBuffer* cb = CB(c);
  Buffer* b = As<Buffer>(buffer);
  VkDeviceSize base = 0;
  uint32_t id = Id(cb, b, &base);
  NoteWrite(cb, b, offset, size);
  cb->ops.Begin(CMD_UPDATE_BUFFER);
  cb->ops.U32(id);
  cb->ops.U32(static_cast<uint32_t>(base + offset));
  cb->ops.Bytes(data, static_cast<size_t>(size));
  cb->ops.End();
}

void vkCmdFillBuffer(VkCommandBuffer c, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size, uint32_t data) {
  CommandBuffer* cb = CB(c);
  Buffer* b = As<Buffer>(buffer);
  if (size == VK_WHOLE_SIZE) size = (b->size - offset) & ~3ull;
  VkDeviceSize base = 0;
  uint32_t id = Id(cb, b, &base);
  NoteWrite(cb, b, offset, size);
  cb->ops.Op(CMD_FILL_BUFFER, {id, static_cast<uint32_t>(base + offset), static_cast<uint32_t>(size), data});
}

static void ClearImage(CommandBuffer* cb, Image* img, const float color[4], float depth, uint32_t stencil,
                       uint32_t n, const VkImageSubresourceRange* ranges, bool ds) {
  for (uint32_t i = 0; i < n; i++) {
    const VkImageSubresourceRange& r = ranges[i];
    uint32_t levels = r.levelCount == VK_REMAINING_MIP_LEVELS ? img->mip_levels - r.baseMipLevel : r.levelCount;
    uint32_t layers = r.layerCount == VK_REMAINING_ARRAY_LAYERS ? img->array_layers - r.baseArrayLayer : r.layerCount;
    char j[400];
    snprintf(j, sizeof j,
             "{\"texture\":%u,\"format\":\"%s\",\"baseMip\":%u,\"mips\":%u,\"baseLayer\":%u,\"layers\":%u,"
             "\"depthStencil\":%s,\"aspect\":%u,\"color\":[%.9g,%.9g,%.9g,%.9g],\"depth\":%.9g,\"stencil\":%u,"
             "\"is3d\":%s,\"depthSize\":%u}",
             img->texture_id, img->info->gpu, r.baseMipLevel, levels, r.baseArrayLayer, layers, ds ? "true" : "false",
             r.aspectMask, JsonNum(color[0]), JsonNum(color[1]), JsonNum(color[2]), JsonNum(color[3]), JsonNum(depth), stencil,
             img->type == VK_IMAGE_TYPE_3D ? "true" : "false", img->extent.depth);
    cb->ops.Begin(CMD_CLEAR_TEXTURE);
    cb->ops.Str(j);
    cb->ops.End();
  }
}

void vkCmdClearColorImage(VkCommandBuffer c, VkImage image, VkImageLayout, const VkClearColorValue* color, uint32_t n,
                          const VkImageSubresourceRange* ranges) {
  Image* img = As<Image>(image);
  // Integer images clear to the integer values.
  float v[4];
  const char* g = img->info->gpu;
  for (int k = 0; k < 4; k++) {
    v[k] = strstr(g, "uint") ? static_cast<float>(color->uint32[k])
           : strstr(g, "sint") ? static_cast<float>(color->int32[k])
                               : color->float32[k];
  }
  ClearImage(CB(c), img, v, 0, 0, n, ranges, false);
}

void vkCmdClearDepthStencilImage(VkCommandBuffer c, VkImage image, VkImageLayout,
                                 const VkClearDepthStencilValue* value, uint32_t n,
                                 const VkImageSubresourceRange* ranges) {
  const float zero[4] = {0, 0, 0, 0};
  ClearImage(CB(c), As<Image>(image), zero, value->depth, value->stencil, n, ranges, true);
}

void vkCmdClearAttachments(VkCommandBuffer c, uint32_t na, const VkClearAttachment* atts, uint32_t nr,
                           const VkClearRect* rects) {
  CommandBuffer* cb = CB(c);
  if (!cb->pass) return;
  const RenderPass::Subpass& sp = cb->pass->subpasses[cb->subpass];
  std::string j = "{\"attachments\":[";
  char b[300];
  for (uint32_t i = 0; i < na; i++) {
    const VkClearAttachment& a = atts[i];
    int target = (a.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) ? static_cast<int>(a.colorAttachment) : -1;
    snprintf(b, sizeof b, "%s{\"color\":%d,\"aspect\":%u,\"value\":[%.9g,%.9g,%.9g,%.9g],\"depth\":%.9g,\"stencil\":%u}",
             i ? "," : "", target, a.aspectMask, JsonNum(a.clearValue.color.float32[0]), JsonNum(a.clearValue.color.float32[1]),
             JsonNum(a.clearValue.color.float32[2]), JsonNum(a.clearValue.color.float32[3]), JsonNum(a.clearValue.depthStencil.depth),
             a.clearValue.depthStencil.stencil);
    j += b;
  }
  j += "],\"rects\":[";
  for (uint32_t i = 0; i < nr; i++) {
    const VkRect2D& r = rects[i].rect;
    snprintf(b, sizeof b, "%s[%d,%d,%u,%u]", i ? "," : "", r.offset.x, r.offset.y, r.extent.width, r.extent.height);
    j += b;
  }
  snprintf(b, sizeof b, "],\"colorCount\":%zu}", sp.color.size());
  j += b;
  cb->ops.Begin(CMD_CLEAR_ATTACHMENTS);
  cb->ops.Str(j);
  cb->ops.End();
  // The page draws the clear with pipelines of its own.
  cb->sent_pipeline = nullptr;
  cb->sets_dirty[0] = 0xf;
  cb->vertex_dirty = 0xffff;
  cb->index_dirty = cb->push_dirty = cb->dyn_dirty = true;
  cb->viewport_dirty = cb->has_viewport;
  cb->scissor_dirty = cb->has_scissor;
  cb->blend_dirty = cb->stencil_ref_dirty = true;
}

// --- synchronization: one in-order queue ---------------------------------------------------

void vkCmdPipelineBarrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags, uint32_t,
                          const VkMemoryBarrier*, uint32_t, const VkBufferMemoryBarrier*, uint32_t,
                          const VkImageMemoryBarrier*) {}

void vkCmdSetEvent(VkCommandBuffer, VkEvent event, VkPipelineStageFlags) {
  // Set when recorded: the queue runs in order, so whatever waits for it
  // later in the queue would see it set.
  As<Event>(event)->set = 1;
}

void vkCmdResetEvent(VkCommandBuffer, VkEvent event, VkPipelineStageFlags) { As<Event>(event)->set = 0; }

void vkCmdWaitEvents(VkCommandBuffer, uint32_t, const VkEvent*, VkPipelineStageFlags, VkPipelineStageFlags, uint32_t,
                     const VkMemoryBarrier*, uint32_t, const VkBufferMemoryBarrier*, uint32_t,
                     const VkImageMemoryBarrier*) {}

// --- queries ------------------------------------------------------------------------------

void vkCmdBeginQuery(VkCommandBuffer c, VkQueryPool pool, uint32_t query, VkQueryControlFlags) {
  CommandBuffer* cb = CB(c);
  QueryPool* q = As<QueryPool>(pool);
  if (!q->query_set_id) return;
  cb->ops.Op(CMD_BEGIN_QUERY, {q->query_set_id, query});
}

void vkCmdEndQuery(VkCommandBuffer c, VkQueryPool pool, uint32_t query) {
  CommandBuffer* cb = CB(c);
  QueryPool* q = As<QueryPool>(pool);
  if (!q->query_set_id) return;
  cb->ops.Op(CMD_END_QUERY, {q->query_set_id, query});
}

void vkCmdResetQueryPool(VkCommandBuffer, VkQueryPool, uint32_t, uint32_t) {}

void vkCmdWriteTimestamp(VkCommandBuffer, VkPipelineStageFlagBits, VkQueryPool, uint32_t) {
  Unsupported("timestamps (timestampValidBits is 0)");
}

void vkCmdCopyQueryPoolResults(VkCommandBuffer c, VkQueryPool pool, uint32_t first, uint32_t count, VkBuffer buffer,
                               VkDeviceSize offset, VkDeviceSize stride, VkQueryResultFlags flags) {
  CommandBuffer* cb = CB(c);
  QueryPool* q = As<QueryPool>(pool);
  if (!q->query_set_id) return;
  if (!(flags & VK_QUERY_RESULT_64_BIT) || stride != 8)
    Unsupported("query results other than packed 64-bit values");
  Buffer* b = As<Buffer>(buffer);
  VkDeviceSize base = 0;
  uint32_t id = Id(cb, b, &base);
  NoteWrite(cb, b, offset, static_cast<VkDeviceSize>(count) * 8);
  cb->ops.Op(CMD_RESOLVE_QUERIES, {q->query_set_id, first, count, id, static_cast<uint32_t>(base + offset)});
}

// --- render passes ------------------------------------------------------------------------

void vkCmdBeginRenderPass(VkCommandBuffer c, const VkRenderPassBeginInfo* info, VkSubpassContents) {
  CommandBuffer* cb = CB(c);
  cb->pass = As<RenderPass>(info->renderPass);
  cb->framebuffer = As<Framebuffer>(info->framebuffer);
  cb->subpass = 0;
  cb->render_area = info->renderArea;
  cb->clears.assign(info->pClearValues, info->pClearValues + info->clearValueCount);
  BeginSubpass(cb);
}

void vkCmdNextSubpass(VkCommandBuffer c, VkSubpassContents) {
  CommandBuffer* cb = CB(c);
  cb->ops.Op(CMD_END_PASS, {});
  cb->subpass++;
  if (cb->pass && cb->subpass < cb->pass->subpasses.size()) {
    // Input attachments are textures there (vk_spirv.cc); one that is also an
    // attachment of the same subpass (a feedback loop) WebGPU refuses.
    BeginSubpass(cb);
  }
}

void vkCmdEndRenderPass(VkCommandBuffer c) {
  CommandBuffer* cb = CB(c);
  cb->ops.Op(CMD_END_PASS, {});
  cb->pass = nullptr;
  cb->framebuffer = nullptr;
  MarkAllDirty(cb);
}

void vkCmdExecuteCommands(VkCommandBuffer c, uint32_t n, const VkCommandBuffer* buffers) {
  CommandBuffer* cb = CB(c);
  for (uint32_t i = 0; i < n; i++) {
    CommandBuffer* sec = CB(buffers[i]);
    cb->ops.Append(sec->ops);
    cb->reads.insert(cb->reads.end(), sec->reads.begin(), sec->reads.end());
    cb->writes.insert(cb->writes.end(), sec->writes.begin(), sec->writes.end());
  }
  // A secondary leaves the state undefined.
  MarkAllDirty(cb);
}

// --- debug labels -----------------------------------------------------------------------------

void vkCmdBeginDebugUtilsLabelEXT(VkCommandBuffer c, const VkDebugUtilsLabelEXT* label) {
  CommandBuffer* cb = CB(c);
  cb->ops.Begin(CMD_DEBUG_GROUP);
  cb->ops.U32(1);
  cb->ops.Str(label && label->pLabelName ? label->pLabelName : "");
  cb->ops.End();
}

void vkCmdEndDebugUtilsLabelEXT(VkCommandBuffer c) { CB(c)->ops.Op(CMD_DEBUG_GROUP, {0}); }

void vkCmdInsertDebugUtilsLabelEXT(VkCommandBuffer c, const VkDebugUtilsLabelEXT* label) {
  CommandBuffer* cb = CB(c);
  cb->ops.Begin(CMD_DEBUG_GROUP);
  cb->ops.U32(2);
  cb->ops.Str(label && label->pLabelName ? label->pLabelName : "");
  cb->ops.End();
}

}  // namespace arcvk
