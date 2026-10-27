// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "posix_translation/virtual_file_system.h"

#include <arpa/inet.h>
#include <limits.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/vfs.h>
#include <unistd.h>  // for access

#include <algorithm>
#include <set>
#include <utility>

#if defined(_STLP_USE_STATIC_LIB)
#include <iostream>  // NOLINT
#endif

#include "base/compiler_specific.h"
#include "base/memory/scoped_ptr.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/time/time.h"  // for base::TimeDelta
#include "common/arc_strace.h"
#include "common/alog.h"
#include "common/file_util.h"
#include "common/options.h"
#include "common/process_emulator.h"
#include "common/trace_event.h"
#include "posix_translation/address_util.h"
#include "posix_translation/dir.h"
#include "posix_translation/directory_file_stream.h"
#include "posix_translation/epoll_stream.h"
#include "posix_translation/fd_to_file_stream_map.h"
#include "posix_translation/local_socket.h"
#include "posix_translation/memory_region.h"
#include "posix_translation/mount_point_manager.h"
#include "posix_translation/passthrough.h"
#include "posix_translation/path_util.h"
#include "posix_translation/pepper_file.h"
#include "posix_translation/process_environment.h"
#include "posix_translation/readonly_file.h"
#include "posix_translation/tcp_socket.h"
#include "posix_translation/time_util.h"
#include "posix_translation/udp_socket.h"

extern "C"
int __wrap_pthread_create(
    pthread_t* thread_out,
    pthread_attr_t const* attr,
    void* (*start_routine)(void*),  // NOLINT(readability/casting)
    void* arg);

namespace posix_translation {

#if defined(DEBUG_POSIX_TRANSLATION)
namespace ipc_stats {
// The implementation is in pepper_file.cc. Do not include pepper_file.h here
// to prevent VirtualFileSystem from depending on a concrete filesytem.
std::string GetIPCStatsAsStringLocked();
}  // namespace ipc_stats
#endif

namespace {

const char kVirtualFileSystemHandlerStr[] ALLOW_UNUSED = "VirtualFileSystem";

const int kPreopenPendingFd = -2;

// Symlinks being followed by this thread's ResolveSymlinks (a handler's
// readlink may let go of mutex_ while it waits: OpfsHandler).
thread_local int t_symlink_depth = 0;

void FillPermissionInfoToStat(const PermissionInfo& permission,
                              struct stat* out) {
  // Files created by apps should not allow other users to read them.
  // This is checked by a CTS suite (FileSystemPermissionTest).
  static const mode_t kDefaultUserFilePermission = 0600;
  static const mode_t kDefaultUserDirPermission = 0700;
  static const mode_t kDefaultSystemFilePermission = 0644;
  static const mode_t kDefaultSystemDirPermission = 0755;

  ALOG_ASSERT(permission.IsValid());
  out->st_uid = permission.file_uid();
  out->st_gid = arc::kRootGid;
  mode_t file_type = out->st_mode & S_IFMT;
  ALOG_ASSERT(file_type);
  mode_t perm = out->st_mode & 0777;
  // If the permission is not set by FileSystemHandler, fill it based on the
  // file type and the owner.
  if (file_type && !perm) {
    // This function must not be used for special files.
    ALOG_ASSERT((file_type == S_IFDIR) || (file_type == S_IFREG));
    const bool is_dir = (file_type == S_IFDIR);
    if (arc::IsAppUid(out->st_uid))
      perm = is_dir ? kDefaultUserDirPermission : kDefaultUserFilePermission;
    else
      perm = (is_dir ? kDefaultSystemDirPermission :
              kDefaultSystemFilePermission);
  } else {
    ARC_STRACE_REPORT("Permission already set %o", static_cast<int>(perm));
  }
  out->st_mode = file_type | perm;
}

bool IsEligibleForPreopen(int oflag) {
  // O_LARGEFILE: ignored on Bionic.
  // O_CLOEXEC: not supported by POSIX translation.
  return (oflag & ~(O_LARGEFILE | O_CLOEXEC)) == 0;
}

// The current VirtualFileSystemInterface exposed to plugins via
// GetVirtualFileSystemInterface().
VirtualFileSystemInterface* g_current_file_system = NULL;

}  // namespace

VirtualFileSystemInterface* GetVirtualFileSystemInterface() {
  // A mutex lock is not necessary here since |SetVirtualFileSystemInterface|
  // must be called by the main thread before the first pthread_create() call
  // is made. It is ensured that a non-main thread can see correct
  // |g_current_file_system| because pthread_create() call to create the thread
  // itself is a memory barrier.
  ALOG_ASSERT(g_current_file_system);
  return g_current_file_system;
}

void SetVirtualFileSystemInterface(VirtualFileSystemInterface* vfs) {
  ALOG_ASSERT(!arc::ProcessEmulator::IsMultiThreaded());
  delete g_current_file_system;
  g_current_file_system = vfs;
}

// The VirtualFileSystem instance to be returned by GetVirtualFileSystem().
// Set in the VFS constructor and unset in the VFS destructor.
// Usually this should be the same as g_current_file_system, but this can be
// NULL while g_current_file_system is non-NULL when a mock
// VirtualFileSystemInterface implementation is set as current in unit tests
// (e.g. FileSystemManagerTest).
VirtualFileSystem* VirtualFileSystem::file_system_ = NULL;

VirtualFileSystem::VirtualFileSystem(
    pp::Instance* instance,
    ProcessEnvironment* process_environment,
    int min_fd,
    int max_fd)
    : browser_ready_(false),
      instance_(instance),
      process_environment_(process_environment),
      cond_(&mutex_),
      fd_to_stream_(new FdToFileStreamMap(min_fd, max_fd)),
      memory_region_(new MemoryRegion),
      // Some file systems do not use zero and very small numbers as inode
      // number. For example, ext4 reserves 0 to 10 (see linux/fs/ext4/ext4.h)
      // for special purposes. Do not use such numbers to emulate the behavior.
      next_inode_(128),
      mount_points_(new MountPointManager),
      abstract_socket_namespace_(&mutex_),
      logd_socket_namespace_(&mutex_),
      host_resolver_(instance),
      preopen_started_(false),
      abort_on_unexpected_memory_maps_(true) {
  ALOG_ASSERT(!file_system_);
  file_system_ = this;
  if (arc::Options::GetInstance()->GetBool("save_logs_to_file")) {
    debug_fds_[STDOUT_FILENO] =
        FileDescNamePair(kInvalidFileNo, "/data/arc_stdout.txt");
    debug_fds_[STDERR_FILENO] =
        FileDescNamePair(kInvalidFileNo, "/data/arc_stderr.txt");
  }
}

VirtualFileSystem::~VirtualFileSystem() {
  file_system_ = NULL;
}

VirtualFileSystem* VirtualFileSystem::GetVirtualFileSystem() {
  ALOG_ASSERT(file_system_);
  // We require this condition so that there is always at most one "current"
  // VirtualFileSystem instance at any time.
  ALOG_ASSERT(GetVirtualFileSystemInterface() == file_system_);
  return file_system_;
}

bool VirtualFileSystem::IsInitialized() {
  return file_system_ != NULL;
}

FileSystemHandler* VirtualFileSystem::GetFileSystemHandler(
    const std::string& path) {
  base::AutoLock lock(mutex_);
  return GetFileSystemHandlerLocked(path, NULL /* permission */);
}

FileSystemHandler* VirtualFileSystem::GetFileSystemHandlerLocked(
    const std::string& path, PermissionInfo* out_permission) {
  mutex_.AssertAcquired();

  uid_t file_uid = 0;
  FileSystemHandler* handler = mount_points_->GetFileSystemHandler(path,
                                                                   &file_uid);
  if (!handler) {
    ARC_STRACE_REPORT("No handler is found for '%s'", path.c_str());
    return NULL;
  }
  // Call REPORT_HANDLER() so that the current function call is categrized as
  // |handler->name()| rather than |kVirtualFileSystemHandlerStr|.
  ARC_STRACE_REPORT_HANDLER(handler->name().c_str());

  if (!handler->IsInitialized()) {
    ALOGI("Initializing %s...", handler->name().c_str());
    const base::TimeTicks now = base::TimeTicks::Now();
    handler->Initialize();
    ALOGI("Initialized %s: %lld ms", handler->name().c_str(),
          (base::TimeTicks::Now() - now).InMillisecondsRoundedUp());
  }
  ALOG_ASSERT(handler->IsInitialized());

  if (out_permission) {
    // Check if |path| is writable. First, compare the current UID with the
    // owner's of the file. Then, check if |path| is writable to the world.
    const uid_t uid = arc::ProcessEmulator::GetUid();
    const bool is_writable = !arc::IsAppUid(uid) || (file_uid == uid) ||
        handler->IsWorldWritable(path);
    *out_permission = PermissionInfo(file_uid, is_writable);
  }

  // Disallow path handlers being used on main thread since at lease one of the
  // handlers (PepperFileHandler) might call pp::BlockUntilComplete() which is
  // not allowed on the thread.
  LOG_ALWAYS_FATAL_IF(pp::Module::Get()->core()->IsMainThread());
  return handler;
}

ino_t VirtualFileSystem::GetInodeLocked(const std::string& path) {
  ALOG_ASSERT(!path.empty());
  ALOG_ASSERT(IsNormalizedPathLocked(path), "%s", path.c_str());
  return GetInodeUncheckedLocked(path);
}

ino_t VirtualFileSystem::GetInodeUncheckedLocked(const std::string& path) {
  // DO NOT CALL THIS FUNCTION DIRECTLY. This is only for VFS::lstat(),
  // VFS::GetInodeLocked(), and DirImpl::GetNext().
  mutex_.AssertAcquired();
  ALOG_ASSERT(!path.empty());

  InodeMap::const_iterator it = inodes_.find(path);
  if (it != inodes_.end())
    return it->second;

  ARC_STRACE_REPORT("Assigning inode %lld for %s",
                      static_cast<int64_t>(next_inode_), path.c_str());
  inodes_[path] = next_inode_;
  // Note: Do not try to reuse returned inode numbers. Doing this would
  // break MemoryRegion::IsWriteMapped().
  return next_inode_++;
}

void VirtualFileSystem::RemoveInodeLocked(const std::string& path) {
  mutex_.AssertAcquired();
  ALOG_ASSERT(IsNormalizedPathLocked(path), "%s", path.c_str());
  inodes_.erase(path);
}

void VirtualFileSystem::ReassignInodeLocked(const std::string& oldpath,
                                            const std::string& newpath) {
  mutex_.AssertAcquired();
  ALOG_ASSERT(IsNormalizedPathLocked(oldpath), "%s", oldpath.c_str());
  ALOG_ASSERT(IsNormalizedPathLocked(newpath), "%s", newpath.c_str());

  InodeMap::iterator it = inodes_.find(oldpath);
  if (it == inodes_.end()) {
    // stat() has not been called for |oldpath|. Removing the inode for
    // |newpath| is for handling the following case:
    //   open("/a.txt", O_CREAT);  // this may not assign an inode yet.
    //   open("/b.txt", O_CREAT);  // ditto.
    //   stat("/b.txt");  // a new inode is assigned to b.txt.
    //   rename("/a.txt", "/b.txt");  // the inode for b.txt should be removed.
    inodes_.erase(newpath);
  } else {
    inodes_[newpath] = it->second;
    inodes_.erase(it);
  }
}

std::string VirtualFileSystem::GetMemoryMapAsString() {
  base::AutoLock lock(mutex_);
  return GetMemoryMapAsStringLocked();
}

std::string VirtualFileSystem::GetMemoryMapAsStringLocked() {
  mutex_.AssertAcquired();
  return memory_region_->GetMemoryMapAsString();
}

std::string VirtualFileSystem::GetIPCStatsAsString() {
#if defined(DEBUG_POSIX_TRANSLATION)
  base::AutoLock lock(mutex_);
  return ipc_stats::GetIPCStatsAsStringLocked();
#else
  return "unknown";
#endif
}

int VirtualFileSystem::StatForTesting(
    const std::string& pathname, struct stat* out) {
  return stat(pathname, out);
}

bool VirtualFileSystem::IsMemoryRangeAvailableLocked(void* addr,
                                                     size_t length) {
  mutex_.AssertAcquired();
  if (!memory_region_->AddFileStreamByAddr(
          addr, length, kBadInode, PROT_NONE, 0, NULL)) {
    return false;
  }
  const int result =
      memory_region_->RemoveFileStreamsByAddr(addr, length, true);
  ALOG_ASSERT(!result);
  return true;
}

int VirtualFileSystem::AddFileStreamLocked(scoped_refptr<FileStream> stream) {
  mutex_.AssertAcquired();
  ALOG_ASSERT(stream->permission().IsValid(), "pathname=%s stream=%s",
              stream->pathname().c_str(), stream->GetStreamType());
  int fd = GetFirstUnusedDescriptorLocked();
  if (fd >= 0)
    fd_to_stream_->AddFileStream(fd, stream);
  return fd;
}

int VirtualFileSystem::open(const std::string& pathname, int oflag,
                            mode_t cmode) {
  base::AutoLock lock(mutex_);
  return OpenLocked(pathname, oflag, cmode, true);
}

// Android uses madvise to hint to the kernel about what Ashmem regions can be
// deleted, and TcMalloc uses it to hint about returned system memory.
int VirtualFileSystem::madvise(void* addr, size_t length, int advice) {
  if (!util::IsPageAligned(addr)) {
    errno = EINVAL;
    return -1;
  }
  base::AutoLock lock(mutex_);
#if defined(__EMSCRIPTEN__)
  if (length && IsInReadonlyImage(addr, util::RoundToPageSize(length)))
    return 0;  // read-only and file-backed: nothing to do for any advice
#endif
  return memory_region_->SetAdviceByAddr(
      addr, util::RoundToPageSize(length), advice);
}

void* VirtualFileSystem::mmap(void* addr, size_t length, int prot, int flags,
                              int fd, off_t offset) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  if (!length) {
    errno = EINVAL;
    return MAP_FAILED;
  }

