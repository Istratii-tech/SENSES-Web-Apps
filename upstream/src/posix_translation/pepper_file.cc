// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "posix_translation/pepper_file.h"

#include <string.h>  // memset
#include <sys/ioctl.h>

#include <vector>

#include "base/containers/mru_cache.h"
#include "base/files/file_path.h"
#include "base/memory/scoped_ptr.h"
#include "base/safe_strerror_posix.h"
#include "base/strings/stringprintf.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/synchronization/lock.h"
#include "common/arc_strace.h"
#include "common/danger.h"
#include "common/trace_event.h"
#include "posix_translation/dir.h"
#include "posix_translation/directory_file_stream.h"
#include "posix_translation/directory_manager.h"
#include "posix_translation/path_util.h"
#include "posix_translation/real_syscall.h"
#include "posix_translation/statfs.h"
#include "posix_translation/virtual_file_system.h"
#include "ppapi/c/pp_errors.h"
#include "ppapi/c/ppb_file_io.h"
#include "ppapi/cpp/directory_entry.h"
#include "ppapi/cpp/file_ref.h"
#include "ppapi/cpp/private/file_io_private.h"

namespace posix_translation {

namespace {

const char kDatabasePath[] = "/data/data/org.chromium.arc/files/db";

const size_t kMaxFSCacheEntries = 1024;
const blksize_t kBlockSize = 4096;

void CloseHandle(PP_FileHandle native_handle) {
  ALOG_ASSERT(native_handle >= 0);
  const int result = real_close(native_handle);
  if (result != 0 && errno == EBADF) {
    ALOGE("CloseHandle() with native_handle=%d failed with EBADF. This may "
          "indicate double close.", native_handle);
    ALOG_ASSERT(false, "Possible double close detected: native_handle=%d",
                native_handle);
  }
}

}  // namespace

#if defined(DEBUG_POSIX_TRANSLATION)
namespace ipc_stats {

// |VirtualFileSystem::mutex_| must be held before updating these variables.
size_t g_delete;
size_t g_fdatasync;
size_t g_fsync;
size_t g_make_directory;
size_t g_open;
size_t g_query;
size_t g_read_directory_entries;
size_t g_rename;
size_t g_set_length;
size_t g_touch;
uint64_t g_write_bytes;
uint64_t g_read_bytes;

std::string GetIPCStatsAsStringLocked() {
  VirtualFileSystem::GetVirtualFileSystem()->mutex().AssertAcquired();
  const size_t total = g_delete + g_make_directory + g_open + g_query +
      g_read_directory_entries + g_rename + g_set_length + g_touch;
  return base::StringPrintf("PepperFile: Delete:%zu MakeDirectory:%zu Open:%zu "
                            "Query:%zu ReadDirectoryEntries:%zu Rename:%zu "
                            "SetLength:%zu Touch:%zu TOTAL:%zu, "
                            "FSync:%zu FDataSync: %zu, "
                            "BytesWritten: %llu BytesRead: %llu",
                            g_delete, g_make_directory, g_open, g_query,
                            g_read_directory_entries, g_rename, g_set_length,
                            g_touch, total, g_fsync, g_fdatasync,
                            g_write_bytes, g_read_bytes);
}

}  // namespace ipc_stats
#endif

// A MRU cache to avoid doing extra calls to access/stat.
// Access is currently implemented in terms of the same function that stat is
// using. Several applications open files by calling access, followed by stat
// and open. This causes one extra superfluous call to Pepper, that can be
// avoided.
class PepperFileCache {
 public:
  explicit PepperFileCache(size_t size) : size_(size), cache_(size) {}

  bool Get(const std::string& path, PP_FileInfo* file_info, bool* exists) {
    VirtualFileSystem::GetVirtualFileSystem()->mutex().AssertAcquired();
    if (!IsCacheEnabled())
      return false;
    std::string key(path);
    RemoveTrailingSlash(&key);
    const MRUCache::iterator it = cache_.Get(key);
    if (it == cache_.end()) {
      ARC_STRACE_REPORT("PepperFileCache: Cache miss for %s", path.c_str());
      return false;
    }
    ARC_STRACE_REPORT("PepperFileCache: Cache hit for %s", path.c_str());
    if (file_info)
      *file_info = it->second.file_info;
    if (exists)
      *exists = it->second.exists;
    return true;
  }

  // Returns true when the |path| is definitely non-existent. When it exists or
  // when it is unknown, returns false.
  bool IsNonExistent(const std::string& path) {
    VirtualFileSystem::GetVirtualFileSystem()->mutex().AssertAcquired();
    if (!IsCacheEnabled())
      return false;
    bool exists = false;
    if (!Get(path, NULL, &exists))
      return false;  // unknown
    return !exists;
  }

  void Set(const std::string& path, const PP_FileInfo& file_info, bool exists) {
    VirtualFileSystem::GetVirtualFileSystem()->mutex().AssertAcquired();
    if (!IsCacheEnabled())
      return;
    ARC_STRACE_REPORT("PepperFileCache: Adding to cache %s, exists: %s",
                      path.c_str(), exists ? "true" : "false");
    const CacheEntry entry = { exists, file_info };
    std::string key(path);
    RemoveTrailingSlash(&key);
    cache_.Put(key, entry);
  }

  void SetNotExistent(const std::string& path) {
    VirtualFileSystem::GetVirtualFileSystem()->mutex().AssertAcquired();
    if (!IsCacheEnabled())
      return;
    PP_FileInfo dummy = {};
    Set(path, dummy, false);
  }

