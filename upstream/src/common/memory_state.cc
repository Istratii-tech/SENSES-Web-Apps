// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "common/memory_state.h"

#include <errno.h>
#include <execinfo.h>
#include <malloc.h>
#include <stdio.h>

#include <sstream>
#include <algorithm>

#include "base/memory/singleton.h"
#include "base/strings/stringprintf.h"
#include "base/synchronization/lock.h"
#include "common/alog.h"
#include "common/backtrace.h"

#if defined(__native_client__)
static const uintptr_t kTrampolinesStartAddress = 0x10000;
static const uintptr_t kIRTStartAddress = 0xfa00000;
#endif

// TODO(crbug.com/238463): Drop this.
#if defined(__native_client__)
extern "C" int nacl_list_mappings(
    arc::NaClMemMappingInfo* info, size_t count, size_t* result_count);
#endif

namespace arc {

ProcessMapHeader::ProcessMapHeader(const struct dl_phdr_info* info, int idx) {
  type_ = info->dlpi_phdr[idx].p_type;
  library_ = info->dlpi_name;
  base_address_ = info->dlpi_addr;
  object_address_ = info->dlpi_phdr[idx].p_vaddr;
  memory_size_ = info->dlpi_phdr[idx].p_memsz;
  file_offset_ = info->dlpi_phdr[idx].p_offset;
  flags_ = info->dlpi_phdr[idx].p_flags;

#if defined(__native_client__)
  // Handle special cases to work around limitations of phdr runtime info.
  if (library_.size() == 0)
    library_ = std::string("arc_") + ARC_TARGET + ".nexe";
#endif
}

std::string ProcessMapHeader::GetTypeStr() const {
  switch (type_) {
    case PT_NULL: return "NULL";
    case PT_LOAD: return "LOAD";
    case PT_DYNAMIC: return "DYNAMIC";
    case PT_INTERP: return "INTERP";
    case PT_NOTE: return "NOTE";
    case PT_SHLIB: return "SHLIB";
    case PT_PHDR: return "PHDR";
    case PT_TLS: return "TLS";
  }
  return base::StringPrintf("type%d", type_);
}

std::string ProcessMapHeader::GetFlagsStr() const {
  return base::StringPrintf(
      "%s%s%s", (flags_ & PF_R ? "R" : ""), (flags_ & PF_W ? "W" : ""),
      (flags_ & PF_X ? "X" : ""));
}

std::string ProcessMapHeader::ConvertToString() const {
  // We do not use PRIxxx macros here because Bionic for KitKat no longer
  // defines such macros for C++.
  return base::StringPrintf(
      "%10x - %10x base %10x %s/%-3s %s %d",
      GetVirtualAddress(), GetVirtualAddress() + memory_size_,
      base_address_, GetTypeStr().c_str(), GetFlagsStr().c_str(),
      GetLibrary().c_str(), file_offset_);
}

std::string ProcessMapHeader::ConvertToJSON() const {
  // We do not use PRIxxx macros here. See ConvertToString().
  return base::StringPrintf(
      "{"
          "\"type\":\"%s\","
          "\"library\":\"%s\","
          "\"baseAddress\":%u,"
          "\"objectAddress\":%u,"
          "\"memorySize\":%u,"
          "\"fileOffset\":%u,"
          "\"flags\":%u"
      "}",
      GetTypeStr().c_str(),
      GetLibrary().c_str(),
      GetBaseAddress(),
      GetObjectAddress(),
      GetMemorySize(),
      GetFileOffset(),
      GetFlags());
}

int ProcessMapHeader::DumpPhdrCallback(
    struct dl_phdr_info* info, size_t size, void* data) {
  List* list = static_cast<List*>(data);
  for (int i = 0; i < info->dlpi_phnum; i++) {
    list->push_back(ProcessMapHeader(info, i));
  }
  return 0;
}

void ProcessMapHeader::DumpLayout(ProcessMapHeader::List* list) {
#if defined(__arm__) && (__native_client__)
  // TODO(crbug.com/256982): Implement this.
  ALOGW("ProcessMapHeader::DumpLayout is not supported on ARM NaCl yet.");
#else
  dl_iterate_phdr(DumpPhdrCallback, list);
#endif
}

bool ProcessMapHeader::CompareByVirtualAddress(
    ProcessMapHeader i, ProcessMapHeader j) {
  return (i.GetVirtualAddress() < j.GetVirtualAddress());
}

void ProcessMapHeader::SortByVirtualAddress(ProcessMapHeader::List* list) {
  std::sort((*list).begin(), (*list).end(), CompareByVirtualAddress);
}

void ProcessMapHeader::AddSyntheticLibraries(
    size_t loader_size, size_t irt_size, ProcessMapHeader::List* list) {
#if defined(__native_client__)
  // Add synthetic entries for special NaCl regions.

  // Trampolines.
  struct dl_phdr_info trampolines;
  ElfW(Phdr) trampolines_phdr;
  memset(&trampolines, 0, sizeof(trampolines));
  memset(&trampolines_phdr, 0, sizeof(trampolines_phdr));
  trampolines.dlpi_name = "NaCl Trampolines + runnable-ld.so";
  trampolines.dlpi_addr = kTrampolinesStartAddress;
  trampolines.dlpi_phdr = &trampolines_phdr;
  trampolines.dlpi_phnum = 1;
  trampolines_phdr.p_type = PT_LOAD;
  trampolines_phdr.p_vaddr = 0;
  trampolines_phdr.p_memsz = loader_size;
  trampolines_phdr.p_offset = 0;
  trampolines_phdr.p_flags = 0;
  list->push_back(ProcessMapHeader(&trampolines, 0));

  // IRT.
  struct dl_phdr_info irt;
  ElfW(Phdr) irt_phdr;
  memset(&irt, 0, sizeof(irt));
  memset(&irt_phdr, 0, sizeof(irt_phdr));
#if defined(__i386__)
  irt.dlpi_name = "nacl_irt_x86_32.nexe";
#elif defined(__x86_64__)
  irt.dlpi_name = "nacl_irt_x86_64.nexe";
#elif defined(__arm__)
  irt.dlpi_name = "nacl_irt_arm.nexe";
#else
# error "Unsupported NaCl platform"
#endif
  irt.dlpi_addr = kIRTStartAddress;
  irt.dlpi_phdr = &irt_phdr;
  irt.dlpi_phnum = 1;
  irt_phdr.p_type = PT_LOAD;
  irt_phdr.p_vaddr = 0;
  irt_phdr.p_memsz = irt_size;
  irt_phdr.p_offset = 0;
  irt_phdr.p_flags = 0;
  list->push_back(ProcessMapHeader(&irt, 0));
#endif
}

void ProcessMapHeader::PrintLayout(int type_filter) {
  List list;
  DumpLayout(&list);
  SortByVirtualAddress(&list);
  std::stringstream sstr;
  for (uint i = 0; i < list.size(); i++) {
    if (type_filter != -1 && type_filter != list[i].type_)
      continue;
    sstr << list[i].ConvertToString() << "\n";
  }
  // Use stderr to avoid extra stuff prepended by logger.
  fprintf(stderr, "%s", sstr.str().c_str());
}

std::string ProcessMapHeader::ConvertListToJSON(
    const ProcessMapHeader::List& list) {
  std::string result;
  result += "[";
  for (uint i = 0; i < list.size(); ++i) {
    if (i != 0)
      result += ",";
    result += list[i].ConvertToJSON();
  }
  result += "]";
  return result;
}

// MemoryMappingBacktraceMap keeps a mapping of start address to end address
// plus backtrace.
struct MemoryMappingBacktrace {
  static const int kBacktraceCapacity = 100;

