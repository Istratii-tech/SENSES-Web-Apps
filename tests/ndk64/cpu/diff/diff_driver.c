// Runs the differential cases (diff.h): deterministic inputs per case, the
// case through run_case, the selected outputs to a record function. The
// same file is built natively (reference) and for the guest.
#include "diff.h"

#ifdef DIFF_NATIVE
#include <string.h>
#else
#include "guest.h"
#endif

static uint64_t rnd(uint64_t* s) {
  uint64_t x = *s;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  *s = x;
  return x * 0x2545f4914f6cdd1dull;
}

static uint64_t interesting_int(uint64_t* s) {
  uint64_t r = rnd(s);
  switch (r & 31) {
    case 0: return 0;
    case 1: return 1;
    case 2: return ~0ull;
    case 3: return 0x7fffffffull;
    case 4: return 0x80000000ull;
    case 5: return 0xffffffffull;
    case 6: return 0x7fffffffffffffffull;
    case 7: return 0x8000000000000000ull;
    case 8: return (r >> 32) & 0xff;
    case 9: return 0ull - ((r >> 32) & 0xff);
    case 10: return 0x100000000ull;
    case 11: return (r >> 40) & 0xffff;
    case 12: return 0xffffffff00000000ull | (r >> 48);
    case 13: return 0x80ull;
    case 14: return 0x8000ull;
    default: return rnd(s);
  }
}

static uint32_t interesting_f32(uint64_t* s) {
  static const uint32_t k[] = {
      0x00000000, 0x80000000, 0x3f800000, 0xbf800000, 0x3f000000, 0x3fc00000, 0x40200000, 0xc0200000,
      0x7f800000, 0xff800000, 0x7fc00000, 0x7fc12345, 0xffc00001, 0x7f800001, 0xff812345, 0x00000001,
      0x007fffff, 0x00800000, 0x7f7fffff, 0xff7fffff, 0x4f32d05e, 0xcf32d05e, 0x60ad78ec, 0x3effffff,
      0x4f000000, 0x4f800000, 0xcf000000, 0x5f000000, 0x3eaaaaab, 0x40490fdb, 0x80000001, 0x3f7fffff,
  };
  uint64_t r = rnd(s);
  if ((r & 3) != 0) return k[(r >> 8) % (sizeof k / sizeof k[0])];
  if (r & 4) return (uint32_t)(r >> 32);
  // A value with a small integer part and a fraction (rounding cases).
  int32_t i = (int32_t)((r >> 32) % 2001) - 1000;
  uint32_t f = (uint32_t)((r >> 16) & 3);
  float v = (float)i + (f == 0 ? 0.5f : f == 1 ? 0.25f : f == 2 ? 0.75f : 0.0f);
  uint32_t b;
  memcpy(&b, &v, 4);
  return b;
}

static uint64_t interesting_f64(uint64_t* s) {
  static const uint64_t k[] = {
      0x0000000000000000ull, 0x8000000000000000ull, 0x3ff0000000000000ull, 0xbff0000000000000ull,
      0x3fe0000000000000ull, 0x3ff8000000000000ull, 0x4004000000000000ull, 0xc004000000000000ull,
      0x7ff0000000000000ull, 0xfff0000000000000ull, 0x7ff8000000000000ull, 0x7ff8000012345678ull,
      0xfff8000000000001ull, 0x7ff0000000000001ull, 0xfff0000012345678ull, 0x0000000000000001ull,
      0x000fffffffffffffull, 0x0010000000000000ull, 0x7fefffffffffffffull, 0xffefffffffffffffull,
      0x41e0000000000000ull, 0xc1e0000000000000ull, 0x41f0000000000000ull, 0x43e0000000000000ull,
      0x43f0000000000000ull, 0xc3e0000000000000ull, 0x3fdfffffffffffffull, 0x400921fb54442d18ull,
      0x36a0000000000000ull, 0x47efffffe0000000ull, 0x3810000000000000ull, 0x3e7ad7f29abcaf48ull,
  };
  uint64_t r = rnd(s);
  if ((r & 3) != 0) return k[(r >> 8) % (sizeof k / sizeof k[0])];
  if (r & 4) return rnd(s);
  int64_t i = (int64_t)((r >> 32) % 2001) - 1000;
  uint32_t f = (uint32_t)((r >> 16) & 3);
  double v = (double)i + (f == 0 ? 0.5 : f == 1 ? 0.25 : f == 2 ? 0.75 : 0.0);
  uint64_t b;
  memcpy(&b, &v, 8);
  return b;
}

static uint16_t interesting_f16(uint64_t* s) {
  static const uint16_t k[] = {0x0000, 0x8000, 0x3c00, 0xbc00, 0x3800, 0x3e00, 0x7c00, 0xfc00, 0x7e00, 0x7e55,
                               0xfe01, 0x7c01, 0x0001, 0x03ff, 0x0400, 0x7bff, 0xfbff, 0x4248, 0x5640, 0x3555};
  uint64_t r = rnd(s);
  if ((r & 3) != 0) return k[(r >> 8) % (sizeof k / sizeof k[0])];
  return (uint16_t)(r >> 32);
}

