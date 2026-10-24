// libndk6test.so: what NDK libraries built for Android 5-6 import from the
// system (topic ndk-imports, docs/topics/ndk-imports.md), exercised from
// ARM code: FORTIFY, properties, getauxval, locales and UTF-8, regex, *at,
// fts, epoll/eventfd/timerfd/POSIX timers, zlib, AAsset, ALooper,
// AndroidBitmap, ANativeWindow, EGL with OpenGL ES 3.0, OpenSL ES. Built
// for armeabi-v7a against the NDK's android-18 headers; what those lack
// (API 21+) is declared here as bionic 6.0 has it.
//
// Each native returns "ok" (with details), "skip WHY", or what failed.

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <android/asset_manager_jni.h>
#include <android/bitmap.h>
#include <android/looper.h>
#include <android/native_window_jni.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <link.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <regex.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/sysinfo.h>
#include <sys/uio.h>
#include <sys/system_properties.h>
#include <sys/time.h>
#include <sys/vfs.h>
#include <time.h>
#include <time64.h>
#include <unistd.h>
#include <wchar.h>
#include <zlib.h>

// --- bionic 6.0 functions the android-18 headers do not declare ------------------------

typedef struct __locale_t* locale_t;
locale_t newlocale(int, const char*, locale_t);
locale_t uselocale(locale_t);
void freelocale(locale_t);
size_t __ctype_get_mb_cur_max(void);
long long strtoll_l(const char*, char**, int, locale_t);
int isdigit_l(int, locale_t);
size_t mbsnrtowcs(wchar_t*, const char**, size_t, size_t, mbstate_t*);
size_t wcsnrtombs(char*, const wchar_t**, size_t, size_t, mbstate_t*);
unsigned long getauxval(unsigned long);
int dl_iterate_phdr(int (*)(struct dl_phdr_info*, size_t, void*), void*);  // arm: API 21
int sched_getaffinity(pid_t, size_t, void*);
int __sched_cpucount(size_t, const void*);
int timerfd_create(int, int);
struct itimerspec;
int timerfd_settime(int, int, const struct itimerspec*, struct itimerspec*);
#ifndef TFD_TIMER_ABSTIME
#define TFD_TIMER_ABSTIME 1
#define TFD_NONBLOCK O_NONBLOCK
#endif
int epoll_create1(int);
int inotify_init1(int);  // API 21
int pthread_condattr_setclock(pthread_condattr_t*, clockid_t);
int pthread_getattr_np(pthread_t, pthread_attr_t*);
int sched_getcpu(void);
int utimensat(int, const char*, const struct timespec[2], int);
int posix_fallocate(int, off_t, off_t);
int statvfs(const char*, void*);
int mkdirat(int, const char*, mode_t);
int unlinkat(int, const char*, int);
int faccessat(int, const char*, int, int);
char* realpath(const char*, char*);
int android_set_abort_message(const char*);
// FORTIFY entry points (what -D_FORTIFY_SOURCE makes calls go to).
void* __memcpy_chk(void*, const void*, size_t, size_t);
void* __memset_chk(void*, int, size_t, size_t);
char* __strcpy_chk(char*, const char*, size_t);
char* __strcat_chk(char*, const char*, size_t);
size_t __strlen_chk(const char*, size_t);
char* __strchr_chk(const char*, int, size_t);
int __vsnprintf_chk(char*, size_t, int, size_t, const char*, va_list);
int __snprintf_chk(char*, size_t, int, size_t, const char*, ...);
ssize_t __read_chk(int, void*, size_t, size_t);
void __FD_SET_chk(int, fd_set*, size_t);
int __FD_ISSET_chk(int, const fd_set*, size_t);
int __open_2(const char*, int);
char* __fgets_chk(char*, int, FILE*, size_t);

// fts as bionic 6.0 has it: the NDK's android-18 fts.h predates API 21 and
// has a 16-bit nlink_t (bionic 6.0: 32 bits), so the layout is spelled out.
typedef struct ftsent6 {
  struct ftsent6 *fts_cycle, *fts_parent, *fts_link;
  long fts_number;
  void* fts_pointer;
  char *fts_accpath, *fts_path;
  int fts_errno, fts_symfd;
  size_t fts_pathlen, fts_namelen;
  uint32_t fts_ino, fts_dev, fts_nlink;
  short fts_level;
  unsigned short fts_info, fts_flags, fts_instr;
  struct stat* fts_statp;
  char fts_name[1];
} FTSENT;
typedef struct FTS FTS;
FTS* fts_open(char* const*, int, int (*)(const FTSENT**, const FTSENT**));
FTSENT* fts_read(FTS*);
int fts_close(FTS*);
#define FTS_NOCHDIR 0x0004
#define FTS_PHYSICAL 0x0010
#define FTS_D 1
#define FTS_DP 6
#define FTS_F 8

#ifndef AT_FDCWD
#define AT_FDCWD -100
#endif
#ifndef AT_REMOVEDIR
#define AT_REMOVEDIR 0x200
#endif
#define LC_ALL_MASK 0x1fbf
#define EPOLL_CLOEXEC 02000000
#define EFD_SEMAPHORE 1

// --- results --------------------------------------------------------------------------

static char g_msg[1024];

#define FAIL(...)                                                \
  do {                                                           \
    snprintf(g_msg, sizeof g_msg, __VA_ARGS__);                  \
    return g_msg;                                                \
  } while (0)
#define CHECK(cond, ...) \
  do {                   \
    if (!(cond))         \
      FAIL(__VA_ARGS__); \
  } while (0)

static jstring Result(JNIEnv* env, const char* s) { return (*env)->NewStringUTF(env, s); }

static const char* PathOf(JNIEnv* env, jstring s, char* buf, size_t n) {
  const char* c = (*env)->GetStringUTFChars(env, s, NULL);
  snprintf(buf, n, "%s", c);
  (*env)->ReleaseStringUTFChars(env, s, c);
  return buf;
}

// --- libc: FORTIFY, properties, auxv, locales, UTF-8, regex, misc ------------------------

static int VsnChk(char* buf, size_t n, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = __vsnprintf_chk(buf, n, 0, n, fmt, ap);
  va_end(ap);
  return r;
}

static int g_phdr_count, g_phdr_self;
static int PhdrCallback(struct dl_phdr_info* info, size_t size, void* data) {
  g_phdr_count++;
  if (info->dlpi_name && strstr(info->dlpi_name, "libndk6test.so")) {
    // Our own text: the PT_LOAD that contains this function.
    uintptr_t self = (uintptr_t)&PhdrCallback & ~1u;
    for (int i = 0; i < info->dlpi_phnum; i++) {
      const Elf32_Phdr* p = &info->dlpi_phdr[i];
      if (p->p_type == PT_LOAD && self >= info->dlpi_addr + p->p_vaddr &&
          self < info->dlpi_addr + p->p_vaddr + p->p_memsz)
        g_phdr_self = 1;
    }
  }
  return 0;
}

static const char* Libc(void) {
  char buf[64], small[8];
  // FORTIFY with sizes in bounds (an overflow would abort the process).
  CHECK(__memcpy_chk(buf, "abcdef", 7, sizeof buf) == buf && !strcmp(buf, "abcdef"), "__memcpy_chk");
  __memset_chk(small, 'x', 7, sizeof small);
  small[7] = 0;
  CHECK(!strcmp(small, "xxxxxxx"), "__memset_chk");
  __strcpy_chk(buf, "foo", sizeof buf);
  __strcat_chk(buf, "bar", sizeof buf);
  CHECK(!strcmp(buf, "foobar") && __strlen_chk(buf, sizeof buf) == 6, "__strcpy/strcat/strlen_chk");
  CHECK(__strchr_chk(buf, 'b', sizeof buf) == buf + 3, "__strchr_chk");
  CHECK(VsnChk(buf, sizeof buf, "%d-%s-%.2f", 42, "x", 1.5) == 9 && !strcmp(buf, "42-x-1.50"),
        "__vsnprintf_chk: %s", buf);
  CHECK(__snprintf_chk(buf, sizeof buf, 0, sizeof buf, "%lld|%c", 1234567890123LL, 'z') == 15 &&
            !strcmp(buf, "1234567890123|z"),
        "__snprintf_chk: %s", buf);
  fd_set set;
  FD_ZERO(&set);
  __FD_SET_chk(5, &set, sizeof set);
  CHECK(__FD_ISSET_chk(5, &set, sizeof set) && !__FD_ISSET_chk(6, &set, sizeof set), "__FD_*_chk");
  int fd = __open_2("/proc/self/cmdline", O_RDONLY);
  if (fd < 0)
    fd = __open_2("/dev/zero", O_RDONLY);
  CHECK(fd >= 0, "__open_2: errno %d", errno);
  CHECK(__read_chk(fd, buf, 4, sizeof buf) >= 0, "__read_chk");
  close(fd);

  // System properties.
  const prop_info* pi = __system_property_find("ro.build.version.sdk");
  CHECK(pi != NULL, "__system_property_find(ro.build.version.sdk)");
  char name[PROP_NAME_MAX], value[PROP_VALUE_MAX];
  int n = __system_property_read(pi, name, value);
  CHECK(n > 0 && !strcmp(name, "ro.build.version.sdk") && atoi(value) > 0,
        "__system_property_read: %d %s=%s", n, name, value);
  CHECK(__system_property_find("no.such.property") == NULL, "a missing property was found");

  // getauxval: the CPU's features, as a kernel tells them.
  unsigned long hwcap = getauxval(16 /* AT_HWCAP */);
  CHECK((hwcap & (1 << 12)) && (hwcap & (1 << 13)) && (hwcap & (1 << 17)),
        "AT_HWCAP 0x%lx lacks NEON/VFPv3/IDIVA", hwcap);
  CHECK(getauxval(6 /* AT_PAGESZ */) == 4096, "AT_PAGESZ");
  CHECK(getauxval(25 /* AT_RANDOM */) != 0, "AT_RANDOM");

  // dl_iterate_phdr finds this library with its program headers.
  g_phdr_count = g_phdr_self = 0;
  dl_iterate_phdr(PhdrCallback, NULL);
  CHECK(g_phdr_count >= 1 && g_phdr_self, "dl_iterate_phdr: %d libraries, self %d", g_phdr_count,
        g_phdr_self);

  // Locales: locale_t, *_l, MB_CUR_MAX, UTF-8 conversions as bionic 6.
  locale_t l = newlocale(LC_ALL_MASK, "C.UTF-8", NULL);
  CHECK(l != NULL, "newlocale(C.UTF-8)");
  locale_t old = uselocale(l);
  CHECK(__ctype_get_mb_cur_max() == 4, "MB_CUR_MAX in C.UTF-8: %zu", __ctype_get_mb_cur_max());
  CHECK(strtoll_l("-9000000000", NULL, 10, l) == -9000000000LL && isdigit_l('7', l), "*_l");
  uselocale(old);
  freelocale(l);
  CHECK(newlocale(LC_ALL_MASK, "de_DE", NULL) == NULL && errno == ENOENT, "newlocale(de_DE)");
  const char* utf8 = "a\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80";  // a é € 😀
  wchar_t w[8];
  mbstate_t st;
  memset(&st, 0, sizeof st);
  const char* src = utf8;
  size_t wn = mbsnrtowcs(w, &src, strlen(utf8) + 1, 8, &st);
  CHECK(wn == 4 && w[0] == 'a' && w[1] == 0xe9 && w[2] == 0x20ac && w[3] == 0x1f600,
        "mbsnrtowcs: %zu %x %x %x %x", wn, w[0], w[1], w[2], w[3]);
  char back[16];
  const wchar_t* wsrc = w;
  memset(&st, 0, sizeof st);
  w[4] = 0;
  size_t bn = wcsnrtombs(back, &wsrc, 5, sizeof back, &st);
  CHECK(bn == 10 && !memcmp(back, utf8, 10), "wcsnrtombs: %zu", bn);
  memset(&st, 0, sizeof st);
  wchar_t one;
  CHECK(mbrtowc(&one, "\xe2\x82", 2, &st) == (size_t)-2 && mbrtowc(&one, "\xac", 1, &st) == 1 &&
            one == 0x20ac,
        "mbrtowc over two calls");
  wchar_t wbuf[32];
  CHECK(swprintf(wbuf, 32, L"%d %ls", 7, L"\x20ac") == 3 && wbuf[2] == 0x20ac, "swprintf");

  // regex (NetBSD flag values).
  regex_t re;
  regmatch_t m[3];
  CHECK(regcomp(&re, "([a-z]+)-([0-9]+)", REG_EXTENDED) == 0 && re.re_nsub == 2, "regcomp");
  CHECK(regexec(&re, "xx abc-123 yy", 3, m, 0) == 0 && m[1].rm_so == 3 && m[2].rm_eo == 10,
        "regexec: %d..%d", (int)m[1].rm_so, (int)m[2].rm_eo);
  CHECK(regexec(&re, "nothing", 0, NULL, 0) == REG_NOMATCH, "REG_NOMATCH");
  regfree(&re);
  CHECK(regcomp(&re, "HeLLo", REG_ICASE | REG_NOSUB) == 0 && regexec(&re, "say hello", 0, NULL, 0) == 0,
        "REG_ICASE");
  regfree(&re);

  // Processes: honest refusals.
  errno = 0;
  CHECK(fork() == -1 && errno == ENOSYS, "fork: errno %d", errno);
  CHECK(system(NULL) == 0, "system(NULL): a shell?");
  CHECK(popen("ls", "r") == NULL, "popen");
  // prctl names the thread.
  char tname[16] = {0};
  CHECK(prctl(PR_SET_NAME, (unsigned long)"ndk6-test", 0, 0, 0) == 0 &&
            prctl(PR_GET_NAME, (unsigned long)tname, 0, 0, 0) == 0 && !strcmp(tname, "ndk6-test"),
        "prctl name: %s", tname);
  // Scheduling, resources. As many CPUs as /proc/cpuinfo lists (the browser's
  // cores): with one, Dart's worker_manager made no worker and spun forever.
  int listed = 0;
  FILE* ci = fopen("/proc/cpuinfo", "r");
  CHECK(ci != NULL, "/proc/cpuinfo: errno %d", errno);
  char line[256];
  while (fgets(line, sizeof line, ci))
    if (!strncmp(line, "processor", 9)) ++listed;
  fclose(ci);
  long online = sysconf(_SC_NPROCESSORS_ONLN);
  CHECK(listed >= 1 && online == listed && sysconf(_SC_NPROCESSORS_CONF) == listed,
        "sysconf: %ld CPUs online, /proc/cpuinfo lists %d", online, listed);
  uint32_t cpus[32];  // a cpu_set_t of bionic 6.0 (1024 bits)
  CHECK(sched_getaffinity(0, sizeof cpus, cpus) == 0 && (cpus[0] & 1) &&
            __sched_cpucount(sizeof cpus, cpus) == listed,
        "sched_getaffinity: %d CPUs", __sched_cpucount(sizeof cpus, cpus));
  CHECK(sched_getcpu() == 0, "sched_getcpu");
  struct sysinfo si;
  CHECK(sysinfo(&si) == 0 && si.totalram > si.freeram && si.mem_unit == 1, "sysinfo");
  struct rlimit rl;
  CHECK(getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur >= 256, "getrlimit");
  // The guest stack as pthread_getattr_np reports it: this frame is in it.
  pthread_attr_t attr;
  void* stack = NULL;
  size_t stack_size = 0;
  CHECK(pthread_getattr_np(pthread_self(), &attr) == 0 &&
            pthread_attr_getstack(&attr, &stack, &stack_size) == 0,
        "pthread_getattr_np");
  char here;
  CHECK((char*)&here > (char*)stack && (char*)&here < (char*)stack + stack_size,
        "the stack %p+%zu does not contain %p", stack, stack_size, &here);
  // time64.
  time64_t t64 = 4102444800LL;  // 2100-01-01
  struct tm tm;
  CHECK(gmtime64_r(&t64, &tm) && tm.tm_year == 200 && timegm64(&tm) == t64, "time64");
  return "ok";
}

