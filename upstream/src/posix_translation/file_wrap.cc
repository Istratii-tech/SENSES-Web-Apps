/* Copyright 2014 The Chromium Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Wrappers for various file system calls.
 */

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <nacl_stat.h>
#include <poll.h>
#include <private/dlsym.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/vfs.h>
#include <unistd.h>
#include <utime.h>

#include <string>
#include <vector>

#include "base/basictypes.h"
#include "base/safe_strerror_posix.h"
#include "base/strings/string_piece.h"
#include "base/strings/string_util.h"
#include "common/arc_strace.h"
#include "common/alog.h"
#include "common/danger.h"
#include "common/dlfcn_injection.h"
#include "common/export.h"
#include "common/file_util.h"
#include "common/irt_wrapper_util.h"
#include "common/logd_write.h"
#include "common/memory_state.h"
#include "common/options.h"
#include "common/process_emulator.h"
#include "common/thread_local.h"
#include "common/trace_event.h"
#include "posix_translation/real_syscall.h"
#include "posix_translation/virtual_file_system.h"

// A helper macro to show both DIR pointer and its file descriptor in
// ARC strace.
#define PRETIFY_DIRP(dirp) (dirp) ? dirfd(dirp) : -1, (dirp)

// Note about large file support in ARC:
//
// Unlike glibc, Bionic does not support _LARGEFILE64_SOURCE and
// _FILE_OFFSET_BITS=64 macros. Instead, it always provides both foo() and
// foo64() functions. It is user code's responsibility to call foo64()
// explicitly instead of foo() when large file support is necessary.
// Note that Android's JNI code properly calls these 64-bit variants.
//
// For Bionic, we should provide both
//    __wrap_foo(type_t param1, another_type_t param2);
// and
//    __wrap_foo64(type64_t param1, another_type64_t param2);
// functions because both could be called.

extern "C" {
// sorted by function name.
ARC_EXPORT int __wrap_access(const char* pathname, int mode);
ARC_EXPORT int __wrap_chdir(const char* path);
ARC_EXPORT int __wrap_chown(const char* path, uid_t owner, gid_t group);
ARC_EXPORT int __wrap_closedir(DIR* dirp);
ARC_EXPORT int __wrap_dirfd(DIR* dirp);
ARC_EXPORT int __wrap_dladdr(const void* addr, Dl_info* info);
ARC_EXPORT int __wrap_dlclose(void* handle);
ARC_EXPORT void* __wrap_dlopen(const char* filename, int flag);
ARC_EXPORT void* __wrap_dlsym(void* handle, const char* symbol);
ARC_EXPORT DIR* __wrap_fdopendir(int fd);
ARC_EXPORT int __wrap_fstatfs(int fd, struct statfs* buf);
ARC_EXPORT long __wrap_fpathconf(int fd, int name);  // NOLINT
ARC_EXPORT char* __wrap_getcwd(char* buf, size_t size);
ARC_EXPORT int __wrap_open(const char* pathname, int flags, ...);
ARC_EXPORT DIR* __wrap_opendir(const char* name);
ARC_EXPORT long __wrap_pathconf(const char* path, int name);  // NOLINT
ARC_EXPORT struct dirent* __wrap_readdir(DIR* dirp);
ARC_EXPORT int __wrap_readdir_r(
    DIR* dirp, struct dirent* entry,
    struct dirent** result);
ARC_EXPORT ssize_t __wrap_readlink(const char* path, char* buf, size_t bufsiz);
ARC_EXPORT char* __wrap_realpath(const char* path, char* resolved_path);
ARC_EXPORT int __wrap_remove(const char* pathname);
ARC_EXPORT int __wrap_rename(const char* oldpath, const char* newpath);
ARC_EXPORT void __wrap_rewinddir(DIR* dirp);
ARC_EXPORT int __wrap_rmdir(const char* pathname);
ARC_EXPORT int __wrap_scandir(
    const char* dirp, struct dirent*** namelist,
    int (*filter)(const struct dirent*),
    int (*compar)(const struct dirent**, const struct dirent**));
ARC_EXPORT int __wrap_statfs(const char* path, struct statfs* stat);
ARC_EXPORT int __wrap_statvfs(const char* path, struct statvfs* stat);
ARC_EXPORT int __wrap_symlink(const char* oldp, const char* newp);
ARC_EXPORT mode_t __wrap_umask(mode_t mask);
ARC_EXPORT mode_t __wrap___umask_chk(mode_t mask);
ARC_EXPORT int __wrap_unlink(const char* pathname);
ARC_EXPORT int __wrap_utime(const char* filename, const struct utimbuf* times);
ARC_EXPORT int __wrap_utimes(
    const char* filename,
    const struct timeval times[2]);

// Bionic's off_t is 32bit but bionic also provides 64 bit version of
// functions which take off64_t. We need to define the wrapper of
// the 64 bit versions as well.
ARC_EXPORT int __wrap_ftruncate(int fd, off_t length);
ARC_EXPORT off_t __wrap_lseek(int fd, off_t offset, int whence);
ARC_EXPORT int __wrap_truncate(const char* path, off_t length);

ARC_EXPORT int __wrap_ftruncate64(int fd, off64_t length);
ARC_EXPORT off64_t __wrap_lseek64(int fd, off64_t offset, int whence);
ARC_EXPORT ssize_t __wrap_pread(int fd, void* buf, size_t count, off_t offset);
ARC_EXPORT ssize_t __wrap_pwrite(
    int fd, const void* buf, size_t count, off_t offset);
ARC_EXPORT ssize_t __wrap_pread64(
    int fd, void* buf, size_t count, off64_t offset);
ARC_EXPORT ssize_t __wrap_pwrite64(
    int fd, const void* buf, size_t count, off64_t offset);
ARC_EXPORT int __wrap_truncate64(const char* path, off64_t length);

// sorted by function name.
ARC_EXPORT int __wrap_close(int fd);
ARC_EXPORT int __wrap_creat(const char* pathname, mode_t mode);
ARC_EXPORT int __wrap_fcntl(int fd, int cmd, ...);
ARC_EXPORT FILE* __wrap_fdopen(int fildes, const char* mode);
ARC_EXPORT int __wrap_fdatasync(int fd);
ARC_EXPORT int __wrap_flock(int fd, int operation);
ARC_EXPORT int __wrap_fsync(int fd);
ARC_EXPORT int __wrap_ioctl(int fd, int request, ...);
ARC_EXPORT int __wrap_madvise(void* addr, size_t length, int advice);
ARC_EXPORT void* __wrap_mmap(
    void* addr, size_t length, int prot, int flags, int fd, off_t offset);
ARC_EXPORT int __wrap_mprotect(const void* addr, size_t length, int prot);
ARC_EXPORT int __wrap_munmap(void* addr, size_t length);
ARC_EXPORT int __wrap_poll(struct pollfd* fds, nfds_t nfds, int timeout);
ARC_EXPORT ssize_t __wrap_read(int fd, void* buf, size_t count);
ARC_EXPORT ssize_t __wrap___read_chk(
    int fd, void* buf, size_t count, size_t buflen);
ARC_EXPORT ssize_t __wrap_readv(int fd, const struct iovec* iov, int iovcnt);
ARC_EXPORT ssize_t __wrap_write(int fd, const void* buf, size_t count);
ARC_EXPORT ssize_t __wrap_writev(int fd, const struct iovec* iov, int iovcnt);
}  // extern "C"

