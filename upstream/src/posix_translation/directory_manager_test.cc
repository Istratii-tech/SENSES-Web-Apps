// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/memory/scoped_ptr.h"
#include "gtest/gtest.h"
#include "posix_translation/dir.h"
#include "posix_translation/directory_manager.h"
#include "posix_translation/test_util/file_system_test_common.h"

namespace posix_translation {

class DirectoryManagerTest : public FileSystemTestCommon {
 protected:
  DirectoryManagerTest() {
  }
  virtual ~DirectoryManagerTest() {
  }

  DirectoryManager manager_;

 private:
  DISALLOW_COPY_AND_ASSIGN(DirectoryManagerTest);
};

TEST_F(DirectoryManagerTest, TestInitialState) {
  EXPECT_TRUE(manager_.StatDirectory(""));
  EXPECT_TRUE(manager_.StatDirectory("/"));
  EXPECT_FALSE(manager_.StatDirectory("/a"));
  EXPECT_FALSE(manager_.StatFile(""));
  EXPECT_FALSE(manager_.StatFile("/"));
  EXPECT_FALSE(manager_.StatFile("a"));
}

TEST_F(DirectoryManagerTest, TestMakeDirectories) {
  // Test if AddFile() automatically creates the directories, "/usr" and
  // "/usr/bin".
  EXPECT_TRUE(manager_.AddFile("/usr/bin/objdump"));
  EXPECT_TRUE(manager_.StatDirectory(""));
  EXPECT_TRUE(manager_.StatDirectory("/"));
  EXPECT_TRUE(manager_.StatDirectory("/usr"));
  EXPECT_TRUE(manager_.StatDirectory("/usr/"));
  EXPECT_TRUE(manager_.StatDirectory("/usr/bin"));
  EXPECT_TRUE(manager_.StatDirectory("/usr/bin/"));
  EXPECT_FALSE(manager_.StatDirectory("/usr/bi"));
  EXPECT_FALSE(manager_.StatDirectory("/usr/bin/o"));
  EXPECT_FALSE(manager_.StatDirectory("/usr/bin/objdump"));
  EXPECT_TRUE(manager_.StatFile("/usr/bin/objdump"));

  EXPECT_TRUE(manager_.AddFile("/usr/sbin/sshd"));
  EXPECT_TRUE(manager_.StatDirectory("/usr/sbin"));
  EXPECT_TRUE(manager_.StatDirectory("/usr/sbin/"));
  EXPECT_TRUE(manager_.StatFile("/usr/sbin/sshd"));
}

TEST_F(DirectoryManagerTest, TestAddRemoveFileBasic) {
  EXPECT_FALSE(manager_.AddFile("./"));  // relative path
  EXPECT_FALSE(manager_.AddFile("/"));  // not a file.
  EXPECT_TRUE(manager_.AddFile("/a.txt"));

  // Add "/a.txt".
  std::string content;
  EXPECT_TRUE(manager_.StatFile("/a.txt"));
  EXPECT_FALSE(manager_.StatFile("a.txt"));
  EXPECT_FALSE(manager_.StatFile("/b.txt"));

  // Add "/b.txt" too.
  EXPECT_TRUE(manager_.AddFile("/b.txt"));
  EXPECT_TRUE(manager_.StatFile("/b.txt"));

  // Read "/" directory.
  {
    static const DirectoryManager::FilesInDir* kNull = NULL;
    const DirectoryManager::FilesInDir* files = manager_.GetFilesInDir("/usr/");
    EXPECT_FALSE(files);
    files = manager_.GetFilesInDir("/");
    ASSERT_NE(kNull, files);
    EXPECT_EQ(2U, files->size());
    ASSERT_EQ(1U, files->count("a.txt"));  // not "/a.txt".
    ASSERT_EQ(1U, files->count("b.txt"));
  }

  // Remove "/a.txt".
  EXPECT_FALSE(manager_.RemoveFile("a.txt"));  // relative path.
  EXPECT_FALSE(manager_.RemoveFile("./a.txt"));  // relative path.
  EXPECT_FALSE(manager_.RemoveFile("/a.txt/"));  // directory
  EXPECT_TRUE(manager_.RemoveFile("/a.txt"));
  EXPECT_FALSE(manager_.StatFile("/a.txt"));

  // Read "/" directory again.
  {
    static const DirectoryManager::FilesInDir* kNull = NULL;
    const DirectoryManager::FilesInDir* files = manager_.GetFilesInDir("/usr/");
    EXPECT_EQ(kNull, files);
    files = manager_.GetFilesInDir("/");
    ASSERT_NE(kNull, files);
    EXPECT_EQ(1U, files->size());  // "a.txt" no longer exist.
    ASSERT_EQ(1U, files->count("b.txt"));
  }

  // Remove "/b.txt".
  EXPECT_TRUE(manager_.RemoveFile("/b.txt"));
  EXPECT_FALSE(manager_.StatFile("/b.txt"));

  // "/" still exists.
  EXPECT_TRUE(manager_.StatDirectory("/"));
  // Read "/" directory again.
  {
    static const DirectoryManager::FilesInDir* kNull = NULL;
    const DirectoryManager::FilesInDir* files = manager_.GetFilesInDir("/usr/");
    EXPECT_EQ(kNull, files);
    files = manager_.GetFilesInDir("/");
    ASSERT_NE(kNull, files);
    EXPECT_EQ(0U, files->size());  // all files are gone.
  }
}

TEST_F(DirectoryManagerTest, TestClear) {
  EXPECT_TRUE(manager_.StatDirectory("/"));
  EXPECT_TRUE(manager_.AddFile("/proc/1234/status"));
  EXPECT_TRUE(manager_.StatDirectory("/proc/1234"));
  EXPECT_TRUE(manager_.StatFile("/proc/1234/status"));

  manager_.Clear();
  EXPECT_TRUE(manager_.StatDirectory("/"));
  EXPECT_FALSE(manager_.StatDirectory("/proc"));
  EXPECT_FALSE(manager_.StatDirectory("/proc/1234"));
  EXPECT_FALSE(manager_.StatFile("/proc/1234/status"));

  EXPECT_TRUE(manager_.AddFile("/proc/1234/status"));
  EXPECT_TRUE(manager_.StatFile("/proc/1234/status"));
}

TEST_F(DirectoryManagerTest, TestMultipleAdds) {
  EXPECT_TRUE(manager_.StatDirectory("/"));
  EXPECT_TRUE(manager_.AddFile("/proc/1234/status"));
  EXPECT_TRUE(manager_.AddFile("/proc/1234/cmdline"));
  EXPECT_TRUE(manager_.StatDirectory("/proc/1234"));
  EXPECT_TRUE(manager_.StatFile("/proc/1234/status"));
  EXPECT_TRUE(manager_.StatFile("/proc/1234/cmdline"));
  {
    scoped_ptr<Dir> dirp(manager_.OpenDirectory("/proc"));
    dirent entry;
    EXPECT_TRUE(dirp->GetNext(&entry));
    EXPECT_EQ(std::string("."), entry.d_name);
    EXPECT_TRUE(dirp->GetNext(&entry));
    EXPECT_EQ(std::string(".."), entry.d_name);
    EXPECT_TRUE(dirp->GetNext(&entry));
    EXPECT_EQ(std::string("1234"), entry.d_name);
    EXPECT_FALSE(dirp->GetNext(&entry));
  }
}

TEST_F(DirectoryManagerTest, TestMakeRemoveDirectory) {
  // Removing the root is not allowed.
  EXPECT_FALSE(manager_.RemoveDirectory("/"));

  EXPECT_FALSE(manager_.RemoveDirectory("/foo"));  // does not exist
  manager_.MakeDirectories("/foo/bar");
  EXPECT_FALSE(manager_.RemoveDirectory("/foo"));  // not empty
  EXPECT_TRUE(manager_.RemoveDirectory("/foo/bar"));
  {
    static const DirectoryManager::FilesInDir* kNull = NULL;
    const DirectoryManager::FilesInDir* files = manager_.GetFilesInDir("/foo/");
    ASSERT_NE(kNull, files);
    EXPECT_EQ(0U, files->size());  // all entries in /foo are gone.
  }
  EXPECT_TRUE(manager_.AddFile("/foo/a.txt"));
  EXPECT_FALSE(manager_.RemoveDirectory("/foo"));  // not empty
  EXPECT_FALSE(manager_.RemoveDirectory("/foo/a.txt"));  // not a directory
  EXPECT_TRUE(manager_.RemoveFile("/foo/a.txt"));
  EXPECT_TRUE(manager_.RemoveDirectory("/foo"));  // now succeeds
  {
    static const DirectoryManager::FilesInDir* kNull = NULL;
    const DirectoryManager::FilesInDir* files = manager_.GetFilesInDir("/");
    ASSERT_NE(kNull, files);
    EXPECT_EQ(0U, files->size());  // all entries in / are gone.
  }
}

TEST_F(DirectoryManagerTest, TestGetFilesInDir) {
  // Test if AddFile() automatically creates the dir, "/usr/bin".
  EXPECT_TRUE(manager_.AddFile("/1"));
  EXPECT_TRUE(manager_.AddFile("/dir1/2"));
  EXPECT_TRUE(manager_.AddFile("/dir1/3"));
  EXPECT_TRUE(manager_.AddFile("/dir1/dir2/4"));
  EXPECT_TRUE(manager_.AddFile("/dir1/dir2/5"));
  EXPECT_TRUE(manager_.AddFile("/dir1/6"));
  EXPECT_TRUE(manager_.AddFile("/dir1/dir3/7"));
  // Add a file and then remove it.
  EXPECT_TRUE(manager_.AddFile("/dir1/8"));
  EXPECT_TRUE(manager_.RemoveFile("/dir1/8"));
  EXPECT_TRUE(manager_.AddFile("/dir4/9"));
  // This operation does not remove the directory "/dir4" itself.
  EXPECT_TRUE(manager_.RemoveFile("/dir4/9"));

  // Read "/dir1" directory. Confirm 1, 4, 5, and 7 are NOT returned.
  {
    static const DirectoryManager::FilesInDir* kNull = NULL;
    const DirectoryManager::FilesInDir* files =
        manager_.GetFilesInDir("/dir1/");
    ASSERT_NE(kNull, files);
    EXPECT_EQ(5U, files->size());
    EXPECT_EQ(1U, files->count("2"));
    EXPECT_EQ(1U, files->count("3"));
    EXPECT_EQ(1U, files->count("dir2/"));
    EXPECT_EQ(1U, files->count("6"));
    EXPECT_EQ(1U, files->count("dir3/"));
  }

  // Read "/" directory. Confirm only "1", "dir1/", and "dir4" are returned.
  {
    static const DirectoryManager::FilesInDir* kNull = NULL;
    const DirectoryManager::FilesInDir* files = manager_.GetFilesInDir("/");
    ASSERT_NE(kNull, files);
    EXPECT_EQ(3U, files->size());
    EXPECT_EQ(1U, files->count("1"));
    EXPECT_EQ(1U, files->count("dir1/"));
    EXPECT_EQ(1U, files->count("dir4/"));
  }

  // Read "/dir1/dir2" directory.
  {
    static const DirectoryManager::FilesInDir* kNull = NULL;
    const DirectoryManager::FilesInDir* files =
        manager_.GetFilesInDir("/dir1/dir2/");
    ASSERT_NE(kNull, files);
    EXPECT_EQ(2U, files->size());
    EXPECT_EQ(1U, files->count("4"));
    EXPECT_EQ(1U, files->count("5"));
  }

  // Read "/dir1/dir3" directory.
  {
    static const DirectoryManager::FilesInDir* kNull = NULL;
    const DirectoryManager::FilesInDir* files =
        manager_.GetFilesInDir("/dir1/dir3/");
    ASSERT_NE(kNull, files);
    EXPECT_EQ(1U, files->size());
    EXPECT_EQ(1U, files->count("7"));
  }

  // Read "/dir4" directory.
  {
    static const DirectoryManager::FilesInDir* kNull = NULL;
    const DirectoryManager::FilesInDir* files =
        manager_.GetFilesInDir("/dir4/");
    ASSERT_NE(kNull, files);
    EXPECT_EQ(0U, files->size());
  }
}

TEST_F(DirectoryManagerTest, TestOpenDirectory) {
  static const Dir* kNullDirp = NULL;

  EXPECT_TRUE(manager_.AddFile("/1"));
  EXPECT_TRUE(manager_.AddFile("/2"));
  EXPECT_TRUE(manager_.AddFile("/3"));
  // Open the dir, remove one file, and use the Dir object.
  scoped_ptr<Dir> dirp(manager_.OpenDirectory("/"));
  ASSERT_NE(kNullDirp, dirp.get());
  EXPECT_TRUE(manager_.RemoveFile("/2"));
  dirent entry;
  EXPECT_TRUE(dirp->GetNext(&entry));
  EXPECT_EQ(std::string("."), entry.d_name);
  EXPECT_TRUE(dirp->GetNext(&entry));
  EXPECT_EQ(std::string(".."), entry.d_name);
  EXPECT_TRUE(dirp->GetNext(&entry));
  EXPECT_EQ(std::string("1"), entry.d_name);
  EXPECT_TRUE(dirp->GetNext(&entry));
  // Since OpenDirectory is called before calling RemoveFile, "2" is returned.
  EXPECT_EQ(std::string("2"), entry.d_name);
  EXPECT_TRUE(dirp->GetNext(&entry));
  EXPECT_EQ(std::string("3"), entry.d_name);
  EXPECT_FALSE(dirp->GetNext(&entry));
  EXPECT_FALSE(dirp->GetNext(&entry));

  // Test error cases.
  dirp.reset(manager_.OpenDirectory("/a"));
  EXPECT_EQ(ENOENT, errno);
  EXPECT_EQ(kNullDirp, dirp.get());
  dirp.reset(manager_.OpenDirectory("/1"));
  EXPECT_EQ(ENOTDIR, errno);
  EXPECT_EQ(kNullDirp, dirp.get());
  dirp.reset(manager_.OpenDirectory("/2"));  // already removed file.
  EXPECT_EQ(ENOENT, errno);
  EXPECT_EQ(kNullDirp, dirp.get());
}

TEST_F(DirectoryManagerTest, TestOpenSubDirectory) {
  static const Dir* kNullDirp = NULL;
  EXPECT_TRUE(manager_.AddFile("/dir/1"));

  // Open "/dir" and scan the directory.
  scoped_ptr<Dir> dirp(manager_.OpenDirectory("/dir"));
  ASSERT_NE(kNullDirp, dirp.get());
  dirent entry;
  EXPECT_TRUE(dirp->GetNext(&entry));
  EXPECT_EQ(std::string("."), entry.d_name);
  EXPECT_TRUE(dirp->GetNext(&entry));
  EXPECT_EQ(std::string(".."), entry.d_name);
  const ino_t ino_1 = entry.d_ino;
  EXPECT_NE(0U, ino_1);
  EXPECT_TRUE(dirp->GetNext(&entry));
  EXPECT_EQ(std::string("1"), entry.d_name);
  EXPECT_FALSE(dirp->GetNext(&entry));

  // Open "/" and scan the directory.
  dirp.reset(manager_.OpenDirectory("/"));
  ASSERT_NE(kNullDirp, dirp.get());
  EXPECT_TRUE(dirp->GetNext(&entry));
  EXPECT_EQ(std::string("."), entry.d_name);
  const ino_t ino_2 = entry.d_ino;
  EXPECT_NE(0U, ino_2);

  // Compare inode numbers for "/." and "/dir/..". They should be the same.
  EXPECT_EQ(ino_1, ino_2);
}

TEST_F(DirectoryManagerTest, TestOpenDirectoryTooLongFileName) {
  static const Dir* kNullDirp = NULL;

  dirent e;
  EXPECT_TRUE(manager_.AddFile(
      "/" + std::string(sizeof(e.d_name) * 2, 'W')));
  EXPECT_TRUE(manager_.AddFile(
      "/" + std::string(sizeof(e.d_name) + 1, 'X')));
  EXPECT_TRUE(manager_.AddFile(
      "/" + std::string(sizeof(e.d_name), 'Y')));
  EXPECT_TRUE(manager_.AddFile(
      "/" + std::string(sizeof(e.d_name) - 1, 'Z')));
  scoped_ptr<Dir> dirp(manager_.OpenDirectory("/"));
  ASSERT_NE(kNullDirp, dirp.get());
  dirent entry;
  EXPECT_TRUE(dirp->GetNext(&entry));
  EXPECT_TRUE(dirp->GetNext(&entry));
  EXPECT_TRUE(dirp->GetNext(&entry));
  // Check that DirImpl is properly handling a too long d_name entry.
  EXPECT_EQ(std::string(sizeof(e.d_name) - 1, 'W'), entry.d_name);
  EXPECT_TRUE(dirp->GetNext(&entry));
  EXPECT_EQ(std::string(sizeof(e.d_name) - 1, 'X'), entry.d_name);
  EXPECT_TRUE(dirp->GetNext(&entry));
  EXPECT_EQ(std::string(sizeof(e.d_name) - 1, 'Y'), entry.d_name);
  EXPECT_TRUE(dirp->GetNext(&entry));
  EXPECT_EQ(std::string(sizeof(e.d_name) - 1, 'Z'), entry.d_name);
}

TEST_F(DirectoryManagerTest, TestSplitPath) {
  std::string test_str;
  EXPECT_EQ("", DirectoryManager::SplitPath(test_str).first);
  EXPECT_EQ("", DirectoryManager::SplitPath(test_str).second);

  test_str = "/";
  EXPECT_EQ("/", DirectoryManager::SplitPath(test_str).first);
  EXPECT_EQ("", DirectoryManager::SplitPath(test_str).second);

  test_str = "/a.txt";
  EXPECT_EQ("/", DirectoryManager::SplitPath(test_str).first);
  EXPECT_EQ("a.txt", DirectoryManager::SplitPath(test_str).second);

  test_str = "/a/b.txt";
  EXPECT_EQ("/a/", DirectoryManager::SplitPath(test_str).first);
  EXPECT_EQ("b.txt", DirectoryManager::SplitPath(test_str).second);

  test_str = "/a/b/";
  EXPECT_EQ("/a/b/", DirectoryManager::SplitPath(test_str).first);
  EXPECT_EQ("", DirectoryManager::SplitPath(test_str).second);

  test_str = "a/b/";
  EXPECT_EQ("a/b/", DirectoryManager::SplitPath(test_str).first);
  EXPECT_EQ("", DirectoryManager::SplitPath(test_str).second);

  test_str = "a.txt";
  EXPECT_EQ("", DirectoryManager::SplitPath(test_str).first);
  EXPECT_EQ("a.txt", DirectoryManager::SplitPath(test_str).second);
}

TEST_F(DirectoryManagerTest, TestOpenDirectoryAddEntryLater) {
  static const Dir* kNullDirp = NULL;

  EXPECT_TRUE(manager_.AddFile("/2"));
  // Open the dir, remove one file, and use the Dir object.
  scoped_ptr<Dir> dirp(manager_.OpenDirectory("/"));
  ASSERT_NE(kNullDirp, dirp.get());

  // Overwrite the entry.
  dirp->Add("2", Dir::SYMLINK);
  dirent entry;
  EXPECT_TRUE(dirp->GetNext(&entry));
  EXPECT_EQ(std::string("."), entry.d_name);
  EXPECT_TRUE(dirp->GetNext(&entry));
  EXPECT_EQ(std::string(".."), entry.d_name);
  EXPECT_TRUE(dirp->GetNext(&entry));
  EXPECT_EQ(std::string("2"), entry.d_name);
  EXPECT_EQ(DT_LNK, entry.d_type);  // no longer DT_REG.
  EXPECT_FALSE(dirp->GetNext(&entry));

  // Add an entry.
  dirp->rewinddir();
  dirp->Add("0", Dir::SYMLINK);
  EXPECT_TRUE(dirp->GetNext(&entry));
  EXPECT_EQ(std::string("."), entry.d_name);
  EXPECT_TRUE(dirp->GetNext(&entry));
  EXPECT_EQ(std::string(".."), entry.d_name);
  EXPECT_TRUE(dirp->GetNext(&entry));
  EXPECT_EQ(std::string("0"), entry.d_name);
  EXPECT_EQ(DT_LNK, entry.d_type);
  EXPECT_TRUE(dirp->GetNext(&entry));
  EXPECT_EQ(std::string("2"), entry.d_name);
  EXPECT_EQ(DT_LNK, entry.d_type);
  EXPECT_FALSE(dirp->GetNext(&entry));

  // Add an entry again.
  dirp->rewinddir();
  dirp->Add("1", Dir::REGULAR);
  EXPECT_TRUE(dirp->GetNext(&entry));
  EXPECT_EQ(std::string("."), entry.d_name);
  EXPECT_TRUE(dirp->GetNext(&entry));
  EXPECT_EQ(std::string(".."), entry.d_name);
  EXPECT_TRUE(dirp->GetNext(&entry));
  EXPECT_EQ(std::string("0"), entry.d_name);
  EXPECT_EQ(DT_LNK, entry.d_type);
  EXPECT_TRUE(dirp->GetNext(&entry));
  EXPECT_EQ(std::string("1"), entry.d_name);
  EXPECT_EQ(DT_REG, entry.d_type);
  EXPECT_TRUE(dirp->GetNext(&entry));
  EXPECT_EQ(std::string("2"), entry.d_name);
  EXPECT_EQ(DT_LNK, entry.d_type);
  EXPECT_FALSE(dirp->GetNext(&entry));
}

}  // namespace posix_translation
