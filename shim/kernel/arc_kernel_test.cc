// End-to-end tests of libc -> posix_translation routing (arc_kernel.cc).
//
// Everything here uses plain libc / C++ library calls. The binary boots the
// kernel before running the tests, so gtest's own output already travels
// stdout -> VFS fd 1 -> PassthroughStream -> host.
//
// /system is the image from tools/make_system_image.py, embedded at
// /arc/system.img and mounted through ReadonlyFileHandler; with
// --image-source=PATH (make kernel-test runs both), read from that file
// instead, its pages filled when first used (topic image-on-demand).

#include <dirent.h>
#include <emscripten.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <linux/ashmem.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/epoll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <fstream>
#include <set>
#include <string>

#include "gtest/gtest.h"
#include "kernel/arc_kernel.h"
#include "posix_translation/host_file.h"
#include "posix_translation/readonly_file.h"
#include "posix_translation/virtual_file_system.h"

namespace {

const char kBuildProp[] = "/system/build.prop";
const char kTexture[] = "/system/media/arc_demo_texture.rgba";
const size_t kTextureSize = 128 * 128 * 4;
const char kFirstLine[] = "ro.build.version.release=5.0.2\n";

TEST(ArcKernelTest, IsBootedAndRouting) {
  EXPECT_TRUE(arc_kernel::IsBooted());
  const unsigned long before = arc_kernel::RoutedCallCount();  // NOLINT
  struct stat st;
  ASSERT_EQ(0, stat(kBuildProp, &st));
  EXPECT_GT(arc_kernel::RoutedCallCount(), before);
}

// The image's pages filled when first used (--image-source): a mapping
// fills the pages it covers, a large read of pages not filled yet goes
// straight from the source, a small one fills its page. (Before the other
// tests touch the jar.)
extern "C" size_t arc_readonly_image_bytes(void);
extern "C" size_t arc_readonly_image_filled_bytes(void);

TEST(ArcKernelTest, ImageOnDemand) {
  struct stat st;
  ASSERT_EQ(0, stat(kBuildProp, &st));  // the image is set up
  const size_t whole = arc_readonly_image_bytes();
  if (arc_readonly_image_filled_bytes() == whole) {
    printf("(the image is in memory whole: no --image-source)\n");
    return;
  }
  const int fd = open("/system/framework/kernel-test.jar", O_RDONLY);
  ASSERT_GE(fd, 0);
  ASSERT_EQ(0, fstat(fd, &st));
  ASSERT_GT(st.st_size, 3 * 64 * 1024);
  const size_t before = arc_readonly_image_filled_bytes();
  std::string all(st.st_size, '\0');
  ASSERT_EQ(st.st_size, pread(fd, &all[0], st.st_size, 0));
  EXPECT_EQ(0, memcmp(all.data(), "PK\x03\x04", 4));
  EXPECT_EQ(before, arc_readonly_image_filled_bytes());
  const size_t size = 100 * 1024;
  char* map = static_cast<char*>(mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 64 * 1024));
  ASSERT_NE(MAP_FAILED, map) << strerror(errno);
  EXPECT_EQ(0, memcmp(map, all.data() + 64 * 1024, size));
  EXPECT_EQ(before + 2 * 64 * 1024, arc_readonly_image_filled_bytes());
  char head[4];
  ASSERT_EQ(4, pread(fd, head, 4, 0));
  EXPECT_EQ(0, memcmp(head, "PK\x03\x04", 4));
  EXPECT_EQ(before + 3 * 64 * 1024, arc_readonly_image_filled_bytes());
  EXPECT_EQ(0, munmap(map, size));
  EXPECT_EQ(0, close(fd));
  EXPECT_LT(arc_readonly_image_filled_bytes(), whole);
}

TEST(ArcKernelTest, StdoutGoesThroughVfs) {
  // Proves gtest's own output path: stdout is VFS fd 1, a PassthroughStream
  // onto the host's stdout.
  const unsigned long before = arc_kernel::RoutedCallCount();  // NOLINT
  printf("[ routed   ] this line went through posix_translation\n");
  fflush(stdout);
  EXPECT_GT(arc_kernel::RoutedCallCount(), before);
  struct stat st;
  ASSERT_EQ(0, fstat(1, &st));
}