JNIEXPORT jstring JNICALL Java_org_arcwasm_ndk6test_Native6_libc(JNIEnv* env, jclass c) {
  return Result(env, Libc());
}

// --- files: *at, fts, statfs, realpath, mremap ------------------------------------------------

static const char* Files(const char* dir) {
  char path[512];
  int dfd = open(dir, O_RDONLY | O_DIRECTORY);
  CHECK(dfd >= 0, "open(%s): %d", dir, errno);
  unlinkat(dfd, "t6/f1", 0);
  unlinkat(dfd, "t6/sub/f2", 0);
  unlinkat(dfd, "t6/sub", AT_REMOVEDIR);
  unlinkat(dfd, "t6", AT_REMOVEDIR);
  CHECK(mkdirat(dfd, "t6", 0700) == 0, "mkdirat: %d", errno);
  int fd = openat(dfd, "t6/f1", O_CREAT | O_WRONLY | O_TRUNC, 0600);
  CHECK(fd >= 0, "openat: %d", errno);
  CHECK(write(fd, "hello", 5) == 5, "write");
  CHECK(posix_fallocate(fd, 0, 4096) == 0, "posix_fallocate");
  close(fd);
  struct stat st;
  CHECK(fstatat(dfd, "t6/f1", &st, 0) == 0 && st.st_size == 4096, "fstatat: size %lld",
        (long long)st.st_size);
  CHECK(faccessat(dfd, "t6/f1", R_OK, 0) == 0, "faccessat");
  int sub = openat(dfd, "t6", O_RDONLY | O_DIRECTORY);
  CHECK(sub >= 0 && mkdirat(sub, "sub", 0700) == 0, "mkdirat via a directory fd: %d", errno);
  fd = openat(sub, "sub/f2", O_CREAT | O_WRONLY, 0600);
  CHECK(fd >= 0, "openat sub/f2");
  close(fd);
  CHECK(utimensat(sub, "sub/f2", NULL, 0) == 0, "utimensat");
  // fts: a physical walk, preorder and postorder.
  snprintf(path, sizeof path, "%s/t6", dir);
  char* roots[] = {path, NULL};
  FTS* fts = fts_open(roots, FTS_PHYSICAL | FTS_NOCHDIR, NULL);
  CHECK(fts != NULL, "fts_open");
  int files = 0, dirs = 0, post = 0;
  FTSENT* e;
  while ((e = fts_read(fts)) != NULL) {
    if (e->fts_info == FTS_F) files++;
    if (e->fts_info == FTS_D) dirs++;
    if (e->fts_info == FTS_DP) post++;
    CHECK(e->fts_info == FTS_DP || e->fts_info == FTS_D || e->fts_statp->st_mode != 0,
          "fts: no stat for %s", e->fts_path);
  }
  fts_close(fts);
  CHECK(files == 2 && dirs == 2 && post == 2, "fts: %d files, %d dirs, %d postorder", files, dirs,
        post);
  char real[512], want[512];
  snprintf(path, sizeof path, "%s/t6/sub/../f1", dir);
  CHECK(realpath(path, real) != NULL, "realpath: %d", errno);
  CHECK(realpath(dir, want) != NULL, "realpath(dir)");
  strcat(want, "/t6/f1");
  CHECK(!strcmp(real, want), "realpath %s != %s", real, want);
  struct statfs sfs;
  CHECK(statfs(dir, &sfs) == 0 && sfs.f_bsize == 4096 && sfs.f_bavail > 0, "statfs");
  CHECK(unlinkat(sub, "sub/f2", 0) == 0 && unlinkat(sub, "sub", AT_REMOVEDIR) == 0 &&
            unlinkat(dfd, "t6/f1", 0) == 0 && unlinkat(dfd, "t6", AT_REMOVEDIR) == 0,
        "unlinkat: %d", errno);
  close(sub);
  close(dfd);
  // FIFOs as Realm uses them: every open of the path reaches one pipe; a
  // byte written through one O_RDWR descriptor wakes a poll on another.
  snprintf(path, sizeof path, "%s/t6.note", dir);
  int made = mkfifo(path, 0600);
  CHECK(made == 0 || errno == EEXIST, "mkfifo: %d", errno);
  CHECK(mkfifo(path, 0600) == -1 && errno == EEXIST, "mkfifo again: %d", errno);
  int fa = open(path, O_RDWR), fb = open(path, O_RDWR);
  CHECK(fa >= 0 && fb >= 0, "open fifo: %d", errno);
  fcntl(fa, F_SETFL, O_NONBLOCK);
  fcntl(fb, F_SETFL, O_NONBLOCK);
  char c = 0;
  while (read(fb, &c, 1) == 1) {}  // a byte left by an earlier run in this VM
  CHECK(read(fb, &c, 1) == -1 && errno == EAGAIN, "empty fifo: %d", errno);
  CHECK(write(fa, "n", 1) == 1, "write fifo: %d", errno);
  struct pollfd pf = {fb, POLLIN, 0};
  CHECK(poll(&pf, 1, 1000) == 1 && (pf.revents & POLLIN), "poll fifo");
  CHECK(read(fb, &c, 1) == 1 && c == 'n', "read fifo");
  close(fa);
  close(fb);
  // As Firebase Messaging wakes its thread: a writer opens and closes, the
  // reader gets end of file; a reader opened after that waits again.
  int fr = open(path, O_RDONLY | O_NONBLOCK);
  CHECK(fr >= 0, "open fifo for reading: %d", errno);
  CHECK(read(fr, &c, 1) == -1 && errno == EAGAIN, "fifo without writer waits: %d", errno);
  int fw = open(path, O_WRONLY);
  CHECK(fw >= 0, "open fifo for writing: %d", errno);
  close(fw);
  CHECK(read(fr, &c, 1) == 0, "fifo end of file after the last writer: %d", errno);
  close(fr);
  fr = open(path, O_RDONLY | O_NONBLOCK);
  CHECK(fr >= 0 && read(fr, &c, 1) == -1 && errno == EAGAIN, "fifo reopened waits: %d", errno);
  close(fr);
  // inotify sees the bridge's own writes: IN_CLOSE_WRITE on a watch of the
  // file and, with the name, of its directory (Firebase Messaging touches its
  // storage file so to wake its thread).
  snprintf(path, sizeof path, "%s/t6.watched", dir);
  int wf = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  CHECK(wf >= 0, "create watched: %d", errno);
  close(wf);
  int in = inotify_init1(O_NONBLOCK);
  int wfile = inotify_add_watch(in, path, IN_CLOSE_WRITE);
  int wdir = inotify_add_watch(in, dir, IN_CLOSE_WRITE | IN_CREATE);
  CHECK(in >= 0 && wfile >= 1 && wdir >= 1 && wdir != wfile, "inotify watches: %d", errno);
  union {
    struct inotify_event e;
    char b[512];
  } ib;
  CHECK(read(in, ib.b, sizeof ib.b) == -1 && errno == EAGAIN, "inotify quiet before");
  int rf = open(path, O_RDONLY);
  close(rf);
  CHECK(read(in, ib.b, sizeof ib.b) == -1 && errno == EAGAIN, "inotify: a read-only close is no write");
  FILE* ff = fopen(path, "a");
  CHECK(ff != NULL, "fopen watched: %d", errno);
  fclose(ff);
  ssize_t got = read(in, ib.b, sizeof ib.b);
  CHECK(got == 16 + 32, "inotify events: %d bytes", (int)got);
  int seen_file = 0, seen_dir = 0;
  for (ssize_t at = 0; at < got;) {
    struct inotify_event* ie = (struct inotify_event*)(ib.b + at);
    CHECK(ie->mask == IN_CLOSE_WRITE, "inotify event mask %x", ie->mask);
    if (ie->wd == wfile && ie->len == 0)
      seen_file = 1;
    else if (ie->wd == wdir && ie->len == 16 && !strcmp(ie->name, "t6.watched"))
      seen_dir = 1;
    at += sizeof *ie + ie->len;
  }
  CHECK(seen_file && seen_dir, "inotify: the file's and the directory's IN_CLOSE_WRITE");
  close(in);
  unlink(path);
  snprintf(path, sizeof path, "%s/t6.cv", dir);
  CHECK(mknod(path, S_IFIFO | 0600, 0) == 0 || errno == EEXIST, "mknod S_IFIFO: %d", errno);
  snprintf(path, sizeof path, "%s/t6.dev", dir);
  CHECK(mknod(path, S_IFCHR | 0600, 0) == -1 && errno == EPERM, "mknod S_IFCHR");
  // mremap grows a mapping, keeping its contents.
  char* p = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(p != MAP_FAILED, "mmap");
  strcpy(p + 8000, "kept");
  char* q = mremap(p, 8192, 65536, 1 /* MREMAP_MAYMOVE */);
  CHECK(q != MAP_FAILED && !strcmp(q + 8000, "kept"), "mremap");
  munmap(q, 65536);
  return "ok";
}

JNIEXPORT jstring JNICALL Java_org_arcwasm_ndk6test_Native6_files(JNIEnv* env, jclass c,
                                                                  jstring dir) {
  char buf[512];
  return Result(env, Files(PathOf(env, dir, buf, sizeof buf)));
}

// --- events: epoll, eventfd, timerfd, POSIX timers, monotonic conditions ----------------------

static int64_t NowMs(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}

static pthread_mutex_t g_timer_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_timer_fired;
static void TimerFired(union sigval v) {
  pthread_mutex_lock(&g_timer_lock);
  g_timer_fired += v.sival_int;
  pthread_mutex_unlock(&g_timer_lock);
}

