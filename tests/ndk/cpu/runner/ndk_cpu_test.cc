// Test runner of the NDK CPU (topic "arm-cpu"): loads the guest test
// programs (position-independent ARM ELF files built by
// mk/topic-arm-cpu.mk, embedded at /guest), gives them host stubs, runs
// them through ndk_call_guest and checks the CPU's runtime contract
// (re-entrancy, threads, ndk_flush_code, faults). Runs under Node and in
// browsers (build/ndk-cpu/index.html).
//
//   ndk_cpu_test.js [VARIANT-SUBSTRING...] [--bench [N]] [--no-threads] [--jit THRESHOLD] [--stress ROUNDS]
#include <dirent.h>
#include <elf.h>
#include <math.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include <emscripten/threading.h>

#include <algorithm>
#include <string>
#include <vector>

#include "ndk/cpu/ndk_cpu_ext.h"
#include "ndk/ndk_cpu.h"

extern "C" uint64_t ndk_cpu_jit_functions(void);   // shim/ndk/cpu/jit.cc

namespace {

int g_pass, g_fail;
std::string g_variant;

void Fail(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Fail(const char* fmt, ...) {
  g_fail++;
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

// --- guest images --------------------------------------------------------------------
struct Symbol {
  uint32_t addr, size;
  std::string name;
};
struct Image {
  std::string name;
  uint32_t base = 0, size = 0;
  std::vector<Symbol> syms;   // sorted by address
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
      if ((s.addr & ~1u) <= addr && (!best || (s.addr & ~1u) > (best->addr & ~1u))) best = &s;
    if (best) {
      *off = addr - (best->addr & ~1u);
      return best->name.c_str();
    }
  }
  return nullptr;
}

bool LoadElf(const std::string& path, Image* img) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  std::vector<uint8_t> data;
  static uint8_t buf[65536];   // not on the wasm stack
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.insert(data.end(), buf, buf + n);
  fclose(f);
  const Elf32_Ehdr* eh = reinterpret_cast<const Elf32_Ehdr*>(data.data());
  if (data.size() < sizeof(*eh) || memcmp(eh->e_ident, ELFMAG, SELFMAG) || eh->e_machine != EM_ARM) return false;
  const Elf32_Phdr* ph = reinterpret_cast<const Elf32_Phdr*>(data.data() + eh->e_phoff);
  uint32_t end = 0;
  for (int i = 0; i < eh->e_phnum; i++)
    if (ph[i].p_type == PT_LOAD) end = std::max(end, ph[i].p_vaddr + ph[i].p_memsz);
  end = (end + 4095) & ~4095u;
  void* mem = aligned_alloc(4096, end);
  memset(mem, 0, end);
  img->base = (uint32_t)(uintptr_t)mem;
  img->size = end;
  for (int i = 0; i < eh->e_phnum; i++)
    if (ph[i].p_type == PT_LOAD) memcpy((char*)mem + ph[i].p_vaddr, data.data() + ph[i].p_offset, ph[i].p_filesz);
  const Elf32_Shdr* sh = reinterpret_cast<const Elf32_Shdr*>(data.data() + eh->e_shoff);
  for (int i = 0; i < eh->e_shnum; i++) {
    if (sh[i].sh_type == SHT_REL) {
      const Elf32_Rel* rel = reinterpret_cast<const Elf32_Rel*>(data.data() + sh[i].sh_offset);
      const Elf32_Shdr& symsec = sh[sh[i].sh_link];
      const Elf32_Sym* dsyms = reinterpret_cast<const Elf32_Sym*>(data.data() + symsec.sh_offset);
      for (size_t k = 0; k < sh[i].sh_size / sizeof(Elf32_Rel); k++) {
        uint32_t* where = reinterpret_cast<uint32_t*>((char*)mem + rel[k].r_offset);
        uint32_t type = ELF32_R_TYPE(rel[k].r_info), sym = ELF32_R_SYM(rel[k].r_info);
        switch (type) {
          case R_ARM_RELATIVE: *where += img->base; break;
          case R_ARM_ABS32: case R_ARM_GLOB_DAT: case R_ARM_JUMP_SLOT:
            if (dsyms[sym].st_shndx == SHN_UNDEF) {
              printf("%s: undefined symbol in relocation\n", path.c_str());
              return false;
            }
            *where = (type == R_ARM_ABS32 ? *where : 0) + img->base + dsyms[sym].st_value;
            break;
          default:
            printf("%s: unsupported relocation %u\n", path.c_str(), type);
            return false;
        }
      }
    }
    if (sh[i].sh_type == SHT_SYMTAB) {
      const Elf32_Sym* syms = reinterpret_cast<const Elf32_Sym*>(data.data() + sh[i].sh_offset);
      const char* strtab = reinterpret_cast<const char*>(data.data() + sh[sh[i].sh_link].sh_offset);
      for (size_t k = 0; k < sh[i].sh_size / sizeof(Elf32_Sym); k++) {
        int type = ELF32_ST_TYPE(syms[k].st_info);
        if ((type != STT_FUNC && type != STT_OBJECT) || syms[k].st_shndx == SHN_UNDEF) continue;
        img->syms.push_back({img->base + syms[k].st_value, syms[k].st_size, strtab + syms[k].st_name});
      }
    }
  }
  std::sort(img->syms.begin(), img->syms.end(), [](const Symbol& a, const Symbol& b) { return a.addr < b.addr; });
  ndk_flush_code(img->base, img->size);
  return true;
}

// --- host functions given to the guest (struct host_api in rt/guest.h) ----------------
inline uint32_t Arg(NdkCpuState* s, int i) {
  return i < 4 ? s->r[i] : *reinterpret_cast<uint32_t*>((uintptr_t)(s->r[13] + 4 * (i - 4)));
}
inline uint64_t Arg64(NdkCpuState* s, int i) { return Arg(s, i) | (uint64_t)Arg(s, i + 1) << 32; }
inline double ArgD(NdkCpuState* s, int i) { uint64_t b = Arg64(s, i); double d; memcpy(&d, &b, 8); return d; }
inline const char* ArgStr(NdkCpuState* s, int i) { return reinterpret_cast<const char*>((uintptr_t)Arg(s, i)); }
inline void Ret64(NdkCpuState* s, uint64_t v) { s->r[0] = (uint32_t)v; s->r[1] = (uint32_t)(v >> 32); }
inline void RetD(NdkCpuState* s, double d) { uint64_t b; memcpy(&b, &d, 8); Ret64(s, b); }

void HostWrite(NdkCpuState* s, void*) { fwrite(ArgStr(s, 0), 1, Arg(s, 1), stdout); }

void HostCheck(NdkCpuState* s, void*) {
  if (Arg(s, 0)) g_pass++;
  else Fail("%s:%u: CHECK(%s)", ArgStr(s, 2), Arg(s, 3), ArgStr(s, 1));
}

void HostCheckEq(NdkCpuState* s, void*) {
  uint64_t got = Arg64(s, 0), want = Arg64(s, 2);
  if (got == want) g_pass++;
  else Fail("line %u: %s: got 0x%llx, want 0x%llx", Arg(s, 5), ArgStr(s, 4), (unsigned long long)got,
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
// r0 = fn, r2:r3 = x, stack: y, result, line.
void HostCheckMath(NdkCpuState* s, void*) {
  std::string fn = ArgStr(s, 0);
  double x = ArgD(s, 2), y = ArgD(s, 4), r = ArgD(s, 6);
  uint32_t line = Arg(s, 8);
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
void HostPing(NdkCpuState* s, void*) {
  uint32_t fn = Arg(s, 0), depth = Arg(s, 1);
  if (depth == 0) { Ret64(s, 0); return; }
  uint32_t args[6] = {depth - 1, Arg(s, 2), Arg(s, 3), Arg(s, 4), Arg(s, 5), Arg(s, 6)};
  uint32_t sp_before = s->r[13], lr_before = s->r[14];
  uint64_t r = ndk_call_guest(fn, args, 6);
  if (s->r[13] != sp_before || s->r[14] != lr_before) Fail("ping: sp/lr not restored after ndk_call_guest");
  Ret64(s, r + depth);
}

void HostSum8(NdkCpuState* s, void*) {
  uint32_t r = 0;
  for (int i = 0; i < 8; i++) r += Arg(s, i) * (i + 1);
  s->r[0] = r;
}
void HostMul64(NdkCpuState* s, void*) { Ret64(s, Arg64(s, 0) * Arg64(s, 2)); }
void HostFma3(NdkCpuState* s, void*) { RetD(s, ArgD(s, 0) * ArgD(s, 2) + ArgD(s, 4)); }
void HostNowUs(NdkCpuState* s, void*) { Ret64(s, NowUs()); }
void HostSetTls(NdkCpuState* s, void*) { s->tls = Arg(s, 0); }

// Signals (t_signal.c, shim/ndk/cpu/signal.cc).
void HostTkill(NdkCpuState* s, void*) {
  int tid = (int)Arg(s, 0);
  int r = ndk_sig_send(tid, (int)Arg(s, 1));
  if (r == 0 && tid == gettid()) ndk_sig_deliver();
  s->r[0] = (uint32_t)r;
}
void HostGettid(NdkCpuState* s, void*) {   // as the bridge's gettid
  int tid;
  ndk_sig_self(nullptr, &tid);
  s->r[0] = (uint32_t)tid;
}
void HostSigaction(NdkCpuState* s, void*) {
  NdkSigaction a = {Arg(s, 1), Arg(s, 2), Arg(s, 3)};
  s->r[0] = (uint32_t)ndk_sig_action((int)Arg(s, 0), &a, nullptr);
}
void HostSigsuspend(NdkCpuState* s, void*) {
  uint64_t mask = Arg(s, 0);
  s->r[0] = (uint32_t)ndk_sig_suspend(&mask);
}
void HostSigmask(NdkCpuState* s, void*) {
  uint64_t set = Arg(s, 1), old = 0;
  ndk_sig_mask((int)Arg(s, 0), &set, &old);
  ndk_sig_deliver();
  s->r[0] = (uint32_t)old;
}
void HostFutexWait(NdkCpuState* s, void*) {
  int ms = (int)Arg(s, 2);
  s->r[0] = (uint32_t)ndk_sig_wait(Arg(s, 0), Arg(s, 1), ms < 0 ? INFINITY : ms);
}
void HostFutexWake(NdkCpuState* s, void*) {
  emscripten_futex_wake(reinterpret_cast<void*>((uintptr_t)Arg(s, 0)), INT32_MAX);
}
void HostSleepMs(NdkCpuState* s, void*) { s->r[0] = (uint32_t)ndk_sig_sleep((int)Arg(s, 0), nullptr); }

uint32_t g_host_table[18];

void MakeStubs() {
  g_host_table[0] = ndk_host_stub(HostWrite, nullptr, "host.write");
  g_host_table[1] = ndk_host_stub(HostCheck, nullptr, "host.check");
  g_host_table[2] = ndk_host_stub(HostCheckEq, nullptr, "host.check_eq");
  g_host_table[3] = ndk_host_stub(HostCheckMath, nullptr, "host.check_math");
  g_host_table[4] = ndk_host_stub(HostPing, nullptr, "host.ping");
  g_host_table[5] = ndk_host_stub(HostSum8, nullptr, "host.sum8");
  g_host_table[6] = ndk_host_stub(HostMul64, nullptr, "host.mul64");
  g_host_table[7] = ndk_host_stub(HostFma3, nullptr, "host.fma3");
  g_host_table[8] = ndk_host_stub(HostNowUs, nullptr, "host.now_us");
  g_host_table[9] = ndk_host_stub(HostSetTls, nullptr, "host.set_tls");
  g_host_table[10] = ndk_host_stub(HostTkill, nullptr, "host.tkill");
  g_host_table[11] = ndk_host_stub(HostGettid, nullptr, "host.gettid");
  g_host_table[12] = ndk_host_stub(HostSigaction, nullptr, "host.sigaction");
  g_host_table[13] = ndk_host_stub(HostSigsuspend, nullptr, "host.sigsuspend");
  g_host_table[14] = ndk_host_stub(HostSigmask, nullptr, "host.sigmask");
  g_host_table[15] = ndk_host_stub(HostFutexWait, nullptr, "host.futex_wait");
  g_host_table[16] = ndk_host_stub(HostFutexWake, nullptr, "host.futex_wake");
  g_host_table[17] = ndk_host_stub(HostSleepMs, nullptr, "host.sleep_ms");
}

// --- runtime contract tests -------------------------------------------------------------
struct ThreadArg {
  uint32_t fn, counter;
  int n;
};
void* ThreadMain(void* p) {
  ThreadArg* a = static_cast<ThreadArg*>(p);
  uint32_t args[2] = {a->counter, (uint32_t)a->n};
  ndk_call_guest(a->fn, args, 2);
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
  uint32_t args[1] = {a->counter};
  a->n = (int)ndk_call_guest(a->fn, args, 1);
  return nullptr;
}

// A guest thread spins on a plain load until the host sets the flag.
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
  pthread_join(th, nullptr);   // hangs if the loop never sees the store
  Expect(arg.n >= 0, "spin_wait returned");
  free((void*)flag);
}

// Boehm-style stop-the-world (t_signal.c): four guest workers (spinning
// in guest code, waiting in a host futex wait, calling host functions, and
// one outside guest code but for a short call every 50 ms) are stopped by
// signals from the main thread and restarted, 20 times.
struct SigWorker {
  uint32_t fn;
  int kind;
};
void* SigWorkerMain(void* p) {
  SigWorker* w = static_cast<SigWorker*>(p);
  if (w->kind == 3) {
    while ((uint32_t)ndk_call_guest(w->fn, nullptr, 0) == 0) usleep(50000);
    return nullptr;
  }
  uint32_t args[2] = {(uint32_t)w->kind, (uint32_t)w->kind};
  ndk_call_guest(w->fn, args, 2);
  return nullptr;
}
void TestSignals(const Image& img) {
  uint32_t worker = img.Find("sig_worker"), world = img.Find("sig_world_test"),
           tick = img.Find("sig_outside_tick");
  if (!worker || !world || !tick) { Fail("no sig_worker / sig_world_test / sig_outside_tick"); return; }
  const int kWorkers = 4;
  SigWorker w[kWorkers];
  pthread_t th[kWorkers];
  for (int i = 0; i < kWorkers; i++) {
    w[i] = {i == 3 ? tick : worker, i};
    pthread_create(&th[i], nullptr, SigWorkerMain, &w[i]);
  }
  uint32_t args[2] = {kWorkers, 20};
  ndk_call_guest(world, args, 2);
  for (int i = 0; i < kWorkers; i++) pthread_join(th[i], nullptr);
}

void TestCalls(const Image& img) {
  uint32_t add6 = img.Find("guest_add6"), ret64 = img.Find("guest_ret64"), hyp = img.Find("guest_hypot");
  uint32_t args6[6] = {1, 2, 3, 4, 5, 6};
  Expect((uint32_t)ndk_call_guest(add6, args6, 6) == 1 + 4 + 9 + 16 + 25 + 36, "guest_add6 with stack arguments");
  uint32_t args3[4] = {0x89abcdef, 0x01234567, 3, 0};   // a = r0:r1, b = r2
  Expect(ndk_call_guest(ret64, args3, 3) == 0x0123456789abcdefull * 3 + 1, "guest_ret64");
  double a = 3, b = 4;
  uint32_t argsd[4];
  memcpy(argsd, &a, 8);
  memcpy(argsd + 2, &b, 8);
  uint64_t r = ndk_call_guest(hyp, argsd, 4);
  double d;
  memcpy(&d, &r, 8);
  Expect(d == 5.0, "guest_hypot soft-float arguments");
}

// Writes ARM code into memory, runs it, patches it, flushes, runs again.
void TestFlush() {
  uint32_t* code = static_cast<uint32_t*>(aligned_alloc(4096, 4096));
  code[0] = 0xe3a00001;   // mov r0, #1
  code[1] = 0xe12fff1e;   // bx lr
  uint32_t addr = (uint32_t)(uintptr_t)code;
  ndk_flush_code(addr, 8);
  Expect(ndk_call_guest(addr, nullptr, 0) == 1, "generated code runs");
  code[0] = 0xe3a00002;   // mov r0, #2
  ndk_flush_code(addr, 8);
  Expect(ndk_call_guest(addr, nullptr, 0) == 2, "ndk_flush_code drops the old decoding");
  // Thumb code at the same place: movs r0, #7; bx lr
  code[0] = 0x47702007;
  ndk_flush_code(addr, 8);
  Expect(ndk_call_guest(addr | 1, nullptr, 0) == 7, "Thumb entry point");
  // The same bytes flushed again (a JIT flushing next to its new code): the
  // decoding stays; only changed code is retired (topic guest-jit).
  NdkCpuStats before, after;
  ndk_cpu_stats(&before);
  ndk_flush_code(addr, 8);
  ndk_cpu_stats(&after);
  Expect(after.blocks_flushed == before.blocks_flushed && after.blocks_kept > before.blocks_kept,
         "a flush keeps unchanged code");
  // Flushes cost the blocks of their range, not every block of the process:
  // 200000 line-sized ones (an IC IVAU loop over 12 MB) in well under 2 s.
  uint64_t t0 = NowUs();
  for (uint32_t i = 0; i < 200000; i++) ndk_flush_code(addr + 64 * (i & 63), 64);
  Expect(NowUs() - t0 < 2000000, "200000 small flushes are quick");
  free(code);
}

jmp_buf g_fault_jmp;
std::string g_fault_msg;
void OnFault(NdkCpuState*, uint32_t pc, const char* msg) {
  g_fault_msg = msg;
  longjmp(g_fault_jmp, 1);
}

void TestFault(uint32_t addr, const char* want) {
  NdkCpuState* st = ndk_current_state();
  uint32_t saved[16];
  memcpy(saved, st->r, sizeof(saved));
  ndk_set_fault_handler(OnFault);
  g_fault_msg.clear();
  if (setjmp(g_fault_jmp) == 0) ndk_call_guest(addr, nullptr, 0);
  ndk_set_fault_handler(nullptr);
  memcpy(st->r, saved, sizeof(saved));
  if (g_fault_msg.find(want) != std::string::npos) g_pass++;
  else Fail("fault: got \"%s\", want \"%s\"", g_fault_msg.c_str(), want);
}

void TestFaults() {
  uint32_t* code = static_cast<uint32_t*>(aligned_alloc(4096, 4096));
  code[0] = 0xe7f000f0;   // udf #0
  code[1] = 0xe3a00010;   // mov r0, #16
  code[2] = 0xe12fff10;   // bx r0
  uint32_t addr = (uint32_t)(uintptr_t)code;
  ndk_flush_code(addr, 12);
  TestFault(addr, "permanently undefined");
  TestFault(addr + 4, "invalid address");
  free(code);
}

void RunBench(const Image& img, int n, const char* name = "bench_main") {
  uint32_t fn = img.Find(name);
  if (!fn) return;
  // Warm up: decode the code and let the browser's wasm compiler tier up
  // the interpreter (V8 starts in its baseline tier).
  uint32_t warm[1] = {(uint32_t)(n / 4 + 1)};
  for (uint64_t t = NowUs(); NowUs() - t < 300000;) ndk_call_guest(fn, warm, 1);
  uint32_t args[1] = {(uint32_t)n};
  uint64_t i0 = ndk_instruction_count(), t0 = NowUs();
  uint32_t result = (uint32_t)ndk_call_guest(fn, args, 1);
  uint64_t t1 = NowUs(), i1 = ndk_instruction_count();
  double secs = (t1 - t0) / 1e6;
  printf("BENCH%s [%s] %llu guest instructions in %.3f s = %.1f MIPS (result %08x)\n",
         strcmp(name, "bench_main") ? name + 5 : "", g_variant.c_str(),
         (unsigned long long)(i1 - i0), secs, (i1 - i0) / secs / 1e6, result);
}

// Stress (--stress N): N rounds of loading a fresh copy of every guest
// program (as System.loadLibrary does: copy, relocate, flush) and running
// it from 4 threads at once, so decoding, translation to wasm and per-thread
// instantiation of brand-new code race each other.
struct StressArg {
  uint32_t bench, worker, counter;
  uint32_t result;
};
void* StressThread(void* p) {
  StressArg* a = static_cast<StressArg*>(p);
  uint32_t n[1] = {3};
  a->result = (uint32_t)ndk_call_guest(a->bench, n, 1);
  uint32_t args[2] = {a->counter, 2000};
  ndk_call_guest(a->worker, args, 2);
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
      // Unload: drop the code and reuse nothing (the next round loads elsewhere).
      ndk_flush_code(img->base, img->size);
      free(counter);
    }
    if (round % 10 == 9) printf("stress: %d rounds\n", round + 1);
  }
}


// --fuzz N [SEED] (topic arm-chains): N random ARM functions -- data
// processing of every form with random conditions and flags, multiplies,
// extends and bitfields, MRS/MSR, loads and stores, forward conditional
// branches, sometimes a counted loop around part of it -- each run 10 times
// from the same registers, flags and memory: the first runs interpreted,
// the later ones translated (regions, flag liveness). Every run must give
// the same registers, flags and memory.
struct FuzzRng {
  uint64_t s;
  uint32_t Next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (uint32_t)(s >> 11); }
  uint32_t operator()(uint32_t n) { return Next() % n; }
};

