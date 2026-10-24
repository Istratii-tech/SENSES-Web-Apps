// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// A class to manage memory regions allocated via mmap with FileStream.

#ifndef POSIX_TRANSLATION_MEMORY_REGION_H_
#define POSIX_TRANSLATION_MEMORY_REGION_H_

#include <sys/stat.h>  // ino_t

#include <map>
#include <set>
#include <string>

#include "base/basictypes.h"
#include "base/memory/ref_counted.h"

namespace posix_translation {

class FileStream;

// A class that contains memory regions with corresponding FileStreams for
// mmap(), and calls underlying munmap() and mprotect() implementation for each
// FileStream.
class MemoryRegion {
 public:
  MemoryRegion();
  ~MemoryRegion();

  // Adds [addr, addr+length) to the PageToStream |map_|. Returns true on
  // success. Returns false if [addr, addr+length) overlaps an existing entry in
  // |map_|. |addr| must be aligned to 2-byte boundary. |length| must be a
  // multiple of 2. |offset| is just for printing debug information. |prot| is
  // a protection mode for the mapping (e.g. PROT_READ).
  // Note: In PageMapValue, when an address is aligned to 2-byte boundary, it is
  // treated as a "start" address. When it is not, it is an "end" address. To
  // fulfill this, |length| must be a multiple of 2. With the rule, we do not
  // have to have an "address type" member in PageMapValue which simplifies
  // the code a little.
  bool AddFileStreamByAddr(void* addr, size_t length, off64_t offset, int prot,
                           int flags, scoped_refptr<FileStream> stream);

  // Removes all memory regions in [addr, addr+length) from |map_|. This method
  // may call FileStream::munmap() against file streams in the |map_|. This
  // method may also remove zero, one or more file streams from the |map_|.
  // |addr| must be aligned to 2-byte boundary. |length| must be a multiple of
  // 2. If |call_munmap| is true, an underlying munmap() implementation is
  // called for each region found in [addr, addr+length). Returns 0 on success.
  // Returns -1 with errno on error. A special errno, ENOSYS, is set when no
  // memory region to remove is found.
  int RemoveFileStreamsByAddr(void* addr, size_t length, bool call_munmap);

  // Sets advice about use of memory regions in [addr, addr+length). |addr|,
  // |length|, and |advice| are the same with Linux's madvise().
  int SetAdviceByAddr(void* addr, size_t length, int advice);

  // Changes the protection mode of [addr, addr+length) to |prot|. This method
  // may call FileStream::mprotect() against file streams in the |map_|. |addr|
  // must be aligned to 2-byte boundary. |length| must be a multiple of 2.
  // Returns 0 on success. Returns -1 with errno on error. A special errno,
  // ENOSYS, is set when no memory region to modify is found.
  int ChangeProtectionModeByAddr(void* addr, size_t length, int prot);

  // Returns true if the file associated with |inode| is or was mmapped with
  // PROT_WRITE. Note that posix_translation never reuses inode numbers.
  // TODO(crbug.com/472211): Remove this.
  bool IsWriteMapped(ino_t inode) const;

  // Returns true if the file associated with |inode| is currently mmapped
  // regardless of the protection mode.
  // TODO(crbug.com/472211): Remove this.
  bool IsCurrentlyMapped(ino_t inode) const;

  // Get a list of mapped files in a human readable format.
  std::string GetMemoryMapAsString() const;

  struct PageMapValue {
    PageMapValue(size_t ref, off64_t offset, scoped_refptr<FileStream> stream);
    ~PageMapValue();

    size_t ref;  // this field is only for "start" nodes.
    off64_t offset;
    // Adding one ref count per a continuous memory region is necessary here.
    // This is because:
    //
    // 1) In user code, the fd might be closed right after mmap.
    //      fd = open(...);  // ref count == 1
    //      addr = mmap(fd, PAGESIZE);  // ref count == 2
    //      close(fd);  // ref count == 1
    //      munmap(addr, PAGESIZE);  // ref count == 0, |this| object is deleted
    //
    // 2) In user code, the mapped address might be partially unmapped.
    //      fd = open(...);  // ref count == 1
    //      addr = mmap(fd, PAGESIZE*3);  // ref count == 2
    //      close(fd);  // ref count == 1
    //      munmap(addr + PAGESIZE, PAGESIZE);  // ref count == 2
    //      munmap(addr, PAGESIZE);  // ref count == 1
    //      munmap(addr + PAGESIZE*2, PAGESIZE);  // ref count == 0
    scoped_refptr<FileStream> stream;
  };

  class PageMapVisitor {
   public:
    virtual ~PageMapVisitor() {}
    // Returns false if no more map walk is needed.
    virtual bool Visit(const PageMapValue& page_map,
                       char* start_addr, char* end_addr) = 0;
   protected:
    void WriteFailureLog(const char* name, bool visited,
                         char* start_addr, char* end_addr,
                         const scoped_refptr<FileStream> stream);
  };

 private:
  friend class MemoryRegionTest;
  friend class FileSystemTestCommon;

  typedef std::map<char*, PageMapValue> PageToStream;

  // Removes [remove_start, remove_end] from an existing memory region,
  // [region_start, region_end].
  // Examples:
  //   1. Complete removal.
  //   RemoveOneRegion(stream, 0x1000, 0x4000-1, 0x1000, 0x4000-1);
  //   2. Partial removal.
  //   RemoveOneRegion(stream, 0x2000, 0x3000-1, 0x1000, 0x4000-1);
  void RemoveOneRegion(PageMapValue value,
                       char* remove_start, char* remove_end,
                       char* region_start, char* region_end);

  // Returns true if the memory region [begin->first, end->first] overlaps an
  // existing region in |map_|.
  bool IsOverlap(PageToStream::const_iterator begin,
                 PageToStream::const_iterator end) const;

  // Returns a PageMapValue object if the exact region, [addr, addr+length),
  // already exists in |map_|. Otherwise returns NULL.
  PageMapValue* FindRegion(char* addr, size_t length);

  // Calls |task| on all FileStream in the memory region [addr, addr+length).
  void CallByAddr(char* addr, size_t length, PageMapVisitor* visitor);

  // Returns true if |addr| is not aligned to 2-byte boundary.
  static bool IsPageEndAddress(const void* addr);

  // This map is an equivalent to Linux kernel's vm_area_struct AVL tree.
  // Unlike the tree in kernel which only uses a "start" address as a key,
  // |map_| uses both "start" and "end" addresses. This is to make
  // AddFileStreamByAddr, especially the code for detecting memory region
  // overlaps, very simple.
  PageToStream map_;
  bool abort_on_unexpected_memory_maps_;  // For unit testing.

  // A set of inode numbers that is (or was) mapped with PROT_WRITE. Note that
  // this set is append-only. RemoveFileStreamsByAddr() does not modify this
  // set.
  std::set<ino_t> write_mapped_;

  DISALLOW_COPY_AND_ASSIGN(MemoryRegion);
};

}  // namespace posix_translation

#endif  // POSIX_TRANSLATION_MEMORY_REGION_H_
