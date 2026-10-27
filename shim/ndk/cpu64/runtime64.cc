// The AArch64 CPU's runtime (topic "ndk-arm64"): the contract of
// shim/ndk/ndk_cpu64.h (host stubs, ndk64_call_guest, per-thread state and
// guest stacks, code flushing, faults), the block cache, system registers
// and raw syscalls. Mirrors shim/ndk/cpu/runtime.cc of the ARMv7 CPU.
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/heap.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

#include "ndk/cpu64/cpu64_internal.h"
#include "ndk/ndk_code_index.h"
#include "ndk/cpu64/fp64.h"

namespace ndkcpu64 {

namespace {

// --- host stubs ----------------------------------------------------------------------------
// Stub i lives at g_stub_base + 4 * i inside a block of ordinary memory (its
// words hold UDF). Stub 0 is the return address of ndk64_call_guest.
constexpr uint32_t kMaxStubs = 1 << 16;
struct Stub {
  Ndk64HostFn fn;
  void* user;
  const char* name;
};
std::mutex g_mu;   // stubs, blocks
uint32_t g_stub_base;
std::atomic<uint32_t> g_stub_count{0};
Stub* g_stubs;

void EnsureStubAreaLocked() {
  if (g_stub_base) return;
  void* area = aligned_alloc(16, kMaxStubs * 4);
  g_stubs = static_cast<Stub*>(calloc(kMaxStubs, sizeof(Stub)));
  if (!area || !g_stubs) { fprintf(stderr, "ndk_cpu64: out of memory for host stubs\n"); abort(); }
  memset(area, 0, kMaxStubs * 4);   // UDF #0
  g_stubs[0].name = "<return to host>";
  g_stub_base = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(area));
  g_stub_count.store(1, std::memory_order_release);
}

inline bool IsStub(uint32_t pc) { return g_stub_base && pc >= g_stub_base && pc < g_stub_base + kMaxStubs * 4; }

// --- blocks ----------------------------------------------------------------------------------
// (Never destroyed: threads may still run guest code while the process exits.)
std::unordered_map<uint32_t, Block*>& g_blocks = *new std::unordered_map<uint32_t, Block*>;
ndkcode::PageIndex<Block>& g_pages = *new ndkcode::PageIndex<Block>;   // the same blocks by guest page (flushes)
std::vector<Block*>& g_retired = *new std::vector<Block*>;
std::atomic<uint32_t> g_epoch{1};
std::atomic<uint64_t> g_blocks_decoded{0}, g_insns_decoded{0}, g_blocks_flushed{0}, g_host_calls{0};
// Flushes, and blocks in flushed ranges kept because their code is the same.
std::atomic<uint64_t> g_flush_calls{0}, g_blocks_kept{0};
std::atomic<uint64_t> g_block_bytes{0};
std::atomic<Ndk64BlockHook> g_block_hook{nullptr};

constexpr int kMaxBlockInsns = 64;

uint32_t HeapEnd() {
#ifdef __EMSCRIPTEN__
  return static_cast<uint32_t>(emscripten_get_heap_size());
#else
  return 0xffffffffu;
#endif
}

Block* NewBlock(uint32_t start, const DInsn* insns, int n, uint32_t end) {
  const size_t bytes = sizeof(Block) + sizeof(DInsn) * (n > 0 ? n - 1 : 0);
  Block* b = static_cast<Block*>(malloc(bytes));
  g_block_bytes.fetch_add(bytes, std::memory_order_relaxed);
  b->start = start;
  b->end = end;
  b->valid = true;
  b->hashed = false;
  b->hash = 0;
  b->n = n;
  b->next[0] = b->next[1] = nullptr;
  b->jit_id = 0;
  b->jit_member = 0;
  b->hits = 0;
  memcpy(b->insns, insns, sizeof(DInsn) * n);
  return b;
}

Block* SpecialBlock(uint32_t pc, uint16_t op, uint64_t imm, uint8_t amt = 0) {
  DInsn d;
  memset(&d, 0, sizeof(d));
  d.op = op;
  d.pc = pc;
  d.imm = imm;
  d.amt = amt;
  return NewBlock(pc, &d, 1, pc + 4);
}

Block* DecodeBlockLocked(uint32_t pc) {
  if (IsStub(pc)) {
    uint32_t index = (pc - g_stub_base) >> 2;
    if (index == 0) return SpecialBlock(pc, OP_RETURN, 0);
    if (index < g_stub_count.load(std::memory_order_acquire) && !(pc & 3))
      return SpecialBlock(pc, OP_HOSTCALL, index);
  }
  if (pc < 0x1000 || (pc & 3) || pc + 4 > HeapEnd())
    return SpecialBlock(pc, OP_UNDEF, 0, kUndefBadAddress);
  DInsn insns[kMaxBlockInsns + 1];
  int n = 0;
  uint32_t a = pc;
  bool ended = false;
  // The hash of exactly the words decoded (ndk_code_index.h).
  uint64_t hash = ndkcode::kHashSeed;
  while (n < kMaxBlockInsns) {
    if (a + 4 > HeapEnd()) break;
    DInsn& d = insns[n];
    const uint32_t w = Ld32(a);
    bool term = Decode(a, w, &d);
    hash = ndkcode::HashWord(hash, w);
    d.seq = n;
    n++;
    a += 4;
    if (term) { ended = true; break; }
    if (d.op == OP_UNDEF) { ended = true; break; }
    // ISB, SVC: context synchronization, after which code a flush (IC IVAU,
    // a syscall) just changed must be seen: the block ends there.
    if ((w & 0xfffff0ffu) == 0xd50330dfu || (w & 0xffe0001fu) == 0xd4000001u) break;
  }
  if (!ended) {
    DInsn* d = &insns[n++];
    memset(d, 0, sizeof(*d));
    d->op = OP_END;
    d->pc = a;
    d->imm = a;
    d->seq = n - 2;   // the instructions before it are counted, the END itself is not
  }
  g_blocks_decoded.fetch_add(1, std::memory_order_relaxed);
  g_insns_decoded.fetch_add(ended ? n : n - 1, std::memory_order_relaxed);
  Block* b = NewBlock(pc, insns, n, a);
  b->hashed = true;
  b->hash = hash;
  return b;
}

// Whether a block no longer matches the code at its address: changed since
// it was decoded. Stub blocks never change; other blocks without a hash
// (bad addresses) go with any flush of their range.
bool CodeChanged(const Block* b) {
  if (!b->hashed) return !IsStub(b->start);
  return b->end > HeapEnd() || ndkcode::HashRange(b->start, b->end) != b->hash;
}

// --- per-thread state ------------------------------------------------------------------------
size_t g_stack_size = 1 << 20;
Ndk64Symbolizer g_symbolizer;
Ndk64SyscallHandler g_syscall_handler;
Ndk64FaultHandler g_fault_handler;

struct ThreadCpu {
  Cpu* cpu = nullptr;
  ~ThreadCpu() {
    if (!cpu) return;
    Cpu* c = cpu;
    cpu = nullptr;   // guest code running after this (key destructors) gets a new state
    UnregisterCpu(c);   // (waits for a helper running its handlers)
    free(HostPtr(c->stack_base));
    delete[] c->l1;
    ndkjit::Free(&c->jit);
    free(c);
  }
};
thread_local ThreadCpu tl_cpu;
thread_local size_t tl_stack_size;

Cpu* CurrentCpu() {
  Cpu* c = tl_cpu.cpu;
  if (c) return c;
  JitApplyBudget();
  c = static_cast<Cpu*>(calloc(1, sizeof(Cpu)));
  size_t stack_size = tl_stack_size ? tl_stack_size : g_stack_size;
  void* stack = aligned_alloc(16, stack_size);
  if (!c || !stack) { fprintf(stderr, "ndk_cpu64: out of memory for a guest thread\n"); abort(); }
  c->stack_base = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(stack));
  c->stack_size = static_cast<uint32_t>(stack_size);
  c->sp = c->stack_base + c->stack_size;
  c->l1 = new Cpu::L1[kL1Size];
  for (int i = 0; i < kL1Size; i++) c->l1[i] = {0xffffffffu, nullptr};
  c->epoch = g_epoch.load();
  c->self = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pthread_self()));
  c->tid = static_cast<int32_t>(gettid());
  tl_cpu.cpu = c;
  RegisterCpu(c);
  return c;
}

}  // namespace

