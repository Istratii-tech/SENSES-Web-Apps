/* Copyright 2014 The Chromium Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Simple wrappers for various socket calls.
 */

#include <errno.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "common/arc_strace.h"
#include "common/danger.h"
#include "common/export.h"
#include "common/irt_wrapper_util.h"
#include "posix_translation/virtual_file_system.h"

extern "C" {
ARC_EXPORT int __wrap_accept(
    int sockfd, struct sockaddr* addr, socklen_t* addrlen);
ARC_EXPORT int __wrap_bind(
    int sockfd, const struct sockaddr* addr, socklen_t addrlen);
ARC_EXPORT int __wrap_connect(
    int sockfd, const struct sockaddr* addr,
    socklen_t addrlen);
ARC_EXPORT int __wrap_epoll_create(int size);
ARC_EXPORT int __wrap_epoll_ctl(
    int epfd, int op, int fd, struct epoll_event* event);
ARC_EXPORT int __wrap_epoll_wait(
    int epfd, struct epoll_event* events, int maxevents, int timeout);
ARC_EXPORT int __wrap_epoll_pwait(
    int epfd, struct epoll_event* events, int maxevents, int timeout,
    const sigset_t *sigmask);
ARC_EXPORT void __wrap_freeaddrinfo(struct addrinfo* res);
ARC_EXPORT const char* __wrap_gai_strerror(int errcode);
ARC_EXPORT int __wrap_getaddrinfo(
    const char* node, const char* service,
    const struct addrinfo* hints, struct addrinfo** res);
ARC_EXPORT struct hostent* __wrap_gethostbyaddr(
    const void* addr, socklen_t len, int type);
ARC_EXPORT struct hostent* __wrap_gethostbyname(const char* hostname);
ARC_EXPORT struct hostent* __wrap_gethostbyname2(
    const char* hostname, int family);
ARC_EXPORT int __wrap_gethostbyname_r(
    const char* hostname, struct hostent* ret, char* buf, size_t buflen,
    struct hostent** result, int* h_errnop);
ARC_EXPORT int __wrap_getnameinfo(
    const struct sockaddr* sa, socklen_t salen,
    char* host, size_t hostlen,
    char* serv, size_t servlen, int flags);
ARC_EXPORT int __wrap_getpeername(
    int sockfd, struct sockaddr* addr, socklen_t* addrlen);
ARC_EXPORT int __wrap_getsockname(
    int sockfd, struct sockaddr* addr, socklen_t* addrlen);
ARC_EXPORT int __wrap_getsockopt(
    int sockfd, int level, int optname, void* optval, socklen_t* optlen);
ARC_EXPORT int __wrap_listen(int sockfd, int backlog);
ARC_EXPORT int __wrap_pipe(int pipefd[2]);
ARC_EXPORT int __wrap_pipe2(int pipefd[2], int flags);
ARC_EXPORT int __wrap_pselect(
    int nfds, fd_set* readfds, fd_set* writefds, fd_set* exceptfds,
    const struct timespec* timeout, const sigset_t* sigmask);
ARC_EXPORT ssize_t __wrap_recv(int sockfd, void* buf, size_t len, int flags);
ARC_EXPORT ssize_t __wrap_recvfrom(
    int sockfd, void* buf, size_t len, int flags, struct sockaddr* src_addr,
    socklen_t* addrlen);
ARC_EXPORT ssize_t __wrap___recvfrom_chk(
    int sockfd, void* buf, size_t len, size_t buflen, int flags,
    struct sockaddr* src_addr, socklen_t* addrlen);
ARC_EXPORT ssize_t __wrap_recvmsg(int sockfd, struct msghdr* msg, int flags);
ARC_EXPORT int __wrap_select(
    int nfds, fd_set* readfds, fd_set* writefds, fd_set* exceptfds,
    struct timeval* timeout);
ARC_EXPORT ssize_t __wrap_send(
    int sockfd, const void* buf, size_t len, int flags);
ARC_EXPORT ssize_t __wrap_sendto(
    int sockfd, const void* buf, size_t len, int flags,
    const struct sockaddr* dest_addr, socklen_t addrlen);
ARC_EXPORT ssize_t __wrap_sendmsg(
    int sockfd, const struct msghdr* msg, int flags);
ARC_EXPORT int __wrap_setsockopt(
    int sockfd, int level, int optname, const void* optval, socklen_t optlen);
ARC_EXPORT int __wrap_shutdown(int sockfd, int how);
ARC_EXPORT int __wrap_socket(int domain, int type, int protocol);
ARC_EXPORT int __wrap_socketpair(int domain, int type, int protocol, int sv[2]);
}  // extern "C"

