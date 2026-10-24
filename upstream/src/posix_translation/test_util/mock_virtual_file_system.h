// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POSIX_TRANSLATION_TEST_UTIL_MOCK_VIRTUAL_FILE_SYSTEM_H_
#define POSIX_TRANSLATION_TEST_UTIL_MOCK_VIRTUAL_FILE_SYSTEM_H_

#include <sys/stat.h>  // ino_t

#include <string>
#include <utility>
#include <vector>

#include "base/basictypes.h"
#include "base/compiler_specific.h"
#include "posix_translation/virtual_file_system_interface.h"
#include "ppapi/c/pp_file_info.h"

namespace posix_translation {

// A mock implementation of VirtualFileSystemInterface.
class MockVirtualFileSystem : public VirtualFileSystemInterface {
 public:
  MockVirtualFileSystem();
  virtual ~MockVirtualFileSystem();

  virtual void Mount(const std::string& path,
                     FileSystemHandler* handler) OVERRIDE;
  virtual void Unmount(const std::string& path) OVERRIDE;
  virtual void ChangeMountPointOwner(const std::string& path,
                                     uid_t owner_uid) OVERRIDE;
  virtual void SetBrowserReady() OVERRIDE;
  virtual void InvalidateCache() OVERRIDE;
  virtual void AddToCache(const std::string& path,
                          const PP_FileInfo& file_info,
                          bool exists) OVERRIDE;
  virtual void SchedulePreopen(const std::string& path) OVERRIDE;
  virtual bool RegisterFileStream(int fd,
                                  scoped_refptr<FileStream> stream) OVERRIDE;
  virtual FileSystemHandler* GetFileSystemHandler(
      const std::string& path) OVERRIDE;
  virtual std::string GetMemoryMapAsString() OVERRIDE;
  virtual std::string GetIPCStatsAsString() OVERRIDE;
  virtual int StatForTesting(
      const std::string& pathname, struct stat* out) OVERRIDE;

  // The call count of each function.
  uint32_t add_to_cache_callcount() const { return add_to_cache_callcount_; }

  const std::vector<std::string>& non_existing_cached_paths() const {
    return non_existing_cached_paths_; }

  const std::vector<std::pair<std::string, PP_FileInfo> >&
      existing_cached_paths() const {
        return existing_cached_paths_;
      }

 private:
  uint32_t add_to_cache_callcount_;
  std::vector<std::string> non_existing_cached_paths_;
  std::vector<std::pair<std::string, PP_FileInfo> > existing_cached_paths_;

  DISALLOW_COPY_AND_ASSIGN(MockVirtualFileSystem);
};

}  // namespace posix_translation
#endif  // POSIX_TRANSLATION_TEST_UTIL_MOCK_VIRTUAL_FILE_SYSTEM_H_
