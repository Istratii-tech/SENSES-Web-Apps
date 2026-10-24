// Entry point of the AArch64 guest test program (one ELF per variant, see
// mk/topic-ndk-arm64.mk). The portable groups come from tests/ndk/cpu.
#include "guest.h"

static void run(const char* name, void (*fn)(void)) {
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
  run("abi", test_abi);
  run("system", test_system);
  run("vector", test_vector);
  run("jit", test_jit);
  run("diff", test_diff);
  return 0;
}
