// See arc_kernel.h.

#include "kernel/arc_kernel.h"

#include <algorithm>
#include <dirent.h>
#include <emscripten/syscalls.h>
#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/threading.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdarg.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <unistd.h>
#include <wasi/api.h>

#include <atomic>
#include <string>
#include <time.h>

#include "common/options.h"
#include "common/process_emulator.h"
#include "posix_translation/dev_alarm.h"
#include "posix_translation/dev_ashmem.h"
#include "posix_translation/dev_null.h"
#include "posix_translation/dev_urandom.h"
#include "posix_translation/device_file.h"
#include "posix_translation/dev_zero.h"
#include "posix_translation/dir.h"
#include "posix_translation/directory_manager.h"
#include "posix_translation/passthrough.h"
#include "posix_translation/permission_info.h"
#include "posix_translation/process_environment.h"
#include "../multiproc/include/arc_lproc.h"
#include "posix_translation/readonly_memory_file.h"
#include "posix_translation/statfs.h"
#include "posix_translation/net_bridge.h"
#include "posix_translation/tcp_socket.h"
#include "posix_translation/virtual_file_system.h"
#include "ppapi/cpp/instance.h"
#include "wasm_mm/wasm_mmap.h"

#include <limits.h>
#include <map>
#include <sys/resource.h>

using posix_translation::VirtualFileSystem;

// Table slots filled lazily, all threads together (tools/wasm_lazy_table.py
// adds to it when it finds this export; the test page logs it): each slot is
// a JS function in that thread's instance on Safari (topic ios-threads).
extern "C" EMSCRIPTEN_KEEPALIVE uint32_t arc_lazy_table_fills = 0;

// The VM's monotonic clock, one for all threads (shim/kernel/library_arc_clock.js):
// the latest reading of any thread (microseconds modulo 2^32), how many times
// a worker's clock was found behind and caught up, the largest lag (us); and
// ARC_CLOCK=0: each worker's own clock, as before (for comparisons).
extern "C" EMSCRIPTEN_KEEPALIVE int32_t arc_clock_now[4] = {0, 0, 0, 0};

__attribute__((constructor)) static void ArcClockSwitch() {
  const char* e = getenv("ARC_CLOCK");
  if (e && strcmp(e, "0") == 0)
    __atomic_store_n(&arc_clock_now[3], 1, __ATOMIC_SEQ_CST);
}

// --- The host: Emscripten's own implementations ----------------------------
//
// Declared under different C names but with the original import names, so
// they bind to the JS implementations even though this file defines the
// C-visible __syscall_* / __wasi_* symbols.

