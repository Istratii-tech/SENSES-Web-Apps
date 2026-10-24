// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Handles files listed in the NaCl manifest file.

#include "posix_translation/nacl_manifest_file.h"

#include <string.h>

#include <utility>
#include <vector>

#include "base/strings/string_piece.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/synchronization/lock.h"
#include "common/arc_strace.h"
#include "common/file_util.h"
#include "common/trace_event.h"
#include "posix_translation/dir.h"
#include "posix_translation/directory_file_stream.h"
#include "posix_translation/file_stream.h"  // DirectoryFileStream
#include "posix_translation/statfs.h"
#include "posix_translation/virtual_file_system.h"

namespace posix_translation {

NaClManifestFileHandler::NaClManifestFileHandler(const NaClManifestEntry* files,
                                                 size_t num_files)
    : FileSystemHandler("NaClManifestFileHandler") {
  if (!nacl_interface_query(NACL_IRT_RESOURCE_OPEN_v0_1,
                            &resource_open_, sizeof(resource_open_))) {
    ALOG_ASSERT(false, "Query for NACL_IRT_RESOURCE_OPEN_v0_1 has failed");
  }
  InitializeDirectoryManager(files, num_files);
}

NaClManifestFileHandler::~NaClManifestFileHandler() {
}

void NaClManifestFileHandler::AddToFdCacheLocked(
    const std::string& pathname, int fd) {
  VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
  sys->mutex().AssertAcquired();

  ALOG_ASSERT(!pathname.empty());
  ALOG_ASSERT(fd >= 0);
  fd_cache_.insert(std::make_pair(pathname, fd));
}

bool NaClManifestFileHandler::ExistsLocked(const std::string& pathname) {
  VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
  sys->mutex().AssertAcquired();

  if (!directory_manager_.StatFile(pathname) &&
      !directory_manager_.StatDirectory(pathname)) {
    ARC_STRACE_REPORT("%s is not found", pathname.c_str());
    return false;
  }
  return true;
}

int NaClManifestFileHandler::OpenLocked(const std::string& pathname) {
  TRACE_EVENT1(ARC_TRACE_CATEGORY, "NaClManifestFileHandler::OpenLocked",
               "pathname", pathname);

  VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
  sys->mutex().AssertAcquired();
  ALOG_ASSERT(ExistsLocked(pathname));

  int fd = -1;
  std::string real_path(pathname);
  const char* key = arc::GetBaseName(pathname.c_str());
  if (key[0] == '\0')
    return -1;

  // open_resource() is kind of a special IRT call which asks the main thread
  // to talk to the renderer process with SRPC and the thread which called
  // open_resource() itself is suspended waiting for the operation on the main
  // thread to be done. For that reason, the |mutex_| should be unlocked before
  // calling open_resource() to avoid dead lock. See crbug.com/274233 and
  // native_client/src/untrusted/irt/irt_manifest.c for more details.
  // TODO(crbug.com/225152): Fix 225152 and remove |unlock|.
  ARC_STRACE_REPORT("Slow path - Calling open_resource(\"%s\")", key);
  base::AutoUnlock unlock(sys->mutex());
  if (resource_open_.open_resource(key, &fd))
    return -1;
  return fd;
}

void NaClManifestFileHandler::InitializeDirectoryManager(
    const NaClManifestEntry* files, size_t num_files) {
  VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
  for (size_t i = 0; i < num_files; ++i) {
    struct stat st = {};
    st.st_mode = files[i].mode;
    st.st_size = files[i].size;
    st.st_mtime = files[i].mtime;
    {
      base::AutoLock lock(sys->mutex());
      // Note: This fails if |files[i].name| is not a normalized path name.
      st.st_ino = sys->GetInodeLocked(files[i].name);
    }
    ALOG_ASSERT(st.st_mode & S_IFREG);
    ALOG_ASSERT(st.st_size > 0);
    ALOG_ASSERT(st.st_mtime > 0);
    ALOG_ASSERT(st.st_ino > 0);
    const bool insert_result =
        stat_cache_.insert(std::make_pair(files[i].name, st)).second;
    ALOG_ASSERT(insert_result);

    std::string name = files[i].name;
    ARC_STRACE_REPORT("Found %s", name.c_str());
    directory_manager_.AddFile(name);
  }
}

scoped_refptr<FileStream> NaClManifestFileHandler::open(
    int fd, const std::string& pathname, int oflag, mode_t cmode) {
  if (oflag & (O_WRONLY | O_RDWR)) {
    errno = EACCES;
    return NULL;
  }

  // Check if |pathname| is a directory.
  if (directory_manager_.StatDirectory(pathname))
    return new DirectoryFileStream("nmf", pathname, this);

  if (oflag & O_DIRECTORY) {
    errno = ENOTDIR;
    return NULL;
  }

  struct stat st;
  if (this->stat(pathname, &st))
    return NULL;  // the file does not exist.

  // First, search for the FD cache.
  int native_handle;
  std::multimap<std::string, int>::iterator it = fd_cache_.find(pathname);
  if (it != fd_cache_.end()) {
    native_handle = it->second;
    fd_cache_.erase(it);
    ARC_STRACE_REPORT("Reusing a cached NaCl descriptor %d for %s",
                      native_handle, pathname.c_str());
  } else {
    // Then, try to open the file with open_resource.
    native_handle = OpenLocked(pathname);
    if (native_handle < 0) {
      errno = ENOENT;
      return NULL;
    }
  }
  return new NaClManifestFile(native_handle, pathname, oflag, st, this);
}

Dir* NaClManifestFileHandler::OnDirectoryContentsNeeded(
    const std::string& name) {
  return directory_manager_.OpenDirectory(name);
}

int NaClManifestFileHandler::stat(const std::string& pathname,
                                  struct stat* out) {
  if (directory_manager_.StatDirectory(pathname)) {
    scoped_refptr<FileStream> stream =
        new DirectoryFileStream("nmf", pathname, this);
    return stream->fstat(out);
  }

