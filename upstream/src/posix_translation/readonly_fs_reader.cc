// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "posix_translation/readonly_fs_reader.h"

#include <arpa/inet.h>
#include <string.h>
#include <utility>
#include <vector>

#include "base/strings/string_piece.h"
#include "base/strings/string_util.h"
#include "common/alog.h"
#include "posix_translation/address_util.h"
#include "posix_translation/path_util.h"

namespace posix_translation {

namespace {

struct FileInfo_ {
  std::string filename;
  std::string link_target;
  uint32_t offset;
  size_t size;
  time_t mtime;
  ReadonlyFsReader::FileType file_type;
};

}  // namespace

ReadonlyFsReader::ReadonlyFsReader(const unsigned char* filesystem_image) {
  ParseImage(filesystem_image);
}

ReadonlyFsReader::~ReadonlyFsReader() {
}

bool ReadonlyFsReader::GetMetadata(const std::string& filename,
                                   Metadata* metadata) const {
  FileToMemory::const_iterator it = file_objects_.find(filename);
  if (it == file_objects_.end())
    return false;
  *metadata = it->second;
  return true;
}

bool ReadonlyFsReader::Exist(const std::string& filename) const {
  if (file_objects_.count(filename) > 0)
    return true;
  return file_names_.StatDirectory(filename);
}

bool ReadonlyFsReader::IsDirectory(const std::string& filename) const {
  return file_names_.StatDirectory(filename);
}

Dir* ReadonlyFsReader::OpenDirectory(const std::string& name) {
  return file_names_.OpenDirectory(name);
}

void ReadonlyFsReader::ParseImage(const unsigned char* image_metadata) {
  // The padding in the image is always for the 64k-page environment.
  static const size_t kNaCl64PageSize = 64 * 1024;
  // FS image must be aligned to the (native) page size. Otherwise, mmap() will
  // return unaligned address.
  ALOG_ASSERT(AlignTo(image_metadata, util::GetPageSize()) == image_metadata);

  const unsigned char* p = image_metadata;
  uint32_t num_files = 0;  // arc-wasm: size_t is not uint32_t on wasm32.
  p = ReadUInt32BE(p, &num_files);

  std::vector<struct FileInfo_> files;
  files.reserve(num_files);

  for (size_t i = 0; i < num_files; ++i) {
    uint32_t offset = 0;
    p = ReadUInt32BE(p, &offset);
    uint32_t size = 0;
    p = ReadUInt32BE(p, &size);
    uint32_t mtime = 0;
    p = ReadUInt32BE(p, &mtime);
    uint32_t file_type = 0;
    p = ReadUInt32BE(p, &file_type);
    std::string filename = reinterpret_cast<const char*>(p);
    p += filename.length() + 1;
    std::string link_target;
    if (file_type == kSymbolicLink) {
      link_target = reinterpret_cast<const char*>(p);
      p += link_target.length() + 1;
    }
    const struct FileInfo_ f =
        { filename, link_target, offset, size, static_cast<time_t>(mtime),
          static_cast<FileType>(file_type) };
    files.push_back(f);
  }

  // Find the beginning of the content.
  ptrdiff_t metadata_size = p - image_metadata;
  size_t pad_len = 0;
  if (metadata_size % kNaCl64PageSize)
    pad_len = kNaCl64PageSize - (metadata_size % kNaCl64PageSize);
  metadata_size += pad_len;

  for (size_t i = 0; i < files.size(); ++i) {
#if defined(DEBUG_POSIX_TRANSLATION)
    ALOGI("Found a read-only file: %s %zu bytes "
          "(at offset 0x%x, mtime %ld)",
          files[i].filename.c_str(), files[i].size,
          files[i].offset + metadata_size, files[i].mtime);
#endif
    const Metadata value = {
      // The offset value in the image is relative to the beginning of the
      // content. To convert it to the file offset, add |metadata_size|.
      static_cast<off_t>(files[i].offset) + metadata_size,
      files[i].size, files[i].mtime, files[i].file_type,
      files[i].link_target
    };
    if (files[i].file_type == kEmptyDirectory) {
      file_names_.MakeDirectories(files[i].filename);
    } else {
      bool result = file_objects_.insert(
          std::make_pair(files[i].filename, value)).second;
      ALOG_ASSERT(result);
      result = file_names_.AddFile(files[i].filename);
      ALOG_ASSERT(result);
    }
  }
}

// static
const unsigned char* ReadonlyFsReader::ReadUInt32BE(
    const unsigned char* p, uint32_t* out_result) {
  p = AlignTo(p, 4);
  *out_result = ntohl(*reinterpret_cast<const uint32_t*>(p));
  return p + sizeof(uint32_t);
}

}  // namespace posix_translation
