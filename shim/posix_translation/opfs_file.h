// Writable, persistent storage for posix_translation backed by the browser's
// Origin Private File System -- the wasm counterpart of ARC's Pepper-backed
// PepperFileHandler (HTML5 file system), typically mounted at /data.
//
// Files live under an OPFS directory, by default "arc-wasm/" (or another one
// the page picks, e.g. "senses/data/a81": each stack of SENSES Web Apps has
// its own /data; topic install8), mirroring the paths below the mount point.
// A read-only handler (the app library mounted at /mnt/apps) reads files
// through File objects instead of sync access handles: no lock on the file,
// nothing counted against Safari's handle limit, and every change is EROFS.
// Limitations come from OPFS itself:
//   * no permissions, owners, links or symlinks; directories have no mtime;
//   * OPFS refuses to remove or move a file with an open sync access handle:
//     an open file that is unlinked or renamed (or renamed over) is closed
//     and moved by the bridge (library_arc_opfs.js: moveOpen), its
//     descriptors keep working, an unlinked one goes at its last close;
//   * renaming needs FileSystemHandle.move() (Chrome, Safari; EXDEV if absent).
//
// Every operation is a synchronous round trip to the OPFS thread (see
// opfs_bridge.h). Part of the arc-wasm shim layer.
#ifndef POSIX_TRANSLATION_OPFS_FILE_H_
#define POSIX_TRANSLATION_OPFS_FILE_H_

#include <map>
#include <string>
#include <vector>

#include "base/basictypes.h"
#include "posix_translation/file_system_handler.h"
#include "posix_translation/file_stream.h"

namespace posix_translation {

class OpfsHandler : public FileSystemHandler {
 public:
  // |base|: the OPFS directory of the mount point, "/DIR[/DIR...]" from the
  // origin's OPFS root; created on first use unless |read_only|.
  explicit OpfsHandler(const std::string& base = "/arc-wasm",
                       bool read_only = false);
  virtual ~OpfsHandler();

  virtual bool IsInitialized() const OVERRIDE;
  virtual void Initialize() OVERRIDE;
  virtual void OnMounted(const std::string& path) OVERRIDE;
  // A single-app runtime: the storage belongs to the app.
  virtual bool IsWorldWritable(const std::string& pathname) OVERRIDE;

  virtual scoped_refptr<FileStream> open(
      int fd, const std::string& pathname, int oflag, mode_t cmode) OVERRIDE;
  virtual Dir* OnDirectoryContentsNeeded(const std::string& name) OVERRIDE;
  virtual int mkdir(const std::string& pathname, mode_t mode) OVERRIDE;
  virtual int rename(const std::string& oldpath,
                     const std::string& newpath) OVERRIDE;
  virtual int rmdir(const std::string& pathname) OVERRIDE;
  virtual int stat(const std::string& pathname, struct stat* out) OVERRIDE;
  virtual int statfs(const std::string& pathname, struct statfs* out) OVERRIDE;
  virtual int truncate(const std::string& pathname, off64_t length) OVERRIDE;
  virtual int unlink(const std::string& pathname) OVERRIDE;
  virtual int utimes(const std::string& pathname,
                     const struct timeval times[2]) OVERRIDE;

  // The OPFS path for a VFS path below the mount point.
  std::string ToOpfsPath(const std::string& pathname) const;
  bool read_only() const { return read_only_; }

 private:
  std::string mount_point_;  // Without trailing slash.
  const std::string base_;   // "/arc-wasm": no trailing slash.
  const bool read_only_;
  bool initialized_;
  DISALLOW_COPY_AND_ASSIGN(OpfsHandler);
};

// A read-only directory standing for the mount points below it: /mnt for
// /mnt/apps (topic install8). Without it /mnt would not exist, and path
// walks (realpath, lstat of each component) would fail before reaching
// the mount below.
class MountPointDirHandler : public FileSystemHandler {
 public:
  explicit MountPointDirHandler(const std::vector<std::string>& children);
  virtual ~MountPointDirHandler();

  virtual bool IsInitialized() const OVERRIDE { return true; }
  virtual void OnMounted(const std::string& path) OVERRIDE;
  virtual scoped_refptr<FileStream> open(
      int fd, const std::string& pathname, int oflag, mode_t cmode) OVERRIDE;
  virtual Dir* OnDirectoryContentsNeeded(const std::string& name) OVERRIDE;
  virtual int stat(const std::string& pathname, struct stat* out) OVERRIDE;
  virtual int statfs(const std::string& pathname, struct statfs* out) OVERRIDE;

 private:
  bool IsSelf(const std::string& pathname) const;

  std::string mount_point_;  // Without trailing slash.
  const std::vector<std::string> children_;
  DISALLOW_COPY_AND_ASSIGN(MountPointDirHandler);
};

class OpfsFile : public FileStream {
 public:
  OpfsFile(int id, int oflag, const std::string& pathname,
           OpfsHandler* handler);

  virtual int fstat(struct stat* out) OVERRIDE;
  virtual int fstatfs(struct statfs* out) OVERRIDE;
  virtual int fsync() OVERRIDE;
  virtual int fdatasync() OVERRIDE;
  virtual int ftruncate(off64_t length) OVERRIDE;
  virtual int ioctl(int request, va_list ap) OVERRIDE;
  virtual off64_t lseek(off64_t offset, int whence) OVERRIDE;
  virtual void* mmap(void* addr, size_t length, int prot, int flags,
                     off_t offset) OVERRIDE;
  virtual int munmap(void* addr, size_t length) OVERRIDE;
  virtual ssize_t pread(void* buf, size_t count, off64_t offset) OVERRIDE;
  virtual ssize_t read(void* buf, size_t count) OVERRIDE;
  virtual ssize_t write(const void* buf, size_t count) OVERRIDE;
  virtual const char* GetStreamType() const OVERRIDE;
  virtual size_t GetSize() const OVERRIDE;

 protected:
  virtual ~OpfsFile();
  virtual ssize_t PwriteImpl(const void* buf, size_t count,
                             off64_t offset) OVERRIDE;

 private:
  struct SharedMapping {
    size_t length;
    off_t offset;
  };

  ssize_t ReadAt(void* buf, size_t count, off64_t offset);
  ssize_t WriteAt(const void* buf, size_t count, off64_t offset);
  int GetSizeAndMtime(double* size, double* mtime) const;

  const int id_;
  off64_t position_;
  OpfsHandler* handler_;
  // Writable MAP_SHARED regions, written back on munmap.
  std::map<void*, SharedMapping> shared_mappings_;

  DISALLOW_COPY_AND_ASSIGN(OpfsFile);
};

}  // namespace posix_translation

#endif  // POSIX_TRANSLATION_OPFS_FILE_H_
