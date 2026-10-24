// The ELF loader and dynamic linker for guest (ARM) libraries, in the
// manner of bionic's linker in Android 4.4: segments are copied into
// anonymous memory (guest address == host address), relocations applied
// (also Android's packed APS2 and RELR forms, ndk_elf_relocs.h), imports
// bound to other guest libraries or to host stubs (the bridge's libc, libm,
// libGLESv2, ...) by name and symbol version, ELF TLS placed in each
// thread's static TLS area, then DT_INIT and DT_INIT_ARRAY run on the CPU.
// Also the guest's libdl. Part of the arc-wasm shim layer.

#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "../multiproc/include/arc_lproc.h"
#include "ndk_bridge.h"
#include "ndk_elf_relocs.h"

extern "C" int __android_log_print(int prio, const char* tag, const char* fmt, ...);
extern "C" int __android_log_write(int prio, const char* tag, const char* text);
// Optional CPU extension (shim/ndk/cpu/ndk_cpu_ext.h); absent in the stub CPU.
extern "C" void ndk_guest_stack(uint32_t* base, uint32_t* size) __attribute__((weak));
extern "C" int property_get(const char* key, char* value, const char* default_value);
extern "C" void ndk_cpu_report_recent_calls(void) __attribute__((weak));
extern "C" void ndk_cpu_set_block_hook(void (*hook)(uint32_t, uint32_t)) __attribute__((weak));
#define LOGI(...) __android_log_print(4, "ndk", __VA_ARGS__)
#define LOGW(...) __android_log_print(5, "ndk", __VA_ARGS__)
#define LOGE(...) __android_log_print(6, "ndk", __VA_ARGS__)

namespace ndk {

struct Library {
  std::string path;
  std::string name;  // last path component
  std::string soname;
  uint32_t base = 0, size = 0;  // the mapping
  uint32_t bias = 0;            // load address - link address
  const Elf32_Dyn* dynamic = nullptr;
  const Elf32_Sym* symtab = nullptr;
  const char* strtab = nullptr;
  uint32_t nsyms = 0;
  // DT_HASH
  uint32_t nbucket = 0;
  const uint32_t* bucket = nullptr;
  const uint32_t* chain = nullptr;
  // DT_GNU_HASH
  uint32_t gnu_nbucket = 0, gnu_symoffset = 0, gnu_bloom_size = 0, gnu_shift = 0;
  const uint32_t* gnu_bloom = nullptr;
  const uint32_t* gnu_bucket = nullptr;
  const uint32_t* gnu_chain = nullptr;
  uint32_t init = 0, fini = 0;
  uint32_t init_array = 0, init_array_count = 0;
  uint32_t fini_array = 0, fini_array_count = 0;
  uint32_t exidx = 0, exidx_count = 0;
  uint32_t phdr = 0, phnum = 0;  // the program headers in memory (dl_iterate_phdr)
  ElfVersions versions;
  // ELF TLS: the block's offset from TPIDRURO in every thread's static TLS
  // area and its module id (__tls_get_addr), 0 without PT_TLS.
  uint32_t tls_offset = 0, tls_size = 0;
  uint32_t tls_module = 0;
  std::vector<std::string> needed;
  std::vector<Library*> deps;  // guest libraries among |needed|
  std::vector<std::string> unresolved;
  std::vector<const char*> unresolved_c;
  std::vector<std::string> missing_needed;  // only with SetSkipMissingNeeded
  int refcount = 0;
  bool constructed = false;
  bool is_host = false;  // a pseudo-handle for a host library
  // The light process whose copy this is (0: the VM's process; topic
  // multiproc): each process loads an app's libraries for itself.
  int ns = 0;
};

namespace {

#ifndef NT_GNU_BUILD_ID
#define NT_GNU_BUILD_ID 3
#endif

// --- compatibility patches for known app libraries ------------------------------------
// A few bytes of a specific build (by name and GNU build-id) of an app's
// library, replaced after relocation where the library's behaviour cannot be
// changed from outside. Each patch checks the bytes it replaces.
struct AppPatch {
  const char* name;       // library file name
  const char* build_id;   // NT_GNU_BUILD_ID, hex
  uint32_t offset;        // from the load address
  const char* expect;     // hex bytes there
  const char* replace;    // hex bytes written instead
  const char* why;
  // A system property that, when "1", leaves this patch out (or null).
  const char* unless_prop;
};
const AppPatch kAppPatches[] = {
    // Firefox 158.0 (org.mozilla.firefox, armeabi-v7a): Gecko's
    // BrowserTabsRemoteAutostart() honours MOZ_FORCE_DISABLE_E10S=1 in official
    // builds only "in automation" (xpc::AreNonLocalConnectionsDisabled(), which
    // also refuses every non-local network connection). The VM is one process,
    // so web content must load in the parent: the check of the env variable
    // becomes unconditional, as in Gecko's non-official builds (three inlined
    // copies: cbnz/beq on the flag -> b). docs/topics/firefox8.md.
    {"libxul.so", "d0d0ca3cd6841f8600c12004e79037621cbc8838", 0x4047d2a, "21b9", "04e0",
     "MOZ_FORCE_DISABLE_E10S outside automation"},
    {"libxul.so", "d0d0ca3cd6841f8600c12004e79037621cbc8838", 0x41e38a4, "05d0", "05e0",
     "MOZ_FORCE_DISABLE_E10S outside automation"},
    {"libxul.so", "d0d0ca3cd6841f8600c12004e79037621cbc8838", 0x590cfc2, "05d0", "05e0",
     "MOZ_FORCE_DISABLE_E10S outside automation"},
    // GeckoView's nsWindow opens its chrome window with the features
    // "chrome,dialog=0,remote,resizable": "remote" makes the window's
    // browsing contexts use remote tabs whatever e10s says. "xemote" is a
    // feature nobody knows.
    // With content processes (light processes of topic multiproc: SystemServer
    // --gecko-e10s sets arcwasm.gecko.e10s) the window keeps its remote tabs.
    {"libxul.so", "d0d0ca3cd6841f8600c12004e79037621cbc8838", 0x10ab161, "72656d6f7465",
     "78656d6f7465", "GeckoView window without remote tabs", "arcwasm.gecko.e10s"},
};

std::string HexOf(const uint8_t* p, size_t n) {
  static const char kHex[] = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; ++i) {
    s += kHex[p[i] >> 4];
    s += kHex[p[i] & 15];
  }
  return s;
}

// The GNU build-id of a loaded library (from its PT_NOTE segments), hex.
std::string BuildId(const Library* lib) {
  for (uint32_t i = 0; i < lib->phnum; ++i) {
    const Elf32_Phdr* ph = Ptr<const Elf32_Phdr>(lib->phdr + i * sizeof(Elf32_Phdr));
    if (ph->p_type != PT_NOTE)
      continue;
    uint32_t p = lib->bias + ph->p_vaddr, end = p + ph->p_memsz;
    while (p + 12 <= end) {
      const uint32_t* h = Ptr<const uint32_t>(p);
      uint32_t namesz = h[0], descsz = h[1], type = h[2];
      uint32_t desc = p + 12 + ((namesz + 3) & ~3u);
      if (type == NT_GNU_BUILD_ID && namesz == 4 && !memcmp(Ptr<const char>(p + 12), "GNU", 4))
        return HexOf(Ptr<const uint8_t>(desc), descsz);
      p = desc + ((descsz + 3) & ~3u);
    }
  }
  return "";
}

void ApplyAppPatches(const Library* lib) {
  std::string id;
  for (const AppPatch& patch : kAppPatches) {
    if (lib->name != patch.name)
      continue;
    if (id.empty())
      id = BuildId(lib);
    if (id != patch.build_id)
      continue;
    if (patch.unless_prop) {
      char v[92] = "";
      if (property_get(patch.unless_prop, v, "0") > 0 && strcmp(v, "1") == 0) {
        LOGI("%s+0x%x: not patched (%s): %s=1", patch.name, patch.offset, patch.why,
             patch.unless_prop);
        continue;
      }
    }
    size_t n = strlen(patch.expect) / 2;
    uint8_t* at = Ptr<uint8_t>(lib->bias + patch.offset);
    if (HexOf(at, n) != patch.expect) {
      LOGW("%s+0x%x: not patched (%s): found %s", patch.name, patch.offset, patch.why,
           HexOf(at, n).c_str());
      continue;
    }
    for (size_t i = 0; i < n; ++i) {
      unsigned v;
      sscanf(patch.replace + 2 * i, "%2x", &v);
      at[i] = static_cast<uint8_t>(v);
    }
    LOGI("%s+0x%x: patched (%s)", patch.name, patch.offset, patch.why);
  }
}

std::recursive_mutex g_lock;
std::vector<Library*> g_libs;          // loaded guest libraries, load order
std::map<std::string, Library*> g_host_libs;  // pseudo-handles
std::vector<std::string> g_ld_library_path;  // android_update_LD_LIBRARY_PATH
std::vector<std::string> g_search_dirs = {"/vendor/lib", "/system/lib"};
bool g_skip_missing_needed;  // import checks: load without absent DT_NEEDED
uint64_t g_loads, g_unloads;  // dl_phdr_info's dlpi_adds and dlpi_subs
// The text of /proc/self/maps for the guest (arc_proc_maps, read by
// shim/app6/proc_self.cc under the kernel's mutex): rebuilt here, under
// g_lock, when a library comes or goes, so that reading it takes only
// g_maps_lock (g_lock is held across file I/O, which needs the kernel's
// mutex).
std::mutex g_maps_lock;
std::string* g_maps = new std::string;
thread_local std::string g_dlerror_buf;
thread_local const char* g_dlerror;

// One line per PT_LOAD segment of each loaded guest library, as the Linux
// kernel prints a mapping: the hooking and crash libraries of apps
// (bytehook, shadowhook, npth, security SDKs) find libraries' load addresses
// there and then read their ELF headers.
void UpdateMapsLocked() {
  std::string text;
  char line[600];
  for (const Library* lib : g_libs) {
    if (lib->is_host || !lib->phdr)
      continue;
    const Elf32_Phdr* ph = Ptr<const Elf32_Phdr>(lib->phdr);
    const unsigned inode = static_cast<unsigned>(std::hash<std::string>()(lib->path) & 0xfffff);
    for (uint32_t i = 0; i < lib->phnum; ++i) {
      if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0)
        continue;
      const uint32_t start = (lib->bias + ph[i].p_vaddr) & ~0xfffu;
      const uint32_t end = (lib->bias + ph[i].p_vaddr + ph[i].p_memsz + 0xfff) & ~0xfffu;
      snprintf(line, sizeof(line), "%08x-%08x %c%c%cp %08x fd:01 %-10u %s\n", start, end,
               (ph[i].p_flags & PF_R) ? 'r' : '-', (ph[i].p_flags & PF_W) ? 'w' : '-',
               (ph[i].p_flags & PF_X) ? 'x' : '-', ph[i].p_offset & ~0xfffu, inode,
               lib->path.c_str());
      text += line;
    }
  }
  std::lock_guard<std::mutex> l(g_maps_lock);
  g_maps->swap(text);
}

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
  for (const unsigned char* p = (const unsigned char*)name; *p; p++)
    h = h * 33 + *p;
  return h;
}