  if (!util::IsPageAligned(addr)) {
    // Do the same as Linux which accepts an unaligned |addr| unless MAP_FIXED
    // is specified. Check MAP_FIXED early in VFS to provide a consistent errno
    // (EINVAL) regardless of the stream the |fd| is bound to.
    if (flags & MAP_FIXED) {
      errno = EINVAL;
      return MAP_FAILED;
    }
    const uintptr_t aligned_addr =
        reinterpret_cast<uintptr_t>(addr) & ~(util::GetPageSize() - 1);
    ARC_STRACE_REPORT("Rewriting an unaligned |addr| %p to 0x%08x",
                      addr, aligned_addr);
    // Not to confuse stream implementations, re-align the bad hint in VFS.
    addr = reinterpret_cast<void*>(aligned_addr);
  }

  // arc-wasm (topic native-exec): apps think in 4 KB pages (sysconf), the
  // host's are 64 KB. A file offset in 4 KB steps that is not on a host page
  // is mapped as a copy of the file in anonymous memory, unless the stream
  // copies anyway and takes any offset (MapsAnyOffset).
  const bool host_aligned =
      util::RoundToPageSize(offset) == static_cast<size_t>(offset);
  if (!host_aligned &&
      ((offset & 4095) != 0 || (flags & (MAP_ANON | MAP_ANONYMOUS)))) {
    // |offset| is not a multiple of the page size.
    errno = EINVAL;
    return MAP_FAILED;
  }

  scoped_refptr<FileStream> stream;
  scoped_refptr<FileStream> copy_from;
  // dlmalloc() in Bionic never calls mmap with MAP_ANONYMOUS | MAP_FIXED.
  // Also, note that calls from Bionic can not be captured by posix_translation,
  // and MemoryRegion can not track such memory regions.
  if (flags & (MAP_ANON | MAP_ANONYMOUS)) {
    stream = new PassthroughStream;
    ARC_STRACE_REPORT_HANDLER(stream->GetStreamType());
  } else {
    stream = fd_to_stream_->GetStream(fd);
  }
  if (!stream) {
    errno = EBADF;
    return MAP_FAILED;
  }
  if (!host_aligned && !stream->MapsAnyOffset()) {
    if ((flags & MAP_SHARED) && (prot & PROT_WRITE)) {
      ALOGW("mmap: %s at offset 0x%llx is a copy: writes stay in memory",
            stream->pathname().c_str(), static_cast<long long>(offset));
    }
    copy_from = stream;
    stream = new PassthroughStream;
    flags = (flags & MAP_FIXED) | MAP_PRIVATE | MAP_ANONYMOUS;
    prot |= PROT_WRITE;
  }

  length = util::RoundToPageSize(length);
  void* new_addr = stream->mmap(addr, length, prot, flags,
                                copy_from ? 0 : offset);
  if (new_addr == MAP_FAILED)
    return new_addr;
  if (copy_from) {
    size_t done = 0;
    ssize_t n = 0;
    while (done < length) {
      n = copy_from->pread(static_cast<char*>(new_addr) + done, length - done,
                           offset + done);
      if (n <= 0)
        break;
      done += n;
    }
    if (n < 0 && done == 0) {  // a stream that cannot be read at an offset
      stream->munmap(new_addr, length);
      errno = ENODEV;
      return MAP_FAILED;
    }
  }

  ALOG_ASSERT(util::IsPageAligned(new_addr));

#if defined(__EMSCRIPTEN__)
  // The /system image's own pages (readonly_file.cc, MemoryImageStream):
  // not one of the MemoryRegion's mappings (two may overlap; nothing to
  // unmap or write back).
  if (IsInReadonlyImage(new_addr, length))
    return new_addr;
#endif

  // If MAP_FIXED is specified, we should remove old FileStream bound to
  // the region [addr, addr+length), but should not call underlying munmap()
  // implementation because the region has already been unmapped by the mmap
  // call above.
  if (flags & MAP_FIXED)
    memory_region_->RemoveFileStreamsByAddr(addr, length, false);

  bool result = memory_region_->AddFileStreamByAddr(
      new_addr, length, offset /* for printing debug info */, prot, flags,
      stream);
  if (!result) {
    if (flags & MAP_FIXED) {
      ALOG_ASSERT(!abort_on_unexpected_memory_maps_,
                  "\n%s\nThis memory region does not support mmap with "
                  "MAP_FIXED because the region is backed by a POSIX "
                  "incompatible stream. address: %p, size: 0x%zx, stream: %s",
                  GetMemoryMapAsStringLocked().c_str(), new_addr, length,
                  stream->GetStreamType());
    } else {
      ALOG_ASSERT(!abort_on_unexpected_memory_maps_,
                  "\n%s\nUnexpected address: %p, size: 0x%zx, stream: %s",
                  GetMemoryMapAsStringLocked().c_str(), new_addr, length,
                  stream->GetStreamType());
    }
    // It should happen because of a bug or the restriction of MemoryFile
    // incompatibility.
    errno = ENODEV;
    new_addr = MAP_FAILED;
  }
  return new_addr;
}

int VirtualFileSystem::mprotect(void* addr, size_t length, int prot) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  // Note: Do not check if |length| is zero here. See the comment in
  // ChangeProtectionModeByAddr.
  if (!util::IsPageAligned(addr)) {
    errno = EINVAL;
    return -1;
  }

  length = util::RoundToPageSize(length);
#if defined(__EMSCRIPTEN__)
  if (length && IsInReadonlyImage(addr, length)) {
    if (prot & PROT_WRITE) {
      errno = EACCES;  // the image's pages: no copy to write to
      return -1;
    }
    return 0;
  }
#endif
  // ChangeProtectionModeByAddr may call FileStream::mprotect() for each stream
  // in [addr, addr+length).
  return memory_region_->ChangeProtectionModeByAddr(addr, length, prot);
}

int VirtualFileSystem::munmap(void* addr, size_t length) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  if (!util::IsPageAligned(addr) || !length) {
    errno = EINVAL;
    return -1;
  }

  length = util::RoundToPageSize(length);
#if defined(__EMSCRIPTEN__)
  if (IsInReadonlyImage(addr, length))
    return 0;  // the image stays
#endif
  // RemoveFileStreamsByAddr may call FileStream::munmap() for each stream in
  // [addr, addr+length).
  return memory_region_->RemoveFileStreamsByAddr(addr, length, true);
}

int VirtualFileSystem::close(int fd) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  if (!CloseLocked(fd)) {
    errno = EBADF;
    return -1;
  }
  return 0;
}

bool VirtualFileSystem::CloseLocked(int fd) {
  mutex_.AssertAcquired();
  scoped_refptr<FileStream> stream = fd_to_stream_->GetStream(fd);
  if (!stream)
    return false;
  fd_to_stream_->RemoveFileStream(fd);
  return true;
}

void VirtualFileSystem::InvalidateCache() {
  base::AutoLock lock(mutex_);
  std::vector<FileSystemHandler*> handlers;
  mount_points_->GetAllFileSystemHandlers(&handlers);
  for (size_t i = 0; i < handlers.size(); ++i) {
    handlers[i]->InvalidateCache();
  }
}

