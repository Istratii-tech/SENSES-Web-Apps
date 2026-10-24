/* libndkreloc.so (topic relocs32): an armeabi-v7a library built the way new
 * NDK builds are (mk/topic-relocs32.mk): lld --pack-dyn-relocs=android+relr
 * (APS2 packed REL relocations in DT_ANDROID_REL, relative ones as RELR),
 * GNU hash only, symbol versions, ELF TLS (API 29, no emulated TLS). The
 * natives of org.arcwasm.ndktest.RelocLib check that every kind of
 * relocated word is right: tables of local pointers (RELR runs and
 * bitmaps), imported functions and data with addends (APS2 groups), a
 * preemptible export, versioned imports, TLS in all models across threads.
 * Each returns "ok" or what went wrong. */
#include <dlfcn.h>
#include <elf.h>
#include <jni.h>
#include <link.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DT_ANDROID_REL
#define DT_ANDROID_REL 0x6000000f
#endif
#ifndef DT_ANDROID_RELR
#define DT_ANDROID_RELR 0x6fffe000
#endif
#ifndef DT_RELR
#define DT_RELR 36
#endif

static char g_err[256];
#define FAIL(...) (snprintf(g_err, sizeof g_err, __VA_ARGS__), g_err)

/* --- from libndkrelocdep.so ------------------------------------------------------------- */
extern const char dep_letters[];
extern int dep_numbers[16];
extern const char* const dep_words[];
extern size_t (*const dep_strlen)(const char*);
extern __thread int dep_tls;
int dep_tls_get(void);
void dep_tls_set(int v);
int ver_value(void);       /* links to the default, ver_value@@VERS_2 */
int ver_value_old(void);   /* the hidden ver_value@VERS_1 */
__asm__(".symver ver_value_old, ver_value@VERS_1");

/* --- local pointers: RELR -------------------------------------------------------------- */
#define F(t, d) static int f##t##d(int x) { return x * (t * 10 + d + 1) + t * 10 + d; }
#define LIST10(M, t) M(t, 0) M(t, 1) M(t, 2) M(t, 3) M(t, 4) M(t, 5) M(t, 6) M(t, 7) M(t, 8) M(t, 9)
LIST10(F, 0) LIST10(F, 1) LIST10(F, 2) LIST10(F, 3)
#undef F
#define P(t, d) f##t##d,
/* 40 consecutive words: one RELR address, then a bitmap. */
static int (*const kFuncs[])(int) = {LIST10(P, 0) LIST10(P, 1) LIST10(P, 2) LIST10(P, 3)};
#undef P

static const char* const kNames[] = {
    "zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine",
    "ten", "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen", "seventeen",
    "eighteen", "nineteen", "twenty", "twenty-one", "twenty-two", "twenty-three",
    "twenty-four", "twenty-five", "twenty-six", "twenty-seven", "twenty-eight",
    "twenty-nine", "thirty", "thirty-one", "thirty-two", "thirty-three", "thirty-four",
    "thirty-five", "thirty-six", "thirty-seven", "thirty-eight", "thirty-nine",
    "forty", "forty-one", "forty-two", "forty-three", "forty-four", "forty-five",
    "forty-six", "forty-seven", "forty-eight", "forty-nine", "fifty", "fifty-one",
    "fifty-two", "fifty-three", "fifty-four", "fifty-five", "fifty-six", "fifty-seven",
    "fifty-eight", "fifty-nine", "sixty", "sixty-one", "sixty-two", "sixty-three",
    "sixty-four", "sixty-five", "sixty-six", "sixty-seven", "sixty-eight", "sixty-nine",
};
#define NNAMES (int)(sizeof kNames / sizeof kNames[0])

/* Pointers with gaps (one word in three): RELR bitmaps with holes; a
 * linked list through a writable array. */
