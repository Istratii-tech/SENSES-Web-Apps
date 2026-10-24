// Instruction-level tests in ARM state (this file is always built -marm):
// flags of arithmetic, shifter carry-out, every condition code, LDM/STM
// addressing modes with writeback, DSP multiplies and saturation (v5TE),
// pc reads and jump tables, interworking returns, and the ARMv6/v7 media
// instructions for the armv7-a variants.
#include "guest.h"

#define NOINLINE __attribute__((noinline))
#define NZCV(x) ((x) >> 28)

// --- interworking partners of t_thumb.c and t_control.c ---------------------------
NOINLINE int arm_side(int x) { return x * 3 + 1; }
NOINLINE int arm_calls_thumb(int (*fn)(int), int x) { return fn(x) + 100; }

// Returns through pop {.., pc} / ldr pc: an interworking return on ARMv5T+.
__attribute__((naked)) int arm_pop_pc(int x) {
  __asm__("push {r4, lr}\n"
          "add r0, r0, #1\n"
          "pop {r4, pc}\n");
}
__attribute__((naked)) int arm_ldr_pc(int x) {
  __asm__("str lr, [sp, #-8]!\n"
          "add r0, r0, #2\n"
          "ldr pc, [sp], #8\n");
}

static NOINLINE uint32_t flags_adds(uint32_t a, uint32_t b) {
  uint32_t r, f;
  __asm__ volatile("adds %0, %2, %3\n mrs %1, cpsr" : "=&r"(r), "=r"(f) : "r"(a), "r"(b) : "cc");
  return NZCV(f);
}
static NOINLINE uint32_t flags_subs(uint32_t a, uint32_t b) {
  uint32_t r, f;
  __asm__ volatile("subs %0, %2, %3\n mrs %1, cpsr" : "=&r"(r), "=r"(f) : "r"(a), "r"(b) : "cc");
  return NZCV(f);
}
static NOINLINE uint32_t flags_rsbs(uint32_t a, uint32_t b) {
  uint32_t r, f;
  __asm__ volatile("rsbs %0, %2, %3\n mrs %1, cpsr" : "=&r"(r), "=r"(f) : "r"(a), "r"(b) : "cc");
  return NZCV(f);
}
// 96-bit add and subtract chains (ADC/SBC/RSC with incoming carry).
static NOINLINE void add96(uint32_t* r, const uint32_t* a, const uint32_t* b) {
  __asm__ volatile(
      "ldr r4, [%1]\n ldr r5, [%2]\n adds r4, r4, r5\n str r4, [%0]\n"
      "ldr r4, [%1, #4]\n ldr r5, [%2, #4]\n adcs r4, r4, r5\n str r4, [%0, #4]\n"
      "ldr r4, [%1, #8]\n ldr r5, [%2, #8]\n adc r4, r4, r5\n str r4, [%0, #8]\n"
      : : "r"(r), "r"(a), "r"(b) : "r4", "r5", "cc", "memory");
}
static NOINLINE void sub96(uint32_t* r, const uint32_t* a, const uint32_t* b) {
  __asm__ volatile(
      "ldr r4, [%1]\n ldr r5, [%2]\n subs r4, r4, r5\n str r4, [%0]\n"
      "ldr r4, [%1, #4]\n ldr r5, [%2, #4]\n sbcs r4, r4, r5\n str r4, [%0, #4]\n"
      "ldr r4, [%1, #8]\n ldr r5, [%2, #8]\n rsc r4, r5, r4\n str r4, [%0, #8]\n"
      : : "r"(r), "r"(a), "r"(b) : "r4", "r5", "cc", "memory");
}

// value and NZCV after MOVS with a shifter operand
#define SHIFT_TEST(name, insn)                                                     \
  static NOINLINE uint64_t name(uint32_t v, uint32_t amount, uint32_t cin) {       \
    uint32_t r, f;                                                                 \
    __asm__ volatile("msr cpsr_f, %3\n" insn "\n mrs %1, cpsr"                     \
                     : "=&r"(r), "=&r"(f) : "r"(v), "r"(cin ? 0x20000000u : 0), "r"(amount) : "cc"); \
    return r | (uint64_t)NZCV(f) << 32;                                            \
  }