Cpu* CurrentCpuIfAny() { return tl_cpu.cpu; }
Cpu* SetCurrentCpu(Cpu* c) {
  Cpu* old = tl_cpu.cpu;
  tl_cpu.cpu = c;
  return old;
}

const char* SymbolizeAddr(uint32_t addr, uint32_t* off) {
  if (IsStub(addr)) {
    uint32_t i = (addr - g_stub_base) >> 2;
    if (i < g_stub_count.load()) { *off = addr & 3; return g_stubs[i].name ? g_stubs[i].name : "<host stub>"; }
  }
  if (g_symbolizer) return g_symbolizer(addr, off);
  return nullptr;
}

Block* FindBlock(uint32_t pc) {
  std::lock_guard<std::mutex> lock(g_mu);
  auto it = g_blocks.find(pc);
  return it == g_blocks.end() ? nullptr : it->second;
}

Block* LookupBlock(Cpu* c, uint32_t pc) {
  uint32_t epoch = g_epoch.load(std::memory_order_acquire);
  if (c->epoch != epoch) {
    for (int i = 0; i < kL1Size; i++) c->l1[i] = {0xffffffffu, nullptr};
    c->epoch = epoch;
  }
  Cpu::L1& e = c->l1[(pc >> 2) & (kL1Size - 1)];
  // (A flush retires blocks without touching the threads' caches.)
  if (e.key == pc && __atomic_load_n(&e.block->valid, __ATOMIC_RELAXED)) return e.block;
  Block* b;
  bool fresh = false;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_blocks.find(pc);
    if (it != g_blocks.end()) {
      b = it->second;
    } else {
      b = DecodeBlockLocked(pc);
      g_blocks.emplace(pc, b);
      g_pages.Add(b);
      fresh = true;
    }
  }
  if (fresh) {
    Ndk64BlockHook hook = g_block_hook.load(std::memory_order_relaxed);
    if (hook) hook(b->start, b->end);
  }
  e.key = pc;
  e.block = b;
  return b;
}

