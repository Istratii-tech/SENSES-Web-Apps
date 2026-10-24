/* NdkJitTest's library (topic guest-jit, docs/topics/guest-jit.md): what a
 * JIT inside an app does with the NDK bridge, built for armeabi-v7a
 * (libndkjit.so) and arm64-v8a (libndkjit64.so). Each check returns "ok" or
 * what went wrong:
 *
 *   rwx      anonymous PROT_READ|WRITE|EXEC memory: code written, flushed
 *            (cacheflush / IC IVAU), run, rewritten 200 times, run again
 *   syscall  the same flushed with the raw cacheflush syscall (ARM) or
 *            IC IVAU by hand (arm64)
 *   wx       W^X: mmap RW, write, mprotect RX (no flush: the mprotect is
 *            one), run; mprotect RW, rewrite, mprotect RX, run
 *   memfd    a double mapping: memfd_create, ftruncate, a writable and an
 *            executable MAP_SHARED view; written through the first, run
 *            through the second, rewritten
 *   ashmem   the same over /dev/ashmem (libandroid's ASharedMemory_create
 *            where there is one, else the ioctls)
 *   threads  a thread runs the code in a loop while this one rewrites and
 *            flushes it: each new value is seen
 *   property ro.arcwasm.jit, for the Java side to compare
 */
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>

#ifdef NDKJIT64   /* (libndkjit64.so: its natives have their own names) */
#define JNI_NAME(n) Java_arcwasm_NdkJitTest_##n##64
#else
#define JNI_NAME(n) Java_arcwasm_NdkJitTest_##n
#endif

static char msg[160];
#define FAIL(...) do { snprintf(msg, sizeof msg, __VA_ARGS__); return msg; } while (0)

typedef int (*fn0)(void);
/* (Through memory: the compiler must not assume what the pointer is.) */
static fn0 Fn(void* p) {
  volatile uintptr_t a = (uintptr_t)p;
  return (fn0)a;
}

#if defined(__aarch64__)
/* movz w0, #k; ret */
static void Emit(void* at, int k) {
  uint32_t* w = (uint32_t*)at;
  w[0] = 0x52800000u | ((uint32_t)k << 5);
  w[1] = 0xd65f03c0u;
}
/* What __clear_cache does on arm64 Linux: DC CVAU, DSB, IC IVAU by CTR_EL0's
 * line sizes, DSB, ISB. */
static void Flush(void* p, size_t n) {
  uint64_t ctr;
  __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
  uintptr_t dline = 4u << ((ctr >> 16) & 15), iline = 4u << (ctr & 15);
  uintptr_t s = (uintptr_t)p, e = s + n, a;
  for (a = s & ~(dline - 1); a < e; a += dline) __asm__ volatile("dc cvau, %0" ::"r"(a) : "memory");
  __asm__ volatile("dsb ish" ::: "memory");
  for (a = s & ~(iline - 1); a < e; a += iline) __asm__ volatile("ic ivau, %0" ::"r"(a) : "memory");
  __asm__ volatile("dsb ish\n\tisb" ::: "memory");
}
static void FlushRaw(void* p, size_t n) { Flush(p, n); }
#else
/* mov r0, #k; bx lr (ARM state: the caller may be Thumb) */
static void Emit(void* at, int k) {
  uint32_t* w = (uint32_t*)at;
  w[0] = 0xe3a00000u | (uint32_t)(k & 0xff);
  w[1] = 0xe12fff1eu;
}
int cacheflush(long start, long end, long flags);
static void Flush(void* p, size_t n) { cacheflush((long)p, (long)p + (long)n, 0); }
static void FlushRaw(void* p, size_t n) { syscall(0xf0002, (long)p, (long)p + (long)n, 0L); }
#endif

/* Writes k (0..199) at |w| through |write|, flushes |run|, runs it, 200 times. */
static const char* Cycle(void* write, void* run, void (*flush)(void*, size_t)) {
  for (int k = 0; k < 200; k++) {
    Emit(write, k);
    flush(run, 8);
    int got = Fn(run)();
    if (got != k) FAIL("round %d: the code returned %d", k, got);
  }
  return NULL;
}

static const char* CheckRwx(void (*flush)(void*, size_t)) {
  void* p = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) FAIL("mmap RWX: errno %d", errno);
  const char* r = Cycle(p, p, flush);
  munmap(p, 4096);
  return r ? r : "ok";
}

static const char* CheckWx(void) {
  uint8_t* p = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) FAIL("mmap RW: errno %d", errno);
  for (int k = 0; k < 50; k++) {
    if (k > 0 && mprotect(p, 8192, PROT_READ | PROT_WRITE) != 0) FAIL("mprotect RW: errno %d", errno);
    Emit(p + 4096, k);
    if (mprotect(p, 8192, PROT_READ | PROT_EXEC) != 0) FAIL("mprotect RX: errno %d", errno);
    int got = Fn(p + 4096)();
    if (got != k) FAIL("round %d: the code returned %d", k, got);
  }
  munmap(p, 8192);
  return "ok";
}

