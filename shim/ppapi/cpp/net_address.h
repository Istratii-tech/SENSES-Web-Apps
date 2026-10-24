// Value-type replacement for pp::NetAddress (no browser resource behind it).
// Lets ARC's socket_util.cc compile unchanged and doubles as the address type
// for a future WebSocket/WebTransport network backend. Part of the arc-wasm
// shim layer.
#ifndef PPAPI_CPP_NET_ADDRESS_H_
#define PPAPI_CPP_NET_ADDRESS_H_

#include <stdint.h>
#include <string.h>

#include "ppapi/c/pp_types.h"
#include "ppapi/cpp/instance.h"

typedef enum {
  PP_NETADDRESS_FAMILY_UNSPECIFIED = 0,
  PP_NETADDRESS_FAMILY_IPV4 = 1,
  PP_NETADDRESS_FAMILY_IPV6 = 2,
} PP_NetAddress_Family;

struct PP_NetAddress_IPv4 {
  uint16_t port;     // Network byte order.
  uint8_t addr[4];   // Network byte order.
};

struct PP_NetAddress_IPv6 {
  uint16_t port;     // Network byte order.
  uint8_t addr[16];  // Network byte order.
};

namespace pp {

class NetAddress {
 public:
  NetAddress() : family_(PP_NETADDRESS_FAMILY_UNSPECIFIED) {
    memset(&v4_, 0, sizeof(v4_));
    memset(&v6_, 0, sizeof(v6_));
  }
  NetAddress(const InstanceHandle&, const PP_NetAddress_IPv4& ipv4) : NetAddress() {
    family_ = PP_NETADDRESS_FAMILY_IPV4;
    v4_ = ipv4;
  }
  NetAddress(const InstanceHandle&, const PP_NetAddress_IPv6& ipv6) : NetAddress() {
    family_ = PP_NETADDRESS_FAMILY_IPV6;
    v6_ = ipv6;
  }

  bool is_null() const { return family_ == PP_NETADDRESS_FAMILY_UNSPECIFIED; }
  PP_NetAddress_Family GetFamily() const { return family_; }

  bool DescribeAsIPv4Address(PP_NetAddress_IPv4* out) const {
    if (family_ != PP_NETADDRESS_FAMILY_IPV4) return false;
    *out = v4_;
    return true;
  }
  bool DescribeAsIPv6Address(PP_NetAddress_IPv6* out) const {
    if (family_ != PP_NETADDRESS_FAMILY_IPV6) return false;
    *out = v6_;
    return true;
  }

 private:
  PP_NetAddress_Family family_;
  PP_NetAddress_IPv4 v4_;
  PP_NetAddress_IPv6 v6_;
};

}  // namespace pp

#endif  // PPAPI_CPP_NET_ADDRESS_H_
