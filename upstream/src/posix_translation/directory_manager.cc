// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "posix_translation/directory_manager.h"

#include <dirent.h>

#include <algorithm>
#include <vector>

#include "base/compiler_specific.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "common/alog.h"
#include "posix_translation/dir.h"
#include "posix_translation/file_system_handler.h"
#include "posix_translation/path_util.h"
#include "posix_translation/virtual_file_system.h"

namespace posix_translation {

// Our implementation of POSIX's ::DIR object.
class DirectoryManager::DirImpl : public Dir {
 public:
  DirImpl(const std::string& dirname, const FilesInDir& files)
      : dirname_(dirname), pos_(0) {
    util::EnsurePathEndsWithSlash(&dirname_);
    files_.reserve(files.size() + 2);
    files_.push_back(std::make_pair("./", Dir::DIRECTORY));
    files_.push_back(std::make_pair("../", Dir::DIRECTORY));
    std::copy(files.begin(), files.end(), std::back_inserter(files_));
    // Keep entries in |files_| sorted for easier unit testing. We
    // skip the first two entries. bionic-unit-tests-cts expects the
    // first entry is ".", not "..".
    std::sort(files_.begin() + 2, files_.end());
  }

  virtual bool GetNext(dirent* entry) OVERRIDE {
    if (pos_ >= files_.size())
      return false;
    std::string name = files_[pos_].first;
    entry->d_type = files_[pos_].second;

    ARC_STRACE_REPORT("Found %s in %s", name.c_str(), dirname_.c_str());
    std::string path = dirname_ + name;

    VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
    // VirtualFileSystem::kResolveParentSymlinks is a must here since |d_ino|
    // must be filled as if it is filled with lstat(2).
    sys->GetNormalizedPathLocked(
        &path, VirtualFileSystem::kResolveParentSymlinks);
    entry->d_ino = sys->GetInodeUncheckedLocked(path);

    entry->d_reclen = sizeof(dirent);
    if (util::EndsWithSlash(name)) {
      ALOG_ASSERT(entry->d_type == Dir::DIRECTORY);
      name.erase(name.size() - 1);  // remove trailing slash.
    } else {
      ALOG_ASSERT(entry->d_type != Dir::DIRECTORY);
    }

    const size_t len = std::min(sizeof(entry->d_name) - 1, name.length());
    if (len != name.length())
      ALOGW("DirImpl::GetNext: '%s' is too long. Truncated.", name.c_str());
    memcpy(entry->d_name, name.data(), len);
    entry->d_name[len] = '\0';

    entry->d_off = pos_;

    ++pos_;
    return true;
  }

  virtual void rewinddir() OVERRIDE { pos_ = 0; }

  virtual void Add(const std::string& name, Dir::Type type) OVERRIDE {
    ALOG_ASSERT(pos_ == 0);
    const std::pair<std::string, Dir::Type> val = std::make_pair(name, type);

    // Find an existing entry with the same |name| (if any). Otherwise, |it|
    // will point to the appropriate insertion point for the sorted vector.
    std::vector<std::pair<std::string, Dir::Type> >::iterator it =
        std::lower_bound(files_.begin(), files_.end(),
                         val,  // The comparator ignores |val.second|.
                         CompareFirst);
    if (it != files_.end() && (it->first == name)) {
      // Overwrite the existing element.
      it->second = type;
    } else {
      files_.insert(it, val);
    }
  }

 private:
  virtual ~DirImpl() {}

  static bool CompareFirst(const std::pair<std::string, Dir::Type>& lhs,
                           const std::pair<std::string, Dir::Type>& rhs) {
    return lhs.first < rhs.first;
  }

  std::string dirname_;
  // files in the directory. sorted by name.
  std::vector<std::pair<std::string, Dir::Type> > files_;
  // The current position in the |files_| list.
  size_t pos_;