using posix_translation::VirtualFileSystem;

int __wrap_accept(int sockfd, struct sockaddr* addr, socklen_t* addrlen) {
  ARC_STRACE_ENTER_FD("accept", "%d, %p, %p", sockfd, addr, addrlen);
  int fd = VirtualFileSystem::GetVirtualFileSystem()->accept(
      sockfd, addr, addrlen);
  ARC_STRACE_REGISTER_FD(fd, "accept");
  ARC_STRACE_RETURN(fd);
}

int __wrap_bind(int sockfd, const struct sockaddr* addr, socklen_t addrlen) {
  ARC_STRACE_ENTER_FD("bind", "%d, %s, %u",
                      sockfd, arc::GetSockaddrStr(addr, addrlen).c_str(),
                      addrlen);
  int result = VirtualFileSystem::GetVirtualFileSystem()->bind(
      sockfd, addr, addrlen);
  ARC_STRACE_RETURN(result);
}

int __wrap_connect(int sockfd, const struct sockaddr* addr,
                   socklen_t addrlen) {
  ARC_STRACE_ENTER_FD("connect", "%d, %s, %u",
                      sockfd, arc::GetSockaddrStr(addr, addrlen).c_str(),
                      addrlen);
  int result = VirtualFileSystem::GetVirtualFileSystem()->connect(
      sockfd, addr, addrlen);
  ARC_STRACE_RETURN(result);
}

int __wrap_epoll_create(int size) {
  ARC_STRACE_ENTER("epoll_create", "%d", size);
  int fd = VirtualFileSystem::GetVirtualFileSystem()->epoll_create1(0);
  ARC_STRACE_REGISTER_FD(fd, "epoll");
  ARC_STRACE_RETURN(fd);
}

int __wrap_epoll_ctl(int epfd, int op, int fd, struct epoll_event* event) {
  ARC_STRACE_ENTER_FD(
      "epoll_ctl", "%d, %s, %d \"%s\", %s",
      epfd, arc::GetEpollCtlOpStr(op).c_str(), fd,
      arc::GetFdStr(fd).c_str(),
      // Recent Linux kernel accepts NULL |event| when |op| is EPOLL_CTL_DEL.
      event ? arc::GetEpollEventStr(event->events).c_str() : "(null)");
  int result = VirtualFileSystem::GetVirtualFileSystem()->epoll_ctl(
      epfd, op, fd, event);
  ARC_STRACE_RETURN(result);
}

int __wrap_epoll_wait(int epfd, struct epoll_event* events, int maxevents,
                      int timeout) {
  ARC_STRACE_ENTER_FD("epoll_wait", "%d, %p, %d, %d",
                      epfd, events, maxevents, timeout);
  int result = VirtualFileSystem::GetVirtualFileSystem()->epoll_wait(
      epfd, events, maxevents, timeout);
  if (arc::StraceEnabled()) {
    for (int i = 0; i < result; ++i) {
      ARC_STRACE_REPORT("fd %d \"%s\" is ready for %s", events[i].data.fd,
                        arc::GetFdStr(events[i].data.fd).c_str(),
                        arc::GetEpollEventStr(events[i].events).c_str());
    }
  }
  ARC_STRACE_RETURN(result);
}

// TODO(khim): Add support for sigmask when we'll start supporting signals on
// ARC.
int __wrap_epoll_pwait(int epfd, struct epoll_event* events, int maxevents,
                       int timeout, const sigset_t *sigmask) {
  ARC_STRACE_ENTER_FD("epoll_pwait", "%d, %p, %d, %d, %p(%s)",
                      epfd, events, maxevents, timeout, sigmask,
                      arc::GetSigSetStr(sigmask).c_str());
  int result = VirtualFileSystem::GetVirtualFileSystem()->epoll_wait(
      epfd, events, maxevents, timeout);
  if (arc::StraceEnabled()) {
    for (int i = 0; i < result; ++i) {
      ARC_STRACE_REPORT("fd %d \"%s\" is ready for %s", events[i].data.fd,
                        arc::GetFdStr(events[i].data.fd).c_str(),
                        arc::GetEpollEventStr(events[i].events).c_str());
    }
  }
  ARC_STRACE_RETURN(result);
}

