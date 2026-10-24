// The network interfaces the kernel reports (see net_bridge.h): "lo"
// (127.0.0.1/8, ::1) and "eth0" (10.0.2.15/24 with a default route via
// 10.0.2.2, as in the Android emulator). There is no real interface behind
// eth0: connections leave through the relay's host. They are visible as
//   /sys/class/net/<if>/{ifindex,mtu,flags,operstate,address}
//   /proc/net/{if_inet6,route,dev}      (libcore's NetworkInterface reads these)
//   ioctl(SIOCGIF*) on an inet socket  (NetworkInterface, if_nametoindex)
//   getifaddrs()/freeifaddrs(), if_nametoindex()/if_indextoname()
//                                       (defined here, replacing musl's)
// Part of the arc-wasm shim layer.

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <string>

#include "posix_translation/host_file.h"
#include "posix_translation/net_bridge.h"
#include "posix_translation/virtual_file_system.h"

namespace posix_translation {
namespace net {

namespace {

struct Interface {
  const char* name;
  int index;
  int flags;
  int mtu;
  const char* addr;
  const char* netmask;
  const char* broadcast;  // NULL: none
  const char* addr6;      // /proc/net/if_inet6 form, NULL: none
  int prefix6;
  int scope6;
  unsigned char hwaddr[6];
};

const Interface kInterfaces[] = {
  {"lo", 1, IFF_UP | IFF_LOOPBACK | IFF_RUNNING, 65536, "127.0.0.1",
   "255.0.0.0", NULL, "00000000000000000000000000000001", 128, 0x10,
   {0, 0, 0, 0, 0, 0}},
  {"eth0", 2, IFF_UP | IFF_BROADCAST | IFF_RUNNING | IFF_MULTICAST, 1500,
   "10.0.2.15", "255.255.255.0", "10.0.2.255", NULL, 0, 0,
   {0x02, 0, 0, 0, 0, 0x01}},
};
const int kInterfaceCount = sizeof(kInterfaces) / sizeof(kInterfaces[0]);

const Interface* Find(const char* name) {
  for (int i = 0; i < kInterfaceCount; ++i) {
    if (strncmp(name, kInterfaces[i].name, IFNAMSIZ) == 0)
      return &kInterfaces[i];
  }
  return NULL;
}

void SetInet(sockaddr* sa, const char* addr) {
  sockaddr_in* sin = reinterpret_cast<sockaddr_in*>(sa);
  memset(sin, 0, sizeof(*sin));
  sin->sin_family = AF_INET;
  if (addr)
    inet_pton(AF_INET, addr, &sin->sin_addr);
}

bool WriteFile(const std::string& path, const std::string& content) {
  FILE* f = fopen(path.c_str(), "w");
  if (!f)
    return false;
  fwrite(content.data(), 1, content.size(), f);
  fclose(f);
  return true;
}

void MakeDirs(const std::string& path) {
  for (size_t i = 1; i <= path.size(); ++i) {
    if (i == path.size() || path[i] == '/')
      mkdir(path.substr(0, i).c_str(), 0755);
  }
}

}  // namespace

int InterfaceIoctl(int request, void* arg, bool* handled) {
  *handled = true;
  if (request == SIOCGIFCONF) {
    ifconf* conf = static_cast<ifconf*>(arg);
    const int room = conf->ifc_len / static_cast<int>(sizeof(ifreq));
    int n = 0;
    for (int i = 0; i < kInterfaceCount; ++i) {
      if (!kInterfaces[i].addr)
        continue;
      if (conf->ifc_req && n < room) {
        ifreq* r = &conf->ifc_req[n];
        memset(r, 0, sizeof(*r));
        strncpy(r->ifr_name, kInterfaces[i].name, IFNAMSIZ - 1);
        SetInet(&r->ifr_addr, kInterfaces[i].addr);
      }
      ++n;
    }
    conf->ifc_len = (conf->ifc_req ? std::min(n, room) : n) * sizeof(ifreq);
    return 0;
  }
  switch (request) {
    case SIOCGIFFLAGS: case SIOCGIFADDR: case SIOCGIFNETMASK:
    case SIOCGIFBRDADDR: case SIOCGIFDSTADDR: case SIOCGIFINDEX:
    case SIOCGIFMTU: case SIOCGIFHWADDR: case SIOCGIFNAME:
      break;
    default:
      *handled = false;
      return 0;
  }
  ifreq* r = static_cast<ifreq*>(arg);
  if (!r) {
    errno = EFAULT;
    return -1;
  }
  const Interface* iface = NULL;
  if (request == SIOCGIFNAME) {
    for (int i = 0; i < kInterfaceCount; ++i) {
      if (kInterfaces[i].index == r->ifr_ifindex)
        iface = &kInterfaces[i];
    }
    if (!iface) {
      errno = ENODEV;
      return -1;
    }
    memset(r->ifr_name, 0, IFNAMSIZ);
    strncpy(r->ifr_name, iface->name, IFNAMSIZ - 1);
    return 0;
  }
  iface = Find(r->ifr_name);
  if (!iface) {
    errno = ENODEV;
    return -1;
  }
  switch (request) {
    case SIOCGIFFLAGS:
      r->ifr_flags = iface->flags;
      return 0;
    case SIOCGIFINDEX:
      r->ifr_ifindex = iface->index;
      return 0;
    case SIOCGIFMTU:
      r->ifr_mtu = iface->mtu;
      return 0;
    case SIOCGIFHWADDR:
      memset(&r->ifr_hwaddr, 0, sizeof(r->ifr_hwaddr));
      r->ifr_hwaddr.sa_family =
          (iface->flags & IFF_LOOPBACK) ? ARPHRD_LOOPBACK : ARPHRD_ETHER;
      memcpy(r->ifr_hwaddr.sa_data, iface->hwaddr, 6);
      return 0;
  }
  // Addresses.
  if (!iface->addr) {
    errno = EADDRNOTAVAIL;
    return -1;
  }
  switch (request) {
    case SIOCGIFADDR:
    case SIOCGIFDSTADDR:
      SetInet(&r->ifr_addr, iface->addr);
      return 0;
    case SIOCGIFNETMASK:
      SetInet(&r->ifr_netmask, iface->netmask);
      return 0;
    case SIOCGIFBRDADDR:
      SetInet(&r->ifr_broadaddr, iface->broadcast);  // 0.0.0.0 if none
      return 0;
  }
  errno = EINVAL;
  return -1;
}

void MountNetworkFiles(VirtualFileSystem* vfs, const std::string& host_root) {
  char buf[256];
  std::string if_inet6, dev =
      "Inter-|   Receive                                                |  Transmit\n"
      " face |bytes    packets errs drop fifo frame compressed multicast|"
      "bytes    packets errs drop fifo colls carrier compressed\n";
  for (int i = 0; i < kInterfaceCount; ++i) {
    const Interface& f = kInterfaces[i];
    const std::string dir = host_root + "/sys/class/net/" + f.name;
    MakeDirs(dir);
    snprintf(buf, sizeof(buf), "%d\n", f.index);
    WriteFile(dir + "/ifindex", buf);
    snprintf(buf, sizeof(buf), "%d\n", f.mtu);
    WriteFile(dir + "/mtu", buf);
    snprintf(buf, sizeof(buf), "0x%x\n", f.flags);
    WriteFile(dir + "/flags", buf);
    WriteFile(dir + "/operstate",
              (f.flags & IFF_LOOPBACK) ? "unknown\n" : "up\n");
    snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x\n",
             f.hwaddr[0], f.hwaddr[1], f.hwaddr[2], f.hwaddr[3], f.hwaddr[4],
             f.hwaddr[5]);
    WriteFile(dir + "/address", buf);
    if (f.addr6) {
      snprintf(buf, sizeof(buf), "%s %02x %02x %02x %02x %8s\n", f.addr6,
               f.index, f.prefix6, f.scope6, 0x80, f.name);
      if_inet6 += buf;
    }
    snprintf(buf, sizeof(buf), "%6s: %7d %7d %4d %4d %4d %5d %10d %9d "
             "%8d %7d %4d %4d %4d %5d %7d %10d\n", f.name,
             0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    dev += buf;
  }
  MakeDirs(host_root + "/proc/net");
  WriteFile(host_root + "/proc/net/if_inet6", if_inet6);
  WriteFile(host_root + "/proc/net/dev", dev);
  // Default route via 10.0.2.2 on eth0 (fields are little-endian hex).
  WriteFile(host_root + "/proc/net/route",
            "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\t"
            "Mask\t\tMTU\tWindow\tIRTT\n"
            "eth0\t00000000\t0202000A\t0003\t0\t0\t0\t00000000\t0\t0\t0\n"
            "eth0\t0002000A\t00000000\t0001\t0\t0\t0\t00FFFFFF\t0\t0\t0\n");
  vfs->Mount("/sys/class/net/", new HostFileHandler(host_root));
  vfs->Mount("/proc/net/", new HostFileHandler(host_root));
}

}  // namespace net
}  // namespace posix_translation

