// Native programs of apps (topic native-exec, docs/topics/native-exec.md):
// an ARM or arm64 executable an app runs (ProcessBuilder / Runtime.exec, or
// arc_spawn) becomes a spawned light process of the VM
// (shim/multiproc/include/arc_lproc.h) whose program runs on a thread of its
// own, translated by the bridge's CPUs like the app's libraries:
//   * dynamic executables (PIE with PT_INTERP: what the NDK builds) load
//     through the bridge's linkers as a library is, in the process's own
//     namespace (its own copy of its libraries' statics), their imports
//     bound to the bridge's bionic;
//   * static ones (static-pie: no PT_INTERP, no DT_NEEDED) are copied into
//     memory as they are and relocate themselves; their raw syscalls are the
//     bridge's (ndk_syscall.cc, ndk64_libc.cc).
// Either way the entry runs with a Linux initial stack (argc, argv, envp,
// auxv). The process has descriptors 0-2 of its own (posix_translation:
// FdToFileStreamMap's process view; pipes to the app), its own working
// directory, environment (getenv) and stdio FILEs; exit(), _exit(), abort()
// and the exit syscalls end it with a status its parent's waitpid() gets
// (Process.waitFor). Not here: fork() (one address space), executables
// linked at a fixed address (ET_EXEC: the VM owns low memory), threads of
// static programs (clone). Part of the arc-wasm shim layer.

#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "../multiproc/include/arc_lproc.h"
#include "ndk_bridge.h"
#include "ndk_cpu64.h"

extern "C" int __android_log_print(int prio, const char* tag, const char* fmt, ...);
extern "C" void ndk64_set_thread_guest_stack_size(size_t bytes) __attribute__((weak));
// shim/kernel/arc_kernel.cc: the process's descriptors 0-2 and directory.
extern "C" int arc_kernel_process_setup(int pid, const int fds[3], const char* dir);
extern "C" void arc_kernel_process_release(int pid);

#define LOGI(...) __android_log_print(4, "exec", __VA_ARGS__)
#define LOGW(...) __android_log_print(5, "exec", __VA_ARGS__)

#ifndef EM_AARCH64
#define EM_AARCH64 183
#endif

