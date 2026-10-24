// Signals to guest threads (shim/ndk/cpu/signal.cc). test_signal: on one
// thread (a signal to itself, the mask, SIGILL from an undefined
// instruction). sig_worker / sig_outside_tick / sig_world_test: the
// runner's TestSignals starts worker threads and stops and restarts them the way Boehm's GC (il2cpp's)
// stops the world: pthread_kill(SIG_SUSPEND), the handler acknowledges and
// waits in sigsuspend for SIG_RESTART.
#include "guest.h"

#define SIGILL_ 4
#define SIGUSR1_ 10
#define SIG_RESTART 24   // SIGXCPU, as Boehm's SIG_THR_RESTART
#define SIG_SUSPEND 30   // SIGPWR, as Boehm's SIG_SUSPEND
#define SA_SIGINFO_ 4
#define SA_RESTART_ 0x10000000
#define BIT(sig) (1u << ((sig) - 1))

// Linux ARM ucontext (as the CPU builds it).
struct gsigcontext {
  uint32_t trap_no, error_code, oldmask;
  uint32_t r[16];
  uint32_t cpsr, fault_address;
};
struct gucontext {
  uint32_t flags, link, ss_sp, ss_flags, ss_size;
  struct gsigcontext mc;
  uint32_t sigmask[2];
};
struct gsiginfo {
  int32_t signo, err, code;
  uint32_t addr;
};

static inline void inc(volatile int* p) { __sync_fetch_and_add(p, 1); }

// --- on one thread -----------------------------------------------------------------

static volatile int g_usr1_hits, g_usr1_bad;
static void usr1_handler(int sig, struct gsiginfo* info, struct gucontext* uc) {
  if (sig != SIGUSR1_ || info->signo != SIGUSR1_ || info->code != -6 /* SI_TKILL */) g_usr1_bad++;
  // While it runs, the signal itself is blocked.
  if (!(uc->sigmask[0] == 0 || uc->sigmask[0] == BIT(SIGILL_))) g_usr1_bad++;
  uint32_t mask = host.sigmask(0, 0);
  if (!(mask & BIT(SIGUSR1_))) g_usr1_bad++;
  g_usr1_hits++;
}

static volatile int g_ill;
static volatile uint32_t g_ill_addr;
// Skips the undefined instruction through the context (rt_sigreturn).
static void ill_skip_handler(int sig, struct gsiginfo* info, struct gucontext* uc) {
  (void)sig;
  g_ill++;
  g_ill_addr = info->addr;
  uc->mc.r[15] += (uc->mc.cpsr & 0x20) ? 2 : 4;
}
static guest_jmp_buf g_ill_jb;
// OpenSSL's armcap probe: siglongjmp out of the handler.
static void ill_jump_handler(int sig) {
  (void)sig;
  g_ill++;
  guest_longjmp(g_ill_jb, 1);
}

static void __attribute__((noinline)) undefined_insn(void) {
#ifdef __thumb__
  __asm__ volatile(".inst.n 0xde00" ::: "memory");   // UDF #0 (Thumb)
#else
  __asm__ volatile(".inst 0xe7f000f0" ::: "memory");  // UDF #0 (ARM)
#endif
}

void test_signal(void) {
  int self = host.gettid();
  CHECK_EQ(host.sigaction(SIGUSR1_, (void*)usr1_handler, SA_SIGINFO_, 0), 0);
  // To itself: the handler runs before tkill returns.
  CHECK_EQ(host.tkill(self, SIGUSR1_), 0);
  CHECK_EQ(g_usr1_hits, 1);
  // Blocked: pending until unblocked, then it runs inside sigmask.
  uint32_t old = host.sigmask(0 /* SIG_BLOCK */, BIT(SIGUSR1_));
  CHECK_EQ(host.tkill(self, SIGUSR1_), 0);
  CHECK_EQ(g_usr1_hits, 1);
  host.sigmask(2 /* SIG_SETMASK */, old);
  CHECK_EQ(g_usr1_hits, 2);
  CHECK_EQ(g_usr1_bad, 0);
  CHECK_EQ(host.sigmask(0, 0), old);
  // Ignored and default actions drop the signal.
  CHECK_EQ(host.sigaction(SIGUSR1_, (void*)1 /* SIG_IGN */, 0, 0), 0);
  CHECK_EQ(host.tkill(self, SIGUSR1_), 0);
  CHECK_EQ(g_usr1_hits, 2);
  CHECK_EQ(host.tkill(0x7fffffff, SIGUSR1_), -3);   // ESRCH

  // SIGILL from an undefined instruction, the handler resumes after it.
  CHECK_EQ(host.sigaction(SIGILL_, (void*)ill_skip_handler, SA_SIGINFO_, 0), 0);
  undefined_insn();
  CHECK_EQ(g_ill, 1);
  CHECK(g_ill_addr >= (uint32_t)(uintptr_t)undefined_insn - 1 &&
        g_ill_addr < (uint32_t)(uintptr_t)undefined_insn + 16);
  undefined_insn();
  CHECK_EQ(g_ill, 2);
  // ... and leaves with longjmp (SIGILL stays blocked then: unblock it).
  CHECK_EQ(host.sigaction(SIGILL_, (void*)ill_jump_handler, 0, 0), 0);
  if (guest_setjmp(g_ill_jb) == 0) {
    undefined_insn();
    CHECK(0);   // not reached
  }
  CHECK_EQ(g_ill, 3);
  CHECK(host.sigmask(0, 0) & BIT(SIGILL_));
  host.sigmask(1 /* SIG_UNBLOCK */, BIT(SIGILL_));
  CHECK_EQ(host.sigaction(SIGILL_, (void*)0, 0, 0), 0);
}

