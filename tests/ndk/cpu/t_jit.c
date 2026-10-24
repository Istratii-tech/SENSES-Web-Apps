// Code the program writes at run time, as JITs do (topic guest-jit,
// docs/topics/guest-jit.md): ARM and Thumb code written into memory,
// flushed with the cacheflush syscall (ARM Linux's __clear_cache), run,
// rewritten and run again -- hot code too (translated to wasm after 50
// runs, or at once with --jit 1); a word rewritten in the middle of a
// decoded block, a patched branch, and code that changes the instruction
// right after its own flush (an SVC ends the block: what follows is
// fetched anew).
#include "guest.h"

static uint32_t code[1024] __attribute__((aligned(4096)));

// code[0..5]: flush(start, end), itself generated (the first run needs no
// flush: nothing was decoded there yet). ARM code; the C caller may be Thumb.
static const uint32_t kFlush[] = {
    0xe92d4080,   // push {r7, lr}
    0xe3a02000,   // mov r2, #0
    0xe3a0780f,   // mov r7, #0xf0000
    0xe3877002,   // orr r7, r7, #2         (__ARM_NR_cacheflush)
    0xef000000,   // svc 0
    0xe8bd8080,   // pop {r7, pc}
};
typedef void (*flush_fn)(uint32_t, uint32_t);
typedef int (*fn0)(void);
typedef int (*fn1)(int);
typedef int (*fn2)(uint32_t, uint32_t);

// (Opaque: the compiler must not turn a call through code[] into a direct
// BL to the array, which would stay in the caller's state.)
static uint32_t A(const void* p) { return OPAQUE((uint32_t)(uintptr_t)p); }
static void flush(const void* p, int bytes) {
  ((flush_fn)(uintptr_t)A(code))(A(p), A(p) + bytes);
}
// ARM B from code[from] to code[to].
static uint32_t branch(int from, int to) {
  return 0xea000000u | ((uint32_t)(to - from - 2) & 0xffffff);
}

void test_jit(void) {
  memcpy(code, kFlush, sizeof(kFlush));

  // ARM: mov r0, #k; bx lr -- written, flushed, run, 200 times.
  uint32_t* f = &code[64];
  int ok = 1;
  for (int k = 0; k < 200; k++) {
    f[0] = 0xe3a00000u | k;
    f[1] = 0xe12fff1e;
    flush(f, 8);
    if (((fn0)(uintptr_t)A(f))() != k) ok = 0;
  }
  CHECK(ok);

  // Thumb: movs r0, #k; bx lr.
  uint16_t* t = (uint16_t*)&code[128];
  ok = 1;
  for (int k = 0; k < 200; k++) {
    t[0] = (uint16_t)(0x2000 | k);
    t[1] = 0x4770;
    flush(t, 4);
    if (((fn0)(uintptr_t)(A(t) | 1))() != k) ok = 0;
  }
  CHECK(ok);

  // A word after a conditional branch, inside the decoded block: only that
  // word is rewritten and flushed.
  uint32_t* g = &code[192];
  g[0] = 0xe3500000;   // cmp r0, #0
  g[1] = 0x0a000001;   // beq g+4
  g[2] = 0xe3a00001;   // mov r0, #1
  g[3] = 0xe12fff1e;   // bx lr
  g[4] = 0xe3a00002;   // mov r0, #2
  g[5] = 0xe12fff1e;   // bx lr
  flush(g, 24);
  int sum = 0;
  for (int i = 0; i < 100; i++) sum += ((fn1)(uintptr_t)A(g))(i & 1);   // hot: translated
  CHECK_EQ(sum, 50 * 1 + 50 * 2);
  g[2] = 0xe3a00005;   // mov r0, #5
  flush(&g[2], 4);
  CHECK_EQ(((fn1)(uintptr_t)A(g))(1), 5);
  CHECK_EQ(((fn1)(uintptr_t)A(g))(0), 2);
  // The same bytes again: the flush keeps the block; the result is the same.
  flush(g, 24);
  CHECK_EQ(((fn1)(uintptr_t)A(g))(1), 5);

  // A patched branch (a JIT linking code): b L1 <-> b L2.
  uint32_t* b = &code[256];
  b[0] = branch(0, 2);
  b[1] = 0xe7f000f0;   // udf (never reached)
  b[2] = 0xe3a0000a;   // L1: mov r0, #10
  b[3] = 0xe12fff1e;
  b[4] = 0xe3a00014;   // L2: mov r0, #20
  b[5] = 0xe12fff1e;
  flush(b, 24);
  ok = 1;
  for (int i = 0; i < 120; i++) {
    b[0] = branch(0, (i & 1) ? 4 : 2);
    flush(b, 4);
    if (((fn0)(uintptr_t)A(b))() != ((i & 1) ? 20 : 10)) ok = 0;
  }
  CHECK(ok);

  // Code that rewrites the instruction after its own cacheflush: f(word, x)
  // stores |word| at x, flushes [x, x + 4) and runs on into x.
  uint32_t* s = &code[320];
  static const uint32_t kSelf[] = {
      0xe92d4080,   // push {r7, lr}
      0xe5810000,   // str r0, [r1]
      0xe1a00001,   // mov r0, r1
      0xe2811004,   // add r1, r1, #4
      0xe3a02000,   // mov r2, #0
      0xe3a0780f,   // mov r7, #0xf0000
      0xe3877002,   // orr r7, r7, #2
      0xef000000,   // svc 0
      0xe3a00001,   // x: mov r0, #1
      0xe8bd8080,   // pop {r7, pc}
  };
  memcpy(s, kSelf, sizeof(kSelf));
  flush(s, sizeof(kSelf));
  CHECK_EQ(((fn2)(uintptr_t)A(s))(0xe3a00001, A(&s[8])), 1);
  CHECK_EQ(((fn2)(uintptr_t)A(s))(0xe3a00007, A(&s[8])), 7);
  CHECK_EQ(((fn2)(uintptr_t)A(s))(0xe3a00009, A(&s[8])), 9);
}