namespace ndk {

uint64_t ExecAuxval64(uint64_t type);  // ndk64_libc.cc
uint32_t ExecAuxval32(uint32_t type);  // ndk_bionic.cc

namespace {

const size_t kGuestStack = 8 << 20;  // a Linux main thread's 8 MB
const int kMaxScriptDepth = 4;

// --- binfmt_misc (topic native-exec) --------------------------------------
// Linux's /proc/sys/fs/binfmt_misc: a program that execve's a file of another
// binary format is run through a registered interpreter (qemu-user for a guest
// ARM binary, say). Rules are registered the Linux way (the VFS files in
// arc_kernel.cc call the C API below); general, no per-package special cases.
struct BinfmtRule {
  std::string name;
  bool enabled = true;
  bool extension = false;    // 'E': match the filename suffix; 'M': match magic
  int offset = 0;
  std::string magic;         // bytes to match at |offset| (M), or the suffix (E)
  std::string mask;          // same length as magic (M); empty = every byte 0xff
  std::string interpreter;
  bool pres_argv0 = false;   // flag P
  bool credentials = false;  // flag C (accepted; we always run as the VM)
  bool fix_binary = false;   // flag F (accepted; the interpreter is read at exec)
};
std::mutex g_binfmt_lock;
std::vector<BinfmtRule>* g_binfmt = new std::vector<BinfmtRule>;
bool g_binfmt_enabled = true;

std::string BinfmtUnescape(const std::string& s) {
  auto hex = [](char c) -> int {
    return c >= '0' && c <= '9' ? c - '0'
         : c >= 'a' && c <= 'f' ? c - 'a' + 10
         : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
  };
  std::string out;
  for (size_t i = 0; i < s.size();) {
    if (s[i] == '\\' && i + 3 < s.size() + 1 && i + 3 < s.size() && s[i + 1] == 'x') {
      int h = hex(s[i + 2]), l = hex(s[i + 3]);
      if (h >= 0 && l >= 0) { out.push_back(static_cast<char>(h * 16 + l)); i += 4; continue; }
    }
    if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == '\\') { out.push_back('\\'); i += 2; continue; }
    out.push_back(s[i++]);
  }
  return out;
}

// Registers a rule from a Linux register line ":name:type:offset:magic:mask:interp:flags"
// (the first byte is the field delimiter). 0, or -errno.
int BinfmtRegister(const std::string& line0) {
  std::string line = line0;
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
  if (line.size() < 2) { errno = EINVAL; return -EINVAL; }
  const char d = line[0];
  std::vector<std::string> f;
  size_t start = 1;
  for (size_t i = 1; i <= line.size(); i++) {
    if (f.size() == 6) { f.push_back(line.substr(start)); break; }  // flags: the rest
    if (i == line.size() || line[i] == d) { f.push_back(line.substr(start, i - start)); start = i + 1; }
  }
  if (f.size() < 7) { errno = EINVAL; return -EINVAL; }
  BinfmtRule r;
  r.name = f[0];
  if (f[1] == "E") r.extension = true;
  else if (f[1] == "M") r.extension = false;
  else { errno = EINVAL; return -EINVAL; }
  r.offset = f[2].empty() ? 0 : atoi(f[2].c_str());
  r.magic = r.extension ? f[3] : BinfmtUnescape(f[3]);
  r.mask = r.extension ? std::string() : BinfmtUnescape(f[4]);
  r.interpreter = f[5];
  for (char c : f[6]) {
    if (c == 'P') r.pres_argv0 = true;
    else if (c == 'C') r.credentials = true;
    else if (c == 'F') r.fix_binary = true;
    // 'O' (keep the binary open as an fd) is accepted but we pass the path.
  }
  if (r.name.empty() || r.name[0] == '-' || r.interpreter.empty() || r.magic.empty() ||
      (!r.mask.empty() && r.mask.size() != r.magic.size()) || r.offset < 0) {
    errno = EINVAL;
    return -EINVAL;
  }
  std::lock_guard<std::mutex> l(g_binfmt_lock);
  for (const BinfmtRule& e : *g_binfmt)
    if (e.name == r.name) { errno = EEXIST; return -EEXIST; }
  g_binfmt->push_back(r);
  LOGI("binfmt_misc: registered \"%s\" -> %s (%s%s)", r.name.c_str(), r.interpreter.c_str(),
       r.extension ? "suffix ." : "magic", r.extension ? r.magic.c_str() : "");
  return 0;
}

bool BinfmtMatch(const uint8_t* head, size_t n, const std::string& path, BinfmtRule* out) {
  std::lock_guard<std::mutex> l(g_binfmt_lock);
  if (!g_binfmt_enabled) return false;
  for (const BinfmtRule& r : *g_binfmt) {
    if (!r.enabled) continue;
    if (r.extension) {
      const std::string suf = "." + r.magic;
      if (path.size() >= suf.size() && path.compare(path.size() - suf.size(), suf.size(), suf) == 0) {
        *out = r;
        return true;
      }
    } else if (static_cast<size_t>(r.offset) + r.magic.size() <= n) {
      bool ok = true;
      for (size_t i = 0; i < r.magic.size(); i++) {
        const uint8_t m = r.mask.empty() ? 0xff : static_cast<uint8_t>(r.mask[i]);
        if (((head[r.offset + i] ^ static_cast<uint8_t>(r.magic[i])) & m) != 0) { ok = false; break; }
      }
      if (ok) { *out = r; return true; }
    }
  }
  return false;
}

// What the file is, from its headers.
struct Image {
  bool lp64 = false;
  bool dynamic = false;  // PT_INTERP or DT_NEEDED: through the linker
  uint64_t entry = 0;    // e_entry (link address)
  uint64_t phoff = 0;
  uint32_t phnum = 0;
  std::vector<uint8_t> phdrs;  // raw program headers
};

struct ExecProcess {
  int pid = 0;
  int uid = 0;
  bool lp64 = false;
  std::string path;
  std::vector<std::string> argv, envp;
  // Filesystem root for exec (qemu's -L / QEMU_LD_PREFIX): where this process's
  // guest files live. Inherited by what it spawns or execs, so an absolute
  // program path that is not in our VFS but exists under the root runs from
  // under the root (topic native-exec). Empty for an ordinary process.
  std::string exec_root;
  pthread_t thread{};
  std::mutex lock;
  FILE* std[3] = {nullptr, nullptr, nullptr};
  std::atomic<bool> has_status{false};
  int status = 0;
  std::atomic<bool> finished{false};
  // vfork (topic native-exec): the parent waits on this until the child
  // execve's or _exit's; non-null only for a vfork/fork child.
  struct Vfork { std::mutex m; std::condition_variable cv; bool done = false; };
  Vfork* vfork = nullptr;
};

std::mutex g_lock;
std::map<int, ExecProcess*>* g_procs = new std::map<int, ExecProcess*>;
std::atomic<bool> g_any{false};
thread_local int t_cached_pid = -1;
thread_local ExecProcess* t_cached = nullptr;

ExecProcess* Find(int pid) {
  std::lock_guard<std::mutex> l(g_lock);
  auto it = g_procs->find(pid);
  return it == g_procs->end() ? nullptr : it->second;
}

ExecProcess* Current() {
  if (!g_any.load(std::memory_order_relaxed))
    return nullptr;
  const int pid = arc_lproc_current();
  if (pid != t_cached_pid) {
    t_cached = Find(pid);
    t_cached_pid = pid;
  }
  return t_cached;
}

// vfork (topic native-exec): the calling thread is a vfork child that has not
// yet execve'd/_exit'd; releasing wakes its suspended parent.
thread_local ExecProcess* t_vfork_pending = nullptr;
void ReleaseVfork(ExecProcess* p) {
  if (!p || !p->vfork) return;
  {
    std::lock_guard<std::mutex> l(p->vfork->m);
    p->vfork->done = true;
  }
  p->vfork->cv.notify_all();
}

bool ReadAt(int fd, void* buf, size_t n, off_t off) {
  uint8_t* p = static_cast<uint8_t*>(buf);
  while (n) {
    ssize_t r = pread(fd, p, n, off);
    if (r <= 0) return false;
    p += r;
    n -= r;
    off += r;
  }
  return true;
}

// Reads the headers of |path|: 0, or an errno (ENOEXEC: not one we run;
// |script| gets the "#!" line of a script).
int ReadImage(const char* path, Image* img, std::string* script) {
  struct stat st;
  if (stat(path, &st) != 0) return errno;
  if (S_ISDIR(st.st_mode)) return EACCES;
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return errno;
  uint8_t head[256];
  ssize_t n = pread(fd, head, sizeof head, 0);
  if (n >= 2 && head[0] == '#' && head[1] == '!') {
    const uint8_t* nl = static_cast<const uint8_t*>(memchr(head, '\n', n));
    script->assign(reinterpret_cast<const char*>(head + 2),
                   nl ? nl - head - 2 : n - 2);
    close(fd);
    return 0;
  }
  if (n < 52 || memcmp(head, ELFMAG, SELFMAG) != 0) {
    close(fd);
    return ENOEXEC;
  }
  int err = 0;
  if (head[EI_CLASS] == ELFCLASS64) {
    Elf64_Ehdr eh;
    memcpy(&eh, head, sizeof eh);
    img->lp64 = true;
    img->entry = eh.e_entry;
    img->phoff = eh.e_phoff;
    img->phnum = eh.e_phnum;
    if (eh.e_machine != EM_AARCH64 || eh.e_phentsize != sizeof(Elf64_Phdr)) err = ENOEXEC;
    if (eh.e_type == ET_EXEC) {
      LOGW("%s: linked at a fixed address (ET_EXEC), not position-independent: not run", path);
      err = ENOEXEC;
    } else if (eh.e_type != ET_DYN) {
      err = ENOEXEC;
    }
    if (!err && !HaveCpu64()) err = ENOEXEC;
    if (!err) {
      img->phdrs.resize(sizeof(Elf64_Phdr) * eh.e_phnum);
      if (!ReadAt(fd, img->phdrs.data(), img->phdrs.size(), eh.e_phoff)) err = ENOEXEC;
    }
    for (uint32_t i = 0; !err && i < eh.e_phnum; i++) {
      const Elf64_Phdr* ph = reinterpret_cast<const Elf64_Phdr*>(img->phdrs.data()) + i;
      if (ph->p_type == PT_INTERP) img->dynamic = true;
      if (ph->p_type == PT_DYNAMIC && !img->dynamic) {
        std::vector<Elf64_Dyn> dyn(ph->p_filesz / sizeof(Elf64_Dyn));
        if (!dyn.empty() && ReadAt(fd, dyn.data(), dyn.size() * sizeof(Elf64_Dyn), ph->p_offset))
          for (const Elf64_Dyn& d : dyn)
            if (d.d_tag == DT_NEEDED) img->dynamic = true;
      }
    }
  } else if (head[EI_CLASS] == ELFCLASS32) {
    Elf32_Ehdr eh;
    memcpy(&eh, head, sizeof eh);
    img->entry = eh.e_entry;
    img->phoff = eh.e_phoff;
    img->phnum = eh.e_phnum;
    if (eh.e_machine != EM_ARM || eh.e_phentsize != sizeof(Elf32_Phdr)) err = ENOEXEC;
    if (eh.e_type == ET_EXEC) {
      LOGW("%s: linked at a fixed address (ET_EXEC), not position-independent: not run", path);
      err = ENOEXEC;
    } else if (eh.e_type != ET_DYN) {
      err = ENOEXEC;
    }
    if (!err) {
      img->phdrs.resize(sizeof(Elf32_Phdr) * eh.e_phnum);
      if (!ReadAt(fd, img->phdrs.data(), img->phdrs.size(), eh.e_phoff)) err = ENOEXEC;
    }
    for (uint32_t i = 0; !err && i < eh.e_phnum; i++) {
      const Elf32_Phdr* ph = reinterpret_cast<const Elf32_Phdr*>(img->phdrs.data()) + i;
      if (ph->p_type == PT_INTERP) img->dynamic = true;
      if (ph->p_type == PT_DYNAMIC && !img->dynamic) {
        std::vector<Elf32_Dyn> dyn(ph->p_filesz / sizeof(Elf32_Dyn));
        if (!dyn.empty() && ReadAt(fd, dyn.data(), dyn.size() * sizeof(Elf32_Dyn), ph->p_offset))
          for (const Elf32_Dyn& d : dyn)
            if (d.d_tag == DT_NEEDED) img->dynamic = true;
      }
    }
  } else {
    err = ENOEXEC;
  }
  close(fd);
  return err;
}

// A static program: its PT_LOAD segments copied into fresh memory as they
// lie in the file (the program relocates itself). Returns the load bias, or
// 0 on failure.
uint32_t LoadStatic(const char* path, const Image& img, uint32_t* phdr_addr) {
  uint64_t lo = ~0ull, hi = 0;
  auto each = [&](auto fn) {
    for (uint32_t i = 0; i < img.phnum; i++) {
      if (img.lp64) {
        const Elf64_Phdr* p = reinterpret_cast<const Elf64_Phdr*>(img.phdrs.data()) + i;
        fn(p->p_type, p->p_vaddr, p->p_memsz, p->p_offset, p->p_filesz, p->p_flags);
      } else {
        const Elf32_Phdr* p = reinterpret_cast<const Elf32_Phdr*>(img.phdrs.data()) + i;
        fn(p->p_type, p->p_vaddr, p->p_memsz, p->p_offset, p->p_filesz, p->p_flags);
      }
    }
  };
  each([&](uint32_t type, uint64_t va, uint64_t memsz, uint64_t, uint64_t, uint32_t) {
    if (type != PT_LOAD) return;
    lo = std::min<uint64_t>(lo, va & ~4095ull);
    hi = std::max<uint64_t>(hi, va + memsz);
  });
  if (lo >= hi || hi - lo > (1ull << 30)) return 0;
  const size_t size = static_cast<size_t>(hi - lo);
  void* mem = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) return 0;
  const uint32_t bias = Addr(mem) - static_cast<uint32_t>(lo);
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  bool ok = fd >= 0;
  *phdr_addr = 0;
  each([&](uint32_t type, uint64_t va, uint64_t memsz, uint64_t off, uint64_t filesz, uint32_t flags) {
    if (type == PT_PHDR) *phdr_addr = bias + static_cast<uint32_t>(va);
    if (type != PT_LOAD || !ok) return;
    if (filesz && !ReadAt(fd, Ptr<void>(bias + static_cast<uint32_t>(va)), filesz, off)) ok = false;
    // (the rest up to memsz: fresh anonymous memory is zero)
    if (img.phoff >= off && img.phoff < off + filesz && !*phdr_addr)
      *phdr_addr = bias + static_cast<uint32_t>(va + (img.phoff - off));
    if (flags & PF_X) {
      const uint32_t s = bias + static_cast<uint32_t>(va), e = s + static_cast<uint32_t>(memsz);
      FlushGuestCode(s, e - s);
      if (img.lp64 && NoteExecRegion64) NoteExecRegion64(s, e);
      if (!img.lp64) NoteExecRegion(s, e);
    }
  });
  if (fd >= 0) close(fd);
  if (!ok) {
    munmap(mem, size);
    return 0;
  }
  return bias;
}

