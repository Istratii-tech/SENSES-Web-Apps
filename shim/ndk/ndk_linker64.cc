// The ELF64 loader and dynamic linker for arm64-v8a guest libraries (topic
// "ndk-arm64"), the counterpart of ndk_linker.cc: segments are copied into
// anonymous memory (guest address == wasm address, below 4 GB), RELA
// relocations applied (also Android's packed APS2 and RELR forms), imports
// bound to other 64-bit guest libraries or to arm64 host stubs, ELF TLS
// placed in each thread's static TLS area (TPREL64 and TLSDESC), then
// DT_INIT and DT_INIT_ARRAY run on the AArch64 CPU. Also libdl for 64-bit
// guests. Separate from the 32-bit linker: a library of one ABI never binds
// to one of the other. Part of the arc-wasm shim layer.

#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "../multiproc/include/arc_lproc.h"
#include "ndk_bridge.h"
#include "ndk_elf_relocs.h"

extern "C" int __android_log_print(int prio, const char* tag, const char* fmt, ...);
extern "C" void ndk64_set_symbolizer(Ndk64Symbolizer fn);
#define LOGI(...) __android_log_print(4, "ndk", __VA_ARGS__)
#define LOGW(...) __android_log_print(5, "ndk", __VA_ARGS__)
#define LOGE(...) __android_log_print(6, "ndk", __VA_ARGS__)

#ifndef EM_AARCH64
#define EM_AARCH64 183
#endif
#ifndef R_AARCH64_TLSDESC
#define R_AARCH64_TLSDESC 1031
#endif
#ifndef R_AARCH64_IRELATIVE
#define R_AARCH64_IRELATIVE 1032
#endif