using posix_translation::VirtualFileSystem;

namespace {

// Counts the depth of __wrap_write() calls to avoid infinite loop back.
DEFINE_THREAD_LOCAL(int, g_wrap_write_nest_count);

// Helper function for converting from nacl_abi_stat to stat.
void NaClAbiStatToStat(struct nacl_abi_stat* nacl_stat, struct stat* st) {
  st->st_dev = nacl_stat->nacl_abi_st_dev;
  st->st_mode = nacl_stat->nacl_abi_st_mode;
  st->st_nlink = nacl_stat->nacl_abi_st_nlink;
  st->st_uid = nacl_stat->nacl_abi_st_uid;
  st->st_gid = nacl_stat->nacl_abi_st_gid;
  st->st_rdev = nacl_stat->nacl_abi_st_rdev;
  st->st_size = nacl_stat->nacl_abi_st_size;
  st->st_blksize = nacl_stat->nacl_abi_st_blksize;
  st->st_blocks = nacl_stat->nacl_abi_st_blocks;
  st->st_atime = nacl_stat->nacl_abi_st_atime;
  st->st_atime_nsec = 0;
  st->st_mtime = nacl_stat->nacl_abi_st_mtime;
  st->st_mtime_nsec = 0;
  st->st_ctime = nacl_stat->nacl_abi_st_ctime;
  st->st_ctime_nsec = 0;
  st->st_ino = nacl_stat->nacl_abi_st_ino;
}

// Helper function for converting from stat to nacl_abi_stat.
void StatToNaClAbiStat(struct stat* st, struct nacl_abi_stat* nacl_stat) {
  nacl_stat->nacl_abi_st_dev = st->st_dev;
  nacl_stat->nacl_abi_st_mode = st->st_mode;
  nacl_stat->nacl_abi_st_nlink = st->st_nlink;
  nacl_stat->nacl_abi_st_uid = st->st_uid;
  nacl_stat->nacl_abi_st_gid = st->st_gid;
  nacl_stat->nacl_abi_st_rdev = st->st_rdev;
  nacl_stat->nacl_abi_st_size = st->st_size;
  nacl_stat->nacl_abi_st_blksize = st->st_blksize;
  nacl_stat->nacl_abi_st_blocks = st->st_blocks;
  nacl_stat->nacl_abi_st_atime = st->st_atime;
  nacl_stat->nacl_abi_st_mtime = st->st_mtime;
  nacl_stat->nacl_abi_st_ctime = st->st_ctime;
  nacl_stat->nacl_abi_st_ino = st->st_ino;
}

// Helper function for stripping "/system/lib/" prefix from |path| if exists.
const char* StripSystemLibPrefix(const char* path) {
  const char kSystemLib[] = "/system/lib/";
  return !StartsWithASCII(path, kSystemLib, true) ?
      path : path + sizeof(kSystemLib) - 1;
}

}  // namespace

// sorted by function name.

int __wrap_access(const char* pathname, int mode) {
  ARC_STRACE_ENTER("access", "\"%s\", %s",
                   SAFE_CSTR(pathname),
                   arc::GetAccessModeStr(mode).c_str());
  int result = VirtualFileSystem::GetVirtualFileSystem()->access(
      pathname, mode);
  if (result == -1 && errno != ENOENT)
    ARC_STRACE_ALWAYS_WARN_FAILURE();
  ARC_STRACE_RETURN(result);
}

int __wrap_chdir(const char* path) {
  ARC_STRACE_ENTER("chdir", "\"%s\"", SAFE_CSTR(path));
  int result = VirtualFileSystem::GetVirtualFileSystem()->chdir(path);
  ARC_STRACE_RETURN(result);
}

int __wrap_chown(const char* path, uid_t owner, gid_t group) {
  ARC_STRACE_ENTER("chown", "\"%s\", %u, %u", SAFE_CSTR(path), owner, group);
  int result = VirtualFileSystem::GetVirtualFileSystem()->chown(
      path, owner, group);
  ARC_STRACE_RETURN(result);
}

// Wrap this just for ARC strace.
int __wrap_closedir(DIR* dirp) {
  ARC_STRACE_ENTER("closedir", "%d, %p", PRETIFY_DIRP(dirp));
  int result = closedir(dirp);
  ARC_STRACE_RETURN(result);
}

// Wrap this just for ARC strace.
int __wrap_dirfd(DIR* dirp) {
  ARC_STRACE_ENTER("dirfd", "%p", dirp);
  int result = dirfd(dirp);
  ARC_STRACE_RETURN(result);
}

// Wrap this just for ARC strace.
int __wrap_dladdr(const void* addr, Dl_info* info) {
  ARC_STRACE_ENTER("dladdr", "%p, %p", addr, info);
  const int result = dladdr(addr, info);
  if (result && info) {  // dladdr returns 0 on error.
    ARC_STRACE_REPORT(
        "info={dli_fname=\"%s\" dli_fbase=%p dli_sname=\"%s\" dli_saddr=%p}",
        SAFE_CSTR(info->dli_fname), info->dli_fbase,
        SAFE_CSTR(info->dli_sname), info->dli_saddr);
  }
  // false since dladdr never sets errno.
  ARC_STRACE_RETURN_INT(result, false);
}

// Wrap this just for ARC strace.
int __wrap_dlclose(void* handle) {
  ARC_STRACE_ENTER("dlclose", "%p \"%s\"",
                   handle, arc::GetDlsymHandleStr(handle).c_str());
  // Remove the handle from ARC_STRACE first. See crbug.com/461155.
  ARC_STRACE_UNREGISTER_DSO_HANDLE(handle);
  int result = dlclose(handle);
  // false since dlclose never sets errno.
  ARC_STRACE_RETURN_INT(result, false);
}

void* __wrap_dlopen(const char* filename, int flag) {
  ARC_STRACE_ENTER("dlopen", "\"%s\", %s",
                   SAFE_CSTR(filename),
                   arc::GetDlopenFlagStr(flag).c_str());
  // dlopen is implemented on top of the open_resource IRT which can be
  // very slow e.g. when either renderer or browser process is busy.
  TRACE_EVENT2(ARC_TRACE_CATEGORY, "wrap_dlopen",
               "filename", TRACE_STR_COPY(SAFE_CSTR(filename)),
               "flag", flag);
  if (filename && (
      (filename[0] != '/' && arc::IsStaticallyLinkedSharedObject(filename)) ||
      (filename[0] == '/' && arc::IsStaticallyLinkedSharedObject(
          StripSystemLibPrefix(filename))))) {
    // ARC statically links some libraries into the main
    // binary. When an app dlopen such library, we should return the
    // handle of the main binary so that apps can find symbols.
    // TODO(crbug.com/400947): Remove this temporary hack once we have stopped
    //                         converting shared objects to archives.
    filename = NULL;
  }
  void* result = dlopen(filename, flag);
  if (result)
    ARC_STRACE_REGISTER_DSO_HANDLE(result, filename);

  // false since dlopen never sets errno.
  ARC_STRACE_RETURN_PTR(result, false);
}