// The guest's argv/envp/auxv strings and the initial stack's words.
struct StartBlock {
  std::vector<char> strings;
  std::vector<uint64_t> words;  // as 64-bit or 32-bit slots
};

void BuildStart(ExecProcess* p, uint32_t phdr, uint32_t phnum, uint32_t entry, StartBlock* b) {
  // Strings first (their guest addresses: the block is guest memory).
  std::vector<size_t> argv_off, envp_off;
  auto add = [&](const std::string& s) {
    size_t at = b->strings.size();
    b->strings.insert(b->strings.end(), s.begin(), s.end());
    b->strings.push_back('\0');
    return at;
  };
  for (const std::string& a : p->argv) argv_off.push_back(add(a));
  for (const std::string& e : p->envp) envp_off.push_back(add(e));
  const size_t execfn_off = add(p->path);
  const size_t platform_off = add(p->lp64 ? "aarch64" : "v7l");
  while (b->strings.size() % 16) b->strings.push_back('\0');
  const size_t random_off = b->strings.size();
  for (int i = 0; i < 16; i++) b->strings.push_back(static_cast<char>(rand()));
  // (strings stay put: |b| outlives the process's main thread)
  const uint32_t base = Addr(b->strings.data());
  std::vector<uint64_t>& w = b->words;
  w.push_back(p->argv.size());
  for (size_t o : argv_off) w.push_back(base + o);
  w.push_back(0);
  for (size_t o : envp_off) w.push_back(base + o);
  w.push_back(0);
  auto aux = [&](uint64_t type, uint64_t value) {
    w.push_back(type);
    w.push_back(value);
  };
  aux(AT_PHDR, phdr);
  aux(AT_PHENT, p->lp64 ? sizeof(Elf64_Phdr) : sizeof(Elf32_Phdr));
  aux(AT_PHNUM, phnum);
  aux(AT_PAGESZ, 4096);
  aux(AT_BASE, 0);
  aux(AT_FLAGS, 0);
  aux(AT_ENTRY, entry);
  aux(AT_UID, p->uid);
  aux(AT_EUID, p->uid);
  aux(AT_GID, p->uid);
  aux(AT_EGID, p->uid);
  aux(AT_PLATFORM, base + platform_off);
  aux(AT_HWCAP, p->lp64 ? ExecAuxval64(AT_HWCAP) : ExecAuxval32(AT_HWCAP));
  aux(AT_HWCAP2, p->lp64 ? ExecAuxval64(AT_HWCAP2) : ExecAuxval32(AT_HWCAP2));
  aux(AT_CLKTCK, 100);
  aux(AT_SECURE, 0);
  aux(AT_RANDOM, base + random_off);
  aux(AT_EXECFN, base + execfn_off);
  aux(AT_NULL, 0);
}

void WriteStderr(const std::string& text) {
  // (descriptor 2 is the process's own)
  ssize_t ignored = write(2, text.data(), text.size());
  (void)ignored;
}