struct Node {
  const char* name;
  int value;
  struct Node* next;
};
#define N(i, nx) {"node" #i, i * 7, nx},
static struct Node nodes[12] = {
    N(0, &nodes[1]) N(1, &nodes[2]) N(2, &nodes[3]) N(3, &nodes[4]) N(4, &nodes[5])
    N(5, &nodes[6]) N(6, &nodes[7]) N(7, &nodes[8]) N(8, &nodes[9]) N(9, &nodes[10])
    N(10, &nodes[11]) N(11, NULL)};
#undef N
/* Interior pointers (a non-zero addend in the word; exported, not folded). */
static const char kText[] = "packed relocations";
const char* const kInterior[] = {kText + 0, kText + 7, kText + 11, kText + 17};

/* --- imports: APS2 groups (by info, by offset delta) ------------------------------------- */
/* Exported (preemptible) tables, so the compiler reads the words instead of
 * folding them into GOT loads: R_ARM_ABS32 against each import. */
typedef size_t (*StrlenFn)(const char*);
StrlenFn const kStrlen[] = {strlen, strlen, strlen, strlen, strlen, strlen, strlen, strlen};
void* const kLibc[] = {(void*)memcpy, (void*)strcmp, (void*)snprintf, (void*)malloc, (void*)free};
const char* const kDepData[] = {dep_letters, dep_letters + 3, dep_letters + 25};
int* const kDepNumbers[] = {&dep_numbers[0], &dep_numbers[5], &dep_numbers[15]};

/* A preemptible export pointed to from data: R_ARM_ABS32 against itself. */
int reloc_exported(int x) { return x + 1000; }
int (*const kExported)(int) = reloc_exported;

/* --- TLS -------------------------------------------------------------------------------- */
__thread int tls_counter = 42;                                     /* general dynamic */
static __thread char tls_text[16] = "init";                        /* local dynamic */
__attribute__((tls_model("initial-exec"))) __thread int tls_ie = 100;   /* TPOFF32 */
static __thread int tls_zero[8];                                   /* .tbss */

JNIEXPORT jstring JNICALL Java_org_arcwasm_ndktest_RelocLib_tables(JNIEnv* env, jclass c) {
  const char* r = "ok";
  for (int i = 0; i < 40 && r[0] == 'o'; i++) {
    if (kFuncs[i](3) != 3 * (i + 1) + i) r = FAIL("kFuncs[%d](3) = %d", i, kFuncs[i](3));
  }
  static const char* const kExpect[] = {"zero", "nine", "nineteen", "thirty-one", "sixty-nine"};
  static const int kAt[] = {0, 9, 19, 31, 69};
  for (int i = 0; i < 5 && r[0] == 'o'; i++) {
    if (strcmp(kNames[kAt[i]], kExpect[i]) != 0) r = FAIL("kNames[%d] = %s", kAt[i], kNames[kAt[i]]);
  }
  if (r[0] == 'o' && NNAMES != 70) r = FAIL("%d names", NNAMES);
  int count = 0, sum = 0;
  for (const struct Node* n = &nodes[0]; n && count < 20; n = n->next, count++) {
    char want[16];
    snprintf(want, sizeof want, "node%d", count);
    if (strcmp(n->name, want) != 0 && r[0] == 'o') r = FAIL("node %d is %s", count, n->name);
    sum += n->value;
  }
  if (r[0] == 'o' && (count != 12 || sum != 7 * 66)) r = FAIL("list: %d nodes, sum %d", count, sum);
  if (r[0] == 'o' && (strcmp(kInterior[1], "relocations") != 0 || strcmp(kInterior[2], "cations") != 0 ||
                      strcmp(kInterior[3], "s") != 0 || kInterior[0] != kText))
    r = FAIL("interior pointers: %s|%s|%s", kInterior[1], kInterior[2], kInterior[3]);
  return (*env)->NewStringUTF(env, r);
}

