// Synchronous calls into the browser's Origin Private File System.
//
// OPFS is asynchronous (except FileSystemSyncAccessHandle, which only works
// on the worker that created it). This bridge runs all OPFS work on one
// dedicated pthread and lets any other thread call it synchronously: the
// caller proxies a request with emscripten_proxy_sync_with_ctx() and blocks
// until the OPFS thread's JavaScript (library_arc_opfs.js) finishes it.
// Callers must not be the browser main thread, which may not block.
// Part of the arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_OPFS_OPFS_BRIDGE_H_
#define ARC_WASM_SHIM_OPFS_OPFS_BRIDGE_H_

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace base {
class Lock;
}  // namespace base

namespace arc_opfs {

enum Op {
  kOpen = 1,
  kClose = 2,
  kRead = 3,
  kWrite = 4,
  kGetSize = 5,
  kTruncate = 6,
  kFlush = 7,
  kStat = 8,
  kMkdir = 9,
  kUnlink = 10,
  kRmdir = 11,
  kRename = 12,
  kList = 13,
};

enum OpenFlags {
  kCreate = 1,
  kExclusive = 2,
  kTruncateOnOpen = 4,
  // Read through a File (getFile()), not a sync access handle: read-only
  // mounts (OpfsHandler's |read_only|).
  kReadOnly = 8,
};

enum EntryType {
  kNone = 0,
  kFile = 1,
  kDirectory = 2,
};

// Shared with library_arc_opfs.js, which reads it by these offsets.
struct Request {
  int32_t op;            // 0
  int32_t flags;         // 4
  int32_t id;            // 8
  const char* path;      // 12
  const char* path2;     // 16
  void* buf;             // 20
  int32_t len;           // 24
  int32_t result;        // 28  out: >= 0, or -errno
  double offset;         // 32  in: file offset / truncate length
  double size;           // 40  out
  double mtime;          // 48  out: seconds since the epoch
  int32_t type;          // 56  out: EntryType
  char* listing;         // 60  out: malloc'ed, see List()
};

struct Entry {
  std::string name;
  bool is_directory;
};

// Starts the OPFS thread. Returns false if OPFS is unavailable (e.g. Node,
// or an insecure context). Idempotent.
bool Start();

// A lock its caller may hold while it calls in here (posix_translation's
// VFS mutex): Call() lets go of it while it waits for the browser and takes
// it again before it returns (topic gc-pause, docs/topics/gc-pause.md).
void SetLockReleasedWhileWaiting(base::Lock* lock);

// Keeps that lock held across the calls of this thread while it lives: for
// callers in the middle of the VFS's own bookkeeping (a stream closed as its
// last reference goes, mmap and munmap inside the VFS's walk of its memory
// regions), where another thread must not get in.
class ScopedKeepLock {
 public:
  ScopedKeepLock();
  ~ScopedKeepLock();

 private:
  ScopedKeepLock(const ScopedKeepLock&);
  void operator=(const ScopedKeepLock&);
};

// Runs |request| on the OPFS thread and returns request->result.
int Call(Request* request);

// Convenience wrappers; all return >= 0 on success or -errno.
int Stat(const std::string& path, EntryType* type, double* size,
         double* mtime);
int List(const std::string& path, std::vector<Entry>* entries);
int PathOp(Op op, const std::string& path,
           const std::string& path2 = std::string());

}  // namespace arc_opfs

#endif  // ARC_WASM_SHIM_OPFS_OPFS_BRIDGE_H_