// Loads and runs the program on this thread; its wait status.
int Run(ExecProcess* p, const Image& img) {
  uint32_t bias = 0, phdr = 0, phnum = img.phnum;
  if (img.lp64 && ndk64_set_thread_guest_stack_size) ndk64_set_thread_guest_stack_size(kGuestStack);
  if (img.dynamic) {
    // Through the linker, as a library: in this process's namespace.
    bool ok;
    if (img.lp64) {
      Library64* lib = Dlopen64 ? Dlopen64(p->path.c_str(), 2 /* RTLD_NOW */) : nullptr;
      ok = lib && LibraryImage64 && LibraryImage64(lib, &bias, &phdr, &phnum);
      if (!lib)
        WriteStderr("CANNOT LINK EXECUTABLE \"" + p->path + "\": " +
                    (Dlerror64 && Dlerror64() ? Dlerror64() : "load failed") + "\n");
    } else {
      Library* lib = Dlopen(p->path.c_str(), 2);
      ok = lib && LibraryImage(lib, &bias, &phdr, &phnum);
      if (!lib)
        WriteStderr("CANNOT LINK EXECUTABLE \"" + p->path + "\": " +
                    (Dlerror() ? Dlerror() : "load failed") + "\n");
    }
    if (!ok) return 1 << 8;
  } else {
    bias = LoadStatic(p->path.c_str(), img, &phdr);
    if (!bias) {
      WriteStderr(p->path + ": cannot load the program\n");
      return 1 << 8;
    }
  }
  const uint32_t entry = bias + static_cast<uint32_t>(img.entry);
  StartBlock* start = new StartBlock;  // kept: the program's argv lives there
  BuildStart(p, phdr, phnum, entry, start);
  LOGI("pid %d: %s (%s, %s) entry 0x%x, %zu args", p->pid, p->path.c_str(), img.lp64 ? "arm64" : "arm",
       img.dynamic ? "dynamic" : "static", entry, p->argv.size());
  char why[256] = "";
  bool returned;
  uint64_t result = 0;
  if (img.lp64) {
    Ndk64Args a;
    memset(&a, 0, sizeof a);
    a.stack = start->words.data();
    a.nstack = static_cast<int>(start->words.size());
    Ndk64Result r;
    returned = CallGuest64Guarded(entry, &a, &r, why, sizeof why);
    result = r.x0;
  } else {
    std::vector<uint32_t> args(4, 0);
    for (uint64_t v : start->words) args.push_back(static_cast<uint32_t>(v));
    returned = CallGuestGuarded(entry, args.data(), static_cast<int>(args.size()), &result, why, sizeof why);
  }
  if (p->has_status.load()) return p->status;
  if (returned) return (static_cast<int>(result) & 0xff) << 8;  // an entry that returned
  LOGW("pid %d: %s ended by a fault: %s", p->pid, p->path.c_str(), why);
  WriteStderr(std::string("Segmentation fault (") + why + ")\n");
  return SIGSEGV;
}

// The process is over: its stdio flushed, descriptors 0-2 let go (the app
// reads EOF), what it left open closed; then its parent's waitpid returns.
void Finish(ExecProcess* p, int status, bool own_thread) {
  if (p->finished.exchange(true)) return;
  {
    std::lock_guard<std::mutex> l(p->lock);
    for (int i = 0; i < 3; i++) {
      if (!p->std[i]) continue;
      fflush(p->std[i]);
      if (own_thread) fclose(p->std[i]);  // (another thread may still use it)
      p->std[i] = nullptr;
    }
  }
  arc_kernel_process_release(p->pid);
  arc_lproc_exited(p->pid, status);
}

struct StartArgs {
  ExecProcess* p;
  Image img;
};

void* ProgramMain(void* arg) {
  StartArgs* s = static_cast<StartArgs*>(arg);
  ExecProcess* p = s->p;
  arc_lproc_enter(p->pid);
  const int status = Run(p, s->img);
  delete s;
  Finish(p, status, true);
  return nullptr;
}

// exit(), _exit(), abort() in a spawned process (light_process_wrap.cc).
void ExitHook(int pid, int wstatus) {
  ExecProcess* p = Find(pid);
  if (!p) return;
  if (p->vfork) {  // a vfork child exiting before exec: wake the parent
    ReleaseVfork(p);
    t_vfork_pending = nullptr;
  }
  {
    std::lock_guard<std::mutex> l(p->lock);
    if (!p->has_status.load()) {
      p->status = wstatus;
      p->has_status.store(true);
    }
  }
  if (pthread_equal(pthread_self(), p->thread)) {
    LeaveGuest("exit");  // back to Run(), which returns the status
  } else {
    // Another thread of it: the process ends now for its parent; this
    // thread goes (the main thread cannot be stopped from here).
    Finish(p, p->status, false);
  }
  pthread_exit(nullptr);
}

void KillHook(int pid) {
  // A spawned process killed (Process.destroy()): it is over for its parent
  // (light_process.cc keeps the signal as its status); its descriptors go
  // so that the app's reads end. Its threads stay until they leave.
  if (ExecProcess* p = Find(pid)) {
    if (p->vfork) ReleaseVfork(p);  // a killed vfork child: free its parent
    if (!pthread_equal(pthread_self(), p->thread))
      Finish(p, SIGKILL, false);
  }
}

