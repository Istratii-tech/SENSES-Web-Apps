// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "posix_translation/socket_util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <algorithm>

#include "common/alog.h"
#include "posix_translation/virtual_file_system.h"
#include "ppapi/cpp/net_address.h"

namespace posix_translation {
namespace internal {

// Because the trailing padding is not actually necessary, the min size of
// the addrlen is slightly less than the size of the sockaddr_{in,in6}.
const socklen_t kIPv4MinAddrLen =
    offsetof(sockaddr_in, sin_addr) + sizeof(in_addr);
const socklen_t kIPv6MinAddrLen =
    offsetof(sockaddr_in6, sin6_addr) + sizeof(in6_addr);

namespace {

// Converts PP_NetAddress_IPv4 to sockaddr_in.
void NetAddressIPv4ToSockAddrIn(
    const PP_NetAddress_IPv4& net_address, sockaddr_in* saddr) {
  saddr->sin_family = AF_INET;
  // Copy the value as is to keep network byte order.
  saddr->sin_port = net_address.port;
  memcpy(&saddr->sin_addr.s_addr, net_address.addr, sizeof(net_address.addr));
}

// Convert PP_NetAddress_IPv4 to sockaddr_in6 as v4mapped address.
void NetAddressIPv4ToSockAddrIn6V4Mapped(
    const PP_NetAddress_IPv4& net_address, sockaddr_in6* saddr6) {
  saddr6->sin6_family = AF_INET6;
  // Copy the value as is to keep network byte order.
  saddr6->sin6_port = net_address.port;
  // V4Mapped address forms: 0::FFFF:xxx.yyy.zzz.www.
  memset(saddr6->sin6_addr.s6_addr, 0, 10);  // Leading 10 bytes are 0.
  saddr6->sin6_addr.s6_addr[10] = 0xFF;
  saddr6->sin6_addr.s6_addr[11] = 0xFF;
  memcpy(&saddr6->sin6_addr.s6_addr[12], net_address.addr,
         sizeof(net_address.addr));
}

// Converts PP_NetAddress_IPv6 to sockaddr_in6.
void NetAddressIPv6ToSockAddrIn6(
    const PP_NetAddress_IPv6& net_address, sockaddr_in6* saddr6) {
  saddr6->sin6_family = AF_INET6;
  // Copy the value as is to keep network byte order.
  saddr6->sin6_port = net_address.port;
  memcpy(&saddr6->sin6_addr.s6_addr, net_address.addr,
         sizeof(net_address.addr));
}

// Converts sockaddr_in to PP_NetAddress_IPv4.
// sockaddr_in may have trailing padding, but it is ensured in this function
// that the padding is not touched in this function.
// In other words, although saddr has type sockaddr_in, the min size of the
// buffer is IPv4MinAddrLen defined above, which can be smaller than
// sizeof(sockaddr_in).
void SockAddrInToNetAddressIPv4(
    const sockaddr_in* saddr, PP_NetAddress_IPv4* net_address) {
  ALOG_ASSERT(saddr->sin_family == AF_INET);
  // Copy the value as is to keep network byte order.
  net_address->port = saddr->sin_port;
  memcpy(net_address->addr, &saddr->sin_addr.s_addr,
         sizeof(net_address->addr));
}

// Converts sockaddr_in6 to PP_NetAddress_IPv6.
// Similar to sockaddr_in, sockaddr_in6 also may have trailing padding, and
// the min size of saddr6 is IPv6MinAddrLen. See also the comment for
// SockAddrInToNetAddressIPv4.
void SockAddrIn6ToNetAddressIPv6(
    const sockaddr_in6* saddr6, PP_NetAddress_IPv6* net_address) {
  ALOG_ASSERT(saddr6->sin6_family == AF_INET6);
  // Copy the value as is to keep network byte order.
  net_address->port = saddr6->sin6_port;
  memcpy(net_address->addr, &saddr6->sin6_addr.s6_addr,
         sizeof(net_address->addr));
}

}  // namespace

int VerifyInputSocketAddress(
    const sockaddr* addr, socklen_t addrlen, int address_family) {
  ALOG_ASSERT(address_family == AF_INET || address_family == AF_INET6);

  // arc-wasm: socklen_t is unsigned in musl but int in bionic, the ABI this
  // code (and Android apps) assume. Compare as int.
  if (static_cast<int>(addrlen) <= 0) {
    ALOGW("addrlen is not positive: %d", addrlen);
    return EINVAL;
  }

  if (!addr) {
    ALOGW("Given addr is NULL");
    return EFAULT;
  }

  // If the addr size is too small or too large, raise EINVAL.
  const socklen_t kMinAddrLen =
      address_family == AF_INET ? kIPv4MinAddrLen : kIPv6MinAddrLen;
  if (addrlen < kMinAddrLen || addrlen > SIZEOF_AS_SOCKLEN(sockaddr_storage)) {
    ALOGW("The addr has invalid size: %d, %d", address_family, addrlen);
    return EINVAL;
  }

  if (addr->sa_family != address_family) {
    ALOGW("The family is differnt from what is expected: %d, %d",
          addr->sa_family, address_family);
    // Note: for bind(), there seems no spec on man in this case.
    // However, as same as connect(), practically bind() raises
    // EAFNOSUPPORT in this case.
    return EAFNOSUPPORT;
  }

  return 0;
}

int VerifyOutputSocketAddress(
    const sockaddr* addr, const socklen_t* addrlen) {
  if (!addrlen) {
    return EFAULT;
  }

  if (static_cast<int>(*addrlen) < 0) {  // arc-wasm: see above.
    return EINVAL;
  }

  // Note that if addrlen is 0, addr can be NULL, because we will not copy
  // the data to it.
  if (*addrlen != 0 && addr == NULL) {
    return EFAULT;
  }

  return 0;
}

void CopySocketAddress(
    const sockaddr_storage& address, sockaddr* name, socklen_t* namelen) {
  int family = address.ss_family;
  ALOG_ASSERT(family == AF_INET || family == AF_INET6);
  const socklen_t address_length =
      (family == AF_INET) ? sizeof(sockaddr_in) : sizeof(sockaddr_in6);
  if (name) {
    memcpy(name, &address, std::min(*namelen, address_length));
  }
  *namelen = address_length;
}

bool SocketAddressEqual(
    const sockaddr_storage& addr1, const sockaddr_storage& addr2) {
  if (addr1.ss_family != addr2.ss_family)
    return false;

  if (addr1.ss_family == AF_INET) {
    const sockaddr_in& saddr_in1 = reinterpret_cast<const sockaddr_in&>(addr1);
    const sockaddr_in& saddr_in2 = reinterpret_cast<const sockaddr_in&>(addr2);
    return (saddr_in1.sin_port == saddr_in2.sin_port) &&
           (saddr_in1.sin_addr.s_addr == saddr_in2.sin_addr.s_addr);
  }

  if (addr1.ss_family == AF_INET6) {
    const sockaddr_in6& saddr_in6_1 =
        reinterpret_cast<const sockaddr_in6&>(addr1);
    const sockaddr_in6& saddr_in6_2 =
        reinterpret_cast<const sockaddr_in6&>(addr2);
    return (saddr_in6_1.sin6_port == saddr_in6_2.sin6_port) &&
           (memcmp(&saddr_in6_1.sin6_addr, &saddr_in6_2.sin6_addr,
                   sizeof(in6_addr)) == 0);
  }

  // Unknown family.
  ALOGE("SocketAddressEqual Unknown socket family: %d", addr1.ss_family);
  return false;
}

bool NetAddressToSockAddrStorage(
    const pp::NetAddress& net_address,
    int dest_family, bool allow_v4mapped, sockaddr_storage* storage) {
  ALOG_ASSERT(dest_family == AF_UNSPEC ||
              dest_family == AF_INET ||
              dest_family == AF_INET6);
  memset(storage, 0, sizeof(sockaddr_storage));
  switch (net_address.GetFamily()) {
    case PP_NETADDRESS_FAMILY_IPV4: {
      // If IPv6 address is required but the v4map is prohibited, there is
      // no way to return the address.
      if (dest_family == AF_INET6 && !allow_v4mapped)
        return false;

      PP_NetAddress_IPv4 ipv4 = {};
      if (!net_address.DescribeAsIPv4Address(&ipv4)) {
        return false;
      }
      if (dest_family == AF_INET6) {
        NetAddressIPv4ToSockAddrIn6V4Mapped(
            ipv4, reinterpret_cast<sockaddr_in6*>(storage));
      } else {
        NetAddressIPv4ToSockAddrIn(
            ipv4, reinterpret_cast<sockaddr_in*>(storage));
      }
      return true;
    }
    case PP_NETADDRESS_FAMILY_IPV6: {
      // IPv6 address cannot return in IPv4 address format.
      if (dest_family == AF_INET)
        return false;

      PP_NetAddress_IPv6 ipv6 = {};
      if (!net_address.DescribeAsIPv6Address(&ipv6)) {
        return false;
      }
      NetAddressIPv6ToSockAddrIn6(
          ipv6, reinterpret_cast<sockaddr_in6*>(storage));
      return true;
    }
    default:
      return false;
  }
}

pp::NetAddress SockAddrToNetAddress(
    const pp::InstanceHandle& instance, const sockaddr* saddr) {
  ALOG_ASSERT(saddr->sa_family == AF_INET || saddr->sa_family == AF_INET6);
  if (saddr->sa_family == AF_INET) {
    PP_NetAddress_IPv4 ipv4;
    SockAddrInToNetAddressIPv4(
        reinterpret_cast<const sockaddr_in*>(saddr), &ipv4);
    return pp::NetAddress(instance, ipv4);
  }

  if (saddr->sa_family == AF_INET6) {
    PP_NetAddress_IPv6 ipv6;
    SockAddrIn6ToNetAddressIPv6(
        reinterpret_cast<const sockaddr_in6*>(saddr), &ipv6);
    return pp::NetAddress(instance, ipv6);
  }

  return pp::NetAddress();
}

// TODO(hidehiko): Clean up the code as the some part of code inside this
// function can be shared with above methods.
bool StringToSockAddrStorage(
    const char* hostname, uint16_t port,
    int dest_family, bool allow_v4mapped, sockaddr_storage* storage) {
  ALOG_ASSERT(dest_family == AF_UNSPEC ||
              dest_family == AF_INET ||
              dest_family == AF_INET6);
  memset(storage, 0, sizeof(*storage));

  in6_addr addr6;
  if (inet_pton(AF_INET6, hostname, &addr6) == 1) {
    if (dest_family == AF_INET)
      return false;

    // TODO(crbug.com/243012): handle scope_id
    sockaddr_in6* saddr6 = reinterpret_cast<sockaddr_in6*>(storage);
    saddr6->sin6_family = AF_INET6;
    saddr6->sin6_port = port;
    memcpy(saddr6->sin6_addr.s6_addr, &addr6, sizeof(addr6));
    return true;
  }

  in_addr addr4;
  if (inet_pton(AF_INET, hostname, &addr4) == 1) {
    if (dest_family == AF_INET6) {
      // Convert to V4Mapped address.
      if (!allow_v4mapped)
        return false;

      sockaddr_in6* saddr6 = reinterpret_cast<sockaddr_in6*>(storage);
      saddr6->sin6_family = AF_INET6;
      saddr6->sin6_port = port;
      // V4Mapped address forms: 0::FFFF:xxx.yyy.zzz.www.
      memset(saddr6->sin6_addr.s6_addr, 0, 10);  // Leading 10 bytes are 0.
      saddr6->sin6_addr.s6_addr[10] = 0xFF;
      saddr6->sin6_addr.s6_addr[11] = 0xFF;
      memcpy(&saddr6->sin6_addr.s6_addr[12], &addr4, sizeof(addr4));
      return true;
    }

    sockaddr_in* saddr = reinterpret_cast<sockaddr_in*>(storage);
    saddr->sin_family = AF_INET;
    saddr->sin_port = port;
    memcpy(&saddr->sin_addr.s_addr, &addr4, sizeof(addr4));
    return true;
  }

  // Failed to convert into sockaddr_storage.
  return false;
}

uint16_t ServiceNameToPort(const char* service_name) {
  if (service_name == NULL)
    return 0;

  char* end;
  long int port = strtol(service_name, &end, 10);  // NOLINT(runtime/int)
  if (port < 0 || port >= 65536 || *end != '\0') {
    ALOGW("Unsupported network service name %s", service_name);
    return 0;
  }

  return htons(static_cast<uint16_t>(port));
}

addrinfo* SockAddrStorageToAddrInfo(
    const sockaddr_storage& storage, int socktype, int protocol,
    const std::string& name) {
  ALOG_ASSERT(storage.ss_family == AF_INET || storage.ss_family == AF_INET6);
  size_t addrlen = storage.ss_family == AF_INET ?
      sizeof(sockaddr_in) : sizeof(sockaddr_in6);
  sockaddr* saddr = static_cast<sockaddr*>(malloc(addrlen));
  memcpy(saddr, &storage, addrlen);

  addrinfo* info = static_cast<addrinfo*>(malloc(sizeof(addrinfo)));
  info->ai_flags = 0;
  info->ai_family = storage.ss_family;
  info->ai_socktype = socktype ? socktype : SOCK_STREAM;
  info->ai_protocol = protocol;
  info->ai_addrlen = addrlen;
  info->ai_addr = saddr;
  // Use malloc + memcpy, instead of stdup. Valgrind seems to detect strdup
  // has some invalid memory access.
  info->ai_canonname = static_cast<char*>(malloc(name.size() + 1));
  memcpy(info->ai_canonname, name.c_str(), name.size() + 1);
  info->ai_next = NULL;
  return info;
}

void ReleaseAddrInfo(addrinfo* info) {
  free(info->ai_canonname);
  free(info->ai_addr);
  free(info);
}

int VerifyGetSocketOption(const void* optval, const socklen_t* optlen) {
  if (!optlen) {
    return EFAULT;
  }

  if (static_cast<int>(*optlen) < 0) {  // arc-wasm: see above.
    return EINVAL;
  }

  // Note that if optlen is 0, optval can be NULL, because we will not copy
  // the data to it.
  if (*optlen != 0 && !optval) {
    return EFAULT;
  }

  return 0;
}

int VerifySetSocketOption(
    const void* optval, socklen_t optlen, socklen_t expected_optlen) {
  if (optlen < expected_optlen) {
    return EINVAL;
  }

  if (!optval) {
    return EFAULT;
  }

  return 0;
}

int VerifyTimeoutSocketOption(const timeval& timeout) {
  // tv_usec must be in the range of [0, 1000000).
  if (timeout.tv_usec < 0 || timeout.tv_usec >= 1000000) {
    return EDOM;
  }
  return 0;
}

void CopySocketOption(const void* storage, socklen_t storage_length,
                      void* optval, socklen_t* optlen) {
  ALOG_ASSERT(storage);
  *optlen = std::min(*optlen, storage_length);
  if (optval)
    memcpy(optval, storage, *optlen);
}

}  // namespace internal
}  // namespace posix_translation
