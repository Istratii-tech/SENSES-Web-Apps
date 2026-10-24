// The NDK CPU's runtime: the contract of shim/ndk/ndk_cpu.h (host stubs,
// ndk_call_guest, per-thread state and guest stacks, code flushing,
// faults), the block cache, Linux kuser helpers and raw syscalls.
#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/time.h>

#include <atomic>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/heap.h>
#include <emscripten/threading.h>
#endif

#include "ndk/cpu/cpu_internal.h"
#include "ndk/cpu/ndk_cpu_ext.h"
#include "ndk/ndk_code_index.h"

namespace ndkcpu {

namespace {

// --- host stubs ----------------------------------------------------------------------
// Stub i lives at g_stub_base + 4 * i, inside a block of ordinary memory, so
// stub addresses never collide with guest code. The words hold a permanently
// undefined instruction in case anything reads them as code. Stub 0 is the
// return address of ndk_call_guest.
constexpr uint32_t kMaxStubs = 1 << 16;
struct Stub {
  NdkHostFn fn;
  void* user;
  const char* name;
};
std::mutex g_mu;   // stubs, blocks
uint32_t g_stub_base;
std::atomic<uint32_t> g_stub_count{0};
Stub* g_stubs;

// Kuser helpers (the Linux vector page).
constexpr uint32_t kKuserStart = 0xffff0f60;
constexpr uint32_t kKuserCmpxchg64 = 0xffff0f60;
constexpr uint32_t kKuserDmb = 0xffff0fa0;
constexpr uint32_t kKuserCmpxchg = 0xffff0fc0;
constexpr uint32_t kKuserGetTls = 0xffff0fe0;

void EnsureStubAreaLocked() {
  if (g_stub_base) return;
  void* area = aligned_alloc(16, kMaxStubs * 4);
  g_stubs = static_cast<Stub*>(calloc(kMaxStubs, sizeof(Stub)));
  if (!area || !g_stubs) { fprintf(stderr, "ndk_cpu: out of memory for host stubs\n"); abort(); }
  uint32_t* words = static_cast<uint32_t*>(area);
  for (uint32_t i = 0; i < kMaxStubs; i++) words[i] = 0xe7ffdefe;   // ARM UDF / Thumb UDF #0xfe
  g_stubs[0].name = "<return to host>";
  g_stub_base = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(area));
  g_stub_count.store(1, std::memory_order_release);
}

// --- blocks --------------------------------------------------------------------------
// (Never destroyed: threads may still run guest code while the process
// exits, and a static destructor freed the table under them.)
std::unordered_map<uint32_t, Block*>& g_blocks = *new std::unordered_map<uint32_t, Block*>;   // key: address | thumb
ndkcode::PageIndex<Block>& g_pages = *new ndkcode::PageIndex<Block>;   // the same blocks by guest page (flushes)
std::vector<Block*>& g_retired = *new std::vector<Block*>;   // flushed blocks (a thread may still run one)
std::atomic<uint32_t> g_epoch{1};
std::atomic<uint64_t> g_blocks_decoded{0}, g_insns_decoded{0}, g_blocks_flushed{0}, g_host_calls{0};
// Flushes, and blocks in flushed ranges kept because their code is the same.
std::atomic<uint64_t> g_flush_calls{0}, g_blocks_kept{0};
// Bytes of decoded blocks (retired ones included: they are never freed, a
// thread may still run one); for the memory budget (ndk_cpu_mem_stats).
std::atomic<uint64_t> g_block_bytes{0};

constexpr int kMaxBlockInsns = 64;

uint32_t HeapEnd() {
#ifdef __EMSCRIPTEN__
  return static_cast<uint32_t>(emscripten_get_heap_size());
#else
  return 0xffffffffu;
#endif
}

Block* NewBlock(uint32_t start, bool thumb, const DInsn* insns, int n, uint32_t end) {
  const size_t bytes = sizeof(Block) + sizeof(DInsn) * (n > 0 ? n - 1 : 0);
  Block* b = static_cast<Block*>(malloc(bytes));
  g_block_bytes.fetch_add(bytes, std::memory_order_relaxed);
  b->start = start;
  b->end = end;
  b->thumb = thumb;
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

Block* SpecialBlock(uint32_t pc, bool thumb, uint16_t op, uint32_t imm) {
  DInsn d;
  memset(&d, 0, sizeof(d));
  d.op = op;
  d.cond = kAL;
  d.len = 4;
  d.pc = pc + (thumb ? 4 : 8);
  d.imm = imm;
  return NewBlock(pc, thumb, &d, 1, pc + 4);
}

// Whether an instruction reads r15 (as an operand, a stored register or for
// a fault report): those get an OP_SETPC in front of them, so the
// interpreter does not have to keep r15 current for every instruction.
// Register fields of VFP operations are S/D numbers; a false positive only
// costs an extra OP_SETPC.
bool ReadsPc(const DInsn& d) {
  switch (d.op) {
    case OP_NEON: case OP_NEON_LDST: case OP_SVC: case OP_UNDEF: case OP_BKPT:
      return true;
    case OP_STM:
      return (d.imm & 0x8000) != 0;
    case OP_LDM: case OP_LDM_PC: case OP_B: case OP_BL: case OP_BLX_IMM: case OP_END: case OP_MOVW:
      return false;
  }
  return d.rd == 15 || d.rn == 15 || d.rm == 15 || d.rs == 15 || d.ra == 15;
}

Block* DecodeBlockLocked(Cpu* c, uint32_t pc, bool thumb) {
  if (g_stub_base && pc >= g_stub_base && pc < g_stub_base + kMaxStubs * 4) {
    uint32_t index = (pc - g_stub_base) >> 2;
    if (index == 0) return SpecialBlock(pc, thumb, OP_RETURN, 0);
    if (index < g_stub_count.load(std::memory_order_acquire) && !(pc & 3))
      return SpecialBlock(pc, thumb, OP_HOSTCALL, index);
  }
  if (pc >= kKuserStart) {
    switch (pc) {
      case kKuserCmpxchg64: case kKuserDmb: case kKuserCmpxchg: case kKuserGetTls:
        return SpecialBlock(pc, thumb, OP_KUSER, pc);
    }
  }
  if (pc < 0x1000 || pc + 8 > HeapEnd()) {
    // Faults when executed (not here: the block lock is held).
    Block* b = SpecialBlock(pc, thumb, OP_UNDEF, 0);
    b->insns[0].imm2 = kUndefBadAddress;
    return b;
  }
  DInsn insns[2 * kMaxBlockInsns + 16];
  int n = 0, guest = 0;
  uint32_t a = pc;
  uint8_t it = 0;
  bool ended = false;
  // The hash of exactly the units decoded (ndk_code_index.h).
  uint64_t hash = ndkcode::kHashSeed;
  while (guest < kMaxBlockInsns || (thumb && (it & 0xf))) {
    if (a + 4 > HeapEnd()) break;
    DInsn d;
    bool term, sync;
    if (thumb) {
      const uint32_t hw1 = Ld16(a), hw2 = Ld16(a + 2);
      term = DecodeThumbUnits(a, hw1, hw2, &it, &d);
      hash = ndkcode::HashUnit(hash, hw1);
      if (d.len == 4) hash = ndkcode::HashUnit(hash, hw2);
      // ISB (f3bf 8f6x), SVC (dfxx): context synchronization, after which
      // code a flush just changed must be seen: the block ends there.
      sync = (hw1 == 0xf3bf && (hw2 & 0xfff0) == 0x8f60) || (hw1 & 0xff00) == 0xdf00;
    } else {
      const uint32_t w = Ld32(a);
      term = DecodeArm(a, w, &d);
      hash = ndkcode::HashWord(hash, w);
      sync = (w & 0xfffffff0u) == 0xf57ff060u || ((w & 0x0f000000u) == 0x0f000000u && (w >> 28) != 0xf);
    }
    d.seq = guest;
    if (ReadsPc(d)) {
      DInsn* s = &insns[n++];
      memset(s, 0, sizeof(*s));
      s->op = OP_SETPC;
      s->cond = kAL;
      s->imm = s->pc = d.pc;
      s->seq = guest;
    }
    insns[n++] = d;
    guest++;
    a += d.len;
    if (term) { ended = true; break; }
    if (d.op == OP_UNDEF) break;   // likely data; stop decoding here
    if (sync && !(thumb && (it & 0xf))) break;
  }
  if (!ended) {
    DInsn* d = &insns[n++];
    memset(d, 0, sizeof(*d));
    d->op = OP_END;
    d->cond = kAL;
    d->len = 0;
    d->pc = a + (thumb ? 4 : 8);
    d->imm = a;
    d->seq = guest;
  }
  g_blocks_decoded.fetch_add(1, std::memory_order_relaxed);
  g_insns_decoded.fetch_add(guest, std::memory_order_relaxed);
  Block* b = NewBlock(pc, thumb, insns, n, a);
  b->hashed = true;
  b->hash = hash;
  return b;
}

// Whether a block no longer matches the code at its address: changed since
// it was decoded. Stub and kuser blocks never change; other blocks without
// a hash (bad addresses) go with any flush of their range.
bool CodeChanged(const Block* b) {
  if (!b->hashed) {
    const bool fixed = (g_stub_base && b->start >= g_stub_base && b->start < g_stub_base + kMaxStubs * 4) ||
                       b->start >= kKuserStart;
    return !fixed;
  }
  return b->end > HeapEnd() || ndkcode::HashRange(b->start, b->end) != b->hash;
}

}  // namespace

// Every thread's CPU, for dumps of them all (ndk_cpu_dump_threads) and
// signals (signal.cc).
std::mutex& CpusLock() {
  static std::mutex* mu = new std::mutex;
  return *mu;
}
std::set<Cpu*>& Cpus() {
  static std::set<Cpu*>* cpus = new std::set<Cpu*>;
  return *cpus;
}

namespace {

// --- per-thread state ----------------------------------------------------------------
size_t g_stack_size = 1 << 20;
NdkSymbolizer g_symbolizer;
NdkSyscallHandler g_syscall_handler;
NdkFaultHandler g_fault_handler;


struct ThreadCpu {
  Cpu* cpu = nullptr;
  ~ThreadCpu() {
    if (!cpu) return;
    Cpu* c = cpu;
    {
      std::lock_guard<std::mutex> lock(CpusLock());
      Cpus().erase(c);
    }
    // A helper may be running a signal handler on it (signal.cc).
    GateEnter(c);
    // Guest code may still run on this thread after this destructor: the
    // guest's pthread key destructors (ndk_pthread.cc) run from a host key
    // destructor, which on Emscripten comes after the C++ thread_local
    // destructors. CurrentCpu() then makes a new Cpu instead of using the
    // freed one (whose translated-function table belonged to it).
    cpu = nullptr;
    free(HostPtr(c->stack_base));
    delete[] c->l1;
    ndkjit::Free(&c->jit);
    free(c);
  }
};
thread_local ThreadCpu tl_cpu;
thread_local size_t tl_stack_size;  // ndk_set_thread_guest_stack_size

Cpu* CurrentCpu() {
  Cpu* c = tl_cpu.cpu;
  if (c) return c;
  JitApplyBudget();
  c = static_cast<Cpu*>(calloc(1, sizeof(Cpu)));
  size_t stack_size = tl_stack_size ? tl_stack_size : g_stack_size;
  void* stack = aligned_alloc(16, stack_size);
  if (!c || !stack) { fprintf(stderr, "ndk_cpu: out of memory for a guest thread\n"); abort(); }
  c->stack_base = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(stack));
  c->stack_size = static_cast<uint32_t>(stack_size);
  c->r[13] = c->stack_base + c->stack_size;
  c->fz = 1;
  c->cpsr = PackCpsr(c);
  c->l1 = new Cpu::L1[kL1Size];
  for (int i = 0; i < kL1Size; i++) c->l1[i] = {0xffffffffu, nullptr};
  c->epoch = g_epoch.load();
  c->tid = gettid();
  c->pid = getpid();  // a light process's pid (multiproc), for dumps
  c->self = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pthread_self()));
  {
    std::lock_guard<std::mutex> lock(CpusLock());
    Cpus().insert(c);
  }
  tl_cpu.cpu = c;
  return c;
}

