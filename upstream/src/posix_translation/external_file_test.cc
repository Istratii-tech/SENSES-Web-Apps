// Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "posix_translation/external_file.h"

#include <algorithm>
#include <set>
#include <vector>

#include "base/basictypes.h"
#include "base/compiler_specific.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "gtest/gtest.h"
#include "posix_translation/test_util/file_system_background_test_common.h"
#include "posix_translation/test_util/file_system_test_common.h"
#include "posix_translation/test_util/mock_virtual_file_system.h"
#include "posix_translation/test_util/mock_file_handler.h"
#include "posix_translation/virtual_file_system.h"

namespace posix_translation {

namespace {

const char kExternalFilesDir[] = "/a/bb/ccc/dddd";
const char kExternalDirPath[] = "/aa/bb/ccc/";

// Puts all directory entries in |stream| into |out|. |stream| must be a
// directory stream. |out| is sorted and does not contain "." and "..".
void ReadDirectoryEntries(scoped_refptr<FileStream> stream,
                          std::vector<std::string>* out) {
  const int kSize = 1024;
  out->clear();
  int read_len = 0;
  uint8_t buffer[kSize];
  do {
    read_len = stream->getdents(reinterpret_cast<dirent*>(buffer), kSize);
    for (int i = 0; i < read_len;) {
      struct dirent* p = reinterpret_cast<struct dirent*>(buffer + i);
      std::string file = p->d_name;
      if (file != "." && file != "..")  // Skip "." and ".."
        out->push_back(p->d_name);
      i += p->d_reclen;
    }
  } while (read_len > 0);
  std::sort(out->begin(), out->end());
}

}  // namespace

typedef FileSystemTestCommon ExternalFileTest;

class TestableExternalFileHandler : public ExternalFileHandler {
 public:
  TestableExternalFileHandler(scoped_ptr<pp::FileSystem> file_system,
                              const std::string& ppapi_file_path,
                              const std::string& virtual_file_path)
      : ExternalFileHandler(file_system.Pass(), ppapi_file_path,
                            virtual_file_path) {}
  using ExternalFileHandler::GetExternalPPAPIPath;
};

// A mock observer for ExternalDirectoryHandler.
class MockObserver : public ExternalDirectoryHandler::Observer {
 public:
  MockObserver() : on_initializing_call_count_(0), handler_(NULL) {}
  virtual ~MockObserver() {}

  virtual void OnInitializing() OVERRIDE {
    on_initializing_call_count_++;
    if (handler_) {
      base::AutoUnlock unlock(
          VirtualFileSystem::GetVirtualFileSystem()->mutex());
      handler_->SetPepperFileSystem(
          make_scoped_ptr(new pp::FileSystem()),
          "/Documents", kExternalDirPath);
    }
  }

  // Caller must free |handler|.
  void SetHandler(ExternalDirectoryHandler* handler) {
    handler_ = handler;
  }

  int on_initializing_call_count() const { return on_initializing_call_count_; }

