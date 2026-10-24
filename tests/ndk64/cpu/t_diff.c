// The differential cases (diff/diff.h) in the guest: every case's outputs
// go to the host, which compares them with the native run.
#include "guest.h"
#include "diff.h"

static void record(int index, const char* name, const uint64_t* out, int nwords) {
  host.compare_case(index, name, out, nwords);
}

void test_diff(void) { run_diff(record); }
