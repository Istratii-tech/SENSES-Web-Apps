// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "posix_translation/udp_socket.h"

#include <fcntl.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <string.h>

#include <algorithm>
#include <limits>

#include "common/arc_strace.h"
#include "common/alog.h"
#include "posix_translation/socket_util.h"
#include "posix_translation/time_util.h"
#include "posix_translation/virtual_file_system.h"
#include "ppapi/c/pp_errors.h"
#include "ppapi/cpp/module.h"
#include "ppapi/cpp/net_address.h"
#include "ppapi/cpp/udp_socket.h"
#include "ppapi/cpp/var.h"

namespace posix_translation {
namespace {

// The minimum address length for AF_UNSPEC.
const socklen_t kUnspecMinAddrLen =
    offsetof(sockaddr, sa_family) + sizeof(sa_family_t);

// Sets socket option to the given socket. Returns whether it succeeds.
bool SetSocketOption(pp::UDPSocket* socket,
                     PP_UDPSocket_Option name,
                     const pp::Var& value) {
  int32_t pp_error ALLOW_UNUSED;
  {
    base::AutoUnlock unlock(
        VirtualFileSystem::GetVirtualFileSystem()->mutex());
    pp_error = socket->SetOption(name, value, pp::BlockUntilComplete());
  }
  ARC_STRACE_REPORT_PP_ERROR(pp_error);

  // We should handle errors as follows:
  // if (pp_error != PP_OK) {
  //   errno = ENOPROTOOPT;  // TODO(crbug.com/358932): Pick correct errno.
  //   return false;
  // }
  // However, failing of some options causes JDWP (Java Debug Wired Protocol)
  // to fail during setup of listening socket. So, here now we just ignore
  // errors.
  // TODO(crbug.com/233914): Fix this problem.
  // TODO(crbug.com/362763): One of the typical case that PPAPI call fails is
  // invoking SO_REUSEADDR after bind(). PPAPI should support this case, too.

  return true;
}

// Common implementation of setsockopt with boolean value for UDP socket, such
// as SO_REUSEADDR or SO_BROADCAST.
// Note that the type of storage is int rather than bool, because it stores
// the given value as is.
int SetSocketBooleanOptionInternal(
    const void* optval, socklen_t optlen, int* storage,
    pp::UDPSocket* socket, PP_UDPSocket_Option name) {
  int error =
      internal::VerifySetSocketOption(optval, optlen, sizeof(*storage));
  if (error) {
    errno = error;
    return -1;
  }

  int new_value = *static_cast<const int*>(optval);
  // Compare as boolean values and call PPAPI only when the new value is
  // different from the old one as boolean.
  // For example, assuming setsockopt(SO_REUSEADDR, 1) is already called,
  // then setsockopt(SO_REUSEADDR, 2) would not need to call PPAPI, because
  // "REUSEADDR" is already true in PPAPI layer. In this case, *storage == 1
  // and value == 2, and both are evaluated to true.
  if ((new_value != 0) != (*storage != 0)) {
    if (!SetSocketOption(socket, name, pp::Var(new_value ? true : false)))
      return -1;
  }
  // PPAPI call successfully done. Update the value.
  *storage = new_value;
  return 0;
}

}  // namespace

// Thin wrapper of pp::UDPSocket. This is introduced to manage the lifetime of
// pp::UDPSocket instance correctly, and resolve race condition.
// The concept of this class is as same as TCPSocket::SocketWrapper. Please
// see also its comment for more details.
class UDPSocket::SocketWrapper
    : public base::RefCountedThreadSafe<SocketWrapper> {
 public:
  explicit SocketWrapper(const pp::UDPSocket& socket)
      : socket_(socket),
        closed_(false) {
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

  pp::UDPSocket* socket() {
    return &socket_;
  }

 private:
  // Do not allow to destruct this class manually from the client code
  // to avoid to delete the object accidentally while there are still
  // references to it.
  friend class base::RefCountedThreadSafe<SocketWrapper>;
  ~SocketWrapper() {
  }

  pp::UDPSocket socket_;
  bool closed_;

  DISALLOW_COPY_AND_ASSIGN(SocketWrapper);
};

UDPSocket::UDPSocket(int fd, int socket_family, int oflag)
    : SocketStream(socket_family, oflag), fd_(fd), factory_(this),
      socket_(new SocketWrapper(pp::UDPSocket(
          VirtualFileSystem::GetVirtualFileSystem()->instance()))),
      state_(UDP_SOCKET_NEW), read_buf_(kBufSize),
      read_sent_(false), write_sent_(false) {
  ALOG_ASSERT(socket_family == AF_INET || socket_family == AF_INET6);
  memset(&connected_addr_, 0, sizeof(connected_addr_));
  connected_addr_.ss_family = AF_UNSPEC;
}

UDPSocket::~UDPSocket() {
  ALOG_ASSERT(socket_->is_closed());
}

int UDPSocket::bind(const sockaddr* saddr, socklen_t addrlen) {
  int error =
      internal::VerifyInputSocketAddress(saddr, addrlen, socket_family_);
  if (error) {
    errno = error;
    return -1;
  }

  if (state_ != UDP_SOCKET_NEW) {
    errno = EISCONN;
    return -1;
  }

  VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
  pp::NetAddress addr = internal::SockAddrToNetAddress(sys->instance(), saddr);

  ALOGI("UDPSocket::Bind: %d %s\n",
        fd_, addr.DescribeAsString(true).AsString().c_str());
  scoped_refptr<SocketWrapper> wrapper(socket_);
  int32_t result;
  state_ = UDP_SOCKET_BINDING;
  {
    base::AutoUnlock unlock(sys->mutex());
    result = wrapper->socket()->Bind(addr, pp::BlockUntilComplete());
  }
  ARC_STRACE_REPORT_PP_ERROR(result);
  // Check close state before accessing any member variables since this
  // instance might be destroyed while this thread was waiting.
  if (wrapper->is_closed()) {
    errno = EBADF;
    return -1;
  }

  if (result != PP_OK) {
    state_ = UDP_SOCKET_NEW;
    if (result == PP_ERROR_ADDRESS_IN_USE) {
      errno = EADDRINUSE;
    } else {
      // We expect PP_ERROR_NOACCESS, but it may be different (unknown) value.
      // In either case, we return EACCES error.
      errno = EACCES;
    }
    return -1;
  }

  // Exception state is (wrongly) changed, so notify listeners about it.
  sys->Broadcast();
  NotifyListeners();

  state_ = UDP_SOCKET_BOUND;
  PostReadTaskLocked();
  return 0;
}

int UDPSocket::connect(const sockaddr* addr, socklen_t addrlen) {
  int error =
      internal::VerifyInputSocketAddress(addr, addrlen, socket_family_);
  if (error) {
    // There is an exception for connect() of UDP socket.
    // If the addr is AF_UNSPEC, it means we should clear the connect state.
    if (!addr || addrlen < kUnspecMinAddrLen || addr->sa_family != AF_UNSPEC) {
      errno = error;
      return -1;
    }

    // Reset the connected state.
    memset(&connected_addr_, 0, sizeof(connected_addr_));
    connected_addr_.ss_family = AF_UNSPEC;
    return 0;
  }

  memset(&connected_addr_, 0, sizeof(connected_addr_));
  // It is ensured that addr can be copied into sockaddr_storage, by
  // VerifyInputSocketAddress above.
  memcpy(&connected_addr_, addr, addrlen);
  return 0;
}

int UDPSocket::setsockopt(
    int level, int optname, const void* optval, socklen_t optlen) {
  // For SO_REUSEADDR and SO_BROADCAST, it is necessary to communicate with
  // PPAPI.
  if (level == SOL_SOCKET && optname == SO_REUSEADDR) {
    return SetSocketBooleanOptionInternal(
        optval, optlen, &reuse_addr_,
        socket_->socket(), PP_UDPSOCKET_OPTION_ADDRESS_REUSE);
  }

  if (level == SOL_SOCKET && optname == SO_BROADCAST) {
    return SetSocketBooleanOptionInternal(
        optval, optlen, &broadcast_,
        socket_->socket(), PP_UDPSOCKET_OPTION_BROADCAST);
  }

  return SocketStream::setsockopt(level, optname, optval, optlen);
}

int UDPSocket::getpeername(sockaddr* name, socklen_t* namelen) {
  int error = internal::VerifyOutputSocketAddress(name, namelen);
  if (error) {
    errno = error;
    return -1;
  }
  if (connected_addr_.ss_family == AF_UNSPEC) {
    errno = ENOTCONN;
    return -1;
  }
  internal::CopySocketAddress(connected_addr_, name, namelen);
  return 0;
}

int UDPSocket::getsockname(sockaddr* name, socklen_t* namelen) {
  int error = internal::VerifyOutputSocketAddress(name, namelen);
  if (error) {
    errno = error;
    return -1;
  }

  sockaddr_storage storage;
  if (!internal::NetAddressToSockAddrStorage(
          socket_->socket()->GetBoundAddress(), AF_UNSPEC, false, &storage)) {
    memset(&storage, 0, sizeof(storage));
    storage.ss_family = socket_family_;
  }

  internal::CopySocketAddress(storage, name, namelen);
  return 0;
}

ssize_t UDPSocket::send(const void* buf, size_t len, int flags) {
  return sendto(buf, len, flags, NULL, 0);
}

ssize_t UDPSocket::sendto(const void* buf, size_t len, int flags,
                          const sockaddr* dest_addr, socklen_t addrlen) {
  if (dest_addr == NULL) {
    // Callers are allowed to pass a NULL dest_addr if the socket is connected,
    // using the previously-connected address as the destination. However,
    // trying this when not connected is an error.
    if (connected_addr_.ss_family == AF_UNSPEC) {
      errno = EDESTADDRREQ;
      return -1;
    }

    dest_addr = reinterpret_cast<const sockaddr*>(&connected_addr_);
    addrlen = sizeof(connected_addr_);
  }

  int error =
      internal::VerifyInputSocketAddress(dest_addr, addrlen, socket_family_);
  if (error) {
    errno = error;
    return -1;
  }

  if (state_ == UDP_SOCKET_NEW) {
    // UDP sockets allow to send data without bind but Pepper requires bind
    // before send/receive so bind it to any address now.
    sockaddr_storage saddr = {};
    saddr.ss_family = socket_family_;
    if (this->bind(reinterpret_cast<sockaddr*>(&saddr), sizeof(saddr))) {
      // On error, errno is set in bind.
      return -1;
    }
  }

  // IPv4 packet has 16-bit packet length field. So, the max UDP packet size
  // which can be represented is:
  //   (64K - 1) - 8 (UDP packet header) - 20 (IPv4 packet header).
  const size_t kMaxIPv4UDPPacketSize =
      std::numeric_limits<uint16_t>::max() - sizeof(udphdr) - sizeof(iphdr);

  // IPv6 packet has 16-bit payload length field, which do not include the size
  // of IP packet header unlike IPv4 packet. So, the max size which can be
  // represented is to UDP packet's size field, which is 16-bit:
  //   (64K - 1) - 8 (UDP packet header).
  const size_t kMaxIPv6UDPPacketSize =
      std::numeric_limits<uint16_t>::max() - sizeof(udphdr);

  const size_t kMaxUDPPacketSize =
      socket_family_ == AF_INET ? kMaxIPv4UDPPacketSize : kMaxIPv6UDPPacketSize;
  if (len > kMaxUDPPacketSize) {
    errno = EMSGSIZE;
    return -1;
  }

  out_queue_.push_back(Message());
  Message* message = &out_queue_.back();
  memcpy(&message->addr, dest_addr, addrlen);
  message->data.assign(static_cast<const char*>(buf),
                       static_cast<const char*>(buf) + len);
  PostWriteTaskLocked();

  if (is_block()) {
    scoped_refptr<SocketWrapper> wrapper(socket_);
    VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
    while (!out_queue_.empty()) {
      sys->Wait();
      // Check close state before accessing any member variables since this
      // instance might be destroyed while this thread was waiting.
      if (wrapper->is_closed()) {
        errno = EBADF;
        return -1;
      }
    }
  }

  // We should handle errors here properly, at least we should handle
  // EMSGSIZE. Otherwise callers have no way to know if the packet is
  // too large or not.
  // TODO(crbug.com/364744): Handle errors.
  return static_cast<ssize_t>(len);
}

ssize_t UDPSocket::recv(void* buffer, size_t len, int flags) {
  if (connected_addr_.ss_family == AF_UNSPEC) {
    errno = ENOTCONN;
    return -1;
  }
  return recvfrom(buffer, len, flags, NULL, NULL);
}

ssize_t UDPSocket::recvfrom(void* buffer, size_t len, int flags,
                            sockaddr* addr, socklen_t* addrlen) {
  if (is_block()) {
    scoped_refptr<SocketWrapper> wrapper(socket_);
    VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
    const base::TimeTicks time_limit =
        internal::TimeOutToTimeLimit(recv_timeout_);
    bool is_timedout = false;
    while (!is_timedout && in_queue_.empty()) {
      is_timedout = sys->WaitUntil(time_limit);
      // Check close state before accessing any member variables since this
      // instance might be destroyed while this thread was waiting.
      if (wrapper->is_closed()) {
        errno = EBADF;
        return -1;
      }
    }
  }

  if (in_queue_.empty()) {
    errno = EAGAIN;
    return -1;
  }

  // Message may be discarded below, so limit the scope in order to avoid
  // illegal access.
  {
    const Message& message = in_queue_.front();
    if (addrlen != NULL && addr != NULL)
      internal::CopySocketAddress(message.addr, addr, addrlen);
    len = std::min(len, message.data.size());
    memcpy(buffer, &message.data[0], len);
  }
  if ((flags & MSG_PEEK) == 0)
    in_queue_.pop_front();

  PostReadTaskLocked();
  return len;
}

ssize_t UDPSocket::read(void* buf, size_t count) {
  return recv(buf, count, 0);
}

ssize_t UDPSocket::write(const void* buf, size_t count) {
  return send(buf, count, 0);
}

bool UDPSocket::IsSelectReadReady() const {
  return socket_->is_closed() || !in_queue_.empty();
}

bool UDPSocket::IsSelectWriteReady() const {
  return true;
}

bool UDPSocket::IsSelectExceptionReady() const {
  // TODO(crbug.com:359400): Fix the select() and poll() implementaiton.
  // See the bug for details.
  return socket_->is_closed();
}

int16_t UDPSocket::GetPollEvents() const {
  // Currently we use IsSelect*Ready() family temporarily (and wrongly).
  // TODO(crbug.com/359400): Fix the implementation.
  return ((IsSelectReadReady() ? POLLIN : 0) |
          (IsSelectWriteReady() ? POLLOUT : 0) |
          (IsSelectExceptionReady() ? POLLERR : 0));
}

void UDPSocket::OnLastFileRef() {
  ALOG_ASSERT(!socket_->is_closed());
  CloseLocked();
}

void UDPSocket::CloseLocked() {
  int32_t result = PP_OK_COMPLETIONPENDING;
  pp::Module::Get()->core()->CallOnMainThread(
      0, factory_.NewCallback(&UDPSocket::Close, &result));
  VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
  while (result == PP_OK_COMPLETIONPENDING)
    sys->Wait();
  ARC_STRACE_REPORT_PP_ERROR(result);
}

void UDPSocket::Close(int32_t result, int32_t* pres) {
  ALOG_ASSERT(result == PP_OK);
  VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
  base::AutoLock lock(sys->mutex());

  factory_.CancelAll();
  socket_->Close();
  *pres = PP_OK;
  // Don't access any member variable after sys->Broadcast() is called.
  // It may make destructor have completed.
  NotifyListeners();
  sys->Broadcast();
}

void UDPSocket::Read(int32_t result) {
  ALOG_ASSERT(result == PP_OK);
  VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
  base::AutoLock lock(sys->mutex());
  ReadLocked();
}

void UDPSocket::ReadLocked() {
  VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
  sys->mutex().AssertAcquired();

  int32_t result = socket_->socket()->RecvFrom(
      &read_buf_[0], read_buf_.size(),
      factory_.NewCallbackWithOutput(&UDPSocket::OnRead));
  ALOG_ASSERT(result == PP_OK_COMPLETIONPENDING);
}

void UDPSocket::OnRead(int32_t result, const pp::NetAddress& addr) {
  VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
  base::AutoLock lock(sys->mutex());
  read_sent_ = false;

  if (result < 0) {
    return;
  }

  ALOGI("UDPSocket::OnRead: %d %s",
        fd_, addr.DescribeAsString(true).AsString().c_str());

  sockaddr_storage src_addr;
  internal::NetAddressToSockAddrStorage(addr, AF_UNSPEC, false, &src_addr);
  if (connected_addr_.ss_family != AF_UNSPEC &&
      !internal::SocketAddressEqual(connected_addr_, src_addr)) {
    // Packet from address other than our connected address. So we
    // merely drop the packet on the floor.
    PostReadTaskLocked();
    return;
  }

  in_queue_.push_back(Message());
  Message* message = &in_queue_.back();
  memcpy(&message->addr, &src_addr, sizeof(src_addr));
  message->data.assign(read_buf_.begin(), read_buf_.begin() + result);

  PostReadTaskLocked();

  sys->Broadcast();
  NotifyListeners();
}

void UDPSocket::Write(int32_t result) {
  ALOG_ASSERT(result == PP_OK);
  VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
  base::AutoLock lock(sys->mutex());
  WriteLocked();
}

void UDPSocket::WriteLocked() {
  VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
  sys->mutex().AssertAcquired();

  ALOG_ASSERT(!out_queue_.empty());

  pp::NetAddress addr = internal::SockAddrToNetAddress(
      sys->instance(),
      reinterpret_cast<const sockaddr*>(&out_queue_.front().addr));
  ALOGI("UDPSocket::Write: %d %s",
        fd_, addr.DescribeAsString(true).AsString().c_str());

  const Message& message = out_queue_.front();
  int32_t result = socket_->socket()->SendTo(
      &message.data[0], message.data.size(), addr,
      factory_.NewCallback(&UDPSocket::OnWrite));
  ALOG_ASSERT(result == PP_OK_COMPLETIONPENDING);
}

void UDPSocket::OnWrite(int32_t result) {
  VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
  base::AutoLock lock(sys->mutex());

  write_sent_ = false;
  if (result < 0) {
    // Write error.
    ALOGI("UDPSocket::OnWrite: write error on %d, result: %d", fd_, result);
  } else {
    // We do not expect partial write. Sent data may be truncated in PPAPI
    // layer if it is too large, but the limit size is currently much bigger
    // than the common MTU (Maximum Transmission Unit). In lower layer,
    // UDP socket communication will fail if the size is bigger than MTU,
    // rather than partial write.
    // Thus, partial write will not happen here.
    ALOG_ASSERT(static_cast<size_t>(result) == out_queue_.front().data.size());
  }

  out_queue_.pop_front();
  sys->Broadcast();
  NotifyListeners();

  // Always try to send more if there are some pending items.
  PostWriteTaskLocked();
}

void UDPSocket::PostReadTaskLocked() {
  VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
  sys->mutex().AssertAcquired();

  if (read_sent_ || in_queue_.size() >= kQueueSize) {
    // If there is in-flight Read() task, or the reading queue is already
    // full, do not send the Read() task.
    return;
  }

  read_sent_ = true;
  if (!pp::Module::Get()->core()->IsMainThread()) {
    pp::Module::Get()->core()->CallOnMainThread(
        0, factory_.NewCallback(&UDPSocket::Read));
  } else {
    // If on main Pepper thread and delay is not required call it directly.
    ReadLocked();
  }
}

void UDPSocket::PostWriteTaskLocked() {
  VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
  sys->mutex().AssertAcquired();

  if (write_sent_ || out_queue_.empty()) {
    // If there is in-flight Write() task, or there is no write message,
    // do not send the Write() task.
    return;
  }

  write_sent_ = true;
  if (!pp::Module::Get()->core()->IsMainThread()) {
    pp::Module::Get()->core()->CallOnMainThread(
        0, factory_.NewCallback(&UDPSocket::Write));
  } else {
    // If on main Pepper thread and delay is not required call it directly.
    WriteLocked();
  }
}

const char* UDPSocket::GetStreamType() const {
  return "udp";
}

}  // namespace posix_translation
