// dexpreopt: Dalvik's dexopt for the /system image, run at build time under
// Node (build/dexopt/dexpreopt.js). Android does the same when it builds a
// device image ("dexpreopt"): each jar is verified and optimized ahead of
// time into NAME.odex next to NAME.jar, and dvmJarFileOpen() prefers that
// file over classes.dex. On device dexopt is a separate process for every
// file; here it is a separate Node process for every file, so the VM state
// of dexopt (gDvm.optimizing) never meets a running app.
//
//   ARC_SYSTEM_IMAGE=IMAGE node dexpreopt.js JAR OUT BOOTCLASSPATH
//
// IMAGE is a /system image (tools/make_system_image.py) with JAR and the
// jars of BOOTCLASSPATH at the paths they have at run time; BOOTCLASSPATH
// must be the one dalvikvm uses, because the .odex records it (with the
// SHA-1 of every DEX in it) and is ignored at run time if it differs. JAR
// in BOOTCLASSPATH is optimized as a bootstrap jar, against the entries
// before it. OUT is a host path; the .odex is written there.
//
// The steps are those of extractAndProcessZip() in dalvik/dexopt/OptMain.cpp
// (the body of AOSP's dexopt command); the output file is in memory
// (memory_file.cc). Part of topic "dexopt" (docs/topics/dexopt.md).

#include <emscripten/emscripten.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "Dalvik.h"
#include "libdex/OptInvocation.h"

extern "C" int arc_dalvik_boot_kernel();
extern "C" int arc_dexopt_open_memory_file(int fd);
extern "C" const void* arc_dexopt_memory_file_data(int fd, size_t* size);

namespace {

const int kOutputFd = 1000;  // The VFS hands out descriptors from 0 up.

std::vector<std::string> SplitPath(const std::string& path) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= path.size()) {
    size_t end = path.find(':', start);
    if (end == std::string::npos)
      end = path.size();
    if (end > start)
      out.push_back(path.substr(start, end - start));
    start = end + 1;
  }
  return out;
}

double NowMs() { return dvmGetRelativeTimeUsec() / 1000.0; }

}  // namespace

int main(int argc, char** argv) {
  if (argc != 4) {
    fprintf(stderr, "usage: ARC_SYSTEM_IMAGE=IMAGE dexpreopt.js JAR OUT BOOTCLASSPATH\n");
    return 2;
  }
  const char* jar = argv[1];
  const char* out = argv[2];
  const double start = NowMs();

  arc_dalvik_boot_kernel();
  if (arc_dexopt_open_memory_file(kOutputFd) != 0) {
    fprintf(stderr, "dexpreopt: cannot register the output file\n");
    return 1;
  }

  // As processZipFile() in OptMain.cpp, but matching whole entries: a jar
  // in the boot class path is optimized against the entries before it.
  std::string bcp;
  bool isBootstrap = false;
  for (const std::string& entry : SplitPath(argv[3])) {
    if (entry == jar) {
      isBootstrap = true;
      break;
    }
    bcp += (bcp.empty() ? "" : ":") + entry;
  }

  int zipFd = open(jar, O_RDONLY);
  if (zipFd < 0) {
    fprintf(stderr, "dexpreopt: cannot open %s: %s\n", jar, strerror(errno));
    return 1;
  }

  // extractAndProcessZip(): an empty opt header, then classes.dex.
  ZipArchive zippy;
  memset(&zippy, 0, sizeof(zippy));
  ZipEntry zipEntry;
  size_t uncompLen;
  long modWhen, crc32;
  off_t dexOffset;
  if (dexOptCreateEmptyHeader(kOutputFd) != 0 ||
      (dexOffset = lseek(kOutputFd, 0, SEEK_CUR)) <= 0 ||
      dexZipPrepArchive(zipFd, jar, &zippy) != 0 ||
      (zipEntry = dexZipFindEntry(&zippy, "classes.dex")) == NULL ||
      dexZipGetEntryInfo(&zippy, zipEntry, NULL, &uncompLen, NULL, NULL,
                         &modWhen, &crc32) != 0 ||
      dexZipExtractEntryToFile(&zippy, zipEntry, kOutputFd) != 0) {
    fprintf(stderr, "dexpreopt: cannot extract classes.dex from %s\n", jar);
    return 1;
  }

  // dexopt's flags for a device image: "v=a,o=v,m=y" -- verify everything,
  // optimize what verified, register maps (live-precise: dexopt.patch makes
  // that the default on wasm, as at run time).
  if (dvmPrepForDexOpt(bcp.c_str(), OPTIMIZE_MODE_VERIFIED, VERIFY_MODE_ALL,
                       DEXOPT_GEN_REGISTER_MAPS) != 0) {
    fprintf(stderr, "dexpreopt: VM init failed\n");
    return 1;
  }
  if (!dvmContinueOptimization(kOutputFd, dexOffset, uncompLen, jar, modWhen,
                               crc32, isBootstrap)) {
    fprintf(stderr, "dexpreopt: optimization of %s failed\n", jar);
    return 1;
  }

  size_t size = 0;
  const void* data = arc_dexopt_memory_file_data(kOutputFd, &size);
  int written = MAIN_THREAD_EM_ASM_INT({
    try {
      require('fs').writeFileSync(UTF8ToString($0), HEAPU8.slice($1, $1 + $2));
      return 1;
    } catch (e) {
      err('dexpreopt: ' + e);
      return 0;
    }
  }, out, data, size);
  if (!written)
    return 1;
  fprintf(stderr, "dexpreopt: %s -> %s (%zu KB, %s) in %.0f ms\n", jar, out,
          size / 1024, isBootstrap ? "bootstrap" : "not bootstrap",
          NowMs() - start);
  // As dexopt: no VM shutdown, the process just ends.
  exit(0);
}
