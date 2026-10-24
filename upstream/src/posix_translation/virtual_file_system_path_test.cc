// Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string.h>

#include "base/compiler_specific.h"
#include "base/memory/scoped_ptr.h"
#include "common/process_emulator.h"
#include "gtest/gtest.h"
#include "posix_translation/dir.h"
#include "posix_translation/path_util.h"
#include "posix_translation/test_util/file_system_background_test_common.h"
#include "posix_translation/test_util/virtual_file_system_test_common.h"
#include "posix_translation/virtual_file_system.h"

namespace posix_translation {

// Changes the user ID and sets it back to the original user ID when this
// object goes away.
class ScopedUidSetter {
 public:
  explicit ScopedUidSetter(uid_t uid)
      : original_uid_(arc::ProcessEmulator::GetUid()) {
    arc::ProcessEmulator::SetFallbackUidForTest(uid);
  }

  ~ScopedUidSetter() {
    arc::ProcessEmulator::SetFallbackUidForTest(original_uid_);
  }

 private:
  const uid_t original_uid_;
  DISALLOW_COPY_AND_ASSIGN(ScopedUidSetter);
};

namespace {

const time_t kTime = 1355707320;
const time_t kTime2 = 1355707399;

const mode_t kDirectoryMode = S_IFDIR | 0755;
const mode_t kRegularFileMode = S_IFREG | 0644;

// A stub implementation of FileStream.
class StubFileStream : public FileStream {
 public:
  StubFileStream()
      : FileStream(0, "") {
  }

  virtual ssize_t read(void*, size_t) OVERRIDE { return -1; }
  virtual ssize_t write(const void*, size_t) OVERRIDE { return -1; }
  virtual const char* GetStreamType() const OVERRIDE { return "stub"; }

  // Sets some dummy value. Used to verify that fstat() is called.
  virtual int fstat(struct stat* out) OVERRIDE {
    out->st_mode = S_IFREG | 0777;
    return 0;
  }
  virtual int fstatfs(struct statfs* out) OVERRIDE {
    memset(out, 0, sizeof *out);
    out->f_type = TMPFS_MAGIC;
    return 0;
  }
};

// A stub/fake-ish implementation of FileSystemHandler. This class maintains
// a map for entries, a map for symlinks, and a map for streams, so that
// functions like readlink() can have fake behaviors. Some functions just
// record parameters for verifycation purpose (ex. open()).
class TestFileSystemHandler : public FileSystemHandler {
 public:
  TestFileSystemHandler()
      : FileSystemHandler("TestFileSystemHandler"),
        mode_param_(-1),
        flags_param_(-1),
        length_param_(-1),
        times_param_() {
  }

  virtual scoped_refptr<FileStream> open(
      int fd, const std::string& path, int flags, mode_t mode) OVERRIDE {
    path_param_ = path;
    flags_param_ = flags;
    mode_param_ = mode;
    std::map<std::string, scoped_refptr<FileStream> >::const_iterator iter =
        stream_map_.find(path);
    if (iter != stream_map_.end())
      return iter->second;
    errno = ENOENT;
    return NULL;
  }
  virtual Dir* OnDirectoryContentsNeeded(const std::string&) OVERRIDE {
    return NULL;
  }

  virtual int mkdir(const std::string& path, mode_t mode) OVERRIDE {
    const std::string parent = util::GetDirName(path);
    std::map<std::string, mode_t>::const_iterator iter =
        entry_map_.find(parent);
    // Parent not found.
    if (iter == entry_map_.end()) {
      errno = ENOENT;
      return -1;
    }
    // Parent is not a directory
    if (!S_ISDIR(iter->second)) {
      errno = ENOTDIR;
      return -1;
    }
    AddEntry(path, S_IFDIR | mode);
    return 0;
  }

  virtual ssize_t readlink(const std::string& path,
                           std::string* resolved) OVERRIDE {
    std::map<std::string, std::string>::const_iterator iter =
        symlink_map_.find(path);
    if (iter != symlink_map_.end()) {
      *resolved = iter->second;
      return resolved->size();
    }

    errno = EINVAL;
    return -1;
  }

  virtual int rename(const std::string& oldpath,
                     const std::string& newpath) OVERRIDE {
    if (entry_map_.count(oldpath) == 0) {
      errno = ENOENT;
      return -1;
    }

    if (entry_map_.count(newpath) != 0) {
      errno = EEXIST;
      return 0;
    }

    entry_map_[newpath] = entry_map_[oldpath];
    entry_map_.erase(oldpath);
    return 0;
  }

  virtual int stat(const std::string& path, struct stat* out) OVERRIDE {
    std::string parent = path;
    while (parent != "/") {
      util::GetDirNameInPlace(&parent);
      std::map<std::string, mode_t>::const_iterator iter =
          entry_map_.find(parent);
      // Parent not found.
      if (iter == entry_map_.end()) {
        errno = ENOENT;
        return -1;
      }
      // Non-directory parent found.
      if (!S_ISDIR(iter->second)) {
        errno = ENOTDIR;
        return -1;
      }
    }

    memset(out, 0, sizeof(*out));
    std::map<std::string, mode_t>::const_iterator iter =
        entry_map_.find(path);
    if (iter != entry_map_.end()) {
      out->st_mode = iter->second;
      return 0;
    }
    errno = ENOENT;
    return -1;
  }

  // If |path| is known, returns the number of files.
  virtual int statfs(const std::string& path, struct statfs* out) OVERRIDE {
    if (entry_map_.count(path) != 0) {
      memset(out, 0, sizeof(*out));
      out->f_files = entry_map_.size();
      return 0;
    } else {
      errno = ENOENT;
      return -1;
    }
  }

  virtual int symlink(const std::string& oldpath,
                      const std::string& newpath) OVERRIDE {
    struct stat st;
    // Save errno because it can be changed by stat below.
    int old_errno = errno;
    if (symlink_map_.count(newpath) != 0 || stat(newpath, &st) == 0) {
      errno = EEXIST;
      return -1;
    }
    errno = old_errno;
    AddSymlink(newpath, oldpath);
    return 0;
  }

