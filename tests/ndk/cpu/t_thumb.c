// Thumb-state tests (this file is always built -mthumb: Thumb-1 for the
// armv5te variants, Thumb-2 encodings for armv7-a): 16-bit flag-setting
// arithmetic observed through conditional branches (Thumb-1 has no MRS),
// high-register operations, PUSH/POP and LDM/STM, literal loads, BX PC
// into ARM state and back, and interworking with t_asm_arm.c.
#include "guest.h"

#define NOINLINE __attribute__((noinline))

int arm_side(int x);
int arm_pop_pc(int x);
int arm_ldr_pc(int x);

NOINLINE int thumb_side(int x) { return x * 5 - 2; }
NOINLINE int thumb_calls_arm(int (*fn)(int), int x) { return fn(x) + 200; }

// 1 if condition |cc| holds after "op" (with %1, %2 as low-register inputs).
#define FLAG(op, cc, a, b) ({                                               \
  uint32_t r_;                                                              \
  __asm__ volatile(op "\n b" cc " 1f\n movs %0, #0\n b 2f\n1: movs %0, #1\n2:" \
                   : "=&l"(r_) : "l"(a), "l"(b) : "cc");                    \
  r_; })
#define NZCV(op, a, b) \
  (FLAG(op, "mi", a, b) << 3 | FLAG(op, "eq", a, b) << 2 | FLAG(op, "cs", a, b) << 1 | FLAG(op, "vs", a, b))

static NOINLINE uint32_t push_pop(uint32_t a) {
  uint32_t r;
  __asm__ volatile("push {r4, r5}\n movs r4, %1\n adds r5, r4, #7\n push {r5}\n pop {r4}\n movs %0, r4\n pop {r4, r5}"
                   : "=l"(r) : "l"(a) : "r4", "r5", "cc");
  return r;
}

static NOINLINE uint32_t ldm_stm(uint32_t* p) {
  uint32_t base = (uint32_t)(uintptr_t)p;
  __asm__ volatile("movs r1, #1\n movs r2, #2\n movs r3, #3\n stmia %0!, {r1, r2, r3}\n"
                   "subs %0, #12\n ldmia %0!, {r1, r2}\n adds r3, r1, r2\n str r3, [%0]"
                   : "+l"(base) : : "r1", "r2", "r3", "memory");
  return base;
}

static NOINLINE uint32_t literal(void) {
  uint32_t r;
  __asm__ volatile("ldr %0, =0x12345678" : "=l"(r));
  return r;
}

static NOINLINE uint32_t hi_regs(uint32_t a, uint32_t b) {
  uint32_t r;
  __asm__ volatile("mov r8, %1\n mov r9, %2\n add r8, r9\n mov %0, r8\n cmp r8, r9\n bhi 1f\n movs %0, #0\n1:"
                   : "=&l"(r) : "l"(a), "l"(b) : "r8", "r9", "cc");
  return r;
}

// Switches to ARM state with BX PC, runs ARM code and comes back.
static NOINLINE uint32_t via_arm(uint32_t x) {
  register uint32_t r0 __asm__("r0") = x;
  __asm__ volatile(
      ".align 2\n"
      "bx pc\n"
      "nop\n"
      ".arm\n"
      "add r0, r0, r0, lsl #4\n"   // x * 17, an ARM-only operand form
      "add r1, pc, #1\n"
      "bx r1\n"
      ".thumb\n"
      : "+r"(r0) : : "r1", "cc");
  return r0;
}

static NOINLINE int sum_stack(int n) {
  volatile int arr[40];
  for (int i = 0; i < 40; i++) arr[i] = i * n;
  int s = 0;
  for (int i = 0; i < 40; i++) s += arr[i];
  return s;
}

void test_thumb(void);
void test_thumb(void) {
  CHECK_EQ(NZCV("adds %0, %1, %2", 0x7fffffffu, 1u), 0x9);
  CHECK_EQ(NZCV("adds %0, %1, %2", 0xffffffffu, 1u), 0x6);
  CHECK_EQ(NZCV("subs %0, %1, %2", 0u, 1u), 0x8);
  CHECK_EQ(NZCV("subs %0, %1, %2", 0x80000000u, 1u), 0x3);
  CHECK_EQ(NZCV("cmp %1, %2", 5u, 5u), 0x6);
  CHECK_EQ(NZCV("cmn %1, %2", 0x80000000u, 0x80000000u), 0x7);
  CHECK_EQ(NZCV("tst %1, %2", 0xf0u, 0x0fu) & 0xc, 0x4);
  CHECK_EQ(NZCV("movs %0, %1\n negs %0, %0", 0u, 0u), 0x6);           // 0 - 0: Z C
  CHECK_EQ(NZCV("movs %0, %1\n negs %0, %0", 1u, 0u), 0x8);
  CHECK_EQ(NZCV("movs %0, %1\n lsls %0, %2", 0x80000001u, 1u) & 0xe, 0x2);  // C out of bit 31
  CHECK_EQ(NZCV("movs %0, %1\n lsls %0, %2", 1u, 32u) & 0xe, 0x6);          // by register 32: C = bit 0
  CHECK_EQ(NZCV("movs %0, %1\n lsrs %0, %2", 3u, 1u) & 0xe, 0x2);
  CHECK_EQ(NZCV("movs %0, %1\n asrs %0, %2", 0x80000000u, 33u) & 0xe, 0xa);
  CHECK_EQ(NZCV("movs %0, %1\n rors %0, %2", 0x80000000u, 32u) & 0xe, 0xa);
  CHECK_EQ(NZCV("movs %0, %1\n lsls %0, %0, #31", 3u, 0u) & 0xe, 0xa);
  CHECK_EQ(NZCV("movs %0, %1\n lsrs %0, %0, #32", 0x80000000u, 0u) & 0xe, 0x6);
  CHECK_EQ(NZCV("movs %0, %1\n asrs %0, %0, #32", 0x80000000u, 0u) & 0xe, 0xa);
  CHECK_EQ(NZCV("cmp %1, %2\n movs %0, %1\n adcs %0, %2", 5u, 3u), 0x0);    // C=1: 5 + 3 + 1
  CHECK_EQ(NZCV("cmp %1, %2\n movs %0, %1\n sbcs %0, %2", 5u, 6u), 0x8);    // C=0: 5 - 6 - 1
  CHECK_EQ(NZCV("movs %0, %1\n muls %0, %2", 0x10000u, 0x10000u) & 0xc, 0x4);
  CHECK_EQ(NZCV("movs %0, %1\n bics %0, %2", 0xffu, 0xffu) & 0xc, 0x4);
  CHECK_EQ(NZCV("mvns %0, %1", 0u, 0u) & 0xc, 0x8);

  CHECK_EQ(push_pop(10), 17);
  {
    uint32_t mem[4] = {0};
    uint32_t nb = ldm_stm(mem);
    CHECK_EQ(nb - (uint32_t)(uintptr_t)mem, 8);
    CHECK_EQ(mem[0], 1); CHECK_EQ(mem[1], 2); CHECK_EQ(mem[2], 3);
  }
  CHECK_EQ(literal(), 0x12345678);
  CHECK_EQ(hi_regs(7, 5), 12);
  CHECK_EQ(via_arm(3), 51);
  CHECK_EQ(sum_stack(2), 2 * 780);
  CHECK_EQ(arm_pop_pc(5), 6);
  CHECK_EQ(arm_ldr_pc(5), 7);
  CHECK_EQ(arm_side(2), 7);
  int (*volatile fp)(int) = arm_side;
  CHECK_EQ(thumb_calls_arm(fp, 10), 231);
}
