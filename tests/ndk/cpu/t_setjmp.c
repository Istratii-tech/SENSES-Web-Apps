// setjmp/longjmp out of deep recursion and running code on another stack.
#include "guest.h"

#define NOINLINE __attribute__((noinline))

static guest_jmp_buf env;
static int depth_reached;

static NOINLINE int dive(int n) {
  volatile char pad[64];
  pad[0] = (char)n;
  depth_reached = n;
  if (n == 500) guest_longjmp(env, 42);
  return dive(n + 1) + pad[0];
}

static NOINLINE uint32_t on_other_stack(uint32_t n) {
  volatile uint32_t local = n;
  uint32_t sp = (uint32_t)(uintptr_t)&local;
  // Recurse a bit on the new stack.
  if (n > 0) return on_other_stack(n - 1) + 1;
  return sp;
}

static uint32_t stack_area[4096] __attribute__((aligned(16)));

void test_setjmp(void) {
  volatile int count = 0;
  int r = guest_setjmp(env);
  count++;
  if (r == 0) {
    dive(0);
    CHECK(0);   // not reached
  }
  CHECK_EQ(r, 42);
  CHECK_EQ(count, 2);
  CHECK_EQ(depth_reached, 500);
  r = guest_setjmp(env);
  if (r == 0) guest_longjmp(env, 0);   // longjmp(0) returns 1
  CHECK_EQ(r, 1);

  uint32_t lo = (uint32_t)(uintptr_t)stack_area, hi = lo + sizeof(stack_area);
  uint32_t res = guest_on_stack(on_other_stack, 10, (char*)stack_area + sizeof(stack_area));
  uint32_t sp = res - 10;
  CHECK(sp >= lo && sp < hi);
  volatile int here;
  uint32_t mysp = (uint32_t)(uintptr_t)&here;
  CHECK(mysp < lo || mysp >= hi);
}