const char* Symbolize(uint32_t addr, uint32_t* off) {
  if (g_stub_base && addr >= g_stub_base && addr < g_stub_base + kMaxStubs * 4) {
    uint32_t i = (addr - g_stub_base) >> 2;
    if (i < g_stub_count.load()) { *off = addr & 3; return g_stubs[i].name ? g_stubs[i].name : "<host stub>"; }
  }
  if (g_symbolizer) return g_symbolizer(addr, off);
  return nullptr;
}

}  // namespace

std::atomic<NdkBlockHook> g_block_hook{nullptr};

Cpu* CurrentCpuIfAny() { return tl_cpu.cpu; }
Cpu* SetCurrentCpu(Cpu* c) {
  Cpu* old = tl_cpu.cpu;
  tl_cpu.cpu = c;
  return old;
}

void GateEnter(Cpu* c) {
  for (;;) {
    uint32_t expected = 0;
    if (c->gate.compare_exchange_strong(expected, 1)) return;
#ifdef __EMSCRIPTEN__
    emscripten_futex_wait(&c->gate, expected, 50);
#else
    usleep(1000);
#endif
  }
}
int& GuestNesting() {
  static thread_local int nesting;
  return nesting;
}

Block* LookupBlock(Cpu* c, uint32_t pc, bool thumb) {
  uint32_t epoch = g_epoch.load(std::memory_order_acquire);
  if (c->epoch != epoch) {
    for (int i = 0; i < kL1Size; i++) c->l1[i] = {0xffffffffu, nullptr};
    c->epoch = epoch;
  }
  uint32_t key = pc | (thumb ? 1 : 0);
  Cpu::L1& e = c->l1[(pc >> 1) & (kL1Size - 1)];
  // (A flush retires blocks without touching the threads' caches.)
  if (e.key == key && __atomic_load_n(&e.block->valid, __ATOMIC_RELAXED)) return e.block;
  Block* b;
  bool fresh = false;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_blocks.find(key);
    if (it != g_blocks.end()) {
      b = it->second;
    } else {
      b = DecodeBlockLocked(c, pc, thumb);
      g_blocks.emplace(key, b);
      g_pages.Add(b);
      fresh = true;
    }
  }
  if (fresh) {
    NdkBlockHook hook = g_block_hook.load(std::memory_order_relaxed);
    if (hook) hook(b->start, b->end);
  }
  e.key = key;
  e.block = b;
  return b;
}