static const char* Events(void) {
  int ep = epoll_create1(EPOLL_CLOEXEC);
  CHECK(ep >= 0, "epoll_create1");
  int efd = eventfd(0, EFD_NONBLOCK);
  CHECK(efd >= 0, "eventfd");
  uint64_t v = 0;
  CHECK(read(efd, &v, 8) == -1 && errno == EAGAIN, "eventfd: a read with nothing in it");
  struct epoll_event ev = {EPOLLIN, {.u32 = 7}}, out[4];
  CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, efd, &ev) == 0, "epoll_ctl eventfd");
  CHECK(epoll_wait(ep, out, 4, 0) == 0, "epoll: ready too early");
  v = 3;
  CHECK(write(efd, &v, 8) == 8, "eventfd write");
  v = 4;
  write(efd, &v, 8);
  CHECK(epoll_wait(ep, out, 4, 100) == 1 && out[0].data.u32 == 7, "epoll: the eventfd");
  CHECK(read(efd, &v, 8) == 8 && v == 7, "eventfd: %llu", (unsigned long long)v);
  CHECK(epoll_wait(ep, out, 4, 0) == 0, "eventfd still readable");
  // A semaphore eventfd counts down by one.
  int sfd = eventfd(2, EFD_SEMAPHORE);
  CHECK(read(sfd, &v, 8) == 8 && v == 1 && read(sfd, &v, 8) == 8 && v == 1, "EFD_SEMAPHORE");
  close(sfd);
  // inotify: a descriptor that takes watches and stays quiet (the VFS has no
  // change notifications; Dart's Directory.watch had ENOSYS, Saber).
  int ifd = inotify_init1(O_NONBLOCK | O_CLOEXEC);
  CHECK(ifd >= 0, "inotify_init1: errno %d", errno);
  int wd = inotify_add_watch(ifd, "/system", IN_CREATE | IN_DELETE | IN_MODIFY);
  CHECK(wd >= 1 && inotify_add_watch(ifd, "/system", IN_MODIFY) == wd,
        "inotify_add_watch: %d, the same path again", wd);
  CHECK(inotify_add_watch(ifd, "/no/such/dir", IN_MODIFY) == -1 && errno == ENOENT,
        "inotify_add_watch of a missing path");
  CHECK(inotify_add_watch(efd, "/system", IN_MODIFY) == -1 && errno == EINVAL,
        "inotify_add_watch on an eventfd");
  ev.data.u32 = 8;
  CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, ifd, &ev) == 0, "epoll_ctl inotify");
  char ibuf[256];
  CHECK(epoll_wait(ep, out, 4, 0) == 0 && read(ifd, ibuf, sizeof ibuf) == -1 && errno == EAGAIN,
        "inotify: no events");
  CHECK(inotify_rm_watch(ifd, wd) == 0, "inotify_rm_watch");
  CHECK(inotify_rm_watch(ifd, wd) == -1 && errno == EINVAL, "inotify_rm_watch twice");
  epoll_ctl(ep, EPOLL_CTL_DEL, ifd, NULL);
  close(ifd);
  // timerfd: one shot after 30 ms, then every 10 ms.
  int tfd = timerfd_create(CLOCK_MONOTONIC, 0);
  CHECK(tfd >= 0, "timerfd_create");
  ev.data.u32 = 9;
  CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, tfd, &ev) == 0, "epoll_ctl timerfd");
  struct itimerspec its = {{0, 10 * 1000000}, {0, 30 * 1000000}};
  int64_t t0 = NowMs();
  CHECK(timerfd_settime(tfd, 0, &its, NULL) == 0, "timerfd_settime");
  int n = epoll_wait(ep, out, 4, 1000);
  int64_t dt = NowMs() - t0;
  CHECK(n == 1 && out[0].data.u32 == 9, "epoll: the timerfd (%d)", n);
  CHECK(dt >= 25 && dt < 500, "timerfd fired after %lld ms", (long long)dt);
  CHECK(read(tfd, &v, 8) == 8 && v >= 1, "timerfd read");
  usleep(55 * 1000);
  CHECK(read(tfd, &v, 8) == 8 && v >= 3 && v <= 7, "timerfd: %llu expirations in 55 ms",
        (unsigned long long)v);
  struct itimerspec off = {{0, 0}, {0, 0}};
  timerfd_settime(tfd, 0, &off, NULL);
  close(tfd);
  // Re-armed for the deadline it already has (Flutter's message loops do so
  // for the earliest task at every post), a timerfd still fires.
  tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
  struct itimerspec at = {{0, 0}, {0, 0}};
  clock_gettime(CLOCK_MONOTONIC, &at.it_value);
  at.it_value.tv_nsec += 20 * 1000000;
  if (at.it_value.tv_nsec >= 1000000000) {
    at.it_value.tv_sec++;
    at.it_value.tv_nsec -= 1000000000;
  }
  for (int i = 0; i < 3; i++)
    CHECK(timerfd_settime(tfd, TFD_TIMER_ABSTIME, &at, NULL) == 0, "timerfd_settime ABSTIME");
  struct pollfd pf = {tfd, POLLIN, 0};
  CHECK(poll(&pf, 1, 1000) == 1, "timerfd re-armed for the same deadline never fired");
  CHECK(read(tfd, &v, 8) == 8 && v == 1, "timerfd re-armed: %llu expirations",
        (unsigned long long)v);
  close(tfd);
  close(efd);
  close(ep);
  // A POSIX timer with SIGEV_THREAD.
  struct sigevent se;
  memset(&se, 0, sizeof se);
  se.sigev_notify = SIGEV_THREAD;
  se.sigev_notify_function = TimerFired;
  se.sigev_value.sival_int = 1;
  timer_t timer;
  CHECK(timer_create(CLOCK_MONOTONIC, &se, &timer) == 0, "timer_create");
  struct itimerspec ts = {{0, 0}, {0, 20 * 1000000}};
  g_timer_fired = 0;
  CHECK(timer_settime(timer, 0, &ts, NULL) == 0, "timer_settime");
  for (int i = 0; i < 100; i++) {
    usleep(10 * 1000);
    pthread_mutex_lock(&g_timer_lock);
    int fired = g_timer_fired;
    pthread_mutex_unlock(&g_timer_lock);
    if (fired)
      break;
  }
  timer_delete(timer);
  CHECK(g_timer_fired == 1, "the timer's thread ran %d times", g_timer_fired);
  // A condition variable on CLOCK_MONOTONIC times out on that clock.
  pthread_condattr_t ca;
  pthread_condattr_init(&ca);
  CHECK(pthread_condattr_setclock(&ca, CLOCK_MONOTONIC) == 0, "pthread_condattr_setclock");
  pthread_cond_t cond;
  pthread_cond_init(&cond, &ca);
  pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
  struct timespec deadline;
  clock_gettime(CLOCK_MONOTONIC, &deadline);
  deadline.tv_nsec += 30 * 1000000;
  if (deadline.tv_nsec >= 1000000000) {
    deadline.tv_sec++;
    deadline.tv_nsec -= 1000000000;
  }
  pthread_mutex_lock(&m);
  t0 = NowMs();
  int r = pthread_cond_timedwait(&cond, &m, &deadline);
  dt = NowMs() - t0;
  pthread_mutex_unlock(&m);
  CHECK(r == ETIMEDOUT && dt >= 20 && dt < 1000, "monotonic timedwait: %d after %lld ms", r,
        (long long)dt);
  return "ok";
}

JNIEXPORT jstring JNICALL Java_org_arcwasm_ndk6test_Native6_events(JNIEnv* env, jclass c) {
  return Result(env, Events());
}

// --- syscalls: raw ARM EABI svc, as TikTok's security libraries issue -------------------------
//
// libmetasec_ov/liboecsec_ov and libnpth* reach the kernel with "svc #0"
// (number in r7) instead of calling bionic. The bridge's syscall table
// (shim/ndk/ndk_syscall.cc) must map those numbers; this exercises them
// from Thumb-2 code with an inline-asm svc. On failure a syscall returns a
// negative bionic errno, exactly as the real kernel ABI does.

// bionic open() flags (asm/fcntl.h on ARM).
#define G_O_RDONLY 0
#define G_O_WRONLY 01
#define G_O_CREAT 0100
#define G_O_TRUNC 01000
#define G_O_DIRECTORY 040000

struct GDent64 {
  uint64_t d_ino;
  int64_t d_off;
  uint16_t d_reclen;
  uint8_t d_type;
  char d_name[256];
};

// A raw syscall with up to six arguments (r7 = number, r0-r5 = args). r7 is
// the Thumb frame pointer, so it is saved around the svc, the way bionic's
// own syscall stubs do.
__attribute__((noinline)) static long sc6(long nr, long a0, long a1, long a2, long a3, long a4,
                                          long a5) {
  register long r0 __asm__("r0") = a0;
  register long r1 __asm__("r1") = a1;
  register long r2 __asm__("r2") = a2;
  register long r3 __asm__("r3") = a3;
  register long r4 __asm__("r4") = a4;
  register long r5 __asm__("r5") = a5;
  __asm__ volatile(
      "push {r7}\n\t"
      "mov r7, %[nr]\n\t"
      "svc #0\n\t"
      "pop {r7}\n\t"
      : "+r"(r0)
      : "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5), [nr] "r"(nr)
      : "memory");
  return r0;
}
#define SC0(n) sc6((n), 0, 0, 0, 0, 0, 0)
#define SC1(n, a) sc6((n), (long)(a), 0, 0, 0, 0, 0)
#define SC2(n, a, b) sc6((n), (long)(a), (long)(b), 0, 0, 0, 0)
#define SC3(n, a, b, c) sc6((n), (long)(a), (long)(b), (long)(c), 0, 0, 0)
#define SC4(n, a, b, c, d) sc6((n), (long)(a), (long)(b), (long)(c), (long)(d), 0, 0)

static const char* Syscalls(const char* dir) {
  int n = 0;
  char path[512], buf[64];
  snprintf(path, sizeof path, "%s/sc_file", dir);
  SC1(10, path);  // unlink: clean up a previous run (ENOENT is fine)

  // openat + write + close (322, 4, 6).
  long fd = SC4(322, AT_FDCWD, path, G_O_CREAT | G_O_WRONLY | G_O_TRUNC, 0600);
  CHECK(fd >= 0, "openat(create): %ld", fd);
  CHECK(SC3(4, fd, "svc!", 4) == 4, "write");
  // writev (146): two buffers appended.
  struct iovec iov[2] = {{(void*)"-", 1}, {(void*)"ok", 2}};
  CHECK(SC3(146, fd, iov, 2) == 3, "writev");
  // fstat64 (197): the file is now 7 bytes.
  struct stat st;
  CHECK(SC2(197, fd, &st) == 0 && st.st_size == 7, "fstat64: size %lld", (long long)st.st_size);
  CHECK(SC1(6, fd) == 0, "close");
  n += 4;

  // stat64 / lstat64 (195, 196) on the path.
  memset(&st, 0, sizeof st);
  CHECK(SC2(195, path, &st) == 0 && st.st_size == 7, "stat64");
  CHECK(SC2(196, path, &st) == 0 && st.st_size == 7, "lstat64");
  // faccessat (334): the file exists.
  CHECK(SC3(334, AT_FDCWD, path, 0 /* F_OK */) == 0, "faccessat");
  n += 3;

  // read it back: openat read-only, lseek, read (322, 19, 3).
  fd = SC4(322, AT_FDCWD, path, G_O_RDONLY, 0);
  CHECK(fd >= 0, "openat(read): %ld", fd);
  CHECK(SC3(19, fd, 1, 0 /* SEEK_SET */) == 1, "lseek");
  long got = SC3(3, fd, buf, sizeof buf);
  CHECK(got == 6 && !memcmp(buf, "vc!-ok", 6), "read: %ld bytes", got);
  SC1(6, fd);
  n += 2;

  // getdents64 (217): find sc_file in the directory.
  int dfd = SC4(322, AT_FDCWD, dir, G_O_RDONLY | G_O_DIRECTORY, 0);
  CHECK(dfd >= 0, "openat(dir): %d", dfd);
  char dents[2048];
  int found = 0;
  for (;;) {
    long len = SC3(217, dfd, dents, sizeof dents);
    CHECK(len >= 0, "getdents64: %ld", len);
    if (len == 0)
      break;
    for (long off = 0; off < len;) {
      struct GDent64* e = (struct GDent64*)(dents + off);
      if (!strcmp(e->d_name, "sc_file"))
        found = 1;
      off += e->d_reclen;
    }
  }
  SC1(6, dfd);
  CHECK(found, "getdents64 did not list sc_file");
  n += 1;

  // mmap2 + mprotect + munmap (192, 125, 91): an anonymous page kept writable.
  long p = sc6(192, 0, 4096, 3 /* RW */, 0x22 /* PRIVATE|ANON */, -1, 0);
  CHECK(p > 0 && (unsigned long)p < 0xfffff000u, "mmap2: %ld", p);
  strcpy((char*)p, "mapped");
  CHECK(SC3(125, p, 4096, 3) == 0, "mprotect");
  CHECK(!strcmp((char*)p, "mapped"), "the mapping lost its contents");
  CHECK(SC2(91, p, 4096) == 0, "munmap");
  n += 3;

  // identity and info (199, 20, 224, 116).
  CHECK(SC0(199) == (long)getuid(), "getuid32");
  CHECK(SC0(20) > 0, "getpid");
  CHECK(SC0(224) > 0, "gettid");
  struct sysinfo si;
  memset(&si, 0, sizeof si);
  CHECK(SC1(116, &si) == 0 && si.totalram > 0, "sysinfo");
  CHECK(SC2(172, 15 /* PR_SET_NAME */, "sc_test") == 0, "prctl(PR_SET_NAME)");
  n += 5;

  // things the one wasm process refuses, with the kernel's own errno.
  CHECK(SC0(2) == -ENOSYS, "fork should be -ENOSYS, got %ld", SC0(2));
  CHECK(SC4(114, -1, 0, 0, 0) == -ECHILD, "wait4 should be -ECHILD");
  CHECK(SC4(322, AT_FDCWD, "/no/such/path/sc", G_O_RDONLY, 0) == -ENOENT,
        "openat of a missing file should be -ENOENT");
  n += 3;

  snprintf(g_msg, sizeof g_msg, "ok %d raw syscalls", n);
  SC1(10, path);  // unlink
  return g_msg;
}

JNIEXPORT jstring JNICALL Java_org_arcwasm_ndk6test_Native6_syscalls(JNIEnv* env, jclass c,
                                                                     jstring dir) {
  char buf[512];
  return Result(env, Syscalls(PathOf(env, dir, buf, sizeof buf)));
}

