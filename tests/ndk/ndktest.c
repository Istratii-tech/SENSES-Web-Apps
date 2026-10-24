/* libndktest.so: an NDK library (armeabi, android-9 headers) that exercises
 * the bridge: JNI in both directions, bionic libc (stdio macros, ctype
 * tables, errno, files, time, setjmp), pthreads, soft-float math, and a
 * second guest library. The Java side is org.arcwasm.ndktest.NativeLib in
 * tests/ndk/apk; arcwasm.NdkTest checks the results. Each checking function
 * returns "ok" or the first failure. */

#include <android/log.h>
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <math.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define TAG "ndktest"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

extern int helper_counter;
int helper_add(int a, int b);
const char* helper_name(void);
long long helper_mix(int a, long long b, double c);
const char* cxx_checks(void);  /* ndktest_cxx.cc */

static JavaVM* g_vm;
static jclass g_class;  /* global ref to NativeLib */
static int g_onload_version;
static int g_ctor_ran;

__attribute__((constructor)) static void init(void) { g_ctor_ran = helper_counter + 1; }

static char g_fail[512];
#define CHECK(cond, ...)                                                   \
  do {                                                                     \
    if (!(cond)) {                                                         \
      snprintf(g_fail, sizeof g_fail, "%s:%d: ", __func__, __LINE__);      \
      snprintf(g_fail + strlen(g_fail), sizeof g_fail - strlen(g_fail),    \
               __VA_ARGS__);                                               \
      LOGI("FAIL %s", g_fail);                                             \
      return g_fail;                                                       \
    }                                                                      \
  } while (0)

static jstring Result(JNIEnv* env, const char* r) { return (*env)->NewStringUTF(env, r ? r : "ok"); }

/* --- simple calls ------------------------------------------------------- */

jstring Java_org_arcwasm_ndktest_NativeLib_stringFromJNI(JNIEnv* env, jclass clazz) {
  return (*env)->NewStringUTF(env, "Hello from ARM");
}

/* The ABI this library was built for. */
jstring Java_org_arcwasm_ndktest_NativeLib_abi(JNIEnv* env, jclass clazz) {
#if defined(__aarch64__)
  return (*env)->NewStringUTF(env, "arm64-v8a");
#elif __ARM_ARCH >= 7
  return (*env)->NewStringUTF(env, "armeabi-v7a");
#else
  return (*env)->NewStringUTF(env, "armeabi");
#endif
}

jint Java_org_arcwasm_ndktest_NativeLib_add(JNIEnv* env, jclass clazz, jint a, jint b) {
  return a + b;
}

jlong Java_org_arcwasm_ndktest_NativeLib_mulLong(JNIEnv* env, jclass clazz, jlong a, jlong b) {
  return a * b;
}

/* (I D F J I): a double at r2:r3, then float, long (8-aligned) and int on the stack. */
jdouble Java_org_arcwasm_ndktest_NativeLib_mix(JNIEnv* env, jclass clazz, jint a, jdouble b,
                                               jfloat c, jlong d, jint e) {
  return a + b * 2 + c + (double)d / 4 + e * 1000;
}

jfloat Java_org_arcwasm_ndktest_NativeLib_half(JNIEnv* env, jclass clazz, jfloat f) { return f / 2; }
jboolean Java_org_arcwasm_ndktest_NativeLib_not(JNIEnv* env, jclass clazz, jboolean b) { return !b; }
jchar Java_org_arcwasm_ndktest_NativeLib_nextChar(JNIEnv* env, jclass clazz, jchar c) { return c + 1; }
jshort Java_org_arcwasm_ndktest_NativeLib_negShort(JNIEnv* env, jclass clazz, jshort s) { return -s; }
jbyte Java_org_arcwasm_ndktest_NativeLib_negByte(JNIEnv* env, jclass clazz, jbyte b) { return -b; }

jintArray Java_org_arcwasm_ndktest_NativeLib_squares(JNIEnv* env, jclass clazz, jintArray in) {
  jsize n = (*env)->GetArrayLength(env, in);
  jint* src = (*env)->GetIntArrayElements(env, in, NULL);
  jint* tmp = malloc(n * sizeof(jint));
  for (int i = 0; i < n; i++) tmp[i] = src[i] * src[i];
  (*env)->ReleaseIntArrayElements(env, in, src, JNI_ABORT);
  jintArray out = (*env)->NewIntArray(env, n);
  (*env)->SetIntArrayRegion(env, out, 0, n, tmp);
  free(tmp);
  return out;
}

jstring Java_org_arcwasm_ndktest_NativeLib_concat(JNIEnv* env, jclass clazz, jstring a, jstring b) {
  const char* sa = (*env)->GetStringUTFChars(env, a, NULL);
  const char* sb = (*env)->GetStringUTFChars(env, b, NULL);
  char buf[256];
  snprintf(buf, sizeof buf, "%s+%s(%d)", sa, sb, (int)(*env)->GetStringLength(env, b));
  (*env)->ReleaseStringUTFChars(env, a, sa);
  (*env)->ReleaseStringUTFChars(env, b, sb);
  return (*env)->NewStringUTF(env, buf);
}