uint32_t ReturnStubAddr() {
  std::lock_guard<std::mutex> lock(g_mu);
  EnsureStubAreaLocked();
  return g_stub_base;
}

// --- host calls ------------------------------------------------------------------------------
extern "C" int __android_log_write(int prio, const char* tag, const char* text) __attribute__((weak));
std::atomic<bool> g_watch_on{false};
std::string g_watch;
std::atomic<uint32_t>* g_watch_state;

static bool MaybeString(uint64_t a) {
  if (a < 0x10000 || a >= HeapEnd() - 64) return false;
  const char* p = static_cast<const char*>(HostPtr(a));
  for (int i = 0; i < 40 && p[i]; ++i)
    if ((unsigned char)p[i] < 0x20 || (unsigned char)p[i] > 0x7e) return false;
  return p[0] != 0;
}

static void WatchHostCall(Cpu* c, uint32_t index, const Stub& s) {
  uint32_t st = g_watch_state[index].load(std::memory_order_relaxed);
  if (st == 0) {
    bool hit = false;
    for (size_t b = 0; s.name && b < g_watch.size();) {
      size_t e = g_watch.find(',', b);
      if (e == std::string::npos) e = g_watch.size();
      if (e > b && strstr(s.name, g_watch.substr(b, e - b).c_str())) hit = true;
      b = e + 1;
    }
    st = hit ? 2 : 1;
    g_watch_state[index].store(st, std::memory_order_relaxed);
  }
  if (st == 1) return;
  uint32_t n = g_watch_state[index].fetch_add(1, std::memory_order_relaxed) - 2;
  if (n < 400 || n % 1000 == 0) {
    char line[240], a0[48] = "", a1[48] = "";
    if (MaybeString(c->x[0])) snprintf(a0, sizeof a0, " \"%.40s\"", static_cast<const char*>(HostPtr(c->x[0])));
    if (MaybeString(c->x[1])) snprintf(a1, sizeof a1, " \"%.40s\"", static_cast<const char*>(HostPtr(c->x[1])));
    snprintf(line, sizeof line, "ndk_cpu64: host call #%u %s(%llx%s, %llx%s, %llx)", n + 1, s.name,
             (unsigned long long)c->x[0], a0, (unsigned long long)c->x[1], a1, (unsigned long long)c->x[2]);
    if (__android_log_write) __android_log_write(4, "ndk", line);
    else fprintf(stderr, "%s\n", line);
  }
}

void HostCall(Cpu* c, uint32_t index) {
  g_host_calls.fetch_add(1, std::memory_order_relaxed);
  const Stub& s = g_stubs[index];
  if (g_watch_on.load(std::memory_order_relaxed)) WatchHostCall(c, index, s);
  c->nzcv = PackNzcv(c);
  Cpu::RecentCall& rc = c->recent[c->recent_next++ & (Cpu::kRecent - 1)];
  rc = {index, c->x[0], c->x[1], c->x[2], 0};
  s.fn(c, s.user);
  UnpackNzcv(c, c->nzcv);
  rc.ret = c->x[0];
}

// --- system registers --------------------------------------------------------------------------
constexpr uint64_t kCounterHz = 24000000;

uint64_t ReadSysReg(Cpu* c, uint32_t reg) {
  switch (reg) {
    case kSysTpidr: return c->tpidr;
    case kSysTpidrro: return 0;
    case kSysFpcr: return c->fpcr;
    case kSysFpsr: return c->fpsr;
    case kSysNzcv: return PackNzcv(c);
    case kSysCntvct: {
      struct timespec ts;
      clock_gettime(CLOCK_MONOTONIC, &ts);
      return static_cast<uint64_t>(ts.tv_sec) * kCounterHz + static_cast<uint64_t>(ts.tv_nsec) * (kCounterHz / 1000000) / 1000;
    }
    case kSysCntfrq: return kCounterHz;
    case kSysDczid: return 4;               // DC ZVA blocks of 2^4 words (64 bytes)
    case kSysCtr: return 0x84448004u;       // 64-byte lines, VIPT instruction cache
    case kSysMidr: return 0x410fd034u;      // Cortex-A53 r0p4
    case kSysMpidr: return 0x80000000u;
    case kSysIdAa64isar0: return 0x00210000u;   // CRC32, LSE atomics (no AES/SHA/PMULL)
    case kSysIdAa64pfr0: return 0x11u;          // AArch64 EL0/EL1; FP and AdvSIMD (0 = implemented)
    default: return 0;
  }
}

