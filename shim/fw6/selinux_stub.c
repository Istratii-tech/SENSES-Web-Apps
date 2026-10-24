/* libselinux for a system without SELinux (shim/fw6/include/selinux):
 * disabled, no contexts, every access allowed. servicemanager 6.0 checks
 * is_selinux_enabled() first and then never calls the rest.
 * Part of the arc-wasm shim layer. */

#include <errno.h>
#include <selinux/android.h>
#include <selinux/selinux.h>
#include <stdlib.h>

int is_selinux_enabled(void) { return 0; }

int getcon(char** con) {
  *con = NULL;
  errno = ENOSYS;
  return -1;
}

int getpidcon(pid_t pid, char** con) {
  (void)pid;
  return getcon(con);
}

void freecon(char* con) { free(con); }

int selinux_check_access(const char* scon, const char* tcon, const char* tclass,
                         const char* perm, void* auditdata) {
  (void)scon, (void)tcon, (void)tclass, (void)perm, (void)auditdata;
  return 0;
}

int selinux_status_open(int fallback) {
  (void)fallback;
  return -1;
}

int selinux_status_updated(void) { return 0; }

void selinux_set_callback(int type, union selinux_callback cb) { (void)type, (void)cb; }

struct selabel_handle* selinux_android_service_context_handle(void) { return NULL; }

int selabel_lookup(struct selabel_handle* handle, char** con, const char* key, int type) {
  (void)handle, (void)key, (void)type;
  *con = NULL;
  errno = ENOENT;
  return -1;
}

void selabel_close(struct selabel_handle* handle) { (void)handle; }

int selinux_log_callback(int type, const char* fmt, ...) {
  (void)type, (void)fmt;
  return 0;
}
