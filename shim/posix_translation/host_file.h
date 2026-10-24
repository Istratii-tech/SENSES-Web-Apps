// Read-only access to files of the wasm host (Emscripten's file system:
// embedded/preloaded files today, OPFS or HTTP-backed later).
//
// This is the wasm counterpart of ARC's NaClManifestFileHandler, which served
// files shipped with the runtime from the NaCl manifest. A typical use is as
// the underlying handler of a ReadonlyFileHandler, which then serves an
// Android image such as /system from a single host file. Part of the arc-wasm
// shim layer.
#ifndef POSIX_TRANSLATION_HOST_FILE_H_
#define POSIX_TRANSLATION_HOST_FILE_H_

#include <string>

#include "base/basictypes.h"
#include "posix_translation/file_system_handler.h"

namespace posix_translation {

class HostFileHandler : public FileSystemHandler {
 public:
  // VFS paths are looked up in the host file system under |host_root|
  // ("" means as-is).
  explicit HostFileHandler(const std::string& host_root = std::string());
  virtual ~HostFileHandler();

  virtual scoped_refptr<FileStream> open(
      int fd, const std::string& pathname, int oflag, mode_t cmode) OVERRIDE;
  virtual Dir* OnDirectoryContentsNeeded(const std::string& name) OVERRIDE;
  virtual int stat(const std::string& pathname, struct stat* out) OVERRIDE;
  virtual int statfs(const std::string& pathname, struct statfs* out) OVERRIDE;

 private:
  std::string HostPath(const std::string& pathname) const;

  const std::string host_root_;
  DISALLOW_COPY_AND_ASSIGN(HostFileHandler);
};

}  // namespace posix_translation

#endif  // POSIX_TRANSLATION_HOST_FILE_H_
