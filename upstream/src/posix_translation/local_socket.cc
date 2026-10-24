// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "posix_translation/local_socket.h"

#include <stddef.h>
#include <string.h>
#include <sys/un.h>

#include <algorithm>
#include <string>

#include "common/alog.h"
#include "common/process_emulator.h"
#include "posix_translation/socket_util.h"
#include "posix_translation/time_util.h"
#include "posix_translation/virtual_file_system.h"

namespace posix_translation {

// 224K is the default SO_SNDBUF/SO_RCVBUF in the linux kernel.
static const int kBufferSize = 224*1024;

LocalSocket::LocalSocket(int oflag, int socket_type,
                         StreamDir stream_dir)
    : SocketStream(AF_UNIX, oflag), socket_type_(socket_type),
      connect_state_(SOCKET_NEW), stream_dir_(stream_dir),
      connection_backlog_(0), written_pos_(0), read_pos_(0), pass_cred_(0) {
  if (socket_type == SOCK_STREAM && stream_dir != WRITE_ONLY)
    buffer_.set_capacity(kBufferSize);
  my_cred_.pid = arc::ProcessEmulator::GetPid();
  my_cred_.uid = arc::ProcessEmulator::GetUid();
  my_cred_.gid = my_cred_.uid;
  // These values are empirically what SO_PEERCRED returns when
  // there has never been a peer to the socket.
  peer_cred_.pid = 0;
  peer_cred_.uid = -1;
  peer_cred_.gid = -1;
}

LocalSocket::~LocalSocket() {
}

bool LocalSocket::IsAllowedOnMainThread() const {
  return true;
}

void LocalSocket::OnLastFileRef() {
  if (peer_) {
    peer_->peer_ = NULL;
    peer_ = NULL;
    // Note that the peer_ == NULL and connect_state_ == SOCKET_CONNECTED
    // means the connection has been closed.
    VirtualFileSystem::GetVirtualFileSystem()->Broadcast();
  }

  if (!abstract_name_.empty()) {
    VirtualFileSystem::GetVirtualFileSystem()->
        GetAbstractSocketNamespace()->Bind(abstract_name_, NULL);
  }

  if (!logd_name_.empty()) {
    VirtualFileSystem::GetVirtualFileSystem()->
        GetLogdSocketNamespace()->Bind(logd_name_, NULL);
  }
}

void LocalSocket::set_peer(scoped_refptr<LocalSocket> peer) {
  // Always called by VirtualFileSystem.
  ALOG_ASSERT(peer != NULL);
  peer_ = peer;
  connect_state_ = SOCKET_CONNECTED;
  peer_cred_ = peer->my_cred_;
}

off64_t LocalSocket::lseek(off64_t offset, int whence) {
  errno = ESPIPE;
  return -1;
}

ssize_t LocalSocket::read(void* buf, size_t count) {
  return this->recv(buf, count, 0);
}

ssize_t LocalSocket::recv(void* buf, size_t len, int flags) {
  return this->recvfrom(buf, len, flags, NULL, NULL);
}

ssize_t LocalSocket::recvfrom(void* buf, size_t len, int flags, sockaddr* addr,
                              socklen_t* addrlen) {
  if (addr || addrlen) {
    errno = EINVAL;
    return -1;
  }
  if (len == 0)
    return 0;

  struct msghdr msg = {};
  struct iovec iov;

  iov.iov_base = buf;
  iov.iov_len = len;
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  return this->recvmsg(&msg, 0);
}

bool LocalSocket::ValidateSockaddr(const sockaddr* addr, socklen_t addrlen) {
  if (addr->sa_family != AF_UNIX) {
    // Observed the error EINVAL when AF_UNIX is given to socket and
    // AF_UNIX is given to connect or bind.
    errno = EINVAL;
    return false;
  }
  const socklen_t kSunPathOffset =
      static_cast<socklen_t>(offsetof(struct sockaddr_un, sun_path));
  if (addrlen < kSunPathOffset + 1) {
    // Technically a sun_path of length 0 is valid, but we cannot use
    // it.  And anything less than 0 is of course invalid and results
    // in EINVAL.  We combine those together.
    errno = EINVAL;
    return false;
  }
  return true;
}

bool LocalSocket::ConvertSockaddrToName(const sockaddr* addr,
                                        socklen_t addrlen,
                                        std::string* out_name) {
  const sockaddr_un* saddr_un = reinterpret_cast<const sockaddr_un*>(addr);
  const socklen_t kSunPathOffset =
      static_cast<socklen_t>(offsetof(struct sockaddr_un, sun_path));
  if (saddr_un->sun_path[0] == '\0')
    return false;

  socklen_t sun_path_length = addrlen - kSunPathOffset;
  const char* str = saddr_un->sun_path;
  out_name->assign(str, strnlen(str, sun_path_length));
  return true;
}

bool LocalSocket::ConvertSockaddrToAbstractName(const sockaddr* addr,
                                                socklen_t addrlen,
                                                std::string* out_name) {
  const sockaddr_un* saddr_un = reinterpret_cast<const sockaddr_un*>(addr);
  const socklen_t kSunPathOffset =
      static_cast<socklen_t>(offsetof(struct sockaddr_un, sun_path));
  if (saddr_un->sun_path[0] != '\0')
    return false;

  socklen_t sun_path_length = addrlen - kSunPathOffset;
  out_name->assign(saddr_un->sun_path + 1, sun_path_length - 1);
  return true;
}

int LocalSocket::bind(const sockaddr* addr, socklen_t addrlen) {
  // You can call bind on a new or connected socket, Linux does not care.
  // You cannot call bind on a pipe, which is also implemented by LocalSocket,
  // because it is not a socket.  We do not catch that case here, but we
  // also do not catch this case in recv/send/recvfrom/sendto/recvmsg/sendmsg,
  // all of which require a socket.
  // TODO(crbug.com/447833): Split out pipes.
  if (!ValidateSockaddr(addr, addrlen))
    return -1;
  if (!abstract_name_.empty() || !logd_name_.empty()) {
    // Trying to bind a second name to a single socket fails.
    errno = EINVAL;
    return -1;
  }
  std::string name;
  if (ConvertSockaddrToName(addr, addrlen, &name)) {
    int result = VirtualFileSystem::GetVirtualFileSystem()->
        GetLogdSocketNamespace()->Bind(name, this);
    if (result == 0) {
      logd_name_ = name;
      if (name.compare(0, 12, "/dev/socket/") != 0)  // (logd's are not files)
        VirtualFileSystem::GetVirtualFileSystem()->CreateSocketFileLocked(name);
    }
    return result;
  }
  if (ConvertSockaddrToAbstractName(addr, addrlen, &name)) {
    int result = VirtualFileSystem::GetVirtualFileSystem()->
        GetAbstractSocketNamespace()->Bind(name, this);
    if (result == 0)
      abstract_name_ = name;
    return result;
  }
  errno = ENOSYS;
  return -1;
}

int LocalSocket::listen(int backlog) {
  if (socket_type_ == SOCK_DGRAM) {
    errno = EOPNOTSUPP;
    return -1;
  }
  if (abstract_name_.empty() && logd_name_.empty()) {
    // Observed the error EINVAL when listen is called on an unbound
    // socket.
    errno = EINVAL;
    return -1;
  }
  connect_state_ = SOCKET_LISTENING;
  connection_backlog_ = backlog;
  return 0;
}

// arc-wasm: an AF_UNIX socket's address, as Linux gives it: the abstract
// name when bound, else an unnamed one (the family alone). CPython's socket
// object made from a descriptor (socket.socketpair, which asyncio's event
// loop uses) asks getsockname first and takes ENOTSOCK as fatal.
static int UnixSocketName(const std::string& abstract_name, const std::string& path,
                          sockaddr* name, socklen_t* namelen) {
  if (!name || !namelen) {
    errno = EFAULT;
    return -1;
  }
  sockaddr_un un;
  memset(&un, 0, sizeof(un));
  un.sun_family = AF_UNIX;
  socklen_t len = offsetof(sockaddr_un, sun_path);
  if (!abstract_name.empty()) {
    size_t n = std::min(abstract_name.size(), sizeof(un.sun_path) - 1);
    memcpy(un.sun_path + 1, abstract_name.data(), n);
    len += 1 + n;
  } else if (!path.empty()) {
    size_t n = std::min(path.size(), sizeof(un.sun_path) - 1);
    memcpy(un.sun_path, path.data(), n);
    len += n + 1;
  }
  memcpy(name, &un, std::min(*namelen, len));
  *namelen = len;
  return 0;
}

int LocalSocket::getsockname(sockaddr* name, socklen_t* namelen) {
  return UnixSocketName(abstract_name_, logd_name_, name, namelen);
}

int LocalSocket::getpeername(sockaddr* name, socklen_t* namelen) {
  if (connect_state_ != SOCKET_CONNECTED || !peer_) {
    errno = ENOTCONN;
    return -1;
  }
  return UnixSocketName(peer_->abstract_name_, peer_->logd_name_, name, namelen);
}

int LocalSocket::getsockopt(int level, int optname, void* optval,
                            socklen_t* optlen) {
  // arc-wasm: what the socket is (CPython reads SO_TYPE of a descriptor).
  if (level == SOL_SOCKET &&
      (optname == SO_TYPE || optname == SO_DOMAIN || optname == SO_PROTOCOL)) {
    int error = internal::VerifyGetSocketOption(optval, optlen);
    if (error) {
      errno = error;
      return -1;
    }
    int v = optname == SO_TYPE ? socket_type_ : optname == SO_DOMAIN ? AF_UNIX : 0;
    *optlen = std::min(*optlen, socklen_t(sizeof(v)));
    memcpy(optval, &v, *optlen);
    return 0;
  }
  if (level == SOL_SOCKET && optname == SO_PEERCRED) {
    int error = internal::VerifyGetSocketOption(optval, optlen);
    if (error) {
      errno = error;
      return -1;
    }
    *optlen = std::min(*optlen, socklen_t(sizeof(peer_cred_)));
    memcpy(optval, &peer_cred_, *optlen);
    return 0;
  }
  if (level == SOL_SOCKET && optname == SO_PASSCRED) {
    int error = internal::VerifyGetSocketOption(optval, optlen);
    if (error) {
      errno = error;
      return -1;
    }
    *optlen = std::min(*optlen, socklen_t(sizeof(pass_cred_)));
    memcpy(optval, &pass_cred_, *optlen);
    return 0;
  }
  return SocketStream::getsockopt(level, optname, optval, optlen);
}

int LocalSocket::setsockopt(int level, int optname, const void* optval,
                            socklen_t optlen) {
  if (level == SOL_SOCKET && optname == SO_PASSCRED) {
    int error =
        internal::VerifySetSocketOption(optval, optlen, sizeof(pass_cred_));
    if (error) {
      errno = error;
      return -1;
    }
    memcpy(&pass_cred_, optval, sizeof(pass_cred_));
    return 0;
  }
  return SocketStream::setsockopt(level, optname, optval, optlen);
}

bool LocalSocket::HandleConnectLocked(LocalSocket* bound_socket) {
  if (socket_type_ == SOCK_STREAM || socket_type_ == SOCK_SEQPACKET) {
    if (connect_state_ != SOCKET_LISTENING) {
      ALOGW("LocalSocket::connect failed - receiving socket not listening");
      return false;
    }
    if (connection_backlog_ == connection_queue_.size()) {
      ALOGW("LocalSocket::connect failed - queue for %s full",
            abstract_name_.empty() ?
            logd_name_.c_str() : abstract_name_.c_str());
      return false;
    }
    connection_queue_.push_back(bound_socket);
    if (connection_queue_.size() == 1) {
      // In case we are already blocking on an accept, wake it up now...
      VirtualFileSystem::GetVirtualFileSystem()->Broadcast();
      // and also notify any polls/selects listening to it.
      NotifyListeners();
    }
  }

  return true;
}

void LocalSocket::WaitForLocalSocketConnect() {
  if (socket_type_ == SOCK_STREAM || socket_type_ == SOCK_SEQPACKET) {
    // The accept() call will set the peer and tell us when to proceed.
    VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
    connect_state_ = SOCKET_CONNECTING;
    while (connect_state_ == SOCKET_CONNECTING) {
      sys->Wait();
    }
  }
}

int LocalSocket::connect(const sockaddr* addr, socklen_t addrlen) {
  if (connect_state_ == SOCKET_CONNECTED ||
      connect_state_ == SOCKET_LISTENING) {
    errno = EISCONN;
    return -1;
  }
  if (socket_type_ != SOCK_DGRAM && !is_block()) {
    ALOGE("Non-blocking local socket stream connect not supported.\n");
    errno = ENOSYS;
    return -1;
  }

  std::string abstract_name;
  std::string logd_name;
  scoped_refptr<LocalSocket> bound_socket;
  if (!ValidateSockaddr(addr, addrlen))
    return -1;
  if (ConvertSockaddrToName(addr, addrlen, &logd_name)) {
    bound_socket = VirtualFileSystem::GetVirtualFileSystem()->
        GetLogdSocketNamespace()->GetByName(logd_name);
  } else if (ConvertSockaddrToAbstractName(addr, addrlen, &abstract_name)) {
    bound_socket = VirtualFileSystem::GetVirtualFileSystem()->
        GetAbstractSocketNamespace()->GetByName(abstract_name);
  } else {
    errno = ENOSYS;
    return -1;
  }

  if (bound_socket == NULL) {
    // Connection to unbound abstract name returns ECONNREFUSED; to a path,
    // ENOENT when no file is there (as Linux: a socket file nobody listens
    // on, or another file, is ECONNREFUSED).
    if (!logd_name.empty() && logd_name.compare(0, 12, "/dev/socket/") != 0 &&
        !VirtualFileSystem::GetVirtualFileSystem()->PathExistsLocked(logd_name)) {
      errno = ENOENT;
      return -1;
    }
    errno = ECONNREFUSED;
    return -1;
  }
  if (bound_socket->socket_type_ != socket_type_) {
    errno = ECONNREFUSED;
    return -1;
  }

  if (!bound_socket->HandleConnectLocked(this)) {
    errno = ECONNREFUSED;
    return -1;
  }
  logd_target_name_ = logd_name;

  WaitForLocalSocketConnect();
  return 0;
}

void LocalSocket::WaitForOpenedConnectToAccept() {
  VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
  const base::TimeTicks time_limit =
      internal::TimeOutToTimeLimit(recv_timeout_);
  do {
    // Skip any queued connects which have since been closed.
    while (!connection_queue_.empty() &&
           connection_queue_.front()->IsClosed()) {
      ALOGW("LocalSocket::accept - enqueued connection was preemptively "
            "closed");
      connection_queue_.pop_front();
    }
    if (!connection_queue_.empty()) break;
  } while (!sys->WaitUntil(time_limit));
}

int LocalSocket::accept(sockaddr* addr, socklen_t* addrlen) {
  if (socket_type_ == SOCK_DGRAM) {
    errno = EOPNOTSUPP;
    return -1;
  }
  if (addr) {
    int error = internal::VerifyOutputSocketAddress(addr, addrlen);
    if (error) {
      errno = error;
      return -1;
    }
  }

  if (connect_state_ != SOCKET_LISTENING) {
    errno = EINVAL;
    return -1;
  }

  if (!is_block()) {
    ALOGE("Non-blocking local socket accept not supported.\n");
    errno = ENOSYS;
    return -1;
  }
  WaitForOpenedConnectToAccept();
  if (IsClosed()) {
    ALOGW("LocalSocket::accept - Listening socket closed while in waiting");
    errno = EBADF;
    return -1;
  }
  if (connection_queue_.empty()) {
    errno = EAGAIN;
    return -1;
  }
  // Create a peer server socket for the client socket at the head of the
  // connection queue.
  scoped_refptr<LocalSocket> server_socket =
      new LocalSocket(oflag(), socket_type_, READ_WRITE);
  VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
  int accept_fd = sys->AddFileStreamLocked(server_socket);
  if (accept_fd < 0) {
    ALOGW("LocalSocket::accept - out of fds creating accepted fd");
    errno = EMFILE;
    return -1;
  }
  scoped_refptr<LocalSocket> client_socket = connection_queue_.front();
  connection_queue_.pop_front();
  server_socket->set_peer(client_socket);
  client_socket->set_peer(server_socket);
  sys->Broadcast();
  NotifyListeners();
  if (addr) {
    sockaddr_un output = {};
    output.sun_family = AF_UNIX;
    memcpy(addr, &output, std::min(*addrlen, socklen_t(sizeof(sa_family_t))));
    *addrlen = sizeof(sa_family_t);
  }
  return accept_fd;
}

int LocalSocket::recvmsg(struct msghdr* msg, int flags) {
  if (stream_dir_ == WRITE_ONLY) {
    // Reading from write socket of a pipe is not allowed.
    errno = EBADF;
    return -1;
  }

  if (socket_type_ != SOCK_DGRAM && connect_state_ != SOCKET_CONNECTED) {
    errno = EINVAL;
    return -1;
  }

  VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
  if (is_block() && !(flags & MSG_DONTWAIT)) {
    while (peer_ && !IsSelectReadReady()) {
      sys->Wait();
    }
  }

  msg->msg_flags = 0;
  ssize_t bytes_read = 0;
  ucred cred;
  // Where this read starts: descriptors queued at or before it come with it.
  const uint64_t read_start = read_pos_;
  if (socket_type_ == SOCK_STREAM) {
    cred = peer_cred_;
    if (buffer_.size() > 0) {
      // Not past the start of the next data that carries descriptors.
      size_t left = buffer_.size();
      for (size_t i = 0; i < cmsg_fd_queue_.size(); ++i) {
        if (cmsg_fd_queue_[i].pos > read_start) {
          left = std::min<uint64_t>(left, cmsg_fd_queue_[i].pos - read_start);
          break;
        }
      }
      for (size_t i = 0; i < msg->msg_iovlen && left > 0; ++i) {
        const size_t n = buffer_.read(static_cast<char*>(msg->msg_iov[i].iov_base),
                                      std::min(msg->msg_iov[i].iov_len, left));
        bytes_read += n;
        left -= n;
      }
      read_pos_ += bytes_read;
    }
  } else {
    if (!queue_.empty()) {
      const Datagram& datagram = queue_.front();
      cred = datagram.cred_;
      std::vector<char>::const_iterator iter = datagram.content_.begin();
      size_t remaining = datagram.content_.size();
      for (size_t i = 0; i < msg->msg_iovlen && remaining > 0; ++i) {
        size_t len = std::min(msg->msg_iov[i].iov_len, remaining);
        std::copy(iter, iter + len,
                  static_cast<char*>(msg->msg_iov[i].iov_base));
        remaining -= len;
        iter += len;
      }
      if (remaining > 0)
        msg->msg_flags |= MSG_TRUNC;
      bytes_read = datagram.content_.size() - remaining;
      queue_.pop_front();
      ++read_pos_;
    }
  }

  // If no bytes are read in recvmsg, control messages are not returned either.
  if (bytes_read <= 0) {
    // Set this output parameter to indicate there are no control messages.
    msg->msg_controllen = 0;
  } else {
    size_t msg_controllen = 0;
    struct cmsghdr* cmsg_last = NULL;

    // The descriptors of the data this read starts with (several groups only
    // if earlier reads were empty).
    std::vector<int> fds;
    while (!cmsg_fd_queue_.empty() && cmsg_fd_queue_.front().pos <= read_start) {
      const std::vector<int>& group = cmsg_fd_queue_.front().fds;
      fds.insert(fds.end(), group.begin(), group.end());
      cmsg_fd_queue_.pop_front();
    }
    if (!fds.empty()) {

      socklen_t cmsg_len = CMSG_LEN(fds.size() * sizeof(int));  // NOLINT
      while (CMSG_SPACE(cmsg_len) > msg->msg_controllen && !fds.empty()) {
        // Cleanup file descriptors that are not passed back to the client so we
        // do not leak them.  Close the last ones first so it acts like a FIFO.
        // This is not part of any spec, but just makes the most intuitive
        // sense.
        int fd = fds.back();
        sys->CloseLocked(fd);
        fds.pop_back();
        cmsg_len = CMSG_LEN(fds.size() * sizeof(int));  // NOLINT
        msg->msg_flags |= MSG_CTRUNC;
      }

      if (CMSG_SPACE(cmsg_len) > msg->msg_controllen) {
        msg_controllen = 0;
      } else {
        struct cmsghdr* cmsg = CMSG_FIRSTHDR(msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = cmsg_len;
        memcpy(CMSG_DATA(cmsg), &fds[0], fds.size() * sizeof(int));  // NOLINT
        msg_controllen = CMSG_SPACE(cmsg_len);
        cmsg_last = cmsg;
      }
    }

    if (pass_cred_) {
      if (CMSG_SPACE(sizeof(ucred)) + msg_controllen <= msg->msg_controllen) {
        struct cmsghdr* cmsg = cmsg_last == NULL ?
            CMSG_FIRSTHDR(msg) : CMSG_NXTHDR(msg, cmsg_last);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_CREDENTIALS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(ucred));
        memcpy(CMSG_DATA(cmsg), &cred, sizeof(ucred));
        msg_controllen += cmsg->cmsg_len;
      }
    }

    msg->msg_controllen = msg_controllen;
  }

  if (bytes_read > 0) {
    // Notify any listeners waiting to write on the peer.
    if (peer_)
      peer_->NotifyListeners();
    return bytes_read;
  }

  if (!peer_ && socket_type_ != SOCK_DGRAM) {
    // The other end of the socketpair has been closed, returns EOF(0).
    return 0;
  }
  errno = EAGAIN;

  return -1;
}

ssize_t LocalSocket::send(const void* buf, size_t len, int flags) {
  return this->sendto(buf, len, flags, NULL, 0);
}

ssize_t LocalSocket::sendto(const void* buf, size_t len, int flags,
                            const sockaddr* dest_addr, socklen_t addrlen) {
  if (dest_addr || addrlen) {
    errno = EINVAL;
    return -1;
  }

  if (len == 0)
    return 0;

  struct msghdr msg = {};
  struct iovec iov;

  // This is passed in as a member of a const struct msghdr below, so casting
  // away constness is ok here.
  iov.iov_base = const_cast<void*>(buf);
  iov.iov_len = len;
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  return this->sendmsg(&msg, 0);
}

int LocalSocket::sendmsg(const struct msghdr* msg, int flags) {
  if (stream_dir_ == READ_ONLY) {
    errno = EBADF;
    return -1;
  }

  if (socket_type_ != SOCK_DGRAM && connect_state_ != SOCKET_CONNECTED) {
    errno = EINVAL;
    return -1;
  }

  if (peer_)
    return peer_->HandleSendmsgLocked(msg, my_cred_);

  if (socket_type_== SOCK_DGRAM) {
    if (!logd_target_name_.empty()) {
      scoped_refptr<LocalSocket> bound_socket;
      bound_socket = VirtualFileSystem::GetVirtualFileSystem()->
          GetLogdSocketNamespace()->GetByName(logd_target_name_);
      if (bound_socket)
        return bound_socket->HandleSendmsgLocked(msg, my_cred_);
    }
    errno = ECONNREFUSED;
    return -1;
  }

  errno = ECONNRESET;
  return -1;
}

ssize_t LocalSocket::write(const void* buf, size_t count) {
  return this->send(buf, count, 0);
}

int LocalSocket::ioctl(int request, va_list ap) {
  if (request == FIONREAD) {
    int* out = va_arg(ap, int*);
    if (socket_type_ == SOCK_STREAM) {
      *out = buffer_.size();
    } else {
      if (!queue_.empty())
        *out = queue_.front().content_.size();
      else
        *out = 0;
    }
    return 0;
  }
  return SocketStream::ioctl(request, ap);
}

bool LocalSocket::IsSelectReadReady() const {
  return (GetPollEvents() & (POLLIN | POLLHUP | POLLERR)) != 0;
}

bool LocalSocket::IsSelectWriteReady() const {
  return (GetPollEvents() & (POLLOUT | POLLERR)) != 0;
}

bool LocalSocket::IsSelectExceptionReady() const {
  // exceptfds are actually out-of-band data, which we do not support.
  return false;
}

int16_t LocalSocket::GetPollEvents() const {
  // Note: Today we support socket_type_ != SOCK_STREAM only for socketpair plus
  // sockets required for logd device.
  // Thus, socket_type_ != SOCK_STREAM implies
  // connect_state_ == SOCKET_CONNECTED.
  switch (connect_state_) {
    case SOCKET_NEW:
      return POLLOUT | POLLHUP;
    case SOCKET_CONNECTING:
      // TODO(crbug.com/470853): Get rid of this state.
      return 0;
    case SOCKET_CONNECTED:
      // WRITE_ONLY/READ_ONLY are used only for pipes, where we need special
      // care.
      if (stream_dir_ == READ_ONLY) {
        // For a read pipe whose peer was closed, POLLIN is set only if there is
        // remaining data, in contrast to stream sockets where POLLIN is always
        // set.
        return
            (buffer_.size() > 0 ? POLLIN : 0) | (peer_ == NULL ? POLLHUP : 0);
      } else if (stream_dir_ == WRITE_ONLY) {
        // For a write pipe whose peer was closed, POLLERR is always set.
        // TODO(crbug.com/359400): On Linux, POLLOUT can be NOT set if the pipe
        // buffer was full at the time the peer was closed.
        if (peer_ == NULL) {
          return POLLOUT | POLLERR;
        }
        return peer_->buffer_.size() < peer_->buffer_.capacity() ? POLLOUT : 0;
      } else {
        int16_t events = 0;
        if (CanRead()) {
          events |= POLLIN;
        }
        if (CanWrite()) {
          events |= POLLOUT;
        }
        if (socket_type_ != SOCK_DGRAM && peer_ == NULL) {
          events |= POLLHUP;
        }
        // TODO(crbug.com/359400): Set POLLERR under some conditions. On Linux,
        // the bit can be set when the peer was closed before the written data
        // was read.
        return events;
      }
    case SOCKET_LISTENING:
      if (!connection_queue_.empty()) {
        return POLLIN | POLLOUT;
      }
      return POLLOUT;
    default:
      LOG_ALWAYS_FATAL("Must not reach here");
  }
}

bool LocalSocket::CanRead() const {
  ALOG_ASSERT(stream_dir_ == READ_WRITE);
  // If the peer has been closed, whether the socket is readable depends on
  // socket type.
  if (socket_type_ != SOCK_DGRAM && peer_ == NULL) {
    return true;
  }
  if (socket_type_ == SOCK_STREAM) {
    return buffer_.size() > 0;
  }
  return !queue_.empty();
}

bool LocalSocket::CanWrite() const {
  ALOG_ASSERT(stream_dir_ == READ_WRITE);
  // If the peer has been closed, the socket is always writable.
  if (peer_ == NULL) {
    return true;
  }
  if (socket_type_ == SOCK_STREAM) {
    return peer_->buffer_.size() < peer_->buffer_.capacity();
  }
  // Our packet-based sockets have unlimited buffer.
  return true;
}

int LocalSocket::HandleSendmsgLocked(const struct msghdr* msg,
                                     const ucred& peer_cred) {
  VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
  sys->mutex().AssertAcquired();

  const struct iovec* buf = msg->msg_iov;
  size_t len = msg->msg_iovlen;

  ssize_t bytes_sent = 0;
  size_t bytes_attempted = 0;
  // The position of this message's data (see ControlMessageFDs).
  const uint64_t data_pos = written_pos_;
  if (len > 0) {
    if (socket_type_ == SOCK_STREAM) {
      ALOG_ASSERT(memcmp(&peer_cred, &peer_cred_, sizeof(peer_cred)) == 0);
      for (size_t i = 0; i < len; ++i) {
        bytes_attempted += buf[i].iov_len;
        bytes_sent += buffer_.write(static_cast<const char*>(buf[i].iov_base),
                                    buf[i].iov_len);
      }
    } else {
      queue_.push_back(Datagram());
      Datagram& datagram = queue_.back();
      datagram.cred_ = peer_cred;
      for (size_t i = 0; i < len; ++i) {
        const char* begin = static_cast<const char*>(buf[i].iov_base);
        const char* end = begin + buf[i].iov_len;
        datagram.content_.insert(datagram.content_.end(), begin, end);
        bytes_attempted += buf[i].iov_len;
        bytes_sent += buf[i].iov_len;
      }
    }
    written_pos_ += socket_type_ == SOCK_STREAM ? bytes_sent : 1;
  }

  // If we did not send any bytes, do not process any control messages either.
  if (bytes_sent && msg->msg_controllen > 0) {
    // The CMSG macros cannot deal with const msghdrs, so cast away constness
    // for this section, but make all access to the underlying data through
    // const local variables.
    struct msghdr* nonconst_msg = const_cast<struct msghdr*>(msg);
    cmsg_fd_queue_.resize(cmsg_fd_queue_.size() + 1);
    cmsg_fd_queue_.back().pos = data_pos;
    std::vector<int>& fds = cmsg_fd_queue_.back().fds;
    for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(nonconst_msg);
         cmsg;
         cmsg = CMSG_NXTHDR(nonconst_msg, cmsg)) {
      // We only support one control message, specifically of type
      // SCM_RIGHTS to send file descriptors.
      ALOG_ASSERT(cmsg->cmsg_level == SOL_SOCKET);
      ALOG_ASSERT(cmsg->cmsg_type == SCM_RIGHTS);
      if (cmsg->cmsg_level == SOL_SOCKET &&
          cmsg->cmsg_type == SCM_RIGHTS &&
          cmsg->cmsg_len >= CMSG_LEN(0)) {
        size_t payload_len = cmsg->cmsg_len - CMSG_LEN(0);
        ALOG_ASSERT(payload_len % sizeof(int) == 0);  // NOLINT
        const int *wire_fds = reinterpret_cast<const int*>(CMSG_DATA(cmsg));
        size_t wire_fds_len = payload_len / sizeof(int);  // NOLINT
        // Dup the file descriptors before adding them to the control message.
        // This emulates what happens in Posix when sending file descriptors in
        // the same process (as webviewchromium does).
        for (size_t i = 0; i < wire_fds_len; ++i)
          fds.push_back(sys->DupLocked(wire_fds[i], -1));
      }
    }
    if (fds.empty())
      cmsg_fd_queue_.pop_back();
  }

  if (bytes_sent > 0) {
    sys->Broadcast();
    NotifyListeners();
  }

  if (bytes_sent == 0 && bytes_attempted != 0) {
    errno = EAGAIN;
    return -1;
  }

  return bytes_sent;
}

const char* LocalSocket::GetStreamType() const {
  return "local_socket";
}

}  // namespace posix_translation
