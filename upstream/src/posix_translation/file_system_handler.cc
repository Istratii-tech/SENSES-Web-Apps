// Copyright 2014 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "posix_translation/file_system_handler.h"

namespace posix_translation {

FileSystemHandler::FileSystemHandler(const std::string& name)
    : name_(name) {
}

FileSystemHandler::~FileSystemHandler() {
}

bool FileSystemHandler::IsInitialized() const {
  return true;
}

void FileSystemHandler::Initialize() {
  ALOG_ASSERT(!IsInitialized());
}

void FileSystemHandler::OnMounted(const std::string& path) {
}

void FileSystemHandler::OnUnmounted(const std::string& path) {
}

void FileSystemHandler::InvalidateCache() {
}

void FileSystemHandler::AddToCache(const std::string& path,
                                   const PP_FileInfo& file_info,
                                   bool exists) {
}

bool FileSystemHandler::IsWorldWritable(const std::string& pathname) {
  struct stat st;
  if (!this->stat(pathname, &st)) {
    const mode_t mode = st.st_mode;
    return (mode & S_IWUSR) && (mode & S_IWGRP) && (mode & S_IWOTH);
  }
  return false;  // |pathname| does not exist.
}

std::string FileSystemHandler::SetPepperFileSystem(
    scoped_ptr<pp::FileSystem> file_system,
    const std::string& path_in_pepperfs,
    const std::string& path_in_vfs) {
  ALOGE("%s does not support Pepper filesystem.", name().c_str());
  return "";
}

int FileSystemHandler::mkdir(const std::string& pathname, mode_t mode) {
  errno = EEXIST;
  return -1;
}

ssize_t FileSystemHandler::readlink(const std::string& pathname,
                                    std::string* resolved) {
  errno = EINVAL;
  return -1;
}

int FileSystemHandler::remove(const std::string& pathname) {
  errno = EACCES;
  return -1;
}

int FileSystemHandler::rename(const std::string& oldpath,
                              const std::string& newpath) {
  if (oldpath == newpath)
    return 0;
  errno = EACCES;
  return -1;
}

int FileSystemHandler::rmdir(const std::string& pathname) {
  errno = EACCES;
  return -1;
}

int FileSystemHandler::symlink(const std::string& oldpath,
                               const std::string& newpath) {
  errno = EPERM;
  return -1;
}

int FileSystemHandler::truncate(const std::string& pathname, off64_t length) {
  errno = EINVAL;
  return -1;
}

int FileSystemHandler::unlink(const std::string& pathname) {
  errno = EACCES;
  return -1;
}

int FileSystemHandler::utimes(const std::string& pathname,
                              const struct timeval times[2]) {
  errno = EPERM;
  return -1;
}

}  // namespace posix_translation