bool Exported(const Elf32_Sym* s) {
  int bind = ELF32_ST_BIND(s->st_info);
  return s->st_shndx != SHN_UNDEF && (bind == STB_GLOBAL || bind == STB_WEAK) &&
         ELF32_ST_VISIBILITY(s->st_other) != STV_HIDDEN &&
         ELF32_ST_VISIBILITY(s->st_other) != STV_INTERNAL;
}

// An exported definition of |name| in |lib|, or nullptr: of version
// |version| when given and |lib| has it, else the default version.
const Elf32_Sym* FindExport(const Library* lib, const char* name, const char* version = nullptr) {
  const uint16_t need = VerdefIndex(lib->versions, lib->strtab, version);
  auto match = [&](uint32_t n) {
    const Elf32_Sym* s = &lib->symtab[n];
    return strcmp(lib->strtab + s->st_name, name) == 0 && Exported(s) &&
           VersionMatches(lib->versions, n, need);
  };
  if (lib->gnu_bucket) {
    uint32_t h = GnuHash(name);
    uint32_t word = lib->gnu_bloom[(h / 32) % lib->gnu_bloom_size];
    uint32_t mask = (1u << (h % 32)) | (1u << ((h >> lib->gnu_shift) % 32));
    if ((word & mask) != mask)
      return nullptr;
    uint32_t n = lib->gnu_bucket[h % lib->gnu_nbucket];
    if (n == 0)
      return nullptr;
    for (;; n++) {
      uint32_t ch = lib->gnu_chain[n - lib->gnu_symoffset];
      if ((ch | 1) == (h | 1) && match(n))
        return &lib->symtab[n];
      if (ch & 1)
        return nullptr;
    }
  }
  if (lib->bucket) {
    for (uint32_t n = lib->bucket[ElfHash(name) % lib->nbucket]; n != 0; n = lib->chain[n]) {
      if (match(n))
        return &lib->symtab[n];
    }
  }
  return nullptr;
}

uint32_t SymAddr(const Library* lib, const Elf32_Sym* s) {
  if (s->st_shndx == SHN_ABS || ELF32_ST_TYPE(s->st_info) == STT_TLS)
    return s->st_value;  // TLS: an offset in the library's TLS block
  return lib->bias + s->st_value;
}

Library* HostLib(const char* name) {
  auto it = g_host_libs.find(name);
  if (it != g_host_libs.end())
    return it->second;
  Library* lib = new Library;
  lib->name = lib->path = lib->soname = name;
  lib->is_host = true;
  g_host_libs[name] = lib;
  return lib;
}

void MissingSymbol(NdkCpuState* s, void* user) {
  LOGE("guest called the unresolved symbol %s (lr=0x%x)", (const char*)user, s->r[14]);
  fprintf(stderr, "ndk: guest called the unresolved symbol %s (lr=0x%x)\n",
          (const char*)user, s->r[14]);
  char why[160];
  snprintf(why, sizeof why, "a call to %s, which this system lacks", (const char*)user);
  ndk_leave_guest(why);
  abort();
}