/* Soft-float libm calls; Java compares with StrictMath. */
jdouble Java_org_arcwasm_ndktest_NativeLib_math(JNIEnv* env, jclass clazz, jdouble x) {
  double r = sin(x) + cos(x) * 2 + sqrt(fabs(x)) + pow(x, 3) + floor(x * 10) / 10 + fmod(x, 0.7) +
             atan2(x, 2) + exp(x / 10) + log(fabs(x) + 1) + ldexp(x, 3) + tanh(x);
  float f = (float)x;
  r += sinf(f) + sqrtf(fabsf(f)) + floorf(f * 3) + atan2f(f, 3.0f) + fmodf(f, 0.25f);
  int e;
  r += frexp(x * 100, &e) + e;
  double ip;
  r += modf(x, &ip) + ip;
  return r;
}

/* --- libc ------------------------------------------------------------------ */

static int compare_ints(const void* a, const void* b) { return *(const int*)a - *(const int*)b; }

static jmp_buf g_jmp;
static int g_jmp_depth;
static void jump_down(int n) {
  if (n == 0)
    longjmp(g_jmp, 42);
  g_jmp_depth++;
  jump_down(n - 1);
}

static const char* libc_checks(const char* dir) {
  char buf[256], path[256];

  /* printf family with mixed arguments (doubles are 8-aligned in varargs) */
  snprintf(buf, sizeof buf, "%d|%5s|%-3c|%.3f|%lld|%x|%lu|%e|%%|%s", -12, "ab", 'z', 3.14159,
           -1234567890123LL, 0xbeef, 4000000000UL, 1e10, (char*)NULL);
  CHECK(!strcmp(buf, "-12|   ab|z  |3.142|-1234567890123|beef|4000000000|1.000000e+10|%|(null)"),
        "snprintf: %s", buf);
  int n = snprintf(buf, 4, "%s", "abcdef");
  CHECK(n == 6 && !strcmp(buf, "abc"), "snprintf truncation: %d %s", n, buf);
  sprintf(buf, "%08.2f/%+d/%#o/%p", -1.5, 7, 8, (void*)0x1234);
  CHECK(!strcmp(buf, "-0001.50/+7/010/0x1234"), "sprintf: %s", buf);
  LOGI("liblog from ARM: %d %s %.2f", 42, "str", 2.5);

  /* strings, ctype macros (bionic's _ctype_ table), qsort with a guest comparator */
  CHECK(strlen("hello") == 5 && strcmp("a", "b") < 0 && !strncmp("abcd", "abxx", 2), "str*");
  CHECK(strstr("needle in haystack", "hay") != NULL && strchr("abc", 'c') && !strrchr("abc", 'z'),
        "strstr/strchr");
  volatile char cs[] = "7aQ\tFg!Z";  /* not constants: the macros read _ctype_ */
  CHECK(isdigit(cs[0]) && !isdigit(cs[1]) && isalpha(cs[2]) && isspace(cs[3]) &&
            isxdigit(cs[4]) && !isxdigit(cs[5]) && toupper(cs[1]) == 'A' &&
            tolower(cs[7]) == 'z' && ispunct(cs[6]) && isupper(cs[7]) && !islower(cs[7]),
        "ctype");
  char tok[] = "a,b,,c";
  char* save;
  int count = 0;
  for (char* t = strtok_r(tok, ",", &save); t; t = strtok_r(NULL, ",", &save)) count++;
  CHECK(count == 3, "strtok_r: %d", count);
  CHECK(strtol("-0x1f", NULL, 16) == -31 && strtoul("4000000000", NULL, 10) == 4000000000UL &&
            atoi("123") == 123 && strtod("2.5e3", NULL) == 2500.0,
        "strtol/strtod");
  int arr[] = {5, -2, 9, 0, 3};
  qsort(arr, 5, sizeof(int), compare_ints);
  CHECK(arr[0] == -2 && arr[1] == 0 && arr[4] == 9, "qsort: %d %d %d", arr[0], arr[1], arr[4]);
  int a1, a2;
  char word[16];
  double dv;
  CHECK(sscanf("17 word 2.5 x", "%d %15s %lf", &a1, word, &dv) == 3 && a1 == 17 &&
            !strcmp(word, "word") && dv == 2.5,
        "sscanf");
  CHECK(sscanf("0x10", "%i", &a2) == 1 && a2 == 16, "sscanf %%i");

  /* malloc family */
  char* p = malloc(10);
  strcpy(p, "123456789");
  p = realloc(p, 100000);
  CHECK(p && !strcmp(p, "123456789"), "realloc");
  free(p);
  int* z = calloc(1000, sizeof(int));
  for (int i = 0; i < 1000; i++) CHECK(z[i] == 0, "calloc");
  free(z);

  /* errno: bionic numbers (ENOENT 2, EEXIST 17) */
  errno = 0;
  snprintf(path, sizeof path, "%s/does/not/exist", dir);
  CHECK(open(path, O_RDONLY) == -1 && errno == ENOENT && ENOENT == 2, "errno after open: %d", errno);
  CHECK(fopen(path, "r") == NULL && errno == 2, "errno after fopen: %d", errno);
  CHECK(!strcmp(strerror(ENOENT), "No such file or directory"), "strerror: %s", strerror(ENOENT));

  /* files: stdio through bionic FILE (getc/putc/feof are macros) */
  snprintf(path, sizeof path, "%s/ndk", dir);
  mkdir(path, 0755);
  CHECK(mkdir(path, 0755) == -1 && errno == EEXIST, "mkdir twice: errno %d", errno);
  snprintf(path, sizeof path, "%s/ndk/file.txt", dir);
  FILE* f = fopen(path, "w");
  CHECK(f != NULL, "fopen w: errno %d", errno);
  fprintf(f, "line %d\n", 1);
  fputs("line 2\n", f);
  putc('x', f);
  fwrite("yz\n", 1, 3, f);
  CHECK(fclose(f) == 0, "fclose");
  struct stat st;
  CHECK(stat(path, &st) == 0 && st.st_size == 18 && S_ISREG(st.st_mode), "stat: size %lld",
        (long long)st.st_size);
  f = fopen(path, "r");
  CHECK(f && fgets(buf, sizeof buf, f) && !strcmp(buf, "line 1\n"), "fgets");
  CHECK(getc(f) == 'l', "getc");
  CHECK(fseek(f, -4, SEEK_END) == 0 && ftell(f) == 14, "fseek/ftell");
  int c, chars = 0;
  while ((c = getc(f)) != EOF) chars++;
  CHECK(chars == 4 && feof(f) && !ferror(f), "getc to EOF: %d", chars);
  fclose(f);
  int fd = open(path, O_RDONLY);
  CHECK(fd >= 0 && lseek(fd, 5, SEEK_SET) == 5 && read(fd, buf, 1) == 1 && buf[0] == '1',
        "open/lseek/read");
  CHECK(fstat(fd, &st) == 0 && st.st_size == 18, "fstat");
  close(fd);
  snprintf(buf, sizeof buf, "%s/ndk", dir);
  DIR* d = opendir(buf);
  CHECK(d != NULL, "opendir");
  int found = 0;
  struct dirent* e;
  while ((e = readdir(d)) != NULL) found |= !strcmp(e->d_name, "file.txt");
  closedir(d);
  CHECK(found, "readdir");
  /* telldir/seekdir (API 23: looked up, the NDK 9 headers lack them) resume
     at the entry after the one read; lseek(SEEK_END) on a directory works */
  long (*telldir_fn)(DIR*) = (long (*)(DIR*))dlsym(RTLD_DEFAULT, "telldir");
  void (*seekdir_fn)(DIR*, long) = (void (*)(DIR*, long))dlsym(RTLD_DEFAULT, "seekdir");
  CHECK(telldir_fn && seekdir_fn, "telldir/seekdir exported");
  if (telldir_fn && seekdir_fn) {
    char second[256] = "", third[256] = "";
    d = opendir(buf);
    CHECK(d && readdir(d) != NULL, "opendir again");
    long pos = telldir_fn(d);
    if ((e = readdir(d)) != NULL) strcpy(second, e->d_name);
    if ((e = readdir(d)) != NULL) strcpy(third, e->d_name);
    seekdir_fn(d, pos);
    e = readdir(d);
    CHECK(second[0] && e && !strcmp(e->d_name, second), "seekdir back: '%s' '%s'", second,
          e ? e->d_name : "(end)");
    long pos3 = telldir_fn(d);
    rewinddir(d);
    seekdir_fn(d, pos3);
    e = readdir(d);
    CHECK(third[0] && e && !strcmp(e->d_name, third), "seekdir forward: '%s'", third);
    CHECK(lseek(dirfd(d), 0, SEEK_END) >= 0, "lseek(SEEK_END) on a directory");
    closedir(d);
  }
  CHECK(unlink(path) == 0 && access(path, F_OK) == -1, "unlink");

  /* time: 32-bit time_t and struct timeval/timespec */
  time_t now = time(NULL);
  CHECK(now > 1600000000, "time: %ld", (long)now);
  struct timeval tv;
  gettimeofday(&tv, NULL);
  CHECK(tv.tv_sec >= now - 1 && tv.tv_usec >= 0 && tv.tv_usec < 1000000, "gettimeofday");
  struct timespec t1, t2, ns = {0, 20 * 1000 * 1000};
  clock_gettime(CLOCK_MONOTONIC, &t1);
  nanosleep(&ns, NULL);
  clock_gettime(CLOCK_MONOTONIC, &t2);
  long ms = (t2.tv_sec - t1.tv_sec) * 1000 + (t2.tv_nsec - t1.tv_nsec) / 1000000;
  CHECK(ms >= 15 && ms < 2000, "nanosleep 20 ms took %ld ms", ms);
  time_t fixed = 1000000000;  /* 2001-09-09 01:46:40 UTC */
  struct tm tm;
  gmtime_r(&fixed, &tm);
  strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
  CHECK(!strcmp(buf, "2001-09-09 01:46:40"), "gmtime_r/strftime: %s", buf);
  CHECK(difftime(fixed + 90, fixed) == 90.0, "difftime");

  /* setjmp/longjmp through guest frames */
  g_jmp_depth = 0;
  volatile int stage = 0;
  int r = setjmp(g_jmp);
  if (r == 0) {
    stage = 1;
    jump_down(5);
    stage = 99;
  }
  CHECK(r == 42 && stage == 1 && g_jmp_depth == 5, "setjmp/longjmp: r=%d stage=%d", r, stage);

#if defined(__aarch64__)
  /* Reservations larger than the memory (Hermes: 3 GB per runtime, two runtimes):
     both succeed; memory comes as it is committed, anywhere in them; a commit
     larger than the wasm memory fails (ENOMEM) and leaves the rest as it was. */
  const size_t kSeg = 4 << 20, kHeap = 3ull << 30;
  char* heaps[2];
  for (int i = 0; i < 2; i++) {
    char* raw = mmap(NULL, kHeap + kSeg, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    CHECK(raw != MAP_FAILED, "reserve 3 GB #%d: errno %d", i, errno);
    char* h = (char*)(((uintptr_t)raw + kSeg - 1) & ~(uintptr_t)(kSeg - 1));
    CHECK(h == raw || munmap(raw, h - raw) == 0, "trim the front #%d", i);
    CHECK(munmap(h + kHeap, raw + kHeap + kSeg - (h + kHeap)) == 0, "trim the back #%d", i);
    CHECK(mmap(h, kSeg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == h,
          "commit the first segment #%d: errno %d", i, errno);
    memset(h, 'a' + i, kSeg);
    h[kSeg - 1] = 'z';
    CHECK(memchr(h, 'z', kSeg) == h + kSeg - 1, "memchr in the reservation #%d: %p of %p", i,
          memchr(h, 'z', kSeg), h);
    char* far = h + (2ull << 30);
    CHECK(mmap(far, kSeg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == far,
          "commit 2 GB into the reservation #%d: errno %d", i, errno);
    CHECK(far[0] == 0 && far[kSeg - 1] == 0, "fresh memory 2 GB into the reservation #%d", i);
    far[kSeg - 2] = 'y';
    CHECK(memchr(far, 'y', kSeg) == far + kSeg - 2, "memchr 2 GB into the reservation #%d", i);
    CHECK(h[kSeg - 1] == 'z' && far[kSeg - 2] == 'y', "the reservation #%d", i);
    CHECK(madvise(h, kSeg, MADV_DONTNEED) == 0 && h[0] == 0, "madvise #%d", i);
    heaps[i] = h;
  }
  {
    /* More than the wasm memory at once: ENOMEM, and the reservation stays usable. */
    const size_t kBig = 6ull << 30;
    char* big = mmap(NULL, kBig, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    CHECK(big != MAP_FAILED, "reserve 6 GB: errno %d", errno);
    CHECK(mmap(big, 5ull << 30, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED &&
              errno == ENOMEM,
          "commit 5 GB at once: errno %d", errno);
    CHECK(mprotect(big + (5ull << 30), kSeg, PROT_READ | PROT_WRITE) == 0, "commit after a failed one: errno %d", errno);
    big[(5ull << 30) + 1] = 'q';
    CHECK(big[(5ull << 30) + 1] == 'q', "memory after a failed commit");
    CHECK(munmap(big, kBig) == 0, "unmap 6 GB");
  }
  heaps[0][1] = 'A';
  heaps[1][1] = 'B';
  CHECK(heaps[0][1] == 'A' && heaps[1][1] == 'B', "reservations apart");
  for (int i = 0; i < 2; i++) CHECK(munmap(heaps[i], kHeap) == 0, "unmap reservation #%d", i);

  /* Unity's Dynamic Heap (Subway Surfers 3.70): 511 MB reserved seven times and more,
     blocks committed by mprotect; more than the wasm memory in all. */
  const size_t kDyn = 511ull << 20, kBlock = 16 << 20;
  char* dyn[8];
  for (int i = 0; i < 8; i++) {
    dyn[i] = mmap(NULL, kDyn, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    CHECK(dyn[i] != MAP_FAILED, "reserve 511 MB #%d: errno %d", i, errno);
    for (int b = 0; b < 2; b++) {
      char* blk = dyn[i] + (size_t)b * (256 << 20);
      CHECK(mprotect(blk, kBlock, PROT_READ | PROT_WRITE) == 0, "commit a block of 511 MB #%d: errno %d", i, errno);
      CHECK(blk[0] == 0 && blk[kBlock - 1] == 0, "a fresh block #%d", i);
      memset(blk, '0' + i, kBlock);
    }
  }
  for (int i = 0; i < 8; i++) {
    char* blk = dyn[i] + (256 << 20);
    CHECK(dyn[i][kBlock - 1] == '0' + i && blk[0] == '0' + i, "blocks apart #%d", i);
    blk[kBlock / 2] = '!';
    CHECK(memchr(blk, '!', kBlock) == blk + kBlock / 2, "memchr in a block #%d", i);
    CHECK(mprotect(blk, kBlock, PROT_NONE) == 0 && madvise(blk, kBlock, MADV_DONTNEED) == 0 &&
          mprotect(blk, kBlock, PROT_READ | PROT_WRITE) == 0 && blk[kBlock / 2] == 0,
          "decommit and commit again #%d", i);
    CHECK(munmap(dyn[i], kDyn) == 0, "unmap 511 MB #%d", i);
  }
  {
    /* A 4 KB header committed, then the block after it: one run of memory for
       a host function (memchr) across all of it. */
    char* d = mmap(NULL, kDyn, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    CHECK(d != MAP_FAILED && mprotect(d, 4096, PROT_READ | PROT_WRITE) == 0, "header: errno %d", errno);
    strcpy(d, "header");
    const size_t kBody = 11 << 20;
    CHECK(mprotect(d + 4096, kBody, PROT_READ | PROT_WRITE) == 0, "the block after the header: errno %d", errno);
    d[4096 + kBody - 1] = '#';
    CHECK(!strcmp(d, "header") && memchr(d + 4096, '#', kBody) == d + 4096 + kBody - 1,
          "header kept, the block one memory: %p of %p", memchr(d + 4096, '#', kBody), d + 4096 + kBody - 1);
    CHECK(munmap(d, kDyn) == 0, "unmap the header's reservation");
  }

  /* Dart with compressed pointers: 8 GB, the half aligned to 4 GB kept; an object's
     address is the heap's base plus its 32-bit offset. */
  const size_t k4G = 4ull << 30;
  char* raw = mmap(NULL, 2 * k4G, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  CHECK(raw != MAP_FAILED, "reserve 8 GB: errno %d", errno);
  char* base = (char*)(((uintptr_t)raw + k4G - 1) & ~(uintptr_t)(k4G - 1));
  CHECK(base == raw || munmap(raw, base - raw) == 0, "trim to 4 GB alignment");
  CHECK(base + k4G == raw + 2 * k4G || munmap(base + k4G, raw + 2 * k4G - (base + k4G)) == 0, "trim the back");
  char* page = base + (1 << 20);
  CHECK(mmap(base, 2 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == base,
        "commit at the aligned base: errno %d", errno);
  strcpy(page, "compressed");
  uint32_t compressed = (uint32_t)(uintptr_t)page;
  char* decompressed = (char*)((uintptr_t)base + compressed);
  CHECK(decompressed == page && !strcmp(decompressed, "compressed") && base[0] == 0,
        "decompressed pointer: %p of %p", decompressed, page);
  CHECK(strchr(page, 'p') == page + 3, "strchr in the aligned heap: %p of %p", strchr(page, 'p'), page);
  CHECK(munmap(base, k4G) == 0, "unmap the aligned heap");
#endif

  return NULL;
}

jstring Java_org_arcwasm_ndktest_NativeLib_libc(JNIEnv* env, jclass clazz, jstring jdir) {
  const char* dir = (*env)->GetStringUTFChars(env, jdir, NULL);
  const char* r = libc_checks(dir);
  (*env)->ReleaseStringUTFChars(env, jdir, dir);
  return Result(env, r);
}

/* --- threads ------------------------------------------------------------------ */

static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
#if defined(__LP64__) || defined(__ANDROID_API_FUTURE__)
/* (The 32-bit ABI's old names are not in LP64 bionic, nor in the unified
 * headers of NDK r14+ that the arm64 and relocs32 builds use.) */
static pthread_mutex_t g_rmutex = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
#else
static pthread_mutex_t g_rmutex = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
#endif
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static int g_counter, g_ready;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static int g_once_calls;
static pthread_key_t g_key;
static int g_dtor_calls;

static void once_fn(void) { g_once_calls++; }
static void key_dtor(void* v) {
  pthread_mutex_lock(&g_mutex);
  g_dtor_calls += (int)(long)v;
  pthread_mutex_unlock(&g_mutex);
}

static void* worker(void* arg) {
  int id = (int)(long)arg;
  pthread_once(&g_once, once_fn);
  pthread_setspecific(g_key, (void*)1);
  for (int i = 0; i < 1000; i++) {
    pthread_mutex_lock(&g_mutex);
    g_counter++;
    pthread_mutex_unlock(&g_mutex);
  }
  pthread_mutex_lock(&g_mutex);
  g_ready++;
  pthread_cond_broadcast(&g_cond);
  pthread_mutex_unlock(&g_mutex);
  return (void*)(long)(id * 10);
}

#if defined(__aarch64__)
/* Stop the world as Boehm's GC (il2cpp, topic unity2022-arm64): each thread
 * gets SIGUSR1, acknowledges and sleeps in sigsuspend until SIGUSR2 -- threads
 * waiting in a condition variable and one spinning in guest code alone (it
 * must really stop). pthread_sigmask holds a signal back until unblocked. */
#include <semaphore.h>
#include <signal.h>
#define SW_N 4
static sem_t g_sw_ack, g_sw_res;   /* (counts: the guest toolchain has no outline atomics) */
static int g_sw_go, g_sw_ready;
static volatile int g_sw_spin_stop;
static volatile unsigned long g_sw_spins;
static void sw_suspend(int sig) {
  sem_post(&g_sw_ack);
  sigset_t m;
  sigfillset(&m);
  sigdelset(&m, SIGUSR2);
  sigsuspend(&m);
}
static void sw_resume(int sig) { sem_post(&g_sw_res); }
static int sw_resumed(void) {
  int v = 0;
  sem_getvalue(&g_sw_res, &v);
  return v;
}
static void* sw_waiter(void* p) {
  pthread_mutex_lock(&g_mutex);
  g_sw_ready++;
  pthread_cond_broadcast(&g_cond);
  while (!g_sw_go) pthread_cond_wait(&g_cond, &g_mutex);
  pthread_mutex_unlock(&g_mutex);
  return p;
}
static void* sw_spinner(void* p) {
  while (!__atomic_load_n(&g_sw_spin_stop, __ATOMIC_ACQUIRE)) g_sw_spins++;
  return p;
}
static int sw_sem_wait_ms(sem_t* s, int ms) {
  struct timespec dl;
  clock_gettime(CLOCK_REALTIME, &dl);
  dl.tv_sec += ms / 1000;
  dl.tv_nsec += (ms % 1000) * 1000000L;
  if (dl.tv_nsec >= 1000000000L) {
    dl.tv_sec++;
    dl.tv_nsec -= 1000000000L;
  }
  return sem_timedwait(s, &dl);
}
static const char* stop_world_checks(void) {
  struct sigaction sa, old;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = sw_suspend;
  sa.sa_flags = SA_RESTART;
  sigfillset(&sa.sa_mask);
  CHECK(sigaction(SIGUSR1, &sa, NULL) == 0, "sigaction USR1");
  sa.sa_handler = sw_resume;
  sigemptyset(&sa.sa_mask);
  CHECK(sigaction(SIGUSR2, &sa, NULL) == 0, "sigaction USR2");
  CHECK(sigaction(SIGUSR2, NULL, &old) == 0 && old.sa_handler == sw_resume, "sigaction gives the handler back");
  sem_init(&g_sw_ack, 0, 0);
  sem_init(&g_sw_res, 0, 0);
  g_sw_go = g_sw_ready = g_sw_spin_stop = 0;
  pthread_t t[SW_N + 1];
  for (int i = 0; i < SW_N; i++) CHECK(pthread_create(&t[i], NULL, sw_waiter, (void*)(long)i) == 0, "waiter");
  CHECK(pthread_create(&t[SW_N], NULL, sw_spinner, (void*)(long)SW_N) == 0, "spinner");
  pthread_mutex_lock(&g_mutex);
  while (g_sw_ready < SW_N) pthread_cond_wait(&g_cond, &g_mutex);
  pthread_mutex_unlock(&g_mutex);
  for (int round = 0; round < 3; round++) {
    for (int i = 0; i <= SW_N; i++) CHECK(pthread_kill(t[i], SIGUSR1) == 0, "pthread_kill USR1 %d", i);
    for (int i = 0; i <= SW_N; i++)
      CHECK(sw_sem_wait_ms(&g_sw_ack, 10000) == 0, "round %d: %d of %d threads stopped", round, i, SW_N + 1);
    unsigned long spins = g_sw_spins;
    usleep(20000);
    CHECK(g_sw_spins == spins, "round %d: the spinning thread did not stop", round);
    for (int i = 0; i <= SW_N; i++) CHECK(pthread_kill(t[i], SIGUSR2) == 0, "pthread_kill USR2 %d", i);
  }
  for (int i = 0; i < 1000 && sw_resumed() < 3 * (SW_N + 1); i++) usleep(1000);
  CHECK(sw_resumed() == 3 * (SW_N + 1), "%d resume handlers ran", sw_resumed());
  /* A blocked signal waits for pthread_sigmask to unblock it. */
  sigset_t m, prev;
  sigemptyset(&m);
  sigaddset(&m, SIGUSR2);
  CHECK(pthread_sigmask(SIG_BLOCK, &m, &prev) == 0, "pthread_sigmask block");
  int before = sw_resumed();
  CHECK(pthread_kill(pthread_self(), SIGUSR2) == 0, "pthread_kill self");
  CHECK(sw_resumed() == before, "a blocked signal ran its handler");
  CHECK(pthread_sigmask(SIG_SETMASK, &prev, NULL) == 0, "pthread_sigmask unblock");
  CHECK(sw_resumed() == before + 1, "an unblocked signal did not run (%d)", sw_resumed() - before);
  __atomic_store_n(&g_sw_spin_stop, 1, __ATOMIC_RELEASE);
  pthread_mutex_lock(&g_mutex);
  g_sw_go = 1;
  pthread_cond_broadcast(&g_cond);
  pthread_mutex_unlock(&g_mutex);
  for (int i = 0; i <= SW_N; i++) {
    void* ret = NULL;
    CHECK(pthread_join(t[i], &ret) == 0 && (long)ret == i, "thread %d after the stops", i);
  }
  signal(SIGUSR1, SIG_DFL);
  signal(SIGUSR2, SIG_DFL);
  return NULL;
}
#endif

static const char* thread_checks(void) {
  pthread_key_create(&g_key, key_dtor);
  pthread_t t[4];
  for (int i = 0; i < 4; i++)
    CHECK(pthread_create(&t[i], NULL, worker, (void*)(long)(i + 1)) == 0, "pthread_create");
  pthread_mutex_lock(&g_mutex);
  while (g_ready < 4) pthread_cond_wait(&g_cond, &g_mutex);
  pthread_mutex_unlock(&g_mutex);
  for (int i = 0; i < 4; i++) {
    void* ret;
    CHECK(pthread_join(t[i], &ret) == 0 && (long)ret == (i + 1) * 10, "pthread_join %d", i);
  }
  CHECK(g_counter == 4000, "counter %d", g_counter);
  CHECK(g_once_calls == 1, "pthread_once ran %d times", g_once_calls);
  CHECK(g_dtor_calls == 4, "key destructors ran %d times", g_dtor_calls);
  CHECK(pthread_mutex_lock(&g_rmutex) == 0 && pthread_mutex_lock(&g_rmutex) == 0 &&
            pthread_mutex_unlock(&g_rmutex) == 0 && pthread_mutex_unlock(&g_rmutex) == 0,
        "recursive mutex");
  CHECK(pthread_mutex_trylock(&g_mutex) == 0, "trylock free");
  /* bionic error numbers: EBUSY 16 */
  pthread_t self = pthread_self();
  (void)self;
  struct timespec ts;
#if defined(__LP64__) || defined(__ANDROID_API_FUTURE__)
  clock_gettime(CLOCK_REALTIME, &ts);
#else
  clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
  ts.tv_nsec += 30 * 1000 * 1000;
  if (ts.tv_nsec >= 1000000000) {
    ts.tv_sec++;
    ts.tv_nsec -= 1000000000;
  }
#if defined(__LP64__) || defined(__ANDROID_API_FUTURE__)
  int r = pthread_cond_timedwait(&g_cond, &g_mutex, &ts);
#else
  int r = pthread_cond_timedwait_monotonic(&g_cond, &g_mutex, &ts);
#endif
  CHECK(r == ETIMEDOUT && ETIMEDOUT == 110, "timedwait_monotonic: %d", r);
  pthread_mutex_unlock(&g_mutex);
#if defined(__aarch64__)
  { const char* why = stop_world_checks(); if (why) return why; }
#endif
  return NULL;
}

jstring Java_org_arcwasm_ndktest_NativeLib_threads(JNIEnv* env, jclass clazz) {
  return Result(env, thread_checks());
}

jstring Java_org_arcwasm_ndktest_NativeLib_cxx(JNIEnv* env, jclass clazz) {
  return Result(env, cxx_checks());
}

/* A second guest library: function, data and constructor order. */
jstring Java_org_arcwasm_ndktest_NativeLib_helper(JNIEnv* env, jclass clazz) {
  char buf[128];
  int before = helper_counter;
  int sum = helper_add(2, 3);
  long long mix = helper_mix(1, 1LL << 40, 2.25);
  snprintf(buf, sizeof buf, "%s %d %d %d %lld", helper_name(), sum, helper_counter - before,
           g_ctor_ran, mix);
  return (*env)->NewStringUTF(env, buf);
}

/* --- calls back into Java ------------------------------------------------------- */

jint Java_org_arcwasm_ndktest_NativeLib_callback(JNIEnv* env, jobject self, jint x) {
  jclass cls = (*env)->GetObjectClass(env, self);
  jmethodID m = (*env)->GetMethodID(env, cls, "onCallback", "(I)I");
  jint r = (*env)->CallIntMethod(env, self, m, x);
  jfieldID calls = (*env)->GetFieldID(env, cls, "calls", "I");
  return r + (*env)->GetIntField(env, self, calls) * 1000;
}

static jdouble call_double_v(JNIEnv* env, jclass cls, jmethodID m, ...) {
  va_list ap;
  va_start(ap, m);
  jdouble r = (*env)->CallStaticDoubleMethodV(env, cls, m, ap);
  va_end(ap);
  return r;
}

jstring Java_org_arcwasm_ndktest_NativeLib_callStatics(JNIEnv* env, jclass cls) {
  jmethodID rec = (*env)->GetStaticMethodID(env, cls, "record", "(ILjava/lang/String;DFJZC)V");
  jstring s = (*env)->NewStringUTF(env, "s");
  /* varargs: float is promoted to double, boolean/char to int */
  (*env)->CallStaticVoidMethod(env, cls, rec, 7, s, 0.5, 1.25f, 1LL << 33, JNI_TRUE, (jchar)'q');
  jvalue args[7];
  args[0].i = -1;
  args[1].l = s;
  args[2].d = -2.0;
  args[3].f = 0.75f;
  args[4].j = -5;
  args[5].z = JNI_FALSE;
  args[6].c = 'A';
  (*env)->CallStaticVoidMethodA(env, cls, rec, args);
  jmethodID lr = (*env)->GetStaticMethodID(env, cls, "longResult", "(J)J");
  jlong l = (*env)->CallStaticLongMethod(env, cls, lr, 0x123456789LL);
  jmethodID dr = (*env)->GetStaticMethodID(env, cls, "doubleResult", "(DF)D");
  jdouble d = call_double_v(env, cls, dr, 1.5, 0.25f);
  jmethodID sr = (*env)->GetStaticMethodID(env, cls, "stringResult",
                                           "(Ljava/lang/String;)Ljava/lang/String;");
  jstring str = (*env)->CallStaticObjectMethod(env, cls, sr, s);
  const char* cs = (*env)->GetStringUTFChars(env, str, NULL);
  jobject obj = (*env)->AllocObject(env, cls);
  jmethodID cb = (*env)->GetMethodID(env, cls, "onCallback", "(I)I");
  jint ci = (*env)->CallNonvirtualIntMethod(env, obj, cls, cb, 5);
  jmethodID ctor = (*env)->GetMethodID(env, cls, "<init>", "()V");
  jobject made = (*env)->NewObject(env, cls, ctor);
  char buf[160];
  snprintf(buf, sizeof buf, "%llx %.2f %s %d %d", (long long)l, d, cs, ci,
           made != NULL && (*env)->IsInstanceOf(env, made, cls));
  (*env)->ReleaseStringUTFChars(env, str, cs);
  return (*env)->NewStringUTF(env, buf);
}

/* A native thread attaches to the VM and calls Java. */
static void* attached(void* arg) {
  JNIEnv* env;
  if ((*g_vm)->AttachCurrentThread(g_vm, &env, NULL) != JNI_OK)
    return (void*)1;
  jmethodID m = (*env)->GetStaticMethodID(env, g_class, "fromNativeThread", "(I)V");
  (*env)->CallStaticVoidMethod(env, g_class, m, (jint)(long)arg);
  JNIEnv* env2;
  int same = (*g_vm)->GetEnv(g_vm, (void**)&env2, JNI_VERSION_1_4) == JNI_OK && env2 == env;
  (*g_vm)->DetachCurrentThread(g_vm);
  return (void*)(long)(same ? 0 : 2);
}

jstring Java_org_arcwasm_ndktest_NativeLib_fromThread(JNIEnv* env, jclass cls) {
  pthread_t t;
  void* r;
  if (pthread_create(&t, NULL, attached, (void*)7) != 0)
    return (*env)->NewStringUTF(env, "pthread_create failed");
  pthread_join(t, &r);
  return (*env)->NewStringUTF(env, r == 0 ? "ok" : "attach failed");
}

jstring Java_org_arcwasm_ndktest_NativeLib_throwIn(JNIEnv* env, jclass cls) {
  jclass ex = (*env)->FindClass(env, "java/lang/IllegalStateException");
  (*env)->ThrowNew(env, ex, "thrown from ARM");
  return NULL;
}

static jint registered(JNIEnv* env, jclass cls, jint x) { return x * 7 + g_onload_version; }
static jint on_load_version(JNIEnv* env, jclass cls) { return g_onload_version; }

jint JNI_OnLoad(JavaVM* vm, void* reserved) {
  JNIEnv* env;
  g_vm = vm;
  if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_4) != JNI_OK)
    return JNI_ERR;
  g_onload_version = (*env)->GetVersion(env);
  jclass cls = (*env)->FindClass(env, "org/arcwasm/ndktest/NativeLib");
  if (!cls)
    return JNI_ERR;
  g_class = (*env)->NewGlobalRef(env, cls);
  static const JNINativeMethod methods[] = {
      {"registered", "(I)I", (void*)registered},
      {"onLoadVersion", "()I", (void*)on_load_version},
  };
  if ((*env)->RegisterNatives(env, cls, methods, 2) != 0)
    return JNI_ERR;
  LOGI("JNI_OnLoad: version 0x%x, constructor saw helper_counter %d", g_onload_version,
       g_ctor_ran - 1);
  return JNI_VERSION_1_4;
}
