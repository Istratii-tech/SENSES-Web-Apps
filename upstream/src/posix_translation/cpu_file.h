// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POSIX_TRANSLATION_CPU_FILE_H_
#define POSIX_TRANSLATION_CPU_FILE_H_

#include <string>

#include "base/basictypes.h"
#include "base/compiler_specific.h"
#include "common/export.h"
#include "posix_translation/directory_manager.h"
#include "posix_translation/file_system_handler.h"

namespace posix_translation {

// A handler for /sys/devices/system/cpu. This handler returns dummy directory
// entries like "cpu0", "cpu1", etc. based on the actual processor count, when
// directory contents of /sys/devices/system/cpu is requested. It also handles
// some special files like /sys/devices/system/cpu/{possible,present}. We need
// this because some apps check the number of processors by checking these
// files and directories.
class ARC_EXPORT CpuFileHandler : public FileSystemHandler {
 public:
  CpuFileHandler();
  virtual ~CpuFileHandler();

  // FileSystemHandler overrides:
  virtual bool IsInitialized() const OVERRIDE;
  virtual void Initialize() OVERRIDE;
  virtual Dir* OnDirectoryContentsNeeded(const std::string& name) OVERRIDE;
  virtual void OnMounted(const std::string& path) OVERRIDE;
  virtual scoped_refptr<FileStream> open(
      int fd, const std::string& pathname, int oflag, mode_t cmode) OVERRIDE;
  virtual int stat(const std::string& pathname, struct stat* out) OVERRIDE;
  virtual int statfs(const std::string& pathname, struct statfs* out) OVERRIDE;

 private:
  bool is_initialized_;
  int num_processors_;
  std::string path_;
  DirectoryManager directory_manager_;

  DISALLOW_COPY_AND_ASSIGN(CpuFileHandler);
};

}  // namespace posix_translation
#endif  // POSIX_TRANSLATION_CPU_FILE_H_