  DISALLOW_COPY_AND_ASSIGN(DirImpl);
};

DirectoryManager::DirectoryManager() {
  Clear();
}

DirectoryManager::~DirectoryManager() {}

void DirectoryManager::Clear() {
  dir_to_files_.clear();
  MakeDirectory("/");
}

bool DirectoryManager::AddFile(const std::string& pathname) {
  return AddFileWithType(pathname, Dir::REGULAR);
}

bool DirectoryManager::AddFileWithType(const std::string& pathname,
                                       Dir::Type type) {
  if (!util::IsAbsolutePath(pathname))
    return false;  // can not handle relative paths.
  if (util::EndsWithSlash(pathname))
    return false;  // not a file, but a directory.
  if (StatDirectory(pathname))
    return false;  // |pathname| is already registered as a directory.

  DirAndFile dir_and_file = SplitPath(pathname);
  // The directory is not in the map yet. Add it.
  if (!StatDirectory(dir_and_file.first))
    MakeDirectories(dir_and_file.first);
  return AddFileInternal(dir_and_file.first, dir_and_file.second, type);
}

bool DirectoryManager::RemoveFile(const std::string& pathname) {
  if (util::EndsWithSlash(pathname))
    return false;  // not a file, but a directory.
  DirAndFile dir_and_file = SplitPath(pathname);
  FilesInDir* files = GetFilesInDir(dir_and_file.first);
  if (!files)
    return false;  // directory not found.
  return files->erase(dir_and_file.second) > 0;
}

bool DirectoryManager::RemoveDirectory(const std::string& dirname) {
  if (dirname == "/")
    return false;  // removing the root is not allowed.
  std::string dirname_slash = dirname;
  util::EnsurePathEndsWithSlash(&dirname_slash);
  FilesInDir* files = GetFilesInDir(dirname_slash);
  if (!files)
    return false;  // directory not found.
  if (!files->empty())
    return false;  // directory not empty.

  // Remove the directory from the map.
  bool result = dir_to_files_.erase(dirname_slash) > 0;
  ALOG_ASSERT(result, "dir=%s", dirname_slash.c_str());

  // Remove the directory from its parent's record.
  std::string parent_slash = util::GetDirName(dirname_slash);
  util::EnsurePathEndsWithSlash(&parent_slash);
  FilesInDir* parent = GetFilesInDir(parent_slash);
  ALOG_ASSERT(parent, "parent=%s", parent_slash.c_str());
  result = parent->erase(dirname_slash.erase(0, parent_slash.length())) > 0;
  ALOG_ASSERT(result,
              "remove %s from %s", dirname_slash.c_str(), parent_slash.c_str());

  return true;
}

bool DirectoryManager::StatFile(const std::string& pathname) const {
  if (util::EndsWithSlash(pathname))
    return false;  // not a file, but a directory.
  DirAndFile dir_and_file = SplitPath(pathname);
  const FilesInDir* files = GetFilesInDir(dir_and_file.first);
  if (!files)
    return false;  // directory not found.
  return files->find(dir_and_file.second) != files->end();
}

bool DirectoryManager::StatDirectory(const std::string& dirname) const {
  // TODO(yusukes): For better performance, we should eliminate the string
  // copy (here and elsewhere in this file). Now that we know the type of each
  // entry in |dir_to_files_|, we should be able to stop using the trailing
  // slash as a marker.
  std::string dirname_slash = dirname;
  util::EnsurePathEndsWithSlash(&dirname_slash);
  return dir_to_files_.find(dirname_slash) != dir_to_files_.end();
}

Dir* DirectoryManager::OpenDirectory(const std::string& dirname) const {
  if (StatFile(dirname)) {
    errno = ENOTDIR;
    return NULL;
  }
  std::string dirname_slash = dirname;
  util::EnsurePathEndsWithSlash(&dirname_slash);
  const FilesInDir* files = GetFilesInDir(dirname_slash);
  if (!files) {
    errno = ENOENT;
    return NULL;
  }
  return new DirImpl(dirname_slash, *files);
}

void DirectoryManager::MakeDirectories(const std::string& dirname) {
  std::vector<std::string> paths;

  std::string dirname_slash = dirname;
  util::EnsurePathEndsWithSlash(&dirname_slash);
  base::SplitString(dirname_slash, '/', &paths);

  std::string current_path = "/";
  for (size_t i = 0; i < paths.size(); ++i) {
    if (paths[i].empty())
      continue;
    AddFileInternal(current_path, paths[i] + "/", Dir::DIRECTORY);
    current_path += paths[i] + "/";
    MakeDirectory(current_path);
  }
}

bool DirectoryManager::MakeDirectory(const std::string& dirname) {
  if (!util::EndsWithSlash(dirname))
    return false;
  return dir_to_files_.insert(make_pair(dirname, FilesInDir())).second;
}

bool DirectoryManager::AddFileInternal(const std::string& directory,
                                       const std::string& filename,
                                       Dir::Type type) {
  ALOG_ASSERT(StatDirectory(directory));
  FilesInDir* files = GetFilesInDir(directory);
  ALOG_ASSERT(files);
  return files->insert(make_pair(filename, type)).second;
}

const DirectoryManager::FilesInDir* DirectoryManager::GetFilesInDir(
    const std::string& directory) const {
  return const_cast<DirectoryManager*>(this)->GetFilesInDir(directory);
}

DirectoryManager::FilesInDir*
DirectoryManager::GetFilesInDir(const std::string& directory) {
  ALOG_ASSERT(directory.empty() || util::EndsWithSlash(directory));
  base::hash_map<std::string, FilesInDir>::iterator it =  // NOLINT
      dir_to_files_.find(directory);
  if (it == dir_to_files_.end())
    return NULL;
  return &(it->second);
}

// static
std::pair<std::string, std::string> DirectoryManager::SplitPath(
    const std::string& pathname) {
  if (pathname.empty())
    return std::make_pair("", "");
  if (pathname.find('/') == std::string::npos)
    return std::make_pair("", pathname);
  if (util::EndsWithSlash(pathname))
    return std::make_pair(pathname, "");

  // |pathname| is not empty and has at least one slash at the beginning of the
  // string or somewhere in the middle.
  std::vector<std::string> paths;
  base::SplitString(pathname, '/', &paths);
  std::string file = paths.back();
  paths.pop_back();
  ALOG_ASSERT(!paths.empty());
  return std::make_pair(JoinString(paths, '/') + "/", file);
}

}  // namespace posix_translation
