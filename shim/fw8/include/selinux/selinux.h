// libselinux's <selinux/selinux.h> for the Android 8.1 stack: what
// servicemanager 8.1 calls (shim/fw8/selinux8_stub.c: a policy that
// allows everything). Part of the arc-wasm shim layer.
#ifndef ARC_WASM_FW8_SELINUX_SELINUX_H_
#define ARC_WASM_FW8_SELINUX_SELINUX_H_

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef char* security_context_t;
typedef unsigned short security_class_t;

int is_selinux_enabled(void);
int getcon(char** con);
int getpidcon(pid_t pid, char** con);
void freecon(char* con);
int selinux_check_access(const char* scon, const char* tcon, const char* tclass,
                         const char* perm, void* auditdata);

union selinux_callback {
  int (*func_log)(int type, const char* fmt, ...);
  int (*func_audit)(void* auditdata, security_class_t cls, char* msgbuf, size_t msgbufsize);
  int (*func_validate)(char** ctx);
  int (*func_setenforce)(int enforcing);
  int (*func_policyload)(int seqno);
};
#define SELINUX_CB_LOG 0
#define SELINUX_CB_AUDIT 1
#define SELINUX_CB_VALIDATE 2
#define SELINUX_CB_SETENFORCE 3
#define SELINUX_CB_POLICYLOAD 4
#define SELINUX_ERROR 0
#define SELINUX_WARNING 1
#define SELINUX_INFO 2
#define SELINUX_AVC 3
void selinux_set_callback(int type, union selinux_callback cb);

#ifdef __cplusplus
}
#endif

#endif  // ARC_WASM_FW8_SELINUX_SELINUX_H_
