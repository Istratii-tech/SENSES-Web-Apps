// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POSIX_TRANSLATION_SOCKET_UTIL_H_
#define POSIX_TRANSLATION_SOCKET_UTIL_H_

#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <string>

// In bionic, socklen_t is int so we cannot compare socklen_t with the
// result of sizeof. With this macro, we can absorb this difference.
#define SIZEOF_AS_SOCKLEN(x) static_cast<socklen_t>(sizeof(x))

namespace base {
class TimeDelta;
}  // namespace base

namespace pp {
class InstanceHandle;
class NetAddress;
}  // namespace pp

namespace posix_translation {

class VirtualFileSystem;

namespace internal {

// Common verification of the input (sockaddr, socklen_t) argument (such as
// arguments for bind() or connect()).
// Returns 0 on success, or a system error number (e.g. EINVAL). This does
// not modify errno.
int VerifyInputSocketAddress(
    const sockaddr* addr, socklen_t addrlen, int address_family);

// Common verification of the output (sockaddr, socklen_t) argument (such as
// arguments for accept() or getsockname()).
// Returns 0 on success, or a system error number (e.g. EINVAL). This does
// not modify errno.
int VerifyOutputSocketAddress(
    const sockaddr* addr, const socklen_t* addrlen);

// Copies the content of the address to the name, and sets the size of the
// original address to namelen. The size is automatically calculated
// based on the socket family of address.
// Caller must set namelen to the size of name before calling this.
// If the size is not enough, the name will have truncated result, but namelen
// will have the size of the result. (i.e., namelen will have the bigger value
// than the input).
// address must represent an address for sockaddr_in or sockaddr_in6. name and
// namelen must pass the verification done by VerifyOutputSocketAddress().
void CopySocketAddress(
    const sockaddr_storage& address, sockaddr* name, socklen_t* namelen);

// Returns whether addr1 and addr2 have same family, port and address.
// Returns false when the family is different from AF_INET or AF_INET6
// even if these are same, just because this function only supports those
// families.
bool SocketAddressEqual(
    const sockaddr_storage& addr1, const sockaddr_storage& addr2);

// Converts pp::NetAddress to sockaddr_storage.
// Returns whether the net_address is successfully converted into the storage.
// The dest_family must be one of AF_UNSPEC, AF_INET or AF_INET6. If AF_UNSPEC
// is given, returned storage will have either AF_INET or AF_INET6 address.
// The allow_v4mapped is effective only if dest_family == AF_INET6, If it is
// set and the net_address represents an IPv4 address, the returned storage
// will have the IPv6 address representing the given IPv4 address.
bool NetAddressToSockAddrStorage(
    const pp::NetAddress& net_address,
    int dest_family, bool allow_v4mapped, sockaddr_storage* storage);

// Converts sockaddr to pp::NetAddress.
// saddr should be verified by VerifyInputSocketAddress in advance, in order
// to avoid illegal memory access.
// The given instance will be used to create a new pp::NetAdress instance.
pp::NetAddress SockAddrToNetAddress(
    const pp::InstanceHandle& instance, const sockaddr* saddr);

// Converts a stringified IPv4 or IPV6 address |hostname| (e.g. "127.0.0.1" or
// "::1") and a port to sockaddr_storage. Returns whether they are
// successfully converted.
// This function works similar to NetAddressPrivateToSockAddrStorage() declared
// above, but different input type. See its comments for how dest_family and
// allow_v4mapped work.
// Note that this function does not resolve the host name (e.g.
// "www.google.co.jp"). Also note that port must be in the network-byte-order.
bool StringToSockAddrStorage(
    const char* hostname, uint16_t port,
    int dest_family, bool allow_v4mapped, sockaddr_storage* storage);

// Parses the given service_name to a port number. On error, returns 0.
// Returned port is in the network-byte-order.
// This function can parse only numbers, e.g. "80" or "22", but not
// named service, such as "http".
uint16_t ServiceNameToPort(const char* service_name);

// Converts sockaddr_storage, socktype, protocol and name into addrinfo
// structure. The storage must have AF_INET or AF_INET6 socket address.
// If socktype is set to 0, returned addrinfo will have SOCK_STREAM as default
// value.
// The result resource must be released by ReleaseAddrInfo declared below.
addrinfo* SockAddrStorageToAddrInfo(
    const sockaddr_storage& storage, int socktype, int protocol,
    const std::string& name);

// Releases the info, allocated by SockAddrStorageToAddrInfo declared above.
void ReleaseAddrInfo(addrinfo* info);

// Common verification for getsockopt().
// Returns 0 on success, or a system error number (e.g. EINVAL). This does not
// modify errno.
int VerifyGetSocketOption(const void* optval, const socklen_t* optlen);

// Common verification for the setsockopt().
// Returns 0 on success, or a system error number (e.g. EINVAL). This does not
// modify errno.
int VerifySetSocketOption(
    const void* optval, socklen_t optlen, socklen_t expected_optlen);

// Verification for SO_RCVTIMEO and SO_SNDTIMEO.
// Returns 0 on success, or a system error number (e.g. EINVAL). This does not
// modify errno.
int VerifyTimeoutSocketOption(const timeval& timeout);

// Copies the content of |storage| whose size is |storage_size| to |optval|.
// The size of copied content is min(storage_size, optlen).
// Upon completion, |optlen| will be set to the copied content size.
void CopySocketOption(const void* storage, socklen_t storage_size,
                      void* optval, socklen_t* optlen);

}  // namespace internal
}  // namespace posix_translation

#endif  // POSIX_TRANSLATION_SOCKET_UTIL_H_