 private:
  int on_initializing_call_count_;
  ExternalDirectoryHandler* handler_;
};

class ExternalDirectoryTest
    : public FileSystemBackgroundTestCommon<ExternalDirectoryTest> {
 public:
  DECLARE_BACKGROUND_TEST(ConstructDestructTest);
  DECLARE_BACKGROUND_TEST(InitializeTest);
};

TEST_F(ExternalFileTest, TestConstructDestruct) {
  // ExternalFileTest ctor tries to acquire the mutex.
  base::AutoUnlock unlock(file_system_->mutex());

  scoped_ptr<ExternalFileHandler> handler;
  handler.reset(new TestableExternalFileHandler(
      make_scoped_ptr(new pp::FileSystem()),
      "/some_file.txt",
      "/some/path/in/vfs/file.txt"));
  handler.reset();
}

TEST_F(ExternalFileTest, GetExternalPPAPIPath) {
  // "\xEF\xBF\xBD": U+FFFE(REPLACEMENT CHARACTER)
  // "\xF0\xA0\x80\x8B": U+2000B(surrogate pair)
  // "\xE2\x80\x8F": U+200F(RIGHT-TO-LEFT MARK)
  // "\xEF\xBC\x8F": U+FF0F(FULLWIDTH SOLIDUS)
  const std::string kDangerousUnicodes =
      "\xEF\xBF\xBD\xF0\xA0\x80\x8B\xE2\x80\x8F\xEF\xBC\x8F";

  // ExternalFileTest ctor tries to acquire the mutex.
  base::AutoUnlock unlock(file_system_->mutex());
  {
    SCOPED_TRACE("External path is regular file.");
    TestableExternalFileHandler handler(
        make_scoped_ptr(new pp::FileSystem()),
        "/regular.txt",
        "/vendor/chromium/.external/1/regular.txt");

    EXPECT_EQ("/regular.txt",
              handler.GetExternalPPAPIPath(
                  "/vendor/chromium/.external/1/regular.txt"));
  }
  {
    SCOPED_TRACE("External path is directory");
    TestableExternalFileHandler handler(
        make_scoped_ptr(new pp::FileSystem()),
        "/directory/", "/sdcard/external/");
    EXPECT_EQ("/directory/",
              handler.GetExternalPPAPIPath("/sdcard/external/"));
    EXPECT_EQ("/directory/",
              handler.GetExternalPPAPIPath("/sdcard/external"));
    EXPECT_EQ("/directory/regular.txt",
              handler.GetExternalPPAPIPath("/sdcard/external/regular.txt"));
    EXPECT_EQ("/directory/sub/regular.txt",
              handler.GetExternalPPAPIPath("/sdcard/external/sub/regular.txt"));
    EXPECT_EQ("/directory/" + kDangerousUnicodes + "/regular.txt",
              handler.GetExternalPPAPIPath(
                  "/sdcard/external/" + kDangerousUnicodes + "/regular.txt"));
  }
  {
    SCOPED_TRACE("External path is root.");
    TestableExternalFileHandler handler(
        make_scoped_ptr(new pp::FileSystem()),
        "/", "/sdcard/external/");

    EXPECT_EQ("/",
              handler.GetExternalPPAPIPath("/sdcard/external/"));
    EXPECT_EQ("/",
              handler.GetExternalPPAPIPath("/sdcard/external"));
    EXPECT_EQ("/regular.txt",
              handler.GetExternalPPAPIPath("/sdcard/external/regular.txt"));
    EXPECT_EQ("/sub/regular.txt",
              handler.GetExternalPPAPIPath("/sdcard/external/sub/regular.txt"));
    EXPECT_EQ("/" + kDangerousUnicodes + "/regular.txt",
              handler.GetExternalPPAPIPath(
                  "/sdcard/external/" + kDangerousUnicodes + "/regular.txt"));
  }
  {
    SCOPED_TRACE("External path has unicode characters.");
    TestableExternalFileHandler handler(
        make_scoped_ptr(new pp::FileSystem()),
        "/" + kDangerousUnicodes + "/",
        "/sdcard/external/");
    EXPECT_EQ("/" + kDangerousUnicodes + "/",
              handler.GetExternalPPAPIPath("/sdcard/external/"));
    EXPECT_EQ("/" + kDangerousUnicodes + "/",
              handler.GetExternalPPAPIPath("/sdcard/external"));
    EXPECT_EQ("/" + kDangerousUnicodes + "/regular.txt",
              handler.GetExternalPPAPIPath("/sdcard/external/regular.txt"));
    EXPECT_EQ("/" + kDangerousUnicodes + "/sub/regular.txt",
              handler.GetExternalPPAPIPath(
                  "/sdcard/external/sub/regular.txt"));
  }
}

TEST_BACKGROUND_F(ExternalDirectoryTest, ConstructDestructTest) {
  scoped_ptr<ExternalDirectoryHandler> handler;
  MockObserver* observer = new MockObserver();
  handler.reset(new ExternalDirectoryHandler(kExternalDirPath, observer));

  EXPECT_EQ(0, observer->on_initializing_call_count());
  handler.reset();
}

TEST_BACKGROUND_F(ExternalDirectoryTest, InitializeTest) {
  scoped_ptr<ExternalDirectoryHandler> handler;
  MockObserver* observer = new MockObserver();
  handler.reset(new ExternalDirectoryHandler(kExternalDirPath, observer));
  observer->SetHandler(handler.get());

  EXPECT_EQ(0, observer->on_initializing_call_count());
  base::AutoLock lock(file_system_->mutex());
  handler->Initialize();
  EXPECT_EQ(1, observer->on_initializing_call_count());
  handler.reset();
}

class TestableExternalFileWrapperHandler : public ExternalFileWrapperHandler {
 public:
  TestableExternalFileWrapperHandler() : ExternalFileWrapperHandler(NULL) {}

