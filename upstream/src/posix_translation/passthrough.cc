// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "posix_translation/passthrough.h"

#include <poll.h>
#include <unistd.h>

#include <string>

#include "common/alog.h"
#include "posix_translation/real_syscall.h"

namespace posix_translation {

namespace {

const ino_t kNativeInodeNumberMask = 0x80000000;
const blksize_t kBlockSize = 4096;
const int kInvalidFd = -1;

}  // namespace

PassthroughHandler::PassthroughHandler()
    : FileSystemHandler("PassthroughHandler") {
}

PassthroughHandler::~PassthroughHandler() {
}

scoped_refptr<FileStream> PassthroughHandler::open(
    int fd, const std::string& pathname, int oflag, mode_t cmode) {
  int native_fd = -1;
  if (!pathname.empty()) {
    errno = EACCES;
  } else {
    ALOG_ASSERT(fd >= 0);
    native_fd = fd;
  }
  return (native_fd < 0) ? NULL :
      new PassthroughStream(native_fd, pathname, oflag, !pathname.empty());
}

int PassthroughHandler::stat(const std::string& pathname, struct stat* out) {
  scoped_refptr<FileStream> stream = this->open(-1, pathname, O_RDONLY, 0);
  if (!stream) {
    errno = ENOENT;
    return -1;
  }
  return stream->fstat(out);
}

int PassthroughHandler::statfs(const std::string& pathname,
                               struct statfs* out) {
  errno = ENOSYS;
  return -1;
}

Dir* PassthroughHandler::OnDirectoryContentsNeeded(const std::string& name) {
  return NULL;
}

PassthroughStream::PassthroughStream(int native_fd,
                                     const std::string& pathname,
                                     int oflag,
                                     bool close_on_destruction)
    : FileStream(oflag, pathname),
      native_fd_(native_fd),
      close_on_destruction_(close_on_destruction) {
  ALOG_ASSERT(native_fd_ >= 0);
}

PassthroughStream::PassthroughStream()
    : FileStream(0, std::string()),
      native_fd_(kInvalidFd),
      close_on_destruction_(false) {
}

PassthroughStream::~PassthroughStream() {
  if (close_on_destruction_)
    real_close(native_fd_);
}

int PassthroughStream::fstat(struct stat* out) {
  ALOG_ASSERT(native_fd_ >= 0);
  const int result = real_fstat(native_fd_, out);
  if (!result) {
    // Add a large number so that st_ino does not conflict with the one
    // generated in our VFS.
    out->st_ino |= kNativeInodeNumberMask;
    // Overwrite the real dev/rdev numbers with zero. See PepperFile::fstat.
    out->st_dev = out->st_rdev = 0;
    // Overwrite atime/ctime too.
    out->st_atime = out->st_ctime = 0;
    out->st_blksize = kBlockSize;
  }
  return result;
}

off64_t PassthroughStream::lseek(off64_t offset, int whence) {
  ALOG_ASSERT(native_fd_ >= 0);
  return real_lseek64(native_fd_, offset, whence);
}

// Note: [addr, addr+length) should be valid even if a part of original mmaped
// region is released partially by munmap(). MemoryRegion manages the memory
// layout, and calls each madvise implementation so that [addr, addr+length)
// is always valid for each FileStream instance.
int PassthroughStream::madvise(void* addr, size_t length, int advice) {
  if (advice != MADV_DONTNEED)
    return FileStream::madvise(addr, length, advice);

  if (native_fd_ != kInvalidFd) {
    ALOGW("madvise with MADV_DONTNEED for native fd backed stream is not "
          "supported.");
    errno = EBADF;
    return -1;
  }

  // TODO(crbug.com/427417): Since MemoryRegion handles memory layout
  // information by FileStream unit basis, we do not have page by page prot
  // information that can be updated by subsequent mmap and mprotect.
  // Use the relaxed protection mode (R/W) here.
  void* result = ::mmap(addr, length, PROT_READ | PROT_WRITE,
                        MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, native_fd_, 0);
  if (result == addr)
    return 0;
  ALOGE("An internal mmap call for PassthroughStream::madvise returns an "
        "unexpected address %p for expected address %p", result, addr);
  // Return 1 for an unrecoverable error to go LOG_ALWAYS_FATAL.
  return 1;
}

void* PassthroughStream::mmap(
      void* addr, size_t length, int prot, int flags, off_t offset) {
  if ((flags & MAP_ANONYMOUS) && (flags & MAP_SHARED))
    ALOGW("mmap with MAP_ANONYMOUS | MAP_SHARED is not fully supported");
  return ::mmap(addr, length, prot, flags, native_fd_, offset);
}

int PassthroughStream::munmap(void* addr, size_t length) {
  return ::munmap(addr, length);
}

ssize_t PassthroughStream::read(void* buf, size_t count) {
  ALOG_ASSERT(native_fd_ >= 0);
  return real_read(native_fd_, buf, count);
}

ssize_t PassthroughStream::write(const void* buf, size_t count) {
  ALOG_ASSERT(native_fd_ >= 0);
  return real_write(native_fd_, buf, count);
}

bool PassthroughStream::IsSelectReadReady() const {
  ALOG_ASSERT(native_fd_ >= 0);
  // Let us pretend we can always read from stdin.
  return (native_fd_ == STDIN_FILENO);
}
bool PassthroughStream::IsSelectWriteReady() const {
  ALOG_ASSERT(native_fd_ >= 0);
  // Let us pretend we can always write to stdout and stderr.
  return (native_fd_ == STDOUT_FILENO || native_fd_ == STDERR_FILENO);
}
bool PassthroughStream::IsSelectExceptionReady() const {
  ALOG_ASSERT(native_fd_ >= 0);
  return false;
}

int16_t PassthroughStream::GetPollEvents() const {
  ALOG_ASSERT(native_fd_ >= 0);
  return ((IsSelectReadReady() ? POLLIN : 0) |
          (IsSelectWriteReady() ? POLLOUT : 0) |
          (IsSelectExceptionReady() ? POLLERR : 0));
}

size_t PassthroughStream::GetSize() const {
  // MemoryRegion calls GetSize() even for an instance of an
  // anonymous memory region in order to show a memory mapping when
  // --logging=posix-translation-debug is enabled. Returning 0 is enough.
  struct stat st;
  if (pathname().empty() || const_cast<PassthroughStream*>(this)->fstat(&st))
    return 0;  // unknown size
  return st.st_size;
}

bool PassthroughStream::IsAllowedOnMainThread() const {
  return true;
}

const char* PassthroughStream::GetStreamType() const {
  return "passthru";
}

}  // namespace posix_translation
