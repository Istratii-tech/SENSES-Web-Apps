// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POSIX_TRANSLATION_TCP_SOCKET_H_
#define POSIX_TRANSLATION_TCP_SOCKET_H_

#include <fcntl.h>

#include <string>
#include <vector>

#include "base/basictypes.h"
#include "base/compiler_specific.h"
#include "base/memory/scoped_ptr.h"
#include "posix_translation/socket_stream.h"
#include "ppapi/cpp/completion_callback.h"
#include "ppapi/cpp/tcp_socket.h"
#include "ppapi/utility/completion_callback_factory.h"

namespace pp {
class NetAddress;
}  // namespace pp

namespace posix_translation {

class TCPSocket : public SocketStream {
 public:
  TCPSocket(int fd, int socket_family, int oflag);

  virtual int bind(const sockaddr* addr, socklen_t addrlen) OVERRIDE;
  virtual int listen(int backlog) OVERRIDE;
  virtual int accept(sockaddr* addr, socklen_t* addrlen) OVERRIDE;
  virtual int connect(const sockaddr* addr, socklen_t addrlen) OVERRIDE;

  virtual off64_t lseek(off64_t offset, int whence) OVERRIDE;
  virtual ssize_t read(void* buf, size_t count) OVERRIDE;
  virtual ssize_t recv(void* buf, size_t len, int flags) OVERRIDE;
  virtual ssize_t recvfrom(void* buf, size_t len, int flags, sockaddr* addr,
                           socklen_t* addrlen) OVERRIDE;
  virtual int recvmsg(struct msghdr* msg, int flags) OVERRIDE;
  virtual ssize_t write(const void* buf, size_t count) OVERRIDE;
  virtual ssize_t send(const void* buf, size_t len, int flags) OVERRIDE;
  virtual ssize_t sendto(const void* buf, size_t len, int flags,
                         const sockaddr* dest_addr, socklen_t addrlen) OVERRIDE;
  virtual int sendmsg(const struct msghdr* msg, int flags) OVERRIDE;

  virtual int ioctl(int request, va_list ap) OVERRIDE;

  virtual int setsockopt(int level, int optname, const void* optval,
                         socklen_t optlen) OVERRIDE;
  virtual int getpeername(sockaddr* name, socklen_t* namelen) OVERRIDE;
  virtual int getsockname(sockaddr* name, socklen_t* namelen) OVERRIDE;

  virtual bool IsSelectReadReady() const OVERRIDE;
  virtual bool IsSelectWriteReady() const OVERRIDE;
  virtual bool IsSelectExceptionReady() const OVERRIDE;
  virtual int16_t GetPollEvents() const OVERRIDE;

  virtual const char* GetStreamType() const OVERRIDE;

 protected:
  virtual ~TCPSocket();
  virtual bool GetOptNameData(int level, int optname, socklen_t* len,
                              void** storage, const void* user_data,
                              socklen_t user_data_len) OVERRIDE;
  virtual void OnLastFileRef() OVERRIDE;

 private:
  friend class PepperTCPSocketTest;

  class SocketWrapper;

  enum ConnectState {
    TCP_SOCKET_NEW,
    TCP_SOCKET_CONNECTING,
    TCP_SOCKET_CONNECTED,
    TCP_SOCKET_LISTENING,
    TCP_SOCKET_ERROR,
  };

  // This is a constructor to create a TCPSocket for accepting a connection.
  // TODO(hidehiko): Unify this overloaded constructor with the one declared
  // as public above.
  explicit TCPSocket(const pp::TCPSocket& socket);

  bool is_block() const { return !(oflag() & O_NONBLOCK); }

  bool is_connected() const {
    return connect_state_ == TCP_SOCKET_CONNECTED;
  }

  // Returns true if the socket is already closed, or an error has occured
  // before (or on background task).
  bool IsTerminated() const;

  void MarkAsErrorLocked(int error);

  void PostReadTaskLocked();

  void Accept(int32_t result);
  void OnAccept(int32_t result, const pp::TCPSocket& socket);

  void Connect(int32_t result, const pp::NetAddress& address);
  void OnConnect(int32_t result);

  void Read(int32_t result);
  void ReadLocked();
  void OnRead(int32_t result);
  void OnReadLocked(int32_t result);

  void Write(int32_t result);
  void WriteLocked();
  void OnWrite(int32_t result);

  void CloseLocked();
  void Close(int32_t result, int32_t* pres);

  static const size_t kBufSize = 64 * 1024;

  int fd_;
  std::string hostname_;
  pp::CompletionCallbackFactory<TCPSocket> factory_;
  scoped_refptr<SocketWrapper> socket_;
  std::vector<char> in_buf_;
  std::vector<char> out_buf_;
  std::vector<char> read_buf_;
  std::vector<char> write_buf_;
  ConnectState connect_state_;
  bool eof_;
  bool read_sent_;
  bool write_sent_;
  int connect_error_;

  // The socket accepted on background, which will be returned when accept()
  // is called.
  pp::TCPSocket accepted_socket_;

  // Storage for TCP_NODELAY's optval. This is int, rather than bool, to keep
  // the value passed via setsockopt as is.
  int no_delay_;

  DISALLOW_COPY_AND_ASSIGN(TCPSocket);
};

}  // namespace posix_translation

#endif  // POSIX_TRANSLATION_TCP_SOCKET_H_
