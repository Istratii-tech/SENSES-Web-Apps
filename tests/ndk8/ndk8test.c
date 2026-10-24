// libndk8test.so: what NDK libraries built for Android 7.0-8.1 (API 24-27)
// import from the system (topic fw8-system, docs/topics/fw8-system.md),
// exercised from ARM code: bionic's stdin/stdout/stderr variables (API 23),
// *64 and FORTIFY functions of API 24, clock_nanosleep, getifaddrs,
// barriers and spin locks, sendmmsg/recvmmsg, ASharedMemory (API 26), and
// the libraries libxul links (libsync, libnativewindow, libmediandk). Built
// for armeabi-v7a against the NDK's android-18 headers; what those lack is
// declared here as bionic 8.1 has it.
//
// Each native returns "ok" (with details) or what failed.

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>
#include <netinet/in.h>

// --- bionic 8.1 declarations the android-18 headers lack ------------------------------

extern FILE* stdin_v __asm__("stdin");
extern FILE* stdout_v __asm__("stdout");
extern FILE* stderr_v __asm__("stderr");
extern FILE __sF[3];
extern const struct in6_addr in6addr_any_v __asm__("in6addr_any");
extern const struct in6_addr in6addr_loopback_v __asm__("in6addr_loopback");
int clock_nanosleep(clockid_t, int, const struct timespec*, struct timespec*);
int posix_madvise(void*, size_t, int);
int dprintf(int, const char*, ...);
char* stpcpy(char*, const char*);
int stat64(const char*, struct stat*) __asm__("stat64");
int open64(const char*, int, ...) __asm__("open64");
size_t __fread_chk(void*, size_t, size_t, FILE*, size_t);
size_t __fwrite_chk(const void*, size_t, size_t, FILE*, size_t);
const char* getprogname(void);
char* nl_langinfo(int);
long double sinl(long double);
long double powl(long double, long double);

struct ifaddrs {
  struct ifaddrs* ifa_next;
  char* ifa_name;
  unsigned ifa_flags;
  struct sockaddr* ifa_addr;
  struct sockaddr* ifa_netmask;
  struct sockaddr* ifa_broadaddr;
  void* ifa_data;
};
int getifaddrs(struct ifaddrs**);
void freeifaddrs(struct ifaddrs*);
struct if_nameindex { unsigned if_index; char* if_name; };
struct if_nameindex* if_nameindex(void);
void if_freenameindex(struct if_nameindex*);

typedef struct { int __private[8]; } pthread_barrier_t;
int pthread_barrier_init(pthread_barrier_t*, const void*, unsigned);
int pthread_barrier_wait(pthread_barrier_t*);
int pthread_barrier_destroy(pthread_barrier_t*);
typedef struct { int __private[2]; } pthread_spinlock_t;
int pthread_spin_init(pthread_spinlock_t*, int);
int pthread_spin_lock(pthread_spinlock_t*);
int pthread_spin_unlock(pthread_spinlock_t*);
int pthread_spin_trylock(pthread_spinlock_t*);

struct mmsghdr { struct msghdr msg_hdr; unsigned msg_len; };
int sendmmsg(int, struct mmsghdr*, unsigned, int);
int recvmmsg(int, struct mmsghdr*, unsigned, int, const struct timespec*);

int ASharedMemory_create(const char*, size_t);
size_t ASharedMemory_getSize(int);
int ASharedMemory_setProt(int, int);

// libsync, libnativewindow, libmediandk (DT_NEEDED through tests/ndk8/stubs.c).
int sync_merge(const char*, int, int);
typedef struct {
  uint32_t width, height, layers, format;
  uint64_t usage;
  uint32_t stride, rfu0;
  uint64_t rfu1;
} AHardwareBuffer_Desc;
int AHardwareBuffer_allocate(const AHardwareBuffer_Desc*, void**);
void AHardwareBuffer_release(void*);
void AHardwareBuffer_describe(const void*, AHardwareBuffer_Desc*);
int AHardwareBuffer_lock(void*, uint64_t, int32_t, const void*, void**);
int AHardwareBuffer_unlock(void*, int32_t*);
void* AMediaCodec_createDecoderByType(const char*);
void* AMediaFormat_new(void);
extern const char* AMEDIAFORMAT_KEY_MIME;
void* android_dlopen_ext(const char*, int, const void*);