void WriteSysReg(Cpu* c, uint32_t reg, uint64_t v) {
  switch (reg) {
    case kSysTpidr: c->tpidr = v; break;
    case kSysFpcr:
      c->fpcr = static_cast<uint32_t>(v) & 0x07ff9f00u;
      fp::SetFpcr(c->fpcr);
      break;
    case kSysFpsr: c->fpsr = static_cast<uint32_t>(v) & 0xf800009fu; break;
    case kSysNzcv: UnpackNzcv(c, static_cast<uint32_t>(v)); break;
  }
}

// --- syscalls ----------------------------------------------------------------------------------
void Syscall(Cpu* c, uint32_t imm) {
  if (g_syscall_handler) {
    c->nzcv = PackNzcv(c);
    if (g_syscall_handler(c)) {
      UnpackNzcv(c, c->nzcv);
      return;
    }
  }
  uint64_t* x = c->x;
  int64_t ret;
  switch (x[8]) {
    case 93: case 94:   // exit, exit_group
      fflush(stdout);
      exit(static_cast<int>(x[0]));
    case 64:   // write
      ret = write(static_cast<int>(x[0]), HostPtr(x[1]), static_cast<size_t>(x[2]));
      if (ret < 0) ret = -errno;
      break;
    case 172: case 178:   // getpid, gettid
      ret = getpid();
      break;
    case 124:   // sched_yield
      ret = sched_yield();
      break;
    case 101: {   // nanosleep
      struct timespec ts = {static_cast<time_t>(Ld64(x[0])), static_cast<long>(Ld64(x[0] + 8))}, rem;
      ret = nanosleep(&ts, &rem) == 0 ? 0 : -errno;
      break;
    }
    case 113: {   // clock_gettime
      struct timespec ts;
      // (the raw, coarse and boot-time clocks: the host's nearest, as
      // ndk_bridge.h's HostClockId)
      int c = static_cast<int>(x[0]);
      c = (c == 4 || c == 6 || c == 7 || c == 9) ? 1 : (c == 5 || c == 8 || c == 11) ? 0 : c;
      if (clock_gettime(static_cast<clockid_t>(c), &ts) != 0) { ret = -errno; break; }
      St64(x[1], static_cast<uint64_t>(ts.tv_sec));
      St64(x[1] + 8, static_cast<uint64_t>(ts.tv_nsec));
      ret = 0;
      break;
    }
    case 169: {   // gettimeofday
      struct timeval tv;
      gettimeofday(&tv, nullptr);
      if (x[0]) { St64(x[0], static_cast<uint64_t>(tv.tv_sec)); St64(x[0] + 8, static_cast<uint64_t>(tv.tv_usec)); }
      ret = 0;
      break;
    }
    default: {
      static std::atomic<uint32_t> reported[64];
      uint32_t n = static_cast<uint32_t>(x[8]);
      bool first = true;
      for (auto& slot : reported) {
        uint32_t v = slot.load();
        if (v == n + 1) { first = false; break; }
        if (v == 0 && slot.compare_exchange_strong(v, n + 1)) break;
      }
      if (first) {
        uint32_t off = 0, pc = static_cast<uint32_t>(c->pc);
        const char* sym = SymbolizeAddr(pc, &off);
        fprintf(stderr, "ndk_cpu64: unsupported syscall %u (svc #%u) at pc=%08x %s+0x%x, returning -ENOSYS\n", n,
                imm, pc, sym ? sym : "?", off);
      }
      ret = -38;   // ENOSYS
    }
  }
  x[0] = static_cast<uint64_t>(ret);
}

// --- faults and guarded calls --------------------------------------------------------------------
struct Guard {
  jmp_buf jb;
  Guard* outer;
  char why[256];
};
thread_local Guard* t_guard;

// The frame records (x29 -> {caller's x29, return address}) inside the thread's guest
// stack, innermost first: who called the code that failed (AAPCS64 code keeps x29 as
// the frame pointer; a record outside the stack ends the walk).
void ReportFrames(Cpu* c) {
  const uint64_t lo = c->sp, hi = static_cast<uint64_t>(c->stack_base) + c->stack_size;
  uint64_t fp = c->x[29];
  for (int n = 0; n < 24 && c->stack_size && fp >= lo && fp + 16 <= hi && !(fp & 7); n++) {
    const uint64_t* rec = static_cast<const uint64_t*>(HostPtr(fp));
    const uint64_t next = rec[0], ret = rec[1];
    uint32_t off = 0;
    const char* sym = SymbolizeAddr(static_cast<uint32_t>(ret), &off);
    fprintf(stderr, "    frame %d: %08llx %s+0x%x\n", n, (unsigned long long)ret, sym ? sym : "?", off);
    if (next <= fp) break;
    fp = next;
  }
}