TEST(ArcKernelTest, StdioFopenFgets) {
  FILE* f = fopen(kBuildProp, "r");
  ASSERT_TRUE(f != NULL) << strerror(errno);
  char line[128] = {0};
  ASSERT_TRUE(fgets(line, sizeof(line), f) != NULL);
  EXPECT_STREQ(kFirstLine, line);
  EXPECT_EQ(0, fclose(f));
}

TEST(ArcKernelTest, CppIfstream) {
  std::ifstream in(kBuildProp);
  ASSERT_TRUE(in.good());
  std::string line;
  std::getline(in, line);
  EXPECT_EQ("ro.build.version.release=5.0.2", line);
}

TEST(ArcKernelTest, OpenReadLseekFstat) {
  const int fd = open(kTexture, O_RDONLY);
  ASSERT_GE(fd, 3) << strerror(errno);  // 0-2 are stdio.
  struct stat st;
  ASSERT_EQ(0, fstat(fd, &st));
  EXPECT_TRUE(S_ISREG(st.st_mode));
  EXPECT_EQ(static_cast<off_t>(kTextureSize), st.st_size);
  unsigned char pixel[4];
  ASSERT_EQ(4, read(fd, pixel, 4));
  // Top-left pixel is the background colour (236, 240, 245, 255).
  EXPECT_EQ(236, pixel[0]);
  EXPECT_EQ(255, pixel[3]);
  EXPECT_EQ(static_cast<off_t>(kTextureSize - 4), lseek(fd, -4, SEEK_END));
  ASSERT_EQ(4, read(fd, pixel, 4));
  EXPECT_EQ(0, read(fd, pixel, 4));  // EOF
  EXPECT_EQ(0, close(fd));
  EXPECT_EQ(-1, close(fd));
  EXPECT_EQ(EBADF, errno);
}

TEST(ArcKernelTest, StatDirectoryAndMissing) {
  struct stat st;
  ASSERT_EQ(0, stat("/system/media", &st));
  EXPECT_TRUE(S_ISDIR(st.st_mode));
  EXPECT_EQ(-1, stat("/system/nonexistent", &st));
  EXPECT_EQ(ENOENT, errno);
  EXPECT_EQ(-1, open("/system/nonexistent", O_RDONLY));
  EXPECT_EQ(ENOENT, errno);
  EXPECT_EQ(0, access(kBuildProp, R_OK));
}

// chmod goes to the VFS (no permission bits: an existing file takes any
// mode), not to emscripten's FS, where the VFS's files do not exist.
TEST(ArcKernelTest, ChmodExistingAndMissing) {
  EXPECT_EQ(0, chmod("/system/media", 0771));
  EXPECT_EQ(0, chmod(kBuildProp, 0644));
  EXPECT_EQ(0, fchmodat(AT_FDCWD, kBuildProp, 0644, 0));
  int fd = open(kBuildProp, O_RDONLY);
  ASSERT_GE(fd, 0);
  EXPECT_EQ(0, fchmod(fd, 0644));
  close(fd);
  EXPECT_EQ(-1, chmod("/system/nonexistent", 0644));
  EXPECT_EQ(ENOENT, errno);
}

TEST(ArcKernelTest, ReadonlySystemRejectsWrites) {
  EXPECT_EQ(-1, open(kBuildProp, O_WRONLY));
  EXPECT_TRUE(errno == EACCES || errno == EROFS) << errno;
  EXPECT_EQ(-1, unlink(kBuildProp));
  EXPECT_EQ(-1, mkdir("/system/newdir", 0755));
}

TEST(ArcKernelTest, OpendirReaddir) {
  DIR* dir = opendir("/system/media");
  ASSERT_TRUE(dir != NULL) << strerror(errno);
  std::set<std::string> names;
  while (struct dirent* ent = readdir(dir))
    names.insert(ent->d_name);
  EXPECT_EQ(0, closedir(dir));
  EXPECT_TRUE(names.count("arc_demo_texture.rgba"));
}

