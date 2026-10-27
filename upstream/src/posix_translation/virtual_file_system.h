// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POSIX_TRANSLATION_VIRTUAL_FILE_SYSTEM_H_
#define POSIX_TRANSLATION_VIRTUAL_FILE_SYSTEM_H_

#include <dirent.h>
#include <errno.h>
#include <memory.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/vfs.h>
#include <termios.h>
#include <unistd.h>
#include <utime.h>

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "base/basictypes.h"
#include "base/compiler_specific.h"
#include "base/containers/hash_tables.h"
#include "base/memory/scoped_ptr.h"
#include "base/synchronization/condition_variable.h"
#include "base/synchronization/lock.h"
#include "common/export.h"
#include "posix_translation/abstract_socket_namespace.h"
#include "posix_translation/file_system_handler.h"
#include "posix_translation/host_resolver.h"
#include "posix_translation/logd_socket_namespace.h"
#include "posix_translation/virtual_file_system_interface.h"
#include "ppapi/cpp/file_system.h"
#include "ppapi/utility/completion_callback_factory.h"

namespace posix_translation {

class FdToFileStreamMap;
class FileStream;
class LocalSocket;
class MemoryRegion;
class MountPointManager;
class PepperFileHandler;
class PermissionInfo;
class ProcessEnvironment;
class ReadonlyFileHandler;

const ino_t kBadInode = -1;
const int kInvalidFileNo = -1;

// Returns the current VirtualFileSystemInterface instance used by
// libposix_translation.
ARC_EXPORT VirtualFileSystemInterface* GetVirtualFileSystemInterface();

// Replaces the current VirtualFileSystemInterface instace used by
// libposix_translation. The function takes ownership of |vfs|.
ARC_EXPORT void SetVirtualFileSystemInterface(VirtualFileSystemInterface* vfs);

// This class is an abstraction layer on top of multiple concrete file
// systems.
class VirtualFileSystem : public VirtualFileSystemInterface {
 public:
  // |min_fd| is the minimum file number used in the file system.
  // |max_fd| is the maximum.
  ARC_EXPORT VirtualFileSystem(pp::Instance* instance,
                               ProcessEnvironment* process_environment,
                               int min_fd,
                               int max_fd);
  virtual ~VirtualFileSystem();

  // Returns the current VirtualFileSystem instance used by
  // libposix_translation.
  // The returned instance is idential to
  //   static_cast<VirtualFileSystem*>(GetVirtualFileSystemInterface())
  // when it is actually VirtualFileSystem. Otherwise it aborts.
  // This function is not exported because it's intended to be called only
  // inside posix_translation.
  static VirtualFileSystem* GetVirtualFileSystem();

  static bool IsInitialized();