JNIEXPORT jstring JNICALL Java_org_arcwasm_ndktest_RelocLib_imports(JNIEnv* env, jclass c) {
  const char* r = "ok";
  void* libc = dlopen("libc.so", RTLD_NOW);
  void* real_strlen = libc ? dlsym(libc, "strlen") : NULL;
  if (!real_strlen) return (*env)->NewStringUTF(env, "dlsym(libc, strlen) failed");
  for (int i = 0; i < 8 && r[0] == 'o'; i++) {
    if ((void*)kStrlen[i] != real_strlen) r = FAIL("kStrlen[%d] = %p, strlen = %p", i, (void*)kStrlen[i], real_strlen);
    else if (kStrlen[i]("seven..") != 7) r = FAIL("kStrlen[%d] returns %zu", i, kStrlen[i]("seven.."));
  }
  static const char* const kLibcNames[] = {"memcpy", "strcmp", "snprintf", "malloc", "free"};
  for (int i = 0; i < 5 && r[0] == 'o'; i++) {
    if (kLibc[i] != dlsym(libc, kLibcNames[i])) r = FAIL("%s: %p vs %p", kLibcNames[i], kLibc[i], dlsym(libc, kLibcNames[i]));
  }
  char buf[8];
  if (r[0] == 'o') {
    ((void* (*)(void*, const void*, size_t))kLibc[0])(buf, "abc", 4);
    if (((int (*)(const char*, const char*))kLibc[1])(buf, "abc") != 0) r = FAIL("memcpy/strcmp via pointers");
  }
  if (r[0] == 'o' && (kDepData[0][0] != 'a' || kDepData[1][0] != 'd' || kDepData[2][0] != 'z'))
    r = FAIL("dep_letters + addend: %c %c %c", kDepData[0][0], kDepData[1][0], kDepData[2][0]);
  if (r[0] == 'o' && (*kDepNumbers[0] != 0 || *kDepNumbers[1] != 25 || *kDepNumbers[2] != 225))
    r = FAIL("&dep_numbers[i]: %d %d %d", *kDepNumbers[0], *kDepNumbers[1], *kDepNumbers[2]);
  if (r[0] == 'o' && kDepNumbers[1] != dep_numbers + 5) r = FAIL("&dep_numbers[5] differs from code's");
  if (r[0] == 'o' && (strcmp(dep_words[2], "gamma") != 0 || dep_strlen("four") != 4))
    r = FAIL("dep's own tables: %s, %zu", dep_words[2], dep_strlen("four"));
  if (r[0] == 'o' && (kExported(5) != 1005 || kExported != dlsym(dlopen("libndkreloc.so", RTLD_NOW), "reloc_exported")))
    r = FAIL("reloc_exported via data: %d", kExported(5));
  return (*env)->NewStringUTF(env, r);
}

JNIEXPORT jstring JNICALL Java_org_arcwasm_ndktest_RelocLib_versions(JNIEnv* env, jclass c) {
  void* dep = dlopen("libndkrelocdep.so", RTLD_NOW);
  int (*by_dlsym)(void) = dep ? (int (*)(void))dlsym(dep, "ver_value") : NULL;
  snprintf(g_err, sizeof g_err, "ver_value=%d ver_value@VERS_1=%d dlsym=%d", ver_value(), ver_value_old(),
           by_dlsym ? by_dlsym() : -1);
  return (*env)->NewStringUTF(env, g_err);
}

struct Seen {
  int counter, ie, dep, zero;
  char text[16];
  int* counter_addr;
  int dep_after;
};

static void* Worker(void* arg) {
  struct Seen* s = (struct Seen*)arg;
  s->counter = tls_counter;
  s->ie = tls_ie;
  s->dep = dep_tls;
  s->zero = tls_zero[3];
  strcpy(s->text, tls_text);
  s->counter_addr = &tls_counter;
  tls_counter = -1;
  tls_ie = -2;
  dep_tls_set(-3);
  tls_zero[3] = -4;
  strcpy(tls_text, "worker");
  s->dep_after = dep_tls;
  return NULL;
}