void __wrap_freeaddrinfo(struct addrinfo* res) {
  ARC_STRACE_ENTER("freeaddrinfo", "%p", res);
  VirtualFileSystem::GetVirtualFileSystem()->freeaddrinfo(res);
  ARC_STRACE_RETURN_VOID();
}

int __wrap_getnameinfo(const struct sockaddr* sa, socklen_t salen,
                       char* host, size_t hostlen,
                       char* serv, size_t servlen, int flags) {
  // TODO(igorc): Add GetNameInfoFlagStr() to src/common/arc_strace.[h,cc].
  ARC_STRACE_ENTER("getnameinfo", "%p, %d, %p, %zu, %p, %zu, %d",
                   sa, salen, host, hostlen, serv, servlen, flags);
  int result = VirtualFileSystem::GetVirtualFileSystem()->getnameinfo(
      sa, salen, host, hostlen, serv, servlen, flags);
  ARC_STRACE_RETURN(result);
}

int __wrap_getaddrinfo(const char* node, const char* service,
                       const struct addrinfo* hints, struct addrinfo** res) {
  ARC_STRACE_ENTER("getaddrinfo", "\"%s\", \"%s\", %p, %p",
                   SAFE_CSTR(node), SAFE_CSTR(service), hints, res);
  int result = VirtualFileSystem::GetVirtualFileSystem()->getaddrinfo(
      node, service, hints, res);
  // TODO(crbug.com/241955): Show errno for EAI_SYSTEM?
  ARC_STRACE_RETURN(result);
}

const char* __wrap_gai_strerror(int errcode) {
  // This code duplicates bionic/libc/netbsd/net/getaddrinfo.c.
  // TODO(crbug.com/356271): Use Bionic impl instead.
  static const char* const kErrorList[] = {
    "Success",
    "Address family for hostname not supported",    /* EAI_ADDRFAMILY */
    "Temporary failure in name resolution",         /* EAI_AGAIN      */
    "Invalid value for ai_flags",                   /* EAI_BADFLAGS   */
    "Non-recoverable failure in name resolution",   /* EAI_FAIL       */
    "ai_family not supported",                      /* EAI_FAMILY     */
    "Memory allocation failure",                    /* EAI_MEMORY     */
    "No address associated with hostname",          /* EAI_NODATA     */
    "hostname nor servname provided, or not known", /* EAI_NONAME     */
    "servname not supported for ai_socktype",       /* EAI_SERVICE    */
    "ai_socktype not supported",                    /* EAI_SOCKTYPE   */
    "System error returned in errno",               /* EAI_SYSTEM     */
    "Invalid value for hints",                      /* EAI_BADHINTS   */
    "Resolved protocol is unknown",                 /* EAI_PROTOCOL   */
    "Argument buffer overflow",                     /* EAI_OVERFLOW   */
    "Unknown error",                                /* EAI_MAX        */
  };

  ALOG_ASSERT((sizeof(kErrorList) / sizeof(kErrorList[0])) == (EAI_MAX + 1));

  if (errcode < 0 || errcode > EAI_MAX)
    errcode = EAI_MAX;
  return kErrorList[errcode];
}

struct hostent* __wrap_gethostbyaddr(
    const void* addr, socklen_t len, int type) {
  // TODO(igorc): Add GetNetFamilyStr() to src/common/arc_strace.[h,cc].
  ARC_STRACE_ENTER("gethostbyaddr", "%p, %d, %d", addr, len, type);
  struct hostent* result = VirtualFileSystem::GetVirtualFileSystem()
      ->gethostbyaddr(addr, len, type);
  if (result == NULL) {
    ARC_STRACE_REPORT("h_errno=%d", h_errno);
  }
  ARC_STRACE_RETURN_PTR(result, false);
}

struct hostent* __wrap_gethostbyname(const char* hostname) {
  ARC_STRACE_ENTER("gethostbyname", "\"%s\"", SAFE_CSTR(hostname));
  struct hostent* result = VirtualFileSystem::GetVirtualFileSystem()
      ->gethostbyname(hostname);
  if (result == NULL) {
    ARC_STRACE_REPORT("h_errno=%d", h_errno);
  }
  ARC_STRACE_RETURN_PTR(result, false);
}