#define HOST_ENV(name) \
  __attribute__((import_module("env"), import_name("__syscall_" #name)))
#define HOST_WASI(name) \
  __attribute__((import_module("wasi_snapshot_preview1"), import_name(#name)))

extern "C" {
HOST_WASI(fd_read) __wasi_errno_t host_fd_read(
    __wasi_fd_t, const __wasi_iovec_t*, size_t, __wasi_size_t*);
HOST_WASI(fd_write) __wasi_errno_t host_fd_write(
    __wasi_fd_t, const __wasi_ciovec_t*, size_t, __wasi_size_t*);
HOST_WASI(fd_pread) __wasi_errno_t host_fd_pread(
    __wasi_fd_t, const __wasi_iovec_t*, size_t, __wasi_filesize_t,
    __wasi_size_t*);
HOST_WASI(fd_pwrite) __wasi_errno_t host_fd_pwrite(
    __wasi_fd_t, const __wasi_ciovec_t*, size_t, __wasi_filesize_t,
    __wasi_size_t*);
HOST_WASI(fd_close) __wasi_errno_t host_fd_close(__wasi_fd_t);
HOST_WASI(fd_seek) __wasi_errno_t host_fd_seek(
    __wasi_fd_t, __wasi_filedelta_t, __wasi_whence_t, __wasi_filesize_t*);
HOST_WASI(fd_sync) __wasi_errno_t host_fd_sync(__wasi_fd_t);
HOST_WASI(fd_fdstat_get) __wasi_errno_t host_fd_fdstat_get(
    __wasi_fd_t, __wasi_fdstat_t*);

HOST_ENV(openat) int host_openat(int, const char*, int, ...);
HOST_ENV(stat64) int host_stat64(const char*, struct stat*);
HOST_ENV(lstat64) int host_lstat64(const char*, struct stat*);
HOST_ENV(fstat64) int host_fstat64(int, struct stat*);
HOST_ENV(newfstatat) int host_newfstatat(int, const char*, struct stat*, int);
HOST_ENV(getdents64) int host_getdents64(int, void*, size_t);
HOST_ENV(fcntl64) int host_fcntl64(int, int, ...);
HOST_ENV(ioctl) int host_ioctl(int, int, ...);
HOST_ENV(access) int host_access(const char*, int);
HOST_ENV(faccessat) int host_faccessat(int, const char*, int, int);
HOST_ENV(chmod) int host_chmod(const char*, mode_t);
HOST_ENV(fchmod) int host_fchmod(int, mode_t);
HOST_ENV(fchmodat2) int host_fchmodat2(int, const char*, mode_t, int);
HOST_ENV(mkdirat) int host_mkdirat(int, const char*, mode_t);
HOST_ENV(rmdir) int host_rmdir(const char*);
HOST_ENV(unlinkat) int host_unlinkat(int, const char*, int);
HOST_ENV(renameat) int host_renameat(int, const char*, int, const char*);
HOST_ENV(readlinkat) int host_readlinkat(int, const char*, char*, size_t);
HOST_ENV(symlinkat) int host_symlinkat(const char*, int, const char*);
HOST_ENV(getcwd) int host_getcwd(char*, size_t);
HOST_ENV(chdir) int host_chdir(const char*);
HOST_ENV(truncate64) int host_truncate64(const char*, off_t);
HOST_ENV(ftruncate64) int host_ftruncate64(int, off_t);
HOST_ENV(statfs64) int host_statfs64(const char*, size_t, struct statfs*);
HOST_ENV(fstatfs64) int host_fstatfs64(int, size_t, struct statfs*);
HOST_ENV(dup) int host_dup(int);
HOST_ENV(dup3) int host_dup3(int, int, int);
HOST_ENV(pipe2) int host_pipe2(int*, int);
HOST_ENV(poll) int host_poll(struct pollfd*, nfds_t, int);
HOST_ENV(poll_nonblocking) int host_poll_nonblocking(struct pollfd*, nfds_t);
HOST_ENV(umask) mode_t host_umask(mode_t);
HOST_ENV(fdatasync) int host_fdatasync(int);
HOST_ENV(socket) int host_socket(int, int, int, int, int, int);
HOST_ENV(bind) int host_bind(int, const struct sockaddr*, socklen_t, int, int,
                             int);
HOST_ENV(connect) int host_connect(int, const struct sockaddr*, socklen_t, int,
                                   int, int);
HOST_ENV(listen) int host_listen(int, int, int, int, int, int);
HOST_ENV(accept4) int host_accept4(int, struct sockaddr*, socklen_t*, int, int,
                                   int);
HOST_ENV(getsockopt) int host_getsockopt(int, int, int, void*, socklen_t*,
                                         int);
HOST_ENV(setsockopt) int host_setsockopt(int, int, int, const void*,
                                         socklen_t, int);
HOST_ENV(getsockname) int host_getsockname(int, struct sockaddr*, socklen_t*,
                                           int, int, int);
HOST_ENV(getpeername) int host_getpeername(int, struct sockaddr*, socklen_t*,
                                           int, int, int);
HOST_ENV(sendto) int host_sendto(int, const void*, size_t, int,
                                 const struct sockaddr*, socklen_t);
HOST_ENV(sendmsg) int host_sendmsg(int, const struct msghdr*, int, int, int,
                                   int);
HOST_ENV(recvfrom) int host_recvfrom(int, void*, size_t, int, struct sockaddr*,
                                     socklen_t*);
HOST_ENV(recvmsg) int host_recvmsg(int, struct msghdr*, int, int, int, int);
HOST_ENV(shutdown) int host_shutdown(int, int, int, int, int, int);
HOST_ENV(epoll_create1) int host_epoll_create1(int);
HOST_ENV(epoll_ctl) int host_epoll_ctl(int, int, int, struct epoll_event*);
HOST_ENV(epoll_pwait) int host_epoll_pwait(int, struct epoll_event*, int, int,
                                           const sigset_t*, size_t);
HOST_ENV(epoll_pwait_nonblocking) int host_epoll_pwait_nonblocking(
    int, struct epoll_event*, int);
}  // extern "C"

// Emscripten has no host implementation of these.
static int host_socketpair(int, int, int, int*, int, int) { return -ENOSYS; }

// --- Routing ---------------------------------------------------------------

namespace {

std::atomic<VirtualFileSystem*> g_vfs(NULL);
std::atomic<unsigned long> g_routed_calls(0);  // NOLINT
thread_local int t_kernel_depth = 0;

// Marks "running inside the kernel" for the current thread: nested libc
// calls made by posix_translation go to the host.
class KernelScope {
 public:
  KernelScope() {
    ++t_kernel_depth;
    g_routed_calls.fetch_add(1, std::memory_order_relaxed);
  }
  ~KernelScope() { --t_kernel_depth; }
};

// Returns the VFS if this call should be routed into it, else NULL.
VirtualFileSystem* Route() {
  if (t_kernel_depth > 0)
    return NULL;
  VirtualFileSystem* vfs = g_vfs.load(std::memory_order_acquire);
  if (!vfs || emscripten_is_main_browser_thread())
    return NULL;
  return vfs;
}

// POSIX result (-1 + errno) -> kernel result (-errno).
inline int Sys(long result) {  // NOLINT
  return result < 0 ? -errno : static_cast<int>(result);
}

// POSIX result -> WASI errno.
inline __wasi_errno_t Wasi(long result) {  // NOLINT
  return result < 0 ? static_cast<__wasi_errno_t>(errno) : 0;
}

void SetNonBlocking(VirtualFileSystem* vfs, int fd) {
  base::AutoLock lock(vfs->mutex());
  scoped_refptr<posix_translation::FileStream> stream =
      vfs->GetStreamLocked(fd);
  if (stream)
    stream->set_oflag(stream->oflag() | O_NONBLOCK);
}

// VirtualFileSystem::shutdown() does nothing; TCP sockets implement it.
int Shutdown(VirtualFileSystem* vfs, int fd, int how) {
  base::AutoLock lock(vfs->mutex());
  scoped_refptr<posix_translation::FileStream> stream =
      vfs->GetStreamLocked(fd);
  if (!stream) {
    errno = EBADF;
    return -1;
  }
  if (strcmp(stream->GetStreamType(), "tcp") == 0)
    return static_cast<posix_translation::TCPSocket*>(stream.get())
        ->Shutdown(how);
  return 0;
}

bool IsAtCwd(int dirfd, const char* path) {
  return dirfd == AT_FDCWD || (path && path[0] == '/');
}

// The working directory: the VM's, and one of its own for each spawned
// process (topic native-exec: a program an app runs with
// ProcessBuilder.directory(), chdir() in it). Called under the VFS's mutex;
// arc_kernel_set_process_cwd takes it too.
class DefaultProcessEnvironment
    : public posix_translation::ProcessEnvironment {
 public:
  DefaultProcessEnvironment() : cwd_("/"), umask_(022) {}
  virtual std::string GetCurrentDirectory() const {
    if (!cwds_.empty()) {
      std::map<int, std::string>::const_iterator it = cwds_.find(arc_lproc_current());
      if (it != cwds_.end())
        return it->second;
    }
    return cwd_;
  }
  virtual void SetCurrentDirectory(const std::string& dir) {
    if (!cwds_.empty()) {
      std::map<int, std::string>::iterator it = cwds_.find(arc_lproc_current());
      if (it != cwds_.end()) {
        it->second = dir;
        return;
      }
    }
    cwd_ = dir;
  }
  virtual mode_t GetCurrentUmask() const { return umask_; }
  virtual void SetCurrentUmask(mode_t mask) { umask_ = mask; }
  // |dir| ("/a/b/", as chdir leaves it), or "" to forget process |pid|'s.
  void SetProcessDirectory(int pid, const std::string& dir) {
    if (dir.empty())
      cwds_.erase(pid);
    else
      cwds_[pid] = dir;
  }

 private:
  std::string cwd_;
  std::map<int, std::string> cwds_;
  mode_t umask_;
};

DefaultProcessEnvironment* g_environment = NULL;

// A read-only file of fixed text (/proc/cpuinfo, /sys/devices/system/cpu/*).
class TextFile : public posix_translation::ReadonlyMemoryFile {
 public:
  TextFile(const std::string& pathname, const std::string& text)
      : ReadonlyMemoryFile(pathname, ENODEV, time(NULL)), content_(text.begin(), text.end()) {}

 protected:
  virtual ~TextFile() {}
  virtual const Content& GetContent() OVERRIDE { return content_; }
  virtual int fstatfs(struct statfs* buf) OVERRIDE {
    return posix_translation::DoStatFsForProc(buf);
  }

 private:
  Content content_;
};

// binfmt_misc (topic native-exec): the registry is in the NDK bridge
// (shim/ndk/ndk_exec.cc); these reach it. Weak — the kernel unit test links
// no bridge, and then /proc/sys/fs/binfmt_misc just refuses writes.
extern "C" int arc_binfmt_register(const char* line, size_t n) __attribute__((weak));
extern "C" int arc_binfmt_status_write(const char* buf, size_t n) __attribute__((weak));
extern "C" size_t arc_binfmt_status_read(char* buf, size_t size) __attribute__((weak));
extern "C" size_t arc_binfmt_list(char* buf, size_t size) __attribute__((weak));

// /proc/sys/fs/binfmt_misc/{register,status}: Linux's interface for
// registering an interpreter for a binary format (register is write-only,
// status is read/write). A program or the stand registers rules the Linux
// way; no per-package special cases.
class BinfmtFile : public posix_translation::FileStream {
 public:
  BinfmtFile(int oflag, const std::string& pathname, bool status)
      : FileStream(oflag, pathname), status_(status) {}
  virtual ssize_t read(void* buf, size_t count) OVERRIDE {
    if (!status_ || !arc_binfmt_status_read)
      return 0;  // "register" is write-only
    char tmp[64];
    const size_t n = arc_binfmt_status_read(tmp, sizeof tmp);
    if (pos_ >= n)
      return 0;
    const size_t c = std::min(count, n - pos_);
    memcpy(buf, tmp + pos_, c);
    pos_ += c;
    return c;
  }
  virtual ssize_t write(const void* buf, size_t count) OVERRIDE {
    const char* p = static_cast<const char*>(buf);
    int r = -ENOSYS;
    if (status_ && arc_binfmt_status_write)
      r = arc_binfmt_status_write(p, count);
    else if (!status_ && arc_binfmt_register)
      r = arc_binfmt_register(p, count);
    if (r < 0) {
      errno = -r;
      return -1;
    }
    return count;  // a whole register line per write, as Linux expects
  }
  virtual int fstat(struct stat* out) OVERRIDE {
    memset(out, 0, sizeof(*out));
    out->st_mode = S_IFREG | (status_ ? 0644 : 0200);
    out->st_nlink = 1;
    return 0;
  }
  virtual int fstatfs(struct statfs* out) OVERRIDE { return posix_translation::DoStatFsForProc(out); }
  virtual const char* GetStreamType() const OVERRIDE { return "binfmt_misc"; }

 private:
  bool status_;
  size_t pos_ = 0;
};

class BinfmtHandler : public posix_translation::FileSystemHandler {
 public:
  BinfmtHandler() : FileSystemHandler("BinfmtHandler") {}
  void OnMounted(const std::string& path) OVERRIDE {
    mount_ = path;
    while (mount_.size() > 1 && mount_.back() == '/') mount_.pop_back();
  }
  // "" for the directory itself, else the entry name ("register", "status", a rule).
  std::string Entry(const std::string& p) const {
    std::string s = p;
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    if (s == mount_) return "";
    if (s.compare(0, mount_.size() + 1, mount_ + "/") == 0) return s.substr(mount_.size() + 1);
    return "\x01";  // not ours
  }
  scoped_refptr<posix_translation::FileStream> open(int fd, const std::string& pathname, int oflag,
                                                    mode_t cmode) OVERRIDE {
    const std::string e = Entry(pathname);
    if (e == "register" || e == "status")
      return new BinfmtFile(oflag, pathname, e == "status");
    errno = (e.empty()) ? EISDIR : EACCES;
    return NULL;
  }
  int stat(const std::string& pathname, struct stat* out) OVERRIDE {
    const std::string e = Entry(pathname);
    if (e == "\x01") { errno = ENOENT; return -1; }
    memset(out, 0, sizeof(*out));
    if (e.empty()) { out->st_mode = S_IFDIR | 0755; out->st_nlink = 2; }
    else { out->st_mode = S_IFREG | (e == "status" ? 0644 : 0200); out->st_nlink = 1; }
    return 0;
  }
  int statfs(const std::string& pathname, struct statfs* out) OVERRIDE {
    return posix_translation::DoStatFsForProc(out);
  }
  posix_translation::Dir* OnDirectoryContentsNeeded(const std::string& name) OVERRIDE {
    if (!Entry(name).empty()) return NULL;
    posix_translation::DirectoryManager listing;
    const std::string dir = mount_ + "/";
    listing.MakeDirectories(dir);
    listing.AddFileWithType(dir + "register", posix_translation::Dir::REGULAR);
    listing.AddFileWithType(dir + "status", posix_translation::Dir::REGULAR);
    if (arc_binfmt_list) {
      std::string names(arc_binfmt_list(NULL, 0), '\0');
      if (!names.empty()) {
        arc_binfmt_list(&names[0], names.size());
        size_t start = 0;
        for (size_t i = 0; i <= names.size(); i++) {
          if (i == names.size() || names[i] == '\n') {
            if (i > start) listing.AddFileWithType(dir + names.substr(start, i - start), posix_translation::Dir::REGULAR);
            start = i + 1;
          }
        }
      }
    }
    return listing.OpenDirectory(dir);
  }

 private:
  std::string mount_;
};

class TextFileHandler : public posix_translation::FileSystemHandler {
 public:
  explicit TextFileHandler(const std::string& text)
      : FileSystemHandler("TextFileHandler"), text_(text) {}

  virtual scoped_refptr<posix_translation::FileStream> open(int fd, const std::string& pathname,
                                                            int oflag, mode_t cmode) OVERRIDE {
    if ((oflag & O_ACCMODE) != O_RDONLY) {
      errno = EACCES;
      return NULL;
    }
    return new TextFile(pathname, text_);
  }
  virtual int stat(const std::string& pathname, struct stat* out) OVERRIDE {
    memset(out, 0, sizeof(*out));
    out->st_mode = S_IFREG | 0444;
    out->st_nlink = 1;
    out->st_size = text_.size();
    return 0;
  }
  virtual int statfs(const std::string& pathname, struct statfs* out) OVERRIDE {
    return posix_translation::DoStatFsForProc(out);
  }
  virtual posix_translation::Dir* OnDirectoryContentsNeeded(const std::string&) OVERRIDE {
    return NULL;
  }

 private:
  const std::string text_;
};

// /proc/cpuinfo of the ARM CPU guest code runs on (shim/ndk/cpu): ARMv7
// with what AT_HWCAP says (ndk_bionic.cc: G_getauxval), no AES/PMULL/SHA.
// Code that finds NEON through the NDK's cpufeatures reads it: Unity's
// libmain.so refused to load libunity.so without it ("Your hardware does
// not support this application", Subway Surfers).
std::string CpuInfo(int cpus) {
  std::string out;
  char buf[512];
  for (int i = 0; i < cpus; ++i) {
    snprintf(buf, sizeof(buf),
             "processor\t: %d\nmodel name\t: ARMv7 Processor rev 1 (v7l)\nBogoMIPS\t: 38.40\n"
             "Features\t: swp half thumb fastmult vfp edsp neon vfpv3 tls vfpv4 idiva idivt vfpd32\n"
             "CPU implementer\t: 0x41\nCPU architecture: 7\nCPU variant\t: 0x0\n"
             "CPU part\t: 0xc0f\nCPU revision\t: 1\n\n", i);
    out += buf;
  }
  out += "Hardware\t: arc-wasm\nRevision\t: 0000\nSerial\t\t: 0000000000000000\n";
  return out;
}

}  // namespace

// Marks the calling thread as part of the kernel for good: its libc calls
// (e.g. logging) go to the host. posix_translation's own service threads
// (the net thread, which runs with the VFS mutex held) call it through a
// weak reference, so posix_translation links without this file.
extern "C" void arc_kernel_mark_kernel_thread() { ++t_kernel_depth; }

// The thread holding the VirtualFileSystem mutex (its gettid(), 0 = free);
// racy, for diagnostics (artvm --watchdog).
extern "C" int arc_kernel_vfs_owner_tid() {
  posix_translation::VirtualFileSystem* vfs =
      posix_translation::VirtualFileSystem::GetVirtualFileSystem();
  return vfs ? vfs->mutex().owner_tid() : 0;
}

namespace arc_kernel {

VirtualFileSystem* Boot() {
  LOG_ALWAYS_FATAL_IF(g_vfs.load(), "arc_kernel::Boot() called twice");
  LOG_ALWAYS_FATAL_IF(emscripten_is_main_browser_thread(),
                      "Boot the kernel off the browser main thread");
  arc::Options* options = arc::Options::GetInstance();
  options->Put("save_logs_to_file", "false");
  options->Put("enable_arc_strace", "false");
  options->Put("enable_external_directory", "false");

  static pp::Instance instance(1);
  static DefaultProcessEnvironment environment;
  g_environment = &environment;
  VirtualFileSystem* vfs = new VirtualFileSystem(&instance, &environment,
                                                 0 /* min fd */, 1023);
  posix_translation::SetVirtualFileSystemInterface(vfs);

  // stdin/stdout/stderr pass through to the host's descriptors 0-2.
  posix_translation::PassthroughHandler stdio;
  const int kStdioFlags[3] = {O_RDONLY, O_WRONLY, O_WRONLY};
  for (int fd = 0; fd < 3; ++fd) {
    scoped_refptr<posix_translation::FileStream> stream =
        stdio.open(fd, std::string(), kStdioFlags[fd], 0);
    stream->set_permission(posix_translation::PermissionInfo(
        arc::ProcessEmulator::GetUid(), fd != 0));
    vfs->RegisterFileStream(fd, stream);
  }

  // stat() of a device reports its number (st_rdev); unregistered names
  // fail an assertion in DeviceHandler::GetDeviceId. Linux's numbers
  // (misc devices: as on an Android 4.4 device).
  posix_translation::DeviceHandler::AddDeviceId("/dev/null", 1, 3);
  posix_translation::DeviceHandler::AddDeviceId("/dev/zero", 1, 5);
  posix_translation::DeviceHandler::AddDeviceId("/dev/random", 1, 8);
  posix_translation::DeviceHandler::AddDeviceId("/dev/urandom", 1, 9);
  posix_translation::DeviceHandler::AddDeviceId("/dev/ashmem", 10, 61);
  posix_translation::DeviceHandler::AddDeviceId("/dev/alarm", 10, 62);
  vfs->Mount("/dev/null", new posix_translation::DevNullHandler);
  vfs->Mount("/dev/zero", new posix_translation::DevZeroHandler);
  vfs->Mount("/dev/urandom", new posix_translation::DevUrandomHandler);
  vfs->Mount("/dev/random", new posix_translation::DevUrandomHandler);
  vfs->Mount("/dev/ashmem", new posix_translation::DevAshmemHandler);
  vfs->Mount("/dev/alarm", new posix_translation::DevAlarmHandler);
  // lo and eth0 under /sys/class/net and /proc/net (host files: libc is not
  // routed yet).
  posix_translation::net::MountNetworkFiles(vfs, "/arc/net");
  // The CPU, as Linux describes it (the browser's cores, 1-8).
  int cpus = emscripten_num_logical_cores();
  cpus = cpus < 1 ? 1 : cpus > 8 ? 8 : cpus;
  vfs->Mount("/proc/cpuinfo", new TextFileHandler(CpuInfo(cpus)));
  const std::string range = cpus == 1 ? "0\n" : "0-" + std::to_string(cpus - 1) + "\n";
  vfs->Mount("/sys/devices/system/cpu/present", new TextFileHandler(range));
  vfs->Mount("/sys/devices/system/cpu/possible", new TextFileHandler(range));
  // binfmt_misc: interpreters for binary formats (topic native-exec), the
  // Linux way. A program (or the stand) registers rules by writing
  // /proc/sys/fs/binfmt_misc/register.
  vfs->Mount("/proc/sys/fs/binfmt_misc/", new BinfmtHandler);
  vfs->SetBrowserReady();

  g_vfs.store(vfs, std::memory_order_release);
  return vfs;
}

bool IsBooted() { return g_vfs.load() != NULL; }

}  // namespace arc_kernel

// Spawned process |pid|'s working directory (topic native-exec): |dir| as
// given (a directory that must exist; NULL: the caller's), "" to forget it.
extern "C" int arc_kernel_set_process_cwd(int pid, const char* dir) {
  VirtualFileSystem* vfs = g_vfs.load(std::memory_order_acquire);
  if (!vfs || !g_environment)
    return -ENOSYS;
  std::string d;
  if (dir && *dir) {
    struct stat st;
    {
      KernelScope scope;
      if (vfs->stat(dir, &st) != 0)
        return -errno;
    }
    if (!S_ISDIR(st.st_mode))
      return -ENOTDIR;
    char real[PATH_MAX];
    {
      KernelScope scope;
      if (!vfs->realpath(dir, real))
        return -errno;
    }
    d = real;
    if (d.empty() || d[d.size() - 1] != '/')
      d += '/';
  } else if (dir) {
    base::AutoLock lock(vfs->mutex());
    g_environment->SetProcessDirectory(pid, std::string());
    return 0;
  } else {
    base::AutoLock lock(vfs->mutex());
    d = g_environment->GetCurrentDirectory();
  }
  base::AutoLock lock(vfs->mutex());
  g_environment->SetProcessDirectory(pid, d);
  return 0;
}

// A spawned process's start and end (topic native-exec, shim/ndk/ndk_exec.cc):
// its descriptors 0-2 (|fds|: the caller's) and working directory (|dir|,
// null: the caller's); then what it left open closed and its directory forgotten.
extern "C" int arc_kernel_process_setup(int pid, const int fds[3], const char* dir) {
  VirtualFileSystem* vfs = g_vfs.load(std::memory_order_acquire);
  if (!vfs)
    return -ENOSYS;
  int r;
  {
    KernelScope scope;
    r = vfs->SetupProcessFds(pid, fds);
  }
  if (r < 0)
    return -errno;
  return arc_kernel_set_process_cwd(pid, dir);
}

extern "C" void arc_kernel_process_release(int pid) {
  VirtualFileSystem* vfs = g_vfs.load(std::memory_order_acquire);
  if (!vfs)
    return;
  {
    KernelScope scope;
    vfs->ReleaseProcessFds(pid);
  }
  arc_kernel_set_process_cwd(pid, "");
}

namespace arc_kernel {

unsigned long RoutedCallCount() {  // NOLINT
  return g_routed_calls.load(std::memory_order_relaxed);
}

}  // namespace arc_kernel

// --- WASI fd_* -------------------------------------------------------------

extern "C" {

__wasi_errno_t __wasi_fd_read(__wasi_fd_t fd, const __wasi_iovec_t* iovs,
                              size_t iovs_len, __wasi_size_t* nread) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return host_fd_read(fd, iovs, iovs_len, nread);
  KernelScope scope;
  // __wasi_iovec_t has struct iovec's layout on wasm32.
  const int r = vfs->readv(fd, reinterpret_cast<const struct iovec*>(iovs),
                           iovs_len);
  if (r >= 0)
    *nread = r;
  return Wasi(r);
}

__wasi_errno_t __wasi_fd_write(__wasi_fd_t fd, const __wasi_ciovec_t* iovs,
                               size_t iovs_len, __wasi_size_t* nwritten) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return host_fd_write(fd, iovs, iovs_len, nwritten);
  KernelScope scope;
  const int r = vfs->writev(fd, reinterpret_cast<const struct iovec*>(iovs),
                            iovs_len);
  if (r >= 0)
    *nwritten = r;
  return Wasi(r);
}

__wasi_errno_t __wasi_fd_pread(__wasi_fd_t fd, const __wasi_iovec_t* iovs,
                               size_t iovs_len, __wasi_filesize_t offset,
                               __wasi_size_t* nread) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return host_fd_pread(fd, iovs, iovs_len, offset, nread);
  KernelScope scope;
  size_t total = 0;
  for (size_t i = 0; i < iovs_len; ++i) {
    const ssize_t r = vfs->pread(fd, iovs[i].buf, iovs[i].buf_len,
                                 offset + total);
    if (r < 0) {
      if (total)
        break;
      return Wasi(r);
    }
    total += r;
    if (static_cast<size_t>(r) < iovs[i].buf_len)
      break;
  }
  *nread = total;
  return 0;
}

__wasi_errno_t __wasi_fd_pwrite(__wasi_fd_t fd, const __wasi_ciovec_t* iovs,
                                size_t iovs_len, __wasi_filesize_t offset,
                                __wasi_size_t* nwritten) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return host_fd_pwrite(fd, iovs, iovs_len, offset, nwritten);
  KernelScope scope;
  size_t total = 0;
  for (size_t i = 0; i < iovs_len; ++i) {
    const ssize_t r = vfs->pwrite(fd, iovs[i].buf, iovs[i].buf_len,
                                  offset + total);
    if (r < 0) {
      if (total)
        break;
      return Wasi(r);
    }
    total += r;
    if (static_cast<size_t>(r) < iovs[i].buf_len)
      break;
  }
  *nwritten = total;
  return 0;
}

__wasi_errno_t __wasi_fd_close(__wasi_fd_t fd) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return host_fd_close(fd);
  KernelScope scope;
  return Wasi(vfs->close(fd));
}

__wasi_errno_t __wasi_fd_seek(__wasi_fd_t fd, __wasi_filedelta_t offset,
                              __wasi_whence_t whence,
                              __wasi_filesize_t* newoffset) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return host_fd_seek(fd, offset, whence, newoffset);
  KernelScope scope;
  const off64_t r = vfs->lseek(fd, offset, whence);
  if (r >= 0)
    *newoffset = r;
  return Wasi(r < 0 ? -1 : 0);
}

__wasi_errno_t __wasi_fd_sync(__wasi_fd_t fd) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return host_fd_sync(fd);
  KernelScope scope;
  return Wasi(vfs->fsync(fd));
}

