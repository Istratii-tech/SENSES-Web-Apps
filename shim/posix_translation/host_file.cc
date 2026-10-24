// See host_file.h.

#include "posix_translation/host_file.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

#include "posix_translation/directory_file_stream.h"
#include "posix_translation/directory_manager.h"
#include "posix_translation/passthrough.h"
#include "posix_translation/statfs.h"

namespace posix_translation {

HostFileHandler::HostFileHandler(const std::string& host_root)
    : FileSystemHandler("HostFileHandler"), host_root_(host_root) {}

HostFileHandler::~HostFileHandler() {}

std::string HostFileHandler::HostPath(const std::string& pathname) const {
  return host_root_ + pathname;
}

scoped_refptr<FileStream> HostFileHandler::open(
    int fd, const std::string& pathname, int oflag, mode_t cmode) {
  if ((oflag & O_ACCMODE) != O_RDONLY || (oflag & (O_CREAT | O_TRUNC))) {
    errno = EACCES;  // Host files are read-only.
    return NULL;
  }
  // This is the host libc, not the VFS: posix_translation does not hook libc
  // in the wasm port.
  struct stat st;
  if (::stat(HostPath(pathname).c_str(), &st) == 0 && S_ISDIR(st.st_mode))
    return new DirectoryFileStream("host", pathname, this);
  const int native_fd = ::open(HostPath(pathname).c_str(), O_RDONLY);
  if (native_fd < 0)
    return NULL;
  return new PassthroughStream(native_fd, pathname, oflag,
                               true /* close_on_destruction */);
}

Dir* HostFileHandler::OnDirectoryContentsNeeded(const std::string& name) {
  // The host's listing (e.g. /sys/class/net, net_interfaces.cc), through
  // DirectoryManager for "." / ".." and sorting, as OpfsHandler does.
  DIR* host = ::opendir(HostPath(name).c_str());
  if (!host)
    return NULL;
  DirectoryManager listing;
  std::string dir = name;
  if (dir.empty() || dir[dir.size() - 1] != '/')
    dir += '/';
  listing.MakeDirectories(dir);
  while (const dirent* e = ::readdir(host)) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
      continue;
    if (e->d_type == DT_DIR)
      listing.MakeDirectories(dir + e->d_name + "/");
    else
      listing.AddFileWithType(dir + e->d_name, Dir::REGULAR);
  }
  ::closedir(host);
  return listing.OpenDirectory(dir);
}

int HostFileHandler::stat(const std::string& pathname, struct stat* out) {
  return ::stat(HostPath(pathname).c_str(), out);
}

int HostFileHandler::statfs(const std::string& pathname, struct statfs* out) {
  return DoStatFsForSystem(out);
}

}  // namespace posix_translation