struct FuzzGen {
  FuzzRng& r;
  uint32_t rd_max;   // destinations r0 .. rd_max-1 (r11 counts a loop, r12 is the memory)
  uint32_t Cond() { return r(3) == 0 ? (r(14) << 28) : 0xe0000000u; }
  uint32_t Rd() { return r(rd_max); }
  uint32_t Dp() {
    uint32_t op = r(16), s = r(2), rd = Rd(), rn = r(13);
    if (op >= 8 && op <= 11) { s = 1; rd = 0; }
    if (op == 13 || op == 15) rn = 0;
    uint32_t op2;
    switch (r(3)) {
      case 0: op2 = (1u << 25) | (r(16) << 8) | r(256); break;
      case 1: op2 = (r(32) << 7) | (r(4) << 5) | r(13); break;
      default: op2 = (r(13) << 8) | (r(4) << 5) | (1u << 4) | r(13); break;
    }
    return Cond() | op2 | (op << 21) | (s << 20) | (rn << 16) | (rd << 12);
  }
  uint32_t Mul() {
    uint32_t k = r(6), s = r(2), rd = Rd(), rn = r(13), rs = r(13), rm = r(13);
    if (k < 2) return Cond() | (k << 21) | (s << 20) | (rd << 16) | ((k ? rn : 0) << 12) | (rs << 8) | 0x90 | rm;
    uint32_t lo = Rd();
    if (lo == rd) lo = (lo + 1) % rd_max;
    return Cond() | (1u << 23) | ((k - 2) << 21) | (s << 20) | (rd << 16) | (lo << 12) | (rs << 8) | 0x90 | rm;
  }
  uint32_t Mem() {
    switch (r(4)) {
      case 0: {   // LDRH/STRH/LDRSB/LDRSH [r12, #imm8]
        uint32_t l = r(2), sh = l ? 1 + r(3) : 1, imm = 2 * r(120), rd = l ? Rd() : r(12);
        return Cond() | 0x01c000b0u | (l << 20) | (12u << 16) | (rd << 12) | ((imm >> 4) << 8) | (sh << 5) | (imm & 15);
      }
      case 1: {   // LDMIA/STMIA r12 (no writeback), registers below rd_max
        uint32_t l = r(2), list = r(1u << rd_max);
        if (!list) list = 1;
        return Cond() | 0x08800000u | (l << 20) | (12u << 16) | list;
      }
    }
    uint32_t b = r(2), l = r(2), rd = l ? Rd() : r(12), imm = b ? r(256) : 4 * r(64);
    return Cond() | 0x05800000u | (b << 22) | (l << 20) | (12u << 16) | (rd << 12) | imm;
  }
  // NEON on d0-d7 (loaded from the memory in the prologue): integer data
  // processing, VLD1/VST1 [r12] (through the interpreter), VMOV core <-> D.
  uint32_t Neon() {
    const uint32_t d = r(8), n = r(8), m = r(8);
    switch (r(6)) {
      case 0: return 0xf2200800u | (n << 16) | (d << 12) | m;                       // VADD.I32 Dd, Dn, Dm
      case 1: return 0xf3200800u | (n << 16) | (d << 12) | m;                       // VSUB.I32
      case 2: return 0xf3000110u | (n << 16) | (d << 12) | m;                       // VEOR
      case 3: return 0xf2200910u | (n << 16) | (d << 12) | m;                       // VMUL.I32
      case 4: {   // VLD1.32 / VST1.32 {Dd, Dd+1}, [r12] (+ writeback-free offset by rm = 15)
        const uint32_t vd = r(7);
        return (r(2) ? 0xf42c0a8fu : 0xf40c0a8fu) | (vd << 12);
      }
      default: {   // VMOV Dm, rt, rt2 / VMOV rt, rt2, Dm
        uint32_t rt = Rd(), rt2 = Rd();
        if (rt == rt2) rt2 = (rt2 + 1) % rd_max;
        return Cond() | (r(2) ? 0x0c400b10u : 0x0c500b10u) | (rt2 << 16) | (rt << 12) | m;
      }
    }
  }
  // BL to the callee at |callee| (a word index) from word |at|.
  uint32_t Call(size_t at, size_t callee) {
    return Cond() | 0x0b000000u | ((uint32_t)(callee - (at + 2)) & 0xffffff);
  }
  uint32_t Misc() {
    switch (r(7)) {
      case 0: return Cond() | 0x016f0f10u | (Rd() << 12) | r(13);                                // CLZ
      case 1: { uint32_t v = r(65536); return Cond() | 0x03000000u | ((v >> 12) << 16) | (Rd() << 12) | (v & 0xfff); }  // MOVW
      case 2: { uint32_t v = r(65536); return Cond() | 0x03400000u | ((v >> 12) << 16) | (Rd() << 12) | (v & 0xfff); }  // MOVT
      case 3: return Cond() | (r(2) ? 0x06ef0070u : 0x06bf0070u) | (Rd() << 12) | (r(4) << 10) | r(13);   // UXTB / SXTH
      case 4: { uint32_t lsb = r(32), w = 1 + r(32 - lsb); return Cond() | 0x07e00050u | ((w - 1) << 16) | (Rd() << 12) | (lsb << 7) | r(13); }  // UBFX
      case 5: return Cond() | 0x010f0000u | (Rd() << 12);                                         // MRS
      default: return Cond() | 0x0128f000u | r(13);                                              // MSR APSR_nzcvq
    }
  }
  // |n| instructions; a forward branch skips at most to the end. Calls go
  // to the callees (word indexes; filled in later: |calls|).
  std::vector<std::pair<size_t, int>>* calls = nullptr;
  bool neon = false;
  void Body(std::vector<uint32_t>* w, int n) {
    for (int i = 0; i < n; i++) {
      const int left = n - 1 - i;
      uint32_t k = r(21);
      if (neon && r(5) == 0) {
        w->push_back(Neon());
      } else if (k == 20 && calls) {
        calls->push_back({w->size(), (int)r(2)});
        w->push_back(0);
      } else if (k < 2 && left > 0) {
        const uint32_t skip = 1 + r(left < 3 ? left : 3);
        w->push_back((r(14) << 28) | 0x0a000000u | (skip - 1));
      } else if (k < 10) w->push_back(Dp());
      else if (k < 13) w->push_back(Mul());
      else if (k < 17) w->push_back(Mem());
      else w->push_back(Misc());
    }
  }
};

