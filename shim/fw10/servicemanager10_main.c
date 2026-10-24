/* Runs servicemanager 10 (frameworks/native/cmds/servicemanager) inside
 * this process, on its own thread, as its own binder process (pid 2, uid
 * 1000), as init would. As for 8.1 (shim/fw8/servicemanager8_main.c),
 * its main() is run as it is: it also sets up the SELinux handles that
 * every add/find needs (shim/fw8/selinux8_stub.c, shared with 8.1). service_manager.c is
 * compiled with main renamed to arc_servicemanager10_main and its call of
 * binder_loop renamed to arc_servicemanager10_loop (below): by then the
 * thread is the context manager, so that is where the starter is told to
 * go on (the first defaultServiceManager() would otherwise sleep a second
 * and retry), and the handler's -1 is turned into -EPERM. New in 10: the
 * handler gets binder_transaction_data_secctx (the caller's SELinux
 * context, when the driver gives one; the in-process driver does not, so
 * binder.c falls back from BINDER_SET_CONTEXT_MGR_EXT to
 * BINDER_SET_CONTEXT_MGR and the context is looked up by pid).
 * Part of the arc-wasm shim layer. */

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "arc_binder.h"
#include "binder.h"

#define AID_SYSTEM 1000

int arc_servicemanager10_main(int argc, char** argv);

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static int g_state; /* 0: not started, 1: starting, 2: running, -1: failed */
static binder_handler g_handler;

static void set_state(int state) {
  pthread_mutex_lock(&g_lock);
  g_state = state;
  pthread_cond_broadcast(&g_cond);
  pthread_mutex_unlock(&g_lock);
}

/* svcmgr_handler fails with -1, which is -EPERM (PERMISSION_DENIED) on
 * Linux; libbinder reads the status with this libc's numbering, where
 * EPERM is 63 (WASI), and would see an unknown error. */
static int handler(struct binder_state* bs, struct binder_transaction_data_secctx* txn,
                   struct binder_io* msg, struct binder_io* reply) {
  int r = g_handler(bs, txn, msg, reply);
  return r == -1 ? -EPERM : r;
}

void arc_servicemanager10_loop(struct binder_state* bs, binder_handler func) {
  g_handler = func;
  set_state(2);
  binder_loop(bs, handler);
}

static void* servicemanager_thread(void* unused) {
  static char* argv[] = {"servicemanager", "/dev/binder", NULL};
  (void)unused;
  arc_binder_set_process_identity(ARC_BINDER_SERVICEMANAGER_PID, AID_SYSTEM);
  arc_servicemanager10_main(2, argv);
  /* main() returns only when the driver cannot be opened or it cannot
   * become the context manager. */
  fprintf(stderr, "servicemanager: failed to start (%s)\n", strerror(errno));
  set_state(-1);
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