// Wrap this just for ARC strace.
void* __wrap_dlsym(void* handle, const char* symbol) {
  ARC_STRACE_ENTER("dlsym", "%p \"%s\", \"%s\"",
                   handle,
                   arc::GetDlsymHandleStr(handle).c_str(),
                   SAFE_CSTR(symbol));
  // We should not simply call dlsym here because it looks at the return address
  // to determine the library lookup chain when |handle| is RTLD_NEXT.
  // TODO(crbug.com/444500): Add an integration test for this. Note that
  // we can't use posix_translation integration test because dlsym is handled
  // differently under NDK translation.
  void* result = __dlsym_with_return_address(
      handle, symbol, __builtin_return_address(0));
  // false since dlsym never sets errno.
  ARC_STRACE_RETURN_PTR(result, false);
}

// Wrap this just for ARC strace.
DIR* __wrap_fdopendir(int fd) {
  ARC_STRACE_ENTER_FD("fdopendir", "%d", fd);
  DIR* dirp = fdopendir(fd);
  ARC_STRACE_RETURN_PTR(dirp, !dirp);
}

int __wrap_fstatfs(int fd, struct statfs* buf) {
  ARC_STRACE_ENTER_FD("fstatfs", "%d, %p", fd, buf);
  int result = VirtualFileSystem::GetVirtualFileSystem()->fstatfs(fd, buf);
  ARC_STRACE_RETURN(result);
}

long __wrap_fpathconf(int fd, int name) {  // NOLINT
  // TODO(halyavin): print a user-friendly |name| description.
  ARC_STRACE_ENTER_FD("fpathconf", "%d, %d", fd, name);
  int old_errno = errno;
  errno = 0;
  int result = VirtualFileSystem::GetVirtualFileSystem()->fpathconf(fd, name);
  if (errno != 0) {
    ARC_STRACE_RETURN_INT(result, true);
  }
  errno = old_errno;
  ARC_STRACE_RETURN_INT(result, false);
}

char* __wrap_getcwd(char* buf, size_t size) {
  ARC_STRACE_ENTER("getcwd", "%p, %zu", buf, size);
  char* result = VirtualFileSystem::GetVirtualFileSystem()->getcwd(buf, size);
  ARC_STRACE_REPORT("result=\"%s\"", SAFE_CSTR(result));
  ARC_STRACE_RETURN_PTR(result, false);
}

IRT_WRAPPER(getdents, int fd, struct dirent* dirp, size_t count,
            size_t* nread) {
  // We intentionally use Bionic's dirent instead of NaCl's. See
  // bionic/libc/arch-nacl/syscalls/getdents.c for detail.
  ARC_STRACE_ENTER_FD("getdents", "%d, %p, %u, %p", fd, dirp, count, nread);
  int result = VirtualFileSystem::GetVirtualFileSystem()->getdents(
      fd, dirp, count);
  if (result >= 0) {
    *nread = result;
    ARC_STRACE_REPORT("nread=\"%zu\"", *nread);
  }
  ARC_STRACE_RETURN_IRT_WRAPPER(result >= 0 ? 0 : errno);
}

IRT_WRAPPER(getcwd, char* buf, size_t size) {
  return __wrap_getcwd(buf, size) ? 0 : errno;
}

IRT_WRAPPER(lstat, const char* path, struct nacl_abi_stat* buf) {
  ARC_STRACE_ENTER("lstat", "\"%s\", %p",
                   SAFE_CSTR(path), buf);
  struct stat st;
  int result = VirtualFileSystem::GetVirtualFileSystem()->lstat(path, &st);
  if (result == -1) {
    if (errno != ENOENT)
      ARC_STRACE_ALWAYS_WARN_FAILURE();
  } else {
    StatToNaClAbiStat(&st, buf);
    ARC_STRACE_REPORT("buf=%s", arc::GetNaClAbiStatStr(buf).c_str());
  }
  ARC_STRACE_RETURN_IRT_WRAPPER(result == 0 ? 0 : errno);
}

IRT_WRAPPER(mkdir, const char* pathname, mode_t mode) {
  ARC_STRACE_ENTER("mkdir", "\"%s\", 0%o", SAFE_CSTR(pathname), mode);
  int result = VirtualFileSystem::GetVirtualFileSystem()->mkdir(pathname, mode);
  if (result == -1 && errno != EEXIST)
    ARC_STRACE_ALWAYS_WARN_FAILURE();
  ARC_STRACE_RETURN_IRT_WRAPPER(result == 0 ? 0 : errno);
}

int __wrap_open(const char* pathname, int flags, ...) {
  va_list argp;
  va_start(argp, flags);
  mode_t mode = 0;
  if (flags & O_CREAT) {
    // Passing mode_t to va_arg with bionic makes compile fail.
    // As bionic's mode_t is short, the value is promoted when it was
    // passed to this vaarg function and fetching it as a short value
    // is not valid. This definition can be bad if mode_t is a 64bit
    // value, but such environment might not exist.
    COMPILE_ASSERT(sizeof(mode) <= sizeof(int), mode_t_is_too_big);  // NOLINT
    mode = va_arg(argp, int);
  }
  va_end(argp);

  ARC_STRACE_ENTER("open", "\"%s\", %s, 0%o",
                   SAFE_CSTR(pathname),
                   arc::GetOpenFlagStr(flags).c_str(), mode);
  int fd;
  if (arc::IsStaticallyLinkedSharedObject(StripSystemLibPrefix(pathname))) {
    // CtsSecurityTest verifies some libraries are ELF format. To pass that
    // check, returns FD of runnable-ld.so instead.
    // TODO(crbug.com/400947): Remove this temporary hack once we have stopped
    //                         converting shared objects to archives.
    ALOGE("open is called for %s. Opening runnable-ld.so instead.", pathname);
    fd = VirtualFileSystem::GetVirtualFileSystem()->open(
        "/system/lib/runnable-ld.so", flags, mode);
  } else {
    fd = VirtualFileSystem::GetVirtualFileSystem()->open(pathname, flags, mode);
  }
  if (fd == -1 && errno != ENOENT)
    ARC_STRACE_ALWAYS_WARN_FAILURE();
  ARC_STRACE_REGISTER_FD(fd, SAFE_CSTR(pathname));
  ARC_STRACE_RETURN(fd);
}

// Wrap this just for ARC strace.
DIR* __wrap_opendir(const char* name) {
  ARC_STRACE_ENTER("opendir", "%s", SAFE_CSTR(name));
  DIR* dirp = opendir(name);
  ARC_STRACE_RETURN_PTR(dirp, !dirp);
}

// Wrap this just for ARC strace.
struct dirent* __wrap_readdir(DIR* dirp) {
  ARC_STRACE_ENTER_FD("readdir", "%d, %p", PRETIFY_DIRP(dirp));
  struct dirent* ent = readdir(dirp);  // NOLINT(runtime/threadsafe_fn)
  ARC_STRACE_RETURN_PTR(ent, false);
}

