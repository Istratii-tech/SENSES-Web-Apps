// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "posix_translation/crx_file.h"

#include <stdarg.h>

#include "base/strings/utf_string_conversions.h"
#include "common/alog.h"
#include "common/trace_event.h"
#include "ppapi/cpp/private/ext_crx_file_system_private.h"

namespace posix_translation {

namespace {

int Fcntl(scoped_refptr<FileStream> stream, int cmd, ...) {
  va_list ap;
  va_start(ap, cmd);
  const int result = stream->fcntl(cmd, ap);
  va_end(ap);
  return result;
}

}  // namespace

CrxFileHandler::CrxFileHandler()
    : PepperFileHandler("CrxFileHandler", 16 /* cache size */),
      factory_(this) {
}

CrxFileHandler::~CrxFileHandler() {
  for (std::map<StreamCacheKey, scoped_refptr<FileStream> >::iterator it =
           stream_cache_.begin(); it != stream_cache_.end(); ++it) {
    it->second->ReleaseFileRef();
  }
}

scoped_refptr<FileStream> CrxFileHandler::open(
    int fd, const std::string& pathname, int oflag, mode_t mode) {
  // TODO(crbug.com/420771): Revisit the caching code once 420771 is fixed. If
  // we add a readonly file image to the CRX, we can just remove this caching
  // code. If we directly add OBB files to the CRX, we could also add a metadata
  // file to the CRX and remove the caching code.
  if (IsNonExistent(pathname)) {
    errno = ENOENT;
    return NULL;
  }

  // Check the |stream_cache_| first. Caching a FileStream object for the CRX
  // filesystem is safe since the filesystem is always readonly. However,
  // associating two independent FDs to a single FileStream is not safe. If we
  // do that, the unrelated two FDs will share the same file offset (held in
  // the native FD in FileIOWrapper) which is not what we want.
  const StreamCacheKey key = std::make_pair(pathname, oflag);
  std::map<StreamCacheKey, scoped_refptr<FileStream> >::const_iterator it =
      stream_cache_.find(key);
  if (it != stream_cache_.end()) {
    if (it->second->HasOneRef()) {
      // Reset the status of the native FD,
      int result = it->second->lseek(SEEK_SET, 0);
      ALOG_ASSERT(result == 0, "lseek: %s", pathname.c_str());
      result = Fcntl(it->second, F_SETFL, oflag);
      ALOG_ASSERT(result == 0, "fcntl: %s", pathname.c_str());
      // ..and then return the cached stream.
      ARC_STRACE_REPORT("CrxFileHandler::open: Reuse cached stream: %s",
                          pathname.c_str());
      return it->second;
    }
    ARC_STRACE_REPORT("CrxFileHandler::open: Cached stream in use: %s",
                        pathname.c_str());
  } else {
    ARC_STRACE_REPORT("CrxFileHandler::open: Cached stream not found: %s",
                        pathname.c_str());
  }

  // If it is not cached, or the cached stream is in use, fall back to the
  // default open() implementation in the parent class which issues an IPC.
  scoped_refptr<FileStream> new_stream =
      PepperFileHandler::open(fd, pathname, oflag, mode);
  if (!new_stream) {
    if (errno == ENOENT) {
      // Since CRX file system is always read-only, it is always very safe to
      // update the stat cache when open() returns ENOENT.
      // TODO(yusukes): Consider moving this to pepper_file.cc.
      SetNotExistent(pathname);
    }
    return NULL;
  }

  // Always overwrite the map with the new stream.
  if (it != stream_cache_.end())
    it->second->ReleaseFileRef();
  stream_cache_[key] = new_stream;
  // Add a file ref so that the stream never goes into the "closed" state
  // even if close() is called against the stream.
  new_stream->AddFileRef();
  return new_stream;
}

void CrxFileHandler::OpenPepperFileSystem(pp::Instance* instance) {
  pp::ExtCrxFileSystemPrivate crxfs_res(instance);
  pp::CompletionCallbackWithOutput<pp::FileSystem> callback =
      factory_.NewCallbackWithOutput(&CrxFileHandler::OnFileSystemOpen);
  TRACE_EVENT_ASYNC_BEGIN0(ARC_TRACE_CATEGORY,
                           "CrxFileHandler::OpenPepperFileSystem",
                           this);
  const int result = crxfs_res.Open(callback);
  ALOG_ASSERT(
      result == PP_OK_COMPLETIONPENDING,
      "Failed to create pp::ExtCrxFileSystemPrivate, error: %d", result);
}

void CrxFileHandler::OnFileSystemOpen(int32_t result,
                                      const pp::FileSystem& file_system) {
  TRACE_EVENT_ASYNC_END1(ARC_TRACE_CATEGORY,
                         "CrxFileHandler::OpenPepperFileSystem",
                         this, "result", result);
  if (result != PP_OK)
    LOG_FATAL("Failed to open pp::ExtCrxFileSystemPrivate, error: %d", result);
  SetPepperFileSystem(make_scoped_ptr(new pp::FileSystem(file_system)),
                      "/", "/");
}

}  // namespace posix_translation
