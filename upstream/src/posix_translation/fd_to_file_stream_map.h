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
#include <set>
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

  // arc-wasm (topic native-exec): a spawned process (a light process,
  // shim/multiproc/include/arc_lproc.h) sees the one table through a layer
  // of its own: its descriptors 0-2 (|stdio|, held as descriptors hold
  // streams; NULL: closed), what it dup2()s onto a descriptor it inherited,
  // and inherited ones it closed (closed for it only: a child closing what
  // its parent has open). What it opens itself is in the one table, and is
  // closed when it ends.
  void SetProcessView(int pid, scoped_refptr<FileStream> stdio[3]);
  // Ends process |pid|'s view; returns the descriptors it opened and left
  // open, for the caller to close.
  std::vector<int> RemoveProcessView(int pid);

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

  struct ProcessView {
    // Descriptors it sees otherwise than the table (NULL: closed for it).
    std::map<int, scoped_refptr<FileStream> > shadow;
    std::set<int> reserved;  // closed ones claimed by GetFirstUnusedDescriptor
    std::set<int> own;       // descriptors of the table it opened
  };
  // The calling thread's process's view, or NULL.
  ProcessView* CurrentView();
  void SetShadow(ProcessView* v, int fd, scoped_refptr<FileStream> stream);
  std::map<int, ProcessView> views_;

  DISALLOW_COPY_AND_ASSIGN(FdToFileStreamMap);
};

}  // namespace posix_translation

#endif  // POSIX_TRANSLATION_FD_TO_FILE_STREAM_MAP_H_
