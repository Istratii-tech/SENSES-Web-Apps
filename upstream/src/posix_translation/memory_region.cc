// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#define __STDC_FORMAT_MACROS  // for PRIxPTR.

#include "posix_translation/memory_region.h"

#include <inttypes.h>
#include <algorithm>  // for min and max
#include <utility>

#include "base/containers/hash_tables.h"
#include "base/strings/stringprintf.h"
#include "common/arc_strace.h"
#include "common/alog.h"
#include "posix_translation/address_util.h"
#include "posix_translation/file_stream.h"
#include "posix_translation/virtual_file_system.h"

namespace {
// In NaCl, all text regions that are used with PROT_EXEC should be mapped at
// lower memory < 256MB. Because of NaCl restriction, ::munmap() against text
// regions always fails.
#if defined(__native_client__)
const uintptr_t kTextEndAddress = 256 * 1024 * 1024;
#endif
}  // namespace

namespace posix_translation {

namespace {

std::string GetStreamPathname(scoped_refptr<FileStream> stream) {
  const std::string result = stream->pathname().empty() ?
      std::string("(anonymous mmap)") : stream->pathname();
  const std::string aux = stream->GetAuxInfo();
  if (!aux.empty())
    return result + " [" + aux + "]";
  return result;
}

class AdviseVisitor : public MemoryRegion::PageMapVisitor {
 public:
  explicit AdviseVisitor(int advice);
  virtual ~AdviseVisitor();

  int Finish();

  virtual bool Visit(const MemoryRegion::PageMapValue& page_map,
                     char* start_addr, char* end_addr) OVERRIDE;
 private:
  bool visited_;
  int errno_;
  const int advice_;
};

class ProtectionVisitor : public MemoryRegion::PageMapVisitor {
 public:
  ProtectionVisitor(int prot, std::set<ino_t>* out_write_mapped);
  virtual ~ProtectionVisitor();

  int Finish();

  virtual bool Visit(const MemoryRegion::PageMapValue& page_map,
                     char* start_addr, char* end_addr) OVERRIDE;