int __wrap_gethostbyname_r(const char* hostname, struct hostent* ret,
                           char* buf, size_t buflen,
                           struct hostent** result, int* h_errnop) {
  ARC_STRACE_ENTER("gethostbyname_r", "\"%s\"", SAFE_CSTR(hostname));
  int res = VirtualFileSystem::GetVirtualFileSystem()
      ->gethostbyname_r(hostname, ret, buf, buflen, result, h_errnop);
  if (res != 0 && *h_errnop != 0) {
    ARC_STRACE_REPORT("h_errno=%d", *h_errnop);
  }
  ARC_STRACE_RETURN(res);
}

struct hostent* __wrap_gethostbyname2(const char* hostname, int family) {
  ARC_STRACE_ENTER("gethostbyname2", "\"%s\" %d",
                   SAFE_CSTR(hostname), family);
  struct hostent* result = VirtualFileSystem::GetVirtualFileSystem()
      ->gethostbyname2(hostname, family);
  if (result == NULL) {
    ARC_STRACE_REPORT("h_errno=%d", h_errno);
  }
  ARC_STRACE_RETURN_PTR(result, false);
}

int __wrap_getpeername(int sockfd, struct sockaddr* addr,
                       socklen_t* addrlen) {
  ARC_STRACE_ENTER_FD("getpeername", "%d, %p, %p", sockfd, addr, addrlen);
  int result = VirtualFileSystem::GetVirtualFileSystem()->getpeername(
      sockfd, addr, addrlen);
  if (result == -1 && errno == EINVAL) {
    DANGER();
  }
  ARC_STRACE_RETURN(result);
}

int __wrap_getsockname(int sockfd, struct sockaddr* addr,
                       socklen_t* addrlen) {
  ARC_STRACE_ENTER_FD("getsockname", "%d, %p, %p", sockfd, addr, addrlen);
  int result = VirtualFileSystem::GetVirtualFileSystem()->getsockname(
      sockfd, addr, addrlen);
  if (result == -1 && errno == EINVAL) {
    DANGER();
  }
  ARC_STRACE_RETURN(result);
}

int __wrap_getsockopt(int sockfd, int level, int optname,
                      void* optval, socklen_t* optlen) {
  ARC_STRACE_ENTER_FD("getsockopt", "%d, %d, %d, %p, %p",
                      sockfd, level, optname, optval, optlen);
  int result = VirtualFileSystem::GetVirtualFileSystem()->getsockopt(
      sockfd, level, optname, optval, optlen);
  ARC_STRACE_RETURN(result);
}

int __wrap_listen(int sockfd, int backlog) {
  ARC_STRACE_ENTER_FD("listen", "%d, %d", sockfd, backlog);
  int result = VirtualFileSystem::GetVirtualFileSystem()->listen(
      sockfd, backlog);
  ARC_STRACE_RETURN(result);
}

int __wrap_pipe(int pipefd[2]) {
  ARC_STRACE_ENTER("pipe", "%p", pipefd);
  int result;
  result = VirtualFileSystem::GetVirtualFileSystem()->pipe2(pipefd, 0);
  if (result >= 0) {
    ARC_STRACE_REGISTER_FD(pipefd[0], "pipe[0]");
    ARC_STRACE_REGISTER_FD(pipefd[1], "pipe[1]");
    ARC_STRACE_REPORT("pipe[0]=%d pipe[1]=%d", pipefd[0], pipefd[1]);
  }
  ARC_STRACE_RETURN(result);
}

int __wrap_pipe2(int pipefd[2], int flags) {
  ARC_STRACE_ENTER("pipe2", "%p, %d", pipefd, flags);

  int result = VirtualFileSystem::GetVirtualFileSystem()->pipe2(pipefd, flags);
  if (result >= 0) {
    ARC_STRACE_REGISTER_FD(pipefd[0], "pipe2[0]");
    ARC_STRACE_REGISTER_FD(pipefd[1], "pipe2[1]");
    ARC_STRACE_REPORT("pipe[0]=%d pipe[1]=%d", pipefd[0], pipefd[1]);
  }
  ARC_STRACE_RETURN(result);
}