// --- Go's raw syscalls (topic compat-wave3) -------------------------------------------------
// Go's runtime and net package reach the kernel with svc themselves: sockets,
// an edge-triggered epoll, SIGURG on a sigaltstack (SA_ONSTACK).

#ifndef SOCK_NONBLOCK   // (android-18's headers lack these; Linux ARM's values)
#define SOCK_NONBLOCK 04000
#define SOCK_CLOEXEC 02000000
#endif
#ifndef EPOLLONESHOT
#define EPOLLONESHOT (1u << 30)
#endif

static volatile uintptr_t g_onstack_sp;
static volatile int g_onstack_flags = -1;
static void OnStackHandler(int sig, siginfo_t* info, void* uc) {
  char here;
  g_onstack_sp = (uintptr_t)&here;
  stack_t cur;
  if (SC2(186, 0, &cur) == 0) g_onstack_flags = cur.ss_flags;
}

static const char* RawNet(void) {
  int n = 0;
  // socketpair (288), non-blocking and close-on-exec as Go asks.
  int sv[2] = {-1, -1};
  CHECK(SC4(288, AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sv) == 0, "socketpair");
  int type = 0;
  socklen_t len = sizeof type;
  CHECK(sc6(295, sv[0], SOL_SOCKET, SO_TYPE, (long)&type, (long)&len, 0) == 0 && type == SOCK_STREAM,
        "getsockopt(SO_TYPE): %d", type);
  char buf[16];
  CHECK(SC4(291, sv[0], buf, sizeof buf, 0) == -EAGAIN, "recv of nothing: -EAGAIN");
  n += 2;

  // epoll_create1 (357), EPOLLET: one event per edge (Go registers every fd so).
  int ep = SC1(357, EPOLL_CLOEXEC);
  CHECK(ep >= 0, "epoll_create1: %d", ep);
  struct epoll_event ev = {EPOLLIN | EPOLLET, {.u32 = 7}}, out[4];
  CHECK(SC4(251, ep, EPOLL_CTL_ADD, sv[0], &ev) == 0, "epoll_ctl(ADD)");
  CHECK(SC4(252, ep, out, 4, 0) == 0, "epoll_wait: ready before any data");
  CHECK(sc6(290, sv[1], (long)"abc", 3, 0, 0, 0) == 3, "sendto");
  long r = SC4(252, ep, out, 4, 1000);
  CHECK(r == 1 && out[0].data.u32 == 7 && (out[0].events & EPOLLIN), "epoll_wait after data: %ld", r);
  CHECK(SC4(252, ep, out, 4, 0) == 0, "EPOLLET: the same edge reported twice");
  CHECK(SC4(289, sv[1], "d", 1, 0) == 1, "send");
  CHECK(SC4(252, ep, out, 4, 1000) == 1, "EPOLLET: new data is a new edge");
  CHECK(sc6(292, sv[0], (long)buf, sizeof buf, 0, 0, 0) == 4 && !memcmp(buf, "abcd", 4), "recvfrom");
  n += 6;

  // EPOLLONESHOT: off after one event until EPOLL_CTL_MOD.
  ev.events = EPOLLIN | EPOLLONESHOT;
  CHECK(SC4(251, ep, EPOLL_CTL_MOD, sv[0], &ev) == 0, "epoll_ctl(MOD)");
  CHECK(SC4(289, sv[1], "e", 1, 0) == 1, "send");
  CHECK(SC4(252, ep, out, 4, 1000) == 1, "EPOLLONESHOT: the first event");
  CHECK(SC4(252, ep, out, 4, 0) == 0, "EPOLLONESHOT: a second event");
  CHECK(SC4(251, ep, EPOLL_CTL_MOD, sv[0], &ev) == 0 && SC4(252, ep, out, 4, 0) == 1,
        "EPOLLONESHOT: re-armed");
  n += 3;

  // sendmsg / recvmsg (296, 297).
  struct iovec iov = {(void*)"msg", 3};
  struct msghdr mh;
  memset(&mh, 0, sizeof mh);
  mh.msg_iov = &iov;
  mh.msg_iovlen = 1;
  CHECK(SC3(296, sv[1], &mh, 0) == 3, "sendmsg");
  char rb[16];
  struct iovec riov = {rb, sizeof rb};
  mh.msg_iov = &riov;
  CHECK(SC3(297, sv[0], &mh, 0) == 4 && !memcmp(rb, "emsg", 4), "recvmsg");
  CHECK(SC3(297, sv[0], &mh, MSG_DONTWAIT) == -EAGAIN, "recvmsg of nothing: -EAGAIN");
  n += 3;
  SC1(6, ep);
  SC1(6, sv[0]);
  SC1(6, sv[1]);

  // socket (281) + connect (283) to a path nobody listens on.
  int s = SC3(281, AF_UNIX, SOCK_STREAM, 0);
  CHECK(s >= 0, "socket: %d", s);
  struct sockaddr_un {
    unsigned short sun_family;
    char sun_path[108];
  } sa;
  memset(&sa, 0, sizeof sa);
  sa.sun_family = AF_UNIX;
  strcpy(sa.sun_path, "/data/no/such/socket");
  r = SC3(283, s, &sa, sizeof sa);
  CHECK(r == -ENOENT, "connect to a missing path: %ld (-ENOENT expected)", r);
  SC1(6, s);
  n += 2;

  // /proc/self/auxv through openat (322): AT_PAGESZ (6) = 4096 (Go's runtime).
  int ax = SC4(322, AT_FDCWD, "/proc/self/auxv", G_O_RDONLY, 0);
  CHECK(ax >= 0, "openat(/proc/self/auxv): %d", ax);
  unsigned long aux[64];
  long an = SC3(3, ax, aux, sizeof aux);
  SC1(6, ax);
  long pagesz = -1;
  for (long i = 0; i + 1 < an / (long)sizeof(long); i += 2)
    if (aux[i] == 6) pagesz = (long)aux[i + 1];
  CHECK(pagesz == 4096, "/proc/self/auxv: AT_PAGESZ %ld (%ld bytes)", pagesz, an);
  n += 2;

  // A SIGURG handler with SA_ONSTACK runs on the sigaltstack (186), which
  // reports SS_ONSTACK there.
  static char alt[16384];
  stack_t ss = {alt, 0, sizeof alt}, old;
  CHECK(SC2(186, &ss, &old) == 0, "sigaltstack");
  struct sigaction sa_new, sa_old;
  memset(&sa_new, 0, sizeof sa_new);
  sa_new.sa_sigaction = OnStackHandler;
  sa_new.sa_flags = SA_SIGINFO | SA_ONSTACK;
  CHECK(sigaction(SIGURG, &sa_new, &sa_old) == 0, "sigaction");
  g_onstack_sp = 0;
  CHECK(SC3(268, getpid(), gettid(), SIGURG) == 0, "tgkill");
  for (int i = 0; i < 100 && !g_onstack_sp; i++) usleep(1000);
  sigaction(SIGURG, &sa_old, NULL);
  stack_t off = {NULL, SS_DISABLE, 0};
  SC2(186, &off, NULL);
  CHECK(g_onstack_sp, "the handler did not run");
  CHECK(g_onstack_sp >= (uintptr_t)alt && g_onstack_sp < (uintptr_t)alt + sizeof alt,
        "SA_ONSTACK: the handler ran at %p, not on the alternate stack %p", (void*)g_onstack_sp, alt);
  CHECK(g_onstack_flags == SS_ONSTACK, "sigaltstack in the handler: flags %d", g_onstack_flags);
  n += 3;

  snprintf(g_msg, sizeof g_msg, "ok %d", n);
  return g_msg;
}

JNIEXPORT jstring JNICALL Java_org_arcwasm_ndk6test_Native6_rawnet(JNIEnv* env, jclass c) {
  return Result(env, RawNet());
}

// --- zlib ----------------------------------------------------------------------------------

static const char* Zlib(const char* dir) {
  static const char text[] = "The quick brown fox jumps over the lazy dog. "
                             "The quick brown fox jumps over the lazy dog.";
  CHECK(crc32(0, (const Bytef*)"123456789", 9) == 0xcbf43926, "crc32");
  CHECK(adler32(1, (const Bytef*)"Wikipedia", 9) == 0x11e60398, "adler32");
  Bytef packed[256], unpacked[256];
  uLongf plen = sizeof packed, ulen = sizeof unpacked;
  CHECK(compressBound(sizeof text) >= sizeof text, "compressBound");
  CHECK(compress2(packed, &plen, (const Bytef*)text, sizeof text, 9) == Z_OK && plen < sizeof text,
        "compress2: %lu", (unsigned long)plen);
  CHECK(uncompress(unpacked, &ulen, packed, plen) == Z_OK && ulen == sizeof text &&
            !memcmp(unpacked, text, ulen),
        "uncompress");
  // A gzip stream with deflateInit2 / inflate.
  z_stream zs;
  memset(&zs, 0, sizeof zs);
  CHECK(deflateInit2(&zs, Z_BEST_SPEED, Z_DEFLATED, 16 + 15, 8, Z_DEFAULT_STRATEGY) == Z_OK,
        "deflateInit2");
  zs.next_in = (Bytef*)text;
  zs.avail_in = sizeof text;
  zs.next_out = packed;
  zs.avail_out = sizeof packed;
  CHECK(deflate(&zs, Z_FINISH) == Z_STREAM_END, "deflate");
  plen = zs.total_out;
  deflateEnd(&zs);
  CHECK(packed[0] == 0x1f && packed[1] == 0x8b, "a gzip header");
  memset(&zs, 0, sizeof zs);
  CHECK(inflateInit2(&zs, 16 + 15) == Z_OK, "inflateInit2");
  zs.next_in = packed;
  zs.avail_in = plen;
  zs.next_out = unpacked;
  zs.avail_out = sizeof unpacked;
  CHECK(inflate(&zs, Z_FINISH) == Z_STREAM_END && zs.total_out == sizeof text, "inflate");
  inflateEnd(&zs);
  // gzFile on a file in the app's directory.
  char path[512];
  snprintf(path, sizeof path, "%s/t6.gz", dir);
  gzFile gz = gzopen(path, "wb");
  CHECK(gz != NULL, "gzopen w");
  CHECK(gzwrite(gz, text, sizeof text) == (int)sizeof text, "gzwrite");
  CHECK(gzclose(gz) == Z_OK, "gzclose");
  gz = gzopen(path, "rb");
  CHECK(gz != NULL, "gzopen r");
  memset(unpacked, 0, sizeof unpacked);
  CHECK(gzread(gz, unpacked, sizeof unpacked) == (int)sizeof text && !memcmp(unpacked, text, sizeof text),
        "gzread");
  gzclose(gz);
  unlink(path);
  return "ok";
}

JNIEXPORT jstring JNICALL Java_org_arcwasm_ndk6test_Native6_zlib(JNIEnv* env, jclass c,
                                                                 jstring dir) {
  char buf[512];
  return Result(env, Zlib(PathOf(env, dir, buf, sizeof buf)));
}

// --- AAsset -------------------------------------------------------------------------------------

static const char* Assets(JNIEnv* env, jobject jassets) {
  static const char kHello[] = "Hello from an asset of the NDK 6 test.\n";
  AAssetManager* am = AAssetManager_fromJava(env, jassets);
  CHECK(am != NULL, "AAssetManager_fromJava");
  AAsset* a = AAssetManager_open(am, "hello.txt", AASSET_MODE_STREAMING);
  CHECK(a != NULL, "AAssetManager_open(hello.txt)");
  CHECK(AAsset_getLength(a) == (off_t)strlen(kHello), "AAsset_getLength %ld", (long)AAsset_getLength(a));
  char buf[128] = {0};
  CHECK(AAsset_read(a, buf, 5) == 5 && !memcmp(buf, "Hello", 5), "AAsset_read");
  CHECK(AAsset_getRemainingLength(a) == (off_t)strlen(kHello) - 5, "AAsset_getRemainingLength");
  CHECK(AAsset_seek(a, 6, SEEK_SET) == 6, "AAsset_seek");
  CHECK(AAsset_read(a, buf, sizeof buf) == (int)strlen(kHello) - 6 && !strncmp(buf, "from", 4),
        "AAsset_read after seek: %s", buf);
  AAsset_close(a);
  a = AAssetManager_open(am, "hello.txt", AASSET_MODE_BUFFER);
  const void* data = AAsset_getBuffer(a);
  CHECK(data != NULL && !memcmp(data, kHello, strlen(kHello)), "AAsset_getBuffer");
  AAsset_close(a);
  CHECK(AAssetManager_open(am, "missing.txt", AASSET_MODE_STREAMING) == NULL, "a missing asset");
  AAssetDir* d = AAssetManager_openDir(am, "dir");
  CHECK(d != NULL, "AAssetManager_openDir");
  int found = 0;
  const char* name;
  while ((name = AAssetDir_getNextFileName(d)) != NULL) {
    if (!strcmp(name, "a.txt") || !strcmp(name, "b.txt"))
      found++;
  }
  AAssetDir_close(d);
  CHECK(found == 2, "AAssetDir: %d of a.txt, b.txt", found);
  return "ok";
}

JNIEXPORT jstring JNICALL Java_org_arcwasm_ndk6test_Native6_assets(JNIEnv* env, jclass c,
                                                                   jobject assets) {
  return Result(env, Assets(env, assets));
}

// --- ALooper ----------------------------------------------------------------------------------

struct LooperTest {
  int fds[2];
  int callbacks;
  int last_events;
  ALooper* looper;
  const char* error;
};

