// TCPSocket and UDPSocket over the net bridge (see tcp_socket.h,
// udp_socket.h and net_bridge.h). Every method runs with the VFS mutex held;
// blocking ones wait on the VFS condition variable, which the net thread
// broadcasts after each event.

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include <algorithm>
#include <set>

#include "common/alog.h"
#include "posix_translation/socket_util.h"
#include "posix_translation/tcp_socket.h"
#include "posix_translation/time_util.h"
#include "posix_translation/udp_socket.h"
#include "posix_translation/virtual_file_system.h"

namespace posix_translation {

namespace {

VirtualFileSystem* Sys() { return VirtualFileSystem::GetVirtualFileSystem(); }

// Starts the net thread without holding the VFS mutex (starting a thread
// must not wait on anything the mutex protects).
bool StartNet() {
  base::AutoUnlock unlock(Sys()->mutex());
  return net::Start();
}

int Fail(int error) {
  errno = error;
  return -1;
}

void SetUnspecified(int family, sockaddr_storage* addr) {
  memset(addr, 0, sizeof(*addr));
  addr->ss_family = family;
}

socklen_t AddressLength(const sockaddr_storage& addr) {
  return addr.ss_family == AF_INET6 ? sizeof(sockaddr_in6)
                                    : sizeof(sockaddr_in);
}

// Converts |addr| for a socket of |family|: IPv4 addresses become
// v4-mapped IPv6 ones on AF_INET6 sockets, as Linux reports them.
sockaddr_storage ForFamily(const sockaddr_storage& addr, int family) {
  if (family != AF_INET6 || addr.ss_family != AF_INET)
    return addr;
  const sockaddr_in* sin = reinterpret_cast<const sockaddr_in*>(&addr);
  sockaddr_storage out;
  memset(&out, 0, sizeof(out));
  sockaddr_in6* sin6 = reinterpret_cast<sockaddr_in6*>(&out);
  sin6->sin6_family = AF_INET6;
  sin6->sin6_port = sin->sin_port;
  sin6->sin6_addr.s6_addr[10] = 0xff;
  sin6->sin6_addr.s6_addr[11] = 0xff;
  memcpy(&sin6->sin6_addr.s6_addr[12], &sin->sin_addr, 4);
  return out;
}

// Normalizes to the bare IPv4 form when |addr| is v4-mapped.
sockaddr_storage Canonical(const sockaddr_storage& addr) {
  if (addr.ss_family != AF_INET6)
    return addr;
  const sockaddr_in6* sin6 = reinterpret_cast<const sockaddr_in6*>(&addr);
  if (!IN6_IS_ADDR_V4MAPPED(&sin6->sin6_addr))
    return addr;
  sockaddr_storage out;
  memset(&out, 0, sizeof(out));
  sockaddr_in* sin = reinterpret_cast<sockaddr_in*>(&out);
  sin->sin_family = AF_INET;
  sin->sin_port = sin6->sin6_port;
  memcpy(&sin->sin_addr, &sin6->sin6_addr.s6_addr[12], 4);
  return out;
}

bool SameAddress(const sockaddr_storage& a, const sockaddr_storage& b) {
  const sockaddr_storage x = Canonical(a), y = Canonical(b);
  if (x.ss_family != y.ss_family)
    return false;
  if (x.ss_family == AF_INET) {
    const sockaddr_in* p = reinterpret_cast<const sockaddr_in*>(&x);
    const sockaddr_in* q = reinterpret_cast<const sockaddr_in*>(&y);
    return p->sin_port == q->sin_port &&
           p->sin_addr.s_addr == q->sin_addr.s_addr;
  }
  const sockaddr_in6* p = reinterpret_cast<const sockaddr_in6*>(&x);
  const sockaddr_in6* q = reinterpret_cast<const sockaddr_in6*>(&y);
  return p->sin6_port == q->sin6_port &&
         memcmp(&p->sin6_addr, &q->sin6_addr, 16) == 0;
}

// Copies |addr| (as seen by a socket of |family|) out through name/namelen.
int CopyOut(const sockaddr_storage& addr, int family, sockaddr* name,
            socklen_t* namelen) {
  const int error = internal::VerifyOutputSocketAddress(name, namelen);
  if (error)
    return Fail(error);
  internal::CopySocketAddress(ForFamily(addr, family), name, namelen);
  return 0;
}

bool IsAnyPortZero(const sockaddr* addr) {
  if (addr->sa_family == AF_INET) {
    const sockaddr_in* sin = reinterpret_cast<const sockaddr_in*>(addr);
    return sin->sin_addr.s_addr == htonl(INADDR_ANY) && sin->sin_port == 0;
  }
  const sockaddr_in6* sin6 = reinterpret_cast<const sockaddr_in6*>(addr);
  return IN6_IS_ADDR_UNSPECIFIED(&sin6->sin6_addr) && sin6->sin6_port == 0;
}

// Integer options that are accepted and remembered but have no effect:
// the relay's host socket keeps its own settings.
bool IsIgnoredIntOption(int level, int optname) {
  switch (level) {
    case SOL_SOCKET:
      return optname == SO_KEEPALIVE || optname == SO_REUSEADDR ||
             optname == SO_REUSEPORT || optname == SO_OOBINLINE ||
             optname == SO_RCVLOWAT || optname == SO_SNDLOWAT ||
             optname == SO_PRIORITY || optname == SO_DONTROUTE ||
             optname == SO_RCVBUF || optname == SO_SNDBUF ||
             optname == SO_BROADCAST || optname == SO_TIMESTAMP;
    case IPPROTO_TCP:
      return optname == TCP_NODELAY || optname == TCP_KEEPIDLE ||
             optname == TCP_KEEPINTVL || optname == TCP_KEEPCNT ||
             optname == TCP_CORK || optname == TCP_QUICKACK ||
             optname == TCP_USER_TIMEOUT || optname == TCP_FASTOPEN ||
             optname == TCP_DEFER_ACCEPT || optname == TCP_MAXSEG;
    case IPPROTO_IP:
      return optname == IP_TOS || optname == IP_TTL ||
             optname == IP_MTU_DISCOVER || optname == IP_RECVERR ||
             optname == IP_MULTICAST_TTL || optname == IP_MULTICAST_LOOP ||
             optname == IP_PKTINFO || optname == IP_RECVTOS;
    case IPPROTO_IPV6:
      return optname == IPV6_V6ONLY || optname == IPV6_TCLASS ||
             optname == IPV6_UNICAST_HOPS || optname == IPV6_MULTICAST_HOPS ||
             optname == IPV6_MULTICAST_LOOP || optname == IPV6_RECVERR ||
             optname == IPV6_MTU_DISCOVER || optname == IPV6_RECVPKTINFO;
  }
  return false;
}

int DefaultIntOption(int level, int optname) {
  if (level == SOL_SOCKET && (optname == SO_RCVBUF || optname == SO_SNDBUF))
    return 256 * 1024;
  if (level == IPPROTO_IP && optname == IP_TTL)
    return 64;
  if (level == IPPROTO_IPV6 && optname == IPV6_UNICAST_HOPS)
    return 64;
  if (level == IPPROTO_TCP && optname == TCP_MAXSEG)
    return 1448;
  return 0;
}

// getsockopt()/setsockopt() for the options both socket kinds share.
// Returns 1 if handled (with the result in *result), 0 if not.
int CommonGetOption(int type, int family, int* pending_error,
                    const std::map<std::pair<int, int>, int>& options,
                    int level, int optname, void* optval, socklen_t* optlen,
                    int* result) {
  int value;
  if (level == SOL_SOCKET && optname == SO_ERROR) {
    value = *pending_error;
    *pending_error = 0;  // Reading SO_ERROR clears it, as on Linux.
  } else if (level == SOL_SOCKET && optname == SO_TYPE) {
    value = type;
  } else if (level == SOL_SOCKET && optname == SO_DOMAIN) {
    value = family;
  } else if (level == SOL_SOCKET && optname == SO_PROTOCOL) {
    value = type == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP;
  } else if (level == SOL_SOCKET && optname == SO_ACCEPTCONN) {
    value = 0;
  } else if (IsIgnoredIntOption(level, optname)) {
    std::map<std::pair<int, int>, int>::const_iterator it =
        options.find(std::make_pair(level, optname));
    value = it != options.end() ? it->second
                                : DefaultIntOption(level, optname);
  } else {
    return 0;
  }
  const int error = internal::VerifyGetSocketOption(optval, optlen);
  if (error) {
    *result = Fail(error);
    return 1;
  }
  internal::CopySocketOption(&value, sizeof(value), optval, optlen);
  *result = 0;
  return 1;
}

int CommonSetOption(std::map<std::pair<int, int>, int>* options, int level,
                    int optname, const void* optval, socklen_t optlen,
                    int* result) {
  if (!IsIgnoredIntOption(level, optname))
    return 0;
  if (!optval || optlen < sizeof(int)) {
    *result = Fail(optval ? EINVAL : EFAULT);
    return 1;
  }
  (*options)[std::make_pair(level, optname)] =
      *static_cast<const int*>(optval);
  *result = 0;
  return 1;
}

// Gathers/scatters msghdr iovecs through a flat buffer.
std::string Gather(const struct msghdr* msg) {
  std::string data;
  for (size_t i = 0; i < msg->msg_iovlen; ++i) {
    data.append(static_cast<const char*>(msg->msg_iov[i].iov_base),
                msg->msg_iov[i].iov_len);
  }
  return data;
}

size_t IovLength(const struct msghdr* msg) {
  size_t n = 0;
  for (size_t i = 0; i < msg->msg_iovlen; ++i)
    n += msg->msg_iov[i].iov_len;
  return n;
}

void Scatter(const char* data, size_t len, struct msghdr* msg) {
  for (size_t i = 0; i < msg->msg_iovlen && len; ++i) {
    const size_t n = std::min(len, msg->msg_iov[i].iov_len);
    memcpy(msg->msg_iov[i].iov_base, data, n);
    data += n;
    len -= n;
  }
}

bool IsLoopback(const sockaddr_storage& addr) {
  const sockaddr_storage a = Canonical(addr);
  if (a.ss_family == AF_INET) {
    return (ntohl(reinterpret_cast<const sockaddr_in*>(&a)->sin_addr.s_addr) >>
            24) == 127;
  }
  return a.ss_family == AF_INET6 &&
         IN6_IS_ADDR_LOOPBACK(&reinterpret_cast<const sockaddr_in6*>(&a)->sin6_addr);
}

bool IsUnspecified(const sockaddr_storage& addr) {
  if (addr.ss_family == AF_INET)
    return reinterpret_cast<const sockaddr_in*>(&addr)->sin_addr.s_addr == 0;
  return IN6_IS_ADDR_UNSPECIFIED(
      &reinterpret_cast<const sockaddr_in6*>(&addr)->sin6_addr);
}

int PortOf(const sockaddr_storage& addr) {
  // sin_port and sin6_port share their offset.
  return ntohs(reinterpret_cast<const sockaddr_in*>(&addr)->sin_port);
}

void SetPort(sockaddr_storage* addr, int port) {
  reinterpret_cast<sockaddr_in*>(addr)->sin_port = htons(port);
}

// TCP ports in use in this process (bound, listening, or the local end of
// a loopback connection) and the listeners among them. VFS mutex.
std::set<int>* g_ports = NULL;
std::map<int, TCPSocket*>* g_listeners = NULL;
int g_next_port = 49152;

// An ephemeral port (49152-65535), registered. 0 if none is free.
int AllocatePort() {
  if (!g_ports)
    g_ports = new std::set<int>;
  for (int i = 0; i < 16384; ++i) {
    const int port = g_next_port;
    g_next_port = g_next_port == 65535 ? 49152 : g_next_port + 1;
    if (!g_ports->count(port)) {
      g_ports->insert(port);
      return port;
    }
  }
  return 0;
}

}  // namespace

// --- TCPSocket ----------------------------------------------------------

const size_t TCPSocket::kSendBufferSize;

TCPSocket::TCPSocket(int fd, int socket_family, int oflag)
    : SocketStream(socket_family, oflag), state_(kNew), id_(0), in_pos_(0),
      unsent_(0), eof_(false), shut_rd_(false), shut_wr_(false),
      last_error_(0), port_(0), loopback_(false), loop_peer_(NULL),
      backlog_(0) {
  SetUnspecified(socket_family, &peer_);
  SetUnspecified(socket_family, &local_);
}

TCPSocket::~TCPSocket() {
  ALOG_ASSERT(id_ == 0, "TCPSocket destroyed with a live connection");
  DetachLoopback();
  ReleasePort();
}

bool TCPSocket::is_block(int flags) const {
  return !(oflag() & O_NONBLOCK) && !(flags & MSG_DONTWAIT);
}

template <typename Ready>
bool TCPSocket::WaitFor(Ready ready, const base::TimeDelta& timeout) {
  VirtualFileSystem* sys = Sys();
  const base::TimeTicks limit = internal::TimeOutToTimeLimit(timeout);
  while (!ready() && state_ != kClosed) {
    if (sys->WaitUntil(limit))
      return ready();
  }
  return true;
}

void TCPSocket::Disconnect() {
  if (id_) {
    net::Close(id_);
    id_ = 0;
  }
}

void TCPSocket::SetError(int error) {
  if (state_ == kClosed)
    return;
  state_ = kError;
  last_error_ = error;
  error_ = error;  // SO_ERROR
  Disconnect();
  NotifyListeners();
}

void TCPSocket::OnLastFileRef() {
  Disconnect();
  DetachLoopback();
  if (state_ == kListening) {
    g_listeners->erase(port_);
    // Connections nobody accepted: their clients read EOF.
    for (size_t i = 0; i < accept_queue_.size(); ++i)
      accept_queue_[i]->DetachLoopback();
    accept_queue_.clear();
  }
  ReleasePort();
  state_ = kClosed;
  Sys()->Broadcast();  // Wake threads blocked on this socket.
}

size_t TCPSocket::SendSpace() const {
  if (loopback_) {
    return loop_peer_ ? kSendBufferSize -
                            std::min(kSendBufferSize, loop_peer_->available())
                      : 0;
  }
  return kSendBufferSize - std::min(unsent_, kSendBufferSize);
}

void TCPSocket::ConnectLoopback(TCPSocket* listener) {
  scoped_refptr<TCPSocket> server =
      new TCPSocket(-1, listener->socket_family_, O_RDWR);
  if (!port_)
    port_ = AllocatePort();
  local_ = peer_;  // The same loopback address, with our own port.
  SetPort(&local_, port_);
  server->local_ = peer_;
  server->peer_ = local_;
  server->state_ = kConnected;
  server->loopback_ = true;
  server->loop_peer_ = this;
  state_ = kConnected;
  loopback_ = true;
  loop_peer_ = server.get();
  listener->accept_queue_.push_back(server);
  listener->NotifyListeners();
  NotifyListeners();
  Sys()->Broadcast();
}

void TCPSocket::DetachLoopback() {
  if (!loop_peer_)
    return;
  TCPSocket* peer = loop_peer_;
  loop_peer_ = NULL;
  peer->loop_peer_ = NULL;
  peer->eof_ = true;
  peer->NotifyListeners();
  Sys()->Broadcast();
}

void TCPSocket::ReleasePort() {
  if (port_ && g_ports)
    g_ports->erase(port_);
  port_ = 0;
}

int TCPSocket::bind(const sockaddr* addr, socklen_t addrlen) {
  const int error =
      internal::VerifyInputSocketAddress(addr, addrlen, socket_family_);
  if (error)
    return posix_translation::Fail(error);
  if (state_ != kNew || port_)
    return posix_translation::Fail(EINVAL);
  sockaddr_storage a;
  memset(&a, 0, sizeof(a));
  memcpy(&a, addr, std::min<size_t>(addrlen, sizeof(a)));
  a = Canonical(a);
  // Any address, loopback, or eth0's (which leads nowhere else): outgoing
  // connections leave from the relay host anyway, and servers are reachable
  // only from this process.
  sockaddr_storage eth0;
  net::ParseAddress("10.0.2.15", 0, &eth0);
  const bool is_eth0 = a.ss_family == AF_INET &&
      reinterpret_cast<sockaddr_in*>(&a)->sin_addr.s_addr ==
          reinterpret_cast<sockaddr_in*>(&eth0)->sin_addr.s_addr;
  if (!IsUnspecified(a) && !IsLoopback(a) && !is_eth0)
    return posix_translation::Fail(EADDRNOTAVAIL);
  int port = PortOf(a);
  if (port == 0) {
    port = AllocatePort();
    if (!port)
      return posix_translation::Fail(EADDRINUSE);
  } else {
    if (!g_ports)
      g_ports = new std::set<int>;
    if (g_ports->count(port))
      return posix_translation::Fail(EADDRINUSE);
    g_ports->insert(port);
  }
  port_ = port;
  local_ = a;
  SetPort(&local_, port);
  return 0;
}

int TCPSocket::listen(int backlog) {
  if (state_ == kListening) {
    backlog_ = std::max(backlog, 1);
    return 0;
  }
  if (state_ != kNew)
    return posix_translation::Fail(EINVAL);
  if (!port_) {  // listen() without bind(): an ephemeral port on any address.
    port_ = AllocatePort();
    if (!port_)
      return posix_translation::Fail(EADDRINUSE);
    SetPort(&local_, port_);
  }
  if (!g_listeners)
    g_listeners = new std::map<int, TCPSocket*>;
  if (g_listeners->count(port_))
    return posix_translation::Fail(EADDRINUSE);
  (*g_listeners)[port_] = this;
  state_ = kListening;
  backlog_ = std::max(backlog, 1);
  ALOGI("TCPSocket::listen: port %d (loopback, this process only)", port_);
  NotifyListeners();
  return 0;
}

int TCPSocket::accept(sockaddr* addr, socklen_t* addrlen) {
  if (addr) {
    const int error = internal::VerifyOutputSocketAddress(addr, addrlen);
    if (error)
      return posix_translation::Fail(error);
  }
  if (state_ != kListening)
    return posix_translation::Fail(EINVAL);
  if (is_block(0)) {
    if (!WaitFor([this] {
          return !accept_queue_.empty() || state_ != kListening;
        }, recv_timeout_)) {
      return posix_translation::Fail(EAGAIN);
    }
  }
  if (state_ == kClosed)
    return posix_translation::Fail(EBADF);
  if (accept_queue_.empty())
    return posix_translation::Fail(EAGAIN);
  scoped_refptr<TCPSocket> socket = accept_queue_.front();
  accept_queue_.pop_front();
  NotifyListeners();
  const int fd = Sys()->AddFileStreamLocked(socket);
  if (fd < 0) {
    socket->DetachLoopback();
    return posix_translation::Fail(EMFILE);
  }
  if (addr)
    CopyOut(socket->peer_, socket_family_, addr, addrlen);
  return fd;
}

int TCPSocket::connect(const sockaddr* addr, socklen_t addrlen) {
  const int error =
      internal::VerifyInputSocketAddress(addr, addrlen, socket_family_);
  if (error)
    return posix_translation::Fail(error);
  switch (state_) {
    case kConnected:
      return posix_translation::Fail(EISCONN);
    case kListening:
      return posix_translation::Fail(EINVAL);
    case kClosed:
      return posix_translation::Fail(EBADF);
    case kConnecting:
      if (!is_block(0))
        return posix_translation::Fail(EALREADY);
      break;
    case kError:
      // Linux allows another attempt after a failed connect().
      state_ = kNew;
      last_error_ = 0;
      // Fall through.
    case kNew: {
      sockaddr_storage peer;
      memset(&peer, 0, sizeof(peer));
      memcpy(&peer, addr, std::min<size_t>(addrlen, sizeof(peer)));
      char host[INET6_ADDRSTRLEN];
      if (net::FormatAddress(peer, host, sizeof(host)) < 0)
        return posix_translation::Fail(EAFNOSUPPORT);
      if (IsLoopback(peer) && g_listeners &&
          g_listeners->count(PortOf(peer))) {
        TCPSocket* listener = (*g_listeners)[PortOf(peer)];
        if (listener->accept_queue_.size() >= listener->backlog_)
          return posix_translation::Fail(ECONNREFUSED);
        peer_ = Canonical(peer);
        ConnectLoopback(listener);
        return 0;
      }
      if (!StartNet()) {
        // Without the app's network (ARC_NET=0) loopback is still there,
        // with nobody listening on that port.
        return posix_translation::Fail(
            !net::Enabled() && IsLoopback(peer) ? ECONNREFUSED : ENETUNREACH);
      }
      if (state_ != kNew)  // Changed while the mutex was released.
        return posix_translation::Fail(state_ == kClosed ? EBADF : EALREADY);
      peer_ = Canonical(peer);
      state_ = kConnecting;
      error_ = 0;
      id_ = net::OpenTcp(this, peer_);
      NotifyListeners();
      ALOGI("TCPSocket::connect: %s:%d", host,
            ntohs(reinterpret_cast<sockaddr_in*>(&peer_)->sin_port));
      if (!is_block(0))
        return posix_translation::Fail(EINPROGRESS);
      break;
    }
  }
  // A blocking connect: SO_SNDTIMEO bounds it, as on Linux.
  if (!WaitFor([this] { return state_ != kConnecting; }, send_timeout_))
    return posix_translation::Fail(EINPROGRESS);
  if (state_ == kConnected)
    return 0;
  if (state_ == kClosed)
    return posix_translation::Fail(EBADF);
  error_ = 0;  // Reported by connect() itself.
  return posix_translation::Fail(last_error_ ? last_error_ : ECONNREFUSED);
}

int TCPSocket::Shutdown(int how) {
  if (how != SHUT_RD && how != SHUT_WR && how != SHUT_RDWR)
    return posix_translation::Fail(EINVAL);
  if (state_ != kConnected && state_ != kError)
    return posix_translation::Fail(ENOTCONN);
  if (how != SHUT_WR)
    shut_rd_ = true;
  if (how != SHUT_RD && !shut_wr_) {
    shut_wr_ = true;
    if (id_)
      net::ShutdownWrite(id_);
    if (loop_peer_) {
      loop_peer_->eof_ = true;
      loop_peer_->NotifyListeners();
    }
  }
  NotifyListeners();
  Sys()->Broadcast();
  return 0;
}

off64_t TCPSocket::lseek(off64_t offset, int whence) {
  return posix_translation::Fail(ESPIPE);
}

ssize_t TCPSocket::read(void* buf, size_t count) {
  return recv(buf, count, 0);
}

ssize_t TCPSocket::recv(void* buf, size_t len, int flags) {
  if (state_ == kNew || state_ == kListening)
    return posix_translation::Fail(ENOTCONN);
  if (shut_rd_ && !available())
    return 0;
  const size_t want = (flags & MSG_WAITALL) ? len : 1;
  if (is_block(flags)) {
    const bool ready = WaitFor([this, want] {
      return state_ != kConnecting &&
             (available() >= want || eof_ || shut_rd_ || state_ != kConnected);
    }, recv_timeout_);
    if (!ready)
      return posix_translation::Fail(EAGAIN);
  }
  if (state_ == kClosed)
    return posix_translation::Fail(EBADF);
  if (state_ == kConnecting)
    return posix_translation::Fail(EAGAIN);
  const size_t n = std::min(len, available());
  if (n) {
    memcpy(buf, in_buf_.data() + in_pos_, n);
    if (!(flags & MSG_PEEK)) {
      in_pos_ += n;
      if (in_pos_ == in_buf_.size()) {
        in_buf_.clear();
        in_pos_ = 0;
      } else if (in_pos_ > 64 * 1024 && in_pos_ * 2 > in_buf_.size()) {
        in_buf_.erase(0, in_pos_);
        in_pos_ = 0;
      }
      NotifyListeners();
      if (loop_peer_) {  // Its writer may be waiting for room.
        loop_peer_->NotifyListeners();
        Sys()->Broadcast();
      }
    }
    return n;
  }
  if (len == 0)
    return 0;
  if (state_ == kError)
    return posix_translation::Fail(last_error_);
  if (eof_ || shut_rd_)
    return 0;
  return posix_translation::Fail(EAGAIN);
}

ssize_t TCPSocket::recvfrom(void* buf, size_t len, int flags, sockaddr* addr,
                            socklen_t* addrlen) {
  const ssize_t n = recv(buf, len, flags);
  if (n >= 0 && addr && addrlen)
    CopyOut(peer_, socket_family_, addr, addrlen);
  return n;
}

int TCPSocket::recvmsg(struct msghdr* msg, int flags) {
  if (!msg || (msg->msg_iovlen && !msg->msg_iov))
    return posix_translation::Fail(EINVAL);
  std::string tmp(IovLength(msg), '\0');
  const ssize_t n = recv(&tmp[0], tmp.size(), flags);
  if (n < 0)
    return -1;
  Scatter(tmp.data(), n, msg);
  msg->msg_controllen = 0;
  msg->msg_flags = 0;
  if (msg->msg_name)
    msg->msg_namelen = 0;
  return n;
}

ssize_t TCPSocket::write(const void* buf, size_t count) {
  return send(buf, count, 0);
}

ssize_t TCPSocket::send(const void* buf, size_t len, int flags) {
  switch (state_) {
    case kNew:
    case kListening:
      return posix_translation::Fail(ENOTCONN);
    case kClosed:
      return posix_translation::Fail(EBADF);
    case kError:
      return posix_translation::Fail(last_error_ ? last_error_ : EPIPE);
    case kConnecting:
      if (!is_block(flags))
        return posix_translation::Fail(EAGAIN);
      break;
    case kConnected:
      break;
  }
  const char* p = static_cast<const char*>(buf);
  size_t done = 0;
  do {
    if (is_block(flags)) {
      const bool ready = WaitFor([this] {
        return state_ != kConnecting &&
               (state_ != kConnected || SendSpace() > 0 ||
                (loopback_ && !loop_peer_));
      }, send_timeout_);
      if (!ready)
        return done ? done : posix_translation::Fail(EAGAIN);
    }
    if (state_ != kConnected || shut_wr_ || (loopback_ && !loop_peer_)) {
      if (done)
        return done;
      if (state_ == kClosed)
        return posix_translation::Fail(EBADF);
      if (state_ == kError)
        return posix_translation::Fail(last_error_ ? last_error_ : EPIPE);
      if (state_ == kConnecting)
        return posix_translation::Fail(EAGAIN);
      return posix_translation::Fail(EPIPE);
    }
    const size_t space = SendSpace();
    if (!space)
      return done ? done : posix_translation::Fail(EAGAIN);
    const size_t n = std::min(len - done, space);
    if (n && loopback_) {
      loop_peer_->in_buf_.append(p + done, n);
      loop_peer_->NotifyListeners();
      Sys()->Broadcast();
    } else if (n) {
      net::Send(id_, p + done, n, NULL);
      unsent_ += n;
    }
    done += n;
  } while (done < len && is_block(flags));
  if (!SendSpace())
    NotifyListeners();  // No longer writable.
  return done;
}

ssize_t TCPSocket::sendto(const void* buf, size_t len, int flags,
                          const sockaddr* dest_addr, socklen_t addrlen) {
  // A connected stream socket ignores the destination (Linux: EISCONN only
  // for some; ignoring it matches what most callers expect).
  return send(buf, len, flags);
}

int TCPSocket::sendmsg(const struct msghdr* msg, int flags) {
  if (!msg || (msg->msg_iovlen && !msg->msg_iov))
    return posix_translation::Fail(EINVAL);
  const std::string data = Gather(msg);
  return send(data.data(), data.size(), flags);
}

int TCPSocket::ioctl(int request, va_list ap) {
  {
    va_list copy;
    va_copy(copy, ap);
    bool handled;
    const int r = net::InterfaceIoctl(request, va_arg(copy, void*), &handled);
    va_end(copy);
    if (handled)
      return r;
  }
  if (request == FIONREAD) {
    int* out = va_arg(ap, int*);
    *out = available();
    return 0;
  }
  if (request == TIOCOUTQ) {  // == SIOCOUTQ
    int* out = va_arg(ap, int*);
    *out = unsent_;
    return 0;
  }
  return SocketStream::ioctl(request, ap);
}

int TCPSocket::getsockopt(int level, int optname, void* optval,
                          socklen_t* optlen) {
  int result;
  if (CommonGetOption(SOCK_STREAM, socket_family_, &error_, int_options_,
                      level, optname, optval, optlen, &result)) {
    return result;
  }
  if (level == IPPROTO_TCP && optname == TCP_INFO)
    return posix_translation::Fail(ENOPROTOOPT);
  return SocketStream::getsockopt(level, optname, optval, optlen);
}

int TCPSocket::setsockopt(int level, int optname, const void* optval,
                          socklen_t optlen) {
  int result;
  if (CommonSetOption(&int_options_, level, optname, optval, optlen, &result))
    return result;
  return SocketStream::setsockopt(level, optname, optval, optlen);
}

int TCPSocket::getpeername(sockaddr* name, socklen_t* namelen) {
  if (state_ != kConnected && !(state_ == kError && eof_))
    return posix_translation::Fail(ENOTCONN);
  return CopyOut(peer_, socket_family_, name, namelen);
}

int TCPSocket::getsockname(sockaddr* name, socklen_t* namelen) {
  return CopyOut(local_, socket_family_, name, namelen);
}

int16_t TCPSocket::GetPollEvents() const {
  switch (state_) {
    case kNew:
      return POLLOUT | POLLHUP;  // As Linux reports an unconnected socket.
    case kConnecting:
      return 0;
    case kListening:
      return accept_queue_.empty() ? 0 : POLLIN | POLLRDNORM;
    case kConnected: {
      int16_t events = 0;
      if (available() || eof_ || shut_rd_)
        events |= POLLIN | POLLRDNORM;
      if (eof_)
        events |= POLLRDHUP;
      if (!shut_wr_ && (SendSpace() > 0 || (loopback_ && !loop_peer_)))
        events |= POLLOUT | POLLWRNORM;
      if (eof_ && shut_wr_)
        events |= POLLHUP;
      return events;
    }
    case kError:
      return POLLIN | POLLOUT | POLLERR | POLLHUP;
    case kClosed:
      return POLLIN | POLLOUT | POLLHUP;
  }
  return 0;
}

bool TCPSocket::IsSelectReadReady() const {
  return GetPollEvents() & (POLLIN | POLLHUP | POLLERR);
}

bool TCPSocket::IsSelectWriteReady() const {
  return GetPollEvents() & (POLLOUT | POLLERR);
}

bool TCPSocket::IsSelectExceptionReady() const {
  return state_ == kError;
}

const char* TCPSocket::GetStreamType() const { return "tcp"; }

void TCPSocket::OnOpen(const sockaddr_storage& local) {
  if (state_ != kConnecting)
    return;
  state_ = kConnected;
  if (local.ss_family == AF_INET || local.ss_family == AF_INET6)
    local_ = Canonical(local);
  NotifyListeners();
}

void TCPSocket::OnOpenFailed(int error) {
  ALOGI("TCPSocket: connect failed: %s", strerror(error));
  SetError(error);
}

void TCPSocket::OnData(const char* data, size_t len,
                       const sockaddr_storage& from) {
  if (state_ != kConnected)
    return;
  in_buf_.append(data, len);
  NotifyListeners();
}

void TCPSocket::OnEof() {
  eof_ = true;
  NotifyListeners();
}

void TCPSocket::OnError(int error) {
  SetError(error);
}

void TCPSocket::OnSent(size_t len) {
  const bool was_full = unsent_ >= kSendBufferSize;
  unsent_ -= std::min(len, unsent_);
  if (was_full && unsent_ < kSendBufferSize)
    NotifyListeners();
}

// --- UDPSocket ----------------------------------------------------------

const size_t UDPSocket::kMaxQueuedBytes;

UDPSocket::UDPSocket(int fd, int socket_family, int oflag)
    : SocketStream(socket_family, oflag), id_(0), open_(false),
      closed_(false), error_state_(0), connected_(false), queued_bytes_(0) {
  SetUnspecified(socket_family, &peer_);
  SetUnspecified(socket_family, &local_);
}

UDPSocket::~UDPSocket() {
  ALOG_ASSERT(id_ == 0, "UDPSocket destroyed with a live association");
}

void UDPSocket::OnLastFileRef() {
  if (id_) {
    net::Close(id_);
    id_ = 0;
  }
  closed_ = true;
  Sys()->Broadcast();
}

int UDPSocket::EnsureOpen() {
  if (error_state_)
    return error_state_;
  if (id_)
    return 0;
  if (!StartNet())
    return ENETUNREACH;
  if (closed_)
    return EBADF;
  if (!id_)
    id_ = net::OpenUdp(this, socket_family_);
  return 0;
}

bool UDPSocket::FromPeer(const sockaddr_storage& from) const {
  return !connected_ || SameAddress(from, peer_);
}

int UDPSocket::bind(const sockaddr* saddr, socklen_t addrlen) {
  const int error =
      internal::VerifyInputSocketAddress(saddr, addrlen, socket_family_);
  if (error)
    return posix_translation::Fail(error);
  if (id_)
    return posix_translation::Fail(EINVAL);
  if (!IsAnyPortZero(saddr)) {
    ALOGW("UDPSocket::bind: only the wildcard address with port 0 works");
    return posix_translation::Fail(EADDRNOTAVAIL);
  }
  // Without the app's network (ARC_NET=0) binding works, as on a device in
  // airplane mode; sending says ENETUNREACH (EnsureOpen).
  if (!net::Enabled())
    return 0;
  const int open_error = EnsureOpen();
  return open_error ? posix_translation::Fail(open_error) : 0;
}

int UDPSocket::connect(const sockaddr* addr, socklen_t addrlen) {
  if (addr && addrlen >= sizeof(sa_family_t) && addr->sa_family == AF_UNSPEC) {
    connected_ = false;  // Dissolve the association.
    SetUnspecified(socket_family_, &peer_);
    return 0;
  }
  const int error =
      internal::VerifyInputSocketAddress(addr, addrlen, socket_family_);
  if (error)
    return posix_translation::Fail(error);
  const int open_error = EnsureOpen();
  if (open_error)
    return posix_translation::Fail(open_error);
  memset(&peer_, 0, sizeof(peer_));
  memcpy(&peer_, addr, std::min<size_t>(addrlen, sizeof(peer_)));
  peer_ = Canonical(peer_);
  connected_ = true;
  // Drop queued datagrams from other peers, as Linux filters them.
  std::deque<Datagram> kept;
  for (size_t i = 0; i < queue_.size(); ++i) {
    if (FromPeer(queue_[i].from))
      kept.push_back(queue_[i]);
  }
  queue_.swap(kept);
  NotifyListeners();
  return 0;
}

int UDPSocket::getpeername(sockaddr* name, socklen_t* namelen) {
  if (!connected_)
    return posix_translation::Fail(ENOTCONN);
  return CopyOut(peer_, socket_family_, name, namelen);
}

int UDPSocket::getsockname(sockaddr* name, socklen_t* namelen) {
  return CopyOut(local_, socket_family_, name, namelen);
}

ssize_t UDPSocket::send(const void* buf, size_t len, int flags) {
  return sendto(buf, len, flags, NULL, 0);
}

ssize_t UDPSocket::sendto(const void* buf, size_t len, int flags,
                          const sockaddr* dest_addr, socklen_t addrlen) {
  sockaddr_storage to;
  if (dest_addr) {
    const int error =
        internal::VerifyInputSocketAddress(dest_addr, addrlen, socket_family_);
    if (error)
      return posix_translation::Fail(error);
    memset(&to, 0, sizeof(to));
    memcpy(&to, dest_addr, std::min<size_t>(addrlen, sizeof(to)));
  } else if (connected_) {
    to = peer_;
  } else {
    return posix_translation::Fail(EDESTADDRREQ);
  }
  if (len > 65507)
    return posix_translation::Fail(EMSGSIZE);
  const int open_error = EnsureOpen();
  if (open_error)
    return posix_translation::Fail(open_error);
  net::Send(id_, buf, len, &to);
  return len;
}

int UDPSocket::sendmsg(const struct msghdr* msg, int flags) {
  if (!msg || (msg->msg_iovlen && !msg->msg_iov))
    return posix_translation::Fail(EINVAL);
  const std::string data = Gather(msg);
  return sendto(data.data(), data.size(), flags,
                static_cast<const sockaddr*>(msg->msg_name),
                msg->msg_name ? msg->msg_namelen : 0);
}

ssize_t UDPSocket::recv(void* buffer, size_t len, int flags) {
  return recvfrom(buffer, len, flags, NULL, NULL);
}

ssize_t UDPSocket::recvfrom(void* buffer, size_t len, int flags,
                            sockaddr* addr, socklen_t* addrlen) {
  if (!(oflag() & O_NONBLOCK) && !(flags & MSG_DONTWAIT)) {
    VirtualFileSystem* sys = Sys();
    const base::TimeTicks limit = internal::TimeOutToTimeLimit(recv_timeout_);
    while (queue_.empty() && !closed_ && !error_state_) {
      if (sys->WaitUntil(limit))
        break;
    }
  }
  if (closed_)
    return posix_translation::Fail(EBADF);
  if (queue_.empty()) {
    if (error_state_)
      return posix_translation::Fail(error_state_);
    return posix_translation::Fail(EAGAIN);
  }
  const Datagram& d = queue_.front();
  const size_t n = std::min(len, d.data.size());
  memcpy(buffer, d.data.data(), n);
  if (addr && addrlen)
    CopyOut(d.from, socket_family_, addr, addrlen);
  const ssize_t result = (flags & MSG_TRUNC) ? d.data.size() : n;
  if (!(flags & MSG_PEEK)) {
    queued_bytes_ -= d.data.size();
    queue_.pop_front();
    NotifyListeners();
  }
  return result;
}

int UDPSocket::recvmsg(struct msghdr* msg, int flags) {
  if (!msg || (msg->msg_iovlen && !msg->msg_iov))
    return posix_translation::Fail(EINVAL);
  const size_t room = IovLength(msg);
  std::string tmp(room ? room : 1, '\0');
  sockaddr_storage from;
  socklen_t fromlen = sizeof(from);
  const bool truncating = !queue_.empty() && queue_.front().data.size() > room;
  const ssize_t n = recvfrom(&tmp[0], room, flags | MSG_TRUNC,
                             reinterpret_cast<sockaddr*>(&from), &fromlen);
  if (n < 0)
    return -1;
  const size_t copied = std::min<size_t>(n, room);
  Scatter(tmp.data(), copied, msg);
  msg->msg_flags = (truncating || static_cast<size_t>(n) > room) ? MSG_TRUNC
                                                                  : 0;
  msg->msg_controllen = 0;
  if (msg->msg_name) {
    const socklen_t len = std::min(msg->msg_namelen, fromlen);
    memcpy(msg->msg_name, &from, len);
    msg->msg_namelen = fromlen;
  }
  return (flags & MSG_TRUNC) ? n : copied;
}

ssize_t UDPSocket::read(void* buf, size_t count) { return recv(buf, count, 0); }

ssize_t UDPSocket::write(const void* buf, size_t count) {
  return send(buf, count, 0);
}

int UDPSocket::ioctl(int request, va_list ap) {
  {
    va_list copy;
    va_copy(copy, ap);
    bool handled;
    const int r = net::InterfaceIoctl(request, va_arg(copy, void*), &handled);
    va_end(copy);
    if (handled)
      return r;
  }
  if (request == FIONREAD) {
    int* out = va_arg(ap, int*);
    *out = queue_.empty() ? 0 : queue_.front().data.size();
    return 0;
  }
  return SocketStream::ioctl(request, ap);
}

int UDPSocket::getsockopt(int level, int optname, void* optval,
                          socklen_t* optlen) {
  int result;
  if (CommonGetOption(SOCK_DGRAM, socket_family_, &error_, int_options_,
                      level, optname, optval, optlen, &result)) {
    return result;
  }
  return SocketStream::getsockopt(level, optname, optval, optlen);
}

int UDPSocket::setsockopt(int level, int optname, const void* optval,
                          socklen_t optlen) {
  int result;
  if (CommonSetOption(&int_options_, level, optname, optval, optlen, &result))
    return result;
  return SocketStream::setsockopt(level, optname, optval, optlen);
}

int16_t UDPSocket::GetPollEvents() const {
  if (closed_)
    return POLLIN | POLLOUT | POLLHUP;
  int16_t events = POLLOUT | POLLWRNORM;
  if (!queue_.empty())
    events |= POLLIN | POLLRDNORM;
  if (error_state_)
    events |= POLLERR;
  return events;
}

bool UDPSocket::IsSelectReadReady() const {
  return GetPollEvents() & (POLLIN | POLLERR | POLLHUP);
}

bool UDPSocket::IsSelectWriteReady() const { return true; }

bool UDPSocket::IsSelectExceptionReady() const { return error_state_ != 0; }

const char* UDPSocket::GetStreamType() const { return "udp"; }

void UDPSocket::OnOpen(const sockaddr_storage& local) {
  open_ = true;
  if (local.ss_family == AF_INET || local.ss_family == AF_INET6)
    local_ = Canonical(local);
}

void UDPSocket::OnOpenFailed(int error) {
  error_state_ = error;
  error_ = error;
  NotifyListeners();
}

void UDPSocket::OnData(const char* data, size_t len,
                       const sockaddr_storage& from) {
  const sockaddr_storage source = Canonical(from);
  if (!FromPeer(source))
    return;
  Datagram d;
  d.from = source;
  d.data.assign(data, len);
  queued_bytes_ += len;
  queue_.push_back(d);
  while (queued_bytes_ > kMaxQueuedBytes && queue_.size() > 1) {
    queued_bytes_ -= queue_.front().data.size();
    queue_.pop_front();
  }
  NotifyListeners();
}

void UDPSocket::OnEof() {}

void UDPSocket::OnError(int error) {
  error_state_ = error;
  error_ = error;
  NotifyListeners();
}

void UDPSocket::OnSent(size_t len) {}

}  // namespace posix_translation
