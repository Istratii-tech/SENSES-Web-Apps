// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "posix_translation/cpu_file.h"

#include <time.h>
#include <unistd.h>

#include <string>

#include "base/basictypes.h"
#include "base/compiler_specific.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "posix_translation/dir.h"
#include "posix_translation/directory_file_stream.h"
#include "posix_translation/path_util.h"
#include "posix_translation/readonly_memory_file.h"
#include "posix_translation/statfs.h"

namespace posix_translation {

namespace {

// An interface for providing file content to CpuFile.
class CpuFileContent {
 public:
  CpuFileContent() {}
  virtual ~CpuFileContent() {}
  virtual const ReadonlyMemoryFile::Content& GetContent() = 0;

 protected:
  // Creates the content of a CPU file from |min| and |max| and stores it in
  // |content_|. |min| must be smaller than or equial to |max|. Both |min| and
  // |max| must not be negative.
  void UpdateContent(int min, int max) {
    ALOG_ASSERT(min >= 0 && max >= 0 && max >= min,
                "min: %d, max: %d", min, max);
    const std::string s = (min == max) ? base::StringPrintf("%d\n", min)
        : base::StringPrintf("%d-%d\n", min, max);
    content_.assign(s.begin(), s.end());
  }

  ReadonlyMemoryFile::Content content_;

 private:
  DISALLOW_COPY_AND_ASSIGN(CpuFileContent);
};

// A class for "kernel_max". The file contains one number which is equal to
// (kNrCpus - 1) followed by "\n".
class KernelMaxFileContent : public CpuFileContent {
 public:
  KernelMaxFileContent() {
    UpdateContent(kNrCpus - 1, kNrCpus - 1);
  }
  virtual ~KernelMaxFileContent() {}

  virtual const ReadonlyMemoryFile::Content& GetContent() OVERRIDE {
    ALOG_ASSERT(!content_.empty());
    return content_;
  }

 private:
  // A constant equivalent to NR_CPUS in the Linux kernel config.
  static const int kNrCpus = 64;

  DISALLOW_COPY_AND_ASSIGN(KernelMaxFileContent);
};

// A class for "offline". The file contains only "\n" when all CPUs are online.
// Otherwise, the file contains CPU numbers that are offline. For example, when
// the last 2 CPUs out of 8 are offline, the content is "6-7".
class OfflineFileContent : public CpuFileContent {
 public:
  explicit OfflineFileContent(int num_processors)
      : num_processors_(num_processors) {
    ALOG_ASSERT(num_processors_ > 0);
  }
  virtual ~OfflineFileContent() {}

  virtual const ReadonlyMemoryFile::Content& GetContent() OVERRIDE {
    const int num_online_processors = sysconf(_SC_NPROCESSORS_ONLN);
    ALOG_ASSERT(num_online_processors > 0);
    const int num_offline_processors = num_processors_ - num_online_processors;
    if (num_offline_processors == 0)
      content_.assign(1, '\n');  // no offline CPUs.
    else
      UpdateContent(num_online_processors, num_processors_ - 1);
    ALOG_ASSERT(num_processors_ >= num_online_processors);  // sanity check.
    ALOG_ASSERT(!content_.empty());
    return content_;
  }

 private:
  const int num_processors_;

  DISALLOW_COPY_AND_ASSIGN(OfflineFileContent);
};

// A class for "online". The file contains CPU numbers that are onine. For
// example, when 2 CPUs out of 2 are online, the content is "0-1". When
// 1 out of 1 is, the content is "0".
class OnlineFileContent : public CpuFileContent {
 public:
  OnlineFileContent() {}
  virtual ~OnlineFileContent() {}

  virtual const ReadonlyMemoryFile::Content& GetContent() OVERRIDE {
    const int num_online_processors = sysconf(_SC_NPROCESSORS_ONLN);
    ALOG_ASSERT(num_online_processors > 0);
    UpdateContent(0, num_online_processors - 1);
    ALOG_ASSERT(!content_.empty());
    return content_;
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(OnlineFileContent);
};

// A class for "present" and "possible". The file contains CPU numbers that are
// configured (i.e. physically available). For example, when 2 CPUs are
// configured, the content is "0-1". When 1 out of 1 is, the content is "0".
class PresentFileContent : public CpuFileContent {
 public:
  explicit PresentFileContent(int num_processors) {
    ALOG_ASSERT(num_processors > 0);
    UpdateContent(0, num_processors - 1);
  }
  virtual ~PresentFileContent() {}