SHIFT_TEST(movs_lsl1, "movs %0, %2, lsl #1")
SHIFT_TEST(movs_lsr32, "movs %0, %2, lsr #32")
SHIFT_TEST(movs_asr32, "movs %0, %2, asr #32")
SHIFT_TEST(movs_asr5, "movs %0, %2, asr #5")
SHIFT_TEST(movs_ror4, "movs %0, %2, ror #4")
SHIFT_TEST(movs_rrx, "movs %0, %2, rrx")
SHIFT_TEST(movs_lslr, "movs %0, %2, lsl %4")
SHIFT_TEST(movs_lsrr, "movs %0, %2, lsr %4")
SHIFT_TEST(movs_asrr, "movs %0, %2, asr %4")
SHIFT_TEST(movs_rorr, "movs %0, %2, ror %4")
SHIFT_TEST(ands_imm, "ands %0, %2, #0xf0000000")   // rotated immediate: C = bit 31
SHIFT_TEST(ands_imm8, "ands %0, %2, #0xff")        // unrotated immediate: C unchanged
SHIFT_TEST(adds_rrx, "adds %0, %2, %2, rrx")

static NOINLINE uint32_t cond_mask(uint32_t nzcv) {
  uint32_t m = 0;
  __asm__ volatile(
      "msr cpsr_f, %1\n"
      "orreq %0, %0, #1\n orrne %0, %0, #2\n orrcs %0, %0, #4\n orrcc %0, %0, #8\n"
      "orrmi %0, %0, #16\n orrpl %0, %0, #32\n orrvs %0, %0, #64\n orrvc %0, %0, #128\n"
      "orrhi %0, %0, #256\n orrls %0, %0, #512\n orrge %0, %0, #1024\n orrlt %0, %0, #2048\n"
      "orrgt %0, %0, #4096\n orrle %0, %0, #8192\n orral %0, %0, #16384\n"
      : "+r"(m) : "r"(nzcv << 28) : "cc");
  return m;
}
static uint32_t expected_cond_mask(uint32_t f) {
  int n = (f >> 3) & 1, z = (f >> 2) & 1, c = (f >> 1) & 1, v = f & 1;
  int pass[15] = {z, !z, c, !c, n, !n, v, !v, c && !z, !c || z, n == v, n != v, !z && n == v, z || n != v, 1};
  uint32_t m = 0;
  for (int i = 0; i < 15; i++) if (pass[i]) m |= 1u << i;
  return m;
}

static NOINLINE uint32_t jump_table(uint32_t i) {
  uint32_t r;
  __asm__ volatile(
      "add pc, pc, %1, lsl #2\n"
      "nop\n"
      "b 1f\n b 2f\n b 3f\n"
      "1: mov %0, #10\n b 9f\n"
      "2: mov %0, #20\n b 9f\n"
      "3: mov %0, #30\n"
      "9:\n"
      : "=r"(r) : "r"(i));
  return r;
}

static NOINLINE int pc_offset(void) {
  uint32_t a, b;
  __asm__ volatile("1: mov %0, pc\n adr %1, 1b\n" : "=r"(a), "=r"(b));
  return (int)(a - b);
}

// LDM/STM addressing modes: returns the new base, writes to mem.
#define STM_TEST(name, insn)                                                           \
  static NOINLINE uint32_t name(uint32_t* base) {                                     \
    register uint32_t b __asm__("r0") = (uint32_t)(uintptr_t)base;                   \
    __asm__ volatile("mov r1, #0x11\n mov r2, #0x22\n mov r3, #0x33\n" insn             \
                     : "+r"(b) : : "r1", "r2", "r3", "memory");                       \
    return b;                                                                         \
  }