 private:
  bool visited_;
  const int prot_;
  std::set<ino_t>* write_mapped_;
};

AdviseVisitor::AdviseVisitor(int advice)
    : visited_(false), errno_(0), advice_(advice) {
}

AdviseVisitor::~AdviseVisitor() {
}

bool AdviseVisitor::Visit(const MemoryRegion::PageMapValue& page_map,
                          char* start_addr, char* end_addr) {
  ARC_STRACE_REPORT_HANDLER(page_map.stream->GetStreamType());
  ARC_STRACE_REPORT("(%p-%p \"%s\")",
                    start_addr,
                    end_addr + 1,
                    GetStreamPathname(page_map.stream).c_str());
  size_t length = static_cast<const char*>(end_addr) -
      start_addr + 1;
  int result = page_map.stream->madvise(start_addr, length, advice_);
  if (result)
    errno_ = errno;
  if (result > 0) {
    WriteFailureLog("madvise", visited_, start_addr, end_addr, page_map.stream);
    return false;
  }
  visited_ = true;
  return result == 0;
}

int AdviseVisitor::Finish() {
  // TODO(crbug.com/362862): Stop returning ENOSYS. We report ENOSYS since
  // MemoryRegion does not manage all regions, and madvise may be issued against
  // these missing regions, e.g., main.nexe, DT_NEEDED DSOs in main.nexe
  // loaded by ld-runnable.so, and so on. Returning ENOSYS helps to support
  // these cases in __wrap_madvise() side.
  if (!visited_)
    errno = ENOSYS;
  else if (errno_)
    errno = errno_;
  else
    return 0;
  return -1;
}

ProtectionVisitor::ProtectionVisitor(int prot, std::set<ino_t>* write_mapped)
    : visited_(false), prot_(prot), write_mapped_(write_mapped) {
}

ProtectionVisitor::~ProtectionVisitor() {
}

bool ProtectionVisitor::Visit(const MemoryRegion::PageMapValue& page_map,
                              char* start_addr, char* end_addr) {
  // TODO(crbug.com/427417): Split page_map if prot is inconsistent.
  ARC_STRACE_REPORT_HANDLER(page_map.stream->GetStreamType());
  ARC_STRACE_REPORT("(%p-%p \"%s\")",
                      start_addr,
                      end_addr + 1,
                      GetStreamPathname(page_map.stream).c_str());
  size_t length = end_addr - start_addr + 1;
  if (!page_map.stream->mprotect(start_addr, length, prot_)) {
    if (prot_ & PROT_WRITE)
      write_mapped_->insert(page_map.stream->inode());
  } else {
    WriteFailureLog(
        "mprotect", visited_, start_addr, end_addr, page_map.stream);
    return false;  // return early on error
  }
  visited_ = true;
  return true;
}

int ProtectionVisitor::Finish() {
  if (!visited_) {
    // TODO(crbug.com/362862): See comments at AdviseVisitor::Finish().
    errno = ENOSYS;
    return -1;
  }
  return 0;
}


}  // namespace

MemoryRegion::MemoryRegion() : abort_on_unexpected_memory_maps_(true) {
}

MemoryRegion::~MemoryRegion() {
}

bool MemoryRegion::AddFileStreamByAddr(
    void* addr, size_t length, off64_t offset, int prot, int flags,
    scoped_refptr<FileStream> stream) {
  ALOG_ASSERT(!IsPageEndAddress(addr) && !(length % 2));
  if (!length)
    return false;

  char* const addr_start = static_cast<char*>(addr);
  if (stream) {
    // Our mmap implementations usually only return an address that are not yet
    // mapped. For example, calling mmap twice against a file in Pepper,
    // Readonnly, and NaClManifest returns two different addresses. However, our
    // current MemoryFile::mmap() implementation does not follow the POSIX
    // convention. The method returns the same address when it is called twice
    // or more. Handles the special case first. See also http://crbug.com/366557
    PageMapValue* region = FindRegion(addr_start, length);
    if (region) {
      if (abort_on_unexpected_memory_maps_) {
        ALOG_ASSERT(!(flags & MAP_FIXED));
        ALOG_ASSERT(stream->ReturnsSameAddressForMultipleMmaps());
        ALOG_ASSERT(region->stream->ReturnsSameAddressForMultipleMmaps());
        ALOG_ASSERT(stream->pathname() == region->stream->pathname());
      }
      if (flags & MAP_FIXED ||
          !stream->ReturnsSameAddressForMultipleMmaps() ||
          !region->stream->ReturnsSameAddressForMultipleMmaps() ||
          stream->pathname() != region->stream->pathname()) {
        return false;
      }
      ++(region->ref);
      return true;
    }
  }

  const PageMapValue value(1, offset, stream);
  typedef std::pair<PageToStream::iterator, bool> InsertResult;
  InsertResult result_addr_start =
      map_.insert(std::make_pair(addr_start, value));

  // Fail if |addr_start| already exists in the map.
  if (!result_addr_start.second)
    return false;
  PageToStream::iterator addr_start_it = result_addr_start.first;

  char* const addr_end = addr_start + length - 1;
  ALOG_ASSERT(IsPageEndAddress(addr_end));
  InsertResult result_addr_end = map_.insert(std::make_pair(addr_end, value));

  // Fail if |addr_end| already exists in the map.
  if (!result_addr_end.second) {
    map_.erase(addr_start_it);
    return false;
  }
  PageToStream::iterator addr_end_it = result_addr_end.first;

  // Fail if [addr_start, addr_end) overlaps with one of the existing regions.
  // It happens for the following cases using MemoryFile.
  //   fd = ashmem_create_region();
  //   mmap(fd, 4096 /* length */);
  //   mmap(fd, 8192 /* different length */);  // fail here
  // If the second length is the same, FindRegion() in this function returns
  // the first region and works.
  if (IsOverlap(addr_start_it, addr_end_it)) {
    map_.erase(addr_start_it);
    map_.erase(addr_end_it);
    return false;
  }

  if (stream) {
    const ino_t inode = stream->inode();
    if (prot & PROT_WRITE)
      write_mapped_.insert(inode);
  }

  // You can uncomment this to print the memory mappings.
  //  ALOGI("\n%s", GetMemoryMapAsString().c_str());
  return true;
}

int MemoryRegion::RemoveFileStreamsByAddr(
    void* addr, size_t length, bool call_munmap) {
  // TODO(toyoshim): Rewrite this function to use PageMapVisitor.
  ALOG_ASSERT(!IsPageEndAddress(addr) && !(length % 2));
  if (!length) {
    errno = EINVAL;
    return -1;
  }

  char* const remove_start = static_cast<char*>(addr);
  PageMapValue* region = FindRegion(remove_start, length);
  if (region && region->ref > 1) {
    --(region->ref);
    return 0;
  }

  char* const remove_end = remove_start + length - 1;

  // Find the first region.
  PageToStream::iterator it = map_.lower_bound(remove_start);
  if (it == map_.end()) {
    // TODO(crbug.com/362862): Stop returning ENOSYS.
    errno = ENOSYS;
    return -1;
  }
  if (IsPageEndAddress(it->first)) {
    // An IsPageEndAddress element should not be the first one.
    ALOG_ASSERT(it != map_.begin());

    // This condition means that |remove_start| is in the midst of an existing
    // region.
    // <start A>                <end A>  <start B>    <end B>
    //     *-----------------------*         *-----------*
    //               ^             ^
    //         |remove_start|  |it->first|
    // The iterator should point <start A> so to shrink the region A.
    --it;
  }

  bool is_region_found = false;
  while (it != map_.end()) {
    PageToStream::iterator region_start_it = it;
    PageToStream::iterator region_end_it = ++it;
    // Since |region_start_it|, which is a !IsPageEndAddress element, is a valid
    // iterator, |region_end_it| (and |it| here) should also be valid.
    ALOG_ASSERT(it != map_.end());

    // Check if [region_start_it->first, region_end_it->first] overlaps
    // [remove_start, remove_end].
    if (remove_end < region_start_it->first)
      break;  // No overlap. No more memory regions to modify.

    // We do not support partial unmapping for a duplicated mmap region. Note
    // that memory_file.cc would return the same address from the two mmap calls
    // below:
    //   fd = ashmem_create_region();
    //   void* addr1 = mmap(fd, 4096*3 /* length */);
    //   void* addr2 = mmap(fd, 4096*3 /* the same length */);
    //   munmap(addr1 + 4096, 4096);  // fail
    ALOG_ASSERT((!abort_on_unexpected_memory_maps_) ||
                (region_start_it->second.ref == 1),
                "Cannot partially unmap a ref-counted region: "
                "unmap_addr=%p, unmap_length=%zu, "
                "mapped_addr=%p, mapped_length=%td",
                addr, length, region_start_it->first,
                region_end_it->first - region_start_it->first + 1);
    if (region_start_it->second.ref > 1) {
      errno = ENOSYS;  // return ENOSYS for unit tests.
      return -1;
    }

    char* remove_start_in_region =
        std::max(remove_start, region_start_it->first);
    char* remove_end_in_region = std::min(remove_end, region_end_it->first);

    // These two variables have to be assigned/updated here since
    // RemoveOneRegion might invalidate iterators.
    scoped_refptr<FileStream> current_stream = region_start_it->second.stream;
    ++it;  // for the next iteration.

    const PageMapValue value(1, region_start_it->second.offset, current_stream);
    RemoveOneRegion(value,
                    remove_start_in_region, remove_end_in_region,
                    region_start_it->first, region_end_it->first);
    is_region_found = true;  // modified at least one memory region.

    // IsMemoryRangeAvailable() may insert a null stream.
    if (current_stream) {
      // Call REPORT_HANDLER() so that the current function call is
      // categorized as |current_stream->GetStreamType()| rather than
      // |kVirtualFileSystemHandlerStr| in virtual_file_system.cc.
      ARC_STRACE_REPORT_HANDLER(current_stream->GetStreamType());
      ARC_STRACE_REPORT("(%p-%p \"%s\")",
                        remove_start_in_region,
                        remove_end_in_region + 1,
                        GetStreamPathname(current_stream).c_str());
      size_t length = remove_end_in_region - remove_start_in_region + 1;
      if (call_munmap) {
        if (current_stream->munmap(remove_start_in_region, length)) {
#if defined(__native_client__)
          if (remove_start_in_region <
              reinterpret_cast<char*>(kTextEndAddress)) {
            // This path is taken when a DSO is unloaded with dlclose(), but
            // under NaCl, unmapping text in the DSO with ::munmap() always
            // fails with EINVAL. Log with ALOGE since this is a memory leak.
            // TODO(crbug.com/380799): Stop special-casing NaCl once the
            // restriction is removed.
            ALOGE("NaCl does not support munmap() for text. "
                  "Leaked %zu bytes of memory: (%p-%p \"%s\")",
                  length,
                  remove_start_in_region,
                  remove_end_in_region + 1,
                  GetStreamPathname(current_stream).c_str());
            ARC_STRACE_REPORT("Do not call munmap for text under NaCl");
          } else  // NOLINT(readability/braces)
#endif
          {
            // munmap with a page-aligned |addr| and non-zero |length| should
            // never fail. Since this function only handles valid addr/length
            // pairs (see VFS::munmap), munmap failure here means that a serious
            // memory error has already occured. Abort here with ALOGE.
            ALOGE("FileStream::munmap failed with %d: (%p-%p \"%s\")",
                  errno,
                  remove_start_in_region,
                  remove_end_in_region + 1,
                  GetStreamPathname(current_stream).c_str());
            ALOG_ASSERT(false);
            return -1;
          }
        }
      } else {
        // Call OnUnmapByOverwritingMmap instead when |call_munmap| is false.
        current_stream->OnUnmapByOverwritingMmap(
            remove_start_in_region, length);
      }
    }

    ALOG_ASSERT(it == map_.end() || !IsPageEndAddress(it->first));
  }

  if (!is_region_found) {
    // TODO(crbug.com/362862): Stop returning ENOSYS.
    errno = ENOSYS;
    return -1;
  }
  // You can uncomment this to print the updated memory mappings.
  //  ALOGI("\n%s", GetMemoryMapAsString().c_str());
  return 0;
}

void MemoryRegion::RemoveOneRegion(PageMapValue value,
                                   char* remove_start, char* remove_end,
                                   char* region_start, char* region_end) {
  ALOG_ASSERT(!IsPageEndAddress(remove_start));
  ALOG_ASSERT(IsPageEndAddress(remove_end));
  ALOG_ASSERT(!IsPageEndAddress(region_start));
  ALOG_ASSERT(IsPageEndAddress(region_end));

  // Split [region_start, region_end] if needed.
  //
  // [new_region_right_start, new_region_right_end] might be created as a result
  // of the removal. For example, if the original region is [0,4], and [2,2] is
  // removed, new_region_left will be [0,1] and new_region_right will be [3,4].
  //
  // arc-wasm: whether each part is left is a comparison of the (unsigned)
  // addresses, not a ptrdiff_t length: in a 4 GB wasm memory a region can be
  // over 2 GB long (Hermes reserves its heap so: React Native apps), and the
  // length then overflowed to negative (assertion below, abort).
  char* const new_region_right_start = remove_end + 1;
  char* const new_region_right_end = region_end;
  ALOG_ASSERT(remove_end <= region_end);
  const bool has_new_region_right = remove_end < region_end;

  // [new_region_left_start, new_region_left_end] might also be created after
  // the removal.
  char* const new_region_left_start = region_start;
  char* const new_region_left_end = remove_start - 1;
  ALOG_ASSERT(remove_start >= region_start);
  const bool has_new_region_left = remove_start > region_start;

  // Delete the memory region (and create new one(s) if it's partial unmapping).
  if (has_new_region_left) {
    ALOG_ASSERT(IsPageEndAddress(new_region_left_end));
    bool result = map_.insert(
        std::make_pair(new_region_left_end, value)).second;
    ALOG_ASSERT(result);
  } else {
    size_t result = map_.erase(new_region_left_start);
    ALOG_ASSERT(result == 1);
  }

  if (has_new_region_right) {
    ALOG_ASSERT(!IsPageEndAddress(new_region_right_start));
    bool result = map_.insert(
        std::make_pair(new_region_right_start, value)).second;
    ALOG_ASSERT(result);
  } else {
    size_t result = map_.erase(new_region_right_end);
    ALOG_ASSERT(result == 1);
  }
}

int MemoryRegion::SetAdviceByAddr(void* addr, size_t length, int advice) {
  // Note: zero-length madvise succeeds on Linux. It returns with 0 without
  // setting advice.
  if (!length)
    return 0;

  AdviseVisitor visitor(advice);
  CallByAddr(static_cast<char*>(addr), length, &visitor);
  return visitor.Finish();
}

int MemoryRegion::ChangeProtectionModeByAddr(
    void* addr, size_t length, int prot) {
  // Note: zero-length mprotect succeeds on Linux. It returns with 0 without
  // changing protection mode.
  if (!length)
    return 0;

  ProtectionVisitor visitor(prot, &write_mapped_);
  CallByAddr(static_cast<char*>(addr), length, &visitor);
  return visitor.Finish();
}

bool MemoryRegion::IsWriteMapped(ino_t inode) const {
  return write_mapped_.count(inode) > 0;
}

bool MemoryRegion::IsCurrentlyMapped(ino_t inode) const {
  for (PageToStream::const_iterator it = map_.begin(); it != map_.end(); ++it) {
    scoped_refptr<FileStream> stream = it->second.stream;
    if (stream && (stream->inode() == inode))
      return true;
  }
  return false;
}

std::string MemoryRegion::GetMemoryMapAsString() const {
  std::string result =
    "Range                 Length           Offset     Backend  FileSize"
    "         Ref  Name\n";
  if (map_.empty()) {
    result += "(No memory mapped files)\n";
    return result;
  }

  typedef base::hash_map<std::string, size_t> BackendStat;  // NOLINT
  BackendStat per_backend;

  for (PageToStream::const_iterator it = map_.begin(); it != map_.end(); ++it) {
    char* start = it->first;
    int ref = it->second.ref;
    off64_t off = it->second.offset;
    scoped_refptr<FileStream> stream = it->second.stream;
    ++it;
    if (it == map_.end()) {
      ALOG_ASSERT("map_ is corrupted" == NULL);
      result += "memory map is corrupted!\n";
      break;
    }
    char* end = it->first;
    if (!stream)
      continue;

    const size_t len = end - start + 1;
    const std::string backend = stream->GetStreamType();

    result += base::StringPrintf(
        "0x%08" PRIxPTR "-0x%08" PRIxPTR " 0x%08x %4zuM 0x%08llx %-8s 0x%08x "
        "%4zuM %-4d %s\n",
        reinterpret_cast<uintptr_t>(start),
        // Add one to make it look more like /proc/<pid>/maps.
        reinterpret_cast<uintptr_t>(end) + 1,
        len,
        len / 1024 / 1024,
        static_cast<uint64_t>(off),
        backend.c_str(),
        stream->GetSize(),
        stream->GetSize() / 1024 / 1024,
        ref,
        GetStreamPathname(stream).c_str());
    per_backend[backend] += len;
  }

  if (!per_backend.empty()) {
    result += "Virtual memory usage per backend:\n";
    for (BackendStat::const_iterator it = per_backend.begin();
         it != per_backend.end(); ++it) {
      result += base::StringPrintf(
          " %-8s: %4zuMB (%zu bytes, %zu pages)\n",
          it->first.c_str(),
          it->second >> 20,
          it->second,
          it->second >> util::GetPageSizeAsNumBits());
    }
  }

  return result;
}

bool MemoryRegion::IsOverlap(PageToStream::const_iterator begin,
                             PageToStream::const_iterator end) const {
  // Return true if there is another element between |addr_start| and
  // |addr_end|.
  if (std::distance(begin, end) != 1)
    return true;

  // Return true if there are two "start" elements in a row.
  if (begin != map_.begin()) {
    --begin;
    const char* previous = begin->first;
    if (!IsPageEndAddress(previous))
      return true;
  }

  // No overlap. Do one more sanity check then return false.
  if (end != map_.end() && ++end != map_.end()) {
    const char* next = end->first;
    ALOG_ASSERT(!IsPageEndAddress(next));
  }
  return false;
}

void MemoryRegion::PageMapVisitor::WriteFailureLog(
  const char* name, bool visited, char* start_addr, char* end_addr,
  const scoped_refptr<FileStream> stream) {
  // Since we do not have a way to undo the previous FileStream call(s), and
  // can not provide a posix compatible behavior, abort here.
  // It is very unlikely to see the failure in practice.
  LOG_ALWAYS_FATAL("%sFileStream::%s %sfailed with %d: (%p-%p \"%s\")",
        (visited ? "One of " : ""),
        name,
        (visited ? "calls " : ""),
        errno,
        start_addr,
        end_addr + 1,
        GetStreamPathname(stream).c_str());
}

MemoryRegion::PageMapValue*
MemoryRegion::FindRegion(char* addr, size_t length) {
  PageToStream::iterator it = map_.find(addr);
  if (it == map_.end())
    return NULL;  // |addr| is not registered.

  // Check the 'end' node.
  PageToStream::iterator end_it(it);
  ++end_it;
  ALOG_ASSERT(end_it != map_.end());
  char* end_addr = addr + length - 1;

  if (end_it->first != end_addr)
    return NULL;

  return &(it->second);
}

void MemoryRegion::CallByAddr(
    char* addr, size_t length, PageMapVisitor* visitor) {
  ALOG_ASSERT(!(length % 2));
  ALOG_ASSERT(!IsPageEndAddress(addr) && visitor);

  char* const start_addr = addr;
  char* const end_addr = start_addr + length - 1;

  // Find the first region.
  PageToStream::const_iterator it = map_.lower_bound(start_addr);
  if (it == map_.end())
    return;

  if (IsPageEndAddress(it->first)) {
    // An IsPageEndAddress element should not be the first one.
    ALOG_ASSERT(it != map_.begin());
    --it;
  }

  while (it != map_.end()) {
    PageToStream::const_iterator region_start_it = it;
    PageToStream::const_iterator region_end_it = ++it;
    // Since |region_start_it|, which is a !IsPageEndAddress element, is a valid
    // iterator, |region_end_it| (and |it| here) should also be valid.
    ALOG_ASSERT(it != map_.end());

    // Check if [region_start_it->first, region_end_it->first] overlaps
    // [start_addr, end_addr].
    if (end_addr < region_start_it->first)
      break;  // No overlap. No more memory regions to visit.

    const PageMapValue& page_map = region_start_it->second;
    scoped_refptr<FileStream> current_stream = page_map.stream;
    // IsMemoryRangeAvailable() may insert a null stream.
    if (current_stream) {
      char* const start_in_region =
          std::max<char*>(start_addr, region_start_it->first);
      char* const end_in_region =
          std::min<char*>(end_addr, region_end_it->first);
      if (!visitor->Visit(page_map, start_in_region, end_in_region))
        break;
    }

    ++it;  // for the next iteration.
    ALOG_ASSERT(it == map_.end() || !IsPageEndAddress(it->first));
  }
}

// static
bool MemoryRegion::IsPageEndAddress(const void* addr) {
  return reinterpret_cast<uintptr_t>(addr) & 1;
}

MemoryRegion::PageMapValue::PageMapValue(
    size_t in_ref, off64_t in_offset, scoped_refptr<FileStream> in_stream)
    : ref(in_ref), offset(in_offset), stream(in_stream) {
}

MemoryRegion::PageMapValue::~PageMapValue() {
}

}  // namespace posix_translation
