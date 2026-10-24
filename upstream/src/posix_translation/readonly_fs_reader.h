// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POSIX_TRANSLATION_READONLY_FS_READER_H_
#define POSIX_TRANSLATION_READONLY_FS_READER_H_

#include <time.h>

#include <string>

#include "base/basictypes.h"
#include "base/containers/hash_tables.h"
#include "gtest/gtest_prod.h"
#include "posix_translation/directory_manager.h"
#include "posix_translation/file_system_handler.h"
#include "posix_translation/virtual_file_system.h"

namespace posix_translation {

// Note: This class is not thread-safe.
class ReadonlyFsReader {
 public:
  // ReadonlyFsReader does not own the |filesystem_image| pointer.
  explicit ReadonlyFsReader(const unsigned char* filesystem_image);
  ~ReadonlyFsReader();

  // File type constants, which should be consistent with ones in
  // create_readonly_fs_image.py.
  enum FileType {
    kRegularFile = 0,
    kSymbolicLink = 1,
    kEmptyDirectory = 2,
  };

  struct Metadata {
    off_t offset;
    size_t size;
    time_t mtime;
    FileType file_type;
    std::string link_target;
    bool operator==(const Metadata& rhs) const {  // for EXPECT_EQ.
      return (offset == rhs.offset && size == rhs.size && mtime == rhs.mtime &&
              file_type == rhs.file_type && link_target == rhs.link_target);
    }
  };

  // Returns true and writes information of the |filename| in
  // |metadata|. Returns false if the file does not exist in the file system.
  bool GetMetadata(const std::string& filename, Metadata* metadata) const;

  // Returns true if |filename| exists in the file system. Note that this
  // function returns true when |filename| is a directory name.
  bool Exist(const std::string& filename) const;

  // Returns true if |filename| refers an existing directory.
  bool IsDirectory(const std::string& filename) const;

  // Returns a list of files in the |name| directory. NULL if |name| is unknown.
  Dir* OpenDirectory(const std::string& name);

 private:
  friend class ReadonlyFsReaderTest;
  FRIEND_TEST(ReadonlyFsReaderTest, TestAlignTo);
  FRIEND_TEST(ReadonlyFsReaderTest, TestParseImage);
  FRIEND_TEST(ReadonlyFsReaderTest, TestParseImageProd);

  template<typename T>
  static T* AlignTo(T* p, size_t boundary) {
    uintptr_t u = reinterpret_cast<uintptr_t>(p);
    u = (u + (boundary - 1)) & ~(boundary - 1);
    return reinterpret_cast<T*>(u);
  }

  // Parses |filesystem_image| and update member variables.
  void ParseImage(const unsigned char* filesystem_image);

  // Reads 4-byte big-endian integer from a next 4B boundary of |p|, assigns the
  // integer to |out_result|, and returns |p| + padding-to-the-boundary +
  // sizeof(uint32_t).
  static const unsigned char* ReadUInt32BE(const unsigned char* p,
                                           uint32_t* out_result);

  // A hash_map from a file name to its metadata such as the size of the file.
  typedef base::hash_map<std::string, Metadata> FileToMemory;  // NOLINT
  FileToMemory file_objects_;
  DirectoryManager file_names_;

  DISALLOW_COPY_AND_ASSIGN(ReadonlyFsReader);
};

}  // namespace posix_translation
#endif  // POSIX_TRANSLATION_READONLY_FS_READER_H_