// --- ELF TLS -----------------------------------------------------------------------------
// Every thread that runs ARM code has a static TLS area after its bionic TLS
// slots (ndk_core.cc, GuestThread: TPIDRURO points at the slots); each
// library with PT_TLS gets a block there at the same offset in all threads
// (bionic's static TLS; dynamic TLS is not needed: blocks never move).
// Blocks are initialized from the library's image in the threads that exist
// at load time and in every thread created later. Thread-local variables
// of libraries built for API < 29 are emulated (__emutls_get_address,
// linked into the library) and do not come here.
struct TlsImage {
  uint32_t offset, image, filesz, memsz;
};
std::mutex g_tls_lock;
std::vector<TlsImage> g_tls_images;   // index = module id - 1
uint32_t g_tls_next = kGuestStaticTlsStart;
std::vector<uint32_t>* g_tls_threads;   // TPIDRURO of the threads with an area
// The blocks' offsets by module id - 1 for __tls_get_addr, read without the
// lock (an entry is written before the count that publishes it).
constexpr uint32_t kMaxTlsModules = 256;
uint32_t g_tls_module_offset[kMaxTlsModules];
std::atomic<uint32_t> g_tls_modules{0};

void InitTlsBlock(uint32_t tp, const TlsImage& t) {
  memcpy(Ptr<void>(tp + t.offset), Ptr<void>(t.image), t.filesz);
  memset(Ptr<void>(tp + t.offset + t.filesz), 0, t.memsz - t.filesz);
}

bool AllocateTls(Library* lib, const Elf32_Phdr& ph) {
  std::lock_guard<std::mutex> l(g_tls_lock);
  uint32_t align = ph.p_align > 8 ? ph.p_align : 8;
  uint32_t off = (g_tls_next + align - 1) & ~(align - 1);
  if (align > 64 || off + ph.p_memsz > kGuestTlsAreaBytes || g_tls_images.size() >= kMaxTlsModules) {
    SetError("dlopen failed: \"%s\": no room for %u bytes of TLS (align %u)", lib->path.c_str(),
             ph.p_memsz, align);
    return false;
  }
  g_tls_next = off + ph.p_memsz;
  lib->tls_offset = off;
  lib->tls_size = ph.p_memsz;
  lib->tls_module = static_cast<uint32_t>(g_tls_images.size()) + 1;
  TlsImage t = {off, lib->bias + ph.p_vaddr, ph.p_filesz, ph.p_memsz};
  g_tls_images.push_back(t);
  g_tls_module_offset[lib->tls_module - 1] = off;
  g_tls_modules.store(lib->tls_module, std::memory_order_release);
  if (g_tls_threads) {
    for (uint32_t tp : *g_tls_threads)
      InitTlsBlock(tp, t);
  }
  return true;
}

// A library going away: later threads get its block zero-filled (its image
// is unmapped; no code refers to the block any more).
void ForgetTls(const Library* lib) {
  if (!lib->tls_module)
    return;
  std::lock_guard<std::mutex> l(g_tls_lock);
  g_tls_images[lib->tls_module - 1].filesz = 0;
}

// __tls_get_addr(tls_index*) (ARM: {module, offset}, general dynamic model).
void TlsGetAddr(NdkCpuState* s, void*) {
  uint32_t module = Load32(s->r[0]), offset = Load32(s->r[0] + 4);
  uint32_t tp = s->tls ? s->tls : CurrentState()->tls;
  if (module == 0 || module > g_tls_modules.load(std::memory_order_acquire)) {
    LOGE("__tls_get_addr: no TLS module %u", module);
    s->r[0] = 0;
    return;
  }
  s->r[0] = tp + g_tls_module_offset[module - 1] + offset;
}

// --- relocation -------------------------------------------------------------------------
struct Resolved {
  uint32_t value;
  const Library* from;  // the defining guest library (TLS: its block), or null
  bool found;
};

// Resolves |name| for a relocation in |lib|, as bionic's soinfo_do_lookup:
// the library itself, then its DT_NEEDED libraries, the host libraries, the
// rest of its local group (its dependencies' dependencies, breadth first),
// then (leniently) other loaded guest libraries -- but not for a weak
// import: bionic does not look in libraries loaded RTLD_LOCAL elsewhere, and
// a weak import it does not find is 0 (WebRTC's 'sdallocx' bound to React
// Native's variable of that name was called: Jitsi Meet). |version| is the
// DT_VERNEED version the import asks for, or null.
Resolved Lookup(Library* lib, const char* name, const char* version, bool weak) {
  if (const Elf32_Sym* s = FindExport(lib, name, version))
    return {SymAddr(lib, s), lib, true};
  for (Library* dep : lib->deps) {
    if (const Elf32_Sym* s = FindExport(dep, name, version))
      return {SymAddr(dep, s), dep, true};
  }
  if (uint32_t a = ResolveHostSymbol(name))
    return {a, nullptr, true};
  std::vector<Library*> group(lib->deps.begin(), lib->deps.end());
  for (size_t i = 0; i < group.size(); i++) {
    for (Library* dep : group[i]->deps) {
      if (dep == lib || std::find(group.begin(), group.end(), dep) != group.end())
        continue;
      group.push_back(dep);
      if (const Elf32_Sym* s = FindExport(dep, name, version))
        return {SymAddr(dep, s), dep, true};
    }
  }
  if (weak)
    return {0, nullptr, false};
  for (Library* other : g_libs) {
    if (other == lib || other->ns != lib->ns)
      continue;
    if (const Elf32_Sym* s = FindExport(other, name, version))
      return {SymAddr(other, s), other, true};
  }
  return {0, nullptr, false};
}

bool IsTlsReloc(uint32_t type) {
  return type == R_ARM_TLS_DTPMOD32 || type == R_ARM_TLS_DTPOFF32 || type == R_ARM_TLS_TPOFF32;
}

// One REL relocation (the addend is the word in place), from DT_REL,
// DT_JMPREL or a packed DT_ANDROID_REL table.
bool RelocateOne(Library* lib, uint32_t type, uint32_t symi, uint32_t offset) {
  uint32_t where = lib->bias + offset;
  if (type == R_ARM_NONE)
    return true;
  if (type == R_ARM_RELATIVE) {
    Store32(where, Load32(where) + lib->bias);
    return true;
  }
  if (type == R_ARM_IRELATIVE) {
    // The resolver gets AT_HWCAP (bionic: call_ifunc_resolver).
    Store32(where, CallGuest1(lib->bias + Load32(where), 0));
    return true;
  }
  const bool tls = IsTlsReloc(type);
  Resolved r = {0, nullptr, true};
  if (symi != 0) {
    const Elf32_Sym* s = &lib->symtab[symi];
    const char* name = lib->strtab + s->st_name;
    if (s->st_shndx != SHN_UNDEF && ELF32_ST_BIND(s->st_info) == STB_LOCAL) {
      r = {SymAddr(lib, s), lib, true};  // a local symbol: itself, never by name
    } else {
      r = Lookup(lib, name, RequiredVersion(lib->versions, lib->strtab, symi),
                 ELF32_ST_BIND(s->st_info) == STB_WEAK);
    }
    if (r.found && tls && !r.from) {
      SetError("%s: TLS symbol %s is not in a guest library", lib->name.c_str(), name);
      return false;
    }
    if (!r.found) {
      if (ELF32_ST_BIND(s->st_info) == STB_WEAK) {
        r = {0, nullptr, true};
      } else {
        bool seen = false;
        for (const std::string& u : lib->unresolved)
          seen |= u == name;
        if (!seen) {
          lib->unresolved.push_back(name);
          LOGW("%s: unresolved symbol %s", lib->name.c_str(), name);
        }
        if (tls) {
          SetError("%s: unresolved TLS symbol %s", lib->name.c_str(), name);
          return false;
        }
        r = {MakeStub(MissingSymbol, strdup(name), strdup(name)), nullptr, true};
      }
    }
  } else if (tls) {
    r = {0, lib, true};  // local dynamic: the library's own block
  }
  const uint32_t sym = r.value;
  switch (type) {
    case R_ARM_ABS32:
      Store32(where, Load32(where) + sym);
      return true;
    case R_ARM_REL32:
      Store32(where, Load32(where) + sym - where);
      return true;
    case R_ARM_GLOB_DAT:
    case R_ARM_JUMP_SLOT:
      Store32(where, sym);
      return true;
    case R_ARM_TLS_DTPMOD32:
      Store32(where, r.from ? r.from->tls_module : 0);
      return true;
    case R_ARM_TLS_DTPOFF32:
      Store32(where, Load32(where) + sym);
      return true;
    case R_ARM_TLS_TPOFF32:
      if (r.from && !r.from->tls_module) {
        SetError("%s: TLS relocation against %s, which has no TLS", lib->name.c_str(),
                 r.from->name.c_str());
        return false;
      }
      Store32(where, Load32(where) + (r.from ? r.from->tls_offset : 0) + sym);
      return true;
  }
  SetError("%s: unsupported relocation type %u", lib->name.c_str(), type);
  return false;
}