// TODO(khim): Add support for sigmask when we'll start supporting signals on
// ARC.
// TODO(khim): Provide nanosecond timeout precision.
int __wrap_pselect(int nfds, fd_set* readfds, fd_set* writefds,
                   fd_set* exceptfds, const struct timespec* timeout_timespec,
                   const sigset_t* sigmask) {
  // TODO(crbug.com/241955): Stringify *fds parameters.
  ARC_STRACE_ENTER("pselect", "%d, %p, %p, %p, %p, %p(%s)",
                   nfds, readfds, writefds, exceptfds, timeout_timespec,
                   sigmask, arc::GetSigSetStr(sigmask).c_str());
  int result;
  if (timeout_timespec) {
    struct timeval timeout = {
      timeout_timespec->tv_sec,
      timeout_timespec->tv_nsec / static_cast<long>(  // NOLINT(runtime/int)
          base::Time::kNanosecondsPerMicrosecond)
    };
    result = VirtualFileSystem::GetVirtualFileSystem()->select(
        nfds, readfds, writefds, exceptfds, &timeout);
  } else {
    result = VirtualFileSystem::GetVirtualFileSystem()->select(
        nfds, readfds, writefds, exceptfds, nullptr);
  }
  ARC_STRACE_RETURN(result);
}

ssize_t __wrap_recv(int sockfd, void* buf, size_t len, int flags) {
  ARC_STRACE_ENTER_FD("recv", "%d, %p, %zu, %d", sockfd, buf, len, flags);
  int result = VirtualFileSystem::GetVirtualFileSystem()->recv(
      sockfd, buf, len, flags);
  if (result >= 0)
    ARC_STRACE_REPORT("buf=%s", arc::GetRWBufStr(buf, result).c_str());
  ARC_STRACE_RETURN(result);
}

ssize_t __wrap_recvfrom(int sockfd, void* buf, size_t len, int flags,
                        struct sockaddr* src_addr, socklen_t* addrlen) {
  ARC_STRACE_ENTER_FD("recvfrom", "%d, %p, %zu, %d, %p, %p",
                      sockfd, buf, len, flags, src_addr, addrlen);
  int result = VirtualFileSystem::GetVirtualFileSystem()->recvfrom(
      sockfd, buf, len, flags, src_addr, addrlen);
  if (result == -1 && errno == EINVAL) {
    DANGER();
  }
  if (result >= 0)
    ARC_STRACE_REPORT("buf=%s", arc::GetRWBufStr(buf, result).c_str());
  ARC_STRACE_RETURN(result);
}

IRT_WRAPPER(recvfrom, int sockfd, void* buf, size_t len, int flags,
                      struct sockaddr* src_addr, socklen_t* addrlen,
                      int* count) {
  *count = __wrap_recvfrom(sockfd, buf, len, flags, src_addr, addrlen);
  return *count >= 0 ? 0 : errno;
}

ssize_t __wrap___recvfrom_chk(int sockfd, void* buf, size_t len, size_t buflen,
                              int flags, struct sockaddr* src_addr,
                              socklen_t* addrlen) {
  // TODO(crbug.com/478000): Wrap __recvfrom_chk accurately?
  return __wrap_recvfrom(sockfd, buf, len, flags, src_addr, addrlen);
}

ssize_t __wrap_recvmsg(int sockfd, struct msghdr* msg, int flags) {
  ARC_STRACE_ENTER_FD("recvmsg", "%d, %p, %d", sockfd, msg, flags);
  ssize_t result = VirtualFileSystem::GetVirtualFileSystem()->recvmsg(
      sockfd, msg, flags);
  ARC_STRACE_RETURN(result);
}

int __wrap_select(int nfds, fd_set* readfds, fd_set* writefds,
                  fd_set* exceptfds, struct timeval* timeout) {
  // TODO(crbug.com/241955): Stringify *fds parameters.
  ARC_STRACE_ENTER("select", "%d, %p, %p, %p, %p",
                   nfds, readfds, writefds, exceptfds, timeout);
  int result = VirtualFileSystem::GetVirtualFileSystem()->select(
      nfds, readfds, writefds, exceptfds, timeout);
  ARC_STRACE_RETURN(result);
}

ssize_t __wrap_send(int sockfd, const void* buf, size_t len, int flags) {
  ARC_STRACE_ENTER_FD("send", "%d, %p, %zu, %d", sockfd, buf, len, flags);
  int result = VirtualFileSystem::GetVirtualFileSystem()->send(
      sockfd, buf, len, flags);
  if (errno != EFAULT)
    ARC_STRACE_REPORT("buf=%s", arc::GetRWBufStr(buf, result).c_str());
  ARC_STRACE_RETURN(result);
}

