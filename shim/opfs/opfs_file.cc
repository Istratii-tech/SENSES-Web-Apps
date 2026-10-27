// See opfs_file.h.

#include "posix_translation/opfs_file.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/vfs.h>

#include <vector>

#include "common/alog.h"
#include "opfs/opfs_bridge.h"
#include "posix_translation/dir.h"
#include "posix_translation/directory_file_stream.h"
#include "posix_translation/directory_manager.h"
#include "posix_translation/statfs.h"
#include "posix_translation/virtual_file_system.h"

// The browser's quota for the origin's storage (navigator.storage.estimate,
// kept by the memory budget's page script: shim/membudget/mem_budget.h).
extern "C" int32_t* arc_mem_js_cells(void) __attribute__((weak));

namespace posix_translation {

namespace {

// statfs of /data: the quota and what is left of it, when the page told.
int StatFsForOpfs(struct statfs* out) {
  int r = DoStatFsForData(out);
  if (r != 0 || !arc_mem_js_cells)
    return r;
  const int32_t* js = arc_mem_js_cells();
  // ARC_MEM_JS_STORAGE_USAGE_KB, _QUOTA_KB, _VALID.
  if (!__atomic_load_n(&js[5], __ATOMIC_ACQUIRE))
    return r;
  const uint64_t usage = static_cast<uint64_t>(static_cast<uint32_t>(js[3])) * 1024;
  const uint64_t quota = static_cast<uint64_t>(static_cast<uint32_t>(js[4])) * 1024;
  if (quota == 0)
    return r;
  out->f_blocks = quota / out->f_bsize;
  out->f_bfree = quota > usage ? (quota - usage) / out->f_bsize : 0;
  out->f_bavail = out->f_bfree;
  return 0;
}

// Converts a bridge result to POSIX (-1 + errno).
int Posix(int result) {
  if (result < 0) {
    errno = -result;
    return -1;
  }
  return result;
}

void FillStat(const std::string& pathname, arc_opfs::EntryType type,
              double size, double mtime, struct stat* out) {
  memset(out, 0, sizeof(*out));
  VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
  out->st_ino = sys->GetInodeLocked(pathname);
  // Permission bits are left to VirtualFileSystem (see DirectoryFileStream).
  out->st_mode = (type == arc_opfs::kDirectory) ? S_IFDIR : S_IFREG;
  out->st_nlink = (type == arc_opfs::kDirectory) ? 2 : 1;
  out->st_size = static_cast<off_t>(size);
  out->st_blksize = 4096;
  out->st_blocks = (out->st_size + 511) / 512;
  out->st_atime = out->st_mtime = out->st_ctime = static_cast<time_t>(mtime);
}

// The handler's table of symlinks and modes (opfs_file.h), in its base
// directory. Records: an op byte, the key, NUL, the argument, NUL.
//   L key target   a symlink
//   M key octal    permission bits
//   D key          forget key and everything below it
//   R key newkey   key and everything below it moved to newkey
const char kMetaName[] = ".arc-wasm-meta";

std::string DirKey(const std::string& key) {
  const size_t slash = key.rfind('/');
  return slash == 0 || slash == std::string::npos ? "/" : key.substr(0, slash);
}

}  // namespace

// --- OpfsHandler -----------------------------------------------------------

std::string OpfsHandler::Key(const std::string& pathname) const {
  const std::string opfs = ToOpfsPath(pathname);
  if (base_ == "/")
    return opfs;
  const std::string rel = opfs.substr(base_.size());
  return rel.empty() ? "/" : rel;
}

void OpfsHandler::ApplyRecord(char op, const std::string& key,
                              const std::string& arg) {
  switch (op) {
    case 'L': {
      Meta& m = meta_[key];
      if (!m.is_link)
        ++links_;
      m.is_link = true;
      m.link = arg;
      m.mode = 0;
      break;
    }
    case 'M': {
      MetaMap::iterator it = meta_.find(key);
      if (it != meta_.end() && it->second.is_link)
        break;
      meta_[key].mode = static_cast<mode_t>(strtoul(arg.c_str(), NULL, 8)) & 07777;
      break;
    }
    case 'D':
    case 'R': {
      // key itself and what lies below it ("key/..."; "key.txt" sorts
      // between the two).
      std::vector<std::pair<std::string, Meta> > moved;
      const std::string below = key == "/" ? "/" : key + "/";
      MetaMap::iterator it = meta_.find(key);
      if (it != meta_.end()) {
        if (op == 'R')
          moved.push_back(std::make_pair(arg, it->second));
        if (it->second.is_link)
          --links_;
        meta_.erase(it);
      }
      it = meta_.lower_bound(below);
      while (it != meta_.end() && it->first.compare(0, below.size(), below) == 0) {
        if (op == 'R')
          moved.push_back(std::make_pair(arg + it->first.substr(key.size()), it->second));
        if (it->second.is_link)
          --links_;
        meta_.erase(it++);
      }
      for (size_t i = 0; i < moved.size(); ++i) {
        MetaMap::iterator old = meta_.find(moved[i].first);
        if (old != meta_.end() && old->second.is_link)
          --links_;
        meta_[moved[i].first] = moved[i].second;
        if (moved[i].second.is_link)
          ++links_;
      }
      break;
    }
  }
}

void OpfsHandler::WriteMeta(const std::string& bytes, bool truncate_first) {
  const std::string path = (base_ == "/" ? "" : base_) + "/" + kMetaName;
  if (meta_id_ < 0) {
    arc_opfs::Request r = {};
    r.op = arc_opfs::kOpen;
    r.path = path.c_str();
    r.flags = arc_opfs::kCreate;
    const int id = arc_opfs::Call(&r);
    if (id < 0) {
      ALOGW("%s: %s cannot be opened (%d); symlinks and modes are not kept",
            mount_point_.c_str(), path.c_str(), -id);
      return;
    }
    if (meta_id_ >= 0) {  // another thread opened it meanwhile
      arc_opfs::Request c = {};
      c.op = arc_opfs::kClose;
      c.id = id;
      arc_opfs::Call(&c);
    } else {
      meta_id_ = id;
    }
  }
  const int id = meta_id_;
  double at = meta_size_;
  if (truncate_first) {
    arc_opfs::Request t = {};
    t.op = arc_opfs::kTruncate;
    t.id = id;
    t.offset = 0;
    arc_opfs::Call(&t);
    at = 0;
    meta_size_ = static_cast<double>(bytes.size());
  } else {
    meta_size_ += static_cast<double>(bytes.size());  // reserved before the call
  }
  arc_opfs::Request w = {};
  w.op = arc_opfs::kWrite;
  w.id = id;
  w.buf = const_cast<char*>(bytes.data());
  w.len = static_cast<int32_t>(bytes.size());
  w.offset = at;
  if (arc_opfs::Call(&w) < 0)
    ALOGW("%s: writing %s failed", mount_point_.c_str(), kMetaName);
  // Kept by a browser as written; the Node stand-in with a host directory
  // writes through at flush (shim/opfs/memory_opfs.js).
  arc_opfs::Request f = {};
  f.op = arc_opfs::kFlush;
  f.id = id;
  arc_opfs::Call(&f);
}

void OpfsHandler::Record(char op, const std::string& key,
                         const std::string& arg) {
  ApplyRecord(op, key, arg);
  std::string bytes(1, op);
  bytes += key;
  bytes += '\0';
  bytes += arg;
  bytes += '\0';
  WriteMeta(bytes, false);
}

void OpfsHandler::LoadMeta() {
  const std::string path = (base_ == "/" ? "" : base_) + "/" + kMetaName;
  arc_opfs::EntryType type = arc_opfs::kNone;
  double size = 0;
  double mtime = 0;
  if (arc_opfs::Stat(path, &type, &size, &mtime) < 0 || type != arc_opfs::kFile)
    return;
  arc_opfs::Request r = {};
  r.op = arc_opfs::kOpen;
  r.path = path.c_str();
  const int id = arc_opfs::Call(&r);
  if (id < 0)
    return;
  meta_id_ = id;
  std::vector<char> data(static_cast<size_t>(size));
  if (!data.empty()) {
    arc_opfs::Request rd = {};
    rd.op = arc_opfs::kRead;
    rd.id = id;
    rd.buf = &data[0];
    rd.len = static_cast<int32_t>(data.size());
    rd.offset = 0;
    const int n = arc_opfs::Call(&rd);
    data.resize(n > 0 ? n : 0);
  }
  size_t records = 0;
  size_t pos = 0;
  while (pos < data.size()) {
    const char op = data[pos];
    const void* nul1 = memchr(&data[pos + 1], 0, data.size() - pos - 1);
    if (!nul1)
      break;
    const size_t key_end = static_cast<const char*>(nul1) - &data[0];
    if (key_end + 1 >= data.size())
      break;
    const void* nul2 = memchr(&data[key_end + 1], 0, data.size() - key_end - 1);
    if (!nul2)
      break;
    const size_t arg_end = static_cast<const char*>(nul2) - &data[0];
    ApplyRecord(op, std::string(&data[pos + 1], key_end - pos - 1),
                std::string(&data[key_end + 1], arg_end - key_end - 1));
    ++records;
    pos = arg_end + 1;
  }
  meta_size_ = static_cast<double>(pos);
  // Compacted when most records are history: one record per entry.
  if (records > 2 * meta_.size() + 32 || pos != data.size()) {
    std::string bytes;
    for (MetaMap::const_iterator it = meta_.begin(); it != meta_.end(); ++it) {
      char mode[16];
      snprintf(mode, sizeof(mode), "%o", static_cast<unsigned>(it->second.mode));
      bytes += it->second.is_link ? 'L' : 'M';
      bytes += it->first;
      bytes += '\0';
      bytes += it->second.is_link ? it->second.link : std::string(mode);
      bytes += '\0';
    }
    WriteMeta(bytes, true);
  }
  if (!meta_.empty())
    ALOGI("%s: %zu symlinks, %zu modes kept", mount_point_.c_str(), links_,
          meta_.size() - links_);
}

const OpfsHandler::Meta* OpfsHandler::FindLink(const std::string& key) {
  MetaMap::const_iterator it = meta_.find(key);
  if (it == meta_.end() || !it->second.is_link)
    return NULL;
  // The link's directory may have been removed from OPFS behind our back
  // (the page clearing an app's data): checked once per directory and
  // session; its symlinks and modes are forgotten if it is gone.
  const std::string dir = DirKey(key);
  if (dir != "/" && !live_dirs_.count(dir)) {
    arc_opfs::EntryType type = arc_opfs::kNone;
    double size = 0;
    double mtime = 0;
    const std::string opfs_dir = (base_ == "/" ? "" : base_) + dir;
    const int r = arc_opfs::Stat(opfs_dir, &type, &size, &mtime);
    if (r == -ENOENT || (r >= 0 && type != arc_opfs::kDirectory)) {
      Record('D', dir, std::string());
      return NULL;
    }
    live_dirs_.insert(dir);
    it = meta_.find(key);  // the table may have changed during the call
    if (it == meta_.end() || !it->second.is_link)
      return NULL;
  }
  return &it->second;
}

bool OpfsHandler::HasMetaAt(const std::string& key) const {
  if (meta_.empty())
    return false;
  if (meta_.count(key))
    return true;
  const std::string below = key == "/" ? "/" : key + "/";
  MetaMap::const_iterator it = meta_.lower_bound(below);
  return it != meta_.end() && it->first.compare(0, below.size(), below) == 0;
}

bool OpfsHandler::HasLinksBelow(const std::string& key) const {
  if (!links_)
    return false;
  const std::string below = key == "/" ? "/" : key + "/";
  for (MetaMap::const_iterator it = meta_.lower_bound(below);
       it != meta_.end() && it->first.compare(0, below.size(), below) == 0; ++it) {
    if (it->second.is_link)
      return true;
  }
  return false;
}

void OpfsHandler::ApplyKeptMode(const std::string& pathname, struct stat* out) {
  if (meta_.empty())
    return;
  MetaMap::const_iterator it = meta_.find(Key(pathname));
  if (it != meta_.end() && !it->second.is_link && it->second.mode)
    out->st_mode = (out->st_mode & S_IFMT) | it->second.mode;
}

int OpfsHandler::chmod(const std::string& pathname, mode_t mode) {
  struct stat st;
  if (stat(pathname, &st) != 0)
    return -1;
  if (read_only_)
    return 0;  // as before: accepted, not kept
  const std::string key = Key(pathname);
  if (key == "/")
    return 0;
  mode &= 07777;
  MetaMap::const_iterator it = meta_.find(key);
  if (it != meta_.end() && it->second.mode == mode)
    return 0;  // unchanged (SharedPreferences sets its mode at every write)
  char octal[16];
  snprintf(octal, sizeof(octal), "%o", static_cast<unsigned>(mode));
  Record('M', key, octal);
  return 0;
}

ssize_t OpfsHandler::readlink(const std::string& pathname,
                              std::string* resolved) {
  if (links_) {
    const Meta* m = FindLink(Key(pathname));
    if (m) {
      *resolved = m->link;
      return resolved->size();
    }
  }
  errno = EINVAL;
  return -1;
}

int OpfsHandler::symlink(const std::string& oldpath,
                         const std::string& newpath) {
  if (read_only_) {
    errno = EROFS;
    return -1;
  }
  if (oldpath.empty()) {
    errno = ENOENT;
    return -1;
  }
  const std::string key = Key(newpath);
  if (key == "/" || FindLink(key)) {
    errno = EEXIST;
    return -1;
  }
  arc_opfs::EntryType type = arc_opfs::kNone;
  double size = 0;
  double mtime = 0;
  const int r = arc_opfs::Stat(ToOpfsPath(newpath), &type, &size, &mtime);
  if (r >= 0) {
    errno = EEXIST;
    return -1;
  }
  if (r != -ENOENT)
    return Posix(r);
  if (FindLink(key)) {  // made by another thread during the call
    errno = EEXIST;
    return -1;
  }
  Record('L', key, oldpath);
  live_dirs_.insert(DirKey(key));
  return 0;
}

OpfsHandler::OpfsHandler(const std::string& base, bool read_only)
    : FileSystemHandler("OpfsHandler"),
      base_(base.size() > 1 && base[base.size() - 1] == '/'
                ? base.substr(0, base.size() - 1) : base),
      read_only_(read_only),
      initialized_(false),
      links_(0),
      meta_id_(-1),
      meta_size_(0) {}

OpfsHandler::~OpfsHandler() {}

bool OpfsHandler::IsInitialized() const { return initialized_; }

void OpfsHandler::Initialize() {
  initialized_ = arc_opfs::Start();
  arc_opfs::SetLockReleasedWhileWaiting(&VirtualFileSystem::GetVirtualFileSystem()->mutex());
  if (!initialized_) {
    ALOGE("OPFS is not available; %s is unusable", mount_point_.c_str());
    return;
  }
  // The base directory and its parents ("/senses", "/senses/data", ...).
  if (read_only_)
    return;
  for (size_t slash = base_.find('/', 1);; slash = base_.find('/', slash + 1)) {
    const std::string dir = base_.substr(0, slash);
    if (!dir.empty() && dir != "/")
      arc_opfs::PathOp(arc_opfs::kMkdir, dir);  // EEXIST is fine
    if (slash == std::string::npos)
      break;
  }
  LoadMeta();
}

void OpfsHandler::OnMounted(const std::string& path) {
  mount_point_ = path;
  while (mount_point_.size() > 1 &&
         mount_point_[mount_point_.size() - 1] == '/') {
    mount_point_.erase(mount_point_.size() - 1);
  }
}

bool OpfsHandler::IsWorldWritable(const std::string& pathname) {
  return !read_only_;
}

std::string OpfsHandler::ToOpfsPath(const std::string& pathname) const {
  std::string rel = pathname;
  if (pathname.compare(0, mount_point_.size(), mount_point_) == 0)
    rel = pathname.substr(mount_point_.size());
  if (!rel.empty() && rel[0] != '/')
    rel = "/" + rel;
  while (rel.size() > 1 && rel[rel.size() - 1] == '/')
    rel.erase(rel.size() - 1);
  if (rel == "/")
    rel.clear();
  return base_ == "/" ? (rel.empty() ? "/" : rel) : base_ + rel;
}

scoped_refptr<FileStream> OpfsHandler::open(
    int fd, const std::string& pathname, int oflag, mode_t cmode) {
  if (!initialized_) {
    errno = EIO;
    return NULL;
  }
  if (read_only_ && ((oflag & O_ACCMODE) != O_RDONLY ||
                     (oflag & (O_CREAT | O_TRUNC)))) {
    errno = EROFS;
    return NULL;
  }
  const std::string opfs_path = ToOpfsPath(pathname);
  arc_opfs::EntryType type = arc_opfs::kNone;
  double size = 0;
  double mtime = 0;
  const int stat_result = arc_opfs::Stat(opfs_path, &type, &size, &mtime);
  if (stat_result < 0 && stat_result != -ENOENT) {
    errno = -stat_result;
    return NULL;
  }
  if (stat_result == -ENOENT && read_only_ && opfs_path == base_)
    type = arc_opfs::kDirectory;  // a library not made yet: empty
  if (type == arc_opfs::kDirectory) {
    if ((oflag & O_ACCMODE) != O_RDONLY || (oflag & O_CREAT)) {
      errno = EISDIR;
      return NULL;
    }
    return new DirectoryFileStream("opfs", pathname, this,
                                   static_cast<time_t>(mtime));
  }
  if (oflag & O_DIRECTORY) {
    errno = (type == arc_opfs::kNone) ? ENOENT : ENOTDIR;
    return NULL;
  }
  if (type == arc_opfs::kNone && !(oflag & O_CREAT)) {
    errno = ENOENT;
    return NULL;
  }
  if (type == arc_opfs::kFile && (oflag & O_CREAT) && (oflag & O_EXCL)) {
    errno = EEXIST;
    return NULL;
  }

  arc_opfs::Request r = {};
  r.op = arc_opfs::kOpen;
  r.path = opfs_path.c_str();
  if (oflag & O_CREAT)
    r.flags |= arc_opfs::kCreate;
  if (oflag & O_EXCL)
    r.flags |= arc_opfs::kExclusive;
  if ((oflag & O_TRUNC) && (oflag & O_ACCMODE) != O_RDONLY)
    r.flags |= arc_opfs::kTruncateOnOpen;
  if (read_only_)
    r.flags |= arc_opfs::kReadOnly;
  const int id = arc_opfs::Call(&r);
  if (id < 0) {
    errno = -id;
    return NULL;
  }
  // A file created with execute bits (an unpacked tool, a tar's member)
  // keeps them; other new files have the default mode.
  if (type == arc_opfs::kNone && (cmode & 0111) && !read_only_) {
    char octal[16];
    snprintf(octal, sizeof(octal), "%o", static_cast<unsigned>(cmode & 0755));
    Record('M', Key(pathname), octal);
  }
  return new OpfsFile(id, oflag, pathname, this);
}

Dir* OpfsHandler::OnDirectoryContentsNeeded(const std::string& name) {
  std::vector<arc_opfs::Entry> entries;
  const int result = arc_opfs::List(ToOpfsPath(name), &entries);
  if (result < 0 && !(result == -ENOENT && read_only_ && ToOpfsPath(name) == base_)) {
    errno = -result;
    return NULL;
  }
  // DirectoryManager adds "." and "..", sorts, and fills d_ino like the rest
  // of posix_translation.
  DirectoryManager listing;
  std::string dir = name;
  if (dir.empty() || dir[dir.size() - 1] != '/')
    dir += '/';
  listing.MakeDirectories(dir);
  const std::string key = Key(name);
  for (size_t i = 0; i < entries.size(); ++i) {
    if (entries[i].is_directory)
      listing.MakeDirectories(dir + entries[i].name + "/");
    else if (key != "/" || entries[i].name != kMetaName)
      listing.AddFileWithType(dir + entries[i].name, Dir::REGULAR);
  }
  // The directory's symlinks (kept in the table, not in OPFS).
  if (links_) {
    live_dirs_.insert(key);
    const std::string below = key == "/" ? "/" : key + "/";
    for (MetaMap::const_iterator it = meta_.lower_bound(below);
         it != meta_.end() && it->first.compare(0, below.size(), below) == 0; ++it) {
      if (it->second.is_link && it->first.find('/', below.size()) == std::string::npos)
        listing.AddFileWithType(dir + it->first.substr(below.size()), Dir::SYMLINK);
    }
  }
  return listing.OpenDirectory(dir);
}

int OpfsHandler::mkdir(const std::string& pathname, mode_t mode) {
  if (read_only_) {
    errno = EROFS;
    return -1;
  }
  return Posix(arc_opfs::PathOp(arc_opfs::kMkdir, ToOpfsPath(pathname)));
}

int OpfsHandler::rename(const std::string& oldpath,
                        const std::string& newpath) {
  if (read_only_) {
    errno = EROFS;
    return -1;
  }
  if (oldpath == newpath)
    return 0;
  const std::string old_key = Key(oldpath);
  const std::string new_key = Key(newpath);
  // POSIX rename() replaces an existing file; OPFS move() may refuse to.
  arc_opfs::EntryType type = arc_opfs::kNone;
  double size = 0;
  double mtime = 0;
  const bool new_is_link = FindLink(new_key) != NULL;
  if (!new_is_link &&
      arc_opfs::Stat(ToOpfsPath(newpath), &type, &size, &mtime) >= 0) {
    if (type == arc_opfs::kFile) {
      const int r = arc_opfs::PathOp(arc_opfs::kUnlink, ToOpfsPath(newpath));
      if (r < 0)
        return Posix(r);
    } else if (FindLink(old_key)) {
      errno = EISDIR;  // a symlink over a directory
      return -1;
    }
  }
  // A symlink moves in the table only.
  if (FindLink(old_key)) {
    Record('D', new_key, std::string());
    Record('R', old_key, new_key);
    live_dirs_.insert(DirKey(new_key));
    return 0;
  }
  const int r = arc_opfs::PathOp(arc_opfs::kRename, ToOpfsPath(oldpath),
                                 ToOpfsPath(newpath));
  if (r < 0)
    return Posix(r);
  // What the table keeps for the file, or the directory's tree, moves too.
  if (new_is_link || meta_.count(new_key))
    Record('D', new_key, std::string());
  if (HasMetaAt(old_key)) {
    Record('R', old_key, new_key);
    live_dirs_.erase(old_key);
  }
  return 0;
}

int OpfsHandler::rmdir(const std::string& pathname) {
  if (read_only_) {
    errno = EROFS;
    return -1;
  }
  if (ToOpfsPath(pathname) == base_) {  // the mount point itself
    errno = EBUSY;
    return -1;
  }
  const std::string key = Key(pathname);
  if (FindLink(key)) {
    errno = ENOTDIR;
    return -1;
  }
  if (HasLinksBelow(key)) {  // its symlinks are not in OPFS
    errno = ENOTEMPTY;
    return -1;
  }
  const int r = arc_opfs::PathOp(arc_opfs::kRmdir, ToOpfsPath(pathname));
  if (r < 0)
    return Posix(r);
  live_dirs_.erase(key);
  if (HasMetaAt(key))
    Record('D', key, std::string());
  return 0;
}

int OpfsHandler::stat(const std::string& pathname, struct stat* out) {
  arc_opfs::EntryType type = arc_opfs::kNone;
  double size = 0;
  double mtime = 0;
  const int result =
      arc_opfs::Stat(ToOpfsPath(pathname), &type, &size, &mtime);
  if (result == -ENOENT && read_only_ && ToOpfsPath(pathname) == base_) {
    // A read-only mount whose directory is not made yet (an empty library).
    FillStat(pathname, arc_opfs::kDirectory, 4096, 0, out);
    return 0;
  }
  if (result < 0) {
    // A symlink (VirtualFileSystem follows them before it asks; this is for
    // the link itself, e.g. a dangling one).
    const Meta* link = (result == -ENOENT && links_) ? FindLink(Key(pathname)) : NULL;
    if (link) {
      memset(out, 0, sizeof(*out));
      out->st_mode = S_IFLNK | 0777;
      out->st_nlink = 1;
      out->st_size = link->link.size();
      out->st_blksize = 4096;
      return 0;
    }
    return Posix(result);
  }
  FillStat(pathname, type, size, mtime, out);
  ApplyKeptMode(pathname, out);
  return 0;
}

int OpfsHandler::statfs(const std::string& pathname, struct statfs* out) {
  return StatFsForOpfs(out);
}

int OpfsHandler::truncate(const std::string& pathname, off64_t length) {
  if (read_only_) {
    errno = EROFS;
    return -1;
  }
  scoped_refptr<FileStream> file = open(-1, pathname, O_WRONLY, 0);
  if (!file)
    return -1;
  return file->ftruncate(length);
}

int OpfsHandler::unlink(const std::string& pathname) {
  if (read_only_) {
    errno = EROFS;
    return -1;
  }
  const std::string key = Key(pathname);
  if (FindLink(key)) {
    Record('D', key, std::string());
    return 0;
  }
  const int r = arc_opfs::PathOp(arc_opfs::kUnlink, ToOpfsPath(pathname));
  if (r >= 0 && meta_.count(key))
    Record('D', key, std::string());
  return Posix(r);
}

int OpfsHandler::utimes(const std::string& pathname,
                        const struct timeval times[2]) {
  // OPFS keeps its own modification times; accept and ignore.
  struct stat st;
  return stat(pathname, &st);
}

// --- MountPointDirHandler --------------------------------------------------

MountPointDirHandler::MountPointDirHandler(const std::vector<std::string>& children)
    : FileSystemHandler("MountPointDirHandler"), children_(children) {}

MountPointDirHandler::~MountPointDirHandler() {}

void MountPointDirHandler::OnMounted(const std::string& path) {
  mount_point_ = path;
  while (mount_point_.size() > 1 && mount_point_[mount_point_.size() - 1] == '/')
    mount_point_.erase(mount_point_.size() - 1);
}

bool MountPointDirHandler::IsSelf(const std::string& pathname) const {
  std::string p = pathname;
  while (p.size() > 1 && p[p.size() - 1] == '/')
    p.erase(p.size() - 1);
  return p == mount_point_;
}

scoped_refptr<FileStream> MountPointDirHandler::open(
    int fd, const std::string& pathname, int oflag, mode_t cmode) {
  if (!IsSelf(pathname)) {
    errno = (oflag & O_CREAT) ? EROFS : ENOENT;
    return NULL;
  }
  if ((oflag & O_ACCMODE) != O_RDONLY || (oflag & O_CREAT)) {
    errno = EISDIR;
    return NULL;
  }
  return new DirectoryFileStream("mountdir", pathname, this, 0);
}

Dir* MountPointDirHandler::OnDirectoryContentsNeeded(const std::string& name) {
  if (!IsSelf(name)) {
    errno = ENOENT;
    return NULL;
  }
  DirectoryManager listing;
  const std::string dir = mount_point_ + "/";
  listing.MakeDirectories(dir);
  for (size_t i = 0; i < children_.size(); ++i)
    listing.MakeDirectories(dir + children_[i] + "/");
  return listing.OpenDirectory(dir);
}

int MountPointDirHandler::stat(const std::string& pathname, struct stat* out) {
  if (!IsSelf(pathname)) {
    errno = ENOENT;
    return -1;
  }
  FillStat(mount_point_, arc_opfs::kDirectory, 4096, 0, out);
  return 0;
}

int MountPointDirHandler::statfs(const std::string& pathname, struct statfs* out) {
  return DoStatFsForData(out);
}

// --- OpfsFile --------------------------------------------------------------

OpfsFile::OpfsFile(int id, int oflag, const std::string& pathname,
                   OpfsHandler* handler)
    : FileStream(oflag, pathname), id_(id), position_(0), handler_(handler) {}

OpfsFile::~OpfsFile() {
  arc_opfs::ScopedKeepLock keep;  // the last reference may go mid-bookkeeping
  arc_opfs::Request r = {};
  r.op = arc_opfs::kClose;
  r.id = id_;
  arc_opfs::Call(&r);
}

int OpfsFile::GetSizeAndMtime(double* size, double* mtime) const {
  arc_opfs::Request r = {};
  r.op = arc_opfs::kGetSize;
  r.id = id_;
  const int result = arc_opfs::Call(&r);
  if (result >= 0) {
    *size = r.size;
    if (mtime)
      *mtime = r.mtime;
  }
  return result;
}

ssize_t OpfsFile::ReadAt(void* buf, size_t count, off64_t offset) {
  if ((oflag() & O_ACCMODE) == O_WRONLY) {
    errno = EBADF;
    return -1;
  }
  if (count == 0)
    return 0;
  arc_opfs::Request r = {};
  r.op = arc_opfs::kRead;
  r.id = id_;
  r.buf = buf;
  r.len = static_cast<int32_t>(count);
  r.offset = static_cast<double>(offset);
  return Posix(arc_opfs::Call(&r));
}

ssize_t OpfsFile::WriteAt(const void* buf, size_t count, off64_t offset) {
  if ((oflag() & O_ACCMODE) == O_RDONLY) {
    errno = EBADF;
    return -1;
  }
  if (count == 0)
    return 0;
  arc_opfs::Request r = {};
  r.op = arc_opfs::kWrite;
  r.id = id_;
  r.buf = const_cast<void*>(buf);
  r.len = static_cast<int32_t>(count);
  r.offset = static_cast<double>(offset);
  return Posix(arc_opfs::Call(&r));
}

ssize_t OpfsFile::read(void* buf, size_t count) {
  const ssize_t n = ReadAt(buf, count, position_);
  if (n > 0)
    position_ += n;
  return n;
}

ssize_t OpfsFile::write(const void* buf, size_t count) {
  if (oflag() & O_APPEND) {
    double size = 0;
    const int r = GetSizeAndMtime(&size, NULL);
    if (r < 0)
      return Posix(r);
    position_ = static_cast<off64_t>(size);
  }
  const ssize_t n = WriteAt(buf, count, position_);
  if (n > 0)
    position_ += n;
  return n;
}

ssize_t OpfsFile::pread(void* buf, size_t count, off64_t offset) {
  if (offset < 0) {
    errno = EINVAL;
    return -1;
  }
  return ReadAt(buf, count, offset);
}

ssize_t OpfsFile::PwriteImpl(const void* buf, size_t count, off64_t offset) {
  if (offset < 0) {
    errno = EINVAL;
    return -1;
  }
  return WriteAt(buf, count, offset);
}

// FIONREAD, as ReadonlyFile has it: what read() would return before EOF
// (FileInputStream.available() asks; SharedPreferences reads files so).
int OpfsFile::ioctl(int request, va_list ap) {
  if (request == FIONREAD) {
    double size = 0;
    const int r = GetSizeAndMtime(&size, NULL);
    if (r < 0)
      return Posix(r);
    int* argp = va_arg(ap, int*);
    const off64_t left = static_cast<off64_t>(size) - position_;
    *argp = left > 0 ? static_cast<int>(left) : 0;
    return 0;
  }
  return FileStream::ioctl(request, ap);
}

off64_t OpfsFile::lseek(off64_t offset, int whence) {
  off64_t base = 0;
  switch (whence) {
    case SEEK_SET:
      break;
    case SEEK_CUR:
      base = position_;
      break;
    case SEEK_END: {
      double size = 0;
      const int r = GetSizeAndMtime(&size, NULL);
      if (r < 0)
        return Posix(r);
      base = static_cast<off64_t>(size);
      break;
    }
    default:
      errno = EINVAL;
      return -1;
  }
  if (base + offset < 0) {
    errno = EINVAL;
    return -1;
  }
  position_ = base + offset;
  return position_;
}

int OpfsFile::fstat(struct stat* out) {
  double size = 0;
  double mtime = 0;
  const int r = GetSizeAndMtime(&size, &mtime);
  if (r < 0)
    return Posix(r);
  FillStat(pathname(), arc_opfs::kFile, size, mtime, out);
  handler_->ApplyKeptMode(pathname(), out);
  return 0;
}

int OpfsFile::fstatfs(struct statfs* out) { return StatFsForOpfs(out); }

int OpfsFile::fsync() {
  arc_opfs::Request r = {};
  r.op = arc_opfs::kFlush;
  r.id = id_;
  return Posix(arc_opfs::Call(&r));
}

int OpfsFile::fdatasync() { return fsync(); }

int OpfsFile::ftruncate(off64_t length) {
  if ((oflag() & O_ACCMODE) == O_RDONLY) {
    errno = EINVAL;
    return -1;
  }
  if (length < 0) {
    errno = EINVAL;
    return -1;
  }
  arc_opfs::Request r = {};
  r.op = arc_opfs::kTruncate;
  r.id = id_;
  r.offset = static_cast<double>(length);
  return Posix(arc_opfs::Call(&r));
}

void* OpfsFile::mmap(void* addr, size_t length, int prot, int flags,
                     off_t offset) {
  if ((flags & MAP_SHARED) && (prot & PROT_WRITE) &&
      (oflag() & O_ACCMODE) == O_RDONLY) {
    errno = EACCES;
    return MAP_FAILED;
  }
  arc_opfs::ScopedKeepLock keep;  // inside the VFS's memory-region walk
  // Copy-in mapping on anonymous pages (inside the kernel, ::mmap is the
  // wasm page allocator).
  void* result = ::mmap(addr, length, prot | PROT_WRITE,
                        (flags & MAP_FIXED) | MAP_PRIVATE | MAP_ANONYMOUS,
                        -1, 0);
  if (result == MAP_FAILED)
    return MAP_FAILED;
  if (ReadAt(result, length, offset) < 0 && errno != EBADF) {
    const int saved = errno;
    ::munmap(result, length);
    errno = saved;
    return MAP_FAILED;
  }
  if ((flags & MAP_SHARED) && (prot & PROT_WRITE)) {
    SharedMapping m = {length, offset};
    shared_mappings_[result] = m;
  }
  return result;
}

int OpfsFile::munmap(void* addr, size_t length) {
  arc_opfs::ScopedKeepLock keep;  // inside the VFS's memory-region walk
  std::map<void*, SharedMapping>::iterator it = shared_mappings_.find(addr);
  if (it != shared_mappings_.end()) {
    // Write back the part of the mapping that lies within the file.
    double size = 0;
    if (GetSizeAndMtime(&size, NULL) >= 0 && it->second.offset < size) {
      size_t n = it->second.length;
      if (it->second.offset + static_cast<off_t>(n) > size)
        n = static_cast<size_t>(size - it->second.offset);
      WriteAt(addr, n, it->second.offset);
    }
    shared_mappings_.erase(it);
  }
  return ::munmap(addr, length);
}

const char* OpfsFile::GetStreamType() const { return "opfs"; }

size_t OpfsFile::GetSize() const {
  double size = 0;
  return GetSizeAndMtime(&size, NULL) >= 0 ? static_cast<size_t>(size) : 0;
}

}  // namespace posix_translation