bool Relocate(Library* lib, const Elf32_Rel* rel, size_t count) {
  for (size_t i = 0; i < count; i++, rel++) {
    if (!RelocateOne(lib, ELF32_R_TYPE(rel->r_info), ELF32_R_SYM(rel->r_info), rel->r_offset))
      return false;
  }
  return true;
}

// DT_ANDROID_REL: APS2 packed REL relocations.
bool RelocatePacked(Library* lib, const uint8_t* data, size_t size) {
  const char* error;
  bool ok = ForEachPackedReloc<uint32_t>(data, size, false,
      [lib](uint32_t offset, uint32_t info, int32_t) {
        return RelocateOne(lib, ELF32_R_TYPE(info), ELF32_R_SYM(info), offset);
      }, &error);
  if (!ok && error)
    SetError("%s: %s", lib->name.c_str(), error);
  return ok;
}

// DT_RELR: relative relocations as addresses and bitmaps.
void RelocateRelr(Library* lib, const uint32_t* relr, size_t count) {
  ForEachRelr<uint32_t>(relr, count, [lib](uint32_t offset) {
    uint32_t where = lib->bias + offset;
    Store32(where, Load32(where) + lib->bias);
  });
}

bool ReadFully(int fd, void* buf, size_t size, off_t offset) {
  char* p = static_cast<char*>(buf);
  while (size > 0) {
    ssize_t n = pread(fd, p, size, offset);
    if (n <= 0)
      return false;
    p += n;
    size -= n;
    offset += n;
  }
  return true;
}

Library* Find(const char* path) {
  bool bare = strchr(path, '/') == nullptr;
  const int ns = arc_lproc_native_ns();
  for (Library* lib : g_libs) {
    if (lib->ns != ns)
      continue;
    if (bare ? (lib->name == path || lib->soname == path) : lib->path == path)
      return lib;
  }
  return nullptr;
}

Library* Load(const char* path, const std::string& near = std::string());