  using ExternalFileWrapperHandler::GetSlot;
  using ExternalFileWrapperHandler::GenerateUniqueSlotLocked;
  using ExternalFileWrapperHandler::file_handlers_;
  using ExternalFileWrapperHandler::slot_file_map_;

  struct MountInfo {
    MountInfo(const std::string& ext, const std::string& vfs)
        : path_in_external_fs(ext), path_in_vfs(vfs) {}
    std::string path_in_external_fs;
    std::string path_in_vfs;
  };

  virtual scoped_ptr<FileSystemHandler> MountExternalFile(
      scoped_ptr<pp::FileSystem> file_system,
      const std::string& path_in_external_fs,
      const std::string& path_in_vfs) OVERRIDE {
    EXPECT_TRUE(file_system);
    mounts_.push_back(MountInfo(path_in_external_fs, path_in_vfs));
    return scoped_ptr<FileSystemHandler>(new MockFileHandler());
  }

  const std::vector<MountInfo>& mounts() {
    return mounts_;
  }

  void SetDelegate(Delegate* delegate) {
    delegate_.reset(delegate);
  }

 private:
  std::vector<MountInfo> mounts_;

  DISALLOW_COPY_AND_ASSIGN(TestableExternalFileWrapperHandler);
};

// ExternalFileWrapperTest must be usable on main thread, so do not test with
// BACKGROUND_TEST_F.
class ExternalFileWrapperTest
    : public FileSystemBackgroundTestCommon<ExternalFileWrapperTest> {
 public:
  virtual void SetUp() {
    FileSystemTestCommon::SetUp();
    handler_.reset(new TestableExternalFileWrapperHandler());
    handler_->OnMounted(std::string(kExternalFilesDir) + "/");
  }

  virtual void TearDown() {
    handler_->OnUnmounted(std::string(kExternalFilesDir) + "/");
    handler_.reset();
    FileSystemTestCommon::TearDown();
  }

 protected:
  scoped_ptr<TestableExternalFileWrapperHandler> handler_;
};

// A mock delegate for ExternalDirectoryHandler.
class MockExtFileWrapperDelegate : public ExternalFileWrapperHandler::Delegate {
 public:
  MockExtFileWrapperDelegate() : call_cnt_(0), target_writable_(false) {}
  virtual ~MockExtFileWrapperDelegate() {}

  virtual bool ResolveExternalFile(const std::string& path,
                                   pp::FileSystem** file_system,
                                   std::string* path_in_external_fs,
                                   bool* is_writable) OVERRIDE {
    ++call_cnt_;
    requested_path_ = path;

    if (path != target_path_) {
      return false;
    }

    *file_system = new pp::FileSystem();
    *path_in_external_fs = target_path_.substr(target_path_.find_last_of('/'));
    *is_writable = target_writable_;

    return true;
  }

  void SetTargetObject(const std::string& path, bool writable) {
    target_path_ = path;
    target_writable_ = writable;
    requested_path_ = "";
  }

  std::string GetRequestedPath() {
    // We reset requested_path_ here and next time when this function
    // calls it returns empty string if no ResolveExternalFile was called.
    // This way we return copy of current string.
    std::string requested_path = requested_path_;
    requested_path_ = "";
    return requested_path;
  }