// Wrap this just for ARC strace.
int __wrap_readdir_r(DIR* dirp, struct dirent* entry, struct dirent** ents) {
  ARC_STRACE_ENTER_FD("readdir_r", "%d, %p, %p, %p",
                      PRETIFY_DIRP(dirp), entry, ents);
  int result = readdir_r(dirp, entry, ents);
  ARC_STRACE_RETURN(result);
}

ssize_t __wrap_readlink(const char* path, char* buf, size_t bufsiz) {
  ARC_STRACE_ENTER("readlink", "\"%s\", %p, %zu",
                   SAFE_CSTR(path), buf, bufsiz);
  ssize_t result = VirtualFileSystem::GetVirtualFileSystem()->readlink(
      path, buf, bufsiz);
  if (result == -1)
    ARC_STRACE_ALWAYS_WARN_FAILURE();
  ARC_STRACE_RETURN(result);
}

char* __wrap_realpath(const char* path, char* resolved_path) {
  ARC_STRACE_ENTER("realpath", "\"%s\", %p", SAFE_CSTR(path), resolved_path);
  char* result = VirtualFileSystem::GetVirtualFileSystem()->realpath(
      path, resolved_path);
  if (!result)
    ARC_STRACE_ALWAYS_WARN_FAILURE();
  ARC_STRACE_RETURN_PTR(result, !result);
}

int __wrap_remove(const char* pathname) {
  ARC_STRACE_ENTER("remove", "\"%s\"", SAFE_CSTR(pathname));
  int result = VirtualFileSystem::GetVirtualFileSystem()->remove(pathname);
  if (result == -1 && errno != ENOENT)
    ARC_STRACE_ALWAYS_WARN_FAILURE();
  ARC_STRACE_RETURN(result);
}

int __wrap_rename(const char* oldpath, const char* newpath) {
  ARC_STRACE_ENTER("rename", "\"%s\", \"%s\"",
                   SAFE_CSTR(oldpath), SAFE_CSTR(newpath));
  int result = VirtualFileSystem::GetVirtualFileSystem()->rename(
      oldpath, newpath);
  if (result == -1)
    ARC_STRACE_ALWAYS_WARN_FAILURE();
  ARC_STRACE_RETURN(result);
}

// Wrap this just for ARC strace.
void __wrap_rewinddir(DIR* dirp) {
  ARC_STRACE_ENTER_FD("rewinddir", "%d, %p", PRETIFY_DIRP(dirp));
  rewinddir(dirp);
  ARC_STRACE_RETURN_VOID();
}

// Wrap this just for ARC strace.
int __wrap_scandir(
    const char* dirp, struct dirent*** namelist,
    int (*filter)(const struct dirent*),
    int (*compar)(const struct dirent**, const struct dirent**)) {
  ARC_STRACE_ENTER("scandir", "%s, %p, %p, %p",
                   SAFE_CSTR(dirp), namelist, filter, compar);
  int result = scandir(dirp, namelist, filter, compar);
  ARC_STRACE_RETURN(result);
}

int __wrap_statfs(const char* pathname, struct statfs* stat) {
  ARC_STRACE_ENTER("statfs", "\"%s\", %p", SAFE_CSTR(pathname), stat);
  int result = VirtualFileSystem::GetVirtualFileSystem()->statfs(
      pathname, stat);
  if (result == -1 && errno != ENOENT)
    ARC_STRACE_ALWAYS_WARN_FAILURE();
  ARC_STRACE_REPORT(
      "stat={type=%lld bsize=%lld blocks=%llu bfree=%llu bavail=%llu "
      "files=%llu ffree=%llu fsid=%d,%d namelen=%lld frsize=%lld "
      // Note: Unlike glibc and older Bionic, f_spare[] in Bionic 4.4 has
      // only 4 elements, not 5.
      "spare=%lld,%lld,%lld,%lld}",
      static_cast<int64_t>(stat->f_type),
      static_cast<int64_t>(stat->f_bsize),
      stat->f_blocks, stat->f_bfree,
      stat->f_bavail, stat->f_files, stat->f_ffree,
      stat->f_fsid.__val[0], stat->f_fsid.__val[1],
      static_cast<int64_t>(stat->f_namelen),
      static_cast<int64_t>(stat->f_frsize),
      static_cast<int64_t>(stat->f_spare[0]),
      static_cast<int64_t>(stat->f_spare[1]),
      static_cast<int64_t>(stat->f_spare[2]),
      static_cast<int64_t>(stat->f_spare[3]));
  ARC_STRACE_RETURN(result);
}

int __wrap_statvfs(const char* pathname, struct statvfs* stat) {
  ARC_STRACE_ENTER("statvfs", "\"%s\", %p", SAFE_CSTR(pathname), stat);
  int result = VirtualFileSystem::GetVirtualFileSystem()->statvfs(
      pathname, stat);
  ARC_STRACE_REPORT(
      "stat={bsize=%llu frsize=%llu blocks=%llu bfree=%llu bavail=%llu "
      "files=%llu ffree=%llu favail=%llu fsid=%llu flag=%llu namemax=%llu}",
      static_cast<int64_t>(stat->f_bsize),
      static_cast<int64_t>(stat->f_frsize),
      static_cast<int64_t>(stat->f_blocks),
      static_cast<int64_t>(stat->f_bfree),
      static_cast<int64_t>(stat->f_bavail),
      static_cast<int64_t>(stat->f_files),
      static_cast<int64_t>(stat->f_ffree),
      static_cast<int64_t>(stat->f_favail),
      static_cast<int64_t>(stat->f_fsid),
      static_cast<int64_t>(stat->f_flag),
      static_cast<int64_t>(stat->f_namemax));
  ARC_STRACE_RETURN(result);
}

int __wrap_symlink(const char* oldp, const char* newp) {
  ARC_STRACE_ENTER("symlink", "\"%s\", \"%s\"",
                   SAFE_CSTR(oldp), SAFE_CSTR(newp));
  int result = VirtualFileSystem::GetVirtualFileSystem()->symlink(oldp, newp);
  if (!result)
    ALOGE("Added a non-persistent symlink from %s to %s", newp, oldp);
  ARC_STRACE_RETURN(result);
}

template <typename OffsetType>
static int TruncateImpl(const char* pathname, OffsetType length) {
  ARC_STRACE_ENTER("truncate", "\"%s\", %lld",
                   SAFE_CSTR(pathname), static_cast<int64_t>(length));
  int result = VirtualFileSystem::GetVirtualFileSystem()->truncate(
      pathname, length);
  if (result == -1)
    ARC_STRACE_ALWAYS_WARN_FAILURE();
  ARC_STRACE_RETURN(result);
}

int __wrap_truncate(const char* pathname, off_t length) {
  return TruncateImpl(pathname, length);
}

int __wrap_truncate64(const char* pathname, off64_t length) {
  return TruncateImpl(pathname, length);
}