bool g_fuzz_neon = false;   // --fuzz-neon: NEON too
int g_fuzz_jit = 3;         // --fuzz-jit T: the threshold (0: interpreter only)
bool g_fuzz_verbose = false;

void Fuzz(int n, uint64_t seed) {
  FuzzRng rng{seed * 0x9e3779b97f4a7c15ull + 1};
  ndk_set_jit(g_fuzz_jit);
  uint32_t* state = static_cast<uint32_t*>(aligned_alloc(16, 64));
  uint8_t* mem = static_cast<uint8_t*>(aligned_alloc(16, 512));
  uint32_t* code = static_cast<uint32_t*>(aligned_alloc(16, 4096));
  int bad = 0;
  const uint64_t fns0 = ndk_cpu_jit_functions();
  for (int f = 0; f < n && bad < 5; f++) {
    std::vector<uint32_t> w = {0xe92d4ff0u, 0xe52d0004u, 0xe590c038u, 0xe5901034u, 0xe128f001u};
    for (uint32_t x = 1; x <= 11; x++) w.push_back(0xe5900000u | (x << 12) | (4 * x));
    w.push_back(0xe5900000u);
    FuzzGen g{rng, 12};
    g.neon = g_fuzz_neon;
    if (g.neon) w.push_back(0xec9c0b10u);   // vldmia r12, {d0-d7}
    std::vector<std::pair<size_t, int>> calls;
    g.calls = &calls;
    if (rng(2)) {
      g.Body(&w, 1 + rng(12));
      w.push_back(0xe3a0b000u | (2 + rng(4)));   // mov r11, #2..5
      const size_t top = w.size();
      g.rd_max = 11;
      g.Body(&w, 1 + rng(10));
      g.rd_max = 12;
      w.push_back(0xe25bb001u);                   // subs r11, r11, #1
      w.push_back(0x1a000000u | ((uint32_t)(top - (w.size() + 2)) & 0xffffff));   // bne top
      g.Body(&w, rng(8));
    } else {
      g.Body(&w, 1 + rng(40));
    }
    const uint32_t tail[] = {0xe92d1fffu, 0xe10f0000u, 0xe59d1034u, 0xe5810034u};
    w.insert(w.end(), tail, tail + 4);
    for (uint32_t k = 0; k < 13; k++) { w.push_back(0xe49d2004u); w.push_back(0xe5812000u | (4 * k)); }
    w.push_back(0xe28dd004u);
    w.push_back(0xe8bd8ff0u);
    // Two callees: a few instructions (not r11, lr) and BX LR.
    size_t callee[2];
    g.calls = nullptr;
    for (int c = 0; c < 2; c++) {
      callee[c] = w.size();
      g.rd_max = 11;
      g.Body(&w, 1 + rng(4));
      g.rd_max = 12;
      w.push_back(0xe12fff1eu);
    }
    for (auto& c : calls) w[c.first] = g.Call(c.first, callee[c.second]);
    if (w.size() * 4 > 4096) continue;
    memcpy(code, w.data(), w.size() * 4);
    ndk_flush_code((uint32_t)(uintptr_t)code, w.size() * 4);
    uint32_t init[13], flags = (rng(16) << 28);
    for (int i = 0; i < 13; i++) init[i] = rng(4) ? rng.Next() : rng(8);
    uint8_t init_mem[512];
    for (int i = 0; i < 512; i++) init_mem[i] = (uint8_t)rng.Next();
    uint32_t ref[14];
    uint8_t ref_mem[512];
    for (int run = 0; run < 10; run++) {
      memcpy(state, init, sizeof(init));
      state[13] = flags;
      state[14] = (uint32_t)(uintptr_t)mem;
      memcpy(mem, init_mem, 512);
      uint32_t arg = (uint32_t)(uintptr_t)state;
      ndk_call_guest((uint32_t)(uintptr_t)code, &arg, 1);
      state[13] &= 0xf8000000u;
      if (!run) { memcpy(ref, state, sizeof(ref)); memcpy(ref_mem, mem, 512); continue; }
      int diff = -1;
      for (int i = 0; i < 14 && diff < 0; i++) if (i != 12 && state[i] != ref[i]) diff = i;
      if (diff < 0 && memcmp(mem, ref_mem, 512)) diff = 99;
      if (diff < 0) continue;
      bad++;
      Fail("fuzz %d run %d: %s %d: %08x, interpreted %08x (seed %llu)", f, run, diff == 99 ? "memory" : "register",
           diff, diff == 99 ? 0 : state[diff], diff == 99 ? 0 : ref[diff], (unsigned long long)seed);
      for (size_t i = 0; i < w.size(); i++) printf("  %08x\n", w[i]);
      break;
    }
    if (!bad) g_pass++;
  }
  printf("fuzz: %d functions, %d failed, %llu translations\n", n, bad,
         (unsigned long long)(ndk_cpu_jit_functions() - fns0));
}

