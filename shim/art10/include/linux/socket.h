// <linux/socket.h> for the Android 10 stack in WebAssembly. The UAPI header
// of bionic 10 (bionic/libc/kernel/uapi/linux/socket.h, found through
// -idirafter) names its struct sockaddr_storage, which bionic's libc headers
// then do not define again; musl's <sys/socket.h> already has it, so the
// struct would be defined twice (libcore's android_system_OsConstants.cpp
// includes <linux/if_addr.h>, which includes this). Everything but the
// struct. Part of the arc-wasm shim layer.
#ifndef _UAPI_LINUX_SOCKET_H
#define _UAPI_LINUX_SOCKET_H
#define _K_SS_MAXSIZE 128
#define _K_SS_ALIGNSIZE (__alignof__(struct sockaddr *))
typedef unsigned short __kernel_sa_family_t;
#endif