static int OnFd(int fd, int events, void* data) {
  struct LooperTest* t = data;
  char c;
  read(fd, &c, 1);
  t->callbacks++;
  t->last_events = events;
  return 1;  // keep watching
}

static void* Writer(void* arg) {
  struct LooperTest* t = arg;
  usleep(20 * 1000);
  write(t->fds[1], "x", 1);
  usleep(20 * 1000);
  ALooper_wake(t->looper);
  return NULL;
}

static const char* LooperThread(struct LooperTest* t) {
  ALooper* l = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
  CHECK(l != NULL && ALooper_forThread() == l, "ALooper_prepare/forThread");
  ALooper_acquire(l);
  t->looper = l;
  CHECK(pipe(t->fds) == 0, "pipe");
  CHECK(ALooper_addFd(l, t->fds[0], 0, ALOOPER_EVENT_INPUT, OnFd, t) == 1, "ALooper_addFd");
  pthread_t w;
  pthread_create(&w, NULL, Writer, t);
  int r = ALooper_pollOnce(1000, NULL, NULL, NULL);
  CHECK(r == ALOOPER_POLL_CALLBACK && t->callbacks == 1 && (t->last_events & ALOOPER_EVENT_INPUT),
        "pollOnce: %d, %d callbacks", r, t->callbacks);
  r = ALooper_pollOnce(1000, NULL, NULL, NULL);
  CHECK(r == ALOOPER_POLL_WAKE, "pollOnce after ALooper_wake: %d", r);
  pthread_join(w, NULL);
  CHECK(ALooper_removeFd(l, t->fds[0]) == 1, "ALooper_removeFd");
  // An identifier instead of a callback: pollAll returns it with the data.
  CHECK(ALooper_addFd(l, t->fds[0], 5, ALOOPER_EVENT_INPUT, NULL, t) == 1, "addFd with ident");
  write(t->fds[1], "y", 1);
  int fd = -1, events = 0;
  void* data = NULL;
  r = ALooper_pollAll(1000, &fd, &events, &data);
  CHECK(r == 5 && fd == t->fds[0] && data == t, "pollAll: ident %d fd %d", r, fd);
  ALooper_removeFd(l, t->fds[0]);
  CHECK(ALooper_pollOnce(0, NULL, NULL, NULL) == ALOOPER_POLL_TIMEOUT, "pollOnce(0) timeout");
  close(t->fds[0]);
  close(t->fds[1]);
  ALooper_release(l);
  return "ok";
}

static void* LooperMain(void* arg) {
  struct LooperTest* t = arg;
  t->error = LooperThread(t);
  return NULL;
}

JNIEXPORT jstring JNICALL Java_org_arcwasm_ndk6test_Native6_looper(JNIEnv* env, jclass c) {
  struct LooperTest t;
  memset(&t, 0, sizeof t);
  pthread_t th;
  pthread_create(&th, NULL, LooperMain, &t);
  pthread_join(th, NULL);
  return Result(env, t.error);
}

// --- AndroidBitmap -------------------------------------------------------------------------

static const char* Bitmaps(JNIEnv* env, jobject b8888, jobject b565) {
  AndroidBitmapInfo info;
  CHECK(AndroidBitmap_getInfo(env, b8888, &info) == ANDROID_BITMAP_RESULT_SUCCESS,
        "AndroidBitmap_getInfo");
  CHECK(info.width == 4 && info.height == 3 && info.stride == 16 &&
            info.format == ANDROID_BITMAP_FORMAT_RGBA_8888,
        "8888 info %ux%u stride %u format %d", info.width, info.height, info.stride, info.format);
  uint32_t* px;
  CHECK(AndroidBitmap_lockPixels(env, b8888, (void**)&px) == 0 && px, "lockPixels");
  // Java set (1,1) to 0xff112233 (ARGB): in memory R G B A.
  CHECK(px[1 * 4 + 1] == 0xff332211u, "pixel (1,1) 0x%08x", px[5]);
  for (int i = 0; i < 12; i++) {
    if (i != 5)
      px[i] = 0xff00ff00u;  // green
  }
  CHECK(AndroidBitmap_unlockPixels(env, b8888) == 0, "unlockPixels");
  CHECK(AndroidBitmap_getInfo(env, b565, &info) == 0 && info.format == ANDROID_BITMAP_FORMAT_RGB_565 &&
            info.stride >= 2 * info.width,
        "565 info format %d stride %u", info.format, info.stride);
  uint16_t* p16;
  CHECK(AndroidBitmap_lockPixels(env, b565, (void**)&p16) == 0, "lockPixels 565");
  p16[0] = 0xf800;  // red
  AndroidBitmap_unlockPixels(env, b565);
  CHECK(AndroidBitmap_getInfo(NULL, b8888, &info) == ANDROID_BITMAP_RESULT_BAD_PARAMETER,
        "a NULL env");
  return "ok";
}

JNIEXPORT jstring JNICALL Java_org_arcwasm_ndk6test_Native6_bitmap(JNIEnv* env, jclass c,
                                                                   jobject b8888, jobject b565) {
  return Result(env, Bitmaps(env, b8888, b565));
}

// --- ANativeWindow: software rendering --------------------------------------------------------

static const char* Window(JNIEnv* env, jobject surface, uint32_t rgba) {
  ANativeWindow* w = ANativeWindow_fromSurface(env, surface);
  CHECK(w != NULL, "ANativeWindow_fromSurface");
  int32_t width = ANativeWindow_getWidth(w), height = ANativeWindow_getHeight(w);
  CHECK(width > 0 && height > 0, "window size %dx%d", width, height);
  // Two frames: one at the window's size and format, one in RGB 565 at
  // half the size (scaled up when posted).
  for (int frame = 0; frame < 2; frame++) {
    if (frame == 1)
      CHECK(ANativeWindow_setBuffersGeometry(w, width / 2, height / 2, WINDOW_FORMAT_RGB_565) == 0,
            "setBuffersGeometry");
    ANativeWindow_Buffer b;
    ARect dirty = {0, 0, width, height};
    CHECK(ANativeWindow_lock(w, &b, &dirty) == 0, "ANativeWindow_lock (frame %d)", frame);
    if (frame == 0) {
      CHECK(b.width == width && b.height == height && b.format == WINDOW_FORMAT_RGBA_8888,
            "buffer %dx%d format %d", b.width, b.height, b.format);
      for (int y = 0; y < b.height; y++) {
        uint32_t* row = (uint32_t*)b.bits + y * b.stride;
        for (int x = 0; x < b.width; x++) row[x] = 0xff0000ffu;  // red (RGBA in memory)
      }
    } else {
      CHECK(b.width == width / 2 && b.format == WINDOW_FORMAT_RGB_565, "565 buffer %dx%d %d",
            b.width, b.height, b.format);
      uint16_t c = (uint16_t)(((rgba & 0xff) >> 3) << 11 | (((rgba >> 8) & 0xff) >> 2) << 5 |
                              ((rgba >> 16) & 0xff) >> 3);
      for (int y = 0; y < b.height; y++) {
        uint16_t* row = (uint16_t*)b.bits + y * b.stride;
        for (int x = 0; x < b.width; x++) row[x] = c;
      }
    }
    CHECK(ANativeWindow_unlockAndPost(w) == 0, "unlockAndPost");
  }
  ANativeWindow_release(w);
  return "ok";
}

JNIEXPORT jstring JNICALL Java_org_arcwasm_ndk6test_Native6_window(JNIEnv* env, jclass c,
                                                                   jobject surface, jint rgba) {
  return Result(env, Window(env, surface, (uint32_t)rgba));
}

// --- EGL + OpenGL ES 3.0 ------------------------------------------------------------------

static GLuint Shader(GLenum type, const char* src, char* log, size_t n) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    glGetShaderInfoLog(s, (GLsizei)n, NULL, log);
    return 0;
  }
  return s;
}

static GLuint Program(const char* vs, const char* fs, char* log, size_t n) {
  GLuint v = Shader(GL_VERTEX_SHADER, vs, log, n), f = Shader(GL_FRAGMENT_SHADER, fs, log, n);
  if (!v || !f)
    return 0;
  GLuint p = glCreateProgram();
  glAttachShader(p, v);
  glAttachShader(p, f);
  glLinkProgram(p);
  GLint ok = 0;
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) {
    glGetProgramInfoLog(p, (GLsizei)n, NULL, log);
    return 0;
  }
  return p;
}

static EGLDisplay g_dpy;
static char g_info[256];

// An ES 3.0 config and context; NULL if this environment has no GL.
static const char* Egl(EGLConfig* cfg, EGLContext* ctx, int version, EGLint surface_type) {
  g_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  EGLint major = 0, minor = 0;
  if (!eglInitialize(g_dpy, &major, &minor))
    return "skip";
  CHECK(major == 1 && minor == 4, "EGL %d.%d", major, minor);
  const EGLint attribs[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                            EGL_DEPTH_SIZE, 16, EGL_RENDERABLE_TYPE, version == 3 ? 0x40 : 4,
                            EGL_SURFACE_TYPE, surface_type, EGL_NONE};
  EGLint n = 0;
  CHECK(eglChooseConfig(g_dpy, attribs, cfg, 1, &n) && n == 1, "eglChooseConfig: %d", n);
  EGLint r = 0, a = 0;
  eglGetConfigAttrib(g_dpy, *cfg, EGL_RED_SIZE, &r);
  eglGetConfigAttrib(g_dpy, *cfg, EGL_ALPHA_SIZE, &a);
  CHECK(r == 8 && a == 8, "config: red %d alpha %d", r, a);
  const EGLint cattr[] = {EGL_CONTEXT_CLIENT_VERSION, version, EGL_NONE};
  *ctx = eglCreateContext(g_dpy, *cfg, EGL_NO_CONTEXT, cattr);
  CHECK(*ctx != EGL_NO_CONTEXT, "eglCreateContext ES %d: 0x%x", version, eglGetError());
  return NULL;
}

static const char* kVs3 =
    "#version 300 es\n"
    "layout(location = 0) in vec2 pos;\n"
    "layout(std140) uniform Block { vec4 color; vec2 offset; };\n"
    "out vec4 vcolor;\n"
    "void main() { gl_Position = vec4(pos + offset, 0.0, 1.0); vcolor = color; }\n";
static const char* kFs3 =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec4 vcolor;\n"
    "out vec4 frag;\n"
    "void main() { frag = vcolor; }\n";

// Draws a full-window quad in |color| with ES 3.0: a vertex array object, a
// uniform block, an ES 3.00 shader; reads the center back.
static const char* DrawEs3(int w, int h, const float color[4], uint32_t* center) {
  char log[512] = "";
  GLuint prog = Program(kVs3, kFs3, log, sizeof log);
  CHECK(prog, "ES 3.00 program: %s", log);
  CHECK(glGetError() == GL_NO_ERROR, "GL error after the program");
  static const float quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
  GLuint vao, vbo, ubo;
  glGenVertexArrays(1, &vao);
  glBindVertexArray(vao);
  glGenBuffers(1, &vbo);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
  glBindVertexArray(0);
  float block[8] = {color[0], color[1], color[2], color[3], 0, 0, 0, 0};
  glGenBuffers(1, &ubo);
  glBindBuffer(GL_UNIFORM_BUFFER, ubo);
  glBufferData(GL_UNIFORM_BUFFER, sizeof block, block, GL_DYNAMIC_DRAW);
  GLuint index = glGetUniformBlockIndex(prog, "Block");
  CHECK(index != GL_INVALID_INDEX, "uniform block");
  glUniformBlockBinding(prog, index, 1);
  glBindBufferBase(GL_UNIFORM_BUFFER, 1, ubo);
  glViewport(0, 0, w, h);
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  glUseProgram(prog);
  glBindVertexArray(vao);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glBindVertexArray(0);
  GLint binding = -1;
  glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &binding);
  CHECK(binding == 0, "GL_VERTEX_ARRAY_BINDING %d after binding 0", binding);
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &binding);
  CHECK(binding == 0, "GL_FRAMEBUFFER_BINDING %d: the surface is framebuffer 0", binding);
  *center = 0;
  glReadPixels(w / 2, h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, center);
  GLenum e = glGetError();
  CHECK(e == GL_NO_ERROR, "GL error 0x%x", e);
  glDeleteBuffers(1, &vbo);
  glDeleteBuffers(1, &ubo);
  glDeleteVertexArrays(1, &vao);
  glDeleteProgram(prog);
  return NULL;
}

