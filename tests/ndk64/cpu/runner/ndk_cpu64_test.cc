// Test runner of the AArch64 CPU (topic "ndk-arm64"): loads the guest test
// programs (position-independent AArch64 ELF files built by
// mk/topic-ndk-arm64.mk, embedded at /guest), gives them host stubs, runs
// them through ndk64_call_guest, compares the differential cases with the
// native reference run (/guest/golden.bin) and checks the CPU's runtime
// contract (re-entrancy, AAPCS64 calls both ways, threads,
// ndk64_flush_code, faults). Runs under Node and in browsers.
//
//   ndk_cpu64_test.js [VARIANT-SUBSTRING...] [--bench [N]] [--no-threads] [--jit THRESHOLD]
//                     [--stress ROUNDS] [--max-fail N] [--windows]
// --windows: as once the bridge has a reservation window (ndk64_windows_on):
// every guest address through the window tables (all of these: table 0).
#include <ctype.h>
#include <dirent.h>
#include <elf.h>
#include <math.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "ndk/ndk_cpu64.h"

namespace {

int g_pass, g_fail, g_max_fail_print = 60, g_printed;
std::string g_variant;

void Fail(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Fail(const char* fmt, ...) {
  g_fail++;
  if (g_printed++ >= g_max_fail_print) {
    if (g_printed == g_max_fail_print + 1) printf("(more failures not shown)\n");
    return;
  }
  va_list ap;
  va_start(ap, fmt);
  printf("FAIL [%s] ", g_variant.c_str());
  vprintf(fmt, ap);
  printf("\n");
  va_end(ap);
}
void Expect(bool ok, const char* what) {
  if (ok) g_pass++;
  else Fail("%s", what);
}

uint64_t NowUs() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

std::vector<uint8_t> ReadFile(const std::string& path) {
  std::vector<uint8_t> data;
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return data;
  static uint8_t buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.insert(data.end(), buf, buf + n);
  fclose(f);
  return data;
}

// --- guest images --------------------------------------------------------------------------
struct Symbol {
  uint32_t addr, size;
  std::string name;
};
struct Image {
  std::string name;
  uint32_t base = 0, size = 0;
  std::vector<Symbol> syms;
  uint32_t Find(const char* name) const {
    for (const Symbol& s : syms)
      if (s.name == name) return s.addr;
    return 0;
  }
};
std::vector<Image*> g_images;

const char* Symbolize(uint32_t addr, uint32_t* off) {
  for (Image* img : g_images) {
    if (addr < img->base || addr >= img->base + img->size) continue;
    const Symbol* best = nullptr;
    for (const Symbol& s : img->syms)
      if (s.addr <= addr && (!best || s.addr > best->addr)) best = &s;
    if (best) {
      *off = addr - best->addr;
      return best->name.c_str();
    }
  }
  return nullptr;
}

bool LoadElf(const std::string& path, Image* img) {
  std::vector<uint8_t> data = ReadFile(path);
  const Elf64_Ehdr* eh = reinterpret_cast<const Elf64_Ehdr*>(data.data());
  if (data.size() < sizeof(*eh) || memcmp(eh->e_ident, ELFMAG, SELFMAG) || eh->e_ident[EI_CLASS] != ELFCLASS64 ||
      eh->e_machine != EM_AARCH64)
    return false;
  const Elf64_Phdr* ph = reinterpret_cast<const Elf64_Phdr*>(data.data() + eh->e_phoff);
  uint64_t end = 0;
  for (int i = 0; i < eh->e_phnum; i++)
    if (ph[i].p_type == PT_LOAD) end = std::max<uint64_t>(end, ph[i].p_vaddr + ph[i].p_memsz);
  end = (end + 4095) & ~4095ull;
  void* mem = aligned_alloc(4096, end);
  memset(mem, 0, end);
  img->base = (uint32_t)(uintptr_t)mem;
  img->size = (uint32_t)end;
  for (int i = 0; i < eh->e_phnum; i++)
    if (ph[i].p_type == PT_LOAD) memcpy((char*)mem + ph[i].p_vaddr, data.data() + ph[i].p_offset, ph[i].p_filesz);
  const Elf64_Shdr* sh = reinterpret_cast<const Elf64_Shdr*>(data.data() + eh->e_shoff);
  for (int i = 0; i < eh->e_shnum; i++) {
    if (sh[i].sh_type == SHT_RELA) {
      const Elf64_Rela* rel = reinterpret_cast<const Elf64_Rela*>(data.data() + sh[i].sh_offset);
      const Elf64_Shdr& symsec = sh[sh[i].sh_link];
      const Elf64_Sym* dsyms = reinterpret_cast<const Elf64_Sym*>(data.data() + symsec.sh_offset);
      for (size_t k = 0; k < sh[i].sh_size / sizeof(Elf64_Rela); k++) {
        uint64_t* where = reinterpret_cast<uint64_t*>((char*)mem + rel[k].r_offset);
        uint32_t type = ELF64_R_TYPE(rel[k].r_info), sym = ELF64_R_SYM(rel[k].r_info);
        switch (type) {
          case R_AARCH64_RELATIVE: *where = img->base + rel[k].r_addend; break;
          case R_AARCH64_ABS64: case R_AARCH64_GLOB_DAT: case R_AARCH64_JUMP_SLOT:
            if (dsyms[sym].st_shndx == SHN_UNDEF) {
              printf("%s: undefined symbol in relocation\n", path.c_str());
              return false;
            }
            *where = img->base + dsyms[sym].st_value + rel[k].r_addend;
            break;
          default:
            printf("%s: unsupported relocation %u\n", path.c_str(), type);
            return false;
        }
      }
    }
    if (sh[i].sh_type == SHT_SYMTAB) {
      const Elf64_Sym* syms = reinterpret_cast<const Elf64_Sym*>(data.data() + sh[i].sh_offset);
      const char* strtab = reinterpret_cast<const char*>(data.data() + sh[sh[i].sh_link].sh_offset);
      for (size_t k = 0; k < sh[i].sh_size / sizeof(Elf64_Sym); k++) {
        int type = ELF64_ST_TYPE(syms[k].st_info);
        if ((type != STT_FUNC && type != STT_OBJECT && type != STT_NOTYPE) || syms[k].st_shndx == SHN_UNDEF ||
            syms[k].st_name == 0)
          continue;
        const char* n = strtab + syms[k].st_name;
        if (n[0] == '$' || n[0] == '.') continue;   // mapping symbols
        img->syms.push_back({(uint32_t)(img->base + syms[k].st_value), (uint32_t)syms[k].st_size, n});
      }
    }
  }
  std::sort(img->syms.begin(), img->syms.end(), [](const Symbol& a, const Symbol& b) { return a.addr < b.addr; });
  ndk64_flush_code(img->base, img->size);
  return true;
}

// --- the reference results of the differential cases ------------------------------------------------
struct Golden {
  std::vector<uint64_t> words;
  std::map<int, std::pair<size_t, int>> at;   // index -> (offset, count)
  bool loaded = false;
};
Golden g_golden;
int g_diff_pass, g_diff_fail;

void LoadGolden() {
  std::vector<uint8_t> d = ReadFile("/guest/golden.bin");
  if (d.size() < 4 || memcmp(d.data(), "A64D", 4)) return;
  g_golden.words.reserve(d.size() / 8);
  size_t p = 4;
  while (p + 8 <= d.size()) {
    uint32_t head[2];
    memcpy(head, d.data() + p, 8);
    p += 8;
    size_t off = g_golden.words.size();
    for (uint32_t k = 0; k < head[1] && p + 8 <= d.size(); k++, p += 8) {
      uint64_t w;
      memcpy(&w, d.data() + p, 8);
      g_golden.words.push_back(w);
    }
    g_golden.at[(int)head[0]] = {off, (int)head[1]};
  }
  g_golden.loaded = true;
}

// --- host functions given to the guest (struct host_api in rt/guest.h) ---------------------------
inline const char* Str(uint64_t a) { return reinterpret_cast<const char*>((uintptr_t)(uint32_t)a); }
inline double D(const NdkCpu64State* s, int i) { double d; memcpy(&d, &s->v[i][0], 8); return d; }
inline float F(const NdkCpu64State* s, int i) { float f; memcpy(&f, &s->v[i][0], 4); return f; }
inline void RetD(NdkCpu64State* s, double d) { memcpy(&s->v[0][0], &d, 8); s->v[0][1] = 0; }
inline uint64_t Stack(const NdkCpu64State* s, int i) {
  uint64_t v;
  memcpy(&v, reinterpret_cast<void*>((uintptr_t)(uint32_t)(s->sp + 8 * i)), 8);
  return v;
}

void HostWrite(NdkCpu64State* s, void*) { fwrite(Str(s->x[0]), 1, (uint32_t)s->x[1], stdout); }
void HostCheck(NdkCpu64State* s, void*) {
  if ((uint32_t)s->x[0]) g_pass++;
  else Fail("%s:%u: CHECK(%s)", Str(s->x[2]), (uint32_t)s->x[3], Str(s->x[1]));
}
void HostCheckEq(NdkCpu64State* s, void*) {
  uint64_t got = s->x[0], want = s->x[1];
  if (got == want) g_pass++;
  else Fail("line %u: %s: got 0x%llx, want 0x%llx", (uint32_t)s->x[3], Str(s->x[2]), (unsigned long long)got,
            (unsigned long long)want);
}

int64_t UlpsD(double a, double b) {
  if (a == b) return 0;
  if (isnan(a) && isnan(b)) return 0;
  int64_t x, y;
  memcpy(&x, &a, 8); memcpy(&y, &b, 8);
  if (x < 0) x = INT64_MIN - x;
  if (y < 0) y = INT64_MIN - y;
  return x > y ? x - y : y - x;
}
int64_t UlpsF(float a, float b) {
  if (a == b) return 0;
  if (isnan(a) && isnan(b)) return 0;
  int32_t x, y;
  memcpy(&x, &a, 4); memcpy(&y, &b, 4);
  if (x < 0) x = INT32_MIN - x;
  if (y < 0) y = INT32_MIN - y;
  return x > y ? (int64_t)x - y : (int64_t)y - x;
}

// check_math(const char* fn, double x, double y, double result, int line):
// x0 = fn, d0..d2 = x, y, result, w1 = line.
void HostCheckMath(NdkCpu64State* s, void*) {
  std::string fn = Str(s->x[0]);
  double x = D(s, 0), y = D(s, 1), r = D(s, 2);
  uint32_t line = (uint32_t)s->x[1];
  double want;
  bool single = false;
  if (fn == "sin") want = sin(x);
  else if (fn == "cos") want = cos(x);
  else if (fn == "atan") want = atan(x);
  else if (fn == "exp") want = exp(x);
  else if (fn == "floor") want = floor(x);
  else if (fn == "ceil") want = ceil(x);
  else if (fn == "fabs") want = fabs(x);
  else if (fn == "atan2") want = atan2(x, y);
  else if (fn == "fmod") want = fmod(x, y);
  else if (fn == "log") want = log(x);
  else if (fn == "log10") want = log10(x);
  else if (fn == "pow") want = pow(x, y);
  else if (fn == "sqrt") want = sqrt(x);
  else if (fn == "sinf") { want = sinf((float)x); single = true; }
  else if (fn == "cosf") { want = cosf((float)x); single = true; }
  else if (fn == "expf") { want = expf((float)x); single = true; }
  else if (fn == "logf") { want = logf((float)x); single = true; }
  else { Fail("line %u: unknown math function %s", line, fn.c_str()); return; }
  int64_t ulps = single ? UlpsF((float)r, (float)want) : UlpsD(r, want);
  if (ulps <= 2) g_pass++;
  else Fail("line %u: %s(%.17g, %.17g) = %.17g, host %.17g (%lld ulps)", line, fn.c_str(), x, y, r, want, (long long)ulps);
}

// ping(fn, depth, a, b, c, d, e): calls guest fn(depth - 1, a..e) while depth > 0.
void HostPing(NdkCpu64State* s, void*) {
  uint32_t fn = (uint32_t)s->x[0], depth = (uint32_t)s->x[1];
  if (depth == 0) { s->x[0] = 0; return; }
  uint64_t args[6] = {depth - 1, (uint32_t)s->x[2], (uint32_t)s->x[3], (uint32_t)s->x[4], (uint32_t)s->x[5],
                      (uint32_t)s->x[6]};
  uint64_t sp_before = s->sp, lr_before = s->x[30], x6 = s->x[6];
  uint64_t r = ndk64_call_guest(fn, args, 6);
  if (s->sp != sp_before || s->x[30] != lr_before || s->x[6] != x6) Fail("ping: sp/lr/x6 not restored after ndk64_call_guest");
  s->x[0] = r + depth;
}

void HostSum8(NdkCpu64State* s, void*) {
  uint32_t r = 0;
  for (int i = 0; i < 8; i++) r += (uint32_t)s->x[i] * (i + 1);
  s->x[0] = r;
}
void HostMul64(NdkCpu64State* s, void*) { s->x[0] = s->x[0] * s->x[1]; }
void HostFma3(NdkCpu64State* s, void*) { RetD(s, D(s, 0) * D(s, 1) + D(s, 2)); }
void HostNowUs(NdkCpu64State* s, void*) { s->x[0] = NowUs(); }
void HostSetTls(NdkCpu64State* s, void*) { s->tpidr = (uint32_t)s->x[0]; }
void HostSum12(NdkCpu64State* s, void*) {
  int64_t r = 0;
  for (int i = 0; i < 8; i++) r += (int64_t)s->x[i] * (i + 1);
  for (int i = 0; i < 4; i++) r += (int64_t)Stack(s, i) * (i + 9);
  s->x[0] = (uint64_t)r;
}
// mixed(int a, double b, float c, int64_t d, double e..k (6), int l..q (6), float r):
// x0 a, d0 b, s1 c, x1 d, d2..d7 e..j, k on the stack[0] (8 bytes), x2..x7 l..q, r on stack[1] (low 4 bytes).
void HostMixed(NdkCpu64State* s, void*) {
  double r = (int32_t)s->x[0] + D(s, 0) * 2 + F(s, 1) * 3 + (double)(int64_t)s->x[1] * 4;
  for (int i = 2; i < 8; i++) r += D(s, i) * (i + 3);
  double k;
  uint64_t kb = Stack(s, 0);
  memcpy(&k, &kb, 8);
  r += k * 11;
  for (int i = 2; i < 8; i++) r += (int32_t)s->x[i] * (i + 10);
  uint64_t rb = Stack(s, 1);
  float rf;
  memcpy(&rf, &rb, 4);
  r += rf * 18;
  RetD(s, r);
}
// call_mixed(fn): calls guest fn(x0..x7 = 1..8, d0..d7 = 0.5..7.5, stack: 9, 10.5, 11) -> double.
void HostCallMixed(NdkCpu64State* s, void*) {
  Ndk64Args a;
  memset(&a, 0, sizeof a);
  for (int i = 0; i < 8; i++) {
    a.x[i] = i + 1;
    double d = i + 0.5;
    memcpy(&a.v[i], &d, 8);
  }
  double d105 = 10.5;
  uint64_t stack[3] = {9, 0, 11};
  memcpy(&stack[1], &d105, 8);
  a.stack = stack;
  a.nstack = 3;
  Ndk64Result r;
  ndk64_call_guest_args((uint32_t)s->x[0], &a, &r);
  memcpy(&s->v[0][0], &r.v0, 8);
  s->v[0][1] = 0;
}

void HostCompareCase(NdkCpu64State* s, void*) {
  int index = (int32_t)s->x[0];
  const char* name = Str(s->x[1]);
  const uint64_t* out = reinterpret_cast<const uint64_t*>((uintptr_t)(uint32_t)s->x[2]);
  int n = (int32_t)s->x[3];
  if (!g_golden.loaded) {
    if (index == 0) Fail("no reference results (/guest/golden.bin)");
    return;
  }
  auto it = g_golden.at.find(index);
  if (it == g_golden.at.end() || it->second.second != n) {
    g_diff_fail++;
    Fail("case %d (%s): no reference result / output count %d", index, name, n);
    return;
  }
  const uint64_t* want = g_golden.words.data() + it->second.first;
  if (memcmp(out, want, 8 * n) == 0) {
    g_pass++;
    g_diff_pass++;
    return;
  }
  g_diff_fail++;
  std::string diff;
  for (int k = 0; k < n; k++) {
    if (out[k] == want[k]) continue;
    char b[96];
    snprintf(b, sizeof b, " [%d] %016llx want %016llx", k, (unsigned long long)out[k], (unsigned long long)want[k]);
    diff += b;
    if (diff.size() > 300) break;
  }
  Fail("case %d: %s:%s", index, name, diff.c_str());
}

constexpr int kHostFns = 14;
uint64_t g_host_table[kHostFns];

void MakeStubs() {
  g_host_table[0] = ndk64_host_stub(HostWrite, nullptr, "host.write");
  g_host_table[1] = ndk64_host_stub(HostCheck, nullptr, "host.check");
  g_host_table[2] = ndk64_host_stub(HostCheckEq, nullptr, "host.check_eq");
  g_host_table[3] = ndk64_host_stub(HostCheckMath, nullptr, "host.check_math");
  g_host_table[4] = ndk64_host_stub(HostPing, nullptr, "host.ping");
  g_host_table[5] = ndk64_host_stub(HostSum8, nullptr, "host.sum8");
  g_host_table[6] = ndk64_host_stub(HostMul64, nullptr, "host.mul64");
  g_host_table[7] = ndk64_host_stub(HostFma3, nullptr, "host.fma3");
  g_host_table[8] = ndk64_host_stub(HostNowUs, nullptr, "host.now_us");
  g_host_table[9] = ndk64_host_stub(HostSetTls, nullptr, "host.set_tls");
  g_host_table[10] = ndk64_host_stub(HostSum12, nullptr, "host.sum12");
  g_host_table[11] = ndk64_host_stub(HostMixed, nullptr, "host.mixed");
  g_host_table[12] = ndk64_host_stub(HostCallMixed, nullptr, "host.call_mixed");
  g_host_table[13] = ndk64_host_stub(HostCompareCase, nullptr, "host.compare_case");
}

// --- runtime contract tests -------------------------------------------------------------------
struct ThreadArg {
  uint32_t fn, counter;
  int n;
};
void* ThreadMain(void* p) {
  ThreadArg* a = static_cast<ThreadArg*>(p);
  uint64_t args[2] = {a->counter, (uint64_t)a->n};
  ndk64_call_guest(a->fn, args, 2);
  return nullptr;
}

void TestThreads(const Image& img) {
  uint32_t fn = img.Find("atomic_worker");
  if (!fn) { Fail("no atomic_worker"); return; }
  int* counter = static_cast<int*>(aligned_alloc(64, 64));
  *counter = 0;
  const int kThreads = 4, kIters = 20000;
  pthread_t th[kThreads];
  ThreadArg arg = {fn, (uint32_t)(uintptr_t)counter, kIters};
  for (int i = 0; i < kThreads; i++) pthread_create(&th[i], nullptr, ThreadMain, &arg);
  for (int i = 0; i < kThreads; i++) pthread_join(th[i], nullptr);
  if (*counter == kThreads * kIters) g_pass++;
  else Fail("threads: counter %d, want %d", *counter, kThreads * kIters);
  free(counter);
}

void* SpinThread(void* p) {
  ThreadArg* a = static_cast<ThreadArg*>(p);
  uint64_t args[1] = {a->counter};
  a->n = (int)ndk64_call_guest(a->fn, args, 1);
  return nullptr;
}

void TestSpin(const Image& img) {
  uint32_t fn = img.Find("spin_wait");
  if (!fn) { Fail("no spin_wait"); return; }
  volatile int* flag = static_cast<volatile int*>(aligned_alloc(64, 64));
  *flag = 0;
  ThreadArg arg = {fn, (uint32_t)(uintptr_t)flag, -1};
  pthread_t th;
  pthread_create(&th, nullptr, SpinThread, &arg);
  usleep(30000);
  __atomic_store_n(flag, 1, __ATOMIC_SEQ_CST);
  pthread_join(th, nullptr);
  Expect(arg.n >= 0, "spin_wait returned");
  free((void*)flag);
}

void TestCalls(const Image& img) {
  uint32_t add6 = img.Find("guest_add6"), ret64 = img.Find("guest_ret64"), hyp = img.Find("guest_hypot");
  uint64_t args6[6] = {1, 2, 3, 4, 5, 6};
  Expect((uint32_t)ndk64_call_guest(add6, args6, 6) == 1 + 4 + 9 + 16 + 25 + 36, "guest_add6");
  uint64_t args2[2] = {0x0123456789abcdefull, 3};
  Expect(ndk64_call_guest(ret64, args2, 2) == 0x0123456789abcdefull * 3 + 1, "guest_ret64");
  Ndk64Args a;
  memset(&a, 0, sizeof a);
  double x = 3, y = 4;
  memcpy(&a.v[0], &x, 8);
  memcpy(&a.v[1], &y, 8);
  Ndk64Result r;
  ndk64_call_guest_args(hyp, &a, &r);
  double d;
  memcpy(&d, &r.v0, 8);
  Expect(d == 5.0, "guest_hypot: doubles in v0, v1 and the result in v0");
  // Twelve integer arguments: four on the stack.
  uint32_t sum = img.Find("guest_sum12");
  if (sum) {
    uint64_t a12[12];
    for (int i = 0; i < 12; i++) a12[i] = (uint64_t)(i + 1) << (i * 3);
    uint64_t want = 0;
    for (int i = 0; i < 12; i++) want += a12[i] * (i + 1);
    Expect(ndk64_call_guest(sum, a12, 12) == want, "guest_sum12: stack arguments");
  }
}

// Writes A64 code into memory, runs it, patches it, flushes, runs again.
void TestFlush() {
  uint32_t* code = static_cast<uint32_t*>(aligned_alloc(4096, 4096));
  code[0] = 0xd2800020;   // mov x0, #1
  code[1] = 0xd65f03c0;   // ret
  uint32_t addr = (uint32_t)(uintptr_t)code;
  ndk64_flush_code(addr, 8);
  Expect(ndk64_call_guest(addr, nullptr, 0) == 1, "generated code runs");
  code[0] = 0xd2800040;   // mov x0, #2
  ndk64_flush_code(addr, 8);
  Expect(ndk64_call_guest(addr, nullptr, 0) == 2, "ndk64_flush_code drops the old decoding");
  // IC IVAU from guest code: x0 = address of the patched word.
  code[0] = 0xd2800060;   // mov x0, #3
  ndk64_flush_code(addr, 8);
  Expect(ndk64_call_guest(addr, nullptr, 0) == 3, "patched again");
  free(code);
}

jmp_buf g_fault_jmp;
std::string g_fault_msg;
void OnFault(NdkCpu64State*, uint32_t, const char* msg) {
  g_fault_msg = msg;
  longjmp(g_fault_jmp, 1);
}

void TestFault(uint32_t addr, const char* want) {
  NdkCpu64State* st = ndk64_current_state();
  NdkCpu64State saved;
  memcpy(&saved, st, sizeof saved);
  ndk64_set_fault_handler(OnFault);
  g_fault_msg.clear();
  if (setjmp(g_fault_jmp) == 0) ndk64_call_guest(addr, nullptr, 0);
  ndk64_set_fault_handler(nullptr);
  memcpy(st, &saved, sizeof saved);
  if (g_fault_msg.find(want) != std::string::npos) g_pass++;
  else Fail("fault: got \"%s\", want \"%s\"", g_fault_msg.c_str(), want);
}

void TestFaults() {
  uint32_t* code = static_cast<uint32_t*>(aligned_alloc(4096, 4096));
  code[0] = 0x00000000;   // udf #0
  code[1] = 0xd2800200;   // mov x0, #16
  code[2] = 0xd61f0000;   // br x0
  code[3] = 0xd4200020;   // brk #1
  uint32_t addr = (uint32_t)(uintptr_t)code;
  ndk64_flush_code(addr, 16);
  TestFault(addr, "permanently undefined");
  TestFault(addr + 4, "invalid address");
  TestFault(addr + 12, "BRK #0x1");
  // A guarded call returns 0 after a fault.
  char why[128];
  Ndk64Result r;
  ndk64_set_fault_handler(nullptr);
  Expect(ndk64_call_guest_guarded(addr + 12, nullptr, &r, why, sizeof why) == 0 && strstr(why, "BRK"),
         "guarded call ends at a fault");
  free(code);
}

void RunBench(const Image& img, int n) {
  uint32_t fn = img.Find("bench_main");
  if (!fn) return;
  uint64_t warm[1] = {(uint64_t)(n / 4 + 1)};
  for (uint64_t t = NowUs(); NowUs() - t < 300000;) ndk64_call_guest(fn, warm, 1);
  uint64_t args[1] = {(uint64_t)n};
  uint64_t i0 = ndk64_instruction_count(), t0 = NowUs();
  uint32_t result = (uint32_t)ndk64_call_guest(fn, args, 1);
  uint64_t t1 = NowUs(), i1 = ndk64_instruction_count();
  double secs = (t1 - t0) / 1e6;
  printf("BENCH [%s] %llu guest instructions in %.3f s = %.1f MIPS (result %08x)\n", g_variant.c_str(),
         (unsigned long long)(i1 - i0), secs, (i1 - i0) / secs / 1e6, result);
}

struct StressArg {
  uint32_t bench, worker, counter;
  uint32_t result;
};
void* StressThread(void* p) {
  StressArg* a = static_cast<StressArg*>(p);
  uint64_t n[1] = {3};
  a->result = (uint32_t)ndk64_call_guest(a->bench, n, 1);
  uint64_t args[2] = {a->counter, 2000};
  ndk64_call_guest(a->worker, args, 2);
  return nullptr;
}

void Stress(const std::vector<std::string>& files, int rounds) {
  uint32_t want[16] = {0};
  for (int round = 0; round < rounds; round++) {
    for (size_t f = 0; f < files.size() && f < 16; f++) {
      g_variant = "stress " + files[f];
      Image* img = new Image;
      if (!LoadElf("/guest/" + files[f], img)) { Fail("cannot load"); return; }
      memcpy(reinterpret_cast<void*>((uintptr_t)img->Find("host")), g_host_table, sizeof(g_host_table));
      int* counter = static_cast<int*>(aligned_alloc(64, 64));
      *counter = 0;
      const int kThreads = 4;
      pthread_t th[kThreads];
      StressArg args[kThreads];
      for (int i = 0; i < kThreads; i++) {
        args[i] = {img->Find("bench_reentrant"), img->Find("atomic_worker"), (uint32_t)(uintptr_t)counter, 0};
        pthread_create(&th[i], nullptr, StressThread, &args[i]);
      }
      for (int i = 0; i < kThreads; i++) pthread_join(th[i], nullptr);
      bool ok = *counter == kThreads * 2000;
      for (int i = 0; i < kThreads; i++) {
        if (!want[f]) want[f] = args[i].result;
        ok &= args[i].result == want[f];
      }
      if (ok) g_pass++;
      else Fail("round %d: counter %d, results %08x %08x %08x %08x", round, *counter, args[0].result,
                args[1].result, args[2].result, args[3].result);
      ndk64_flush_code(img->base, img->size);
      free(counter);
    }
    if (round % 10 == 9) printf("stress: %d rounds\n", round + 1);
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> filters;
  int bench = 0, stress = 0;
  bool threads = true;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--bench")) bench = (i + 1 < argc && isdigit((unsigned char)argv[i + 1][0])) ? atoi(argv[++i]) : 200;
    else if (!strcmp(argv[i], "--no-threads")) threads = false;
    else if (!strcmp(argv[i], "--stress") && i + 1 < argc) stress = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--jit") && i + 1 < argc) ndk64_set_jit(atoi(argv[++i]));
    else if (!strcmp(argv[i], "--max-fail") && i + 1 < argc) g_max_fail_print = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--windows")) ndk64_windows_on();
    else filters.push_back(argv[i]);
  }
  ndk64_set_symbolizer(Symbolize);
  MakeStubs();
  LoadGolden();
  std::vector<std::string> files;
  if (DIR* dir = opendir("/guest")) {
    while (dirent* e = readdir(dir)) {
      std::string n = e->d_name;
      if (n.size() > 4 && n.substr(n.size() - 4) == ".elf") files.push_back(n);
    }
    closedir(dir);
  }
  std::sort(files.begin(), files.end());
  uint64_t t_start = NowUs();
  for (const std::string& file : files) {
    std::string variant = file.substr(0, file.size() - 4);
    bool selected = filters.empty();
    for (const std::string& f : filters) selected |= variant.find(f) != std::string::npos;
    if (!selected) continue;
    g_variant = variant;
    Image* img = new Image;
    img->name = variant;
    if (!LoadElf("/guest/" + file, img)) { Fail("cannot load %s", file.c_str()); continue; }
    g_images.push_back(img);
    uint32_t host = img->Find("host");
    if (!host) { Fail("no host table"); continue; }
    memcpy(reinterpret_cast<void*>((uintptr_t)host), g_host_table, sizeof(g_host_table));
    int fails_before = g_fail, passes_before = g_pass;
    printf("== %s (loaded at %08x, %u KiB)\n", variant.c_str(), img->base, img->size / 1024);
    uint64_t t0 = NowUs();
    ndk64_call_guest(img->Find("guest_main"), nullptr, 0);
    TestCalls(*img);
    if (threads) { TestThreads(*img); TestSpin(*img); }
    printf("== %s: %d passed, %d failed (%.0f ms)\n", variant.c_str(), g_pass - passes_before, g_fail - fails_before,
           (NowUs() - t0) / 1000.0);
    if (bench) RunBench(*img, bench);
  }
  if (g_diff_pass + g_diff_fail) printf("differential cases: %d match the native run, %d differ\n", g_diff_pass, g_diff_fail);
  if (stress) {
    std::vector<std::string> sel;
    for (const std::string& file : files) {
      bool selected = filters.empty();
      for (const std::string& f : filters) selected |= file.find(f) != std::string::npos;
      if (selected) sel.push_back(file);
    }
    Stress(sel, stress);
  }
  g_variant = "runtime";
  TestFlush();
  TestFaults();
  Ndk64CpuStats st;
  ndk64_cpu_stats(&st);
  printf("blocks decoded %llu (%llu instructions), flushed %llu, host calls %llu; "
         "translated to wasm %llu (%llu bytes), not translatable %llu\n",
         (unsigned long long)st.blocks_decoded, (unsigned long long)st.insns_decoded,
         (unsigned long long)st.blocks_flushed, (unsigned long long)st.host_calls,
         (unsigned long long)st.blocks_translated, (unsigned long long)st.translated_bytes,
         (unsigned long long)st.blocks_not_translatable);
  printf("ndk_cpu64_test: %d passed, %d failed (%.0f ms)\n", g_pass, g_fail, (NowUs() - t_start) / 1000.0);
  fflush(stdout);
  return g_fail ? 1 : 0;
}