namespace ndk {

struct Library64 {
  std::string path;
  std::string name;
  std::string soname;
  uint32_t base = 0, size = 0;
  uint32_t bias = 0;
  const Elf64_Dyn* dynamic = nullptr;
  const Elf64_Sym* symtab = nullptr;
  const char* strtab = nullptr;
  uint32_t nsyms = 0;
  uint32_t nbucket = 0;
  const uint32_t* bucket = nullptr;
  const uint32_t* chain = nullptr;
  uint32_t gnu_nbucket = 0, gnu_symoffset = 0, gnu_bloom_size = 0, gnu_shift = 0;
  const uint64_t* gnu_bloom = nullptr;
  const uint32_t* gnu_bucket = nullptr;
  const uint32_t* gnu_chain = nullptr;
  uint32_t init = 0, fini = 0;
  uint32_t init_array = 0, init_array_count = 0;
  uint32_t fini_array = 0, fini_array_count = 0;
  uint32_t phdr = 0, phnum = 0;
  // ELF TLS: the block's place in every thread's static TLS area (an offset
  // from TPIDR_EL0), or 0 without PT_TLS.
  uint32_t tls_offset = 0, tls_size = 0;
  int tls_module = 0;
  std::vector<std::string> needed;
  std::vector<Library64*> deps;
  std::vector<std::string> unresolved;
  std::vector<const char*> unresolved_c;
  std::vector<std::string> missing_needed;
  ElfVersions versions;
  int refcount = 0;
  bool constructed = false;
  bool is_host = false;  // The light process whose copy this is (see ndk_linker.cc).
  int ns = 0;
};

namespace {

std::recursive_mutex g_lock;
std::vector<Library64*> g_libs;
std::map<std::string, Library64*> g_host_libs;
std::vector<std::string> g_search_dirs = {"/vendor/lib64", "/system/lib64"};
std::map<uint32_t, uint32_t> g_exec_regions;
// /proc/self/maps lines of the 64-bit libraries: rebuilt under g_lock when a
// library loads, read under g_maps_lock only (arc_proc_maps runs under the
// kernel's mutex, which g_lock's holders may wait for).
std::mutex g_maps_lock;
std::string* g_maps = new std::string;
thread_local std::string g_dlerror_buf;
thread_local const char* g_dlerror;

void SetError(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void SetError(const char* fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  g_dlerror_buf = buf;
  g_dlerror = g_dlerror_buf.c_str();
}

const char* BaseName(const char* path) {
  const char* s = strrchr(path, '/');
  return s ? s + 1 : path;
}

uint32_t ElfHash(const char* name) {
  uint32_t h = 0, g;
  for (const unsigned char* p = (const unsigned char*)name; *p; p++) {
    h = (h << 4) + *p;
    g = h & 0xf0000000;
    h ^= g;
    h ^= g >> 24;
  }
  return h;
}

uint32_t GnuHash(const char* name) {
  uint32_t h = 5381;
  for (const unsigned char* p = (const unsigned char*)name; *p; p++) h = h * 33 + *p;
  return h;
}

bool Exported(const Elf64_Sym* s) {
  int bind = ELF64_ST_BIND(s->st_info);
  return s->st_shndx != SHN_UNDEF && (bind == STB_GLOBAL || bind == STB_WEAK || bind == STB_GNU_UNIQUE) &&
         ELF64_ST_VISIBILITY(s->st_other) != STV_HIDDEN && ELF64_ST_VISIBILITY(s->st_other) != STV_INTERNAL;
}

// An exported definition of |name| in |lib|, or nullptr: of version
// |version| when given and |lib| has it, else the default version.
const Elf64_Sym* FindExport(const Library64* lib, const char* name, const char* version = nullptr) {
  if (lib->is_host) return nullptr;
  const uint16_t need = VerdefIndex(lib->versions, lib->strtab, version);
  auto match = [&](uint32_t n) {
    const Elf64_Sym* s = &lib->symtab[n];
    return strcmp(lib->strtab + s->st_name, name) == 0 && Exported(s) && VersionMatches(lib->versions, n, need);
  };
  if (lib->gnu_bucket) {
    uint32_t h = GnuHash(name);
    uint64_t word = lib->gnu_bloom[(h / 64) % lib->gnu_bloom_size];
    uint64_t mask = (1ull << (h % 64)) | (1ull << ((h >> lib->gnu_shift) % 64));
    if ((word & mask) != mask) return nullptr;
    uint32_t n = lib->gnu_bucket[h % lib->gnu_nbucket];
    if (n == 0) return nullptr;
    for (;; n++) {
      uint32_t ch = lib->gnu_chain[n - lib->gnu_symoffset];
      if ((ch | 1) == (h | 1) && match(n)) return &lib->symtab[n];
      if (ch & 1) return nullptr;
    }
  }
  if (lib->bucket) {
    for (uint32_t n = lib->bucket[ElfHash(name) % lib->nbucket]; n != 0; n = lib->chain[n])
      if (match(n)) return &lib->symtab[n];
  }
  return nullptr;
}

uint64_t SymAddr(const Library64* lib, const Elf64_Sym* s) {
  if (s->st_shndx == SHN_ABS) return s->st_value;
  if (ELF64_ST_TYPE(s->st_info) == STT_TLS) return s->st_value;   // an offset in the TLS block
  return lib->bias + s->st_value;
}

Library64* HostLib(const char* name) {
  auto it = g_host_libs.find(name);
  if (it != g_host_libs.end()) return it->second;
  Library64* lib = new Library64;
  lib->name = lib->path = lib->soname = name;
  lib->is_host = true;
  g_host_libs[name] = lib;
  return lib;
}

void MissingSymbol(NdkCpu64State* s, void* user) {
  LOGE("guest called the unresolved symbol %s (lr=0x%llx)", (const char*)user, (unsigned long long)s->x[30]);
  fprintf(stderr, "ndk: arm64 guest called the unresolved symbol %s (lr=0x%llx)\n", (const char*)user,
          (unsigned long long)s->x[30]);
  char why[160];
  snprintf(why, sizeof why, "a call to %s, which this system lacks", (const char*)user);
  LeaveGuest(why);
  abort();
}

// --- TLS --------------------------------------------------------------------------------------
// Every thread that runs arm64 code has a static TLS area right after its TLS
// slots (ndk_core.cc, CurrentState64: TPIDR_EL0 points at the slots); each
// library with PT_TLS gets a block there at the same offset in all threads.
// Blocks are initialized from the library's image in the threads that exist
// at load time and in every thread created later.
struct TlsImage {
  uint32_t offset, image, filesz, memsz;
};
std::mutex g_tls_lock;
std::vector<TlsImage> g_tls_images;
uint32_t g_tls_next = 0;   // set from the first area's layout

}  // namespace

// ndk_core.cc reserves the area; these say how big and register threads.
constexpr uint32_t kTls64SlotsBytes = 64 * 8;
constexpr uint32_t kTls64AreaBytes = 16 * 1024;   // ndk_core.cc: GuestThread64
std::vector<uint32_t>* g_tls_threads;   // TPIDR_EL0 of live threads with an area

// A new thread's area (called by CurrentState64 with |tp| = its slots).
void InitThreadTls64(uint32_t tp) {
  std::lock_guard<std::mutex> l(g_tls_lock);
  if (!g_tls_threads) g_tls_threads = new std::vector<uint32_t>;
  g_tls_threads->push_back(tp);
  for (const TlsImage& t : g_tls_images) {
    memcpy(Ptr<void>(tp + t.offset), Ptr<void>(t.image), t.filesz);
    memset(Ptr<void>(tp + t.offset + t.filesz), 0, t.memsz - t.filesz);
  }
}

void ForgetThreadTls64(uint32_t tp) {
  std::lock_guard<std::mutex> l(g_tls_lock);
  if (!g_tls_threads) return;
  auto it = std::find(g_tls_threads->begin(), g_tls_threads->end(), tp);
  if (it != g_tls_threads->end()) g_tls_threads->erase(it);
}

namespace {

bool AllocateTls(Library64* lib, const Elf64_Phdr& ph) {
  std::lock_guard<std::mutex> l(g_tls_lock);
  if (g_tls_next == 0) g_tls_next = kTls64SlotsBytes;
  uint32_t align = ph.p_align > 16 ? static_cast<uint32_t>(ph.p_align) : 16;
  uint32_t off = (g_tls_next + align - 1) & ~(align - 1);
  if (off + ph.p_memsz > kTls64AreaBytes) {
    SetError("dlopen failed: \"%s\": no room for %llu bytes of TLS", lib->path.c_str(),
             (unsigned long long)ph.p_memsz);
    return false;
  }
  g_tls_next = off + static_cast<uint32_t>(ph.p_memsz);
  lib->tls_offset = off;
  lib->tls_size = static_cast<uint32_t>(ph.p_memsz);
  lib->tls_module = static_cast<int>(g_tls_images.size()) + 1;
  TlsImage t = {off, lib->bias + static_cast<uint32_t>(ph.p_vaddr), static_cast<uint32_t>(ph.p_filesz),
                static_cast<uint32_t>(ph.p_memsz)};
  g_tls_images.push_back(t);
  if (g_tls_threads) {
    for (uint32_t tp : *g_tls_threads) {
      memcpy(Ptr<void>(tp + t.offset), Ptr<void>(t.image), t.filesz);
      memset(Ptr<void>(tp + t.offset + t.filesz), 0, t.memsz - t.filesz);
    }
  }
  return true;
}

// The TLSDESC resolver: x0 = the descriptor, its second word the TP-relative
// offset (static TLS: a constant for every thread). Only x0 changes.
void TlsDescResolver(NdkCpu64State* s, void*) { s->x[0] = Load64(static_cast<uint32_t>(s->x[0]) + 8); }
uint32_t TlsDescStub() {
  static uint32_t stub = MakeStub64(TlsDescResolver, nullptr, "<tlsdesc resolver>");
  return stub;
}

// --- relocation -----------------------------------------------------------------------------------
struct Resolved {
  uint64_t value;
  const Library64* from;   // the defining library (TLS: its block)
  bool found;
};

// As ndk_linker.cc's Lookup: the local group breadth first; a weak import is
// not looked up in unrelated libraries.
Resolved Lookup(Library64* lib, const char* name, const char* version, bool weak) {
  if (const Elf64_Sym* s = FindExport(lib, name, version)) return {SymAddr(lib, s), lib, true};
  for (Library64* dep : lib->deps)
    if (const Elf64_Sym* s = FindExport(dep, name, version)) return {SymAddr(dep, s), dep, true};
  if (uint32_t a = ResolveHostSymbol64(name)) return {a, nullptr, true};
  std::vector<Library64*> group(lib->deps.begin(), lib->deps.end());
  for (size_t i = 0; i < group.size(); i++) {
    for (Library64* dep : group[i]->deps) {
      if (dep == lib || std::find(group.begin(), group.end(), dep) != group.end()) continue;
      group.push_back(dep);
      if (const Elf64_Sym* s = FindExport(dep, name, version)) return {SymAddr(dep, s), dep, true};
    }
  }
  if (weak) return {0, nullptr, false};
  for (Library64* other : g_libs) {
    if (other == lib || other->ns != lib->ns) continue;
    if (const Elf64_Sym* s = FindExport(other, name, version)) return {SymAddr(other, s), other, true};
  }
  return {0, nullptr, false};
}

bool RelocateOne(Library64* lib, uint32_t type, uint32_t symi, uint64_t offset, int64_t addend) {
  uint32_t where = lib->bias + static_cast<uint32_t>(offset);
  if (type == R_AARCH64_NONE) return true;
  if (type == R_AARCH64_RELATIVE) {
    Store64(where, lib->bias + addend);
    return true;
  }
  if (type == R_AARCH64_IRELATIVE) {
    uint64_t resolver = lib->bias + addend;
    uint64_t hwcap = 0;
    Store64(where, CallGuest64(static_cast<uint32_t>(resolver), &hwcap, 1));
    return true;
  }
  Resolved r = {0, nullptr, true};
  const Elf64_Sym* s = symi ? &lib->symtab[symi] : nullptr;
  const char* name = s ? lib->strtab + s->st_name : "";
  bool tls = type == R_AARCH64_TLS_TPREL64 || type == R_AARCH64_TLSDESC || type == R_AARCH64_TLS_DTPREL64 ||
             type == R_AARCH64_TLS_DTPMOD64;
  if (s) {
    if (tls && s->st_shndx != SHN_UNDEF) {
      r = {s->st_value, lib, true};   // a local or own TLS symbol
    } else if (ELF64_ST_BIND(s->st_info) == STB_LOCAL) {
      r = {SymAddr(lib, s), lib, true};
    } else {
      r = Lookup(lib, name, RequiredVersion(lib->versions, lib->strtab, symi),
                 ELF64_ST_BIND(s->st_info) == STB_WEAK);
    }
    if (!r.found) {
      if (ELF64_ST_BIND(s->st_info) == STB_WEAK) {
        r = {0, nullptr, true};
      } else {
        bool seen = false;
        for (const std::string& u : lib->unresolved) seen |= u == name;
        if (!seen) {
          lib->unresolved.push_back(name);
          LOGW("%s: unresolved symbol %s", lib->name.c_str(), name);
        }
        r = {MakeStub64(MissingSymbol, strdup(name), strdup(name)), nullptr, true};
      }
    }
  } else if (tls) {
    r = {0, lib, true};   // symbol 0: the library's own block
  }
  switch (type) {
    case R_AARCH64_ABS64:
    case R_AARCH64_GLOB_DAT:
    case R_AARCH64_JUMP_SLOT:
      Store64(where, r.value + addend);
      return true;
    case R_AARCH64_ABS32:
      Store32(where, static_cast<uint32_t>(r.value + addend));
      return true;
    case R_AARCH64_PREL64:
      Store64(where, r.value + addend - where);
      return true;
    case R_AARCH64_TLS_TPREL64: {
      uint32_t base = r.from ? r.from->tls_offset : 0;
      Store64(where, base + r.value + addend);
      return true;
    }
    case R_AARCH64_TLSDESC: {
      uint32_t base = r.from ? r.from->tls_offset : 0;
      Store64(where, TlsDescStub());
      Store64(where + 8, base + r.value + addend);
      return true;
    }
    case R_AARCH64_TLS_DTPMOD64:
      Store64(where, r.from ? r.from->tls_module : 0);
      return true;
    case R_AARCH64_TLS_DTPREL64:
      Store64(where, r.value + addend);
      return true;
  }
  SetError("%s: unsupported relocation type %u", lib->name.c_str(), type);
  return false;
}

bool RelocateRela(Library64* lib, const Elf64_Rela* rel, size_t count) {
  for (size_t i = 0; i < count; i++, rel++) {
    if (!RelocateOne(lib, ELF64_R_TYPE(rel->r_info), ELF64_R_SYM(rel->r_info), rel->r_offset, rel->r_addend))
      return false;
  }
  return true;
}

// RELR: a list of addresses and bitmaps of relative relocations (no addend
// in the entry: the word in place is the addend).
void RelocateRelr(Library64* lib, const uint64_t* relr, size_t count) {
  ForEachRelr<uint64_t>(relr, count, [lib](uint64_t offset) {
    uint32_t where = lib->bias + static_cast<uint32_t>(offset);
    Store64(where, Load64(where) + lib->bias);
  });
}

// Android's APS2 packed relocations (ndk_elf_relocs.h).
bool RelocatePacked(Library64* lib, const uint8_t* data, size_t size, bool rela) {
  const char* error;
  bool ok = ForEachPackedReloc<uint64_t>(data, size, rela,
      [lib](uint64_t offset, uint64_t info, int64_t addend) {
        return RelocateOne(lib, ELF64_R_TYPE(info), ELF64_R_SYM(info), offset, addend);
      }, &error);
  if (!ok && error) SetError("%s: %s", lib->name.c_str(), error);
  return ok;
}

bool ReadFully(int fd, void* buf, size_t size, off_t offset) {
  char* p = static_cast<char*>(buf);
  while (size > 0) {
    ssize_t n = pread(fd, p, size, offset);
    if (n <= 0) return false;
    p += n;
    size -= n;
    offset += n;
  }
  return true;
}

Library64* Find(const char* path) {
  bool bare = strchr(path, '/') == nullptr;
  const int ns = arc_lproc_native_ns();
  for (Library64* lib : g_libs)
    if (lib->ns == ns && (bare ? (lib->name == path || lib->soname == path) : lib->path == path))
      return lib;
  return nullptr;
}

Library64* Load(const char* path, const std::string& near = std::string());

void UpdateMapsLocked() {
  std::string text;
  char line[600];
  for (const Library64* lib : g_libs) {
    if (lib->is_host || !lib->phdr) continue;
    const Elf64_Phdr* ph = Ptr<const Elf64_Phdr>(lib->phdr);
    const unsigned inode = static_cast<unsigned>(std::hash<std::string>()(lib->path) & 0xfffff);
    for (uint32_t i = 0; i < lib->phnum; ++i) {
      if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0) continue;
      const uint32_t start = (lib->bias + static_cast<uint32_t>(ph[i].p_vaddr)) & ~0xfffu;
      const uint32_t end = (lib->bias + static_cast<uint32_t>(ph[i].p_vaddr + ph[i].p_memsz) + 0xfff) & ~0xfffu;
      snprintf(line, sizeof(line), "%08x-%08x %c%c%cp %08llx fd:01 %-10u %s\n", start, end,
               (ph[i].p_flags & PF_R) ? 'r' : '-', (ph[i].p_flags & PF_W) ? 'w' : '-',
               (ph[i].p_flags & PF_X) ? 'x' : '-', (unsigned long long)(ph[i].p_offset & ~0xfffull), inode,
               lib->path.c_str());
      text += line;
    }
  }
  std::lock_guard<std::mutex> l(g_maps_lock);
  g_maps->swap(text);
}

Library64* LoadFile(const char* path) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    SetError("dlopen failed: \"%s\": %s", path, strerror(errno));
    return nullptr;
  }
  Elf64_Ehdr eh;
  if (!ReadFully(fd, &eh, sizeof eh, 0) || memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 ||
      eh.e_ident[EI_CLASS] != ELFCLASS64 || eh.e_ident[EI_DATA] != ELFDATA2LSB) {
    close(fd);
    SetError("dlopen failed: \"%s\" is not a 64-bit little-endian ELF file", path);
    return nullptr;
  }
  if (eh.e_machine != EM_AARCH64 || eh.e_type != ET_DYN) {
    close(fd);
    SetError("dlopen failed: \"%s\" is not an AArch64 shared object (machine %d, type %d)", path, eh.e_machine,
             eh.e_type);
    return nullptr;
  }
  std::vector<Elf64_Phdr> ph(eh.e_phnum);
  if (eh.e_phentsize != sizeof(Elf64_Phdr) ||
      !ReadFully(fd, ph.data(), ph.size() * sizeof(Elf64_Phdr), static_cast<off_t>(eh.e_phoff))) {
    close(fd);
    SetError("dlopen failed: \"%s\": bad program headers", path);
    return nullptr;
  }
  uint64_t lo = UINT64_MAX, hi = 0;
  uint64_t max_align = 4096;
  for (const Elf64_Phdr& p : ph) {
    if (p.p_type != PT_LOAD) continue;
    lo = std::min<uint64_t>(lo, p.p_vaddr & ~4095ull);
    hi = std::max<uint64_t>(hi, (p.p_vaddr + p.p_memsz + 4095) & ~4095ull);
    if (p.p_align > max_align && p.p_align <= 65536) max_align = p.p_align;
  }
  if (lo >= hi || hi - lo > 0x40000000ull) {
    close(fd);
    SetError("dlopen failed: \"%s\" has no (or too large) loadable segments", path);
    return nullptr;
  }
  // Segments aligned to more than a page (16 KB/64 KB page builds) keep
  // their alignment: map with slack and align the start.
  uint32_t span = static_cast<uint32_t>(hi - lo);
  void* raw = mmap(nullptr, span + (max_align > 4096 ? max_align : 0), PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (raw == MAP_FAILED) {
    close(fd);
    SetError("dlopen failed: \"%s\": cannot map %u bytes", path, span);
    return nullptr;
  }
  uint32_t map = (Addr(raw) + static_cast<uint32_t>(max_align) - 1) & ~(static_cast<uint32_t>(max_align) - 1);
  Library64* lib = new Library64;
  lib->path = path;
  lib->name = BaseName(path);
  lib->base = map;
  lib->size = span;
  lib->bias = map - static_cast<uint32_t>(lo);
  memset(Ptr<void>(map), 0, span);
  bool ok = true;
  const Elf64_Phdr* tls = nullptr;
  for (const Elf64_Phdr& p : ph) {
    if (p.p_type == PT_LOAD && p.p_filesz > 0)
      ok &= ReadFully(fd, Ptr<void>(lib->bias + static_cast<uint32_t>(p.p_vaddr)), p.p_filesz,
                      static_cast<off_t>(p.p_offset));
    else if (p.p_type == PT_DYNAMIC)
      lib->dynamic = Ptr<Elf64_Dyn>(lib->bias + static_cast<uint32_t>(p.p_vaddr));
    else if (p.p_type == PT_TLS && p.p_memsz)
      tls = &p;
  }
  for (const Elf64_Phdr& p : ph)
    if (p.p_type == PT_PHDR) lib->phdr = lib->bias + static_cast<uint32_t>(p.p_vaddr);
  for (const Elf64_Phdr& p : ph) {
    if (!lib->phdr && p.p_type == PT_LOAD && eh.e_phoff >= p.p_offset &&
        eh.e_phoff + ph.size() * sizeof(Elf64_Phdr) <= p.p_offset + p.p_filesz)
      lib->phdr = lib->bias + static_cast<uint32_t>(p.p_vaddr + (eh.e_phoff - p.p_offset));
  }
  lib->phnum = lib->phdr ? eh.e_phnum : 0;
  close(fd);
  if (!ok || !lib->dynamic) {
    SetError("dlopen failed: \"%s\": %s", path, ok ? "no dynamic section" : "read error");
    delete lib;
    return nullptr;
  }
  if (tls && !AllocateTls(lib, *tls)) {
    delete lib;
    return nullptr;
  }

  const Elf64_Rela *rela = nullptr, *jmprel = nullptr;
  uint64_t relasz = 0, jmprelsz = 0, relrsz = 0, packedsz = 0;
  const uint64_t* relr = nullptr;
  const uint8_t* packed = nullptr;
  bool packed_rela = true;
  std::vector<uint64_t> needed_off;
  uint64_t soname_off = UINT64_MAX;
  for (const Elf64_Dyn* d = lib->dynamic; d->d_tag != DT_NULL; d++) {
    uint64_t v = d->d_un.d_val;
    uint32_t a = lib->bias + static_cast<uint32_t>(v);
    switch (d->d_tag) {
      case DT_NEEDED: needed_off.push_back(v); break;
      case DT_SONAME: soname_off = v; break;
      case DT_STRTAB: lib->strtab = Ptr<const char>(a); break;
      case DT_SYMTAB: lib->symtab = Ptr<const Elf64_Sym>(a); break;
      case DT_HASH:
        lib->nbucket = Load32(a);
        lib->nsyms = Load32(a + 4);
        lib->bucket = Ptr<const uint32_t>(a + 8);
        lib->chain = lib->bucket + lib->nbucket;
        break;
      case DT_GNU_HASH:
        lib->gnu_nbucket = Load32(a);
        lib->gnu_symoffset = Load32(a + 4);
        lib->gnu_bloom_size = Load32(a + 8);
        lib->gnu_shift = Load32(a + 12);
        lib->gnu_bloom = Ptr<const uint64_t>(a + 16);
        lib->gnu_bucket = reinterpret_cast<const uint32_t*>(lib->gnu_bloom + lib->gnu_bloom_size);
        lib->gnu_chain = lib->gnu_bucket + lib->gnu_nbucket;
        break;
      case DT_RELA: rela = Ptr<const Elf64_Rela>(a); break;
      case DT_RELASZ: relasz = v; break;
      case DT_JMPREL: jmprel = Ptr<const Elf64_Rela>(a); break;
      case DT_PLTRELSZ: jmprelsz = v; break;
      case DT_PLTREL:
        if (v != DT_RELA) {
          SetError("dlopen failed: \"%s\": DT_PLTREL is not DT_RELA", path);
          ok = false;
        }
        break;
      case DT_REL:
        SetError("dlopen failed: \"%s\": REL relocations on AArch64", path);
        ok = false;
        break;
      case DT_RELR: case DT_ANDROID_RELR: relr = Ptr<const uint64_t>(a); break;
      case DT_RELRSZ: case DT_ANDROID_RELRSZ: relrsz = v; break;
      case DT_RELRENT: case DT_ANDROID_RELRENT:
        if (v != 8) {
          SetError("dlopen failed: \"%s\": RELR entries of %llu bytes", path, (unsigned long long)v);
          ok = false;
        }
        break;
      case DT_ANDROID_RELA: packed = Ptr<const uint8_t>(a); packed_rela = true; break;
      case DT_ANDROID_REL: packed = Ptr<const uint8_t>(a); packed_rela = false; break;
      case DT_ANDROID_RELASZ: case DT_ANDROID_RELSZ: packedsz = v; break;
      case DT_VERSYM: lib->versions.versym = Ptr<const uint16_t>(a); break;
      case DT_VERDEF: lib->versions.verdef = a; break;
      case DT_VERDEFNUM: lib->versions.verdefnum = static_cast<uint32_t>(v); break;
      case DT_VERNEED: lib->versions.verneed = a; break;
      case DT_VERNEEDNUM: lib->versions.verneednum = static_cast<uint32_t>(v); break;
      case DT_INIT: lib->init = a; break;
      case DT_FINI: lib->fini = a; break;
      case DT_INIT_ARRAY: lib->init_array = a; break;
      case DT_INIT_ARRAYSZ: lib->init_array_count = static_cast<uint32_t>(v / 8); break;
      case DT_FINI_ARRAY: lib->fini_array = a; break;
      case DT_FINI_ARRAYSZ: lib->fini_array_count = static_cast<uint32_t>(v / 8); break;
      default: break;
    }
  }
  if (ok && (!lib->strtab || !lib->symtab || (!lib->bucket && !lib->gnu_bucket))) {
    SetError("dlopen failed: \"%s\": no symbol table or hash", path);
    ok = false;
  }
  if (!ok) {
    delete lib;
    return nullptr;
  }
  if (soname_off != UINT64_MAX) lib->soname = lib->strtab + soname_off;
  for (uint64_t off : needed_off) lib->needed.push_back(lib->strtab + off);

  lib->ns = arc_lproc_native_ns();
  g_libs.push_back(lib);
  for (const std::string& n : lib->needed) {
    if (IsHostLibrary(n.c_str())) continue;
    Library64* dep = Load(n.c_str(), std::string(path, BaseName(path) - path));
    if (!dep) {
      lib->missing_needed.push_back(n);
      std::string err = g_dlerror ? g_dlerror : "?";
      LOGW("%s: needed library %s: %s", lib->name.c_str(), n.c_str(), err.c_str());
      continue;   // lenient: its symbols resolve elsewhere or become unresolved stubs
    }
    lib->deps.push_back(dep);
  }
  // Relative relocations first (packed RELR), then the rest.
  if (relr) RelocateRelr(lib, relr, relrsz / 8);
  if ((packed && !RelocatePacked(lib, packed, packedsz, packed_rela)) ||
      !RelocateRela(lib, rela, relasz / sizeof(Elf64_Rela)) ||
      !RelocateRela(lib, jmprel, jmprelsz / sizeof(Elf64_Rela))) {
    g_libs.pop_back();
    delete lib;
    return nullptr;
  }
  for (const std::string& u : lib->unresolved) lib->unresolved_c.push_back(u.c_str());
  UpdateMapsLocked();
  ndk64_flush_code(lib->base, lib->size);
  LOGI("loaded arm64 %s at 0x%x-0x%x (%zu unresolved%s)", path, lib->base, lib->base + lib->size,
       lib->unresolved.size(), lib->tls_size ? ", TLS" : "");
  return lib;
}

void Construct(Library64* lib) {
  if (lib->constructed) return;
  lib->constructed = true;
  for (Library64* dep : lib->deps) Construct(dep);
  // (argc, argv, envp), as bionic's linker calls them
  const uint64_t argv = GuestProcessArgv64();
  const uint64_t args[3] = {1, argv, argv + 16};
  if (lib->init) CallGuest64(lib->init, args, 3);
  for (uint32_t i = 0; i < lib->init_array_count; i++) {
    uint64_t f = Load64(lib->init_array + 8 * i);
    if (f != 0 && f != ~0ull) CallGuest64(static_cast<uint32_t>(f), args, 3);
  }
}

void Destruct(Library64* lib) {
  for (uint32_t i = lib->fini_array_count; i-- > 0;) {
    uint64_t f = Load64(lib->fini_array + 8 * i);
    if (f != 0 && f != ~0ull) CallGuest64(static_cast<uint32_t>(f), nullptr, 0);
  }
  if (lib->fini) CallGuest64(lib->fini, nullptr, 0);
}

Library64* Load(const char* path, const std::string& near) {
  if (!near.empty() && !strchr(path, '/')) {
    std::string full = near + path;
    if (Library64* lib = Find(full.c_str())) {
      lib->refcount++;
      return lib;
    }
    if (access(full.c_str(), R_OK) == 0) {
      Library64* lib = LoadFile(full.c_str());
      if (lib) lib->refcount = 1;
      return lib;
    }
  }
  if (Library64* lib = Find(path)) {
    lib->refcount++;
    return lib;
  }
  Library64* lib = nullptr;
  if (strchr(path, '/')) {
    lib = LoadFile(path);
  } else {
    for (const std::string& dir : g_search_dirs) {
      std::string full = dir + "/" + path;
      if (access(full.c_str(), R_OK) != 0) continue;
      lib = LoadFile(full.c_str());
      break;
    }
    if (!lib && !g_dlerror) SetError("dlopen failed: library \"%s\" not found", path);
  }
  if (lib) lib->refcount = 1;
  return lib;
}

const Library64* Containing(uint32_t addr) {
  for (const Library64* lib : g_libs)
    if (!lib->is_host && addr - lib->base < lib->size) return lib;
  return nullptr;
}

uint32_t SymbolCount(const Library64* lib) {
  uint32_t n = lib->nsyms;
  if (n == 0 && lib->gnu_bucket) {
    for (uint32_t b = 0; b < lib->gnu_nbucket; b++) n = std::max(n, lib->gnu_bucket[b]);
    if (n >= lib->gnu_symoffset)
      while (!(lib->gnu_chain[n - lib->gnu_symoffset] & 1)) n++;
    n++;
  }
  return n;
}

const Elf64_Sym* Nearest(const Library64* lib, uint32_t addr) {
  const Elf64_Sym* best = nullptr;
  uint32_t n = SymbolCount(lib);
  for (uint32_t i = 1; i < n; i++) {
    const Elf64_Sym* s = &lib->symtab[i];
    if (s->st_shndx == SHN_UNDEF || ELF64_ST_TYPE(s->st_info) == STT_SECTION || ELF64_ST_TYPE(s->st_info) == STT_TLS)
      continue;
    uint64_t a = SymAddr(lib, s);
    if (a <= addr && (!best || a > SymAddr(lib, best))) best = s;
  }
  return best;
}

const char* SymbolizeImpl(uint32_t addr, uint32_t* offset) {
  static thread_local std::string buf;
  if (const char* stub = HostStubName64(addr)) {
    *offset = 0;
    buf = std::string("<host> ") + stub;
    return buf.c_str();
  }
  std::lock_guard<std::recursive_mutex> l(g_lock);
  const Library64* lib = Containing(addr);
  if (!lib) return nullptr;
  const Elf64_Sym* s = Nearest(lib, addr);
  if (s) {
    *offset = addr - static_cast<uint32_t>(SymAddr(lib, s));
    buf = lib->name + "!" + (lib->strtab + s->st_name);
  } else {
    *offset = addr - lib->base;
    buf = lib->name;
  }
  return buf.c_str();
}

void AddSearchDirOf(const char* path) {
  if (!strchr(path, '/') || BaseName(path) == path + 1) return;
  std::string dir(path, BaseName(path) - path - 1);
  for (const std::string& d : g_search_dirs)
    if (d == dir) return;
  g_search_dirs.insert(g_search_dirs.begin(), dir);
}

void InstallSymbolizer64() {
  static std::once_flag once;
  std::call_once(once, [] { ndk64_set_symbolizer(SymbolizeImpl); });
}

}  // namespace