static const char* Gles3(JNIEnv* env, jobject surface) {
  EGLConfig cfg;
  EGLContext ctx;
  const char* r = Egl(&cfg, &ctx, 3, EGL_WINDOW_BIT);
  if (r)
    return r;
  ANativeWindow* win = ANativeWindow_fromSurface(env, surface);
  CHECK(win != NULL, "ANativeWindow_fromSurface");
  EGLSurface s = eglCreateWindowSurface(g_dpy, cfg, win, NULL);
  CHECK(s != EGL_NO_SURFACE, "eglCreateWindowSurface: 0x%x", eglGetError());
  CHECK(eglMakeCurrent(g_dpy, s, s, ctx), "eglMakeCurrent: 0x%x", eglGetError());
  CHECK(eglGetCurrentContext() == ctx && eglGetCurrentSurface(EGL_DRAW) == s, "eglGetCurrent*");
  EGLint w = 0, h = 0;
  eglQuerySurface(g_dpy, s, EGL_WIDTH, &w);
  eglQuerySurface(g_dpy, s, EGL_HEIGHT, &h);
  CHECK(w == ANativeWindow_getWidth(win) && h == ANativeWindow_getHeight(win), "surface %dx%d", w, h);
  const char* version = (const char*)glGetString(GL_VERSION);
  CHECK(version && !strncmp(version, "OpenGL ES 3.0", 13), "GL_VERSION %s", version ? version : "-");
  GLint major = 0;
  glGetIntegerv(GL_MAJOR_VERSION, &major);
  CHECK(major == 3, "GL_MAJOR_VERSION %d", major);
  static const float kBlue[4] = {0.0f, 0.25f, 1.0f, 1.0f};
  uint32_t center = 0;
  for (int frame = 0; frame < 3; frame++) {
    r = DrawEs3(w, h, kBlue, &center);
    if (r)
      return r;
    CHECK(eglSwapBuffers(g_dpy, s), "eglSwapBuffers: 0x%x", eglGetError());
  }
  CHECK((center & 0xffffff) == 0xff4000, "the window's center 0x%08x (RGBA in memory)", center);
  snprintf(g_info, sizeof g_info, "ok %s; %s; %dx%d", version,
           (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION), w, h);
  eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroySurface(g_dpy, s);
  eglDestroyContext(g_dpy, ctx);
  ANativeWindow_release(win);
  return g_info;
}

struct GlArgs {
  JavaVM* vm;
  jobject surface;
  const char* result;
};
static void* GlThread(void* arg) {
  struct GlArgs* a = arg;
  JNIEnv* env;
  (*a->vm)->AttachCurrentThread(a->vm, &env, NULL);
  a->result = Gles3(env, a->surface);
  (*a->vm)->DetachCurrentThread(a->vm);
  return NULL;
}

// On a thread of its own, as an app's render thread.
JNIEXPORT jstring JNICALL Java_org_arcwasm_ndk6test_Native6_gles3(JNIEnv* env, jclass c,
                                                                  jobject surface) {
  struct GlArgs a = {NULL, (*env)->NewGlobalRef(env, surface), NULL};
  (*env)->GetJavaVM(env, &a.vm);
  pthread_t t;
  pthread_create(&t, NULL, GlThread, &a);
  pthread_join(t, NULL);
  (*env)->DeleteGlobalRef(env, a.surface);
  return Result(env, a.result);
}

// GLSL ES 1.00 that WebGL2 compiles only as ESSL 3.00 (topic glsl-es3, as
// Flutter's Impeller writes it): standard derivatives, a loop to a uniform
// (Appendix A), a sampler named as an ESSL 3.00 function. Red and blue from
// the loop on a 64x32 pbuffer.
static const char* Essl1As3(void) {
  char log[512] = "";
  GLuint prog = Program(
      "attribute vec2 pos; varying vec2 uv;"
      "void main() { uv = pos * 0.5 + 0.5; gl_Position = vec4(pos, 0.0, 1.0); }",
      "#extension GL_OES_standard_derivatives : enable\n"
      "precision mediump float; uniform sampler2D texture; uniform vec4 colors[4];"
      "uniform int n; varying vec2 uv;"
      "void main() { vec4 c = vec4(0.0); for (int i = 0; i < n; i++) c += colors[i];"
      " float d = fwidth(uv.x) > 0.0 ? 1.0 : 0.0;"
      " gl_FragColor = vec4(c.rgb * d, 1.0) + vec4(0.0, 0.0, 0.0, texture2D(texture, uv).a); }",
      log, sizeof log);
  CHECK(prog, "ESSL 1.00 with derivatives and a loop: %s", log);
  glUseProgram(prog);
  GLint tex = glGetUniformLocation(prog, "texture");
  CHECK(tex >= 0, "glGetUniformLocation(\"texture\") %d", tex);
  glUniform1i(tex, 0);
  GLint count = 0;
  glGetProgramiv(prog, GL_ACTIVE_UNIFORMS, &count);
  int named = 0;
  for (GLint i = 0; i < count; i++) {
    char name[64];
    GLint size;
    GLenum type;
    glGetActiveUniform(prog, (GLuint)i, sizeof name, NULL, &size, &type, name);
    named += !strcmp(name, "texture");
  }
  CHECK(named == 1, "glGetActiveUniform: \"texture\" %d times of %d uniforms", named, count);
  static const float colors[16] = {1, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, 0, 0};
  glUniform4fv(glGetUniformLocation(prog, "colors"), 4, colors);
  glUniform1i(glGetUniformLocation(prog, "n"), 2);
  GLint loc = glGetAttribLocation(prog, "pos");
  static const float verts[] = {-1, -1, 1, -1, -1, 1, 1, 1};
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glVertexAttribPointer(loc, 2, GL_FLOAT, GL_FALSE, 0, verts);
  glEnableVertexAttribArray(loc);
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  uint32_t px = 0;
  glReadPixels(32, 16, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &px);
  CHECK((px & 0xffffff) == 0xff00ff, "ESSL 1.00 as 3.00: pixel 0x%08x, GL error 0x%x", px,
        glGetError());
  glDisableVertexAttribArray(loc);
  glDeleteProgram(prog);
  return NULL;
}

// A pbuffer: ES 3.0 features WebGL2 lacks or does differently (reading a
// mapped buffer, fence syncs, program binaries), then an ES 2.0 context on
// the same thread with client-side vertex and index arrays.
// Textures a context sharing objects with the caller's uploads on another
// thread, as Flutter's IO thread does for the images its raster thread draws
// (topic gl-share): a storage texture and its later partial update, a
// mipmapped one with its parameters; one deleted here is gone there too.
struct Uploader {
  EGLContext ctx;
  pthread_mutex_t lock;
  pthread_cond_t cond;
  int step, done;
  GLuint tex[2];
  char error[128];
  int deleted_gone;
};
static void* UploaderMain(void* arg) {
  struct Uploader* u = arg;
  if (!eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, u->ctx))
    snprintf(u->error, sizeof u->error, "eglMakeCurrent (surfaceless): 0x%x", eglGetError());
  for (int step = 1; step <= 3; step++) {
    pthread_mutex_lock(&u->lock);
    while (u->step < step) pthread_cond_wait(&u->cond, &u->lock);
    pthread_mutex_unlock(&u->lock);
    if (!u->error[0] && step == 1) {
      uint32_t blue[16], green[4];
      for (int i = 0; i < 16; i++) blue[i] = 0xffff0000;
      for (int i = 0; i < 4; i++) green[i] = 0xff00ff00;
      glGenTextures(2, u->tex);
      glBindTexture(GL_TEXTURE_2D, u->tex[0]);
      glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 4, 4);
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, blue);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glBindTexture(GL_TEXTURE_2D, u->tex[1]);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, green);
      glGenerateMipmap(GL_TEXTURE_2D);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glFlush();
    } else if (!u->error[0] && step == 2) {  // after the other thread drew with it
      uint32_t red[4];
      for (int i = 0; i < 4; i++) red[i] = 0xff0000ff;
      glBindTexture(GL_TEXTURE_2D, u->tex[0]);
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, red);
      glFlush();
    } else if (!u->error[0]) {  // after the other thread deleted it
      GLuint t;
      glGenTextures(1, &t);
      glDeleteTextures(1, &t);
      u->deleted_gone = !glIsTexture(u->tex[0]);
    }
    GLenum e = glGetError();
    if (e != GL_NO_ERROR && !u->error[0])
      snprintf(u->error, sizeof u->error, "GL error 0x%x at step %d", e, step);
    pthread_mutex_lock(&u->lock);
    u->done = step;
    pthread_cond_broadcast(&u->cond);
    pthread_mutex_unlock(&u->lock);
  }
  eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  return NULL;
}
static void UploaderStep(struct Uploader* u, int step) {
  pthread_mutex_lock(&u->lock);
  u->step = step;
  pthread_cond_broadcast(&u->cond);
  while (u->done < step) pthread_cond_wait(&u->cond, &u->lock);
  pthread_mutex_unlock(&u->lock);
}
static uint32_t DrawTexture(GLuint prog, GLuint vao, GLuint tex, int x, int y) {
  glBindTexture(GL_TEXTURE_2D, tex);
  glClear(GL_COLOR_BUFFER_BIT);
  glUseProgram(prog);
  glBindVertexArray(vao);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glBindVertexArray(0);
  uint32_t px = 0;
  glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &px);
  return px;
}
static const char* SharedTextures(EGLConfig cfg, EGLContext ctx) {
  char log[512] = "";
  GLuint prog = Program("#version 300 es\n"
                        "layout(location = 0) in vec2 pos; out vec2 uv;\n"
                        "void main() { uv = pos * 0.5 + 0.5; gl_Position = vec4(pos, 0.0, 1.0); }\n",
                        "#version 300 es\n"
                        "precision mediump float; uniform sampler2D tex; in vec2 uv; out vec4 o;\n"
                        "void main() { o = texture(tex, uv); }\n",
                        log, sizeof log);
  CHECK(prog, "texture program: %s", log);
  static const float quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
  GLuint vao, vbo;
  glGenVertexArrays(1, &vao);
  glBindVertexArray(vao);
  glGenBuffers(1, &vbo);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
  glBindVertexArray(0);
  glViewport(0, 0, 64, 32);
  glClearColor(0, 0, 0, 1);
  glActiveTexture(GL_TEXTURE0);

  static struct Uploader u;
  memset(&u, 0, sizeof u);
  const EGLint cattr[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  u.ctx = eglCreateContext(g_dpy, cfg, ctx, cattr);
  CHECK(u.ctx != EGL_NO_CONTEXT, "a context sharing with it: 0x%x", eglGetError());
  pthread_mutex_init(&u.lock, NULL);
  pthread_cond_init(&u.cond, NULL);
  pthread_t t;
  pthread_create(&t, NULL, UploaderMain, &u);
  UploaderStep(&u, 1);
  uint32_t first = DrawTexture(prog, vao, u.tex[0], 32, 16);
  uint32_t mip = DrawTexture(prog, vao, u.tex[1], 32, 16);
  GLenum e1 = glGetError();
  UploaderStep(&u, 2);
  uint32_t updated = DrawTexture(prog, vao, u.tex[0], 8, 8);
  uint32_t rest = DrawTexture(prog, vao, u.tex[0], 56, 28);
  GLuint gone = u.tex[0];
  glDeleteTextures(1, &gone);
  UploaderStep(&u, 3);
  pthread_join(t, NULL);
  glDeleteTextures(1, &u.tex[1]);
  glDeleteBuffers(1, &vbo);
  glDeleteVertexArrays(1, &vao);
  glDeleteProgram(prog);
  eglDestroyContext(g_dpy, u.ctx);
  CHECK(!u.error[0], "the other thread: %s", u.error);
  CHECK((first & 0xffffff) == 0xff0000 && e1 == GL_NO_ERROR,
        "texture from the other thread: pixel 0x%08x, GL error 0x%x", first, e1);
  CHECK((mip & 0xffffff) == 0x00ff00, "mipmapped texture from the other thread: 0x%08x", mip);
  CHECK((updated & 0xffffff) == 0x0000ff && (rest & 0xffffff) == 0xff0000,
        "its partial update there: 0x%08x, the rest 0x%08x", updated, rest);
  CHECK(u.deleted_gone, "deleted here, still a texture there");
  return NULL;
}

static const char* Pbuffer(void) {
  EGLConfig cfg;
  EGLContext ctx;
  const char* r = Egl(&cfg, &ctx, 3, EGL_PBUFFER_BIT);
  if (r)
    return r;
  const EGLint pattr[] = {EGL_WIDTH, 64, EGL_HEIGHT, 32, EGL_NONE};
  EGLSurface pb = eglCreatePbufferSurface(g_dpy, cfg, pattr);
  CHECK(pb != EGL_NO_SURFACE, "eglCreatePbufferSurface: 0x%x", eglGetError());
  CHECK(eglMakeCurrent(g_dpy, pb, pb, ctx), "eglMakeCurrent pbuffer");
  static const float kRed[4] = {1, 0, 0, 1};
  uint32_t center;
  r = DrawEs3(64, 32, kRed, &center);
  if (r)
    return r;
  CHECK((center & 0xffffff) == 0x0000ff, "pbuffer center 0x%08x", center);
  // glMapBufferRange for reading: a PBO the pixels were packed into.
  GLuint pbo;
  glGenBuffers(1, &pbo);
  glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
  glBufferData(GL_PIXEL_PACK_BUFFER, 64 * 32 * 4, NULL, GL_STREAM_READ);
  glReadPixels(0, 0, 64, 32, GL_RGBA, GL_UNSIGNED_BYTE, 0);
  GLsync sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  CHECK(glClientWaitSync(sync, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull) != GL_WAIT_FAILED,
        "glClientWaitSync");
  glDeleteSync(sync);
  const uint32_t* mapped = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, 64 * 32 * 4, GL_MAP_READ_BIT);
  CHECK(mapped != NULL, "glMapBufferRange(READ): 0x%x", glGetError());
  CHECK((mapped[32 * 16 + 5] & 0xffffff) == 0x0000ff, "mapped pixel 0x%08x", mapped[32 * 16 + 5]);
  CHECK(glUnmapBuffer(GL_PIXEL_PACK_BUFFER), "glUnmapBuffer");
  glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  glDeleteBuffers(1, &pbo);
  GLint formats = -1;
  glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &formats);
  CHECK(formats == 0, "program binary formats %d", formats);
  const char* ext = (const char*)glGetStringi(GL_EXTENSIONS, 0);
  CHECK(ext && !strncmp(ext, "GL_", 3), "glGetStringi");

  // ES 2.0 on the same thread: client-side arrays, a GLSL ES 1.00 shader.
  GLint program3 = -1;
  glGetIntegerv(GL_CURRENT_PROGRAM, &program3);
  const EGLint cattr2[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  EGLContext ctx2 = eglCreateContext(g_dpy, cfg, EGL_NO_CONTEXT, cattr2);
  CHECK(ctx2 != EGL_NO_CONTEXT, "ES 2.0 context");
  EGLSurface pb2 = eglCreatePbufferSurface(g_dpy, cfg, pattr);
  CHECK(eglMakeCurrent(g_dpy, pb2, pb2, ctx2), "eglMakeCurrent ES 2.0");
  char log[512] = "";
  GLuint prog = Program("attribute vec2 pos; void main() { gl_Position = vec4(pos, 0.0, 1.0); }",
                        "precision mediump float; uniform vec4 color;"
                        "void main() { gl_FragColor = color; }",
                        log, sizeof log);
  CHECK(prog, "ES 1.00 program: %s", log);
  glUseProgram(prog);
  glUniform4f(glGetUniformLocation(prog, "color"), 0, 1, 0, 1);
  GLint loc = glGetAttribLocation(prog, "pos");
  static const float verts[] = {-1, -1, 1, -1, -1, 1, 1, 1};
  static const uint16_t indices[] = {0, 1, 2, 2, 1, 3};
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glVertexAttribPointer(loc, 2, GL_FLOAT, GL_FALSE, 0, verts);  // client memory
  glEnableVertexAttribArray(loc);
  glViewport(0, 0, 64, 32);
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, indices);  // client indices
  uint32_t px = 0;
  glReadPixels(10, 10, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &px);
  CHECK((px & 0xffffff) == 0x00ff00, "client arrays: pixel 0x%08x, GL error 0x%x", px, glGetError());
  const char* v2 = (const char*)glGetString(GL_VERSION);
  CHECK(v2 && !strncmp(v2, "OpenGL ES 2.0", 13), "ES 2.0 GL_VERSION %s", v2);
  // GL_EXT_texture_rg's unsized GL_RED (Skia's glyph atlas on ES 2.0: text
  // of Flutter apps) is a valid internal format.
  GLuint atlas;
  glGenTextures(1, &atlas);
  glBindTexture(GL_TEXTURE_2D, atlas);
  while (glGetError() != GL_NO_ERROR) {
  }
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, 64, 64, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);
  static const uint8_t glyph[16] = {255, 128};
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 4, 4, GL_RED, GL_UNSIGNED_BYTE, glyph);
  GLenum rg = glGetError();
  CHECK(rg == GL_NO_ERROR, "GL_RED texture (EXT_texture_rg): GL error 0x%x", rg);
  glDeleteTextures(1, &atlas);
  r = Essl1As3();
  if (r)
    return r;
  // Back to the ES 3.0 context: its state is its own.
  CHECK(eglMakeCurrent(g_dpy, pb, pb, ctx), "eglMakeCurrent back");
  GLint program = -1;
  glGetIntegerv(GL_CURRENT_PROGRAM, &program);
  CHECK(program == program3 && program != (GLint)prog, "the ES 3.0 context sees program %d, not %d",
        program, program3);
  r = SharedTextures(cfg, ctx);
  if (r)
    return r;
  eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroySurface(g_dpy, pb);
  eglDestroySurface(g_dpy, pb2);
  eglDestroyContext(g_dpy, ctx);
  eglDestroyContext(g_dpy, ctx2);
  return "ok";
}

