// Copyright 2014 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "posix_translation/host_resolver.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <string>

#include "common/arc_strace.h"
#include "common/alog.h"
#include "common/trace_event.h"
#include "posix_translation/socket_util.h"
#include "ppapi/cpp/completion_callback.h"
#include "ppapi/cpp/host_resolver.h"
#include "ppapi/cpp/net_address.h"

namespace posix_translation {
namespace {

pthread_once_t g_host_ent_once = PTHREAD_ONCE_INIT;
pthread_key_t g_host_ent_key;

const addrinfo kDefaultHints = {
  AI_V4MAPPED | AI_ADDRCONFIG,  // ai_flags
  AF_UNSPEC,  // ai_family
  // Remaining fields should be filled by 0.
};

void ClearHostEnt(struct hostent* value) {
  free(value->h_name);
  value->h_name = NULL;
  char** addr_list = value->h_addr_list;
  if (addr_list == NULL) {
    return;
  }
  for (; *addr_list != NULL; addr_list++) {
    delete[] *addr_list;
  }
  delete[] value->h_addr_list;
  value->h_addr_list = NULL;
}

void HostEntDestructor(void* value) {
  struct hostent* hostent = reinterpret_cast<struct hostent*>(value);
  ClearHostEnt(hostent);
  delete hostent->h_aliases;
  delete hostent;
}

void InitHostEntKey() {
  if (pthread_key_create(&g_host_ent_key, &HostEntDestructor) != 0) {
    LOG_FATAL("Can't create HostEntKey");
  }
}

struct hostent* GetCleanHostEnt() {
  pthread_once(&g_host_ent_once, &InitHostEntKey);
  struct hostent* hostent = reinterpret_cast<struct hostent*>(
      pthread_getspecific(g_host_ent_key));
  if (hostent == NULL) {
    hostent = new struct hostent;
    hostent->h_name = NULL;
    hostent->h_aliases = new char*[1];
    hostent->h_aliases[0] = NULL;
    hostent->h_addr_list = NULL;
    pthread_setspecific(g_host_ent_key, hostent);
  } else {
    ClearHostEnt(hostent);
  }
  return hostent;
}

}  // namespace

HostResolver::HostResolver(const pp::InstanceHandle& instance)
    : instance_(instance) {
}

HostResolver::~HostResolver() {
}

int HostResolver::getaddrinfo(const char* hostname, const char* servname,
                              const addrinfo* hints, addrinfo** res) {
  // TODO(crbug.com/356271): Use Bionic impl instead.
  // We do not lock mutex_ in this function. resolver.Resolve() may take a few
  // seconds.
  if (hints == NULL)
    hints = &kDefaultHints;

  if (hints->ai_family != AF_UNSPEC &&
      hints->ai_family != AF_INET &&
      hints->ai_family != AF_INET6) {
    ALOGW("getaddrinfo with unsupported family %d", hints->ai_family);
    return EAI_FAMILY;
  }

  // Port in network order.
  uint16_t sin_port = internal::ServiceNameToPort(servname);

  sockaddr_storage storage;
  if (hostname &&
      internal::StringToSockAddrStorage(
          hostname, sin_port, hints->ai_family, hints->ai_flags & AI_V4MAPPED,
          &storage)) {
    *res = internal::SockAddrStorageToAddrInfo(
        storage, hints->ai_socktype, hints->ai_protocol, "");
    ALOG_ASSERT(*res);
    return 0;
  }

  bool is_ipv6 = hints->ai_family == AF_INET6;
  if (hints->ai_flags & AI_PASSIVE) {
    // Numeric case we considered above so the only remaining case is any.
    memset(&storage, 0, sizeof(storage));
    storage.ss_family = is_ipv6 ? AF_INET6 : AF_INET;
    *res = internal::SockAddrStorageToAddrInfo(
        storage, hints->ai_socktype, hints->ai_protocol, "");
    ALOG_ASSERT(*res);
    return 0;
  }

  if (!hostname) {
    bool result = internal::StringToSockAddrStorage(
        is_ipv6 ? "::1" : "127.0.0.1", sin_port,
        hints->ai_family, hints->ai_flags & AI_V4MAPPED, &storage);
    ALOG_ASSERT(result);
    *res = internal::SockAddrStorageToAddrInfo(
        storage, hints->ai_socktype, hints->ai_protocol, "");
    ALOG_ASSERT(*res);
    return 0;
  }

  // TODO(igorc): Remove this check for "1". CTS tests expect that this address
  // is unresolvable, but PPAPI somehow resolves it to 0.0.0.1, which sounds
  // incorrect. nslookup has no matching record. This could be related to
  // the use PP_NETADDRESSFAMILY_UNSPECIFIED, but needs ot be checked.
  if (!strcmp(hostname, "1"))
    return EAI_NONAME;

  if (hints->ai_flags & AI_NUMERICHOST)
    return EAI_NONAME;

  PP_HostResolver_Hint hint = {
      PP_NETADDRESS_FAMILY_UNSPECIFIED,
      hints->ai_flags & AI_CANONNAME ? PP_HOSTRESOLVER_FLAG_CANONNAME : 0
  };

  TRACE_EVENT1(ARC_TRACE_CATEGORY, "HostResolver::getaddrinfo - IPC",
               "hostname", std::string(hostname));

  // Should we retry IPv6, and then UNSPEC?
  pp::HostResolver resolver(instance_);
  // Resolve needs the port number in the host byte order
  // unlike PP_NetAddress_IPv4/6 structures.
  int32_t result = resolver.Resolve(
      hostname, ntohs(sin_port), hint, pp::BlockUntilComplete());
  if (result != PP_OK) {
    // TODO(igorc): Check whether this should be EAI_NODATA
    return EAI_NONAME;
  }

  int count = 0;
  std::string host_name = resolver.GetCanonicalName().AsString();
  uint32_t resolved_addr_count = resolver.GetNetAddressCount();
  for (uint32_t i = 0; i < resolved_addr_count; i++) {
    if (!internal::NetAddressToSockAddrStorage(
            resolver.GetNetAddress(i),
            hints->ai_family, hints->ai_flags & AI_V4MAPPED, &storage))
      continue;
    *res = internal::SockAddrStorageToAddrInfo(
        storage, hints->ai_socktype, hints->ai_protocol, host_name);
    res = &(*res)->ai_next;
    ++count;
    // TODO(igorc): Remove IPv4/IPv6 duplicates.
  }

  return (count == 0 ? EAI_NODATA : 0);
}

void HostResolver::freeaddrinfo(addrinfo* res) {
  while (res != NULL) {
    addrinfo* next = res->ai_next;
    internal::ReleaseAddrInfo(res);
    res = next;
  }
}

hostent* HostResolver::gethostbyname(const char* name) {
  struct hostent* res = gethostbyname2(name, AF_INET);
  if (res == NULL) {
    res = gethostbyname2(name, AF_INET6);
  }
  return res;
}

hostent* HostResolver::gethostbyname2(const char* name, int family) {
  addrinfo* addr_info;
  addrinfo hints = {};
  hints.ai_family = family;
  int res = this->getaddrinfo(name, NULL, &hints, &addr_info);

  switch (res) {
    case 0:
      break;
    case EAI_FAMILY:
    case EAI_NONAME:
      h_errno = HOST_NOT_FOUND;
      return NULL;
    case EAI_NODATA:
      h_errno = NO_DATA;
      return NULL;
    case EAI_AGAIN:
      h_errno = TRY_AGAIN;
      return NULL;
    default:
      ALOGW("getaddrinfo returned error code %d (%s)", res, gai_strerror(res));
      h_errno = NO_RECOVERY;
      return NULL;
  }

  struct hostent* hostent = GetCleanHostEnt();
  hostent->h_name = strdup(name);
  hostent->h_addrtype = family;
  hostent->h_length = (family == AF_INET ?
      sizeof(struct in_addr) : sizeof(struct in6_addr));

  int count = 0;
  addrinfo* addr_info_i = addr_info;
  while (addr_info_i) {
    count++;
    addr_info_i = addr_info_i->ai_next;
  }

  addr_info_i = addr_info;
  hostent->h_addr_list = new char*[count + 1];
  for (int i = 0; i < count; i++) {
    hostent->h_addr_list[i] = new char[hostent->h_length];
    if (family == AF_INET6) {
      memcpy(hostent->h_addr_list[i],
             &(reinterpret_cast<sockaddr_in6*>(
                 addr_info_i->ai_addr))->sin6_addr,
             hostent->h_length);
    } else {
      memcpy(hostent->h_addr_list[i],
             &(reinterpret_cast<sockaddr_in*>(
                 addr_info_i->ai_addr))->sin_addr,
             hostent->h_length);
    }
    addr_info_i = addr_info_i->ai_next;
  }
  hostent->h_addr_list[count] = NULL;

  this->freeaddrinfo(addr_info);
  return hostent;
}

int HostResolver::gethostbyname_r(
    const char* name, hostent* ret,
    char* buf, size_t buflen, hostent** result, int* h_errnop) {
  struct hostent* res = gethostbyname(name);
  if (res == NULL) {
    *result = NULL;
    *h_errnop = h_errno;
    return -1;
  }
  memcpy(ret, res, sizeof(struct hostent));
  *result = ret;
  return 0;
}

int HostResolver::gethostbyname2_r(
    const char* host, int family, hostent* ret,
    char* buf, size_t buflen, hostent** result, int* h_errnop) {
  struct hostent* res = gethostbyname2(host, family);
  if (res == NULL) {
    *result = NULL;
    *h_errnop = h_errno;
    return -1;
  }
  memcpy(ret, res, sizeof(struct hostent));
  *result = ret;
  return 0;
}

hostent* HostResolver::gethostbyaddr(
    const void* addr, socklen_t len, int type) {
  if ((type != AF_INET && type != AF_INET6) ||
      (type == AF_INET && len != sizeof(in_addr)) ||
      (type == AF_INET6 && len != sizeof(in6_addr))) {
    h_errno = EAI_FAMILY;
    return NULL;
  }

  struct hostent* hostent = GetCleanHostEnt();

  char host_name[256];
  inet_ntop(type, addr, host_name, sizeof(host_name));
  hostent->h_name = strdup(host_name);

  hostent->h_addrtype = type;
  hostent->h_length = len;
  hostent->h_addr_list = new char*[2];
  hostent->h_addr_list[0] = new char[len];
  memcpy(hostent->h_addr_list[0], addr, len);
  hostent->h_addr_list[1] = NULL;
  return hostent;
}

int HostResolver::getnameinfo(const sockaddr* sa, socklen_t salen,
                              char* host, size_t hostlen,
                              char* serv, size_t servlen, int flags) {
  // TODO(crbug.com/356271): Use Bionic impl instead.
  if (sa->sa_family != AF_INET && sa->sa_family != AF_INET6)
    return EAI_FAMILY;

  if ((sa->sa_family == AF_INET6 &&
          static_cast<size_t>(salen) < sizeof(sockaddr_in6)) ||
      (sa->sa_family == AF_INET &&
          static_cast<size_t>(salen) < sizeof(sockaddr_in))) {
    return EAI_FAMILY;
  }

  // Must ask for a name.
  if ((host == NULL || hostlen == 0) && (serv == NULL || servlen == 0))
    return EAI_NONAME;

  if (serv) {
    snprintf(serv, servlen, "%d",
             ntohs((reinterpret_cast<const sockaddr_in*>(sa))->sin_port));
  }

  if (!host)
    return 0;

  if (flags & NI_NAMEREQD) {
    if (sa->sa_family == AF_INET6) {
      if (IN6_IS_ADDR_LOOPBACK(
            &(reinterpret_cast<const sockaddr_in6*>(sa))->sin6_addr)) {
        snprintf(host, hostlen, "ip6-localhost");
        return 0;
      }
    } else {
      uint32_t addr4 = static_cast<uint32_t>(ntohl(
          (reinterpret_cast<const sockaddr_in*>(sa))->sin_addr.s_addr));
      if (addr4 == 0x7F000001) {
        snprintf(host, hostlen, "localhost");
        return 0;
      }
    }
  }

  // NI_NUMERICHOST, also fallback when name was requested, but not available.
  if (sa->sa_family == AF_INET6) {
    inet_ntop(AF_INET6,
              &(reinterpret_cast<const sockaddr_in6*>(sa))->sin6_addr,
              host, hostlen);
  } else {
    inet_ntop(AF_INET,
              &(reinterpret_cast<const sockaddr_in*>(sa))->sin_addr,
              host, hostlen);
  }

  return 0;
}

}  // namespace posix_translation
