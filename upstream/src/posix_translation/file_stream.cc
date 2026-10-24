// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Implements functions of the base file API interface.

#include "posix_translation/file_stream.h"

#include <algorithm>
#include <utility>

#include "base/memory/scoped_ptr.h"
#include "common/alog.h"
#include "posix_translation/directory_file_stream.h"
#include "posix_translation/statfs.h"
#include "posix_translation/virtual_file_system.h"

namespace posix_translation {
namespace {

// Verifies the |iov| and |iovcnt|, and returns appropriate error code
// If verification successfully passes, returns 0.
// Along with the verification, |total_size| is returned on success.
// Note that this does not modify |errno|.
int VerifyIoVec(const struct iovec* iov, int iovcnt, ssize_t* out_total_size) {
  ALOG_ASSERT(out_total_size != NULL);
  if (iovcnt < 0 || UIO_MAXIOV < iovcnt)
    return EINVAL;

  // Then check size overflow.
  ssize_t total_size = 0;
  for (int i = 0; i < iovcnt; ++i) {
    if (iov[i].iov_len > static_cast<size_t>(SSIZE_MAX - total_size))
      return EINVAL;
    total_size += iov[i].iov_len;
  }

  *out_total_size = total_size;
  return 0;
}

}  // namespace

FileStream::FileStream(int oflag, const std::string& pathname)
    : oflag_(oflag), inode_(kBadInode), pathname_(pathname),
      is_listening_enabled_(false), file_ref_count_(0),
      had_file_refs_(false) {
  // When the stream is not associated with a file (e.g. socket), |pathname|
  // is empty.
  if (!pathname_.empty()) {
    // Claim a unique inode for the pathname before the file is unlinked.
    VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
    inode_ = sys->GetInodeLocked(pathname_);
  }
}

FileStream::~FileStream() {
  // Make sure it was never properly opened, or has no remaining file refs.
  ALOG_ASSERT(!had_file_refs_ || file_ref_count_ == 0);
}

bool FileStream::IsAllowedOnMainThread() const {
  return false;
}

bool FileStream::ReturnsSameAddressForMultipleMmaps() const {
  return false;
}

bool FileStream::IsClosed() const {
  return had_file_refs_ && file_ref_count_ == 0;
}

void FileStream::CheckNotClosed() const {
  ALOG_ASSERT(!IsClosed());
}

void FileStream::AddFileRef() {
  CheckNotClosed();
  file_ref_count_++;
  had_file_refs_ = true;
}

void FileStream::ReleaseFileRef() {
  CheckNotClosed();
  ALOG_ASSERT(had_file_refs_);
  ALOG_ASSERT(file_ref_count_ > 0);

  file_ref_count_--;
  if (file_ref_count_ != 0)
    return;

  // Clear listeners first to prevent OnLastFileRef() from notifying them.
  for (FileMap::iterator it = listeners_.begin();
       it != listeners_.end(); it++) {
    it->second->HandleNotificationFrom(this, true);
  }
  listeners_.clear();

  OnLastFileRef();
}

int FileStream::accept(sockaddr* addr, socklen_t* addrlen) {
  errno = ENOTSOCK;
  return -1;
}

int FileStream::bind(const sockaddr* addr, socklen_t addrlen) {
  errno = ENOTSOCK;
  return -1;
}

int FileStream::connect(const sockaddr* addr, socklen_t addrlen) {
  errno = ENOTSOCK;
  return -1;
}

int FileStream::epoll_ctl(
    int op, scoped_refptr<FileStream> file, struct epoll_event* event) {
  errno = EINVAL;
  return -1;
}

int FileStream::epoll_wait(struct epoll_event* events, int maxevents,
                           int timeout) {
  errno = EINVAL;
  return -1;
}

int FileStream::fcntl(int cmd, va_list ap) {
  switch (cmd) {
    case F_GETFD:
    case F_SETFD:
      // Ignore since we do not support exec().
      return 0;
    case F_GETLK: {
      struct flock* lk = va_arg(ap, struct flock*);
      if (lk) {
        memset(lk, 0, sizeof(struct flock));
        lk->l_type = F_UNLCK;
      }
      return 0;
    }
    case F_SETLK:
    case F_SETLKW:
#if F_SETLK64 != F_SETLK  // arc-wasm: musl aliases the *64 commands.
    case F_SETLK64:
    case F_SETLKW64:
#endif
      return 0;
    case F_GETFL:
      // TODO(yusukes): Exclude file creation flags.
      return oflag();
    case F_SETFL:
      set_oflag(va_arg(ap, long));  // NOLINT(runtime/int)
      return 0;
  }
  errno = EINVAL;
  return -1;
}

int FileStream::fdatasync() {
  return 0;
}

int FileStream::fstat(struct stat* out) {
  memset(out, 0, sizeof(struct stat));
  return 0;
}

int FileStream::fstatfs(struct statfs* out) {
  // Best effort guess.
  return DoStatFsForData(out);
}

int FileStream::fsync() {
  return 0;
}

int FileStream::ftruncate(off64_t length) {
  errno = EINVAL;
  return -1;
}

int FileStream::getdents(dirent* buf, size_t count) {
  errno = ENOTDIR;
  return -1;
}

int FileStream::getpeername(sockaddr* name, socklen_t* namelen) {
  errno = ENOTSOCK;
  return -1;
}

int FileStream::getsockname(sockaddr* name, socklen_t* namelen) {
  errno = ENOTSOCK;
  return -1;
}

int FileStream::getsockopt(int level, int optname, void* optval,
                           socklen_t* optlen) {
  errno = ENOTSOCK;
  return -1;
}

int FileStream::ioctl(int request, va_list ap) {
  errno = ENOTTY;  // arc-wasm: as Linux: the request does not apply to this file
  return -1;
}

int FileStream::listen(int backlog) {
  errno = ENOTSOCK;
  return -1;
}

off64_t FileStream::lseek(off64_t offset, int whence) {
  // There is no good default error code for most files. Sockets should
  // return ESPIPE, but there is no documented errno for non-seekable files.
  errno = EINVAL;
  return -1;
}

int FileStream::madvise(void* addr, size_t length, int advice) {
  // Accept advices that are supported, or do not have visible side effects.
  switch (advice) {
    case MADV_NORMAL:
    case MADV_RANDOM:
    case MADV_SEQUENTIAL:
    case MADV_WILLNEED:
    case MADV_SOFT_OFFLINE:
    case MADV_MERGEABLE:
    case MADV_UNMERGEABLE:
    case MADV_NOHUGEPAGE:
      // Theese advices can be ignored safely.
      break;
    case MADV_DONTNEED:
      // Has a FileStream dependent side affects. Should be handlded by
      // inheritance.
      // TODO(crbug.com/425955): Only PassthroughStream and DevAshmem have
      // implementation. If needed, implement this function for other streams.
      errno = EINVAL;
      return -1;
    case MADV_REMOVE:
      // Linux supports it only on shmfs/tmpfs.
      errno = ENOSYS;
      return -1;
    case MADV_DONTFORK:
      // Contrary to the madvise(2) man page, MADV_DONTFORK does influence the
      // semantics of the application. MADV_DONTFORK'ed pages must not be
      // available to the child process, and if the process touches the page,
      // it must crash. Returning 0 for now since we do not support fork().
      ALOGE("MADV_DONTFORK for address %p is ignored.", addr);
      break;
    case MADV_DOFORK:
      // The same. Write an error message just in case.
      ALOGE("MADV_DOFORK for address %p is ignored.", addr);
      break;
    default:
      // Handle an unknown advice, and MADV_HWPOISON, MADV_HUGEPAGE, and
      // MADV_DONTDUMP that are not defined in NaCl.
      errno = EINVAL;
      return -1;
  }
  return 0;
}

void* FileStream::mmap(
    void* addr, size_t length, int prot, int flags, off_t offset) {
  errno = ENODEV;
  return MAP_FAILED;
}

int FileStream::mprotect(void* addr, size_t length, int prot) {
  return ::mprotect(addr, length, prot);
}

int FileStream::munmap(void* addr, size_t length) {
  errno = ENODEV;
  return -1;
}

ssize_t FileStream::pread(void* buf, size_t count, off64_t offset) {
  // Implementing pread with lseek-lseek-read-lseek is somewhat slow but works
  // thanks to the giant mutex lock in VirtualFileSystem.
  // TODO(crbug.com/269075): Switch to pread IRT once it is implemented for
  // better performance.
  const off64_t original = this->lseek(0, SEEK_CUR);
  if (original == -1)
    return -1;
  if (this->lseek(offset, SEEK_SET) == -1)
    return -1;
  const ssize_t result = this->read(buf, count);
  const off64_t now = this->lseek(original, SEEK_SET);
  ALOG_ASSERT(original == now);
  return result;
}

ssize_t FileStream::pwrite(const void* buf, size_t count, off64_t offset) {
  // Linux kernel ignores |offset| when the file is opened with O_APPEND.
  // Emulate the behavior.
  if (oflag_ & O_APPEND) {
    ARC_STRACE_REPORT("in O_APPEND mode. redirecting to write");
    return this->write(buf, count);
  }
  return this->PwriteImpl(buf, count, offset);
}

ssize_t FileStream::PwriteImpl(const void* buf, size_t count, off64_t offset) {
  const off64_t original = this->lseek(0, SEEK_CUR);
  if (original == -1)
    return -1;
  if (this->lseek(offset, SEEK_SET) == -1)
    return -1;
  const ssize_t result = this->write(buf, count);
  const off64_t now = this->lseek(original, SEEK_SET);
  ALOG_ASSERT(original == now);
  return result;
}

int FileStream::readv(const struct iovec* iov, int count) {
  ssize_t total;
  int error = VerifyIoVec(iov, count, &total);
  if (error != 0) {
    errno = error;
    return -1;
  }
  if (total == 0)
    return 0;

  // arc-wasm: libc's read() is readv() of one iovec here (__wasi_fd_read):
  // straight into it, without a temporary buffer the size of the read (an
  // asset of several MB was a malloc as large, and wasm memory touched once
  // stays the tab's on an iPhone; topic wasm-heap-trim).
  for (int i = 0; i < count; ++i) {
    if (iov[i].iov_len == static_cast<size_t>(total))
      return this->read(iov[i].iov_base, total);
  }

  scoped_ptr<char[]> buffer(new char[total]);
  ssize_t result = this->read(buffer.get(), total);
  if (result < 0) {
    // An error is found in read(). |errno| should be set in it.
    return result;
  }

  // Copy to the iov.
  ssize_t current = 0;
  for (int i = 0; i < count && current < result; ++i) {
    size_t copy_size = std::min<size_t>(result - current, iov[i].iov_len);
    memcpy(iov[i].iov_base, &buffer[current], copy_size);
    current += copy_size;
  }

  ALOG_ASSERT(current == result);
  return result;
}

int FileStream::writev(const struct iovec* iov, int count) {
  ssize_t total;
  int error = VerifyIoVec(iov, count, &total);
  if (error != 0) {
    errno = error;
    return -1;
  }
  if (total == 0) {
    return 0;
  }

  // arc-wasm: one iovec with data (libc's write()): straight from it.
  for (int i = 0; i < count; ++i) {
    if (iov[i].iov_len == static_cast<size_t>(total))
      return this->write(iov[i].iov_base, total);
  }

  scoped_ptr<char[]> buffer(new char[total]);
  size_t offset = 0;
  for (int i = 0; i < count; i++) {
    memcpy(&buffer[offset], iov[i].iov_base, iov[i].iov_len);
    offset += iov[i].iov_len;
  }
  return this->write(buffer.get(), total);
}

ssize_t FileStream::recv(void* buf, size_t len, int flags) {
  errno = ENOTSOCK;
  return -1;
}

ssize_t FileStream::recvfrom(void* buf, size_t len, int flags, sockaddr* addr,
                             socklen_t* addrlen) {
  errno = ENOTSOCK;
  return -1;
}

int FileStream::recvmsg(struct msghdr* msg, int flags) {
  errno = ENOTSOCK;
  return -1;
}

ssize_t FileStream::send(const void* buf, size_t len, int flags) {
  errno = ENOTSOCK;
  return -1;
}

ssize_t FileStream::sendto(const void* buf, size_t len, int flags,
                           const sockaddr* dest_addr, socklen_t addrlen) {
  errno = ENOTSOCK;
  return -1;
}

int FileStream::sendmsg(const struct msghdr* msg, int flags) {
  errno = ENOTSOCK;
  return -1;
}

int FileStream::setsockopt(int level, int optname, const void* optval,
                           socklen_t optlen) {
  errno = ENOTSOCK;
  return -1;
}

bool FileStream::IsSelectReadReady() const {
  return true;
}

bool FileStream::IsSelectWriteReady() const {
  return true;
}

bool FileStream::IsSelectExceptionReady() const {
  return false;
}

size_t FileStream::GetSize() const {
  return 0;
}

std::string FileStream::GetAuxInfo() const {
  return std::string();
}

void FileStream::OnLastFileRef() {
}

void FileStream::OnUnmapByOverwritingMmap(void* addr, size_t length) {
}

int16_t FileStream::GetPollEvents() const {
  return POLLIN | POLLOUT;
}

void FileStream::NotifyListeners() {
  ALOG_ASSERT(is_listening_enabled_,
              "Cannot notify listeners when file cannot be listened to");
  if (IsClosed())
    return;  // Likely processing the last read event.
  for (FileMap::iterator it = listeners_.begin();
      it != listeners_.end(); it++) {
    it->second->CheckNotClosed();
    it->second->HandleNotificationFrom(this, false);
  }
}

void FileStream::HandleNotificationFrom(
    scoped_refptr<FileStream> file, bool is_closing) {
  // Whoever added itself as a listener must be able to handle notifications.
  ALOG_ASSERT(false, "FileStream listener '%s' does not handle notifications",
              GetStreamType());
}

bool FileStream::StartListeningTo(scoped_refptr<FileStream> file) {
  if (!file->is_listening_enabled_)
    return false;
  CheckNotClosed();
  file->CheckNotClosed();
  ALOG_ASSERT(file->listeners_.count(this) == 0,
              "Cannot add the same listener twice");
  file->listeners_.insert(std::make_pair(this, this));
  return true;
}

void FileStream::StopListeningTo(scoped_refptr<FileStream> file) {
  file->listeners_.erase(this);
}

}  // namespace posix_translation
