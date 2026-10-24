// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POSIX_TRANSLATION_DEV_ASHMEM_H_
#define POSIX_TRANSLATION_DEV_ASHMEM_H_

#include <string>

#include "base/basictypes.h"
#include "base/compiler_specific.h"
#include "base/synchronization/lock.h"
#include "common/export.h"
#include "posix_translation/device_file.h"

namespace posix_translation {

// This handler is for emulating Ashmem. AshMem is short for Anonymous Shared
// Memory, and Android has the special device in its specially-configured
// Linux kernel. User space code in Android uses the device like this:
//
// fd = open(“/dev/ashmem”, O_RDWR);
// ioctl(fd, ASHMEM_SET_NAME, name);
// ioctl(fd, ASHMEM_SET_SIZE, size);
// p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
// /* read/write the memory region |p|. */
// …
// /* Pass the |fd| to another process via Binder. */
class ARC_EXPORT DevAshmemHandler : public DeviceHandler {
 public:
  DevAshmemHandler();
  virtual ~DevAshmemHandler();

  virtual scoped_refptr<FileStream> open(
      int fd, const std::string& pathname, int oflag, mode_t cmode) OVERRIDE;
  virtual int stat(const std::string& pathname, struct stat* out) OVERRIDE;

 private:
  DISALLOW_COPY_AND_ASSIGN(DevAshmemHandler);
};

class DevAshmem : public DeviceStream {
 public:
  DevAshmem(int fd, const std::string& pathname, int oflag);

  virtual int fstat(struct stat* out) OVERRIDE;
  virtual int ftruncate(off64_t length) OVERRIDE;
  virtual int ioctl(int request, va_list ap) OVERRIDE;
  virtual off64_t lseek(off64_t offset, int whence) OVERRIDE;
  virtual int madvise(void* addr, size_t length, int advice) OVERRIDE;
  virtual void* mmap(
      void* addr, size_t length, int prot, int flags, off_t offset) OVERRIDE;
  virtual int munmap(void* addr, size_t length) OVERRIDE;
  virtual ssize_t pread(void* buf, size_t count, off64_t offset) OVERRIDE;
  virtual ssize_t read(void* buf, size_t count) OVERRIDE;
  virtual ssize_t write(const void* buf, size_t count) OVERRIDE;

  virtual bool ReturnsSameAddressForMultipleMmaps() const OVERRIDE;
  virtual void OnUnmapByOverwritingMmap(void* addr, size_t length) OVERRIDE;

  virtual const char* GetStreamType() const OVERRIDE;
  virtual size_t GetSize() const OVERRIDE;
  virtual std::string GetAuxInfo() const OVERRIDE;

 protected:
  virtual ~DevAshmem();

 private:
  // Returns true if |addr| is in [content_, content_ + mmap_length_).
  bool IsMapShared(uint8_t* addr) const;

  int IoctlSetName(int request, va_list ap);
  int IoctlGetName(int request, va_list ap);
  int IoctlSetSize(int request, va_list ap);
  int IoctlGetSize(int request, va_list ap);
  int IoctlPin(int request, va_list ap);
  int IoctlUnpin(int request, va_list ap);
  int IoctlSetProtMask(int request, va_list ap);

  int fd_;  // our VFS's FD, not NaCl's. This is for debug prints.
  size_t size_;  // passed via ioctl. Might not be multiples of the page size.
  std::string name_;  // passed via ioctl.

  uint8_t* content_;  // the MAP_ANONOYMOUS region for emulating MAP_SHARED.
  size_t mmap_length_;  // the length of the |content_|.
  off_t offset_;  // the file offset for read, pread, write, and lseek.

  // True if mmap with MAP_PRIVATE has succeeded at least once.
  bool has_private_mapping_;

  // The current status of the |content_|. The possible transition of the
  // state is as follows:
  //
  //         +--------+
  //         v        +
  // 0+----->1+------>2        3
  //         +                 ^
  //         +-----------------+
  //
  // The transition from 2 to 1 happens if mmap is called after full-munmap.
  // Note that neither ioctl nor mmap with MAP_PRIVATE affects the |state_|.
  enum {
    // mmap with MAP_SHARED has not been called yet. |content_| is MAP_FAILED.
    STATE_INITIAL = 0,
    // mmap with MAP_SHARED has been called and munmap has not. |content_|
    // points to a memory region returned from the mmap.
    STATE_MAPPED = 1,
    // The region has been fully unmapped by VFS::munmap, but the actual munmap
    // IRT has not been called yet to allow VFS::read and pread to read the
    // |content_|. Some CTS tests fail without this trick.
    STATE_UNMAP_DELAYED = 2,
    // The region has been partially unmapped by VFS::munmap, and the actual
    // munmap IRT has also been called. Subsequent read, pread and mmap calls
    // will fail.
    STATE_PARTIALLY_UNMAPPED = 3,
  } state_;

  DISALLOW_COPY_AND_ASSIGN(DevAshmem);
};

}  // namespace posix_translation
#endif  // POSIX_TRANSLATION_DEV_ASHMEM_H_
