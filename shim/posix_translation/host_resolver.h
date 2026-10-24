// Overrides ARC's posix_translation/host_resolver.h (Pepper HostResolver).
// The methods call libc's resolver functions, which net_resolver.cc defines
// on top of the net bridge (names go to the relay's or Node's resolver).
// Part of the arc-wasm shim layer.
#ifndef POSIX_TRANSLATION_HOST_RESOLVER_H_
#define POSIX_TRANSLATION_HOST_RESOLVER_H_

#include <netdb.h>
#include <sys/socket.h>

#include "base/basictypes.h"
#include "ppapi/cpp/instance.h"

namespace posix_translation {

class HostResolver {
 public:
  explicit HostResolver(const pp::InstanceHandle& instance);
  ~HostResolver();

  int getaddrinfo(const char* hostname, const char* servname,
                  const addrinfo* hints, addrinfo** res);
  void freeaddrinfo(addrinfo* res);

  hostent* gethostbyname(const char* name);
  hostent* gethostbyname2(const char* name, int family);
  int gethostbyname_r(
      const char* name, hostent* ret,
      char* buf, size_t buflen, hostent** result, int* h_errnop);
  int gethostbyname2_r(
      const char* host, int family, hostent* ret,
      char* buf, size_t buflen, hostent** result, int* h_errnop);

  hostent* gethostbyaddr(const void* addr, socklen_t len, int type);

  int getnameinfo(const sockaddr* sa, socklen_t salen,
                  char* host, size_t hostlen,
                  char* serv, size_t servlen, int flags);

 private:
  DISALLOW_COPY_AND_ASSIGN(HostResolver);
};

}  // namespace posix_translation

#endif  // POSIX_TRANSLATION_HOST_RESOLVER_H_