// A texture of a share group taken by threads that end (topic
// gl-share-webgl): Java threads have a host thread each, whose WebGL context
// goes when it ends, and the next thread's new context may get the same
// Emscripten handle (an address from malloc). The texture copied to the dead
// thread's WebGL is not in the new one: ndk_egl.cc tells WebGL contexts apart
// by serial number, not handle. Steps (Java, Ndk6Test): 0 on the calling
// thread (it stays): contexts U and B of one group, a texture U uploads; then
// each round 1 on a new thread (B draws with it, the thread ends), 2 on the
// caller (U uploads it again, another color), 1 on a new thread again; 3: the
// caller cleans up. The color step 1 is to see is |color| (ABGR).
static struct {
  EGLConfig cfg;
  EGLContext u, b;
  GLuint tex;
} g_reuse;
static void ReuseUpload(uint32_t abgr) {
  uint32_t px[16];
  for (int i = 0; i < 16; i++) px[i] = abgr;
  glBindTexture(GL_TEXTURE_2D, g_reuse.tex);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, px);
  glFlush();
}
static const char* Reuse(int step, uint32_t color) {
  if (step == 0) {
    memset(&g_reuse, 0, sizeof g_reuse);
    const char* r = Egl(&g_reuse.cfg, &g_reuse.u, 3, EGL_PBUFFER_BIT);
    if (r)
      return r;
    const EGLint cattr[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    g_reuse.b = eglCreateContext(g_dpy, g_reuse.cfg, g_reuse.u, cattr);
    CHECK(g_reuse.b != EGL_NO_CONTEXT, "a context sharing with it: 0x%x", eglGetError());
    CHECK(eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, g_reuse.u),
          "eglMakeCurrent (surfaceless): 0x%x", eglGetError());
    glGenTextures(1, &g_reuse.tex);
    glBindTexture(GL_TEXTURE_2D, g_reuse.tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 4, 4);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    ReuseUpload(color);
  } else if (step == 2) {
    ReuseUpload(color);
  } else if (step == 3) {
    glDeleteTextures(1, &g_reuse.tex);
    eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(g_dpy, g_reuse.b);
    eglDestroyContext(g_dpy, g_reuse.u);
  } else {
    const EGLint pattr[] = {EGL_WIDTH, 64, EGL_HEIGHT, 32, EGL_NONE};
    EGLSurface pb = eglCreatePbufferSurface(g_dpy, g_reuse.cfg, pattr);
    CHECK(pb != EGL_NO_SURFACE, "eglCreatePbufferSurface: 0x%x", eglGetError());
    CHECK(eglMakeCurrent(g_dpy, pb, pb, g_reuse.b), "eglMakeCurrent: 0x%x", eglGetError());
    char log[512] = "";
    GLuint prog = Program("#version 300 es\n"
                          "layout(location = 0) in vec2 pos; out vec2 uv;\n"
                          "void main() { uv = pos * 0.5 + 0.5; gl_Position = vec4(pos, 0.0, 1.0); }\n",
                          "#version 300 es\n"
                          "precision mediump float; uniform sampler2D tex; in vec2 uv; out vec4 o;\n"
                          "void main() { o = texture(tex, uv); }\n",
                          log, sizeof log);
    CHECK(prog, "texture program: %s", log);
    static const float quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    GLuint vao, vbo;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glBindVertexArray(0);
    glViewport(0, 0, 64, 32);
    glClearColor(0, 0, 0, 1);
    glActiveTexture(GL_TEXTURE0);
    while (glGetError() != GL_NO_ERROR) {
    }
    uint32_t px = DrawTexture(prog, vao, g_reuse.tex, 32, 16);
    GLenum e = glGetError();
    glBindTexture(GL_TEXTURE_2D, 0);
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(prog);
    eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(g_dpy, pb);
    CHECK(px == color && e == GL_NO_ERROR, "pixel 0x%08x, not 0x%08x; GL error 0x%x", px, color,
          e);
  }
  return "ok";
}

JNIEXPORT jstring JNICALL Java_org_arcwasm_ndk6test_Native6_texreuse(JNIEnv* env, jclass c,
                                                                    jint step, jint color) {
  return Result(env, Reuse(step, (uint32_t)color));
}

static void* PbufferThread(void* arg) {
  *(const char**)arg = Pbuffer();
  return NULL;
}

JNIEXPORT jstring JNICALL Java_org_arcwasm_ndk6test_Native6_pbuffer(JNIEnv* env, jclass c) {
  const char* r = NULL;
  pthread_t t;
  pthread_create(&t, NULL, PbufferThread, &r);
  pthread_join(t, NULL);
  return Result(env, r);
}

// --- OpenSL ES: a 440 Hz tone through a buffer queue ---------------------------------------

#define TONE_RATE 44100
#define TONE_FRAMES 1024
struct Tone {
  int16_t buffers[2][TONE_FRAMES * 2];
  int next, callbacks;
  double phase;
  pthread_mutex_t lock;
};
static void FillTone(struct Tone* t, int16_t* b) {
  for (int i = 0; i < TONE_FRAMES; i++) {
    int16_t v = (int16_t)(sin(t->phase) * 8000);
    t->phase += 2 * M_PI * 440 / TONE_RATE;
    b[2 * i] = b[2 * i + 1] = v;
  }
}
static void OnBuffer(SLAndroidSimpleBufferQueueItf q, void* ctx) {
  struct Tone* t = ctx;
  pthread_mutex_lock(&t->lock);
  t->callbacks++;
  int16_t* b = t->buffers[t->next];
  t->next ^= 1;
  pthread_mutex_unlock(&t->lock);
  FillTone(t, b);
  (*q)->Enqueue(q, b, sizeof t->buffers[0]);
}

static const char* OpenSles(int ms) {
  static struct Tone tone;
  memset(&tone, 0, sizeof tone);
  pthread_mutex_init(&tone.lock, NULL);
  SLObjectItf engine_obj, mix, player;
  SLEngineItf engine;
  CHECK(slCreateEngine(&engine_obj, 0, NULL, 0, NULL, NULL) == SL_RESULT_SUCCESS, "slCreateEngine");
  CHECK((*engine_obj)->Realize(engine_obj, SL_BOOLEAN_FALSE) == SL_RESULT_SUCCESS, "Realize engine");
  CHECK((*engine_obj)->GetInterface(engine_obj, SL_IID_ENGINE, &engine) == SL_RESULT_SUCCESS,
        "SL_IID_ENGINE");
  const SLInterfaceID mix_ids[] = {SL_IID_ENVIRONMENTALREVERB};
  const SLboolean mix_req[] = {SL_BOOLEAN_FALSE};
  CHECK((*engine)->CreateOutputMix(engine, &mix, 1, mix_ids, mix_req) == SL_RESULT_SUCCESS,
        "CreateOutputMix");
  CHECK((*mix)->Realize(mix, SL_BOOLEAN_FALSE) == SL_RESULT_SUCCESS, "Realize mix");
  SLEnvironmentalReverbItf reverb;
  if ((*mix)->GetInterface(mix, SL_IID_ENVIRONMENTALREVERB, &reverb) == SL_RESULT_SUCCESS) {
    SLEnvironmentalReverbSettings s = SL_I3DL2_ENVIRONMENT_PRESET_STONECORRIDOR;
    CHECK((*reverb)->SetEnvironmentalReverbProperties(reverb, &s) == SL_RESULT_SUCCESS, "reverb");
  }
  SLDataLocator_AndroidSimpleBufferQueue loc = {SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 2};
  SLDataFormat_PCM pcm = {SL_DATAFORMAT_PCM, 2, SL_SAMPLINGRATE_44_1, SL_PCMSAMPLEFORMAT_FIXED_16,
                          SL_PCMSAMPLEFORMAT_FIXED_16,
                          SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT, SL_BYTEORDER_LITTLEENDIAN};
  SLDataSource src = {&loc, &pcm};
  SLDataLocator_OutputMix out = {SL_DATALOCATOR_OUTPUTMIX, mix};
  SLDataSink sink = {&out, NULL};
  const SLInterfaceID ids[] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_VOLUME};
  const SLboolean req[] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE};
  CHECK((*engine)->CreateAudioPlayer(engine, &player, &src, &sink, 2, ids, req) == SL_RESULT_SUCCESS,
        "CreateAudioPlayer");
  CHECK((*player)->Realize(player, SL_BOOLEAN_FALSE) == SL_RESULT_SUCCESS, "Realize player");
  SLPlayItf play;
  SLAndroidSimpleBufferQueueItf queue;
  SLVolumeItf volume;
  CHECK((*player)->GetInterface(player, SL_IID_PLAY, &play) == SL_RESULT_SUCCESS, "SL_IID_PLAY");
  CHECK((*player)->GetInterface(player, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &queue) == SL_RESULT_SUCCESS,
        "SL_IID_ANDROIDSIMPLEBUFFERQUEUE");
  CHECK((*player)->GetInterface(player, SL_IID_VOLUME, &volume) == SL_RESULT_SUCCESS, "SL_IID_VOLUME");
  CHECK((*volume)->SetVolumeLevel(volume, -600) == SL_RESULT_SUCCESS, "SetVolumeLevel");
  CHECK((*queue)->RegisterCallback(queue, OnBuffer, &tone) == SL_RESULT_SUCCESS, "RegisterCallback");
  for (int i = 0; i < 2; i++) {
    FillTone(&tone, tone.buffers[i]);
    CHECK((*queue)->Enqueue(queue, tone.buffers[i], sizeof tone.buffers[i]) == SL_RESULT_SUCCESS,
          "Enqueue");
  }
  CHECK((*queue)->Enqueue(queue, tone.buffers[0], 4) == SL_RESULT_BUFFER_INSUFFICIENT,
        "a third buffer in a two-buffer queue");
  CHECK((*play)->SetPlayState(play, SL_PLAYSTATE_PLAYING) == SL_RESULT_SUCCESS, "SetPlayState");
  int64_t t0 = NowMs();
  usleep(ms * 1000);
  SLmillisecond pos = 0;
  (*play)->GetPosition(play, &pos);
  int64_t dt = NowMs() - t0;
  (*play)->SetPlayState(play, SL_PLAYSTATE_STOPPED);
  pthread_mutex_lock(&tone.lock);
  int callbacks = tone.callbacks;
  pthread_mutex_unlock(&tone.lock);
  SLAndroidSimpleBufferQueueState state;
  (*queue)->GetState(queue, &state);
  (*player)->Destroy(player);
  (*mix)->Destroy(mix);
  (*engine_obj)->Destroy(engine_obj);
  // Playback runs in real time: about one callback per 1024 frames (23 ms).
  int expect = (int)(dt * TONE_RATE / 1000 / TONE_FRAMES);
  CHECK(callbacks >= expect / 2 && callbacks <= expect + 8, "%d callbacks in %lld ms (about %d)",
        callbacks, (long long)dt, expect);
  CHECK(pos > dt / 3 && pos <= dt + 100, "position %u ms after %lld ms", (unsigned)pos, (long long)dt);
  CHECK(state.index >= (SLuint32)callbacks, "queue index %u", (unsigned)state.index);
  CHECK(slCreateEngine(&engine_obj, 0, NULL, 0, NULL, NULL) == SL_RESULT_SUCCESS, "a second engine");
  (*engine_obj)->Realize(engine_obj, SL_BOOLEAN_FALSE);
  (*engine_obj)->GetInterface(engine_obj, SL_IID_ENGINE, &engine);
  SLObjectItf rec;
  CHECK((*engine)->CreateAudioRecorder(engine, &rec, &src, &sink, 0, NULL, NULL) != SL_RESULT_SUCCESS,
        "a recorder without a microphone");
  (*engine_obj)->Destroy(engine_obj);
  snprintf(g_info, sizeof g_info, "ok %d buffers in %lld ms, position %u ms", callbacks, (long long)dt,
           (unsigned)pos);
  return g_info;
}