  uintptr_t end;  // Keeping this as uintptr_t to allow comparisons without
                  // casting.

  // A raw backtrace as a series of code addresses. Decoded to symbols on
  // demand.
  void* backtrace[kBacktraceCapacity];
  int backtrace_size;
};

MemoryMappingBacktraceMap::MemoryMappingBacktraceMap()
      : mu_(new base::Lock) {
  backtracer_ = BacktraceInterface::Get();
}

MemoryMappingBacktraceMap::~MemoryMappingBacktraceMap() {
  delete mu_;
  delete backtracer_;
  for (Map::iterator i = memory_.begin(); i != memory_.end(); ++i) {
    delete i->second;
  }
}

int MemoryMappingBacktraceMap::GetUninterestingLayers() {
  // Number of layers to cull from the backtrace:
  // BacktracerInterface::Backtrace
  // MemoryMappingBacktraceMap::MapCurrentStackFrame
  // __wrap_mmap
  return 3;
}

MemoryMappingBacktraceMap* MemoryMappingBacktraceMap::GetInstance() {
  return Singleton<MemoryMappingBacktraceMap,
                   LeakySingletonTraits<MemoryMappingBacktraceMap> >::get();
}

void MemoryMappingBacktraceMap::MapCurrentStackFrame(
    void* addr, size_t length) {
  Unmap(addr, length);
  base::AutoLock lock(*mu_);
  size_t addri = reinterpret_cast<size_t>(addr);
  MemoryMappingBacktrace* trace = new MemoryMappingBacktrace;
  trace->backtrace_size = backtracer_->Backtrace(
      trace->backtrace, MemoryMappingBacktrace::kBacktraceCapacity);
  trace->end = addri + length;
  memory_[addri] = trace;
}

void MemoryMappingBacktraceMap::Unmap(void* addr, size_t length) {
  // This operation is a NOP in the case where there are no mappings
  // in [addr:addr+length). (Matches some munmap definitions and
  // relied on inside Add above).
  base::AutoLock lock(*mu_);
  size_t addri = reinterpret_cast<size_t>(addr);
  // Loop over all regions intersecting the area to remove.
  Map::iterator start = memory_.lower_bound(addri);
  // If the first region starting at least at addri is does not begin at
  // exactly addri, the region in front of it may need to be clipped.
  // Go back one assuming we are not on the first region already.
  if (start != memory_.begin() && start->first != addri)
    --start;
  Map::iterator end = memory_.upper_bound(addri + length);
  Map::iterator i = start;
  while (i != end) {
    // Split the current region if needed.
    //
    // There are six cases.
    // |--| = original region.
    // (--) = region to remove [addr, addr + length).
    // xxxx = region deleted
    //
    // (i)
    // a    b             c    d
    // |-----xxxxxxxxxxxxx-----|
    //      (-------------)
    //
    // (ii)
    // b    a        c    d
    //      |xxxxxxxx-----|
    // (-------------)
    //
    // (iii)
    // a    b        d    c
    // |-----xxxxxxxx|
    //      (-------------)
    //
    // (iv)
    // b    a             d    c
    //      |xxxxxxxxxxxxx|
    // (-----------------------)
    //
    // (v)
    // a     d    b     c
    // |-----|
    //            (-----)
    //
    // (vi)
    // b     c    a     d
    //            |-----|
    // (-----)
    //
    // If the regions not overlap (cases v and vi), skip to the next region.
    if (addri + length <= i->first ||
        i->second->end <= addri) {
      ++i;
      continue;
    }
    // In the remaining cases, note that only in the cases where b > a does a
    // left half survive the split (from a to b). Similarly, only when c > d
    // does a right half survive (from c to d).
    // Remove the old region, compute the ranges of the left and right sections,
    // keeping only those with positive size.
    size_t left_start = i->first;
    size_t left_end = addri;
    size_t right_start = addri + length;
    size_t right_end = i->second->end;

    // Extract the current area.
    MemoryMappingBacktrace* trace = i->second;
    Map::iterator next = i;
    ++next;
    memory_.erase(i);

    // Add left and right divisions if they are needed.
    if (left_end > left_start) {
      MemoryMappingBacktrace* left_trace = new MemoryMappingBacktrace;
      *left_trace = *trace;
      left_trace->end = left_end;
      memory_[left_start] = left_trace;
    }
    if (right_end > right_start) {
      MemoryMappingBacktrace* right_trace = new MemoryMappingBacktrace;
      *right_trace = *trace;
      right_trace->end = right_end;
      memory_[right_start] = right_trace;
    }

    delete trace;
    i = next;
  }
}

std::string MemoryMappingBacktraceMap::ConvertBacktraceToJSON(void* addr) {
  base::AutoLock lock(*mu_);
  size_t addri = reinterpret_cast<size_t>(addr);
  // Find the first range after addr.
  Map::iterator i = memory_.upper_bound(addri);
  // If it is the lowest range, give up.
  if (i == memory_.begin())
    return "[]";
  // Look at the range in front of it.
  --i;
  // If it does not include this address, give up.
  if (addri < i->first || addri >= i->second->end)
    return "[]";
  char** names = backtracer_->BacktraceSymbols(i->second->backtrace,
                                               i->second->backtrace_size);
  std::string result;
  result += "[";
  // Skip the initial uninteresting ones.
  for (int j = GetUninterestingLayers(); j < i->second->backtrace_size; ++j) {
    if (j != GetUninterestingLayers())
      result += ",";
    result += "\"";
    result += BacktraceInterface::DemangleAll(names[j]);
    result += "\"";
  }
  result += "]";
  free(names);
  return result;
}

MemoryMappingInfo::MemoryMappingInfo(const struct NaClMemMappingInfo* info)
    : info_(*info) {
}

void MemoryMappingInfo::DumpRegions(MemoryMappingInfo::List* list) {
#if defined(__native_client__)
  static const size_t capacity = 0x10000;
  NaClMemMappingInfo* regions = new NaClMemMappingInfo[capacity];
  size_t count = 0;
  int ret = nacl_list_mappings(regions, capacity, &count);
  if (ret != 0) {
    ALOGE("nacl_list_mappings failed with errno:%d", errno);
    delete[] regions;
    return;
  }
  if (count > capacity) {
    ALOGE("nacl_list_mappings returning only the first %u of %u mappings.",
          capacity, count);
    count = capacity;
  }
  for (size_t i = 0; i < count; ++i) {
    list->push_back(MemoryMappingInfo(&regions[i]));
  }
  delete[] regions;
#else
  ALOGW("nacl_list_mappings not available on this platform.");
#endif
}

std::string MemoryMappingInfo::ConvertToJSON() const {
  void* start = reinterpret_cast<void*>(GetStart());
  std::string backtrace = MemoryMappingBacktraceMap::GetInstance()->
      ConvertBacktraceToJSON(start);
  // We do not use PRIxxx macros here. See ConvertToString().
  return base::StringPrintf(
      "{"
          "\"start\":%u,"
          "\"size\":%u,"
          "\"prot\":%u,"
          "\"maxProt\":%u,"
          "\"vmmapType\":%u,"
          "\"backtrace\":%s"
      "}",
      GetStart(),
      GetSize(),
      GetProtection(),
      GetMaximumProtection(),
      GetVmmapType(),
      backtrace.c_str());
}

std::string MemoryMappingInfo::ConvertListToJSON(
    const MemoryMappingInfo::List& list) {
  std::string result;
  result += "[";
  for (uint i = 0; i < list.size(); ++i) {
    if (i != 0)
      result += ",";
    result += list[i].ConvertToJSON();
  }
  result += "]";
  return result;
}

void MemoryMappingInfo::ExtractNaClSizes(
    const MemoryMappingInfo::List& list,
    size_t* loader_size, size_t* irt_size) {
  *loader_size = 0;
  *irt_size = 0;
#if defined(__native_client__)
  for (uint i = 0; i < list.size(); ++i) {
    if (list[i].GetStart() == kTrampolinesStartAddress)
      *loader_size = list[i].GetSize();
    if (list[i].GetStart() == kIRTStartAddress)
      *irt_size = list[i].GetSize();
  }
#endif
}

std::string MemoryState::DumpMallocInfoAsJSON() {
  struct mallinfo minfo = mallinfo();
  return base::StringPrintf(
      "{"
          "\"arena\":%d,"
          "\"ordblks\":%d,"
          "\"hblks\":%d,"
          "\"hblkhd\":%d,"
          "\"uordblks\":%d,"
          "\"fordblks\":%d,"
          "\"keepcost\":%d"
      "}", minfo.arena,
           minfo.ordblks,
           minfo.hblks,
           minfo.hblkhd,
           minfo.uordblks,
           minfo.fordblks,
           minfo.keepcost);
}

std::string MemoryState::DumpAsJSON() {
  MemoryMappingInfo::List mmi;
  MemoryMappingInfo::DumpRegions(&mmi);
  std::string mmis = MemoryMappingInfo::ConvertListToJSON(mmi);
  size_t loader_size, irt_size;
  MemoryMappingInfo::ExtractNaClSizes(mmi, &loader_size, &irt_size);

  ProcessMapHeader::List pmh;
  ProcessMapHeader::DumpLayout(&pmh);
  ProcessMapHeader::AddSyntheticLibraries(loader_size, irt_size, &pmh);
  std::string pmhs = ProcessMapHeader::ConvertListToJSON(pmh);

  std::string minfo = DumpMallocInfoAsJSON();

  return base::StringPrintf(
      "{"
          "\"namespace\":\"memory-state\","
          "\"command\":\"snapshot\","
          "\"data\":{"
              "\"processMapHeaders\": %s,"
              "\"memoryMappingInfo\": %s,"
              "\"mallinfo\": %s,"
              "\"arcTarget\": \"%s\""
          "}"
      "}", pmhs.c_str(), mmis.c_str(), minfo.c_str(), ARC_TARGET);
}

}  // namespace arc
