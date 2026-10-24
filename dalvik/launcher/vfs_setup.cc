// File systems for the Dalvik launcher (compiled with the posix_translation
// headers, separately from dalvikvm.cc, which uses Dalvik's).
//
//   /system  read-only image packaged with dalvikvm.js (/arc/system.img;
//            dalvikvm.data, or in the wasm with DSYSTEM_PACKAGING=embed):
//            framework/core.jar, usr/icu/icudt51l.dat, test jars
//   /data    the browser's OPFS when available (persistent), else absent:
//            its directory "arc-wasm", or $ARC_DATA_DIR ("senses/data/a81":
//            SENSES Web Apps gives each stack its own; topic install8)
//   /mnt/apps with $ARC_APPS_DIR ("senses/apps"): SENSES Web Apps' library
//            of APKs (APPS_DIR/PACKAGE/base.apk), read-only, in the same
//            storage as /data; PackageManagerService installs from there
//   $EXTERNAL_STORAGE/Android/obb/PACKAGE with $ARC_APP_OBB=PACKAGE: that
//            app's OBB files from the library (APPS_DIR/PACKAGE/obb), read-only
//   $ARC_OPFS_ROOT_MOUNT (tests): the whole storage, writable, there
//   /dev     null, zero, urandom, random, ashmem, alarm (arc_kernel::Boot),
//            binder (shim/binder)

#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "binder/dev_binder.h"
#include "kernel/arc_kernel.h"
#include "opfs/opfs_bridge.h"
#include "posix_translation/host_file.h"
#include "posix_translation/opfs_file.h"
#include "posix_translation/readonly_file.h"
#include "posix_translation/virtual_file_system.h"

namespace {

// "senses/data/a81" -> "/senses/data/a81"; "" when unset or empty.
std::string OpfsDir(const char* name) {
  const char* v = getenv(name);
  std::string dir = v ? v : "";
  while (!dir.empty() && dir[dir.size() - 1] == '/')
    dir.erase(dir.size() - 1);
  if (!dir.empty() && dir[0] != '/')
    dir = "/" + dir;
  return dir;
}

// /data, and SENSES Web Apps' library at /mnt/apps (topic install8).
void MountOpfs(posix_translation::VirtualFileSystem* vfs) {
  const std::string data = OpfsDir("ARC_DATA_DIR");
  vfs->Mount("/data/", new posix_translation::OpfsHandler(data.empty() ? "/arc-wasm" : data));
  std::vector<std::string> in_mnt;  // what /mnt shows
  const std::string apps = OpfsDir("ARC_APPS_DIR");
  if (!apps.empty()) {
    vfs->Mount("/mnt/apps/", new posix_translation::OpfsHandler(apps, /*read_only=*/true));
    in_mnt.push_back("apps");
    // The OBB files of the app the page runs ($ARC_APP_OBB=PACKAGE: an
    // .xapk's Android/obb/PACKAGE/*.obb, topic apks-split), kept in the
    // library (APPS_DIR/PACKAGE/obb): read-only, nothing copied, at the
    // app's OBB directory on its external storage.
    const char* obb = getenv("ARC_APP_OBB");
    if (obb && *obb && !strchr(obb, '/') && strcmp(obb, ".") && strcmp(obb, "..")) {
      const char* ext = getenv("EXTERNAL_STORAGE");
      std::string at = std::string(ext && *ext ? ext : "/data/media/0") + "/Android/obb/" + obb + "/";
      vfs->Mount(at, new posix_translation::OpfsHandler(apps + "/" + obb + "/obb",
                                                         /*read_only=*/true));
    }
  }
  // Tests only (InstallTest plays the shell): the whole storage, writable.
  const std::string root = OpfsDir("ARC_OPFS_ROOT_MOUNT");
  if (!root.empty()) {
    vfs->Mount(root + "/", new posix_translation::OpfsHandler("/"));
    if (root.compare(0, 5, "/mnt/") == 0 && root.find('/', 5) == std::string::npos)
      in_mnt.push_back(root.substr(5));
  }
  if (!in_mnt.empty())
    vfs->Mount("/mnt/", new posix_translation::MountPointDirHandler(in_mnt));
}

}  // namespace

extern "C" int arc_dalvik_boot_kernel() {
  posix_translation::VirtualFileSystem* vfs = arc_kernel::Boot();
  vfs->Mount("/dev/binder", new posix_translation::DevBinderHandler);
  vfs->Mount("/system/", new posix_translation::ReadonlyFileHandler(
      "/arc/system.img", 64 * 1024,
      new posix_translation::HostFileHandler()));
  const bool opfs = arc_opfs::Start();
  if (opfs)
    MountOpfs(vfs);
  return opfs;
}