void VirtualFileSystem::AddToCache(const std::string& path,
                                   const PP_FileInfo& file_info,
                                   bool exists) {
  base::AutoLock lock(mutex_);
  std::string resolved(path);
  GetNormalizedPathLocked(&resolved, kResolveSymlinks);
  uid_t dummy = 0;
  // Use |mount_points_| directly instead of GetFileSystemHandlerLocked so
  // that the main thread can call this method.
  FileSystemHandler* handler =
      mount_points_->GetFileSystemHandler(path, &dummy);
  if (handler)
    handler->AddToCache(path, file_info, exists);
  else
    ALOGW("AddToCache: handler for %s not found", path.c_str());
}

void VirtualFileSystem::SchedulePreopen(const std::string& path) {
  base::AutoLock lock(mutex_);
  LOG_ALWAYS_FATAL_IF(
      preopen_started_, "SchedulePreopen() called after StartPreopen()");
  std::string resolved(path);
  GetNormalizedPathLocked(&resolved, kResolveSymlinks);
  scheduled_preopens_.push_back(resolved);
  preopened_fds_.insert(std::make_pair(resolved, kPreopenPendingFd));
}

bool VirtualFileSystem::RegisterFileStream(
    int fd, scoped_refptr<FileStream> stream) {
  base::AutoLock lock(mutex_);
  if (fd_to_stream_->IsKnownDescriptor(fd))
    return false;
  ALOG_ASSERT(stream->permission().IsValid());
  fd_to_stream_->AddFileStream(fd, stream);
  return true;
}

bool VirtualFileSystem::IsKnownDescriptor(int fd) {
  base::AutoLock lock(mutex_);
  return fd_to_stream_->IsKnownDescriptor(fd);
}

ssize_t VirtualFileSystem::read(int fd, void* buf, size_t count) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  scoped_refptr<FileStream> stream = fd_to_stream_->GetStream(fd);
  if (stream)
    return stream->read(buf, count);
  errno = EBADF;
  return -1;
}

ssize_t VirtualFileSystem::write(int fd, const void* buf, size_t count) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  scoped_refptr<FileStream> stream = fd_to_stream_->GetStream(fd);
  if (stream)
    return stream->write(buf, count);
  errno = EBADF;
  return -1;
}

void VirtualFileSystem::DebugWriteLocked(int fd, const void* buf,
                                         size_t count) {
  if (!browser_ready_) {
    return;
  }
  auto iter = debug_fds_.find(fd);
  if (iter == debug_fds_.end()) {
    return;
  }

  FileDescNamePair& debug_fd = iter->second;

  scoped_refptr<FileStream> stream;
  if (debug_fd.fd_ == kInvalidFileNo) {
    // We delay this check until here because this function is sometimes called
    // without the lock held before the browser is ready.  Furthermore, this
    // function is called from a critical code path and so the check is delayed
    // until absolutely necessary.
    mutex_.AssertAcquired();

    debug_fd.fd_ = GetFirstUnusedDescriptorLocked();
    if (debug_fd.fd_ >= 0) {
      PermissionInfo permission;
      FileSystemHandler* handler = GetFileSystemHandlerLocked(debug_fd.name_,
                                                              &permission);
      if (handler) {
        static const mode_t kDefaultUserFilePermission = 0600;
        stream = handler->open(debug_fd.fd_, debug_fd.name_, O_CREAT | O_RDWR,
                               kDefaultUserFilePermission);
        if (stream) {
          stream->set_permission(permission);
          fd_to_stream_->AddFileStream(debug_fd.fd_, stream);
        }
      }
    }
  } else {
    stream = fd_to_stream_->GetStream(debug_fd.fd_);
  }
  if (stream) {
    stream->debug_write(buf, count);
  }
}

int VirtualFileSystem::readv(int fd, const struct iovec* iov, int count) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  scoped_refptr<FileStream> stream = fd_to_stream_->GetStream(fd);
  if (stream)
    return stream->readv(iov, count);
  errno = EBADF;
  return -1;
}

char* VirtualFileSystem::realpath(const char* path,
                                  char* resolved_path) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  if (!path) {
    errno = EINVAL;
    return NULL;
  }
  // Return NULL when |path| does not exist.
  struct stat st;
  if (StatLocked(path, &st))
    return NULL;  // errno is set in StatLocked.

  std::string resolved(path);
  GetNormalizedPathLocked(&resolved, kResolveSymlinks);
  if (resolved.length() >= PATH_MAX) {
    errno = ENAMETOOLONG;
    return NULL;
  }

  // Note: resolved_path == NULL means we need to allocate a buffer.
  char* output = resolved_path;
  if (!output)
    output = static_cast<char*>(malloc(PATH_MAX));

  snprintf(output, PATH_MAX, "%s", resolved.c_str());
  ARC_STRACE_REPORT("result=\"%s\"", output);
  return output;
}

int VirtualFileSystem::writev(int fd, const struct iovec* iov, int count) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  scoped_refptr<FileStream> stream = fd_to_stream_->GetStream(fd);
  if (stream)
    return stream->writev(iov, count);
  errno = EBADF;
  return -1;
}

int VirtualFileSystem::chdir(const std::string& path) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  if (path.empty()) {
    errno = ENOENT;
    return -1;
  }
  size_t length = path.length();
  while (length > 0 && path.at(length - 1) == '/') {
    // Remove trailing slashes if exist. This is because chdir("foo/") should
    // succeed if the directory "foo" exists, but stat("foo/", &st), or
    // StatLocked("foo/", &st) fails with ENOENT.
    length--;
  }
  std::string new_path = path.substr(0, length);
  if (length)
    GetNormalizedPathLocked(&new_path, kResolveSymlinks);

  // We do not check if the root directory exist here.
  if (!new_path.empty()) {
    struct stat st;
    int result = StatLocked(new_path, &st);
    if (result)
      return result;
    if (!S_ISDIR(st.st_mode)) {
      errno = ENOTDIR;
      return -1;
    }
  }

  // Keep the last character always being "/".
  process_environment_->SetCurrentDirectory(new_path + "/");
  return 0;
}

char* VirtualFileSystem::getcwd(char* buf, size_t size) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  char* result = NULL;
  const std::string& current_working_directory =
      process_environment_->GetCurrentDirectory();
  size_t path_length = current_working_directory.length();
  // |current_working_directory| contains "/" at the end of the path, and
  // |result| should not contain the last "/" if the path is not root("/").
  ALOG_ASSERT(util::EndsWithSlash(current_working_directory));
  if (path_length > 1)
    path_length--;

  if (buf && !size) {
    errno = EINVAL;
    return NULL;
  } else if (size <= path_length && (buf || size)) {
    errno = ERANGE;
    return NULL;
  } else if (!buf) {
    if (!size)
      size = path_length + 1;
    result = static_cast<char*>(malloc(size));
    if (!result) {
      errno = ENOMEM;
      return NULL;
    }
  } else {
    result = buf;
  }
  // Copy |current_working_directory| without the last "/".
  strncpy(result, current_working_directory.c_str(), path_length);
  result[path_length] = 0;
  return result;
}

int VirtualFileSystem::IsPollReadyLocked(
    struct pollfd* fds, nfds_t nfds, bool apply) {
  mutex_.AssertAcquired();
  ALOG_ASSERT(fds);

  int result = 0;
  for (nfds_t i = 0; i < nfds; ++i) {
    const int16_t events_mask = fds[i].events | POLLHUP | POLLERR | POLLNVAL;
    scoped_refptr<FileStream> stream = fd_to_stream_->GetStream(fds[i].fd);
    int16_t events =
        (stream ? stream->GetPollEvents() : POLLNVAL) & events_mask;
    if (events)
      ++result;

    if (apply)
      fds[i].revents = events;
  }

  return result;
}

int VirtualFileSystem::poll(struct pollfd* fds, nfds_t nfds, int timeout) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  if (timeout != 0) {
    const base::TimeTicks time_limit =  internal::TimeOutToTimeLimit(
        base::TimeDelta::FromMilliseconds(std::max(0, timeout)));
    while (!IsPollReadyLocked(fds, nfds, false)) {
      if (WaitUntil(time_limit)) {
        // timedout, or spurious wakeup, or real wakeup. Either way, we can
        // just break since |timeout| has expired.
        break;
      }
    }
  }

  return IsPollReadyLocked(fds, nfds, true);
}

ssize_t VirtualFileSystem::pread(int fd, void* buf, size_t count,
                                 off64_t offset) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  scoped_refptr<FileStream> stream = fd_to_stream_->GetStream(fd);
  if (stream)
    return stream->pread(buf, count, offset);
  errno = EBADF;
  return -1;
}

ssize_t VirtualFileSystem::pwrite(int fd, const void* buf, size_t count,
                                  off64_t offset) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  scoped_refptr<FileStream> stream = fd_to_stream_->GetStream(fd);
  if (stream)
    return stream->pwrite(buf, count, offset);
  errno = EBADF;
  return -1;
}

off64_t VirtualFileSystem::lseek(int fd, off64_t offset, int whence) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  scoped_refptr<FileStream> stream = fd_to_stream_->GetStream(fd);
  if (stream)
    return stream->lseek(offset, whence);
  errno = EBADF;
  return -1;
}

int VirtualFileSystem::dup(int fd) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);
  return DupLocked(fd, -1);
}

int VirtualFileSystem::dup2(int fd, int newfd) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);
  return DupLocked(fd, newfd);
}

int VirtualFileSystem::DupLocked(int fd, int newfd) {
  mutex_.AssertAcquired();

  scoped_refptr<FileStream> stream = fd_to_stream_->GetStream(fd);
  if (!stream) {
    errno = EBADF;
    return -1;
  }

  if (newfd < 0)
    newfd = GetFirstUnusedDescriptorLocked();
  if (newfd < 0) {
    errno = EMFILE;
    return -1;
  }

  ARC_STRACE_DUP_FD(fd, newfd);
  if (fd == newfd)
    return newfd;  // NB: Do not reuse this code for dup3().
  CloseLocked(newfd);
  fd_to_stream_->AddFileStream(newfd, stream);
  return newfd;
}

int VirtualFileSystem::GetMinFd() const {
  return fd_to_stream_->min_file_id();
}

int VirtualFileSystem::GetMaxFd() const {
  return fd_to_stream_->max_file_id();
}

scoped_refptr<FileStream> VirtualFileSystem::GetStreamLocked(int fd) {
  mutex_.AssertAcquired();
  return fd_to_stream_->GetStream(fd);
}