TEST(ArcKernelTest, ChdirAndRelativePaths) {
  char cwd[256];
  ASSERT_TRUE(getcwd(cwd, sizeof(cwd)) != NULL);
  EXPECT_STREQ("/", cwd);
  ASSERT_EQ(0, chdir("/system"));
  ASSERT_TRUE(getcwd(cwd, sizeof(cwd)) != NULL);
  EXPECT_STREQ("/system", cwd);
  FILE* f = fopen("build.prop", "r");
  EXPECT_TRUE(f != NULL);
  if (f)
    fclose(f);
  ASSERT_EQ(0, chdir("/"));
}

TEST(ArcKernelTest, MmapFileMatchesRead) {
  const int fd = open(kTexture, O_RDONLY);
  ASSERT_GE(fd, 0);
  void* map = mmap(NULL, kTextureSize, PROT_READ, MAP_PRIVATE, fd, 0);
  ASSERT_NE(MAP_FAILED, map) << strerror(errno);
  std::string via_read(kTextureSize, '\0');
  ASSERT_EQ(static_cast<ssize_t>(kTextureSize),
            pread(fd, &via_read[0], kTextureSize, 0));
  EXPECT_EQ(0, memcmp(map, via_read.data(), kTextureSize));
  EXPECT_EQ(0, munmap(map, kTextureSize));
  EXPECT_EQ(0, close(fd));
}

// classes.dex of a jar of the image stored 64 KiB-aligned (make_system_image.py
// --store-dex), mapped as ART 10 maps a boot jar's dex: read-only mappings,
// shared or private, are the image's own pages (readonly_file.cc), which
// cannot be made writable. A private one is copy-on-write by hand: what is
// written after arc_readonly_image_will_write stays in it, and the file
// (pread, later mappings) keeps its bytes.
extern "C" void arc_readonly_image_will_write(const void* addr, size_t length);

TEST(ArcKernelTest, MmapStoredDexImagePages) {
  const off_t kDexAt = 64 * 1024;
  const int fd = open("/system/framework/kernel-test.jar", O_RDONLY);
  ASSERT_GE(fd, 0);
  char head[4];
  ASSERT_EQ(4, pread(fd, head, 4, 0));
  ASSERT_EQ(0, memcmp(head, "PK\x03\x04", 4));
  const size_t size = 100 * 1024;
  std::string file(size, '\0');
  ASSERT_EQ(static_cast<ssize_t>(size), pread(fd, &file[0], size, kDexAt));
  EXPECT_NE(std::string::npos, file.find("EGL"));  // shim/ndk/ndk_egl.cc, stored
  char* shared = static_cast<char*>(mmap(NULL, size, PROT_READ, MAP_SHARED, fd, kDexAt));
  ASSERT_NE(MAP_FAILED, shared) << strerror(errno);
  char* priv = static_cast<char*>(mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, kDexAt));
  ASSERT_NE(MAP_FAILED, priv) << strerror(errno);
  EXPECT_EQ(shared, priv);
  EXPECT_TRUE(posix_translation::IsInReadonlyImage(priv, size));
  EXPECT_EQ(0, memcmp(priv, file.data(), size));
  EXPECT_EQ(-1, mprotect(priv, size, PROT_READ | PROT_WRITE));
  EXPECT_EQ(EACCES, errno);
  EXPECT_EQ(0, mprotect(priv, size, PROT_READ));
  // The private mapping writes (ART's quickening does so).
  arc_readonly_image_will_write(priv + 6, 4);
  priv[6] = ~file[6];
  priv[9] = ~file[9];
  char back[16];
  ASSERT_EQ(16, pread(fd, back, 16, kDexAt));
  EXPECT_EQ(0, memcmp(back, file.data(), 16));
  char* priv2 = static_cast<char*>(mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, kDexAt));
  ASSERT_NE(MAP_FAILED, priv2) << strerror(errno);
  EXPECT_NE(priv, priv2);
  EXPECT_FALSE(posix_translation::IsInReadonlyImage(priv2, size));
  EXPECT_EQ(0, memcmp(priv2, file.data(), size));
  char* shared2 = static_cast<char*>(mmap(NULL, size, PROT_READ, MAP_SHARED, fd, kDexAt));
  ASSERT_NE(MAP_FAILED, shared2) << strerror(errno);
  EXPECT_NE(priv, shared2);
  EXPECT_EQ(0, memcmp(shared2, file.data(), size));
  EXPECT_EQ(static_cast<char>(~file[6]), priv[6]);
  // A writable private mapping is a copy.
  char* copy = static_cast<char*>(
      mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, kDexAt));
  ASSERT_NE(MAP_FAILED, copy) << strerror(errno);
  EXPECT_FALSE(posix_translation::IsInReadonlyImage(copy, size));
  EXPECT_EQ(0, memcmp(copy, file.data(), size));
  EXPECT_EQ(0, munmap(copy, size));
  EXPECT_EQ(0, munmap(shared2, size));
  EXPECT_EQ(0, munmap(priv2, size));
  EXPECT_EQ(0, munmap(priv, size));
  EXPECT_EQ(0, munmap(shared, size));
  EXPECT_EQ(0, close(fd));
}

