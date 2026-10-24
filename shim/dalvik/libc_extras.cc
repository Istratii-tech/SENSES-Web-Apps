// libc functions Dalvik and libcore expect from bionic/glibc that musl in
// Emscripten lacks. Part of the arc-wasm shim layer.

#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <sys/sendfile.h>
#include <unistd.h>

// sendfile(2) as a read/write loop (the kernel layer routes both).
extern "C" ssize_t sendfile(int out_fd, int in_fd, off_t* offset,
                            size_t count) {
  char buf[16 * 1024];
  size_t done = 0;
  off_t pos = offset ? *offset : 0;
  while (done < count) {
    size_t want = count - done < sizeof(buf) ? count - done : sizeof(buf);
    ssize_t n = offset ? pread(in_fd, buf, want, pos) : read(in_fd, buf, want);
    if (n < 0)
      return done ? static_cast<ssize_t>(done) : -1;
    if (n == 0)
      break;
    ssize_t w = 0;
    while (w < n) {
      ssize_t m = write(out_fd, buf + w, n - w);
      if (m < 0)
        return done + w ? static_cast<ssize_t>(done + w) : -1;
      w += m;
    }
    done += n;
    pos += n;
  }
  if (offset)
    *offset = pos;
  return done;
}

// Zygote-only mount namespace setup in Dalvik's Init.cpp; never reached.
extern "C" int mount(const char*, const char*, const char*, unsigned long,
                     const void*) {
  errno = ENOSYS;
  return -1;
}

extern "C" int unshare(int) {
  errno = ENOSYS;
  return -1;
}

// The native heap is Emscripten's own malloc; Dalvik only trims it and
// walks it for DDMS heap statistics.
extern "C" int dlmalloc_trim(size_t) { return 0; }

extern "C" void dlmalloc_inspect_all(
    void (*)(void*, void*, size_t, void*), void*) {}

// Password database, synthesized from the uid the way bionic does it (there
// is no /etc/passwd): uid 0 is "root" with home "/", app uids (10000+) are
// "u0_aN" with home "/data", anything else is "uid_N". Only the reentrant
// versions: Emscripten's libc already has getpwuid()/getpwnam() stubs, and
// libcore uses the _r functions.
namespace {

int FillPasswd(uid_t uid, struct passwd* pwd, char* buf, size_t buflen,
               struct passwd** result) {
  char name[32];
  const char* dir = "/data";
  if (uid == 0) {
    snprintf(name, sizeof(name), "root");
    dir = "/";
  } else if (uid >= 10000 && uid < 100000) {
    snprintf(name, sizeof(name), "u0_a%u", static_cast<unsigned>(uid - 10000));
  } else {
    snprintf(name, sizeof(name), "uid_%u", static_cast<unsigned>(uid));
  }
  const char kShell[] = "/system/bin/sh";
  const size_t need = strlen(name) + 1 + strlen(dir) + 1 + sizeof(kShell) + 1;
  *result = nullptr;
  if (buflen < need)
    return ERANGE;
  char* p = buf;
  pwd->pw_name = p;
  p = stpcpy(p, name) + 1;
  pwd->pw_dir = p;
  p = stpcpy(p, dir) + 1;
  pwd->pw_shell = p;
  p = stpcpy(p, kShell) + 1;
  pwd->pw_passwd = p;
  *p = '\0';
  pwd->pw_uid = uid;
  pwd->pw_gid = uid;
  pwd->pw_gecos = pwd->pw_passwd;
  *result = pwd;
  return 0;
}

uid_t UidForName(const char* name) {
  unsigned n;
  if (strcmp(name, "root") == 0)
    return 0;
  if (sscanf(name, "u0_a%u", &n) == 1)
    return 10000 + n;
  if (sscanf(name, "uid_%u", &n) == 1)
    return n;
  return static_cast<uid_t>(-1);
}

}  // namespace

extern "C" int getpwuid_r(uid_t uid, struct passwd* pwd, char* buf,
                          size_t buflen, struct passwd** result) {
  return FillPasswd(uid, pwd, buf, buflen, result);
}

extern "C" int getpwnam_r(const char* name, struct passwd* pwd, char* buf,
                          size_t buflen, struct passwd** result) {
  const uid_t uid = UidForName(name);
  if (uid == static_cast<uid_t>(-1)) {
    *result = nullptr;
    return 0;
  }
  return FillPasswd(uid, pwd, buf, buflen, result);
}