__wasi_errno_t __wasi_fd_fdstat_get(__wasi_fd_t fd, __wasi_fdstat_t* stat) {
  VirtualFileSystem* vfs = Route();
  // stdio is passed through to the host; let it answer isatty() & co. (A
  // spawned process's 0-2 are its own: topic native-exec.)
  if (!vfs || (fd <= 2 && !(arc_lproc_any() && arc_lproc_is_spawned(arc_lproc_current()))))
    return host_fd_fdstat_get(fd, stat);
  KernelScope scope;
  struct stat st;
  if (vfs->fstat(fd, &st) < 0)
    return Wasi(-1);
  memset(stat, 0, sizeof(*stat));
  if (S_ISREG(st.st_mode))
    stat->fs_filetype = __WASI_FILETYPE_REGULAR_FILE;
  else if (S_ISDIR(st.st_mode))
    stat->fs_filetype = __WASI_FILETYPE_DIRECTORY;
  else if (S_ISCHR(st.st_mode))
    stat->fs_filetype = __WASI_FILETYPE_CHARACTER_DEVICE;
  else if (S_ISLNK(st.st_mode))
    stat->fs_filetype = __WASI_FILETYPE_SYMBOLIC_LINK;
  else if (S_ISSOCK(st.st_mode))
    stat->fs_filetype = __WASI_FILETYPE_SOCKET_STREAM;
  else
    stat->fs_filetype = __WASI_FILETYPE_UNKNOWN;
  stat->fs_rights_base = ~0ULL;
  stat->fs_rights_inheriting = ~0ULL;
  return 0;
}

