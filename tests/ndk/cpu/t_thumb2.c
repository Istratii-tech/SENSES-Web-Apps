// Thumb-2 (armv7-a variants; always built -mthumb): IT blocks, CBZ/CBNZ,
// TBB/TBH, modified immediates with their carry, the 32-bit addressing
// modes, wide data-processing forms, bit-field and media instructions,
// exclusive access with offsets, hardware divide encodings.
#include "guest.h"

#define NOINLINE __attribute__((noinline))

static NOINLINE uint32_t it_test(uint32_t a, uint32_t b) {
  uint32_t r = 0;
  __asm__ volatile("cmp %1, %2\n"
                   "ite gt\n movgt %0, #1\n movle %0, #2\n"
                   "itete eq\n addeq %0, %0, #10\n addne %0, %0, #20\n addeq %0, %0, #40\n addne %0, %0, #80\n"
                   : "+r"(r) : "r"(a), "r"(b) : "cc");
  return r;
}

// A 16-bit ADD inside an IT block must not set flags.
static NOINLINE uint32_t it_noflags(void) {
  uint32_t r, s;
  __asm__ volatile("movs %0, #0\n it eq\n addeq %0, %0, #1\n ite eq\n moveq %1, #7\n movne %1, #9"
                   : "=&l"(r), "=&l"(s) : : "cc");
  return s * 100 + r;
}

static NOINLINE uint32_t cbz_test(uint32_t x) {
  uint32_t r;
  __asm__ volatile("movs %0, #1\n cbz %1, 1f\n movs %0, #2\n1: cbnz %1, 2f\n adds %0, #10\n2:"
                   : "=&l"(r) : "l"(x) : "cc");
  return r;
}

static NOINLINE uint32_t tbb_test(uint32_t i) {
  uint32_t r;
  __asm__ volatile("tbb [pc, %1]\n"
                   "1: .byte (2f-1b)/2, (3f-1b)/2, (4f-1b)/2, 0\n"
                   "2: movs %0, #11\n b 5f\n"
                   "3: movs %0, #22\n b 5f\n"
                   "4: movs %0, #33\n"
                   "5:\n"
                   : "=l"(r) : "r"(i) : "cc");
  return r;
}

static NOINLINE uint32_t tbh_test(uint32_t i) {
  uint32_t r;
  __asm__ volatile("tbh [pc, %1, lsl #1]\n"
                   "1: .hword (2f-1b)/2, (3f-1b)/2\n"
                   "2: movs %0, #44\n b 5f\n"
                   "3: movs %0, #55\n"
                   "5:\n"
                   : "=l"(r) : "r"(i) : "cc");
  return r;
}

__attribute__((naked)) static int t2_ldr_pc(int x) {
  __asm__("str lr, [sp, #-8]!\n adds r0, #3\n ldr pc, [sp], #8\n");
}
__attribute__((naked)) static int t2_pop_pc(int x) {
  __asm__("push.w {r4, r5, r6, lr}\n adds r0, #4\n pop.w {r4, r5, r6, pc}\n");
}

static NOINLINE uint32_t sdiv_raw(uint32_t n, uint32_t m) {
  register uint32_t r0 __asm__("r0");
  register uint32_t r1 __asm__("r1") = n;
  register uint32_t r2 __asm__("r2") = m;
  __asm__ volatile(".inst.w 0xfb91f0f2" : "=r"(r0) : "r"(r1), "r"(r2));   // sdiv r0, r1, r2
  return r0;
}
static NOINLINE uint32_t udiv_raw(uint32_t n, uint32_t m) {
  register uint32_t r0 __asm__("r0");
  register uint32_t r1 __asm__("r1") = n;
  register uint32_t r2 __asm__("r2") = m;
  __asm__ volatile(".inst.w 0xfbb1f0f2" : "=r"(r0) : "r"(r1), "r"(r2));   // udiv r0, r1, r2
  return r0;
}

static NOINLINE int dense_switch(int x) {
  switch (x) {
    case 0: return 5; case 1: return 7; case 2: return 11; case 3: return 13;
    case 4: return 17; case 5: return 19; case 6: return 23; case 7: return 29;
    case 8: return 31; case 9: return 37; case 10: return 41; case 11: return 43;
    default: return 0;
  }
}

