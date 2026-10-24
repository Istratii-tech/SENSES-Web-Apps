/* libmemtrack (system/core/libmemtrack) as on a device without a memtrack
 * HAL: memtrack_init() fails (hw_get_module finds no memtrack module), so
 * android.os.Debug reports no graphics/GL memory. The calls after a failed
 * init are not made; they fail too if they are. Part of the arc-wasm shim
 * layer. */

#include <errno.h>
#include <memtrack/memtrack.h>
#include <stddef.h>

int memtrack_init(void) { return -ENOENT; }

struct memtrack_proc* memtrack_proc_new(void) { return NULL; }

void memtrack_proc_destroy(struct memtrack_proc* p) { (void)p; }

int memtrack_proc_get(struct memtrack_proc* p, pid_t pid) {
  (void)p, (void)pid;
  return -ENOENT;
}

ssize_t memtrack_proc_graphics_total(struct memtrack_proc* p) { (void)p; return -EINVAL; }
ssize_t memtrack_proc_graphics_pss(struct memtrack_proc* p) { (void)p; return -EINVAL; }
ssize_t memtrack_proc_gl_total(struct memtrack_proc* p) { (void)p; return -EINVAL; }
ssize_t memtrack_proc_gl_pss(struct memtrack_proc* p) { (void)p; return -EINVAL; }
ssize_t memtrack_proc_other_total(struct memtrack_proc* p) { (void)p; return -EINVAL; }
ssize_t memtrack_proc_other_pss(struct memtrack_proc* p) { (void)p; return -EINVAL; }