// --- Time and ids, without a JavaScript call each --------------------------
//
// Emscripten's clock_time_get takes and writes 64-bit values: two BigInts on
// the calling worker per call, ~250 calls a second in Angry Birds' menu, all
// garbage for that worker's GC (docs/topics/ios-mem.md). Its get[e]uid32 and
// get[e]gid32 (0 here) are proxied to the page's main thread and wait for it:
// a round trip per getuid(), 30 a second there. emscripten_get_now and
// emscripten_date_now return doubles; the same clocks as Emscripten's
// (realtime: Date.now(), the others: performance.now()).

__wasi_errno_t __wasi_clock_time_get(__wasi_clockid_t id, __wasi_timestamp_t precision,
                                     __wasi_timestamp_t* time) {
  (void)precision;
  double ms;
  switch (id) {
    case __WASI_CLOCKID_REALTIME:
      ms = emscripten_date_now();
      break;
    case __WASI_CLOCKID_MONOTONIC:
    case __WASI_CLOCKID_PROCESS_CPUTIME_ID:
    case __WASI_CLOCKID_THREAD_CPUTIME_ID:
      ms = emscripten_get_now();
      break;
    default:
      return __WASI_ERRNO_INVAL;
  }
  *time = static_cast<__wasi_timestamp_t>(llround(ms * 1e6));
  return __WASI_ERRNO_SUCCESS;
}