// Loads one file (no constructors yet). Returns nullptr with dlerror set.
Library* LoadFile(const char* path) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    SetError("dlopen failed: \"%s\": %s", path, strerror(errno));
    return nullptr;
  }
  Elf32_Ehdr eh;
  if (!ReadFully(fd, &eh, sizeof eh, 0) || memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 ||
      eh.e_ident[EI_CLASS] != ELFCLASS32 || eh.e_ident[EI_DATA] != ELFDATA2LSB) {
    close(fd);
    SetError("dlopen failed: \"%s\" is not a 32-bit little-endian ELF file", path);
    return nullptr;
  }
  if (eh.e_machine != EM_ARM || eh.e_type != ET_DYN) {
    close(fd);
    SetError("dlopen failed: \"%s\" is not an ARM shared object (machine %d, type %d)",
             path, eh.e_machine, eh.e_type);
    return nullptr;
  }
  std::vector<Elf32_Phdr> ph(eh.e_phnum);
  if (eh.e_phentsize != sizeof(Elf32_Phdr) ||
      !ReadFully(fd, ph.data(), ph.size() * sizeof(Elf32_Phdr), eh.e_phoff)) {
    close(fd);
    SetError("dlopen failed: \"%s\": bad program headers", path);
    return nullptr;
  }
  uint32_t lo = UINT32_MAX, hi = 0;
  for (const Elf32_Phdr& p : ph) {
    if (p.p_type != PT_LOAD)
      continue;
    lo = std::min(lo, p.p_vaddr & ~4095u);
    hi = std::max(hi, (p.p_vaddr + p.p_memsz + 4095) & ~4095u);
  }
  if (lo >= hi) {
    close(fd);
    SetError("dlopen failed: \"%s\" has no loadable segments", path);
    return nullptr;
  }
  void* map = mmap(nullptr, hi - lo, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (map == MAP_FAILED) {
    close(fd);
    SetError("dlopen failed: \"%s\": cannot map %u bytes", path, hi - lo);
    return nullptr;
  }
  Library* lib = new Library;
  lib->path = path;
  lib->name = BaseName(path);
  lib->base = Addr(map);
  lib->size = hi - lo;
  lib->bias = lib->base - lo;
  memset(map, 0, lib->size);
  bool ok = true;
  for (const Elf32_Phdr& p : ph) {
    if (p.p_type == PT_LOAD && p.p_filesz > 0)
      ok &= ReadFully(fd, Ptr<void>(lib->bias + p.p_vaddr), p.p_filesz, p.p_offset);
    else if (p.p_type == PT_DYNAMIC)
      lib->dynamic = Ptr<Elf32_Dyn>(lib->bias + p.p_vaddr);
    else if (p.p_type == PT_ARM_EXIDX) {
      lib->exidx = lib->bias + p.p_vaddr;
      lib->exidx_count = p.p_memsz / 8;
    }
  }
  const Elf32_Phdr* tls_ph = nullptr;
  for (const Elf32_Phdr& p : ph) {
    if (p.p_type == PT_TLS && p.p_memsz > 0)
      tls_ph = &p;
  }
  // The program headers as loaded: PT_PHDR, or inside the segment that maps
  // file offset e_phoff.
  for (const Elf32_Phdr& p : ph) {
    if (p.p_type == PT_PHDR)
      lib->phdr = lib->bias + p.p_vaddr;
  }
  for (const Elf32_Phdr& p : ph) {
    if (!lib->phdr && p.p_type == PT_LOAD && eh.e_phoff >= p.p_offset &&
        eh.e_phoff + ph.size() * sizeof(Elf32_Phdr) <= p.p_offset + p.p_filesz)
      lib->phdr = lib->bias + p.p_vaddr + (eh.e_phoff - p.p_offset);
  }
  lib->phnum = lib->phdr ? eh.e_phnum : 0;
  close(fd);
  if (!ok || !lib->dynamic) {
    SetError("dlopen failed: \"%s\": %s", path, ok ? "no dynamic section" : "read error");
    munmap(map, lib->size);
    delete lib;
    return nullptr;
  }

  const Elf32_Rel *rel = nullptr, *jmprel = nullptr;
  uint32_t relsz = 0, jmprelsz = 0;
  const uint8_t* packed = nullptr;  // DT_ANDROID_REL
  uint32_t packedsz = 0;
  const uint32_t* relr = nullptr;
  uint32_t relrsz = 0;
  std::vector<uint32_t> needed_off;
  uint32_t soname_off = UINT32_MAX;
  for (const Elf32_Dyn* d = lib->dynamic; d->d_tag != DT_NULL; d++) {
    uint32_t v = d->d_un.d_val;
    uint32_t a = lib->bias + v;
    switch (d->d_tag) {
      case DT_NEEDED: needed_off.push_back(v); break;
      case DT_SONAME: soname_off = v; break;
      case DT_STRTAB: lib->strtab = Ptr<const char>(a); break;
      case DT_SYMTAB: lib->symtab = Ptr<const Elf32_Sym>(a); break;
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
        lib->gnu_bloom = Ptr<const uint32_t>(a + 16);
        lib->gnu_bucket = lib->gnu_bloom + lib->gnu_bloom_size;
        lib->gnu_chain = lib->gnu_bucket + lib->gnu_nbucket;
        break;
      case DT_REL: rel = Ptr<const Elf32_Rel>(a); break;
      case DT_RELSZ: relsz = v; break;
      case DT_JMPREL: jmprel = Ptr<const Elf32_Rel>(a); break;
      case DT_PLTRELSZ: jmprelsz = v; break;
      case DT_PLTREL:
        if (v != DT_REL) {
          SetError("dlopen failed: \"%s\": DT_PLTREL is not DT_REL", path);
          ok = false;
        }
        break;
      case DT_RELA:
      case DT_ANDROID_RELA:
        SetError("dlopen failed: \"%s\": RELA relocations on ARM", path);
        ok = false;
        break;
      case DT_ANDROID_REL: packed = Ptr<const uint8_t>(a); break;
      case DT_ANDROID_RELSZ: packedsz = v; break;
      case DT_RELR: case DT_ANDROID_RELR: relr = Ptr<const uint32_t>(a); break;
      case DT_RELRSZ: case DT_ANDROID_RELRSZ: relrsz = v; break;
      case DT_RELRENT: case DT_ANDROID_RELRENT:
        if (v != 4) {
          SetError("dlopen failed: \"%s\": RELR entries of %u bytes", path, v);
          ok = false;
        }
        break;
      case DT_VERSYM: lib->versions.versym = Ptr<const uint16_t>(a); break;
      case DT_VERDEF: lib->versions.verdef = a; break;
      case DT_VERDEFNUM: lib->versions.verdefnum = v; break;
      case DT_VERNEED: lib->versions.verneed = a; break;
      case DT_VERNEEDNUM: lib->versions.verneednum = v; break;
      case DT_INIT: lib->init = a; break;
      case DT_FINI: lib->fini = a; break;
      case DT_INIT_ARRAY: lib->init_array = a; break;
      case DT_INIT_ARRAYSZ: lib->init_array_count = v / 4; break;
      case DT_FINI_ARRAY: lib->fini_array = a; break;
      case DT_FINI_ARRAYSZ: lib->fini_array_count = v / 4; break;
      default: break;
    }
  }
  if (ok && (!lib->strtab || !lib->symtab || (!lib->bucket && !lib->gnu_bucket))) {
    SetError("dlopen failed: \"%s\": no symbol table or hash", path);
    ok = false;
  }
  if (!ok) {
    munmap(map, lib->size);
    delete lib;
    return nullptr;
  }
  if (tls_ph && !AllocateTls(lib, *tls_ph)) {
    munmap(map, lib->size);
    delete lib;
    return nullptr;
  }
  if (soname_off != UINT32_MAX)
    lib->soname = lib->strtab + soname_off;
  for (uint32_t off : needed_off)
    lib->needed.push_back(lib->strtab + off);

  // Dependencies first (they are relocated and constructed before us).
  lib->ns = arc_lproc_native_ns();
  g_libs.push_back(lib);
  for (const std::string& n : lib->needed) {
    if (IsHostLibrary(n.c_str()))
      continue;
    // The directory of the library itself first: one VM runs several
    // apps, whose libraries may share names.
    Library* dep = Load(n.c_str(), std::string(path, BaseName(path) - path));
    if (!dep && g_skip_missing_needed) {
      lib->missing_needed.push_back(n);
      continue;
    }
    if (!dep) {
      std::string err = g_dlerror ? g_dlerror : "?";
      SetError("dlopen failed: library \"%s\" needed by \"%s\": %s", n.c_str(),
               lib->name.c_str(), err.c_str());
      g_libs.pop_back();
      ForgetTls(lib);
      munmap(map, lib->size);
      delete lib;
      return nullptr;
    }
    lib->deps.push_back(dep);
  }

  // Relative relocations first (RELR), then the rest, as bionic.
  if (relr)
    RelocateRelr(lib, relr, relrsz / 4);
  if ((packed && !RelocatePacked(lib, packed, packedsz)) ||
      !Relocate(lib, rel, relsz / sizeof(Elf32_Rel)) ||
      !Relocate(lib, jmprel, jmprelsz / sizeof(Elf32_Rel))) {
    g_libs.pop_back();
    ForgetTls(lib);
    munmap(map, lib->size);
    delete lib;
    return nullptr;
  }
  for (const std::string& u : lib->unresolved)
    lib->unresolved_c.push_back(u.c_str());
  ApplyAppPatches(lib);
  g_loads++;
  NoteGuestRange(lib->base, lib->base + lib->size);
  UpdateMapsLocked();
  ndk_flush_code(lib->base, lib->size);
  LOGI("loaded %s at 0x%x-0x%x (%zu unresolved%s)", path, lib->base, lib->base + lib->size,
       lib->unresolved.size(), lib->tls_size ? ", TLS" : "");
  return lib;
}

void Construct(Library* lib) {
  if (lib->constructed)
    return;
  lib->constructed = true;
  for (Library* dep : lib->deps)
    Construct(dep);
  // (argc, argv, envp), as bionic's linker calls them
  const uint32_t argv = GuestProcessArgv();
  const uint32_t args[3] = {1, argv, argv + 8};
  if (lib->init)
    CallGuest(lib->init, args, 3);
  for (uint32_t i = 0; i < lib->init_array_count; i++) {
    uint32_t f = Load32(lib->init_array + 4 * i);
    if (f != 0 && f != UINT32_MAX)
      CallGuest(f, args, 3);
  }
}

void Destruct(Library* lib) {
  for (uint32_t i = lib->fini_array_count; i-- > 0;) {
    uint32_t f = Load32(lib->fini_array + 4 * i);
    if (f != 0 && f != UINT32_MAX)
      CallGuest0(f);
  }
  if (lib->fini)
    CallGuest0(lib->fini);
}