  base::hash_map<std::string, struct stat>::iterator it =  // NOLINT
    stat_cache_.find(pathname);
  if (it != stat_cache_.end()) {
    *out = it->second;
    return 0;
  }
  errno = ENOENT;
  return -1;
}

int NaClManifestFileHandler::statfs(const std::string& pathname,
                                    struct statfs* out) {
  // TODO(crbug.com/269075): Implement this.
  if (ExistsLocked(pathname))
    return DoStatFsForSystem(out);
  errno = ENOENT;
  return -1;
}

int NaClManifestFileHandler::mkdir(const std::string& pathname, mode_t mode) {
  if (ExistsLocked(pathname)) {
    errno = EEXIST;
    return -1;
  }
  errno = EACCES;
  return -1;
}

int NaClManifestFileHandler::rename(const std::string& oldpath,
                                    const std::string& newpath) {
  if (!ExistsLocked(oldpath) || newpath.empty()) {
    errno = ENOENT;
    return -1;
  }
  if (oldpath == newpath)
    return 0;
  errno = EACCES;
  return -1;
}

int NaClManifestFileHandler::truncate(const std::string& pathname,
                                      off64_t length) {
  if (!ExistsLocked(pathname))
    errno = ENOENT;
  else
    errno = EACCES;
  return -1;
}

int NaClManifestFileHandler::unlink(const std::string& pathname) {
  if (!ExistsLocked(pathname))
    errno = ENOENT;
  else
    errno = EACCES;
  return -1;
}

int NaClManifestFileHandler::utimes(const std::string& pathname,
                                    const struct timeval times[2]) {
  if (!ExistsLocked(pathname))
    errno = ENOENT;
  else
    errno = EACCES;
  return -1;
}

NaClManifestFile::NaClManifestFile(int native_handle,
                                   const std::string& pathname, int oflag,
                                   const struct stat& st,
                                   NaClManifestFileHandler* handler)
    : PassthroughStream(native_handle, pathname, oflag,
                        false),  // The |native_handle| will NEVER be closed on
                                 // destruction.
      st_(st),
      handler_(handler) {
  ALOG_ASSERT(native_handle >= 0);
  ALOG_ASSERT(!pathname.empty());
  ALOG_ASSERT(st_.st_ino == inode());
  ALOG_ASSERT(handler);
}

NaClManifestFile::~NaClManifestFile() {
  this->lseek(0, SEEK_SET);
  ARC_STRACE_REPORT("Adding NaCl descriptor %d for %s to the cache",
                    native_fd(), pathname().c_str());
  handler_->AddToFdCacheLocked(pathname(), native_fd());
}

int NaClManifestFile::fstat(struct stat* out) {
  *out = st_;
  return 0;
}

int NaClManifestFile::fstatfs(struct statfs* out) {
  return DoStatFsForSystem(out);
}

ssize_t NaClManifestFile::write(const void* buf, size_t count) {
  errno = EINVAL;
  return -1;
}

const char* NaClManifestFile::GetStreamType() const {
  return "nmf";  // should be <=8 characters.
}

}  // namespace posix_translation