extern "C" {

int getifaddrs(ifaddrs** out) {
  using posix_translation::net::kInterfaces;
  using posix_translation::net::kInterfaceCount;
  // One block per entry: the ifaddrs and its three addresses.
  struct Entry {
    ifaddrs ifa;
    sockaddr_storage addr, netmask, broadcast;
    char name[IFNAMSIZ];
  };
  ifaddrs* head = NULL;
  ifaddrs** tail = &head;
  for (int i = 0; i < kInterfaceCount; ++i) {
    const posix_translation::net::Interface& f = kInterfaces[i];
    for (int v6 = 0; v6 < 2; ++v6) {
      if (v6 ? !f.addr6 : !f.addr)
        continue;
      Entry* e = static_cast<Entry*>(calloc(1, sizeof(Entry)));
      if (!e) {
        freeifaddrs(head);
        errno = ENOMEM;
        return -1;
      }
      strncpy(e->name, f.name, IFNAMSIZ - 1);
      e->ifa.ifa_name = e->name;
      e->ifa.ifa_flags = f.flags;
      if (v6) {
        sockaddr_in6* a = reinterpret_cast<sockaddr_in6*>(&e->addr);
        sockaddr_in6* m = reinterpret_cast<sockaddr_in6*>(&e->netmask);
        a->sin6_family = m->sin6_family = AF_INET6;
        for (int b = 0; b < 16; ++b) {
          unsigned byte = 0;
          sscanf(f.addr6 + 2 * b, "%2x", &byte);
          a->sin6_addr.s6_addr[b] = byte;
          const int bits = f.prefix6 - 8 * b;
          m->sin6_addr.s6_addr[b] =
              bits >= 8 ? 0xff : bits <= 0 ? 0 : (0xff << (8 - bits)) & 0xff;
        }
      } else {
        posix_translation::net::SetInet(reinterpret_cast<sockaddr*>(&e->addr),
                                        f.addr);
        posix_translation::net::SetInet(
            reinterpret_cast<sockaddr*>(&e->netmask), f.netmask);
        if (f.broadcast) {
          posix_translation::net::SetInet(
              reinterpret_cast<sockaddr*>(&e->broadcast), f.broadcast);
          e->ifa.ifa_broadaddr = reinterpret_cast<sockaddr*>(&e->broadcast);
        }
      }
      e->ifa.ifa_addr = reinterpret_cast<sockaddr*>(&e->addr);
      e->ifa.ifa_netmask = reinterpret_cast<sockaddr*>(&e->netmask);
      *tail = &e->ifa;
      tail = &e->ifa.ifa_next;
    }
  }
  *out = head;
  return 0;
}

// musl asks an AF_UNIX socket, which posix_translation's LocalSocket does
// not answer; look the table up directly.
unsigned if_nametoindex(const char* name) {
  const posix_translation::net::Interface* f =
      posix_translation::net::Find(name);
  return f ? f->index : 0;
}

char* if_indextoname(unsigned index, char* name) {
  using posix_translation::net::kInterfaces;
  for (int i = 0; i < posix_translation::net::kInterfaceCount; ++i) {
    if (static_cast<unsigned>(kInterfaces[i].index) == index) {
      strncpy(name, kInterfaces[i].name, IF_NAMESIZE);
      return name;
    }
  }
  errno = ENXIO;
  return NULL;
}

void freeifaddrs(ifaddrs* ifa) {
  while (ifa) {
    ifaddrs* next = ifa->ifa_next;
    free(ifa);  // The Entry block starts with the ifaddrs.
    ifa = next;
  }
}

}  // extern "C"
