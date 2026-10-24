// Atomics: __sync builtins (kuser cmpxchg on armv5te, LDREX/STREX + DMB
// on armv7-a), the kuser TLS helper and MRC p15 TPIDRURO, and a worker
// the runner calls from several threads at once.
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

uint32_t read_tls_kuser(void) { return (uint32_t)(uintptr_t)kuser_get_tls(); }

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

  CHECK_EQ(kuser_cmpxchg(3, 4, &counter), 0);
  CHECK_EQ(counter, 4);
  CHECK(kuser_cmpxchg(3, 5, &counter) != 0);
  CHECK_EQ(counter, 4);
  kuser_dmb();

  host.set_tls(0x12345678);
  CHECK_EQ(read_tls_kuser(), 0x12345678);
#if __ARM_ARCH >= 7
  uint32_t tls;
  __asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(tls));
  CHECK_EQ(tls, 0x12345678);
  {
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
  }
#endif
  host.set_tls(0);
  volatile int local = 0;
  atomic_worker(&local, 100);
  CHECK_EQ(local, 100);
}

// Spins on a flag another thread sets (runner TestThreads): a loop with
// only a load, which translated code must not hoist out of the loop.
int spin_wait(volatile int* flag) {
  int n = 0;
  while (!*flag) n++;
  return n;
}
