// Name resolution for wasm programs: libc's getaddrinfo(), freeaddrinfo(),
// getnameinfo() and musl's __lookup_name() (behind gethostbyname*()) are
// defined here and resolve names through the net bridge (net_bridge.h:
// the relay's or Node's resolver). They replace Emscripten's JS versions,
// which invent 172.29.x.x addresses for its WebSocket sockets. Numeric
// addresses and "localhost" never leave the process. posix_translation's
// HostResolver (used by VirtualFileSystem::getaddrinfo and friends) calls
// the same functions. Part of the arc-wasm shim layer.

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>

#include <string>
#include <vector>

#include "posix_translation/host_resolver.h"
#include "posix_translation/net_bridge.h"

namespace {

using posix_translation::net::ParseAddress;

// musl's struct address (src/network/lookup.h), filled by __lookup_name().
struct MuslAddress {
  int family;
  unsigned scopeid;
  uint8_t addr[16];
  int sortkey;
};
const int kMuslMaxAddrs = 48;

bool IsLocalhost(const char* name) {
  const size_t n = strlen(name);
  return strcasecmp(name, "localhost") == 0 ||
         strcasecmp(name, "localhost.") == 0 ||
         (n > 10 && strcasecmp(name + n - 10, ".localhost") == 0);
}

void AddLoopback(int family, std::vector<sockaddr_storage>* out,
                 bool passive) {
  sockaddr_storage ss;
  if (family != AF_INET6) {
    ParseAddress(passive ? "0.0.0.0" : "127.0.0.1", 0, &ss);
    out->push_back(ss);
  }
  if (family != AF_INET) {
    ParseAddress(passive ? "::" : "::1", 0, &ss);
    out->push_back(ss);
  }
}

sockaddr_storage MapV4(const sockaddr_storage& v4) {
  sockaddr_storage out;
  memset(&out, 0, sizeof(out));
  sockaddr_in6* sin6 = reinterpret_cast<sockaddr_in6*>(&out);
  sin6->sin6_family = AF_INET6;
  sin6->sin6_addr.s6_addr[10] = 0xff;
  sin6->sin6_addr.s6_addr[11] = 0xff;
  memcpy(&sin6->sin6_addr.s6_addr[12],
         &reinterpret_cast<const sockaddr_in*>(&v4)->sin_addr, 4);
  return out;
}

// Numeric address, "localhost" or a name for the resolver. Returns 0 or EAI_*.
int LookupHost(const char* name, int family, int flags,
               std::vector<sockaddr_storage>* out) {
  const bool v4mapped = family == AF_INET6 && (flags & AI_V4MAPPED);
  if (!name) {
    AddLoopback(family, out, flags & AI_PASSIVE);
    return 0;
  }
  // Numeric forms, with an IPv6 scope ("fe80::1%1") ignored.
  std::string host(name);
  const size_t percent = host.find('%');
  if (percent != std::string::npos && host.find(':') != std::string::npos)
    host.resize(percent);
  sockaddr_storage ss;
  if (ParseAddress(host.c_str(), 0, &ss)) {
    if (family == AF_UNSPEC || ss.ss_family == family) {
      out->push_back(ss);
      return 0;
    }
    if (v4mapped && ss.ss_family == AF_INET) {
      out->push_back(MapV4(ss));
      return 0;
    }
    return EAI_ADDRFAMILY;
  }
  if (flags & AI_NUMERICHOST)
    return EAI_NONAME;
  if (IsLocalhost(name)) {
    AddLoopback(family, out, false);
    return 0;
  }
  std::vector<sockaddr_storage> found;
  const int error = posix_translation::net::Resolve(
      name, v4mapped ? AF_UNSPEC : family, &found);
  if (error)
    return error;
  bool have_v6 = false;
  for (size_t i = 0; i < found.size(); ++i)
    have_v6 |= found[i].ss_family == AF_INET6;
  for (size_t i = 0; i < found.size(); ++i) {
    if (!v4mapped || found[i].ss_family == AF_INET6) {
      out->push_back(found[i]);
    } else if (!have_v6 || (flags & AI_ALL)) {
      out->push_back(MapV4(found[i]));
    }
  }
  return out->empty() ? EAI_NONAME : 0;
}

int ParseService(const char* service, int flags, int* port) {
  *port = 0;
  if (!service)
    return 0;
  char* end = NULL;
  const long value = strtol(service, &end, 10);  // NOLINT(runtime/int)
  if (*service && end && !*end) {
    if (value < 0 || value > 65535)
      return EAI_SERVICE;
    *port = value;
    return 0;
  }
  if (flags & AI_NUMERICSERV)
    return EAI_NONAME;
  // There is no /etc/services; the names programs actually pass.
  static const struct { const char* name; int port; } kServices[] = {
    {"http", 80}, {"https", 443}, {"www", 80}, {"ftp", 21}, {"ssh", 22},
    {"telnet", 23}, {"smtp", 25}, {"domain", 53}, {"pop3", 110},
    {"imap", 143}, {"ntp", 123}, {"imaps", 993}, {"pop3s", 995},
    {"submission", 587}, {"http-alt", 8080}, {"xmpp-client", 5222},
  };
  for (size_t i = 0; i < sizeof(kServices) / sizeof(kServices[0]); ++i) {
    if (strcasecmp(service, kServices[i].name) == 0) {
      *port = kServices[i].port;
      return 0;
    }
  }
  return EAI_SERVICE;
}

// One malloc'ed block per node: the addrinfo, its address and canonname.
struct Node {
  addrinfo ai;
  sockaddr_storage addr;
  char name[1];
};

addrinfo* NewNode(const sockaddr_storage& addr, int port, int socktype,
                  int protocol, const char* canon) {
  const size_t canon_len = canon ? strlen(canon) : 0;
  Node* node = static_cast<Node*>(calloc(1, sizeof(Node) + canon_len));
  if (!node)
    return NULL;
  node->addr = addr;
  if (addr.ss_family == AF_INET6) {
    reinterpret_cast<sockaddr_in6*>(&node->addr)->sin6_port = htons(port);
    node->ai.ai_addrlen = sizeof(sockaddr_in6);
  } else {
    reinterpret_cast<sockaddr_in*>(&node->addr)->sin_port = htons(port);
    node->ai.ai_addrlen = sizeof(sockaddr_in);
  }
  node->ai.ai_family = addr.ss_family;
  node->ai.ai_socktype = socktype;
  node->ai.ai_protocol = protocol;
  node->ai.ai_addr = reinterpret_cast<sockaddr*>(&node->addr);
  if (canon) {
    memcpy(node->name, canon, canon_len + 1);
    node->ai.ai_canonname = node->name;
  }
  return &node->ai;
}

}  // namespace

