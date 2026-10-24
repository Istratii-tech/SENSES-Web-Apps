// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POSIX_TRANSLATION_EPOLL_STREAM_H_
#define POSIX_TRANSLATION_EPOLL_STREAM_H_

#include <string.h>
#include <sys/epoll.h>

#include <map>
#include <utility>

#include "base/basictypes.h"
#include "base/compiler_specific.h"
#include "base/synchronization/condition_variable.h"
#include "posix_translation/virtual_file_system.h"

namespace posix_translation {

class EPollStream : public FileStream {
 public:
  EPollStream(int fd, int oflag);

  virtual int epoll_ctl(int op, scoped_refptr<FileStream> file,
                        struct epoll_event* event) OVERRIDE;
  virtual int epoll_wait(struct epoll_event* events, int maxevents,
                         int timeout) OVERRIDE;

  virtual ssize_t read(void* buf, size_t count) OVERRIDE;
  virtual ssize_t write(const void* buf, size_t count) OVERRIDE;

  virtual const char* GetStreamType() const OVERRIDE;

 protected:
  virtual ~EPollStream();

  virtual void OnLastFileRef() OVERRIDE;

  virtual void HandleNotificationFrom(
      scoped_refptr<FileStream> file, bool is_closing) OVERRIDE;

 private:
  // arc-wasm (topic java-pool-park): events for a dormant Java thread.
  bool HasEventsLocked() const;
  void WakeDormantLocked();

  class EPollEntry {
   public:
    EPollEntry();
    EPollEntry(scoped_refptr<FileStream> stream, struct epoll_event event);
    virtual ~EPollEntry();

    scoped_refptr<FileStream> stream_;
    struct epoll_event event_;
    // EPOLLET: the events last seen ready (reported again only when they
    // were not, or after the file notified a change); EPOLLONESHOT: off
    // after an event until EPOLL_CTL_MOD.
    uint32_t seen_;
    bool disabled_;
  };

  // The key is FileStream*, obfuscated to avoid direct use.
  typedef std::map<void*, EPollEntry> EPollMap;

  int fd_;
  EPollMap epoll_map_;
  base::ConditionVariable cond_;

  DISALLOW_COPY_AND_ASSIGN(EPollStream);
};

}  // namespace posix_translation
#endif  // POSIX_TRANSLATION_EPOLL_STREAM_H_