Block* FindBlock(uint32_t pc, bool thumb) {
  std::lock_guard<std::mutex> lock(g_mu);
  auto it = g_blocks.find(pc | (thumb ? 1 : 0));
  return it == g_blocks.end() ? nullptr : it->second;
}

uint32_t ReturnStubAddr() {
  std::lock_guard<std::mutex> lock(g_mu);
  EnsureStubAreaLocked();
  return g_stub_base;
}

// Debugging (ndk_cpu_watch_host_calls): host functions whose name contains
// one of the comma-separated words are logged, their first 400 calls and every
// 1000th. Per stub: 0 not decided yet, 1 not watched, else 2 + calls.
extern "C" int __android_log_write(int prio, const char* tag, const char* text) __attribute__((weak));
std::atomic<bool> g_watch_on{false};
std::string g_watch;
// debug.ndk.watch_str (ndk_cpu_watch_strings): host calls whose first three
// arguments point at memory holding TEXT in its first 256 bytes, with the
// caller's (light) pid: which process handles some data (a page's text).
std::atomic<bool> g_watch_str_on{false};
std::string g_watch_str;
std::atomic<int> g_watch_str_hits{0};
std::atomic<uint32_t>* g_watch_state;

}  // namespace ndkcpu
static bool MaybeString(uint32_t a);
namespace ndkcpu {

void WatchHostCall(Cpu* c, uint32_t index, const Stub& s) {
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
  if (st == 1)
    return;
  uint32_t n = g_watch_state[index].fetch_add(1, std::memory_order_relaxed) - 2;
  if (n < 400 || n % 1000 == 0) {
    char line[200], a0[48] = "", a1[48] = "";
    if (MaybeString(c->r[0])) snprintf(a0, sizeof a0, " \"%.40s\"", static_cast<const char*>(HostPtr(c->r[0])));
    if (MaybeString(c->r[1])) snprintf(a1, sizeof a1, " \"%.40s\"", static_cast<const char*>(HostPtr(c->r[1])));
    snprintf(line, sizeof line, "ndk_cpu: host call #%u [%d] %s(%08x%s, %08x%s, %08x)", n + 1,
             c->tid, s.name, c->r[0], a0, c->r[1], a1, c->r[2]);
    if (__android_log_write)
      __android_log_write(4, "ndk", line);
    else
      fprintf(stderr, "%s\n", line);
  }
}

void WatchHostString(Cpu* c, const Stub& s) {
  const size_t kScan = 256;
  for (int i = 0; i < 3; ++i) {
    const uint32_t a = c->r[i];
    if (a < 0x10000 || a >= emscripten_get_heap_size() - kScan)
      continue;
    const char* p = static_cast<const char*>(HostPtr(a));
    const char* hit = static_cast<const char*>(memmem(p, kScan, g_watch_str.data(), g_watch_str.size()));
    if (!hit)
      continue;
    if (g_watch_str_hits.fetch_add(1, std::memory_order_relaxed) >= 100)
      return;
    char text[81];
    const char* from = hit - p > 20 ? hit - 20 : p;
    size_t n = 0;
    for (; n < 80 && from + n < p + kScan; ++n)
      text[n] = from[n] >= 0x20 && from[n] < 0x7f ? from[n] : '.';
    text[n] = 0;
    char line[200];
    snprintf(line, sizeof line, "ndk_cpu: [tid %d pid %d] %s(arg%d=%08x): \"%s\"", c->tid, c->pid,
             s.name ? s.name : "?", i, a, text);
    if (__android_log_write)
      __android_log_write(4, "ndk", line);
    else
      fprintf(stderr, "%s\n", line);
    return;
  }
}

void HostCall(Cpu* c, uint32_t index) {
  g_host_calls.fetch_add(1, std::memory_order_relaxed);
  const Stub& s = g_stubs[index];
  if (g_watch_on.load(std::memory_order_relaxed))
    WatchHostCall(c, index, s);
  if (g_watch_str_on.load(std::memory_order_relaxed))
    WatchHostString(c, s);
  c->cpsr = PackCpsr(c);
  Cpu::RecentCall& rc = c->recent[c->recent_next++ & (Cpu::kRecent - 1)];
  rc = {index, c->r[0], c->r[1], c->r[2], 0};
  const uint32_t outer = c->in_host;
  c->in_host = index + 1;
  // A green thread's call that parked here before runs again: its record
  // goes to the host function (ndk_cpu_resumed); a nested host call does not
  // see its caller's.
  NdkResume* const outer_resumed = c->resumed;
  c->resumed = nullptr;
  if (__builtin_expect(c->nresumes != 0, 0)) {
    const Cpu::Resume& top = c->resumes[c->nresumes - 1];
    if (top.index == index && top.sp == c->r[13]) {
      c->resumed_rec = top.rec;
      c->resumed = &c->resumed_rec;
      c->nresumes--;
    }
  }
  if (__builtin_expect(c->green, 0)) {
    const uint32_t a0 = c->r[0], a1 = c->r[1], a2 = c->r[2], a3 = c->r[3];
    s.fn(c, s.user);
    if (c->park == kNdkParkCall) {   // the call runs again: its arguments as they were
      c->r[0] = a0; c->r[1] = a1; c->r[2] = a2; c->r[3] = a3;
    }
  } else {
    s.fn(c, s.user);
  }
  c->resumed = outer_resumed;
  c->in_host = outer;
  rc.ret = c->r[0];
}

void Kuser(Cpu* c, uint32_t addr) {
  switch (addr) {
    case kKuserGetTls:
      c->r[0] = c->tls;
      return;
    case kKuserDmb:
      __atomic_thread_fence(__ATOMIC_SEQ_CST);
      return;
    case kKuserCmpxchg: {
      uint32_t expected = c->r[0];
      bool ok = __atomic_compare_exchange_n(static_cast<uint32_t*>(HostPtr(c->r[2])), &expected, c->r[1], false,
                                            __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
      c->r[0] = ok ? 0 : 1;
      c->fc = ok;
      c->fz = ok ? 0 : 1;
      c->fn = 0;
      return;
    }
    case kKuserCmpxchg64: {
      uint64_t expected = Ld64(c->r[0]), desired = Ld64(c->r[1]);
      bool ok = __atomic_compare_exchange_n(static_cast<uint64_t*>(HostPtr(c->r[2])), &expected, desired, false,
                                            __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
      c->r[0] = ok ? 0 : 1;
      c->fc = ok;
      c->fz = ok ? 0 : 1;
      c->fn = 0;
      return;
    }
  }
  Fault(c, addr, "unknown kuser helper");
}

void Syscall(Cpu* c, uint32_t imm) {
  if (g_syscall_handler) {
    c->cpsr = PackCpsr(c);
    if (g_syscall_handler(c)) return;
  }
  uint32_t* r = c->r;
  int32_t ret;
  switch (r[7]) {
    case 1: case 248:   // exit, exit_group
      fflush(stdout);
      exit((int)r[0]);
    case 4:   // write
      ret = (int32_t)write((int)r[0], HostPtr(r[1]), r[2]);
      if (ret < 0) ret = -errno;
      break;
    case 20:   // getpid
    case 224:  // gettid
      ret = getpid();
      break;
    case 78: {   // gettimeofday
      struct timeval tv;
      gettimeofday(&tv, nullptr);
      if (r[0]) { St32(r[0], (uint32_t)tv.tv_sec); St32(r[0] + 4, (uint32_t)tv.tv_usec); }
      ret = 0;
      break;
    }
    case 158:   // sched_yield
      ret = sched_yield();
      break;
    case 162: {   // nanosleep
      struct timespec ts = {(time_t)(int32_t)Ld32(r[0]), (long)(int32_t)Ld32(r[0] + 4)}, rem;
      ret = nanosleep(&ts, &rem) == 0 ? 0 : -errno;
      break;
    }
    case 263: {   // clock_gettime
      struct timespec ts;
      // (the raw, coarse and boot-time clocks: the host's nearest, as
      // ndk_bridge.h's HostClockId)
      int c = (int)r[0];
      c = (c == 4 || c == 6 || c == 7 || c == 9) ? 1 : (c == 5 || c == 8 || c == 11) ? 0 : c;
      if (clock_gettime((clockid_t)c, &ts) != 0) { ret = -errno; break; }
      St32(r[1], (uint32_t)ts.tv_sec);
      St32(r[1] + 4, (uint32_t)ts.tv_nsec);
      ret = 0;
      break;
    }
    case 0xf0002:   // ARM cacheflush(start, end, flags)
      ndk_flush_code(r[0], r[1] - r[0]);
      ret = 0;
      break;
    case 0xf0005:   // ARM set_tls
      c->tls = r[0];
      ret = 0;
      break;
    default: {
      static std::atomic<uint32_t> reported[64];
      uint32_t n = r[7];
      bool first = true;
      for (auto& slot : reported) {
        uint32_t v = slot.load();
        if (v == n + 1) { first = false; break; }
        if (v == 0 && slot.compare_exchange_strong(v, n + 1)) break;
      }
      if (first) {
        uint32_t pc = r[15] - (c->thumb ? 4 : 8), off = 0;
        const char* sym = Symbolize(pc, &off);
        fprintf(stderr, "ndk_cpu: unsupported syscall %u (svc #%u) at pc=%08x %s+0x%x, returning -ENOSYS\n",
                n, imm, pc, sym ? sym : "?", off);
      }
      ret = -38;   // ENOSYS
    }
  }
  r[0] = (uint32_t)ret;
}

// Guarded guest calls (ndk_call_guest_guarded) of this thread, innermost first.
struct Guard {
  jmp_buf jb;
  Guard* outer;
  char why[256];
};
thread_local Guard* t_guard;

void Fault(Cpu* c, uint32_t pc, const char* fmt, ...) {
  char msg[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  c->r[15] = pc;
  c->cpsr = PackCpsr(c);
  if (g_fault_handler) g_fault_handler(c, pc, msg);
  uint32_t off = 0;
  const char* sym = Symbolize(pc, &off);
  fprintf(stderr, "ndk_cpu: %s at pc=%08x (%s) %s+0x%x\n", msg, pc, c->thumb ? "Thumb" : "ARM", sym ? sym : "?", off);
  for (int i = 0; i < 16; i += 4)
    fprintf(stderr, "  r%-2d %08x  r%-2d %08x  r%-2d %08x  r%-2d %08x\n", i, c->r[i], i + 1, c->r[i + 1], i + 2,
            c->r[i + 2], i + 3, c->r[i + 3]);
  uint32_t lr_off = 0;
  const char* lr_sym = Symbolize(c->r[14] & ~1u, &lr_off);
  fprintf(stderr, "  cpsr %08x  lr = %s+0x%x\n", c->cpsr, lr_sym ? lr_sym : "?", lr_off);
  fflush(stderr);
  ndk_leave_guest(msg);
  abort();
}

}  // namespace ndkcpu

using namespace ndkcpu;

extern "C" {

uint32_t ndk_host_stub(NdkHostFn fn, void* user, const char* name) {
  std::lock_guard<std::mutex> lock(g_mu);
  EnsureStubAreaLocked();
  uint32_t i = g_stub_count.load(std::memory_order_relaxed);
  if (i >= kMaxStubs) { fprintf(stderr, "ndk_cpu: too many host stubs\n"); abort(); }
  g_stubs[i].fn = fn;
  g_stubs[i].user = user;
  g_stubs[i].name = name;
  g_stub_count.store(i + 1, std::memory_order_release);
  return g_stub_base + 4 * i;
}

uint64_t ndk_call_guest(uint32_t addr, const uint32_t* args, int nargs) {
  Cpu* c = CurrentCpu();
  // The outermost call of this host thread holds the CPU's gate (a signal
  // proxy may be using the CPU: signal.cc) for as long as it touches it.
  const bool outer = GuestNesting()++ == 0;
  if (outer) GateEnter(c);
  uint32_t ret_addr = ReturnStubAddr();
  // Everything but r0/r1 is restored afterwards: a host stub calling back
  // into the guest still finds its own arguments, lr and sp.
  uint32_t saved[16];
  memcpy(saved, c->r, sizeof(saved));
  uint32_t sfn = c->fn, sfz = c->fz, sfc = c->fc, sfv = c->fv, sq = c->q, sge = c->ge;
  bool sthumb = c->thumb;
  bool sex = c->ex_valid;
  uint32_t sp = c->r[13];
  int nstack = nargs > 4 ? nargs - 4 : 0;
  sp = (sp - 4 * nstack) & ~7u;
  for (int i = 4; i < nargs; i++) St32(sp + 4 * (i - 4), args[i]);
  for (int i = 0; i < 4; i++) c->r[i] = i < nargs ? args[i] : 0;
  c->r[13] = sp;
  c->r[14] = ret_addr;
  c->thumb = addr & 1;
  c->r[15] = addr & ~1u;
  c->ex_valid = false;
  Run(c);
  uint64_t result = c->r[0] | (uint64_t)c->r[1] << 32;
  memcpy(c->r, saved, sizeof(saved));
  c->fn = sfn; c->fz = sfz; c->fc = sfc; c->fv = sfv; c->q = sq; c->ge = sge;
  c->thumb = sthumb;
  c->ex_valid = sex;
  c->cpsr = PackCpsr(c);
  if (outer) GateLeave(c);
  GuestNesting()--;
  return result;
}

int ndk_call_guest_guarded(uint32_t addr, const uint32_t* args, int nargs, uint64_t* result,
                           char* why, size_t why_size) {
  Cpu* c = CurrentCpu();
  const int snest = GuestNesting()++;
  if (snest == 0) GateEnter(c);
  // What ndk_call_guest restores when the call returns, for when it does not.
  uint32_t saved[16];
  memcpy(saved, c->r, sizeof(saved));
  uint32_t sfn = c->fn, sfz = c->fz, sfc = c->fc, sfv = c->fv, sq = c->q, sge = c->ge;
  bool sthumb = c->thumb;
  int sdepth = c->depth;
  Guard g;
  g.outer = t_guard;
  g.why[0] = '\0';
  t_guard = &g;
  if (setjmp(g.jb) == 0) {
    *result = ndk_call_guest(addr, args, nargs);
    t_guard = g.outer;
    if (snest == 0) GateLeave(c);
    GuestNesting() = snest;
    return 1;
  }
  t_guard = g.outer;
  memcpy(c->r, saved, sizeof(saved));
  c->fn = sfn; c->fz = sfz; c->fc = sfc; c->fv = sfv; c->q = sq; c->ge = sge;
  c->thumb = sthumb;
  c->ex_valid = false;
  c->depth = sdepth;
  c->cpsr = PackCpsr(c);
  if (snest == 0) GateLeave(c);
  GuestNesting() = snest;
  snprintf(why, why_size, "%s", g.why);
  *result = 0;
  return 0;
}

void ndk_leave_guest(const char* why) {
  Guard* g = t_guard;
  if (!g) return;
  snprintf(g->why, sizeof(g->why), "%s", why);
  longjmp(g->jb, 1);
}

NdkCpuState* ndk_current_state(void) {
  return CurrentCpu();
}

// Retires the blocks of [start, start + size) whose code changed
// (ndk_code_index.h): other threads stop entering them at their next block
// boundary (L1 caches and chains check |valid|); a thread inside one
// finishes it, as a core runs on until a context synchronization.
void ndk_flush_code(uint32_t start, size_t size) {
  uint64_t end64 = static_cast<uint64_t>(start) + size;
  uint32_t end = end64 > 0xffffffffull ? 0xffffffffu : static_cast<uint32_t>(end64);
  std::vector<Block*> hit;
  std::lock_guard<std::mutex> lock(g_mu);
  if (g_blocks.empty()) return;   // (the other ABI's CPU: the bridge flushes both)
  const uint64_t calls = g_flush_calls.fetch_add(1, std::memory_order_relaxed) + 1;
  // Code generated at run time shows as many flushes (a JIT: topic
  // guest-jit); the log says so now and then.
  if (calls >= 1024 && (calls & (calls - 1)) == 0)
    fprintf(stderr, "%s: %llu code flushes, %llu blocks retired, %llu kept\n", "ndk_cpu",
            (unsigned long long)calls, (unsigned long long)g_blocks_flushed.load(),
            (unsigned long long)g_blocks_kept.load());
  g_pages.Overlapping(start, end, &hit);
  // (Past |changed|: blocks of the retired ones' regions, retired with them:
  // their translation has the changed code inside.)
  const size_t changed = hit.size();
  for (size_t i = 0; i < hit.size(); i++) {
    Block* b = hit[i];
    if (!__atomic_load_n(&b->valid, __ATOMIC_RELAXED)) continue;
    if (i < changed && !CodeChanged(b)) {
      g_blocks_kept.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    __atomic_store_n(&b->valid, false, __ATOMIC_RELAXED);
    g_blocks.erase(b->start | (b->thumb ? 1 : 0));
    g_pages.Remove(b);
    g_retired.push_back(b);
    JitForget(b, &hit);
    g_blocks_flushed.fetch_add(1, std::memory_order_relaxed);
  }
}

void ndk_set_symbolizer(NdkSymbolizer fn) { g_symbolizer = fn; }
void ndk_set_syscall_handler(NdkSyscallHandler fn) { g_syscall_handler = fn; }
void ndk_set_fault_handler(NdkFaultHandler fn) { g_fault_handler = fn; }
void ndk_set_guest_stack_size(size_t bytes) { g_stack_size = (bytes + 15) & ~(size_t)15; }
void ndk_set_thread_guest_stack_size(size_t bytes) { tl_stack_size = (bytes + 15) & ~(size_t)15; }
void ndk_guest_stack(uint32_t* base, uint32_t* size) {
  Cpu* c = CurrentCpu();
  *base = c->stack_base;
  *size = c->stack_size;
}

uint64_t ndk_instruction_count(void) { return CurrentCpu()->icount; }

// --- green threads (ndk_cpu_ext.h; the scheduler is shim/ndk/ndk_green.cc) -------------

NdkCpuState* ndk_cpu_state_new(size_t stack_size, int tid, uint32_t self, void* owner) {
  JitApplyBudget();
  Cpu* c = static_cast<Cpu*>(calloc(1, sizeof(Cpu)));
  stack_size = stack_size ? (stack_size + 15) & ~(size_t)15 : g_stack_size;
  void* stack = aligned_alloc(16, stack_size);
  if (!c || !stack) { fprintf(stderr, "ndk_cpu: out of memory for a guest thread\n"); abort(); }
  c->stack_base = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(stack));
  c->stack_size = static_cast<uint32_t>(stack_size);
  c->r[13] = c->stack_base + c->stack_size;
  c->fz = 1;
  c->cpsr = PackCpsr(c);
  c->l1 = new Cpu::L1[kL1Size];
  for (int i = 0; i < kL1Size; i++) c->l1[i] = {0xffffffffu, nullptr};
  c->epoch = g_epoch.load();
  c->tid = tid;
  c->pid = getpid();
  c->self = self;
  c->green = true;
  c->green_owner = owner;
  c->gate.store(3);   // parked until a carrier enters it
  std::lock_guard<std::mutex> lock(CpusLock());
  Cpus().insert(c);
  return c;
}

void ndk_cpu_state_free(NdkCpuState* s) {
  Cpu* c = static_cast<Cpu*>(s);
  {
    std::lock_guard<std::mutex> lock(CpusLock());
    Cpus().erase(c);
  }
  free(HostPtr(c->stack_base));
  delete[] c->l1;
  free(c);
}

void* ndk_cpu_state_owner(const NdkCpuState* s) { return static_cast<const Cpu*>(s)->green_owner; }

void ndk_cpu_green_start(NdkCpuState* s, uint32_t fn, uint32_t arg) {
  Cpu* c = static_cast<Cpu*>(s);
  c->r[0] = arg;
  c->r[13] = c->stack_base + c->stack_size;
  c->r[14] = ReturnStubAddr();
  c->thumb = fn & 1;
  c->r[15] = fn & ~1u;
  c->cpsr = PackCpsr(c);
}

namespace {
// A carrier's translations: instantiated in its wasm table, so every green
// thread it runs uses them (as a signal proxy does, signal.cc).
struct CarrierJit {
  ndkjit::ThreadJit jit{};
  ~CarrierJit() { ndkjit::Free(&jit); }
};
thread_local CarrierJit tl_carrier_jit;
thread_local Cpu* tl_carrier_prev;
}  // namespace

void ndk_cpu_green_enter(NdkCpuState* s) {
  Cpu* c = static_cast<Cpu*>(s);
  tl_carrier_prev = SetCurrentCpu(c);
  GuestNesting()++;   // nested ndk_call_guest of its host calls: not outermost
  c->gate.store(1);
  c->jit = tl_carrier_jit.jit;
  c->yield_req.store(0);
  c->park = kNdkParkNone;
}

int ndk_cpu_green_run(NdkCpuState* s) {
  Cpu* c = static_cast<Cpu*>(s);
  c->park = kNdkParkNone;
  Run(c);
  const int why = c->park;
  c->park = kNdkParkNone;
  return why;
}

int ndk_cpu_green_leave(NdkCpuState* s, int parked) {
  Cpu* c = static_cast<Cpu*>(s);
  tl_carrier_jit.jit = c->jit;
  memset(&c->jit, 0, sizeof(c->jit));
  c->cpsr = PackCpsr(c);
  GuestNesting()--;
  SetCurrentCpu(tl_carrier_prev);
  tl_carrier_prev = nullptr;
  c->gate.store(parked ? 3 : 1);
  // (A signal posted while the gate was 1 only set the attention flag.)
  return parked && c->sig_attention.load() != 0 && GreenSignalDeliverable(c);
}

uint32_t ndk_cpu_carrier_translations(void) { return tl_carrier_jit.jit.functions; }

void ndk_cpu_green_yield(NdkCpuState* s) {
  Cpu* c = static_cast<Cpu*>(s);
  c->yield_req.store(1);
  c->sig_attention.store(1);
}

NdkResume* ndk_cpu_parked_record(NdkCpuState* s) {
  Cpu* c = static_cast<Cpu*>(s);
  return c->nresumes ? &c->resumes[c->nresumes - 1].rec : nullptr;
}

int ndk_cpu_can_park(void) {
  Cpu* c = CurrentCpuIfAny();
  return c && c->green && c->depth == 1 && c->in_host != 0 && c->park == kNdkParkNone &&
         GuestNesting() == 1;
}

int ndk_cpu_park(const NdkResume* rec) {
  if (!ndk_cpu_can_park()) return -1;
  Cpu* c = CurrentCpuIfAny();
  const uint32_t sp = c->r[13];
  // Records of frames that are gone (a handler that longjmp'd out of a
  // parked call's handler) sit at or below this sp: dropped.
  while (c->nresumes && c->resumes[c->nresumes - 1].sp <= sp) c->nresumes--;
  if (c->nresumes == Cpu::kMaxResumes) return -1;
  c->resumes[c->nresumes++] = {c->in_host - 1, sp, *rec};
  c->park = kNdkParkCall;
  return 0;
}

NdkResume* ndk_cpu_resumed(void) {
  Cpu* c = CurrentCpuIfAny();
  return c ? c->resumed : nullptr;
}

// Whether the calling host thread has a guest context: a Java thread with one
// keeps its worker (topic java-pool-park, shim/art10/dormant_wasm10.cc).
int ndk_cpu_thread_has_state(void) { return CurrentCpuIfAny() != nullptr; }

void ndk_cpu_thread_exit(void) {
  Cpu* c = tl_cpu.cpu;
  if (!c) return;
  c->depth = 0;
  c->in_host = 0;
  GuestNesting() = 0;
  t_guard = nullptr;
  c->gate.store(0);
#ifdef __EMSCRIPTEN__
  emscripten_futex_wake(&c->gate, 0x7fffffff);
#endif
}

int ndk_cpu_green_end(void) {
  if (!ndk_cpu_can_park()) return -1;
  CurrentCpuIfAny()->park = kNdkParkExit;
  return 0;
}

// A guest address that holds a short printable C string (trap reports).
static bool MaybeString(uint32_t a) {
  if (a < 0x10000 || a >= emscripten_get_heap_size() - 64)
    return false;
  const char* p = static_cast<const char*>(HostPtr(a));
  for (int i = 0; i < 40 && p[i]; ++i)
    if ((unsigned char)p[i] < 0x20 || (unsigned char)p[i] > 0x7e) return false;
  return p[0] != 0;
}

static void ReportRecentHostCalls(Cpu* c, uint32_t n);

// Called from JS after a wasm trap (an out-of-bounds guest access, say) on
// a thread that runs guest code (shim/app6/guest_trap_report.js): which
// block it was in, the registers. Nothing if the thread has no CPU.
EMSCRIPTEN_KEEPALIVE void ndk_cpu_report_trap(void) {
  Cpu* c = tl_cpu.cpu;
  if (c == nullptr || c->depth == 0) {
    fprintf(stderr, "ndk_cpu: wasm trap outside guest code (cpu %p)\n", static_cast<void*>(c));
    return;
  }
  // Registers first: symbols take the linker's lock, which the trapped
  // thread may hold (a library's constructors).
  fprintf(stderr, "ndk_cpu: wasm trap in guest code, in the block at pc=%08x (%s), insn %08x\n",
          c->block_start, c->thumb ? "Thumb" : "ARM", c->cur_insn_addr);
  for (int i = 0; i < 16; i += 4)
    fprintf(stderr, "  r%-2d %08x  r%-2d %08x  r%-2d %08x  r%-2d %08x\n", i, c->r[i], i + 1, c->r[i + 1],
            i + 2, c->r[i + 2], i + 3, c->r[i + 3]);
  fflush(stderr);
  uint32_t off = 0;
  const char* sym = Symbolize(c->block_start, &off);
  uint32_t lr_off = 0;
  const char* lr_sym = Symbolize(c->r[14] & ~1u, &lr_off);
  fprintf(stderr, "  block = %s+0x%x, lr = %s+0x%x\n", sym ? sym : "?", off, lr_sym ? lr_sym : "?", lr_off);
  ReportRecentHostCalls(c, 16);
}

// The same without stdio, for the JS side of the report (a thread that
// trapped may not get its writes out, Node): 0 outside guest code, else
// words: block start, current instruction, Thumb, r0..r15.
EMSCRIPTEN_KEEPALIVE const uint32_t* ndk_cpu_trap_regs(void) {
  static thread_local uint32_t w[19];
  Cpu* c = tl_cpu.cpu;
  if (c == nullptr || c->depth == 0) return nullptr;
  w[0] = c->block_start;
  w[1] = c->cur_insn_addr;
  w[2] = c->thumb;
  for (int i = 0; i < 16; i++) w[3 + i] = c->r[i];
  return w;
}

// Every thread that ran guest code: where it is (racy reads of the other
// threads' state: for hangs, called from the page, e.g. demo/senses-apps-vm
// when the screen stops; any thread). Guest pc and lr with symbols, the
// host function it is in, its last host calls.
extern "C" void ndk_green_dump(void) __attribute__((weak));   // shim/ndk/ndk_green.cc

// For measurements (the page): ARM translations instantiated by threads
// with a CPU of their own (green threads use their carriers': not counted).
EMSCRIPTEN_KEEPALIVE int ndk_cpu_thread_translations(void) {
  std::lock_guard<std::mutex> lock(CpusLock());
  int n = 0;
  for (Cpu* c : Cpus()) if (!c->green) n += c->jit.functions;
  return n;
}

extern "C" void ndk64_cpu_dump_threads(void) __attribute__((weak));   // the AArch64 CPU's

EMSCRIPTEN_KEEPALIVE void ndk_cpu_dump_threads(void) {
  if (ndk64_cpu_dump_threads) ndk64_cpu_dump_threads();
  if (ndk_green_dump) ndk_green_dump();
  std::lock_guard<std::mutex> lock(CpusLock());
  fprintf(stderr, "ndk_cpu: %zu threads with guest code\n", Cpus().size());
  for (Cpu* c : Cpus()) {
    uint32_t off = 0, lr_off = 0;
    const uint32_t pc = c->depth ? c->cur_insn_addr : 0;
    const char* sym = pc ? Symbolize(pc, &off) : nullptr;
    const char* lr_sym = c->depth ? Symbolize(c->r[14] & ~1u, &lr_off) : nullptr;
    const uint32_t in = c->in_host;
    const char* host = in && in - 1 < g_stub_count.load() ? g_stubs[in - 1].name : nullptr;
    // A parked green thread: the host function it waits in (its record's).
    const char* where = c->depth ? "in guest code" : "outside guest code";
    if (c->green && c->gate.load() == 3) {
      where = "green, parked";
      if (c->nresumes) {
        const uint32_t i = c->resumes[c->nresumes - 1].index;
        host = i < g_stub_count.load() ? g_stubs[i].name : nullptr;
      }
      lr_sym = Symbolize(c->r[14] & ~1u, &lr_off);
    } else if (c->green) {
      where = c->depth ? "green, in guest code" : "green, runnable";
    }
    // (Another thread's counters: read as they are.)
    fprintf(stderr, "  tid %d (pid %d): %s pc=%08x %s+0x%x lr=%s+0x%x%s%s, %llu insns, %u translations in %u modules\n",
            c->tid, c->pid, where, pc, sym ? sym : "?", off,
            lr_sym ? lr_sym : "?", lr_off, host ? ", in " : "", host ? host : "",
            static_cast<unsigned long long>(c->icount), c->jit.functions, c->jit.npacks);
    for (uint32_t i = Cpu::kRecent - 4; i < Cpu::kRecent; ++i) {
      const Cpu::RecentCall& rc = c->recent[(c->recent_next + i) & (Cpu::kRecent - 1)];
      if (rc.index == 0 && rc.r0 == 0 && rc.ret == 0) continue;
      const char* name = rc.index < g_stub_count.load() ? g_stubs[rc.index].name : "?";
      fprintf(stderr, "      %s(%08x, %08x, %08x) = %08x\n", name ? name : "?", rc.r0, rc.r1, rc.r2, rc.ret);
    }
    // Return addresses on its stack (words that fall in loaded code: a
    // heuristic backtrace, innermost first), when it is in guest code.
    if (c->depth && c->r[13] >= c->stack_base && c->r[13] < c->stack_base + c->stack_size) {
      int found = 0;
      for (uint32_t a = c->r[13]; a + 4 <= c->stack_base + c->stack_size && a < c->r[13] + 8192 && found < 24;
           a += 4) {
        const uint32_t v = *static_cast<const uint32_t*>(HostPtr(a));
        if (v < 0x10000 || (g_stub_base && v >= g_stub_base && v < g_stub_base + kMaxStubs * 4)) continue;
        uint32_t voff = 0;
        const char* vsym = g_symbolizer ? g_symbolizer(v & ~1u, &voff) : nullptr;
        if (!vsym) continue;
        fprintf(stderr, "      sp+%03x: %s+0x%x\n", a - c->r[13], vsym, voff);
        ++found;
      }
    }
  }
  fflush(stderr);
}

// The last |n| host calls of the calling thread (at most Cpu::kRecent).
static void ReportRecentHostCalls(Cpu* c, uint32_t n) {
  fprintf(stderr, "  last host calls of this thread (oldest first):\n");
  for (uint32_t i = Cpu::kRecent - n; i < Cpu::kRecent; ++i) {
    const Cpu::RecentCall& rc = c->recent[(c->recent_next + i) & (Cpu::kRecent - 1)];
    if (rc.index == 0 && rc.r0 == 0 && rc.ret == 0)
      continue;
    const char* name = rc.index < g_stub_count.load() ? g_stubs[rc.index].name : "?";
    char arg0[48] = "";
    if (MaybeString(rc.r0)) snprintf(arg0, sizeof(arg0), " \"%.40s\"", static_cast<const char*>(HostPtr(rc.r0)));
    fprintf(stderr, "    %s(%08x%s, %08x, %08x) = %08x\n", name ? name : "?", rc.r0, arg0, rc.r1, rc.r2,
            rc.ret);
  }
  fflush(stderr);
}

void ndk_cpu_set_block_hook(NdkBlockHook hook) { ndkcpu::g_block_hook.store(hook); }

void ndk_cpu_watch_host_calls(const char* words) {
  if (!words || !*words || g_watch_on.load())
    return;
  g_watch = words;
  g_watch_state = new std::atomic<uint32_t>[kMaxStubs]();
  g_watch_on.store(true);
}

void ndk_cpu_watch_strings(const char* text) {
  if (!text || !*text || g_watch_str_on.load())
    return;
  g_watch_str = text;
  g_watch_str_on.store(true);
}

// Debugging (shim/ndk ReportGuestStack): the calling thread's last host calls.
void ndk_cpu_report_recent_calls(void) {
  if (Cpu* c = tl_cpu.cpu)
    ReportRecentHostCalls(c, Cpu::kRecent);
}

void ndk_cpu_stats(NdkCpuStats* out) {
  out->blocks_decoded = g_blocks_decoded.load();
  out->insns_decoded = g_insns_decoded.load();
  out->blocks_flushed = g_blocks_flushed.load();
  out->host_calls = g_host_calls.load();
  JitStats(&out->blocks_translated, &out->translated_bytes, &out->blocks_not_translatable);
  out->flush_calls = g_flush_calls.load();
  out->blocks_kept = g_blocks_kept.load();
}

const char* ndk_host_stub_name(uint32_t addr) {
  if (!g_stub_base || addr < g_stub_base || addr >= g_stub_base + kMaxStubs * 4 || (addr & 3)) return nullptr;
  uint32_t i = (addr - g_stub_base) >> 2;
  if (i == 0 || i >= g_stub_count.load()) return nullptr;
  return g_stubs[i].name;
}

}  // extern "C"

// The memory budget (shim/membudget/mem_budget.h): what the interpreter's
// decoded blocks and the translations to wasm hold.
extern "C" void ndk_cpu_mem_stats(uint64_t* block_bytes, uint64_t* jit_bytes) {
  uint64_t blocks = 0, bytes = 0, rejected = 0;
  ndkcpu::JitStats(&blocks, &bytes, &rejected);
  *block_bytes = ndkcpu::g_block_bytes.load();
  *jit_bytes = bytes;
}

// A memory trim: from TRIM_MEMORY_RUNNING_CRITICAL no new block is
// translated to wasm (each translation keeps its module's bytes, and every
// thread compiles its own instance outside the wasm memory); the ones
// translated stay in use. Level 0 (the pressure is over) resumes it.
// Decoded blocks are not freed: a thread may be running any of them and
// blocks chain to each other without a lock (see Block::next).
extern "C" int64_t ndk_cpu_mem_trim(int level) {
  ndkcpu::JitPause(level >= 15);
  return 0;
}
