// CPU-bound guest benchmark for MIPS measurements (runner --bench N): a
// prime sieve, bitwise CRC-32, insertion sort, an integer matrix product
// and a little soft-float/VFP arithmetic per iteration.
#include "guest.h"

static uint8_t sieve[8192];
static uint8_t data[4096];
static uint32_t sortbuf[256];
static int32_t ma[16][16], mb[16][16], mc[16][16];

static uint32_t crc32(const uint8_t* p, int n) {
  uint32_t crc = 0xffffffff;
  for (int i = 0; i < n; i++) {
    crc ^= p[i];
    for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xedb88320u & -(crc & 1));
  }
  return ~crc;
}

uint32_t bench_main(int n) {
  uint32_t acc = 0, seed = 12345;
  for (int i = 0; i < 4096; i++) data[i] = (uint8_t)(i * 7 + (i >> 3));
  double x = 0.5;
  for (int it = 0; it < n; it++) {
    memset(sieve, 1, sizeof(sieve));
    int primes = 0;
    for (int i = 2; i < 8192; i++) {
      if (!sieve[i]) continue;
      primes++;
      for (int j = i * 2; j < 8192; j += i) sieve[j] = 0;
    }
    acc += primes;
    acc ^= crc32(data, 1024);
    for (int i = 0; i < 256; i++) { seed = seed * 1103515245 + 12345; sortbuf[i] = seed >> 8; }
    for (int i = 1; i < 256; i++) {
      uint32_t v = sortbuf[i];
      int j = i - 1;
      while (j >= 0 && sortbuf[j] > v) { sortbuf[j + 1] = sortbuf[j]; j--; }
      sortbuf[j + 1] = v;
    }
    acc += sortbuf[128];
    for (int i = 0; i < 16; i++)
      for (int j = 0; j < 16; j++) { ma[i][j] = i + j + it; mb[i][j] = i - j; }
    for (int i = 0; i < 16; i++)
      for (int j = 0; j < 16; j++) {
        int32_t s = 0;
        for (int k = 0; k < 16; k++) s += ma[i][k] * mb[k][j];
        mc[i][j] = s;
      }
    acc += mc[3][7];
    for (int i = 0; i < 64; i++) x = x * 1.0001 + 0.25 / (x + 1.0);
  }
  return acc + (uint32_t)x;
}

// Re-entrant variant for the runner's --stress mode (several threads at
// once): everything on the stack.
uint32_t bench_reentrant(int n) {
  uint8_t sv[2048];
  uint32_t buf[64], acc = 0, seed = 99;
  for (int it = 0; it < n; it++) {
    memset(sv, 1, sizeof(sv));
    for (int i = 2; i < 2048; i++)
      if (sv[i]) { acc += i; for (int j = 2 * i; j < 2048; j += i) sv[j] = 0; }
    for (int i = 0; i < 64; i++) { seed = seed * 1103515245 + 12345; buf[i] = seed >> 8; }
    for (int i = 1; i < 64; i++) {
      uint32_t v = buf[i];
      int j = i - 1;
      while (j >= 0 && buf[j] > v) { buf[j + 1] = buf[j]; j--; }
      buf[j + 1] = v;
    }
    acc ^= crc32((const uint8_t*)buf, sizeof(buf)) + buf[31];
    double x = it * 0.5;
    acc += (uint32_t)(x * x / 3.0);
  }
  return acc;
}

// Branchy, call-heavy code like an app's (runner --bench, topic arm-chains):
// a bytecode interpreter's switch, calls through function pointers, small
// functions that are not inlined, recursion, a tree walk.
struct node { uint32_t key; int left, right; };
static struct node tree[512];
static int tree_n;

__attribute__((noinline)) static uint32_t mix(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }
__attribute__((noinline)) static uint32_t op_add(uint32_t a, uint32_t b) { return a + b; }
__attribute__((noinline)) static uint32_t op_xor(uint32_t a, uint32_t b) { return a ^ (b << 3); }
__attribute__((noinline)) static uint32_t op_mul(uint32_t a, uint32_t b) { return a * (b | 1); }
__attribute__((noinline)) static uint32_t op_sub(uint32_t a, uint32_t b) { return a - (b >> 1); }
static uint32_t (*const ops[4])(uint32_t, uint32_t) = {op_add, op_xor, op_mul, op_sub};

__attribute__((noinline)) static uint32_t fib(uint32_t n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

__attribute__((noinline)) static void tree_insert(uint32_t key) {
  if (tree_n >= 512) return;
  int i = tree_n++;
  tree[i].key = key; tree[i].left = tree[i].right = -1;
  if (!i) return;
  int at = 0;
  for (;;) {
    int* next = key < tree[at].key ? &tree[at].left : &tree[at].right;
    if (*next < 0) { *next = i; return; }
    at = *next;
  }
}

__attribute__((noinline)) static int tree_find(uint32_t key) {
  int at = 0, depth = 0;
  while (at >= 0) {
    if (tree[at].key == key) return depth;
    at = key < tree[at].key ? tree[at].left : tree[at].right;
    depth++;
  }
  return -1;
}

// A little stack machine: |code| bytes are opcodes.
__attribute__((noinline)) static uint32_t vm_run(const uint8_t* code, int len, uint32_t x) {
  uint32_t st[16];
  int sp = 0;
  st[sp++] = x;
  for (int pc = 0; pc < len; pc++) {
    uint8_t op = code[pc];
    switch (op & 15) {
      case 0: st[sp++] = op >> 4; break;
      case 1: if (sp > 1) { sp--; st[sp - 1] += st[sp]; } break;
      case 2: if (sp > 1) { sp--; st[sp - 1] ^= st[sp] << 1; } break;
      case 3: st[sp - 1] = mix(st[sp - 1], op); break;
      case 4: if (sp < 15) { st[sp] = st[sp - 1]; sp++; } break;
      case 5: st[sp - 1] = ops[op >> 6](st[sp - 1], op); break;
      case 6: if (st[sp - 1] & 1) pc++; break;
      case 7: st[sp - 1] >>= 1; break;
      case 8: st[sp - 1] = st[sp - 1] * 3 + 1; break;
      case 9: if (sp > 1) sp--; break;
      case 10: st[sp - 1] += (uint32_t)tree_find(st[sp - 1] & 1023); break;
      default: st[sp - 1] = ~st[sp - 1]; break;
    }
    if (sp >= 15) sp = 1;
  }
  return st[sp - 1];
}

uint32_t bench_calls(int n) {
  static uint8_t code[256];
  uint32_t acc = 7, seed = 4242;
  for (int i = 0; i < 256; i++) { seed = seed * 1103515245 + 12345; code[i] = (uint8_t)(seed >> 16); }
  tree_n = 0;
  for (int i = 0; i < 512; i++) { seed = seed * 1103515245 + 12345; tree_insert((seed >> 8) & 1023); }
  for (int it = 0; it < n; it++) {
    acc = vm_run(code, 256, acc + it);
    for (int i = 0; i < 64; i++) acc = ops[(acc >> 7) & 3](acc, (uint32_t)i);
    acc += fib(12 + (it & 1));
    for (int i = 0; i < 32; i++) acc += (uint32_t)tree_find((acc + i * 37) & 1023);
  }
  return acc;
}
