/* libndkhelper.so: a second NDK library that libndktest.so links against
 * (DT_NEEDED), for symbol binding between guest libraries: a function, a
 * data object and a constructor that runs before libndktest's. */

#include <string.h>

int helper_counter = 0;
static char helper_text[32];

__attribute__((constructor)) static void helper_init(void) {
  helper_counter += 40;
  strcpy(helper_text, "helper");
}

int helper_add(int a, int b) {
  helper_counter++;
  return a + b;
}

const char* helper_name(void) { return helper_text; }

/* 64-bit and double arguments across a guest-to-guest call. */
long long helper_mix(int a, long long b, double c) { return a + b + (long long)(c * 2); }