static void fill_lane_ints(uint64_t* v, uint64_t* s) {
  // Random bytes, sometimes with an edge value in a lane.
  v[0] = rnd(s);
  v[1] = rnd(s);
  uint64_t r = rnd(s);
  if (r & 1) {
    static const uint64_t edges[] = {0, ~0ull, 0x8080808080808080ull, 0x7f7f7f7f7f7f7f7full, 0x8000800080008000ull,
                                     0x7fff7fff7fff7fffull, 0x8000000080000000ull, 0x7fffffff7fffffffull,
                                     0x8000000000000000ull, 0x0101010101010101ull, 0x00ff00ff00ff00ffull};
    v[(r >> 1) & 1] = edges[(r >> 8) % (sizeof edges / sizeof edges[0])];
  }
}

static void prepare(uint64_t* io, const struct diff_case* c, int index) {
  uint64_t s = 0x9e3779b97f4a7c15ull ^ ((uint64_t)(index + 1) * 0xbf58476d1ce4e5b9ull);
  for (int i = 0; i < 8; i++) rnd(&s);
  for (int i = 0; i < 28; i++) io[IO_X + i] = interesting_int(&s);
  io[18] = 0;
  io[IO_NZCV] = (rnd(&s) & 0xf) << 28;
  io[IO_FPCR] = 0;
  if (c->cls & CL_FPCR) {
    uint64_t r = rnd(&s);
    io[IO_FPCR] = ((r & 3) << 22) | (((r >> 2) & 3) == 0 ? (1u << 25) : 0);
  }
  io[IO_FPSR] = 0;
  for (int v = 0; v < 32; v++) {
    uint64_t* w = &io[IO_V + 2 * v];
    switch (c->cls & 0xf) {
      case CL_F32:
        w[0] = interesting_f32(&s) | (uint64_t)interesting_f32(&s) << 32;
        w[1] = interesting_f32(&s) | (uint64_t)interesting_f32(&s) << 32;
        break;
      case CL_F64:
        w[0] = interesting_f64(&s);
        w[1] = interesting_f64(&s);
        break;
      case CL_F16:
        w[0] = interesting_f16(&s) | (uint64_t)interesting_f16(&s) << 16 | (uint64_t)interesting_f16(&s) << 32 |
               (uint64_t)interesting_f16(&s) << 48;
        w[1] = interesting_f16(&s) | (uint64_t)interesting_f16(&s) << 16 | (uint64_t)interesting_f16(&s) << 32 |
               (uint64_t)interesting_f16(&s) << 48;
        break;
      default:
        fill_lane_ints(w, &s);
    }
  }
  for (int i = 0; i < 32; i++) io[IO_MEM + i] = rnd(&s);
  uint64_t mem = (uint64_t)(uintptr_t)(io + IO_MEM);
  for (int k = 0; k < c->nsetup; k++) {
    const struct diff_setup* su = &c->setup[k];
    switch (su->kind) {
      case SU_ADDR: io[su->reg] = mem + su->value; break;
      case SU_IMM: io[su->reg] = (uint64_t)(int64_t)su->value; break;
      case SU_MASK: io[su->reg] &= (uint64_t)(uint16_t)su->value; break;
      case SU_VMASK: {
        uint64_t m = (uint64_t)(uint8_t)su->value * 0x0101010101010101ull;
        io[IO_V + 2 * su->reg] &= m;
        io[IO_V + 2 * su->reg + 1] &= m;
        break;
      }
    }
  }
}

void run_diff(diff_record_fn record) {
  static uint64_t io[IO_WORDS] __attribute__((aligned(64)));
  static uint64_t out[IO_WORDS + 8];
  for (int i = 0; i < diff_case_count; i++) {
    const struct diff_case* c = &diff_cases[i];
    prepare(io, c, i);
    uint64_t mem = (uint64_t)(uintptr_t)(io + IO_MEM);
    run_case(io, c->fn);
    int n = 0;
    for (int r = 0; r < 28; r++) {
      if (!(c->out_x & (1u << r))) continue;
      uint64_t v = io[IO_X + r];
      for (int k = 0; k < c->nsetup; k++)
        if (c->setup[k].kind == SU_ADDR && c->setup[k].reg == r) v -= mem;
      out[n++] = v;
    }
    for (int r = 0; r < 32; r++) {
      if (!(c->out_v & (1u << r))) continue;
      out[n++] = io[IO_V + 2 * r];
      out[n++] = io[IO_V + 2 * r + 1];
    }
    if (c->flags & DF_NZCV) out[n++] = io[IO_NZCV] & 0xf0000000u;
    if (c->flags & DF_QC) out[n++] = (io[IO_FPSR] >> 27) & 1;
    if (c->flags & DF_MEM)
      for (int k = 0; k < 32; k++) out[n++] = io[IO_MEM + k];
    record(i, c->name, out, n);
  }
  run_neon_snippets(record, diff_case_count);
}
