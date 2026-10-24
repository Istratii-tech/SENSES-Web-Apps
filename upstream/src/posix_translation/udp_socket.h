// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POSIX_TRANSLATION_UDP_SOCKET_H_
#define POSIX_TRANSLATION_UDP_SOCKET_H_

#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <deque>
#include <utility>
#include <vector>

#include "base/basictypes.h"
#include "base/compiler_specific.h"
#include "base/memory/ref_counted.h"
#include "posix_translation/socket_stream.h"
#include "ppapi/cpp/completion_callback.h"
#include "ppapi/utility/completion_callback_factory.h"

namespace pp {
class NetAddress;
}  // namespace pp

namespace posix_translation {

class UDPSocket : public SocketStream {
 public:
  UDPSocket(int fd, int socket_family, int oflag);

  virtual int bind(const sockaddr* saddr, socklen_t addrlen) OVERRIDE;
  virtual int connect(const sockaddr* addr, socklen_t addrlen) OVERRIDE;
  virtual int setsockopt(
      int level, int optname, const void* optval, socklen_t optlen) OVERRIDE;
  virtual int getpeername(sockaddr* name, socklen_t* namelen) OVERRIDE;
  virtual int getsockname(sockaddr* name, socklen_t* namelen) OVERRIDE;
  virtual ssize_t send(const void* buf, size_t len, int flags) OVERRIDE;
  virtual ssize_t sendto(const void* buf, size_t len, int flags,
                         const sockaddr* dest_addr, socklen_t addrlen) OVERRIDE;
  virtual ssize_t recv(void* buffer, size_t len, int flags) OVERRIDE;
  virtual ssize_t recvfrom(void* buffer, size_t len, int flags,
                           sockaddr* addr, socklen_t* addrlen) OVERRIDE;

  virtual ssize_t read(void* buf, size_t count) OVERRIDE;
  virtual ssize_t write(const void* buf, size_t count) OVERRIDE;

  virtual bool IsSelectReadReady() const OVERRIDE;
  virtual bool IsSelectWriteReady() const OVERRIDE;
  virtual bool IsSelectExceptionReady() const OVERRIDE;
  virtual int16_t GetPollEvents() const OVERRIDE;

  virtual const char* GetStreamType() const OVERRIDE;

 protected:
  virtual ~UDPSocket();
  virtual void OnLastFileRef() OVERRIDE;

 private:
  // A message unit which is sent to or received from the peer.
  // Note: in libcxx, deque implementation uses sizeof(T) in the inlined
  // initialization "const static" member, so we cannot use forward declaration
  // here. cf): android/external/libcxx/include/deque.
  struct Message {
    // The address where this message is being sent to or where the message
    // comes from.
    sockaddr_storage addr;

    // Sent or received data.
    std::vector<char> data;
  };
  typedef std::deque<Message> MessageQueue;
  class SocketWrapper;

  enum State {
    UDP_SOCKET_NEW,
    UDP_SOCKET_BINDING,
    UDP_SOCKET_BOUND,
  };

  bool is_block() { return !(oflag() & O_NONBLOCK); }

  void CloseLocked();
  void Close(int32_t result, int32_t* pres);

  void Read(int32_t result);
  void ReadLocked();
  void OnRead(int32_t result, const pp::NetAddress& addr);

  void Write(int32_t result);
  void WriteLocked();
  void OnWrite(int32_t result);

  void PostReadTaskLocked();
  void PostWriteTaskLocked();

  // Number of messages in incoming queue that we can read ahead.
  static const size_t kQueueSize = 16;

  // Read buffer size for incoming message.
  static const size_t kBufSize = 64 * 1024;

  int fd_;
  pp::CompletionCallbackFactory<UDPSocket> factory_;
  scoped_refptr<SocketWrapper> socket_;
  State state_;
  MessageQueue in_queue_;
  MessageQueue out_queue_;
  std::vector<char> read_buf_;
  bool read_sent_;
  bool write_sent_;
  struct sockaddr_storage connected_addr_;

  DISALLOW_COPY_AND_ASSIGN(UDPSocket);
};

}  // namespace posix_translation

#endif  // POSIX_TRANSLATION_UDP_SOCKET_H_
