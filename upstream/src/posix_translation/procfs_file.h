// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POSIX_TRANSLATION_PROCFS_FILE_H_
#define POSIX_TRANSLATION_PROCFS_FILE_H_

#include <string>

#include "base/basictypes.h"
#include "base/compiler_specific.h"
#include "common/export.h"
#include "common/update_tracking.h"
#include "posix_translation/directory_manager.h"
#include "posix_translation/file_system_handler.h"

namespace posix_translation {

// A handler for /proc/cpuinfo. This handler returns a file based on the actual
// online processor count.
class ARC_EXPORT ProcfsFileHandler : public FileSystemHandler {
 public:
  explicit ProcfsFileHandler(FileSystemHandler* readonly_fs_handler);
  virtual ~ProcfsFileHandler();

  // FileSystemHandler overrides:
  virtual bool IsInitialized() const OVERRIDE;
  virtual void Initialize() OVERRIDE;
  virtual Dir* OnDirectoryContentsNeeded(const std::string& name) OVERRIDE;
  virtual scoped_refptr<FileStream> open(
      int fd, const std::string& pathname, int oflag, mode_t cmode) OVERRIDE;
  virtual int stat(const std::string& pathname, struct stat* out) OVERRIDE;
  virtual int statfs(const std::string& pathname, struct statfs* out) OVERRIDE;
  virtual ssize_t readlink(const std::string& pathname, std::string* resolved)
      OVERRIDE;
  virtual void SetMountPointManager(MountPointManager* manager) OVERRIDE;

 private:
  friend class ProcfsHandlerTest;
  FRIEND_TEST(ProcfsHandlerTest, TestParsePidBasedPathMalformed);
  FRIEND_TEST(ProcfsHandlerTest, TestParsePidBasedPathValid);
  void SynchronizeDirectoryTreeStructure();

  bool ParsePidBasedPath(const std::string& pathname, pid_t* out_pid,
                         std::string* out_post_pid);

  // |header|, |body|, and |footer| are used for generating the content of the
  // cpuinfo file. |body| must contain "$1" and is repeated N times (where N is
  // the number of CPUs online). Both |header| and |footer| can be empty when
  // they are not needed.
  // Example:
  // When N is 2, |header| is "H", |body| is "B$1", and |footer| is "F", the
  // content of the file will be "HB0B1F".
  void SetCpuInfoFileTemplate(const std::string& header,
                              const std::string& body,
                              const std::string& footer);


  std::string cpuinfo_header_;
  std::string cpuinfo_body_;
  std::string cpuinfo_footer_;
  arc::UpdateConsumer update_consumer_;

  FileSystemHandler* readonly_fs_handler_;

  DirectoryManager file_names_;

  MountPointManager* mount_point_manager_;

  DISALLOW_COPY_AND_ASSIGN(ProcfsFileHandler);
};

}  // namespace posix_translation
#endif  // POSIX_TRANSLATION_PROCFS_FILE_H_