// The calling thread's process (topic multiproc, shim/multiproc): the VM's
// (pid 42, uid 0) unless the thread belongs to a light process.
uid_t __syscall_getuid32() { return arc_lproc_current_uid(); }
uid_t __syscall_geteuid32() { return arc_lproc_current_uid(); }
gid_t __syscall_getgid32() { return arc_lproc_current_uid(); }
gid_t __syscall_getegid32() { return arc_lproc_current_uid(); }
pid_t __syscall_getpid() { return arc_lproc_current(); }
pid_t __syscall_getppid() { return arc_lproc_parent(arc_lproc_current()); }

// waitpid/wait4: the spawned processes (topic native-exec); a child of the
// VM itself there is none.
pid_t __syscall_wait4(pid_t pid, int* wstatus, int options, struct rusage* rusage) {
  if (rusage)
    memset(rusage, 0, sizeof(*rusage));
  const int r = arc_lproc_waitpid(pid, wstatus, options);
  return r < 0 ? -errno : r;
}

// --- __syscall_*: files and directories ------------------------------------

int __syscall_openat(int dirfd, const char* path, int flags, ...) {
  int mode = 0;
  if (flags & (O_CREAT | O_TMPFILE)) {
    va_list ap;
    va_start(ap, flags);
    mode = va_arg(ap, int);
    va_end(ap);
  }
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return host_openat(dirfd, path, flags, mode);
  if (!IsAtCwd(dirfd, path))
    return -ENOTSUP;  // posix_translation has no *at() family.
  KernelScope scope;
  return Sys(vfs->open(path, flags, mode));
}

