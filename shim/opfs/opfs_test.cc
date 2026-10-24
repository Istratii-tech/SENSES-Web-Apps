// Browser tests for OPFS-backed /data (OpfsHandler) through plain libc.
//
// OPFS only exists in browsers, so this runs from build/opfs-test/index.html
// (see `make opfs-test`). Pass gtest flags as ?arg=...&arg=... in the URL.
//
// Persistence across page loads is checked in two runs:
//   ?arg=--opfs-phase=write    writes /data/persist/token
//   ?arg=--opfs-phase=verify   (after a reload) expects it to be there

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "kernel/arc_kernel.h"
#include "posix_translation/opfs_file.h"
#include "posix_translation/virtual_file_system.h"

namespace {

std::string g_phase;

const char kRoot[] = "/data/opfs-test";
const char kPersistDir[] = "/data/persist";
const char kPersistFile[] = "/data/persist/token";
const char kPersistToken[] = "arc-wasm persists across reloads\n";

// rm -rf with plain libc (exercises readdir/unlink/rmdir on the way).
void RemoveTree(const std::string& path) {
  struct stat st;
  if (lstat(path.c_str(), &st) != 0)
    return;
  if (S_ISDIR(st.st_mode)) {
    std::vector<std::string> names;
    if (DIR* dir = opendir(path.c_str())) {
      while (struct dirent* ent = readdir(dir)) {
        if (strcmp(ent->d_name, ".") && strcmp(ent->d_name, ".."))
          names.push_back(ent->d_name);
      }
      closedir(dir);
    }
    for (size_t i = 0; i < names.size(); ++i)
      RemoveTree(path + "/" + names[i]);
    rmdir(path.c_str());
  } else {
    unlink(path.c_str());
  }
}

std::string ReadAll(const std::string& path) {
  std::string out;
  FILE* f = fopen(path.c_str(), "rb");
  if (!f)
    return "<open failed: " + std::string(strerror(errno)) + ">";
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
    out.append(buf, n);
  fclose(f);
  return out;
}

std::string P(const char* name) { return std::string(kRoot) + "/" + name; }

class OpfsTest : public ::testing::Test {
 protected:
  virtual void SetUp() {
    RemoveTree(kRoot);
    ASSERT_EQ(0, mkdir(kRoot, 0755)) << strerror(errno);
  }
  virtual void TearDown() { RemoveTree(kRoot); }
};

TEST_F(OpfsTest, CreateWriteReadBack) {
  FILE* f = fopen(P("hello.txt").c_str(), "w");
  ASSERT_TRUE(f != NULL) << strerror(errno);
  EXPECT_GE(fputs("hello, opfs\n", f), 0);
  EXPECT_EQ(0, fclose(f));
  EXPECT_EQ("hello, opfs\n", ReadAll(P("hello.txt")));
  struct stat st;
  ASSERT_EQ(0, stat(P("hello.txt").c_str(), &st));
  EXPECT_TRUE(S_ISREG(st.st_mode));
  EXPECT_EQ(12, st.st_size);
}

TEST_F(OpfsTest, CppFstream) {
  {
    std::ofstream out(P("stream.txt"));
    ASSERT_TRUE(out.good());
    out << "line 1\nline 2\n";
  }
  std::ifstream in(P("stream.txt"));
  std::string a, b;
  std::getline(in, a);
  std::getline(in, b);
  EXPECT_EQ("line 1", a);
  EXPECT_EQ("line 2", b);
}

TEST_F(OpfsTest, AppendMode) {
  const int fd = open(P("log").c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
  ASSERT_GE(fd, 0) << strerror(errno);
  ASSERT_EQ(3, write(fd, "abc", 3));
  ASSERT_EQ(0, lseek(fd, 0, SEEK_SET));
  ASSERT_EQ(3, write(fd, "def", 3));  // Still appended.
  EXPECT_EQ(0, close(fd));
  EXPECT_EQ("abcdef", ReadAll(P("log")));
}

TEST_F(OpfsTest, SeekPreadPwriteAndHoles) {
  const int fd = open(P("data").c_str(), O_RDWR | O_CREAT, 0644);
  ASSERT_GE(fd, 0);
  ASSERT_EQ(10, write(fd, "0123456789", 10));
  ASSERT_EQ(2, pwrite(fd, "XY", 2, 4));
  char buf[16] = {0};
  ASSERT_EQ(10, pread(fd, buf, sizeof(buf), 0));
  EXPECT_STREQ("0123XY6789", buf);
  EXPECT_EQ(10, lseek(fd, 0, SEEK_CUR));  // pread/pwrite keep the offset.
  EXPECT_EQ(10, lseek(fd, 0, SEEK_END));
  ASSERT_EQ(20, lseek(fd, 10, SEEK_END));
  ASSERT_EQ(1, write(fd, "!", 1));  // Leaves a hole of zeros.
  struct stat st;
  ASSERT_EQ(0, fstat(fd, &st));
  EXPECT_EQ(21, st.st_size);
  ASSERT_EQ(1, pread(fd, buf, 1, 15));
  EXPECT_EQ(0, buf[0]);
  EXPECT_EQ(0, close(fd));
}

TEST_F(OpfsTest, Truncate) {
  const int fd = open(P("t").c_str(), O_RDWR | O_CREAT, 0644);
  ASSERT_GE(fd, 0);
  ASSERT_EQ(6, write(fd, "abcdef", 6));
  ASSERT_EQ(0, ftruncate(fd, 3));
  struct stat st;
  ASSERT_EQ(0, fstat(fd, &st));
  EXPECT_EQ(3, st.st_size);
  ASSERT_EQ(0, ftruncate(fd, 8));
  ASSERT_EQ(0, fstat(fd, &st));
  EXPECT_EQ(8, st.st_size);
  EXPECT_EQ(0, close(fd));
  ASSERT_EQ(0, truncate(P("t").c_str(), 2));
  EXPECT_EQ("ab", ReadAll(P("t")));
  // O_TRUNC on open.
  const int fd2 = open(P("t").c_str(), O_WRONLY | O_TRUNC);
  ASSERT_GE(fd2, 0);
  EXPECT_EQ(0, close(fd2));
  EXPECT_EQ("", ReadAll(P("t")));
}

TEST_F(OpfsTest, DirectoriesAndReaddir) {
  ASSERT_EQ(0, mkdir(P("a").c_str(), 0755));
  ASSERT_EQ(0, mkdir(P("a/b").c_str(), 0755));
  fclose(fopen(P("a/one").c_str(), "w"));
  fclose(fopen(P("a/two").c_str(), "w"));
  struct stat st;
  ASSERT_EQ(0, stat(P("a/b").c_str(), &st));
  EXPECT_TRUE(S_ISDIR(st.st_mode));

  DIR* dir = opendir(P("a").c_str());
  ASSERT_TRUE(dir != NULL) << strerror(errno);
  std::set<std::string> names;
  while (struct dirent* ent = readdir(dir))
    names.insert(ent->d_name);
  closedir(dir);
  const char* expected[] = {".", "..", "b", "one", "two"};
  EXPECT_EQ(std::set<std::string>(expected, expected + 5), names);

  EXPECT_EQ(-1, rmdir(P("a").c_str()));
  EXPECT_EQ(ENOTEMPTY, errno);
  EXPECT_EQ(0, unlink(P("a/one").c_str()));
  EXPECT_EQ(0, unlink(P("a/two").c_str()));
  EXPECT_EQ(0, rmdir(P("a/b").c_str()));
  EXPECT_EQ(0, rmdir(P("a").c_str()));
  EXPECT_EQ(-1, stat(P("a").c_str(), &st));
  EXPECT_EQ(ENOENT, errno);
}

TEST_F(OpfsTest, Rename) {
  fclose(fopen(P("old").c_str(), "w"));
  FILE* f = fopen(P("old").c_str(), "w");
  fputs("payload", f);
  fclose(f);
  ASSERT_EQ(0, rename(P("old").c_str(), P("new").c_str())) << strerror(errno);
  struct stat st;
  EXPECT_EQ(-1, stat(P("old").c_str(), &st));
  EXPECT_EQ("payload", ReadAll(P("new")));
  // Replacing an existing file.
  f = fopen(P("other").c_str(), "w");
  fputs("other", f);
  fclose(f);
  ASSERT_EQ(0, rename(P("other").c_str(), P("new").c_str()));
  EXPECT_EQ("other", ReadAll(P("new")));
}

TEST_F(OpfsTest, Errors) {
  EXPECT_EQ(-1, open(P("missing").c_str(), O_RDONLY));
  EXPECT_EQ(ENOENT, errno);
  fclose(fopen(P("f").c_str(), "w"));
  EXPECT_EQ(-1, open(P("f").c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644));
  EXPECT_EQ(EEXIST, errno);
  ASSERT_EQ(0, mkdir(P("d").c_str(), 0755));
  EXPECT_EQ(-1, open(P("d").c_str(), O_WRONLY));
  EXPECT_EQ(EISDIR, errno);
  EXPECT_EQ(-1, unlink(P("d").c_str()));
  EXPECT_EQ(EISDIR, errno);
  EXPECT_EQ(-1, mkdir(P("d").c_str(), 0755));
  EXPECT_EQ(EEXIST, errno);
  EXPECT_EQ(-1, rmdir(P("f").c_str()));
  EXPECT_EQ(ENOTDIR, errno);
  EXPECT_EQ(-1, open(P("f/x").c_str(), O_RDONLY));
}

TEST_F(OpfsTest, UnlinkWhileOpen) {
  // As POSIX: the name goes, the open descriptor keeps the data (the bridge
  // moves the file away; TDLib unlinks its open binlog so).
  const int fd = open(P("busy").c_str(), O_RDWR | O_CREAT, 0644);
  ASSERT_GE(fd, 0);
  ASSERT_EQ(5, write(fd, "hello", 5));
  EXPECT_EQ(0, unlink(P("busy").c_str())) << strerror(errno);
  struct stat st;
  EXPECT_EQ(-1, stat(P("busy").c_str(), &st));
  EXPECT_EQ(ENOENT, errno);
  // A new file of that name is another file.
  const int other = open(P("busy").c_str(), O_RDWR | O_CREAT, 0644);
  ASSERT_GE(other, 0);
  ASSERT_EQ(3, write(other, "new", 3));
  ASSERT_EQ(6, pwrite(fd, " world", 6, 5));
  char buf[16] = {};
  ASSERT_EQ(11, pread(fd, buf, sizeof(buf), 0));
  EXPECT_EQ("hello world", std::string(buf, 11));
  EXPECT_EQ(0, close(fd));
  EXPECT_EQ(0, close(other));
  EXPECT_EQ("new", ReadAll(P("busy")));
  EXPECT_EQ(0, unlink(P("busy").c_str()));
}

TEST_F(OpfsTest, RenameWhileOpen) {
  // Renaming an open file, and renaming over one (the binlog's rotation).
  const int a = open(P("a").c_str(), O_RDWR | O_CREAT, 0644);
  const int b = open(P("b").c_str(), O_RDWR | O_CREAT, 0644);
  ASSERT_GE(a, 0);
  ASSERT_GE(b, 0);
  ASSERT_EQ(3, write(a, "aaa", 3));
  ASSERT_EQ(3, write(b, "bbb", 3));
  EXPECT_EQ(0, rename(P("a").c_str(), P("b").c_str())) << strerror(errno);
  EXPECT_EQ("aaa", ReadAll(P("b")));
  ASSERT_EQ(3, write(a, "AAA", 3));  // still the file now called "b"
  char buf[8] = {};
  ASSERT_EQ(3, pread(b, buf, sizeof(buf), 0));  // the old "b", unlinked
  EXPECT_EQ("bbb", std::string(buf, 3));
  EXPECT_EQ(0, close(a));
  EXPECT_EQ(0, close(b));
  EXPECT_EQ("aaaAAA", ReadAll(P("b")));
  struct stat st;
  EXPECT_EQ(-1, stat(P("a").c_str(), &st));
  EXPECT_EQ(0, unlink(P("b").c_str()));
}

TEST_F(OpfsTest, TwoDescriptorsShareContent) {
  const int w = open(P("shared").c_str(), O_WRONLY | O_CREAT, 0644);
  const int r = open(P("shared").c_str(), O_RDONLY);
  ASSERT_GE(w, 0);
  ASSERT_GE(r, 0);
  ASSERT_EQ(4, write(w, "data", 4));
  char buf[8] = {0};
  ASSERT_EQ(4, read(r, buf, sizeof(buf)));
  EXPECT_STREQ("data", buf);
  EXPECT_EQ(0, close(w));
  EXPECT_EQ(0, close(r));
}

// Threads opening one file at once (TikTok's Keva does): the OPFS thread
// must not create a second sync access handle for it (Safari:
// InvalidStateError, the open fails with EIO).
TEST_F(OpfsTest, ThreadsOpenOneFileAtOnce) {
  struct Arg {
    std::string path;
    int index;
    int failures;
  };
  const int kThreads = 8;
  pthread_t threads[kThreads];
  Arg args[kThreads];
  for (int i = 0; i < kThreads; ++i) {
    args[i] = {P("race"), i, 0};
    ASSERT_EQ(0, pthread_create(&threads[i], NULL, [](void* p) -> void* {
      Arg* a = static_cast<Arg*>(p);
      for (int round = 0; round < 10; ++round) {
        const int fd = open(a->path.c_str(), O_RDWR | O_CREAT, 0644);
        if (fd < 0) {
          a->failures++;
          continue;
        }
        const char c = static_cast<char>('a' + a->index);
        if (pwrite(fd, &c, 1, a->index) != 1) a->failures++;
        if (close(fd) != 0) a->failures++;
      }
      return NULL;
    }, &args[i]));
  }
  for (int i = 0; i < kThreads; ++i) {
    pthread_join(threads[i], NULL);
    EXPECT_EQ(0, args[i].failures) << "thread " << i;
  }
  const int fd = open(P("race").c_str(), O_RDONLY);
  ASSERT_GE(fd, 0);
  char buf[kThreads + 1] = {0};
  EXPECT_EQ(kThreads, read(fd, buf, kThreads));
  EXPECT_STREQ("abcdefgh", buf);
  EXPECT_EQ(0, close(fd));
}

// More open files than Safari allows sync access handles (about 250): the
// bridge keeps at most 192 live and reopens the others when used.
TEST_F(OpfsTest, FourHundredOpenFiles) {
  const int kFiles = 400;
  std::vector<int> fds;
  for (int i = 0; i < kFiles; ++i) {
    const int fd = open(P(("many" + std::to_string(i)).c_str()).c_str(), O_RDWR | O_CREAT, 0644);
    ASSERT_GE(fd, 0) << "file " << i << ": " << strerror(errno);
    fds.push_back(fd);
    const std::string s = std::to_string(i);
    ASSERT_EQ(static_cast<ssize_t>(s.size()), write(fd, s.data(), s.size())) << i;
  }
  for (int i = kFiles - 1; i >= 0; --i) {  // newest first: the oldest were closed by the pool
    char buf[16] = {0};
    ASSERT_GT(pread(fds[i], buf, sizeof(buf) - 1, 0), 0) << i;
    EXPECT_EQ(std::to_string(i), buf);
    struct stat st;
    ASSERT_EQ(0, fstat(fds[i], &st));
    EXPECT_EQ(static_cast<off_t>(std::to_string(i).size()), st.st_size);
  }
  for (int fd : fds) EXPECT_EQ(0, close(fd));
}

TEST_F(OpfsTest, SharedMmapWritesBack) {
  const size_t page = sysconf(_SC_PAGESIZE);
  const int fd = open(P("mapped").c_str(), O_RDWR | O_CREAT, 0644);
  ASSERT_GE(fd, 0);
  ASSERT_EQ(0, ftruncate(fd, 2 * page));
  char* p = static_cast<char*>(
      mmap(NULL, 2 * page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
  ASSERT_NE(MAP_FAILED, p) << strerror(errno);
  memcpy(p + page, "mapped!", 7);
  ASSERT_EQ(0, munmap(p, 2 * page));
  char buf[8] = {0};
  ASSERT_EQ(7, pread(fd, buf, 7, page));
  EXPECT_STREQ("mapped!", buf);
  EXPECT_EQ(0, close(fd));
}

TEST_F(OpfsTest, OneMegabyte) {
  std::string data(1 << 20, '\0');
  for (size_t i = 0; i < data.size(); ++i)
    data[i] = static_cast<char>((i * 2654435761u) >> 24);
  FILE* f = fopen(P("big").c_str(), "wb");
  ASSERT_TRUE(f != NULL);
  EXPECT_EQ(data.size(), fwrite(data.data(), 1, data.size(), f));
  EXPECT_EQ(0, fclose(f));
  EXPECT_TRUE(ReadAll(P("big")) == data);
}

TEST_F(OpfsTest, FionreadReportsBytesLeft) {
  const std::string path = P("fionread");
  int fd = open(path.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
  ASSERT_GE(fd, 0) << strerror(errno);
  ASSERT_EQ(10, write(fd, "0123456789", 10));
  ASSERT_EQ(3, lseek(fd, 3, SEEK_SET));
  int left = -1;
  ASSERT_EQ(0, ioctl(fd, FIONREAD, &left)) << strerror(errno);
  EXPECT_EQ(7, left);
  ASSERT_EQ(12, lseek(fd, 12, SEEK_SET));
  ASSERT_EQ(0, ioctl(fd, FIONREAD, &left));
  EXPECT_EQ(0, left);
  close(fd);
  EXPECT_EQ(0, unlink(path.c_str()));
}

TEST(OpfsPersistence, AcrossPageLoads) {
  if (g_phase == "write") {
    RemoveTree(kPersistDir);
    ASSERT_EQ(0, mkdir(kPersistDir, 0755));
    FILE* f = fopen(kPersistFile, "w");
    ASSERT_TRUE(f != NULL);
    fputs(kPersistToken, f);
    ASSERT_EQ(0, fclose(f));
    printf("[ persist  ] wrote %s; reload with ?arg=--opfs-phase=verify\n",
           kPersistFile);
  } else if (g_phase == "verify") {
    EXPECT_EQ(kPersistToken, ReadAll(kPersistFile));
    printf("[ persist  ] found %s after reload\n", kPersistFile);
    RemoveTree(kPersistDir);
  } else {
    GTEST_SKIP() << "run with ?arg=--opfs-phase=write, then =verify";
  }
}

}  // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  for (int i = 1; i < argc; ++i) {
    if (strncmp(argv[i], "--opfs-phase=", 13) == 0)
      g_phase = argv[i] + 13;
  }
  posix_translation::VirtualFileSystem* vfs = arc_kernel::Boot();
  vfs->Mount("/data/", new posix_translation::OpfsHandler);
  return RUN_ALL_TESTS();
}