STM_TEST(stmia_wb, "stmia r0!, {r1-r3}")
STM_TEST(stmib_wb, "stmib r0!, {r1-r3}")
STM_TEST(stmda_wb, "stmda r0!, {r1-r3}")
STM_TEST(stmdb_wb, "stmdb r0!, {r1-r3}")
#define LDM_TEST(name, insn)                                                           \
  static NOINLINE uint32_t name(uint32_t* base, uint32_t* out) {                     \
    register uint32_t b __asm__("r0") = (uint32_t)(uintptr_t)base;                   \
    __asm__ volatile(insn "\n stmia %1, {r1-r3}" : "+r"(b) : "r"(out) : "r1", "r2", "r3", "memory"); \
    return b;                                                                         \
  }
LDM_TEST(ldmia_wb, "ldmia r0!, {r1-r3}")
LDM_TEST(ldmib_wb, "ldmib r0!, {r1-r3}")
LDM_TEST(ldmda_wb, "ldmda r0!, {r1-r3}")
LDM_TEST(ldmdb_wb, "ldmdb r0!, {r1-r3}")

static NOINLINE uint32_t ldr_post_neg(uint32_t* p, uint32_t idx, uint32_t* out_base) {
  uint32_t v;
  __asm__ volatile("ldr %0, [%1], -%2, lsl #2" : "=r"(v), "+r"(p) : "r"(idx));
  *out_base = (uint32_t)(uintptr_t)p;
  return v;
}
static NOINLINE uint32_t ldrh_pre_wb(uint16_t* p, uint32_t* out_base) {
  uint32_t v;
  __asm__ volatile("ldrh %0, [%1, #4]!" : "=r"(v), "+r"(p));
  *out_base = (uint32_t)(uintptr_t)p;
  return v;
}
static NOINLINE int32_t ldrsb_reg(int8_t* p, uint32_t i) {
  int32_t v;
  __asm__ volatile("ldrsb %0, [%1, -%2]" : "=r"(v) : "r"(p), "r"(i));
  return v;
}

