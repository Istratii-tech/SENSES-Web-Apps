// Android ashmem UAPI (bionic libc/kernel/uapi/linux/ashmem.h). The wasm
// port emulates /dev/ashmem in posix_translation/dev_ashmem.cc; only the ABI
// constants are needed. Part of the arc-wasm shim layer.
#ifndef _UAPI_LINUX_ASHMEM_H
#define _UAPI_LINUX_ASHMEM_H

#include <stddef.h>
#include <stdint.h>
#include <sys/ioctl.h>

#define ASHMEM_NAME_LEN 256
#define ASHMEM_NAME_DEF "dev/ashmem"
#define ASHMEM_NOT_PURGED 0
#define ASHMEM_WAS_PURGED 1
#define ASHMEM_IS_UNPINNED 0
#define ASHMEM_IS_PINNED 1

struct ashmem_pin {
  uint32_t offset;
  uint32_t len;
};

#define __ASHMEMIOC 0x77
#define ASHMEM_SET_NAME _IOW(__ASHMEMIOC, 1, char[ASHMEM_NAME_LEN])
#define ASHMEM_GET_NAME _IOR(__ASHMEMIOC, 2, char[ASHMEM_NAME_LEN])
#define ASHMEM_SET_SIZE _IOW(__ASHMEMIOC, 3, size_t)
#define ASHMEM_GET_SIZE _IO(__ASHMEMIOC, 4)
#define ASHMEM_SET_PROT_MASK _IOW(__ASHMEMIOC, 5, unsigned long)
#define ASHMEM_GET_PROT_MASK _IO(__ASHMEMIOC, 6)
#define ASHMEM_PIN _IOW(__ASHMEMIOC, 7, struct ashmem_pin)
#define ASHMEM_UNPIN _IOW(__ASHMEMIOC, 8, struct ashmem_pin)
#define ASHMEM_GET_PIN_STATUS _IO(__ASHMEMIOC, 9)
#define ASHMEM_PURGE_ALL_CACHES _IO(__ASHMEMIOC, 10)

#endif  // _UAPI_LINUX_ASHMEM_H