extern "C" {

int getaddrinfo(const char* node, const char* service, const addrinfo* hints,
                addrinfo** res) {
  const int family = hints ? hints->ai_family : AF_UNSPEC;
  const int socktype = hints ? hints->ai_socktype : 0;
  const int protocol = hints ? hints->ai_protocol : 0;
  const int flags = hints ? hints->ai_flags : 0;
  *res = NULL;
  if (!node && !service)
    return EAI_NONAME;
  if (family != AF_UNSPEC && family != AF_INET && family != AF_INET6)
    return EAI_FAMILY;
  if (socktype != 0 && socktype != SOCK_STREAM && socktype != SOCK_DGRAM &&
      socktype != SOCK_RAW) {
    return EAI_SOCKTYPE;
  }
  int port;
  int error = ParseService(service, flags, &port);
  if (error)
    return error;
  std::vector<sockaddr_storage> addrs;
  error = LookupHost(node, family, flags, &addrs);
  if (error)
    return error;

  // Without a socktype (or protocol), one entry each for TCP and UDP.
  struct Kind { int socktype, protocol; };
  std::vector<Kind> kinds;
  if (socktype == SOCK_STREAM || protocol == IPPROTO_TCP) {
    kinds.push_back(Kind{SOCK_STREAM, IPPROTO_TCP});
  } else if (socktype == SOCK_DGRAM || protocol == IPPROTO_UDP) {
    kinds.push_back(Kind{SOCK_DGRAM, IPPROTO_UDP});
  } else if (socktype == SOCK_RAW) {
    kinds.push_back(Kind{SOCK_RAW, protocol});
  } else {
    kinds.push_back(Kind{SOCK_STREAM, IPPROTO_TCP});
    kinds.push_back(Kind{SOCK_DGRAM, IPPROTO_UDP});
  }

  addrinfo* head = NULL;
  addrinfo** tail = &head;
  for (size_t i = 0; i < addrs.size(); ++i) {
    for (size_t k = 0; k < kinds.size(); ++k) {
      const char* canon = (!head && (flags & AI_CANONNAME))
                              ? (node ? node : "localhost") : NULL;
      addrinfo* ai = NewNode(addrs[i], port, kinds[k].socktype,
                             kinds[k].protocol, canon);
      if (!ai) {
        freeaddrinfo(head);
        return EAI_MEMORY;
      }
      *tail = ai;
      tail = &ai->ai_next;
    }
  }
  *res = head;
  return 0;
}

void freeaddrinfo(addrinfo* ai) {
  while (ai) {
    addrinfo* next = ai->ai_next;
    free(ai);  // The Node block starts with the addrinfo.
    ai = next;
  }
}

int getnameinfo(const sockaddr* sa, socklen_t salen, char* host,
                socklen_t hostlen, char* serv, socklen_t servlen, int flags) {
  if (!sa)
    return EAI_FAMILY;
  if (sa->sa_family == AF_INET) {
    if (salen < sizeof(sockaddr_in))
      return EAI_FAMILY;
  } else if (sa->sa_family == AF_INET6) {
    if (salen < sizeof(sockaddr_in6))
      return EAI_FAMILY;
  } else {
    return EAI_FAMILY;
  }
  if ((!host || !hostlen) && (!serv || !servlen))
    return EAI_NONAME;
  if (serv && servlen) {
    const int port =
        ntohs(reinterpret_cast<const sockaddr_in*>(sa)->sin_port);
    if (snprintf(serv, servlen, "%d", port) >= static_cast<int>(servlen))
      return EAI_OVERFLOW;
  }
  if (!host || !hostlen)
    return 0;
  char numeric[INET6_ADDRSTRLEN];
  bool loopback;
  if (sa->sa_family == AF_INET6) {
    const in6_addr* a = &reinterpret_cast<const sockaddr_in6*>(sa)->sin6_addr;
    inet_ntop(AF_INET6, a, numeric, sizeof(numeric));
    loopback = IN6_IS_ADDR_LOOPBACK(a);
  } else {
    const in_addr* a = &reinterpret_cast<const sockaddr_in*>(sa)->sin_addr;
    inet_ntop(AF_INET, a, numeric, sizeof(numeric));
    loopback = (ntohl(a->s_addr) >> 24) == 127;
  }
  // No reverse DNS through the relay. As in ARC, a name is given only when
  // one is required, and only for loopback.
  const char* result = numeric;
  if (flags & NI_NAMEREQD) {
    if (!loopback)
      return EAI_NONAME;
    result = sa->sa_family == AF_INET6 ? "ip6-localhost" : "localhost";
  }
  if (strlen(result) >= hostlen)
    return EAI_OVERFLOW;
  strcpy(host, result);  // NOLINT(runtime/printf)
  return 0;
}

// musl's internal resolver entry (hidden in libc; this definition takes
// precedence over Emscripten's IPv4-only one). gethostbyname(),
// gethostbyname2() and their _r variants call it.
int __lookup_name(MuslAddress* buf, char* canon, const char* name, int family,
                  int flags) {
  *canon = 0;
  if (!name)
    return EAI_NONAME;
  const size_t len = strnlen(name, 255);
  if (len == 0 || len >= 254)
    return EAI_NONAME;
  memcpy(canon, name, len + 1);
  std::vector<sockaddr_storage> addrs;
  const int error = LookupHost(name, family, flags & AI_NUMERICHOST, &addrs);
  if (error)
    return error;
  int n = 0;
  for (size_t i = 0; i < addrs.size() && n < kMuslMaxAddrs; ++i, ++n) {
    memset(&buf[n], 0, sizeof(buf[n]));
    buf[n].family = addrs[i].ss_family;
    buf[n].sortkey = n;
    if (addrs[i].ss_family == AF_INET6) {
      memcpy(buf[n].addr,
             &reinterpret_cast<const sockaddr_in6*>(&addrs[i])->sin6_addr, 16);
    } else {
      memcpy(buf[n].addr,
             &reinterpret_cast<const sockaddr_in*>(&addrs[i])->sin_addr, 4);
    }
  }
  return n;
}

}  // extern "C"

