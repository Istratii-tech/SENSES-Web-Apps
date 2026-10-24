// Network tests of the kernel: TCP/UDP sockets and DNS through libc ->
// posix_translation -> the net bridge (shim/posix_translation/net_bridge.h).
//
// The echo server (TCP and UDP on 127.0.0.1) is tools/serve.py's, at the
// page's port + 1 in browsers and at $ARC_NET_ECHO_PORT under Node; without
// that variable, Node starts its own on the main JS thread. Tests that need
// the internet (DNS of real names, HTTP to example.com) are skipped with
// ARC_NET_OFFLINE=1 (Node) or ?arg=--offline (browsers).
//
//   make net-test                         # Node, direct transport
//   ARC_NET_RELAY=ws://127.0.0.1:8135/__net ARC_NET_ECHO_PORT=8136 make net-test
//   /net-test/index.html                  # browsers, through serve.py

#include <arpa/inet.h>
#include <emscripten/em_asm.h>
#include <emscripten/em_js.h>
#include <emscripten/threading.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <set>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "kernel/arc_kernel.h"

EM_JS(int, net_test_env_echo_port, (), {
  if (typeof process === 'object' && process.env && process.env.ARC_NET_ECHO_PORT)
    return +process.env.ARC_NET_ECHO_PORT;
  if (typeof process !== 'object' && globalThis.location && globalThis.location.port)
    return +globalThis.location.port + 1;
  return 0;
});

EM_JS(int, net_test_env_offline, (), {
  return (typeof process === 'object' && process.env && process.env.ARC_NET_OFFLINE === '1') ? 1 : 0;
});

namespace {

bool g_offline = false;
int g_echo_port = 0;

// Under Node without serve.py: TCP and UDP echo servers on the main thread
// (StartNodeEchoServer runs this there).
EM_JS(void, net_test_start_echo, (), {
  const net = require('net'), dgram = require('dgram');
  const srv = net.createServer({allowHalfOpen: true}, (s) => {
    s.on('data', (d) => s.write(d));
    s.on('end', () => s.end());
    s.on('error', () => {});
  });
  srv.listen(0, '127.0.0.1', () => {
    const port = srv.address().port;
    const u = dgram.createSocket('udp4');
    u.on('message', (m, r) => u.send(m, r.port, r.address));
    u.bind(port, '127.0.0.1', () => { globalThis.arcNetTestEcho = port; });
    u.unref();
  });
  srv.unref();
});

int StartNodeEchoServer() {
  emscripten_sync_run_in_main_runtime_thread(EM_FUNC_SIG_V,
                                             net_test_start_echo);
  for (int i = 0; i < 500; ++i) {
    const int port = MAIN_THREAD_EM_ASM_INT({ return globalThis.arcNetTestEcho | 0; });
    if (port)
      return port;
    usleep(10000);
  }
  return 0;
}

double NowMs() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

sockaddr_in Loopback(int port) {
  sockaddr_in sin;
  memset(&sin, 0, sizeof(sin));
  sin.sin_family = AF_INET;
  sin.sin_port = htons(port);
  sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  return sin;
}

int ConnectEcho(int flags = 0) {
  const int fd = socket(AF_INET, SOCK_STREAM | flags, 0);
  if (fd < 0)
    return -1;
  sockaddr_in sin = Loopback(g_echo_port);
  if (connect(fd, reinterpret_cast<sockaddr*>(&sin), sizeof(sin)) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

// Reads exactly |n| bytes (blocking socket).
std::string ReadN(int fd, size_t n) {
  std::string out;
  char buf[4096];
  while (out.size() < n) {
    const ssize_t r = recv(fd, buf, std::min(sizeof(buf), n - out.size()), 0);
    if (r <= 0)
      break;
    out.append(buf, r);
  }
  return out;
}

class NetTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!g_echo_port)
      GTEST_SKIP() << "no echo server";
  }
};

