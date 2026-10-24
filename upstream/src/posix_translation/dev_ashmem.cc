// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "posix_translation/dev_ashmem.h"

#include <fcntl.h>
#include <linux/ashmem.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/vfs.h>

#include <algorithm>

#include "base/strings/string_util.h"  // strlcpy
#include "posix_translation/dir.h"
#include "posix_translation/statfs.h"
#include "posix_translation/virtual_file_system.h"

namespace posix_translation {

namespace {

int DoStatLocked(const std::string& pathname, struct stat* out) {
  memset(out, 0, sizeof(struct stat));
  out->st_ino =
      VirtualFileSystem::GetVirtualFileSystem()->GetInodeLocked(pathname);
  out->st_mode = S_IFCHR | 0666;
  out->st_nlink = 1;
  out->st_blksize = 4096;
  // st_uid, st_gid, st_size, st_blocks should be zero.

  // TODO(crbug.com/242337): Fill st_dev if needed.
  out->st_rdev = DeviceHandler::GetDeviceId(pathname);
  return 0;
}

}  // namespace

DevAshmemHandler::DevAshmemHandler() : DeviceHandler("DevAshmemHandler") {
}

DevAshmemHandler::~DevAshmemHandler() {
}

scoped_refptr<FileStream> DevAshmemHandler::open(
    int fd, const std::string& pathname, int oflag, mode_t cmode) {
  if (oflag & O_DIRECTORY) {
    errno = ENOTDIR;
    return NULL;
  }
  return new DevAshmem(fd, pathname, oflag);
}

int DevAshmemHandler::stat(const std::string& pathname, struct stat* out) {
  return DoStatLocked(pathname, out);
}

DevAshmem::DevAshmem(int fd, const std::string& pathname, int oflag)
    : DeviceStream(oflag, pathname),
      fd_(fd),
      size_(0),
      content_(static_cast<uint8_t*>(MAP_FAILED)),
      mmap_length_(0),
      offset_(0),
      has_private_mapping_(false),
      state_(STATE_INITIAL) {
}

DevAshmem::~DevAshmem() {
  if (state_ == STATE_UNMAP_DELAYED)
    ::munmap(content_, mmap_length_);
}

int DevAshmem::fstat(struct stat* out) {
  const int result = DoStatLocked(pathname(), out);
  // arc-wasm (topic guest-jit): the region's size, as for a memfd.
  if (result == 0)
    out->st_size = size_;
  return result;
}

// arc-wasm (topic guest-jit): the NDK bridge's memfd_create gives an ashmem
// region (a JIT's double mapping: every MAP_SHARED mapping is the same
// memory), sized with ftruncate as ASHMEM_SET_SIZE does: before it is
// mapped; the size it has is always fine.
int DevAshmem::ftruncate(off64_t length) {
  if (length < 0) {
    errno = EINVAL;
    return -1;
  }
  if (static_cast<size_t>(length) == size_)
    return 0;
  if (state_ != STATE_INITIAL || has_private_mapping_) {
    errno = EINVAL;
    return -1;
  }
  size_ = static_cast<size_t>(length);
  return 0;
}

int DevAshmem::ioctl(int request, va_list ap) {
  const unsigned int urequest = static_cast<unsigned int>(request);

  if (urequest == ASHMEM_SET_NAME) {
    return IoctlSetName(request, ap);
  } else if (urequest == ASHMEM_GET_NAME) {
    return IoctlGetName(request, ap);
  } else if (urequest == ASHMEM_SET_SIZE) {
    return IoctlSetSize(request, ap);
  } else if (urequest == ASHMEM_GET_SIZE) {
    return IoctlGetSize(request, ap);
  } else if (urequest == ASHMEM_SET_PROT_MASK) {
    return IoctlSetProtMask(request, ap);
  } else if (urequest == ASHMEM_PIN) {
    return IoctlPin(request, ap);
  } else if (urequest == ASHMEM_UNPIN) {
    return IoctlUnpin(request, ap);
  } else if (urequest == FIONREAD) {
    // arc-wasm (topic userspace): what read() still has, as for a file.
    // FileInputStream.available() asks it of ParcelFileDescriptor.fromData
    // regions (Settings' battery statistics).
    int* out = va_arg(ap, int*);
    *out = offset_ < static_cast<off64_t>(size_) ? static_cast<int>(size_ - offset_) : 0;
    return 0;
  }
  ALOGE("ioctl command %u is not supported", urequest);
  errno = EINVAL;
  return -1;
}

off64_t DevAshmem::lseek(off64_t offset, int whence) {
  if (!size_) {
    // ASHMEM_SET_SIZE has not been called yet. Return EINVAL.
    // This behavior is compatible with the Linux kernel.
    errno = EINVAL;
    return -1;
  }
  if (state_ == STATE_INITIAL && !has_private_mapping_) {
    // This behavior is compatible with the Linux kernel too.
    errno = EBADF;
    return -1;
  }

  switch (whence) {
    case SEEK_SET:
      offset_ = offset;
      break;
    case SEEK_CUR:
      offset_ += offset;
      break;
    case SEEK_END:
      offset_ = size_ + offset;
      break;
    default:
      errno = EINVAL;
      return -1;
  }
  return offset_;
}

// Note: [addr, addr+length) should be valid even if a part of original mmaped
// region is released partially by munmap(). MemoryRegion manages the memory
// layout, and calls each madvise implementation so that [addr, addr+length)
// is always valid for each FileStream instance.
int DevAshmem::madvise(void* addr, size_t length, int advice) {
  if (advice != MADV_DONTNEED)
    return FileStream::madvise(addr, length, advice);

  // TODO(crbug.com/427417): Since MemoryRegion handles memory layout
  // information by FileStream unit basis, we do not have page by page prot
  // information that can be updated by subsequent mmap and mprotect.
  // Use the relaxed protection mode (R/W) here.
  void* result = ::mmap(addr, length, PROT_READ | PROT_WRITE,
                        MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, -1, 0);
  if (result == addr)
    return 0;
  ALOGE("An internal mmap call for DevAshmem::madvise returns an unexpected "
        "address %p for expected address %p", result, addr);
  // Return 1 for an unrecoverable error to go LOG_ALWAYS_FATAL.
  return 1;
}

void* DevAshmem::mmap(
    void* addr, size_t length, int prot, int flags, off_t offset) {
  if (!size_) {
    // This behavior is compatible with the Linux kernel.
    errno = EINVAL;
    return MAP_FAILED;
  }

  const int fixed_flag = (flags & MAP_FIXED);
  if (!(flags & MAP_SHARED)) {
    // Handling MAP_PRIVATE is simple. We can just emulate it with
    // MAP_ANONYMOUS. We should NOT share the content with a previously
    // mapped MAP_SHARED region even when it exists. We can also ignore
    // the offset value as long as it is multiples of the page size (which
    // has already been checked in VFS). The stream class does not remember
    // the returned address.
    void* result = ::mmap(
        addr, length, prot, MAP_ANONYMOUS | MAP_PRIVATE | fixed_flag, -1, 0);
    if (result != MAP_FAILED)
      has_private_mapping_ = true;
    return result;
  }

  if (offset) {
    // For simplicity, reject MAP_SHARED mmaps with non-zero offset. Linux
    // kernel supports it though.
    ALOGE("Non-zero offset with MAP_SHARED is currently not supported: %ld",
          offset);
    errno = EINVAL;
    return MAP_FAILED;
  }

  if (content_ == MAP_FAILED) {
    ALOG_ASSERT(state_ == STATE_INITIAL);
    // TODO(crbug.com/427417): Since subsequent mmap calls may reuse the
    // address, use the relaxed protection mode (R/W).
    content_ = static_cast<uint8_t*>(
        ::mmap(addr, length, PROT_READ | PROT_WRITE,
               MAP_ANONYMOUS | MAP_PRIVATE | fixed_flag, -1, 0));
    mmap_length_ = length;
    ARC_STRACE_REPORT("MAP_ANONYMOUS returned %p (name_=%s)",
                      content_, name_.c_str());
    state_ = STATE_MAPPED;
    return content_;
  }

  // mmap(MAP_SHARED) is called twice (or more).
  ALOG_ASSERT(state_ != STATE_INITIAL);

  if (state_ == STATE_PARTIALLY_UNMAPPED) {
    ALOGE("The second mmap was called after munmap partially unmmapped the "
          "region");
    errno = EINVAL;
    return MAP_FAILED;
  }

  if (length != mmap_length_) {
    ALOGE("The second mmap was called with a different length (%zu) than "
          "the first one (%zu)", length, mmap_length_);
    errno = EINVAL;
    return MAP_FAILED;
  }

  if (fixed_flag && static_cast<uint8_t*>(addr) != content_) {
    ALOGE("The second mmap was called with MAP_FIXED (addr=%p, content_=%p)",
          addr, content_);
    errno = EINVAL;
    return MAP_FAILED;
  }

  if (state_ == STATE_UNMAP_DELAYED)
    state_ = STATE_MAPPED;

  return content_;
}

int DevAshmem::munmap(void* addr_vp, size_t length) {
  ARC_STRACE_REPORT("munmap(%p, %zu) is called for fd_=%d, name_=%s",
                    addr_vp, length, fd_, name_.c_str());

  uint8_t* addr = static_cast<uint8_t*>(addr_vp);
  if (!IsMapShared(addr)) {
    // The munmap request is against one of the MAP_PRIVATE regions. Just call
    // ::munmap.
    const int result = ::munmap(addr, length);
    ALOG_ASSERT(!result);
    return 0;
  }

  if ((state_ == STATE_MAPPED) &&
      (addr == content_) && (length == mmap_length_)) {
    // Full unmap of the MAP_SHARED region. Do not call unmap yet so that
    // subsequent read() calls can read the content. Note that we do support
    // "mmap, full-munmap, then read" cases, but do not support "mmap,
    // partial-munmap, then read" ones. This is because the latter is uncommon
    // and CTS does not require it.
    state_ = STATE_UNMAP_DELAYED;
    return 0;
  }

  if (state_ == STATE_UNMAP_DELAYED) {
    ALOGE("munmap(%p, %zu) is called against a memory region which has already "
          "been unmapped. Ignore the call.", addr, length);
    return 0;
  }

  state_ = STATE_PARTIALLY_UNMAPPED;
  const int result = ::munmap(addr, length);
  ALOG_ASSERT(!result);
  return 0;
}

ssize_t DevAshmem::pread(void* buf, size_t count, off64_t offset) {
  if ((oflag() & O_ACCMODE) == O_WRONLY) {
    errno = EBADF;
    return -1;
  }
  if (!size_) {
    // ASHMEM_SET_SIZE has not been called yet. Return EOF to make one CTS test
    //  cts.CtsOsTestCases:android.os.cts.ParcelFileDescriptorTest#testFromData
    // happy.
    return 0;
  }
  if (state_ == STATE_INITIAL && !has_private_mapping_) {
    // This behavior is compatible with the Linux kernel.
    errno = EBADF;
    return -1;
  }

  const ssize_t read_max = size_ - offset;
  if (read_max <= 0)
    return 0;

  if (state_ == STATE_PARTIALLY_UNMAPPED) {
    ALOG_ASSERT(content_ != MAP_FAILED);
    // Calling memcpy is not safe since |content_| might be pointing
    // to a unmmapped page.
    errno = EBADF;
    return -1;
  }

  // If there is a MAP_SHARED region, copy the content from there. If not, just
  // fill zeros.
  const size_t read_size = std::min<int64_t>(count, read_max);
  if (content_ != MAP_FAILED)
    memcpy(buf, content_ + offset, read_size);
  else
    memset(buf, 0, read_size);

  return read_size;
}

ssize_t DevAshmem::read(void* buf, size_t count) {
  ssize_t result = this->pread(buf, count, offset_);
  if (result > 0)
    offset_ += result;
  return result;
}

ssize_t DevAshmem::write(const void* buf, size_t count) {
  // This behavior is compatible with the Linux kernel.
  errno = EINVAL;
  return -1;
}

bool DevAshmem::ReturnsSameAddressForMultipleMmaps() const {
  return true;
}

void DevAshmem::OnUnmapByOverwritingMmap(void* addr_vp, size_t length) {
  uint8_t* addr = static_cast<uint8_t*>(addr_vp);
  if (!IsMapShared(addr))
    return;
  // This object no longer owns [addr, addr + length). Change the |state_| so
  // that subsequent read and pread calls will fail. Do not transit to
  // STATE_UNMAP_DELAYED even when |length| is equal to |mmap_length_| since
  // the object no longer ownes the memory region.
  if (state_ == STATE_MAPPED)
    state_ = STATE_PARTIALLY_UNMAPPED;
}

const char* DevAshmem::GetStreamType() const {
  return "ashmem";
}

size_t DevAshmem::GetSize() const {
  VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
  sys->mutex().AssertAcquired();
  return size_;
}

std::string DevAshmem::GetAuxInfo() const {
  return name_;
}

bool DevAshmem::IsMapShared(uint8_t* addr) const {
  return (content_ != MAP_FAILED) &&
      (content_ <= addr) && (addr < content_ + mmap_length_);
}

int DevAshmem::IoctlSetName(int request, va_list ap) {
  if (state_ != STATE_INITIAL || has_private_mapping_) {
    // This behavior is compatible with the Linux kernel.
    errno = EINVAL;
    return -1;
  }
  const char* name = va_arg(ap, const char*);
  ALOG_ASSERT(name);
  ARC_STRACE_REPORT("ASHMEM_SET_NAME: %s", name);
  name_ = name;
  return 0;
}

int DevAshmem::IoctlGetName(int request, va_list ap) {
  char* name = va_arg(ap, char*);
  ALOG_ASSERT(name);
  base::strlcpy(name, name_.c_str(), ASHMEM_NAME_LEN);
  ARC_STRACE_REPORT("ASHMEM_GET_NAME: %s", name);
  return 0;
}

int DevAshmem::IoctlSetSize(int request, va_list ap) {
  if (state_ != STATE_INITIAL || has_private_mapping_) {
    // This behavior is compatible with the Linux kernel.
    errno = EINVAL;
    return -1;
  }
  size_ = va_arg(ap, size_t);
  // Note: cts.CtsOsTestCases:android.os.cts.MemoryFileTest#testLength
  // calls this with INT_MIN.
  ARC_STRACE_REPORT("ASHMEM_SET_SIZE: %zu (%zuMB)", size_, size_ / 1024 / 1024);
  return 0;
}

int DevAshmem::IoctlGetSize(int request, va_list ap) {
  return size_;
}

int DevAshmem::IoctlPin(int request, va_list ap) {
  // TODO(crbug.com/379838): Implement this once a new IRT for handling real
  // shared memory is added. For now, return the same value as ashmem-host.c
  // as a safe fallback.
  ALOGW("ASHMEM_PIN: not implemented: fd=%d", fd_);
  return ASHMEM_NOT_PURGED;
}

int DevAshmem::IoctlUnpin(int request, va_list ap) {
  // TODO(crbug.com/379838): Implement this too.
  ALOGW("ASHMEM_UNPIN: not implemented: fd=%d", fd_);
  return ASHMEM_IS_UNPINNED;
}

int DevAshmem::IoctlSetProtMask(int request, va_list ap) {
  // TODO(crbug.com/379838): Implement this too.
  int prot = va_arg(ap, int);
  ALOGW("ASHMEM_SET_PROT_MASK: not implemented: fd=%d, prot=%d", fd_, prot);
  return 0;
}

}  // namespace posix_translation
