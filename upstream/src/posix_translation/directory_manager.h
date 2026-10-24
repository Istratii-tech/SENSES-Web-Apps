// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POSIX_TRANSLATION_DIRECTORY_MANAGER_H_
#define POSIX_TRANSLATION_DIRECTORY_MANAGER_H_

#include <dirent.h>

#include <string>
#include <utility>

#include "base/basictypes.h"  // DISALLOW_COPY_AND_ASSIGN
#include "base/containers/hash_tables.h"
#include "gtest/gtest_prod.h"
#include "posix_translation/dir.h"

namespace posix_translation {

class Dir;

// A class to keep track of a list of directories in a file system as well as a
// list of files in each directory. This class is not thread-safe.
class DirectoryManager {
 public:
  DirectoryManager();
  ~DirectoryManager();

  // Clears entire file system.
  void Clear();

  // Adds a file to the manager. |pathname| must be absolute, and must not end
  // with '/'.
  bool AddFile(const std::string& pathname);

  // Adds a file with specified type to the manager. |pathname| must be
  // absolute, and must not end with '/'. |type| is DT_* defined in
  // dirent.h.
  bool AddFileWithType(const std::string& pathname, Dir::Type type);

  // Removes |pathname| and returns true. Returns false if |pathname| is not
  // registered. This function does not remove directories.
  bool RemoveFile(const std::string& pathname);

  // Removes |dirname| if the directory exists and is empty. Both "/usr/bin/"
  // and "/usr/bin" forms are accepted as |dirname|.
  bool RemoveDirectory(const std::string& dirname);

  // Returns true if |pathname| is registered. Returns NULL if |pathname| is
  // not.
  bool StatFile(const std::string& pathname) const;

  // Returns true if the directory, |dirname|, exists. Both "/usr/bin/" and
  // "/usr/bin" forms are accepted as |dirname|.
  bool StatDirectory(const std::string& dirname) const;

  // Returns a Dir object which contains a list of files in |dirname|. Both
  // "/usr/bin/" and "/usr/bin" forms are accepted as |dirname|. Returns NULL
  // if |dirname| is not registered.
  Dir* OpenDirectory(const std::string& dirname) const;

  // Adds a directory or directories to the manager.  Both "/usr/bin/" and
  // "/usr/bin" forms are accepted as |dirname|.
  void MakeDirectories(const std::string& dirname);

  // TODO(crbug.com/190550): If needed, support rmdir.
 private:
  FRIEND_TEST(DirectoryManagerTest, TestAddRemoveFileBasic);
  FRIEND_TEST(DirectoryManagerTest, TestClear);
  FRIEND_TEST(DirectoryManagerTest, TestMakeRemoveDirectory);
  FRIEND_TEST(DirectoryManagerTest, TestGetFilesInDir);
  FRIEND_TEST(DirectoryManagerTest, TestIsAbsolute);
  FRIEND_TEST(DirectoryManagerTest, TestSplitPath);

  class DirImpl;
  typedef std::pair<std::string /* dir */, std::string /* file */> DirAndFile;
  typedef base::hash_map<std::string, Dir::Type> FilesInDir;  // NOLINT

  bool MakeDirectory(const std::string& dirname);
  bool AddFileInternal(const std::string& directory,
                       const std::string& filename,
                       Dir::Type type);

  const FilesInDir* GetFilesInDir(const std::string& directory) const;
  FilesInDir* GetFilesInDir(const std::string& directory);

  // Splits "/path/to/file" into a pair of "/path/to/" and "file".
  static DirAndFile SplitPath(const std::string& pathname);

  // A mapping from a full directory name (e.g. "/usr/lib/") to a list of files
  // in the directory (e.g. {"libc.so.6", "X11/"}). Since we do not support
  // symlinks/hardlinks and we only handle canonicalized file names, we don't
  // have to have a tree. The simple map would suffice.
  base::hash_map<std::string, FilesInDir> dir_to_files_;  // NOLINT

  DISALLOW_COPY_AND_ASSIGN(DirectoryManager);
};

}  // namespace posix_translation
#endif  // POSIX_TRANSLATION_DIRECTORY_MANAGER_H_