// --- helpers --------------------------------------------------------------------------

static char g_result[512];

static jstring Result(JNIEnv* env, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(g_result, sizeof g_result, fmt, ap);
  va_end(ap);
  return (*env)->NewStringUTF(env, g_result);
}

#define CHECK(cond, ...) \
  do { \
    if (!(cond)) return Result(env, __VA_ARGS__); \
  } while (0)

static double Now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec / 1e9;
}

// --- libc ------------------------------------------------------------------------------

JNIEXPORT jstring JNICALL Java_org_arcwasm_ndk8test_Native8_libc(JNIEnv* env, jclass c,
                                                                 jstring jdir) {
  // API 23: the stdio variables are &__sF[n].
  CHECK(stdin_v == &__sF[0] && stdout_v == &__sF[1] && stderr_v == &__sF[2],
        "stdin/stdout/stderr %p %p %p, __sF %p", stdin_v, stdout_v, stderr_v, __sF);
  CHECK(fprintf(stderr_v, "ndk8test: hello from stderr\n") > 0, "fprintf(stderr)");
  CHECK(IN6_IS_ADDR_LOOPBACK(&in6addr_loopback_v) && IN6_IS_ADDR_UNSPECIFIED(&in6addr_any_v),
        "in6addr_*");

  // clock_nanosleep: relative and TIMER_ABSTIME.
  double t0 = Now();
  struct timespec ts = {0, 20 * 1000 * 1000};
  CHECK(clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL) == 0, "clock_nanosleep relative");
  struct timespec until;
  clock_gettime(CLOCK_MONOTONIC, &until);
  until.tv_nsec += 20 * 1000 * 1000;
  if (until.tv_nsec >= 1000000000) {
    until.tv_sec++;
    until.tv_nsec -= 1000000000;
  }
  CHECK(clock_nanosleep(CLOCK_MONOTONIC, 1 /* TIMER_ABSTIME */, &until, NULL) == 0,
        "clock_nanosleep absolute");
  double slept = Now() - t0;
  CHECK(slept >= 0.035, "slept %.3f s, wanted >= 0.04", slept);
  CHECK(posix_madvise(&ts, sizeof ts, 0) == 0, "posix_madvise");

  // *64 and FORTIFY of API 24 on a file.
  const char* dir = (*env)->GetStringUTFChars(env, jdir, NULL);
  char path[256];
  snprintf(path, sizeof path, "%s/ndk8.txt", dir);
  (*env)->ReleaseStringUTFChars(env, jdir, dir);
  int fd = open64(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
  CHECK(fd >= 0, "open64 %s: errno %d", path, errno);
  CHECK(dprintf(fd, "%s-%d", "eight", 81) == 8, "dprintf");
  close(fd);
  struct stat st;
  CHECK(stat64(path, &st) == 0 && st.st_size == 8, "stat64 size %lld", (long long)st.st_size);
  FILE* f = fopen(path, "r");
  char buf[16] = {0};
  CHECK(f && __fread_chk(buf, 1, 8, f, sizeof buf) == 8 && strcmp(buf, "eight-81") == 0,
        "__fread_chk: '%s'", buf);
  fclose(f);
  char s[16];
  CHECK(stpcpy(s, "ab") == s + 2, "stpcpy");
  CHECK(strcmp(nl_langinfo(1 /* CODESET */), "UTF-8") == 0, "nl_langinfo(CODESET)");
  CHECK(getprogname() != NULL, "getprogname");
  CHECK(fabs((double)sinl(1.0L) - sin(1.0)) < 1e-12 && powl(2.0L, 10.0L) == 1024.0L,
        "long double math");
  return Result(env, "ok slept %.0f ms", slept * 1000);
}

// --- threads: barriers, spin locks ------------------------------------------------------

static pthread_barrier_t g_barrier;
static pthread_spinlock_t g_spin;
static int g_counter;
static int g_serial;

