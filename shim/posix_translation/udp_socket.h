// Overrides ARC's posix_translation/udp_socket.h (Pepper UDPSocket backend).
//
// AF_INET/AF_INET6 datagram sockets over the net bridge (net_bridge.h). The
// transport socket is opened on the first bind(), connect() or sendto();
// it is bound to an ephemeral port of the relay's host, so bind() accepts
// any address with port 0 only. Received datagrams queue here (up to
// kMaxQueuedBytes; older ones are dropped beyond that, as a full socket
// buffer would). Part of the arc-wasm shim layer.
#ifndef POSIX_TRANSLATION_UDP_SOCKET_H_
#define POSIX_TRANSLATION_UDP_SOCKET_H_

#include <deque>
#include <map>
#include <string>
#include <utility>

#include "base/basictypes.h"
#include "posix_translation/net_bridge.h"
#include "posix_translation/socket_stream.h"

namespace posix_translation {

class UDPSocket : public SocketStream, public net::Endpoint {
 public:
  UDPSocket(int fd, int socket_family, int oflag);

  virtual int bind(const sockaddr* saddr, socklen_t addrlen) OVERRIDE;
  virtual int connect(const sockaddr* addr, socklen_t addrlen) OVERRIDE;
  virtual int getpeername(sockaddr* name, socklen_t* namelen) OVERRIDE;
  virtual int getsockname(sockaddr* name, socklen_t* namelen) OVERRIDE;
  virtual ssize_t send(const void* buf, size_t len, int flags) OVERRIDE;
  virtual ssize_t sendto(const void* buf, size_t len, int flags,
                         const sockaddr* dest_addr, socklen_t addrlen) OVERRIDE;
  virtual int sendmsg(const struct msghdr* msg, int flags) OVERRIDE;
  virtual ssize_t recv(void* buffer, size_t len, int flags) OVERRIDE;
  virtual ssize_t recvfrom(void* buffer, size_t len, int flags,
                           sockaddr* addr, socklen_t* addrlen) OVERRIDE;
  virtual int recvmsg(struct msghdr* msg, int flags) OVERRIDE;
  virtual ssize_t read(void* buf, size_t count) OVERRIDE;
  virtual ssize_t write(const void* buf, size_t count) OVERRIDE;
  virtual int ioctl(int request, va_list ap) OVERRIDE;
  virtual int getsockopt(int level, int optname, void* optval,
                         socklen_t* optlen) OVERRIDE;
  virtual int setsockopt(int level, int optname, const void* optval,
                         socklen_t optlen) OVERRIDE;

  virtual bool IsSelectReadReady() const OVERRIDE;
  virtual bool IsSelectWriteReady() const OVERRIDE;
  virtual bool IsSelectExceptionReady() const OVERRIDE;
  virtual int16_t GetPollEvents() const OVERRIDE;

  virtual const char* GetStreamType() const OVERRIDE;

  // net::Endpoint
  virtual void OnOpen(const sockaddr_storage& local) OVERRIDE;
  virtual void OnOpenFailed(int error) OVERRIDE;
  virtual void OnData(const char* data, size_t len,
                      const sockaddr_storage& from) OVERRIDE;
  virtual void OnEof() OVERRIDE;
  virtual void OnError(int error) OVERRIDE;
  virtual void OnSent(size_t len) OVERRIDE;

  static const size_t kMaxQueuedBytes = 1024 * 1024;

 protected:
  virtual ~UDPSocket();
  virtual void OnLastFileRef() OVERRIDE;

 private:
  struct Datagram {
    sockaddr_storage from;
    std::string data;
  };

  // Opens the transport socket if needed; returns 0 or an errno.
  int EnsureOpen();
  bool FromPeer(const sockaddr_storage& from) const;

  int id_;
  bool open_;
  bool closed_;
  int error_state_;
  bool connected_;
  sockaddr_storage peer_;
  sockaddr_storage local_;
  std::deque<Datagram> queue_;
  size_t queued_bytes_;
  std::map<std::pair<int, int>, int> int_options_;

  DISALLOW_COPY_AND_ASSIGN(UDPSocket);
};

}  // namespace posix_translation

#endif  // POSIX_TRANSLATION_UDP_SOCKET_H_