// Finds or loads |path| (no constructors), counting a reference. A bare
// name is looked for in |near| (a directory with a trailing '/') first.
Library* Load(const char* path, const std::string& near) {
  if (!near.empty() && !strchr(path, '/')) {
    std::string full = near + path;
    if (Library* lib = Find(full.c_str())) {
      lib->refcount++;
      return lib;
    }
    if (access(full.c_str(), R_OK) == 0) {
      Library* lib = LoadFile(full.c_str());
      if (lib)
        lib->refcount = 1;
      return lib;
    }
  }
  if (Library* lib = Find(path)) {
    lib->refcount++;
    return lib;
  }
  Library* lib = nullptr;
  if (strchr(path, '/')) {
    lib = LoadFile(path);
  } else {
    std::vector<std::string> dirs = g_ld_library_path;
    dirs.insert(dirs.end(), g_search_dirs.begin(), g_search_dirs.end());
    for (const std::string& dir : dirs) {
      std::string full = dir + "/" + path;
      if (access(full.c_str(), R_OK) != 0)
        continue;
      lib = LoadFile(full.c_str());
      break;
    }
    if (!lib && !g_dlerror)
      SetError("dlopen failed: library \"%s\" not found", path);
  }
  if (lib)
    lib->refcount = 1;
  return lib;
}

const Library* Containing(uint32_t addr) {
  for (const Library* lib : g_libs) {
    if (addr - lib->base < lib->size)
      return lib;
  }
  return nullptr;
}

// The exported symbol of |lib| nearest below |addr|.
const Elf32_Sym* Nearest(const Library* lib, uint32_t addr) {
  const Elf32_Sym* best = nullptr;
  uint32_t n = lib->nsyms;
  if (n == 0 && lib->gnu_bucket) {
    // GNU hash only: the last chain entry with bit 0 set ends the table.
    for (uint32_t b = 0; b < lib->gnu_nbucket; b++)
      n = std::max(n, lib->gnu_bucket[b]);
    if (n >= lib->gnu_symoffset)
      while (!(lib->gnu_chain[n - lib->gnu_symoffset] & 1)) n++;
    n++;
  }
  for (uint32_t i = 1; i < n; i++) {
    const Elf32_Sym* s = &lib->symtab[i];
    if (s->st_shndx == SHN_UNDEF || ELF32_ST_TYPE(s->st_info) == STT_SECTION ||
        ELF32_ST_TYPE(s->st_info) == STT_TLS)
      continue;
    uint32_t a = SymAddr(lib, s) & ~1u;
    if (a <= addr && (!best || a > (SymAddr(lib, best) & ~1u)))
      best = s;
  }
  return best;
}

const char* SymbolizeImpl(uint32_t addr, uint32_t* offset) {
  static thread_local std::string buf;
  std::lock_guard<std::recursive_mutex> l(g_lock);
  if (const char* stub = HostStubName(addr)) {
    *offset = 0;
    buf = std::string("<host> ") + stub;
    return buf.c_str();
  }
  const Library* lib = Containing(addr);
  if (!lib)
    return nullptr;
  const Elf32_Sym* s = Nearest(lib, addr);
  if (s) {
    *offset = addr - (SymAddr(lib, s) & ~1u);
    buf = lib->name + "!" + (lib->strtab + s->st_name);
  } else {
    *offset = addr - lib->base;
    buf = lib->name;
  }
  return buf.c_str();
}

// DT_NEEDED and later bare-name dlopens also find siblings of a library
// loaded by path (bionic 4.4 searches only /vendor/lib and /system/lib, so
// apps load their dependencies first; this is the lenient superset).
void AddSearchDirOf(const char* path) {
  if (!strchr(path, '/') || BaseName(path) == path + 1)
    return;
  std::string dir(path, BaseName(path) - path - 1);
  for (const std::string& d : g_search_dirs) {
    if (d == dir)
      return;
  }
  g_search_dirs.insert(g_search_dirs.begin(), dir);
}

}  // namespace

bool IsLibrary(void* handle) {
  std::lock_guard<std::recursive_mutex> l(g_lock);
  for (Library* lib : g_libs) {
    if (lib == handle)
      return true;
  }
  for (auto& it : g_host_libs) {
    if (it.second == handle)
      return true;
  }
  return false;
}

namespace {
// Executable memory guest code made itself (mmap or mprotect with
// PROT_EXEC): JIT code, libffi closure trampolines (JNA's direct-mapped
// natives are such trampolines, registered with RegisterNatives), by start.
std::map<uint32_t, uint32_t> g_exec_regions;  // start -> end
}  // namespace

void NoteExecRegion(uint32_t lo, uint32_t hi) {
  if (hi <= lo)
    return;
  NoteGuestRange(lo, hi);
  std::lock_guard<std::recursive_mutex> l(g_lock);
  uint32_t& end = g_exec_regions[lo];
  end = end > hi ? end : hi;
}

void ForgetExecRegion(uint32_t lo, uint32_t hi) {
  std::lock_guard<std::recursive_mutex> l(g_lock);
  for (auto it = g_exec_regions.begin(); it != g_exec_regions.end();) {
    if (it->first >= lo && it->second <= hi)
      it = g_exec_regions.erase(it);
    else
      ++it;
  }
}

bool IsGuestCode(uint32_t addr) {
  if (!MaybeGuest(addr))
    return false;
  if (HostStubName(addr))
    return true;
  std::lock_guard<std::recursive_mutex> l(g_lock);
  if (Containing(addr) != nullptr)
    return true;
  auto it = g_exec_regions.upper_bound(addr);
  return it != g_exec_regions.begin() && addr < (--it)->second;
}

Library* Dlopen(const char* path, int flags) {
  MaybeWatchHostCalls();
  EnsureInit();
  std::lock_guard<std::recursive_mutex> l(g_lock);
  g_dlerror = nullptr;
  if (path == nullptr)
    return HostLib("<main>");
  if (IsHostLibrary(BaseName(path)))
    return HostLib(BaseName(path));
  AddSearchDirOf(path);
  Library* lib = Load(path);
  if (!lib) {
    LOGE("%s", g_dlerror);
    return nullptr;
  }
  Construct(lib);
  return lib;
}

void SetLibraryPath(const char* path) {
  std::lock_guard<std::recursive_mutex> l(g_lock);
  g_ld_library_path.clear();
  for (const char* p = path; p && *p;) {
    const char* colon = strchr(p, ':');
    size_t n = colon ? colon - p : strlen(p);
    if (n > 0)
      g_ld_library_path.push_back(std::string(p, n));
    p = colon ? colon + 1 : nullptr;
  }
}

Library* LoadNoInit(const char* path) {
  EnsureInit();
  std::lock_guard<std::recursive_mutex> l(g_lock);
  g_dlerror = nullptr;
  AddSearchDirOf(path);
  Library* lib = Load(path);
  if (!lib)
    LOGE("%s", g_dlerror);
  return lib;
}

uint32_t Dlsym(Library* lib, const char* name) {
  std::lock_guard<std::recursive_mutex> l(g_lock);
  g_dlerror = nullptr;
  uint32_t a = 0;
  bool global = lib == nullptr || (lib->is_host && lib->name == "<main>");
  if (global) {
    bool found;
    const int ns = arc_lproc_native_ns();
    for (Library* g : g_libs) {
      if (g->ns != ns)
        continue;
      if (const Elf32_Sym* s = FindExport(g, name))
        return SymAddr(g, s);
    }
    a = ResolveHostSymbol(name);
    (void)found;
  } else if (lib->is_host) {
    a = ResolveHostSymbol(name);
  } else if (const Elf32_Sym* s = FindExport(lib, name)) {
    a = SymAddr(lib, s);
  } else {
    for (Library* dep : lib->deps) {
      if (const Elf32_Sym* s = FindExport(dep, name))
        return SymAddr(dep, s);
    }
  }
  if (!a)
    SetError("undefined symbol: %s", name);
  return a;
}

