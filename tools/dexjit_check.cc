// Host check of the dex JIT's compiler (topic dex-jit): compiles every
// method of the given .dex files (and every loop header as an on-stack
// replacement entry) and writes the modules for tools/dexjit_check.js,
// which validates them with WebAssembly.validate.
//
//   c++ -std=c++14 -O1 -o build/dexjit_check tools/dexjit_check.cc shim/dexjit/dex_jit_compiler.cc
//   build/dexjit_check OUT.bin a.dex b.dex ...   # then: node tools/dexjit_check.js OUT.bin
//
// No runtime: the resolver knows nothing (every constant-pool reference goes
// to the runtime helpers), quickened instructions do not occur (dex files on
// disk are not quickened).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

#include "../shim/dexjit/dex_jit.h"

namespace {

struct Dex {
  std::vector<uint8_t> d;
  uint32_t u32(uint32_t off) const { uint32_t v; memcpy(&v, &d[off], 4); return v; }
  uint16_t u16(uint32_t off) const { uint16_t v; memcpy(&v, &d[off], 2); return v; }
  uint32_t uleb(uint32_t* off) const {
    uint32_t r = 0, shift = 0;
    while (true) {
      uint8_t b = d[(*off)++];
      r |= static_cast<uint32_t>(b & 0x7f) << shift;
      if (!(b & 0x80)) break;
      shift += 7;
    }
    return r;
  }
  int32_t sleb(uint32_t* off) const {
    int32_t r = 0;
    uint32_t shift = 0;
    uint8_t b;
    do {
      b = d[(*off)++];
      r |= static_cast<int32_t>(b & 0x7f) << shift;
      shift += 7;
    } while (b & 0x80);
    if (shift < 32 && (b & 0x40)) r |= -(1 << shift);
    return r;
  }
  std::string str(uint32_t idx) const {
    uint32_t off = u32(u32(60) + 4 * idx);
    uleb(&off);
    return std::string(reinterpret_cast<const char*>(&d[off]));
  }
  std::string type(uint32_t idx) const { return str(u32(u32(68) + 4 * idx)); }
};

class NoResolver : public dexjit::Resolver {
 public:
  uint32_t String(uint32_t) override { return 0; }
  uint32_t Class(uint32_t) override { return 0; }
  bool StaticField(uint32_t idx, uint32_t* object, uint32_t* offset, bool* is_volatile) override {
    if (idx % 3 == 0) return false;
    *object = 0x10000 + 8 * idx; *offset = 8 + (idx % 4) * 8; *is_volatile = idx % 2; return true;
  }
  bool InstanceField(uint32_t idx, uint32_t* offset, bool* is_volatile) override {
    if (idx % 3 == 0) return false;
    *offset = 8 + (idx % 4) * 8; *is_volatile = idx % 2; return true;
  }
  bool IsStringInit(uint32_t) override { return false; }
  uint32_t NewInstanceClass(uint32_t idx) override { return idx % 2 ? 0x6000 + 8 * idx : 0; }
  uint32_t NewArrayClass(uint32_t idx, uint32_t* shift) override {
    *shift = idx % 4;
    return idx % 3 ? 0x6000 + 8 * idx : 0;
  }
  // Every kind of call target, by method index.
  void Invoke(uint8_t op, uint32_t idx, dexjit::Callee* out) override {
    const bool is_static = op == 0x71 || op == 0x77;
    switch (idx % 4) {
      case 0: out->kind = dexjit::Callee::kNone; break;
      case 1: out->kind = dexjit::Callee::kKnown; out->method = 0x5000 + idx; out->is_static = is_static;
              out->initialized = (idx & 4) != 0; break;
      case 2: out->kind = is_static ? dexjit::Callee::kKnown : dexjit::Callee::kVirtual; out->index = idx % 50;
              out->is_static = is_static; out->method = 0x5000 + idx; break;
      default: out->kind = is_static ? dexjit::Callee::kKnown : dexjit::Callee::kInterface;
               out->is_static = is_static; out->index = idx % 43; out->method = 0x4000; break;
    }
    // Inlining and intrinsics: the target's code is the method's own code
    // (any method of the dex file, for the validity check).
    if (idx % 4 == 1 || idx % 4 == 2) {
      const auto it = codes.find(idx);
      if (it != codes.end()) {
        out->insns = it->second.insns;
        out->insns_size = it->second.insns_size;
        out->registers = it->second.registers;
        out->ins = it->second.ins;
        out->is_static = it->second.is_static;
        if (out->kind == dexjit::Callee::kVirtual) out->method = 0x5000 + idx;
      }
      out->intrinsic = static_cast<dexjit::Intrinsic>(idx % 7 == 0 ? 1 + (idx / 7) % (dexjit::kIntrinsicCount - 1) : 0);
    }
  }
  dexjit::Resolver* Local(uint32_t method, dexjit::Method* out) override {
    // (Targets 0x5000 + method index: the method itself, as for inlining.)
    if (method < 0x5000) return nullptr;
    const auto it = codes.find(method - 0x5000);
    if (it == codes.end()) return nullptr;
    *out = it->second;
    out->art_method = method;
    return this;
  }
  std::map<uint32_t, dexjit::Method> codes;
};

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s OUT.bin FILE.dex...\n", argv[0]);
    return 2;
  }
  FILE* out = fopen(argv[1], "wb");
  std::string names_path = std::string(argv[1]) + ".names";
  FILE* names = fopen(names_path.c_str(), "w");
  dexjit::Layout layout;
  layout.vregs = 36;
  layout.dex_pc = 28;
  layout.thread_flags = 0;
  layout.thread_exception = 132;
  layout.object_class = 0;
  layout.class_component = 60;
  layout.array_length = 8;
  layout.array_data[0] = 12;
  layout.array_data[1] = 12;
  layout.array_data[2] = 12;
  layout.array_data[3] = 16;
  layout.object_class_ptr = 0x1000;
  layout.card_table = 0x12345678;
  layout.card_shift = 10;
  layout.card_dirty = 0x70;
  layout.memory_shared = true;
  layout.fast_calls = true;
  layout.wasm_frames = getenv("WASM_FRAMES") == nullptr || atoi(getenv("WASM_FRAMES")) != 0;
  layout.frame_link = 0; layout.frame_method = 4; layout.frame_code_item = 16;
  layout.thread_top_shadow_frame = 200; layout.thread_jit = 300;
  layout.jit_fns = 0; layout.jit_cap = 4; layout.jit_used = 8; layout.jit_depth = 12; layout.jit_chunk = 16;
  layout.jit_depth_cap = 48; layout.method_data = 20;
  layout.jit_method_id = 4; layout.jit_method_code_item = 8; layout.jit_method_registers = 12;
  layout.chunk_top = 4; layout.chunk_end = 8; layout.chunk_data = 16; layout.calls_ok = 0x500;
  layout.frame_num_vregs = 24;
  layout.method_declaring_class = 0;
  layout.method_dex_index = 12;
  layout.class_status = 100;
  layout.class_initialized = 11;
  layout.class_vtable = 400;
  layout.class_imt = 380;
  layout.string_count = 8;
  layout.string_value = 16;
  layout.thread_peer = 120;
  // DIRECT=1: direct calls (topic dex-jit-speed), the calls module first.
  layout.direct_calls = getenv("DIRECT") != nullptr && atoi(getenv("DIRECT")) != 0;
  if (layout.direct_calls) {   // (and allocation in wasm)
    layout.alloc_ok = 0x600; layout.class_object_size_fast = 116;
    layout.thread_rosalloc_runs = 400; layout.thread_alloc_stack_top = 160;
    layout.thread_alloc_stack_end = 164; layout.rosalloc_max_size = 128;
    layout.rosalloc_quantum_shift = 3; layout.run_free_list_head = 8; layout.run_free_list_size = 24;
    layout.local_max = 4; layout.local_max_insns = 80; layout.suspend_poll = 64;
  }
  layout.jit_method_method = 0;
  if (layout.direct_calls) {
    layout.wasm_frames = false;
    std::vector<uint8_t> calls;
    dexjit::BuildCallsModule(layout, &calls);
    const uint32_t n = static_cast<uint32_t>(calls.size());
    fwrite(&n, 4, 1, out);
    fwrite(calls.data(), 1, n, out);
    fprintf(names, "(calls module)\n");
  }
  uint32_t methods = 0, ok = 0, refused = 0, osr = 0;
  size_t bytes = 0, max_size = 0;
  NoResolver resolver;
  for (int a = 2; a < argc; ++a) {
    Dex dex;
    FILE* f = fopen(argv[a], "rb");
    if (!f) { perror(argv[a]); return 1; }
    fseek(f, 0, SEEK_END);
    dex.d.resize(ftell(f));
    fseek(f, 0, SEEK_SET);
    if (fread(dex.d.data(), 1, dex.d.size(), f) != dex.d.size()) return 1;
    fclose(f);
    const uint32_t class_defs = dex.u32(96), class_defs_off = dex.u32(100);
    const uint32_t method_ids_off = dex.u32(92), proto_ids_off = dex.u32(76);
    resolver.codes.clear();
    for (uint32_t c = 0; c < class_defs; ++c) {
      const uint32_t cd = class_defs_off + 32 * c;
      uint32_t data = dex.u32(cd + 24);
      if (data == 0) continue;
      const uint32_t sf = dex.uleb(&data), inf = dex.uleb(&data);
      const uint32_t dm = dex.uleb(&data), vm = dex.uleb(&data);
      for (uint32_t k = 0; k < sf + inf; ++k) { dex.uleb(&data); dex.uleb(&data); }
      uint32_t midx = 0;
      for (uint32_t k = 0; k < dm + vm; ++k) {
        if (k == dm) midx = 0;
        midx += dex.uleb(&data);
        const uint32_t flags = dex.uleb(&data);
        const uint32_t code = dex.uleb(&data);
        if (code == 0) continue;
        const uint32_t mid = method_ids_off + 8 * midx;
        const uint16_t proto = dex.u16(mid + 2);
        const std::string shorty = dex.str(dex.u32(proto_ids_off + 12 * proto));
        const std::string name = dex.type(dex.u16(mid)) + "." + dex.str(dex.u32(mid + 4));
        dexjit::Method m;
        m.art_method = 0x7000 + 64 * midx;
        m.registers = dex.u16(code);
        m.ins = dex.u16(code + 2);
        const uint16_t tries = dex.u16(code + 6);
        m.insns_size = dex.u32(code + 12);
        m.insns = reinterpret_cast<const uint16_t*>(&dex.d[code + 16]);
        m.shorty = shorty.c_str();
        m.is_static = (flags & 0x8) != 0;
        uint32_t toff = code + 16 + 2 * m.insns_size;
        if (tries && (m.insns_size & 1)) toff += 2;
        const uint32_t handlers_base = toff + 8 * tries;
        for (uint32_t t = 0; t < tries; ++t) {
          dexjit::TryItem ti;
          ti.start = dex.u32(toff + 8 * t);
          ti.count = dex.u16(toff + 8 * t + 4);
          uint32_t h = handlers_base + dex.u16(toff + 8 * t + 6);
          int32_t size = dex.sleb(&h);
          for (int32_t s = 0; s < (size < 0 ? -size : size); ++s) {
            dex.uleb(&h);
            ti.handlers.push_back(dex.uleb(&h));
          }
          if (size <= 0) ti.handlers.push_back(dex.uleb(&h));
          m.tries.push_back(ti);
        }
        ++methods;
        resolver.codes[midx] = m;
        // Entries: 0, and the targets of backward branches (loop headers).
        std::vector<uint32_t> entries(1, 0);
        for (uint32_t t : dexjit::BackwardBranchTargets(m)) {
          if (t != 0) entries.push_back(t);
        }
        for (uint32_t e : entries) {
          dexjit::Options o;
          o.entry_pc = e;
          std::vector<uint8_t> mod;
          std::string why;
          if (!dexjit::Compile(m, layout, &resolver, o, &mod, &why)) {
            if (e == 0) {
              ++refused;
              fprintf(names, "# refused %s%s: %s\n", name.c_str(), shorty.c_str(), why.c_str());
            }
            continue;
          }
          if (e == 0) ++ok; else ++osr;
          const uint32_t n = static_cast<uint32_t>(mod.size());
          fwrite(&n, 4, 1, out);
          fwrite(mod.data(), 1, n, out);
          fprintf(names, "%s%s @%u\n", name.c_str(), shorty.c_str(), e);
          bytes += n;
          if (n > max_size) max_size = n;
        }
      }
    }
  }
  fclose(out);
  fclose(names);
  printf("%u methods: %u compiled, %u refused, %u osr entries; %zu bytes (largest %zu)\n", methods,
         ok, refused, osr, bytes, max_size);
  return 0;
}