// --- stopping the world ---------------------------------------------------------------

#define MAX_WORKERS 4
volatile int sig_tids[MAX_WORKERS];
static volatile int g_stop;
static volatile int g_world_stopped;
static volatile int g_acks, g_restarts, g_suspended, g_bad;
static volatile int g_wait_word;
static volatile uint32_t g_spin_count, g_call_count, g_outside_count;

static void suspend_handler(int sig, struct gsiginfo* info, struct gucontext* uc) {
  // The interrupted registers are on this thread's stack, above us (a
  // conservative GC scans from here up).
  uint32_t here = (uint32_t)(uintptr_t)&here;
  if (sig != SIG_SUSPEND || info->signo != SIG_SUSPEND || uc->mc.r[13] < here ||
      (uint32_t)(uintptr_t)uc < here)
    inc(&g_bad);
  // It runs as the thread it stopped (a proxy too).
  int me = host.gettid(), known = 0;
  for (int i = 0; i < MAX_WORKERS; i++) known |= sig_tids[i] == me;
  if (!known) inc(&g_bad);
  inc(&g_suspended);
  inc(&g_acks);
  host.futex_wake(&g_acks);
  // As Boehm: everything but SIG_RESTART blocked until the world restarts.
  do {
    host.sigsuspend(~BIT(SIG_RESTART));
  } while (g_world_stopped);
  __sync_fetch_and_sub(&g_suspended, 1);
}
static void restart_handler(int sig) {
  if (sig != SIG_RESTART) inc(&g_bad);
  inc(&g_restarts);
}

// kind 0: spins in guest code (an LCG whose result shows that the
// interruptions kept its registers); 1: waits in a host futex wait; 2:
// calls host functions in a loop.
uint32_t sig_worker(int kind, int index) {
  sig_tids[index] = host.gettid();
  if (kind == 0) {
    uint32_t x = 1, n = 0;
    while (!g_stop) {
      x = x * 1664525u + 1013904223u;
      x ^= x >> 7;
      g_spin_count = ++n;
    }
    uint32_t y = 1;
    for (uint32_t i = 0; i < n; i++) {
      y = y * 1664525u + 1013904223u;
      y ^= y >> 7;
    }
    CHECK_EQ(x, y);
    return n;
  }
  if (kind == 1) {
    uint32_t n = 0;
    while (!g_stop) {
      host.futex_wait(&g_wait_word, 0, -1);
      n++;
    }
    return n;
  }
  uint32_t n = 0;
  while (!g_stop) {
    host.now_us();
    g_call_count = ++n;
  }
  return n;
}

// Worker 3: a thread outside guest code most of the time (the runner calls
// this every 50 ms from a host loop until it returns nonzero), stopped by a
// handler a helper thread runs for it.
int sig_outside_tick(void) {
  if (!sig_tids[3]) sig_tids[3] = host.gettid();
  g_outside_count++;
  return g_stop;
}

static int wait_for(volatile int* p, int want, int ms) {
  uint64_t end = host.now_us() + (uint64_t)ms * 1000;
  for (;;) {
    int v = *p;
    if (v == want) return 1;
    if (host.now_us() > end) return 0;
    host.futex_wait(p, v, 10);
  }
}

// Stops and restarts |n| workers |rounds| times; then lets them finish.
void sig_world_test(int n, int rounds) {
  CHECK_EQ(host.sigaction(SIG_SUSPEND, (void*)suspend_handler, SA_SIGINFO_ | SA_RESTART_, BIT(SIG_RESTART)), 0);
  CHECK_EQ(host.sigaction(SIG_RESTART, (void*)restart_handler, SA_RESTART_, 0), 0);
  for (int i = 0; i < n; i++) {
    uint64_t end = host.now_us() + 5000000;
    while (!sig_tids[i] && host.now_us() < end) host.sleep_ms(1);
    CHECK(sig_tids[i] != 0);
  }
  for (int round = 1; round <= rounds; round++) {
    g_world_stopped = 1;
    for (int i = 0; i < n; i++) {
      int r = host.tkill(sig_tids[i], SIG_SUSPEND);
      if (r != 0) printf("round %d: tkill(%d) of worker %d = %d\n", round, sig_tids[i], i, r);
      CHECK_EQ(r, 0);
    }
    if (!wait_for(&g_acks, round * n, 5000)) {
      printf("round %d: %d of %d threads acknowledged\n", round, g_acks - (round - 1) * n, n);
      CHECK(0);
      g_world_stopped = 0;
      for (int i = 0; i < n; i++) host.tkill(sig_tids[i], SIG_RESTART);
      break;
    }
    // Stopped: neither the spinner nor the caller moves.
    uint32_t spin = g_spin_count, calls = g_call_count, outside = g_outside_count;
    host.sleep_ms(n > 3 ? 60 : 3);
    CHECK_EQ(g_spin_count, spin);
    CHECK_EQ(g_call_count, calls);
    CHECK_EQ(g_outside_count, outside);
    g_world_stopped = 0;
    for (int i = 0; i < n; i++) CHECK_EQ(host.tkill(sig_tids[i], SIG_RESTART), 0);
    if (!wait_for(&g_suspended, 0, 5000)) {
      printf("round %d: %d threads did not restart\n", round, g_suspended);
      CHECK(0);
      break;
    }
  }
  CHECK_EQ(g_restarts, rounds * n);
  CHECK_EQ(g_bad, 0);
  CHECK(g_spin_count > 0);
  g_stop = 1;
  g_wait_word = 1;
  host.futex_wake(&g_wait_word);
}
