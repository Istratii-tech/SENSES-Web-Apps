// A writable file in memory for the build-time dexopt tool (dexpreopt.cc):
// dvmContinueOptimization() writes the .odex through a file descriptor with
// write/lseek/read and a writable MAP_SHARED mmap, and under Node the VFS has
// no writable file system. The file is registered directly as a descriptor
// (like stdio in arc_kernel::Boot) and its contents are read back at the end.
//
// Compiled with the posix_translation headers; dexpreopt.cc (Dalvik headers)
// uses the C functions at the bottom. Part of topic "dexopt"
// (docs/topics/dexopt.md).

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <algorithm>
#include <map>
#include <vector>

#include "common/process_emulator.h"
#include "posix_translation/file_stream.h"
#include "posix_translation/permission_info.h"
#include "posix_translation/virtual_file_system.h"

namespace {

using posix_translation::FileStream;

class MemoryFile : public FileStream {
 public:
  // No pathname: the file is not in any directory (and needs no inode).
  MemoryFile() : FileStream(O_RDWR, std::string()), position_(0) {}

  const std::vector<char>& data() const { return data_; }

  virtual ssize_t read(void* buf, size_t count) OVERRIDE {
    const ssize_t n = pread(buf, count, position_);
    if (n > 0)
      position_ += n;
    return n;
  }

  virtual ssize_t write(const void* buf, size_t count) OVERRIDE {
    const ssize_t n = PwriteImpl(buf, count, position_);
    if (n > 0)
      position_ += n;
    return n;
  }

  virtual ssize_t pread(void* buf, size_t count, off64_t offset) OVERRIDE {
    if (offset < 0) {
      errno = EINVAL;
      return -1;
    }
    if (offset >= static_cast<off64_t>(data_.size()))
      return 0;
    const size_t n = std::min(count, data_.size() - static_cast<size_t>(offset));
    memcpy(buf, &data_[offset], n);
    return n;
  }

  virtual off64_t lseek(off64_t offset, int whence) OVERRIDE {
    off64_t base = 0;
    if (whence == SEEK_CUR) {
      base = position_;
    } else if (whence == SEEK_END) {
      base = data_.size();
    } else if (whence != SEEK_SET) {
      errno = EINVAL;
      return -1;
    }
    if (base + offset < 0) {
      errno = EINVAL;
      return -1;
    }
    position_ = base + offset;
    return position_;
  }

  virtual int fstat(struct stat* out) OVERRIDE {
    memset(out, 0, sizeof(*out));
    out->st_mode = S_IFREG | 0644;
    out->st_nlink = 1;
    out->st_size = data_.size();
    out->st_blksize = 4096;
    return 0;
  }

  virtual int ftruncate(off64_t length) OVERRIDE {
    if (length < 0) {
      errno = EINVAL;
      return -1;
    }
    data_.resize(length);
    return 0;
  }

  virtual int fsync() OVERRIDE { return 0; }
  virtual int fdatasync() OVERRIDE { return 0; }

  // Copy-in mapping on anonymous pages (inside the kernel, ::mmap is the wasm
  // page allocator), written back on munmap, as OpfsFile does.
  virtual void* mmap(void* addr, size_t length, int prot, int flags,
                     off_t offset) OVERRIDE {
    void* result = ::mmap(addr, length, prot | PROT_WRITE,
                          (flags & MAP_FIXED) | MAP_PRIVATE | MAP_ANONYMOUS,
                          -1, 0);
    if (result == MAP_FAILED)
      return MAP_FAILED;
    pread(result, length, offset);
    if ((flags & MAP_SHARED) && (prot & PROT_WRITE)) {
      SharedMapping m = {length, offset};
      shared_mappings_[result] = m;
    }
    return result;
  }

  virtual int munmap(void* addr, size_t length) OVERRIDE {
    std::map<void*, SharedMapping>::iterator it = shared_mappings_.find(addr);
    if (it != shared_mappings_.end()) {
      // Only the part that lies within the file; mappings do not extend it.
      const off_t offset = it->second.offset;
      if (offset < static_cast<off_t>(data_.size())) {
        const size_t n = std::min(it->second.length,
                                  data_.size() - static_cast<size_t>(offset));
        memcpy(&data_[offset], addr, n);
      }
      shared_mappings_.erase(it);
    }
    return ::munmap(addr, length);
  }

  virtual const char* GetStreamType() const OVERRIDE { return "memory"; }
  virtual size_t GetSize() const OVERRIDE { return data_.size(); }

 protected:
  virtual ~MemoryFile() {}

  virtual ssize_t PwriteImpl(const void* buf, size_t count,
                             off64_t offset) OVERRIDE {
    if (offset < 0) {
      errno = EINVAL;
      return -1;
    }
    if (offset + count > data_.size())
      data_.resize(offset + count);
    memcpy(&data_[offset], buf, count);
    return count;
  }

 private:
  struct SharedMapping {
    size_t length;
    off_t offset;
  };

  std::vector<char> data_;
  off64_t position_;
  std::map<void*, SharedMapping> shared_mappings_;
};

std::map<int, scoped_refptr<MemoryFile> > g_files;

}  // namespace

// Registers an empty in-memory file as descriptor "fd" (which must be
// unused). Returns 0 or -1.
extern "C" int arc_dexopt_open_memory_file(int fd) {
  scoped_refptr<MemoryFile> file = new MemoryFile;
  file->set_permission(posix_translation::PermissionInfo(
      arc::ProcessEmulator::GetUid(), true /* writable */));
  if (!posix_translation::VirtualFileSystem::GetVirtualFileSystem()
           ->RegisterFileStream(fd, file))
    return -1;
  g_files[fd] = file;
  return 0;
}

// The contents of the file registered as "fd" (valid until the next write).
extern "C" const void* arc_dexopt_memory_file_data(int fd, size_t* size) {
  std::map<int, scoped_refptr<MemoryFile> >::iterator it = g_files.find(fd);
  if (it == g_files.end())
    return NULL;
  *size = it->second->data().size();
  return it->second->data().data();
}
