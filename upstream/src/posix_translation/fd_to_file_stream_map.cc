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

// The calling thread's light process (shim/multiproc/light_process.cc).
extern "C" int arc_lproc_current(void);

namespace posix_translation {

FdToFileStreamMap::ProcessView* FdToFileStreamMap::CurrentView() {
  if (views_.empty())
    return NULL;
  std::map<int, ProcessView>::iterator it = views_.find(arc_lproc_current());
  return it != views_.end() ? &it->second : NULL;
}

void FdToFileStreamMap::SetShadow(ProcessView* v, int fd,
                                  scoped_refptr<FileStream> stream) {
  if (stream)
    stream->AddFileRef();
  std::map<int, scoped_refptr<FileStream> >::iterator it = v->shadow.find(fd);
  scoped_refptr<FileStream> old_stream;
  if (it != v->shadow.end()) {
    old_stream = it->second;
    it->second = stream;
  } else {
    v->shadow[fd] = stream;
  }
  if (stream)
    v->reserved.erase(fd);
  if (old_stream)
    old_stream->ReleaseFileRef();
}

void FdToFileStreamMap::SetProcessView(int pid,
                                       scoped_refptr<FileStream> stdio[3]) {
  ProcessView& v = views_[pid];
  for (int i = 0; i < 3; ++i)
    SetShadow(&v, i, stdio[i]);
}

std::vector<int> FdToFileStreamMap::RemoveProcessView(int pid) {
  std::vector<int> left;
  std::map<int, ProcessView>::iterator it = views_.find(pid);
  if (it == views_.end())
    return left;
  ProcessView v = it->second;
  views_.erase(it);
  left.assign(v.own.begin(), v.own.end());
  for (std::map<int, scoped_refptr<FileStream> >::iterator s = v.shadow.begin();
       s != v.shadow.end(); ++s) {
    if (s->second)
      s->second->ReleaseFileRef();
  }
  return left;
}

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
  if (ProcessView* v = CurrentView()) {
    // Its own 0-2, a descriptor it already sees otherwise, or one it
    // inherited (dup2 onto it): its layer. A new descriptor: the table's.
    FileStreamMap::const_iterator g = streams_.find(fd);
    if (fd < 3 || v->shadow.count(fd) ||
        (g != streams_.end() && g->second && !v->own.count(fd))) {
      SetShadow(v, fd, stream);
      if (!stream)
        v->reserved.insert(fd);
      return;
    }
    v->own.insert(fd);
  }
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
  if (ProcessView* v = CurrentView()) {
    if (v->shadow.count(fd)) {
      SetShadow(v, fd, stream);
      return;
    }
  }
  ALOG_ASSERT(streams_.find(fd) != streams_.end() && streams_.find(fd)->second);
  scoped_refptr<FileStream> old_stream = streams_[fd];
  if (stream != old_stream) {
    streams_[fd] = stream;
    stream->AddFileRef();
    old_stream->ReleaseFileRef();
  }
}

void FdToFileStreamMap::RemoveFileStream(int fd) {
  if (ProcessView* v = CurrentView()) {
    if (!v->own.erase(fd)) {
      // Its layer's, or an inherited one: closed for it only.
      SetShadow(v, fd, NULL);
      v->reserved.erase(fd);
      return;
    }
  }
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
  if (ProcessView* v = CurrentView()) {
    // The lowest number free for it: one closed in its layer, or the table's.
    const int table_fd = unused_fds_.empty() ? -1 : unused_fds_.front();
    for (std::map<int, scoped_refptr<FileStream> >::iterator it = v->shadow.begin();
         it != v->shadow.end(); ++it) {
      if (table_fd >= 0 && it->first > table_fd)
        break;
      if (!it->second && !v->reserved.count(it->first)) {
        v->reserved.insert(it->first);
        return it->first;
      }
    }
  }
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
  if (ProcessView* v = CurrentView()) {
    std::map<int, scoped_refptr<FileStream> >::iterator it = v->shadow.find(fd);
    if (it != v->shadow.end())
      return it->second || v->reserved.count(fd);
  }
  return streams_.find(fd) != streams_.end();
}

scoped_refptr<FileStream> FdToFileStreamMap::GetStream(int fd) {
  scoped_refptr<FileStream> stream;
  ProcessView* v = CurrentView();
  std::map<int, scoped_refptr<FileStream> >::iterator sh;
  if (v && (sh = v->shadow.find(fd)) != v->shadow.end()) {
    stream = sh->second;
  } else {
    FileStreamMap::const_iterator it = streams_.find(fd);
    stream = it != streams_.end() ? it->second : NULL;
  }

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
