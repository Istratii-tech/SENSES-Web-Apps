// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "posix_translation/procfs_file.h"

#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

#include "base/basictypes.h"
#include "base/compiler_specific.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "common/process_emulator.h"
#include "posix_translation/dir.h"
#include "posix_translation/directory_file_stream.h"
#include "posix_translation/path_util.h"
#include "posix_translation/readonly_memory_file.h"
#include "posix_translation/statfs.h"
#include "posix_translation/virtual_file_system.h"

namespace posix_translation {

namespace {

// On ARC, we provide ARM version of /proc/cpuinfo ignoring the host's
// CPU architecture. Since |kProcCpuInfoHeader| is compatible with the
// lowest-end ARM Chromebook (Snow and Spring), it is also compatible with
// the direct NDK execution mode in Bare Metal ARM.
const char kProcCpuInfoHeader[] =
    "Processor\t: ARMv7 Processor rev 4 (v7l)\n";

const char kProcCpuInfoBody[] =
    "processor\t: $1\n"  // $1 is a placeholder for CPU#
    "BogoMIPS\t: 1700.00\n"
    "\n";

const char kProcCpuInfoFooter[] =
    "Features\t: swp half thumb fastmult vfp edsp thumbee neon vfpv3 tls "
    "vfpv4 idiva idivt \n"
    "CPU implementer\t: 0x41\n"  // ARM
    "CPU architecture\t: 7\n"
    "CPU variant\t: 0x0\n"
    "CPU part\t: 0xc0f\n"  // Cortex-A15
    "CPU revision\t: 4\n"
    "\n"
    "Hardware\t: ARC\n"
    "Revision\t: 0000\n"
    "Serial\t: 0000000000000000\n";

class ProcfsFile : public ReadonlyMemoryFile {
 public:
  explicit ProcfsFile(const std::string& pathname)
      : ReadonlyMemoryFile(pathname, EIO, 0) {}

 protected:
  virtual ~ProcfsFile() {}
  virtual void UpdateContentIfNecessary() = 0;
  virtual const Content& GetContent() OVERRIDE {
    UpdateContentIfNecessary();
    set_mtime(time(NULL));
    return content_;
  }
  virtual int fstatfs(struct statfs* buf) OVERRIDE {
    return DoStatFsForProc(buf);
  }

  Content content_;
};

// Implements /proc/cpuinfo.
class CpuInfoFile : public ProcfsFile {
 public:
  // Creates a ReadonlyMemoryFile stream with |header| and |footer|.
  // See CpuInfoFileHandler for more details.
  CpuInfoFile(const std::string& pathname,
              const std::string& header,
              const std::string& body,
              const std::string& footer)
      // Pass the UNIX epoch. GetContent() below updates this later.
      : ProcfsFile(pathname),
        num_online_processors_(-1),
        header_(header), body_(body), footer_(footer) {}

 protected:
  virtual ~CpuInfoFile() {}

  // Updates |content_| when needed.
  void UpdateContentIfNecessary() OVERRIDE {
    // The cpuinfo file should be generated based on the number of online
    // CPUs, rather than the number of configured CPUs.
    const int num_online_processors = sysconf(_SC_NPROCESSORS_ONLN);
    ALOG_ASSERT(num_online_processors > 0);

    // We should not update the content when it is unnecessary to not slow down
    // a series of short CpuInfoFile::read() operations to read through the
    // file. Otherwise, in the worst case, they can touch content_.size()
    // squared bytes of memory in total, which can be very slow.
    // TODO(crbug.com/368344): Once _SC_NPROCESSORS_ONLN is fully implemented
    // for Bare Metal ARM, we should check how often the ARM Linux kernel
    // (especially the one for Pit/Pi ARM Chromebooks) changes the number of
    // CPUs in practice.
    if (num_online_processors_ == num_online_processors)
      return;
    num_online_processors_ = num_online_processors;

    std::string s = header_;
    for (int i = 0; i < num_online_processors; ++i) {
      std::vector<std::string> subst;
      subst.push_back(base::StringPrintf("%d", i));
      s += ReplaceStringPlaceholders(body_, subst, NULL);
    }
    s += footer_;
    content_.assign(s.begin(), s.end());
  }

