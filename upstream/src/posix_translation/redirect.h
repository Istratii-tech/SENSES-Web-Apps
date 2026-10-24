// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POSIX_TRANSLATION_REDIRECT_H_
#define POSIX_TRANSLATION_REDIRECT_H_

#include <string>
#include <utility>
#include <vector>

#include "base/compiler_specific.h"
#include "base/containers/hash_tables.h"
#include "base/memory/scoped_ptr.h"
#include "common/export.h"
#include "posix_translation/file_system_handler.h"

namespace posix_translation {

// A thin wrapper around an existing FileSystemHandler class. This class handles
// symlink() and readlink() calls to add a non-persistent symbolic link feature
// to the existing handler class.
class ARC_EXPORT RedirectHandler : public FileSystemHandler {
 public:
  // |underlying| is the handler which handles all FileSystemHandler calls
  // except readlink() and symlink(). The handler must be used only by a
  // redirect handler because the redirect handler delegates all calls
  // including IsInitialized, Initialize, and so on. RedirectHandler takes
  // ownership of the |underlying| handler if |own_underlying| is true.
  // |symlinks| are an array of a pair of dest/src path names that are added
  // to the handler during its initialization. Unlike symlink() which may
  // return EEXIST, the existence of src paths passed to the constructor are
  // never checked.
  RedirectHandler(
      FileSystemHandler* underlying,
      const std::vector<std::pair<std::string, std::string> >& symlinks,
      bool own_underlying);
  virtual ~RedirectHandler();

  // FileSystemHandler overrides. This class should override ALL virtual
  // functions in FileSystemHandler.
  virtual bool IsInitialized() const OVERRIDE;
  virtual void Initialize() OVERRIDE;
  virtual void OnMounted(const std::string& path) OVERRIDE;
  virtual void OnUnmounted(const std::string& path) OVERRIDE;
  virtual void InvalidateCache() OVERRIDE;
  virtual void AddToCache(const std::string& path,
                          const PP_FileInfo& file_info,
                          bool exists) OVERRIDE;
  virtual bool IsWorldWritable(const std::string& pathname) OVERRIDE;
  virtual std::string SetPepperFileSystem(
      scoped_ptr<pp::FileSystem> pepper_file_system,
      const std::string& mount_source_in_pepper_file_system,
      const std::string& mount_dest_in_vfs) OVERRIDE;

  virtual int mkdir(const std::string& pathname, mode_t mode) OVERRIDE;
  virtual scoped_refptr<FileStream> open(
      int fd, const std::string& pathname, int oflag, mode_t cmode) OVERRIDE;
  virtual Dir* OnDirectoryContentsNeeded(const std::string& name) OVERRIDE;
  virtual ssize_t readlink(const std::string& pathname,
                           std::string* resolved) OVERRIDE;
  virtual int remove(const std::string& pathname) OVERRIDE;
  virtual int rename(const std::string& oldpath,
                     const std::string& newpath) OVERRIDE;
  virtual int rmdir(const std::string& pathname) OVERRIDE;
  virtual int stat(const std::string& pathname, struct stat* out) OVERRIDE;
  virtual int statfs(const std::string& pathname, struct statfs* out) OVERRIDE;
  virtual int symlink(const std::string& oldpath,
                      const std::string& newpath) OVERRIDE;
  virtual int truncate(const std::string& pathname, off64_t length) OVERRIDE;
  virtual int unlink(const std::string& pathname) OVERRIDE;
  virtual int utimes(const std::string& pathname,
                     const struct timeval times[2]) OVERRIDE;

 private:
  void AddSymlink(const std::string& dest, const std::string& src);
  std::string GetSymlinkTarget(const std::string& src) const;
  bool RemoveSymlinkTarget(const std::string& src);

  // True if this handler has been initialized.
  bool is_initialized_;

  // A map from a source file to a link target.
  base::hash_map<std::string, std::string> symlinks_;  // NOLINT

  // A map from a directory containing symlink(s) to the symlinks. For
  // example, when /dir/a points to /foo, and /dir/b points to /bar,
  // dir_to_symlinks_ has "/dir" as a key, and ["a", "b"] as its value.
  base::hash_map<  // NOLINT
    std::string, std::vector<std::string> > dir_to_symlinks_;

  // The handler which handles all calls except readlink() and symlink().
  // TODO(crbug.com/423063): Use a ref-counted smart pointer to remove the
  // own_underlying_ hack.
  scoped_ptr<FileSystemHandler> underlying_;
  const bool own_underlying_;

  std::string mount_point_;

  DISALLOW_COPY_AND_ASSIGN(RedirectHandler);
};

}  // namespace posix_translation

#endif  // POSIX_TRANSLATION_REDIRECT_H_