  // Implements file system functions.
  int accept(int sockfd, sockaddr* addr, socklen_t* addrlen);
  int access(const std::string& pathname, int mode);
  int bind(int sockfd, const sockaddr* serv_addr, int addrlen);
  int chdir(const std::string& path);
  int chown(const std::string& path, uid_t owner, gid_t group);
  int close(int fd);
  int connect(int sockfd, const sockaddr* serv_addr, socklen_t addrlen);
  int dup(int fd);
  int dup2(int fd, int newfd);
  int epoll_create1(int flags);
  int epoll_ctl(int epfd, int op, int fd,
                struct epoll_event* event);
  int epoll_wait(int epfd, struct epoll_event* events, int maxevents,
                 int timeout);
  int fcntl(int fd, int cmd, va_list ap);
  int fdatasync(int fd);
  long fpathconf(int fd, int name);  // NOLINT
  void freeaddrinfo(addrinfo* ai);
  int fstat(int fd, struct stat* out);
  int fstatfs(int fd, struct statfs* out);
  int fsync(int fd);
  int ftruncate(int fd, off64_t length);
  int getaddrinfo(const char* hostname, const char* servname,
                  const addrinfo* hints, addrinfo** res);
  char* getcwd(char* buf, size_t size);
  int getdents(int fd, dirent*, size_t count);
  struct hostent* gethostbyaddr(
      const void* addr, socklen_t len, int type);
  struct hostent* gethostbyname(const char* hostname);
  int gethostbyname_r(
      const char* hostname, struct hostent* ret,
      char* buf, size_t buflen,
      struct hostent** result, int* h_errnop);
  struct hostent* gethostbyname2(const char* hostname,
                                 int family);
  int gethostbyname2_r(
      const char* hostname, int family, struct hostent* ret,
      char* buf, size_t buflen,
      struct hostent** result, int* h_errnop);
  int getnameinfo(const sockaddr* sa, socklen_t salen,
                  char* host, size_t hostlen,
                  char* serv, size_t servlen, int flags);
  int getpeername(int s, sockaddr* name, socklen_t* namelen);
  int getsockname(int s, sockaddr* name, socklen_t* namelen);
  int getsockopt(int sockfd, int level, int optname, void* optval,
                 socklen_t* optlen);
  // arc-wasm (topic native-exec): spawned process |pid| gets descriptors
  // 0-2 of its own: the streams of |fds| (the caller's descriptors; -1:
  // closed). FdToFileStreamMap::SetProcessView.
  int SetupProcessFds(int pid, const int fds[3]);
  // Its end: what it left open is closed, its 0-2 let go.
  void ReleaseProcessFds(int pid);
  // arc-wasm: chmod/fchmod reach the file's handler (topic native-exec).
  int chmod(const std::string& pathname, mode_t mode);
  int fchmod(int fd, mode_t mode);
  int ioctl(int fd, int request, va_list ap);
  int listen(int sockfd, int backlog);
  off64_t lseek(int fd, off64_t offset, int whence);
  int lstat(const std::string& pathname, struct stat* out);
  int madvise(void* addr, size_t length, int advice);
  int mkdir(const std::string& pathname, mode_t mode);
  void* mmap(void* addr, size_t length, int prot, int flags, int fd,
             off_t offset);
  int mprotect(void* addr, size_t length, int prot);
  int munmap(void* addr, size_t length);
  int open(const std::string& pathname, int oflag,
           mode_t cmode);
  long pathconf(const std::string& pathname, int name);  // NOLINT
  int pipe2(int pipefd[2], int flags);
  int poll(struct pollfd* fds, nfds_t nfds, int timeout);
  ssize_t pread(int fd, void* buf, size_t count,
                off64_t offset);
  ssize_t pwrite(int fd, const void* buf, size_t count,
                 off64_t offset);
  ssize_t read(int fd, void* buf, size_t count);
  ssize_t readlink(const std::string& pathname, char* buf,
                   size_t bufsiz);
  int readv(int fd, const struct iovec* iovec, int count);
  char* realpath(const char* path, char* resolved_path);
  ssize_t recv(int sockfd, void* buf, size_t len, int flags);
  ssize_t recvfrom(int socket, void* buffer, size_t len, int flags,
                   sockaddr* addr, socklen_t* addrlen);
  int recvmsg(int sockfd, struct msghdr* msg, int flags);
  int remove(const std::string& pathname);
  int rename(const std::string& oldpath,
             const std::string& newpath);
  int rmdir(const std::string& pathname);
  int select(int nfds, fd_set* readfds, fd_set* writefds,
             fd_set* exceptfds, struct timeval* timeout);
  ssize_t send(int sockfd, const void* buf, size_t len, int flags);
  ssize_t sendto(int sockfd, const void* buf, size_t len, int flags,
                 const sockaddr* dest_addr, socklen_t addrlen);
  int sendmsg(int sockfd, const struct msghdr* msg, int flags);
  int setsockopt(int sockfd, int level, int optname, const void* optval,
                 socklen_t optlen);
  int shutdown(int sockfd, int how);
  int socket(int socket_family, int socket_type, int protocol);
  int socketpair(int socket_family, int socket_type, int protocol,
                 int sv[2]);
  int stat(const std::string& pathname, struct stat* out);
  int statfs(const std::string& pathname, struct statfs* out);
  int statvfs(const std::string& pathname,
              struct statvfs* out);
  int symlink(const std::string& oldpath,
              const std::string& newpath);
  int truncate(const std::string& pathname, off64_t length);
  mode_t umask(mode_t mask);
  int unlink(const std::string& pathname);
  int utime(const std::string& pathname,
            const struct utimbuf* times);
  int utimes(const std::string& pathname,
             const struct timeval times[2]);
  ssize_t write(int fd, const void* buf, size_t count);
  int writev(int fd, const struct iovec* iov, int count);

  // VirtualFileSystemInterface overrides.
  virtual void Mount(const std::string& path,
                     FileSystemHandler* handler) OVERRIDE;
  virtual void Unmount(const std::string& path) OVERRIDE;
  virtual void ChangeMountPointOwner(const std::string& path,
                                     uid_t owner_uid) OVERRIDE;

