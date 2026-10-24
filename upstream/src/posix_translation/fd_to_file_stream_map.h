// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// A map from file descriptor to FileStream object.

#ifndef POSIX_TRANSLATION_FD_TO_FILE_STREAM_MAP_H_
#define POSIX_TRANSLATION_FD_TO_FILE_STREAM_MAP_H_

#include <sys/select.h>

#include <functional>
#include <map>
#include <vector>

#include "base/basictypes.h"
#include "base/memory/ref_counted.h"
#include "base/synchronization/lock.h"

namespace posix_translation {

class FileStream;

class FdToFileStreamMap {
 public:
  FdToFileStreamMap(int min_file_id, int max_file_id);
  ~FdToFileStreamMap();

  int GetFirstUnusedDescriptor();
  void AddFileStream(int fd, scoped_refptr<FileStream> stream);
  void ReplaceFileStream(int fd, scoped_refptr<FileStream> stream);
  void RemoveFileStream(int fd);
  bool IsKnownDescriptor(int fd);
  scoped_refptr<FileStream> GetStream(int fd);

  int min_file_id() const { return min_file_id_; }
  int max_file_id() const { return max_file_id_; }

 protected:
  friend class VirtualFileSystem;

 private:
  // File streams that have assigned file descriptors. For allocated file
  // descriptors without a stream (when stream is in a process of being created
  // or assigned) the value will be NULL.
  typedef std::map<int, scoped_refptr<FileStream> > FileStreamMap;
  FileStreamMap streams_;
  std::vector<int> unused_fds_;  // min-heap.
  std::greater<int> cmp_;  // to use |unused_fds_| as a min-heap.

  // The minimum/maximum fd number allowed.
  const int min_file_id_;
  const int max_file_id_;

  DISALLOW_COPY_AND_ASSIGN(FdToFileStreamMap);
};

}  // namespace posix_translation

#endif  // POSIX_TRANSLATION_FD_TO_FILE_STREAM_MAP_H_