TEST(NetDnsTest, NumericAndLocal) {
  addrinfo hints = {};
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = NULL;
  ASSERT_EQ(0, getaddrinfo("127.0.0.1", "8080", &hints, &res));
  ASSERT_NE(nullptr, res);
  EXPECT_EQ(AF_INET, res->ai_family);
  EXPECT_EQ(SOCK_STREAM, res->ai_socktype);
  EXPECT_EQ(8080, ntohs(reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_port));
  EXPECT_EQ(nullptr, res->ai_next);
  freeaddrinfo(res);

  ASSERT_EQ(0, getaddrinfo("::1", "https", &hints, &res));
  EXPECT_EQ(AF_INET6, res->ai_family);
  EXPECT_EQ(443, ntohs(reinterpret_cast<sockaddr_in6*>(res->ai_addr)->sin6_port));
  freeaddrinfo(res);

  // No socktype: one entry for TCP and one for UDP.
  ASSERT_EQ(0, getaddrinfo("192.0.2.1", NULL, NULL, &res));
  ASSERT_NE(nullptr, res->ai_next);
  EXPECT_EQ(SOCK_STREAM, res->ai_socktype);
  EXPECT_EQ(SOCK_DGRAM, res->ai_next->ai_socktype);
  freeaddrinfo(res);

  hints.ai_family = AF_INET;
  ASSERT_EQ(0, getaddrinfo("localhost", "80", &hints, &res));
  EXPECT_EQ(htonl(INADDR_LOOPBACK),
            reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr.s_addr);
  freeaddrinfo(res);

  hints.ai_flags = AI_NUMERICHOST;
  EXPECT_EQ(EAI_NONAME, getaddrinfo("example.com", NULL, &hints, &res));
  hints.ai_flags = 0;
  EXPECT_EQ(EAI_SERVICE, getaddrinfo("127.0.0.1", "no-such-service", &hints, &res));

  // AF_INET6 + AI_V4MAPPED turns IPv4 into ::ffff:a.b.c.d (Java does this).
  hints.ai_family = AF_INET6;
  hints.ai_flags = AI_V4MAPPED;
  ASSERT_EQ(0, getaddrinfo("192.0.2.7", NULL, &hints, &res));
  char text[INET6_ADDRSTRLEN];
  inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6*>(res->ai_addr)->sin6_addr,
            text, sizeof(text));
  EXPECT_STREQ("::ffff:192.0.2.7", text);
  freeaddrinfo(res);

  sockaddr_in sin = Loopback(8080);
  char host[64], serv[16];
  ASSERT_EQ(0, getnameinfo(reinterpret_cast<sockaddr*>(&sin), sizeof(sin), host,
                           sizeof(host), serv, sizeof(serv),
                           NI_NUMERICHOST | NI_NUMERICSERV));
  EXPECT_STREQ("127.0.0.1", host);
  EXPECT_STREQ("8080", serv);
}

TEST(NetDnsTest, RealNames) {
  if (g_offline)
    GTEST_SKIP() << "offline";
  addrinfo hints = {};
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = NULL;
  ASSERT_EQ(0, getaddrinfo("example.com", "80", &hints, &res));
  ASSERT_NE(nullptr, res);
  // IPv4 first: the relay host may have no IPv6 route.
  EXPECT_EQ(AF_INET, res->ai_family);
  int n = 0;
  for (addrinfo* ai = res; ai; ai = ai->ai_next)
    ++n;
  EXPECT_GE(n, 1);
  freeaddrinfo(res);

  EXPECT_EQ(EAI_NONAME, getaddrinfo("does-not-exist.invalid", NULL, &hints, &res));

  hostent* he = gethostbyname("example.com");
  ASSERT_NE(nullptr, he);
  EXPECT_EQ(AF_INET, he->h_addrtype);
  EXPECT_EQ(4, he->h_length);
  ASSERT_NE(nullptr, he->h_addr_list[0]);
  EXPECT_EQ(nullptr, gethostbyname("does-not-exist.invalid"));
}

