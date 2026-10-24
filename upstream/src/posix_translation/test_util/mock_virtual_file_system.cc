// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "posix_translation/test_util/mock_virtual_file_system.h"

namespace posix_translation {

MockVirtualFileSystem::MockVirtualFileSystem()
    : add_to_cache_callcount_(0) {
}

MockVirtualFileSystem::~MockVirtualFileSystem() {
}

void MockVirtualFileSystem::Mount(const std::string& path,
                                  FileSystemHandler* handler) {
}

void MockVirtualFileSystem::Unmount(const std::string& path) {
}

void MockVirtualFileSystem::ChangeMountPointOwner(const std::string& path,
                                                  uid_t owner_uid) {
}

void MockVirtualFileSystem::SetBrowserReady() {
}

void MockVirtualFileSystem::InvalidateCache() {
}

void MockVirtualFileSystem::AddToCache(const std::string& path,
                                       const PP_FileInfo& file_info,
                                       bool exists) {
  if (exists) {
    existing_cached_paths_.push_back(make_pair(path, file_info));
  } else {
    non_existing_cached_paths_.push_back(path);
  }
  ++add_to_cache_callcount_;
}

void MockVirtualFileSystem::SchedulePreopen(const std::string& path) {
}

bool MockVirtualFileSystem::RegisterFileStream(
    int fd, scoped_refptr<FileStream> stream) {
  return true;
}

FileSystemHandler* MockVirtualFileSystem::GetFileSystemHandler(
    const std::string& path) {
  return NULL;
}

std::string MockVirtualFileSystem::GetMemoryMapAsString() {
  return "";
}

std::string MockVirtualFileSystem::GetIPCStatsAsString() {
  return "";
}

int MockVirtualFileSystem::StatForTesting(
    const std::string& pathname, struct stat* out) {
  return 0;
}

}  // namespace posix_translation
