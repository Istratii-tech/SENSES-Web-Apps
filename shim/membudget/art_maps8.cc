// ART 8.1's own memory by kind, for the budget's accounting (mem_budget.h,
// topic mem-budget): ART names every MemMap it makes, and MemMap::DumpMaps
// lists them ("[MemMap: 0xBEGIN-0xEND prot=0x3 NAME]"). Compiled with ART's
// headers (ART8_FLAGS; ART10_FLAGS for ART 10, topic fw10-system: there
// MemMap is libartbase's). Part of the arc-wasm shim layer.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sstream>
#include <string>

#if __has_include("mem_map.h")
#include "mem_map.h"
#else
#include "base/mem_map.h"
#endif

// posix_translation (readonly_file.cc): the /system image's own pages.
extern "C" int arc_readonly_image_has(const void* addr, size_t length) __attribute__((weak));

namespace {

bool Has(const std::string& s, const char* part) {
  return s.find(part) != std::string::npos;
}

}  // namespace

extern "C" void arc_art_mem_maps(int64_t* dex, int64_t* spaces, int64_t* los, int64_t* other) {
  *dex = *spaces = *los = *other = 0;
  std::ostringstream os;
  art::MemMap::DumpMaps(os, false);
  // ARC_MEM_MAPS=1: the list on stderr, once (what the kinds are made of).
  static bool dumped = false;
  if (!dumped && getenv("ARC_MEM_MAPS")) {
    dumped = true;
    fprintf(stderr, "membudget: ART's maps\n%s", os.str().c_str());
  }
  std::istringstream in(os.str());
  std::string line;
  while (std::getline(in, line)) {
    unsigned long begin = 0, end = 0;
    unsigned prot = 0;
    int name_at = 0;
    if (sscanf(line.c_str(), "[MemMap: %lx-%lx prot=%x %n", &begin, &end, &prot, &name_at) < 3 ||
        name_at == 0 || end <= begin) {
      continue;
    }
    std::string name = line.substr(name_at);
    if (!name.empty() && name.back() == ']')
      name.pop_back();
    const int64_t size = static_cast<int64_t>(end - begin);
    // A stored dex mapped from a jar of the /system image (topic boot-dex):
    // the image's pages, counted as the image (ARC_MEM_IMAGE).
    if (arc_readonly_image_has &&
        arc_readonly_image_has(reinterpret_cast<const void*>(begin), static_cast<size_t>(size))) {
      continue;
    }
    // Dex files: entries extracted from jars and APKs ("classes.dex
    // extracted in memory from /system/framework/core-oj.jar"), or mapped
    // from a file (an uncompressed entry or a .dex).
    if (Has(name, "extracted in memory") || Has(name, ".dex") || Has(name, ".jar") ||
        Has(name, ".apk")) {
      *dex += size;
    } else if (Has(name, "large object space allocation")) {
      *los += size;
    } else if (Has(name, "space") && !Has(name, "bitmap") && !Has(name, "large object")) {
      // "main space", "non moving space", "zygote space": reserved whole,
      // used as the heap grows.
      *spaces += size;
    } else {
      // Mark bitmaps, card table, mark and allocation stacks, LinearAlloc
      // (class metadata), the large object free list's info map...
      *other += size;
    }
  }
}