std::string BaseNameOf(const std::string& path) {
  size_t slash = path.rfind('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

// The exec filesystem root a process's env names, if any. QEMU_LD_PREFIX is
// qemu-user's own variable for -L (the directory that stands in for the guest
// "/"); any emulator that sets it gets this. General: no knowledge of a guest
// or a package, just a well-known env var.
std::string PrefixFromEnv(const std::vector<std::string>& env) {
  static const char kVar[] = "QEMU_LD_PREFIX=";
  for (const std::string& e : env)
    if (e.rfind(kVar, 0) == 0) return e.substr(sizeof kVar - 1);
  return std::string();
}

// qemu-user also names the guest root on the command line, as `-L DIR` or
// `-LDIR`; recognise it so a process started that way carries the root even
// without QEMU_LD_PREFIX in its env. Still a general convention (no package
// knowledge), the counterpart of PrefixFromEnv.
std::string PrefixFromArgv(const std::vector<std::string>& argv) {
  for (size_t i = 1; i < argv.size(); i++) {
    const std::string& a = argv[i];
    if (a == "-L" && i + 1 < argv.size()) return argv[i + 1];
    if (a.size() > 2 && a.compare(0, 2, "-L") == 0) return a.substr(2);
  }
  return std::string();
}

// Carries the exec root into a child's env as QEMU_LD_PREFIX, so an emulator
// re-run through binfmt_misc (whose rule names only the interpreter, no `-L`)
// still finds the guest tree the standard way. A no-op when the env already
// names it or there is no root.
void EnsurePrefixEnv(std::vector<std::string>* env, const std::string& root) {
  if (root.empty()) return;
  for (const std::string& e : *env)
    if (e.rfind("QEMU_LD_PREFIX=", 0) == 0) return;
  env->push_back("QEMU_LD_PREFIX=" + root);
}

// One-time: binfmt rules from the ARC_BINFMT env, newline-separated Linux
// register lines (":name:M:offset:magic:mask:interp:flags"). Lets a rule be
// set without writing /proc — the general ?binfmt= shell param sets this env.
// Runs on the first exec, when the VM's env is already in place.
void MaybeRegisterEnvBinfmt() {
  static std::once_flag once;
  std::call_once(once, [] {
    const char* v = getenv("ARC_BINFMT");
    if (!v || !*v) return;
    const std::string s(v);
    size_t at = 0;
    while (at < s.size()) {
      size_t nl = s.find('\n', at);
      if (nl == std::string::npos) nl = s.size();
      std::string line = s.substr(at, nl - at);
      at = nl + 1;
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
      if (line.empty()) continue;
      const int r = BinfmtRegister(line);
      if (r < 0) LOGI("binfmt_misc: ARC_BINFMT rule rejected (%d): %s", -r, line.c_str());
    }
  });
}

// Resolves an absolute exec |path| against a process's exec |root|: if the raw
// path is not in our VFS but |root| + |path| is, returns the rooted path (what
// qemu's -L does for a guest binary). A relative path, an empty root, or a raw
// path that already exists is returned unchanged, so an ordinary process (no
// root) is never affected.
std::string UnderRoot(const std::string& root, const std::string& path) {
  if (root.empty() || path.empty() || path[0] != '/') return path;
  struct stat st;
  if (stat(path.c_str(), &st) == 0) return path;
  std::string rooted = root;
  while (rooted.size() > 1 && rooted.back() == '/') rooted.pop_back();
  rooted += path;
  if (stat(rooted.c_str(), &st) == 0) return rooted;
  return path;  // neither exists: ReadImage reports ENOENT on the raw path
}

// PATH lookup of a name without a slash, as execvp does.
std::string Which(const std::string& name, const std::vector<std::string>& envp) {
  if (name.find('/') != std::string::npos) return name;
  std::string path;
  for (const std::string& e : envp)
    if (e.compare(0, 5, "PATH=") == 0) path = e.substr(5);
  if (path.empty() && getenv("PATH")) path = getenv("PATH");
  if (path.empty()) path = "/system/bin:/system/xbin";
  size_t at = 0;
  while (at <= path.size()) {
    size_t colon = path.find(':', at);
    if (colon == std::string::npos) colon = path.size();
    std::string dir = path.substr(at, colon - at);
    std::string full = (dir.empty() ? "." : dir) + "/" + name;
    struct stat st;
    if (stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode)) return full;
    at = colon + 1;
  }
  return std::string();
}

// What runs for |path| with |argv|: a script's interpreter ("#!interpreter
// [one argument]", the script then its argument) in their place, the
// program's headers in |img|. 0, or what exec says (ENOENT, ENOEXEC...).
int ResolveProgram(std::string* path, std::vector<std::string>* argv, Image* img) {
  for (int depth = 0;; depth++) {
    // binfmt_misc: a file of a registered format runs through its interpreter
    // (Linux's /proc/sys/fs/binfmt_misc; e.g. qemu-user for a guest ARM
    // binary). The interpreter itself, read next time round, matches nothing
    // (a different format) and runs; the depth guard stops a bad loop.
    if (g_binfmt_enabled && depth < kMaxScriptDepth) {
      uint8_t head[256];
      const int bfd = open(path->c_str(), O_RDONLY | O_CLOEXEC);
      if (bfd >= 0) {
        const ssize_t hn = pread(bfd, head, sizeof head, 0);
        close(bfd);
        BinfmtRule r;
        if (hn > 0 && BinfmtMatch(head, static_cast<size_t>(hn), *path, &r)) {
          std::vector<std::string> nargv;
          nargv.push_back(r.interpreter);
          if (r.pres_argv0) nargv.push_back(argv->empty() ? *path : (*argv)[0]);
          nargv.push_back(*path);
          nargv.insert(nargv.end(), argv->begin() + (argv->empty() ? 0 : 1), argv->end());
          *argv = nargv;
          *path = r.interpreter;
          continue;
        }
      }
    }
    std::string script;
    *img = Image();
    const int err = ReadImage(path->c_str(), img, &script);
    if (err) return err;
    if (script.empty()) return 0;
    if (depth >= kMaxScriptDepth) return ELOOP;
    size_t a = script.find_first_not_of(" \t");
    if (a == std::string::npos) return ENOEXEC;
    size_t b = script.find_first_of(" \t", a);
    std::string interp = script.substr(a, b == std::string::npos ? std::string::npos : b - a);
    std::string opt;
    if (b != std::string::npos) {
      size_t c = script.find_first_not_of(" \t", b);
      if (c != std::string::npos) {
        opt = script.substr(c);
        while (!opt.empty() && (opt.back() == ' ' || opt.back() == '\t' || opt.back() == '\r')) opt.pop_back();
      }
    }
    std::vector<std::string> nargv{interp};
    if (!opt.empty()) nargv.push_back(opt);
    nargv.push_back(*path);
    nargv.insert(nargv.end(), argv->begin() + (argv->empty() ? 0 : 1), argv->end());
    *argv = nargv;
    *path = interp;
  }
}

// Diagnostics (ARC_SPAWN_LOG=1): the env vars that matter for guest init.
void LogSpawnEnv(const char* how, int pid, const std::vector<std::string>& argv,
                 const std::vector<std::string>& env, const int* fds) {
  static const bool on = getenv("ARC_SPAWN_LOG") && getenv("ARC_SPAWN_LOG")[0] == '1';
  if (!on) return;
  std::string a;
  for (size_t i = 0; i < argv.size() && i < 8; i++) a += (i ? " " : "") + argv[i];
  std::string socks;
  for (const std::string& e : env)
    if (e.rfind("ANDROID_SOCKET_", 0) == 0 || e.rfind("QEMU_LD_PREFIX=", 0) == 0)
      socks += " [" + e + "]";
  LOGI("%s pid=%d argv: %s | env=%zu%s%s | fds=%d,%d,%d", how, pid, a.c_str(), env.size(),
       socks.empty() ? " (no ANDROID_SOCKET/QEMU_LD_PREFIX)" : "", socks.c_str(),
       fds ? fds[0] : -9, fds ? fds[1] : -9, fds ? fds[2] : -9);
}

int Spawn(std::string path, std::vector<std::string> argv, std::vector<std::string> envp, const char* dir,
          const int fds[3]) {
  EnsureInit();
  MaybeRegisterEnvBinfmt();
  // The exec root for the child: its env's QEMU_LD_PREFIX, else a `-L DIR` on
  // its command line (qemu-user), else the one the caller already carried (a
  // guest tool a qemu process spawns inherits the root). Carried on into the
  // env so a binfmt-re-run interpreter finds the tree too.
  std::string root = PrefixFromEnv(envp);
  if (root.empty()) root = PrefixFromArgv(argv);
  if (root.empty())
    if (ExecProcess* par = Current()) {
      std::lock_guard<std::mutex> l(par->lock);
      root = par->exec_root;
    }
  EnsurePrefixEnv(&envp, root);
  path = UnderRoot(root, path);
  Image img;
  const int err = ResolveProgram(&path, &argv, &img);
  if (err) return -err;
  ExecProcess* p = new ExecProcess;
  p->lp64 = img.lp64;
  p->path = path;
  p->argv = argv;
  p->envp = envp;
  p->exec_root = root;
  p->uid = arc_lproc_current_uid();
  p->pid = arc_lproc_spawn_create(BaseNameOf(argv.empty() ? path : argv[0]).c_str(), p->uid);
  {
    std::lock_guard<std::mutex> l(g_lock);
    (*g_procs)[p->pid] = p;
    g_any.store(true);
  }
  LogSpawnEnv("spawn", p->pid, argv, envp, fds);
  int r = arc_kernel_process_setup(p->pid, fds, dir);
  if (r < 0) {
    p->finished.store(true);
    arc_lproc_exited(p->pid, 127 << 8);
    return r;
  }
  StartArgs* s = new StartArgs{p, img};
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  // (a host pthread: it inherits the caller's light process, ProgramMain
  // moves it into the new one)
  r = pthread_create(&p->thread, &attr, ProgramMain, s);
  pthread_attr_destroy(&attr);
  if (r != 0) {
    delete s;
    Finish(p, 127 << 8, false);
    return -r;
  }
  return p->pid;
}

// --- threads of static programs: clone / clone3 ------------------------------------
// A static program makes its threads itself (its libc's pthread_create:
// clone or clone3 with CLONE_VM | CLONE_THREAD). Each is a host thread of the
// process that goes on from the instruction after the SVC with the parent's
// registers, x0 = 0, the new stack and TLS; CLONE_CHILD_CLEARTID's word is
// cleared and woken when it exits (pthread_join waits on it).

const uint64_t kCloneVm = 0x100, kCloneThread = 0x10000, kCloneSettls = 0x80000,
               kCloneParentSettid = 0x100000, kCloneChildCleartid = 0x200000,
               kCloneChildSettid = 0x1000000;

struct CloneStart {
  NdkCpu64State regs;
  uint64_t pc, sp, tls, flags, ctid;
  std::mutex lock;
  std::condition_variable cv;
  int tid = 0;
};

thread_local bool t_clone_thread = false;
thread_local uint32_t t_clear_tid = 0;

void* CloneMain(void* arg) {
  CloneStart* cs = static_cast<CloneStart*>(arg);
  // (the wrapped pthread_create made this a thread of the caller's process)
  const NdkCpu64State regs = cs->regs;
  const uint64_t pc = cs->pc, sp = cs->sp, tls = cs->tls, flags = cs->flags, ctid = cs->ctid;
  const int tid = GuestTid();
  if (flags & kCloneChildSettid) Store32(GuestAddr64(ctid), static_cast<uint32_t>(tid));
  t_clone_thread = true;
  t_clear_tid = (flags & kCloneChildCleartid) ? GuestAddr64(ctid) : 0;
  {
    std::lock_guard<std::mutex> l(cs->lock);
    cs->tid = tid;
  }
  cs->cv.notify_all();  // (cs belongs to the parent from here)
  if (ndk64_set_thread_guest_stack_size) ndk64_set_thread_guest_stack_size(64 << 10);
  NdkCpu64State* st = CurrentState64();
  for (int i = 8; i < 30; i++) st->x[i] = regs.x[i];
  memcpy(st->v, regs.v, sizeof st->v);
  st->fpcr = regs.fpcr;
  st->tpidr = (flags & kCloneSettls) ? tls : regs.tpidr;
  st->sp = sp ? sp : regs.sp;
  Ndk64Args a;
  memset(&a, 0, sizeof a);
  for (int i = 1; i < 8; i++) a.x[i] = regs.x[i];
  for (int i = 0; i < 8; i++) a.v[i] = regs.v[i][0];
  Ndk64Result r;
  char why[256] = "";
  if (!CallGuest64Guarded(static_cast<uint32_t>(pc), &a, &r, why, sizeof why) && strcmp(why, "thread exit"))
    LOGW("pid %d: a thread of the program ended by a fault: %s", arc_lproc_current(), why);
  if (t_clear_tid) {
    Store32(t_clear_tid, 0);
    GuestFutexWake(Ptr<void>(t_clear_tid), 1);
  }
  return nullptr;
}

// --- fork / vfork + exec (topic native-exec) ---------------------------------------
// A supervisor (dhdrun, or any program) forks a child then execs/exits in it.
// True fork is impossible in one wasm address space, but fork+exec and
// vfork+exec are: the child runs the parent's code (x0 = 0) in shared memory on
// its own host thread, from the instruction after the clone SVC; the parent is
// suspended until the child execve's or _exit's (vfork semantics). A child that
// neither execs nor exits (plain fork that keeps running) is not supported.
struct VforkStart {
  ExecProcess* p;
  NdkCpu64State regs;
};

void* VforkChildMain(void* arg) {
  VforkStart* vs = static_cast<VforkStart*>(arg);
  ExecProcess* p = vs->p;
  NdkCpu64State regs = vs->regs;
  delete vs;
  arc_lproc_enter(p->pid);
  t_vfork_pending = p;
  char why[160] = "";
  ndk64_resume_guest(&regs, why, sizeof why);
  // Reached here: the child execve'd (ExecInPlace ran the new program and
  // unwound via LeaveGuest), or it returned/faulted without exec. (_exit ends
  // the thread in ExitHook and never gets here.)
  if (t_vfork_pending == p) {  // never exec'd
    LOGW("pid %d: fork/vfork child did not exec (%s); ending it", p->pid, why[0] ? why : "returned");
    ReleaseVfork(p);
    t_vfork_pending = nullptr;
  }
  Finish(p, p->has_status.load() ? p->status : 0, true);
  return nullptr;
}

void DoVfork(const NdkCpu64State* s, uint64_t child_pc, int64_t* result) {
  ExecProcess* parent = Current();
  if (!parent) { *result = -38; return; }
  ExecProcess* p = new ExecProcess;
  p->lp64 = true;
  p->vfork = new ExecProcess::Vfork;
  {
    std::lock_guard<std::mutex> l(parent->lock);
    p->path = parent->path;
    p->envp = parent->envp;
  }
  p->uid = arc_lproc_current_uid();
  p->pid = arc_lproc_spawn_create(BaseNameOf(p->path).c_str(), p->uid);
  {
    std::lock_guard<std::mutex> l(g_lock);
    (*g_procs)[p->pid] = p;
    g_any.store(true);
  }
  // Inherit the parent's descriptors 0-2 (resolved in the parent's context
  // here) and working directory; fds >= 3 are shared across the VM's processes.
  const int fds[3] = {0, 1, 2};
  arc_kernel_process_setup(p->pid, fds, nullptr);
  VforkStart* vs = new VforkStart{p, *s};
  vs->regs.x[0] = 0;
  vs->regs.pc = child_pc;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  const int r = pthread_create(&p->thread, &attr, VforkChildMain, vs);
  pthread_attr_destroy(&attr);
  if (r != 0) {
    delete vs;
    arc_lproc_exited(p->pid, 127 << 8);
    *result = -11;  // EAGAIN
    return;
  }
  std::unique_lock<std::mutex> l(p->vfork->m);
  p->vfork->cv.wait(l, [p] { return p->vfork->done; });
  *result = p->pid;
}

}  // namespace