IRT_WRAPPER(unlink, const char* pathname) {
  ARC_STRACE_ENTER("unlink", "\"%s\"", SAFE_CSTR(pathname));
  int result = VirtualFileSystem::GetVirtualFileSystem()->unlink(pathname);
  if (result == -1 && errno != ENOENT)
    ARC_STRACE_ALWAYS_WARN_FAILURE();
  ARC_STRACE_RETURN_IRT_WRAPPER(result == 0 ? 0 : errno);
}

int __wrap_utimes(const char* filename, const struct timeval times[2]) {
  ARC_STRACE_ENTER("utimes", "\"%s\", %p", SAFE_CSTR(filename), times);
  int result = VirtualFileSystem::GetVirtualFileSystem()->utimes(
      filename, times);
  if (result == -1 && errno != ENOENT)
    ARC_STRACE_ALWAYS_WARN_FAILURE();
  ARC_STRACE_RETURN(result);
}

IRT_WRAPPER(stat, const char* pathname, struct nacl_abi_stat* buf) {
  ARC_STRACE_ENTER("stat", "\"%s\", %p", SAFE_CSTR(pathname), buf);
  struct stat st;
  int result = VirtualFileSystem::GetVirtualFileSystem()->stat(pathname, &st);
  if (result == -1) {
    if (errno != ENOENT)
      ARC_STRACE_ALWAYS_WARN_FAILURE();
  } else {
    StatToNaClAbiStat(&st, buf);
    ARC_STRACE_REPORT("buf=%s", arc::GetNaClAbiStatStr(buf).c_str());
  }
  ARC_STRACE_RETURN_IRT_WRAPPER(result == 0 ? 0 : errno);
}

int __wrap_close(int fd) {
  ARC_STRACE_ENTER_FD("close", "%d", fd);
  // Remove the descriptor from ARC_STRACE first. See crbug.com/461155.
  if (fd >= 0)
    ARC_STRACE_UNREGISTER_FD(fd);
  int result = VirtualFileSystem::GetVirtualFileSystem()->close(fd);
  if (result == -1) {
    // Closing with a bad file descriptor may be indicating a double
    // close, which is more dangerous than it seems since everything
    // shares one address space and we reuse file descriptors quickly.
    // It can cause a newly allocated file descriptor in another
    // thread to now be unallocated.
    // We just use DANGERF() instead of LOG_FATAL_IF() because
    // cts.CtsNetTestCases:android.net.rtp.cts.AudioStreamTest#testDoubleRelease
    // hits the case.
    if (errno == EBADF)
      DANGERF("Close of bad file descriptor may indicate double close");
    ARC_STRACE_ALWAYS_WARN_FAILURE();
  }
  ARC_STRACE_RETURN(result);
}

int __wrap_creat(const char* pathname, mode_t mode) {
  ARC_STRACE_ENTER("creat", "\"%s\", 0%o", SAFE_CSTR(pathname), mode);
  int result = VirtualFileSystem::GetVirtualFileSystem()->open(
      pathname, O_CREAT | O_WRONLY | O_TRUNC, mode);
  ARC_STRACE_REGISTER_FD(result, SAFE_CSTR(pathname));
  ARC_STRACE_RETURN(result);
}

IRT_WRAPPER(dup, int oldfd, int* newfd) {
  ARC_STRACE_ENTER_FD("dup", "%d", oldfd);
  int fd = VirtualFileSystem::GetVirtualFileSystem()->dup(oldfd);
  if (fd == -1)
    ARC_STRACE_ALWAYS_WARN_FAILURE();
  *newfd = fd;
  ARC_STRACE_RETURN_IRT_WRAPPER(fd >= 0 ? 0 : errno);
}

IRT_WRAPPER(dup2, int oldfd, int newfd) {
  ARC_STRACE_ENTER_FD("dup2", "%d, %d", oldfd, newfd);
  int fd = VirtualFileSystem::GetVirtualFileSystem()->dup2(oldfd, newfd);
  if (fd == -1)
    ARC_STRACE_ALWAYS_WARN_FAILURE();
  ARC_STRACE_RETURN_IRT_WRAPPER(fd >= 0 ? 0 : errno);
}

// Although Linux has fcntl64 syscall, user code does not use it directly.
// Therefore, we do not have to wrap the 64bit variant.
int __wrap_fcntl(int fd, int cmd, ...) {
  // TODO(crbug.com/241955): Support variable args?
  ARC_STRACE_ENTER_FD("fcntl", "%d, %s, ...",
                      fd, arc::GetFcntlCommandStr(cmd).c_str());

  va_list ap;
  va_start(ap, cmd);
  int result = VirtualFileSystem::GetVirtualFileSystem()->fcntl(fd, cmd, ap);
  va_end(ap);

  if (result == -1)
    ARC_STRACE_ALWAYS_WARN_FAILURE();
  ARC_STRACE_RETURN(result);
}

int __wrap_fdatasync(int fd) {
  ARC_STRACE_ENTER_FD("fdatasync", "%d", fd);
  int result = VirtualFileSystem::GetVirtualFileSystem()->fdatasync(fd);
  ARC_STRACE_RETURN(result);
}

int __wrap_fsync(int fd) {
  ARC_STRACE_ENTER_FD("fsync", "%d", fd);
  int result = VirtualFileSystem::GetVirtualFileSystem()->fsync(fd);
  ARC_STRACE_RETURN(result);
}

IRT_WRAPPER(fstat, int fd, struct nacl_abi_stat* buf) {
  ARC_STRACE_ENTER_FD("fstat", "%d, %p", fd, buf);
  struct stat st;
  int result = VirtualFileSystem::GetVirtualFileSystem()->fstat(fd, &st);
  if (result) {
    result = errno;
    ARC_STRACE_ALWAYS_WARN_FAILURE();
  } else {
    StatToNaClAbiStat(&st, buf);
    ARC_STRACE_REPORT("buf=%s", arc::GetNaClAbiStatStr(buf).c_str());
  }
  ARC_STRACE_RETURN_IRT_WRAPPER(result);
}

template <typename OffsetType>
static int FtruncateImpl(int fd, OffsetType length) {
  ARC_STRACE_ENTER_FD("ftruncate", "%d, %lld",
                      fd, static_cast<int64_t>(length));
  int result = VirtualFileSystem::GetVirtualFileSystem()->ftruncate(fd, length);
  if (result == -1)
    ARC_STRACE_ALWAYS_WARN_FAILURE();
  ARC_STRACE_RETURN(result);
}

int __wrap_ftruncate(int fd, off_t length) {
  return FtruncateImpl(fd, length);
}

int __wrap_ftruncate64(int fd, off64_t length) {
  return FtruncateImpl(fd, length);
}

int __wrap_ioctl(int fd, int request, ...) {
  // TODO(crbug.com/241955): Pretty-print variable args?
  ARC_STRACE_ENTER_FD("ioctl", "%d, %s, ...",
                      fd, arc::GetIoctlRequestStr(request).c_str());
  va_list ap;
  va_start(ap, request);
  int result = VirtualFileSystem::GetVirtualFileSystem()->ioctl(
      fd, request, ap);
  va_end(ap);
  if (result == -1)
    ARC_STRACE_ALWAYS_WARN_FAILURE();
  ARC_STRACE_RETURN(result);
}

