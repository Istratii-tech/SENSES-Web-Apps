// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "posix_translation/socket_util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>

#include "base/basictypes.h"
#include "base/memory/scoped_ptr.h"
#include "common/alog.h"
#include "gtest/gtest.h"

namespace posix_translation {
namespace internal {
namespace {

struct AddrInfoDeleter {
  void operator()(addrinfo* info) {
    ReleaseAddrInfo(info);
  }
};

// Returns true if buffer contains only 0. Otherwise false.
bool IsFilledByZero(const void* buffer, size_t length) {
  const char* ptr = reinterpret_cast<const char*>(buffer);
  for (size_t i = 0; i < length; ++i) {
    if (ptr[i])
      return false;
  }
  return true;
}

// htons is defined by macro in Bionic, so it confuses gtest template codes.
// This is just a wrapper to avoid the compile errors.
uint16_t HostToNetShort(uint16_t value) {
  return htons(value);
}

}  // namespace

TEST(SocketUtilTest, VerifyInputSocketAddressIPv4) {
  sockaddr_in addr_in = {};
  addr_in.sin_family = AF_INET;
  const sockaddr* addr = reinterpret_cast<sockaddr*>(&addr_in);

  // Typical usage.
  EXPECT_EQ(0, VerifyInputSocketAddress(addr, sizeof(sockaddr_in), AF_INET));

  // Test for addrlen.
  EXPECT_EQ(EINVAL, VerifyInputSocketAddress(NULL, 0, AF_INET));
  EXPECT_EQ(EINVAL, VerifyInputSocketAddress(addr, 0, AF_INET));
  EXPECT_EQ(EINVAL, VerifyInputSocketAddress(NULL, -1, AF_INET));
  EXPECT_EQ(EINVAL, VerifyInputSocketAddress(addr, -1, AF_INET));

  // Test for NULL check of addr.
  EXPECT_EQ(EFAULT, VerifyInputSocketAddress(NULL, 1, AF_INET));
  EXPECT_EQ(EFAULT,
            VerifyInputSocketAddress(NULL, sizeof(sockaddr_in), AF_INET));

  // If the size is not enough, EINVAL is expected.
  EXPECT_EQ(EINVAL, VerifyInputSocketAddress(addr, 1, AF_INET));
  // The min size for INET is 8.
  EXPECT_EQ(EINVAL, VerifyInputSocketAddress(addr, 7, AF_INET));
  EXPECT_EQ(0, VerifyInputSocketAddress(addr, 8, AF_INET));
  // The max size for INET is sizeof(sockaddr_storage).
  char too_large_addr[sizeof(sockaddr_storage) + 1] = {};
  memcpy(too_large_addr, &addr_in, sizeof(addr_in));
  EXPECT_EQ(EINVAL, VerifyInputSocketAddress(
      reinterpret_cast<sockaddr*>(too_large_addr),
      sizeof(too_large_addr), AF_INET));

  // Set other family.
  addr_in.sin_family = AF_UNSPEC;
  EXPECT_EQ(EAFNOSUPPORT,
            VerifyInputSocketAddress(addr, sizeof(sockaddr_in), AF_INET));

  addr_in.sin_family = AF_INET6;
  EXPECT_EQ(EAFNOSUPPORT,
            VerifyInputSocketAddress(addr, sizeof(sockaddr_in), AF_INET));
}

TEST(SocketUtilTest, VerifyInputSocketAddressIPv6) {
  sockaddr_in6 addr_in6 = {};
  addr_in6.sin6_family = AF_INET6;
  const sockaddr* addr = reinterpret_cast<sockaddr*>(&addr_in6);

  // Typical usage.
  EXPECT_EQ(
      0, VerifyInputSocketAddress(addr, sizeof(sockaddr_in6), AF_INET6));

  // Test for addrlen.
  EXPECT_EQ(EINVAL, VerifyInputSocketAddress(NULL, 0, AF_INET6));
  EXPECT_EQ(EINVAL, VerifyInputSocketAddress(addr, 0, AF_INET6));
  EXPECT_EQ(EINVAL, VerifyInputSocketAddress(NULL, -1, AF_INET6));
  EXPECT_EQ(EINVAL, VerifyInputSocketAddress(addr, -1, AF_INET6));

  // Test for NULL check of addr.
  EXPECT_EQ(EFAULT, VerifyInputSocketAddress(NULL, 1, AF_INET6));
  EXPECT_EQ(EFAULT,
            VerifyInputSocketAddress(NULL, sizeof(sockaddr_in6), AF_INET6));

  // If the size is not enough, EINVAL is expected.
  EXPECT_EQ(EINVAL, VerifyInputSocketAddress(addr, 1, AF_INET6));
  // The min size for INET6 is 24
  EXPECT_EQ(EINVAL, VerifyInputSocketAddress(addr, 23, AF_INET6));
  EXPECT_EQ(0, VerifyInputSocketAddress(addr, 24, AF_INET6));
  // The max size for INET6 is sizeof(sockaddr_storage).
  char too_large_addr[sizeof(sockaddr_storage) + 1] = {};
  memcpy(too_large_addr, &addr_in6, sizeof(addr_in6));
  EXPECT_EQ(EINVAL, VerifyInputSocketAddress(
      reinterpret_cast<sockaddr*>(too_large_addr),
      sizeof(too_large_addr), AF_INET6));

  // Set other family.
  addr_in6.sin6_family = AF_UNSPEC;
  EXPECT_EQ(EAFNOSUPPORT,
            VerifyInputSocketAddress(addr, sizeof(sockaddr_in6), AF_INET6));

  addr_in6.sin6_family = AF_INET;
  EXPECT_EQ(EAFNOSUPPORT,
            VerifyInputSocketAddress(addr, sizeof(sockaddr_in6), AF_INET6));
}

TEST(SocketUtilTest, VerifyOutputSocketAddress) {
  sockaddr_storage storage = {};
  const sockaddr* addr = reinterpret_cast<sockaddr*>(&storage);

  // Typical usage.
  socklen_t addrlen = sizeof(storage);
  EXPECT_EQ(0, VerifyOutputSocketAddress(addr, &addrlen));
  addrlen = sizeof(sockaddr_in);
  EXPECT_EQ(0, VerifyOutputSocketAddress(addr, &addrlen));
  addrlen = sizeof(sockaddr_in6);
  EXPECT_EQ(0, VerifyOutputSocketAddress(addr, &addrlen));

  // Or, addrlen can be small or even 0.
  addrlen = 1;
  EXPECT_EQ(0, VerifyOutputSocketAddress(addr, &addrlen));
  addrlen = 0;
  EXPECT_EQ(0, VerifyOutputSocketAddress(addr, &addrlen));

  // addr can be NULL only when addrlen is 0.
  addrlen = 0;
  EXPECT_EQ(0, VerifyOutputSocketAddress(NULL, &addrlen));
  addrlen = 1;
  EXPECT_EQ(EFAULT, VerifyOutputSocketAddress(NULL, &addrlen));

  // addrlen cannot be NULL or negative.
  EXPECT_EQ(EFAULT, VerifyOutputSocketAddress(addr, NULL));
  addrlen = -1;
  EXPECT_EQ(EINVAL, VerifyOutputSocketAddress(addr, &addrlen));
  EXPECT_EQ(EINVAL, VerifyOutputSocketAddress(NULL, &addrlen));
}

TEST(SocketUtilTest, CopySocketAddressIPv4) {
  // Fake IPv4 address.
  sockaddr_in addr_in = {};
  addr_in.sin_family = AF_INET;
  addr_in.sin_port = htons(12345);
  addr_in.sin_addr.s_addr = htonl(0x12345678);

  sockaddr_storage storage = {};
  memcpy(&storage, &addr_in, sizeof(addr_in));

  sockaddr_storage result = {};
  socklen_t result_len;

  // Test with the buffer size equal to socketaddr_storage.
  result_len = sizeof(result);
  CopySocketAddress(
      storage, reinterpret_cast<sockaddr*>(&result), &result_len);
  EXPECT_EQ(SIZEOF_AS_SOCKLEN(addr_in), result_len);
  EXPECT_EQ(0, memcmp(&addr_in, &result, sizeof(addr_in)));

  // Test with the buffer size equal to sockaddr_in.
  memset(&result, 0, sizeof(result));
  result_len = sizeof(addr_in);
  CopySocketAddress(
      storage, reinterpret_cast<sockaddr*>(&result), &result_len);
  EXPECT_EQ(SIZEOF_AS_SOCKLEN(addr_in), result_len);
  EXPECT_EQ(0, memcmp(&addr_in, &result, sizeof(addr_in)));

  // Test with the buffer size smaller than sockaddr_in.
  memset(&result, 0, sizeof(result));
  const socklen_t kHalfSize = sizeof(addr_in) / 2;
  result_len = kHalfSize;
  CopySocketAddress(
      storage, reinterpret_cast<sockaddr*>(&result), &result_len);
  EXPECT_EQ(SIZEOF_AS_SOCKLEN(addr_in), result_len);
  EXPECT_EQ(0, memcmp(&addr_in, &result, kHalfSize));
  // Make sure the remaining half is untouched.
  EXPECT_TRUE(
      IsFilledByZero(reinterpret_cast<const char*>(&result) + kHalfSize,
                     sizeof(result) - kHalfSize));

  // Test with the buffer size of zero.
  memset(&result, 0, sizeof(result));
  result_len = 0;
  CopySocketAddress(
      storage, reinterpret_cast<sockaddr*>(&result), &result_len);
  EXPECT_EQ(SIZEOF_AS_SOCKLEN(addr_in), result_len);
  EXPECT_TRUE(IsFilledByZero(&result, sizeof(result)));

  // If result_len is 0, the second param can be NULL.
  memset(&result, 0, sizeof(result));
  result_len = 0;
  CopySocketAddress(storage, NULL, &result_len);
  EXPECT_EQ(SIZEOF_AS_SOCKLEN(addr_in), result_len);
}

TEST(SocketUtilTest, CopySocketAddressIPv6) {
  // Fake IPv6 address.
  sockaddr_in6 addr_in6 = {};
  addr_in6.sin6_family = AF_INET6;
  addr_in6.sin6_port = htons(54321);
  const in6_addr kDummyIPv6Addr = {{{
      0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
      0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
  }}};
  addr_in6.sin6_addr = kDummyIPv6Addr;

  sockaddr_storage storage = {};
  memcpy(&storage, &addr_in6, sizeof(addr_in6));

  sockaddr_storage result = {};
  socklen_t result_len;

  // Test with the buffer size equal to sockaddr_storage.
  result_len = sizeof(result);
  CopySocketAddress(
      storage, reinterpret_cast<sockaddr*>(&result), &result_len);
  EXPECT_EQ(SIZEOF_AS_SOCKLEN(addr_in6), result_len);
  EXPECT_EQ(0, memcmp(&addr_in6, &result, sizeof(addr_in6)));

  // Test with the buffer size equal to sockaddr_in6.
  memset(&result, 0, sizeof(result));
  result_len = sizeof(addr_in6);
  CopySocketAddress(
      storage, reinterpret_cast<sockaddr*>(&result), &result_len);
  EXPECT_EQ(SIZEOF_AS_SOCKLEN(addr_in6), result_len);
  EXPECT_EQ(0, memcmp(&addr_in6, &result, sizeof(addr_in6)));

  // Test with the buffer size smaller than sockaddr_in6.
  memset(&result, 0, sizeof(result));
  const socklen_t kHalfSize = sizeof(addr_in6) / 2;
  result_len = kHalfSize;
  CopySocketAddress(
      storage, reinterpret_cast<sockaddr*>(&result), &result_len);
  EXPECT_EQ(SIZEOF_AS_SOCKLEN(addr_in6), result_len);
  EXPECT_EQ(0, memcmp(&addr_in6, &result, kHalfSize));
  // Make sure the remaining half is untouched.
  EXPECT_TRUE(
      IsFilledByZero(reinterpret_cast<const char*>(&result) + kHalfSize,
                     sizeof(result) - kHalfSize));

  // Test with the buffer size of zero.
  memset(&result, 0, sizeof(result));
  result_len = 0;
  CopySocketAddress(
      storage, reinterpret_cast<sockaddr*>(&result), &result_len);
  EXPECT_EQ(SIZEOF_AS_SOCKLEN(addr_in6), result_len);
  EXPECT_TRUE(IsFilledByZero(&result, sizeof(result)));

  // If result_len is 0, the second param can be NULL.
  memset(&result, 0, sizeof(result));
  result_len = 0;
  CopySocketAddress(storage, NULL, &result_len);
  EXPECT_EQ(SIZEOF_AS_SOCKLEN(addr_in6), result_len);
}

TEST(SocketUtilTest, SocketAddressEqualIPv4) {
  sockaddr_storage addr1 = {};
  sockaddr_storage addr2 = {};

  addr1.ss_family = AF_INET;
  reinterpret_cast<sockaddr_in&>(addr1).sin_port = htons(8080);
  reinterpret_cast<sockaddr_in&>(addr1).sin_addr.s_addr =
      htonl(0x7F000001);  // 127.0.0.1
  memcpy(&addr2, &addr1, sizeof(sockaddr_storage));
  EXPECT_TRUE(SocketAddressEqual(addr1, addr2));

  // Not equal if family is different.
  addr2.ss_family = AF_UNIX;
  EXPECT_FALSE(SocketAddressEqual(addr1, addr2));
  addr2.ss_family = AF_UNSPEC;
  EXPECT_FALSE(SocketAddressEqual(addr1, addr2));
  addr2.ss_family = AF_INET6;
  EXPECT_FALSE(SocketAddressEqual(addr1, addr2));

  // Not equal if port is different.
  memcpy(&addr2, &addr1, sizeof(sockaddr_storage));
  reinterpret_cast<sockaddr_in&>(addr2).sin_port = htons(12345);
  EXPECT_FALSE(SocketAddressEqual(addr1, addr2));

  // Not equal if address is different.
  memcpy(&addr2, &addr1, sizeof(sockaddr_storage));
  reinterpret_cast<sockaddr_in&>(addr2).sin_addr.s_addr =
      htonl(0xC0A80001);  // 192.168.0.1
  EXPECT_FALSE(SocketAddressEqual(addr1, addr2));
}

TEST(SocketUtilTest, SocketAddressEqualIPv6) {
  sockaddr_storage addr1 = {};
  sockaddr_storage addr2 = {};

  addr1.ss_family = AF_INET6;
  reinterpret_cast<sockaddr_in6&>(addr1).sin6_port = htons(8080);
  const in6_addr kAddress = {{{
      0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
      0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
  }}};
  reinterpret_cast<sockaddr_in6&>(addr1).sin6_addr = kAddress;
  memcpy(&addr2, &addr1, sizeof(sockaddr_storage));
  EXPECT_TRUE(SocketAddressEqual(addr1, addr2));

  // Not equal if family is different.
  addr2.ss_family = AF_UNIX;
  EXPECT_FALSE(SocketAddressEqual(addr1, addr2));
  addr2.ss_family = AF_UNSPEC;
  EXPECT_FALSE(SocketAddressEqual(addr1, addr2));
  addr2.ss_family = AF_INET;
  EXPECT_FALSE(SocketAddressEqual(addr1, addr2));

  // Not equal if port is different.
  memcpy(&addr2, &addr1, sizeof(sockaddr_storage));
  reinterpret_cast<sockaddr_in6&>(addr2).sin6_port = htons(12345);
  EXPECT_FALSE(SocketAddressEqual(addr1, addr2));

  // Not equal if address is different.
  memcpy(&addr2, &addr1, sizeof(sockaddr_storage));
  const in6_addr kDifferentAddress = {{{
      0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10,
      0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10,
  }}};
  reinterpret_cast<sockaddr_in6&>(addr2).sin6_addr = kDifferentAddress;
  EXPECT_FALSE(SocketAddressEqual(addr1, addr2));
}

TEST(SocketUtilTest, StringToSockAddrStorageUnspec) {
  // Parse IPv4 address.
  sockaddr_storage storage;
  ASSERT_TRUE(StringToSockAddrStorage(
      "127.0.0.1", htons(22), AF_UNSPEC, false, &storage));
  {
    sockaddr_in saddr4 = {};
    saddr4.sin_family = AF_INET;
    saddr4.sin_port = htons(22);
    saddr4.sin_addr.s_addr = htonl(0x7F000001);
    EXPECT_EQ(0, memcmp(&storage, &saddr4, sizeof(saddr4)));
  }

  // Parse IPv6 address.
  memset(&storage, 0x5A, sizeof(storage));  // Fill garbled data.
  ASSERT_TRUE(StringToSockAddrStorage(
      "::1", htons(22), AF_UNSPEC, false, &storage));
  {
    sockaddr_in6 saddr6 = {};
    saddr6.sin6_family = AF_INET6;
    saddr6.sin6_port = htons(22);
    const in6_addr kExpectAddr = {{{
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1
    }}};
    saddr6.sin6_addr = kExpectAddr;
    EXPECT_EQ(0, memcmp(&storage, &saddr6, sizeof(saddr6)));
  }

  // allow_v4map is not effective for AF_UNSPEC.
  memset(&storage, 0x5A, sizeof(storage));  // Fill garbled data.
  ASSERT_TRUE(StringToSockAddrStorage(
      "127.0.0.1", htons(22), AF_UNSPEC, true, &storage));
  {
    sockaddr_in saddr4 = {};
    saddr4.sin_family = AF_INET;
    saddr4.sin_port = htons(22);
    saddr4.sin_addr.s_addr = htonl(0x7F000001);
    EXPECT_EQ(0, memcmp(&storage, &saddr4, sizeof(saddr4)));
  }

  // The address must form stringified IP.
  ASSERT_FALSE(StringToSockAddrStorage(
      "www.google.com", htons(80), AF_UNSPEC, false, &storage));
  ASSERT_FALSE(StringToSockAddrStorage(
      "localhost", htons(12345), AF_UNSPEC, false, &storage));
}

TEST(SocketUtilTest, StringToSockAddrStorageIPv4) {
  sockaddr_storage storage;
  ASSERT_TRUE(StringToSockAddrStorage(
      "127.0.0.1", htons(22), AF_INET, false, &storage));
  {
    sockaddr_in saddr4 = {};
    saddr4.sin_family = AF_INET;
    saddr4.sin_port = htons(22);
    saddr4.sin_addr.s_addr = htonl(0x7F000001);
    EXPECT_EQ(0, memcmp(&storage, &saddr4, sizeof(saddr4)));
  }

  // The address must form stringified IP.
  ASSERT_FALSE(StringToSockAddrStorage(
      "www.google.com", htons(80), AF_INET, false, &storage));
  ASSERT_FALSE(StringToSockAddrStorage(
      "localhost", htons(12345), AF_INET, false, &storage));

  // IPv6 address is not accepted.
  ASSERT_FALSE(StringToSockAddrStorage(
      "::1", htons(8080), AF_INET, false, &storage));
}

TEST(SocketUtilTest, StrAddrToSockAddrStorageIPv6) {
  // Parse IPv6 address.
  sockaddr_storage storage;
  ASSERT_TRUE(StringToSockAddrStorage(
      "::1", htons(22), AF_UNSPEC, false, &storage));
  {
    sockaddr_in6 saddr6 = {};
    saddr6.sin6_family = AF_INET6;
    saddr6.sin6_port = htons(22);
    const in6_addr kExpectAddr = {{{
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1
    }}};
    saddr6.sin6_addr = kExpectAddr;
    EXPECT_EQ(0, memcmp(&storage, &saddr6, sizeof(saddr6)));
  }

  // IPv4 address is not accepted, if allow_v4mapped is set false.
  ASSERT_FALSE(StringToSockAddrStorage(
      "127.0.0.1", htons(22), AF_INET6, false, &storage));

  // V4Mapped address is returend if allow_v4mapped is set true.
  memset(&storage, 0x5A, sizeof(storage));  // Fill garbled data.
  ASSERT_TRUE(StringToSockAddrStorage(
      "127.0.0.1", htons(22), AF_INET6, true, &storage));
  {
    sockaddr_in6 saddr6 = {};
    saddr6.sin6_family = AF_INET6;
    saddr6.sin6_port = htons(22);
    const in6_addr kExpectAddr = {{{
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0x7F, 0, 0, 1
    }}};
    saddr6.sin6_addr = kExpectAddr;
    EXPECT_EQ(0, memcmp(&storage, &saddr6, sizeof(saddr6)));
  }

  // The address must form stringified IP.
  ASSERT_FALSE(StringToSockAddrStorage(
      "ipv6.google.com", htons(80), AF_INET, false, &storage));
  ASSERT_FALSE(StringToSockAddrStorage(
      "ipv6.google.com", htons(80), AF_INET, true, &storage));
  ASSERT_FALSE(StringToSockAddrStorage(
      "localhost", htons(12345), AF_INET, false, &storage));
  ASSERT_FALSE(StringToSockAddrStorage(
      "localhost", htons(12345), AF_INET, true, &storage));
}

TEST(SocketUtilTest, ServiceNameToPort) {
  // Common use cases.
  EXPECT_EQ(HostToNetShort(0), ServiceNameToPort("0"));
  EXPECT_EQ(HostToNetShort(22), ServiceNameToPort("22"));
  EXPECT_EQ(HostToNetShort(80), ServiceNameToPort("80"));
  EXPECT_EQ(HostToNetShort(443), ServiceNameToPort("443"));
  EXPECT_EQ(HostToNetShort(8080), ServiceNameToPort("8080"));
  EXPECT_EQ(HostToNetShort(65535), ServiceNameToPort("65535"));

  // Returns 0 for NULL.
  EXPECT_EQ(0, ServiceNameToPort(NULL));

  // Out of range.
  EXPECT_EQ(0, ServiceNameToPort("-1"));
  EXPECT_EQ(0, ServiceNameToPort("65536"));
  EXPECT_EQ(0, ServiceNameToPort("1000000"));

  // Currently named-services are not supported.
  EXPECT_EQ(0, ServiceNameToPort("http"));
  EXPECT_EQ(0, ServiceNameToPort("https"));
  EXPECT_EQ(0, ServiceNameToPort("ftp"));
  EXPECT_EQ(0, ServiceNameToPort("ssh"));
}

TEST(SocketUtilTest, SockAddrStorageToAddrInfo) {
  {
    sockaddr_storage storage;
    ASSERT_TRUE(StringToSockAddrStorage(
        "127.0.0.1", htons(22), AF_UNSPEC, false, &storage));
    scoped_ptr<addrinfo, AddrInfoDeleter> info(SockAddrStorageToAddrInfo(
        storage, SOCK_STREAM, IPPROTO_IP, "localhost"));
    ASSERT_TRUE(info.get());
    EXPECT_EQ(0, info->ai_flags);
    EXPECT_EQ(AF_INET, info->ai_family);
    EXPECT_EQ(SOCK_STREAM, info->ai_socktype);
    EXPECT_EQ(IPPROTO_IP, info->ai_protocol);
    EXPECT_EQ(SIZEOF_AS_SOCKLEN(sockaddr_in), info->ai_addrlen);
    EXPECT_EQ(0, memcmp(&storage, info->ai_addr, info->ai_addrlen));
    EXPECT_STREQ("localhost", info->ai_canonname);
    EXPECT_EQ(NULL, info->ai_next);
  }

  {
    sockaddr_storage storage;
    ASSERT_TRUE(StringToSockAddrStorage(
        "::1", htons(22), AF_UNSPEC, false, &storage));
    scoped_ptr<addrinfo, AddrInfoDeleter> info(SockAddrStorageToAddrInfo(
        storage, SOCK_STREAM, IPPROTO_IP, "localhost"));
    ASSERT_TRUE(info.get());
    EXPECT_EQ(0, info->ai_flags);
    EXPECT_EQ(AF_INET6, info->ai_family);
    EXPECT_EQ(SOCK_STREAM, info->ai_socktype);
    EXPECT_EQ(IPPROTO_IP, info->ai_protocol);
    EXPECT_EQ(SIZEOF_AS_SOCKLEN(sockaddr_in6), info->ai_addrlen);
    EXPECT_EQ(0, memcmp(&storage, info->ai_addr, info->ai_addrlen));
    EXPECT_STREQ("localhost", info->ai_canonname);
    EXPECT_EQ(NULL, info->ai_next);
  }
}


TEST(SocketUtilTest, VerifyGetSocketOption) {
  char optval[10];
  socklen_t optlen = 10;

  // Typical usage.
  EXPECT_EQ(0, VerifyGetSocketOption(optval, &optlen));

  // NULL check for optval
  EXPECT_EQ(EFAULT, VerifyGetSocketOption(NULL, &optlen));

  // optlen can be 0. In that case, optval can be NULL.
  optlen = 0;
  EXPECT_EQ(0, VerifyGetSocketOption(optval, &optlen));
  EXPECT_EQ(0, VerifyGetSocketOption(NULL, &optlen));

  // NULL check for optlen.
  EXPECT_EQ(EFAULT, VerifyGetSocketOption(optval, NULL));
  EXPECT_EQ(EFAULT, VerifyGetSocketOption(NULL, NULL));

  optlen = -1;
  EXPECT_EQ(EINVAL, VerifyGetSocketOption(optval, &optlen));
  EXPECT_EQ(EINVAL, VerifyGetSocketOption(NULL, &optlen));
}

TEST(SocketUtilTest, VerifySetSocketOption) {
  char optval[10];

  // Typical usage.
  EXPECT_EQ(0, VerifySetSocketOption(optval, 4, 4));
  EXPECT_EQ(0, VerifySetSocketOption(optval, 8, 4));

  // If the buffer size is smaller than expected value, EINVAL is expected.
  EXPECT_EQ(EINVAL, VerifySetSocketOption(optval, 4, 8));

  // If optval is NULL, EFAULT is expected.
  EXPECT_EQ(EFAULT, VerifySetSocketOption(NULL, 4, 4));
}

TEST(SocketUtilTest, VerifyTimeoutSocketOption) {
  timeval t;

  // Typical usage.
  t.tv_sec = 1;
  t.tv_usec = 500;
  EXPECT_EQ(0, VerifyTimeoutSocketOption(t));
  t.tv_sec = 1;
  t.tv_usec = 0;
  EXPECT_EQ(0, VerifyTimeoutSocketOption(t));
  t.tv_sec = 0;
  t.tv_usec = 1000;
  EXPECT_EQ(0, VerifyTimeoutSocketOption(t));
  t.tv_sec = 0;
  t.tv_usec = 0;
  EXPECT_EQ(0, VerifyTimeoutSocketOption(t));

  // Negative value is allowed for tv_sec.
  t.tv_sec = -1;
  t.tv_usec = 0;
  EXPECT_EQ(0, VerifyTimeoutSocketOption(t));

  // tv_usec must be in the range of [0, 1000000).
  t.tv_sec = 0;
  t.tv_usec = -1;
  EXPECT_EQ(EDOM, VerifyTimeoutSocketOption(t));
  t.tv_usec = 0;
  EXPECT_EQ(0, VerifyTimeoutSocketOption(t));
  t.tv_usec = 999999;
  EXPECT_EQ(0, VerifyTimeoutSocketOption(t));
  t.tv_usec = 1000000;
  EXPECT_EQ(EDOM, VerifyTimeoutSocketOption(t));
}

TEST(SocketUtilTest, CopySocketOption) {
  const char kStorage[] = {1, 2, 3, 4};
  char optval[8] = {};
  socklen_t optlen = 0;

  memset(optval, 0x5A, sizeof(optval));
  optlen = 4;
  CopySocketOption(kStorage, sizeof(kStorage), optval, &optlen);
  {
    const char kExpectedOptval[] = {1, 2, 3, 4, 0x5A, 0x5A, 0x5A, 0x5A};
    EXPECT_EQ(0, memcmp(optval, kExpectedOptval, sizeof(kExpectedOptval)));
    EXPECT_EQ(4, static_cast<int>(optlen));
  }

  memset(optval, 0x5A, sizeof(optval));
  optlen = 8;
  CopySocketOption(kStorage, sizeof(kStorage), optval, &optlen);
  {
    const char kExpectedOptval[] = {1, 2, 3, 4, 0x5A, 0x5A, 0x5A, 0x5A};
    EXPECT_EQ(0, memcmp(optval, kExpectedOptval, sizeof(kExpectedOptval)));
    EXPECT_EQ(4, static_cast<int>(optlen));
  }

  memset(optval, 0x5A, sizeof(optval));
  optlen = 2;
  CopySocketOption(kStorage, sizeof(kStorage), optval, &optlen);
  {
    const char kExpectedOptval[] = {1, 2, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A};
    EXPECT_EQ(0, memcmp(optval, kExpectedOptval, sizeof(kExpectedOptval)));
    EXPECT_EQ(2, static_cast<int>(optlen));
  }

  // If optlen is 0, do nothing, espcially, optval can be null.
  memset(optval, 0x5A, sizeof(optval));
  optlen = 0;
  CopySocketOption(kStorage, sizeof(kStorage), NULL, &optlen);
  EXPECT_EQ(0, static_cast<int>(optlen));
}

}  // namespace internal
}  // namespace posix_translation
