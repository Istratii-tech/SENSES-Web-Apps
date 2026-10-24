// The reference run of the differential tests (diff.h) on the Mac's own
// arm64 CPU: writes every case's outputs to the golden file the guest run
// is compared with.
//
//   diff_native OUT.bin
#include <stdio.h>
#include <stdlib.h>

#include "diff.h"

static FILE* g_out;
static int g_count;

static void record(int index, const char* name, const uint64_t* out, int nwords) {
  (void)name;
  uint32_t head[2] = {(uint32_t)index, (uint32_t)nwords};
  fwrite(head, sizeof head, 1, g_out);
  fwrite(out, sizeof(uint64_t), nwords, g_out);
  g_count++;
}

int main(int argc, char** argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s OUT.bin\n", argv[0]);
    return 2;
  }
  g_out = fopen(argv[1], "wb");
  if (!g_out) {
    perror(argv[1]);
    return 1;
  }
  fwrite("A64D", 4, 1, g_out);
  run_diff(record);
  fclose(g_out);
  printf("%d reference results in %s\n", g_count, argv[1]);
  return 0;
}