  virtual void SetBrowserReady() OVERRIDE;
  virtual void InvalidateCache() OVERRIDE;
  virtual void AddToCache(const std::string& path, const PP_FileInfo& file_info,
                          bool exists) OVERRIDE;
  virtual void SchedulePreopen(const std::string& path) OVERRIDE;
  virtual bool RegisterFileStream(int fd,
                                  scoped_refptr<FileStream> stream) OVERRIDE;
  virtual FileSystemHandler* GetFileSystemHandler(
      const std::string& path) OVERRIDE;
  virtual std::string GetMemoryMapAsString() OVERRIDE;
  virtual std::string GetIPCStatsAsString() OVERRIDE;
  virtual int StatForTesting(
      const std::string& pathname, struct stat* out) OVERRIDE;

  // TODO(crbug.com/245003): Get rid of the getter.
  base::Lock& mutex() { return mutex_; }

  pp::Instance* instance() { return instance_; }

  // Condition variable operations.
  // Blocks current thread and waits for the condition variable is signaled.
  void Wait();

  // Blocks current thread and waits for the condition variable to be signaled
  // until |time_limit|. Returns true if it is timed out.
  // If |time_limit| is null (i.e. is_null() returns true), this blocks the
  // current thread forever until the condition variable is signaled.
  // See WaitUntil() in time_util.{h,cc} for more details.
  bool WaitUntil(const base::TimeTicks& time_limit);

  void Broadcast();

  // Return true if the file system initialization on the browser side has
  // already been done.
  bool IsBrowserReadyLocked() const;

  // Checks if |fd| is managed by posix_translation.
  bool IsKnownDescriptor(int fd);

  // Return an inode number for the |path|. If it's not assigned yet, assign
  // a new number and return it.
  ino_t GetInodeLocked(const std::string& path);

  // The same as GetInodeLocked() except that this function does not check if
  // |path| is normalized. This function is only for implementing GetInodeLocked
  // and lstat. Always use GetInodeLocked instead.
  ino_t GetInodeUncheckedLocked(const std::string& path);

  // Remove the inode number for the |path| assigned by GetInodeLocked().
  void RemoveInodeLocked(const std::string& path);
  // Reassign the inode for |oldpath| to |newpath|. This is for supporting
  // rename(2).
  void ReassignInodeLocked(const std::string& oldpath,
                           const std::string& newpath);

  void DebugWriteLocked(int fd, const void* buf, size_t count);

  int AddFileStreamLocked(scoped_refptr<FileStream> stream);
  bool CloseLocked(int fd);
  // arc-wasm: the file a pathname AF_UNIX socket's bind() makes (an empty
  // regular file here: apps chmod and unlink it), kept if it is there.
  void CreateSocketFileLocked(const std::string& path);
  // Whether |path| names a file (connect() to a path: ENOENT or ECONNREFUSED).
  bool PathExistsLocked(const std::string& path);
  int DupLocked(int fd, int newfd);

  int GetMinFd() const;
  int GetMaxFd() const;

  scoped_refptr<FileStream> GetStreamLocked(int fd);

  AbstractSocketNamespace* GetAbstractSocketNamespace() {
    return &abstract_socket_namespace_;
  }

  LogdSocketNamespace* GetLogdSocketNamespace() {
    return &logd_socket_namespace_;
  }

  // Option to specify how to normalize a path. Public for testing.
  enum NormalizeOption {
    // Resolve all symlinks for a path.
    // Example: /link/link/link -> /dir/dir/file
    kResolveSymlinks,
    // Resolve parent symlinks for a path. This is used for implementing
    // functions that handles symlinks such as readlink() and lstat().
    // Example: /link/link/link -> /dir/dir/link
    kResolveParentSymlinks,
    kDoNotResolveSymlinks,
  };

  // Converts |in_out_path| to an absolute path. If |option| is
  // kResolveSymlinks or kResolveParentSymlinks, symlinks are resolved.
  void GetNormalizedPathLocked(std::string* in_out_path,
                               NormalizeOption option);

 private:
  enum SelectReadyEvent {
    SELECT_READY_READ,
    SELECT_READY_WRITE,
    SELECT_READY_EXCEPTION
  };

  struct FileDescNamePair {
    FileDescNamePair() : fd_(kInvalidFileNo) {}
    FileDescNamePair(int fd, const char* name) :
      fd_(fd), name_(name) {}
    int fd_;
    std::string name_;
  };