// SVC clone (220) / clone3 (435) of an arm64 program the VM spawned (others:
// false, the caller answers as before). *result: the child's tid or -errno.
bool ExecClone64(NdkCpu64State* s, int64_t* result) {
  if (!Current()) return false;
  uint64_t flags, sp, ptid, tls, ctid;
  {
    static const bool on = getenv("ARC_SPAWN_LOG") && getenv("ARC_SPAWN_LOG")[0] == '1';
    if (on) {
      uint64_t f = s->x[8] == 435 ? Load64(GuestAddr64(s->x[0])) : s->x[0];
      LOGI("clone pid=%d nr=%llu flags=%#llx (VM=%d THREAD=%d VFORK=%d SIGCHLD=%d)",
           arc_lproc_current(), (unsigned long long)s->x[8], (unsigned long long)f,
           !!(f & 0x100), !!(f & 0x10000), !!(f & 0x4000), (int)(f & 0xff));
    }
  }
  if (s->x[8] == 435) {
    // struct clone_args: flags, pidfd, child_tid, parent_tid, exit_signal,
    // stack, stack_size, tls, ...
    const uint32_t ca = GuestAddr64(s->x[0]);
    uint64_t f[8] = {};
    memcpy(f, Ptr<void>(ca), std::min<uint64_t>(s->x[1], sizeof f));
    flags = f[0];
    ctid = f[2];
    ptid = f[3];
    sp = f[5] ? f[5] + f[6] : 0;
    tls = f[7];
  } else {
    flags = s->x[0];
    sp = s->x[1];
    ptid = s->x[2];
    tls = s->x[3];
    ctid = s->x[4];
  }
  if (!(flags & kCloneThread)) {
    // fork / vfork via a raw clone SVC: resume the child after the SVC.
    DoVfork(s, s->pc + 4, result);
    return true;
  }
  CloneStart* cs = new CloneStart;
  cs->regs = *s;
  cs->pc = s->pc + 4;
  cs->sp = sp;
  cs->tls = tls;
  cs->flags = flags;
  cs->ctid = ctid;
  pthread_t th;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  const int r = pthread_create(&th, &attr, CloneMain, cs);
  pthread_attr_destroy(&attr);
  if (r != 0) {
    delete cs;
    *result = -11;  // EAGAIN
    return true;
  }
  int tid;
  {
    std::unique_lock<std::mutex> l(cs->lock);
    cs->cv.wait(l, [cs] { return cs->tid != 0; });
    tid = cs->tid;
  }
  delete cs;
  if (flags & kCloneParentSettid) Store32(GuestAddr64(ptid), static_cast<uint32_t>(tid));
  *result = tid;
  return true;
}