static void* Worker(void* arg) {
  for (int i = 0; i < 1000; i++) {
    pthread_spin_lock(&g_spin);
    g_counter++;
    pthread_spin_unlock(&g_spin);
  }
  if (pthread_barrier_wait(&g_barrier) == -1 /* PTHREAD_BARRIER_SERIAL_THREAD */)
    __sync_fetch_and_add(&g_serial, 1);
  return arg;
}

JNIEXPORT jstring JNICALL Java_org_arcwasm_ndk8test_Native8_threads(JNIEnv* env, jclass c) {
  CHECK(pthread_spin_init(&g_spin, 0) == 0, "pthread_spin_init");
  CHECK(pthread_barrier_init(&g_barrier, NULL, 3) == 0, "pthread_barrier_init");
  pthread_t t[2];
  for (int i = 0; i < 2; i++)
    pthread_create(&t[i], NULL, Worker, NULL);
  Worker(NULL);
  for (int i = 0; i < 2; i++)
    pthread_join(t[i], NULL);
  CHECK(g_counter == 3000, "counter %d", g_counter);
  CHECK(g_serial == 1, "%d serial threads at the barrier", g_serial);
  CHECK(pthread_spin_trylock(&g_spin) == 0 && pthread_spin_trylock(&g_spin) != 0,
        "pthread_spin_trylock");
  pthread_spin_unlock(&g_spin);
  pthread_barrier_destroy(&g_barrier);
  return Result(env, "ok");
}

// --- network: getifaddrs, if_nameindex, sendmmsg/recvmmsg ---------------------------------

JNIEXPORT jstring JNICALL Java_org_arcwasm_ndk8test_Native8_net(JNIEnv* env, jclass c) {
  struct ifaddrs* ifs = NULL;
  CHECK(getifaddrs(&ifs) == 0 && ifs, "getifaddrs");
  int n = 0, eth0 = 0;
  for (struct ifaddrs* i = ifs; i; i = i->ifa_next) {
    n++;
    if (strcmp(i->ifa_name, "eth0") == 0 && i->ifa_addr && i->ifa_addr->sa_family == AF_INET) {
      struct sockaddr_in* in = (struct sockaddr_in*)i->ifa_addr;
      eth0 = ntohl(in->sin_addr.s_addr) == 0x0a00020f;  // 10.0.2.15
    }
  }
  freeifaddrs(ifs);
  CHECK(n == 2 && eth0, "getifaddrs: %d interfaces, eth0 %d", n, eth0);
  struct if_nameindex* ni = if_nameindex();
  CHECK(ni && ni[0].if_index == 1 && strcmp(ni[0].if_name, "lo") == 0 && ni[2].if_index == 0,
        "if_nameindex");
  if_freenameindex(ni);

  int sv[2];
  if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) != 0)
    return Result(env, "ok getifaddrs (no datagram socketpair here: errno %d)", errno);
  char a[] = "one", b[] = "two";
  struct iovec iov[2] = {{a, 3}, {b, 3}};
  struct mmsghdr out[2];
  memset(out, 0, sizeof out);
  out[0].msg_hdr.msg_iov = &iov[0];
  out[0].msg_hdr.msg_iovlen = 1;
  out[1].msg_hdr.msg_iov = &iov[1];
  out[1].msg_hdr.msg_iovlen = 1;
  int sent = sendmmsg(sv[0], out, 2, 0);
  char r0[8] = {0}, r1[8] = {0};
  struct iovec riov[2] = {{r0, sizeof r0}, {r1, sizeof r1}};
  struct mmsghdr in[2];
  memset(in, 0, sizeof in);
  in[0].msg_hdr.msg_iov = &riov[0];
  in[0].msg_hdr.msg_iovlen = 1;
  in[1].msg_hdr.msg_iov = &riov[1];
  in[1].msg_hdr.msg_iovlen = 1;
  int got = recvmmsg(sv[1], in, 2, 0, NULL);
  close(sv[0]);
  close(sv[1]);
  CHECK(sent == 2 && got == 2 && in[0].msg_len == 3 && strcmp(r0, "one") == 0 &&
            strcmp(r1, "two") == 0,
        "sendmmsg %d, recvmmsg %d: '%s' '%s'", sent, got, r0, r1);
  return Result(env, "ok 2 interfaces, 2 datagrams each way");
}