  // If |path| is known, succeeds. Records |length| for verfiication.
  virtual int truncate(const std::string& path, off64_t length) OVERRIDE {
    length_param_ = length;
    if (entry_map_.count(path) != 0) {
      return 0;
    } else {
      errno = EINVAL;
      return -1;
    }
  }


  // If |path| is known, removes it from the entry map.
  virtual int unlink(const std::string& path) OVERRIDE {
    if (entry_map_.count(path) != 0) {
      entry_map_.erase(path);
      return 0;
    } else {
      errno = ENOENT;
      return -1;
    }
  }

  // If |path| is known. Records |times| for verification.
  virtual int utimes(const std::string& path,
                     const struct timeval times[2]) OVERRIDE {
    times_param_[0] = times[0];
    times_param_[1] = times[1];
    if (entry_map_.count(path) != 0) {
      return 0;
    } else {
      errno = ENOENT;
      return -1;
    }
  }

  void AddSymlink(const std::string& from, const std::string& to) {
    symlink_map_[from] = to;
  }

  void AddStream(const std::string& path, scoped_refptr<FileStream> stream) {
    stream_map_[path] = stream;
    AddEntry(path, kRegularFileMode);
  }

  void AddEntry(const std::string& path, mode_t mode) {
    entry_map_[path] = mode;
  }

  std::string path_param_;
  mode_t mode_param_;
  int flags_param_;
  off64_t length_param_;
  struct timeval times_param_[2];