  virtual const ReadonlyMemoryFile::Content& GetContent() OVERRIDE {
    ALOG_ASSERT(!content_.empty());
    return content_;
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(PresentFileContent);
};

// A file stream for readonly files managed by CpuFileHandler.
class CpuFile : public ReadonlyMemoryFile {
 public:
  // Creates a ReadonlyMemoryFile stream with |content|. The object takes
  // ownership of |content|.
  CpuFile(const std::string& pathname, CpuFileContent* content)
      : ReadonlyMemoryFile(pathname, EIO /* no mmap support */, time(NULL)),
        content_(content) {
    ALOG_ASSERT(content_.get());
  }

 protected:
  virtual ~CpuFile() {}

 private:
  // ReadonlyMemoryFile overrides:
  virtual const Content& GetContent() OVERRIDE {
    ALOG_ASSERT(content_.get());
    return content_->GetContent();
  }

  virtual int fstatfs(struct statfs* buf) OVERRIDE {
    return DoStatFsForProc(buf);
  }

  scoped_ptr<CpuFileContent> content_;

  DISALLOW_COPY_AND_ASSIGN(CpuFile);
};

}  // namespace

CpuFileHandler::CpuFileHandler() : FileSystemHandler("CpuFileHandler"),
                                   is_initialized_(false), num_processors_(-1) {
}

CpuFileHandler::~CpuFileHandler() {
}

bool CpuFileHandler::IsInitialized() const {
  return is_initialized_;
}

void CpuFileHandler::Initialize() {
  ALOG_ASSERT(!IsInitialized());
  ALOG_ASSERT(!path_.empty());
  directory_manager_.MakeDirectories(path_);

  num_processors_ = sysconf(_SC_NPROCESSORS_CONF);
  ALOG_ASSERT(num_processors_ > 0);
  ALOGI("Number of processors: %d", num_processors_);

  for (int i = 0; i < num_processors_; ++i) {
    directory_manager_.MakeDirectories(
        base::StringPrintf("%scpu%d", path_.c_str(), i));
  }

  static const char* kFiles[] =
    { "kernel_max", "offline", "online", "possible", "present" };
  for (size_t i = 0; i < arraysize(kFiles); ++i) {
    bool result = directory_manager_.AddFile(path_ + kFiles[i]);
    ALOG_ASSERT(result);
  }

  is_initialized_ = true;
}

Dir* CpuFileHandler::OnDirectoryContentsNeeded(const std::string& name) {
  return directory_manager_.OpenDirectory(name);
}

void CpuFileHandler::OnMounted(const std::string& path) {
  ALOG_ASSERT(util::EndsWithSlash(path));
  path_ = path;
}

scoped_refptr<FileStream> CpuFileHandler::open(
    int fd, const std::string& pathname, int oflag, mode_t cmode) {
  if ((oflag & O_ACCMODE) != O_RDONLY) {
    errno = EACCES;
    return NULL;
  }
  if (directory_manager_.StatDirectory(pathname))
    return new DirectoryFileStream("cpu", pathname, this);
  if (!directory_manager_.StatFile(pathname)) {
    errno = ENOENT;
    return NULL;
  }

  // Emulate Linux kernel's behavior as much as possible. See
  // https://www.kernel.org/doc/Documentation/cputopology.txt

  CpuFileContent* content = NULL;
  if (EndsWith(pathname, "kernel_max", true)) {
    content = new KernelMaxFileContent;
  } else if (EndsWith(pathname, "offline", true)) {
    content = new OfflineFileContent(num_processors_);
  } else if (EndsWith(pathname, "online", true)) {
    content = new OnlineFileContent;
  } else if (EndsWith(pathname, "possible", true) ||
             EndsWith(pathname, "present", true)) {
    content = new PresentFileContent(num_processors_);
  }
  ALOG_ASSERT(content, "Unhandled path: %s", pathname.c_str());

  return new CpuFile(pathname, content);
}

int CpuFileHandler::stat(const std::string& pathname, struct stat* out) {
  scoped_refptr<FileStream> file = this->open(-1, pathname, O_RDONLY, 0);
  if (!file) {
    errno = ENOENT;
    return -1;
  }
  return file->fstat(out);
}

int CpuFileHandler::statfs(const std::string& pathname, struct statfs* out) {
  return DoStatFsForSys(out);
}

}  // namespace posix_translation