int VirtualFileSystem::epoll_create1(int flags) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  int fd = GetFirstUnusedDescriptorLocked();
  if (fd < 0) {
    errno = EMFILE;
    return -1;
  }
  scoped_refptr<EPollStream> stream = new EPollStream(fd, flags);
  fd_to_stream_->AddFileStream(fd, stream);
  // Since this function does not call GetFileSystemHandlerLocked(), call
  // REPORT_HANDLER() explicitly to make STATS in arc_strace.txt easier
  // to read.
  ARC_STRACE_REPORT_HANDLER(stream->GetStreamType());
  return fd;
}

int VirtualFileSystem::epoll_ctl(int epfd, int op, int fd,
                                 struct epoll_event* event) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  scoped_refptr<FileStream> epoll_stream = fd_to_stream_->GetStream(epfd);
  scoped_refptr<FileStream> target_stream = fd_to_stream_->GetStream(fd);
  if (!epoll_stream || !target_stream) {
    errno = EBADF;
    return -1;
  }
  if (epfd == fd) {
    errno = EINVAL;
    return -1;
  }
  return epoll_stream->epoll_ctl(op, target_stream, event);
}

int VirtualFileSystem::epoll_wait(int epfd, struct epoll_event* events,
                                  int maxevents, int timeout) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  scoped_refptr<FileStream> stream = fd_to_stream_->GetStream(epfd);
  if (!stream) {
    errno = EBADF;
    return -1;
  }
  return stream->epoll_wait(events, maxevents, timeout);
}

// This is ARC-specific function in libc.so.
extern "C" long __arc_fs_conf(struct statfs* buf, int name);  // NOLINT

long VirtualFileSystem::fpathconf(int fd, int name) {  // NOLINT
  // No locking since all synchronization we need is inside fstatfs.
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);
  struct statfs buf;
  int ret = this->fstatfs(fd, &buf);
  if (ret < 0) {
    return -1;
  }
  return __arc_fs_conf(&buf, name);
}

long VirtualFileSystem::pathconf(const std::string& pathname,  // NOLINT
                                 int name) {
  // No locking since all synchronization we need is inside statfs.
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);
  struct statfs buf;
  int ret = this->statfs(pathname, &buf);
  if (ret < 0) {
    return -1;
  }
  return __arc_fs_conf(&buf, name);
}

int VirtualFileSystem::fstat(int fd, struct stat* out) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  scoped_refptr<FileStream> stream = fd_to_stream_->GetStream(fd);
  if (!stream) {
    errno = EBADF;
    return -1;
  }
  const int result = stream->fstat(out);
  if (!result) {
    ALOG_ASSERT(stream->permission().IsValid(), "fd=%d pathname=%s stream=%s",
                fd, stream->pathname().c_str(), stream->GetStreamType());
    FillPermissionInfoToStat(stream->permission(), out);
  }
  return result;
}

int VirtualFileSystem::fstatfs(int fd, struct statfs* out) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  scoped_refptr<FileStream> stream = fd_to_stream_->GetStream(fd);
  if (!stream) {
    errno = EBADF;
    return -1;
  }
  return stream->fstatfs(out);
}

int VirtualFileSystem::lstat(const std::string& pathname, struct stat* out) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  // Get an absolute path with parent symlinks resolved.
  std::string normalized(pathname);
  GetNormalizedPathLocked(&normalized, kResolveParentSymlinks);
  uid_t dummy = 0;
  FileSystemHandler* handler = mount_points_->GetFileSystemHandler(normalized,
                                                                   &dummy);
  if (!handler) {
    // Nothing is mounted there (e.g. /etc/localtime): as stat() says.
    errno = ENOENT;
    return -1;
  }
  // Resolve the symlink to get the length of the symlink for st_size.
  // TODO(crbug.com/335418): The resolved path is always an absolute
  // path. That means symlinks of relative paths are not handled correctly.
  std::string resolved;
  const int old_errono = errno;
  if (handler->readlink(normalized, &resolved) < 0) {
    errno = old_errono;
    return StatLocked(normalized, out);
  }

  memset(out, 0, sizeof(*out));
  // Use the private function GetInodeUncheckedLocked to bypass the
  // IsNormalizedPathLocked() check in the public version. Passing a path name
  // which is a symlink to a file (i.e. not normalized) to
  // GetInodeUncheckedLocked() is valid since lstat() is for stat'ing the link
  // itself.
  out->st_ino = GetInodeUncheckedLocked(normalized);
  out->st_uid = arc::kRootUid;
  out->st_gid = arc::kRootGid;
  out->st_mode = S_IFLNK | 0777;
  out->st_nlink = 1;
  out->st_size = resolved.size();
  out->st_blksize = 4096;
  return 0;
}

int VirtualFileSystem::stat(const std::string& pathname, struct stat* out) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);
  return StatLocked(pathname, out);
}

int VirtualFileSystem::StatLocked(const std::string& pathname,
                                  struct stat* out) {
  mutex_.AssertAcquired();
  std::string resolved(pathname);
  GetNormalizedPathLocked(&resolved, kResolveSymlinks);
  PermissionInfo permission;
  FileSystemHandler* handler = GetFileSystemHandlerLocked(resolved,
                                                          &permission);
  if (!handler) {
    errno = ENOENT;
    return -1;
  }
  ALOG_ASSERT(permission.IsValid(), "pathname=%s handler=%s",
              pathname.c_str(), handler->name().c_str());
  const int result = handler->stat(resolved, out);
  if (!result)
    FillPermissionInfoToStat(permission, out);
  return result;
}

ssize_t VirtualFileSystem::readlink(const std::string& pathname, char* buf,
                                    size_t bufsiz) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  // Get an absolute path with parent symlinks resolved.
  std::string normalized(pathname);
  GetNormalizedPathLocked(&normalized, kResolveParentSymlinks);
  uid_t dummy = 0;
  FileSystemHandler* handler = mount_points_->GetFileSystemHandler(normalized,
                                                                   &dummy);
  if (!handler) {
    errno = ENOENT;
    return -1;
  }
  // TODO(crbug.com/335418): The resolved path is always an absolute
  // path. That means symlinks of relative paths are not handled correctly.
  std::string resolved;
  if (handler->readlink(normalized, &resolved) >= 0) {
    // Truncate if resolved path is too long.
    if (resolved.size() > bufsiz) {
      resolved.resize(bufsiz);
    }
    // readlink does not append a NULL byte to |buf|.
    memcpy(buf, resolved.data(), resolved.size());
    return resolved.size();
  }

  struct stat st;
  if (handler->stat(normalized, &st))
    errno = ENOENT;
  else
    errno = EINVAL;
  return -1;
}

int VirtualFileSystem::statfs(const std::string& pathname, struct statfs* out) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  std::string resolved(pathname);
  GetNormalizedPathLocked(&resolved, kResolveSymlinks);
  FileSystemHandler* handler = GetFileSystemHandlerLocked(resolved, NULL);
  if (!handler) {
    errno = ENOENT;
    return -1;
  }
  return handler->statfs(resolved, out);
}

int VirtualFileSystem::statvfs(const std::string& pathname,
                               struct statvfs* out) {
  struct statfs tmp;
  int result = this->statfs(pathname, &tmp);
  if (result != 0)
    return result;
  out->f_bsize = tmp.f_bsize;
  out->f_frsize = tmp.f_bsize;
  out->f_blocks = tmp.f_blocks;
  out->f_bfree = tmp.f_bfree;
  out->f_bavail = tmp.f_bavail;
  out->f_files = tmp.f_files;
  out->f_ffree = tmp.f_ffree;
  out->f_favail = tmp.f_ffree;
  out->f_fsid = tmp.f_fsid.__val[0];
  out->f_flag = 0;
  out->f_namemax = tmp.f_namelen;

  return 0;
}

int VirtualFileSystem::ftruncate(int fd, off64_t length) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  if (length < 0) {
    errno = EINVAL;
    return -1;
  }
  scoped_refptr<FileStream> stream = fd_to_stream_->GetStream(fd);
  if (stream)
    return stream->ftruncate(length);
  errno = EBADF;
  return -1;
}

int VirtualFileSystem::getdents(int fd, dirent* buf, size_t count) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  scoped_refptr<FileStream> stream = fd_to_stream_->GetStream(fd);
  if (stream)
    return stream->getdents(buf, count);
  errno = EBADF;
  return -1;
}

int VirtualFileSystem::fcntl(int fd, int cmd, va_list ap) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  scoped_refptr<FileStream> stream = fd_to_stream_->GetStream(fd);
  if (stream && (cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC)) {
    // arc-wasm: dup to the lowest free descriptor >= the argument
    // (libcore 8.1's FileDescriptor dups stdin/stdout/stderr this way).
    // close-on-exec means nothing here: there is no exec().
    int newfd = va_arg(ap, int);
    if (newfd < 0) {
      errno = EINVAL;
      return -1;
    }
    if (newfd < fd_to_stream_->min_file_id())
      newfd = fd_to_stream_->min_file_id();
    while (newfd <= fd_to_stream_->max_file_id() && fd_to_stream_->IsKnownDescriptor(newfd))
      ++newfd;
    if (newfd > fd_to_stream_->max_file_id()) {
      errno = EMFILE;
      return -1;
    }
    return DupLocked(fd, newfd);
  }
  if (stream) {
    return stream->fcntl(cmd, ap);
  } else if (fd_to_stream_->IsKnownDescriptor(fd)) {
    // Socket with reserved FD but not allocated yet, for now just ignore.
    ALOGW("Ignoring fcntl() on file %d", fd);
    return 0;
  } else {
    errno = EBADF;
    return -1;
  }
}

int VirtualFileSystem::fdatasync(int fd) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  scoped_refptr<FileStream> stream = fd_to_stream_->GetStream(fd);
  if (stream)
    return stream->fdatasync();
  errno = EBADF;
  return -1;
}

int VirtualFileSystem::fsync(int fd) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  scoped_refptr<FileStream> stream = fd_to_stream_->GetStream(fd);
  if (stream)
    return stream->fsync();
  errno = EBADF;
  return -1;
}

int VirtualFileSystem::ioctl(int fd, int request, va_list ap) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  scoped_refptr<FileStream> stream = fd_to_stream_->GetStream(fd);
  // arc-wasm: FIOCLEX/FIONCLEX work on every descriptor, as F_SETFD (no
  // exec here). CPython's set_inheritable tries them first and takes any
  // error but ENOTTY/EACCES as fatal (python-for-android: no 'encodings').
  if (stream && (request == FIOCLEX || request == FIONCLEX))
    return 0;
  if (stream) {
    return stream->ioctl(request, ap);
  } else {
    errno = EBADF;
    return -1;
  }
}