bool IsElf64File(const char* path) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  unsigned char h[20];
  bool yes = read(fd, h, sizeof h) == (ssize_t)sizeof h && memcmp(h, "\177ELF", 4) == 0 && h[4] == ELFCLASS64 &&
             (h[18] | h[19] << 8) == EM_AARCH64;
  close(fd);
  return yes;
}

bool IsLibrary64(void* handle) {
  std::lock_guard<std::recursive_mutex> l(g_lock);
  for (Library64* lib : g_libs)
    if (lib == handle) return true;
  for (auto& it : g_host_libs)
    if (it.second == handle) return true;
  return false;
}

void NoteExecRegion64(uint32_t lo, uint32_t hi) {
  if (hi <= lo) return;
  std::lock_guard<std::recursive_mutex> l(g_lock);
  uint32_t& end = g_exec_regions[lo];
  end = end > hi ? end : hi;
}

bool IsGuestCode64(uint32_t addr) {
  if (!HaveCpu64()) return false;
  if (ndk64_is_host_stub(addr)) return true;
  std::lock_guard<std::recursive_mutex> l(g_lock);
  if (Containing(addr)) return true;
  auto it = g_exec_regions.upper_bound(addr);
  return it != g_exec_regions.begin() && addr < (--it)->second;
}

Library64* Dlopen64(const char* path, int flags) {
  if (!HaveCpu64()) {
    SetError("dlopen failed: this build runs no arm64 code");
    return nullptr;
  }
  MaybeWatchHostCalls();
  EnsureInit();
  InstallSymbolizer64();
  std::lock_guard<std::recursive_mutex> l(g_lock);
  g_dlerror = nullptr;
  if (path == nullptr) return HostLib("<main>");
  if (IsHostLibrary(BaseName(path))) return HostLib(BaseName(path));
  AddSearchDirOf(path);
  Library64* lib = Load(path);
  if (!lib) {
    LOGE("%s", g_dlerror);
    return nullptr;
  }
  Construct(lib);
  return lib;
}

