// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POSIX_TRANSLATION_DEV_NULL_H_
#define POSIX_TRANSLATION_DEV_NULL_H_

#include <errno.h>
#include <string>

#include "base/basictypes.h"
#include "base/compiler_specific.h"
#include "common/export.h"
#include "posix_translation/device_file.h"
#include "posix_translation/file_system_handler.h"

namespace posix_translation {

class ARC_EXPORT DevNullHandler : public DeviceHandler {
 public:
  DevNullHandler();
  explicit DevNullHandler(mode_t mode);
  virtual ~DevNullHandler();

  virtual scoped_refptr<FileStream> open(
      int fd, const std::string& pathname, int oflag, mode_t cmode) OVERRIDE;
  virtual int stat(const std::string& pathname, struct stat* out) OVERRIDE;

 private:
  const mode_t mode_;

  DISALLOW_COPY_AND_ASSIGN(DevNullHandler);
};

class DevNull : public DeviceStream {
 public:
  DevNull(const std::string& pathname, mode_t mode, int oflag);

  virtual int fstat(struct stat* out) OVERRIDE;
  virtual void* mmap(
      void* addr, size_t length, int prot, int flags, off_t offset) OVERRIDE;
  virtual int munmap(void* addr, size_t length) OVERRIDE;
  virtual ssize_t read(void* buf, size_t count) OVERRIDE;
  virtual ssize_t write(const void* buf, size_t count) OVERRIDE;

  virtual const char* GetStreamType() const OVERRIDE;

 private:
  virtual ~DevNull();

 private:
  const mode_t mode_;

  DISALLOW_COPY_AND_ASSIGN(DevNull);
};

}  // namespace posix_translation
#endif  // POSIX_TRANSLATION_DEV_NULL_H_