// SVC exit (93, this thread only) in a thread made by ExecClone64: it ends
// here (false for every other thread: exit() of the process).
bool ExecThreadExit64() {
  if (!t_clone_thread) return false;
  LeaveGuest("thread exit");  // back to CloneMain
  return false;
}

namespace {

// --- thunks -------------------------------------------------------------------------

// An arm64 executable's entry (__libc_init from crtbegin_dynamic.o): runs
// main with the initial stack's arguments, then exits with its result.
void G64_libc_init(uint64_t raw_args, uint64_t, uint64_t main_fn, uint64_t) {
  const uint32_t a = GuestAddr64(raw_args);
  uint64_t argc;
  memcpy(&argc, Ptr<void>(a), 8);
  uint64_t args[3] = {argc, a + 8ull, a + 8ull + 8 * (argc + 1)};
  exit(static_cast<int>(CallGuest64(GuestAddr64(main_fn), args, 3)));
}

// execve in a spawned program: the process (pid, descriptors, directory)
// runs another program, on this thread, the old one's frames left below it;
// when the new one ends the process ends. Its other threads cannot be
// stopped from here: they stay as zombies. Outside a spawned program (the
// app's own process) exec is refused, as before.
int ExecInPlace(std::string file, bool search, std::vector<std::string> argv,
                const std::vector<std::string>* envp) {
  ExecProcess* p = Current();
  if (!p) {
    __android_log_print(5, "ndk", "%s: exec in the app's own process: not available", file.c_str());
    errno = ENOSYS;
    return -1;
  }
  std::vector<std::string> env;
  {
    std::lock_guard<std::mutex> l(p->lock);
    env = envp ? *envp : p->envp;
  }
  std::string path = search ? Which(file, env) : file;
  if (path.empty()) {
    errno = ENOENT;
    return -1;
  }
  MaybeRegisterEnvBinfmt();
  // The exec root carries across exec: the new env's QEMU_LD_PREFIX, else a
  // `-L DIR` on the new command line, else what this process already carried
  // (qemu re-invoked for a guest binary keeps the guest's env, which need not
  // repeat the root). Put back into the env for a binfmt-re-run interpreter.
  std::string root = PrefixFromEnv(env);
  if (root.empty()) root = PrefixFromArgv(argv);
  if (root.empty()) {
    std::lock_guard<std::mutex> l(p->lock);
    root = p->exec_root;
  }
  EnsurePrefixEnv(&env, root);
  path = UnderRoot(root, path);
  Image img;
  const int err = ResolveProgram(&path, &argv, &img);
  if (err) {
    errno = err;
    return -1;
  }
  {
    std::lock_guard<std::mutex> l(p->lock);
    p->path = path;
    p->argv = argv;
    p->envp = env;
    p->exec_root = root;
    p->lp64 = img.lp64;
  }
  arc_lproc_set_name(p->pid, BaseNameOf(argv.empty() ? path : argv[0]).c_str());
  LOGI("pid %d: execve %s", p->pid, path.c_str());
  LogSpawnEnv("execve", p->pid, argv, env, nullptr);
  // A vfork child that reached exec: wake its suspended parent now (vfork), and
  // this thread goes on to run the new program.
  if (t_vfork_pending == p) {
    ReleaseVfork(p);
    t_vfork_pending = nullptr;
  }
  const int status = Run(p, img);
  {
    std::lock_guard<std::mutex> l(p->lock);
    if (!p->has_status.load()) {
      p->status = status;
      p->has_status.store(true);
    }
  }
  if (pthread_equal(pthread_self(), p->thread)) LeaveGuest("exit");  // to ProgramMain
  Finish(p, p->status, false);
  pthread_exit(nullptr);
}

std::vector<std::string> GuestArgv32(uint32_t a) {
  std::vector<std::string> v;
  for (uint32_t i = 0; a; i++) {
    uint32_t s = Load32(a + 4 * i);
    if (!s) break;
    v.push_back(Ptr<const char>(s));
  }
  return v;
}
std::vector<std::string> GuestArgv64(uint64_t a64) {
  std::vector<std::string> v;
  const uint32_t a = GuestAddr64(a64);
  for (uint32_t i = 0; a; i++) {
    uint64_t s;
    memcpy(&s, Ptr<void>(a + 8 * i), 8);
    if (!s) break;
    v.push_back(Ptr<const char>(GuestAddr64(s)));
  }
  return v;
}

int G_execv(const char* path, uint32_t argv) { return ExecInPlace(path ? path : "", false, GuestArgv32(argv), nullptr); }
int G_execvp(const char* file, uint32_t argv) { return ExecInPlace(file ? file : "", true, GuestArgv32(argv), nullptr); }
int G_execve(const char* path, uint32_t argv, uint32_t envp) {
  std::vector<std::string> env = GuestArgv32(envp);
  return ExecInPlace(path ? path : "", false, GuestArgv32(argv), envp ? &env : nullptr);
}
int G_execvpe(const char* file, uint32_t argv, uint32_t envp) {
  std::vector<std::string> env = GuestArgv32(envp);
  return ExecInPlace(file ? file : "", true, GuestArgv32(argv), envp ? &env : nullptr);
}
int G64_execv(const char* path, uint64_t argv) { return ExecInPlace(path ? path : "", false, GuestArgv64(argv), nullptr); }
int G64_execvp(const char* file, uint64_t argv) { return ExecInPlace(file ? file : "", true, GuestArgv64(argv), nullptr); }
int G64_execve(const char* path, uint64_t argv, uint64_t envp) {
  std::vector<std::string> env = GuestArgv64(envp);
  return ExecInPlace(path ? path : "", false, GuestArgv64(argv), envp ? &env : nullptr);
}
int G64_execvpe(const char* file, uint64_t argv, uint64_t envp) {
  std::vector<std::string> env = GuestArgv64(envp);
  return ExecInPlace(file ? file : "", true, GuestArgv64(argv), envp ? &env : nullptr);
}

const HostSymbol kExec[] = {
    NDK_FN64("__libc_init", G64_libc_init),
    NDK_FN("execv", G_execv), NDK_FN("execvp", G_execvp), NDK_FN("execve", G_execve),
    NDK_FN("execvpe", G_execvpe),
    NDK_FN64("execv", G64_execv), NDK_FN64("execvp", G64_execvp), NDK_FN64("execve", G64_execve),
    NDK_FN64("execvpe", G64_execvpe),
};

}  // namespace