int __syscall_stat64(const char* path, struct stat* buf) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return host_stat64(path, buf);
  KernelScope scope;
  return Sys(vfs->stat(path, buf));
}

int __syscall_lstat64(const char* path, struct stat* buf) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return host_lstat64(path, buf);
  KernelScope scope;
  return Sys(vfs->lstat(path, buf));
}

int __syscall_fstat64(int fd, struct stat* buf) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return host_fstat64(fd, buf);
  KernelScope scope;
  return Sys(vfs->fstat(fd, buf));
}

int __syscall_newfstatat(int dirfd, const char* path, struct stat* buf,
                         int flags) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return host_newfstatat(dirfd, path, buf, flags);
  KernelScope scope;
  if ((flags & AT_EMPTY_PATH) && (!path || !*path))
    return Sys(vfs->fstat(dirfd, buf));
  if (!IsAtCwd(dirfd, path))
    return -ENOTSUP;
  return Sys((flags & AT_SYMLINK_NOFOLLOW) ? vfs->lstat(path, buf)
                                           : vfs->stat(path, buf));
}

int __syscall_getdents64(int fd, void* dirp, size_t count) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return host_getdents64(fd, dirp, count);
  KernelScope scope;
  return Sys(vfs->getdents(fd, static_cast<dirent*>(dirp), count));
}

int __syscall_fcntl64(int fd, int cmd, ...) {
  va_list ap;
  va_start(ap, cmd);
  VirtualFileSystem* vfs = Route();
  int r;
  if (!vfs) {
    void* arg = va_arg(ap, void*);  // int or pointer: both 32-bit on wasm32.
    r = host_fcntl64(fd, cmd, arg);
  } else {
    KernelScope scope;
    r = Sys(vfs->fcntl(fd, cmd, ap));
  }
  va_end(ap);
  return r;
}

int __syscall_ioctl(int fd, int request, ...) {
  va_list ap;
  va_start(ap, request);
  VirtualFileSystem* vfs = Route();
  int r;
  if (!vfs) {
    void* arg = va_arg(ap, void*);
    r = host_ioctl(fd, request, arg);
  } else {
    KernelScope scope;
    r = Sys(vfs->ioctl(fd, request, ap));
  }
  va_end(ap);
  return r;
}

int __syscall_access(const char* path, int amode) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return host_access(path, amode);
  KernelScope scope;
  return Sys(vfs->access(path, amode));
}

int __syscall_faccessat(int dirfd, const char* path, int amode, int flags) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return host_faccessat(dirfd, path, amode, flags);
  if (!IsAtCwd(dirfd, path))
    return -ENOTSUP;
  KernelScope scope;
  return Sys(vfs->access(path, amode));
}

// chmod: the file's handler takes the mode (VirtualFileSystem::chmod): /data
// on OPFS keeps it (File.setExecutable, chmod +x; topic native-exec), other
// handlers keep no permission bits (every file is the VM's) and only check
// that the file exists, as file_wrap_stub's chmod did for NaCl. Without
// these emscripten's own FS answered ENOENT for every file of the VFS:
// File.setWritable/setExecutable were false (SoLoader's "cannot make
// directory writable for us", topic a10-default).
int __syscall_chmod(const char* path, mode_t mode) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return host_chmod(path, mode);
  KernelScope scope;
  return Sys(vfs->chmod(path, mode));
}

int __syscall_fchmod(int fd, mode_t mode) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return host_fchmod(fd, mode);
  KernelScope scope;
  return Sys(vfs->fchmod(fd, mode));
}

int __syscall_fchmodat2(int dirfd, const char* path, mode_t mode, int flags) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return host_fchmodat2(dirfd, path, mode, flags);
  KernelScope scope;
  if ((flags & AT_EMPTY_PATH) && (!path || !*path))
    return Sys(vfs->fchmod(dirfd, mode));
  if (!IsAtCwd(dirfd, path))
    return -ENOTSUP;
  struct stat st;
  // A symbolic link has no mode of its own (Linux: EOPNOTSUPP for one).
  if (flags & AT_SYMLINK_NOFOLLOW) {
    if (vfs->lstat(path, &st) != 0)
      return -errno;
    if (S_ISLNK(st.st_mode))
      return -EOPNOTSUPP;
  }
  return Sys(vfs->chmod(path, mode));
}

