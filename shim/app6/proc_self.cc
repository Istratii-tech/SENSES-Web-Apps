// /proc/self/ (and /proc/PID/, /proc/thread-self/, their task/TID/) for
// artvm: the files apps' native libraries read about their own process
// (topic tiktok, docs/topics/tiktok.md). Without them, a hooking or crash
// library (bytehook, shadowhook, npth) or a security SDK opens
// /proc/self/maps, gets ENOENT and parses an empty buffer as a mapping.
//
//   maps     the ARM libraries of the NDK bridge, as Linux prints mappings
//            (arc_proc_maps, shim/ndk/ndk_linker.cc; empty without it)
//   cmdline  the process name (Process.setArgV0: the app's process name)
//   comm     the same, at most 15 characters
//   status   Linux 3.18's fields: Name, State, Tgid, Pid, PPid, TracerPid
//            (0: no debugger), Uid/Gid, threads, memory from the heap size
//   stat     the 52 fields, mostly 0
//   statm    sizes in pages, from the heap size (Dart's ProcessInfo.currentRss:
//            Saber polled it without end)
//   exe      a symbolic link to /system/bin/app_process32
//
// Read-only, generated at open. Light processes (topic multiproc,
// shim/multiproc/include/arc_lproc.h) have their own /proc/PID/ with their
// name; /proc/self/ is the calling thread's process. Part of the arc-wasm
// shim layer.

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <string>

#include <emscripten/heap.h>
#include <emscripten/threading.h>

#include "posix_translation/dir.h"
#include "posix_translation/file_system_handler.h"
#include "posix_translation/readonly_memory_file.h"
#include "posix_translation/statfs.h"
#include "posix_translation/virtual_file_system.h"
#include "../multiproc/include/arc_lproc.h"

extern "C" size_t arc_proc_maps(char* buf, size_t size) __attribute__((weak));

namespace {

posix_translation::FileSystemHandler* g_handler;

std::string ProcessName(pid_t pid) {
  std::string name(arc_lproc_name(pid, NULL, 0), '\0');
  if (!name.empty())
    arc_lproc_name(pid, &name[0], name.size() + 1);
  return name;
}

// /proc/PID/ of a new light process.
void MountLightProcess(int pid) {
  if (g_handler)
    posix_translation::VirtualFileSystem::GetVirtualFileSystem()->Mount(
        "/proc/" + std::to_string(pid) + "/", g_handler);
}

class MemoryFile : public posix_translation::ReadonlyMemoryFile {
 public:
  MemoryFile(const std::string& pathname, const std::string& text)
      : ReadonlyMemoryFile(pathname, ENODEV, time(NULL)), content_(text.begin(), text.end()) {}

 protected:
  virtual ~MemoryFile() {}
  virtual const Content& GetContent() OVERRIDE { return content_; }
  virtual int fstatfs(struct statfs* buf) OVERRIDE {
    return posix_translation::DoStatFsForProc(buf);
  }