int Dlclose(Library* lib) {
  std::lock_guard<std::recursive_mutex> l(g_lock);
  if (lib->is_host)
    return 0;
  if (--lib->refcount > 0)
    return 0;
  if (lib->constructed)
    Destruct(lib);
  for (size_t i = 0; i < g_libs.size(); i++) {
    if (g_libs[i] == lib) {
      g_libs.erase(g_libs.begin() + i);
      break;
    }
  }
  UpdateMapsLocked();
  ndk_flush_code(lib->base, lib->size);
  ForgetTls(lib);
  g_unloads++;
  munmap(Ptr<void>(lib->base), lib->size);
  std::vector<Library*> deps = lib->deps;
  delete lib;
  for (Library* dep : deps)
    Dlclose(dep);
  return 0;
}

const char* Dlerror() {
  const char* e = g_dlerror;
  g_dlerror = nullptr;
  return e;
}

uint32_t FindExidx(uint32_t pc, int* count) {
  std::lock_guard<std::recursive_mutex> l(g_lock);
  const Library* lib = Containing(pc);
  if (!lib || !lib->exidx) {
    *count = 0;
    return 0;
  }
  *count = lib->exidx_count;
  return lib->exidx;
}

void SetSkipMissingNeeded(bool skip) { g_skip_missing_needed = skip; }

size_t MissingNeededCount(Library* lib) { return lib->missing_needed.size(); }
const char* MissingNeededName(Library* lib, size_t i) { return lib->missing_needed[i].c_str(); }

const char* const* UnresolvedSymbols(Library* lib, size_t* n) {
  *n = lib->unresolved_c.size();
  return lib->unresolved_c.data();
}

const char* Symbolize(uint32_t addr, uint32_t* offset) { return SymbolizeImpl(addr, offset); }

void InstallSymbolizer() { ndk_set_symbolizer(SymbolizeImpl); }

namespace {
// debug.ndk.watch_pc: guest code addresses as "lib.so+0xVADDR" (the address
// objdump shows); the first 3 times a block holding one is decoded (first
// run, or after a flush), the thread's guest stack is reported.
struct WatchedPc {
  std::string lib;
  uint32_t vaddr;
  int reports = 0;
};
std::vector<WatchedPc>* g_watched_pcs;

// TraceNewBlocks: the blocks this thread decodes (first runs of code).
thread_local std::vector<uint32_t>* t_new_blocks;

void WatchPcHook(uint32_t start, uint32_t end) {
  if (t_new_blocks && t_new_blocks->size() < 100000)
    t_new_blocks->push_back(start);
  if (!g_watched_pcs)
    return;
  std::vector<std::string> hits;
  {
    std::lock_guard<std::recursive_mutex> l(g_lock);
    const Library* lib = Containing(start);
    if (!lib)
      return;
    for (WatchedPc& w : *g_watched_pcs) {
      uint32_t a = lib->bias + w.vaddr;
      if (w.lib == lib->name && a >= start && a < end && w.reports < 3) {
        w.reports++;
        char line[200];
        snprintf(line, sizeof line, "guest pc %s+0x%x reached (block 0x%x-0x%x)", w.lib.c_str(),
                 w.vaddr, start - lib->bias, end - lib->bias);
        hits.push_back(line);
      }
    }
  }
  for (const std::string& h : hits) ReportGuestStack(h.c_str());
}
}  // namespace

void TraceNewBlocks(bool on, const char* why, size_t last) {
  if (!ndk_cpu_set_block_hook)
    return;
  ndk_cpu_set_block_hook(WatchPcHook);
  if (on) {
    if (!t_new_blocks)
      t_new_blocks = new std::vector<uint32_t>;
    t_new_blocks->clear();
    return;
  }
  if (!t_new_blocks)
    return;
  size_t n = t_new_blocks->size(), from = n > last ? n - last : 0;
  if (n)
    __android_log_print(4, "ndk", "%s: %zu new blocks, the last %zu:", why, n, n - from);
  std::string out;
  for (size_t i = from; i < n; i++) {
    uint32_t a = (*t_new_blocks)[i];
    char item[80];
    {
      std::lock_guard<std::recursive_mutex> l(g_lock);
      const Library* lib = Containing(a);
      snprintf(item, sizeof item, " %s+0x%x", lib ? lib->name.c_str() : "?", lib ? a - lib->bias : a);
    }
    out += item;
    if ((i - from) % 8 == 7 || i + 1 == n) {  // (log lines are short)
      __android_log_print(4, "ndk", " %s", out.c_str());
      out.clear();
    }
  }
  t_new_blocks->clear();
}

void WatchGuestPcs(const char* list) {
  if (!ndk_cpu_set_block_hook || g_watched_pcs)
    return;
  g_watched_pcs = new std::vector<WatchedPc>;
  std::string all(list);
  for (size_t b = 0; b < all.size();) {
    size_t e = all.find(',', b);
    if (e == std::string::npos) e = all.size();
    std::string item = all.substr(b, e - b);
    size_t plus = item.find('+');
    if (plus != std::string::npos)
      g_watched_pcs->push_back({item.substr(0, plus),
                                (uint32_t)strtoul(item.c_str() + plus + 1, nullptr, 0)});
    b = e + 1;
  }
  ndk_cpu_set_block_hook(WatchPcHook);
}

namespace {
// Whether |a| (bit 0: Thumb) follows a call instruction in |lib|: a likely
// return address (BL/BLX imm, BLX reg).
bool AfterCall(uint32_t a, const Library* lib) {
  uint32_t pc = a & ~1u;
  if (pc < lib->base + 4 || pc > lib->base + lib->size)
    return false;
  if (a & 1) {
    uint16_t h1 = *Ptr<uint16_t>(pc - 4), h2 = *Ptr<uint16_t>(pc - 2);
    if ((h1 & 0xf800) == 0xf000 && (h2 & 0xc000) == 0xc000)   // BL / BLX imm
      return true;
    return (h2 & 0xff87) == 0x4780;                             // BLX reg
  }
  uint32_t w = *Ptr<uint32_t>(pc - 4);
  return (w & 0x0f000000) == 0x0b000000 || (w & 0xfe000000) == 0xfa000000 ||  // BL, BLX imm
         (w & 0x0ffffff0) == 0x012fff30;                                        // BLX reg
}
}  // namespace