int __syscall_mkdirat(int dirfd, const char* path, mode_t mode) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return host_mkdirat(dirfd, path, mode);
  if (!IsAtCwd(dirfd, path))
    return -ENOTSUP;
  KernelScope scope;
  return Sys(vfs->mkdir(path, mode));
}

int __syscall_rmdir(const char* path) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return host_rmdir(path);
  KernelScope scope;
  return Sys(vfs->rmdir(path));
}

int __syscall_unlinkat(int dirfd, const char* path, int flags) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return host_unlinkat(dirfd, path, flags);
  if (!IsAtCwd(dirfd, path))
    return -ENOTSUP;
  KernelScope scope;
  return Sys((flags & AT_REMOVEDIR) ? vfs->rmdir(path) : vfs->unlink(path));
}

int __syscall_renameat(int olddirfd, const char* oldpath, int newdirfd,
                       const char* newpath) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return host_renameat(olddirfd, oldpath, newdirfd, newpath);
  if (!IsAtCwd(olddirfd, oldpath) || !IsAtCwd(newdirfd, newpath))
    return -ENOTSUP;
  KernelScope scope;
  return Sys(vfs->rename(oldpath, newpath));
}

int __syscall_readlinkat(int dirfd, const char* path, char* buf,
                         size_t bufsize) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return host_readlinkat(dirfd, path, buf, bufsize);
  if (!IsAtCwd(dirfd, path))
    return -ENOTSUP;
  KernelScope scope;
  return Sys(vfs->readlink(path, buf, bufsize));
}

int __syscall_symlinkat(const char* target, int newdirfd,
                        const char* linkpath) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return host_symlinkat(target, newdirfd, linkpath);
  if (!IsAtCwd(newdirfd, linkpath))
    return -ENOTSUP;
  KernelScope scope;
  return Sys(vfs->symlink(target, linkpath));
}

int __syscall_getcwd(char* buf, size_t size) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return host_getcwd(buf, size);
  KernelScope scope;
  if (!vfs->getcwd(buf, size))
    return -errno;
  return strlen(buf) + 1;
}

int __syscall_chdir(const char* path) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return host_chdir(path);
  KernelScope scope;
  return Sys(vfs->chdir(path));
}

int __syscall_truncate64(const char* path, off_t length) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return host_truncate64(path, length);
  KernelScope scope;
  return Sys(vfs->truncate(path, length));
}

int __syscall_ftruncate64(int fd, off_t length) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return host_ftruncate64(fd, length);
  KernelScope scope;
  return Sys(vfs->ftruncate(fd, length));
}

int __syscall_statfs64(const char* path, size_t size, struct statfs* buf) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return host_statfs64(path, size, buf);
  KernelScope scope;
  return Sys(vfs->statfs(path, buf));
}

int __syscall_fstatfs64(int fd, size_t size, struct statfs* buf) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return host_fstatfs64(fd, size, buf);
  KernelScope scope;
  return Sys(vfs->fstatfs(fd, buf));
}

int __syscall_dup(int fd) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return host_dup(fd);
  KernelScope scope;
  return Sys(vfs->dup(fd));
}

int __syscall_dup3(int oldfd, int newfd, int flags) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return host_dup3(oldfd, newfd, flags);
  if (oldfd == newfd)
    return -EINVAL;  // dup3 semantics; dup2(fd, fd) is handled by musl.
  KernelScope scope;
  return Sys(vfs->dup2(oldfd, newfd));  // O_CLOEXEC: there is no exec().
}

int __syscall_pipe2(int fd[2], int flags) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return host_pipe2(fd, flags);
  KernelScope scope;
  return Sys(vfs->pipe2(fd, flags));
}

int __syscall_poll(struct pollfd* fds, nfds_t nfds, int timeout) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return host_poll(fds, nfds, timeout);
  KernelScope scope;
  return Sys(vfs->poll(fds, nfds, timeout));
}

// Emscripten's poll() and epoll_wait() send a zero timeout here instead of
// to __syscall_poll/__syscall_epoll_pwait (an import that never suspends,
// for JSPI).
int __syscall_poll_nonblocking(struct pollfd* fds, nfds_t nfds) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return host_poll_nonblocking(fds, nfds);
  KernelScope scope;
  return Sys(vfs->poll(fds, nfds, 0));
}

mode_t __syscall_umask(mode_t mask) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return host_umask(mask);
  KernelScope scope;
  return vfs->umask(mask);
}

int __syscall_fdatasync(int fd) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return host_fdatasync(fd);
  KernelScope scope;
  return Sys(vfs->fdatasync(fd));
}

// --- __syscall_*: sockets and epoll -----------------------------------------

int __syscall_socket(int domain, int type, int protocol, int u1, int u2,
                     int u3) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return host_socket(domain, type, protocol, u1, u2, u3);
  KernelScope scope;
  // The VFS knows plain types only; apply SOCK_NONBLOCK/SOCK_CLOEXEC here
  // (musl does not retry without them on EAFNOSUPPORT).
  const int flags = type & (SOCK_NONBLOCK | SOCK_CLOEXEC);
  const int fd = vfs->socket(domain, type & ~flags, protocol);
  if (fd >= 0 && (flags & SOCK_NONBLOCK))
    SetNonBlocking(vfs, fd);
  return Sys(fd);
}

int __syscall_socketpair(int domain, int type, int protocol, int fd[2], int u1,
                         int u2) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return host_socketpair(domain, type, protocol, fd, u1, u2);
  KernelScope scope;
  // As socket(): SOCK_NONBLOCK/SOCK_CLOEXEC applied here — Gecko's IPC channels
  // (topic multiproc); musl does not retry without the flags on EOPNOTSUPP
  // (tokio's UnixStream::pair, LocalSend).
  const int flags = type & (SOCK_NONBLOCK | SOCK_CLOEXEC);
  const int r = vfs->socketpair(domain, type & ~flags, protocol, fd);
  if (r == 0 && (flags & SOCK_NONBLOCK)) {
    SetNonBlocking(vfs, fd[0]);
    SetNonBlocking(vfs, fd[1]);
  }
  return Sys(r);
}

int __syscall_bind(int sockfd, const struct sockaddr* addr, socklen_t len,
                   int u1, int u2, int u3) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return host_bind(sockfd, addr, len, u1, u2, u3);
  KernelScope scope;
  return Sys(vfs->bind(sockfd, addr, len));
}

int __syscall_connect(int sockfd, const struct sockaddr* addr, socklen_t len,
                      int u1, int u2, int u3) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return host_connect(sockfd, addr, len, u1, u2, u3);
  KernelScope scope;
  return Sys(vfs->connect(sockfd, addr, len));
}

