// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POSIX_TRANSLATION_NACL_MANIFEST_FILE_H_
#define POSIX_TRANSLATION_NACL_MANIFEST_FILE_H_

#include <map>
#include <string>

#include "base/basictypes.h"
#include "base/compiler_specific.h"
#include "base/containers/hash_tables.h"
#include "common/export.h"
#include "posix_translation/directory_manager.h"
#include "posix_translation/file_system_handler.h"
#include "posix_translation/passthrough.h"

// For nacl_irt_resource_open.
#include "irt.h"  // NOLINT(build/include)

namespace posix_translation {

struct NaClManifestEntry {
  const char* name;
  mode_t mode;
  off_t size;
  time_t mtime;
};

// Simulates a file system based on file keys from NaCl manifest.
class ARC_EXPORT NaClManifestFileHandler : public FileSystemHandler {
 public:
  NaClManifestFileHandler(const NaClManifestEntry* files, size_t num_files);
  virtual ~NaClManifestFileHandler();

  virtual int mkdir(const std::string& pathname, mode_t mode) OVERRIDE;
  virtual scoped_refptr<FileStream> open(
      int fd, const std::string& pathname, int oflag, mode_t cmode) OVERRIDE;
  virtual int rename(const std::string& oldpath,
                     const std::string& newpath) OVERRIDE;
  virtual Dir* OnDirectoryContentsNeeded(const std::string& name) OVERRIDE;
  virtual int stat(const std::string& pathname, struct stat* out) OVERRIDE;
  virtual int statfs(const std::string& pathname, struct statfs* out) OVERRIDE;
  virtual int truncate(const std::string& pathname, off64_t length) OVERRIDE;
  virtual int unlink(const std::string& pathname) OVERRIDE;
  virtual int utimes(const std::string& pathname,
                     const struct timeval times[2]) OVERRIDE;

  void AddToFdCacheLocked(const std::string& pathname, int fd);

 private:
  // The function unlocks VirtualFileSystem::mutex_.
  bool ExistsLocked(const std::string& pathname);
  int OpenLocked(const std::string& pathname);

  // Initializes |directory_manager_|.
  void InitializeDirectoryManager(const NaClManifestEntry* files,
                                  size_t num_files);

  struct nacl_irt_resource_open resource_open_;

  // A object which knows a list of all files
  // (e.g. "/system/lib/egl/libEGL_emulation.so") in the nmf file.
  DirectoryManager directory_manager_;

  // A cahce for getting a stat() result without NaCl IPC.
  base::hash_map<std::string, struct stat> stat_cache_;  // NOLINT

  // A cache for getting a file descriptor for the file without calling
  // into the slow open_resource IRT.
  std::multimap<std::string, int> fd_cache_;

  DISALLOW_COPY_AND_ASSIGN(NaClManifestFileHandler);
};

class NaClManifestFile : public PassthroughStream {
 public:
  NaClManifestFile(int fd, const std::string& pathname, int oflag,
                   const struct stat& st, NaClManifestFileHandler* handler);

  virtual int fstat(struct stat* out) OVERRIDE;
  virtual int fstatfs(struct statfs* out) OVERRIDE;
  virtual ssize_t write(const void* buf, size_t count) OVERRIDE;

  virtual const char* GetStreamType() const OVERRIDE;

 protected:
  virtual ~NaClManifestFile();

 private:
  const struct stat st_;
  NaClManifestFileHandler* handler_;

  DISALLOW_COPY_AND_ASSIGN(NaClManifestFile);
};

}  // namespace posix_translation
#endif  // POSIX_TRANSLATION_NACL_MANIFEST_FILE_H_
