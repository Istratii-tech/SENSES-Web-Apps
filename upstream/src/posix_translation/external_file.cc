// Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "posix_translation/external_file.h"

#include <errno.h>

#include <utility>

#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/time/time.h"
#include "common/alog.h"
#include "common/process_emulator.h"
#include "native_client/src/untrusted/irt/irt.h"
#include "posix_translation/directory_file_stream.h"
#include "posix_translation/directory_manager.h"
#include "posix_translation/path_util.h"
#include "posix_translation/statfs.h"
#include "posix_translation/virtual_file_system.h"

namespace posix_translation {

namespace {

const char kExternalFileDirName[] = "external_file";

base::Lock& GetFileSystemMutex() {
  return VirtualFileSystem::GetVirtualFileSystem()->mutex();
}

}  // namespace

///////////////////////////////////////////////////////////////////////////////
// ExternalFileWrapperHandler
ExternalFileWrapperHandler::ExternalFileWrapperHandler(Delegate* delegate)
  : FileSystemHandler("ExternalFileWrapperHandler"),
    delegate_(delegate) {
  nacl_interface_query(NACL_IRT_RANDOM_v0_1, &random_, sizeof(random_));
  ALOG_ASSERT(random_.get_random_bytes);
}

ExternalFileWrapperHandler::~ExternalFileWrapperHandler() {
  STLDeleteValues(&file_handlers_);
}

int ExternalFileWrapperHandler::mkdir(
    const std::string& pathname, mode_t mode) {
  ALOG_ASSERT(!util::EndsWithSlash(pathname));
  if (pathname == root_directory_) {
    // Request to the root directory.
    errno = EEXIST;
    return -1;
  }

  std::string slot = GetSlot(pathname);
  if (slot.empty()) {
    // Request to an invalid path.
    errno = EPERM;
    return -1;
  }

  errno = (slot_file_map_.find(slot) == slot_file_map_.end()) ? EPERM : EEXIST;
  return -1;
}

FileSystemHandler* ExternalFileWrapperHandler::ResolveExternalFile(
    const std::string& pathname) {
  HandlerMap::const_iterator it = file_handlers_.find(pathname);
  if (it != file_handlers_.end())
    return it->second;

  if (!delegate_ || !IsResourcePath(pathname))
    return NULL;

  pp::FileSystem* file_system = NULL;
  std::string path_in_external_fs;
  bool is_writable = false;

  if (!delegate_->ResolveExternalFile(pathname,
                                      &file_system,
                                      &path_in_external_fs,
                                      &is_writable))
    return NULL;

  FileSystemHandler* handler = NULL;
  const std::string path_in_vfs = SetPepperFileSystemLocked(
      make_scoped_ptr(file_system), path_in_external_fs, pathname, &handler);
  ALOG_ASSERT(handler);
  if (is_writable) {
    VirtualFileSystem::GetVirtualFileSystem()->ChangeMountPointOwner(
        path_in_vfs, arc::kFirstAppUid);
  }

  return handler;
}

scoped_refptr<FileStream> ExternalFileWrapperHandler::open(
    int unused_fd, const std::string& pathname, int oflag, mode_t cmode) {
  ALOG_ASSERT(!util::EndsWithSlash(pathname));

  if (pathname != root_directory_) {
    const std::string slot = GetSlot(pathname);
    if (slot.empty() || slot_file_map_.find(slot) == slot_file_map_.end()) {
      // It may be yet unmounted external file
      FileSystemHandler* handler = ResolveExternalFile(pathname);
      if (handler)
        return handler->open(unused_fd, pathname, oflag, cmode);

      errno = ENOENT;
      return NULL;
    }
  }

  // Request to the root or slot directory.
  return new DirectoryFileStream(kExternalFileDirName, pathname, this);
}

int ExternalFileWrapperHandler::stat(
    const std::string& pathname, struct stat* out) {
  ALOG_ASSERT(!util::EndsWithSlash(pathname));
  if (pathname != root_directory_) {
    const std::string slot = GetSlot(pathname);
    if (slot.empty() || slot_file_map_.find(slot) == slot_file_map_.end()) {
      // It may be yet unmounted external file
      FileSystemHandler* handler = ResolveExternalFile(pathname);
      if (handler)
        return handler->stat(pathname, out);

      errno = ENOENT;
      return -1;
    }
  }

  // Request to the root or slot directory.
  scoped_refptr<FileStream> stream =
      new DirectoryFileStream(kExternalFileDirName, pathname, this);
  return stream->fstat(out);
}

int ExternalFileWrapperHandler::statfs(
    const std::string& pathname, struct statfs* out) {
  struct stat st;
  if (this->stat(pathname, &st) == 0)
    return DoStatFsForData(out);
  errno = ENOENT;
  return -1;
}

void ExternalFileWrapperHandler::OnMounted(const std::string& path) {
  ALOG_ASSERT(root_directory_.empty(),
              "Do not mount the same wrapper handler to two or more places: %s",
              path.c_str());
  ALOG_ASSERT(util::EndsWithSlash(path));
  root_directory_ = path;
  util::RemoveTrailingSlashes(&root_directory_);
}

void ExternalFileWrapperHandler::OnUnmounted(const std::string& path) {
  ALOG_ASSERT(path == root_directory_ + "/");
  root_directory_.clear();
}

Dir* ExternalFileWrapperHandler::OnDirectoryContentsNeeded(
    const std::string& pathname) {
  ALOG_ASSERT(!util::EndsWithSlash(pathname));
  DirectoryManager directory;

  if (pathname == root_directory_) {
    // Request for the root directory.
    for (SlotFileMap::iterator i = slot_file_map_.begin();
         i != slot_file_map_.end(); ++i) {
      directory.MakeDirectories(pathname + i->first);
    }
    return directory.OpenDirectory(pathname);
  }

  std::string slot = GetSlot(pathname);
  if (slot.empty()) {
    errno = ENOENT;
    return NULL;
  }

  SlotFileMap::iterator i = slot_file_map_.find(slot);
  if (i == slot_file_map_.end()) {
    errno = ENOENT;
    return NULL;
  }

  // Request for the slot directory.
  directory.MakeDirectories(pathname);
  directory.AddFile(pathname + i->second);
  return directory.OpenDirectory(pathname);
}

bool ExternalFileWrapperHandler::IsResourcePath(
    const std::string& file_path) const {
  ALOG_ASSERT(!root_directory_.empty(), "OnMounted() has not been called.");
  if (!StartsWithASCII(file_path, root_directory_, true) ||
      util::EndsWithSlash(file_path))
    return false;

  std::string slot_with_resource = file_path.substr(root_directory_.size());
  if (slot_with_resource.empty() || slot_with_resource == "/")
    return false;

  ALOG_ASSERT(StartsWithASCII(slot_with_resource, "/", true));

  // We must have only one segment name after slot
  int next_slash = slot_with_resource.find('/', 1);
  if (next_slash == std::string::npos ||
      slot_with_resource.find('/', next_slash + 1) != std::string::npos)
    return false;

  return true;
}

std::string ExternalFileWrapperHandler::GetSlot(
    const std::string& file_path) const {
  ALOG_ASSERT(!root_directory_.empty(), "OnMounted() has not been called.");
  if (!StartsWithASCII(file_path, root_directory_, true) ||
      util::EndsWithSlash(file_path))
    return "";

  std::string slot = file_path.c_str() + root_directory_.size();
  if (slot.empty() || slot == "/")
    return "";

  ALOG_ASSERT(StartsWithASCII(slot, "/", true));
  if (slot.find('/', 1) != std::string::npos)
    return "";

  return slot;
}

std::string ExternalFileWrapperHandler::GenerateUniqueSlotLocked() const {
  // Generate 128-bit random string.
  GetFileSystemMutex().AssertAcquired();
  const ssize_t kRandLen = 16;
  unsigned char buffer[kRandLen];
  for (ssize_t i = 0; i < kRandLen; ) {
    size_t nread = 0;
    int r;
    do {
      r = random_.get_random_bytes(buffer, kRandLen - i, &nread);
      ALOG_ASSERT(r == 0 || r == EINTR);
    } while (r == EINTR);  // try again in the case of EINTR.
    ALOG_ASSERT(nread > 0);
    i += nread;
  }
  return "/" + base::HexEncode(buffer, kRandLen);
}

std::string ExternalFileWrapperHandler::SetPepperFileSystem(
    scoped_ptr<pp::FileSystem> pepper_file_system,
    const std::string& mount_source_in_pepper_file_system,
    const std::string& mount_dest_in_vfs) {
  base::AutoLock lock(GetFileSystemMutex());
  return SetPepperFileSystemLocked(pepper_file_system.Pass(),
                                   mount_source_in_pepper_file_system,
                                   mount_dest_in_vfs,
                                   NULL);
}

std::string ExternalFileWrapperHandler::SetPepperFileSystemLocked(
    scoped_ptr<pp::FileSystem> pepper_file_system,
    const std::string& mount_source_in_pepper_file_system,
    const std::string& mount_dest_in_vfs,
    FileSystemHandler** file_handler) {
  ALOG_ASSERT(pepper_file_system);
  ALOG_ASSERT(util::IsAbsolutePath(mount_source_in_pepper_file_system));
  ALOG_ASSERT(mount_source_in_pepper_file_system.find('/', 1) ==
              std::string::npos);

  std::string slot;
  if (mount_dest_in_vfs.empty()) {
    // If |mount_point_in_vfs| is not specified, mount it on a unique path.
    slot = GenerateUniqueSlotLocked();
  } else {
    ALOG_ASSERT(StartsWithASCII(mount_dest_in_vfs, root_directory_, true));
    ALOG_ASSERT(
        EndsWith(mount_dest_in_vfs, mount_source_in_pepper_file_system, true));
    // Remove leading |root_directory_| and trailing
    // |mount_source_in_pepper_file_system| to get slot.
    // For example, if the |mount_dest_in_vfs| is "/a/b/c/d.txt" and the
    // |root_directory_| is "/a/b" and
    // |mount_source_in_pepper_file_system| is "/d.txt", the slot is just
    // after |root_directory_| and just before
    // |mount_source_in_pepper_file_system|.
    slot = mount_dest_in_vfs.substr(
        root_directory_.size(),
        mount_dest_in_vfs.size() - root_directory_.size() -
        mount_source_in_pepper_file_system.size());
    ALOG_ASSERT(StartsWithASCII(slot, "/", true));
    ALOG_ASSERT(slot.find('/', 1) == std::string::npos);
  }

  ALOG_ASSERT(!GetSlot(root_directory_ + slot).empty());

  std::string mount_point =
      root_directory_ + slot + mount_source_in_pepper_file_system;
  ALOG_ASSERT(mount_dest_in_vfs.empty() || mount_dest_in_vfs == mount_point);

  LOG_ALWAYS_FATAL_IF(
      !slot_file_map_.insert(
          make_pair(slot, mount_source_in_pepper_file_system)).second,
      "%s", mount_point.c_str());
  scoped_ptr<FileSystemHandler> handler;
  {
    // Need to unlock the file system lock since handler creation and Mount
    // requires filesystem lock.
    base::AutoUnlock unlock(GetFileSystemMutex());
    handler = MountExternalFile(pepper_file_system.Pass(),
                                mount_source_in_pepper_file_system,
                                mount_point);
  }
  if (file_handler != NULL)
    *file_handler = handler.get();

  ALOG_ASSERT(file_handlers_.find(mount_point) == file_handlers_.end());
  file_handlers_[mount_point] = handler.release();
  return mount_point;
}

scoped_ptr<FileSystemHandler> ExternalFileWrapperHandler::MountExternalFile(
    scoped_ptr<pp::FileSystem> file_system,
    const std::string& path_in_external_fs,
    const std::string& path_in_vfs) {
  scoped_ptr<FileSystemHandler> handler(new ExternalFileHandler(
      file_system.Pass(), path_in_external_fs, path_in_vfs));
  VirtualFileSystem::GetVirtualFileSystem()->Mount(path_in_vfs, handler.get());
  return handler.Pass();
}

///////////////////////////////////////////////////////////////////////////////
// ExternalFileHandlerBase
ExternalFileHandlerBase::ExternalFileHandlerBase(const char* classname)
    : PepperFileHandler(classname, 0 /* disable cache */) {
}

ExternalFileHandlerBase::~ExternalFileHandlerBase() {
}

std::string ExternalFileHandlerBase::SetPepperFileSystem(
    scoped_ptr<pp::FileSystem> file_system,
    const std::string& path_in_pepperfs,
    const std::string& path_in_vfs) {
  ppapi_file_path_ = path_in_pepperfs;

  // If already mount point path is set, |path_in_vfs| must equal with it.
  ALOG_ASSERT(virtual_file_path_.empty() || virtual_file_path_ == path_in_vfs);
  virtual_file_path_ = path_in_vfs;
  return PepperFileHandler::SetPepperFileSystem(
      file_system.Pass(), path_in_pepperfs, path_in_vfs);
}

int ExternalFileHandlerBase::mkdir(const std::string& pathname, mode_t mode) {
  return PepperFileHandler::mkdir(GetExternalPPAPIPath(pathname), mode);
}

scoped_refptr<FileStream> ExternalFileHandlerBase::open(
    int unused_fd, const std::string& pathname, int oflag, mode_t cmode) {
  return PepperFileHandler::open(unused_fd, GetExternalPPAPIPath(pathname),
                                 oflag, cmode);
}

int ExternalFileHandlerBase::remove(const std::string& pathname) {
  return PepperFileHandler::remove(GetExternalPPAPIPath(pathname));
}

int ExternalFileHandlerBase::rename(const std::string& oldpath,
                                    const std::string& newpath) {
  return PepperFileHandler::rename(GetExternalPPAPIPath(oldpath),
                                   GetExternalPPAPIPath(newpath));
}

int ExternalFileHandlerBase::rmdir(const std::string& pathname) {
  return PepperFileHandler::rmdir(GetExternalPPAPIPath(pathname));
}

int ExternalFileHandlerBase::stat(const std::string& pathname,
                                  struct stat* out) {
  return PepperFileHandler::stat(GetExternalPPAPIPath(pathname), out);
}

int ExternalFileHandlerBase::statfs(
    const std::string& pathname, struct statfs* out) {
  return PepperFileHandler::statfs(GetExternalPPAPIPath(pathname), out);
}

int ExternalFileHandlerBase::truncate(const std::string& pathname,
                                      off64_t length) {
  return PepperFileHandler::truncate(GetExternalPPAPIPath(pathname), length);
}

int ExternalFileHandlerBase::unlink(const std::string& pathname) {
  return PepperFileHandler::unlink(GetExternalPPAPIPath(pathname));
}

int ExternalFileHandlerBase::utimes(const std::string& pathname,
                                    const struct timeval times[2]) {
  return PepperFileHandler::utimes(GetExternalPPAPIPath(pathname), times);
}

void ExternalFileHandlerBase::OnMounted(const std::string& path) {
  return PepperFileHandler::OnMounted(GetExternalPPAPIPath(path));
}

void ExternalFileHandlerBase::OnUnmounted(const std::string& path) {
  return PepperFileHandler::OnUnmounted(GetExternalPPAPIPath(path));
}

void ExternalFileHandlerBase::SetMountPointInVFS(const std::string& path) {
  ALOG_ASSERT(virtual_file_path_.empty(),
              "The mount point has already been set: %s", path.c_str());
  virtual_file_path_ = path;
}

std::string ExternalFileHandlerBase::GetExternalPPAPIPath(
    const std::string& file_path) const {
  std::string output = file_path;

  if (StartsWithASCII(output, virtual_file_path_, true)) {
    ReplaceFirstSubstringAfterOffset(&output, 0, virtual_file_path_,
                                     ppapi_file_path_);
  } else {
    const std::string non_slash_tail_path =
        virtual_file_path_.substr(0, virtual_file_path_.size() - 1);
    if (StartsWithASCII(output, non_slash_tail_path, true)) {
      ReplaceFirstSubstringAfterOffset(&output, 0, non_slash_tail_path,
                                       ppapi_file_path_);
    } else {
      // Some method calls other functions with re-written path. For example
      // PepperFileHandler::statfs calls PepperFileHandler::stat. Passing
      // through without re-writing.
      ALOG_ASSERT(StartsWithASCII(output, ppapi_file_path_, true));
    }
  }
  return output;
}

///////////////////////////////////////////////////////////////////////////////
// ExternalFileHandler
ExternalFileHandler::ExternalFileHandler(
    scoped_ptr<pp::FileSystem> file_system,
    const std::string& ppapi_file_path,
    const std::string& virtual_file_path)
    : ExternalFileHandlerBase("ExternalFileHandler") {
  SetPepperFileSystem(file_system.Pass(), ppapi_file_path, virtual_file_path);
}

ExternalFileHandler::~ExternalFileHandler() {
}

scoped_refptr<FileStream> ExternalFileHandler::open(
    int unused_fd, const std::string& pathname, int oflag, mode_t cmode) {
  // Drop TRUNC and CREAT here because pp::FileIO::Open with TRUNC/CREAT for
  // chosen file does not work. (crbug.com/336160).
  scoped_refptr<FileStream> fs =
      ExternalFileHandlerBase::open(unused_fd, pathname,
                                    oflag & ~(O_TRUNC | O_CREAT), cmode);
  if (fs && (oflag & O_TRUNC))
    fs->ftruncate(0);
  return fs;
}

///////////////////////////////////////////////////////////////////////////////
// ExternalDirectoryHandler
ExternalDirectoryHandler::ExternalDirectoryHandler(
    const std::string& virtual_file_path,
    ExternalDirectoryHandler::Observer* observer)
    : ExternalFileHandlerBase("ExternalDirectoryHandler"),
      observer_(observer) {
  ALOG_ASSERT(observer_.get());
  SetMountPointInVFS(virtual_file_path);
}

ExternalDirectoryHandler::~ExternalDirectoryHandler() {
}

void ExternalDirectoryHandler::Initialize() {
  ALOG_ASSERT(!IsInitialized());
  VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
  sys->mutex().AssertAcquired();

  observer_->OnInitializing();

  // Check IsInitialized again since OnInitializing may initialize this
  // handler synchronously.
  if (!IsInitialized())
    ExternalFileHandlerBase::Initialize();
}

}  // namespace posix_translation