TEST(NetInterfacesTest, LoAndEth0) {
  EXPECT_EQ(1u, if_nametoindex("lo"));
  EXPECT_EQ(2u, if_nametoindex("eth0"));
  EXPECT_EQ(0u, if_nametoindex("wlan9"));
  char name[IF_NAMESIZE];
  ASSERT_NE(nullptr, if_indextoname(2, name));
  EXPECT_STREQ("eth0", name);

  ifaddrs* list = NULL;
  ASSERT_EQ(0, getifaddrs(&list));
  bool lo4 = false, lo6 = false, eth0 = false;
  for (ifaddrs* i = list; i; i = i->ifa_next) {
    char text[INET6_ADDRSTRLEN] = "";
    if (i->ifa_addr->sa_family == AF_INET) {
      inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr,
                text, sizeof(text));
    } else {
      inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6*>(i->ifa_addr)->sin6_addr,
                text, sizeof(text));
    }
    lo4 |= strcmp(i->ifa_name, "lo") == 0 && strcmp(text, "127.0.0.1") == 0;
    lo6 |= strcmp(i->ifa_name, "lo") == 0 && strcmp(text, "::1") == 0;
    eth0 |= strcmp(i->ifa_name, "eth0") == 0 && (i->ifa_flags & IFF_UP);
  }
  freeifaddrs(list);
  EXPECT_TRUE(lo4 && lo6 && eth0);

  // What libcore's NetworkInterface reads.
  DIR* dir = opendir("/sys/class/net");
  ASSERT_NE(nullptr, dir);
  std::set<std::string> names;
  while (dirent* e = readdir(dir))
    names.insert(e->d_name);
  closedir(dir);
  std::string listing;
  for (const std::string& n : names)
    listing += n + " ";
  EXPECT_TRUE(names.count("lo") && names.count("eth0")) << listing;
  FILE* f = fopen("/sys/class/net/eth0/ifindex", "r");
  ASSERT_NE(nullptr, f);
  int index = 0;
  EXPECT_EQ(1, fscanf(f, "%d", &index));
  fclose(f);
  EXPECT_EQ(2, index);
  f = fopen("/proc/net/if_inet6", "r");
  ASSERT_NE(nullptr, f);
  char line[128] = "";
  ASSERT_NE(nullptr, fgets(line, sizeof(line), f));
  fclose(f);
  EXPECT_NE(nullptr, strstr(line, " lo"));

  const int s = socket(AF_INET, SOCK_DGRAM, 0);
  ifreq req = {};
  strcpy(req.ifr_name, "eth0");
  ASSERT_EQ(0, ioctl(s, SIOCGIFADDR, &req));
  char text[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(&req.ifr_addr)->sin_addr,
            text, sizeof(text));
  EXPECT_STREQ("10.0.2.15", text);
  strcpy(req.ifr_name, "nope0");
  EXPECT_EQ(-1, ioctl(s, SIOCGIFFLAGS, &req));
  EXPECT_EQ(ENODEV, errno);
  close(s);
}

TEST_F(NetTest, BlockingEcho) {
  const int fd = ConnectEcho();
  ASSERT_GE(fd, 0) << strerror(errno);
  sockaddr_in peer;
  socklen_t len = sizeof(peer);
  ASSERT_EQ(0, getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &len));
  EXPECT_EQ(g_echo_port, ntohs(peer.sin_port));
  sockaddr_in local;
  len = sizeof(local);
  ASSERT_EQ(0, getsockname(fd, reinterpret_cast<sockaddr*>(&local), &len));
  EXPECT_EQ(AF_INET, local.sin_family);

  const char kHello[] = "hello, relay";
  ASSERT_EQ((ssize_t)strlen(kHello), send(fd, kHello, strlen(kHello), 0));
  EXPECT_EQ(kHello, ReadN(fd, strlen(kHello)));

  // write()/read() work too (Java's streams use them).
  ASSERT_EQ(3, write(fd, "abc", 3));
  char buf[8];
  ASSERT_EQ(3, read(fd, buf, sizeof(buf)));
  EXPECT_EQ(0, memcmp(buf, "abc", 3));

  // Half close: the echo server answers with its own FIN.
  ASSERT_EQ(0, shutdown(fd, SHUT_WR));
  EXPECT_EQ(-1, send(fd, "x", 1, MSG_NOSIGNAL));
  EXPECT_EQ(EPIPE, errno);
  EXPECT_EQ(0, recv(fd, buf, sizeof(buf), 0));
  EXPECT_EQ(0, close(fd));
}