// The same in Thumb-2 (--fuzz-thumb N [SEED]): 16- and 32-bit data
// processing, IT blocks (flags not set by 16-bit operations inside), CBZ
// and B<cond> forward, multiplies and divides (SDIV/UDIV through the
// interpreter), extends, UBFX, MRS/MSR, loads/stores of every size, LDRD,
// LDM/STM, BL to callees that return by BX LR, a counted loop.
struct ThumbInsn {
  uint16_t hw[2];
  int n;           // halfwords
  int target = -1; // branch: index of the instruction it goes to (-2: callee 0, -3: callee 1)
  int kind = 0;    // 1 CBZ/CBNZ, 2 B<cond> 16-bit, 3 BL, 4 B<cond> 32-bit backwards
};

struct ThumbGen {
  FuzzRng& r;
  uint32_t rd_max;
  std::vector<ThumbInsn>* out;
  bool in_it = false;
  void W(uint16_t a) { out->push_back({{a, 0}, 1}); }
  void W2(uint16_t a, uint16_t b) { out->push_back({{a, b}, 2}); }
  uint32_t Rd() { return r(rd_max); }
  uint32_t Lo() { return r(rd_max < 8 ? rd_max : 8); }
  void Dp32() {
    static const uint32_t ops[] = {0, 1, 2, 3, 4, 8, 10, 11, 13, 14};
    uint32_t op = ops[r(10)], s = r(2), rn = r(13), rd = Rd();
    if (r(5) == 0 && (op == 0 || op == 4 || op == 8 || op == 13)) { rd = 15; s = 1; }   // TST TEQ CMN CMP
    if (r(4) == 0 && (op == 2 || op == 3)) rn = 15;                                      // MOV MVN
    if (r(2)) {   // shifted register
      uint32_t amt = r(32), type = r(4);
      W2(0xea00 | (op << 5) | (s << 4) | rn, ((amt >> 2) << 12) | (rd << 8) | ((amt & 3) << 6) | (type << 4) | r(13));
    } else {      // modified immediate
      uint32_t imm = r(4096);
      W2(0xf000 | ((imm >> 11) << 10) | (op << 5) | (s << 4) | rn, (((imm >> 8) & 7) << 12) | (rd << 8) | (imm & 255));
    }
  }
  void Dp16() {
    switch (r(6)) {
      case 0: W(0x4000 | (r(16) << 6) | (Lo() << 3) | Lo()); break;          // ALU (MULS included)
      case 1: W((r(2) ? 0x2000 : 0x2800) | (Lo() << 8) | r(256)); break;     // MOVS / CMP #imm8
      case 2: W((r(2) ? 0x3000 : 0x3800) | (Lo() << 8) | r(256)); break;     // ADDS / SUBS #imm8
      case 3: W((r(2) ? 0x1800 : 0x1a00) | (Lo() << 6) | (Lo() << 3) | Lo()); break;   // ADDS / SUBS reg
      case 4: W((r(3) << 11) % 0x1800 | (r(32) << 6) | (Lo() << 3) | Lo()); break;     // LSLS LSRS ASRS #imm
      default: W(0x4600 | (r(13) << 3) | Lo()); break;                       // MOV rd (low), rm (any of r0-r12)
    }
  }
  void Mul() {
    uint32_t rn = r(13), rm = r(13), rd = Rd();
    switch (r(6)) {
      case 0: W2(0xfb00 | rn, 0xf000 | (rd << 8) | rm); break;                  // MUL
      case 1: W2(0xfb00 | rn, (r(13) << 12) | (rd << 8) | rm); break;           // MLA
      case 2: case 3: {
        uint32_t lo = Rd();
        if (lo == rd) lo = (lo + 1) % rd_max;
        static const uint16_t ops[] = {0xfba0, 0xfb80, 0xfbe0, 0xfbc0};          // UMULL SMULL UMLAL SMLAL
        W2(ops[r(4)] | rn, (lo << 12) | (rd << 8) | rm);
        break;
      }
      default: W2((r(2) ? 0xfb90 : 0xfbb0) | rn, 0xf0f0 | (rd << 8) | rm); break;   // SDIV UDIV
    }
  }
  void Misc() {
    switch (r(5)) {
      case 0: W2(r(2) ? 0xfa5f : 0xfa0f, 0xf080 | (Rd() << 8) | (r(4) << 4) | r(13)); break;   // UXTB / SXTH
      case 1: { uint32_t lsb = r(32), w = 1 + r(32 - lsb);
                W2(0xf3c0 | r(13), ((lsb >> 2) << 12) | (Rd() << 8) | ((lsb & 3) << 6) | (w - 1)); break; }  // UBFX
      case 2: W2(0xf3ef, 0x8000 | (Rd() << 8)); break;                            // MRS
      case 3: W2(0xf380 | r(13), 0x8800); break;                                  // MSR APSR_nzcvq
      default: { uint32_t v = r(65536);                                           // MOVW / MOVT
                 W2((r(2) ? 0xf240 : 0xf2c0) | (((v >> 11) & 1) << 10) | (v >> 12), (((v >> 8) & 7) << 12) | (Rd() << 8) | (v & 255)); }
    }
  }
  void Mem() {
    switch (r(4)) {
      case 0: {   // LDRD / STRD [r12, #imm8*4]
        uint32_t a = Rd(), b = Rd();
        if (a == b) b = (b + 1) % rd_max;
        W2((r(2) ? 0xe9dc : 0xe9cc), (a << 12) | (b << 8) | r(63));
        break;
      }
      case 1: {   // LDMIA / STMIA r12 (two or more registers)
        uint32_t list = r(1u << rd_max) | 3;
        list &= (1u << rd_max) - 1;
        if (__builtin_popcount(list) < 2) list = 3;
        W2(r(2) ? 0xe89c : 0xe88c, list);
        break;
      }
      default: {
        static const uint16_t st[] = {0xf8cc, 0xf88c, 0xf8ac}, ld[] = {0xf8dc, 0xf89c, 0xf8bc, 0xf99c, 0xf9bc};
        if (r(2)) { uint32_t k = r(3); W2(st[k], (r(12) << 12) | (k == 0 ? 4 * r(64) : k == 1 ? r(256) : 2 * r(128))); }
        else { uint32_t k = r(5); W2(ld[k], (Rd() << 12) | (k == 0 ? 4 * r(64) : k == 2 || k == 4 ? 2 * r(128) : r(256))); }
      }
    }
  }
  void One() {
    switch (r(9)) {
      case 0: case 1: case 2: Dp32(); break;
      case 3: case 4: Dp16(); break;
      case 5: Mul(); break;
      case 6: Misc(); break;
      default: Mem(); break;
    }
  }
  // |n| instructions; forward branches (CBZ, B<cond>), IT blocks, calls.
  void Body(int n, bool calls) {
    const size_t first = out->size();
    for (int i = 0; i < n; i++) {
      const uint32_t k = r(24);
      if (k == 0 && i + 1 < n) {   // CBZ / CBNZ over 1..3
        out->push_back({{(uint16_t)(0xb100 | (r(2) << 11) | Lo()), 0}, 1, (int)(out->size() + 1 + r(3)), 1});
      } else if (k == 1 && i + 1 < n) {   // B<cond> over 1..3
        out->push_back({{(uint16_t)(0xd000 | (r(14) << 8)), 0}, 1, (int)(out->size() + 1 + r(3)), 2});
      } else if (k == 2) {   // IT with 1 or 2 instructions
        const uint32_t cond = r(14), two = r(2);
        const uint32_t mask = two ? (((r(2) ? cond & 1 : ~cond & 1) << 3) | 4) : 8;
        W(0xbf00 | (cond << 4) | mask);
        One();
        if (two) One();
      } else if (k == 3 && calls) {
        out->push_back({{0, 0}, 2, r(2) ? -2 : -3, 3});
      } else {
        One();
      }
    }
    // (Forward targets past the end land on the next thing emitted.)
    for (size_t i = first; i < out->size(); i++)
      if ((*out)[i].target > (int)out->size()) (*out)[i].target = (int)out->size();
  }
};