  std::map<std::string, mode_t> entry_map_;
  std::map<std::string, std::string> symlink_map_;
  std::map<std::string, scoped_refptr<FileStream> > stream_map_;
};

}  // namespace

// This class is used to test path-related functions in VirtualFileSystem,
// such as access(), chdir(), lstat(), readlink(), rename(), etc.
class FileSystemPathTest
    : public FileSystemBackgroundTestCommon<FileSystemPathTest> {
 public:
  DECLARE_BACKGROUND_TEST(TestGetNormalizedPathResolvingSymlinks);
  DECLARE_BACKGROUND_TEST(TestAccess);
  DECLARE_BACKGROUND_TEST(TestChangedDirectoryPath);
  DECLARE_BACKGROUND_TEST(TestClose);
  DECLARE_BACKGROUND_TEST(TestCloseBadFD);
  DECLARE_BACKGROUND_TEST(TestFstat);
  DECLARE_BACKGROUND_TEST(TestFstatBadFD);
  DECLARE_BACKGROUND_TEST(TestFstatClosedFD);
  DECLARE_BACKGROUND_TEST(TestFstatfs);
  DECLARE_BACKGROUND_TEST(TestFtruncateNegative);
  DECLARE_BACKGROUND_TEST(TestFtruncateBadFD);
  DECLARE_BACKGROUND_TEST(TestFtruncateClosedFD);
  DECLARE_BACKGROUND_TEST(TestLstat);
  DECLARE_BACKGROUND_TEST(TestLstat_RelativePath);
  DECLARE_BACKGROUND_TEST(TestLstat_NestedSymlinks);
  DECLARE_BACKGROUND_TEST(TestMkdir);
  DECLARE_BACKGROUND_TEST(TestMkdirFail);
  DECLARE_BACKGROUND_TEST(TestOpen);
  DECLARE_BACKGROUND_TEST(TestOpenDup2Close);
  DECLARE_BACKGROUND_TEST(TestOpenDupClose);
  DECLARE_BACKGROUND_TEST(TestOpenFail);
  DECLARE_BACKGROUND_TEST(TestReadLink);
  DECLARE_BACKGROUND_TEST(TestReadLink_RelativePath);
  DECLARE_BACKGROUND_TEST(TestReadLink_RelativeTargetPath);
  DECLARE_BACKGROUND_TEST(TestReadLink_NestedSymlinks);
  DECLARE_BACKGROUND_TEST(TestRealpath);
  DECLARE_BACKGROUND_TEST(TestRealpathWithBuf);
  DECLARE_BACKGROUND_TEST(TestRename);
  DECLARE_BACKGROUND_TEST(TestStat);
  DECLARE_BACKGROUND_TEST(TestStatFS);
  DECLARE_BACKGROUND_TEST(TestSymlink);
  DECLARE_BACKGROUND_TEST(TestTruncate);
  DECLARE_BACKGROUND_TEST(TestUnlink);
  DECLARE_BACKGROUND_TEST(TestUTime);
  DECLARE_BACKGROUND_TEST(TestUTimes);

 protected:
  typedef FileSystemBackgroundTestCommon<FileSystemPathTest> CommonType;

  virtual void SetUp() OVERRIDE {
    CommonType::SetUp();
    handler_.AddEntry("/", kDirectoryMode);
    AddMountPoint("/", &handler_);  // for realpath(".");
    errno = -1;
  }

  virtual void TearDown() OVERRIDE {
    ClearMountPoints();
    CommonType::TearDown();
  }

  const char* GetCurrentWorkingDirectory() {
    current_working_directory_.reset(file_system_->getcwd(NULL, 0));
    return current_working_directory_.get();
  }

  TestFileSystemHandler handler_;
  scoped_ptr<char, base::FreeDeleter> current_working_directory_;
};

TEST_BACKGROUND_F(FileSystemPathTest, TestGetNormalizedPathResolvingSymlinks) {
  base::AutoLock lock(mutex());
  handler_.AddSymlink("/link.file", "/test.file");
  handler_.AddSymlink("/test.dir/link.file", "/test.file");
  handler_.AddSymlink("/link.dir/link.file", "/test.file");
  handler_.AddSymlink("/link.dir", "/test.dir");
  handler_.AddSymlink("/test.dir/link.dir", "/test2.dir");

  EXPECT_EQ("/link.file",
            GetNormalizedPath("/link.file",
                              VirtualFileSystem::kDoNotResolveSymlinks));
  EXPECT_EQ("/link.file",
            GetNormalizedPath("/link.file",
                              VirtualFileSystem::kResolveParentSymlinks));
  EXPECT_EQ("/test.file",
            GetNormalizedPath("/link.file",
                              VirtualFileSystem::kResolveSymlinks));

  EXPECT_EQ("/test.dir/link.file",
            GetNormalizedPath("/test.dir/link.file",
                              VirtualFileSystem::kDoNotResolveSymlinks));
  EXPECT_EQ("/test.dir/link.file",
            GetNormalizedPath("/test.dir/link.file",
                              VirtualFileSystem::kResolveParentSymlinks));
  EXPECT_EQ("/test.file",
            GetNormalizedPath("/test.dir/link.file",
                              VirtualFileSystem::kResolveSymlinks));

  EXPECT_EQ("/link.dir/link.file",
            GetNormalizedPath("/link.dir/link.file",
                              VirtualFileSystem::kDoNotResolveSymlinks));
  EXPECT_EQ("/test.dir/link.file",
            GetNormalizedPath("/link.dir/link.file",
                              VirtualFileSystem::kResolveParentSymlinks));
  EXPECT_EQ("/test.file",
            GetNormalizedPath("/link.dir/link.file",
                              VirtualFileSystem::kResolveSymlinks));

  // Test '..' resolution.
  std::string test_path = "/link.dir/../link.dir";
  EXPECT_EQ("/link.dir",
            GetNormalizedPath(test_path,
                              VirtualFileSystem::kDoNotResolveSymlinks));
  EXPECT_EQ("/link.dir",
            GetNormalizedPath(test_path,
                              VirtualFileSystem::kResolveParentSymlinks));
  EXPECT_EQ("/test.dir",
            GetNormalizedPath(test_path,
                              VirtualFileSystem::kResolveSymlinks));

  test_path = "/link.dir/../link.dir/link.file";
  EXPECT_EQ("/link.dir/link.file",
            GetNormalizedPath(test_path,
                              VirtualFileSystem::kDoNotResolveSymlinks));
  EXPECT_EQ("/test.dir/link.file",
            GetNormalizedPath(test_path,
                              VirtualFileSystem::kResolveParentSymlinks));
  EXPECT_EQ("/test.file",
            GetNormalizedPath(test_path,
                              VirtualFileSystem::kResolveSymlinks));

  test_path = "/test.dir/link.dir/..";
  EXPECT_EQ("/test.dir",
            GetNormalizedPath(test_path,
                              VirtualFileSystem::kDoNotResolveSymlinks));
  EXPECT_EQ("/",
            GetNormalizedPath(test_path,
                              VirtualFileSystem::kResolveSymlinks));
  EXPECT_EQ("/",
            GetNormalizedPath(test_path,
                              VirtualFileSystem::kResolveParentSymlinks));

  // Test '.' resolution.
  EXPECT_EQ("/link.dir",
            GetNormalizedPath("/link.dir/.",
                              VirtualFileSystem::kDoNotResolveSymlinks));
  EXPECT_EQ("/link.dir",
            GetNormalizedPath("/link.dir/./",
                              VirtualFileSystem::kDoNotResolveSymlinks));
  EXPECT_EQ("/link.dir",
            GetNormalizedPath("/link.dir/.//",
                              VirtualFileSystem::kDoNotResolveSymlinks));
  EXPECT_EQ("/test.dir",
            GetNormalizedPath("/link.dir/.",
                              VirtualFileSystem::kResolveSymlinks));
  EXPECT_EQ("/test.dir",
            GetNormalizedPath("/link.dir/./",
                              VirtualFileSystem::kResolveSymlinks));
  EXPECT_EQ("/test.dir",
            GetNormalizedPath("/link.dir/.//",
                              VirtualFileSystem::kResolveSymlinks));
  EXPECT_EQ("/test.dir",
            GetNormalizedPath("/link.dir/.",
                              VirtualFileSystem::kResolveParentSymlinks));
  EXPECT_EQ("/test.dir",
            GetNormalizedPath("/link.dir/./",
                              VirtualFileSystem::kResolveParentSymlinks));
  EXPECT_EQ("/test.dir",
            GetNormalizedPath("/link.dir/.//",
                              VirtualFileSystem::kResolveParentSymlinks));
}

TEST_BACKGROUND_F(FileSystemPathTest, TestAccess) {
  handler_.AddEntry("/test.dir", kDirectoryMode);
  handler_.AddEntry("/test.file", kRegularFileMode);

  // Test as a system user.
  errno = 0;
  EXPECT_EQ(0, file_system_->access("/test.dir", F_OK));
  EXPECT_EQ(0, errno);

  errno = 0;
  EXPECT_EQ(0, file_system_->access("/test.dir", R_OK | W_OK | X_OK));
  EXPECT_EQ(0, errno);

  errno = 0;
  EXPECT_EQ(0, file_system_->access("/test.file", F_OK));
  EXPECT_EQ(0, errno);

  errno = 0;
  EXPECT_EQ(0, file_system_->access("/test.file", R_OK | W_OK));
  EXPECT_EQ(0, errno);

  // A file is not executable.
  errno = 0;
  EXPECT_EQ(-1, file_system_->access("/test.file", X_OK));
  EXPECT_EQ(EACCES, errno);
  errno = 0;

  // Test as an app.
  ScopedUidSetter setter(arc::kFirstAppUid);
  errno = 0;
  EXPECT_EQ(0, file_system_->access("/test.dir", F_OK));
  EXPECT_EQ(0, errno);

  errno = 0;
  EXPECT_EQ(0, file_system_->access("/test.dir", R_OK | X_OK));
  EXPECT_EQ(0, errno);

  // User cannot modify system directories.
  errno = 0;
  EXPECT_EQ(-1, file_system_->access("/test.dir", W_OK));
  EXPECT_EQ(EACCES, errno);

  errno = 0;
  EXPECT_EQ(0, file_system_->access("/test.dir", R_OK));
  EXPECT_EQ(0, errno);

  // User cannot write system files.
  errno = 0;
  EXPECT_EQ(-1, file_system_->access("/test.file", W_OK));
  EXPECT_EQ(EACCES, errno);

  // A file is not executable.
  errno = 0;
  EXPECT_EQ(-1, file_system_->access("/test.file", X_OK));
  EXPECT_EQ(EACCES, errno);
}

TEST_BACKGROUND_F(FileSystemPathTest, TestChangedDirectoryPath) {
  handler_.AddEntry("/", kDirectoryMode);
  handler_.AddEntry("/test.file", kRegularFileMode);
  handler_.AddEntry("/test.dir", kDirectoryMode);

  // Check if chdir("") fails with ENOENT.
  EXPECT_EQ(-1, file_system_->chdir(""));
  EXPECT_EQ(ENOENT, errno);
  EXPECT_STREQ("/", GetCurrentWorkingDirectory());

  // Check if chdir("/test.file") fails with ENOTDIR.
  errno = 0;
  EXPECT_EQ(-1, file_system_->chdir("/test.file"));
  EXPECT_EQ(ENOTDIR, errno);
  EXPECT_STREQ("/", GetCurrentWorkingDirectory());

  // Check if chdir("/test.dir") dir works
  EXPECT_EQ(0, file_system_->chdir("/test.dir"));
  EXPECT_STREQ("/test.dir", GetCurrentWorkingDirectory());

  // Check if chdir(".") succeeds with current directory.
  EXPECT_EQ(0, file_system_->chdir("."));
  EXPECT_STREQ("/test.dir", GetCurrentWorkingDirectory());

  // Reset the current directory.
  EXPECT_EQ(0, file_system_->chdir("/"));
  EXPECT_STREQ("/", GetCurrentWorkingDirectory());

  // Check if chdir("/test.dir/") works (with a trailing "/").
  EXPECT_EQ(0, file_system_->chdir("/test.dir" + std::string("/")));
  EXPECT_STREQ("/test.dir", GetCurrentWorkingDirectory());

  // Check if chdir("no-such-dir") fails, and the current directory does not
  // change.
  EXPECT_EQ(-1, file_system_->chdir("no-such-dir"));
  EXPECT_EQ(ENOENT, errno);
  EXPECT_STREQ("/test.dir", GetCurrentWorkingDirectory());

  // Reset the current directory.
  EXPECT_EQ(0, file_system_->chdir("/"));
  EXPECT_STREQ("/", GetCurrentWorkingDirectory());

  // Check if chdir("test.dir") works (chdir via a relative path).
  EXPECT_EQ(0, file_system_->chdir("test.dir"));
  EXPECT_STREQ("/test.dir", GetCurrentWorkingDirectory());

  // Reset the current directory.
  EXPECT_EQ(0, file_system_->chdir("/"));
  EXPECT_STREQ("/", GetCurrentWorkingDirectory());

  // Check if chdir("/test.dir////" works.
  EXPECT_EQ(0, file_system_->chdir("test.dir////"));
  EXPECT_STREQ("/test.dir", GetCurrentWorkingDirectory());

  // Reset the current directory.
  EXPECT_EQ(0, file_system_->chdir("/"));
  EXPECT_STREQ("/", GetCurrentWorkingDirectory());

  // Check if chdir("/test.dir/./") works.
  EXPECT_EQ(0, file_system_->chdir("/test.dir/./"));
  EXPECT_STREQ("/test.dir", GetCurrentWorkingDirectory());

  // Reset the current directory.
  EXPECT_EQ(0, file_system_->chdir("/"));
  EXPECT_STREQ("/", GetCurrentWorkingDirectory());

  // Check if chdir("/test.dir/././.") works.
  EXPECT_EQ(0, file_system_->chdir("/test.dir/././."));
  EXPECT_STREQ("/test.dir", GetCurrentWorkingDirectory());

  // Check if chdir("..") works.
  EXPECT_EQ(0, file_system_->chdir(".."));
  EXPECT_STREQ("/", GetCurrentWorkingDirectory());
}

TEST_BACKGROUND_F(FileSystemPathTest, TestClose) {
  handler_.AddStream("/test.file", new StubFileStream);
  int fd = file_system_->open("/test.file", O_RDONLY, 0);
  EXPECT_LE(0, fd);
  errno = 0;
  EXPECT_EQ(0, file_system_->close(fd));
  EXPECT_EQ(0, errno);
  EXPECT_ERROR(file_system_->close(fd), EBADF);
}

TEST_BACKGROUND_F(FileSystemPathTest, TestCloseBadFD) {
  EXPECT_ERROR(file_system_->close(-1), EBADF);
}

TEST_BACKGROUND_F(FileSystemPathTest, TestFstat) {
  handler_.AddStream("/test.file", new StubFileStream);
  int fd = file_system_->open("/test.file", O_RDONLY, 0);
  struct stat st = {};

  errno = 0;
  // Verify that StubFileStream::fstat() is called.
  EXPECT_EQ(0, file_system_->fstat(fd, &st));
  EXPECT_EQ(static_cast<mode_t>(S_IFREG | 0777), st.st_mode);
  EXPECT_EQ(0, errno);
  EXPECT_EQ(0, file_system_->close(fd));
}

TEST_BACKGROUND_F(FileSystemPathTest, TestFstatBadFD) {
  struct stat st, zerost;
  memset(&zerost, 0, sizeof(st));
  st = zerost;
  EXPECT_ERROR(file_system_->fstat(-1, &st), EBADF);
  EXPECT_EQ(0, memcmp(&zerost, &st, sizeof(st)));
}

TEST_BACKGROUND_F(FileSystemPathTest, TestFstatClosedFD) {
  handler_.AddStream("/test.file", new StubFileStream);
  int fd = file_system_->open("/test.file", O_RDONLY, 0);
  EXPECT_LE(0, fd);
  EXPECT_EQ(0, file_system_->close(fd));
  struct stat st;
  EXPECT_ERROR(file_system_->fstat(fd, &st), EBADF);
}

TEST_BACKGROUND_F(FileSystemPathTest, TestFstatfs) {
  handler_.AddStream("/test.file", new StubFileStream);
  int fd = file_system_->open("/test.file", O_RDONLY, 0);
  struct statfs st = {};

  errno = 0;
  EXPECT_EQ(0, file_system_->fstatfs(fd, &st));
  // Verify that StubFileStream::fstatfs() is called.
  EXPECT_EQ(uint32_t(TMPFS_MAGIC), st.f_type);
  EXPECT_EQ(0, errno);
  EXPECT_EQ(0, file_system_->close(fd));
}

TEST_BACKGROUND_F(FileSystemPathTest, TestFtruncateNegative) {
  EXPECT_ERROR(file_system_->ftruncate(-1, -123), EINVAL);
}

TEST_BACKGROUND_F(FileSystemPathTest, TestFtruncateBadFD) {
  EXPECT_ERROR(file_system_->ftruncate(-1, 0), EBADF);
}

TEST_BACKGROUND_F(FileSystemPathTest, TestFtruncateClosedFD) {
  handler_.AddStream("/test.file", new StubFileStream);
  int fd = file_system_->open("/test.file", O_RDWR, 0);
  EXPECT_EQ(0, file_system_->close(fd));
  EXPECT_ERROR(file_system_->ftruncate(fd, 0), EBADF);
}

TEST_BACKGROUND_F(FileSystemPathTest, TestLstat) {
  handler_.AddEntry("/test.file", S_IFREG);
  handler_.AddSymlink("/link.file", "/test.file");

  errno = 0;
  struct stat st;
  memset(&st, 1, sizeof(st));
  EXPECT_EQ(0, file_system_->lstat("/test.file", &st));
  EXPECT_EQ(0, errno);

  memset(&st, 1, sizeof(st));
  errno = 0;
  EXPECT_EQ(0, file_system_->lstat("/link.file", &st));
  EXPECT_EQ(S_IFLNK, static_cast<int>(st.st_mode & S_IFMT));
  EXPECT_EQ(0, errno);
}

TEST_BACKGROUND_F(FileSystemPathTest, TestLstat_RelativePath) {
  handler_.AddEntry("/test.dir", kDirectoryMode);
  handler_.AddSymlink("/test.dir/link.file", "/test.file");

  EXPECT_EQ(0, file_system_->chdir("/test.dir"));

  // Confirm that lstat() works with a relative path.
  struct stat st;
  memset(&st, 1, sizeof(st));
  EXPECT_EQ(0, file_system_->lstat("link.file", &st));
  EXPECT_EQ(S_IFLNK, static_cast<int>(st.st_mode & S_IFMT));
  EXPECT_EQ(0, errno);
}

TEST_BACKGROUND_F(FileSystemPathTest, TestLstat_NestedSymlinks) {
  handler_.AddEntry("/test.dir", kDirectoryMode);
  handler_.AddSymlink("/link.dir", "/test.dir");
  handler_.AddSymlink("/test.dir/link.file", "/test.file");

  // Confirm that lstat() works with nested symlinks.
  struct stat st;
  memset(&st, 1, sizeof(st));
  EXPECT_EQ(0, file_system_->lstat("/link.dir/link.file", &st));
  EXPECT_EQ(S_IFLNK, static_cast<int>(st.st_mode & S_IFMT));
  EXPECT_EQ(0, errno);
}

TEST_BACKGROUND_F(FileSystemPathTest, TestMkdir) {
  ScopedUidSetter setter(arc::kFirstAppUid);
  // Make "/test.dir" app-writable, to allow mkdir() on this path.
  ChangeMountPointOwner("/test.dir", arc::kFirstAppUid);

  // "/test.dir" should be created as expected.
  errno = 0;
  EXPECT_EQ(0, file_system_->mkdir("/test.dir", 0777));
  EXPECT_EQ(0, errno);
  EXPECT_EQ(static_cast<mode_t>(S_IFDIR | 0777),
            handler_.entry_map_["/test.dir"]);

  // If the parent directory exists, mkdir should set EACCES to errno.
  handler_.AddEntry("/readonly.dir",  kDirectoryMode);
  EXPECT_EQ(-1, file_system_->mkdir("/readonly.dir/foo", 0777));
  EXPECT_EQ(EACCES, errno);
  errno = 0;

  // If the parent directory does not exist, mkdir should set ENOENT to errno.
  EXPECT_EQ(-1, file_system_->mkdir("/nonexistent.dir/bar", 0777));
  EXPECT_EQ(ENOENT, errno);
  errno = 0;
}

TEST_BACKGROUND_F(FileSystemPathTest, TestMkdirFail) {
  handler_.AddStream("/test.file", new StubFileStream);
  AddMountPoint("/test.file", &handler_);

  ScopedUidSetter setter(arc::kFirstAppUid);
  // Linux kernel prefers EEXIST over EACCES. We emulate the behavior.
  EXPECT_ERROR(file_system_->mkdir("/test.file", 0), EEXIST);
}

TEST_BACKGROUND_F(FileSystemPathTest, TestOpen) {
  handler_.AddStream("/test.file", new StubFileStream);
  errno = 0;
  int fd = file_system_->open("/test.file", O_RDONLY, 0);
  EXPECT_LE(0, fd);
  EXPECT_EQ(0, errno);
  EXPECT_EQ("/test.file", handler_.path_param_);
  EXPECT_EQ(O_RDONLY, handler_.flags_param_);
  EXPECT_EQ(static_cast<mode_t>(0), handler_.mode_param_);

  // If the path is empty, ENOENT should be returned.
  fd = file_system_->open("", O_RDONLY, 0);
  EXPECT_LE(-1, fd);
  EXPECT_EQ(ENOENT, errno);
  fd = file_system_->open("", O_WRONLY | O_CREAT, 0700);
  EXPECT_LE(-1, fd);
  EXPECT_EQ(ENOENT, errno);
}

TEST_BACKGROUND_F(FileSystemPathTest, TestOpenDup2Close) {
  handler_.AddStream("/test.file", new StubFileStream);

  int fd = file_system_->open("/test.file", O_RDWR | O_CREAT, 0);
  EXPECT_EQ(0, errno);
  EXPECT_EQ("/test.file", handler_.path_param_);
  EXPECT_EQ(O_RDWR | O_CREAT, handler_.flags_param_);
  EXPECT_EQ(static_cast<mode_t>(0), handler_.mode_param_);

  static const int kUnusedFd = 12345;  // large number
  int fd2 = file_system_->dup2(fd, kUnusedFd);
  EXPECT_EQ(kUnusedFd, fd2);
  EXPECT_EQ(0, errno);
  EXPECT_EQ(0, file_system_->close(fd));
  EXPECT_EQ(0, file_system_->close(fd2));
}

TEST_BACKGROUND_F(FileSystemPathTest, TestOpenDupClose) {
  handler_.AddStream("/test.file", new StubFileStream);

  int fd = file_system_->open("/test.file", O_RDWR | O_CREAT, 0);
  EXPECT_EQ(0, errno);
  EXPECT_EQ("/test.file", handler_.path_param_);
  EXPECT_EQ(O_RDWR | O_CREAT, handler_.flags_param_);
  EXPECT_EQ(static_cast<mode_t>(0), handler_.mode_param_);

  int fd2 = file_system_->dup(fd);
  EXPECT_NE(fd, fd2);
  EXPECT_EQ(0, errno);
  EXPECT_EQ(0, file_system_->close(fd2));
  EXPECT_EQ(0, file_system_->close(fd));
}

TEST_BACKGROUND_F(FileSystemPathTest, TestOpenFail) {
  // No stream is associated with "/test.file".
  EXPECT_ERROR(file_system_->open("/test.file", O_RDONLY, 0), ENOENT);

  handler_.AddStream("/test.file", new StubFileStream);
  AddMountPoint("/test.file", &handler_);

  // open() will fail because "/test.file" is owned by the system UID, which
  // cannot be modified by the app UID.
  ScopedUidSetter setter(arc::kFirstAppUid);
  EXPECT_ERROR(file_system_->open("/test.file", O_RDWR | O_CREAT, 0), EACCES);
  EXPECT_ERROR(file_system_->open("/test.file", O_RDONLY | O_CREAT, 0), EACCES);
  // When O_CREAT|O_EXCL is specified, Linux kernel prefers EEXIST over EACCES.
  // We emulate the behavior.
  EXPECT_ERROR(file_system_->open("/test.file", O_RDONLY | O_CREAT | O_EXCL, 0),
               EEXIST);
  EXPECT_ERROR(file_system_->open("/test.file", O_RDONLY | O_TRUNC, 0), EACCES);
}

TEST_BACKGROUND_F(FileSystemPathTest, TestReadLink) {
  handler_.AddSymlink("/link.file", "/test.file");
  handler_.AddEntry("/test.file", kRegularFileMode);

  char buf[64];
  errno = 0;
  ssize_t len = file_system_->readlink("/link.file", buf, 63);
  ASSERT_EQ(strlen("/test.file"), static_cast<size_t>(len));
  EXPECT_EQ(0, errno);
  buf[len] = '\0';
  EXPECT_STREQ("/test.file", buf);

  // The buffer size is too small.
  buf[5] = 'X';  // Sentinel to make sure the result is actually truncated.
  len = file_system_->readlink("/link.file", buf, 5);
  EXPECT_EQ(0, errno);
  EXPECT_EQ(5, len);
  EXPECT_EQ('X', buf[5]);  // The trailing bytes should not be touched.
  buf[5] = '\0';  // '\0'-terminate to compare the buf as a string.
  EXPECT_STREQ("/test", buf);

  // The path is not a symbolic link.
  len = file_system_->readlink("/test.file", buf, 63);
  EXPECT_EQ(-1, len);
  EXPECT_EQ(EINVAL, errno);

  // The path does not exist.
  errno = 0;
  len = file_system_->readlink("/nonexistent.file", buf, 63);
  EXPECT_EQ(-1, len);
  EXPECT_EQ(ENOENT, errno);
}

TEST_BACKGROUND_F(FileSystemPathTest, TestReadLink_RelativePath) {
  handler_.AddEntry("/test.dir", kDirectoryMode);
  handler_.AddSymlink("/test.dir/link.file", "/test.file");

  // Move to "/test.dir".
  EXPECT_EQ(0, file_system_->chdir("/test.dir"));

  // Confirm that readlink() works with a relative path.
  struct stat st;
  memset(&st, 1, sizeof(st));
  char buf[64];
  errno = 0;
  ssize_t len = file_system_->readlink("link.file", buf, 63);
  buf[len] = '\0';
  EXPECT_STREQ("/test.file", buf);
  EXPECT_EQ(0, errno);
}

TEST_BACKGROUND_F(FileSystemPathTest, TestReadLink_RelativeTargetPath) {
  handler_.AddEntry("/test.dir", kDirectoryMode);
  handler_.AddSymlink("/test.dir/link.file", "../test.file");

  // Move to "/test.dir".
  EXPECT_EQ(0, file_system_->chdir("/test.dir"));

  // Confirm that readlink() works with a relative path.
  struct stat st;
  memset(&st, 1, sizeof(st));
  char buf[64];
  errno = 0;
  ssize_t len = file_system_->readlink("link.file", buf, 63);
  buf[len] = '\0';
  EXPECT_STREQ("../test.file", buf);
  EXPECT_EQ(0, errno);
}

TEST_BACKGROUND_F(FileSystemPathTest, TestReadLink_NestedSymlinks) {
  handler_.AddEntry("/test.dir", kDirectoryMode);
  handler_.AddSymlink("/link.dir", "/test.dir");
  handler_.AddSymlink("/test.dir/link.file", "/test.file");

  // Confirm that readlink() works nested symlinks
  struct stat st;
  memset(&st, 1, sizeof(st));
  char buf[64];
  ssize_t len = file_system_->readlink("/link.dir/link.file", buf, 63);
  buf[len] = '\0';
  EXPECT_STREQ("/test.file", buf);
}

TEST_BACKGROUND_F(FileSystemPathTest, TestRealpath) {
  handler_.AddEntry("/test.file", kRegularFileMode);

  EXPECT_EQ(0, file_system_->chdir("/"));

  // Test if NULL is allowed.
  char* result = file_system_->realpath(NULL, NULL);
  ASSERT_TRUE(result == NULL);
  result = file_system_->realpath("", NULL);
  ASSERT_TRUE(result == NULL);
  result = file_system_->realpath("/test.file", NULL);
  ASSERT_TRUE(result != NULL);
  EXPECT_STREQ("/test.file", result);
  free(result);  // confirm this does not crash.

  // Check that the function normalize dot(s).
  result = file_system_->realpath(".", NULL);
  EXPECT_TRUE(result != NULL);
  free(result);  // confirm this does not crash.
  result = file_system_->realpath(("/." + std::string("/test.file")).c_str(),
                                  NULL);
  ASSERT_TRUE(result != NULL);
  EXPECT_STREQ("/test.file", result);
  free(result);  // confirm this does not crash.
}

TEST_BACKGROUND_F(FileSystemPathTest, TestRealpathWithBuf) {
  handler_.AddEntry("/test.file", kRegularFileMode);

  // Confirm that non-NULL buffer is also allowed.
  char buf[PATH_MAX];
  char* result = file_system_->realpath("/test.file", buf);
  ASSERT_EQ(buf, result);
  EXPECT_STREQ("/test.file", result);

  // Check that the function normalize dots.
  result = file_system_->realpath(
      ("/." + std::string("/test.file")).c_str(), buf);
  ASSERT_EQ(buf, result);
  EXPECT_STREQ("/test.file", result);
  result = file_system_->realpath(("/./." + std::string("/test.file")).c_str(),
                                  buf);
  ASSERT_EQ(buf, result);
  EXPECT_STREQ("/test.file", result);
}

TEST_BACKGROUND_F(FileSystemPathTest, TestRename) {
  ScopedUidSetter setter(arc::kFirstAppUid);

  handler_.AddEntry("/readonly.dir", kDirectoryMode);
  handler_.AddStream("/test.file", new StubFileStream);
  AddMountPoint("/test.file", &handler_);

  // This mount point will be unmounted in TearDown().
  AddMountPoint("/test.new", &handler_);
  // Make the following paths writable, to allow rename() on these paths.
  ChangeMountPointOwner("/test.file", arc::kFirstAppUid);
  ChangeMountPointOwner("/test.new", arc::kFirstAppUid);

  // Before the rename, "/test.file" should exist but "/test.new" should not.
  EXPECT_EQ(1u, handler_.entry_map_.count("/test.file"));
  EXPECT_EQ(0u, handler_.entry_map_.count("/test.new"));

  EXPECT_EQ(0, file_system_->rename("/test.file", "/test.new"));
  EXPECT_EQ(0, errno);

  // After the rename, "/test.file" should not exist but "/test.new" should.
  EXPECT_EQ(0u, handler_.entry_map_.count("/test.file"));
  EXPECT_EQ(1u, handler_.entry_map_.count("/test.new"));

  // Rename it back to "/test.file" as it's referenced later.
  ASSERT_EQ(0, file_system_->rename("/test.new", "/test.file"));

  // If the old path does not exist, rename should set ENOENT to errno.
  EXPECT_EQ(-1, file_system_->rename("/readonly.dir/old", "/readonly.dir/new"));
  EXPECT_EQ(ENOENT, errno);

  // If the old path and the parent path exist, rename should set EACCES to
  // errno.
  errno = 0;
  handler_.AddEntry("/readonly.dir/old", kRegularFileMode);
  EXPECT_EQ(-1, file_system_->rename("/readonly.dir/old", "/readonly.dir/new"));
  EXPECT_EQ(EACCES, errno);

  // If the parent of the old path does not exist, rename should set ENOENT
  // to errno.
  errno = 0;
  EXPECT_EQ(-1, file_system_->rename("/nonexistent.dir/old",
                                     "/readonly.dir/new"));
  EXPECT_EQ(ENOENT, errno);

  // ENOTDIR is preferred to ENOENT. Here, ENOTDIR should be raised because
  // "/test.file" in the old path is not a directory.
  errno = 0;
  EXPECT_EQ(-1, file_system_->rename("/test.file/old", "/readonly.dir/new"));
  EXPECT_EQ(ENOTDIR, errno);

  // Likewise, ENOTDIR should be raised because "/test.file" in the new path
  // is not a directory.
  // TODO(crbug.com/370788) However, this test does not pass because
  // VirtualFileSystem::rename() does not handle this case correctly.
  // errno = 0;
  // EXPECT_EQ(-1, file_system_->rename("/readonly.dir/old", "/test.file/new"));
  // EXPECT_EQ(ENOTDIR, errno);

  // If |old_path| is empty, ENOENT should be returned.
  errno = 0;
  EXPECT_EQ(-1, file_system_->rename("", "/readonly.dir/new"));
  EXPECT_EQ(ENOENT, errno);

  // If |new_path| is empty, ENOENT should be returned too.
  EXPECT_EQ(-1, file_system_->rename("/readonly.dir/old", ""));
  EXPECT_EQ(ENOENT, errno);
}

TEST_BACKGROUND_F(FileSystemPathTest, TestStat) {
  handler_.AddEntry("/test.file", kRegularFileMode);
  handler_.AddSymlink("/link.file", "/test.file");

  struct stat st;
  memset(&st, 1, sizeof(st));
  errno = 0;
  EXPECT_EQ(0, file_system_->stat("/test.file", &st));
  EXPECT_EQ(0, errno);

  memset(&st, 1, sizeof(st));
  EXPECT_EQ(0, file_system_->stat("/link.file", &st));
  EXPECT_NE(S_IFLNK, static_cast<int>(st.st_mode & S_IFMT));
  EXPECT_EQ(0, errno);
}

TEST_BACKGROUND_F(FileSystemPathTest, TestStatFS) {
  struct statfs statfs = {};

  errno = 0;
  EXPECT_EQ(-1, file_system_->statfs("/nonexistent.file", &statfs));
  EXPECT_EQ(ENOENT, errno);

  // "/" always exists in the file system.
  errno = 0;
  EXPECT_EQ(0, file_system_->statfs("/", &statfs));
  // Because we have 1 entry (the root).
  EXPECT_EQ(1u, statfs.f_files);
  EXPECT_EQ(0, errno);
}

TEST_BACKGROUND_F(FileSystemPathTest, TestSymlink) {
  errno = 0;
  EXPECT_EQ(0, file_system_->symlink("/test.file", "/link.file"));
  EXPECT_EQ(0, errno);

  handler_.AddEntry("/test.file", kRegularFileMode);
  errno = 0;
  // test.dir doesn't exist.
  EXPECT_EQ(-1, file_system_->symlink("/test.file", "/test.dir/link1.file"));
  EXPECT_EQ(ENOENT, errno);

  // Access rights are ignored by root, so run tests below as normal user.
  ScopedUidSetter setter(arc::kFirstAppUid);
  EXPECT_EQ(0, handler_.mkdir("/test.dir", 0555));

  errno = 0;
  EXPECT_EQ(-1, file_system_->symlink("/test.file", "/test.dir/link2.file"));
  EXPECT_EQ(EACCES, errno);

  handler_.AddEntry("/test.dir/link3.file", kRegularFileMode);
  errno = 0;
  // Check that EEXIST has priority over EACCES.
  EXPECT_EQ(-1, file_system_->symlink("/test.file", "/test.dir/link3.file"));
  EXPECT_EQ(EEXIST, errno);
}

TEST_BACKGROUND_F(FileSystemPathTest, TestTruncate) {
  handler_.AddEntry("/readonly.file", kRegularFileMode);
  handler_.AddEntry("/test.file", kRegularFileMode);

  ScopedUidSetter setter(arc::kFirstAppUid);
  // Make "/test.file" app-writable, to allow tuncate() on this path.
  ChangeMountPointOwner("/test.file", arc::kFirstAppUid);

  EXPECT_EQ(0, file_system_->truncate("/test.file", 0));
  EXPECT_EQ(0, handler_.length_param_);

  // Do the same with non-zero |length|.
  errno = 0;
  EXPECT_EQ(0, file_system_->truncate("/test.file", 12345));
  EXPECT_EQ(12345, handler_.length_param_);
  EXPECT_EQ(0, errno);

  // If the read-only file eixsts, truncate() should set EACCES to errno.
  errno = 0;
  EXPECT_EQ(-1, file_system_->truncate("/readonly.file", 0777));
  EXPECT_EQ(EACCES, errno);

  // If the file does not exist, truncate should set ENOENT to errno.
  errno = 0;
  EXPECT_EQ(-1, file_system_->truncate("/nonexistent.file", 0777));
  EXPECT_EQ(ENOENT, errno);
}

TEST_BACKGROUND_F(FileSystemPathTest, TestUnlink) {
  handler_.AddEntry("/readonly.file", kRegularFileMode);
  handler_.AddEntry("/test.file", kRegularFileMode);

  ScopedUidSetter setter(arc::kFirstAppUid);
  // Make "/test.file" app-writable, to allow unlink() on this path.
  ChangeMountPointOwner("/test.file", arc::kFirstAppUid);

  errno = 0;
  EXPECT_EQ(0, file_system_->unlink("/test.file"));
  EXPECT_EQ(0, errno);

  // This time, unlink() should fail because /test.file is gone.
  errno = 0;
  EXPECT_EQ(-1, file_system_->unlink("/test.file"));
  EXPECT_EQ(ENOENT, errno);

  // If the read-only file exists, unlink should set EACCES to errno.
  errno = 0;
  EXPECT_EQ(-1, file_system_->unlink("/readonly.file"));
  EXPECT_EQ(EACCES, errno);

  // If the file does not exist, unlink should set ENOENT to errno.
  errno = 0;
  EXPECT_EQ(-1, file_system_->unlink("/nonexistent.file"));
  EXPECT_EQ(ENOENT, errno);
}

TEST_BACKGROUND_F(FileSystemPathTest, TestUTime) {
  handler_.AddEntry("/readonly.file", kRegularFileMode);
  handler_.AddEntry("/test.file", kRegularFileMode);

  ScopedUidSetter setter(arc::kFirstAppUid);
  // Make "/test.file" app-writable, to allow utime() on this path.
  ChangeMountPointOwner("/test.file", arc::kFirstAppUid);

  struct utimbuf time;
  time.actime = kTime;
  time.modtime = kTime2;
  // Expect the microseconds are 0.
  errno = 0;
  EXPECT_EQ(0, file_system_->utime("/test.file", &time));
  EXPECT_EQ(0, errno);
  EXPECT_EQ(kTime, handler_.times_param_[0].tv_sec);
  EXPECT_EQ(kTime2, handler_.times_param_[1].tv_sec);
  EXPECT_EQ(0, handler_.times_param_[0].tv_usec);
  EXPECT_EQ(0, handler_.times_param_[1].tv_usec);

  // If the read-only file exists, utime should set EACCES to errno.
  errno = 0;
  EXPECT_EQ(-1, file_system_->utime("/readonly.file", &time));
  EXPECT_EQ(EACCES, errno);

  // If the file does not exist, utime should set ENOENT to errno.
  errno = 0;
  EXPECT_EQ(-1, file_system_->utime("/nonexistent.file", &time));
  EXPECT_EQ(ENOENT, errno);
}

TEST_BACKGROUND_F(FileSystemPathTest, TestUTimes) {
  handler_.AddEntry("/readonly.file", kRegularFileMode);
  handler_.AddEntry("/test.file", kRegularFileMode);

  ScopedUidSetter setter(arc::kFirstAppUid);
  // Make "/test.file" app-writable, to allow utimes() on this path.
  ChangeMountPointOwner("/test.file", arc::kFirstAppUid);

  struct timeval times[2];
  times[0].tv_sec = kTime;
  times[0].tv_usec = 100;
  times[1].tv_sec = kTime2;
  times[1].tv_usec = 200;
  errno = 0;
  file_system_->utimes("/test.file", times);
  EXPECT_EQ(0, errno);
  EXPECT_EQ(kTime, handler_.times_param_[0].tv_sec);
  EXPECT_EQ(kTime2, handler_.times_param_[1].tv_sec);
  EXPECT_EQ(100, handler_.times_param_[0].tv_usec);
  EXPECT_EQ(200, handler_.times_param_[1].tv_usec);

  // If the read-only file exists, utimes should set EACCES to errno.
  errno = 0;
  EXPECT_EQ(-1, file_system_->utimes("/readonly.file", times));
  EXPECT_EQ(EACCES, errno);

  // If the file does not exist, utimes should set ENOENT to errno.
  errno = 0;
  EXPECT_EQ(-1, file_system_->utimes("/nonexistent.file", times));
  EXPECT_EQ(ENOENT, errno);
}

}  // namespace posix_translation
