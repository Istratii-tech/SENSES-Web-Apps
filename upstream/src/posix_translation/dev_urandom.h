// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POSIX_TRANSLATION_DEV_URANDOM_H_
#define POSIX_TRANSLATION_DEV_URANDOM_H_

#include <errno.h>
#include <string>

#include "base/basictypes.h"
#include "base/compiler_specific.h"
#include "common/export.h"
#include "native_client/src/untrusted/irt/irt.h"
#include "posix_translation/device_file.h"
#include "posix_translation/file_system_handler.h"

namespace posix_translation {

class ARC_EXPORT DevUrandomHandler : public DeviceHandler {
 public:
  DevUrandomHandler();
  virtual ~DevUrandomHandler();

  virtual scoped_refptr<FileStream> open(
      int fd, const std::string& pathname, int oflag, mode_t cmode) OVERRIDE;
  virtual int stat(const std::string& pathname, struct stat* out) OVERRIDE;

 private:
  DISALLOW_COPY_AND_ASSIGN(DevUrandomHandler);
};

class DevUrandom : public DeviceStream {
 public:
  DevUrandom(const std::string& pathname, int oflag);

  virtual int fstat(struct stat* out) OVERRIDE;
  virtual ssize_t read(void* buf, size_t count) OVERRIDE;
  virtual ssize_t write(const void* buf, size_t count) OVERRIDE;

  virtual const char* GetStreamType() const OVERRIDE;

 protected:
  virtual ~DevUrandom();

 private:
  bool GetRandomBytes(void* buf, size_t count, size_t* nread);

  nacl_irt_random random_;

  DISALLOW_COPY_AND_ASSIGN(DevUrandom);
};

}  // namespace posix_translation
#endif  // POSIX_TRANSLATION_DEV_URANDOM_H_
