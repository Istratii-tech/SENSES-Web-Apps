// Loads and stores: all sizes, sign extension, unaligned accesses (as on
// ARMv6+), writeback addressing, LDM/STM from struct copies, LDRD/STRD,
// big stack frames.
#include "guest.h"

#define NOINLINE __attribute__((noinline))

struct __attribute__((packed)) packed {
  uint8_t a;
  uint32_t b;
  uint16_t c;
  uint64_t d;
};

struct big { uint32_t w[12]; };

static NOINLINE void copy_big(struct big* dst, const struct big* src) { *dst = *src; }
static NOINLINE uint32_t sum_down(const uint32_t* p, int n) {
  uint32_t s = 0;
  p += n;
  while (n--) s += *--p;
  return s;
}
static NOINLINE void fill_up(uint16_t* p, int n) {
  for (int i = 0; i < n; i++) *p++ = (uint16_t)(i * 3);
}
static NOINLINE uint32_t load_unaligned32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static NOINLINE uint16_t load_unaligned16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }
static NOINLINE uint64_t load64(const uint64_t* p, int i) { return p[i]; }
static NOINLINE void store64(uint64_t* p, int i, uint64_t v) { p[i] = v; }

static NOINLINE uint32_t deep_frame(int n) {
  volatile uint32_t buf[2048];
  for (int i = 0; i < 2048; i++) buf[i] = i * n;
  uint32_t s = 0;
  for (int i = 0; i < 2048; i += 7) s += buf[i];
  return s;
}

void test_memory(void) {
  static volatile int8_t s8 = -1;
  static volatile uint8_t u8 = 255;
  static volatile int16_t s16 = -2;
  static volatile uint16_t u16 = 0xfffe;
  static volatile int32_t s32 = -3;
  CHECK_EQ((int32_t)s8, (uint32_t)-1);
  CHECK_EQ((int32_t)u8, 255);
  CHECK_EQ((int32_t)s16, (uint32_t)-2);
  CHECK_EQ((int32_t)u16, 0xfffe);
  CHECK_EQ(s32, (uint32_t)-3);

  uint8_t bytes[32];
  for (int i = 0; i < 32; i++) bytes[i] = (uint8_t)(i * 17 + 1);
  for (int off = 0; off < 4; off++) {
    uint32_t want = bytes[off + 4] | bytes[off + 5] << 8 | bytes[off + 6] << 16 | (uint32_t)bytes[off + 7] << 24;
    CHECK_EQ(load_unaligned32(OPAQUE(bytes + off + 4)), want);
    CHECK_EQ(load_unaligned16(OPAQUE(bytes + off + 1)), bytes[off + 1] | bytes[off + 2] << 8);
  }
  {
    static struct packed pk;
    struct packed* p = OPAQUE(&pk);
    p->a = 1; p->b = 0xdeadbeef; p->c = 0x1234; p->d = 0x0102030405060708ull;
    CHECK_EQ(p->b, 0xdeadbeef);
    CHECK_EQ(p->c, 0x1234);
    CHECK_EQ(p->d, 0x0102030405060708ull);
    uint8_t* raw = (uint8_t*)p;
    CHECK_EQ(raw[1], 0xef);
    CHECK_EQ(raw[4], 0xde);
    CHECK_EQ(raw[7], 0x08);
  }
  {
    struct big a, b;
    for (int i = 0; i < 12; i++) a.w[i] = 0x1000 + i;
    copy_big(&b, &a);
    CHECK_EQ(b.w[0], 0x1000);
    CHECK_EQ(b.w[11], 0x100b);
    CHECK(memcmp(&a, &b, sizeof(a)) == 0);
    CHECK_EQ(sum_down(b.w, 12), 12 * 0x1000 + 66);
  }
  {
    uint16_t h[9];
    fill_up(h, 9);
    CHECK_EQ(h[0], 0);
    CHECK_EQ(h[8], 24);
  }
  {
    uint64_t q[4] = {0};
    store64(q, 1, 0x1122334455667788ull);
    store64(q, 3, 0x99aabbccddeeff00ull);
    CHECK_EQ(load64(q, 1), 0x1122334455667788ull);
    CHECK_EQ(load64(q, 3), 0x99aabbccddeeff00ull);
    CHECK_EQ(((uint32_t*)q)[2], 0x55667788u);
    CHECK_EQ(load64(q, 0), 0);
  }
  {
    char buf[64];
    snprintf(buf, sizeof(buf), "%d|%u|%x|%s|%c|%5d|%-4s|%lld", -42, 42u, 0xbeefu, "str", 'z', 7, "ab",
             -1234567890123ll);
    CHECK(strcmp(buf, "-42|42|beef|str|z|    7|ab  |-1234567890123") == 0);
  }
  CHECK_EQ(deep_frame(3), deep_frame(1) * 3);
  {
    int arr[10];
    for (int i = 0; i < 10; i++) arr[i] = i;
    int* p = OPAQUE(arr + 9);
    int s = 0;
    for (int i = 0; i < 10; i++) s += p[-i] * (i + 1);
    CHECK_EQ(s, 165);
  }
}
