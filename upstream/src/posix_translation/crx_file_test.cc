// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/compiler_specific.h"
#include "base/memory/scoped_ptr.h"
#include "base/synchronization/lock.h"
#include "posix_translation/crx_file.h"
#include "posix_translation/test_util/file_system_test_common.h"

namespace posix_translation {

class CrxFileTest : public FileSystemTestCommon {
 public:
  CrxFileTest() {}
  virtual void SetUp() OVERRIDE;

 private:
  scoped_ptr<PepperFileHandler> handler_;

  DISALLOW_COPY_AND_ASSIGN(CrxFileTest);
};

void CrxFileTest::SetUp() {
  FileSystemTestCommon::SetUp();
  SetUpCrxFileSystemConstructExpectations(kInstanceNumber);
  handler_.reset(new CrxFileHandler);
  handler_->OpenPepperFileSystem(instance_.get());
  {
    // CrxFileHandler::OnFileSystemOpen tries to acquire the mutex.
    base::AutoUnlock unlock(file_system_->mutex());
    RunCompletionCallbacks();
  }
}

TEST_F(CrxFileTest, TestConstructDestruct) {
  // Execute this empty test to let Valgrind examine the object construction
  // code.
}

}  // namespace posix_translation
