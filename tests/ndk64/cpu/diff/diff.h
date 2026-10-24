// Differential tests of the AArch64 CPU (topic "ndk-arm64"): generated
// cases (tools/ndk64/gen_a64_cases.py) and NEON intrinsic snippets run once
// natively on the Mac's own arm64 CPU, which writes the reference results,
// and once as guest code on the emulated CPU, whose results the test runner
// compares with them.
//
// A case is a short instruction sequence ending in ret. run_case
// (rt/guest_asm.S) loads every register from an io buffer, runs it and
// stores every register back.
#ifndef NDK_CPU64_DIFF_H_
#define NDK_CPU64_DIFF_H_

#include <stdint.h>

// io buffer layout, in 64-bit words.
enum {
  IO_X = 0,       // x0-x27 (x18 unused)
  IO_NZCV = 32,
  IO_FPCR = 33,
  IO_FPSR = 34,
  IO_V = 36,      // v0-v31, two words each
  IO_MEM = 100,   // 256 bytes of scratch memory for loads and stores
  IO_WORDS = 132,
};

// What a case's outputs include besides out_x/out_v.
enum { DF_NZCV = 1, DF_QC = 2, DF_MEM = 4, DF_FPSR_ALL = 8 };
// Input classes.
enum {
  CL_INT = 0,     // x: interesting integers; v: random bytes with integer edge values per lane
  CL_F32 = 1,     // v lanes: interesting single-precision values
  CL_F64 = 2,     // v lanes: interesting doubles
  CL_F16 = 3,     // v lanes: interesting halves
  CL_FPCR = 0x10, // also a random rounding mode / default-NaN in FPCR
};
// Setups applied after the random fill.
enum { SU_ADDR = 1, SU_IMM = 2, SU_MASK = 3, SU_VMASK = 4 };

struct diff_setup {
  uint8_t reg, kind;
  int16_t value;
};
struct diff_case {
  void (*fn)(void);
  const char* name;
  uint32_t out_x, out_v;
  uint8_t flags, cls, nsetup, pad;
  struct diff_setup setup[4];
};

extern const struct diff_case diff_cases[];
extern const int diff_case_count;

// Runs every case (and the intrinsic snippets of neon_diff.c), passing each
// one's outputs to |record|.
typedef void (*diff_record_fn)(int index, const char* name, const uint64_t* out, int nwords);
void run_diff(diff_record_fn record);
// The intrinsic snippets (neon_diff.c), indices after the generated cases.
int run_neon_snippets(diff_record_fn record, int first_index);

void run_case(uint64_t* io, void (*fn)(void));

#endif  // NDK_CPU64_DIFF_H_