template <typename OffsetType>
static OffsetType LseekImpl(int fd, OffsetType offset, int whence) {
  ARC_STRACE_ENTER_FD("lseek", "%d, %lld, %s",
                      fd, static_cast<int64_t>(offset),
                      arc::GetLseekWhenceStr(whence).c_str());
  OffsetType result = VirtualFileSystem::GetVirtualFileSystem()->lseek(
      fd, offset, whence);
  if (result == -1)
    ARC_STRACE_ALWAYS_WARN_FAILURE();
  ARC_STRACE_RETURN(result);
}

off64_t __wrap_lseek64(int fd, off64_t offset, int whence) {
  return LseekImpl(fd, offset, whence);
}

int __wrap_madvise(void* addr, size_t length, int advice) {
  ARC_STRACE_ENTER("madvise", "%p, %zu, %s", addr, length,
                   arc::GetMadviseAdviceStr(advice).c_str());
  int saved_errno = errno;
  int result = VirtualFileSystem::GetVirtualFileSystem()->madvise(
      addr, length, advice);
  if (result != 0) {
    ARC_STRACE_ALWAYS_WARN_FAILURE();
    if (errno == ENOSYS && advice != MADV_REMOVE) {
      // TODO(crbug.com/362862): Stop special-casing ENOSYS once the bug is
      // fixed.
      // Note: We should call mprotect IRT here once the IRT is supported and
      // crbug.com/36282 is still open.
      errno = saved_errno;
      result = 0;
    }
  }
  ARC_STRACE_RETURN(result);
}

// NB: Do NOT use off64_t for |offset|. It is not compatible with Bionic.
// Bionic's mmap() does not support large file, and it does not provide
// mmap64() either.
void* __wrap_mmap(
    void* addr, size_t length, int prot, int flags, int fd, off_t offset) {
  ARC_STRACE_ENTER("mmap", "%p, %zu(0x%zx), %s, %s, %d \"%s\", 0x%llx",
                   addr, length, length,
                   arc::GetMmapProtStr(prot).c_str(),
                   arc::GetMmapFlagStr(flags).c_str(),
                   fd, arc::GetFdStr(fd).c_str(),
                   static_cast<int64_t>(offset));
  // WRITE + EXEC mmap is not allowed.
  if ((prot & PROT_WRITE) && (prot & PROT_EXEC)) {
    ALOGE("mmap with PROT_WRITE + PROT_EXEC! "
          "addr=%p length=%zu prot=%d flags=%d fd=%d offset=%lld",
          addr, length, prot, flags, fd, static_cast<int64_t>(offset));
    // However, with Bare Metal, our JIT engines or NDK apps may want WX mmap.
#if defined(__native_client__)
    ALOG_ASSERT(false, "PROT_WRITE + PROT_EXEC mmap is not allowed");
    // This mmap call gracefully fails in release build.
#endif
  } else if (prot & PROT_EXEC) {
    // There are two reasons we will see PROT_EXEC:
    // - The Bionic loader use PROT_EXEC to map dlopen-ed files. Note
    //   that we inject posix_translation based file operations to the
    //   Bionic loader. See src/common/dlfcn_injection.cc for detail.
    // - On Bare Metal ARM, v8 uses PROT_EXEC to run JIT-ed code directly.
    //
    // But it is still an interesting event. So, we log this by ALOGI.
    ALOGI("mmap with PROT_EXEC! "
          "addr=%p length=%zu prot=%d flags=%d fd=%d offset=%lld",
          addr, length, prot, flags, fd, static_cast<int64_t>(offset));
  }

  if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC))
    ALOGE("mmap with an unorthodox prot: %d", prot);
  // We do not support MAP_NORESERVE but this flag is used often and
  // we can safely ignore it.
  const int supported_flag = (MAP_SHARED | MAP_PRIVATE | MAP_FIXED |
                              MAP_ANONYMOUS | MAP_NORESERVE);
  if (flags & ~supported_flag)
    ALOGE("mmap with an unorthodox flags: %d", flags);

  void* result = VirtualFileSystem::GetVirtualFileSystem()->mmap(
      addr, length, prot, flags, fd, offset);
#if defined(USE_VERBOSE_MEMORY_VIEWER)
  if (result != MAP_FAILED)
    arc::MemoryMappingBacktraceMap::GetInstance()->
        MapCurrentStackFrame(result, length);
#endif

  if (result == MAP_FAILED)
    ARC_STRACE_ALWAYS_WARN_FAILURE();
  ARC_STRACE_RETURN_PTR(result, result == MAP_FAILED);
}

int __wrap_mprotect(const void* addr, size_t len, int prot) {
  ARC_STRACE_ENTER("mprotect", "%p, %zu(0x%zx), %s", addr, len, len,
                   arc::GetMmapProtStr(prot).c_str());
#if defined(__native_client__)
  // PROT_EXEC mprotect is not allowed on NaCl, where all executable
  // pages are validated through special APIs.
  if (prot & PROT_EXEC) {
    ALOGE("mprotect with PROT_EXEC! addr=%p length=%zu prot=%d",
          addr, len, prot);
    ALOG_ASSERT(false, "mprotect with PROT_EXEC is not allowed");
    // This mmap call gracefully fails in release build.
  }
#else
  if ((prot & PROT_WRITE) && (prot & PROT_EXEC)) {
    // TODO(crbug.com/365349): Currently, it seems Dalvik JIT is
    // enabled on Bare Metal ARM. Disable it and increase the
    // verbosity of this ALOG.
    ALOGV("mprotect with PROT_WRITE + PROT_EXEC! addr=%p length=%zu prot=%d",
          addr, len, prot);
  }
#endif

  const int errno_orig = errno;
  // mprotect in Bionic defines the first argument is const void*, but
  // POSIX does it as void*. We use const void* for wrap, and use void* for
  // posix_translation.
  int result = VirtualFileSystem::GetVirtualFileSystem()->mprotect(
      const_cast<void*>(addr), len, prot);
  if (result != 0 && errno == ENOSYS) {
    // TODO(crbug.com/362862): Stop falling back to real mprotect on ENOSYS and
    // do this only for unit tests.
    ARC_STRACE_REPORT("falling back to real mprotect");
    result = mprotect(addr, len, prot);
    if (!result && errno == ENOSYS)
      errno = errno_orig;  // restore |errno| overwritten by posix_translation
  }
  ARC_STRACE_RETURN(result);
}

int __wrap_munmap(void* addr, size_t length) {
  ARC_STRACE_ENTER("munmap", "%p, %zu(0x%zx)", addr, length, length);
  ARC_STRACE_REPORT("RANGE (%p-%p)",
                    addr, reinterpret_cast<char*>(addr) + length);
  const int errno_orig = errno;
  int result = VirtualFileSystem::GetVirtualFileSystem()->munmap(addr, length);
  if (result != 0 && errno == ENOSYS) {
    // TODO(crbug.com/362862): Stop falling back to real munmap on ENOSYS and
    // do this only for unit tests.
    ARC_STRACE_REPORT("falling back to real munmap");
    result = munmap(addr, length);
    if (!result && errno == ENOSYS)
      errno = errno_orig;  // restore |errno| overwritten by posix_translation
  }
#if defined(USE_VERBOSE_MEMORY_VIEWER)
  if (result == 0)
    arc::MemoryMappingBacktraceMap::GetInstance()->Unmap(addr, length);
#endif
  ARC_STRACE_RETURN(result);
}

