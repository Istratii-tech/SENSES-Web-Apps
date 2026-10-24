// SPIR-V before Tint (vk_spirv.h).

#include "vk_spirv.h"

#include <string.h>

#include <map>
#include <set>

namespace arcvk {
namespace {

enum : uint32_t {
  kOpName = 5, kOpExtension = 10, kOpExtInstImport = 11, kOpMemoryModel = 14, kOpEntryPoint = 15,
  kOpExecutionMode = 16, kOpCapability = 17, kOpTypeVoid = 19, kOpTypeBool = 20, kOpTypeInt = 21,
  kOpTypeFloat = 22, kOpTypeVector = 23, kOpTypeImage = 25, kOpTypeSampledImage = 27, kOpTypeArray = 28,
  kOpTypeRuntimeArray = 29, kOpTypeStruct = 30, kOpTypePointer = 32, kOpTypeFunction = 33,
  kOpConstantTrue = 41, kOpConstantFalse = 42, kOpConstant = 43, kOpConstantComposite = 44,
  kOpSpecConstantTrue = 48, kOpSpecConstantFalse = 49, kOpSpecConstant = 50, kOpSpecConstantComposite = 51,
  kOpSpecConstantOp = 52, kOpFunction = 54, kOpFunctionEnd = 56, kOpFunctionCall = 57, kOpVariable = 59,
  kOpLoad = 61, kOpStore = 62, kOpAccessChain = 65, kOpDecorate = 71, kOpMemberDecorate = 72,
  kOpCompositeExtract = 81, kOpCompositeInsert = 82, kOpFNegate = 127, kOpLabel = 248, kOpReturn = 253,
  kOpExecutionModeId = 331, kOpDecorateId = 332, kOpImageFetch = 95, kOpImageRead = 98,
  kOpVectorShuffle = 79, kOpConvertFToS = 110, kOpIAdd = 128,
};
enum : uint32_t {
  kDecSpecId = 1, kDecBlock = 2, kDecBufferBlock = 3, kDecBuiltIn = 11, kDecNonWritable = 24,
  kDecBinding = 33, kDecDescriptorSet = 34,
};
enum : uint32_t {
  kScUniformConstant = 0, kScUniform = 2, kScOutput = 3, kScPushConstant = 9, kScStorageBuffer = 12,
};
constexpr uint32_t kBuiltInPosition = 0;
constexpr uint32_t kBuiltInFragCoord = 15;
constexpr uint32_t kDimSubpassData = 6;
constexpr uint32_t kCapInputAttachment = 40;
constexpr uint32_t kDecInputAttachmentIndex = 43;
constexpr uint32_t kScInput = 1;

struct Inst {
  uint32_t op;
  std::vector<uint32_t> w;  // all words, w[0] the header
};

std::string Literal(const std::vector<uint32_t>& w, size_t at, size_t* end) {
  std::string s;
  for (size_t i = at; i < w.size(); i++) {
    const char* c = reinterpret_cast<const char*>(&w[i]);
    for (int k = 0; k < 4; k++) {
      if (!c[k]) {
        if (end) *end = i + 1;
        return s;
      }
      s += c[k];
    }
  }
  if (end) *end = w.size();
  return s;
}

uint32_t ExecutionModel(VkShaderStageFlagBits stage) {
  switch (stage) {
    case VK_SHADER_STAGE_VERTEX_BIT: return 0;
    case VK_SHADER_STAGE_FRAGMENT_BIT: return 4;
    case VK_SHADER_STAGE_COMPUTE_BIT: return 5;
    default: return 0xffffffff;
  }
}

class Module {
 public:
  bool Parse(const std::vector<uint32_t>& in, std::string* error) {
    if (in.size() < 5 || in[0] != 0x07230203) {
      *error = "not SPIR-V";
      return false;
    }
    header_.assign(in.begin(), in.begin() + 5);
    for (size_t i = 5; i < in.size();) {
      uint32_t n = in[i] >> 16, op = in[i] & 0xffff;
      if (n == 0 || i + n > in.size()) {
        *error = "truncated instruction";
        return false;
      }
      insts_.push_back({op, std::vector<uint32_t>(in.begin() + i, in.begin() + i + n)});
      i += n;
    }
    return true;
  }
  uint32_t version() const { return header_[1]; }
  uint32_t NewId() { return header_[3]++; }
  std::vector<Inst>& insts() { return insts_; }