void VirtualFileSystem::CreateSocketFileLocked(const std::string& path) {
  mutex_.AssertAcquired();
  int saved_errno = errno;
  int fd = OpenLocked(path, O_WRONLY | O_CREAT, 0777, false);
  if (fd >= 0)
    CloseLocked(fd);
  errno = saved_errno;
}

bool VirtualFileSystem::PathExistsLocked(const std::string& path) {
  mutex_.AssertAcquired();
  int saved_errno = errno;
  struct stat st;
  const bool exists = StatLocked(path, &st) == 0;
  errno = saved_errno;
  return exists;
}

int VirtualFileSystem::GetFirstUnusedDescriptorLocked() {
  mutex_.AssertAcquired();
  return fd_to_stream_->GetFirstUnusedDescriptor();
}

int VirtualFileSystem::OpenLocked(
    const std::string& pathname, int oflag, mode_t cmode, bool use_preopened) {
  mutex_.AssertAcquired();
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  // Crash early when called from the main thread.
  // Otherwise this function can succeed even if called from the main thread
  // if the file is in preopen cache.
  LOG_ALWAYS_FATAL_IF(pp::Module::Get()->core()->IsMainThread());

  // Linux kernel also accepts 'O_RDONLY|O_TRUNC' and truncates the file. Even
  // though pp::FileIO seems to refuse 'O_RDONLY|O_TRUNC', show a warning here.
  if (((oflag & O_ACCMODE) == O_RDONLY) && (oflag & O_TRUNC))
    ALOGW("O_RDONLY|O_TRUNC is specified for %s", pathname.c_str());

  std::string resolved(pathname);
  GetNormalizedPathLocked(&resolved, kResolveSymlinks);

  if (use_preopened && IsEligibleForPreopen(oflag)) {
    while (true) {
      PreopenedFdMultimap::iterator it = preopened_fds_.lower_bound(resolved);
      if (it == preopened_fds_.end() || it->first != resolved) {
        break;
      }
      if (it->second != kPreopenPendingFd) {
        int fd = it->second;
        preopened_fds_.erase(it);
        return fd;
      }
      // Wait until the file is opened by PerformPreopen().
      ALOGW("preopen: Waiting for slow preopen: %s", resolved.c_str());
      Wait();
    }
  }

  PermissionInfo permission;
  FileSystemHandler* handler = GetFileSystemHandlerLocked(resolved,
                                                          &permission);
  if (!handler) {
    errno = ENOENT;
    return -1;
  }
  ALOG_ASSERT(permission.IsValid(), "pathname=%s handler=%s",
              pathname.c_str(), handler->name().c_str());
  // Linux kernel accepts both 'O_RDONLY|O_CREAT' and 'O_RDONLY|O_TRUNC'.
  // If the directory is not writable, the request should be denied.
  if (((oflag & O_ACCMODE) != O_RDONLY || (oflag & (O_CREAT | O_TRUNC))) &&
      !permission.is_writable()) {
    if (oflag & O_CREAT) {
      struct stat st;
      if (!handler->stat(resolved, &st)) {
        // TODO(crbug.com/463434): Re-enable L's CTS tests once the EISDIR fix
        // is merged into ARC's L branch.
        if (S_ISDIR(st.st_mode)) {
          // When O_CREAT is specified, Linux kernel prefers EISDIR over EACCES
          // for directories. Emulate the behavior.
          errno = EISDIR;
          return -1;
        } else if (oflag & O_EXCL) {
          // When O_CREAT|O_EXCL is specified, Linux kernel prefers EEXIST
          // over EACCES. Emulate the behavior too.
          errno = EEXIST;
          return -1;
        }
      }
      return DenyAccessForCreateLocked(&resolved, handler);
    } else {
      return DenyAccessForModifyLocked(resolved, handler);
    }
  }
  int fd = GetFirstUnusedDescriptorLocked();
  if (fd < 0) {
    errno = EMFILE;
    return -1;
  }
  scoped_refptr<FileStream> stream = handler->open(fd, resolved, oflag, cmode);
  if (!stream) {
    ALOG_ASSERT(errno > 0, "pathname=%s, handler=%s",
                pathname.c_str(), handler->name().c_str());
    fd_to_stream_->RemoveFileStream(fd);
    return -1;
  }
  stream->set_permission(permission);
  fd_to_stream_->AddFileStream(fd, stream);
  if (!IsEligibleForPreopen(oflag)) {
    // We must invalidate the preopen cache when a file is opened with
    // non-eligible flags. For example, in this scenario, we must invalidate
    // the cache at the step (2):
    //
    // 1. File A is attempted to preopen, but file A does not exist, so error
    //    file descriptor is cached.
    // 2. File A is opened for write with O_CREAT. File A is now created.
    // 3. File A is opened for read. If preopen is disabled, this would
    //    succeed by opening a file created at (2). If preopen is enabled,
    //    we should not reuse the error file descriptor cached at (1).
    ClosePreopenedFilesWithResolvedPathLocked(resolved);
  }
  if (use_preopened && IsEligibleForPreopen(oflag) &&
      strcmp(stream->GetStreamType(), "pepper") == 0) {
    ALOGI("preopen: candidate: %s", resolved.c_str());
  }
  return fd;
}

int VirtualFileSystem::IsSelectReadyLocked(int nfds, fd_set* fds,
                                           SelectReadyEvent event,
                                           bool apply) {
  mutex_.AssertAcquired();
  if (!fds)
    return 0;

  int nset = 0;
  for (int i = 0; i < nfds; i++) {
    if (!FD_ISSET(i, fds))
      continue;

    scoped_refptr<FileStream> stream = fd_to_stream_->GetStream(i);
    if (!stream)
      continue;

    bool is_ready;
    if (event == SELECT_READY_READ) {
      is_ready = stream->IsSelectReadReady();
    } else if (event == SELECT_READY_WRITE) {
      is_ready = stream->IsSelectWriteReady();
    } else {  // SELECT_READY_EXCEPTION
      is_ready = stream->IsSelectExceptionReady();
    }

    if (is_ready) {
      if (!apply)
        return 1;

      ARC_STRACE_REPORT("select ready: fd=%d, event=%s", i,
                          event == SELECT_READY_READ ? "read" :
                          event == SELECT_READY_WRITE ? "write" :
                          "exception");
      nset++;
    } else {
      if (apply)
        FD_CLR(i, fds);
    }
  }
  return nset;
}

int VirtualFileSystem::select(int nfds, fd_set* readfds, fd_set* writefds,
                              fd_set* exceptfds, struct timeval* timeout) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  // If timeout is set and it's 0, it means just a polling.
  const bool is_polling =
      (timeout && timeout->tv_sec == 0 && timeout->tv_usec == 0);
  if (!is_polling) {
    // If timeout is NULL, use base::TimeTicks(), which lets WaitUntil block
    // indefinitely.
    const base::TimeTicks time_limit = timeout ?
        internal::TimeOutToTimeLimit(internal::TimeValToTimeDelta(*timeout)) :
        base::TimeTicks();
    while (!(IsSelectReadyLocked(
                 nfds, readfds, SELECT_READY_READ, false) ||
             IsSelectReadyLocked(
                 nfds, writefds, SELECT_READY_WRITE, false) ||
             IsSelectReadyLocked(
                 nfds, exceptfds, SELECT_READY_EXCEPTION, false))) {
      if (WaitUntil(time_limit)) {
        // timedout, or spurious wakeup, or real wakeup. Either way, we can
        // just break since |timeout| has expired.
        break;
      }
    }

    // Linux always updates |timeout| while POSIX does not require it. Emulate
    // the behavior.
    if (timeout) {
      const base::TimeTicks end_time = base::TimeTicks::Now();
      const base::TimeDelta remaining_time =
          time_limit <= end_time ? base::TimeDelta() : time_limit - end_time;
      ARC_STRACE_REPORT(
          "new_timeout={ %lld ms }, original_timeout={ %lld s, %lld us }",
          static_cast<int64_t>(remaining_time.InMilliseconds()),
          static_cast<int64_t>(timeout->tv_sec),
          static_cast<int64_t>(timeout->tv_usec));
      *timeout = internal::TimeDeltaToTimeVal(remaining_time);
    }
  }

  int nread =
      IsSelectReadyLocked(nfds, readfds, SELECT_READY_READ, true);
  int nwrite =
      IsSelectReadyLocked(nfds, writefds, SELECT_READY_WRITE, true);
  int nexcpt =
      IsSelectReadyLocked(nfds, exceptfds, SELECT_READY_EXCEPTION, true);
  if (nread < 0 || nwrite < 0 || nexcpt < 0) {
    errno = EBADF;
    return -1;
  }
  return nread + nwrite + nexcpt;
}


int VirtualFileSystem::getaddrinfo(const char* hostname, const char* servname,
                                   const addrinfo* hints, addrinfo** res) {
  TRACE_EVENT1(ARC_TRACE_CATEGORY, "VirtualFileSystem::getaddrinfo",
               "hostname", std::string(hostname));
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);
  return host_resolver_.getaddrinfo(hostname, servname, hints, res);
}

void VirtualFileSystem::freeaddrinfo(addrinfo* ai) {
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);
  return host_resolver_.freeaddrinfo(ai);
}

struct hostent* VirtualFileSystem::gethostbyname(const char* host) {
  return host_resolver_.gethostbyname(host);
}

struct hostent* VirtualFileSystem::gethostbyname2(
    const char* host, int family) {
  return host_resolver_.gethostbyname2(host, family);
}

int VirtualFileSystem::gethostbyname_r(
    const char* host, struct hostent* ret, char* buf, size_t buflen,
    struct hostent** result, int* h_errnop) {
  return host_resolver_.gethostbyname_r(
      host, ret, buf, buflen, result, h_errnop);
}

int VirtualFileSystem::gethostbyname2_r(
    const char* host, int family, struct hostent* ret, char* buf, size_t buflen,
    struct hostent** result, int* h_errnop) {
  return host_resolver_.gethostbyname2_r(
      host, family, ret, buf, buflen, result, h_errnop);
}

struct hostent* VirtualFileSystem::gethostbyaddr(
    const void* addr, socklen_t len, int type) {
  return host_resolver_.gethostbyaddr(addr, len, type);
}

int VirtualFileSystem::getnameinfo(const sockaddr* sa, socklen_t salen,
                                   char* host, size_t hostlen,
                                   char* serv, size_t servlen, int flags) {
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);
  return host_resolver_.getnameinfo(
      sa, salen, host, hostlen, serv, servlen, flags);
}