bool ThumbLayout(std::vector<ThumbInsn>& v, const int* callee, std::vector<uint16_t>* hw) {
  std::vector<int> at(v.size() + 1);
  int pos = 0;
  for (size_t i = 0; i < v.size(); i++) { at[i] = pos; pos += v[i].n; }
  at[v.size()] = pos;
  for (size_t i = 0; i < v.size(); i++) {
    ThumbInsn& x = v[i];
    if (!x.kind) continue;
    const int tgt = x.target == -2 ? at[callee[0]] : x.target == -3 ? at[callee[1]] : at[x.target];
    const int off = 2 * (tgt - at[i]) - 4;   // from pc = insn + 4
    switch (x.kind) {
      case 1:
        if (off < 0 || off > 126) return false;
        x.hw[0] = (x.hw[0] & 0xfd07) | (((off >> 6) & 1) << 9) | (((off >> 1) & 31) << 3);
        break;
      case 2:
        if (off < -256 || off > 254) return false;
        x.hw[0] = (x.hw[0] & 0xff00) | ((off >> 1) & 0xff);
        break;
      case 3: case 4: {
        const uint32_t S = off < 0, imm = (uint32_t)off >> 1;
        if (x.kind == 3) {   // BL: S:I1:I2:imm10:imm11
          const uint32_t i1 = (imm >> 22) & 1, i2 = (imm >> 21) & 1, j1 = !(i1 ^ S), j2 = !(i2 ^ S);
          x.hw[0] = 0xf000 | (S << 10) | ((imm >> 11) & 0x3ff);
          x.hw[1] = 0xd000 | (j1 << 13) | (j2 << 11) | (imm & 0x7ff);
        } else {             // B<cond>.W: S:J2:J1:imm6:imm11
          const uint32_t j1 = (imm >> 17) & 1, j2 = (imm >> 18) & 1;
          x.hw[0] = (x.hw[0] & 0xfbc0) | (S << 10) | ((imm >> 11) & 0x3f);
          x.hw[1] = 0x8000 | (j1 << 13) | (j2 << 11) | (imm & 0x7ff);
        }
        break;
      }
    }
  }
  for (const ThumbInsn& x : v)
    for (int k = 0; k < x.n; k++) hw->push_back(x.hw[k]);
  return true;
}