void Fault(Cpu* c, uint32_t pc, const char* fmt, ...) {
  char msg[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  c->pc = pc;
  c->nzcv = PackNzcv(c);
  if (g_fault_handler) g_fault_handler(c, pc, msg);
  uint32_t off = 0;
  const char* sym = SymbolizeAddr(pc, &off);
  fprintf(stderr, "ndk_cpu64: %s at pc=%08x %s+0x%x\n", msg, pc, sym ? sym : "?", off);
  for (int i = 0; i < 32; i += 4) {
    fprintf(stderr, " ");
    for (int k = i; k < i + 4; k++) {
      if (k < 31) fprintf(stderr, " x%-2d %016llx", k, (unsigned long long)c->x[k]);
      else fprintf(stderr, " sp  %016llx", (unsigned long long)c->sp);
    }
    fprintf(stderr, "\n");
  }
  uint32_t lr_off = 0;
  const char* lr_sym = SymbolizeAddr(static_cast<uint32_t>(c->x[30]), &lr_off);
  fprintf(stderr, "  nzcv %08x  lr = %s+0x%x\n", c->nzcv, lr_sym ? lr_sym : "?", lr_off);
  ReportFrames(c);
  fflush(stderr);
  ndk64_leave_guest(msg);
  abort();
}

}  // namespace ndkcpu64

using namespace ndkcpu64;

namespace {

// What ndk64_call_guest_args saves of the interrupted guest code.
struct Saved {
  uint64_t x[32];   // x0-x30, sp
  uint64_t v[8][2];
  uint32_t fn, fz, fc, fv, ex_valid;
  uint64_t pc;
};
void Save(Cpu* c, Saved* s) {
  memcpy(s->x, c->x, sizeof(s->x));
  memcpy(s->v, c->v, sizeof(s->v));
  s->fn = c->fn; s->fz = c->fz; s->fc = c->fc; s->fv = c->fv;
  s->ex_valid = c->ex_valid;
  s->pc = c->pc;
}
void Restore(Cpu* c, const Saved* s) {
  memcpy(c->x, s->x, sizeof(s->x));
  memcpy(c->v, s->v, sizeof(s->v));
  c->fn = s->fn; c->fz = s->fz; c->fc = s->fc; c->fv = s->fv;
  c->ex_valid = s->ex_valid;
  c->pc = s->pc;
  c->nzcv = PackNzcv(c);
}

}  // namespace