void test_thumb2(void) {
  CHECK_EQ(it_test(5, 3), 101);
  CHECK_EQ(it_test(3, 3), 52);
  CHECK_EQ(it_test(1, 3), 102);
  CHECK_EQ(it_noflags(), 701);
  CHECK_EQ(cbz_test(0), 11);
  CHECK_EQ(cbz_test(5), 2);
  CHECK_EQ(tbb_test(0), 11);
  CHECK_EQ(tbb_test(1), 22);
  CHECK_EQ(tbb_test(2), 33);
  CHECK_EQ(tbh_test(0), 44);
  CHECK_EQ(tbh_test(1), 55);
  int sum = 0;
  for (int i = -1; i < 14; i++) sum += dense_switch(OPAQUE(i));
  CHECK_EQ(sum, 5 + 7 + 11 + 13 + 17 + 19 + 23 + 29 + 31 + 37 + 41 + 43);
  CHECK_EQ(t2_ldr_pc(1), 4);
  CHECK_EQ(t2_pop_pc(1), 5);

  uint32_t r, f;
  __asm__ volatile("mov.w %0, #0x00ff00ff" : "=r"(r));
  CHECK_EQ(r, 0x00ff00ff);
  __asm__ volatile("mvn.w %0, #0xff00ff00" : "=r"(r));
  CHECK_EQ(r, 0x00ff00ff);
  __asm__ volatile("mov.w %0, #0xabababab" : "=r"(r));
  CHECK_EQ(r, 0xabababab);
  __asm__ volatile("mov.w %0, #0x3fc00" : "=r"(r));
  CHECK_EQ(r, 0x3fc00);
  __asm__ volatile("msr APSR_nzcvq, %3\n ands.w %0, %2, #0x80000000\n mrs %1, APSR" : "=&r"(r), "=&r"(f) : "r"(0x80000001), "r"(0) : "cc");
  CHECK_EQ(r, 0x80000000); CHECK_EQ(f >> 28, 0xa);   // N, C from the rotated immediate
  __asm__ volatile("msr APSR_nzcvq, %3\n ands.w %0, %2, #0xff\n mrs %1, APSR" : "=&r"(r), "=&r"(f) : "r"(0x12345678), "r"(0x20000000) : "cc");
  CHECK_EQ(r, 0x78); CHECK_EQ(f >> 28, 0x2);         // C kept
  __asm__ volatile("orn %0, %1, %2" : "=r"(r) : "r"(0), "r"(0xffff0000));
  CHECK_EQ(r, 0x0000ffff);
  __asm__ volatile("teq %1, %2\n mrs %0, APSR" : "=r"(f) : "r"(0x1234), "r"(0x1234) : "cc");
  CHECK_EQ((f >> 30) & 1, 1);
  __asm__ volatile("msr APSR_nzcvq, %2\n rrx %0, %1" : "=r"(r) : "r"(6), "r"(0x20000000) : "cc");
  CHECK_EQ(r, 0x80000003);
  __asm__ volatile("rsb.w %0, %1, #100" : "=r"(r) : "r"(30));
  CHECK_EQ(r, 70);
  __asm__ volatile("lsls.w %0, %2, #4\n mrs %1, APSR" : "=&r"(r), "=r"(f) : "r"(0x18000000) : "cc");
  CHECK_EQ(r, 0x80000000); CHECK_EQ((f >> 28) & 0xe, 0xa);   // N, C = bit 28
  __asm__ volatile("add.w %0, %1, %2, lsl #3" : "=r"(r) : "r"(1), "r"(2));
  CHECK_EQ(r, 17);
  __asm__ volatile("sub.w %0, %1, %2, asr #1" : "=r"(r) : "r"(100), "r"(-8));
  CHECK_EQ(r, 104);
  __asm__ volatile("addw %0, %1, #4095" : "=r"(r) : "r"(1));
  CHECK_EQ(r, 4096);
  __asm__ volatile("subw %0, %1, #1" : "=r"(r) : "r"(0));
  CHECK_EQ(r, 0xffffffff);
  {
    uint32_t a = 0xffffffff, b = 0, c = 1, d = 0;   // 64-bit add with adds.w/adc.w
    __asm__ volatile("adds.w %0, %0, %2\n adc.w %1, %1, %3" : "+&r"(a), "+&r"(b) : "r"(c), "r"(d) : "cc");
    CHECK_EQ(a, 0); CHECK_EQ(b, 1);
    a = 0; b = 1;
    __asm__ volatile("subs.w %0, %0, %2\n sbc.w %1, %1, %3" : "+&r"(a), "+&r"(b) : "r"(c), "r"(d) : "cc");
    CHECK_EQ(a, 0xffffffff); CHECK_EQ(b, 0);
  }
  {
    uint32_t arr[4] = {10, 20, 30, 40};
    uint32_t* p = arr + 1;
    __asm__ volatile("ldr %0, [%1, #-4]" : "=r"(r) : "r"(p));
    CHECK_EQ(r, 10);
    __asm__ volatile("ldr %0, [%1], #4" : "=r"(r), "+r"(p));
    CHECK_EQ(r, 20); CHECK(p == arr + 2);
    __asm__ volatile("ldr %0, [%1, #4]!" : "=r"(r), "+r"(p));
    CHECK_EQ(r, 40); CHECK(p == arr + 3);
    __asm__ volatile("ldr.w %0, [%1, %2, lsl #2]" : "=r"(r) : "r"(arr), "r"(2));
    CHECK_EQ(r, 30);
    uint32_t x, y;
    __asm__ volatile("ldrd %0, %1, [%2, #8]" : "=&r"(x), "=r"(y) : "r"(arr) : "memory");
    CHECK_EQ(x, 30); CHECK_EQ(y, 40);
    p = arr + 4;
    __asm__ volatile("strd %1, %2, [%0, #-8]!" : "+r"(p) : "r"(7), "r"(8) : "memory");
    CHECK(p == arr + 2); CHECK_EQ(arr[2], 7); CHECK_EQ(arr[3], 8);
    int16_t hs[3] = {1, -300, 3};
    __asm__ volatile("ldrsh.w %0, [%1, #2]" : "=r"(r) : "r"(hs));
    CHECK_EQ(r, (uint32_t)-300);
    uint8_t bs[4] = {0};
    __asm__ volatile("strb.w %1, [%0, #3]" : : "r"(bs), "r"(0x1ab) : "memory");
    CHECK_EQ(bs[3], 0xab);
    __asm__ volatile("ldrb %0, [%1, #-1]" : "=r"(r) : "r"(bs + 4));
    CHECK_EQ(r, 0xab);
    uint32_t word[2] = {0, 41}, status;
    __asm__ volatile("1: ldrex %0, [%2, #4]\n adds %0, #1\n strex %1, %0, [%2, #4]\n cmp %1, #0\n bne 1b"
                     : "=&r"(r), "=&r"(status) : "r"(word) : "cc", "memory");
    CHECK_EQ(word[1], 42);
  }
  CHECK_EQ(sdiv_raw(-100, 7), (uint32_t)-14);
  CHECK_EQ(sdiv_raw(0x80000000, -1), 0x80000000);
  CHECK_EQ(sdiv_raw(5, 0), 0);
  CHECK_EQ(udiv_raw(0xffffffff, 16), 0x0fffffff);
  __asm__ volatile("ubfx %0, %1, #28, #4" : "=r"(r) : "r"(0xa0000000));
  CHECK_EQ(r, 0xa);
  __asm__ volatile("sbfx %0, %1, #0, #1" : "=r"(r) : "r"(1));
  CHECK_EQ(r, 0xffffffff);
  r = 0;
  __asm__ volatile("bfi %0, %1, #0, #32" : "+r"(r) : "r"(0x12345678));
  CHECK_EQ(r, 0x12345678);
  __asm__ volatile("bfc %0, #4, #24" : "+r"(r));
  CHECK_EQ(r, 0x10000008);
  __asm__ volatile("rbit %0, %1" : "=r"(r) : "r"(0x12345678));
  CHECK_EQ(r, 0x1e6a2c48);
  __asm__ volatile("clz %0, %1" : "=r"(r) : "r"(0x8000));
  CHECK_EQ(r, 16);
  __asm__ volatile("uxtah %0, %1, %2, ror #16" : "=r"(r) : "r"(1), "r"(0xfffe0000));
  CHECK_EQ(r, 0xffff);
  __asm__ volatile("sxtb.w %0, %1, ror #8" : "=r"(r) : "r"(0x8000));
  CHECK_EQ(r, 0xffffff80);
  __asm__ volatile("usat %0, #16, %1, asr #4" : "=r"(r) : "r"(0x7fffffff));
  CHECK_EQ(r, 0xffff);
  __asm__ volatile("pkhtb %0, %1, %2, asr #8" : "=r"(r) : "r"(0xaaaa0000), "r"(0x00123400));
  CHECK_EQ(r, 0xaaaa1234);
  __asm__ volatile("msr APSR_nzcvq, %3\n qadd %0, %2, %2\n mrs %1, APSR" : "=&r"(r), "=&r"(f) : "r"(0x60000000), "r"(0) : "cc");
  CHECK_EQ(r, 0x7fffffff); CHECK((f >> 27) & 1);
  __asm__ volatile("smulbb %0, %1, %2" : "=r"(r) : "r"(-3), "r"(7));
  CHECK_EQ(r, (uint32_t)-21);
  __asm__ volatile("smlabt %0, %1, %2, %3" : "=r"(r) : "r"(-3), "r"(0x00070000), "r"(1));
  CHECK_EQ(r, (uint32_t)-20);
  __asm__ volatile("smulwt %0, %1, %2" : "=r"(r) : "r"(0x10000), "r"(0x00090000));
  CHECK_EQ(r, 9);
  __asm__ volatile("mls %0, %1, %2, %3" : "=r"(r) : "r"(6), "r"(7), "r"(50));
  CHECK_EQ(r, 8);
  {
    uint32_t lo, hi;
    __asm__ volatile("smull %0, %1, %2, %3" : "=&r"(lo), "=&r"(hi) : "r"(-5), "r"(0x40000000));
    CHECK_EQ(((uint64_t)hi << 32) | lo, (uint64_t)(-5ll * 0x40000000));
    lo = 1; hi = 1;
    __asm__ volatile("umaal %0, %1, %2, %3" : "+r"(lo), "+r"(hi) : "r"(3), "r"(4));
    CHECK_EQ(((uint64_t)hi << 32) | lo, 14);
    lo = 0xffffffff; hi = 0;
    __asm__ volatile("umlal %0, %1, %2, %3" : "+r"(lo), "+r"(hi) : "r"(1), "r"(1));
    CHECK_EQ(hi, 1); CHECK_EQ(lo, 0);
    lo = 0; hi = 0;
    __asm__ volatile("smlaltb %0, %1, %2, %3" : "+r"(lo), "+r"(hi) : "r"(0xfffe0000), "r"(5));
    CHECK_EQ(((uint64_t)hi << 32) | lo, (uint64_t)-10ll);
  }
  __asm__ volatile("smmul %0, %1, %2" : "=r"(r) : "r"(0x80000000), "r"(0x80000000));
  CHECK_EQ(r, 0x40000000);
  __asm__ volatile("uadd8 %0, %1, %2\n sel %0, %3, %4" : "=&r"(r) : "r"(0xff000000), "r"(0x01000000), "r"(0x11111111), "r"(0x22222222));
  CHECK_EQ(r, 0x11222222);
  __asm__ volatile("uhadd8 %0, %1, %2" : "=r"(r) : "r"(0xff010203), "r"(0xff030201));
  CHECK_EQ(r, 0xff020202);
  __asm__ volatile("qsub8 %0, %1, %2" : "=r"(r) : "r"(0x80800000), "r"(0x01ff7f81));
  CHECK_EQ(r, 0x8081817f);
  __asm__ volatile("rev16.w %0, %1" : "=r"(r) : "r"(0xaabbccdd));
  CHECK_EQ(r, 0xbbaaddcc);
}
