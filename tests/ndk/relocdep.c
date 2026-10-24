/* libndkrelocdep.so (topic relocs32): the dependency of libndkreloc.so, built
 * like a modern armeabi-v7a NDK library (mk/topic-relocs32.mk): packed
 * relocations, GNU hash only, symbol versions (relocdep.map), ELF TLS.
 * Exports data with pointers in it, a thread-local variable and one
 * function in two versions: ver_value@VERS_1 (hidden, the old one) and
 * ver_value@@VERS_2 (the default). */
#include <string.h>

/* Two versions of one name. */
int ver_value_v1(void) { return 1; }
int ver_value_v2(void) { return 2; }
__asm__(".symver ver_value_v1, ver_value@VERS_1");
__asm__(".symver ver_value_v2, ver_value@@VERS_2");

/* Data that others point into (an addend in the relocated word). */
const char dep_letters[] = "abcdefghijklmnopqrstuvwxyz";
int dep_numbers[16] = {0, 1, 4, 9, 16, 25, 36, 49, 64, 81, 100, 121, 144, 169, 196, 225};

/* Exported pointers: relative relocations (RELR) and imports (APS2). */
const char* const dep_words[] = {"alpha", "beta", "gamma", "delta"};
size_t (*const dep_strlen)(const char*) = strlen;

/* ELF TLS another library uses (R_ARM_TLS_DTPMOD32/DTPOFF32 with a symbol). */
__thread int dep_tls = 5;

int dep_tls_get(void) { return dep_tls; }
void dep_tls_set(int v) { dep_tls = v; }