void test_asm(void) {
  CHECK_EQ(flags_adds(0x7fffffff, 1), 0x9);       // N V
  CHECK_EQ(flags_adds(0xffffffff, 1), 0x6);       // Z C
  CHECK_EQ(flags_adds(0x80000000, 0x80000000), 0x7);   // Z C V
  CHECK_EQ(flags_adds(1, 2), 0x0);
  CHECK_EQ(flags_subs(0, 1), 0x8);                // N, borrow
  CHECK_EQ(flags_subs(5, 5), 0x6);                // Z C
  CHECK_EQ(flags_subs(0x80000000, 1), 0x3);       // C V
  CHECK_EQ(flags_subs(1, 0x80000000), 0x9);       // N V
  CHECK_EQ(flags_rsbs(1, 0), 0x8);
  {
    uint32_t a[3] = {0xffffffff, 0xffffffff, 1}, b[3] = {1, 0, 2}, r[3];
    add96(r, a, b);
    CHECK_EQ(r[0], 0); CHECK_EQ(r[1], 0); CHECK_EQ(r[2], 4);
    uint32_t c[3] = {0, 0, 5}, d[3] = {1, 0, 1};
    sub96(r, c, d);
    CHECK_EQ(r[0], 0xffffffff); CHECK_EQ(r[1], 0xffffffff); CHECK_EQ(r[2], 3);
  }
  CHECK_EQ(movs_lsl1(0x80000001, 0, 0), 0x2ull | 2ull << 32);            // C from bit 31
  CHECK_EQ(movs_lsr32(0x80000000, 0, 0), 0x0ull | 6ull << 32);           // Z C
  CHECK_EQ(movs_asr32(0x80000000, 0, 0), 0xffffffffull | 0xaull << 32);  // N C
  CHECK_EQ(movs_asr5(0x80000010, 0, 0), 0xfc000000ull | 0xaull << 32);   // N, C = bit 4
  CHECK_EQ(movs_ror4(0x0000001f, 0, 0), 0xf0000001ull | 0xaull << 32);
  CHECK_EQ(movs_rrx(2, 0, 1), 0x80000001ull | 0x8ull << 32);             // C in -> bit 31, C out = 0
  CHECK_EQ(movs_rrx(3, 0, 0), 0x1ull | 0x2ull << 32);
  CHECK_EQ(movs_lslr(1, 0, 1), 0x1ull | 0x2ull << 32);                   // amount 0: C unchanged
  CHECK_EQ(movs_lslr(1, 32, 0), 0x0ull | 0x6ull << 32);                  // C = bit 0
  CHECK_EQ(movs_lslr(1, 33, 1), 0x0ull | 0x4ull << 32);                  // C = 0
  CHECK_EQ(movs_lslr(3, 0x101, 0), 0x6ull);                              // bottom byte only: lsl #1
  CHECK_EQ(movs_lsrr(0x80000000, 32, 0), 0x0ull | 0x6ull << 32);
  CHECK_EQ(movs_lsrr(0x80000000, 255, 1), 0x0ull | 0x4ull << 32);
  CHECK_EQ(movs_asrr(0x80000000, 40, 0), 0xffffffffull | 0xaull << 32);
  CHECK_EQ(movs_rorr(0x80000000, 32, 0), 0x80000000ull | 0xaull << 32);  // C = bit 31
  CHECK_EQ(movs_rorr(0x00000003, 33, 0), 0x80000001ull | 0xaull << 32);
  CHECK_EQ(ands_imm(0xf0000000, 0, 0), 0xf0000000ull | 0xaull << 32);
  CHECK_EQ(ands_imm8(0x100, 0, 1), 0x0ull | 0x6ull << 32);
  CHECK_EQ(adds_rrx(0x80000000, 0, 1), 0x40000000ull | 0x3ull << 32);   // 0x80000000 + 0xc0000000

  for (uint32_t f = 0; f < 16; f++) CHECK_EQ(cond_mask(f), expected_cond_mask(f));

  CHECK_EQ(jump_table(0), 10);
  CHECK_EQ(jump_table(1), 20);
  CHECK_EQ(jump_table(2), 30);
  CHECK_EQ(pc_offset(), 8);

  {
    uint32_t mem[12] = {0}, nb;
    uint32_t* mid = mem + 6;
    nb = stmia_wb(mid);
    CHECK_EQ(nb - (uint32_t)(uintptr_t)mid, 12); CHECK_EQ(mid[0], 0x11); CHECK_EQ(mid[2], 0x33);
    memset(mem, 0, sizeof(mem));
    nb = stmib_wb(mid);
    CHECK_EQ(nb - (uint32_t)(uintptr_t)mid, 12); CHECK_EQ(mid[1], 0x11); CHECK_EQ(mid[3], 0x33); CHECK_EQ(mid[0], 0);
    memset(mem, 0, sizeof(mem));
    nb = stmda_wb(mid);
    CHECK_EQ((uint32_t)(uintptr_t)mid - nb, 12); CHECK_EQ(mid[-2], 0x11); CHECK_EQ(mid[0], 0x33);
    memset(mem, 0, sizeof(mem));
    nb = stmdb_wb(mid);
    CHECK_EQ((uint32_t)(uintptr_t)mid - nb, 12); CHECK_EQ(mid[-3], 0x11); CHECK_EQ(mid[-1], 0x33); CHECK_EQ(mid[0], 0);
    for (int i = 0; i < 12; i++) mem[i] = 100 + i;
    uint32_t out[3];
    nb = ldmia_wb(mid, out);
    CHECK_EQ(nb - (uint32_t)(uintptr_t)mid, 12); CHECK_EQ(out[0], 106); CHECK_EQ(out[2], 108);
    nb = ldmib_wb(mid, out);
    CHECK_EQ(out[0], 107); CHECK_EQ(out[2], 109);
    nb = ldmda_wb(mid, out);
    CHECK_EQ((uint32_t)(uintptr_t)mid - nb, 12); CHECK_EQ(out[0], 104); CHECK_EQ(out[2], 106);
    nb = ldmdb_wb(mid, out);
    CHECK_EQ(out[0], 103); CHECK_EQ(out[2], 105);
    uint32_t base;
    CHECK_EQ(ldr_post_neg(mid, 2, &base), 106);
    CHECK_EQ((uint32_t)(uintptr_t)mid - base, 8);
    uint16_t hw[6] = {1, 2, 3, 4, 5, 6};
    CHECK_EQ(ldrh_pre_wb(hw, &base), 3);
    CHECK_EQ(base - (uint32_t)(uintptr_t)hw, 4);
    int8_t sb[4] = {-100, 5, 6, 7};
    CHECK_EQ(ldrsb_reg(sb + 3, 3), (uint32_t)-100);
  }
  {   // multiplies
    uint32_t lo = 0x80000000, hi = 1, r;
    __asm__ volatile("umlal %0, %1, %2, %3" : "+r"(lo), "+r"(hi) : "r"(0xffffffffu), "r"(2u));
    CHECK_EQ(((uint64_t)hi << 32) | lo, 0x37ffffffeull);
    lo = 5; hi = 0;
    __asm__ volatile("smlal %0, %1, %2, %3" : "+r"(lo), "+r"(hi) : "r"(-3), "r"(4));
    CHECK_EQ(((uint64_t)hi << 32) | lo, (uint64_t)-7ll);
    uint32_t f;
    __asm__ volatile("umulls %0, %1, %3, %4\n mrs %2, cpsr" : "=&r"(lo), "=&r"(hi), "=r"(f) : "r"(0x80000000u), "r"(2u) : "cc");
    CHECK_EQ(NZCV(f) & 0xc, 0x0);
    CHECK_EQ(hi, 1);
    __asm__ volatile("muls %0, %2, %3\n mrs %1, cpsr" : "=&r"(r), "=r"(f) : "r"(0x10000u), "r"(0x10000u) : "cc");
    CHECK_EQ(r, 0);
    CHECK_EQ(NZCV(f) & 0xc, 0x4);
    __asm__ volatile("mla %0, %1, %2, %3" : "=&r"(r) : "r"(-2), "r"(3), "r"(10));
    CHECK_EQ(r, 4);
  }
  {   // v5TE DSP: saturation and halfword multiplies
    uint32_t r, f;
    __asm__ volatile("msr cpsr_f, #0\n qadd %0, %2, %3\n mrs %1, cpsr" : "=&r"(r), "=r"(f) : "r"(0x7fffffff), "r"(1) : "cc");
    CHECK_EQ(r, 0x7fffffff); CHECK((f >> 27) & 1);
    __asm__ volatile("msr cpsr_f, #0\n qsub %0, %2, %3\n mrs %1, cpsr" : "=&r"(r), "=r"(f) : "r"(0x80000000), "r"(1) : "cc");
    CHECK_EQ(r, 0x80000000); CHECK((f >> 27) & 1);
    __asm__ volatile("msr cpsr_f, #0\n qadd %0, %2, %3\n mrs %1, cpsr" : "=&r"(r), "=r"(f) : "r"(5), "r"(-7) : "cc");
    CHECK_EQ(r, (uint32_t)-2); CHECK(!((f >> 27) & 1));
    __asm__ volatile("qdadd %0, %1, %2" : "=r"(r) : "r"(1), "r"(0x40000000));   // 1 + sat(2 * 0x40000000)
    CHECK_EQ(r, 0x7fffffff);
    __asm__ volatile("qdsub %0, %1, %2" : "=r"(r) : "r"(0), "r"(0x10));
    CHECK_EQ(r, (uint32_t)-0x20);
    __asm__ volatile("smulbb %0, %1, %2" : "=r"(r) : "r"(0x0003fffe), "r"(0x00050007));
    CHECK_EQ(r, (uint32_t)-14);
    __asm__ volatile("smultt %0, %1, %2" : "=r"(r) : "r"(0x0003fffe), "r"(0xfffb0007));
    CHECK_EQ(r, (uint32_t)-15);
    __asm__ volatile("smulbt %0, %1, %2" : "=r"(r) : "r"(0x0003fffe), "r"(0x00050007));
    CHECK_EQ(r, (uint32_t)-10);
    __asm__ volatile("smlabb %0, %1, %2, %3" : "=r"(r) : "r"(0x00000010), "r"(0x00000010), "r"(1000));
    CHECK_EQ(r, 1256);
    __asm__ volatile("smulwb %0, %1, %2" : "=r"(r) : "r"(0x30000), "r"(0x00050007));
    CHECK_EQ(r, 21);
    __asm__ volatile("smlawt %0, %1, %2, %3" : "=r"(r) : "r"(-0x20000), "r"(0x00030000), "r"(4));
    CHECK_EQ(r, (uint32_t)-2);
    uint32_t lo = 0xffffffff, hi = 0;
    __asm__ volatile("smlalbb %0, %1, %2, %3" : "+r"(lo), "+r"(hi) : "r"(2), "r"(3));
    CHECK_EQ(((uint64_t)hi << 32) | lo, 0x100000005ull);
    __asm__ volatile("clz %0, %1" : "=r"(r) : "r"(0x00ffffff));
    CHECK_EQ(r, 8);
    __asm__ volatile("clz %0, %1" : "=r"(r) : "r"(0));
    CHECK_EQ(r, 32);
  }
  {   // LDRD/STRD, SWP
    uint64_t q[2] = {0x1111111122222222ull, 0};
    uint32_t lo, hi;
    __asm__ volatile("ldrd r2, r3, [%2]\n mov %0, r2\n mov %1, r3" : "=r"(lo), "=r"(hi) : "r"(q) : "r2", "r3", "memory");
    CHECK_EQ(lo, 0x22222222); CHECK_EQ(hi, 0x11111111);
    __asm__ volatile("mov r2, #5\n mov r3, #6\n strd r2, r3, [%0, #8]" : : "r"(q) : "r2", "r3", "memory");
    CHECK_EQ(q[1], 0x600000005ull);
    uint32_t word = 7, old;
    __asm__ volatile("swp %0, %1, [%2]" : "=&r"(old) : "r"(9), "r"(&word) : "memory");
    CHECK_EQ(old, 7); CHECK_EQ(word, 9);
    uint8_t byte = 0xaa;
    __asm__ volatile("swpb %0, %1, [%2]" : "=&r"(old) : "r"(0x1bb), "r"(&byte) : "memory");
    CHECK_EQ(old, 0xaa); CHECK_EQ(byte, 0xbb);
  }
  {   // MSR/MRS, str pc
    uint32_t f;
    __asm__ volatile("msr cpsr_f, %1\n mrs %0, cpsr" : "=r"(f) : "r"(0xf8000000u) : "cc");
    CHECK_EQ(f & 0xf8000000u, 0xf8000000u);
    __asm__ volatile("msr cpsr_f, #0\n mrs %0, cpsr" : "=r"(f) : : "cc");
    CHECK_EQ(f & 0xf8000000u, 0);
    uint32_t stored, here;
    __asm__ volatile("1: str pc, [%1]\n adr %0, 1b" : "=r"(here) : "r"(&stored) : "memory");
    CHECK_EQ(stored - here, 8);
  }
  CHECK_EQ(arm_pop_pc(OPAQUE(1)), 2);
  CHECK_EQ(arm_ldr_pc(OPAQUE(1)), 3);

#if __ARM_ARCH >= 7
  {
    uint32_t r, s;
    __asm__ volatile("movw %0, #0xbeef\n movt %0, #0xdead" : "=r"(r));
    CHECK_EQ(r, 0xdeadbeef);
    __asm__ volatile("ubfx %0, %1, #4, #8" : "=r"(r) : "r"(0x12345678));
    CHECK_EQ(r, 0x67);
    __asm__ volatile("sbfx %0, %1, #4, #8" : "=r"(r) : "r"(0x12345f78));
    CHECK_EQ(r, (uint32_t)(int32_t)(int8_t)0xf7);
    r = 0xffffffff;
    __asm__ volatile("bfc %0, #8, #16" : "+r"(r));
    CHECK_EQ(r, 0xff0000ff);
    __asm__ volatile("bfi %0, %1, #12, #8" : "+r"(r) : "r"(0xabcd));
    CHECK_EQ(r, 0xff0cd0ff);
    __asm__ volatile("rev %0, %1" : "=r"(r) : "r"(0x11223344));
    CHECK_EQ(r, 0x44332211);
    __asm__ volatile("rev16 %0, %1" : "=r"(r) : "r"(0x11223344));
    CHECK_EQ(r, 0x22114433);
    __asm__ volatile("revsh %0, %1" : "=r"(r) : "r"(0x000080ff));
    CHECK_EQ(r, 0xffffff80);
    __asm__ volatile("rbit %0, %1" : "=r"(r) : "r"(0x00000001));
    CHECK_EQ(r, 0x80000000);
    __asm__ volatile("mls %0, %1, %2, %3" : "=r"(r) : "r"(3), "r"(4), "r"(100));
    CHECK_EQ(r, 88);
    __asm__ volatile("uxtb %0, %1, ror #8" : "=r"(r) : "r"(0x11223344));
    CHECK_EQ(r, 0x33);
    __asm__ volatile("sxth %0, %1, ror #16" : "=r"(r) : "r"(0x8001ffff));
    CHECK_EQ(r, 0xffff8001);
    __asm__ volatile("uxtab %0, %1, %2" : "=r"(r) : "r"(1000), "r"(0x1ff));
    CHECK_EQ(r, 1255);
    __asm__ volatile("sxtab16 %0, %1, %2" : "=r"(r) : "r"(0x00010001), "r"(0x00ff0080));
    CHECK_EQ(r, 0x0000ff81);
    __asm__ volatile("uxtb16 %0, %1" : "=r"(r) : "r"(0x11223344));
    CHECK_EQ(r, 0x00220044);
    __asm__ volatile("usat %0, #8, %1" : "=r"(r) : "r"(300));
    CHECK_EQ(r, 255);
    __asm__ volatile("usat %0, #8, %1" : "=r"(r) : "r"(-5));
    CHECK_EQ(r, 0);
    __asm__ volatile("ssat %0, #8, %1, lsl #2" : "=r"(r) : "r"(-40));
    CHECK_EQ(r, (uint32_t)-128);
    __asm__ volatile("ssat16 %0, #4, %1" : "=r"(r) : "r"(0x0020fff0));
    CHECK_EQ(r, 0x0007fff8);
    __asm__ volatile("pkhbt %0, %1, %2, lsl #16" : "=r"(r) : "r"(0x11112222), "r"(0x33334444));
    CHECK_EQ(r, 0x44442222);
    __asm__ volatile("pkhtb %0, %1, %2, asr #16" : "=r"(r) : "r"(0x11112222), "r"(0x33334444));
    CHECK_EQ(r, 0x11113333);
    __asm__ volatile("uadd8 %0, %2, %3\n sel %1, %4, %5" : "=&r"(r), "=r"(s)
                     : "r"(0x80ff0102), "r"(0x80010101), "r"(0xaaaaaaaa), "r"(0xbbbbbbbb));
    CHECK_EQ(r, 0x00000203);
    CHECK_EQ(s, 0xaaaabbbb);   // GE set for the lanes that carried out
    __asm__ volatile("uqsub8 %0, %1, %2" : "=r"(r) : "r"(0x10203040), "r"(0x20103050));
    CHECK_EQ(r, 0x00100000);
    __asm__ volatile("shadd16 %0, %1, %2" : "=r"(r) : "r"(0x7fff0003), "r"(0x7fff0006));
    CHECK_EQ(r, 0x7fff0004);
    __asm__ volatile("qadd16 %0, %1, %2" : "=r"(r) : "r"(0x7fff8000), "r"(0x0001ffff));
    CHECK_EQ(r, 0x7fff8000);
    __asm__ volatile("ssub16 %0, %1, %2" : "=r"(r) : "r"(0x00050005), "r"(0x00070001));
    CHECK_EQ(r, 0xfffe0004);
    __asm__ volatile("uasx %0, %1, %2" : "=r"(r) : "r"(0x00100010), "r"(0x00030002));
    CHECK_EQ(r, 0x0012000d);
    __asm__ volatile("smmul %0, %1, %2" : "=r"(r) : "r"(0x40000000), "r"(0x40000000));
    CHECK_EQ(r, 0x10000000);
    __asm__ volatile("smmulr %0, %1, %2" : "=r"(r) : "r"(0x7fffffff), "r"(1));
    CHECK_EQ(r, 0);
    __asm__ volatile("smmla %0, %1, %2, %3" : "=r"(r) : "r"(0x40000000), "r"(-0x40000000), "r"(5));
    CHECK_EQ(r, (uint32_t)(5 - 0x10000000));
    __asm__ volatile("smmls %0, %1, %2, %3" : "=r"(r) : "r"(0x40000000), "r"(0x40000000), "r"(5));
    CHECK_EQ(r, (uint32_t)(5 - 0x10000000));
    __asm__ volatile("smuad %0, %1, %2" : "=r"(r) : "r"(0x00020003), "r"(0x00040005));
    CHECK_EQ(r, 23);
    __asm__ volatile("smusdx %0, %1, %2" : "=r"(r) : "r"(0x00020003), "r"(0x00040005));
    CHECK_EQ(r, 3 * 4 - 2 * 5);
    __asm__ volatile("smlad %0, %1, %2, %3" : "=r"(r) : "r"(0x00020003), "r"(0x00040005), "r"(100));
    CHECK_EQ(r, 123);
    uint32_t lo = 1, hi = 2;
    __asm__ volatile("umaal %0, %1, %2, %3" : "+r"(lo), "+r"(hi) : "r"(0xffffffffu), "r"(0xffffffffu));
    CHECK_EQ(((uint64_t)hi << 32) | lo, 0xfffffffe00000004ull);
    lo = 0; hi = 0;
    __asm__ volatile("smlald %0, %1, %2, %3" : "+r"(lo), "+r"(hi) : "r"(0x7fff7fff), "r"(0x7fff7fff));
    CHECK_EQ(((uint64_t)hi << 32) | lo, 2ull * 0x7fff * 0x7fff);
    __asm__ volatile("usad8 %0, %1, %2" : "=r"(r) : "r"(0x01020304), "r"(0x04030201));
    CHECK_EQ(r, 8);
    uint32_t word = 5, status, old;
    __asm__ volatile("1: ldrex %0, [%3]\n add %0, %0, #1\n strex %1, %0, [%3]\n cmp %1, #0\n bne 1b\n ldr %2, [%3]"
                     : "=&r"(r), "=&r"(status), "=&r"(old) : "r"(&word) : "cc", "memory");
    CHECK_EQ(word, 6); CHECK_EQ(status, 0);
    __asm__ volatile("ldrex %0, [%2]\n clrex\n strex %1, %0, [%2]" : "=&r"(r), "=&r"(status) : "r"(&word) : "memory");
    CHECK_EQ(status, 1);
    uint64_t dw = 0x100000001ull;
    __asm__ volatile("1: ldrexd r2, r3, [%1]\n adds r2, r2, #1\n adc r3, r3, #0\n strexd %0, r2, r3, [%1]\n cmp %0, #0\n bne 1b"
                     : "=&r"(status) : "r"(&dw) : "r2", "r3", "cc", "memory");
    CHECK_EQ(dw, 0x100000002ull);
    uint16_t hw = 0xffff;
    __asm__ volatile("ldrexh %0, [%2]\n add %0, %0, #2\n strexh %1, %0, [%2]" : "=&r"(r), "=&r"(status) : "r"(&hw) : "memory");
    CHECK_EQ(hw, 1); CHECK_EQ(status, 0);
    (void)old;
  }
#endif
}