TEST_F(NetTest, NonBlockingConnectPollAndSoError) {
  const int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
  ASSERT_GE(fd, 0);
  EXPECT_TRUE(fcntl(fd, F_GETFL) & O_NONBLOCK);
  sockaddr_in sin = Loopback(g_echo_port);
  ASSERT_EQ(-1, connect(fd, reinterpret_cast<sockaddr*>(&sin), sizeof(sin)));
  ASSERT_EQ(EINPROGRESS, errno);
  char buf[16];
  EXPECT_EQ(-1, recv(fd, buf, sizeof(buf), 0));
  EXPECT_EQ(EAGAIN, errno);

  pollfd pfd = {fd, POLLOUT, 0};
  ASSERT_EQ(1, poll(&pfd, 1, 10000));
  EXPECT_TRUE(pfd.revents & POLLOUT);
  int err = -1;
  socklen_t len = sizeof(err);
  ASSERT_EQ(0, getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len));
  EXPECT_EQ(0, err);
  EXPECT_EQ(-1, connect(fd, reinterpret_cast<sockaddr*>(&sin), sizeof(sin)));
  EXPECT_EQ(EISCONN, errno);

  ASSERT_EQ(4, send(fd, "ping", 4, 0));
  pfd.events = POLLIN;
  ASSERT_EQ(1, poll(&pfd, 1, 10000));
  // FIONREAD reports what arrived (maybe in more than one piece).
  int avail = 0;
  for (int i = 0; i < 100 && avail < 4; ++i) {
    ASSERT_EQ(0, ioctl(fd, FIONREAD, &avail));
    if (avail < 4)
      usleep(10000);
  }
  EXPECT_EQ(4, avail);
  ASSERT_EQ(4, recv(fd, buf, sizeof(buf), MSG_PEEK));
  ASSERT_EQ(4, recv(fd, buf, sizeof(buf), 0));
  EXPECT_EQ(0, memcmp(buf, "ping", 4));
  EXPECT_EQ(-1, recv(fd, buf, sizeof(buf), 0));
  EXPECT_EQ(EAGAIN, errno);
  close(fd);
}

TEST_F(NetTest, ConnectionRefused) {
  // Nothing listens on port 1.
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in sin = Loopback(1);
  EXPECT_EQ(-1, connect(fd, reinterpret_cast<sockaddr*>(&sin), sizeof(sin)));
  EXPECT_EQ(ECONNREFUSED, errno);
  close(fd);

  fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
  ASSERT_EQ(-1, connect(fd, reinterpret_cast<sockaddr*>(&sin), sizeof(sin)));
  ASSERT_EQ(EINPROGRESS, errno);
  pollfd pfd = {fd, POLLOUT, 0};
  ASSERT_EQ(1, poll(&pfd, 1, 10000));
  EXPECT_TRUE(pfd.revents & POLLERR);
  int err = 0;
  socklen_t len = sizeof(err);
  ASSERT_EQ(0, getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len));
  EXPECT_EQ(ECONNREFUSED, err);
  ASSERT_EQ(0, getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len));
  EXPECT_EQ(0, err);  // Reading SO_ERROR cleared it.
  close(fd);
}

TEST_F(NetTest, Epoll) {
  const int s = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
  sockaddr_in sin = Loopback(g_echo_port);
  ASSERT_EQ(-1, connect(s, reinterpret_cast<sockaddr*>(&sin), sizeof(sin)));
  ASSERT_EQ(EINPROGRESS, errno);
  const int ep = epoll_create1(0);
  ASSERT_GE(ep, 0);
  epoll_event ev = {};
  ev.events = EPOLLOUT;
  ev.data.fd = s;
  ASSERT_EQ(0, epoll_ctl(ep, EPOLL_CTL_ADD, s, &ev));
  epoll_event got[4];
  ASSERT_EQ(1, epoll_wait(ep, got, 4, 10000));
  EXPECT_TRUE(got[0].events & EPOLLOUT);
  EXPECT_EQ(s, got[0].data.fd);

  ev.events = EPOLLIN;
  ASSERT_EQ(0, epoll_ctl(ep, EPOLL_CTL_MOD, s, &ev));
  EXPECT_EQ(0, epoll_wait(ep, got, 4, 50));  // Nothing to read yet.
  ASSERT_EQ(5, send(s, "epoll", 5, 0));
  ASSERT_EQ(1, epoll_wait(ep, got, 4, 10000));
  EXPECT_TRUE(got[0].events & EPOLLIN);
  char buf[8];
  EXPECT_GT(recv(s, buf, sizeof(buf), 0), 0);
  close(ep);
  close(s);
}

