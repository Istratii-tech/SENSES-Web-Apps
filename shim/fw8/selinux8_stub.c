/* libselinux for the Android 8.1 stack, without SELinux: a policy that
 * allows everything. servicemanager 8.1 no longer asks is_selinux_enabled()
 * (6.0's did, shim/fw6/selinux_stub.c): it needs a service_contexts handle
 * and its own context at startup, and every add/find/list asks
 * selinux_check_access() about the caller's context. So there are
 * contexts (fixed strings), every lookup finds one, and every access is
 * allowed; apps (uid >= 10000) still cannot register services, which
 * servicemanager checks by uid. Part of the arc-wasm shim layer. */

#include <errno.h>
#include <selinux/android.h>
#include <selinux/avc.h>
#include <selinux/selinux.h>
#include <stdlib.h>
#include <string.h>

struct selabel_handle {
  int unused;
};

static struct selabel_handle g_service_contexts;

int is_selinux_enabled(void) { return 0; }

int getcon(char** con) {
  *con = strdup("u:r:servicemanager:s0");
  return *con ? 0 : -1;
}

int getpidcon(pid_t pid, char** con) {
  (void)pid;
  *con = strdup("u:r:arcwasm:s0");
  return *con ? 0 : -1;
}

void freecon(char* con) { free(con); }

int selinux_check_access(const char* scon, const char* tcon, const char* tclass,
                         const char* perm, void* auditdata) {
  (void)scon, (void)tcon, (void)tclass, (void)perm, (void)auditdata;
  return 0;
}

int selinux_status_open(int fallback) {
  (void)fallback;
  return 0;
}

int selinux_status_updated(void) { return 0; }

void selinux_set_callback(int type, union selinux_callback cb) { (void)type, (void)cb; }

struct selabel_handle* selinux_android_service_context_handle(void) {
  return &g_service_contexts;
}

struct selabel_handle* selinux_android_hw_service_context_handle(void) {
  return &g_service_contexts;
}

struct selabel_handle* selinux_android_vendor_service_context_handle(void) {
  return &g_service_contexts;
}

int selabel_lookup(struct selabel_handle* handle, char** con, const char* key, int type) {
  (void)handle, (void)key, (void)type;
  *con = strdup("u:object_r:default_service:s0");
  return *con ? 0 : -1;
}

void selabel_close(struct selabel_handle* handle) { (void)handle; }

int selinux_log_callback(int type, const char* fmt, ...) {
  (void)type, (void)fmt;
  return 0;
}

int selinux_vendor_log_callback(int type, const char* fmt, ...) {
  (void)type, (void)fmt;
  return 0;
}