/* Two MAP_SHARED views of |fd| (4096 bytes): RW and RX. */
static const char* DualViews(int fd) {
  uint8_t* rw = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (rw == MAP_FAILED) FAIL("mmap RW view: errno %d", errno);
  uint8_t* rx = mmap(NULL, 4096, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
  if (rx == MAP_FAILED) FAIL("mmap RX view: errno %d", errno);
  /* The views share memory, whatever their addresses (here: one). */
  rw[100] = 0x5a;
  if (rx[100] != 0x5a) FAIL("the views are not one memory (rw %p, rx %p)", rw, rx);
  const char* r = Cycle(rw + 256, rx + 256, Flush);
  munmap(rx, 4096);
  munmap(rw, 4096);
  return r;
}

static const char* CheckMemfd(void) {
  int fd = (int)syscall(__NR_memfd_create, "ndkjit", 1 /* MFD_CLOEXEC */);
  if (fd < 0) FAIL("memfd_create: errno %d", errno);
  if (ftruncate(fd, 4096) != 0) FAIL("ftruncate: errno %d", errno);
  struct stat st;
  if (fstat(fd, &st) != 0 || st.st_size != 4096) FAIL("fstat: size %lld", (long long)st.st_size);
  const char* r = DualViews(fd);
  close(fd);
  return r ? r : "ok";
}

static const char* CheckAshmem(void) {
  int fd = -1;
  void* lib = dlopen("libandroid.so", RTLD_NOW);
  int (*create)(const char*, size_t) = lib ? (int (*)(const char*, size_t))dlsym(lib, "ASharedMemory_create") : NULL;
  if (create) {
    fd = create("ndkjit", 4096);
    if (fd < 0) FAIL("ASharedMemory_create: errno %d", errno);
  } else {
#if defined(__aarch64__)
    return "ok";   /* (arm64 apps are API 21+: the ioctls want a 64-bit size_t) */
#else
    fd = open("/dev/ashmem", O_RDWR);
    if (fd < 0) FAIL("open /dev/ashmem: errno %d", errno);
    char name[256] = "ndkjit";
    if (ioctl(fd, _IOW(0x77, 1, char[256]), name) < 0 || ioctl(fd, _IOW(0x77, 3, size_t), (size_t)4096) < 0)
      FAIL("ashmem ioctls: errno %d", errno);
#endif
  }
  const char* r = DualViews(fd);
  close(fd);
  return r ? r : "ok";
}

static void* volatile g_code;
static volatile int g_stop, g_seen;
static void* Runner(void* arg) {
  (void)arg;
  while (!g_stop) {
    int v = Fn(g_code)();
    __atomic_store_n(&g_seen, v, __ATOMIC_RELEASE);
  }
  return NULL;
}
static double NowMs(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}
static const char* CheckThreads(void) {
  uint8_t* p = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) FAIL("mmap: errno %d", errno);
  Emit(p, 0);
  Flush(p, 8);
  g_code = p;
  g_stop = 0;
  g_seen = -1;
  pthread_t t;
  if (pthread_create(&t, NULL, Runner, NULL) != 0) FAIL("pthread_create");
  const char* r = NULL;
  for (int k = 1; k < 100 && !r; k++) {
    Emit(p, k);
    Flush(p, 8);
    double t0 = NowMs();
    while (__atomic_load_n(&g_seen, __ATOMIC_ACQUIRE) != k) {
      if (NowMs() - t0 > 5000) {
        snprintf(msg, sizeof msg, "value %d not seen by the other thread in 5 s (it sees %d)", k, g_seen);
        r = msg;
        break;
      }
      sched_yield();
    }
  }
  g_stop = 1;
  pthread_join(t, NULL);
  munmap(p, 4096);
  return r ? r : "ok";
}

JNIEXPORT jstring JNICALL JNI_NAME(nativeCheck)(JNIEnv* env, jclass cls, jstring jname) {
  (void)cls;
  const char* name = (*env)->GetStringUTFChars(env, jname, NULL);
  const char* r;
  char prop[PROP_VALUE_MAX] = "";
  if (!strcmp(name, "rwx")) r = CheckRwx(Flush);
  else if (!strcmp(name, "syscall")) r = CheckRwx(FlushRaw);
  else if (!strcmp(name, "wx")) r = CheckWx();
  else if (!strcmp(name, "memfd")) r = CheckMemfd();
  else if (!strcmp(name, "ashmem")) r = CheckAshmem();
  else if (!strcmp(name, "threads")) r = CheckThreads();
  else if (!strcmp(name, "property")) {
    __system_property_get("ro.arcwasm.jit", prop);
    r = prop;
  } else r = "unknown check";
  (*env)->ReleaseStringUTFChars(env, jname, name);
  return (*env)->NewStringUTF(env, r);
}