// A small file of the image, packed at an offset that is not page-aligned
// (make_system_image.py): ReadonlyFile maps it by copying.
TEST(ArcKernelTest, MmapPackedSmallFile) {
  const char kPacked[] = "/system/etc/arc_kernel_test.cc";
  const int fd = open(kPacked, O_RDONLY);
  ASSERT_GE(fd, 0);
  struct stat st;
  ASSERT_EQ(0, fstat(fd, &st));
  const size_t size = st.st_size;
  ASSERT_GT(size, 1000u);
  std::string via_read(size, '\0');
  ASSERT_EQ(static_cast<ssize_t>(size), pread(fd, &via_read[0], size, 0));
  EXPECT_NE(std::string::npos, via_read.find("MmapPackedSmallFile"));
  char* map = static_cast<char*>(mmap(NULL, size + 1, PROT_READ, MAP_SHARED, fd, 0));
  ASSERT_NE(MAP_FAILED, map) << strerror(errno);
  EXPECT_EQ(0, memcmp(map, via_read.data(), size));
  EXPECT_EQ(0, map[size]);  // the rest of the page is zero
  EXPECT_EQ(0, munmap(map, size + 1));
  // A private writable mapping: changes stay in it.
  map = static_cast<char*>(mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0));
  ASSERT_NE(MAP_FAILED, map) << strerror(errno);
  map[0] = '#';
  map[1] = '!';
  char head[2];
  ASSERT_EQ(2, pread(fd, head, 2, 0));
  EXPECT_EQ(via_read[0], head[0]);
  EXPECT_EQ(0, munmap(map, size));
  // Unaligned file offsets fail as on Linux.
  EXPECT_EQ(MAP_FAILED, mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 1));
  EXPECT_EQ(EINVAL, errno);
  EXPECT_EQ(0, close(fd));
}