TEST_F(NetTest, ReceiveTimeout) {
  const int fd = ConnectEcho();
  ASSERT_GE(fd, 0);
  timeval tv = {0, 200 * 1000};
  ASSERT_EQ(0, setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)));
  char buf[4];
  const double start = NowMs();
  EXPECT_EQ(-1, recv(fd, buf, sizeof(buf), 0));
  EXPECT_EQ(EAGAIN, errno);
  EXPECT_GE(NowMs() - start, 150);
  // poll() with a timeout and nothing to read.
  pollfd pfd = {fd, POLLIN, 0};
  EXPECT_EQ(0, poll(&pfd, 1, 100));
  close(fd);
}

TEST_F(NetTest, SocketOptions) {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  int one = 1, value = 0;
  socklen_t len = sizeof(value);
  EXPECT_EQ(0, setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)));
  EXPECT_EQ(0, getsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &value, &len));
  EXPECT_EQ(1, value);
  EXPECT_EQ(0, setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one)));
  EXPECT_EQ(0, getsockopt(fd, SOL_SOCKET, SO_TYPE, &value, &len));
  EXPECT_EQ(SOCK_STREAM, value);
  EXPECT_EQ(0, getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &value, &len));
  EXPECT_GT(value, 0);
  // Not connected yet.
  char buf[1];
  EXPECT_EQ(-1, recv(fd, buf, 1, 0));
  EXPECT_EQ(ENOTCONN, errno);
  sockaddr_in peer;
  len = sizeof(peer);
  EXPECT_EQ(-1, getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &len));
  EXPECT_EQ(ENOTCONN, errno);
  EXPECT_EQ(0, listen(fd, 1));  // A loopback server (NetLoopbackTest).
  close(fd);
}

TEST_F(NetTest, Ipv6SocketToV4MappedAddress) {
  // Java on Android 4.4 makes AF_INET6 sockets and connects them to
  // ::ffff:a.b.c.d for IPv4 hosts.
  const int fd = socket(AF_INET6, SOCK_STREAM, 0);
  ASSERT_GE(fd, 0);
  sockaddr_in6 sin6 = {};
  sin6.sin6_family = AF_INET6;
  sin6.sin6_port = htons(g_echo_port);
  inet_pton(AF_INET6, "::ffff:127.0.0.1", &sin6.sin6_addr);
  ASSERT_EQ(0, connect(fd, reinterpret_cast<sockaddr*>(&sin6), sizeof(sin6)))
      << strerror(errno);
  sockaddr_in6 peer = {};
  socklen_t len = sizeof(peer);
  ASSERT_EQ(0, getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &len));
  EXPECT_EQ(AF_INET6, peer.sin6_family);
  EXPECT_TRUE(IN6_IS_ADDR_V4MAPPED(&peer.sin6_addr));
  ASSERT_EQ(2, send(fd, "v6", 2, 0));
  EXPECT_EQ("v6", ReadN(fd, 2));
  close(fd);
}

struct Writer {
  int fd;
  std::string data;
};

void* WriteAll(void* arg) {
  Writer* w = static_cast<Writer*>(arg);
  size_t off = 0;
  while (off < w->data.size()) {
    const ssize_t n = send(w->fd, w->data.data() + off,
                           std::min<size_t>(65536, w->data.size() - off), 0);
    if (n <= 0)
      break;
    off += n;
  }
  return NULL;
}