JNIEXPORT jstring JNICALL Java_org_arcwasm_ndk6test_Native6_opensles(JNIEnv* env, jclass c, jint ms) {
  return Result(env, OpenSles(ms));
}

// --- threads (topic threads-per-worker): many threads that mostly wait ------------------
// The bridge runs guest threads as green threads on a few carrier threads
// (shim/ndk/ndk_green.cc): their waits park and resume. Checked here: a ring
// of condition-variable turns, a futex chain, semaphores, sleeps and
// sched_yield, rwlocks, thread-specific data destructors, pthread_exit and
// join values, detached threads; a stop-the-world as Boehm GC's (each
// waiting thread gets SIGUSR1, acknowledges and sigsuspends until SIGUSR2);
// threads that spin without waiting while the others need a carrier.

#define TN 40
long syscall(long number, ...);   // (bionic's; <unistd.h> of android-18 lacks it)
static pthread_mutex_t g_tm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_tc = PTHREAD_COND_INITIALIZER;
static pthread_rwlock_t g_trw = PTHREAD_RWLOCK_INITIALIZER;
static int g_turn, g_count, g_rw_value, g_detached_done;
static volatile int g_dtor_runs;
static int g_futex_word[TN];
static sem_t g_tsem;
static pthread_key_t g_tkey;
static void TkeyDtor(void* p) { __sync_fetch_and_add(&g_dtor_runs, (int)(intptr_t)p); }

struct Tw {
  int i;
  int tid;
  pthread_t self;
  size_t stack;
  char why[96];
};

static void* TWorker(void* p) {
  struct Tw* w = (struct Tw*)p;
  w->tid = gettid();
  w->self = pthread_self();
  pthread_attr_t a;
  if (pthread_getattr_np(pthread_self(), &a) == 0) {
    pthread_attr_getstacksize(&a, &w->stack);
    pthread_attr_destroy(&a);
  }
  pthread_setspecific(g_tkey, (void*)1);
  // Turns: thread i goes at g_turn == round * TN + i.
  for (int round = 0; round < 3; round++) {
    pthread_mutex_lock(&g_tm);
    while (g_turn != round * TN + w->i) pthread_cond_wait(&g_tc, &g_tm);
    g_turn++;
    g_count++;
    pthread_cond_broadcast(&g_tc);
    pthread_mutex_unlock(&g_tm);
  }
  // A futex chain: each waits for its word, then sets the next one's.
  while (__atomic_load_n(&g_futex_word[w->i], __ATOMIC_ACQUIRE) == 0)
    syscall(240 /* futex */, &g_futex_word[w->i], 128 /* FUTEX_WAIT_PRIVATE */, 0, NULL);
  if (w->i + 1 < TN) {
    __atomic_store_n(&g_futex_word[w->i + 1], 1, __ATOMIC_RELEASE);
    syscall(240, &g_futex_word[w->i + 1], 129 /* FUTEX_WAKE_PRIVATE */, 1);
  }
  // A reader and a writer of the rwlock in turn.
  for (int k = 0; k < 5; k++) {
    if ((w->i + k) & 1) {
      pthread_rwlock_wrlock(&g_trw);
      int v = g_rw_value;
      usleep(100);
      g_rw_value = v + 1;
      pthread_rwlock_unlock(&g_trw);
    } else {
      pthread_rwlock_rdlock(&g_trw);
      int v = g_rw_value;
      sched_yield();
      if (v != g_rw_value) snprintf(w->why, sizeof w->why, "rwlock: a writer while reading");
      pthread_rwlock_unlock(&g_trw);
    }
  }
  usleep(1000 * (w->i % 5));
  sched_yield();
  // A timed wait that times out.
  struct timespec dl;
  clock_gettime(CLOCK_REALTIME, &dl);
  dl.tv_nsec += 20 * 1000000;
  if (dl.tv_nsec >= 1000000000) {
    dl.tv_sec++;
    dl.tv_nsec -= 1000000000;
  }
  pthread_cond_t never = PTHREAD_COND_INITIALIZER;
  pthread_mutex_t nm = PTHREAD_MUTEX_INITIALIZER;
  pthread_mutex_lock(&nm);
  int64_t t0 = NowMs();
  int r = pthread_cond_timedwait(&never, &nm, &dl);
  int64_t dt = NowMs() - t0;
  pthread_mutex_unlock(&nm);
  if (r != ETIMEDOUT || dt < 10 || dt > 3000)
    snprintf(w->why, sizeof w->why, "timedwait: %d after %lld ms", r, (long long)dt);
  sem_post(&g_tsem);
  if (w->i == TN - 1) pthread_exit((void*)(intptr_t)(1000 + w->i));
  return (void*)(intptr_t)(1000 + w->i);
}

static void* TDetached(void* p) {
  usleep(2000);
  __sync_fetch_and_add(&g_detached_done, 1);
  sem_post(&g_tsem);
  return NULL;
}

// Stop the world: waiting threads acknowledge SIGUSR1 and sleep in
// sigsuspend until SIGUSR2.
#define SN 16
static sem_t g_ack;
static int g_go, g_ready, g_woke;
static volatile int g_spin_stop;
static void OnSuspend(int sig) {
  sem_post(&g_ack);
  sigset_t m;
  sigfillset(&m);
  sigdelset(&m, SIGUSR2);
  sigsuspend(&m);
}
static void OnResume(int sig) {}
static void* TStopped(void* p) {
  pthread_mutex_lock(&g_tm);
  g_ready++;
  pthread_cond_broadcast(&g_tc);
  while (!g_go) pthread_cond_wait(&g_tc, &g_tm);
  g_woke++;
  pthread_mutex_unlock(&g_tm);
  return p;
}
static void* TSpin(void* p) {
  volatile unsigned n = 0;
  while (!__atomic_load_n(&g_spin_stop, __ATOMIC_ACQUIRE)) n++;
  return NULL;
}

static int SemWaitMs(sem_t* s, int ms) {
  struct timespec dl;
  clock_gettime(CLOCK_REALTIME, &dl);
  dl.tv_sec += ms / 1000;
  dl.tv_nsec += (ms % 1000) * 1000000;
  if (dl.tv_nsec >= 1000000000) {
    dl.tv_sec++;
    dl.tv_nsec -= 1000000000;
  }
  return sem_timedwait(s, &dl);
}

static const char* Threads(void) {
  static struct Tw w[TN];
  pthread_t t[TN];
  int64_t t0 = NowMs();
  sem_init(&g_tsem, 0, 0);
  pthread_key_create(&g_tkey, TkeyDtor);
  g_turn = g_count = g_rw_value = g_detached_done = 0;
  g_dtor_runs = 0;
  memset(g_futex_word, 0, sizeof g_futex_word);
  // Threads that spin until the rest is done: green threads that never wait
  // must not keep the others from a carrier.
  pthread_t spin[6];
  const char* ns = getenv("ARC_TEST_SPINNERS");   // (debugging)
  const int nspin = ns ? atoi(ns) : 6;
  g_spin_stop = 0;
  for (int i = 0; i < nspin; i++) CHECK(pthread_create(&spin[i], NULL, TSpin, NULL) == 0, "spinner");
  for (int i = 0; i < TN; i++) {
    memset(&w[i], 0, sizeof w[i]);
    w[i].i = i;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 256 * 1024);
    CHECK(pthread_create(&t[i], &a, TWorker, &w[i]) == 0, "pthread_create %d", i);
    pthread_attr_destroy(&a);
  }
  for (int i = 0; i < 4; i++) {
    pthread_t d;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    CHECK(pthread_create(&d, &a, TDetached, NULL) == 0, "detached");
    pthread_attr_destroy(&a);
  }
  __atomic_store_n(&g_futex_word[0], 1, __ATOMIC_RELEASE);
  syscall(240, &g_futex_word[0], 129, 1);
  for (int i = 0; i < TN + 4; i++)
    CHECK(SemWaitMs(&g_tsem, 30000) == 0, "only %d of %d threads got through (turn %d, count %d)", i,
          TN + 4, g_turn, g_count);
  for (int i = 0; i < TN; i++) {
    void* ret = NULL;
    CHECK(pthread_join(t[i], &ret) == 0, "pthread_join %d", i);
    CHECK((intptr_t)ret == 1000 + i, "thread %d returned %d", i, (int)(intptr_t)ret);
    CHECK(!w[i].why[0], "thread %d: %s", i, w[i].why);
    CHECK(pthread_equal(w[i].self, t[i]), "thread %d: pthread_self", i);
    CHECK(w[i].stack >= 200 * 1024, "thread %d: a stack of %u", i, (unsigned)w[i].stack);
    for (int j = 0; j < i; j++) CHECK(w[i].tid != w[j].tid, "threads %d and %d: tid %d", i, j, w[i].tid);
  }
  __atomic_store_n(&g_spin_stop, 1, __ATOMIC_RELEASE);
  for (int i = 0; i < nspin; i++) pthread_join(spin[i], NULL);
  CHECK(g_count == 3 * TN, "turns: %d", g_count);
  CHECK(g_rw_value == 5 * TN / 2, "rwlock writes: %d", g_rw_value);
  CHECK(g_dtor_runs == TN, "key destructors ran %d times", g_dtor_runs);
  CHECK(g_detached_done == 4, "detached: %d", g_detached_done);
  int64_t dt1 = NowMs() - t0;

  // Stop the world.
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = OnSuspend;
  sa.sa_flags = SA_RESTART;
  sigfillset(&sa.sa_mask);
  sigaction(SIGUSR1, &sa, NULL);
  sa.sa_handler = OnResume;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGUSR2, &sa, NULL);
  sem_init(&g_ack, 0, 0);
  g_go = g_ready = g_woke = 0;
  pthread_t st[SN];
  for (int i = 0; i < SN; i++) CHECK(pthread_create(&st[i], NULL, TStopped, (void*)(intptr_t)i) == 0, "stopped");
  pthread_mutex_lock(&g_tm);
  while (g_ready < SN) pthread_cond_wait(&g_tc, &g_tm);
  pthread_mutex_unlock(&g_tm);
  for (int round = 0; round < 3; round++) {
    for (int i = 0; i < SN; i++) CHECK(pthread_kill(st[i], SIGUSR1) == 0, "pthread_kill USR1 %d", i);
    for (int i = 0; i < SN; i++)
      CHECK(SemWaitMs(&g_ack, 10000) == 0, "round %d: %d of %d threads stopped", round, i, SN);
    for (int i = 0; i < SN; i++) CHECK(pthread_kill(st[i], SIGUSR2) == 0, "pthread_kill USR2 %d", i);
  }
  pthread_mutex_lock(&g_tm);
  g_go = 1;
  pthread_cond_broadcast(&g_tc);
  pthread_mutex_unlock(&g_tm);
  for (int i = 0; i < SN; i++) {
    void* ret = NULL;
    CHECK(pthread_join(st[i], &ret) == 0 && (intptr_t)ret == i, "stopped thread %d", i);
  }
  CHECK(g_woke == SN, "%d threads woke", g_woke);
  signal(SIGUSR1, SIG_DFL);
  signal(SIGUSR2, SIG_DFL);
  pthread_key_delete(g_tkey);
  snprintf(g_info, sizeof g_info, "ok %d threads in %lld ms; stop-the-world x3 of %d in %lld ms", TN + 10,
           (long long)dt1, SN, (long long)(NowMs() - t0 - dt1));
  return g_info;
}

JNIEXPORT jstring JNICALL Java_org_arcwasm_ndk6test_Native6_threads(JNIEnv* env, jclass c) {
  return Result(env, Threads());
}
