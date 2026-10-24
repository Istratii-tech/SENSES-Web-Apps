// /proc/meminfo of the budget (topic mem-budget, mem_budget.h): MemTotal
// is the budget, MemFree and MemAvailable what the system server's
// MemoryMonitor last found left of it (arc_mem_publish). Read by
// Process.getTotalMemory/getFreeMemory (ActivityManager.MemoryInfo on a
// device), and by native code sizing its caches by the RAM (Gecko,
// Chromium, ...). Compiled with the posix_translation headers (the
// kernel's). Part of the arc-wasm shim layer.

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <atomic>
#include <string>

#include "membudget/mem_budget.h"
#include "posix_translation/file_system_handler.h"
#include "posix_translation/readonly_memory_file.h"
#include "posix_translation/statfs.h"
#include "posix_translation/virtual_file_system.h"

namespace {

std::atomic<int64_t> g_in_use{-1};

class MeminfoFile : public posix_translation::ReadonlyMemoryFile {
 public:
  MeminfoFile(const std::string& pathname, const std::string& text)
      : ReadonlyMemoryFile(pathname, ENODEV, time(NULL)), content_(text.begin(), text.end()) {}

 protected:
  virtual ~MeminfoFile() {}
  virtual const Content& GetContent() OVERRIDE { return content_; }
  virtual int fstatfs(struct statfs* buf) OVERRIDE {
    return posix_translation::DoStatFsForProc(buf);
  }

 private:
  Content content_;
};

std::string Meminfo() {
  int64_t s[ARC_MEM_SAMPLE_COUNT];
  arc_mem_sample(s, 0);
  const int64_t total = s[ARC_MEM_BUDGET];
  int64_t used = g_in_use.load();
  if (used < 0) {
    // No monitor (a VM without the system server): what the heap handed
    // out (not the memory's size: it may be made whole at once), less what
    // is known to be free in it.
    used = (s[ARC_MEM_BRK] > 0 ? s[ARC_MEM_BRK] : s[ARC_MEM_WASM]) - s[ARC_MEM_LARGE_FREE] - s[ARC_MEM_IMAGE];
  }
  int64_t avail = total - used;
  if (avail < 0)
    avail = 0;
  const unsigned long kb_total = static_cast<unsigned long>(total / 1024);
  const unsigned long kb_free = static_cast<unsigned long>(avail / 1024);
  char buf[1024];
  // Linux 3.18's fields (what readers look for); no swap, no page cache.
  snprintf(buf, sizeof(buf),
           "MemTotal:       %8lu kB\nMemFree:        %8lu kB\nMemAvailable:   %8lu kB\n"
           "Buffers:               0 kB\nCached:                0 kB\nSwapCached:            0 kB\n"
           "Active:         %8lu kB\nInactive:              0 kB\nUnevictable:           0 kB\n"
           "Mlocked:               0 kB\nSwapTotal:             0 kB\nSwapFree:              0 kB\n"
           "Dirty:                 0 kB\nWriteback:             0 kB\nAnonPages:      %8lu kB\n"
           "Mapped:                0 kB\nShmem:                 0 kB\nSlab:                  0 kB\n"
           "KernelStack:           0 kB\nPageTables:            0 kB\nCommitLimit:    %8lu kB\n"
           "Committed_AS:   %8lu kB\nVmallocTotal:          0 kB\nVmallocUsed:           0 kB\n",
           kb_total, kb_free, kb_free, kb_total - kb_free, kb_total - kb_free, kb_total,
           kb_total - kb_free);
  return buf;
}

class ProcMeminfoHandler : public posix_translation::FileSystemHandler {
 public:
  ProcMeminfoHandler() : FileSystemHandler("ProcMeminfoHandler") {}

  virtual scoped_refptr<posix_translation::FileStream> open(int fd, const std::string& pathname,
                                                            int oflag, mode_t cmode) OVERRIDE {
    if ((oflag & O_ACCMODE) != O_RDONLY) {
      errno = EACCES;
      return NULL;
    }
    return new MeminfoFile(pathname, Meminfo());
  }

  virtual int stat(const std::string& pathname, struct stat* out) OVERRIDE {
    memset(out, 0, sizeof(*out));
    out->st_mode = S_IFREG | 0444;
    out->st_nlink = 1;
    return 0;
  }

  virtual int statfs(const std::string& pathname, struct statfs* out) OVERRIDE {
    return posix_translation::DoStatFsForProc(out);
  }

  virtual posix_translation::Dir* OnDirectoryContentsNeeded(const std::string&) OVERRIDE {
    return NULL;
  }
};

}  // namespace

extern "C" {

// What the monitor found in use (bytes; MemoryBudget.nativePublish).
void arc_mem_publish_in_use(int64_t bytes) {
  g_in_use.store(bytes);
}

// Mounts /proc/meminfo (art/vfs_setup.cc, after the kernel boots).
void arc_membudget_mount_proc(posix_translation::VirtualFileSystem* vfs) {
  vfs->Mount("/proc/meminfo", new ProcMeminfoHandler);
}

}  // extern "C"
