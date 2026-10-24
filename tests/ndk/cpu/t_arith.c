// Integer arithmetic: 64-bit add/sub/mul/div (carries, EABI helpers on
// armv5te), signed vs unsigned comparisons, overflow (V flag), shifts and
// rotates by variable amounts, bit counting, jump tables.
#include "guest.h"

#define NOINLINE __attribute__((noinline))

static NOINLINE uint64_t add64(uint64_t a, uint64_t b) { return a + b; }
static NOINLINE uint64_t sub64(uint64_t a, uint64_t b) { return a - b; }
static NOINLINE uint64_t mul64(uint64_t a, uint64_t b) { return a * b; }
static NOINLINE int64_t smul32(int32_t a, int32_t b) { return (int64_t)a * b; }
static NOINLINE uint64_t umul32(uint32_t a, uint32_t b) { return (uint64_t)a * b; }
static NOINLINE int64_t smla(int64_t acc, int32_t a, int32_t b) { return acc + (int64_t)a * b; }
static NOINLINE uint32_t mla(uint32_t a, uint32_t b, uint32_t c) { return a * b + c; }
static NOINLINE uint32_t rotr(uint32_t x, unsigned n) { return (x >> n) | (x << ((32 - n) & 31)); }
static NOINLINE int64_t sdiv64(int64_t a, int64_t b) { return a / b; }
static NOINLINE int64_t smod64(int64_t a, int64_t b) { return a % b; }
static NOINLINE uint64_t udiv64(uint64_t a, uint64_t b) { return a / b; }
static NOINLINE uint64_t umod64(uint64_t a, uint64_t b) { return a % b; }
static NOINLINE uint64_t shl64(uint64_t a, unsigned n) { return a << n; }
static NOINLINE uint64_t shr64(uint64_t a, unsigned n) { return a >> n; }
static NOINLINE int64_t sar64(int64_t a, unsigned n) { return a >> n; }

static NOINLINE int classify(int x) {
  switch (x) {
    case 0: return 10;
    case 1: return 21;
    case 2: return 32;
    case 3: return 43;
    case 4: return 54;
    case 5: return 65;
    case 6: return 76;
    case 7: return 87;
    case 8: return 98;
    case 9: return 109;
    default: return -1;
  }
}

// (A saturating-add idiom here crashes Apple clang 15's Thumb-1 backend.)
static NOINLINE int add_ovf(int a, int b, int* r) { return __builtin_add_overflow(a, b, r); }