int VirtualFileSystem::socket(int socket_family, int socket_type,
                              int protocol) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  int fd = GetFirstUnusedDescriptorLocked();
  if (fd < 0) {
    errno = EMFILE;
    return -1;
  }

  bool is_inet = (socket_family == AF_INET || socket_family == AF_INET6);
  scoped_refptr<FileStream> socket;
  if (is_inet && socket_type == SOCK_DGRAM) {
    socket = new UDPSocket(fd, socket_family, 0);
  } else if (is_inet && socket_type == SOCK_STREAM) {
    socket = new TCPSocket(fd, socket_family, O_RDWR);
  } else if (socket_family == AF_UNIX && socket_type == SOCK_STREAM) {
    socket = new LocalSocket(O_RDWR, socket_type, LocalSocket::READ_WRITE);
  } else if (socket_family == AF_UNIX && socket_type == SOCK_SEQPACKET) {
    socket = new LocalSocket(O_RDWR, socket_type, LocalSocket::READ_WRITE);
  } else if (socket_family == AF_UNIX && socket_type == SOCK_DGRAM) {
    socket = new LocalSocket(O_RDWR, socket_type, LocalSocket::READ_WRITE);
  } else {
    fd_to_stream_->RemoveFileStream(fd);
    // Only supporting SOCK_DGRAM and SOCK_STREAM right now. Fail otherwise.
    ALOGE("Request for unknown socket type %d, family=%d, protocol=%d",
          socket_type, socket_family, protocol);
    errno = EAFNOSUPPORT;
    return -1;
  }
  fd_to_stream_->AddFileStream(fd, socket);
  // Since this function does not call GetFileSystemHandlerLocked(), call
  // REPORT_HANDLER() explicitly to make STATS in arc_strace.txt easier
  // to read.
  ARC_STRACE_REPORT_HANDLER(socket->GetStreamType());
  return fd;
}

int VirtualFileSystem::socketpair(int socket_family, int socket_type,
                                  int protocol, int sv[2]) {
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  if (socket_family != AF_UNIX) {
    errno = EAFNOSUPPORT;
    return -1;
  }
  if (protocol != 0) {
    errno = EOPNOTSUPP;
    return -1;
  }
  if (socket_type != SOCK_SEQPACKET && socket_type != SOCK_STREAM &&
      socket_type != SOCK_DGRAM) {
    errno = EOPNOTSUPP;
    return -1;
  }
  if (sv == NULL) {
    errno = EFAULT;
    return -1;
  }
  base::AutoLock lock(mutex_);
  int fd1 = GetFirstUnusedDescriptorLocked();
  if (fd1 < 0) {
    errno = EMFILE;
    return -1;
  }
  int fd2 = GetFirstUnusedDescriptorLocked();
  if (fd2 < 0) {
    fd_to_stream_->RemoveFileStream(fd1);
    errno = EMFILE;
    return -1;
  }
  scoped_refptr<LocalSocket> sock1 = new LocalSocket(
      0, socket_type, LocalSocket::READ_WRITE);
  scoped_refptr<LocalSocket> sock2 = new LocalSocket(
      0, socket_type, LocalSocket::READ_WRITE);
  sock1->set_peer(sock2);
  sock2->set_peer(sock1);
  fd_to_stream_->AddFileStream(fd1, sock1);
  fd_to_stream_->AddFileStream(fd2, sock2);
  sv[0] = fd1;
  sv[1] = fd2;
  ARC_STRACE_REPORT_HANDLER(sock1->GetStreamType());
  return 0;
}

int VirtualFileSystem::connect(int fd, const sockaddr* serv_addr,
                               socklen_t addrlen) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  scoped_refptr<FileStream> stream = fd_to_stream_->GetStream(fd);
  if (!stream) {
    errno = EBADF;
    return -1;
  }
  return stream->connect(serv_addr, addrlen);
}

int VirtualFileSystem::shutdown(int fd, int how) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  scoped_refptr<FileStream> stream = fd_to_stream_->GetStream(fd);
  if (stream) {
    // TODO(http://crbug.com/318921): Actually shutdown should be something
    // more complicated but for now it works.
    return 0;
  } else {
    errno = EBADF;
    return -1;
  }
}

int VirtualFileSystem::bind(int fd, const sockaddr* addr, int addrlen) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  scoped_refptr<FileStream> stream = fd_to_stream_->GetStream(fd);
  if (!stream) {
    errno = EBADF;
    return -1;
  }
  return stream->bind(addr, addrlen);
}

int VirtualFileSystem::SetupProcessFds(int pid, const int fds[3]) {
  base::AutoLock lock(mutex_);
  scoped_refptr<FileStream> stdio[3];
  for (int i = 0; i < 3; ++i) {
    if (fds[i] < 0)
      continue;
    stdio[i] = fd_to_stream_->GetStream(fds[i]);
    if (!stdio[i]) {
      errno = EBADF;
      return -1;
    }
  }
  fd_to_stream_->SetProcessView(pid, stdio);
  return 0;
}

void VirtualFileSystem::ReleaseProcessFds(int pid) {
  base::AutoLock lock(mutex_);
  const std::vector<int> left = fd_to_stream_->RemoveProcessView(pid);
  for (size_t i = 0; i < left.size(); ++i)
    CloseLocked(left[i]);
}

// arc-wasm (topic native-exec): the mode goes to the file's handler, which
// may keep it (OpfsHandler: File.setExecutable, chmod +x) or, by default,
// only checks that the file exists (every file is the VM's).
int VirtualFileSystem::chmod(const std::string& pathname, mode_t mode) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  std::string resolved(pathname);
  GetNormalizedPathLocked(&resolved, kResolveSymlinks);
  FileSystemHandler* handler = GetFileSystemHandlerLocked(resolved, NULL);
  if (!handler) {
    errno = ENOENT;
    return -1;
  }
  return handler->chmod(resolved, mode);
}

int VirtualFileSystem::fchmod(int fd, mode_t mode) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  scoped_refptr<FileStream> stream = fd_to_stream_->GetStream(fd);
  if (!stream) {
    errno = EBADF;
    return -1;
  }
  // Pipes, sockets and other unnamed streams, or a file unlinked since it
  // was opened: nothing to keep.
  const std::string pathname = stream->pathname();
  if (pathname.empty() || pathname[0] != '/')
    return 0;
  FileSystemHandler* handler = GetFileSystemHandlerLocked(pathname, NULL);
  const int saved_errno = errno;
  if (!handler || handler->chmod(pathname, mode) != 0) {
    errno = saved_errno;
    return 0;
  }
  return 0;
}

int VirtualFileSystem::chown(const std::string& path, uid_t owner,
                             gid_t group) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  if (arc::IsAppUid(arc::ProcessEmulator::GetUid())) {
    errno = EPERM;
    return -1;
  }
  std::string resolved(path);
  GetNormalizedPathLocked(&resolved, kResolveSymlinks);

  struct stat st;
  if (StatLocked(path, &st) != 0) {
    // All errno values except this one are valid as the errno of chown.
    ALOG_ASSERT(errno != EOVERFLOW);
    return -1;
  }

  if (S_ISDIR(st.st_mode) && !util::EndsWithSlash(path))
    mount_points_->ChangeOwner(path + '/', owner);
  else
    mount_points_->ChangeOwner(path, owner);

  return 0;
}

int VirtualFileSystem::listen(int sockfd, int backlog) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  scoped_refptr<FileStream> stream = fd_to_stream_->GetStream(sockfd);
  if (!stream) {
    errno = EBADF;
    return -1;
  }
  return stream->listen(backlog);
}

int VirtualFileSystem::accept(int sockfd, sockaddr* addr, socklen_t* addrlen) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  scoped_refptr<FileStream> stream = fd_to_stream_->GetStream(sockfd);
  if (!stream) {
    errno = EBADF;
    return -1;
  }
  return stream->accept(addr, addrlen);
}

int VirtualFileSystem::getpeername(int sockfd, sockaddr* name,
                                   socklen_t* namelen) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  scoped_refptr<FileStream> stream = fd_to_stream_->GetStream(sockfd);
  if (!stream) {
    errno = EBADF;
    return -1;
  }
  return stream->getpeername(name, namelen);
}

int VirtualFileSystem::getsockname(int sockfd, sockaddr* name,
                                   socklen_t* namelen) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  scoped_refptr<FileStream> stream = fd_to_stream_->GetStream(sockfd);
  if (!stream) {
    errno = EBADF;
    return -1;
  }
  return stream->getsockname(name, namelen);
}

ssize_t VirtualFileSystem::send(int sockfd, const void* buf, size_t len,
                                int flags) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  scoped_refptr<FileStream> stream = fd_to_stream_->GetStream(sockfd);
  if (!stream) {
    errno = EBADF;
    return -1;
  }
  return stream->send(buf, len, flags);
}

ssize_t VirtualFileSystem::sendto(
    int sockfd, const void* buf, size_t len, int flags,
    const sockaddr* dest_addr, socklen_t addrlen) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  scoped_refptr<FileStream> stream = fd_to_stream_->GetStream(sockfd);
  if (!stream) {
    errno = EBADF;
    return -1;
  }
  return stream->sendto(buf, len, flags, dest_addr, addrlen);
}

int VirtualFileSystem::sendmsg(
    int sockfd, const struct msghdr* msg, int flags) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  scoped_refptr<FileStream> stream = fd_to_stream_->GetStream(sockfd);
  if (!stream) {
    errno = EBADF;
    return -1;
  }
  return stream->sendmsg(msg, flags);
}

ssize_t VirtualFileSystem::recv(int sockfd, void* buf, size_t len, int flags) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  scoped_refptr<FileStream> stream = fd_to_stream_->GetStream(sockfd);
  if (!stream) {
    errno = EBADF;
    return -1;
  }
  return stream->recv(buf, len, flags);
}

ssize_t VirtualFileSystem::recvfrom(
    int sockfd, void* buffer, size_t len, int flags, sockaddr* addr,
    socklen_t* addrlen) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  scoped_refptr<FileStream> stream = fd_to_stream_->GetStream(sockfd);
  if (!stream) {
    errno = EBADF;
    return -1;
  }
  return stream->recvfrom(buffer, len, flags, addr, addrlen);
}

int VirtualFileSystem::recvmsg(
    int sockfd, struct msghdr* msg, int flags) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  scoped_refptr<FileStream> stream = fd_to_stream_->GetStream(sockfd);
  if (!stream) {
    errno = EBADF;
    return -1;
  }
  return stream->recvmsg(msg, flags);
}