  int call_cnt() const {
    return call_cnt_;
  }

 private:
  int call_cnt_;
  bool target_writable_;
  std::string target_path_;
  std::string requested_path_;
};


TEST_F(ExternalFileWrapperTest, ConstructDestructTest) {
  scoped_ptr<ExternalFileWrapperHandler> handler;
  handler.reset(new ExternalFileWrapperHandler(NULL));
  handler.reset();
}

TEST_F(ExternalFileWrapperTest, Mount_EmptyMountPoint) {
  const char kPathInExtFs[] = "/foo.txt";
  std::string mount_point =
      handler_->SetPepperFileSystem(
          make_scoped_ptr(new pp::FileSystem()), kPathInExtFs,
          std::string() /* assign new directory */);
  EXPECT_FALSE(mount_point.empty());
  EXPECT_TRUE(StartsWithASCII(mount_point, kExternalFilesDir, true));
  EXPECT_TRUE(EndsWith(mount_point, kPathInExtFs, true));

  ASSERT_EQ(1U, handler_->mounts().size());
  EXPECT_EQ(mount_point, handler_->mounts()[0].path_in_vfs);
  EXPECT_EQ(kPathInExtFs, handler_->mounts()[0].path_in_external_fs);

  ASSERT_EQ(1U, handler_->slot_file_map_.size());
  EXPECT_TRUE(
      StartsWithASCII(handler_->slot_file_map_.begin()->first, "/", true));
  EXPECT_EQ(std::string::npos,
            handler_->slot_file_map_.begin()->first.find('/', 1));
  EXPECT_EQ(kPathInExtFs,
            handler_->slot_file_map_.begin()->second);

  ASSERT_EQ(1U, handler_->file_handlers_.size());
}

TEST_F(ExternalFileWrapperTest, Mount_WithMountPoint) {
  const char kPathInExtFs[] = "/foo.txt";
  const char kMountPosition[] = "/a/bb/ccc/dddd/0ABE8364802/foo.txt";
  std::string mount_point = handler_->SetPepperFileSystem(
      make_scoped_ptr(new pp::FileSystem()),
      kPathInExtFs, kMountPosition);

  EXPECT_EQ(kMountPosition, mount_point);
  EXPECT_TRUE(StartsWithASCII(mount_point, kExternalFilesDir, true));
  EXPECT_TRUE(EndsWith(mount_point, kPathInExtFs, true));

  ASSERT_EQ(1U, handler_->mounts().size());
  EXPECT_EQ(mount_point, handler_->mounts()[0].path_in_vfs);
  EXPECT_EQ(kPathInExtFs, handler_->mounts()[0].path_in_external_fs);

  ASSERT_EQ(1U, handler_->slot_file_map_.size());
  EXPECT_TRUE(
      StartsWithASCII(handler_->slot_file_map_.begin()->first, "/", true));
  EXPECT_EQ(std::string::npos,
            handler_->slot_file_map_.begin()->first.find('/', 1));
  EXPECT_EQ(kPathInExtFs,
            handler_->slot_file_map_.begin()->second);

  ASSERT_EQ(1U, handler_->file_handlers_.size());
}

TEST_F(ExternalFileWrapperTest, GetSlotTest) {
  static const struct TestData {
    const char* input;
    const char* expected_slot;
  } kTestData[] = {
    // Success cases
    { "/a/bb/ccc/dddd/ABE8364802", "/ABE8364802" },
    { "/a/bb/ccc/dddd/938493948", "/938493948" },

    // Fail cases
    { "/a/bb/ccc/dddd/ABE8364802/", "" },
    { "/a/bb/ccc/dddd/ABE8/364802", "" },
    { "/a/bb/ccc/dddd/ABE8/364802/", "" },
    { "/a/bb/ccc/dddd/ABE8364802/foo/", "" },
    { "/a/bb/ccc/dddd/ABE8364802/foo.txt", "" },
    { "/a/bb/ccc/dddd", "" },
    { "/a/bb/ccc/dddd/", "" },
    { "/a/bb/ccc/dddd//", "" },
    { "/x/bb/ccc/dddd/938493948", "" },
  };

  for (size_t i = 0; i < ARRAYSIZE_UNSAFE(kTestData) ; ++i) {
    EXPECT_EQ(kTestData[i].expected_slot,
              handler_->GetSlot(kTestData[i].input))
        << kTestData[i].input;
  }
}

TEST_F(ExternalFileWrapperTest, GenerateUniqueSlot) {
  base::AutoLock lock(file_system_->mutex());
  const size_t kCount = 1000U;
  std::set<std::string> set_for_unique_check;
  for (size_t i = 0U; i < kCount; ++i) {
    set_for_unique_check.insert(handler_->GenerateUniqueSlotLocked());
  }
  EXPECT_EQ(kCount, set_for_unique_check.size());
}

TEST_F(ExternalFileWrapperTest, mkdir) {
  const std::string ext_files_dir = kExternalFilesDir;  // for easy appending.

  // root directory
  errno = 0;
  EXPECT_EQ(-1, handler_->mkdir(ext_files_dir, 0755));
  EXPECT_EQ(EEXIST, errno);

  const std::string path_in_extfs = "/foo.txt";

  for (int i = 0; i < 10; ++i) {
    std::string slot = base::StringPrintf("/%X", i);

    // before calling Mount, NOENT.
    errno = 0;
    EXPECT_EQ(-1, handler_->mkdir(ext_files_dir + slot, 0755));
    EXPECT_EQ(EPERM, errno);

    std::string mount_point = handler_->SetPepperFileSystem(
        make_scoped_ptr(new pp::FileSystem()), path_in_extfs,
        kExternalFilesDir + slot + path_in_extfs);

    // newly mounted slot directory.
    errno = 0;
    EXPECT_EQ(-1, handler_->mkdir(ext_files_dir + slot, 0755));
    EXPECT_EQ(EEXIST, errno);

    // previously mounted slots must be able to open.
    for (int j = 0; j < i; ++j) {
      std::string old_slot = base::StringPrintf("/%X", i);
      errno = 0;
      EXPECT_EQ(-1, handler_->mkdir(ext_files_dir + old_slot, 0755));
      EXPECT_EQ(EEXIST, errno);
    }
  }

  // unknown slot
  errno = 0;
  EXPECT_EQ(-1, handler_->mkdir(ext_files_dir + "/ABCDEF", 0755));
  EXPECT_EQ(EPERM, errno);

  // every path in correct slot.
  errno = 0;
  EXPECT_EQ(-1, handler_->mkdir(ext_files_dir + "/0/foo", 0755));
  EXPECT_EQ(EPERM, errno);

  errno = 0;
  EXPECT_EQ(-1, handler_->mkdir(ext_files_dir + "/0/bar", 0755));
  EXPECT_EQ(EPERM, errno);
}

TEST_F(ExternalFileWrapperTest, open) {
  base::AutoLock lock(file_system_->mutex());

  const int kUnusedFd = 10;
  const char kDirectoryStreamType[] = "external_file_dir";
  const std::string ext_files_dir = kExternalFilesDir;  // for easy appending.
  scoped_refptr<FileStream> stream;

  // root directory
  errno = 0;
  stream = handler_->open(kUnusedFd, ext_files_dir, 0, 0644);
  ASSERT_TRUE(stream.get());
  EXPECT_EQ(kDirectoryStreamType, std::string(stream->GetStreamType()));
  EXPECT_EQ(0, errno);

  const std::string path_in_extfs = "/foo.txt";

  for (int i = 0; i < 10; ++i) {
    std::string slot = base::StringPrintf("/%X", i);

    // before calling Mount, NOENT.
    errno = 0;
    stream = handler_->open(kUnusedFd, ext_files_dir + slot, 0, 0644);
    ASSERT_FALSE(stream.get());
    EXPECT_EQ(ENOENT, errno);

    {
      base::AutoUnlock unlock(file_system_->mutex());
      std::string mount_point = handler_->SetPepperFileSystem(
          make_scoped_ptr(new pp::FileSystem()), path_in_extfs,
          kExternalFilesDir + slot + path_in_extfs);
    }

    // newly mounted slot directory.
    errno = 0;
    stream = handler_->open(kUnusedFd, ext_files_dir + slot, 0, 0644);
    ASSERT_TRUE(stream.get());
    EXPECT_EQ(kDirectoryStreamType, std::string(stream->GetStreamType()));
    EXPECT_EQ(0, errno);

    // previously mounted slots must be able to open.
    for (int j = 0; j < i; ++j) {
      std::string old_slot = base::StringPrintf("/%X", i);
      errno = 0;
      stream = handler_->open(kUnusedFd, ext_files_dir + old_slot, 0, 0644);
      ASSERT_TRUE(stream.get());
      EXPECT_EQ(kDirectoryStreamType, std::string(stream->GetStreamType()));
      EXPECT_EQ(0, errno);
    }
  }
  // unknown slot
  errno = 0;
  stream = handler_->open(kUnusedFd, ext_files_dir + "/12345", 0, 0644);
  ASSERT_FALSE(stream.get());
  EXPECT_EQ(ENOENT, errno);

  // every path in correct slot. foo is mounted. Mock returned
  errno = 0;
  stream = handler_->open(kUnusedFd,
                          ext_files_dir + "/0/foo.txt", 0, 0644);
  ASSERT_TRUE(stream.get());
  EXPECT_EQ(0, errno);

  errno = 0;
  stream = handler_->open(kUnusedFd,
                          ext_files_dir + "/0/bar.txt", 0, 0644);
  ASSERT_FALSE(stream.get());
  EXPECT_EQ(ENOENT, errno);
}

TEST_F(ExternalFileWrapperTest, stat) {
  base::AutoLock lock(file_system_->mutex());
  const std::string ext_files_dir = kExternalFilesDir;  // for easy appending.

  struct stat st = {};
  // root directory
  errno = 0;
  EXPECT_EQ(0, handler_->stat(ext_files_dir, &st));
  EXPECT_EQ(0, errno);
  EXPECT_TRUE(st.st_mode & S_IFDIR);

  const std::string path_in_extfs = "/foo.txt";

  for (int i = 0; i < 10; ++i) {
    std::string slot = base::StringPrintf("/%X", i);

    // before calling Mount, NOENT.
    errno = 0;
    EXPECT_EQ(-1, handler_->stat(ext_files_dir + slot, &st));
    EXPECT_EQ(ENOENT, errno);

    {
      base::AutoUnlock unlock(file_system_->mutex());
      std::string mount_point = handler_->SetPepperFileSystem(
          make_scoped_ptr(new pp::FileSystem()), path_in_extfs,
          kExternalFilesDir + slot + path_in_extfs);
    }

    // newly mounted slot directory.
    errno = 0;
    st.st_mode = 0;
    EXPECT_EQ(0, handler_->stat(ext_files_dir + slot, &st));
    EXPECT_EQ(0, errno);
    EXPECT_TRUE(st.st_mode & S_IFDIR);

    // previously mounted slots must be able to open.
    for (int j = 0; j < i; ++j) {
      std::string old_slot = base::StringPrintf("/%X", i);
      errno = 0;
      st.st_mode = 0;
      EXPECT_EQ(0, handler_->stat(ext_files_dir + old_slot, &st));
      EXPECT_EQ(0, errno);
      EXPECT_TRUE(st.st_mode & S_IFDIR);
    }
  }

  // unknown slot
  errno = 0;
  EXPECT_EQ(-1, handler_->stat(ext_files_dir + "/ABCDEF", &st));
  EXPECT_EQ(ENOENT, errno);

  // every path in correct slot.
  errno = 0;
  EXPECT_EQ(-1, handler_->stat(ext_files_dir + "/0/foo", &st));
  EXPECT_EQ(ENOENT, errno);

  errno = 0;
  EXPECT_EQ(-1, handler_->stat(ext_files_dir + "/0/bar", &st));
  EXPECT_EQ(ENOENT, errno);
}

TEST_F(ExternalFileWrapperTest, statfs) {
  base::AutoLock lock(file_system_->mutex());
  const std::string ext_files_dir = kExternalFilesDir;  // for easy appending.

  struct statfs stfs = {};
  // root directory
  errno = 0;
  EXPECT_EQ(0, handler_->statfs(ext_files_dir, &stfs));
  EXPECT_EQ(0, errno);

  const std::string path_in_extfs = "/foo.txt";

  for (int i = 0; i < 10; ++i) {
    std::string slot = base::StringPrintf("/%X", i);

    // before calling Mount, NOENT.
    errno = 0;
    EXPECT_EQ(-1, handler_->statfs(ext_files_dir + slot, &stfs));
    EXPECT_EQ(ENOENT, errno);

    {
      base::AutoUnlock unlock(file_system_->mutex());
      std::string mount_point = handler_->SetPepperFileSystem(
          make_scoped_ptr(new pp::FileSystem()), path_in_extfs,
          kExternalFilesDir + slot + path_in_extfs);
    }

    // newly mounted slot directory.
    errno = 0;
    EXPECT_EQ(0, handler_->statfs(ext_files_dir + slot, &stfs));
    EXPECT_EQ(0, errno);

    // previously mounted slots must be able to open.
    for (int j = 0; j < i; ++j) {
      std::string old_slot = base::StringPrintf("/%X", i);
      errno = 0;
      EXPECT_EQ(0, handler_->statfs(ext_files_dir + old_slot, &stfs));
      EXPECT_EQ(0, errno);
    }
  }

  // unknown slot
  errno = 0;
  EXPECT_EQ(-1, handler_->statfs(ext_files_dir + "/ABCDEF", &stfs));
  EXPECT_EQ(ENOENT, errno);

  // every path in correct slot.
  errno = 0;
  EXPECT_EQ(-1, handler_->statfs(ext_files_dir + "/0/foo", &stfs));
  EXPECT_EQ(ENOENT, errno);

  errno = 0;
  EXPECT_EQ(-1, handler_->statfs(ext_files_dir + "/0/bar", &stfs));
  EXPECT_EQ(ENOENT, errno);
}

TEST_F(ExternalFileWrapperTest, getdents_root) {
  base::AutoLock lock(file_system_->mutex());

  const int kUnusedFd = 10;
  const std::string ext_files_dir = kExternalFilesDir;  // for easy appending.
  const std::string path_in_extfs = "/foo.txt";
  scoped_refptr<FileStream> stream;
  std::vector<std::string> dir_entries;

  for (size_t i = 0; i < 10; ++i) {
    std::string slot = base::StringPrintf("/%X", i);

    errno = 0;
    stream = handler_->open(kUnusedFd, ext_files_dir, 0, 0644);
    ASSERT_TRUE(stream.get());
    EXPECT_EQ(0, errno);
    ReadDirectoryEntries(stream, &dir_entries);
    EXPECT_EQ(i, dir_entries.size());
    for (size_t j = 0; j < i; ++j) {
      EXPECT_EQ(base::StringPrintf("%X", j), dir_entries[j]);
    }

    {
      base::AutoUnlock unlock(file_system_->mutex());
      std::string mount_point = handler_->SetPepperFileSystem(
          make_scoped_ptr(new pp::FileSystem()), path_in_extfs,
          kExternalFilesDir + slot + path_in_extfs);
    }

    errno = 0;
    stream = handler_->open(kUnusedFd, ext_files_dir, 0, 0644);
    ASSERT_TRUE(stream.get());
    EXPECT_EQ(0, errno);
    ReadDirectoryEntries(stream, &dir_entries);
    EXPECT_EQ(i + 1, dir_entries.size());
    for (size_t j = 0; j < i + 1; ++j) {
      EXPECT_EQ(base::StringPrintf("%X", j), dir_entries[j]);
    }
  }
}

TEST_F(ExternalFileWrapperTest, getdents_slot) {
  base::AutoLock lock(file_system_->mutex());

  const int kUnusedFd = 10;
  const std::string ext_files_dir = kExternalFilesDir;  // for easy appending.
  const std::string path_in_extfs = "/foo.txt";
  const std::string slot = "/987923847";
  scoped_refptr<FileStream> stream;
  std::vector<std::string> dir_entries;

  {
    base::AutoUnlock unlock(file_system_->mutex());
    std::string mount_point = handler_->SetPepperFileSystem(
        make_scoped_ptr(new pp::FileSystem()), path_in_extfs,
        kExternalFilesDir + slot + path_in_extfs);
  }

  errno = 0;
  stream = handler_->open(kUnusedFd, ext_files_dir + slot, 0, 0644);
  ASSERT_TRUE(stream.get());
  EXPECT_EQ(0, errno);
  ReadDirectoryEntries(stream, &dir_entries);
  EXPECT_EQ(1U, dir_entries.size());
  EXPECT_EQ("foo.txt", dir_entries[0]);
}

TEST_F(ExternalFileWrapperTest, resolvemount) {
  base::AutoLock lock(file_system_->mutex());

  const int kUnusedFd = 10;

  const std::string ext_files_dir = kExternalFilesDir;  // for easy appending.
  std::string slot = base::StringPrintf("/%X/", 1000);
  std::string testfile = ext_files_dir + slot + "test.txt";
  std::string testfile2 = ext_files_dir + slot + "test2.txt";

  struct stat st = {};
  scoped_refptr<FileStream> stream;

  st.st_mode = 0;
  errno = 0;
  EXPECT_EQ(-1, handler_->stat(testfile, &st));
  EXPECT_EQ(ENOENT, errno);
  stream = handler_->open(kUnusedFd, testfile, 0, 0644);
  ASSERT_FALSE(stream.get());
  EXPECT_EQ(ENOENT, errno);

  TestableExternalFileWrapperHandler* handler =
      reinterpret_cast<TestableExternalFileWrapperHandler*>(handler_.get());
  MockExtFileWrapperDelegate* delegate = new MockExtFileWrapperDelegate();
  handler->SetDelegate(delegate);

  errno = 0;
  EXPECT_EQ(-1, handler_->stat(testfile, &st));
  EXPECT_EQ(ENOENT, errno);

  EXPECT_EQ(1, delegate->call_cnt());
  EXPECT_EQ(testfile, delegate->GetRequestedPath());

  delegate->SetTargetObject(testfile, false);

  errno = 0;
  EXPECT_EQ(0, handler_->stat(testfile, &st));
  EXPECT_EQ(0, errno);

  EXPECT_EQ(2, delegate->call_cnt());
  EXPECT_EQ(testfile, delegate->GetRequestedPath());

  errno = 0;
  stream = handler_->open(kUnusedFd, testfile, 0, 0644);
  ASSERT_TRUE(stream.get());
  EXPECT_EQ(0, errno);

  // Result should be cached. No more calls to delegate.
  EXPECT_EQ(2, delegate->call_cnt());

  // Delegate is set but cannot resolve.
  errno = 0;
  EXPECT_EQ(-1, handler_->stat(testfile2, &st));
  EXPECT_EQ(ENOENT, errno);
  EXPECT_EQ(3, delegate->call_cnt());
  EXPECT_EQ(testfile2, delegate->GetRequestedPath());

  stream = handler_->open(kUnusedFd, testfile2, 0, 0644);
  ASSERT_FALSE(stream.get());
  EXPECT_EQ(ENOENT, errno);
  EXPECT_EQ(4, delegate->call_cnt());
  EXPECT_EQ(testfile2, delegate->GetRequestedPath());
}

}  // namespace posix_translation
