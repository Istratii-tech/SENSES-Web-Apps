// netd's client library (system/netd/include/NetdClient.h) for HTTPBase:
// one network, nothing to bind sockets to. Part of the arc-wasm shim layer
// (topic mediacodec).
#ifndef ARC_WASM_MEDIA_NETDCLIENT_H_
#define ARC_WASM_MEDIA_NETDCLIENT_H_

#include <sys/types.h>

static inline int setNetworkForUser(uid_t, int) { return 0; }
static inline int setNetworkForProcess(unsigned) { return 0; }

#endif  // ARC_WASM_MEDIA_NETDCLIENT_H_
