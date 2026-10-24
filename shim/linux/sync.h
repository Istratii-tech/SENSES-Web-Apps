// Android (pre-4.x staging) sync-fence UAPI, as used by ARC's fence_stream.
// Part of the arc-wasm shim layer.
#ifndef _UAPI_LINUX_SYNC_H
#define _UAPI_LINUX_SYNC_H

#include <stdint.h>
#include <sys/ioctl.h>

struct sync_merge_data {
  int32_t fd2;
  char name[32];
  int32_t fence;
};

struct sync_pt_info {
  uint32_t len;
  char obj_name[32];
  char driver_name[32];
  int32_t status;
  uint64_t timestamp_ns;
  uint8_t driver_data[0];
};

struct sync_fence_info_data {
  uint32_t len;
  char name[32];
  int32_t status;
  uint8_t pt_info[0];
};

#define SYNC_IOC_MAGIC '>'
#define SYNC_IOC_WAIT _IOW(SYNC_IOC_MAGIC, 0, int32_t)
#define SYNC_IOC_MERGE _IOWR(SYNC_IOC_MAGIC, 1, struct sync_merge_data)
#define SYNC_IOC_FENCE_INFO _IOWR(SYNC_IOC_MAGIC, 2, struct sync_fence_info_data)

#endif  // _UAPI_LINUX_SYNC_H
