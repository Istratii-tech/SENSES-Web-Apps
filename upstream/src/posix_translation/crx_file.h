// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POSIX_TRANSLATION_CRX_FILE_H_
#define POSIX_TRANSLATION_CRX_FILE_H_

#include <map>
#include <string>
#include <utility>

#include "base/memory/ref_counted.h"
#include "base/compiler_specific.h"
#include "common/export.h"
#include "posix_translation/pepper_file.h"
#include "ppapi/cpp/file_system.h"
#include "ppapi/utility/completion_callback_factory.h"

namespace posix_translation {

// A handler which handles read-only files in a CRX archive.
// TODO(crbug.com/274451): This handler does not support accessing files in an
// imported CRX specified by "import" section of manifest.json for the main CRX.
class ARC_EXPORT CrxFileHandler : public PepperFileHandler {
 public:
  CrxFileHandler();
  virtual ~CrxFileHandler();

  // Overrides PepperFileHandler's so that the function can return a cached
  // FileStream object.
  virtual scoped_refptr<FileStream> open(
      int fd, const std::string& pathname, int oflag, mode_t mode) OVERRIDE;

  // Overrides PepperFileHandler's so that the function initializes a CRX
  // filesystem instead of the LOCALPERSISTENT HTML5 filesystem.
  virtual void OpenPepperFileSystem(pp::Instance* instance) OVERRIDE;

 private:
  void OnFileSystemOpen(int32_t result,
                        const pp::FileSystem& file_system);
  pp::CompletionCallbackFactory<CrxFileHandler> factory_;

  typedef std::pair<std::string, int> StreamCacheKey;
  std::map<StreamCacheKey, scoped_refptr<FileStream> > stream_cache_;

  DISALLOW_COPY_AND_ASSIGN(CrxFileHandler);
};

}  // namespace posix_translation

#endif  // POSIX_TRANSLATION_CRX_FILE_H_
