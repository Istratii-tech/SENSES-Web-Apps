// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// FileStream for sockets.

#ifndef POSIX_TRANSLATION_SOCKET_STREAM_H_
#define POSIX_TRANSLATION_SOCKET_STREAM_H_

#include <errno.h>

#include "base/compiler_specific.h"
#include "base/time/time.h"
#include "posix_translation/file_stream.h"

namespace posix_translation {

class SocketStream : public FileStream {
 public:
  static const int kUnknownSocketFamily;

  // socket_family is the protocol family of this socket, such as AF_INET
  // or AF_INET6. oflag is the flag passed via open(), and is just redirected
  // to FileStream. See FileStream for more details.
  SocketStream(int socket_family, int oflag);

  virtual int fdatasync() OVERRIDE;
  virtual int fstat(struct stat* out) OVERRIDE;
  virtual int fsync() OVERRIDE;
  virtual int getsockopt(int level, int optname, void* optval,
                         socklen_t* optlen) OVERRIDE;
  virtual int ioctl(int request, va_list ap) OVERRIDE;
  virtual int setsockopt(int level, int optname, const void* optval,
                         socklen_t optlen) OVERRIDE;

 protected:
  virtual ~SocketStream();
  virtual bool GetOptNameData(int level, int optname, socklen_t* len,
                              void** storage, const void* user_data,
                              socklen_t user_data_len);

  int socket_family_;
  int broadcast_;
  int error_;
  struct linger linger_;
  int reuse_addr_;
  base::TimeDelta recv_timeout_;
  base::TimeDelta send_timeout_;
  int recv_buffer_size_;
  int send_buffer_size_;
};

}  // namespace posix_translation

#endif  // POSIX_TRANSLATION_SOCKET_STREAM_H_