  void SetNotExistentDirectory(const std::string& path) {
    VirtualFileSystem::GetVirtualFileSystem()->mutex().AssertAcquired();
    if (!IsCacheEnabled())
      return;
    std::string key(path);
    if (!util::EndsWithSlash(key))
      key.append("/");

    PP_FileInfo dummy = {};
    const CacheEntry entry = { false, dummy };
    for (MRUCache::iterator i = cache_.begin(); i != cache_.end(); ++i) {
      if (StartsWithASCII(i->first, key, true))
        i->second = entry;
    }
  }

  void Invalidate(const std::string& path) {
    VirtualFileSystem::GetVirtualFileSystem()->mutex().AssertAcquired();
    if (!IsCacheEnabled())
      return;
    ARC_STRACE_REPORT("PepperFileCache: Cache invalidation for %s",
                      path.c_str());
    std::string key(path);
    RemoveTrailingSlash(&key);
    const MRUCache::iterator it = cache_.Peek(key);
    if (it != cache_.end())
      cache_.Erase(it);
  }

  void InvalidateEntriesWithPrefix(const std::string& prefix) {
    VirtualFileSystem::GetVirtualFileSystem()->mutex().AssertAcquired();
    if (!IsCacheEnabled())
      return;
    Invalidate(prefix);

    // Then invalidate prefix/*
    std::string key(prefix);
    RemoveTrailingSlash(&key);
    std::vector<std::string> targets;
    for (MRUCache::iterator i = cache_.begin(); i != cache_.end(); ++i) {
      if (StartsWithASCII(i->first, key + "/", true))
        targets.push_back(i->first);
    }
    for (int i = 0; i < targets.size(); ++i) {
      ARC_STRACE_REPORT("PepperFileCache: Cache invalidation for %s",
                        targets[i].c_str());
      cache_.Erase(cache_.Peek(targets[i]));
    }
  }

  void Clear() {
    VirtualFileSystem::GetVirtualFileSystem()->mutex().AssertAcquired();
    if (!IsCacheEnabled())
      return;
    ARC_STRACE_REPORT("PepperFileCache: Invalidate all cache entries");
    cache_.Clear();
  }

  void DisableForTesting() {
    VirtualFileSystem::GetVirtualFileSystem()->mutex().AssertAcquired();
    Clear();
    size_ = 0;
  }

 private:
  bool IsCacheEnabled() const {
    return size_ > 0;
  }

  static void RemoveTrailingSlash(std::string* in_out_path) {
    ALOG_ASSERT(in_out_path);
    const size_t len = in_out_path->length();
    if (len < 2 || !util::EndsWithSlash(*in_out_path))
      return;
    in_out_path->erase(len - 1);
  }

  struct CacheEntry {
    bool exists;
    PP_FileInfo file_info;
  };
  typedef base::MRUCache<std::string, CacheEntry> MRUCache;

  size_t size_;
  MRUCache cache_;

  DISALLOW_COPY_AND_ASSIGN(PepperFileCache);
};

class FileIOWrapper {
 public:
  FileIOWrapper(pp::FileIO* file_io, PP_FileHandle native_handle)
      : file_io_(file_io), native_handle_(native_handle) {
  }
  ~FileIOWrapper() {
    CloseHandle(native_handle_);
  }

  pp::FileIO* file_io() { return file_io_.get(); }
  PP_FileHandle native_handle() { return native_handle_; }

 private:
  scoped_ptr<pp::FileIO> file_io_;
  PP_FileHandle native_handle_;