TEST_F(NetTest, TwoMegabytesBothWays) {
  // A writer thread and this reader at once: exercises the send buffer
  // limit (TCPSocket::kSendBufferSize) and the relay's flow.
  const int fd = ConnectEcho();
  ASSERT_GE(fd, 0);
  Writer w = {fd, std::string()};
  w.data.resize(2 * 1024 * 1024);
  for (size_t i = 0; i < w.data.size(); ++i)
    w.data[i] = static_cast<char>((i * 2654435761u) >> 13);
  pthread_t thread;
  ASSERT_EQ(0, pthread_create(&thread, NULL, WriteAll, &w));
  const double start = NowMs();
  const std::string back = ReadN(fd, w.data.size());
  pthread_join(thread, NULL);
  EXPECT_EQ(w.data.size(), back.size());
  EXPECT_TRUE(back == w.data);
  printf("[ info     ] 2 MB echoed in %.0f ms\n", NowMs() - start);
  close(fd);
}

TEST_F(NetTest, CloseWakesBlockedReader) {
  const int fd = ConnectEcho();
  ASSERT_GE(fd, 0);
  struct Args { int fd; ssize_t result; int error; } args = {fd, 0, 0};
  pthread_t thread;
  pthread_create(&thread, NULL, [](void* p) -> void* {
    Args* a = static_cast<Args*>(p);
    char buf[4];
    a->result = recv(a->fd, buf, sizeof(buf), 0);
    a->error = errno;
    return NULL;
  }, &args);
  usleep(100000);
  // shutdown(SHUT_RD) makes the blocked recv() return 0.
  ASSERT_EQ(0, shutdown(fd, SHUT_RDWR));
  pthread_join(thread, NULL);
  EXPECT_EQ(0, args.result);
  close(fd);
}

TEST_F(NetTest, UdpEcho) {
  const int fd = socket(AF_INET, SOCK_DGRAM, 0);
  ASSERT_GE(fd, 0);
  sockaddr_in to = Loopback(g_echo_port);
  const char kMsg[] = "datagram";
  ASSERT_EQ((ssize_t)sizeof(kMsg),
            sendto(fd, kMsg, sizeof(kMsg), 0, reinterpret_cast<sockaddr*>(&to),
                   sizeof(to)));
  pollfd pfd = {fd, POLLIN, 0};
  ASSERT_EQ(1, poll(&pfd, 1, 10000));
  char buf[64];
  sockaddr_in from = {};
  socklen_t len = sizeof(from);
  ASSERT_EQ((ssize_t)sizeof(kMsg),
            recvfrom(fd, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from),
                     &len));
  EXPECT_STREQ(kMsg, buf);
  EXPECT_EQ(g_echo_port, ntohs(from.sin_port));
  EXPECT_EQ(htonl(INADDR_LOOPBACK), from.sin_addr.s_addr);

  // Connected UDP: send()/recv().
  ASSERT_EQ(0, connect(fd, reinterpret_cast<sockaddr*>(&to), sizeof(to)));
  ASSERT_EQ(3, send(fd, "abc", 3, 0));
  timeval tv = {10, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ASSERT_EQ(3, recv(fd, buf, sizeof(buf), 0));
  EXPECT_EQ(0, memcmp(buf, "abc", 3));
  // Truncation of a long datagram.
  ASSERT_EQ(10, send(fd, "0123456789", 10, 0));
  ASSERT_EQ(1, poll(&pfd, 1, 10000));
  EXPECT_EQ(10, recv(fd, buf, 4, MSG_TRUNC));
  close(fd);
}