JNIEXPORT jstring JNICALL Java_org_arcwasm_ndktest_RelocLib_tls(JNIEnv* env, jclass c) {
  const char* r = "ok";
  tls_counter = 1000;
  tls_ie = 2000;
  dep_tls = 3000;
  tls_zero[3] = 4000;
  strcpy(tls_text, "main");
  struct Seen s;
  memset(&s, 0, sizeof s);
  pthread_t t;
  if (pthread_create(&t, NULL, Worker, &s) != 0 || pthread_join(t, NULL) != 0)
    return (*env)->NewStringUTF(env, "pthread_create failed");
  if (s.counter != 42 || s.ie != 100 || s.dep != 5 || s.zero != 0 || strcmp(s.text, "init") != 0)
    r = FAIL("a new thread's initial TLS: %d %d %d %d %s", s.counter, s.ie, s.dep, s.zero, s.text);
  else if (s.counter_addr == &tls_counter)
    r = FAIL("both threads have tls_counter at %p", (void*)s.counter_addr);
  else if (s.dep_after != -3)
    r = FAIL("dep_tls in the worker after dep_tls_set(-3): %d", s.dep_after);
  else if (tls_counter != 1000 || tls_ie != 2000 || dep_tls_get() != 3000 || tls_zero[3] != 4000 ||
           strcmp(tls_text, "main") != 0)
    r = FAIL("the main thread's TLS after the worker: %d %d %d %d %s", tls_counter, tls_ie, dep_tls_get(),
             tls_zero[3], tls_text);
  return (*env)->NewStringUTF(env, r);
}

/* The dynamic tags and segments of a loaded library, as words. */
struct Layout {
  const char* name;
  char out[160];
};

static int Describe(struct dl_phdr_info* info, size_t size, void* data) {
  struct Layout* l = (struct Layout*)data;
  if (!info->dlpi_name || !strstr(info->dlpi_name, l->name)) return 0;
  int hash = 0, gnu = 0, rel = 0, relr = 0, arelr = 0, verneed = 0, verdef = 0, tls = 0, flags1 = 0;
  for (int i = 0; i < info->dlpi_phnum; i++) {
    const Elf32_Phdr* p = &info->dlpi_phdr[i];
    if (p->p_type == PT_TLS) tls = 1;
    if (p->p_type != PT_DYNAMIC) continue;
    for (const Elf32_Dyn* d = (const Elf32_Dyn*)(info->dlpi_addr + p->p_vaddr); d->d_tag != DT_NULL; d++) {
      switch (d->d_tag) {
        case DT_HASH: hash = 1; break;
        case DT_GNU_HASH: gnu = 1; break;
        case DT_ANDROID_REL: rel = 1; break;
        case DT_RELR: relr = 1; break;
        case DT_ANDROID_RELR: arelr = 1; break;
        case DT_VERNEED: verneed = 1; break;
        case DT_VERDEF: verdef = 1; break;
        case DT_FLAGS_1: flags1 = 1; break;
      }
    }
  }
  snprintf(l->out, sizeof l->out, "%s%s%s%s%s%s%s%s%s", hash ? "HASH " : "", gnu ? "GNU_HASH " : "",
           rel ? "ANDROID_REL " : "", relr ? "RELR " : "", arelr ? "ANDROID_RELR " : "",
           verdef ? "VERDEF " : "", verneed ? "VERNEED " : "", flags1 ? "FLAGS_1 " : "", tls ? "TLS" : "");
  size_t n = strlen(l->out);
  if (n > 0 && l->out[n - 1] == ' ') l->out[n - 1] = 0;
  return 1;
}

JNIEXPORT jstring JNICALL Java_org_arcwasm_ndktest_RelocLib_layout(JNIEnv* env, jclass c, jstring name) {
  struct Layout l;
  l.name = (*env)->GetStringUTFChars(env, name, NULL);
  strcpy(l.out, "not loaded");
  dl_iterate_phdr(Describe, &l);
  (*env)->ReleaseStringUTFChars(env, name, l.name);
  return (*env)->NewStringUTF(env, l.out);
}
