// Atomics on AArch64: __sync and __atomic builtins of every size (LDXR/STXR
// loops, or LSE instructions in the "lse" variant), 128-bit
// compare-and-swap (LDXP/STXP or CASP), the thread pointer (TPIDR_EL0), and
// a worker the runner calls from several threads at once.
#include "guest.h"

static volatile int counter;

int atomic_worker(volatile int* p, int n) {
  for (int i = 0; i < n; i++) {
    __sync_fetch_and_add(p, 1);
    if ((i & 15) == 0) {
      int old;
      do { old = *p; } while (__sync_val_compare_and_swap(p, old, old + 2) != old);
      __sync_fetch_and_sub(p, 2);
    }
  }
  return 0;
}

static uint64_t read_tpidr(void) {
  uint64_t v;
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(v));
  return v;
}

void test_atomic(void) {
  counter = 5;
  CHECK_EQ(__sync_fetch_and_add(&counter, 3), 5);
  CHECK_EQ(counter, 8);
  CHECK_EQ(__sync_add_and_fetch(&counter, 2), 10);
  CHECK_EQ(__sync_fetch_and_sub(&counter, 4), 10);
  CHECK_EQ(__sync_fetch_and_or(&counter, 0x100), 6);
  CHECK_EQ(__sync_fetch_and_and(&counter, 0xff), 0x106);
  CHECK_EQ(__sync_fetch_and_xor(&counter, 0xf), 6);
  CHECK_EQ(counter, 9);
  CHECK_EQ(__sync_val_compare_and_swap(&counter, 9, 77), 9);
  CHECK_EQ(__sync_val_compare_and_swap(&counter, 9, 88), 77);
  CHECK_EQ(counter, 77);
  CHECK(__sync_bool_compare_and_swap(&counter, 77, 1));
  CHECK_EQ(__sync_lock_test_and_set(&counter, 3), 1);
  __sync_synchronize();
  CHECK_EQ(counter, 3);

  static volatile uint64_t wide = 0xfffffffffull;
  CHECK_EQ(__sync_fetch_and_add(&wide, 1), 0xfffffffffull);
  CHECK_EQ(wide, 0x1000000000ull);
  CHECK_EQ(__sync_val_compare_and_swap(&wide, 0x1000000000ull, 0x123456789abcull), 0x1000000000ull);
  CHECK_EQ(wide, 0x123456789abcull);
  static volatile uint8_t b = 250;
  static volatile uint16_t h = 65530;
  CHECK_EQ(__sync_fetch_and_add(&b, 10), 250);
  CHECK_EQ(b, 4);
  CHECK_EQ(__sync_fetch_and_add(&h, 10), 65530);
  CHECK_EQ(h, 4);
  static volatile int64_t s64 = -5;
  CHECK_EQ(__atomic_fetch_max(&s64, 7, __ATOMIC_SEQ_CST), -5);
  CHECK_EQ(__atomic_fetch_min(&s64, -100, __ATOMIC_SEQ_CST), 7);
  CHECK_EQ(s64, -100);
  static volatile uint32_t u32 = 0x80000000u;
  CHECK_EQ(__atomic_fetch_max(&u32, 5u, __ATOMIC_ACQ_REL), 0x80000000u);
  CHECK_EQ(__atomic_exchange_n(&u32, 42u, __ATOMIC_SEQ_CST), 0x80000000u);
  CHECK_EQ(__atomic_load_n(&u32, __ATOMIC_ACQUIRE), 42u);
  __atomic_store_n(&u32, 43u, __ATOMIC_RELEASE);
  CHECK_EQ(u32, 43u);

  // 128 bits: LDXP/STXP (or CASP with LSE).
  static unsigned __int128 q __attribute__((aligned(16)));
  q = ((unsigned __int128)0x1111222233334444ull << 64) | 0x5555666677778888ull;
  unsigned __int128 expected = q;
  unsigned __int128 desired = ((unsigned __int128)0xaaaabbbbccccddddull << 64) | 0x0123456789abcdefull;
  CHECK(__atomic_compare_exchange_n(&q, &expected, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
  CHECK_EQ((uint64_t)(q >> 64), 0xaaaabbbbccccddddull);
  CHECK_EQ((uint64_t)q, 0x0123456789abcdefull);
  expected = 0;
  CHECK(!__atomic_compare_exchange_n(&q, &expected, (unsigned __int128)1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
  CHECK_EQ((uint64_t)expected, 0x0123456789abcdefull);

  host.set_tls(0x12345678);
  CHECK_EQ(read_tpidr(), 0x12345678);
  __asm__ volatile("msr tpidr_el0, %0" : : "r"((uint64_t)0x87654321));
  CHECK_EQ(read_tpidr(), 0x87654321);
  host.set_tls(0);
  volatile int local = 0;
  atomic_worker(&local, 100);
  CHECK_EQ(local, 100);
}

// Spins on a flag another thread sets (runner TestSpin).
int spin_wait(volatile int* flag) {
  int n = 0;
  while (!*flag) n++;
  return n;
}