 private:
  Content content_;
};

std::string Maps() {
  if (!arc_proc_maps) return std::string();
  std::string s(arc_proc_maps(NULL, 0), '\0');
  if (!s.empty()) {
    const size_t n = arc_proc_maps(&s[0], s.size());  // (it may have changed)
    if (n < s.size()) s.resize(n);
  }
  return s;
}

std::string Status(pid_t pid, pid_t tid) {
  const std::string name = ProcessName(pid).substr(0, 15);
  const int uid = arc_lproc_uid(pid);
  const unsigned long kb = static_cast<unsigned long>(emscripten_get_heap_size() / 1024);
  // The browser's cores (1-8), as /proc/cpuinfo and the NDK bridge's sysconf say.
  int cpus = emscripten_num_logical_cores();
  cpus = cpus < 1 ? 1 : cpus > 8 ? 8 : cpus;
  char cpus_list[8] = "0";
  if (cpus > 1) snprintf(cpus_list, sizeof(cpus_list), "0-%d", cpus - 1);
  char buf[1024];
  snprintf(buf, sizeof(buf),
           "Name:\t%s\nState:\tR (running)\nTgid:\t%d\nPid:\t%d\nPPid:\t1\nTracerPid:\t0\n"
           "Uid:\t%d\t%d\t%d\t%d\nGid:\t%d\t%d\t%d\t%d\nFDSize:\t256\nGroups:\t\n"
           "VmPeak:\t%8lu kB\nVmSize:\t%8lu kB\nVmLck:\t       0 kB\nVmPin:\t       0 kB\n"
           "VmHWM:\t%8lu kB\nVmRSS:\t%8lu kB\nVmData:\t%8lu kB\nVmStk:\t     136 kB\n"
           "VmExe:\t      20 kB\nVmLib:\t       0 kB\nVmPTE:\t       0 kB\nVmSwap:\t       0 kB\n"
           "Threads:\t1\nSigQ:\t0/0\nSigPnd:\t0000000000000000\nShdPnd:\t0000000000000000\n"
           "SigBlk:\t0000000000000000\nSigIgn:\t0000000000000000\nSigCgt:\t0000000000000000\n"
           "CapInh:\t0000000000000000\nCapPrm:\t0000000000000000\nCapEff:\t0000000000000000\n"
           "CapBnd:\t0000000000000000\nSeccomp:\t0\nCpus_allowed:\t%x\nCpus_allowed_list:\t%s\n"
           "voluntary_ctxt_switches:\t0\nnonvoluntary_ctxt_switches:\t0\n",
           name.c_str(), pid, tid, uid, uid, uid, uid, uid, uid, uid, uid, kb, kb, kb, kb, kb,
           (1u << cpus) - 1, cpus_list);
  return buf;
}

std::string Stat(pid_t pid, pid_t tid) {
  const unsigned long bytes = static_cast<unsigned long>(emscripten_get_heap_size());
  char buf[512];
  // pid (comm) state ppid pgrp session tty tpgid flags minflt cminflt majflt
  // cmajflt utime stime cutime cstime priority nice threads itrealvalue
  // starttime vsize rss rsslim, then 28 more.
  snprintf(buf, sizeof(buf),
           "%d (%s) R 1 %d %d 0 -1 4194560 0 0 0 0 0 0 0 0 20 0 1 0 0 %lu %lu 4294967295 "
           "0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n",
           tid, ProcessName(pid).substr(0, 15).c_str(), pid, pid, bytes, bytes / 4096);
  return buf;
}

// size resident shared text lib data dt, in pages.
std::string Statm() {
  const unsigned long pages = static_cast<unsigned long>(emscripten_get_heap_size() / 4096);
  char buf[128];
  snprintf(buf, sizeof(buf), "%lu %lu 0 5 0 %lu 0\n", pages, pages, pages);
  return buf;
}

// /proc/self/ & co. Paths reach open() whole ("/proc/self/maps").
class ProcSelfHandler : public posix_translation::FileSystemHandler {
 public:
  ProcSelfHandler() : FileSystemHandler("ProcSelfHandler") {}

  virtual scoped_refptr<posix_translation::FileStream> open(int fd, const std::string& pathname,
                                                            int oflag, mode_t cmode) OVERRIDE {
    if ((oflag & O_ACCMODE) != O_RDONLY) {
      errno = EACCES;
      return NULL;
    }
    pid_t pid, tid;
    std::string file;
    if (!Parse(pathname, &pid, &tid, &file)) {
      errno = ENOENT;
      return NULL;
    }
    if (!IsFile(file)) {
      errno = file.empty() ? EISDIR : ENOENT;
      return NULL;
    }
    std::string text;
    if (file == "maps") {
      text = Maps();
    } else if (file == "cmdline") {
      text = ProcessName(pid);
      text.push_back('\0');
    } else if (file == "comm") {
      text = ProcessName(pid).substr(0, 15) + "\n";
    } else if (file == "status") {
      text = Status(pid, tid);
    } else if (file == "stat") {
      text = Stat(pid, tid);
    } else if (file == "statm") {
      text = Statm();
    }  // environ, auxv, wchan: empty
    return new MemoryFile(pathname, text);
  }

  virtual posix_translation::Dir* OnDirectoryContentsNeeded(const std::string&) OVERRIDE {
    return NULL;
  }