TEST(ArcKernelTest, AnonymousMmapWithPartialMunmap) {
  const size_t page = sysconf(_SC_PAGESIZE);
  char* p = static_cast<char*>(mmap(NULL, 4 * page, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  ASSERT_NE(MAP_FAILED, p) << strerror(errno);
  EXPECT_EQ(0, p[0]);
  memset(p, 'x', 4 * page);
  EXPECT_EQ(0, munmap(p + page, page));  // A hole in the middle.
  EXPECT_EQ('x', p[3 * page]);
  EXPECT_EQ(0, mprotect(p, page, PROT_READ));
  EXPECT_EQ(0, munmap(p, 4 * page));
}

TEST(ArcKernelTest, Pipe) {
  int fds[2];
  ASSERT_EQ(0, pipe(fds)) << strerror(errno);
  ASSERT_EQ(5, write(fds[1], "hello", 5));
  struct pollfd pfd = {fds[0], POLLIN, 0};
  EXPECT_EQ(1, poll(&pfd, 1, 0));  // a zero timeout takes its own syscall
  EXPECT_EQ(POLLIN, pfd.revents);
  pfd.revents = 0;
  EXPECT_EQ(1, poll(&pfd, 1, 10));
  EXPECT_EQ(POLLIN, pfd.revents);
  char buf[8] = {0};
  ASSERT_EQ(5, read(fds[0], buf, sizeof(buf)));
  EXPECT_STREQ("hello", buf);
  EXPECT_EQ(0, close(fds[1]));
  EXPECT_EQ(0, read(fds[0], buf, sizeof(buf)));  // EOF after writer closes.
  EXPECT_EQ(0, close(fds[0]));
}

// libutils' Looper: epoll on a pipe, waited on with and without a timeout.
TEST(ArcKernelTest, EpollOnPipe) {
  int fds[2];
  ASSERT_EQ(0, pipe(fds)) << strerror(errno);
  const int ep = epoll_create(8);
  ASSERT_GE(ep, 0) << strerror(errno);
  struct epoll_event ev = {};
  ev.events = EPOLLIN;
  ev.data.fd = fds[0];
  ASSERT_EQ(0, epoll_ctl(ep, EPOLL_CTL_ADD, fds[0], &ev)) << strerror(errno);
  struct epoll_event out[4];
  EXPECT_EQ(0, epoll_wait(ep, out, 4, 0)) << strerror(errno);
  EXPECT_EQ(0, epoll_wait(ep, out, 4, 10)) << strerror(errno);
  ASSERT_EQ(1, write(fds[1], "w", 1));
  ASSERT_EQ(1, epoll_wait(ep, out, 4, 0)) << strerror(errno);
  EXPECT_EQ(fds[0], out[0].data.fd);
  EXPECT_EQ(static_cast<uint32_t>(EPOLLIN), out[0].events);
  ASSERT_EQ(1, epoll_wait(ep, out, 4, -1)) << strerror(errno);
  EXPECT_EQ(0, close(ep));
  EXPECT_EQ(0, close(fds[0]));
  EXPECT_EQ(0, close(fds[1]));
}

TEST(ArcKernelTest, UnixSocketpair) {
  int sv[2];
  ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) << strerror(errno);
  ASSERT_EQ(4, send(sv[0], "ping", 4, 0));
  char buf[8] = {0};
  ASSERT_EQ(4, recv(sv[1], buf, sizeof(buf), 0));
  EXPECT_STREQ("ping", buf);
  EXPECT_EQ(0, close(sv[0]));
  EXPECT_EQ(0, close(sv[1]));
}

// SCM_RIGHTS on a stream socket: the descriptors come with the first byte of
// the data they were sent with, and a read stops before the next data that
// carries descriptors (Gecko's IPC: a read that took two such messages gave
// the second one's descriptors only with the next read).
static int SendWithFd(int s, const char* data, int fd) {
  struct iovec iov = {const_cast<char*>(data), strlen(data)};
  char control[CMSG_SPACE(sizeof(int))] = {0};
  struct msghdr msg = {};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  if (fd >= 0) {
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    struct cmsghdr* c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof(int));
  }
  return sendmsg(s, &msg, 0);
}

static int RecvWithFds(int s, char* buf, size_t len, int* fds, int* nfds) {
  struct iovec iov = {buf, len};
  char control[CMSG_SPACE(4 * sizeof(int))] = {0};
  struct msghdr msg = {};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = control;
  msg.msg_controllen = sizeof(control);
  const int r = recvmsg(s, &msg, MSG_DONTWAIT);
  *nfds = 0;
  for (struct cmsghdr* c = CMSG_FIRSTHDR(&msg); r > 0 && c; c = CMSG_NXTHDR(&msg, c)) {
    if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
      const int n = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
      memcpy(fds + *nfds, CMSG_DATA(c), n * sizeof(int));
      *nfds += n;
    }
  }
  return r;
}

