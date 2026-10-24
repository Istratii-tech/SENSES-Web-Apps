// Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <string.h>

#include "base/compiler_specific.h"
#include "base/memory/scoped_ptr.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "ppapi_mocks/ppb_host_resolver.h"
#include "ppapi_mocks/ppb_net_address.h"
#include "posix_translation/dir.h"
#include "posix_translation/test_util/file_system_background_test_common.h"
#include "posix_translation/test_util/virtual_file_system_test_common.h"
#include "posix_translation/virtual_file_system.h"

using ::testing::DoAll;
using ::testing::ElementsAreArray;
using ::testing::Eq;
using ::testing::NotNull;
using ::testing::SetArgPointee;

namespace posix_translation {

// This class is used to test host resolution functions in VirtualFileSystem
// such as getaddrinfo().
class FileSystemHostResolverTest
    : public FileSystemBackgroundTestCommon<FileSystemHostResolverTest> {
 public:
  FileSystemHostResolverTest() {
  }

  DECLARE_BACKGROUND_TEST(TestGetAddrInfoIPv4);
  DECLARE_BACKGROUND_TEST(TestGetAddrInfoIPv4NumberNullHint);
  DECLARE_BACKGROUND_TEST(TestGetAddrInfoIPv4NumberAF_INET);
  DECLARE_BACKGROUND_TEST(TestGetAddrInfoIPv4NumberAF_UNSPEC);
  DECLARE_BACKGROUND_TEST(TestGetAddrInfoIPv4NumberAF_INET6);
  DECLARE_BACKGROUND_TEST(TestGetAddrInfoIPv6);
  DECLARE_BACKGROUND_TEST(TestGetAddrInfoIPv6NumberNullHint);
  DECLARE_BACKGROUND_TEST(TestGetAddrInfoIPv6NumberAF_INET6);
  DECLARE_BACKGROUND_TEST(TestGetAddrInfoIPv6NumberAF_UNSPEC);
  // TODO(crbug.com/247201): Add tests for failure cases for the various API's
  // invoked by the getaddrinfo() implementation.

 protected:
  typedef FileSystemBackgroundTestCommon<FileSystemHostResolverTest>
      CommonType;

  static const int kResolverResource = 191;
  static const int kNetAddressResource = 192;

  virtual void SetUp() OVERRIDE {
    CommonType::SetUp();
    factory_.GetMock(&ppb_host_resolver_);
    factory_.GetMock(&ppb_netaddress_);
  }

  void ExpectResolve(const char* expected_hostname, uint16_t expected_port) {
    EXPECT_CALL(*ppb_host_resolver_, Create(kInstanceNumber)).
        WillOnce(Return(kResolverResource));
    // We only support blocking call.
    EXPECT_CALL(*ppb_host_resolver_,
                Resolve(kResolverResource,
                        expected_hostname,
                        expected_port, NotNull(), _)).
        WillOnce(Return(static_cast<int32_t>(PP_OK)));
  }

  void ExpectGetCanonicalName(const char* returned_hostname) {
    EXPECT_CALL(*ppb_host_resolver_,
                GetCanonicalName(kResolverResource)).
        WillOnce(Return(pp::Var(returned_hostname).pp_var()));
  }

  void ExpectGetNetAddressCount(int size) {
    EXPECT_CALL(*ppb_host_resolver_,
                GetNetAddressCount(kResolverResource)).WillOnce(Return(size));
  }

  void ExpectGetNetAddressIPv4(int index, uint16_t returned_port,
                               const struct in_addr& returned_addr) {
    EXPECT_CALL(*ppb_host_resolver_, GetNetAddress(kResolverResource, index)).
        WillOnce(Return(kNetAddressResource));

    // Setup IPv4 NetAddress instance.
    PP_NetAddress_IPv4 ipv4_addr = {};
    ipv4_addr.port = returned_port;
    memcpy(ipv4_addr.addr, &returned_addr, sizeof(ipv4_addr.addr));

    EXPECT_CALL(*ppb_netaddress_, GetFamily(kNetAddressResource)).
        WillRepeatedly(Return(PP_NETADDRESS_FAMILY_IPV4));
    EXPECT_CALL(*ppb_netaddress_,
                DescribeAsIPv4Address(kNetAddressResource, _)).
        WillRepeatedly(DoAll(SetArgPointee<1>(ipv4_addr), Return(PP_TRUE)));
  }

  void ExpectGetNetAddressIPv6(int index, uint16_t returned_port,
                               const struct in6_addr& returned_addr) {
    EXPECT_CALL(*ppb_host_resolver_, GetNetAddress(kResolverResource, index)).
        WillOnce(Return(kNetAddressResource));

    // Setup IPv6 NetAddress instance.
    PP_NetAddress_IPv6 ipv6_addr = {};
    ipv6_addr.port = returned_port;
    memcpy(ipv6_addr.addr, &returned_addr, sizeof(ipv6_addr.addr));

    EXPECT_CALL(*ppb_netaddress_, GetFamily(kNetAddressResource)).
        WillRepeatedly(Return(PP_NETADDRESS_FAMILY_IPV6));
    EXPECT_CALL(*ppb_netaddress_,
                DescribeAsIPv6Address(kNetAddressResource, _)).
        WillRepeatedly(DoAll(SetArgPointee<1>(ipv6_addr), Return(PP_TRUE)));
  }

 private:
  ::testing::NiceMock<PPB_HostResolver_Mock>* ppb_host_resolver_;
  ::testing::NiceMock<PPB_NetAddress_Mock>* ppb_netaddress_;
};

TEST_BACKGROUND_F(FileSystemHostResolverTest, TestGetAddrInfoIPv4) {
  ExpectResolve("example.com", 0);
  ExpectGetCanonicalName("resolve.example.com");
  ExpectGetNetAddressCount(1);
  const in_addr kReturnAddr = {0x12345678};
  ExpectGetNetAddressIPv4(0, 101, kReturnAddr);

  addrinfo* res = NULL;
  errno = 0;
  EXPECT_EQ(0, file_system_->getaddrinfo("example.com", NULL, NULL, &res));
  EXPECT_EQ(0, errno);
  EXPECT_EQ(0, res[0].ai_flags);
  EXPECT_EQ(AF_INET, res[0].ai_family);
  EXPECT_EQ(SOCK_STREAM, res[0].ai_socktype);
  EXPECT_EQ(0, res[0].ai_protocol);
  // socklen_t is signed in bionic so we need a cast.
  EXPECT_EQ(static_cast<socklen_t>(sizeof(struct sockaddr_in)),
            res[0].ai_addrlen);
  EXPECT_STREQ("resolve.example.com", res[0].ai_canonname);
  struct sockaddr_in* addr = reinterpret_cast<struct sockaddr_in*>(
      res[0].ai_addr);
  EXPECT_EQ(AF_INET, addr->sin_family);
  EXPECT_EQ(101, addr->sin_port);
  EXPECT_EQ(kReturnAddr.s_addr, addr->sin_addr.s_addr);

  file_system_->freeaddrinfo(res);
  res = NULL;
}

TEST_BACKGROUND_F(FileSystemHostResolverTest,
                  TestGetAddrInfoIPv4NumberNullHint) {
  const in_addr kReturnAddr = {htonl(0x7F000001)};

  // getaddrinfo with no hint.
  addrinfo* res = NULL;
  errno = 0;
  ASSERT_EQ(0, file_system_->getaddrinfo("127.0.0.1", NULL, NULL, &res));
  EXPECT_EQ(0, errno);
  EXPECT_EQ(0, res->ai_flags);
  EXPECT_EQ(AF_INET, res->ai_family);
  EXPECT_EQ(SOCK_STREAM, res->ai_socktype);
  EXPECT_EQ(0, res->ai_protocol);
  EXPECT_EQ(static_cast<socklen_t>(sizeof(struct sockaddr_in)),
            res->ai_addrlen);
  struct sockaddr_in* addr =
      reinterpret_cast<struct sockaddr_in*>(res->ai_addr);
  ASSERT_TRUE(addr);
  EXPECT_EQ(AF_INET, addr->sin_family);
  EXPECT_EQ(0, addr->sin_port);
  EXPECT_EQ(kReturnAddr.s_addr, addr->sin_addr.s_addr);

  file_system_->freeaddrinfo(res);
}

TEST_BACKGROUND_F(FileSystemHostResolverTest,
                  TestGetAddrInfoIPv4NumberAF_INET) {
  const in_addr kReturnAddr = {htonl(0x7F000001)};

  addrinfo* res = NULL;
  addrinfo hint;
  memset(&hint, 0, sizeof(hint));
  hint.ai_family = AF_INET;
  errno = 0;
  ASSERT_EQ(0, file_system_->getaddrinfo("127.0.0.1", NULL, &hint, &res));
  EXPECT_EQ(0, errno);
  EXPECT_EQ(0, res->ai_flags);
  EXPECT_EQ(AF_INET, res->ai_family);
  EXPECT_EQ(SOCK_STREAM, res->ai_socktype);
  EXPECT_EQ(0, res->ai_protocol);
  EXPECT_EQ(static_cast<socklen_t>(sizeof(struct sockaddr_in)),
            res->ai_addrlen);
  struct sockaddr_in* addr =
      reinterpret_cast<struct sockaddr_in*>(res->ai_addr);
  ASSERT_TRUE(addr);
  EXPECT_EQ(AF_INET, addr->sin_family);
  EXPECT_EQ(0, addr->sin_port);
  EXPECT_EQ(kReturnAddr.s_addr, addr->sin_addr.s_addr);

  file_system_->freeaddrinfo(res);
}

TEST_BACKGROUND_F(FileSystemHostResolverTest,
                  TestGetAddrInfoIPv4NumberAF_UNSPEC) {
  const in_addr kReturnAddr = {htonl(0x7F000001)};

  addrinfo* res = NULL;
  addrinfo hint;
  memset(&hint, 0, sizeof(hint));
  hint.ai_family = AF_UNSPEC;
  errno = 0;
  ASSERT_EQ(0, file_system_->getaddrinfo("127.0.0.1", NULL, &hint, &res));
  EXPECT_EQ(0, errno);
  EXPECT_EQ(0, res->ai_flags);
  EXPECT_EQ(AF_INET, res->ai_family);
  EXPECT_EQ(SOCK_STREAM, res->ai_socktype);
  EXPECT_EQ(0, res->ai_protocol);
  EXPECT_EQ(static_cast<socklen_t>(sizeof(struct sockaddr_in)),
            res->ai_addrlen);
  struct sockaddr_in* addr =
      reinterpret_cast<struct sockaddr_in*>(res->ai_addr);
  ASSERT_TRUE(addr);
  EXPECT_EQ(AF_INET, addr->sin_family);
  EXPECT_EQ(0, addr->sin_port);
  EXPECT_EQ(kReturnAddr.s_addr, addr->sin_addr.s_addr);

  file_system_->freeaddrinfo(res);
}

TEST_BACKGROUND_F(FileSystemHostResolverTest,
                  TestGetAddrInfoIPv4NumberAF_INET6) {
  const in6_addr kReturnAddr = {{{
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 127, 0, 0, 1}}};

  addrinfo* res = NULL;
  addrinfo hint;
  memset(&hint, 0, sizeof(hint));
  hint.ai_family = AF_INET6;
  hint.ai_flags = AI_V4MAPPED;
  errno = 0;
  ASSERT_EQ(0, file_system_->getaddrinfo("127.0.0.1", NULL, &hint, &res));
  EXPECT_EQ(0, errno);
  EXPECT_EQ(0, res->ai_flags);
  EXPECT_EQ(AF_INET6, res->ai_family);
  EXPECT_EQ(SOCK_STREAM, res->ai_socktype);
  EXPECT_EQ(0, res->ai_protocol);
  EXPECT_EQ(static_cast<socklen_t>(sizeof(struct sockaddr_in6)),
            res->ai_addrlen);
  struct sockaddr_in6* addr =
      reinterpret_cast<struct sockaddr_in6*>(res->ai_addr);
  ASSERT_TRUE(addr);
  EXPECT_EQ(AF_INET6, addr->sin6_family);
  EXPECT_EQ(0, addr->sin6_port);
  EXPECT_THAT(addr->sin6_addr.s6_addr, ElementsAreArray(kReturnAddr.s6_addr));

  file_system_->freeaddrinfo(res);
}

TEST_BACKGROUND_F(FileSystemHostResolverTest, TestGetAddrInfoIPv6) {
  ExpectResolve("example.com", 0);
  ExpectGetCanonicalName("resolve.example.com");
  ExpectGetNetAddressCount(1);
  const in6_addr kReturnAddr = {{{
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}}};
  ExpectGetNetAddressIPv6(0, 101, kReturnAddr);

  addrinfo* res = NULL;
  errno = 0;
  EXPECT_EQ(0, file_system_->getaddrinfo("example.com", NULL, NULL, &res));
  EXPECT_EQ(0, errno);
  EXPECT_EQ(0, res[0].ai_flags);
  EXPECT_EQ(AF_INET6, res[0].ai_family);
  EXPECT_EQ(SOCK_STREAM, res[0].ai_socktype);
  EXPECT_EQ(0, res[0].ai_protocol);
  // socklen_t is signed in bionic so we need a cast.
  EXPECT_EQ(static_cast<socklen_t>(sizeof(struct sockaddr_in6)),
            res[0].ai_addrlen);
  EXPECT_STREQ("resolve.example.com", res[0].ai_canonname);
  struct sockaddr_in6* addr = reinterpret_cast<struct sockaddr_in6*>(
      res[0].ai_addr);
  EXPECT_EQ(AF_INET6, addr->sin6_family);
  EXPECT_EQ(101, addr->sin6_port);
  EXPECT_THAT(addr->sin6_addr.s6_addr, ElementsAreArray(kReturnAddr.s6_addr));

  file_system_->freeaddrinfo(res);
}

TEST_BACKGROUND_F(FileSystemHostResolverTest,
                  TestGetAddrInfoIPv6NumberNullHint) {
  const in6_addr kReturnAddr = {{{
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}}};

  // getaddrinfo with no hint.
  addrinfo* res = NULL;
  errno = 0;
  ASSERT_EQ(0, file_system_->getaddrinfo(
      "1:203:405:607:809:A0B:C0D:E0F", NULL, NULL, &res));
  EXPECT_EQ(0, errno);
  EXPECT_EQ(0, res->ai_flags);
  EXPECT_EQ(AF_INET6, res->ai_family);
  EXPECT_EQ(SOCK_STREAM, res->ai_socktype);
  EXPECT_EQ(0, res->ai_protocol);
  EXPECT_EQ(static_cast<socklen_t>(sizeof(struct sockaddr_in6)),
            res->ai_addrlen);
  struct sockaddr_in6* addr =
      reinterpret_cast<struct sockaddr_in6*>(res->ai_addr);
  ASSERT_TRUE(addr);
  EXPECT_EQ(AF_INET6, addr->sin6_family);
  EXPECT_EQ(0, addr->sin6_port);
  EXPECT_THAT(addr->sin6_addr.s6_addr, ElementsAreArray(kReturnAddr.s6_addr));

  file_system_->freeaddrinfo(res);
}

TEST_BACKGROUND_F(FileSystemHostResolverTest,
                  TestGetAddrInfoIPv6NumberAF_INET6) {
  const in6_addr kReturnAddr = {{{
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}}};

  addrinfo* res = NULL;
  addrinfo hint;
  memset(&hint, 0, sizeof(hint));
  hint.ai_family = AF_INET6;
  errno = 0;
  ASSERT_EQ(0, file_system_->getaddrinfo(
      "1:203:405:607:809:A0B:C0D:E0F", NULL, &hint, &res));
  EXPECT_EQ(0, errno);
  EXPECT_EQ(0, res->ai_flags);
  EXPECT_EQ(AF_INET6, res->ai_family);
  EXPECT_EQ(SOCK_STREAM, res->ai_socktype);
  EXPECT_EQ(0, res->ai_protocol);
  EXPECT_EQ(static_cast<socklen_t>(sizeof(struct sockaddr_in6)),
            res->ai_addrlen);
  struct sockaddr_in6* addr =
      reinterpret_cast<struct sockaddr_in6*>(res->ai_addr);
  ASSERT_TRUE(addr);
  EXPECT_EQ(AF_INET6, addr->sin6_family);
  EXPECT_EQ(0, addr->sin6_port);
  EXPECT_THAT(addr->sin6_addr.s6_addr, ElementsAreArray(kReturnAddr.s6_addr));

  file_system_->freeaddrinfo(res);
}

TEST_BACKGROUND_F(FileSystemHostResolverTest,
                  TestGetAddrInfoIPv6NumberAF_UNSPEC) {
  const in6_addr kReturnAddr = {{{
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}}};

  addrinfo* res = NULL;
  addrinfo hint;
  memset(&hint, 0, sizeof(hint));
  hint.ai_family = AF_UNSPEC;
  errno = 0;
  ASSERT_EQ(0, file_system_->getaddrinfo(
      "1:203:405:607:809:A0B:C0D:E0F", NULL, &hint, &res));
  EXPECT_EQ(0, errno);
  EXPECT_EQ(0, res->ai_flags);
  EXPECT_EQ(AF_INET6, res->ai_family);
  EXPECT_EQ(SOCK_STREAM, res->ai_socktype);
  EXPECT_EQ(0, res->ai_protocol);
  EXPECT_EQ(static_cast<socklen_t>(sizeof(struct sockaddr_in6)),
            res->ai_addrlen);
  struct sockaddr_in6* addr =
      reinterpret_cast<struct sockaddr_in6*>(res->ai_addr);
  ASSERT_TRUE(addr);
  EXPECT_EQ(AF_INET6, addr->sin6_family);
  EXPECT_EQ(0, addr->sin6_port);
  EXPECT_THAT(addr->sin6_addr.s6_addr, ElementsAreArray(kReturnAddr.s6_addr));

  file_system_->freeaddrinfo(res);
}

}  // namespace posix_translation