  virtual int stat(const std::string& pathname, struct stat* out) OVERRIDE {
    pid_t pid, tid;
    std::string file;
    if (!Parse(pathname, &pid, &tid, &file) || !(file.empty() || IsFile(file))) {
      errno = ENOENT;
      return -1;
    }
    memset(out, 0, sizeof(*out));
    // Owner-writable: the kernel's access() assumes S_IWUSR everywhere (opening
    // for writing still fails).
    out->st_mode = file.empty() ? (S_IFDIR | 0755) : (S_IFREG | 0644);
    out->st_nlink = 1;
    out->st_uid = arc_lproc_uid(pid);
    out->st_gid = arc_lproc_uid(pid);
    out->st_blksize = 1024;
    return 0;
  }

  virtual int statfs(const std::string&, struct statfs* out) OVERRIDE {
    return posix_translation::DoStatFsForProc(out);
  }

  // exe: a link to the zygote's binary, as on a 32-bit device (Dart's
  // Platform.resolvedExecutable reads it, and copies -1 bytes without it:
  // LocalSend). Not followed: the binary is not in this /system.
  virtual ssize_t readlink(const std::string& pathname, std::string* resolved) OVERRIDE {
    pid_t pid, tid;
    std::string file;
    if (!Parse(pathname, &pid, &tid, &file) || file != "exe") {
      errno = EINVAL;
      return -1;
    }
    *resolved = "/system/bin/app_process32";
    return resolved->size();
  }

 private:
  static bool IsFile(const std::string& f) {
    return f == "maps" || f == "cmdline" || f == "comm" || f == "status" || f == "stat" ||
           f == "statm" ||
           f == "environ" || f == "auxv" || f == "wchan";
  }

  // "/proc/{self,PID}[/task/TID]/FILE" -> PID (the calling thread's process
  // for self), TID (the process id when absent, as Linux's /proc/PID/stat
  // shows the main thread; the caller's for thread-self), FILE ("" for the
  // directory itself). A killed light process has no /proc/PID any more.
  static bool Parse(const std::string& path, pid_t* pid, pid_t* tid, std::string* file) {
    std::string rest;
    *pid = getpid();
    if (path.compare(0, 10, "/proc/self") == 0) {
      rest = path.substr(10);
    } else if (path.compare(0, 17, "/proc/thread-self") == 0) {
      rest = path.substr(17);
      *tid = gettid();
    } else if (path.compare(0, 6, "/proc/") == 0 && path.size() > 6 && isdigit(path[6])) {
      size_t end = path.find('/', 6);
      *pid = atoi(path.substr(6, end == std::string::npos ? std::string::npos : end - 6).c_str());
      if (!arc_lproc_alive(*pid))
        return false;
      rest = end == std::string::npos ? std::string() : path.substr(end);
    } else {
      return false;
    }
    if (path.compare(0, 17, "/proc/thread-self") != 0)
      *tid = *pid;
    if (rest.compare(0, 6, "/task/") == 0) {
      size_t end = rest.find('/', 6);
      *tid = atoi(rest.substr(6, end == std::string::npos ? std::string::npos : end - 6).c_str());
      rest = end == std::string::npos ? std::string() : rest.substr(end);
    }
    if (rest.empty() || rest == "/") {
      file->clear();
      return true;
    }
    if (rest[0] != '/' || rest.find('/', 1) != std::string::npos)
      return false;
    *file = rest.substr(1);
    return true;
  }
};

}  // namespace

// The name in /proc/self/cmdline (shim/fw6/android_runtime.cc:
// AndroidRuntime::setArgv0, from Process.setArgV0): the calling thread's
// process's.
extern "C" void arc_proc_set_name(const char* name) {
  arc_lproc_set_name(getpid(), name);
}

// Mounts the handler (art/vfs_setup.cc, after the kernel boots).
extern "C" void arc_app6_mount_proc_self(posix_translation::VirtualFileSystem* vfs) {
  ProcSelfHandler* h = new ProcSelfHandler;
  vfs->Mount("/proc/self/", h);
  vfs->Mount("/proc/thread-self/", h);
  vfs->Mount("/proc/" + std::to_string(getpid()) + "/", h);
  g_handler = h;
  arc_lproc_set_create_hook(MountLightProcess);
}