TEST(ArcKernelTest, UnixSocketpairFdsComeWithTheirData) {
  int sv[2], p1[2], p2[2];
  ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) << strerror(errno);
  ASSERT_EQ(0, pipe(p1));
  ASSERT_EQ(0, pipe(p2));
  ASSERT_EQ(3, SendWithFd(sv[0], "aaa", -1));
  ASSERT_EQ(3, SendWithFd(sv[0], "bbb", p1[1]));
  ASSERT_EQ(3, SendWithFd(sv[0], "ccc", p2[1]));
  ASSERT_EQ(3, SendWithFd(sv[0], "ddd", -1));
  char buf[32] = {0};
  int fds[4], nfds;
  // "aaa" alone: the read stops before "bbb", which carries a descriptor.
  ASSERT_EQ(3, RecvWithFds(sv[1], buf, sizeof(buf), fds, &nfds));
  EXPECT_EQ(0, memcmp(buf, "aaa", 3));
  EXPECT_EQ(0, nfds);
  // "bbb" with its descriptor, not past "ccc".
  ASSERT_EQ(3, RecvWithFds(sv[1], buf, sizeof(buf), fds, &nfds));
  EXPECT_EQ(0, memcmp(buf, "bbb", 3));
  ASSERT_EQ(1, nfds);
  ASSERT_EQ(1, write(fds[0], "1", 1));
  ASSERT_EQ(1, read(p1[0], buf, 1));
  EXPECT_EQ('1', buf[0]);
  close(fds[0]);
  // "ccc" with its descriptor, then "ddd" in the same read.
  ASSERT_EQ(6, RecvWithFds(sv[1], buf, sizeof(buf), fds, &nfds));
  EXPECT_EQ(0, memcmp(buf, "cccddd", 6));
  ASSERT_EQ(1, nfds);
  ASSERT_EQ(1, write(fds[0], "2", 1));
  ASSERT_EQ(1, read(p2[0], buf, 1));
  EXPECT_EQ('2', buf[0]);
  close(fds[0]);
  for (int fd : {sv[0], sv[1], p1[0], p1[1], p2[0], p2[1]})
    close(fd);
}

TEST(ArcKernelTest, UnixSocketpairWithFlags) {
  // tokio's UnixStream::pair asks for both flags at once.
  int sv[2];
  ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sv))
      << strerror(errno);
  EXPECT_NE(0, fcntl(sv[0], F_GETFL) & O_NONBLOCK);
  EXPECT_NE(0, fcntl(sv[1], F_GETFL) & O_NONBLOCK);
  char buf[8];
  EXPECT_EQ(-1, recv(sv[1], buf, sizeof(buf), 0));
  EXPECT_EQ(EAGAIN, errno);
  EXPECT_EQ(0, close(sv[0]));
  EXPECT_EQ(0, close(sv[1]));
}

TEST(ArcKernelTest, InetSocketReachesTheNetwork) {
  // TCP goes through the net bridge (arc_net_test.cc has the real tests);
  // nothing listens on port 1, so the attempt is refused rather than having
  // no route as before the network existed.
  const int s = socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(s, 0) << strerror(errno);
  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(1);
  addr.sin_addr.s_addr = htonl(0x7f000001);
  EXPECT_EQ(-1, connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)));
  EXPECT_EQ(ECONNREFUSED, errno);
  EXPECT_EQ(0, close(s));
}

TEST(ArcKernelTest, DevNullZeroUrandom) {
  FILE* null = fopen("/dev/null", "w");
  ASSERT_TRUE(null != NULL);
  EXPECT_EQ(5u, fwrite("12345", 1, 5, null));
  EXPECT_EQ(0, fclose(null));

  const int zero = open("/dev/zero", O_RDONLY);
  ASSERT_GE(zero, 0);
  char buf[64];
  memset(buf, 1, sizeof(buf));
  ASSERT_EQ(64, read(zero, buf, sizeof(buf)));
  for (size_t i = 0; i < sizeof(buf); ++i)
    ASSERT_EQ(0, buf[i]);
  close(zero);

  const int rnd = open("/dev/urandom", O_RDONLY);
  ASSERT_GE(rnd, 0);
  unsigned char a[32], b[32];
  ASSERT_EQ(32, read(rnd, a, sizeof(a)));
  ASSERT_EQ(32, read(rnd, b, sizeof(b)));
  EXPECT_NE(0, memcmp(a, b, sizeof(a)));
  close(rnd);
}

TEST(ArcKernelTest, AshmemSharedMapping) {
  const int fd = open("/dev/ashmem", O_RDWR);
  ASSERT_GE(fd, 0) << strerror(errno);
  const size_t page = sysconf(_SC_PAGESIZE);
  ASSERT_EQ(0, ioctl(fd, ASHMEM_SET_NAME, "arc-kernel-test"));
  ASSERT_EQ(0, ioctl(fd, ASHMEM_SET_SIZE, 2 * page));
  EXPECT_EQ(static_cast<int>(2 * page), ioctl(fd, ASHMEM_GET_SIZE, 0));
  char* p = static_cast<char*>(mmap(NULL, 2 * page, PROT_READ | PROT_WRITE,
                                    MAP_SHARED, fd, 0));
  ASSERT_NE(MAP_FAILED, p) << strerror(errno);
  strcpy(p, "shared");  // NOLINT
  char buf[8] = {0};
  ASSERT_EQ(6, pread(fd, buf, 6, 0));  // Same memory through the fd.
  EXPECT_STREQ("shared", buf);
  EXPECT_EQ(0, munmap(p + page, page));  // Partial unmap.
  EXPECT_EQ(0, munmap(p, page));
  EXPECT_EQ(0, close(fd));
}