namespace posix_translation {

HostResolver::HostResolver(const pp::InstanceHandle& instance) {}
HostResolver::~HostResolver() {}

int HostResolver::getaddrinfo(const char* hostname, const char* servname,
                              const addrinfo* hints, addrinfo** res) {
  return ::getaddrinfo(hostname, servname, hints, res);
}

void HostResolver::freeaddrinfo(addrinfo* res) { ::freeaddrinfo(res); }

hostent* HostResolver::gethostbyname(const char* name) {
  return ::gethostbyname(name);
}

hostent* HostResolver::gethostbyname2(const char* name, int family) {
  return ::gethostbyname2(name, family);
}

int HostResolver::gethostbyname_r(const char* name, hostent* ret, char* buf,
                                  size_t buflen, hostent** result,
                                  int* h_errnop) {
  return ::gethostbyname_r(name, ret, buf, buflen, result, h_errnop);
}

int HostResolver::gethostbyname2_r(const char* host, int family, hostent* ret,
                                   char* buf, size_t buflen, hostent** result,
                                   int* h_errnop) {
  return ::gethostbyname2_r(host, family, ret, buf, buflen, result, h_errnop);
}

hostent* HostResolver::gethostbyaddr(const void* addr, socklen_t len,
                                     int type) {
  return ::gethostbyaddr(addr, len, type);
}

int HostResolver::getnameinfo(const sockaddr* sa, socklen_t salen,
                              char* host, size_t hostlen,
                              char* serv, size_t servlen, int flags) {
  return ::getnameinfo(sa, salen, host, hostlen, serv, servlen, flags);
}

}  // namespace posix_translation