void ReportGuestStack(const char* why) {
  NdkCpuState* s = ndk_current_state();
  uint32_t base = 0, size = 0;
  if (ndk_guest_stack)
    ndk_guest_stack(&base, &size);
  std::string out = std::string(why) + ": guest stack";
  // Mozilla's MOZ_CRASH reason (libmozglue's gMozCrashReason), if an app has one.
  {
    std::lock_guard<std::recursive_mutex> l(g_lock);
    for (Library* lib : g_libs) {
      uint32_t reason = lib->is_host ? 0 : Dlsym(lib, "gMozCrashReason");
      uint32_t text = reason ? *Ptr<uint32_t>(reason) : 0;
      if (text) {
        out += " (gMozCrashReason: ";
        out += std::string(Ptr<const char>(text), strnlen(Ptr<const char>(text), 300));
        out += ")";
        break;
      }
    }
  }
  auto add = [&](const char* what, uint32_t a, uint32_t at) {
    std::lock_guard<std::recursive_mutex> l(g_lock);
    const Library* lib = Containing(a & ~1u);
    if (!lib || !IsGuestCode(a & ~1u))
      return false;
    if (at && !AfterCall(a, lib))
      return false;
    char line[160];
    snprintf(line, sizeof(line), "\n  %s%s+0x%x", what, lib->name.c_str(), (a & ~1u) - lib->base);
    out += line;
    if (at) {
      snprintf(line, sizeof(line), " (at sp+0x%x)", at - s->r[13]);
      out += line;
    }
    return true;
  };
  add("pc ", s->r[15], 0);
  add("lr ", s->r[14], 0);
  int found = 0;
  for (uint32_t sp = s->r[13] & ~3u; sp >= base && sp + 4 <= base + size && found < 160; sp += 4) {
    if (add("", *Ptr<uint32_t>(sp), sp))
      ++found;
  }
  __android_log_write(6, "ndk", out.c_str());
  if (ndk_cpu_report_recent_calls)
    ndk_cpu_report_recent_calls();
}

// --- libdl for guest code -----------------------------------------------------------

namespace {

// bionic's Dl_info: the same four pointers as the host's.
struct GuestDlInfo {
  uint32_t dli_fname, dli_fbase, dli_sname, dli_saddr;
};

// bionic (Android 4.4, ARM): RTLD_DEFAULT ((void*) 0xffffffff),
// RTLD_NEXT ((void*) 0xfffffffe).
void* G_dlopen(const char* path, int flags) { return Dlopen(path, flags); }

uint32_t G_dlsym(void* handle, const char* name) {
  if (handle == reinterpret_cast<void*>(0xffffffff) ||
      handle == reinterpret_cast<void*>(0xfffffffe))
    return Dlsym(nullptr, name);
  if (!IsLibrary(handle)) {
    SetError("dlsym: invalid handle %p", handle);
    return 0;
  }
  return Dlsym(static_cast<Library*>(handle), name);
}

int G_dlclose(void* handle) {
  if (!IsLibrary(handle)) {
    SetError("dlclose: invalid handle %p", handle);
    return -1;
  }
  return Dlclose(static_cast<Library*>(handle));
}

const char* G_dlerror() { return Dlerror(); }

int G_dladdr(uint32_t addr, GuestDlInfo* info) {
  std::lock_guard<std::recursive_mutex> l(g_lock);
  const Library* lib = Containing(addr);
  if (!lib)
    return 0;
  info->dli_fname = Addr(lib->path.c_str());
  info->dli_fbase = lib->base;
  const Elf32_Sym* s = Nearest(lib, addr);
  info->dli_sname = s ? Addr(lib->strtab + s->st_name) : 0;
  info->dli_saddr = s ? SymAddr(lib, s) : 0;
  return 1;
}

uint32_t G_dl_unwind_find_exidx(uint32_t pc, int* count) { return FindExidx(pc, count); }

// dl_iterate_phdr: the loaded guest libraries, in load order, each with a
// bionic struct dl_phdr_info (addr, name, phdr, phnum; since API 30 also
// adds/subs and the TLS module with this thread's block). The callback is
// guest code and runs without the loader lock.
struct GuestPhdrInfo {
  uint32_t dlpi_addr, dlpi_name, dlpi_phdr;
  uint16_t dlpi_phnum, pad;
  uint64_t dlpi_adds, dlpi_subs;
  uint32_t dlpi_tls_modid, dlpi_tls_data;
};
static_assert(sizeof(GuestPhdrInfo) == 40, "bionic's 32-bit dl_phdr_info");
int G_dl_iterate_phdr(uint32_t callback, uint32_t data) {
  std::vector<GuestPhdrInfo> infos;
  {
    const uint32_t tp = CurrentState()->tls;
    std::lock_guard<std::recursive_mutex> l(g_lock);
    for (const Library* lib : g_libs) {
      if (lib->is_host || !lib->phdr)
        continue;
      GuestPhdrInfo i;
      memset(&i, 0, sizeof i);
      i.dlpi_addr = lib->bias;
      i.dlpi_name = Addr(lib->path.c_str());
      i.dlpi_phdr = lib->phdr;
      i.dlpi_phnum = static_cast<uint16_t>(lib->phnum);
      i.dlpi_adds = g_loads;
      i.dlpi_subs = g_unloads;
      i.dlpi_tls_modid = lib->tls_module;
      i.dlpi_tls_data = lib->tls_module ? tp + lib->tls_offset : 0;
      infos.push_back(i);
    }
  }
  for (GuestPhdrInfo& info : infos) {
    uint32_t args[3] = {Addr(&info), sizeof info, data};
    int32_t r = (int32_t)CallGuest(callback, args, 3);
    if (r != 0)
      return r;
  }
  return 0;
}

const HostSymbol kLibdl[] = {
    NDK_FN("dlopen", G_dlopen),
    NDK_FN("dlsym", G_dlsym),
    NDK_FN("dlclose", G_dlclose),
    NDK_FN("dlerror", G_dlerror),
    NDK_FN("dladdr", G_dladdr),
    NDK_FN("dl_unwind_find_exidx", G_dl_unwind_find_exidx),
    NDK_FN("dl_iterate_phdr", G_dl_iterate_phdr),
    // libc.so exports the same lookup under its libgcc name.
    NDK_FN("__gnu_Unwind_Find_exidx", G_dl_unwind_find_exidx),
};

}  // namespace

// bionic's libc exports __tls_get_addr on ARM (API 29+).
const HostSymbol kLibcTls[] = {
    NDK_RAW("__tls_get_addr", TlsGetAddr),
};

void RegisterLibdl() {
  RegisterHostSymbols("libdl.so", kLibdl);
  RegisterHostSymbols("libc.so", kLibcTls);
}

// A thread's static TLS area (ndk_core.cc, with |tp| = its TPIDRURO): the
// blocks of the libraries loaded so far.
void InitThreadTls(uint32_t tp) {
  std::lock_guard<std::mutex> l(g_tls_lock);
  if (!g_tls_threads)
    g_tls_threads = new std::vector<uint32_t>;
  g_tls_threads->push_back(tp);
  for (const TlsImage& t : g_tls_images)
    InitTlsBlock(tp, t);
}

// The area goes (a green thread's, ndk_green.cc): libraries loaded later no
// longer initialize their blocks in it.
void ForgetThreadTls(uint32_t tp) {
  std::lock_guard<std::mutex> l(g_tls_lock);
  if (!g_tls_threads) return;
  for (size_t i = 0; i < g_tls_threads->size(); ++i) {
    if ((*g_tls_threads)[i] == tp) {
      g_tls_threads->erase(g_tls_threads->begin() + i);
      return;
    }
  }
}

}  // namespace ndk

// /proc/self/maps of the guest (shim/app6/proc_self.cc asks for it): copies
// up to |size| bytes, returns the full length.
extern "C" size_t arc_proc_maps(char* buf, size_t size) {
  std::lock_guard<std::mutex> l(ndk::g_maps_lock);
  std::string both;
  if (ndk::Maps64)   // the arm64 libraries (ndk_linker64.cc), in builds that have them
    both = *ndk::g_maps + ndk::Maps64();
  const std::string& s = ndk::Maps64 ? both : *ndk::g_maps;
  if (buf && size)
    memcpy(buf, s.data(), s.size() < size ? s.size() : size);
  return s.size();
}