long __wrap_pathconf(const char* path, int name) {  // NOLINT
  // TODO(halyavin): print a user-friendly |name| description.
  ARC_STRACE_ENTER("pathconf", "\"%s\", %d", SAFE_CSTR(path), name);
  int old_errno = errno;
  errno = 0;
  int result = VirtualFileSystem::GetVirtualFileSystem()->pathconf(path, name);
  if (errno != 0) {
    ARC_STRACE_RETURN_INT(result, true);
  }
  errno = old_errno;
  ARC_STRACE_RETURN_INT(result, false);
}

int __wrap_poll(struct pollfd* fds, nfds_t nfds, int timeout) {
  ARC_STRACE_ENTER("poll", "%p, %lld, %d",
                   fds, static_cast<int64_t>(nfds), timeout);
  if (arc::StraceEnabled()) {
    for (nfds_t i = 0; i < nfds; ++i) {
      ARC_STRACE_REPORT("polling fd %d \"%s\" for %s",
                        fds[i].fd, arc::GetFdStr(fds[i].fd).c_str(),
                        arc::GetPollEventStr(fds[i].events).c_str());
    }
  }
  int result = VirtualFileSystem::GetVirtualFileSystem()->poll(
      fds, nfds, timeout);
  if (result == -1) {
    ARC_STRACE_ALWAYS_WARN_FAILURE();
  } else if (arc::StraceEnabled()) {
    for (int i = 0; i < result; ++i) {
      if (!fds[i].revents)
        continue;
      ARC_STRACE_REPORT("fd %d \"%s\" is ready for %s",
                        fds[i].fd, arc::GetFdStr(fds[i].fd).c_str(),
                        arc::GetPollEventStr(fds[i].revents).c_str());
    }
  }
  ARC_STRACE_RETURN(result);
}

IRT_WRAPPER(poll, struct pollfd* fds, nfds_t nfds, int timeout, int* count) {
  *count = __wrap_poll(fds, nfds, timeout);
  return *count >= 0 ? 0 : errno;
}

template <typename OffsetType>
static ssize_t PreadImpl(int fd, void* buf, size_t count, OffsetType offset) {
  ARC_STRACE_ENTER_FD("pread", "%d, %p, %zu, %lld",
                      fd, buf, count, static_cast<int64_t>(offset));
  ssize_t result = VirtualFileSystem::GetVirtualFileSystem()->pread(
      fd, buf, count, offset);
  if (result == -1)
    ARC_STRACE_ALWAYS_WARN_FAILURE();
  if (result >= 0)
    ARC_STRACE_REPORT("buf=%s", arc::GetRWBufStr(buf, result).c_str());
  ARC_STRACE_RETURN(result);
}

ssize_t __wrap_pread(int fd, void* buf, size_t count, off_t offset) {
  return PreadImpl(fd, buf, count, offset);
}

ssize_t __wrap_pread64(int fd, void* buf, size_t count, off64_t offset) {
  return PreadImpl(fd, buf, count, offset);
}

template <typename OffsetType>
static ssize_t PwriteImpl(int fd, const void* buf, size_t count,
                          OffsetType offset) {
  ARC_STRACE_ENTER_FD("pwrite", "%d, %p, %zu, %lld",
                      fd, buf, count, static_cast<int64_t>(offset));
  ssize_t result = VirtualFileSystem::GetVirtualFileSystem()->pwrite(
      fd, buf, count, offset);
  if (result == -1)
    ARC_STRACE_ALWAYS_WARN_FAILURE();
  if (errno != EFAULT)
    ARC_STRACE_REPORT("buf=%s", arc::GetRWBufStr(buf, count).c_str());
  ARC_STRACE_RETURN(result);
}

ssize_t __wrap_pwrite(int fd, const void* buf, size_t count, off_t offset) {
  return PwriteImpl(fd, buf, count, offset);
}

ssize_t __wrap_pwrite64(int fd, const void* buf, size_t count,
                        off64_t offset) {
  return PwriteImpl(fd, buf, count, offset);
}

ssize_t __wrap_read(int fd, void* buf, size_t count) {
  ARC_STRACE_ENTER_FD("read", "%d, %p, %zu", fd, buf, count);
  ssize_t result = VirtualFileSystem::GetVirtualFileSystem()->read(
      fd, buf, count);
  if (result == -1 && errno != EAGAIN)
    ARC_STRACE_ALWAYS_WARN_FAILURE();
  if (result >= 0)
    ARC_STRACE_REPORT("buf=%s", arc::GetRWBufStr(buf, result).c_str());
  ARC_STRACE_RETURN(result);
}

ssize_t __wrap___read_chk(int fd, void* buf, size_t count, size_t buflen) {
  // TODO(crbug.com/478000): Wrap __read_chk accurately?
  return __wrap_read(fd, buf, count);
}

ssize_t __wrap_readv(int fd, const struct iovec* iov, int iovcnt) {
  // TODO(crbug.com/241955): Stringify |iov|?
  ARC_STRACE_ENTER_FD("readv", "%d, %p, %d", fd, iov, iovcnt);
  ssize_t result = VirtualFileSystem::GetVirtualFileSystem()->readv(
      fd, iov, iovcnt);
  if (result == -1)
    ARC_STRACE_ALWAYS_WARN_FAILURE();
  ARC_STRACE_RETURN(result);
}

int __wrap_rmdir(const char* pathname) {
  ARC_STRACE_ENTER("rmdir", "\"%s\"", SAFE_CSTR(pathname));
  int result = VirtualFileSystem::GetVirtualFileSystem()->rmdir(pathname);
  if (result == -1 && errno != ENOENT)
    ARC_STRACE_ALWAYS_WARN_FAILURE();
  ARC_STRACE_RETURN(result);
}

int __wrap_utime(const char* filename, const struct utimbuf* times) {
  ARC_STRACE_ENTER("utime", "\"%s\", %p", SAFE_CSTR(filename), times);
  int result = VirtualFileSystem::GetVirtualFileSystem()->utime(
      filename, times);
  if (result == -1 && errno != ENOENT)
    ARC_STRACE_ALWAYS_WARN_FAILURE();
  ARC_STRACE_RETURN(result);
}