void test_arith(void) {
  CHECK_EQ(add64(OPAQUE(0xffffffffull), OPAQUE(1ull)), 0x100000000ull);
  CHECK_EQ(add64(0xffffffffffffffffull, 2), 1);
  CHECK_EQ(sub64(0x100000000ull, 1), 0xffffffffull);
  CHECK_EQ(sub64(0, 1), 0xffffffffffffffffull);
  CHECK_EQ(mul64(0x123456789ull, 0xabcdefull), 0xc379aaaa375de7ull);
  CHECK_EQ(mul64(0xdeadbeefcafebabeull, 0x0123456789abcdefull), 0x7eb689f4ea447d62ull);
  CHECK_EQ(smul32(-2, 0x40000000), 0xffffffff80000000ull);
  CHECK_EQ(smul32(-65536, -65536), 0x100000000ull);
  CHECK_EQ(umul32(0xffffffffu, 0xffffffffu), 0xfffffffe00000001ull);
  CHECK_EQ(smla(-1, -3, 5), (uint64_t)-16);
  CHECK_EQ(smla(0x7fffffff, 0x7fffffff, 2), 0x17ffffffdull);
  CHECK_EQ(mla(7, 9, 100), 163);

  CHECK_EQ(OPAQUE(-7) / OPAQUE(2), -3);
  CHECK_EQ(OPAQUE(-7) % OPAQUE(2), -1);
  CHECK_EQ(OPAQUE(0xffffffffu) / OPAQUE(3u), 0x55555555u);
  CHECK_EQ(OPAQUE(1000000007u) % OPAQUE(97u), 1000000007u % 97u);
  CHECK_EQ(OPAQUE((int)0x80000000) / OPAQUE(-2), 0x40000000);
  CHECK_EQ(sdiv64(-1000000000000ll, 7), (uint64_t)-142857142857ll);
  CHECK_EQ(smod64(-1000000000000ll, 7), (uint64_t)-1ll);
  CHECK_EQ(udiv64(0xffffffffffffffffull, 10), 1844674407370955161ull);
  CHECK_EQ(umod64(0xffffffffffffffffull, 10), 5);
  CHECK_EQ(udiv64(0x123456789abcdef0ull, 0x12345ull), 0x123456789abcdef0ull / 0x12345ull);

  CHECK(OPAQUE((int)0x80000000) < OPAQUE(0));
  CHECK(OPAQUE(0x80000000u) > OPAQUE(1u));
  CHECK(OPAQUE(-1) < OPAQUE(1));
  CHECK(!(OPAQUE(0xffffffffu) < OPAQUE(1u)));
  CHECK(OPAQUE(-5ll) < OPAQUE(3ll));
  CHECK(OPAQUE(0x100000000ll) > OPAQUE(0xffffffffll));

  {
    int r;
    CHECK(add_ovf(0x7fffffff, 1, &r));
    CHECK_EQ(r, 0x80000000u);
    CHECK(add_ovf((int)0x80000000, -1, &r));
    CHECK_EQ(r, 0x7fffffff);
    CHECK(!add_ovf(5, -7, &r));
    CHECK_EQ(r, (uint32_t)-2);
  }
  {
    unsigned r;
    CHECK(__builtin_umul_overflow(OPAQUE(0x10000u), OPAQUE(0x10000u), &r));
    CHECK(!__builtin_umul_overflow(OPAQUE(0xffffu), OPAQUE(0x10001u), &r));
    CHECK_EQ(r, 0xffffffffu);
    int s;
    CHECK(__builtin_sub_overflow(OPAQUE((int)0x80000000), OPAQUE(1), &s));
    CHECK(__builtin_mul_overflow(OPAQUE(0x10000), OPAQUE(-0x10000), &s));
    CHECK_EQ(s, 0);
  }

  {
    uint32_t x = OPAQUE(0x80000001u), acc = 0;
    for (unsigned n = 0; n < 32; n++) acc += (x << n) ^ (x >> n) ^ (uint32_t)((int32_t)x >> n);
    uint32_t want = 0;
    for (unsigned n = 0; n < 32; n++) {
      uint32_t l = 0x80000001u, r = 0x80000001u, s = 0x80000001u;
      for (unsigned k = 0; k < n; k++) { l *= 2; r /= 2; s = (s / 2) | (s & 0x80000000u); }
      want += l ^ r ^ s;
    }
    CHECK_EQ(acc, want);
  }
  CHECK_EQ(rotr(0x12345678, 8), 0x78123456);
  CHECK_EQ(rotr(0x12345678, 0), 0x12345678);
  CHECK_EQ(rotr(0x80000001, 1), 0xc0000000);
  {
    uint64_t s = 0, t = 0;
    for (unsigned n = 0; n < 64; n++) {
      s += shr64(0x8000000000000001ull, n) ^ shl64(0x0123456789abcdefull, n);
      t += (uint64_t)sar64(-0x123456789abcdef0ll, n);
    }
    CHECK_EQ(s, 0xf4d15b3a7654320full);
    CHECK_EQ(t, 0xdb97530eca864205ull);
  }

  CHECK_EQ(__builtin_clz(OPAQUE(1u)), 31);
  CHECK_EQ(__builtin_clz(OPAQUE(0x80000000u)), 0);
  CHECK_EQ(__builtin_clz(OPAQUE(0x00010000u)), 15);
  CHECK_EQ(__builtin_ctz(OPAQUE(0x00010000u)), 16);
  CHECK_EQ(__builtin_popcount(OPAQUE(0xf0f0f0f1u)), 17);
  CHECK_EQ(__builtin_bswap32(OPAQUE(0x11223344u)), 0x44332211u);
  CHECK_EQ(__builtin_bswap16(OPAQUE((uint16_t)0xabcd)), 0xcdab);
  CHECK_EQ(__builtin_clzll(OPAQUE(0x0000000100000000ull)), 31);

  {
    int sum = 0;
    for (int i = -2; i < 12; i++) sum += classify(OPAQUE(i));
    CHECK_EQ(sum, 10 + 21 + 32 + 43 + 54 + 65 + 76 + 87 + 98 + 109 - 4);
  }
  {
    uint32_t h = 0x811c9dc5;
    for (int i = 0; i < 1000; i++) {
      h ^= i & 0xff;
      h *= 0x01000193;
      h = (h << 5) | (h >> 27);
      h += i * 7;
      if (h & 1) h ^= 0xa5a5a5a5;
    }
    CHECK_EQ(h, 0xe29c0880u);
  }
  {
    int mn = 0x7fffffff, mx = (int)0x80000000;
    static const int vals[] = {5, -3, 99, -1000, 42, 0, 7};
    for (unsigned i = 0; i < sizeof(vals) / sizeof(vals[0]); i++) {
      int v = OPAQUE(vals[i]);
      mn = v < mn ? v : mn;
      mx = v > mx ? v : mx;
    }
    CHECK_EQ(mn, -1000);
    CHECK_EQ(mx, 99);
  }
}
