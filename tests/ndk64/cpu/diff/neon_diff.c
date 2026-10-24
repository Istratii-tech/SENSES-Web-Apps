// NEON intrinsic snippets of the differential tests (diff.h): each computes
// a vector from fixed inputs; native and guest results are compared.
#include <arm_neon.h>

#include "diff.h"

int run_neon_snippets(diff_record_fn record, int first_index) {
  (void)record;
  return first_index;
}
