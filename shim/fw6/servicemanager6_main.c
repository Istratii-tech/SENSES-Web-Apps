/* Runs servicemanager 6.0 (frameworks/native/cmds/servicemanager) inside
 * this process, on its own thread, as its own binder process: what its
 * main() does, with a signal once it is the context manager so that the
 * first defaultServiceManager() does not race it (libbinder would sleep a
 * second and retry). service_manager.c is compiled with its main()
 * renamed; SELinux is off (shim/fw6/selinux_stub.c), so every service may
 * register except from app uids, as on a device without a policy.
 * The 6.0 counterpart of shim/binder/servicemanager_main.c (4.4's API).
 * Part of the arc-wasm shim layer. */

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "arc_binder.h"
#include "binder.h"

#define AID_SYSTEM 1000

int svcmgr_handler(struct binder_state* bs, struct binder_transaction_data* txn,
                   struct binder_io* msg, struct binder_io* reply);

/* svcmgr_handler fails with -1, which is -EPERM (PERMISSION_DENIED) on
 * Linux; libbinder reads the status with this libc's numbering, where
 * EPERM is 63 (WASI), and would see an unknown error. */
static int handler(struct binder_state* bs, struct binder_transaction_data* txn,
                   struct binder_io* msg, struct binder_io* reply) {
  int r = svcmgr_handler(bs, txn, msg, reply);
  return r == -1 ? -EPERM : r;
}

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static int g_state; /* 0: not started, 1: starting, 2: running, -1: failed */

static void set_state(int state) {
  pthread_mutex_lock(&g_lock);
  g_state = state;
  pthread_cond_broadcast(&g_cond);
  pthread_mutex_unlock(&g_lock);
}

static void* servicemanager_thread(void* unused) {
  struct binder_state* bs;
  (void)unused;
  arc_binder_set_process_identity(ARC_BINDER_SERVICEMANAGER_PID, AID_SYSTEM);
  bs = binder_open(128 * 1024);
  if (!bs) {
    set_state(-1);
    return NULL;
  }
  if (binder_become_context_manager(bs)) {
    fprintf(stderr, "servicemanager: cannot become context manager (%s)\n",
            strerror(errno));
    set_state(-1);
    return NULL;
  }
  set_state(2);
  binder_loop(bs, handler);
  return NULL;
}

int arc_binder_start_servicemanager(void) {
  pthread_t thread;
  int state;
  pthread_mutex_lock(&g_lock);
  if (g_state == 0) {
    g_state = 1;
    if (pthread_create(&thread, NULL, servicemanager_thread, NULL) == 0)
      pthread_detach(thread);
    else
      g_state = -1;
  }
  while (g_state == 1)
    pthread_cond_wait(&g_cond, &g_lock);
  state = g_state;
  pthread_mutex_unlock(&g_lock);
  return state == 2 ? 0 : -1;
}