void FuzzThumb(int n, uint64_t seed) {
  FuzzRng rng{seed * 0x9e3779b97f4a7c15ull + 7};
  ndk_set_jit(g_fuzz_jit);
  uint32_t* state = static_cast<uint32_t*>(aligned_alloc(16, 64));
  uint8_t* mem = static_cast<uint8_t*>(aligned_alloc(16, 512));
  uint16_t* code = static_cast<uint16_t*>(aligned_alloc(16, 8192));
  int bad = 0, made = 0;
  const uint64_t fns0 = ndk_cpu_jit_functions();
  for (int f = 0; f < n && bad < 5; f++) {
    std::vector<ThumbInsn> v;
    ThumbGen g{rng, 12, &v};
    g.W2(0xe92d, 0x4ff0); g.W2(0xf84d, 0x0d04); g.W2(0xf8d0, 0xc038); g.W2(0xf8d0, 0x1034); g.W2(0xf381, 0x8800);
    for (uint16_t x = 1; x <= 11; x++) g.W2(0xf8d0, (uint16_t)((x << 12) | (4 * x)));
    g.W2(0xf8d0, 0x0000);
    if (rng(2)) {
      g.Body(1 + rng(10), true);
      g.W2(0xf04f, 0x0b00 | (2 + rng(4)));        // mov r11, #2..5
      const int top = (int)v.size();
      g.rd_max = 11;
      g.Body(1 + rng(8), true);
      g.rd_max = 12;
      g.W2(0xf1bb, 0x0b01);                       // subs r11, r11, #1
      v.push_back({{0xf040 /* bne.w */, 0}, 2, top, 4});
      g.Body(rng(6), true);
    } else {
      g.Body(1 + rng(30), true);
    }
    g.W2(0xe92d, 0x1fff); g.W2(0xf3ef, 0x8000); g.W2(0xf8dd, 0x1034); g.W2(0xf8c1, 0x0034);
    for (uint16_t k = 0; k < 13; k++) { g.W2(0xf85d, 0x2b04); g.W2(0xf8c1, (uint16_t)(0x2000 | (4 * k))); }
    g.W(0xb001);
    g.W2(0xe8bd, 0x8ff0);
    int callee[2];
    for (int c = 0; c < 2; c++) {
      callee[c] = (int)v.size();
      g.rd_max = 11;
      g.Body(1 + rng(4), false);
      g.rd_max = 12;
      g.W(0x4770);   // bx lr
    }
    std::vector<uint16_t> hw;
    if (!ThumbLayout(v, callee, &hw) || hw.size() * 2 > 8192) continue;
    made++;
    if (g_fuzz_verbose) {
      printf("fuzz-thumb %d: %zu halfwords:", f, hw.size());
      for (uint16_t h : hw) printf(" %04x", h);
      printf("\n");
      fflush(stdout);
    }
    memcpy(code, hw.data(), hw.size() * 2);
    ndk_flush_code((uint32_t)(uintptr_t)code, hw.size() * 2);
    uint32_t init[13], flags = (rng(16) << 28);
    for (int i = 0; i < 13; i++) init[i] = rng(4) ? rng.Next() : rng(8);
    uint8_t init_mem[512];
    for (int i = 0; i < 512; i++) init_mem[i] = (uint8_t)rng.Next();
    uint32_t ref[14];
    uint8_t ref_mem[512];
    for (int run = 0; run < 10; run++) {
      memcpy(state, init, sizeof(init));
      state[13] = flags;
      state[14] = (uint32_t)(uintptr_t)mem;
      memcpy(mem, init_mem, 512);
      uint32_t arg = (uint32_t)(uintptr_t)state;
      ndk_call_guest((uint32_t)(uintptr_t)code | 1, &arg, 1);
      state[13] &= 0xf8000000u;
      if (!run) { memcpy(ref, state, sizeof(ref)); memcpy(ref_mem, mem, 512); continue; }
      int diff = -1;
      for (int i = 0; i < 14 && diff < 0; i++) if (i != 12 && state[i] != ref[i]) diff = i;
      if (diff < 0 && memcmp(mem, ref_mem, 512)) diff = 99;
      if (diff < 0) continue;
      bad++;
      Fail("fuzz-thumb %d run %d: %s %d: %08x, interpreted %08x (seed %llu)", f, run, diff == 99 ? "memory" : "register",
           diff, diff == 99 ? 0 : state[diff], diff == 99 ? 0 : ref[diff], (unsigned long long)seed);
      for (size_t i = 0; i < hw.size(); i++) printf("  %04x\n", hw[i]);
      break;
    }
    if (!bad) g_pass++;
  }
  printf("fuzz-thumb: %d functions, %d failed, %llu translations\n", made, bad,
         (unsigned long long)(ndk_cpu_jit_functions() - fns0));
}
}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> filters;
  int bench = 0, stress = 0, fuzz = 0, fuzz_thumb = 0;
  uint64_t fuzz_seed = 1;
  bool threads = true;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--bench")) bench = (i + 1 < argc && isdigit((unsigned char)argv[i + 1][0])) ? atoi(argv[++i]) : 200;
    else if (!strcmp(argv[i], "--no-threads")) threads = false;
    else if (!strcmp(argv[i], "--stress") && i + 1 < argc) stress = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--jit") && i + 1 < argc) ndk_set_jit(atoi(argv[++i]));
    else if (!strcmp(argv[i], "--env") && i + 1 < argc) putenv(argv[++i]);   // e.g. ARC_CPU_JIT_STATS=1
    else if (!strcmp(argv[i], "--fuzz-jit") && i + 1 < argc) g_fuzz_jit = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--fuzz-verbose")) g_fuzz_verbose = true;
    else if (!strcmp(argv[i], "--fuzz-neon")) g_fuzz_neon = true;
    else if (!strcmp(argv[i], "--fuzz-thumb") && i + 1 < argc) {
      fuzz_thumb = atoi(argv[++i]);
      if (i + 1 < argc && isdigit((unsigned char)argv[i + 1][0])) fuzz_seed = strtoull(argv[++i], nullptr, 10);
    }
    else if (!strcmp(argv[i], "--fuzz") && i + 1 < argc) {
      fuzz = atoi(argv[++i]);
      if (i + 1 < argc && isdigit((unsigned char)argv[i + 1][0])) fuzz_seed = strtoull(argv[++i], nullptr, 10);
    }
    else filters.push_back(argv[i]);
  }
  ndk_set_symbolizer(Symbolize);
  MakeStubs();
  if (fuzz || fuzz_thumb) {
    if (fuzz) Fuzz(fuzz, fuzz_seed);
    if (fuzz_thumb) FuzzThumb(fuzz_thumb, fuzz_seed);
    printf("ndk_cpu_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
  }
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
    ndk_call_guest(img->Find("guest_main"), nullptr, 0);
    TestCalls(*img);
    if (threads) { TestThreads(*img); TestSpin(*img); TestSignals(*img); }
    printf("== %s: %d passed, %d failed (%.0f ms)\n", variant.c_str(), g_pass - passes_before, g_fail - fails_before,
           (NowUs() - t0) / 1000.0);
    if (bench) { RunBench(*img, bench); RunBench(*img, bench / 4, "bench_calls"); }
  }
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
  NdkCpuStats st;
  ndk_cpu_stats(&st);
  printf("blocks decoded %llu (%llu instructions), flushed %llu, host calls %llu; "
         "translated to wasm %llu in %llu functions (%llu bytes), not translatable %llu\n",
         (unsigned long long)st.blocks_decoded, (unsigned long long)st.insns_decoded,
         (unsigned long long)st.blocks_flushed, (unsigned long long)st.host_calls,
         (unsigned long long)st.blocks_translated, (unsigned long long)ndk_cpu_jit_functions(),
         (unsigned long long)st.translated_bytes,
         (unsigned long long)st.blocks_not_translatable);
  printf("ndk_cpu_test: %d passed, %d failed (%.0f ms)\n", g_pass, g_fail, (NowUs() - t_start) / 1000.0);
  fflush(stdout);
  return g_fail ? 1 : 0;
}
