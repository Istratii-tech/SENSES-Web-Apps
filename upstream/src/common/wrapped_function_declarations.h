// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Forward declarations necessary to define the table of wrapped
// functions.
//

#ifndef COMMON_WRAPPED_FUNCTION_DECLARATIONS_H_
#define COMMON_WRAPPED_FUNCTION_DECLARATIONS_H_

#include <dirent.h>
#include <sys/types.h>

extern "C" {
// *_chk() are not in the source standard; they are only in the binary standard.
ssize_t __read_chk(int fd, void* buf, size_t nbytes, size_t buflen);
ssize_t __recvfrom_chk(int fd, void* buf, size_t len, size_t buflen, int flag,
                       struct sockaddr* from, socklen_t* fromlen);
mode_t __umask_chk(mode_t mask);

// Bionic does not have forward declarations for them.
int getdents(unsigned int fd, struct dirent* dirp, unsigned int count);
int mkstemps(char* path, int slen);
int tgkill(int tgid, int tid, int sig);
int tkill(int tid, int sig);
pid_t wait3(int* status, int options, struct rusage* rusage);

// TODO(crbug.com/350701): Remove them once we have removed glibc
// support and had some kind of check for symbols in Bionic.
int epoll_create1(int flags);
int epoll_pwait(int epfd, struct epoll_event* events,
                int maxevents, int timeout,
                const sigset_t* sigmask);
int inotify_init1(int flags);
int mkostemp(char* tmpl, int flags);
int mkostemps(char* tmpl, int suffixlen, int flags);
int ppoll(struct pollfd* fds, nfds_t nfds,
          const struct timespec* timeout_ts, const sigset_t* sigmask);
ssize_t preadv(int fd, const struct iovec* iov, int iovcnt,
               off_t offset);
ssize_t pwritev(int fd, const struct iovec* iov, int iovcnt,
                off_t offset);
}

#endif  // COMMON_WRAPPED_FUNCTION_DECLARATIONS_H_