TEST(NetLoopbackTest, ServerInThisProcess) {
  const int srv = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
  ASSERT_GE(srv, 0);
  sockaddr_in addr = Loopback(0);
  ASSERT_EQ(0, bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)));
  socklen_t len = sizeof(addr);
  ASSERT_EQ(0, getsockname(srv, reinterpret_cast<sockaddr*>(&addr), &len));
  const int port = ntohs(addr.sin_port);
  EXPECT_GT(port, 0);
  ASSERT_EQ(0, listen(srv, 4));
  EXPECT_EQ(-1, accept(srv, NULL, NULL));
  EXPECT_EQ(EAGAIN, errno);

  // A second socket cannot take the port.
  const int other = socket(AF_INET, SOCK_STREAM, 0);
  EXPECT_EQ(-1, bind(other, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)));
  EXPECT_EQ(EADDRINUSE, errno);
  close(other);

  // An AF_INET6 client to ::ffff:127.0.0.1, as Java connects.
  const int cli = socket(AF_INET6, SOCK_STREAM, 0);
  sockaddr_in6 to6 = {};
  to6.sin6_family = AF_INET6;
  to6.sin6_port = htons(port);
  inet_pton(AF_INET6, "::ffff:127.0.0.1", &to6.sin6_addr);
  ASSERT_EQ(0, connect(cli, reinterpret_cast<sockaddr*>(&to6), sizeof(to6)));
  pollfd pfd = {srv, POLLIN, 0};
  ASSERT_EQ(1, poll(&pfd, 1, 1000));
  sockaddr_in from = {};
  len = sizeof(from);
  const int conn = accept4(srv, reinterpret_cast<sockaddr*>(&from), &len, SOCK_CLOEXEC);
  ASSERT_GE(conn, 0) << strerror(errno);
  EXPECT_EQ(htonl(INADDR_LOOPBACK), from.sin_addr.s_addr);
  sockaddr_in6 cli_local = {};
  len = sizeof(cli_local);
  ASSERT_EQ(0, getsockname(cli, reinterpret_cast<sockaddr*>(&cli_local), &len));
  EXPECT_EQ(from.sin_port, cli_local.sin6_port);

  ASSERT_EQ(4, send(cli, "ping", 4, 0));
  EXPECT_EQ("ping", ReadN(conn, 4));
  ASSERT_EQ(4, send(conn, "pong", 4, 0));
  EXPECT_EQ("pong", ReadN(cli, 4));

  // 2 MB server -> client, more than the send buffer: the writer blocks
  // until the reader drains.
  Writer w = {conn, std::string(2 * 1024 * 1024, 'x')};
  for (size_t i = 0; i < w.data.size(); i += 4096)
    w.data[i] = static_cast<char>(i >> 12);
  pthread_t thread;
  ASSERT_EQ(0, pthread_create(&thread, NULL, WriteAll, &w));
  const std::string back = ReadN(cli, w.data.size());
  pthread_join(thread, NULL);
  EXPECT_TRUE(back == w.data);

  // Closing one end: the other reads EOF and cannot write.
  ASSERT_EQ(0, close(conn));
  char buf[4];
  EXPECT_EQ(0, recv(cli, buf, sizeof(buf), 0));
  EXPECT_EQ(-1, send(cli, "x", 1, MSG_NOSIGNAL));
  EXPECT_EQ(EPIPE, errno);
  close(cli);
  close(srv);

  // The port is free again.
  const int again = socket(AF_INET, SOCK_STREAM, 0);
  addr.sin_port = htons(port);
  EXPECT_EQ(0, bind(again, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)));
  close(again);
}

TEST_F(NetTest, HttpToTheInternet) {
  if (g_offline)
    GTEST_SKIP() << "offline";
  addrinfo hints = {};
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = NULL;
  ASSERT_EQ(0, getaddrinfo("example.com", "80", &hints, &res));
  const int fd = socket(res->ai_family, SOCK_STREAM, 0);
  ASSERT_EQ(0, connect(fd, res->ai_addr, res->ai_addrlen)) << strerror(errno);
  freeaddrinfo(res);
  const char kRequest[] =
      "GET / HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n";
  ASSERT_EQ((ssize_t)strlen(kRequest), send(fd, kRequest, strlen(kRequest), 0));
  std::string response;
  char buf[4096];
  ssize_t n;
  while ((n = recv(fd, buf, sizeof(buf), 0)) > 0)
    response.append(buf, n);
  close(fd);
  EXPECT_EQ(0u, response.find("HTTP/1.1 200")) << response.substr(0, 200);
  EXPECT_NE(std::string::npos, response.find("Example Domain"));
}

}  // namespace

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--offline") == 0)
      g_offline = true;
  }
  g_offline = g_offline || net_test_env_offline();
  arc_kernel::Boot();
  g_echo_port = net_test_env_echo_port();
  if (!g_echo_port && MAIN_THREAD_EM_ASM_INT({ return typeof process === 'object' ? 1 : 0; }))
    g_echo_port = StartNodeEchoServer();
  printf("[ info     ] echo server on 127.0.0.1:%d%s\n", g_echo_port,
         g_offline ? ", offline" : "");
  return RUN_ALL_TESTS();
}