extern "C" {

// Whether the calling host thread has a guest context: a Java thread with one
// keeps its worker (topic java-pool-park, shim/art10/dormant_wasm10.cc).
int ndk_cpu64_thread_has_state(void) { return ndkcpu64::tl_cpu.cpu != nullptr; }

uint32_t ndk64_host_stub(Ndk64HostFn fn, void* user, const char* name) {
  std::lock_guard<std::mutex> lock(g_mu);
  EnsureStubAreaLocked();
  uint32_t i = g_stub_count.load(std::memory_order_relaxed);
  if (i >= kMaxStubs) { fprintf(stderr, "ndk_cpu64: too many host stubs\n"); abort(); }
  g_stubs[i].fn = fn;
  g_stubs[i].user = user;
  g_stubs[i].name = name;
  g_stub_count.store(i + 1, std::memory_order_release);
  return g_stub_base + 4 * i;
}

const char* ndk64_host_stub_name(uint32_t addr) {
  if (!IsStub(addr) || (addr & 3)) return nullptr;
  uint32_t i = (addr - g_stub_base) >> 2;
  if (i == 0 || i >= g_stub_count.load()) return nullptr;
  return g_stubs[i].name;
}

int ndk64_is_host_stub(uint32_t addr) { return IsStub(addr) ? 1 : 0; }

void ndk64_call_guest_args(uint32_t addr, const Ndk64Args* a, Ndk64Result* r) {
  Cpu* c = CurrentCpu();
  // From outside guest code: not while a helper runs this thread's signal handlers.
  const bool outer = c->depth == 0;
  if (outer) GateEnter(c);
  uint32_t ret_addr = ReturnStubAddr();
  Saved saved;
  Save(c, &saved);
  uint64_t sp = c->sp;
  int nstack = a ? a->nstack : 0;
  sp = (sp - 8 * static_cast<uint64_t>(nstack)) & ~15ull;
  for (int i = 0; i < nstack; i++) St64(sp + 8 * i, a->stack[i]);
  for (int i = 0; i < 8; i++) {
    c->x[i] = a ? a->x[i] : 0;
    c->v[i][0] = a ? a->v[i] : 0;
    c->v[i][1] = 0;
  }
  c->sp = sp;
  c->x[30] = ret_addr;
  c->pc = addr;
  c->ex_valid = 0;
  Run(c);
  if (r) {
    r->x0 = c->x[0];
    r->x1 = c->x[1];
    r->v0 = c->v[0][0];
    r->v0_hi = c->v[0][1];
  }
  Restore(c, &saved);
  if (outer) GateLeave(c);
}

uint64_t ndk64_call_guest(uint32_t addr, const uint64_t* args, int nargs) {
  Ndk64Args a;
  memset(&a, 0, sizeof(a));
  for (int i = 0; i < nargs && i < 8; i++) a.x[i] = args[i];
  if (nargs > 8) {
    a.stack = args + 8;
    a.nstack = nargs - 8;
  }
  Ndk64Result r;
  ndk64_call_guest_args(addr, &a, &r);
  return r.x0;
}

int ndk64_call_guest_guarded(uint32_t addr, const Ndk64Args* args, Ndk64Result* result, char* why,
                             size_t why_size) {
  Cpu* c = CurrentCpu();
  Saved saved;
  Save(c, &saved);
  int sdepth = c->depth;
  Guard g;
  g.outer = t_guard;
  g.why[0] = '\0';
  t_guard = &g;
  if (setjmp(g.jb) == 0) {
    ndk64_call_guest_args(addr, args, result);
    t_guard = g.outer;
    return 1;
  }
  t_guard = g.outer;
  Restore(c, &saved);
  c->ex_valid = 0;
  c->depth = sdepth;
  if (sdepth == 0) GateLeave(c);   // (the call left without its own)
  snprintf(why, why_size, "%s", g.why);
  if (result) memset(result, 0, sizeof(*result));
  return 0;
}

// Resumes the guest at the full state |st| (all registers, sp, pc, flags) on
// the calling host thread — a continuation, not a fresh call (topic
// native-exec: a vfork child continues the parent's code mid-function, from
// the instruction after the clone SVC, with x0 = 0). It runs until the guest
// returns past this frame (rare: the child usually execve's or _exit's and
// never returns) or faults. 1 normally, 0 with |why| on a fault.
int ndk64_resume_guest(const NdkCpu64State* st, char* why, size_t why_size) {
  Cpu* c = CurrentCpu();
  Saved saved;
  Save(c, &saved);
  int sdepth = c->depth;
  Guard g;
  g.outer = t_guard;
  g.why[0] = '\0';
  t_guard = &g;
  if (setjmp(g.jb) == 0) {
    const bool outer = c->depth == 0;
    if (outer) GateEnter(c);
    memcpy(c->x, st->x, sizeof(c->x));
    memcpy(c->v, st->v, sizeof(c->v));
    c->sp = st->sp;
    c->tpidr = st->tpidr;
    c->fpcr = st->fpcr;
    c->fpsr = st->fpsr;
    UnpackNzcv(c, st->nzcv & 0xf0000000u);
    c->pc = st->pc;
    c->ex_valid = 0;
    Run(c);
    if (outer) GateLeave(c);
    t_guard = g.outer;
    Restore(c, &saved);
    return 1;
  }
  t_guard = g.outer;
  Restore(c, &saved);
  c->ex_valid = 0;
  c->depth = sdepth;
  if (sdepth == 0) GateLeave(c);
  if (why && why_size) snprintf(why, why_size, "%s", g.why);
  return 0;
}

void ndk64_leave_guest(const char* why) {
  Guard* g = t_guard;
  if (!g) return;
  snprintf(g->why, sizeof(g->why), "%s", why);
  longjmp(g->jb, 1);
}

int ndk64_in_guarded_call(void) { return t_guard != nullptr; }

NdkCpu64State* ndk64_current_state(void) { return CurrentCpu(); }

// Retires the blocks of [start, start + size) whose code changed
// (ndk_code_index.h); see ndk_flush_code (shim/ndk/cpu/runtime.cc).
void ndk64_flush_code(uint32_t start, size_t size) {
  uint64_t end64 = static_cast<uint64_t>(start) + size;
  uint32_t end = end64 > 0xffffffffull ? 0xffffffffu : static_cast<uint32_t>(end64);
  std::vector<Block*> hit;
  std::lock_guard<std::mutex> lock(g_mu);
  if (g_blocks.empty()) return;   // (the other ABI's CPU: the bridge flushes both)
  const uint64_t calls = g_flush_calls.fetch_add(1, std::memory_order_relaxed) + 1;
  // Code generated at run time shows as many flushes (a JIT: topic
  // guest-jit); the log says so now and then.
  if (calls >= 1024 && (calls & (calls - 1)) == 0)
    fprintf(stderr, "%s: %llu code flushes, %llu blocks retired, %llu kept\n", "ndk_cpu64",
            (unsigned long long)calls, (unsigned long long)g_blocks_flushed.load(),
            (unsigned long long)g_blocks_kept.load());
  g_pages.Overlapping(start, end, &hit);
  // (Past |changed|: blocks of the retired ones' regions, retired with them:
  // their translation has the changed code inside; jit64.cc.)
  const size_t changed = hit.size();
  for (size_t i = 0; i < hit.size(); i++) {
    Block* b = hit[i];
    if (!__atomic_load_n(&b->valid, __ATOMIC_RELAXED)) continue;
    if (i < changed && !CodeChanged(b)) {
      g_blocks_kept.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    __atomic_store_n(&b->valid, false, __ATOMIC_RELAXED);
    g_blocks.erase(b->start);
    g_pages.Remove(b);
    g_retired.push_back(b);
    JitForget(b, &hit);
    g_blocks_flushed.fetch_add(1, std::memory_order_relaxed);
  }
}

// The bridge's first reservation window (shim/ndk/ndk_libc.cc):
// translations made so far take a guest address's low 32 bits as it is.
// Every block goes, as for changed code; the blocks are decoded and
// translated again, with the window tables.
void ndk64_windows_on() {
  std::lock_guard<std::mutex> lock(g_mu);
  if (g_windows_on.exchange(true))
    return;
  for (auto& e : g_blocks) {
    Block* b = e.second;
    __atomic_store_n(&b->valid, false, __ATOMIC_RELAXED);
    g_pages.Remove(b);
    g_retired.push_back(b);
    std::vector<Block*> region;   // (every block goes anyway)
    JitForget(b, &region);
  }
  g_blocks.clear();
}

void ndk64_set_symbolizer(Ndk64Symbolizer fn) { g_symbolizer = fn; }
void ndk64_set_syscall_handler(Ndk64SyscallHandler fn) { g_syscall_handler = fn; }
void ndk64_set_fault_handler(Ndk64FaultHandler fn) { g_fault_handler = fn; }
void ndk64_set_guest_stack_size(size_t bytes) { g_stack_size = (bytes + 15) & ~(size_t)15; }
void ndk64_set_thread_guest_stack_size(size_t bytes) { tl_stack_size = (bytes + 15) & ~(size_t)15; }
void ndk64_guest_stack(uint32_t* base, uint32_t* size) {
  Cpu* c = CurrentCpu();
  *base = c->stack_base;
  *size = c->stack_size;
}

uint64_t ndk64_instruction_count(void) { return CurrentCpu()->icount; }

void ndk64_cpu_stats(Ndk64CpuStats* out) {
  out->blocks_decoded = g_blocks_decoded.load();
  out->insns_decoded = g_insns_decoded.load();
  out->blocks_flushed = g_blocks_flushed.load();
  out->host_calls = g_host_calls.load();
  JitStats(&out->blocks_translated, &out->translated_bytes, &out->blocks_not_translatable);
  out->flush_calls = g_flush_calls.load();
  out->blocks_kept = g_blocks_kept.load();
}

void ndk64_watch_host_calls(const char* words) {
  if (!words || !*words || g_watch_on.load()) return;
  g_watch = words;
  g_watch_state = new std::atomic<uint32_t>[kMaxStubs]();
  g_watch_on.store(true);
}

static void ReportRecentHostCalls(Cpu* c, uint32_t n) {
  fprintf(stderr, "  last host calls of this thread (oldest first):\n");
  for (uint32_t i = Cpu::kRecent - n; i < Cpu::kRecent; ++i) {
    const Cpu::RecentCall& rc = c->recent[(c->recent_next + i) & (Cpu::kRecent - 1)];
    if (rc.index == 0 && rc.x0 == 0 && rc.ret == 0) continue;
    const char* name = rc.index < g_stub_count.load() ? g_stubs[rc.index].name : "?";
    char arg0[48] = "";
    if (MaybeString(rc.x0)) snprintf(arg0, sizeof(arg0), " \"%.40s\"", static_cast<const char*>(HostPtr(rc.x0)));
    fprintf(stderr, "    %s(%llx%s, %llx, %llx) = %llx\n", name ? name : "?", (unsigned long long)rc.x0, arg0,
            (unsigned long long)rc.x1, (unsigned long long)rc.x2, (unsigned long long)rc.ret);
  }
  fflush(stderr);
}

// Every thread with an AArch64 CPU: where it is (racy reads of the other threads'
// state: for hangs, with ndk_cpu_dump_threads). Guest pc (the block it runs, or the
// host function it is in) and lr with symbols, its gate and signals, its last host
// calls, return addresses of its frame records.
void ndk64_cpu_dump_threads(void) {
  std::vector<Cpu*> cpus = CpusSnapshot();
  fprintf(stderr, "ndk_cpu64: %zu threads with arm64 guest code\n", cpus.size());
  for (Cpu* c : cpus) {
    const uint32_t pc = static_cast<uint32_t>(c->pc);
    uint32_t off = 0, lr_off = 0, b_off = 0;
    const char* host = IsStub(pc) ? SymbolizeAddr(pc, &off) : nullptr;
    const char* blk = c->depth ? SymbolizeAddr(c->block_start, &b_off) : nullptr;
    const char* lr = c->depth ? SymbolizeAddr(static_cast<uint32_t>(c->x[30]), &lr_off) : nullptr;
    fprintf(stderr, "  tid %d: depth %d gate %u, signals pending %llx blocked %llx; block %08x %s+0x%x%s%s lr %s+0x%x, %llu insns\n",
            c->tid, c->depth, c->gate.load(), (unsigned long long)c->sig_pending.load(),
            (unsigned long long)c->sig_blocked, c->block_start, blk ? blk : "?", b_off,
            host ? ", in " : "", host ? host : "", lr ? lr : "?", lr_off, (unsigned long long)c->icount);
    for (uint32_t i = Cpu::kRecent - 4; i < Cpu::kRecent; ++i) {
      const Cpu::RecentCall& rc = c->recent[(c->recent_next + i) & (Cpu::kRecent - 1)];
      if (rc.index == 0 && rc.x0 == 0 && rc.ret == 0) continue;
      const char* name = rc.index < g_stub_count.load() ? g_stubs[rc.index].name : "?";
      fprintf(stderr, "      %s(%llx, %llx, %llx) = %llx\n", name ? name : "?", (unsigned long long)rc.x0,
              (unsigned long long)rc.x1, (unsigned long long)rc.x2, (unsigned long long)rc.ret);
    }
    if (c->depth) {
      uint64_t fp = c->x[29];
      const uint64_t hi = static_cast<uint64_t>(c->stack_base) + c->stack_size;
      for (int n = 0; n < 12 && fp >= c->stack_base && fp + 16 <= hi && !(fp & 7); n++) {
        const uint64_t* rec = static_cast<const uint64_t*>(HostPtr(fp));
        uint32_t o = 0;
        const char* sym = SymbolizeAddr(static_cast<uint32_t>(rec[1]), &o);
        fprintf(stderr, "      frame %d: %s+0x%x\n", n, sym ? sym : "?", o);
        if (rec[0] <= fp) break;
        fp = rec[0];
      }
    }
  }
  fflush(stderr);
}

void ndk64_report_recent_calls(void) {
  if (Cpu* c = tl_cpu.cpu) ReportRecentHostCalls(c, Cpu::kRecent);
}

void ndk64_set_block_hook(Ndk64BlockHook hook) { g_block_hook.store(hook); }

int ndk64_decodable(uint32_t addr, char* why, size_t why_size) {
  DInsn d;
  Decode(addr, Ld32(addr), &d);
  if (d.op != OP_UNDEF) return 1;
  if (why && why_size) {
    static const char* const kWhy[] = {"undefined", "unsupported", "unallocated", "UDF", "bad address"};
    snprintf(why, why_size, "%s %08x", kWhy[d.amt < 5 ? d.amt : 0], static_cast<uint32_t>(d.imm));
  }
  return 0;
}

// Without stdio, for the JS side of the report: 0 outside guest code, else
// words: block start, current pc, then x0..x30 and sp as low/high pairs.
EMSCRIPTEN_KEEPALIVE const uint32_t* ndk64_cpu_trap_regs(void) {
  static thread_local uint32_t w[2 + 64];
  Cpu* c = tl_cpu.cpu;
  if (c == nullptr || c->depth == 0) return nullptr;
  w[0] = c->block_start;
  w[1] = c->cur_pc;
  for (int i = 0; i < 31; i++) {
    w[2 + 2 * i] = static_cast<uint32_t>(c->x[i]);
    w[3 + 2 * i] = static_cast<uint32_t>(c->x[i] >> 32);
  }
  w[64] = static_cast<uint32_t>(c->sp);
  w[65] = static_cast<uint32_t>(c->sp >> 32);
  return w;
}

// After a wasm trap in guest code (an out-of-bounds access): where it was.
EMSCRIPTEN_KEEPALIVE void ndk64_cpu_report_trap(void) {
  Cpu* c = tl_cpu.cpu;
  if (c == nullptr || c->depth == 0) return;
  uint32_t off = 0;
  const char* sym = SymbolizeAddr(c->block_start, &off);
  fprintf(stderr, "ndk_cpu64: wasm trap in guest code, in the block at pc=%08x %s+0x%x\n", c->block_start,
          sym ? sym : "?", off);
  for (int i = 0; i < 31; i += 4) {
    fprintf(stderr, " ");
    for (int k = i; k < i + 4 && k < 31; k++) fprintf(stderr, " x%-2d %016llx", k, (unsigned long long)c->x[k]);
    fprintf(stderr, "\n");
  }
  uint32_t lr_off = 0;
  const char* lr_sym = SymbolizeAddr(static_cast<uint32_t>(c->x[30]), &lr_off);
  fprintf(stderr, "  sp %016llx  lr = %s+0x%x\n", (unsigned long long)c->sp, lr_sym ? lr_sym : "?", lr_off);
  ReportFrames(c);
  ReportRecentHostCalls(c, 48);
}

}  // extern "C"

// The bridge's reservation windows (cpu64_internal.h, shim/ndk/ndk_libc.cc):
// table 0 covers the first 4 GB, the wasm memory (the JIT looks it up for
// every guest address once there are windows).
namespace {
uint32_t g_no_window_table[1u << 16];
}  // namespace
uint32_t* ndk64_window_table[64] = {g_no_window_table};
std::atomic<bool> ndkcpu64::g_windows_on{false};

// The memory budget (shim/membudget): decoded blocks and translations.
extern "C" void ndk64_cpu_mem_stats(uint64_t* block_bytes, uint64_t* jit_bytes) {
  uint64_t blocks = 0, bytes = 0, rejected = 0;
  ndkcpu64::JitStats(&blocks, &bytes, &rejected);
  *block_bytes = ndkcpu64::g_block_bytes.load();
  *jit_bytes = bytes;
}

extern "C" int64_t ndk64_cpu_mem_trim(int level) {
  ndkcpu64::JitPause(level >= 15);
  return 0;
}
