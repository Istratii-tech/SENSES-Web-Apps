// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "posix_translation/tcp_socket.h"

#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <string.h>

#include <algorithm>

#include "common/arc_strace.h"
#include "common/alog.h"
#include "posix_translation/socket_util.h"
#include "posix_translation/time_util.h"
#include "posix_translation/virtual_file_system.h"
#include "ppapi/c/pp_errors.h"
#include "ppapi/cpp/module.h"
#include "ppapi/cpp/net_address.h"
#include "ppapi/cpp/tcp_socket.h"

namespace posix_translation {

// Thin wrapper of pp::TCPSocket to manage the lifetime of pp::TCPSocket.
// Background: the problem is some blocking call (such as ::read()), and
// ::close() for this class may have race condition.
// Assuming ::read() is called on a thread and it is blocked, and ::close() is
// called on another thread.
// On current FileStream implementation, the final ::close() destructs the
// stream instance. So, when ::read() is unblocked after the ::close(), it is
// necessary to know if the socket is closed or not without touching the
// TCPSocket instance (otherwise it may cause use-after-free problem).
// This thin wrapper provides such a functionality.
//
// How to use:
//
//     :
//   // Keep the reference to SocketWrapper locally.
//   scoped_refptr<SocketWrapper> wrapper(socket_);
//   VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
//   const base::TimeTicks time_limit = internal::TimeOutToTimeLimit(timeout);
//   bool is_timedout = false;
//   while (!is_timedout && ... condition ...) {
//     is_timedout = sys->WaitUntil(time_limit);
//     // Check close state before accessing any member variables since this
//     // instance might be destroyed while this thread was waiting.
//     if (wrapper->is_closed()) {
//       errno = EBADF;
//       return -1;
//     }
//   }
//     :
//
// Note: this is very close to what WeakPtr does. However, we cannot use
// WeakPtrFactory because the factory is bound to the thread where the first
// WeakPtr is created, and then it is prohibited to destruct it on another
// thread.
//
// This class will be touched from multi threads. To access is_closed() and
// Close(), the caller has responsibility to lock the filesystem-wise giant
// mutex in advance.
class TCPSocket::SocketWrapper
    : public base::RefCountedThreadSafe<SocketWrapper> {
 public:
  // Takes the ownership of socket.
  explicit SocketWrapper(const pp::TCPSocket& socket)
      : socket_(socket), closed_(false) {
  }

  bool is_closed() const {
    VirtualFileSystem::GetVirtualFileSystem()->mutex().AssertAcquired();
    return closed_;
  }

  void Close() {
    VirtualFileSystem::GetVirtualFileSystem()->mutex().AssertAcquired();
    if (is_closed())
      return;
    closed_ = true;
    socket_.Close();
  }

  pp::TCPSocket* socket() {
    return &socket_;
  }

 private:
  // Do not allow to destruct this class manually from the client code
  // to avoid to delete the object accidentally while there are still
  // references to it.
  friend class base::RefCountedThreadSafe<SocketWrapper>;
  ~SocketWrapper() {
  }

  pp::TCPSocket socket_;
  bool closed_;
  DISALLOW_COPY_AND_ASSIGN(SocketWrapper);
};

TCPSocket::TCPSocket(int fd, int socket_family, int oflag)
    : SocketStream(socket_family, oflag), fd_(fd), factory_(this),
      socket_(new SocketWrapper(pp::TCPSocket(
          VirtualFileSystem::GetVirtualFileSystem()->instance()))),
      read_buf_(kBufSize), connect_state_(TCP_SOCKET_NEW), eof_(false),
      read_sent_(false), write_sent_(false), connect_error_(0),
      no_delay_(0) {
  ALOG_ASSERT(socket_family == AF_INET || socket_family == AF_INET6);
}

TCPSocket::TCPSocket(const pp::TCPSocket& socket)
    : SocketStream(kUnknownSocketFamily, O_RDWR), fd_(-1), factory_(this),
      socket_(new SocketWrapper(socket)),
      read_buf_(kBufSize), connect_state_(TCP_SOCKET_NEW), eof_(false),
      read_sent_(false), write_sent_(false), connect_error_(0),
      no_delay_(0) {
}

TCPSocket::~TCPSocket() {
  if (!socket_->is_closed()) {
    // Unlike UDPSocket, this happens when TCPSocket instance is created but
    // discarded before it is registered to file system. For example, this
    // happens on error case of accept().
    CloseLocked();
  }
}

void TCPSocket::MarkAsErrorLocked(int error) {
  VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
  sys->mutex().AssertAcquired();
  if (!IsTerminated()) {
    if (connect_state_ == TCP_SOCKET_CONNECTING)
      connect_error_ = error;
    if (!is_block()) {
      // getsockopt() does not seem to expose SO_ERROR for blocking sockets.
      // This is likely because the main reason for SO_ERROR is to allow apps
      // to query errors after a successful select() call, during which
      // a non-blocking connect may have failed.
      error_ = error;
    }
    connect_state_ = TCP_SOCKET_ERROR;
    NotifyListeners();
  }
}

int TCPSocket::bind(const sockaddr* addr, socklen_t addrlen) {
  int error =
      internal::VerifyInputSocketAddress(addr, addrlen, socket_family_);
  if (error) {
    errno = error;
    return -1;
  }

  VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
  pp::NetAddress address =
      internal::SockAddrToNetAddress(sys->instance(), addr);

  ALOGI("TCPSocket::Bind: %s",
        address.DescribeAsString(true).AsString().c_str());
  scoped_refptr<SocketWrapper> wrapper(socket_);
  int32_t result = PP_OK_COMPLETIONPENDING;
  {
    base::AutoUnlock unlock(sys->mutex());
    result = wrapper->socket()->Bind(address, pp::BlockUntilComplete());
  }
  ARC_STRACE_REPORT_PP_ERROR(result);
  // Check close state before accessing any member variables since this
  // instance might be destroyed while this thread was waiting.
  if (wrapper->is_closed()) {
    errno = EBADF;
    return -1;
  }

  if (result != PP_OK) {
    if (result == PP_ERROR_ADDRESS_IN_USE) {
      errno = EADDRINUSE;
    } else {
      errno = EINVAL;
    }
    return -1;
  }

  return 0;
}

int TCPSocket::listen(int backlog) {
  if (connect_state_ != TCP_SOCKET_NEW) {
    // This could happen, for example, when a user writes as follows:
    //   s = socket(AF_INET, SOCK_STREAM, 0);
    //   connect(s, ... something peer ...);
    //   listen(s, 5);
    // There is no explicit documentation in the man page, but empirically
    // under Linux, EINVAL is raised.
    errno = EINVAL;
    return -1;
  }

  connect_state_ = TCP_SOCKET_LISTENING;

  scoped_refptr<SocketWrapper> wrapper(socket_);
  int32_t result = PP_OK_COMPLETIONPENDING;
  {
    base::AutoUnlock unlock(VirtualFileSystem::GetVirtualFileSystem()->mutex());
    result = wrapper->socket()->Listen(backlog, pp::BlockUntilComplete());
  }
  ARC_STRACE_REPORT_PP_ERROR(result);
  // Check close state before accessing any member variables since this
  // instance might be destroyed while this thread was waiting.
  if (wrapper->is_closed()) {
    errno = EBADF;
    return -1;
  }

  if (result != PP_OK) {
    if (result == PP_ERROR_NOSPACE)
      errno = EOPNOTSUPP;
    else
      errno = EADDRINUSE;
    MarkAsErrorLocked(errno);
    return -1;
  }

  // The listen() has actually been started. So, start "accept" as a background
  // task to support non-blocking ::accept().
  pp::Module::Get()->core()->CallOnMainThread(
      0, factory_.NewCallback(&TCPSocket::Accept));
  return 0;
}

int TCPSocket::accept(sockaddr* addr, socklen_t* addrlen) {
  // accept(2) allows NULL/NULL to be passed for sockaddr.
  if (addr) {
    int error = internal::VerifyOutputSocketAddress(addr, addrlen);
    if (error) {
      errno = error;
      return -1;
    }
  }

  if (connect_state_ != TCP_SOCKET_LISTENING) {
    errno = EINVAL;
    return -1;
  }

  VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
  if (is_block()) {
    // Wait until some peer connects to the listening socket, or timed out.
    const base::TimeTicks time_limit =
        internal::TimeOutToTimeLimit(recv_timeout_);
    bool is_timedout = false;
    scoped_refptr<SocketWrapper> wrapper(socket_);
    while (!is_timedout && accepted_socket_.is_null()) {
      is_timedout = sys->WaitUntil(time_limit);
      // Check close state before accessing any member variables since this
      // instance might be destroyed while this thread was waiting.
      if (wrapper->is_closed()) {
        errno = EBADF;
        return -1;
      }
    }
  }

  if (accepted_socket_.is_null()) {
    errno = EAGAIN;
    return -1;
  }

  pp::TCPSocket accepted_socket = accepted_socket_;
  accepted_socket_ = pp::TCPSocket();
  pp::Module::Get()->core()->CallOnMainThread(
      0, factory_.NewCallback(&TCPSocket::Accept));

  // Before creating TCPSocket instance, extract the address to check an error.
  sockaddr_storage storage = {};
  if (addr) {
    if (!internal::NetAddressToSockAddrStorage(
            accepted_socket.GetRemoteAddress(), AF_UNSPEC, false, &storage)) {
      // According to man, there seems no appropriate error is defined for
      // this case. So, use ENOBUF to let the client know that this is some
      // internal error.
      errno = ENOBUFS;
      return -1;
    }
  }

  scoped_refptr<TCPSocket> socket = new TCPSocket(accepted_socket);
  int fd = sys->AddFileStreamLocked(socket);
  if (fd < 0) {
    errno = EMFILE;
    return -1;
  }

  socket->fd_ = fd;
  socket->connect_state_ = TCP_SOCKET_CONNECTED;
  // Start reading on background.
  socket->PostReadTaskLocked();

  // Finally, copy the address data if necessary.
  if (addr)
    internal::CopySocketAddress(storage, addr, addrlen);
  return fd;
}

int TCPSocket::connect(const sockaddr* serv_addr, socklen_t addrlen) {
  int error =
      internal::VerifyInputSocketAddress(serv_addr, addrlen, socket_family_);
  if (error) {
    errno = error;
    return -1;
  }

  if (IsTerminated()) {
    // TODO(crbug.com/358855): Allow new connect() calls after an unsuccessful
    // connection attempt.
    errno = EBADF;
    return -1;
  }

  if (connect_state_ == TCP_SOCKET_CONNECTED ||
      connect_state_ == TCP_SOCKET_LISTENING) {
    errno = EISCONN;
    return -1;
  }

  VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
  if (connect_state_ == TCP_SOCKET_NEW) {
    pp::NetAddress address =
        internal::SockAddrToNetAddress(sys->instance(), serv_addr);
    ALOGI("TCPSocket::connect: %s",
          address.DescribeAsString(true).AsString().c_str());

    connect_state_ = TCP_SOCKET_CONNECTING;
    pp::Module::Get()->core()->CallOnMainThread(
        0, factory_.NewCallback(&TCPSocket::Connect, address));
    if (!is_block()) {
      errno = EINPROGRESS;
      return -1;
    }
  } else {
    ALOG_ASSERT(connect_state_ == TCP_SOCKET_CONNECTING);
    if (!is_block()) {
      errno = EALREADY;
      return -1;
    }
    // Blocking connect should block, waiting for results of a pending connect.
  }

  scoped_refptr<SocketWrapper> wrapper(socket_);
  while (connect_state_ == TCP_SOCKET_CONNECTING) {
    sys->Wait();
    // Check close state before accessing any member variables since this
    // instance might be destroyed while this thread was waiting.
    if (wrapper->is_closed()) {
      errno = EBADF;
      return -1;
    }
  }

  if (connect_state_ == TCP_SOCKET_ERROR) {
    errno = connect_error_;
    return -1;
  }

  ALOG_ASSERT(connect_state_ == TCP_SOCKET_CONNECTED);
  return 0;
}

off64_t TCPSocket::lseek(off64_t offset, int whence) {
  errno = ESPIPE;
  return -1;
}

ssize_t TCPSocket::read(void* buf, size_t count) {
  return recv(buf, count, 0);
}

ssize_t TCPSocket::recv(void *buf, size_t len, int flags) {
  // TODO(crbug.com/242604): Handle flags such as MSG_DONTWAIT
  if (connect_state_ == TCP_SOCKET_NEW ||
      connect_state_ == TCP_SOCKET_LISTENING) {
    errno = ENOTCONN;
    return -1;
  }

  if (is_block()) {
    scoped_refptr<SocketWrapper> wrapper(socket_);
    VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
    const base::TimeTicks time_limit =
        internal::TimeOutToTimeLimit(recv_timeout_);
    bool is_timedout = false;
    while (!is_timedout && !IsSelectReadReady() && !IsTerminated()) {
      is_timedout = sys->WaitUntil(time_limit);
      // Check close state before accessing any member variables since this
      // instance might be destroyed while this thread was waiting.
      if (wrapper->is_closed()) {
        errno = EBADF;
        return -1;
      }
    }
  } else if (connect_state_ == TCP_SOCKET_CONNECTING) {
    // Non-blocking and still connecting.
    errno = EAGAIN;
    return -1;
  }

  size_t nread = std::min(len, in_buf_.size());
  if (nread) {
    std::copy(in_buf_.begin(), in_buf_.begin() + nread,
              reinterpret_cast<char*>(buf));
    if (!(flags & MSG_PEEK))
      in_buf_.erase(in_buf_.begin(), in_buf_.begin() + nread);
    PostReadTaskLocked();

    return nread;
  }

  if (!is_connected() || eof_)
    return 0;

  errno = EAGAIN;
  return -1;
}

ssize_t TCPSocket::recvfrom(void* buf, size_t len, int flags, sockaddr* addr,
                            socklen_t* addrlen) {
  if (!addr && !addrlen)
    return recv(buf, len, flags);
  errno = EINVAL;
  return -1;
}

int TCPSocket::recvmsg(struct msghdr* msg, int flags) {
  if (!msg || !msg->msg_iov) {
    errno = EINVAL;
    return -1;
  }
  if (msg->msg_iovlen != 1) {
    ALOGE("TCPSocket only supports trivial recvmsg with msg_iovlen of 1");
    errno = EINVAL;
    return -1;
  }
  if (msg->msg_controllen != 0) {
    ALOGE("TCPSocket only supports trivial recvmsg with no control data");
    errno = EINVAL;
    return -1;
  }
  msg->msg_flags = 0;
  return recv(msg->msg_iov[0].iov_base, msg->msg_iov[0].iov_len, flags);
}

ssize_t TCPSocket::write(const void* buf, size_t count) {
  return send(buf, count, 0);
}

ssize_t TCPSocket::send(const void* buf, size_t len, int flags) {
  // TODO(crbug.com/242604): Handle flags such as MSG_DONTWAIT
  if (!is_connected()) {
    errno = EPIPE;
    return -1;
  }

  bool is_blocking = is_block();

  if (is_blocking && out_buf_.size() >= kBufSize) {
    scoped_refptr<SocketWrapper> wrapper(socket_);
    VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
    const base::TimeTicks time_limit =
        internal::TimeOutToTimeLimit(send_timeout_);
    bool is_timedout = false;
    while (!is_timedout && out_buf_.size() >= kBufSize && is_connected()) {
      is_timedout = sys->WaitUntil(time_limit);
      // Check close state before accessing any member variables since this
      // instance might be destroyed while this thread was waiting.
      if (wrapper->is_closed()) {
        errno = EBADF;
        return -1;
      }
    }
    if (!is_connected()) {
      errno = EIO;
      return -1;
    }
  }

  if (out_buf_.size() < kBufSize) {
    out_buf_.insert(out_buf_.end(),
                    reinterpret_cast<const char*>(buf),
                    reinterpret_cast<const char*>(buf) + len);
    if (!write_sent_) {
      pp::Module::Get()->core()->CallOnMainThread(
          0, factory_.NewCallback(&TCPSocket::Write));
    }
    return len;
  }

  ALOG_ASSERT(!is_blocking);

  errno = EAGAIN;
  return -1;
}

ssize_t TCPSocket::sendto(const void* buf, size_t len, int flags,
                          const sockaddr* dest_addr, socklen_t addrlen) {
  if (!dest_addr && !addrlen)
    return send(buf, len, flags);
  errno = EINVAL;
  return -1;
}

int TCPSocket::sendmsg(const struct msghdr* msg, int flags) {
  if (!msg || !msg->msg_iov) {
    errno = EINVAL;
    return -1;
  }
  if (msg->msg_iovlen != 1) {
    ALOGE("TCPSocket only supports trivial sendmsg with msg_iovlen of 1");
    errno = EINVAL;
    return -1;
  }
  if (msg->msg_controllen != 0) {
    ALOGE("TCPSocket only supports trivial sendmsg with no control data");
    errno = EINVAL;
    return -1;
  }
  return send(msg->msg_iov[0].iov_base, msg->msg_iov[0].iov_len, flags);
}

int TCPSocket::ioctl(int request, va_list ap) {
  if (request == FIONREAD) {
    int* out = va_arg(ap, int*);
    *out = in_buf_.size();
    return 0;
  }
  return SocketStream::ioctl(request, ap);
}

bool TCPSocket::GetOptNameData(
    int level, int optname, socklen_t* len, void** storage,
    const void* user_data, socklen_t user_data_len) {
  if (level == IPPROTO_TCP) {
    switch (optname) {
      case TCP_NODELAY:
        *storage = &no_delay_;
        *len = SIZEOF_AS_SOCKLEN(int);  // NOLINT
        ALOG_ASSERT(*len == sizeof(no_delay_));
        return true;
    }
  }

  return SocketStream::GetOptNameData(level, optname, len, storage, user_data,
                                      user_data_len);
}

int TCPSocket::setsockopt(int level, int optname, const void* optval,
                          socklen_t optlen) {
  if (level == SOL_IPV6 && optname == IPV6_V6ONLY) {
    // Currently, IPV6_V6ONLY is not supported by pepper.
    // This is just a work around until it is supported. The default value of
    // IPV6_V6ONLY is 0 (false). Some applications try to set the 0
    // explicitly and fail if it is not supported. So, here, we return
    // 0 (success) only if *optval is 0.
    // TODO(crbug.com/371334): Use pepper's IPV6_V6ONLY option when supported.
    if (optlen < SIZEOF_AS_SOCKLEN(int) ||  // NOLINT(readability/casting)
        *static_cast<const int*>(optval) != 0) {
      errno = EINVAL;
      return -1;
    }
    return 0;
  }

  int no_delay = no_delay_;
  int result = SocketStream::setsockopt(level, optname, optval, optlen);
  if (result != 0)
    return result;

  if (no_delay == no_delay_)
    return 0;

  scoped_refptr<SocketWrapper> wrapper(socket_);
  int32_t pp_error = PP_OK_COMPLETIONPENDING;
  {
    base::AutoUnlock unlock(
        VirtualFileSystem::GetVirtualFileSystem()->mutex());
    pp_error = wrapper->socket()->SetOption(
        PP_TCPSOCKET_OPTION_NO_DELAY, pp::Var(no_delay_ ? true : false),
        pp::BlockUntilComplete());
  }
  ARC_STRACE_REPORT_PP_ERROR(pp_error);
  // Check close state before accessing any member variables since this
  // instance might be destroyed while this thread was waiting.
  if (wrapper->is_closed()) {
    errno = EBADF;
    return -1;
  }

  if (pp_error != PP_OK) {
    errno = ENOPROTOOPT;  // TODO(crbug.com/358932): Pick correct errno.
    return -1;
  }
  return 0;
}

int TCPSocket::getpeername(sockaddr* name, socklen_t* namelen) {
  int error = internal::VerifyOutputSocketAddress(name, namelen);
  if (error) {
    errno = error;
    return -1;
  }

  sockaddr_storage storage;
  if (!internal::NetAddressToSockAddrStorage(
          socket_->socket()->GetRemoteAddress(), AF_UNSPEC, false, &storage)) {
    memset(&storage, 0, sizeof(storage));
    storage.ss_family = socket_family_;
  }

  internal::CopySocketAddress(storage, name, namelen);
  return 0;
}

int TCPSocket::getsockname(sockaddr* name, socklen_t* namelen) {
  int error = internal::VerifyOutputSocketAddress(name, namelen);
  if (error) {
    errno = error;
    return -1;
  }

  sockaddr_storage storage;
  if (!internal::NetAddressToSockAddrStorage(
          socket_->socket()->GetLocalAddress(), AF_UNSPEC, false, &storage)) {
    memset(&storage, 0, sizeof(storage));
    storage.ss_family = socket_family_;
  }

  internal::CopySocketAddress(storage, name, namelen);
  return 0;
}

bool TCPSocket::IsSelectReadReady() const {
  // Closed socket should return an error without blocking.
  if (socket_->is_closed())
    return true;

  switch (connect_state_) {
    case TCP_SOCKET_NEW:
      // If the socket is neither connected nor listening, the socket is
      // considered read_ready, as the read() should return error without
      // blocking.
      return true;
    case TCP_SOCKET_CONNECTING:
      // If the socket is connecting, no readable data is available.
      return false;
    case TCP_SOCKET_CONNECTED:
      // A connected socket is considered read_ready if there is data
      // available for reading, or if EOF has been detected.
      return !in_buf_.empty() || eof_;
    case TCP_SOCKET_LISTENING:
      // A listening socket is considered read_ready when there is a
      // connection waiting to be accepted.
      return !accepted_socket_.is_null();
    case TCP_SOCKET_ERROR:
      // On error, the read() should return error without blocking.
      return true;
    default:
      // Should not reach here.
      ALOG_ASSERT(false);
  }
  return false;
}

bool TCPSocket::IsSelectWriteReady() const {
  // Closed socket should return an error without blocking.
  if (socket_->is_closed())
    return true;

  switch (connect_state_) {
    case TCP_SOCKET_NEW:
      // If the socket is neither connected nor listening, the socket is
      // considered write_ready, as the write() should return error without
      // blocking.
      return true;
    case TCP_SOCKET_CONNECTING:
      // If the socket is connecting, the socket is not yet writable.
      return false;
    case TCP_SOCKET_CONNECTED:
      // A connected socket is considered write_ready if there is some space
      // available in the internal buffer.
      return out_buf_.size() < kBufSize;
    case TCP_SOCKET_LISTENING:
      // The listening socket is unwritable.
      return false;
    case TCP_SOCKET_ERROR:
      // On error, the write() should return error without blocking.
      return true;
    default:
      // Should not reach here.
      ALOG_ASSERT(false);
  }
  return false;
}

bool TCPSocket::IsSelectExceptionReady() const {
  return connect_state_ == TCP_SOCKET_ERROR;
}

int16_t TCPSocket::GetPollEvents() const {
  // Currently we use IsSelect*Ready() family temporarily (and wrongly).
  // TODO(crbug.com/359400): Fix the implementation.
  return ((IsSelectReadReady() ? POLLIN : 0) |
          (IsSelectWriteReady() ? POLLOUT : 0) |
          (IsSelectExceptionReady() ? POLLERR : 0));
}

void TCPSocket::OnLastFileRef() {
  ALOG_ASSERT(!socket_->is_closed());
  CloseLocked();
}

bool TCPSocket::IsTerminated() const {
  return socket_->is_closed() || connect_state_ == TCP_SOCKET_ERROR;
}

void TCPSocket::PostReadTaskLocked() {
  VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
  sys->mutex().AssertAcquired();

  if (!is_connected() || read_sent_) {
    return;  // No more async reads.
  }
  if (in_buf_.size() >= kBufSize / 2) {
    return;  // Enough to read locally.
  }
  if (eof_) {
    return;  // We already hit the EOF.
  }
  read_sent_ = true;
  if (!pp::Module::Get()->core()->IsMainThread()) {
    pp::Module::Get()->core()->CallOnMainThread(
        0, factory_.NewCallback(&TCPSocket::Read));
  } else {
    // If on main Pepper thread call it directly.
    ReadLocked();
  }
}

void TCPSocket::Accept(int32_t result) {
  ALOG_ASSERT(result == PP_OK);
  VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
  base::AutoLock lock(sys->mutex());

  int32_t pp_error = socket_->socket()->Accept(
      factory_.NewCallbackWithOutput(&TCPSocket::OnAccept));
  ALOG_ASSERT(pp_error == PP_OK_COMPLETIONPENDING);
}

void TCPSocket::OnAccept(int32_t result, const pp::TCPSocket& accepted_socket) {
  // TODO(crbug.com/364744): Handle error cases.
  VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
  base::AutoLock lock(sys->mutex());
  ALOG_ASSERT(accepted_socket_.is_null());
  accepted_socket_ = accepted_socket;
  sys->Broadcast();
  NotifyListeners();
}

void TCPSocket::Connect(int32_t result, const pp::NetAddress& address) {
  ALOG_ASSERT(result == PP_OK);
  VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
  base::AutoLock lock(sys->mutex());
  // A closed socket means we are in destructor. On the other hand,
  // error should not happen in connect.
  ALOG_ASSERT(connect_state_ == TCP_SOCKET_CONNECTING);
  int32_t pp_error = socket_->socket()->Connect(
      address, factory_.NewCallback(&TCPSocket::OnConnect));
  ALOG_ASSERT(pp_error == PP_OK_COMPLETIONPENDING);
}

void TCPSocket::OnConnect(int32_t result) {
  VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
  base::AutoLock lock(sys->mutex());
  // A closed socket means we are in destructor. On the other hand,
  // error should not happen in connect.
  ALOG_ASSERT(connect_state_ == TCP_SOCKET_CONNECTING);
  if (result == PP_OK) {
    connect_state_ = TCP_SOCKET_CONNECTED;
    PostReadTaskLocked();
    NotifyListeners();
  } else {
    MarkAsErrorLocked(ECONNREFUSED);
  }
  sys->Broadcast();
}

void TCPSocket::Read(int32_t result) {
  ALOG_ASSERT(result == PP_OK);
  VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
  base::AutoLock lock(sys->mutex());
  ReadLocked();
}

void TCPSocket::ReadLocked() {
  VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
  sys->mutex().AssertAcquired();

  if (IsTerminated()) {
    read_sent_ = false;
    sys->Broadcast();
    return;
  }

  pp::CompletionCallback callback = factory_.NewCallback(&TCPSocket::OnRead);
  int32_t pp_error = socket_->socket()->Read(
      &read_buf_[0], read_buf_.size(),
      callback);
  if (pp_error >= 0) {
    // This usually only happens on tests. We need to cancel the original
    // callback to avoid leaks, and to use OnReadLocked instead of OnRead in
    // order to avoid re-acquiring sys->mutex() and crashing.
    callback.Run(PP_ERROR_USERCANCEL);
    OnReadLocked(pp_error);
  } else {
    ALOG_ASSERT(pp_error == PP_OK_COMPLETIONPENDING);
  }
}

void TCPSocket::OnRead(int32_t result) {
  if (result == PP_ERROR_USERCANCEL) {
    // The callback was cancelled since it was possible to run it synchronously
    // on the same thread that requested the read.
    return;
  }
  VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
  base::AutoLock lock(sys->mutex());
  OnReadLocked(result);
}

void TCPSocket::OnReadLocked(int32_t result) {
  VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
  sys->mutex().AssertAcquired();

  read_sent_ = false;
  if (IsTerminated()) {
    sys->Broadcast();
    return;
  }

  if (result > 0) {
    in_buf_.insert(in_buf_.end(),
                   read_buf_.begin(), read_buf_.begin() + result);
    PostReadTaskLocked();
    NotifyListeners();
  } else if (result == 0) {
    eof_ = true;
    NotifyListeners();
  } else {
    MarkAsErrorLocked(EIO);  // TODO(crbug.com/358932): Pick correct error.
  }
  sys->Broadcast();
}

void TCPSocket::Write(int32_t result) {
  VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
  base::AutoLock lock(sys->mutex());
  if (!write_sent_) {
    WriteLocked();
  }
}

void TCPSocket::WriteLocked() {
  VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
  sys->mutex().AssertAcquired();
  ALOG_ASSERT(!write_sent_);

  if (IsTerminated()) {
    sys->Broadcast();
    return;
  }
  if (write_buf_.size() == 0) {
    write_buf_.swap(out_buf_);
  } else if (write_buf_.size() < kBufSize / 2) {
    // Avoid to shift the content in out_buf_ too often by only allowing
    // to move chunks either larger than kBufSize / 2 or coinciding with
    // the whole out_buf_ buffer.
    int size = std::min(kBufSize - write_buf_.size(), out_buf_.size());
    write_buf_.insert(write_buf_.end(), out_buf_.begin(),
                      out_buf_.begin() + size);
    out_buf_.erase(out_buf_.begin(), out_buf_.begin() + size);
  }

  write_sent_ = true;
  int32_t result = socket_->socket()->Write(
      &write_buf_[0], write_buf_.size(),
      factory_.NewCallback(&TCPSocket::OnWrite));
  ALOG_ASSERT(result == PP_OK_COMPLETIONPENDING);
}

void TCPSocket::OnWrite(int32_t result) {
  VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
  base::AutoLock lock(sys->mutex());

  write_sent_ = false;
  if (IsTerminated()) {
    sys->Broadcast();
    return;
  }

  if (result < 0 || (size_t)result > write_buf_.size()) {
    // Write error.
    ALOGI("TCPSocket::OnWrite: write error on %d, result: %d", fd_, result);
    MarkAsErrorLocked(EIO);  // TODO(crbug.com/358932): Pick correct error.
    sys->Broadcast();
    return;
  } else {
    write_buf_.erase(write_buf_.begin(), write_buf_.begin() + (size_t)result);
  }
  if (!write_buf_.empty() || !out_buf_.empty()) {
    WriteLocked();
  }
  sys->Broadcast();
  NotifyListeners();
}

void TCPSocket::CloseLocked() {
  VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
  // Wait for write operations to complete
  // TODO(crbug.com/351755): Refactor code so that close can't hang for ever.
  while (write_sent_ && is_connected()) {
    sys->Wait();
  }

  // Post task to the main thread, so that any pending tasks on main thread
  // will be canceled.
  int32_t result = PP_OK_COMPLETIONPENDING;
  pp::Module::Get()->core()->CallOnMainThread(
      0, factory_.NewCallback(&TCPSocket::Close, &result));
  while (result == PP_OK_COMPLETIONPENDING)
    sys->Wait();
  ARC_STRACE_REPORT_PP_ERROR(result);
}

void TCPSocket::Close(int32_t result, int32_t* pres) {
  ALOG_ASSERT(result == PP_OK);
  VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
  base::AutoLock lock(sys->mutex());
  factory_.CancelAll();
  socket_->Close();
  *pres = PP_OK;
  // Don't access any member variable after sys->Browadcast() is called.
  // It may make destructor have completed.
  NotifyListeners();
  sys->Broadcast();
}

const char* TCPSocket::GetStreamType() const {
  return "tcp";
}

}  // namespace posix_translation