Library64* LoadNoInit64(const char* path) {
  EnsureInit();
  InstallSymbolizer64();
  std::lock_guard<std::recursive_mutex> l(g_lock);
  g_dlerror = nullptr;
  AddSearchDirOf(path);
  Library64* lib = Load(path);
  if (!lib) LOGE("%s", g_dlerror);
  return lib;
}

uint32_t Dlsym64(Library64* lib, const char* name) {
  std::lock_guard<std::recursive_mutex> l(g_lock);
  g_dlerror = nullptr;
  uint64_t a = 0;
  bool global = lib == nullptr || (lib->is_host && lib->name == "<main>");
  if (global) {
    const int ns = arc_lproc_native_ns();
    for (Library64* g : g_libs)
      if (g->ns == ns)
        if (const Elf64_Sym* s = FindExport(g, name)) return static_cast<uint32_t>(SymAddr(g, s));
    a = ResolveHostSymbol64(name);
  } else if (lib->is_host) {
    a = ResolveHostSymbol64(name);
  } else if (const Elf64_Sym* s = FindExport(lib, name)) {
    a = SymAddr(lib, s);
  } else {
    for (Library64* dep : lib->deps)
      if (const Elf64_Sym* s = FindExport(dep, name)) return static_cast<uint32_t>(SymAddr(dep, s));
  }
  if (!a) SetError("undefined symbol: %s", name);
  return static_cast<uint32_t>(a);
}