ssize_t __wrap_write(int fd, const void* buf, size_t count) {
  const int wrap_write_nest_count = g_wrap_write_nest_count.Get();
  if (wrap_write_nest_count) {
    // Calling write() to a stdio descriptor inside __wrap_write may cause
    // infinite wrap loop. Here, we show a warning, and just return.
    // It may happen when a chromium base DCHECK fails, e.g. inside AutoLock.
    ALOGE("write() for stdio is called inside __wrap_write(): "
          "fd=%d count=%zu buf=%p msg='%s'",
          fd, count, buf,
          std::string(static_cast<const char*>(buf), count).c_str());
    return 0;
  } else {
    ARC_STRACE_ENTER_FD("write", "%d, %p, %zu", fd, buf, count);
    g_wrap_write_nest_count.Set(wrap_write_nest_count + 1);
    int result = VirtualFileSystem::GetVirtualFileSystem()->write(
        fd, buf, count);
    if (errno != EFAULT)
      ARC_STRACE_REPORT("buf=%s", arc::GetRWBufStr(buf, count).c_str());
    g_wrap_write_nest_count.Set(wrap_write_nest_count);
    if (result == -1 && errno != EAGAIN)
      ARC_STRACE_ALWAYS_WARN_FAILURE();
    ARC_STRACE_RETURN(result);
  }
}

ssize_t __wrap_writev(int fd, const struct iovec* iov, int iovcnt) {
  // TODO(crbug.com/241955): Output the first N bytes in |iov|.
  // TODO(crbug.com/241955): Stringify |iov|?
  ARC_STRACE_ENTER_FD("writev", "%d, %p, %d", fd, iov, iovcnt);
  ssize_t result = VirtualFileSystem::GetVirtualFileSystem()->writev(
      fd, iov, iovcnt);
  if (result == -1)
    ARC_STRACE_ALWAYS_WARN_FAILURE();
  ARC_STRACE_RETURN(result);
}

mode_t __wrap_umask(mode_t mask) {
  ARC_STRACE_ENTER("umask", "0%o", mask);
  mode_t return_umask = VirtualFileSystem::GetVirtualFileSystem()->umask(mask);
  ARC_STRACE_RETURN(return_umask);
}

mode_t __wrap___umask_chk(mode_t mask) {
  // TODO(crbug.com/478000): Wrap __umask_chk accurately?
  return __wrap_umask(mask);
}

// The following is an example call stach when close() is called:
//
// our_function_that_calls_close()
//   close()  // in Bionic
//     __nacl_irt_close()  // function pointer call
//        __nacl_irt_close_wrap()  // this function
//          __wrap_close()  // in file_wrap.cc
//             FileSystem::close()  // in posix_translation
//
// Also note that code in posix_translation/ is always able to call into the
// original IRT by calling real_close() defined in file_wrap.cc.
IRT_WRAPPER(close, int fd) {
  int result = __wrap_close(fd);
  return !result ? 0 : errno;
}

// See native_client/src/trusted/service_runtime/include/sys/fcntl.h
#define NACL_ABI_O_SYNC 010000

IRT_WRAPPER(open, const char* pathname, int oflag, mode_t cmode, int* newfd) {
  // |oflag| is mostly compatible between NaCl and Bionic, O_SYNC is
  // the only exception.
  int bionic_oflag = oflag;
  if ((bionic_oflag & NACL_ABI_O_SYNC)) {
    bionic_oflag &= ~NACL_ABI_O_SYNC;
    bionic_oflag |= O_SYNC;
  }
  *newfd = __wrap_open(pathname, bionic_oflag, cmode);
  return *newfd >= 0 ? 0 : errno;
}

IRT_WRAPPER(read, int fd, void* buf, size_t count, size_t* nread) {
  ssize_t result = __wrap_read(fd, buf, count);
  *nread = result;
  return result >= 0 ? 0 : errno;
}

IRT_WRAPPER(seek, int fd, off64_t offset, int whence, off64_t* new_offset) {
  *new_offset = __wrap_lseek64(fd, offset, whence);
  return *new_offset >= 0 ? 0 : errno;
}

IRT_WRAPPER(write, int fd, const void* buf, size_t count, size_t* nwrote) {
  ssize_t result = __wrap_write(fd, buf, count);
  *nwrote = result;
  return result >= 0 ? 0 : errno;
}

// We implement IRT wrappers using __wrap_* functions. As posix_translation
// may call real functions, we define them using real IRT interfaces.

int real_close(int fd) {
  ALOG_ASSERT(__nacl_irt_close_real);
  int result = __nacl_irt_close_real(fd);
  if (result) {
    errno = result;
    return -1;
  }
  return 0;
}

int real_fstat(int fd, struct stat* buf) {
  ALOG_ASSERT(__nacl_irt_fstat_real);
  struct nacl_abi_stat nacl_buf;
  int result = __nacl_irt_fstat_real(fd, &nacl_buf);
  if (result) {
    errno = result;
    return -1;
  }
  NaClAbiStatToStat(&nacl_buf, buf);
  return 0;
}

ssize_t real_read(int fd, void* buf, size_t count) {
  ALOG_ASSERT(__nacl_irt_read_real);
  size_t nread;
  int result = __nacl_irt_read_real(fd, buf, count, &nread);
  if (result) {
    errno = result;
    return -1;
  }
  return nread;
}

off64_t real_lseek64(int fd, off64_t offset, int whence) {
  ALOG_ASSERT(__nacl_irt_seek_real);
  off64_t nacl_offset;
  int result = __nacl_irt_seek_real(fd, offset, whence, &nacl_offset);
  if (result) {
    errno = result;
    return -1;
  }
  return nacl_offset;
}

ssize_t real_write(int fd, const void* buf, size_t count) {
  ALOG_ASSERT(__nacl_irt_write_real);
  if (VirtualFileSystem::IsInitialized()) {
    VirtualFileSystem::GetVirtualFileSystem()->DebugWriteLocked(fd, buf, count);
  }
  size_t nwrote;
  int result = __nacl_irt_write_real(fd, buf, count, &nwrote);
  if (result) {
    errno = result;
    return -1;
  }
  return nwrote;
}

namespace posix_translation {

extern void InitializeIRTHooksForSockets();

// The call stack gets complicated when IRT is hooked. See the comment near
// IRT_WRAPPER(close) for more details.
void InitializeIRTHooks() {
  DO_WRAP(close);
  DO_WRAP(dup);
  DO_WRAP(dup2);
  DO_WRAP(fstat);
  DO_WRAP(getcwd);
  DO_WRAP(getdents);
  DO_WRAP(lstat);
  DO_WRAP(mkdir);
  DO_WRAP(open);
  DO_WRAP(poll);
  DO_WRAP(read);
  DO_WRAP(seek);
  DO_WRAP(stat);
  DO_WRAP(unlink);
  DO_WRAP(write);

  InitializeIRTHooksForSockets();
}

void InitializeIRTHooksForPosixTranslationTest() {
  // Some tests in posix_translation_test call real_XXX functions. To make them
  // work, set up the *_real pointers here. We cannot do that in IRT_WRAPPER
  // since doing so introduces static initializers. This function should NOT be
  // ARC_EXPORT'ed.
  __nacl_irt_close_real = __nacl_irt_close;
  __nacl_irt_fstat_real = __nacl_irt_fstat;
  __nacl_irt_seek_real = __nacl_irt_seek;
  __nacl_irt_read_real = __nacl_irt_read;
  __nacl_irt_write_real = __nacl_irt_write;
}

}  // namespace posix_translation