  void Write(std::vector<uint32_t>* out) const {
    out->assign(header_.begin(), header_.end());
    for (const auto& i : insts_) out->insert(out->end(), i.w.begin(), i.w.end());
  }

  // The first instruction index of the function section (where globals end).
  size_t FirstFunction() const {
    for (size_t i = 0; i < insts_.size(); i++)
      if (insts_[i].op == kOpFunction) return i;
    return insts_.size();
  }
  // Where new annotations go: after the last decoration, else before the first type.
  size_t AnnotationEnd() const {
    size_t last = 0;
    bool any = false;
    for (size_t i = 0; i < insts_.size(); i++) {
      uint32_t op = insts_[i].op;
      if (op == kOpDecorate || op == kOpMemberDecorate || op == kOpDecorateId || op == 73 /* group */ ||
          op == 74 || op == 75 || op == 5632 /* DecorateString */ || op == 5633) {
        last = i;
        any = true;
      }
    }
    if (any) return last + 1;
    for (size_t i = 0; i < insts_.size(); i++)
      if (insts_[i].op >= kOpTypeVoid && insts_[i].op <= 39) return i;
    return FirstFunction();
  }
  void Insert(size_t at, Inst inst) { insts_.insert(insts_.begin() + at, std::move(inst)); }

  static Inst Make(uint32_t op, std::initializer_list<uint32_t> operands) {
    Inst i{op, {0}};
    i.w.insert(i.w.end(), operands);
    i.w[0] = op | static_cast<uint32_t>(i.w.size()) << 16;
    return i;
  }

