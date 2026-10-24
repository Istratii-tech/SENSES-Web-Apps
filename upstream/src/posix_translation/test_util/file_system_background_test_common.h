// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POSIX_TRANSLATION_TEST_UTIL_FILE_SYSTEM_BACKGROUND_TEST_COMMON_H_
#define POSIX_TRANSLATION_TEST_UTIL_FILE_SYSTEM_BACKGROUND_TEST_COMMON_H_

#include <sys/types.h>
#include <unistd.h>

#include <string>

#include "base/basictypes.h"
#include "base/compiler_specific.h"
#include "common/process_emulator.h"
#include "posix_translation/fd_to_file_stream_map.h"
#include "posix_translation/mount_point_manager.h"
#include "posix_translation/readonly_file.h"
#include "posix_translation/test_util/file_system_test_common.h"
#include "posix_translation/virtual_file_system.h"
#include "ppapi_mocks/background_test.h"
#include "ppapi_mocks/background_thread.h"

namespace posix_translation {

// A class template that allows gtest tests to run in a non-main thread.
// This is useful when the method to test has a check like
//   ALOG_ASSERT(!pp::Module::Get()->core()->IsMainThread());
// For more details, see ppapi_mocks/background_*.h.
template <typename Derived>
class FileSystemBackgroundTestCommon : public BackgroundTest<Derived>,
                                       public FileSystemTestCommon {
 public:
  FileSystemBackgroundTestCommon()
    : cc_factory_(static_cast<Derived*>(this)),
      bg_(this) {
    set_is_background_test(true);
  }

 protected:
  virtual void SetUp() OVERRIDE {
    FileSystemTestCommon::SetUp();
    bg_.SetUp();
  }

  ino_t GetInode(const char* path) {
    return file_system_->GetInodeLocked(path);
  }
  void RemoveInode(const char* path) {
    file_system_->RemoveInodeLocked(path);
  }
  void ReassignInode(const char* oldpath, const char* newpath) {
    file_system_->ReassignInodeLocked(oldpath, newpath);
  }
  void AddMountPoint(const std::string& path, FileSystemHandler* handler) {
    file_system_->mount_points_->Add(path, handler);
  }
  void ChangeMountPointOwner(const std::string& path, uid_t uid) {
    file_system_->mount_points_->ChangeOwner(
        path, arc::ProcessEmulator::GetUid());
  }
  void RemoveMountPoint(const std::string& path) {
    uid_t uid = 0;
    ALOG_ASSERT(file_system_->mount_points_->GetFileSystemHandler(path, &uid));
    file_system_->mount_points_->Remove(path);
    ALOG_ASSERT(!file_system_->mount_points_->GetFileSystemHandler(path, &uid));
  }
  void ClearMountPoints() {
    file_system_->mount_points_->Clear();
  }
  FileSystemHandler* GetFileSystemHandlerLocked(const std::string& path) {
    return file_system_->GetFileSystemHandlerLocked(path, NULL);
  }
  int GetFirstUnusedDescriptor() {
    return file_system_->fd_to_stream_->GetFirstUnusedDescriptor();
  }
  void AddFileStream(int fd, scoped_refptr<FileStream> stream) {
    file_system_->fd_to_stream_->AddFileStream(fd, stream);
  }
  void ReplaceFileStream(int fd, scoped_refptr<FileStream> stream) {
    file_system_->fd_to_stream_->ReplaceFileStream(fd, stream);
  }
  void RemoveFileStream(int fd) {
    file_system_->fd_to_stream_->RemoveFileStream(fd);
  }
  bool IsKnownDescriptor(int fd) {
    return file_system_->fd_to_stream_->IsKnownDescriptor(fd);
  }
  scoped_refptr<FileStream> GetStream(int fd) {
    return file_system_->fd_to_stream_->GetStream(fd);
  }
  std::string GetNormalizedPath(const std::string& s,
                                VirtualFileSystem::NormalizeOption option) {
    std::string tmp(s);
    file_system_->GetNormalizedPathLocked(&tmp, option);
    return tmp;
  }
  base::Lock& mutex() {
    return file_system_->mutex();
  }

  // Overridden from BackgroundTest<Derived>:
  virtual BackgroundThread* GetBackgroundThread() OVERRIDE {
    return &bg_;
  }
  virtual pp::CompletionCallbackFactory<Derived>*
      GetCompletionCallbackFactory() OVERRIDE {
    return &cc_factory_;
  }

  pp::CompletionCallbackFactory<Derived> cc_factory_;
  BackgroundThread bg_;

 private:
  DISALLOW_COPY_AND_ASSIGN(FileSystemBackgroundTestCommon);
};

}  // namespace posix_translation

#endif  // POSIX_TRANSLATION_TEST_UTIL_FILE_SYSTEM_BACKGROUND_TEST_COMMON_H_