  friend class FileSystemTestCommon;
  template <typename> friend class FileSystemBackgroundTestCommon;
  friend class MemoryRegionTest;
  friend class PepperFileTest;
  friend class PepperTCPSocketTest;

  typedef base::hash_map<std::string, ino_t> InodeMap;  // NOLINT

  // Gets the FileSystemHandler object for |path|. See the comment in
  // mount_point_manager.h for detail about the return value.
  // Also sets |out_permission| if not NULL.
  FileSystemHandler* GetFileSystemHandlerLocked(
      const std::string& path, PermissionInfo* out_permission);

  int GetFirstUnusedDescriptorLocked();

  int OpenLocked(
      const std::string& pathname, int oflag, mode_t cmode, bool use_preopened);

  int IsSelectReadyLocked(int nfds, fd_set* fds,
                          SelectReadyEvent event,
                          bool apply);
  int IsPollReadyLocked(struct pollfd* fds, nfds_t nfds, bool apply);

  // Returns true if all memory pages in [addr, addr+length) are not in use.
  // This is for testing.
  bool IsMemoryRangeAvailableLocked(void* addr, size_t length);

  int StatLocked(const std::string& pathname, struct stat* out);

  // Returns true if |path| is already normalized with kResolveSymlinks.
  bool IsNormalizedPathLocked(const std::string& path);

  // Sets appropriate errno for file creation. This function should be
  // called only when we already know write access to |path| is denied.
  // |path| must be already normalized.|path| might be modified in the
  // function. This function always returns -1.
  int DenyAccessForCreateLocked(std::string* path, FileSystemHandler* handler);

  // Sets appropriate errno for file modification. See above comment
  // for other details of this function.
  int DenyAccessForModifyLocked(const std::string& path,
                                FileSystemHandler* handler);

  // Starts preopening files as scheduled by SchedulePreopen().
  void StartPreopenLocked();
  static void* PreopenThreadMain(void* arg);
  void PerformPreopens();

  // Invalidates preopened files.
  void ClosePreopenedFilesWithResolvedPathLocked(
      const std::string& resolved_path);

  // Gets a /proc/self/maps like memory map for debugging in a human readable
  // format.
  std::string GetMemoryMapAsStringLocked();

  // Resolves symlinks in path in-place.
  // TODO(satorux): Write a unit test for this function once gmock is gone
  // from virtual_file_system_test.cc crbug.com/335430.
  void ResolveSymlinks(std::string* in_out_path);

  static VirtualFileSystem* file_system_;

  // True if the file system initialization on the browser side has been done.
  bool browser_ready_;

  pp::Instance* instance_;

  ProcessEnvironment* process_environment_;

  // TODO(crbug.com/245003): Stop locking the |mutex_| when calling into
  // FileSystemHandler/FileStream.
  base::Lock mutex_;
  // TODO(yusukes): Remove this global cond_. All condition variables
  // should be targeted to specific functions or streams to reduce contention
  // on var's internal lock. At the same time try to avoid using broadcast().
  base::ConditionVariable cond_;

  scoped_ptr<FdToFileStreamMap> fd_to_stream_;
  scoped_ptr<MemoryRegion> memory_region_;
  InodeMap inodes_;
  ino_t next_inode_;
  scoped_ptr<MountPointManager> mount_points_;
  AbstractSocketNamespace abstract_socket_namespace_;
  // TODO(crbug/513081): Implement UNIX domain socket with names and remove this
  LogdSocketNamespace logd_socket_namespace_;

  HostResolver host_resolver_;

  bool preopen_started_;

  // The list of resolved paths of files scheduled for preopen.
  std::vector<std::string> scheduled_preopens_;

  // The map from resolved path to fd of preopened files.
  // If a file was scheduled for preopen but not opened yet, the value is set to
  // kPreopenPendingFd.
  typedef std::multimap<std::string, int> PreopenedFdMultimap;
  PreopenedFdMultimap preopened_fds_;

  bool abort_on_unexpected_memory_maps_;  // For unit testing.
  std::map<int, FileDescNamePair> debug_fds_;

  DISALLOW_COPY_AND_ASSIGN(VirtualFileSystem);
};

}  // namespace posix_translation
#endif  // POSIX_TRANSLATION_VIRTUAL_FILE_SYSTEM_H_