  DISALLOW_COPY_AND_ASSIGN(FileIOWrapper);
};

PepperFileHandler::PepperFileHandler()
    : FileSystemHandler("PepperFileHandler"),
      factory_(this),
      cache_(new PepperFileCache(kMaxFSCacheEntries)) {
}

PepperFileHandler::PepperFileHandler(const char* name, size_t max_cache_size)
    : FileSystemHandler(name),
      factory_(this),
      cache_(new PepperFileCache(max_cache_size)) {
}

PepperFileHandler::~PepperFileHandler() {
}

void PepperFileHandler::OpenPepperFileSystem(pp::Instance* instance) {
  // Since Chrome ignores |kExpectedUsage|, the actual value is not important.
  static const uint64_t kExpectedUsage = 16ULL * 1024 * 1024 * 1024;
  ALOG_ASSERT(pp::Module::Get()->core()->IsMainThread());
  scoped_ptr<pp::FileSystem> file_system(
      new pp::FileSystem(instance, PP_FILESYSTEMTYPE_LOCALPERSISTENT));
  TRACE_EVENT_ASYNC_BEGIN1(ARC_TRACE_CATEGORY,
                           "PepperFileHandler::OpenPepperFileSystem",
                           this, "type", PP_FILESYSTEMTYPE_LOCALPERSISTENT);
  const int32_t result = file_system->Open(
      kExpectedUsage,
      factory_.NewCallback(&PepperFileHandler::OnFileSystemOpen,
                           file_system.release()));
  ALOG_ASSERT(result == PP_OK_COMPLETIONPENDING,
              "Failed to create pp::FileSystem, error: %d", result);
}

void PepperFileHandler::DisableCacheForTesting() {
  cache_->DisableForTesting();
}

void PepperFileHandler::OnFileSystemOpen(
    int32_t result,
    pp::FileSystem* file_system_ptr) {
  scoped_ptr<pp::FileSystem> file_system(file_system_ptr);
  TRACE_EVENT_ASYNC_END1(ARC_TRACE_CATEGORY,
                         "PepperFileHandler::OpenPepperFileSystem",
                         this, "result", result);
  if (result != PP_OK)
    LOG_FATAL("Failed to open pp::FileSystem, error: %d", result);
  SetPepperFileSystem(file_system.Pass(), "/", "/");
}

bool PepperFileHandler::IsInitialized() const {
  VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
  sys->mutex().AssertAcquired();
  return file_system_.get() && sys->IsBrowserReadyLocked();
}

void PepperFileHandler::Initialize() {
  ALOG_ASSERT(!IsInitialized());
  TRACE_EVENT0(ARC_TRACE_CATEGORY, "PepperFileHandler::Initialize");
  VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
  sys->mutex().AssertAcquired();
  ALOG_ASSERT(!IsInitialized());
  while (!IsInitialized())
    sys->Wait();
}

std::string PepperFileHandler::SetPepperFileSystem(
    scoped_ptr<pp::FileSystem> pepper_file_system,
    const std::string& mount_source_in_pepper_file_system,
    const std::string& mount_dest_in_vfs) {
  VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
  base::AutoLock lock(sys->mutex());
  ALOG_ASSERT(pepper_file_system);
  ALOG_ASSERT(!file_system_);
  file_system_ = pepper_file_system.Pass();
  ARC_STRACE_REPORT("Mounting %s in pp::FileSystem %p to %s in VFS",
                    mount_source_in_pepper_file_system.c_str(),
                    file_system_.get(),
                    mount_dest_in_vfs.c_str());
  sys->Broadcast();
  return mount_dest_in_vfs;
}

bool PepperFileHandler::IsWorldWritable(const std::string& pathname) {
  // This is the location of the shared Content Provider database files, which
  // is created/owned by kFirstAppUid.  In order for other contexts to be able
  // to open this to create individual database files, make it world writable.
  if (pathname.compare(0, strlen(kDatabasePath), kDatabasePath) == 0)
    return true;
  // Calling this->stat() every time when VFS::GetFileSystemHandlerLocked() is
  // invoked is too expensive for this handler (and this handler's stat() does
  // not fill the permission part of st_mode anyway). Just returning false
  // would be just fine.
  return false;
}

scoped_refptr<FileStream> PepperFileHandler::open(
    int unused_fd, const std::string& pathname, int oflag, mode_t cmode) {
  // TODO(crbug.com/242355): Use |cmode|.
  TRACE_EVENT2(ARC_TRACE_CATEGORY, "PepperFileHandler::open",
               "pathname", pathname, "oflag", oflag);
  // First, check the cache if O_CREAT is not in |oflag|.
  if (pathname.empty() ||
      (!(oflag & O_CREAT) && cache_->IsNonExistent(pathname))) {
    errno = ENOENT;
    return NULL;
  }

  TRACE_EVENT0(ARC_TRACE_CATEGORY, "PepperFileHandler::open - Pepper");
  const int access_mode = (oflag & O_ACCMODE);

  // When needed, invalidate the |cache_| before calling "new PepperFile" which
  // might unlock the |mutex_|. Note that 'O_RDONLY|O_CREAT' is allowed at least
  // on Linux and it may actually create the file. Just in case, do the same for
  // 'O_RDONLY|O_TRUNC' which may also truncate the file at least on Linux (even
  // though pp::FileIO seems to refuse the latter).
  if ((access_mode != O_RDONLY) || (oflag & (O_CREAT | O_TRUNC)))
    cache_->Invalidate(pathname);

  TRACE_EVENT1(ARC_TRACE_CATEGORY, "PepperFile::open",
               "pathname", pathname);

  VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();

  const int open_flags = ConvertNativeOpenFlagsToPepper(oflag);
  int32_t result;
  scoped_ptr<pp::FileIO_Private> file_io;
  PP_FileHandle file_handle = PP_kInvalidFileHandle;
  {
    // TODO(crbug.com/225152): Fix 225152 and remove |unlock|.
    base::AutoUnlock unlock(sys->mutex());
    pp::FileRef file_ref(*file_system_, pathname.c_str());
    file_io.reset(new pp::FileIO_Private(sys->instance()));
    result = file_io->Open(file_ref, open_flags, pp::BlockUntilComplete());
    if (result == PP_OK) {
      pp::CompletionCallbackWithOutput<pp::PassFileHandle> cb(&file_handle);
      result = file_io->RequestOSFileHandle(cb);
      if (result == PP_OK) {
        if (file_handle >= (sys->GetMaxFd() - sys->GetMinFd() + 1)) {
          // If this path is taken, it likely means that ARC is leaking a native
          // file handle somewhere.
          ALOGE("PPB_FileIO_Private::RequestOSFileHandle returned unexpected "
                "file handle %d for pathname=\"%s\" and oflag=%d.",
                file_handle, pathname.c_str(), oflag);
          ALOG_ASSERT(false, "Possible native handle leak detected: handle=%d",
                      file_handle);
          CloseHandle(file_handle);
          errno = EMFILE;
          return NULL;
        }
      } else {
        ALOGE("PPB_FileIO_Private::RequestOSFileHandle failed for "
              "pathname=\"%s\" and oflag=%d with PP error %d. This usually "
              "means that your app does not have 'unlimitedStorage' "
              "permission.", pathname.c_str(), oflag, result);
      }
    }
  }

  ARC_STRACE_REPORT_PP_ERROR(result);

#if defined(DEBUG_POSIX_TRANSLATION)
  ++ipc_stats::g_open;
#endif
  scoped_refptr<FileStream> stream = NULL;
  if (result == PP_OK) {
    LOG_ALWAYS_FATAL_IF(file_handle == PP_kInvalidFileHandle,
                        "Unexpected file handle %d: %s",
                        file_handle, pathname.c_str());
    if (oflag & O_DIRECTORY) {
      CloseHandle(file_handle);
      errno = ENOTDIR;
      return NULL;
    }
    stream = new PepperFile(oflag, cache_.get(), pathname,
                            new FileIOWrapper(file_io.release(), file_handle));
  } else {
    LOG_ALWAYS_FATAL_IF(file_handle != PP_kInvalidFileHandle,
                        "Unexpected file handle %d: %s",
                        file_handle, pathname.c_str());
    if (result == PP_ERROR_NOTAFILE) {
      // A directory is opened.
      if (access_mode != O_RDONLY) {
        errno = EISDIR;
        return NULL;
      }
      stream = new DirectoryFileStream("pepper", pathname, this);
    } else {
      errno = ConvertPepperErrorToErrno(result);
    }
  }

  return stream;
}

Dir* PepperFileHandler::OnDirectoryContentsNeeded(const std::string& name) {
  TRACE_EVENT1(ARC_TRACE_CATEGORY,
               "PepperFileHandler::OnDirectoryContentsNeeded", "name", name);

  // First, check the cache.
  if (name.empty() || cache_->IsNonExistent(name)) {
    errno = ENOENT;
    return NULL;
  }

  TRACE_EVENT0(ARC_TRACE_CATEGORY,
               "PepperFileHandler::OnDirectoryContentsNeeded - Pepper");
  VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
  int32_t result;

  pp::internal::DirectoryEntryArrayOutputAdapterWithStorage adapter;
  pp::CompletionCallbackWithOutput<std::vector<pp::DirectoryEntry> > cb(
      &adapter);

  {
    // TODO(crbug.com/225152): Fix 225152 and remove |unlock|.
    base::AutoUnlock unlock(sys->mutex());
    pp::FileRef file_ref(*file_system_, name.c_str());
    result = file_ref.ReadDirectoryEntries(cb);
  }
#if defined(DEBUG_POSIX_TRANSLATION)
  ++ipc_stats::g_read_directory_entries;
#endif

  ARC_STRACE_REPORT_PP_ERROR(result);
  if (result != PP_OK) {
    errno = ConvertPepperErrorToErrno(result);
    // getdents should not return these values.
    if (errno == EEXIST || errno == EISDIR ||
        errno == ENOSPC || errno == EPERM) {
      ALOG_ASSERT(false, "errno=%d", errno);
      errno = ENOENT;
    }
    return NULL;
  }

  const std::vector<pp::DirectoryEntry>& directories = adapter.output();
  const base::FilePath base_path(name);
  DirectoryManager directory_manager;
  // We have already confirmed the directory exists. Make sure
  // OpenDirectory will succeed for empty directories by adding the
  // directory we are checking.
  directory_manager.MakeDirectories(name);
  for (size_t i = 0; i < directories.size(); ++i) {
    const pp::DirectoryEntry& entry = directories[i];
    const pp::FileRef& ref = entry.file_ref();
    std::string filename = base_path.Append(ref.GetName().AsString()).value();
    if (entry.file_type() == PP_FILETYPE_DIRECTORY) {
      directory_manager.MakeDirectories(filename);
    } else {
      bool result = directory_manager.AddFile(filename);
      ALOG_ASSERT(result);
    }
  }

  return directory_manager.OpenDirectory(name);
}

int PepperFileHandler::stat(const std::string& pathname, struct stat* out) {
  TRACE_EVENT1(ARC_TRACE_CATEGORY, "PepperFileHandler::stat",
               "pathname", pathname);

  PP_FileInfo file_info = {};
  bool exists = false;
  if (!cache_->Get(pathname, &file_info, &exists)) {
    TRACE_EVENT0(ARC_TRACE_CATEGORY, "PepperFileHandler::stat - Pepper");
    int32_t result = QueryRefLocked(pathname, &file_info);
    ARC_STRACE_REPORT_PP_ERROR(result);
    exists = result == PP_OK;
    cache_->Set(pathname, file_info, exists);
  }

  if (!exists) {
    errno = ENOENT;
    return -1;
  }

  if (file_info.type == PP_FILETYPE_DIRECTORY) {
    scoped_refptr<FileStream> stream =
        new DirectoryFileStream("pepper", pathname, this);
    return stream->fstat(out);
  } else {
    memset(out, 0, sizeof(struct stat));
    // Always assigning 0 (or another constant) to |st_ino| does not always
    // work. For example, since SQLite3 manages the current file lock status
    // per inode (see unixLock() in sqlite/dist/sqlite3.c), always using
    // 0 for |st_ino| may cause deadlock.
    out->st_ino =
        VirtualFileSystem::GetVirtualFileSystem()->GetInodeLocked(pathname);
    out->st_mode = S_IFREG;
    out->st_nlink = 1;
    out->st_size = file_info.size;
    out->st_blksize = kBlockSize;
    // We do not support atime and ctime. See PepperFile::fstat().
    out->st_mtime = static_cast<time_t>(file_info.last_modified_time);
  }

  return 0;
}

int PepperFileHandler::statfs(const std::string& pathname, struct statfs* out) {
  // TODO(crbug.com/242832): Return real values by apps v2 API.
  // http://developer.chrome.com/extensions/experimental.systemInfo.storage.html
  struct stat st;
  if (this->stat(pathname, &st) == 0)
    return DoStatFsForData(out);
  errno = ENOENT;
  return -1;
}

int PepperFileHandler::mkdir(const std::string& pathname, mode_t mode) {
  TRACE_EVENT2(ARC_TRACE_CATEGORY, "PepperFileHandler::mkdir",
               "pathname", pathname, "mode", mode);

  // First, check the cache.
  PP_FileInfo file_info = {};
  bool exists = false;
  if (cache_->Get(pathname, &file_info, &exists) && exists) {
    // |pathname| already exists (either file or directory).
    errno = EEXIST;
    return -1;
  }

  TRACE_EVENT0(ARC_TRACE_CATEGORY, "PepperFileHandler::mkdir - Pepper");
  cache_->Invalidate(pathname);  // call this before unlocking the |mutex_|.
  VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
  int32_t result;
  {
    // TODO(crbug.com/225152): Fix 225152 and remove |unlock|.
    base::AutoUnlock unlock(sys->mutex());
    pp::FileRef file_ref(*file_system_, pathname.c_str());
    result = file_ref.MakeDirectory(PP_MAKEDIRECTORYFLAG_EXCLUSIVE,
                                    pp::BlockUntilComplete());
  }
#if defined(DEBUG_POSIX_TRANSLATION)
  ++ipc_stats::g_make_directory;
#endif
  ARC_STRACE_REPORT_PP_ERROR(result);
  if (result == PP_OK)
    return 0;
  errno = ConvertPepperErrorToErrno(result);
  // mkdir should not return EISDIR.
  if (errno == EISDIR) {
    ALOG_ASSERT(false, "errno=%d", errno);
    errno = ENOENT;
  }
  return -1;
}

int PepperFileHandler::remove(const std::string& pathname) {
  // Remove an empty directory or a file specified by |pathname|.
  TRACE_EVENT1(ARC_TRACE_CATEGORY, "PepperFileHandler::remove",
               "pathname", pathname);

  // First, check the cache.
  if (cache_->IsNonExistent(pathname)) {
    errno = ENOENT;
    return -1;
  }

  TRACE_EVENT0(ARC_TRACE_CATEGORY, "PepperFileHandler::remove - Pepper");
  cache_->Invalidate(pathname);  // call this before unlocking the |mutex_|.
  VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
  int32_t result;
  {
    // TODO(crbug.com/225152): Fix 225152 and remove |unlock|.
    base::AutoUnlock unlock(sys->mutex());
    pp::FileRef file_ref(*file_system_, pathname.c_str());
    result = file_ref.Delete(pp::BlockUntilComplete());
  }
#if defined(DEBUG_POSIX_TRANSLATION)
  ++ipc_stats::g_delete;
#endif
  ARC_STRACE_REPORT_PP_ERROR(result);
  if (result == PP_ERROR_FILENOTFOUND) {
    errno = ENOENT;
    return -1;
  }
  if (result != PP_OK) {
    // TODO(crbug.com/180985): ARC running on Windows might return PP_ERROR
    // to Remove. We might have to add a "delete later" logic here for Windows.
    // Use ConvertPepperErrorToErrno once this issue is resolved.
    errno = EISDIR;
    return -1;
  }
  sys->RemoveInodeLocked(pathname);
  // No need to call SetNotExistentDirectory since remove() can remove only
  // empty directory.
  cache_->SetNotExistent(pathname);
  return 0;
}

int PepperFileHandler::rename(const std::string& oldpath,
                              const std::string& newpath) {
  TRACE_EVENT2(ARC_TRACE_CATEGORY, "PepperFileHandler::rename",
               "oldpath", oldpath, "newpath", newpath);

  // First, check the cache.
  if (cache_->IsNonExistent(oldpath)) {
    errno = ENOENT;
    return -1;
  }

  TRACE_EVENT0(ARC_TRACE_CATEGORY, "PepperFileHandler::rename - Pepper");
  PP_FileInfo old_file_info = {};
  bool old_file_has_metadata = cache_->Get(oldpath, &old_file_info, NULL);
  // Invalidate before unlocking the |mutex_|.
  cache_->InvalidateEntriesWithPrefix(oldpath);
  cache_->InvalidateEntriesWithPrefix(newpath);
  VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
  int32_t result;
  {
    // TODO(crbug.com/225152): Fix 225152 and remove |unlock|.
    base::AutoUnlock unlock(sys->mutex());
    pp::FileRef old_file_ref(*file_system_, oldpath.c_str());
    pp::FileRef new_file_ref(*file_system_, newpath.c_str());
    result = old_file_ref.Rename(
        new_file_ref, pp::BlockUntilComplete());
  }
#if defined(DEBUG_POSIX_TRANSLATION)
  ++ipc_stats::g_rename;
#endif
  ARC_STRACE_REPORT_PP_ERROR(result);
  if (result != PP_OK) {
    errno = ConvertPepperErrorToErrno(result);
    return -1;
  }
  if (oldpath != newpath)
    cache_->SetNotExistentDirectory(oldpath);
  if (old_file_has_metadata)
    cache_->Set(newpath, old_file_info, true);  // rename preserves metadata.
  sys->ReassignInodeLocked(oldpath, newpath);
  // rename() should not change inode.
  return 0;
}

int PepperFileHandler::rmdir(const std::string& pathname) {
  // TODO(crbug.com/190550): Implement this properly. Note that we should
  // return ENOTDIR if |pathname| is a file, but right now we do not have a
  // good way to perform the check without unlocking the |mutex|. For now,
  // just call remove() since some apps require this API and a file name is
  // usually not passed to rmdir(). To fix this issue properly, we likely
  // have to add an API to pp::FileRef.
  ALOGW("PepperFileHandler::rmdir is not fully POSIX compatible and may"
        " delete a file: %s", pathname.c_str());
  return this->remove(pathname);
}

int PepperFileHandler::truncate(const std::string& pathname,
                                off64_t length) {
  TRACE_EVENT2(ARC_TRACE_CATEGORY, "PepperFileHandler::truncate",
               "pathname", pathname, "length", length);

  // First, check the cache.
  if (cache_->IsNonExistent(pathname)) {
    errno = ENOENT;
    return -1;
  }

  TRACE_EVENT0(ARC_TRACE_CATEGORY, "PepperFileHandler::truncate - Pepper");
  scoped_refptr<FileStream> stream = this->open(-1, pathname, O_WRONLY, 0);
  if (stream == NULL) {
    // truncate should not return these errno values.
    if (errno == EEXIST || errno == ENOMEM || errno == ENOSPC) {
      ALOG_ASSERT(false, "errno=%d", errno);
      errno = ENOENT;
    }
    return -1;
  }
  return stream->ftruncate(length);
}

int PepperFileHandler::unlink(const std::string& pathname) {
  // TODO(crbug.com/190550): Return EISDIR if |pathname| is a directory. Right
  // now, we do not have a good way to perform the check without unlocking the
  // |mutex|.
  return this->remove(pathname);
}

int PepperFileHandler::utimes(const std::string& pathname,
                              const struct timeval times[2]) {
  TRACE_EVENT1(ARC_TRACE_CATEGORY, "PepperFileHandler::utimes",
               "pathname", pathname);

  // First, check the cache.
  if (cache_->IsNonExistent(pathname)) {
    errno = ENOENT;
    return -1;
  }

  TRACE_EVENT0(ARC_TRACE_CATEGORY, "PepperFileHandler::utimes - Pepper");
  VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
  if (!times) {
    errno = EACCES;
    return -1;
  }
  cache_->Invalidate(pathname);  // call this before unlocking the |mutex_|.
  int32_t result;
  {
    // TODO(crbug.com/225152): Fix 225152 and remove |unlock|.
    base::AutoUnlock unlock(sys->mutex());
    pp::FileRef file_ref(*file_system_, pathname.c_str());
    result = file_ref.Touch(
        times[0].tv_sec, times[1].tv_sec, pp::BlockUntilComplete());
  }
#if defined(DEBUG_POSIX_TRANSLATION)
  ++ipc_stats::g_touch;
#endif
  ARC_STRACE_REPORT_PP_ERROR(result);
  if (result != PP_OK) {
    errno = ConvertPepperErrorToErrno(result);
    // utimes should not return these errno values.
    if (errno == EEXIST || errno == EISDIR ||
        errno == ENOMEM || errno == ENOSPC) {
      ALOG_ASSERT(false, "errno=%d", errno);
      errno = ENOENT;
    }
    return -1;
  }
  return 0;
}

void PepperFileHandler::InvalidateCache() {
  cache_->Clear();
}

void PepperFileHandler::AddToCache(const std::string& path,
                                   const PP_FileInfo& file_info,
                                   bool exists) {
  cache_->Set(path, file_info, exists);
}

void PepperFileHandler::OnMounted(const std::string& path) {
  // Check if |path| being mounted exists. If this function is called on the
  // main thread, do not check the existence. There are two cases when this
  // function is called on the main thread: during handler initialization, the
  // library user mounts a static set of paths that are known to be validn.
  // The other case is that the external file handler mounts an existing
  // external file.
  // Note: It is better to move this check to MountPointManager::Add, but doing
  // so breaks many unit tests outside this library.
  PP_FileInfo info;
  ALOG_ASSERT(pp::Module::Get()->core()->IsMainThread() ||
              (QueryRefLocked(path, &info) == PP_OK),
              "Unknown path '%s' is mounted", path.c_str());

  // Update the cache when possible.
  if (!util::EndsWithSlash(path)) {
    // Ignore OnMounted calls against files since it is difficult to fill the
    // cache for files. Note that chown("/path/to/pepper/file", ..) may end up
    // taking this path.
    return;
  }
  PP_FileInfo file_info = {};
  file_info.size = 4096;
  file_info.type = PP_FILETYPE_DIRECTORY;
  // For directories, we do not have to fill mtime. See DirectoryFileStream.cc.
  cache_->Set(path, file_info, true);
}

void PepperFileHandler::OnUnmounted(const std::string& path) {
  cache_->Invalidate(path);
}

int32_t PepperFileHandler::QueryRefLocked(const std::string& pathname,
                                          PP_FileInfo* out_file_info) {
  VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
  sys->mutex().AssertAcquired();
#if defined(DEBUG_POSIX_TRANSLATION)
  ++ipc_stats::g_query;
#endif
  // TODO(crbug.com/225152): Fix 225152 and remove |unlock|.
  base::AutoUnlock unlock(sys->mutex());
  pp::FileRef file_ref(*file_system_, pathname.c_str());
  pp::CompletionCallbackWithOutput<PP_FileInfo> cb(out_file_info);
  return file_ref.Query(cb);
}

// static
int PepperFileHandler::ConvertPepperErrorToErrno(int pp_error) {
  switch (pp_error) {
    case PP_ERROR_FILENOTFOUND:
      return ENOENT;
    case PP_ERROR_FILEEXISTS:
      return EEXIST;
    case PP_ERROR_NOACCESS:
      // This error code is returned when the system tries to write
      // something to CRX file system. As the CRX file system is
      // read-only, EPERM is more appropriate than EACCES.
      return EPERM;
    case PP_ERROR_NOMEMORY:
      return ENOMEM;
    case PP_ERROR_NOQUOTA:
    case PP_ERROR_NOSPACE:
      return ENOSPC;
    case PP_ERROR_NOTAFILE:
      return EISDIR;
    case PP_ERROR_BADRESOURCE:
      return EBADF;
    default:
      // TODO(crbug.com/293953): Some of PP_ERROR_FAILED should be ENOTDIR.
      DANGERF("Unknown Pepper error code: %d", pp_error);
      return ENOENT;
  }
}

// static
int PepperFileHandler::ConvertNativeOpenFlagsToPepper(int native_flags) {
  int pepper_flags = 0;

  if ((native_flags & O_ACCMODE) == O_WRONLY) {
    pepper_flags = PP_FILEOPENFLAG_WRITE;
  } else if ((native_flags & O_ACCMODE) == O_RDONLY) {
    pepper_flags = PP_FILEOPENFLAG_READ;
  } else if ((native_flags & O_ACCMODE) == O_RDWR) {
    pepper_flags = PP_FILEOPENFLAG_READ | PP_FILEOPENFLAG_WRITE;
  } else {
    ALOGW("Unknown open flags %o, falling back to O_RDONLY", native_flags);
    pepper_flags = PP_FILEOPENFLAG_READ;
  }

  if (native_flags & O_CREAT)
    pepper_flags |= PP_FILEOPENFLAG_CREATE;
  if (native_flags & O_EXCL)
    pepper_flags |= PP_FILEOPENFLAG_EXCLUSIVE;
  if (native_flags & O_TRUNC)
    pepper_flags |= PP_FILEOPENFLAG_TRUNCATE;

  if (native_flags & O_NOCTTY)
    ALOGW("O_NOCTTY is not supported");
  if (native_flags & O_NONBLOCK)
    ALOGW("O_NONBLOCK is not supported");
  if (native_flags & O_SYNC)
    ALOGW("O_SYNC is not supported");
  if (native_flags & O_ASYNC)
    ALOGW("O_ASYNC is not supported");
  if (native_flags & O_NOFOLLOW)
    ALOGW("O_NOFOLLOW is not supported");
  if (native_flags & O_CLOEXEC)
    ALOGW("O_CLOEXEC is not supported");
  if (native_flags & O_NOATIME)
    ALOGW("O_NOATIME is not supported");

  if (native_flags & O_APPEND) {
    if (pepper_flags & PP_FILEOPENFLAG_TRUNCATE) {
      // TODO(crbug.com/308809): Support O_APPEND | O_TRUNC file open.
      ALOGW("O_TRUNC with O_APPEND is not supported.");
    }
    if (pepper_flags & PP_FILEOPENFLAG_WRITE) {
      // _WRITE and _APPEND flags are exclusive in Pepper.
      pepper_flags |= PP_FILEOPENFLAG_APPEND;
      pepper_flags &= ~PP_FILEOPENFLAG_WRITE;
    } else {
      ALOGW("O_APPEND is specified with O_RDONLY. Ignored.");
    }
  }

  return pepper_flags;
}

bool PepperFileHandler::IsNonExistent(const std::string& path) {
  return cache_->IsNonExistent(path);
}

void PepperFileHandler::SetNotExistent(const std::string& path) {
  cache_->SetNotExistent(path);
}

//------------------------------------------------------------------------------

PepperFile::PepperFile(int oflag,
                       PepperFileCache* cache,
                       const std::string& pathname,
                       FileIOWrapper* file_wrapper)
    : FileStream(oflag, pathname),
      factory_(this),
      cache_(cache),
      file_(file_wrapper) {
  ALOG_ASSERT(cache);
  ALOG_ASSERT(file_wrapper);
}

PepperFile::~PepperFile() {}

void* PepperFile::mmap(
    void* addr, size_t length, int prot, int flags, off_t offset) {
  void* result =
      ::mmap(addr, length, prot, flags, file_->native_handle(), offset);
  if (prot & PROT_WRITE)
    cache_->Invalidate(pathname());
  return result;
}

int PepperFile::munmap(void* addr, size_t length) {
  int result = ::munmap(addr, length);
  if ((oflag() & O_ACCMODE) != O_RDONLY)
    cache_->Invalidate(pathname());
  return result;
}

ssize_t PepperFile::read(void* buf, size_t count) {
  const ssize_t result = real_read(file_->native_handle(), buf, count);
#if defined(DEBUG_POSIX_TRANSLATION)
  if (result > 0)
    ipc_stats::g_read_bytes += result;
#endif
  return result;
}

// Note for atomicity of the write/pread/pwrite operations below:
//
// PepperFile::write(), PepperFile::pread(), and PepperFile::pwrite() calls
// lseek() to emulate Linux kernel's behavior. The
// "lseek-lseek-read/write-lseek" (for emulating pread and pwrite) sequence
// is safe for the following reasons.
//
// * Only the PPAPI (or NaCl) process for the app and HTML5 FS code in browser
//   process access files for the app in the FS.
// * For each app, only one PPAPI (or NaCl) process is started.
// * All POSIX compatible functions in this file are synchronized. For example,
//   VirtualFileSystem::write locks the |mutex_| before calling into
//   PepperFile::write.
// * All operations that might change the file offset of a file descriptor,
//   PepperFile::lseek, PepperFile::read, PepperFile::write, PepperFile::pread,
//   and PepperFile::pwrite, are done within this process. They never issues an
//   IPC.
// * Other asynchronous operations, such as PepperFileHandler::unlink,
//   PepperFileHandler::truncate, and PepperFile::ftruncate could be done in the
//   browser process in parallel to the lseek, read, write, pread, and pwrite
//   operations above, but the operations in the browser never change the offset
//   of a descriptor.

ssize_t PepperFile::write(const void* buf, size_t count) {
  cache_->Invalidate(pathname());
  const ssize_t result = real_write(file_->native_handle(), buf, count);
#if defined(DEBUG_POSIX_TRANSLATION)
  if (result > 0)
    ipc_stats::g_write_bytes += result;
#endif
  return result;
}

void PepperFile::debug_write(const void* buf, size_t count) {
  real_write(file_->native_handle(), buf, count);
}

off64_t PepperFile::lseek(off64_t offset, int whence) {
  return real_lseek64(file_->native_handle(), offset, whence);
}

int PepperFile::fdatasync() {
  TRACE_EVENT0(ARC_TRACE_CATEGORY, "PepperFile::fdatasync");
  // TODO(crbug.com/242349): Call NaCl IRT or pp::FileIO::Flush().
  ARC_STRACE_REPORT("not implemented yet");
#if defined(DEBUG_POSIX_TRANSLATION)
  ++ipc_stats::g_fdatasync;
#endif
  return 0;
}

int PepperFile::fstat(struct stat* out) {
  int result = real_fstat(file_->native_handle(), out);
  if (!result) {
    // If we expose the values got from host filesystem, the result
    // will be inconsistent with stat and lstat. Let VirtualFileSystem set
    // permission bits.
    out->st_mode &= ~0777;
    out->st_ino = inode();
    // Overwrite the real dev/rdev numbers with zero. This is necessary for e.g.
    // dexopt to work. dvmOpenCachedDexFile() in DexPrepare.cpp checks if st_dev
    // numbers returned from ::stat(path) and ::fstat(fd_for_the_path) are the
    // same, and retries until they return the same st_dev numbers.
    out->st_dev = out->st_rdev = 0;
    // We do not support atime and ctime. Note that java.io.File does not
    // provide a way to access them.
    out->st_atime = out->st_ctime = 0;
    // TODO(crbug.com/242337): Fill this value?
    out->st_blocks = 0;
    out->st_blksize = kBlockSize;
  }
  return result;
}

int PepperFile::fsync() {
  TRACE_EVENT0(ARC_TRACE_CATEGORY, "PepperFile::fsync");
  // TODO(crbug.com/242349): Call NaCl IRT or pp::FileIO::Flush().
  ARC_STRACE_REPORT("not implemented yet");
#if defined(DEBUG_POSIX_TRANSLATION)
  ++ipc_stats::g_fsync;
#endif
  return 0;
}

int PepperFile::ftruncate(off64_t length) {
  TRACE_EVENT1(ARC_TRACE_CATEGORY, "PepperFile::ftruncate", "length", length);

  if ((oflag() & O_ACCMODE) == O_RDONLY) {
    errno = EBADF;
    return -1;
  }

  cache_->Invalidate(pathname());
  VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
  int32_t result;
  {
    // TODO(crbug.com/225152): Fix 225152 and remove |unlock|.
    base::AutoUnlock unlock(sys->mutex());
    pp::FileIO* file_io = file_->file_io();
    result = file_io->SetLength(length, pp::BlockUntilComplete());
  }
#if defined(DEBUG_POSIX_TRANSLATION)
  ++ipc_stats::g_set_length;
#endif
  ARC_STRACE_REPORT_PP_ERROR(result);
  if (result != PP_OK) {
    DANGERF("ftruncate failed with Pepper error code: %d", result);
    errno = EACCES;
    return -1;
  }
  return 0;
}

int PepperFile::ioctl(int request, va_list ap) {
  if (request == FIONREAD) {
    // According to "man ioctl_list", FIONREAD stores its value as an int*.
    int* argp = va_arg(ap, int*);
    *argp = 0;
    off64_t pos = this->lseek(0, SEEK_CUR);
    if (pos == -1) {
      ALOGE("lseek(cur) returned error %d", errno);
      errno = EINVAL;
      return -1;
    }
    struct stat st;
    if (this->fstat(&st)) {
      ALOGE("fstat() returned error %d", errno);
      errno = EINVAL;
      return -1;
    }
    if (pos < st.st_size)
      *argp = (st.st_size - pos);
    return 0;
  }
  ALOGE("ioctl command %d not supported\n", request);
  errno = EINVAL;
  return -1;
}

const char* PepperFile::GetStreamType() const {
  return "pepper";
}

size_t PepperFile::GetSize() const {
  struct stat st;
  if (const_cast<PepperFile*>(this)->fstat(&st))
    return 0;  // unknown size
  return st.st_size;
}

}  // namespace posix_translation