ssize_t __wrap_sendto(int sockfd, const void* buf, size_t len, int flags,
                      const struct sockaddr* dest_addr, socklen_t addrlen) {
  ARC_STRACE_ENTER_FD("sendto", "%d, %p, %zu, %d, %s, %u",
                      sockfd, buf, len, flags,
                      arc::GetSockaddrStr(dest_addr, addrlen).c_str(), addrlen);
  int result = VirtualFileSystem::GetVirtualFileSystem()->sendto(
      sockfd, buf, len, flags, dest_addr, addrlen);
  if (result == -1 && errno == EINVAL) {
    DANGER();
  }
  if (errno != EFAULT)
    ARC_STRACE_REPORT("buf=%s", arc::GetRWBufStr(buf, result).c_str());
  ARC_STRACE_RETURN(result);
}

IRT_WRAPPER(sendto, int sockfd, const void *buf, size_t len, int flags,
            const struct sockaddr* dest_addr, socklen_t addrlen, int* count) {
  *count = __wrap_sendto(sockfd, buf, len, flags, dest_addr, addrlen);
  return *count >= 0 ? 0 : errno;
}

ssize_t __wrap_sendmsg(int sockfd, const struct msghdr* msg, int flags) {
  ARC_STRACE_ENTER_FD("sendmsg", "%d, %p, %d", sockfd, msg, flags);
  ssize_t result = VirtualFileSystem::GetVirtualFileSystem()->sendmsg(
      sockfd, msg, flags);
  ARC_STRACE_RETURN(result);
}

int __wrap_setsockopt(int sockfd, int level, int optname,
                      const void* optval, socklen_t optlen) {
  ARC_STRACE_ENTER_FD("setsockopt", "%d, %d, %d, %p, %d",
                      sockfd, level, optname, optval, optlen);
  int result = VirtualFileSystem::GetVirtualFileSystem()->setsockopt(
      sockfd, level, optname, optval, optlen);
  ARC_STRACE_RETURN(result);
}

int __wrap_shutdown(int sockfd, int how) {
  ARC_STRACE_ENTER_FD("shutdown", "%d, %d", sockfd, how);
  int result = VirtualFileSystem::GetVirtualFileSystem()->shutdown(sockfd, how);
  ARC_STRACE_RETURN(result);
}

int __wrap_socket(int domain, int type, int protocol) {
  ARC_STRACE_ENTER("socket", "%s, %s, %s",
                   arc::GetSocketDomainStr(domain).c_str(),
                   arc::GetSocketTypeStr(type).c_str(),
                   arc::GetSocketProtocolStr(protocol).c_str());
  int fd = VirtualFileSystem::GetVirtualFileSystem()->socket(
      domain, type, protocol);
  ARC_STRACE_REGISTER_FD(fd, "socket");
  ARC_STRACE_RETURN(fd);
}

int __wrap_socketpair(int domain, int type, int protocol, int sv[2]) {
  ARC_STRACE_ENTER("socketpair", "%s, %s, %s, %p",
                   arc::GetSocketDomainStr(domain).c_str(),
                   arc::GetSocketTypeStr(type).c_str(),
                   arc::GetSocketProtocolStr(protocol).c_str(),
                   sv);
  int result = VirtualFileSystem::GetVirtualFileSystem()->socketpair(
      domain, type, protocol, sv);
  if (result >= 0) {
    ARC_STRACE_REGISTER_FD(sv[0], "socketpair[0]");
    ARC_STRACE_REGISTER_FD(sv[1], "socketpair[1]");
    ARC_STRACE_REPORT("sock[0]=%d sock[1]=%d", sv[0], sv[1]);
  }
  ARC_STRACE_RETURN(result);
}

IRT_WRAPPER(socketpair, int domain, int type, int protocol, int sv[2]) {
  return __wrap_socketpair(domain, type, protocol, sv) == 0 ? 0 : errno;
}

namespace posix_translation {

void InitializeIRTHooksForSockets() {
  DO_WRAP(recvfrom);
  DO_WRAP(sendto);
  DO_WRAP(socketpair);
}

}  // namespace posix_translation