FILE* ExecStdFile(int i) {
  ExecProcess* p = Current();
  if (!p || p->finished.load() || i < 0 || i > 2) return nullptr;
  std::lock_guard<std::mutex> l(p->lock);
  if (!p->std[i]) {
    p->std[i] = fdopen(i, i == 0 ? "r" : "w");
    if (!p->std[i]) return nullptr;
    // stdout by lines (an app reads a program's output as it comes),
    // stderr unbuffered, as on a terminal.
    setvbuf(p->std[i], nullptr, i == 2 ? _IONBF : _IOLBF, 0);
  }
  return p->std[i];
}

const char* ExecGetenv(const char* name, bool* handled) {
  ExecProcess* p = Current();
  *handled = p != nullptr;
  if (!p || !name) return nullptr;
  const size_t n = strlen(name);
  for (const std::string& e : p->envp)
    if (e.size() > n && e[n] == '=' && e.compare(0, n, name) == 0) return e.c_str() + n + 1;
  return nullptr;
}

// Raw execve (SVC 221) from a spawned program (topic native-exec): qemu-user
// re-execs itself for the guest's execve. Reads the guest argv/envp (LP64
// pointer arrays) and runs the new image in this process. Returns -1/errno
// on failure; on success it does not return.
int ExecRaw64(const char* path, uint64_t argv, uint64_t envp) {
  if (!Current()) {
    errno = ENOSYS;
    return -1;
  }
  std::vector<std::string> a = GuestArgv64(argv);
  std::vector<std::string> e = GuestArgv64(envp);
  std::string joined;
  for (size_t i = 0; i < a.size() && i < 6; i++) joined += (i ? " " : "") + a[i];
  LOGI("pid %d: raw execve \"%s\" argv[%zu]: %s", arc_lproc_current(), path ? path : "(null)",
       a.size(), joined.c_str());
  return ExecInPlace(path ? path : "", false, a, envp ? &e : nullptr);
}

bool ExecSetenv(const char* name, const char* value, int overwrite, int* result) {
  ExecProcess* p = Current();
  if (!p) return false;
  if (!name || !*name || strchr(name, '=')) {
    errno = EINVAL;
    *result = -1;
    return true;
  }
  const size_t n = strlen(name);
  std::lock_guard<std::mutex> l(p->lock);
  for (std::vector<std::string>::iterator it = p->envp.begin(); it != p->envp.end(); ++it) {
    if (it->size() > n && (*it)[n] == '=' && it->compare(0, n, name) == 0) {
      if (value == nullptr) {
        p->envp.erase(it);  // unsetenv
      } else if (overwrite) {
        *it = std::string(name) + "=" + value;
      }
      *result = 0;
      return true;
    }
  }
  if (value) p->envp.push_back(std::string(name) + "=" + value);
  *result = 0;
  return true;
}

// fork()/vfork() libc thunks of an arm64 spawned program (ndk_bionic.cc): the
// child resumes at the caller (x30) with 0, the parent gets the child pid; the
// parent is suspended until the child execve's/_exit's. -1/errno on failure.
int arc_vfork64(void) {
  NdkCpu64State* s = ndk64_current_state();
  int64_t r = -38;
  DoVfork(s, s->x[30], &r);
  if (r < 0) { errno = static_cast<int>(-r); return -1; }
  return static_cast<int>(r);
}

void RegisterExec() {
  RegisterHostSymbols("libc.so", kExec);
  arc_lproc_set_exit_hook(ExitHook);
  arc_lproc_set_kill_hook(KillHook);
}

// Bridges to the binfmt registry (anonymous namespace above) for the extern
// "C" API that /proc/sys/fs/binfmt_misc uses (arc_kernel.cc).
int BinfmtRegisterApi(const char* line, size_t n) { return BinfmtRegister(std::string(line, n)); }
int BinfmtStatusWrite(const char* buf, size_t n) {
  std::string s(buf, n);
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
  std::lock_guard<std::mutex> l(g_binfmt_lock);
  if (s == "1") g_binfmt_enabled = true;
  else if (s == "0") g_binfmt_enabled = false;
  else if (s == "-1") g_binfmt->clear();
  else { errno = EINVAL; return -EINVAL; }
  return 0;
}
size_t BinfmtStatusRead(char* buf, size_t size) {
  std::lock_guard<std::mutex> l(g_binfmt_lock);
  const char* s = g_binfmt_enabled ? "enabled\n" : "disabled\n";
  const size_t len = strlen(s);
  if (buf && size) memcpy(buf, s, len < size ? len : size);
  return len;
}
size_t BinfmtList(char* buf, size_t size) {
  std::lock_guard<std::mutex> l(g_binfmt_lock);
  std::string s;
  for (const BinfmtRule& r : *g_binfmt) { s += r.name; s += '\n'; }
  if (buf && size) memcpy(buf, s.data(), s.size() < size ? s.size() : size);
  return s.size();
}

}  // namespace ndk

// Runs |path| (a name without '/' is looked up in PATH) as a spawned process
// of the calling thread's process: argv, envp (null: the VM's environment),
// working directory |dir| (null: the caller's), and |fds| its descriptors
// 0-2 (the caller's descriptors; -1: closed). Returns its pid, or -errno
// (ENOENT, EACCES, ENOEXEC: what exec would have said).
extern "C" int arc_spawn(const char* path, char* const argv[], char* const envp[], const char* dir,
                         const int fds[3]) {
  std::vector<std::string> args, env;
  for (size_t i = 0; argv && argv[i]; i++) args.push_back(argv[i]);
  if (envp) {
    for (size_t i = 0; envp[i]; i++) env.push_back(envp[i]);
  } else {
    for (size_t i = 0; environ && environ[i]; i++) env.push_back(environ[i]);
  }
  if (!path || !*path) return -ENOENT;
  std::string full = ndk::Which(path, env);
  if (full.empty()) return -ENOENT;
  if (args.empty()) args.push_back(path);
  return ndk::Spawn(full, args, env, dir, fds);
}

// binfmt_misc for /proc/sys/fs/binfmt_misc (arc_kernel.cc mounts the files).
extern "C" int arc_binfmt_register(const char* line, size_t n) {
  ndk::EnsureInit();
  return ndk::BinfmtRegisterApi(line, n);
}
extern "C" int arc_binfmt_status_write(const char* buf, size_t n) { return ndk::BinfmtStatusWrite(buf, n); }
extern "C" size_t arc_binfmt_status_read(char* buf, size_t size) { return ndk::BinfmtStatusRead(buf, size); }
extern "C" size_t arc_binfmt_list(char* buf, size_t size) { return ndk::BinfmtList(buf, size); }

// Keeps this file in the link (-Wl,-u,ndk_exec_linked: mk/topic-native-exec.mk).
extern "C" int ndk_exec_linked = 1;