int __syscall_listen(int sockfd, int backlog, int u1, int u2, int u3,
                     int u4) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return host_listen(sockfd, backlog, u1, u2, u3, u4);
  KernelScope scope;
  return Sys(vfs->listen(sockfd, backlog));
}

int __syscall_accept4(int sockfd, struct sockaddr* addr, socklen_t* len,
                      int flags, int u1, int u2) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return host_accept4(sockfd, addr, len, flags, u1, u2);
  KernelScope scope;
  const int fd = vfs->accept(sockfd, addr, len);
  if (fd >= 0 && (flags & SOCK_NONBLOCK))
    SetNonBlocking(vfs, fd);
  return Sys(fd);
}

int __syscall_getsockopt(int sockfd, int level, int optname, void* optval,
                         socklen_t* optlen, int u1) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return host_getsockopt(sockfd, level, optname, optval, optlen, u1);
  KernelScope scope;
  const int r = vfs->getsockopt(sockfd, level, optname, optval, optlen);
  // The VFS keeps no socket buffer sizes (0 unless set); Linux answers its
  // default, and Chromium's IPC (Gecko's channels to its content process,
  // topic multiproc) sizes its writes by it.
  if (r == 0 && level == SOL_SOCKET && (optname == SO_SNDBUF || optname == SO_RCVBUF) &&
      optval && optlen && *optlen >= sizeof(int) && *static_cast<int*>(optval) == 0)
    *static_cast<int*>(optval) = 212992;
  return Sys(r);
}

int __syscall_setsockopt(int sockfd, int level, int optname,
                         const void* optval, socklen_t optlen, int u1) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return host_setsockopt(sockfd, level, optname, optval, optlen, u1);
  KernelScope scope;
  return Sys(vfs->setsockopt(sockfd, level, optname, optval, optlen));
}

int __syscall_getsockname(int sockfd, struct sockaddr* addr, socklen_t* len,
                          int u1, int u2, int u3) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return host_getsockname(sockfd, addr, len, u1, u2, u3);
  KernelScope scope;
  return Sys(vfs->getsockname(sockfd, addr, len));
}

int __syscall_getpeername(int sockfd, struct sockaddr* addr, socklen_t* len,
                          int u1, int u2, int u3) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return host_getpeername(sockfd, addr, len, u1, u2, u3);
  KernelScope scope;
  return Sys(vfs->getpeername(sockfd, addr, len));
}

int __syscall_sendto(int sockfd, const void* buf, size_t len, int flags,
                     const struct sockaddr* addr, socklen_t alen) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return host_sendto(sockfd, buf, len, flags, addr, alen);
  KernelScope scope;
  return Sys(vfs->sendto(sockfd, buf, len, flags, addr, alen));
}

int __syscall_sendmsg(int sockfd, const struct msghdr* msg, int flags, int u1,
                      int u2, int u3) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return host_sendmsg(sockfd, msg, flags, u1, u2, u3);
  KernelScope scope;
  return Sys(vfs->sendmsg(sockfd, msg, flags));
}

int __syscall_recvfrom(int sockfd, void* buf, size_t len, int flags,
                       struct sockaddr* addr, socklen_t* alen) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return host_recvfrom(sockfd, buf, len, flags, addr, alen);
  KernelScope scope;
  return Sys(vfs->recvfrom(sockfd, buf, len, flags, addr, alen));
}

int __syscall_recvmsg(int sockfd, struct msghdr* msg, int flags, int u1,
                      int u2, int u3) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return host_recvmsg(sockfd, msg, flags, u1, u2, u3);
  KernelScope scope;
  return Sys(vfs->recvmsg(sockfd, msg, flags));
}

int __syscall_shutdown(int sockfd, int how, int u1, int u2, int u3, int u4) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return host_shutdown(sockfd, how, u1, u2, u3, u4);
  KernelScope scope;
  return Sys(Shutdown(vfs, sockfd, how));
}

int __syscall_epoll_create1(int flags) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return host_epoll_create1(flags);
  KernelScope scope;
  return Sys(vfs->epoll_create1(flags));
}

int __syscall_epoll_ctl(int epfd, int op, int fd, struct epoll_event* ev) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return host_epoll_ctl(epfd, op, fd, ev);
  KernelScope scope;
  return Sys(vfs->epoll_ctl(epfd, op, fd, ev));
}

int __syscall_epoll_pwait(int epfd, struct epoll_event* ev, int maxevents,
                          int timeout, const sigset_t* sigmask,
                          size_t sigsetsize) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return host_epoll_pwait(epfd, ev, maxevents, timeout, sigmask, sigsetsize);
  KernelScope scope;
  return Sys(vfs->epoll_wait(epfd, ev, maxevents, timeout));
}

int __syscall_epoll_pwait_nonblocking(int epfd, struct epoll_event* ev,
                                      int maxevents) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return host_epoll_pwait_nonblocking(epfd, ev, maxevents);
  KernelScope scope;
  return Sys(vfs->epoll_wait(epfd, ev, maxevents, 0));
}

// --- libc's mmap family ----------------------------------------------------
//
// Application mappings go through the VFS (which tracks them per stream and
// handles device files such as /dev/ashmem). Inside the kernel, the same
// names mean physical pages from the wasm page allocator.

void* mmap(void* addr, size_t length, int prot, int flags, int fd,
           off_t offset) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return __wasm_mm_mmap(addr, length, prot, flags, fd, offset);
  KernelScope scope;
  return vfs->mmap(addr, length, prot, flags, fd, offset);
}

int munmap(void* addr, size_t length) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return __wasm_mm_munmap(addr, length);
  KernelScope scope;
  return vfs->munmap(addr, length);
}

int mprotect(void* addr, size_t length, int prot) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return __wasm_mm_mprotect(addr, length, prot);
  KernelScope scope;
  return vfs->mprotect(addr, length, prot);
}

int madvise(void* addr, size_t length, int advice) {
  VirtualFileSystem* vfs = Route();
  if (!vfs)
    return __wasm_mm_madvise(addr, length, advice);
  KernelScope scope;
  return vfs->madvise(addr, length, advice);
}

// Not tracked by posix_translation: straight to the page allocator.
int msync(void* addr, size_t length, int flags) {
  return __wasm_mm_msync(addr, length, flags);
}
int posix_madvise(void* addr, size_t length, int advice) {
  return __wasm_mm_posix_madvise(addr, length, advice);
}
void* mremap(void* old_address, size_t old_size, size_t new_size, int flags,
             ...) {
  return __wasm_mm_mremap(old_address, old_size, new_size, flags);
}

}  // extern "C"
