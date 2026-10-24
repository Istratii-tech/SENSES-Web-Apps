// Static instruction coverage of the NDK CPU on a real library: walks the
// code of an ARM .so by recursive descent (from the dynamic symbols, the
// .ARM.exidx function starts and every call target found on the way)
// through the CPU's own decoders and reports what they cannot decode, plus
// NEON encodings the executor rejects (run once on a scratch state).
//
//   node ndk_cpu_scan.js LIB.so      (built with NODERAWFS: reads host files)
#include <elf.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <set>
#include <string>
#include <vector>

#include "ndk/cpu/cpu_internal.h"

using namespace ndkcpu;

namespace {

uint32_t g_base, g_text_lo, g_text_hi;
std::set<uint32_t> g_seen;                 // instruction address | thumb
std::vector<uint32_t> g_work;              // entry address | thumb
std::map<uint32_t, std::string> g_undef;   // raw word -> "reason @addr"
std::map<uint32_t, int> g_undef_count;
std::map<uint32_t, std::string> g_neon_bad;
std::map<std::string, int> g_kinds;
uint64_t g_insns[2];
Cpu* g_scratch;
jmp_buf g_jmp;
std::string g_fault;

void OnFault(NdkCpuState*, uint32_t, const char* msg) {
  g_fault = msg;
  longjmp(g_jmp, 1);
}

bool InText(uint32_t a) { return a >= g_text_lo && a < g_text_hi; }

void TryNeon(const DInsn& d, uint32_t addr) {
  if (g_neon_bad.count(d.imm)) return;
  static uint8_t scratch[4096] __attribute__((aligned(16)));
  for (int i = 0; i < 15; i++) g_scratch->r[i] = (uint32_t)(uintptr_t)(scratch + 1024);
  g_scratch->r[15] = addr + 8;
  g_fault.clear();
  if (setjmp(g_jmp) == 0) {
    if (d.op == OP_NEON) NeonExec(g_scratch, d.imm);
    else NeonLdSt(g_scratch, d.imm);
  }
  if (!g_fault.empty()) {
    char buf[64];
    snprintf(buf, sizeof(buf), "@%x", addr - g_base);
    g_neon_bad[d.imm] = buf;
  }
}

const char* Kind(const DInsn& d) {
  if (d.op >= OP_MEM_BASE) return "load/store";
  if (d.op >= OP_DP_BASE) return "data processing";
  switch (d.op) {
    case OP_B: case OP_BL: case OP_BLX_IMM: case OP_BX: case OP_BLX_REG: case OP_CBZ: case OP_TBB: case OP_TBH:
    case OP_DP_PC: case OP_LDR_PC: case OP_LDM_PC: return "branch";
    case OP_LDM: case OP_STM: return "load/store multiple";
    case OP_VFP_BIN: case OP_VFP_UN: case OP_VFP_CMP: case OP_VFP_CVT: case OP_VFP_MOVI: case OP_VFP_LDST:
    case OP_VFP_LDM: case OP_VMOV_CORE_S: case OP_VMOV_CORE_D: case OP_VMOV_SCALAR: case OP_VMRS: case OP_VMSR:
      return "VFP";
    case OP_NEON: case OP_NEON_LDST: case OP_VDUP_CORE: return "NEON";
    case OP_LDREX: case OP_STREX: case OP_CLREX: case OP_DMB: case OP_SWP: case OP_SWPB: return "sync";
    case OP_SVC: return "svc";
    case OP_MRC_TLS: return "mrc tls";
    case OP_UNDEF: return "UNDEFINED";
    default: return "other (mul, media, misc)";
  }
}

void Walk(uint32_t entry) {
  bool thumb = entry & 1;
  uint32_t a = entry & ~1u;
  uint8_t it = 0;
  for (;;) {
    if (!InText(a)) return;
    uint32_t key = a | thumb;
    if (g_seen.count(key)) return;
    g_seen.insert(key);
    DInsn d;
    bool term = thumb ? DecodeThumb(a, &it, &d) : DecodeArm(a, Ld32(a), &d);
    g_insns[thumb]++;
    g_kinds[Kind(d)]++;
    if (d.op == OP_UNDEF) {
      uint32_t raw = thumb && d.len == 4 ? (Ld16(a) << 16) | Ld16(a + 2) : thumb ? Ld16(a) : Ld32(a);
      static const char* const why[] = {"undefined", "unsupported", "unpredictable", "udf", "bad address"};
      char buf[96];
      snprintf(buf, sizeof(buf), "%s %s @%x", thumb ? "thumb" : "arm", why[d.imm2 & 7], a - g_base);
      if (!g_undef.count(raw)) g_undef[raw] = buf;
      g_undef_count[raw]++;
      return;   // most likely data reached through a mis-guessed entry
    }
    if (d.op == OP_NEON || d.op == OP_NEON_LDST) TryNeon(d, a);
    switch (d.op) {
      case OP_BL:
        g_work.push_back(d.imm | thumb);
        break;
      case OP_BLX_IMM:
        g_work.push_back(d.imm);   // bit 0 already selects the state
        break;
      case OP_B: case OP_CBZ:
        g_work.push_back(d.imm | thumb);
        break;
    }
    bool is_call = d.op == OP_BL || d.op == OP_BLX_IMM || d.op == OP_BLX_REG;
    if (term && !is_call) return;
    a += d.len;
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) { fprintf(stderr, "usage: ndk_cpu_scan LIB.so\n"); return 2; }
  FILE* f = fopen(argv[1], "rb");
  if (!f) { perror(argv[1]); return 2; }
  std::vector<uint8_t> data;
  static uint8_t buf[1 << 16];   // not on the 64 KiB wasm stack
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.insert(data.end(), buf, buf + n);
  fclose(f);
  const Elf32_Ehdr* eh = reinterpret_cast<const Elf32_Ehdr*>(data.data());
  const Elf32_Phdr* ph = reinterpret_cast<const Elf32_Phdr*>(data.data() + eh->e_phoff);
  uint32_t end = 0;
  for (int i = 0; i < eh->e_phnum; i++)
    if (ph[i].p_type == PT_LOAD) end = std::max(end, ph[i].p_vaddr + ph[i].p_memsz);
  void* mem = aligned_alloc(4096, (end + 4095) & ~4095u);
  memset(mem, 0, end);
  g_base = (uint32_t)(uintptr_t)mem;
  for (int i = 0; i < eh->e_phnum; i++)
    if (ph[i].p_type == PT_LOAD) memcpy((char*)mem + ph[i].p_vaddr, data.data() + ph[i].p_offset, ph[i].p_filesz);
  const Elf32_Shdr* sh = reinterpret_cast<const Elf32_Shdr*>(data.data() + eh->e_shoff);
  const char* shstr = reinterpret_cast<const char*>(data.data() + sh[eh->e_shstrndx].sh_offset);
  std::vector<uint32_t> exidx_starts;
  int nsyms = 0;
  for (int i = 0; i < eh->e_shnum; i++) {
    std::string name = shstr + sh[i].sh_name;
    if (name == ".text") { g_text_lo = g_base + sh[i].sh_addr; g_text_hi = g_text_lo + sh[i].sh_size; }
    if (name == ".ARM.exidx") {
      for (uint32_t k = 0; k < sh[i].sh_size; k += 8) {
        uint32_t w0;
        memcpy(&w0, data.data() + sh[i].sh_offset + k, 4);
        int32_t off = (int32_t)(w0 << 1) >> 1;
        exidx_starts.push_back(g_base + sh[i].sh_addr + k + off);
      }
    }
  }
  for (int i = 0; i < eh->e_shnum; i++) {
    if (sh[i].sh_type != SHT_DYNSYM) continue;
    const Elf32_Sym* syms = reinterpret_cast<const Elf32_Sym*>(data.data() + sh[i].sh_offset);
    for (size_t k = 0; k < sh[i].sh_size / sizeof(Elf32_Sym); k++) {
      if (ELF32_ST_TYPE(syms[k].st_info) != STT_FUNC || syms[k].st_shndx == SHN_UNDEF) continue;
      g_work.push_back(g_base + syms[k].st_value);
      nsyms++;
    }
  }
  g_scratch = static_cast<Cpu*>(calloc(1, sizeof(Cpu)));
  ndk_set_fault_handler(OnFault);
  auto drain = [] {
    while (!g_work.empty()) {
      uint32_t e = g_work.back();
      g_work.pop_back();
      Walk(e);
    }
  };
  drain();
  uint64_t from_syms = g_insns[0] + g_insns[1];
  // exidx starts not reached from the symbols: decode the first
  // instructions in both states and take the one that looks like code
  // (fewer undecodable words; Thumb if not word-aligned).
  auto trial = [](uint32_t a, bool thumb) {
    int bad = 0, al = 0;
    uint8_t it = 0;
    for (int k = 0; k < 24 && InText(a + 4); k++) {
      DInsn d;
      bool term = thumb ? DecodeThumb(a, &it, &d) : DecodeArm(a, Ld32(a), &d);
      if (d.op == OP_UNDEF) bad++;
      if (!thumb && (Ld32(a) >> 28) == 0xe) al++;
      a += d.len;
      if (term && d.op != OP_BL && d.op != OP_BLX_IMM && d.op != OP_BLX_REG) break;
    }
    return bad * 16 - (thumb ? 0 : al);
  };
  int guessed = 0;
  for (uint32_t s : exidx_starts) {
    if (g_seen.count(s) || g_seen.count(s | 1) || !InText(s)) continue;
    bool thumb = (s & 2) || trial(s, true) < trial(s, false);
    g_work.push_back(thumb ? (s | 1) : s);
    guessed++;
    drain();
  }
  printf("%s\n", argv[1]);
  printf("  entry points: %d dynamic symbols, %zu exidx functions (%d entered by guess)\n", nsyms,
         exidx_starts.size(), guessed);
  printf("  instructions decoded: %llu ARM, %llu Thumb (%llu reached from symbols and calls)\n",
         (unsigned long long)g_insns[0], (unsigned long long)g_insns[1], (unsigned long long)from_syms);
  for (auto& k : g_kinds) printf("    %-28s %d\n", k.first.c_str(), k.second);
  printf("  undecodable encodings: %zu distinct\n", g_undef.size());
  for (auto& u : g_undef) printf("    %08x x%-5d %s\n", u.first, g_undef_count[u.first], u.second.c_str());
  printf("  NEON encodings the executor rejects: %zu distinct\n", g_neon_bad.size());
  for (auto& u : g_neon_bad) printf("    %08x %s\n", u.first, u.second.c_str());
  return 0;
}
