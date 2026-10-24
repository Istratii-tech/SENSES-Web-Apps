// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "posix_translation/fd_to_file_stream_map.h"

#include <algorithm>  // for heap
#include <utility>

#include "common/arc_strace.h"
#include "common/alog.h"
#include "posix_translation/file_stream.h"
#include "ppapi/cpp/module.h"

namespace posix_translation {

FdToFileStreamMap::FdToFileStreamMap(int min_file_id, int max_file_id)
    : min_file_id_(min_file_id), max_file_id_(max_file_id) {
  ALOG_ASSERT(max_file_id_ >= min_file_id_);
  unused_fds_.reserve(max_file_id - min_file_id_ + 1);
  for (int fd = min_file_id_; fd <= max_file_id_; ++fd) {
    unused_fds_.push_back(fd);
  }
  std::make_heap(unused_fds_.begin(), unused_fds_.end(), cmp_);
}

FdToFileStreamMap::~FdToFileStreamMap() {
  for (FileStreamMap::const_iterator it = streams_.begin();
       it != streams_.end();
       ++it) {
    if (it->second)
      it->second->ReleaseFileRef();
  }
}

void FdToFileStreamMap::AddFileStream(
    int fd, scoped_refptr<FileStream> stream) {
  if (stream)
    stream->AddFileRef();
  std::pair<FileStreamMap::iterator, bool> p =
      streams_.insert(std::make_pair(fd, stream));
  FileStreamMap::iterator it = p.first;
  if (p.second) {
    // Slow path. The |fd| is not the one claimed by GetFirstUnusedDescriptor().
    std::vector<int>::iterator remove_it =
        std::remove(unused_fds_.begin(), unused_fds_.end(), fd);
    unused_fds_.erase(remove_it, unused_fds_.end());
    std::make_heap(unused_fds_.begin(), unused_fds_.end(), cmp_);
  } else {
    ALOG_ASSERT(!it->second, "fd=%d", fd);
    it->second = stream;
  }
}

void FdToFileStreamMap::ReplaceFileStream(
    int fd, scoped_refptr<FileStream> stream) {
  ALOG_ASSERT(streams_.find(fd) != streams_.end() && streams_.find(fd)->second);
  scoped_refptr<FileStream> old_stream = streams_[fd];
  if (stream != old_stream) {
    streams_[fd] = stream;
    stream->AddFileRef();
    old_stream->ReleaseFileRef();
  }
}

void FdToFileStreamMap::RemoveFileStream(int fd) {
  FileStreamMap::iterator iter = streams_.find(fd);
  ALOG_ASSERT(iter != streams_.end());

  // OnLastFileRef() of the stream could call Wait(), which unlocks the mutex.
  // During the unlocked period, if other thread tries to access the stream
  // via this fd map, it'll cause a problem of accessing already closed stream,
  // which is asserted in FileStream. So, we remove the stream from the map
  // first.
  scoped_refptr<FileStream> old_stream(iter->second);
  streams_.erase(iter);
  unused_fds_.push_back(fd);
  std::push_heap(unused_fds_.begin(), unused_fds_.end(), cmp_);
  if (old_stream)
    old_stream->ReleaseFileRef();
}

int FdToFileStreamMap::GetFirstUnusedDescriptor() {
  int fd = unused_fds_.empty() ? -1 : unused_fds_.front();
  if (fd != -1) {
    std::pop_heap(unused_fds_.begin(), unused_fds_.end(), cmp_);
    unused_fds_.pop_back();
    AddFileStream(fd, NULL);  // mark as used.
  } else {
    ALOGW("All %d file descriptors in use, cannot allocate a new one.",
          max_file_id_ - min_file_id_ + 1);
  }
  return fd;
}

bool FdToFileStreamMap::IsKnownDescriptor(int fd) {
  return streams_.find(fd) != streams_.end();
}

scoped_refptr<FileStream> FdToFileStreamMap::GetStream(int fd) {
  FileStreamMap::const_iterator it = streams_.find(fd);
  scoped_refptr<FileStream> stream = it != streams_.end() ? it->second : NULL;

  if (stream) {
    stream->CheckNotClosed();
    ALOG_ASSERT(stream->IsAllowedOnMainThread() ||
                !pp::Module::Get()->core()->IsMainThread());

    // Call REPORT_HANDLER() so that the current function call is categrized as
    // |stream->GetStreamType()| rather than |kVirtualFileSystemHandlerStr|.
    ARC_STRACE_REPORT_HANDLER(stream->GetStreamType());
  }

  return stream;
}

}  // namespace posix_translation