int VirtualFileSystem::getsockopt(int sockfd, int level, int optname,
                                  void* optval, socklen_t* optlen) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  scoped_refptr<FileStream> stream = fd_to_stream_->GetStream(sockfd);
  if (stream)
    return stream->getsockopt(level, optname, optval, optlen);
  errno = EBADF;
  return -1;
}

int VirtualFileSystem::setsockopt(int sockfd, int level, int optname,
                                  const void* optval, socklen_t optlen) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  scoped_refptr<FileStream> stream = fd_to_stream_->GetStream(sockfd);
  if (stream)
    return stream->setsockopt(level, optname, optval, optlen);
  errno = EBADF;
  return -1;
}

int VirtualFileSystem::pipe2(int pipefd[2], int flags) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  int read_fd = GetFirstUnusedDescriptorLocked();
  if (read_fd < 0) {
    errno = EMFILE;
    return -1;
  }
  int write_fd = GetFirstUnusedDescriptorLocked();
  if (write_fd < 0) {
    fd_to_stream_->RemoveFileStream(read_fd);
    errno = EMFILE;
    return -1;
  }
  scoped_refptr<LocalSocket> read_sock = new LocalSocket(
      flags, SOCK_STREAM, LocalSocket::READ_ONLY);
  scoped_refptr<LocalSocket> write_sock = new LocalSocket(
      flags, SOCK_STREAM, LocalSocket::WRITE_ONLY);
  read_sock->set_peer(write_sock);
  write_sock->set_peer(read_sock);
  fd_to_stream_->AddFileStream(read_fd, read_sock);
  fd_to_stream_->AddFileStream(write_fd, write_sock);
  pipefd[0] = read_fd;
  pipefd[1] = write_fd;
  // Since this function does not call GetFileSystemHandlerLocked(), call
  // REPORT_HANDLER() explicitly to make STATS in arc_strace.txt easier
  // to read.
  ARC_STRACE_REPORT_HANDLER(read_sock->GetStreamType());
  return 0;
}

int VirtualFileSystem::mkdir(const std::string& pathname, mode_t mode) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  std::string resolved(pathname);
  GetNormalizedPathLocked(&resolved, kResolveSymlinks);
  PermissionInfo permission;
  FileSystemHandler* handler = GetFileSystemHandlerLocked(resolved,
                                                          &permission);
  if (!handler) {
    errno = ENOENT;
    return -1;
  }
  if (!permission.is_writable()) {
    struct stat st;
    if (!handler->stat(resolved, &st)) {
      errno = EEXIST;
      return -1;
    }
    return DenyAccessForCreateLocked(&resolved, handler);
  }
  return handler->mkdir(resolved, mode);
}

int VirtualFileSystem::access(const std::string& pathname, int mode) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  struct stat st;
  int result = StatLocked(pathname, &st);
  if (result) {
    // All other errno from stat is compatible with access.
    ALOG_ASSERT(errno != EOVERFLOW);
    return -1;
  }

  // Apps cannot modify files owned by system unless it is explicitly
  // allowed.
  if ((mode & W_OK) && !(st.st_mode & S_IWOTH) &&
      arc::IsAppUid(arc::ProcessEmulator::GetUid()) &&
      !arc::IsAppUid(st.st_uid)) {
    errno = EACCES;
    return -1;
  }
  // Check for the exec bit.
  if (mode & X_OK) {
    if (!(st.st_mode & S_IXUSR)) {
      errno = EACCES;
      return -1;
    }
    // arc-wasm: no check of the owner here (ARC asserted an app's file or
    // one everyone may run): every file is the VM's, and a mode kept by
    // chmod (OpfsHandler, topic native-exec) may say 0700 for root's.
  }
  // There are no restrictions for read access in ARC. Files without S_IWUSR
  // exist (arc-wasm: /proc/meminfo is 0444, as on Linux; Subway Surfers
  // asks whether it can read it): writing them is refused.
  if ((mode & W_OK) && !(st.st_mode & S_IWUSR)) {
    errno = EACCES;
    return -1;
  }
  return 0;
}

int VirtualFileSystem::remove(const std::string& pathname) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  std::string resolved(pathname);
  // Use kResolveParentSymlinks rather than kResolveSymlinks because
  // man 2 remove says "If the name referred to a symbolic link, the link is
  // removed."
  GetNormalizedPathLocked(&resolved, kResolveParentSymlinks);

  PermissionInfo permission;
  FileSystemHandler* handler = GetFileSystemHandlerLocked(resolved,
                                                          &permission);
  if (!handler) {
    errno = ENOENT;
    return -1;
  }
  if (!permission.is_writable())
    return DenyAccessForModifyLocked(resolved, handler);

  ClosePreopenedFilesWithResolvedPathLocked(resolved);

  return handler->remove(resolved);
}

int VirtualFileSystem::rename(const std::string& oldpath,
                              const std::string& newpath) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  // man 2 rename says "If oldpath refers to a symbolic link, the link is
  // renamed; if newpath refers to a symbolic link the link will be
  // overwritten." arc-wasm: kResolveParentSymlinks for both paths, as
  // remove() and unlink() (symlinks kept on OPFS, topic native-exec).

  std::string resolved_oldpath(oldpath);
  GetNormalizedPathLocked(&resolved_oldpath, kResolveParentSymlinks);
  PermissionInfo permission_old;
  FileSystemHandler* handler = GetFileSystemHandlerLocked(
      resolved_oldpath, &permission_old);
  if (!handler) {
    errno = ENOENT;
    return -1;
  }
  std::string resolved_newpath(newpath);
  GetNormalizedPathLocked(&resolved_newpath, kResolveParentSymlinks);
  PermissionInfo permission_new;
  FileSystemHandler* another_handler = GetFileSystemHandlerLocked(
      resolved_newpath, &permission_new);
  if (!another_handler) {
    errno = ENOENT;
    return -1;
  }
  if (handler != another_handler) {
    errno = EXDEV;
    return -1;
  }

  if (resolved_newpath == resolved_oldpath) {
    // Renaming to the same path should be successfully done, if it exists.
    // To check its existence, call stat here. Note that this operation should
    // succeed, even if it is readonly.
    struct stat st;
    int result = StatLocked(resolved_newpath, &st);
    ALOG_ASSERT(errno != EOVERFLOW);
    return result;
  }

  if (!permission_old.is_writable() || !permission_new.is_writable()) {
    DenyAccessForModifyLocked(resolved_oldpath, handler);
    const int oldpath_errno = errno;
    ALOG_ASSERT(oldpath_errno == ENOENT || oldpath_errno == ENOTDIR ||
                oldpath_errno == EACCES);
    DenyAccessForCreateLocked(&resolved_newpath, handler);
    const int newpath_errno = errno;
    ALOG_ASSERT(newpath_errno == ENOENT || newpath_errno == ENOTDIR ||
                newpath_errno == EACCES);
    // This behavior is compatible with ext4. ENOTDIR is preferred to
    // ENOENT, which is preferred to EACCES.
    if (oldpath_errno == ENOTDIR || newpath_errno == ENOTDIR) {
      errno = ENOTDIR;
      return -1;
    }
    if (oldpath_errno == ENOENT || newpath_errno == ENOENT) {
      errno = ENOENT;
      return -1;
    }
    errno = EACCES;
    return -1;
  }

  ClosePreopenedFilesWithResolvedPathLocked(resolved_oldpath);
  ClosePreopenedFilesWithResolvedPathLocked(resolved_newpath);

  return handler->rename(resolved_oldpath, resolved_newpath);
}

int VirtualFileSystem::rmdir(const std::string& pathname) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  std::string resolved(pathname);
  GetNormalizedPathLocked(&resolved, kResolveSymlinks);
  PermissionInfo permission;
  FileSystemHandler* handler = GetFileSystemHandlerLocked(resolved,
                                                          &permission);
  if (!handler) {
    errno = ENOENT;
    return -1;
  }
  if (!permission.is_writable())
    return DenyAccessForModifyLocked(resolved, handler);
  return handler->rmdir(resolved);
}

int VirtualFileSystem::symlink(const std::string& oldpath,
                               const std::string& newpath) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  // arc-wasm: the link itself is made, not what an existing |newpath| points
  // to (that is EEXIST): parent symlinks only (topic native-exec).
  std::string resolved_newpath(newpath);
  GetNormalizedPathLocked(&resolved_newpath, kResolveParentSymlinks);

  // TODO(crbug.com/423063): The current logic seems good enough for handling
  // the test cases added in 021377bc and for handling two
  // android.system.Os.symlink calls for setting up /data/app-lib/<appname> and
  // /vendor/lib-<arch>. However, there are lots of wrong assumptions like
  // |parent| and |newpath| are handled by the same handler. If we need more
  // generic symlink() functionality, we should rewrite the logic below.

  const std::string parent = util::GetDirName(resolved_newpath);
  PermissionInfo permission_new;
  FileSystemHandler* newpath_handler = GetFileSystemHandlerLocked(
      resolved_newpath, &permission_new);
  struct stat st;
  if (!newpath_handler || newpath_handler->stat(parent, &st) < 0) {
    errno = ENOENT;
    return -1;
  }

  if (!permission_new.is_writable()) {
    if (!newpath_handler->stat(resolved_newpath, &st)) {
      errno = EEXIST;
      return -1;
    }
    return DenyAccessForModifyLocked(parent, newpath_handler);
  }
  return newpath_handler->symlink(oldpath, resolved_newpath);
}

int VirtualFileSystem::truncate(const std::string& pathname, off64_t length) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  std::string resolved(pathname);
  GetNormalizedPathLocked(&resolved, kResolveSymlinks);
  PermissionInfo permission;
  FileSystemHandler* handler = GetFileSystemHandlerLocked(resolved,
                                                          &permission);
  if (!handler) {
    errno = ENOENT;
    return -1;
  }
  if (!permission.is_writable())
    return DenyAccessForModifyLocked(resolved, handler);
  return handler->truncate(resolved, length);
}

mode_t VirtualFileSystem::umask(mode_t mask) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  mode_t result_umask = process_environment_->GetCurrentUmask();
  process_environment_->SetCurrentUmask(mask);
  return result_umask;
}

int VirtualFileSystem::unlink(const std::string& pathname) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  std::string resolved(pathname);
  // Use kResolveParentSymlinks rather than kResolveSymlinks because
  // man 2 unlink says "If the name referred to a symbolic link, the link is
  // removed."
  GetNormalizedPathLocked(&resolved, kResolveParentSymlinks);
  PermissionInfo permission;
  FileSystemHandler* handler = GetFileSystemHandlerLocked(resolved,
                                                          &permission);
  if (!handler) {
    errno = ENOENT;
    return -1;
  }
  if (!permission.is_writable())
    return DenyAccessForModifyLocked(resolved, handler);

  ClosePreopenedFilesWithResolvedPathLocked(resolved);

  return handler->unlink(resolved);
}