namespace {

int Dlclose(Library64* lib) {
  std::lock_guard<std::recursive_mutex> l(g_lock);
  if (lib->is_host) return 0;
  if (--lib->refcount > 0) return 0;
  // As bionic since Android 7 for libraries with TLS or thread_local
  // destructors, and to keep guest code other threads may still run: a
  // library stays loaded; its destructors run.
  if (lib->constructed) Destruct(lib);
  lib->constructed = false;
  return 0;
}

}  // namespace

const char* const* UnresolvedSymbols64(Library64* lib, size_t* n) {
  *n = lib->unresolved_c.size();
  return lib->unresolved_c.data();
}
size_t MissingNeededCount64(Library64* lib) { return lib->missing_needed.size(); }
const char* MissingNeededName64(Library64* lib, size_t i) { return lib->missing_needed[i].c_str(); }

const char* Symbolize64(uint32_t addr, uint32_t* offset) { return SymbolizeImpl(addr, offset); }

const char* Dlerror64() {
  const char* e = g_dlerror;
  g_dlerror = nullptr;
  return e;
}

std::string Maps64() {
  std::lock_guard<std::mutex> l(g_maps_lock);
  return *g_maps;
}

// --- libdl for 64-bit guests ------------------------------------------------------------------------
namespace {

// bionic LP64: RTLD_DEFAULT ((void*) 0), RTLD_NEXT ((void*) -1L).
uint64_t G64_dlopen(const char* path, int flags) { return GuestPtr64(Dlopen64(path, flags)); }

void G64_dlsym(NdkCpu64State* s, void*) {
  uint64_t handle = s->x[0];
  const char* name = Ptr<const char>(GuestAddr64(s->x[1]));
  uint32_t a;
  if (handle == 0 || handle == ~0ull) {
    a = Dlsym64(nullptr, name);
  } else if (!IsLibrary64(Ptr<void>(GuestAddr64(handle)))) {
    SetError("dlsym: invalid handle 0x%llx", (unsigned long long)handle);
    a = 0;
  } else {
    a = Dlsym64(Ptr<Library64>(GuestAddr64(handle)), name);
  }
  s->x[0] = a;
}

int G64_dlclose(void* handle) {
  if (!IsLibrary64(handle)) {
    SetError("dlclose: invalid handle %p", handle);
    return -1;
  }
  return Dlclose(static_cast<Library64*>(handle));
}

const char* G64_dlerror() {
  const char* e = g_dlerror;
  g_dlerror = nullptr;
  return e;
}

// Dl_info: four 64-bit pointers.
int G64_dladdr(uint64_t addr, uint64_t* info) {
  std::lock_guard<std::recursive_mutex> l(g_lock);
  const Library64* lib = Containing(static_cast<uint32_t>(addr));
  if (!lib) return 0;
  info[0] = Addr(lib->path.c_str());
  info[1] = lib->base;
  const Elf64_Sym* s = Nearest(lib, static_cast<uint32_t>(addr));
  info[2] = s ? Addr(lib->strtab + s->st_name) : 0;
  info[3] = s ? SymAddr(lib, s) : 0;
  return 1;
}

// dl_iterate_phdr: struct dl_phdr_info of LP64 bionic (addr, name, phdr,
// phnum; then adds, subs, tls_modid, tls_data). The callback is guest code
// and runs without the loader lock.
struct GuestPhdrInfo64 {
  uint64_t dlpi_addr, dlpi_name, dlpi_phdr;
  uint16_t dlpi_phnum, pad[3];
  uint64_t dlpi_adds, dlpi_subs, dlpi_tls_modid, dlpi_tls_data;
};
int G64_dl_iterate_phdr(uint64_t callback, uint64_t data) {
  std::vector<GuestPhdrInfo64> infos;
  {
    std::lock_guard<std::recursive_mutex> l(g_lock);
    for (const Library64* lib : g_libs) {
      if (lib->is_host || !lib->phdr) continue;
      GuestPhdrInfo64 i;
      memset(&i, 0, sizeof i);
      i.dlpi_addr = lib->bias;
      i.dlpi_name = Addr(lib->path.c_str());
      i.dlpi_phdr = lib->phdr;
      i.dlpi_phnum = static_cast<uint16_t>(lib->phnum);
      i.dlpi_adds = g_libs.size();
      i.dlpi_tls_modid = lib->tls_module;
      infos.push_back(i);
    }
  }
  for (GuestPhdrInfo64& info : infos) {
    uint64_t args[3] = {Addr(&info), sizeof info, data};
    int32_t r = static_cast<int32_t>(CallGuest64(static_cast<uint32_t>(callback), args, 3));
    if (r != 0) return r;
  }
  return 0;
}

// android_dlopen_ext(path, flags, const android_dlextinfo*): the extensions
// (reserved address space, file descriptors) are not needed here.
uint64_t G64_android_dlopen_ext(const char* path, int flags, const void*) { return G64_dlopen(path, flags); }
// Control-flow integrity of the platform: nothing to check.
void G64_cfi_slowpath(uint64_t, const void*) {}
void G64_cfi_slowpath_diag(uint64_t, const void*, const void*) {}

const HostSymbol kLibdl64[] = {
    NDK_FN64("dlopen", G64_dlopen),
    NDK_RAW64("dlsym", G64_dlsym),
    NDK_FN64("dlclose", G64_dlclose),
    NDK_FN64("dlerror", G64_dlerror),
    NDK_FN64("dladdr", G64_dladdr),
    NDK_FN64("dl_iterate_phdr", G64_dl_iterate_phdr),
    NDK_FN64("android_dlopen_ext", G64_android_dlopen_ext),
    // The linker's own (ld-android.so), which libdl's wrappers call with the
    // caller's address as the last argument (ignored: one namespace).
    NDK_FN64("__loader_dlopen", G64_dlopen), NDK_RAW64("__loader_dlsym", G64_dlsym),
    NDK_FN64("__loader_android_dlopen_ext", G64_android_dlopen_ext),
    NDK_FN64("__loader_dlclose", G64_dlclose), NDK_FN64("__loader_dlerror", G64_dlerror),
    NDK_FN64("__loader_dladdr", G64_dladdr), NDK_FN64("__loader_dl_iterate_phdr", G64_dl_iterate_phdr),
    NDK_FN64("__cfi_slowpath", G64_cfi_slowpath),
    NDK_FN64("__cfi_slowpath_diag", G64_cfi_slowpath_diag),
};

}  // namespace

void RegisterLibdl64() { RegisterHostSymbols("libdl.so", kLibdl64); }

}  // namespace ndk

// Keeps this file in the link (-Wl,-u,ndk64_linker_linked, mk/topic-ndk-arm64.mk): the
// shared bridge files refer to it only weakly.
extern "C" int ndk64_linker_linked = 1;