 private:
  std::vector<uint32_t> header_;
  std::vector<Inst> insts_;
};

// --- specialization ---------------------------------------------------------------------

void Specialize(Module& m, const VkSpecializationInfo* spec) {
  std::map<uint32_t, uint32_t> spec_id;  // result id -> SpecId
  for (const auto& i : m.insts())
    if (i.op == kOpDecorate && i.w.size() >= 4 && i.w[2] == kDecSpecId) spec_id[i.w[1]] = i.w[3];
  auto value = [&](uint32_t id, const void** data, size_t* size) {
    auto it = spec_id.find(id);
    if (it == spec_id.end() || !spec) return false;
    for (uint32_t k = 0; k < spec->mapEntryCount; k++) {
      const VkSpecializationMapEntry& e = spec->pMapEntries[k];
      if (e.constantID == it->second && e.offset + e.size <= spec->dataSize) {
        *data = static_cast<const uint8_t*>(spec->pData) + e.offset;
        *size = e.size;
        return true;
      }
    }
    return false;
  };
  for (auto& i : m.insts()) {
    const void* data;
    size_t size;
    switch (i.op) {
      case kOpSpecConstantTrue:
      case kOpSpecConstantFalse: {
        bool v = i.op == kOpSpecConstantTrue;
        if (value(i.w[2], &data, &size)) {
          uint32_t b = 0;
          memcpy(&b, data, size < 4 ? size : 4);
          v = b != 0;
        }
        i.op = v ? kOpConstantTrue : kOpConstantFalse;
        i.w[0] = i.op | (i.w[0] & 0xffff0000);
        break;
      }
      case kOpSpecConstant: {
        if (value(i.w[2], &data, &size)) {
          size_t words = i.w.size() - 3;
          std::vector<uint32_t> v(words, 0);
          memcpy(v.data(), data, size < words * 4 ? size : words * 4);
          for (size_t k = 0; k < words; k++) i.w[3 + k] = v[k];
        }
        i.op = kOpConstant;
        i.w[0] = i.op | (i.w[0] & 0xffff0000);
        break;
      }
      case kOpSpecConstantComposite:
        i.op = kOpConstantComposite;
        i.w[0] = i.op | (i.w[0] & 0xffff0000);
        break;
      default:
        break;
    }
  }
  // SpecId on what are constants now is invalid.
  auto& v = m.insts();
  for (size_t k = 0; k < v.size();) {
    if (v[k].op == kOpDecorate && v[k].w.size() >= 3 && v[k].w[2] == kDecSpecId) v.erase(v.begin() + k);
    else k++;
  }
}

// --- push constants --------------------------------------------------------------------

bool PushConstantsToStorage(Module& m, uint32_t group, uint32_t binding, bool* found) {
  uint32_t var = 0, block = 0;
  std::map<uint32_t, uint32_t> struct_members;
  for (auto& i : m.insts()) {
    if (i.op == kOpTypeStruct) struct_members[i.w[1]] = static_cast<uint32_t>(i.w.size() - 2);
    if (i.op == kOpTypePointer && i.w[2] == kScPushConstant) i.w[2] = kScStorageBuffer;
    if (i.op == kOpVariable && i.w.size() >= 4 && i.w[3] == kScPushConstant) {
      i.w[3] = kScStorageBuffer;
      var = i.w[2];
    }
  }
  *found = var != 0;
  if (!var) return true;
  // The block struct: the pointee of the variable's type.
  uint32_t ptr_type = 0;
  for (auto& i : m.insts())
    if (i.op == kOpVariable && i.w[2] == var) ptr_type = i.w[1];
  for (auto& i : m.insts())
    if (i.op == kOpTypePointer && i.w[1] == ptr_type) block = i.w[3];
  size_t at = m.AnnotationEnd();
  m.Insert(at++, Module::Make(kOpDecorate, {var, kDecDescriptorSet, group}));
  m.Insert(at++, Module::Make(kOpDecorate, {var, kDecBinding, binding}));
  for (uint32_t k = 0; k < struct_members[block]; k++)
    m.Insert(at++, Module::Make(kOpMemberDecorate, {block, k, kDecNonWritable}));
  // StorageBuffer is SPIR-V 1.3's, an extension before.
  if (m.version() < 0x00010300) {
    bool have = false;
    size_t last_cap = 0;
    for (size_t k = 0; k < m.insts().size(); k++) {
      const Inst& i = m.insts()[k];
      if (i.op == kOpCapability) last_cap = k;
      if (i.op == kOpExtension && Literal(i.w, 1, nullptr) == "SPV_KHR_storage_buffer_storage_class") have = true;
    }
    if (!have) {
      const char kName[] = "SPV_KHR_storage_buffer_storage_class";
      Inst e{kOpExtension, {0}};
      std::vector<uint32_t> lit((sizeof kName + 3) / 4, 0);
      memcpy(lit.data(), kName, sizeof kName);
      e.w.insert(e.w.end(), lit.begin(), lit.end());
      e.w[0] = kOpExtension | static_cast<uint32_t>(e.w.size()) << 16;
      m.Insert(last_cap + 1, e);
    }
  }
  return true;
}

// --- the y flip ----------------------------------------------------------------------------

class Types {
 public:
  explicit Types(Module& m) : m_(m) {}
  uint32_t Void() {
    return Find([](const Inst& i) { return i.op == kOpTypeVoid; }, [] { return std::vector<uint32_t>{}; },
                kOpTypeVoid);
  }
  uint32_t Float32() {
    return Find([](const Inst& i) { return i.op == kOpTypeFloat && i.w[2] == 32; },
                [] { return std::vector<uint32_t>{32}; }, kOpTypeFloat);
  }
  uint32_t Int32() {
    for (const auto& i : m_.insts())
      if (i.op == kOpTypeInt && i.w[2] == 32) return i.w[1];
    return Add(kOpTypeInt, {32, 1});
  }
  uint32_t Vec(uint32_t c, uint32_t n) {
    return Find([c, n](const Inst& i) { return i.op == kOpTypeVector && i.w[2] == c && i.w[3] == n; },
                [c, n] { return std::vector<uint32_t>{c, n}; }, kOpTypeVector);
  }
  uint32_t SInt32() {
    for (const auto& i : m_.insts())
      if (i.op == kOpTypeInt && i.w[2] == 32 && i.w[3] == 1) return i.w[1];
    return Add(kOpTypeInt, {32, 1});
  }
  uint32_t Vec4(uint32_t f) {
    return Find([f](const Inst& i) { return i.op == kOpTypeVector && i.w[2] == f && i.w[3] == 4; },
                [f] { return std::vector<uint32_t>{f, 4}; }, kOpTypeVector);
  }
  uint32_t VoidFn(uint32_t v) {
    return Find([v](const Inst& i) { return i.op == kOpTypeFunction && i.w.size() == 3 && i.w[2] == v; },
                [v] { return std::vector<uint32_t>{v}; }, kOpTypeFunction);
  }
  uint32_t Pointer(uint32_t sc, uint32_t t) {
    return Find([sc, t](const Inst& i) { return i.op == kOpTypePointer && i.w[2] == sc && i.w[3] == t; },
                [sc, t] { return std::vector<uint32_t>{sc, t}; }, kOpTypePointer);
  }
  uint32_t IntConstant(uint32_t type, uint32_t v) {
    return Find([type, v](const Inst& i) { return i.op == kOpConstant && i.w[1] == type && i.w[3] == v; },
                [v] { return std::vector<uint32_t>{v}; }, kOpConstant, type);
  }

