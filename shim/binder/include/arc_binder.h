// arc-wasm extensions of the in-process binder driver (shim/binder/
// dev_binder.cc). Part of the arc-wasm shim layer.
#ifndef ARC_WASM_BINDER_H_
#define ARC_WASM_BINDER_H_

#include <linux/binder.h>
#include <stdint.h>

// The pid the in-process servicemanager runs as.
#define ARC_BINDER_SERVICEMANAGER_PID 2

#ifdef __cplusplus
extern "C" {
#endif

// Every open() of /dev/binder is a binder "process" with its own nodes,
// handles and threads. By default it carries the identity of the real
// process (getpid(), geteuid()). In-process stand-ins for other Android
// processes (servicemanager, test services) call this on their thread
// before they open the device: the binder processes this thread opens
// from then on have this pid and uid, which is what the other side sees
// as the calling pid/uid.
void arc_binder_set_process_identity(int pid, int uid);

// Starts the in-process servicemanager (frameworks/native/cmds/
// servicemanager) on its own thread, as init would, and returns once it is
// the context manager (handle 0). Safe to call more than once.
int arc_binder_start_servicemanager(void);

#ifdef __cplusplus
}
#endif

#endif  // ARC_WASM_BINDER_H_
