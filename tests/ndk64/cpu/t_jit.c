// Code the program writes at run time, as JITs do (topic guest-jit,
// docs/topics/guest-jit.md): A64 code written into memory, made visible
// the architectural way (DC CVAU, DSB, IC IVAU by the line sizes CTR_EL0
// gives, DSB, ISB: what __clear_cache does on arm64 Linux), run,
// rewritten and run again -- hot code too; a word rewritten inside a
// decoded block, a patched branch, and code that changes the instruction
// right after its own IC IVAU + ISB.
#include "guest.h"

static uint32_t code[1024] __attribute__((aligned(4096)));

typedef int (*fn0)(void);
typedef int (*fn1)(int);
typedef int (*fn2)(uint32_t, uint64_t);
// (Opaque: no direct BL to the array from the compiler.)
#define P(p) ((uintptr_t)OPAQUE((uintptr_t)(p)))

static void flush(const void* p, int bytes) {
  uint64_t ctr;
  __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
  const uintptr_t dline = 4u << ((ctr >> 16) & 15), iline = 4u << (ctr & 15);
  const uintptr_t start = (uintptr_t)p, end = start + bytes;
  for (uintptr_t a = start & ~(dline - 1); a < end; a += dline) __asm__ volatile("dc cvau, %0" ::"r"(a) : "memory");
  __asm__ volatile("dsb ish" ::: "memory");
  for (uintptr_t a = start & ~(iline - 1); a < end; a += iline) __asm__ volatile("ic ivau, %0" ::"r"(a) : "memory");
  __asm__ volatile("dsb ish\n\tisb" ::: "memory");
}
static uint32_t movz_w0(int k) { return 0x52800000u | ((uint32_t)k << 5); }
static const uint32_t kRet = 0xd65f03c0;
// B from code[from] to code[to].
static uint32_t branch(int from, int to) { return 0x14000000u | ((uint32_t)(to - from) & 0x3ffffff); }

void test_jit(void) {
  // movz w0, #k; ret -- written, flushed, run, 200 times.
  uint32_t* f = &code[64];
  int ok = 1;
  for (int k = 0; k < 200; k++) {
    f[0] = movz_w0(k);
    f[1] = kRet;
    flush(f, 8);
    if (((fn0)P(f))() != k) ok = 0;
  }
  CHECK(ok);

  // A word after a conditional branch, inside the decoded block.
  uint32_t* g = &code[128];
  g[0] = 0x34000060;   // cbz w0, g+3
  g[1] = movz_w0(1);
  g[2] = kRet;
  g[3] = movz_w0(2);
  g[4] = kRet;
  flush(g, 20);
  int sum = 0;
  for (int i = 0; i < 100; i++) sum += ((fn1)P(g))(i & 1);   // hot: translated
  CHECK_EQ(sum, 50 * 1 + 50 * 2);
  g[1] = movz_w0(5);
  flush(&g[1], 4);
  CHECK_EQ(((fn1)P(g))(1), 5);
  CHECK_EQ(((fn1)P(g))(0), 2);
  flush(g, 20);   // the same bytes: the block stays, the result too
  CHECK_EQ(((fn1)P(g))(1), 5);

  // A patched branch: b L1 <-> b L2.
  uint32_t* b = &code[192];
  b[0] = branch(0, 2);
  b[1] = 0;            // udf (never reached)
  b[2] = movz_w0(10);  // L1
  b[3] = kRet;
  b[4] = movz_w0(20);  // L2
  b[5] = kRet;
  flush(b, 24);
  ok = 1;
  for (int i = 0; i < 120; i++) {
    b[0] = branch(0, (i & 1) ? 4 : 2);
    flush(b, 4);
    if (((fn0)P(b))() != ((i & 1) ? 20 : 10)) ok = 0;
  }
  CHECK(ok);

  // f(word, x): stores |word| at x, cleans and invalidates that line, ISB,
  // and runs on into x.
  uint32_t* s = &code[256];
  static const uint32_t kSelf[] = {
      0xb9000020,   // str w0, [x1]
      0xd50b7b21,   // dc cvau, x1
      0xd5033b9f,   // dsb ish
      0xd50b7521,   // ic ivau, x1
      0xd5033b9f,   // dsb ish
      0xd5033fdf,   // isb
      0x52800020,   // x: movz w0, #1
      0xd65f03c0,   // ret
  };
  memcpy(s, kSelf, sizeof(kSelf));
  flush(s, sizeof(kSelf));
  CHECK_EQ(((fn2)P(s))(movz_w0(1), (uintptr_t)&s[6]), 1);
  CHECK_EQ(((fn2)P(s))(movz_w0(7), (uintptr_t)&s[6]), 7);
  CHECK_EQ(((fn2)P(s))(movz_w0(9), (uintptr_t)&s[6]), 9);
}