 private:
  template <typename P, typename O>
  uint32_t Find(P pred, O operands, uint32_t op, uint32_t result_type = 0) {
    for (const auto& i : m_.insts())
      if (pred(i)) return result_type ? i.w[2] : i.w[1];
    std::vector<uint32_t> o = operands();
    if (result_type) {
      uint32_t id = m_.NewId();
      Inst inst{op, {0, result_type, id}};
      inst.w.insert(inst.w.end(), o.begin(), o.end());
      inst.w[0] = op | static_cast<uint32_t>(inst.w.size()) << 16;
      m_.Insert(m_.FirstFunction(), inst);
      return id;
    }
    return Add(op, o);
  }
  uint32_t Add(uint32_t op, const std::vector<uint32_t>& o) {
    uint32_t id = m_.NewId();
    Inst inst{op, {0, id}};
    inst.w.insert(inst.w.end(), o.begin(), o.end());
    inst.w[0] = op | static_cast<uint32_t>(inst.w.size()) << 16;
    m_.Insert(m_.FirstFunction(), inst);
    return id;
  }
  Module& m_;
};

bool FlipY(Module& m, uint32_t entry_index, std::string* error) {
  // gl_Position: an Output variable decorated Position, or a member of an
  // Output block (gl_PerVertex).
  uint32_t pos_var = 0, block = 0, member = 0;
  std::set<uint32_t> pos_ids;
  for (const auto& i : m.insts()) {
    if (i.op == kOpDecorate && i.w.size() >= 4 && i.w[2] == kDecBuiltIn && i.w[3] == kBuiltInPosition)
      pos_ids.insert(i.w[1]);
    if (i.op == kOpMemberDecorate && i.w.size() >= 5 && i.w[3] == kDecBuiltIn && i.w[4] == kBuiltInPosition) {
      block = i.w[1];
      member = i.w[2];
    }
  }
  std::map<uint32_t, std::pair<uint32_t, uint32_t>> ptr;  // pointer type -> (class, pointee)
  for (const auto& i : m.insts())
    if (i.op == kOpTypePointer) ptr[i.w[1]] = {i.w[2], i.w[3]};
  uint32_t block_var = 0;
  for (const auto& i : m.insts()) {
    if (i.op != kOpVariable || i.w.size() < 4 || i.w[3] != kScOutput) continue;
    if (pos_ids.count(i.w[2])) pos_var = i.w[2];
    auto p = ptr.find(i.w[1]);
    if (block && p != ptr.end() && p->second.second == block) block_var = i.w[2];
  }
  if (!pos_var && !block_var) return true;  // no position written
  Inst& ep = m.insts()[entry_index];
  uint32_t main = ep.w[2];
  Types t(m);
  uint32_t f32 = t.Float32();
  uint32_t v4 = t.Vec4(f32);
  uint32_t vd = t.Void();
  uint32_t fn = t.VoidFn(vd);
  uint32_t pv4 = t.Pointer(kScOutput, v4);
  uint32_t idx = 0;
  if (!pos_var) idx = t.IntConstant(t.Int32(), member);
  uint32_t wrap = m.NewId(), label = m.NewId(), call = m.NewId(), p = m.NewId(), v = m.NewId(), y = m.NewId(),
           ny = m.NewId(), v2 = m.NewId();
  std::vector<Inst> body;
  body.push_back(Module::Make(kOpFunction, {vd, wrap, 0, fn}));
  body.push_back(Module::Make(kOpLabel, {label}));
  body.push_back(Module::Make(kOpFunctionCall, {vd, call, main}));
  uint32_t where = pos_var;
  if (!pos_var) {
    body.push_back(Module::Make(kOpAccessChain, {pv4, p, block_var, idx}));
    where = p;
  }
  body.push_back(Module::Make(kOpLoad, {v4, v, where}));
  body.push_back(Module::Make(kOpCompositeExtract, {f32, y, v, 1}));
  body.push_back(Module::Make(kOpFNegate, {f32, ny, y}));
  body.push_back(Module::Make(kOpCompositeInsert, {v4, v2, ny, v, 1}));
  body.push_back(Module::Make(kOpStore, {where, v2}));
  body.push_back(Module::Make(kOpReturn, {}));
  body.push_back(Module::Make(kOpFunctionEnd, {}));
  // The entry point (index may have moved: types were inserted after it,
  // never before) now names the wrapper; so do its execution modes.
  for (auto& i : m.insts()) {
    if (i.op == kOpEntryPoint && i.w[2] == main) i.w[2] = wrap;
    if ((i.op == kOpExecutionMode || i.op == kOpExecutionModeId) && i.w[1] == main) i.w[1] = wrap;
  }
  for (auto& b : body) m.insts().push_back(std::move(b));
  (void)error;
  return true;
}


// --- input attachments --------------------------------------------------------------------

// subpassLoad(a, offset) -> texelFetch(a, ivec2(gl_FragCoord.xy) + offset, 0): an
// input attachment becomes a 2D texture of the previous pass's attachment
// (the subpasses are separate WebGPU passes).
bool InputAttachmentsToTextures(Module& m, uint32_t model, const std::string& name) {
  std::set<uint32_t> subpass_types;
  for (auto& i : m.insts()) {
    if (i.op == kOpTypeImage && i.w[3] == kDimSubpassData) {
      i.w[3] = 1;   // 2D
      i.w[7] = 1;   // sampled
      subpass_types.insert(i.w[1]);
    }
  }
  if (subpass_types.empty()) return true;
  auto& v = m.insts();
  for (size_t k = 0; k < v.size();) {
    bool drop = (v[k].op == kOpCapability && v[k].w[1] == kCapInputAttachment) ||
                (v[k].op == kOpDecorate && v[k].w.size() >= 3 && v[k].w[2] == kDecInputAttachmentIndex);
    if (drop) v.erase(v.begin() + k);
    else k++;
  }
  // gl_FragCoord: the module's, or a new input of the entry point.
  uint32_t frag = 0;
  std::set<uint32_t> fc_ids;
  for (const auto& i : m.insts())
    if (i.op == kOpDecorate && i.w.size() >= 4 && i.w[2] == kDecBuiltIn && i.w[3] == kBuiltInFragCoord)
      fc_ids.insert(i.w[1]);
  for (const auto& i : m.insts())
    if (i.op == kOpVariable && fc_ids.count(i.w[2])) frag = i.w[2];
  Types t(m);
  uint32_t f32 = t.Float32(), i32 = t.SInt32();
  uint32_t v4 = t.Vec4(f32), v2f = t.Vec(f32, 2), v2i = t.Vec(i32, 2);
  uint32_t zero = t.IntConstant(i32, 0);
  if (!frag) {
    uint32_t pv4 = t.Pointer(kScInput, v4);
    frag = m.NewId();
    m.Insert(m.FirstFunction(), Module::Make(kOpVariable, {pv4, frag, kScInput}));
    m.Insert(m.AnnotationEnd(), Module::Make(kOpDecorate, {frag, kDecBuiltIn, kBuiltInFragCoord}));
    // On the entry point's interface.
    for (auto& i : m.insts()) {
      if (i.op == kOpEntryPoint && i.w[1] == model && Literal(i.w, 3, nullptr) == name) {
        i.w.push_back(frag);
        i.w[0] = kOpEntryPoint | static_cast<uint32_t>(i.w.size()) << 16;
      }
    }
  }
  // The image types of the loaded values.
  std::map<uint32_t, uint32_t> type_of;
  for (const auto& i : m.insts())
    if (i.op == kOpLoad) type_of[i.w[2]] = i.w[1];
  for (size_t k = 0; k < m.insts().size(); k++) {
    Inst& i = m.insts()[k];
    if (i.op != kOpImageRead || !subpass_types.count(type_of[i.w[3]])) continue;
    uint32_t result_type = i.w[1], result = i.w[2], image = i.w[3], offset = i.w[4];
    uint32_t fc = m.NewId(), xy = m.NewId(), ixy = m.NewId(), coord = m.NewId();
    std::vector<Inst> add;
    add.push_back(Module::Make(kOpLoad, {v4, fc, frag}));
    add.push_back(Module::Make(kOpVectorShuffle, {v2f, xy, fc, fc, 0, 1}));
    add.push_back(Module::Make(kOpConvertFToS, {v2i, ixy, xy}));
    add.push_back(Module::Make(kOpIAdd, {v2i, coord, ixy, offset}));
    add.push_back(Module::Make(kOpImageFetch, {result_type, result, image, coord, 2 /* Lod */, zero}));
    m.insts().erase(m.insts().begin() + k);
    m.insts().insert(m.insts().begin() + k, add.begin(), add.end());
    k += add.size() - 1;
  }
  return true;
}

}  // namespace

bool PrepareSpirv(const std::vector<uint32_t>& in, const SpirvOptions& opt, std::vector<uint32_t>* out,
                  SpirvInfo* info, std::string* error) {
  Module m;
  if (!m.Parse(in, error)) return false;
  // The entry point.
  uint32_t model = ExecutionModel(opt.stage);
  size_t entry = SIZE_MAX;
  for (size_t k = 0; k < m.insts().size(); k++) {
    const Inst& i = m.insts()[k];
    if (i.op == kOpEntryPoint && i.w[1] == model && Literal(i.w, 3, nullptr) == opt.entry) entry = k;
  }
  if (entry == SIZE_MAX) {
    *error = "no entry point " + opt.entry + " for this stage";
    return false;
  }
  // What the pipeline needs to know: combined image samplers (their sampler
  // half gets a binding of its own) and storage buffers written.
  std::map<uint32_t, uint32_t> set_of, binding_of;
  std::set<uint32_t> non_writable_struct_members;  // structs whose members are all NonWritable
  std::map<uint32_t, uint32_t> nw_count, member_count;
  std::map<uint32_t, std::pair<uint32_t, uint32_t>> ptr;
  std::map<uint32_t, uint32_t> array_elem;
  std::set<uint32_t> sampled_images, buffer_blocks, blocks;
  for (const auto& i : m.insts()) {
    if (i.op == kOpDecorate && i.w.size() >= 4 && i.w[2] == kDecDescriptorSet) set_of[i.w[1]] = i.w[3];
    if (i.op == kOpDecorate && i.w.size() >= 4 && i.w[2] == kDecBinding) binding_of[i.w[1]] = i.w[3];
    if (i.op == kOpDecorate && i.w.size() >= 3 && i.w[2] == kDecBufferBlock) buffer_blocks.insert(i.w[1]);
    if (i.op == kOpDecorate && i.w.size() >= 3 && i.w[2] == kDecBlock) blocks.insert(i.w[1]);
    if (i.op == kOpMemberDecorate && i.w.size() >= 4 && i.w[3] == kDecNonWritable) nw_count[i.w[1]]++;
    if (i.op == kOpTypeStruct) member_count[i.w[1]] = static_cast<uint32_t>(i.w.size() - 2);
    if (i.op == kOpTypePointer) ptr[i.w[1]] = {i.w[2], i.w[3]};
    if (i.op == kOpTypeSampledImage) sampled_images.insert(i.w[1]);
    if (i.op == kOpTypeArray || i.op == kOpTypeRuntimeArray) array_elem[i.w[1]] = i.w[2];
  }
  std::set<uint32_t> non_writable_vars;
  for (const auto& i : m.insts())
    if (i.op == kOpDecorate && i.w.size() >= 3 && i.w[2] == kDecNonWritable) non_writable_vars.insert(i.w[1]);
  for (const auto& i : m.insts()) {
    if (i.op != kOpVariable || i.w.size() < 4) continue;
    uint32_t id = i.w[2], sc = i.w[3];
    auto p = ptr.find(i.w[1]);
    if (p == ptr.end()) continue;
    uint32_t t = p->second.second;
    while (array_elem.count(t)) t = array_elem[t];
    if (sc == kScUniformConstant && sampled_images.count(t))
      info->combined.push_back({set_of[id], binding_of[id]});
    bool ssbo = sc == kScStorageBuffer || (sc == kScUniform && buffer_blocks.count(t));
    if (ssbo && !non_writable_vars.count(id) && nw_count[t] < member_count[t])
      info->written_storage.push_back({set_of[id], binding_of[id]});
  }
  Specialize(m, opt.spec);
  if (opt.push_group >= 0) {
    bool found = false;
    if (!PushConstantsToStorage(m, opt.push_group, opt.push_binding, &found)) return false;
    info->has_push_constants = found;
  }
  if (opt.stage == VK_SHADER_STAGE_FRAGMENT_BIT) InputAttachmentsToTextures(m, model, opt.entry);
  if (opt.stage == VK_SHADER_STAGE_VERTEX_BIT && opt.flip_y) {
    // (The entry point's index is stable: insertions happen after it.)
    for (size_t k = 0; k < m.insts().size(); k++) {
      const Inst& i = m.insts()[k];
      if (i.op == kOpEntryPoint && i.w[1] == model && Literal(i.w, 3, nullptr) == opt.entry) entry = k;
    }
    if (!FlipY(m, entry, error)) return false;
  }
  m.Write(out);
  return true;
}

}  // namespace arcvk
