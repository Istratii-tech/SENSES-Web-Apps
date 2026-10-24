// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Base interface for file API.

#ifndef POSIX_TRANSLATION_FILE_STREAM_H_
#define POSIX_TRANSLATION_FILE_STREAM_H_

#include <dirent.h>
#include <errno.h>
#include <sys/epoll.h>
#include <sys/mman.h>  // MAP_FAILED
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/vfs.h>
#include <termios.h>
#include <unistd.h>

#include <map>
#include <string>

#include "base/basictypes.h"
#include "base/memory/ref_counted.h"
#include "common/arc_strace.h"
#include "posix_translation/permission_info.h"

namespace posix_translation {

class FileStream : public base::RefCounted<FileStream> {
 public:
  FileStream(int oflag, const std::string& pathname);

  const PermissionInfo& permission() const {
    return permission_;
  }
  void set_permission(const PermissionInfo& permission) {
    permission_ = permission;
  }

  virtual bool IsAllowedOnMainThread() const;

  // Returns true if FileStream that returns the same address when mmap() is
  // called twice or more. Such a stream needs a special handling in
  // MemoryRegion.
  virtual bool ReturnsSameAddressForMultipleMmaps() const;

  // Adds a file reference to allow the object to call OnLastFileRef() later
  // when the file reference count is dropped to zero.
  void AddFileRef();
  // Releases a file reference. OnLastFileRef() might be called.
  void ReleaseFileRef();

  // Sorted by syscall name.
  virtual int accept(sockaddr* addr, socklen_t* addrlen);
  virtual int bind(const sockaddr* addr, socklen_t addrlen);
  virtual int connect(const sockaddr* addr, socklen_t addrlen);
  virtual int epoll_ctl(
      int op, scoped_refptr<FileStream> file, struct epoll_event* event);
  virtual int epoll_wait(struct epoll_event* events, int maxevents,
                         int timeout);
  virtual int fcntl(int cmd, va_list ap);
  virtual int fdatasync();
  virtual int fstat(struct stat* out);
  virtual int fstatfs(struct statfs* out);
  virtual int fsync();
  virtual int ftruncate(off64_t length);
  virtual int getdents(dirent* buf, size_t count);
  virtual int getpeername(sockaddr* name, socklen_t* namelen);
  virtual int getsockname(sockaddr* name, socklen_t* namelen);
  virtual int getsockopt(int level, int optname, void* optval,
                         socklen_t* optlen);
  virtual int ioctl(int request, va_list ap);
  virtual int listen(int backlog);
  virtual off64_t lseek(off64_t offset, int whence);
  // If madvise returns 1, VFS should abort immediately.
  virtual int madvise(void* addr, size_t length, int advice);
  virtual void* mmap(
      void* addr, size_t length, int prot, int flags, off_t offset);
  // If mprotect returns 1, VFS should abort immediately.
  virtual int mprotect(void* addr, size_t length, int prot);
  virtual int munmap(void* addr, size_t length);
  virtual ssize_t pread(void* buf, size_t count, off64_t offset);
  virtual ssize_t read(void* buf, size_t count) = 0;
  virtual int readv(const struct iovec* iov, int count);
  virtual ssize_t recv(void* buf, size_t len, int flags);
  virtual ssize_t recvfrom(void* buf, size_t len, int flags, sockaddr* addr,
                           socklen_t* addrlen);
  virtual int recvmsg(struct msghdr* msg, int flags);
  virtual ssize_t send(const void* buf, size_t len, int flags);
  virtual ssize_t sendto(const void* buf, size_t len, int flags,
                         const sockaddr* dest_addr, socklen_t addrlen);
  virtual int sendmsg(const struct msghdr* msg, int flags);
  virtual int setsockopt(int level, int optname, const void* optval,
                         socklen_t optlen);
  // Note: In write() and writev(), do not call any function which
  // directly or indirectly calls ::write() or libc's write().
  // It will be trapped at IRT layer and may loop back to VirtualFileSystem and
  // FileStream. printf() and fprintf() are good examples. ARC logging
  // functions in common/alog.h and common/arc_strace.h are safe to call.
  virtual ssize_t write(const void* buf, size_t count) = 0;
  virtual int writev(const struct iovec* iov, int count);

