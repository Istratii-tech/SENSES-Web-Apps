// Overrides ARC's posix_translation/tcp_socket.h (Pepper TCPSocket backend).
//
// AF_INET/AF_INET6 stream sockets over the net bridge (net_bridge.h): the
// connection itself lives on the net thread (a WebSocket to the relay, or a
// Node socket); this class keeps the receive buffer and the state, and
// implements blocking, non-blocking (EINPROGRESS + poll + SO_ERROR) and
// timed (SO_RCVTIMEO/SO_SNDTIMEO) operation on top of the VFS condition
// variable. Servers work on loopback only: listen() registers the port in
// this process, and a connect() to 127.0.0.1/::1 at that port becomes an
// in-process pair of sockets (nothing from outside the browser can connect;
// a loopback port without a listener here goes to the relay host's own
// loopback, as any other address).
// Part of the arc-wasm shim layer.
#ifndef POSIX_TRANSLATION_TCP_SOCKET_H_
#define POSIX_TRANSLATION_TCP_SOCKET_H_

#include <deque>
#include <map>
#include <string>
#include <utility>

#include "base/basictypes.h"
#include "base/memory/ref_counted.h"
#include "posix_translation/net_bridge.h"
#include "posix_translation/socket_stream.h"

namespace posix_translation {

class TCPSocket : public SocketStream, public net::Endpoint {
 public:
  TCPSocket(int fd, int socket_family, int oflag);

  virtual int bind(const sockaddr* addr, socklen_t addrlen) OVERRIDE;
  virtual int listen(int backlog) OVERRIDE;
  virtual int accept(sockaddr* addr, socklen_t* addrlen) OVERRIDE;
  virtual int connect(const sockaddr* addr, socklen_t addrlen) OVERRIDE;
  // shutdown(2); called by the kernel (arc_kernel.cc) with the VFS mutex held.
  int Shutdown(int how);

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
  virtual int getsockopt(int level, int optname, void* optval,
                         socklen_t* optlen) OVERRIDE;
  virtual int setsockopt(int level, int optname, const void* optval,
                         socklen_t optlen) OVERRIDE;
  virtual int getpeername(sockaddr* name, socklen_t* namelen) OVERRIDE;
  virtual int getsockname(sockaddr* name, socklen_t* namelen) OVERRIDE;

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

  // Bytes sent but not yet flushed by the transport beyond which send()
  // blocks (or fails with EAGAIN).
  static const size_t kSendBufferSize = 256 * 1024;

 protected:
  virtual ~TCPSocket();
  virtual void OnLastFileRef() OVERRIDE;

 private:
  enum State { kNew, kConnecting, kConnected, kListening, kError, kClosed };

  bool is_block(int flags) const;
  size_t available() const { return in_buf_.size() - in_pos_; }
  // Waits on the VFS condition variable until |ready| holds, the socket is
  // closed or |timeout| (zero: none) expires. Returns false on timeout.
  template <typename Ready>
  bool WaitFor(Ready ready, const base::TimeDelta& timeout);
  void SetError(int error);
  void Disconnect();
  // Bytes send() may queue now.
  size_t SendSpace() const;
  // Loopback: connects to |listener| in this process.
  void ConnectLoopback(TCPSocket* listener);
  // Loopback: ends the pairing; the peer reads EOF and cannot write.
  void DetachLoopback();
  void ReleasePort();

  State state_;
  int id_;  // net bridge connection, 0 if none
  std::string in_buf_;
  size_t in_pos_;
  size_t unsent_;
  bool eof_;
  bool shut_rd_;
  bool shut_wr_;
  int last_error_;
  sockaddr_storage peer_;
  sockaddr_storage local_;
  // Loopback (see the top): the port from bind()/listen(), the other end of
  // an in-process connection and, when listening, the connections not
  // accept()ed yet.
  int port_;
  bool loopback_;
  TCPSocket* loop_peer_;
  std::deque<scoped_refptr<TCPSocket> > accept_queue_;
  size_t backlog_;
  // Accepted-and-ignored integer options (TCP_NODELAY, SO_KEEPALIVE, ...).
  std::map<std::pair<int, int>, int> int_options_;

  DISALLOW_COPY_AND_ASSIGN(TCPSocket);
};

}  // namespace posix_translation

#endif  // POSIX_TRANSLATION_TCP_SOCKET_H_