TEST(ArcKernelTest, DupDup2Fcntl) {
  const int fd = open(kBuildProp, O_RDONLY);
  ASSERT_GE(fd, 0);
  const int copy = dup(fd);
  ASSERT_GT(copy, fd);
  char c;
  ASSERT_EQ(1, read(copy, &c, 1));
  EXPECT_EQ(1, lseek(fd, 0, SEEK_CUR));  // Offset is shared.
  ASSERT_EQ(100, dup2(fd, 100));
  EXPECT_EQ(O_RDONLY, fcntl(100, F_GETFL) & O_ACCMODE);
  EXPECT_EQ(0, close(100));
  EXPECT_EQ(0, close(copy));
  EXPECT_EQ(0, close(fd));
}

// The monotonic clock is one for all threads (shim/kernel/library_arc_clock.js):
// a time read on one thread is never in the future for another that hears of
// it afterwards. (Each worker's own performance.timeOrigin + now() is not: a
// Choreographer then left a frame callback posted from another thread queued
// for good; topic unity2022-menu-hang.) Threads take turns: each reads the
// clock and checks it against the previous thread's reading. Each thread's
// worker first gets a clock of its own, off by -20..+15 ms, as the browsers'
// workers have (Node's agree).
struct ClockTurns {
  static constexpr int kThreads = 4;
  static constexpr int kTurns = 4000;
  std::atomic<int> turn{0};
  std::atomic<int64_t> last{0};
  std::atomic<int> earlier{0};  // readings before the previous thread's
  std::atomic<int64_t> worst{0};
};

int64_t MonotonicNs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

void* ClockTurnsThread(void* arg) {
  ClockTurns* c = static_cast<ClockTurns*>(arg);
  static std::atomic<int> next_id{0};
  const int id = next_id++;
  static const double kSkewMs[ClockTurns::kThreads] = {0, -20, 15, -5};
  EM_ASM({ ArcClock.off += $0; }, kSkewMs[id]);
  for (int t = id; t < ClockTurns::kTurns; t += ClockTurns::kThreads) {
    while (c->turn.load() != t)
      sched_yield();
    const int64_t now = MonotonicNs();
    const int64_t before = c->last.load();
    if (now < before) {
      c->earlier++;
      if (before - now > c->worst.load())
        c->worst.store(before - now);
    }
    c->last.store(now);
    c->turn.store(t + 1);
  }
  return nullptr;
}

TEST(ArcKernelTest, MonotonicClockAcrossThreads) {
  ClockTurns c;
  pthread_t threads[ClockTurns::kThreads];
  for (pthread_t& t : threads)
    ASSERT_EQ(0, pthread_create(&t, nullptr, ClockTurnsThread, &c));
  for (pthread_t& t : threads)
    pthread_join(t, nullptr);
  EXPECT_EQ(ClockTurns::kTurns, c.turn.load());
  EXPECT_EQ(0, c.earlier.load()) << "worst by " << c.worst.load() << " ns";
}

}  // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  for (int i = 1; i < argc; ++i) {
    if (!strncmp(argv[i], "--image-source=", 15))
      setenv("ARC_IMAGE_SOURCE", argv[i] + 15, 1);
  }
  posix_translation::VirtualFileSystem* vfs = arc_kernel::Boot();
  vfs->Mount("/system/", new posix_translation::ReadonlyFileHandler(
      "/arc/system.img", 64 * 1024, new posix_translation::HostFileHandler()));
  const int result = RUN_ALL_TESTS();
  printf("routed libc calls: %lu\n", arc_kernel::RoutedCallCount());
  return result;
}