  // For the implementation of select().
  // Streams which support select must override these 3 functions.
  // Implementations of these IsSelect*Ready() functions *must* return
  // immediately without communicating with the main thread. Otherwise, select
  // syscall families with short timeout might not work as expected.
  virtual bool IsSelectReadReady() const;
  virtual bool IsSelectWriteReady() const;
  virtual bool IsSelectExceptionReady() const;

  // For the implementation of poll().
  // Returns the bits of poll events, e.g. (POLLIN | POLLOUT).
  // This function *must* return immediately without communicating with the
  // main thread.
  virtual int16_t GetPollEvents() const;

  // TODO(crbug.com/359400): Currently, poll uses IsSelect*Ready() family
  // incorrectly, due to historical reason. Fix the implementation.

  // Called when the memory region [addr, addr+length) associated when the
  // stream is implicitly unmapped without munmap. This happens with the
  // region is overwritten by another mmap call with MAP_FIXED. File handlers
  // that do not support the implicit unmap with MAP_FIXED should override
  // this function to call abort.
  // TODO(crbug.com/418801): Remove once we fix 418801 and change dev_ashmem.cc
  // to use a shared memory IRT which does not exist today.
  virtual void OnUnmapByOverwritingMmap(void* addr, size_t length);

  // For debugging.
  virtual const char* GetStreamType() const = 0;
  virtual size_t GetSize() const;
  virtual std::string GetAuxInfo() const;

  // A debug-only version of write used for saving stdout/stderr logs to disk.
  virtual void debug_write(const void* buf, size_t count) {}

  // A non-virtual wrapper around write() and PwriteImpl().
  ssize_t pwrite(const void* buf, size_t count, off64_t offset);

  // Debug check verifying that this file has not lost its last reference.
  void CheckNotClosed() const;

  int oflag() const { return oflag_; }
  void set_oflag(int oflag) { oflag_ = oflag; }
  ino_t inode() const { return inode_; }
  const std::string& pathname() const { return pathname_; }

 protected:
  friend class base::RefCounted<FileStream>;
  virtual ~FileStream();

  // Invoked by the non-virtual pwrite() above.
  virtual ssize_t PwriteImpl(const void* buf, size_t count, off64_t offset);

  // Invoked upon release of the last file reference.
  virtual void OnLastFileRef();

  // TODO(crbug.com/284239): Functions below are mostly for socket
  // related classes. Create a base class for them and move them to
  // the new class.

  // Allows this file to be listened to.
  void EnableListenerSupport() {
    is_listening_enabled_ = true;
  }

  // Listener invokes this on itself to start listening to a particular file.
  // Returns false if file does not support listeners (cannot notify them).
  bool StartListeningTo(scoped_refptr<FileStream> file);

  // Listener invokes this on itself to stop listening to a particular file.
  void StopListeningTo(scoped_refptr<FileStream> file);

  // Notifies all registered listeners.
  void NotifyListeners();

  // Called on listener to notify about a change in file.
  virtual void HandleNotificationFrom(
      scoped_refptr<FileStream> file, bool is_closing);

  // Returns true if this file has lost its last reference.
  bool IsClosed() const;

 private:
  // The key is FileStream*, obfuscated to avoid direct use.
  typedef std::map<void*, scoped_refptr<FileStream> > FileMap;

  int oflag_;
  // -1 when the stream is not associated with a file (e.g. socket).
  ino_t inode_;
  // "" when the stream is not associated with a file (e.g. socket).
  const std::string pathname_;
  bool is_listening_enabled_;
  FileMap listeners_;
  // Permission of this file. VirtualFileSystem sets this value for
  // FileStream created by FileSystemHandler. Other FileStream should fill
  // this by themselves.
  PermissionInfo permission_;
  // The number of open-file references this stream currently has.
  // It is different from base::RefCounted, as the latter merely counts
  // the code references and prevents the object from being destroyed.
  // file_ref_count_, on the other hand, allows tracking of the actual
  // use count, such as with open or duplicated fd's.
  int file_ref_count_;
  // True if this stream ever had positive file_ref_count_.
  // This field is needed for integrity checks only.
  bool had_file_refs_;
};

}  // namespace posix_translation

#endif  // POSIX_TRANSLATION_FILE_STREAM_H_
