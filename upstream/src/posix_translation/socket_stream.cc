// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "posix_translation/socket_stream.h"

#include <fcntl.h>
#include <netinet/in.h>
#include <sys/ioctl.h>

#include "common/alog.h"
#include "common/process_emulator.h"
#include "posix_translation/permission_info.h"
#include "posix_translation/socket_util.h"
#include "posix_translation/time_util.h"

namespace posix_translation {
namespace {

const int kMinSocketBuffSize = 128;
const int kMaxSocketBuffSize = 4 * 1024 * 1024;

// Converts |value| to timeval and copies the value into |optval|.
// Returns 0 on success, or -1 on error with setting system error number to
// errno.
int GetTimeoutSocketOption(const base::TimeDelta& value,
                           void* optval, socklen_t* optlen) {
  int error = internal::VerifyGetSocketOption(optval, optlen);
  if (error) {
    errno = error;
    return -1;
  }

  timeval value_timeval = {};
  // If Timeout is set to negative value, getsockopt() returns {0, 0}.
  if (value > base::TimeDelta())
    value_timeval = internal::TimeDeltaToTimeVal(value);

  internal::CopySocketOption(
      &value_timeval, SIZEOF_AS_SOCKLEN(value_timeval), optval, optlen);
  return 0;
}

// Interprets |optval| as a timeval structure, converts the value to TimeDelta
// and stores it to |storage|. Returns 0 on success, or -1 on error with
// setting system error number to errno.
int SetTimeoutSocketOption(
    const void* optval, socklen_t optlen, base::TimeDelta* storage) {
  ALOG_ASSERT(storage);

  int error = internal::VerifySetSocketOption(optval, optlen, sizeof(timeval));
  if (error) {
    errno = error;
    return -1;
  }

  const timeval& timeout = *static_cast<const timeval*>(optval);
  error = internal::VerifyTimeoutSocketOption(timeout);
  if (error) {
    errno = error;
    return -1;
  }

  *storage = internal::TimeValToTimeDelta(timeout);
  return 0;
}

}  // namespace

const int SocketStream::kUnknownSocketFamily = -1;

SocketStream::SocketStream(int socket_family, int oflag)
    : FileStream(oflag, ""), socket_family_(socket_family),
      broadcast_(0), error_(0), reuse_addr_(0),
      recv_buffer_size_(0), send_buffer_size_(0) {
  memset(&linger_, 0, sizeof(linger_));
  memset(&recv_timeout_, 0, sizeof(recv_timeout_));
  memset(&send_timeout_, 0, sizeof(send_timeout_));
  set_permission(PermissionInfo(arc::ProcessEmulator::GetUid(),
                                true  /* is_writable */));
  EnableListenerSupport();
}

SocketStream::~SocketStream() {
}

int SocketStream::fdatasync() {
  errno = EINVAL;
  return -1;
}

int SocketStream::fsync() {
  errno = EINVAL;
  return -1;
}

bool SocketStream::GetOptNameData(int level, int optname, socklen_t* len,
                                  void** storage, const void* user_data,
                                  socklen_t user_data_len) {
  // We cannot use SIZEOF_AS_SOCKLEN(int) for this as the linter is
  // confused by this and emits two warnings (readability/casting and
  // readability/function).
  static const socklen_t sizeof_int = sizeof(int);  // NOLINT
  if (level == SOL_SOCKET) {
    switch (optname) {
      case SO_ERROR:
        // TODO(igorc): Consider disabling SO_ERROR for setsockopt().
        *storage = &error_;
        *len = sizeof_int;
        ALOG_ASSERT(*len == sizeof(error_));
        return true;
      case SO_REUSEADDR:
        // TODO(crbug.com/233914): Pass this setting to Pepper. Now we claim
        // this option is supported since failing would cause JDWP to fail
        // during setup of the listening socket.
        if (user_data != NULL && user_data_len >= sizeof_int &&
            *(static_cast<const int*>(user_data)) != 0)
          ALOGW("setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, ..) not supported");
        *storage = &reuse_addr_;
        *len = sizeof_int;
        ALOG_ASSERT(*len == sizeof(reuse_addr_));
        return true;
      case SO_RCVBUF:
      case SO_SNDBUF:
        // TODO(crbug.com/242619): Support read/write buffer size options.
        // Note that OS defaults could be rather large (200K-2M),
        // so it is probably even more important to change the defaults.
        // The return value should normally be doubled, but we will be
        // returning the value that was originally set.
        if (user_data != NULL && user_data_len >= sizeof_int) {
          int value = *(static_cast<const int*>(user_data));
          if (value < kMinSocketBuffSize || value > kMaxSocketBuffSize) {
            return false;
          }
          ALOGW("Setting socket buffer size is not supported, opt=%d value=%d",
                optname, value);
        }
        *storage = (optname == SO_RCVBUF ?
            &recv_buffer_size_ : &send_buffer_size_);
        *len = sizeof_int;
        ALOG_ASSERT(*len == sizeof(recv_buffer_size_));
        ALOG_ASSERT(*len == sizeof(send_buffer_size_));
        return true;
      case SO_BROADCAST:
        *storage = &broadcast_;
        *len = sizeof_int;
        ALOG_ASSERT(*len == sizeof(broadcast_));
        return true;
      case SO_LINGER:
        socklen_t expected_len = sizeof(struct linger);
        *storage = &linger_;
        *len = expected_len;
        ALOG_ASSERT(*len == sizeof(linger_));
        return true;
    }
  } else if (level == IPPROTO_IPV6) {
    if (optname == IPV6_MULTICAST_HOPS) {
      // IoBridge.java is setting this to work around a Linux kernel oddity.
      // Merely ignore this value as Pepper does not support it.
      *storage = NULL;
      *len = sizeof_int;
      return true;
    }
  } else if (level == IPPROTO_IP) {
    // arc-wasm: libcore 8.1's PlainDatagramSocketImpl turns IP_MULTICAST_ALL
    // off on every UDP socket; there is no multicast here, so it is ignored
    // like IPV6_MULTICAST_HOPS above.
#ifndef IP_MULTICAST_ALL
#define IP_MULTICAST_ALL 49
#endif
    if (optname == IP_MULTICAST_ALL) {
      *storage = NULL;
      *len = sizeof_int;
      return true;
    }
  }
  return false;
}

int SocketStream::fstat(struct stat* out) {
  memset(out, 0, sizeof(struct stat));
  // Empirical values based on fstat of a connected socket on Linux-3.2.5.
  out->st_mode = S_IFSOCK | 0777;
  out->st_nlink = 1;
  out->st_blksize = 4096;
  return 0;
}

int SocketStream::getsockopt(int level, int optname, void* optval,
                             socklen_t* optlen) {
  if (level == SOL_SOCKET) {
    // TODO(hidehiko): Merge other options to this switch.
    switch (optname) {
      case SO_RCVTIMEO:
        return GetTimeoutSocketOption(recv_timeout_, optval, optlen);
      case SO_SNDTIMEO:
        return GetTimeoutSocketOption(send_timeout_, optval, optlen);
    }
  }

  socklen_t len = 0;
  void* storage = NULL;
  if (optlen == NULL) {
    errno = EFAULT;
    return -1;
  }
  if (!GetOptNameData(level, optname, &len, &storage, NULL, 0)) {
    errno = EINVAL;
    return -1;
  }
  if (*optlen < len && optval != NULL) {
    errno = EINVAL;
    return -1;
  }
  *optlen = len;
  if (optval != NULL) {
    if (storage != NULL)
      memcpy(optval, storage, len);
    else
      memset(optval, 0, len);
  }
  return 0;
}

int SocketStream::ioctl(int request, va_list ap) {
  if (request == FIONBIO) {
    int* val = va_arg(ap, int*);
    if (*val)
      set_oflag(oflag() | O_NONBLOCK);
    else
      set_oflag(oflag() & ~O_NONBLOCK);
    return 0;
  }
  return FileStream::ioctl(request, ap);
}

int SocketStream::setsockopt(int level, int optname, const void* optval,
                             socklen_t optlen) {
  if (level == SOL_SOCKET) {
    // TODO(hidehiko): Merge other options to this switch.
    switch (optname) {
      case SO_RCVTIMEO:
        return SetTimeoutSocketOption(optval, optlen, &recv_timeout_);
      case SO_SNDTIMEO:
        return SetTimeoutSocketOption(optval, optlen, &send_timeout_);
    }
  }

  socklen_t len = 0;
  void* storage = NULL;
  if (!GetOptNameData(level, optname, &len, &storage, optval, optlen)) {
    errno = EINVAL;
    return -1;
  }
  if (optlen < len) {
    errno = EINVAL;
    return -1;
  }
  if (optval != NULL && storage != NULL)
    memcpy(storage, optval, len);
  return 0;
}

}  // namespace posix_translation