// --- ASharedMemory (API 26) ----------------------------------------------------------------

JNIEXPORT jstring JNICALL Java_org_arcwasm_ndk8test_Native8_sharedMemory(JNIEnv* env, jclass c) {
  const size_t size = 64 * 1024;
  int fd = ASharedMemory_create("ndk8test", size);
  CHECK(fd >= 0, "ASharedMemory_create: errno %d", errno);
  CHECK(ASharedMemory_getSize(fd) == size, "ASharedMemory_getSize %zu",
        ASharedMemory_getSize(fd));
  char* p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  CHECK(p != MAP_FAILED, "mmap: errno %d", errno);
  strcpy(p + 100, "shared");
  // A second mapping (another "process" of Gecko's) sees the same bytes.
  int fd2 = dup(fd);
  char* q = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd2, 0);
  CHECK(q != MAP_FAILED && strcmp(q + 100, "shared") == 0, "second mapping");
  q[200] = 'x';
  CHECK(p[200] == 'x', "writes through the second mapping");
  CHECK(ASharedMemory_setProt(fd, PROT_READ) == 0, "ASharedMemory_setProt");
  munmap(q, size);
  munmap(p, size);
  close(fd2);
  close(fd);
  return Result(env, "ok");
}

// --- libsync, libnativewindow, libmediandk, android_dlopen_ext -----------------------------

JNIEXPORT jstring JNICALL Java_org_arcwasm_ndk8test_Native8_api26Libraries(JNIEnv* env,
                                                                         jclass c) {
  // No fence fds here (sync_merge of two -1 is EINVAL in both libsyncs).
  CHECK(sync_merge("ndk8", -1, -1) < 0, "sync_merge of no fences");
  // fw8-graphics' AHardwareBuffer is a gralloc buffer; the stand-in fails.
  AHardwareBuffer_Desc desc = {64, 64, 1, 1 /* RGBA_8888 */, 0x33 /* CPU read/write often */};
  void* hb = (void*)1;
  int hr = AHardwareBuffer_allocate(&desc, &hb);
  const char* buffers;
  if (hr == 0) {
    CHECK(hb != NULL && hb != (void*)1, "AHardwareBuffer_allocate: no buffer");
    AHardwareBuffer_Desc got;
    memset(&got, 0, sizeof got);
    AHardwareBuffer_describe(hb, &got);
    CHECK(got.width == 64 && got.height == 64, "describe %ux%u", got.width, got.height);
    void* px = NULL;
    CHECK(AHardwareBuffer_lock(hb, 0x33, -1, NULL, &px) == 0 && px, "AHardwareBuffer_lock");
    memset(px, 0x7f, 64 * 4);
    CHECK(AHardwareBuffer_unlock(hb, NULL) == 0, "AHardwareBuffer_unlock");
    AHardwareBuffer_release(hb);
    buffers = "gralloc";
  } else {
    CHECK(hb == NULL, "AHardwareBuffer_allocate failed but left %p", hb);
    buffers = "none (no gralloc)";
  }
  // libmediandk (the bridge's stub, or fw8-crypto-media's on WebCodecs): no codec
  // for an unknown type; the keys are there.
  CHECK(AMediaCodec_createDecoderByType("video/x-arcwasm-none") == NULL, "AMediaCodec");
  CHECK(AMEDIAFORMAT_KEY_MIME && strcmp(AMEDIAFORMAT_KEY_MIME, "mime") == 0,
        "AMEDIAFORMAT_KEY_MIME");
  void* libc = android_dlopen_ext("libc.so", RTLD_NOW, NULL);
  CHECK(libc && dlsym(libc, "getifaddrs"), "android_dlopen_ext(libc.so)");
  void* aaudio = dlopen("libaaudio.so", RTLD_NOW);
  return Result(env, "ok hardware buffers: %s; libaaudio %s", buffers,
                aaudio ? "present" : "absent (cubeb uses OpenSL ES)");
}
