// The network backend of posix_translation's TCPSocket, UDPSocket and
// HostResolver (replacing Pepper's PPB_TCPSocket/UDPSocket/HostResolver).
//
// One dedicated "net thread" (a pthread idling in its JS event loop, like the
// OPFS thread) owns every connection. Its JavaScript (net_transport.js.inc)
// has two transports with the same events:
//   relay   WebSockets to tools/net_relay.py (served by tools/serve.py):
//           browsers always; Node when ARC_NET_RELAY=ws://host:port/__net
//   direct  Node's net/dgram/dns modules: Node by default
//
// Sockets talk to the net thread asynchronously: Open*/Send/ShutdownWrite/
// Close post a message and return at once (they are called with the VFS
// mutex held), and the net thread reports back through Endpoint callbacks,
// which it calls with the VFS mutex held and followed by a VFS Broadcast().
// Blocking socket calls therefore wait on the VFS condition variable, as
// they did with Pepper. Resolve() is the only synchronous call; it must be
// made without the VFS mutex.
//
// Nothing here depends on the Java runtime: Dalvik, ART and NDK code all
// reach it through libc. Part of the arc-wasm shim layer.
#ifndef POSIX_TRANSLATION_NET_BRIDGE_H_
#define POSIX_TRANSLATION_NET_BRIDGE_H_

#include <stddef.h>
#include <sys/socket.h>

#include <string>
#include <vector>

namespace posix_translation {

class VirtualFileSystem;

namespace net {

// Receives the events of one connection. All callbacks run on the net
// thread with the VFS mutex held; none may block.
class Endpoint {
 public:
  // TCP: connected; UDP: bound. |local| is the relay's/host's local address
  // (port 0 and an unspecified address when unknown).
  virtual void OnOpen(const sockaddr_storage& local) = 0;
  virtual void OnOpenFailed(int error) = 0;
  // Bytes (TCP) or one datagram from |from| (UDP).
  virtual void OnData(const char* data, size_t len,
                      const sockaddr_storage& from) = 0;
  // The peer shut down its sending side (TCP FIN).
  virtual void OnEof() = 0;
  // The connection broke (e.g. ECONNRESET). No more events follow.
  virtual void OnError(int error) = 0;
  // |len| bytes passed to Send() have left the transport's buffers.
  virtual void OnSent(size_t len) = 0;

 protected:
  virtual ~Endpoint() {}
};

// The app's network, from the VM's environment (SENSES Web Apps' per-app
// "Сеть" setting, docs/topics/app-network.md), read once:
//   ARC_NET=0            no network: Start() fails without starting the
//                        transport, names do not resolve (EAI_AGAIN),
//                        connect() and sendto() say ENETUNREACH (loopback
//                        inside the process still works)
//   ARC_NET_DENY=a,b.c   names a and b.c and their subdomains do not
//                        resolve (EAI_NONAME, "not found"), as the
//                        development relay's ARC_NET_DENY does for everyone
// Client side: it holds whatever relay the page talks to.
bool Enabled();
// True if |name| is one of ARC_NET_DENY's names or under one.
bool Denied(const char* name);

// Starts the net thread if needed. Blocks until it runs, so call it without
// the VFS mutex. Returns false if there is no transport (e.g. no
// WebSocket, or ARC_NET=0). Idempotent and thread-safe.
bool Start();

// The calls below need Start() to have succeeded and the VFS mutex held.
// They return immediately; results arrive through |endpoint|, until Close().
// Returns the connection id (> 0).
int OpenTcp(Endpoint* endpoint, const sockaddr_storage& peer);
int OpenUdp(Endpoint* endpoint, int family);
// Copies |data|. |to| is the destination of a UDP datagram, NULL for TCP.
void Send(int id, const void* data, size_t len, const sockaddr_storage* to);
void ShutdownWrite(int id);
// Unregisters the endpoint (no callbacks after this returns) and closes the
// connection once the data already sent has been flushed.
void Close(int id);

// Resolves |name| to addresses (AF_INET first) of |family| (AF_UNSPEC,
// AF_INET or AF_INET6); ports are 0. Blocks; call without the VFS mutex and
// not on the browser main thread. Returns 0 or an EAI_* error.
int Resolve(const char* name, int family, std::vector<sockaddr_storage>* out);

// Network interfaces (net_interfaces.cc): "lo" and a virtual "eth0".
// Answers the SIOCGIF* ioctls on inet sockets; *handled is false for other
// requests. Returns 0 or -1 with errno.
int InterfaceIoctl(int request, void* arg, bool* handled);
// Writes /sys/class/net/... and /proc/net/{if_inet6,route,dev} into the
// host file system under |host_root| and mounts them in |vfs|. Call before
// libc is routed into the VFS (arc_kernel::Boot does).
void MountNetworkFiles(VirtualFileSystem* vfs, const std::string& host_root);

// Helpers shared by the socket classes.
// Formats the address of |addr| (v4-mapped IPv6 addresses as IPv4) into
// |host| and returns its port, or -1 if the family is not AF_INET(6).
int FormatAddress(const sockaddr_storage& addr, char* host, size_t host_len);
// Parses a numeric IPv4/IPv6 address. Returns false if it is neither.
bool ParseAddress(const char* host, int port, sockaddr_storage* out);

}  // namespace net
}  // namespace posix_translation

#endif  // POSIX_TRANSLATION_NET_BRIDGE_H_