  int num_online_processors_;
  const std::string header_;
  const std::string body_;
  const std::string footer_;

 private:
  DISALLOW_COPY_AND_ASSIGN(CpuInfoFile);
};

// Implements /proc/$PID/auxv.
class ProcessAuxvFile : public ProcfsFile {
 public:
  explicit ProcessAuxvFile(const std::string& pathname)
      : ProcfsFile(pathname) {}

 protected:
  virtual ~ProcessAuxvFile() {}

  void UpdateContentIfNecessary() OVERRIDE {
    // This came from a file that used to be canned.
    // TODO(kmixter): Generate a sensical auxv byte array.
    static const unsigned char kBytes[] = {
      0x10, 0x00, 0x00, 0x00, 0xd7, 0xb8, 0x07, 0x00,
      0x06, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00,
      0x11, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00,
      0x03, 0x00, 0x00, 0x00, 0x34, 0x20, 0xfb, 0x76,
      0x04, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00,
      0x05, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00,
      0x07, 0x00, 0x00, 0x00, 0x00, 0x20, 0xf9, 0x76,
      0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x09, 0x00, 0x00, 0x00, 0xbd, 0x4d, 0xfb, 0x76,
      0x0b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x0d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x0e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x17, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x19, 0x00, 0x00, 0x00, 0x4f, 0x47, 0xcc, 0x7e,
      0x1f, 0x00, 0x00, 0x00, 0xf4, 0x4f, 0xcc, 0x7e,
      0x0f, 0x00, 0x00, 0x00, 0x5f, 0x47, 0xcc, 0x7e,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    std::vector<unsigned char> bytes;
    bytes.assign(kBytes, kBytes + sizeof(kBytes));
    content_.assign(bytes.begin(), bytes.end());
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(ProcessAuxvFile);
};

const char kProcStatFormat[] =
    "$2 ($1) R $2 $2 $2 0 $2 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 "
    "0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n";

const char kProcStatusFormat[] =
    "Name:   $1\n"
    "State:  R (running)\n"
    "Tgid:   $2\n"
    "Pid:    $2\n"
    "PPid:   $2\n"
    "TracerPid:      0\n"
    "Uid:    $3   $3   $3    $3\n"
    "Gid:    $3   $3   $3    $3\n"
    "FDSize: 32\n"
    "Groups: 0\n"
    "VmPeak:        0 kB\n"
    "VmSize:        0 kB\n"
    "VmLck:         0 kB\n"
    "VmPin:         0 kB\n"
    "VmHWM:         0 kB\n"
    "VmRSS:         0 kB\n"
    "VmData:        0 kB\n"
    "VmStk:         0 kB\n"
    "VmExe:         0 kB\n"
    "VmLib:         0 kB\n"
    "VmPTE:         0 kB\n"
    "VmSwap:        0 kB\n";

// This file was previously a canned file.
// TODO(crbug.com/452367): Construct a valid map.
const char kProcMapsFormat[] =
    "00008000-0002e000 r-xp 00000000 00:01 26         /sbin/adbd\n"
    "0002f000-00031000 rw-p 00026000 00:01 26         /sbin/adbd\n"
    "00031000-0004c000 rw-p 00031000 00:00 0          [heap]\n"
    "40000000-40008000 r--s 00000000 00:0a 47         "
    "/dev/__properties__ (deleted)\n"
    "40008000-40009000 r--p 40008000 00:00 0 \n"
    "40009000-4000a000 ---p 40009000 00:00 0 \n"
    "4000a000-40109000 rw-p 4000a000 00:00 0 \n"
    "40109000-4010a000 ---p 40109000 00:00 0 \n"
    "4010a000-40209000 rw-p 4010a000 00:00 0 \n"
    "40209000-4020a000 ---p 40209000 00:00 0 \n"
    "4020a000-40309000 rw-p 4020a000 00:00 0 \n"
    "40309000-4030a000 ---p 40309000 00:00 0 \n"
    "4030a000-40409000 rw-p 4030a000 00:00 0 \n"
    "40409000-4040a000 ---p 40409000 00:00 0 \n"
    "4040a000-40509000 rw-p 4040a000 00:00 0 \n"
    "bec72000-bec87000 rw-p befeb000 00:00 0          [stack]\n";

// Implements files like /proc/$PID/{maps,stat,status}.
class ProcessFormattedFile : public ProcfsFile {
 public:
  ProcessFormattedFile(const std::string& pathname, pid_t pid,
                      const char* format)
      : ProcfsFile(pathname), pid_(pid), format_(format) {}

 protected:
  virtual ~ProcessFormattedFile() {}

  void UpdateContentIfNecessary() OVERRIDE {
    std::string argv0;
    std::string s;
    uid_t uid;
    if (arc::ProcessEmulator::GetInfoByPid(pid_, &argv0, &uid)) {
      std::vector<std::string> subst;
      subst.push_back(argv0);  // $1 - name
      subst.push_back(base::StringPrintf("%d", pid_));  // $2 - pid
      subst.push_back(base::StringPrintf("%d", uid));  // $3 - uid
      s = ReplaceStringPlaceholders(format_, subst, NULL);
    }
    content_.assign(s.begin(), s.end());
  }

  pid_t pid_;
  const char* format_;

 private:
  DISALLOW_COPY_AND_ASSIGN(ProcessFormattedFile);
};

// A base class to implement procfs files which are arrays of strings that
// are delimited, like /proc/$PID/{cmdline,environ,mounts}.
class ProcessDelimitedFile : public ProcfsFile {
 public:
  ProcessDelimitedFile(const std::string& pathname, unsigned char delimiter)
      : ProcfsFile(pathname), delimiter_(delimiter) {}

 protected:
  typedef std::vector<std::string> StringVector;
  virtual ~ProcessDelimitedFile() {}

  // If no updates have occurred, return false, otherwise return true and
  // update string_vector_.
  virtual bool UpdateStringVectorIfNecessary() = 0;

  void UpdateContentIfNecessary() OVERRIDE {
    if (!UpdateStringVectorIfNecessary()) return;
    size_t resulting_size = 0;
    content_.clear();
    for (StringVector::iterator i = string_vector_.begin();
         i != string_vector_.end(); ++i) {
      resulting_size += i->size() + 1;
    }
    content_.reserve(resulting_size);
    for (StringVector::iterator i = string_vector_.begin();
         i != string_vector_.end(); ++i) {
      for (int j = 0; j < i->size(); ++j)
        content_.push_back((*i)[j]);
      content_.push_back(delimiter_);
    }
    LOG_ALWAYS_FATAL_IF(content_.size() != resulting_size);
  }

  StringVector string_vector_;
  unsigned char delimiter_;
};

class ProcessCmdlineFile : public ProcessDelimitedFile {
 public:
  ProcessCmdlineFile(const std::string& pathname, pid_t pid)
      : ProcessDelimitedFile(pathname, '\0'), pid_(pid) {}

 protected:
  bool UpdateStringVectorIfNecessary() OVERRIDE {
    std::string argv0;
    string_vector_.clear();
    if (arc::ProcessEmulator::GetInfoByPid(pid_, &argv0, NULL)) {
      string_vector_.push_back(argv0);
    }
    return true;
  }

  pid_t pid_;
};

class ProcessMountsFile : public ProcessDelimitedFile {
 public:
  ProcessMountsFile(const std::string& pathname,
                    MountPointManager* manager)
      : ProcessDelimitedFile(pathname, '\n'),
        manager_(manager) {}

 protected:
  virtual bool UpdateStringVectorIfNecessary() OVERRIDE {
    if (manager_ != NULL) {
      const MountPointManager::MountPointMap* mounts =
          manager_->GetMountPointMap();
      if (!update_consumer_.AreThereUpdatesAndConsumeIfSo(
              manager_->GetUpdateProducer())) {
        return false;
      }
      string_vector_.clear();
      typedef std::vector<std::string> StringVector;
      StringVector sorted_mount_paths;
      for (MountPointManager::MountPointMap::const_iterator i = mounts->begin();
           i != mounts->end(); ++i) {
        sorted_mount_paths.push_back(i->first);
      }
      std::sort(sorted_mount_paths.begin(), sorted_mount_paths.end());
      for (StringVector::iterator i = sorted_mount_paths.begin();
           i != sorted_mount_paths.end(); ++i) {
        // This file has fstab (5) formatting, specifically:
        // <fs_spec> <fs_file> <fs_vfstype> <fs_mntops> <fs_freq> <fs_passno>\n
        std::string mount_path = *i;
        const MountPointManager::MountPoint* point =
            &mounts->find(mount_path)->second;
        const FileSystemHandler* handler = point->handler;
        bool is_single_file = !util::EndsWithSlash(mount_path);
        // Only / should have a trailing slash, all other directory paths
        // should end just with the directory name.
        if (mount_path != "/")
          util::RemoveTrailingSlashes(&mount_path);
        string_vector_.push_back(base::StringPrintf(
            "none %s %s uid=%d%s 0 0",
            mount_path.c_str(), handler->name().c_str(), point->owner_uid,
            is_single_file ? ",single_file" : ""));
      }
    }
    return true;
  }

 private:
  MountPointManager* manager_;
  arc::UpdateConsumer update_consumer_;

  DISALLOW_COPY_AND_ASSIGN(ProcessMountsFile);
};

class ProcNetUnixFile : public ProcfsFile {
 public:
  explicit ProcNetUnixFile(const std::string& pathname)
      : ProcfsFile(pathname) {}

 protected:
  virtual void UpdateContentIfNecessary() OVERRIDE {
    VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
    AbstractSocketNamespace* ns = sys->GetAbstractSocketNamespace();
    if (!update_consumer_.AreThereUpdatesAndConsumeIfSo(
            ns->GetUpdateProducer())) {
      return;
    }
    AbstractSocketNamespace::Streams streams;
    ns->GetAllStreams(&streams);
    // See the code in Chrome adb_device_info_query.cc#MapSocketsToProcesses
    // which reads this format back out.
    std::string contents =
        "Num       RefCount Protocol Flags    Type St Inode Path\n";
    for (AbstractSocketNamespace::Streams::iterator i = streams.begin();
         i != streams.end(); ++i) {
      contents.append(base::StringPrintf(
          "%08d: %08d %08d %08x %04d %02d %d @%s\n",
          0, 1, 0, 0x10000, 0, 1, 0, (*i)->GetBoundAbstractName().c_str()));
    }
    content_.assign(contents.begin(), contents.end());
  }

 private:
  arc::UpdateConsumer update_consumer_;

  DISALLOW_COPY_AND_ASSIGN(ProcNetUnixFile);
};

}  // namespace

ProcfsFileHandler::ProcfsFileHandler(FileSystemHandler* readonly_fs_handler)
    : FileSystemHandler("ProcfsFileHandler"),
      readonly_fs_handler_(readonly_fs_handler),
      mount_point_manager_(NULL) {
  SetCpuInfoFileTemplate(kProcCpuInfoHeader, kProcCpuInfoBody,
                         kProcCpuInfoFooter);
}

void ProcfsFileHandler::SetCpuInfoFileTemplate(const std::string& header,
                                               const std::string& body,
                                               const std::string& footer) {
  cpuinfo_header_ = header;
  cpuinfo_body_ = body;
  cpuinfo_footer_ = footer;

  // |body| must contain (exactly) one placeholder, "$1".
  ALOG_ASSERT(cpuinfo_body_.find("$1") != std::string::npos);
  ALOG_ASSERT(cpuinfo_body_.find("$2") == std::string::npos);
}

ProcfsFileHandler::~ProcfsFileHandler() {
}

void ProcfsFileHandler::SetMountPointManager(MountPointManager* manager) {
  mount_point_manager_ = manager;
}

void ProcfsFileHandler::SynchronizeDirectoryTreeStructure() {
  arc::ProcessEmulator* emulator = arc::ProcessEmulator::GetInstance();
  if (!update_consumer_.AreThereUpdatesAndConsumeIfSo(
          emulator->GetUpdateProducer())) {
    return;
  }
  file_names_.Clear();
  // We provide cpuinfo's contents.
  file_names_.AddFile("/proc/cpuinfo");
  // We provide these symlinks.
  file_names_.AddFileWithType("/proc/mounts", Dir::SYMLINK);
  file_names_.AddFileWithType("/proc/self", Dir::SYMLINK);
  // GetFirstPid/GetNextPid are guaranteed to be tolerant of mutation while
  // iterating the list of PIDs.  If a process is created or removed while
  // executing this function, the last_process_state_ will not reflect it
  // but the directory structure may.  The only consequence is this function
  // will be rerun again later.
  for (pid_t pid = emulator->GetFirstPid(); pid != 0;
       pid = emulator->GetNextPid(pid)) {
    file_names_.AddFile(base::StringPrintf("/proc/%d/auxv", pid));
    file_names_.AddFile(base::StringPrintf("/proc/%d/cmdline", pid));
    // The /proc/*/fd/* files are not actually usable, but were included
    // to pass ProcessManagerTest testCloseNonStandardFds.
    file_names_.AddFile(base::StringPrintf("/proc/%d/fd/0", pid));
    file_names_.AddFile(base::StringPrintf("/proc/%d/fd/1", pid));
    file_names_.AddFile(base::StringPrintf("/proc/%d/fd/2", pid));
    file_names_.AddFile(base::StringPrintf("/proc/%d/exe", pid));
    file_names_.AddFile(base::StringPrintf("/proc/%d/maps", pid));
    file_names_.AddFile(base::StringPrintf("/proc/%d/mounts", pid));
    file_names_.AddFile(base::StringPrintf("/proc/%d/stat", pid));
    file_names_.AddFile(base::StringPrintf("/proc/%d/status", pid));
  }
  // We provide the file /proc/net/unix.
  file_names_.AddFile("/proc/net/unix");
  // Now add all the files that are provided by the readonlyfs.
  file_names_.AddFile("/proc/cmdline");
  file_names_.AddFile("/proc/loadavg");
  file_names_.AddFile("/proc/meminfo");
  file_names_.AddFile("/proc/net/tcp");
  file_names_.AddFile("/proc/net/tcp6");
  file_names_.AddFile("/proc/net/udp");
  file_names_.AddFile("/proc/net/udp6");
  file_names_.AddFile("/proc/stat");
  file_names_.AddFile("/proc/version");
}

bool ProcfsFileHandler::IsInitialized() const {
  return (!readonly_fs_handler_) || readonly_fs_handler_->IsInitialized();
}

void ProcfsFileHandler::Initialize() {
  ALOG_ASSERT(!IsInitialized());
  if (readonly_fs_handler_ && !readonly_fs_handler_->IsInitialized())
    readonly_fs_handler_->Initialize();
}

Dir* ProcfsFileHandler::OnDirectoryContentsNeeded(const std::string& name) {
  SynchronizeDirectoryTreeStructure();
  return file_names_.OpenDirectory(name);
}

// Parse a path like /(\d+)(\/.*) and return the two groups in out_pid and
// out_post_pid.  Returns false if does not match.
bool ProcfsFileHandler::ParsePidBasedPath(const std::string& pathname,
                                          pid_t* out_pid,
                                          std::string* out_post_pid) {
  if (strncmp(pathname.c_str(), "/proc/", 6) != 0)
    return false;
  int pid = 0;
  int i;
  for (i = 6; i < pathname.size(); ++i) {
    if (pathname[i] < '0' || pathname[i] > '9')
      break;
    pid = pid * 10 + pathname[i] - '0';
  }
  if (pathname[i] !='/') {
    return false;
  }
  *out_pid = pid;
  *out_post_pid = pathname.substr(i);
  return true;
}

scoped_refptr<FileStream> ProcfsFileHandler::open(
    int fd, const std::string& pathname, int oflag, mode_t cmode) {
  // Check if |pathname| is a directory.
  SynchronizeDirectoryTreeStructure();
  if (file_names_.StatDirectory(pathname))
    return new DirectoryFileStream("procfs", pathname, this);

  std::string post_pid;
  pid_t pid;
  if (ParsePidBasedPath(pathname, &pid, &post_pid)) {
    if (arc::ProcessEmulator::GetInfoByPid(pid, NULL, NULL)) {
      if (post_pid == "/auxv") {
        return new ProcessAuxvFile(pathname);
      } else if (post_pid == "/cmdline") {
        return new ProcessCmdlineFile(pathname, pid);
      } else if (post_pid == "/maps") {
        return new ProcessFormattedFile(pathname, pid, kProcMapsFormat);
      } else if (post_pid == "/mounts") {
        return new ProcessMountsFile(pathname, mount_point_manager_);
      } else if (post_pid == "/stat") {
        return new ProcessFormattedFile(pathname, pid, kProcStatFormat);
      } else if (post_pid == "/status") {
        return new ProcessFormattedFile(pathname, pid, kProcStatusFormat);
      }
    }
    errno = ENOENT;
    return NULL;
  } else if (pathname == "/proc/cpuinfo") {
    return new CpuInfoFile(pathname, cpuinfo_header_, cpuinfo_body_,
                           cpuinfo_footer_);
  } else if (pathname == "/proc/net/unix") {
    return new ProcNetUnixFile(pathname);
  } else if (readonly_fs_handler_ != NULL) {
    return readonly_fs_handler_->open(fd, pathname, oflag, cmode);
  } else {
    ALOGE("No entry for procfs path and no readonly-fs fallback for: %s.",
          pathname.c_str());
    errno = ENOENT;
    return NULL;
  }
}

int ProcfsFileHandler::stat(const std::string& pathname, struct stat* out) {
  scoped_refptr<FileStream> file = this->open(-1, pathname, O_RDONLY, 0);
  if (!file) {
    errno = ENOENT;
    return -1;
  }
  return file->fstat(out);
}

int ProcfsFileHandler::statfs(const std::string& pathname,
                              struct statfs* out) {
  return DoStatFsForProc(out);
}

ssize_t ProcfsFileHandler::readlink(const std::string& pathname,
                                    std::string* resolved) {
  if (pathname == "/proc/self") {
    pid_t my_pid = arc::ProcessEmulator::GetPid();
    *resolved = base::StringPrintf("/proc/%d", my_pid);
    return resolved->size();
  } else if (pathname == "/proc/mounts") {
    pid_t my_pid = arc::ProcessEmulator::GetPid();
    *resolved = base::StringPrintf("/proc/%d/mounts", my_pid);
    return resolved->size();
  }
  pid_t request_pid = 0;
  std::string post_pid;
  if (ParsePidBasedPath(pathname, &request_pid, &post_pid)) {
    if (post_pid == "/exe") {
      // On upstream Android, the exe symlink points to Dalvik's executable.
      // However, since such a binary is not available on our system, we
      // approximate using runnable-ld.so (which is ET_EXEC) instead. We prefer
      // runnable-ld.so over main.nexe since some apps crash if the /proc file
      // points to a huge binary like main.nexe which does not fit into the
      // NaCl's small virtual address space.
      *resolved = "/system/lib/runnable-ld.so";
      return resolved->size();
    }
  }
  errno = EINVAL;
  return -1;
}

}  // namespace posix_translation
