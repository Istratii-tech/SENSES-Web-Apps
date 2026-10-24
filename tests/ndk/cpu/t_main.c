// Entry points of the guest test program (one ELF per variant: armv5te /
// armv7-a, ARM / Thumb; see mk/topic-arm-cpu.mk).
#include "guest.h"

#define WEAK __attribute__((weak))
WEAK void test_thumb2(void);
WEAK void test_vfp(void);
WEAK void test_neon(void);

static void run(const char* name, void (*fn)(void)) {
  if (!fn) return;
  printf("-- %s\n", name);
  fn();
}

int guest_main(void) {
  run("arith", test_arith);
  run("memory", test_memory);
  run("control", test_control);
  run("setjmp", test_setjmp);
  run("float", test_float);
  run("atomic", test_atomic);
  run("callback", test_callback);
  run("asm", test_asm);
  run("thumb", test_thumb);
  run("thumb2", test_thumb2);
  run("vfp", test_vfp);
  run("neon", test_neon);
  run("signal", test_signal);
  run("jit", test_jit);
  return 0;
}