int VirtualFileSystem::utime(const std::string& pathname,
                             const struct utimbuf* times) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  std::string resolved(pathname);
  GetNormalizedPathLocked(&resolved, kResolveSymlinks);
  PermissionInfo permission;
  FileSystemHandler* handler = GetFileSystemHandlerLocked(resolved,
                                                          &permission);
  if (!handler) {
    errno = ENOENT;
    return -1;
  }
  if (!permission.is_writable())
    return DenyAccessForModifyLocked(resolved, handler);
  struct timeval t[2];
  t[0].tv_sec = times->actime;
  t[0].tv_usec = 0;
  t[1].tv_sec = times->modtime;
  t[1].tv_usec = 0;
  return handler->utimes(resolved, t);
}

int VirtualFileSystem::utimes(const std::string& pathname,
                              const struct timeval times[2]) {
  base::AutoLock lock(mutex_);
  ARC_STRACE_REPORT_HANDLER(kVirtualFileSystemHandlerStr);

  std::string resolved(pathname);
  GetNormalizedPathLocked(&resolved, kResolveSymlinks);
  PermissionInfo permission;
  FileSystemHandler* handler = GetFileSystemHandlerLocked(resolved,
                                                          &permission);
  if (!handler) {
    errno = ENOENT;
    return -1;
  }
  if (!permission.is_writable())
    return DenyAccessForModifyLocked(resolved, handler);
  return handler->utimes(resolved, times);
}

void VirtualFileSystem::Wait() {
  // Calling cond_.Wait() on main thread results in deadlock.
  ALOG_ASSERT(!pp::Module::Get()->core()->IsMainThread());
  // Chromium's Wait() automatically checks if mutex_ is locked.
  cond_.Wait();
}

bool VirtualFileSystem::WaitUntil(const base::TimeTicks& time_limit) {
  return internal::WaitUntil(&cond_, time_limit);
}

void VirtualFileSystem::Broadcast() {
  mutex_.AssertAcquired();
  cond_.Broadcast();
}

void VirtualFileSystem::SetBrowserReady() {
  base::AutoLock lock(mutex_);
  ALOG_ASSERT(!browser_ready_);
  browser_ready_ = true;
  ALOGI("VirtualFileSystem::SetBrowserReady: the browser is ready to run ARC");
  if (arc::Options::GetInstance()->GetBool("enable_preopen", false)) {
    StartPreopenLocked();
  }
  cond_.Broadcast();
}

bool VirtualFileSystem::IsBrowserReadyLocked() const {
  mutex_.AssertAcquired();
  return browser_ready_;
}

void VirtualFileSystem::Mount(const std::string& path,
                              FileSystemHandler* handler) {
  base::AutoLock lock(mutex_);
  mount_points_->Add(path, handler);
  handler->SetMountPointManager(mount_points_.get());
}

void VirtualFileSystem::Unmount(const std::string& path) {
  base::AutoLock lock(mutex_);
  mount_points_->Remove(path);
}

void VirtualFileSystem::ChangeMountPointOwner(const std::string& path,
                                              uid_t owner_uid) {
  base::AutoLock lock(mutex_);
  mount_points_->ChangeOwner(path, owner_uid);
}

bool VirtualFileSystem::IsNormalizedPathLocked(const std::string& path) {
  std::string resolved(path);
  GetNormalizedPathLocked(&resolved, kResolveSymlinks);
  if (path != "/" && util::EndsWithSlash(path))
    resolved += '/';
  return path == resolved;
}

void VirtualFileSystem::GetNormalizedPathLocked(std::string* in_out_path,
                                                NormalizeOption option) {
  mutex_.AssertAcquired();
  ALOG_ASSERT(in_out_path);

  // Handle lstat("/path/to/symlink_to_dir/.") and readdir() for "." after
  // opendir("/path/to/symlink_to_dir") cases properly.
  util::RemoveTrailingSlashes(in_out_path);
  if (option == kResolveParentSymlinks && EndsWith(*in_out_path, "/.", true))
    option = kResolveSymlinks;

  // Remove . and //.
  util::RemoveSingleDotsAndRedundantSlashes(in_out_path);
  if (in_out_path->empty())
    return;

  // If the path is relative, prepend CWD.
  if (*in_out_path == ".") {
    *in_out_path = process_environment_->GetCurrentDirectory();
    util::RemoveTrailingSlashes(in_out_path);
  } else if ((*in_out_path)[0] != '/') {
    in_out_path->insert(0, process_environment_->GetCurrentDirectory());
  }
  ALOG_ASSERT(*in_out_path == "/" || !util::EndsWithSlash(*in_out_path));

  // Resolve .. and symlinks.
  std::vector<std::string> directories;
  base::SplitString(*in_out_path, '/', &directories);
  in_out_path->clear();
  for (size_t i = 0; i < directories.size(); i++) {
    if (directories[i].empty()) {
      // Splitting "/" and "/foo" results in ["", ""] and ["", "foo"],
      // respectively.
      continue;
    }
    ALOG_ASSERT(!util::EndsWithSlash(*in_out_path), "%s", in_out_path->c_str());
    if (directories[i] == "..") {
      if (!in_out_path->empty()) {  // to properly handle "/.."
        // TODO(crbug.com/287721): Check if |*in_out_path| is a directory.
        const size_t pos = in_out_path->rfind('/');
        ALOG_ASSERT(pos != std::string::npos);
        in_out_path->resize(pos);
      }
    } else {
      in_out_path->append("/" + directories[i]);
      if (option == kResolveSymlinks ||
          (option == kResolveParentSymlinks && i != directories.size() - 1)) {
        ResolveSymlinks(in_out_path);
      }
    }
  }
  // Handles cases like "/.." and "/../".
  if (in_out_path->empty())
    in_out_path->assign("/");

  ARC_STRACE_REPORT(
      "Normalized to: %s%s", in_out_path->c_str(),
      (option == kResolveParentSymlinks ? " (parent only)" : ""));
}

int VirtualFileSystem::DenyAccessForCreateLocked(std::string* path,
                                                 FileSystemHandler* handler) {
  mutex_.AssertAcquired();
  ALOG_ASSERT(path);
  util::GetDirNameInPlace(path);
  return DenyAccessForModifyLocked(*path, handler);
}

int VirtualFileSystem::DenyAccessForModifyLocked(const std::string& path,
                                                 FileSystemHandler* handler) {
  mutex_.AssertAcquired();
  // Linux checks the existence of a file before it checks the
  // permission of it. To emulate this behavior, we will prefer errno
  // set by access to EACCES.
  struct stat st;
  if (!handler->stat(path, &st))
    errno = EACCES;
  ALOG_ASSERT(errno == ENOENT || errno == ENOTDIR || errno == EACCES);
  ARC_STRACE_REPORT("DenyAccess: path=%s errno=%d", path.c_str(), errno);
  return -1;
}

void VirtualFileSystem::StartPreopenLocked() {
  mutex_.AssertAcquired();
  LOG_ALWAYS_FATAL_IF(preopen_started_, "StartPreopen() called multiple times");
  preopen_started_ = true;

  pthread_t thread;
  pthread_attr_t thread_attr;
  pthread_attr_init(&thread_attr);
  pthread_attr_setdetachstate(&thread_attr, PTHREAD_CREATE_DETACHED);
  int thread_result = __wrap_pthread_create(
      &thread, &thread_attr, &VirtualFileSystem::PreopenThreadMain, this);
  LOG_ALWAYS_FATAL_IF(thread_result < 0, "Failed to start a preopen thread");
  pthread_attr_destroy(&thread_attr);
}

void* VirtualFileSystem::PreopenThreadMain(void* arg) {
  VirtualFileSystem* self = static_cast<VirtualFileSystem*>(arg);
  self->PerformPreopens();
  return NULL;
}

void VirtualFileSystem::PerformPreopens() {
  base::AutoLock lock(mutex_);
  ARC_STRACE_ENTER("PerformPreopens", "");
  TRACE_EVENT0(ARC_TRACE_CATEGORY, "VirtualFileSystem::PerformPreopens");
  for (const std::string& resolved : scheduled_preopens_) {
    PreopenedFdMultimap::iterator it = preopened_fds_.lower_bound(resolved);
    while (it != preopened_fds_.end() && it->first == resolved &&
           it->second != kPreopenPendingFd) {
      ++it;
    }
    // We do not call OpenLocked() if the preopen cache has already been
    // invalidated by ClosePreopenedFilesWithResolvedPathLocked().
    if (it != preopened_fds_.end() && it->first == resolved) {
      it->second = OpenLocked(resolved, 0, 0, false);
      // Wake up possibly blocking open().
      Broadcast();
    }
  }
  ARC_STRACE_RETURN_VOID();
}

void VirtualFileSystem::ClosePreopenedFilesWithResolvedPathLocked(
    const std::string& resolved_path) {
  const auto& range = preopened_fds_.equal_range(resolved_path);
  for (PreopenedFdMultimap::const_iterator it = range.first;
       it != range.second;
       ++it) {
    ALOGW("preopen: Invalidating %s. Please update the list.",
          it->first.c_str());
    if (it->second >= 0) {
      CloseLocked(it->second);
    }
  }
  preopened_fds_.erase(range.first, range.second);
}

void VirtualFileSystem::ResolveSymlinks(std::string* in_out_path) {
  // Check if |in_out_path| is a symlink.
  uid_t dummy = 0;
  FileSystemHandler* handler =
    mount_points_->GetFileSystemHandler(*in_out_path, &dummy);
  if (!handler)
    return;
  std::string resolved;
  const int old_errono = errno;
  if (handler->readlink(*in_out_path, &resolved) >= 0) {
    ALOG_ASSERT(*in_out_path != resolved);
    // arc-wasm (topic native-exec): a target relative to the link's
    // directory ("../../proc/mounts", as trees unpacked by apps have them)
    // and targets with symlinks of their own are resolved too, as Linux
    // does; after 40 links in one walk the path stays as it is (ELOOP).
    if (resolved.empty() || t_symlink_depth >= 40) {
      errno = old_errono;
      return;
    }
    if (resolved[0] != '/')
      resolved = util::GetDirName(*in_out_path) + "/" + resolved;
    ++t_symlink_depth;
    GetNormalizedPathLocked(&resolved, kResolveSymlinks);
    --t_symlink_depth;
    in_out_path->replace(0, in_out_path->length(), resolved);
    errno = old_errono;
    return;
  }
  errno = old_errono;
}

}  // namespace posix_translation
