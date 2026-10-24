// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "posix_translation/pepper_file.h"

#include "base/compiler_specific.h"
#include "base/memory/scoped_ptr.h"
#include "common/process_emulator.h"
#include "gtest/gtest.h"
#include "posix_translation/dir.h"
#include "posix_translation/file_system_handler.h"
#include "posix_translation/test_util/file_system_background_test_common.h"
#include "posix_translation/virtual_file_system.h"
#include "ppapi/utility/completion_callback_factory.h"
#include "ppapi_mocks/background_test.h"
#include "ppapi_mocks/background_thread.h"
#include "ppapi_mocks/ppapi_test.h"
#include "ppapi_mocks/ppb_file_io.h"
#include "ppapi_mocks/ppb_file_io_private.h"
#include "ppapi_mocks/ppb_file_ref.h"

using ::testing::Invoke;
using ::testing::NiceMock;
using ::testing::SetArgPointee;
using ::testing::StrEq;
using ::testing::WithArgs;

namespace posix_translation {

class PepperFileTest : public FileSystemBackgroundTestCommon<PepperFileTest> {
 public:
  DECLARE_BACKGROUND_TEST(TestAccess);
  DECLARE_BACKGROUND_TEST(TestAccessDirectory);
  DECLARE_BACKGROUND_TEST(TestAccessFail);
  DECLARE_BACKGROUND_TEST(TestFstat);
  DECLARE_BACKGROUND_TEST(TestFtruncateReadonly);
  DECLARE_BACKGROUND_TEST(TestMkdir);
  DECLARE_BACKGROUND_TEST(TestMkdirFail);
  DECLARE_BACKGROUND_TEST(TestMkdirNoPermission);
  DECLARE_BACKGROUND_TEST(TestOpenAppend);
  DECLARE_BACKGROUND_TEST(TestOpenCreat);
  DECLARE_BACKGROUND_TEST(TestOpenCreatExclusive);
  DECLARE_BACKGROUND_TEST(TestOpenCreatTruncate);
  DECLARE_BACKGROUND_TEST(TestOpenCreatWriteOnly);
  DECLARE_BACKGROUND_TEST(TestOpenDirectory);
  DECLARE_BACKGROUND_TEST(TestOpenDirectoryWithWriteAccess);
  DECLARE_BACKGROUND_TEST(TestOpenClose);
  DECLARE_BACKGROUND_TEST(TestOpenExclusiveFail);
  DECLARE_BACKGROUND_TEST(TestOpenNoentFail);
  DECLARE_BACKGROUND_TEST(TestOpenPermFail);
  DECLARE_BACKGROUND_TEST(TestOpenRead);
  DECLARE_BACKGROUND_TEST(TestOpenWithOpenDirectoryFlag);
  DECLARE_BACKGROUND_TEST(TestPacketCalls);
  DECLARE_BACKGROUND_TEST(TestRename);
  DECLARE_BACKGROUND_TEST(TestRenameInode);
  DECLARE_BACKGROUND_TEST(TestRenameInode2);
  DECLARE_BACKGROUND_TEST(TestRenameDirectoryCached);
  DECLARE_BACKGROUND_TEST(TestRenameEnoentFail);
  DECLARE_BACKGROUND_TEST(TestRenameEisdirFail);
  DECLARE_BACKGROUND_TEST(TestRequestHandleFail);
  DECLARE_BACKGROUND_TEST(TestStat);
  DECLARE_BACKGROUND_TEST(TestStatCache);
  DECLARE_BACKGROUND_TEST(TestStatCacheDisabled);
  DECLARE_BACKGROUND_TEST(TestStatCacheInvalidation);
  DECLARE_BACKGROUND_TEST(TestStatCacheWithTrailingSlash);
  DECLARE_BACKGROUND_TEST(TestStatDirectory);
  DECLARE_BACKGROUND_TEST(TestStatWithENOENT);
  DECLARE_BACKGROUND_TEST(TestTruncate);
  DECLARE_BACKGROUND_TEST(TestTruncateFail);
  DECLARE_BACKGROUND_TEST(TestUTime);
  DECLARE_BACKGROUND_TEST(TestUTimeFail);
  DECLARE_BACKGROUND_TEST(TestUnlink);
  DECLARE_BACKGROUND_TEST(TestUnlinkFail);

 protected:
  static const PP_Resource kFileRefResource = 74;
  static const PP_Resource kFileRefResource2 = 75;
  static const PP_Resource kFileIOResource = 76;

  PepperFileTest()
    : default_executor_(&bg_, PP_OK),
      ppb_file_io_(NULL),
      ppb_file_io_private_(NULL),
      ppb_file_ref_(NULL) {
  }
  virtual void SetUp() OVERRIDE;

  void SetUpOpenExpectations(
      const char* path,
      int native_flags,
      CompletionCallbackExecutor* open_callback_executor,
      CompletionCallbackExecutor* request_handle__callback_executor,
      CompletionCallbackExecutor* query_callback_executor,
      const PP_FileInfo& file_info);
  void SetUpStatExpectations(
      CompletionCallbackExecutor* callback_executor,
      const PP_FileInfo& file_info);
  void SetUpFtruncateExpectations(
      CompletionCallbackExecutor* callback_executor,
      off64_t length);
  void SetUpMkdirExpectations(const char* path,
                              CompletionCallbackExecutor* callback_executor);
  void SetUpRenameExpectations(const char* oldpath,
                               const char* newpath,
                               CompletionCallbackExecutor* callback_executor);
  void SetUpUnlinkExpectations(const char* path,
                               CompletionCallbackExecutor* callback_executor);
  void SetUpUTimeExpectations(const char* path,
                              time_t time,
                              CompletionCallbackExecutor* callback_executor);
  void SetUpNoFileRefExpectations(
      CompletionCallbackExecutor* callback_executor,
      const PP_FileInfo& file_info);
  scoped_refptr<FileStream> OpenFile(int oflag);
  scoped_refptr<FileStream> OpenFileWithExpectations(int flags);
  bool IsDirectory(scoped_refptr<FileStream> file);
  void CheckStatStructure(const struct stat& st,
                          mode_t mode,
                          nlink_t link,
                          off64_t size,
                          ino_t inode,
                          time_t ctime,
                          time_t atime,
                          time_t mtime);
  void DisableCache();

  static int ConvertNativeOpenFlagsToPepper(int native_flags) {
    return PepperFileHandler::ConvertNativeOpenFlagsToPepper(native_flags);
  }

  static int ConvertPepperErrorToErrno(int pp_error) {
    return PepperFileHandler::ConvertPepperErrorToErrno(pp_error);
  }

  static const char kPepperPath[];
  static const char kAnotherPepperPath[];
  static const char kPepperOldDir[];
  static const char kPepperNewDir[];
  static const char kPepperPathNewDir[];
  static const time_t kTime;
  CompletionCallbackExecutor default_executor_;
  NiceMock<PPB_FileIO_Mock>* ppb_file_io_;
  NiceMock<PPB_FileIO_Private_Mock>* ppb_file_io_private_;
  NiceMock<PPB_FileRef_Mock>* ppb_file_ref_;
  scoped_ptr<PepperFileHandler> handler_;
};

#define EXPECT_ERROR(result, expected_error)     \
  EXPECT_EQ(-1, result);                         \
  EXPECT_EQ(expected_error, errno);              \
  errno = 0;

const char PepperFileTest::kPepperPath[] = "/old/pepperfs.file";
const char PepperFileTest::kAnotherPepperPath[] = "/old/another.pepperfs.file";
const char PepperFileTest::kPepperOldDir[] = "/old";
const char PepperFileTest::kPepperNewDir[] = "/new";
const char PepperFileTest::kPepperPathNewDir[] = "/new/pepperfs.file";
const time_t PepperFileTest::kTime = 1355707320;

void PepperFileTest::SetUp() {
  FileSystemBackgroundTestCommon<PepperFileTest>::SetUp();
  factory_.GetMock(&ppb_file_io_);
  factory_.GetMock(&ppb_file_io_private_);
  factory_.GetMock(&ppb_file_ref_);
  SetUpPepperFileSystemConstructExpectations(kInstanceNumber);
  handler_.reset(new PepperFileHandler);
  handler_->OpenPepperFileSystem(instance_.get());
  RunCompletionCallbacks();
}

void PepperFileTest::SetUpOpenExpectations(
    const char* path,
    int native_flags,
    CompletionCallbackExecutor* open_callback_executor,
    CompletionCallbackExecutor* request_handle_callback_executor,
    CompletionCallbackExecutor* query_callback_executor,
    const PP_FileInfo& file_info) {
  const int pepper_flags = ConvertNativeOpenFlagsToPepper(native_flags);

  EXPECT_CALL(*ppb_file_ref_, Create(kFileSystemResource, StrEq(path))).
    WillOnce(Return(kFileRefResource));

  EXPECT_CALL(*ppb_file_io_, Create(kInstanceNumber)).
    WillOnce(Return(kFileIOResource));
  // Note that kFileIOResource is not released until close() is called.
  EXPECT_CALL(*ppb_file_io_, Open(kFileIOResource,
                                  kFileRefResource,
                                  pepper_flags,
                                  _)).
    WillOnce(WithArgs<3>(
        Invoke(open_callback_executor,
               &CompletionCallbackExecutor::ExecuteOnMainThread)));
  if (open_callback_executor->final_result() == PP_OK) {
    int handle = -1;
    if (request_handle_callback_executor->final_result() == PP_OK)
      handle = dup(0);
    EXPECT_CALL(*ppb_file_io_private_,
                RequestOSFileHandle(kFileIOResource, _, _)).
        WillOnce(DoAll(
          SetArgPointee<1>(handle),
          WithArgs<2>(Invoke(
              request_handle_callback_executor,
              &CompletionCallbackExecutor::ExecuteOnMainThread)))).
        RetiresOnSaturation();
  }
}

void PepperFileTest::SetUpStatExpectations(
    CompletionCallbackExecutor* callback_executor,
    const PP_FileInfo& file_info) {
  EXPECT_CALL(*ppb_file_ref_, Create(kFileSystemResource, _)).
    WillOnce(Return(kFileRefResource));
  EXPECT_CALL(*ppb_file_ref_, Query(kFileRefResource,
                                    _,
                                    _)).
      WillOnce(DoAll(
          SetArgPointee<1>(file_info),
          WithArgs<2>(Invoke(
              callback_executor,
              &CompletionCallbackExecutor::ExecuteOnMainThread)))).
      RetiresOnSaturation();
}

void PepperFileTest::SetUpFtruncateExpectations(
    CompletionCallbackExecutor* callback_executor,
    off64_t length) {
  EXPECT_CALL(*ppb_file_io_, SetLength(kFileIOResource,
                                       length,
                                       _)).
    WillOnce(WithArgs<2>(Invoke(
        callback_executor,
        &CompletionCallbackExecutor::ExecuteOnMainThread))).
    RetiresOnSaturation();
}

void PepperFileTest::SetUpMkdirExpectations(
    const char* path, CompletionCallbackExecutor* callback_executor) {
  EXPECT_CALL(*ppb_file_ref_, Create(kFileSystemResource, StrEq(path))).
    WillOnce(Return(kFileRefResource));
  EXPECT_CALL(*ppb_file_ref_,
              MakeDirectory(kFileRefResource,
                            PP_MAKEDIRECTORYFLAG_EXCLUSIVE,
                            _)).
    WillOnce(WithArgs<2>(
        Invoke(callback_executor,
               &CompletionCallbackExecutor::ExecuteOnMainThread))).
    RetiresOnSaturation();
}

void PepperFileTest::SetUpRenameExpectations(
    const char* oldpath,
    const char* newpath,
    CompletionCallbackExecutor* callback_executor) {
  EXPECT_CALL(*ppb_file_ref_, Create(kFileSystemResource, StrEq(oldpath))).
    WillOnce(Return(kFileRefResource));
  EXPECT_CALL(*ppb_file_ref_, Create(kFileSystemResource, StrEq(newpath))).
    WillOnce(Return(kFileRefResource2));
  EXPECT_CALL(*ppb_file_ref_,
              Rename(kFileRefResource, kFileRefResource2, _)).
    WillOnce(WithArgs<2>(
        Invoke(callback_executor,
               &CompletionCallbackExecutor::ExecuteOnMainThread))).
    RetiresOnSaturation();
}

void PepperFileTest::SetUpUnlinkExpectations(
    const char* path, CompletionCallbackExecutor* callback_executor) {
  EXPECT_CALL(*ppb_file_ref_, Create(kFileSystemResource, StrEq(path))).
    WillOnce(Return(kFileRefResource));
  EXPECT_CALL(*ppb_file_ref_, Delete(kFileRefResource, _)).
    WillOnce(WithArgs<1>(
        Invoke(callback_executor,
               &CompletionCallbackExecutor::ExecuteOnMainThread))).
    RetiresOnSaturation();
}

void PepperFileTest::SetUpUTimeExpectations(
    const char* path,
    time_t time,
    CompletionCallbackExecutor* callback_executor) {
  EXPECT_CALL(*ppb_file_ref_, Create(kFileSystemResource, StrEq(path))).
    WillOnce(Return(kFileRefResource));
  EXPECT_CALL(*ppb_file_ref_, Touch(kFileRefResource, time, time, _)).
    WillOnce(WithArgs<3>(
        Invoke(callback_executor,
               &CompletionCallbackExecutor::ExecuteOnMainThread))).
    RetiresOnSaturation();
}

void PepperFileTest::SetUpNoFileRefExpectations(
    CompletionCallbackExecutor* callback_executor,
    const PP_FileInfo& file_info) {
  EXPECT_CALL(*ppb_file_ref_, Create(kFileSystemResource, _)).
    WillOnce(Return(kFileRefResource));
  EXPECT_CALL(*ppb_file_ref_, Query(kFileRefResource,
                                    _,
                                    _)).
      WillOnce(Return(PP_ERROR_FAILED)).
      RetiresOnSaturation();
}

scoped_refptr<FileStream> PepperFileTest::OpenFile(int oflag) {
  int fd = file_system_->fd_to_stream_->GetFirstUnusedDescriptor();
  return handler_->open(fd, kPepperPath, oflag, 0);
}

scoped_refptr<FileStream> PepperFileTest::OpenFileWithExpectations(
    int open_flags) {
  PP_FileInfo file_info = {};
  SetUpOpenExpectations(kPepperPath, open_flags,
                        &default_executor_, &default_executor_,
                        &default_executor_, file_info);
  return OpenFile(open_flags);
}

bool PepperFileTest::IsDirectory(scoped_refptr<FileStream> file) {
  if (!file) {
    ADD_FAILURE() << "No file stream";
    return false;
  }
  return strcmp(file->GetStreamType(), "pepper") != 0;
}

void PepperFileTest::CheckStatStructure(const struct stat& st,
                                        mode_t mode,
                                        nlink_t link,
                                        off64_t size,
                                        ino_t inode,
                                        time_t ctime,
                                        time_t atime,
                                        time_t mtime) {
  EXPECT_EQ(static_cast<dev_t>(0), st.st_dev);
  EXPECT_EQ(inode, st.st_ino);
  // PepperFile does not set permission bits, relying VirtualFileSystem.
  EXPECT_EQ(mode, st.st_mode);
  EXPECT_EQ(link, st.st_nlink);
  // UID and GID must not be set in FileSystemHandler.
  EXPECT_EQ(arc::kRootUid, st.st_uid);
  EXPECT_EQ(arc::kRootGid, st.st_gid);
  EXPECT_EQ(static_cast<dev_t>(0), st.st_rdev);
  // Sanity check of the sizes.
  EXPECT_EQ(sizeof(size), sizeof(st.st_size));
  EXPECT_EQ(size, st.st_size);
  EXPECT_EQ(static_cast<blksize_t>(4096), st.st_blksize);
  EXPECT_EQ(static_cast<blkcnt_t>(0), st.st_blocks);
  // We casts st_[cam]time for bionic. In bionic, their type is
  // unsigned long and time_t is long.
  EXPECT_EQ(ctime, static_cast<time_t>(st.st_ctime));
  EXPECT_EQ(atime, static_cast<time_t>(st.st_atime));
  EXPECT_EQ(mtime, static_cast<time_t>(st.st_mtime));
}

void PepperFileTest::DisableCache() {
  handler_->DisableCacheForTesting();
}

TEST_F(PepperFileTest, ConstructPendingDestruct) {
  // Just tests that the initialization that runs in SetUp() itself
  // succeeds.
}

TEST_F(PepperFileTest, TestConvertNativeOpenFlagsToPepper) {
  EXPECT_EQ(PP_FILEOPENFLAG_WRITE,
            ConvertNativeOpenFlagsToPepper(O_WRONLY));
  EXPECT_EQ(PP_FILEOPENFLAG_READ,
            ConvertNativeOpenFlagsToPepper(O_RDONLY));
  EXPECT_EQ(PP_FILEOPENFLAG_READ | PP_FILEOPENFLAG_WRITE,
            ConvertNativeOpenFlagsToPepper(O_RDWR));
  // Unknown flag should be treated as O_RDONLY.
  EXPECT_EQ(PP_FILEOPENFLAG_READ,
            ConvertNativeOpenFlagsToPepper(O_ACCMODE));
  // _WRITE and _APPEND flags are exclusive in Pepper.
  EXPECT_EQ(PP_FILEOPENFLAG_APPEND,
            ConvertNativeOpenFlagsToPepper(O_WRONLY | O_APPEND));
  EXPECT_EQ(PP_FILEOPENFLAG_READ | PP_FILEOPENFLAG_APPEND,
            ConvertNativeOpenFlagsToPepper(O_RDWR | O_APPEND));
  // O_RDONLY | O_APPEND is an error. O_APPEND should be ignored.
  EXPECT_EQ(PP_FILEOPENFLAG_READ,
            ConvertNativeOpenFlagsToPepper(O_RDONLY | O_APPEND));
  // Test misc flags.
  EXPECT_EQ(PP_FILEOPENFLAG_WRITE | PP_FILEOPENFLAG_CREATE,
            ConvertNativeOpenFlagsToPepper(O_WRONLY | O_CREAT));
  EXPECT_EQ(PP_FILEOPENFLAG_WRITE | PP_FILEOPENFLAG_CREATE |
            PP_FILEOPENFLAG_EXCLUSIVE,
            ConvertNativeOpenFlagsToPepper(O_WRONLY | O_CREAT | O_EXCL));
  EXPECT_EQ(PP_FILEOPENFLAG_WRITE | PP_FILEOPENFLAG_TRUNCATE,
            ConvertNativeOpenFlagsToPepper(O_WRONLY | O_TRUNC));
  // Test unsupported flags.
  EXPECT_EQ(PP_FILEOPENFLAG_READ | PP_FILEOPENFLAG_WRITE,
            ConvertNativeOpenFlagsToPepper(O_RDWR | O_NOCTTY));
  EXPECT_EQ(PP_FILEOPENFLAG_READ | PP_FILEOPENFLAG_WRITE,
            ConvertNativeOpenFlagsToPepper(O_RDWR | O_NONBLOCK));
  EXPECT_EQ(PP_FILEOPENFLAG_READ | PP_FILEOPENFLAG_WRITE,
            ConvertNativeOpenFlagsToPepper(O_RDWR | O_SYNC));
  EXPECT_EQ(PP_FILEOPENFLAG_READ | PP_FILEOPENFLAG_WRITE,
            ConvertNativeOpenFlagsToPepper(O_RDWR | O_ASYNC));
  EXPECT_EQ(PP_FILEOPENFLAG_READ | PP_FILEOPENFLAG_WRITE,
            ConvertNativeOpenFlagsToPepper(O_RDWR | O_NOFOLLOW));
  EXPECT_EQ(PP_FILEOPENFLAG_READ | PP_FILEOPENFLAG_WRITE,
            ConvertNativeOpenFlagsToPepper(O_RDWR | O_CLOEXEC));
  EXPECT_EQ(PP_FILEOPENFLAG_READ | PP_FILEOPENFLAG_WRITE,
            ConvertNativeOpenFlagsToPepper(O_RDWR | O_NOATIME));
}

TEST_F(PepperFileTest, TestConvertPepperErrorToErrno) {
  EXPECT_EQ(ENOENT, ConvertPepperErrorToErrno(PP_ERROR_FILENOTFOUND));
  EXPECT_EQ(EEXIST, ConvertPepperErrorToErrno(PP_ERROR_FILEEXISTS));
  EXPECT_EQ(EPERM, ConvertPepperErrorToErrno(PP_ERROR_NOACCESS));
  EXPECT_EQ(ENOMEM, ConvertPepperErrorToErrno(PP_ERROR_NOMEMORY));
  EXPECT_EQ(ENOSPC, ConvertPepperErrorToErrno(PP_ERROR_NOQUOTA));
  EXPECT_EQ(ENOSPC, ConvertPepperErrorToErrno(PP_ERROR_NOSPACE));
  EXPECT_EQ(EISDIR, ConvertPepperErrorToErrno(PP_ERROR_NOTAFILE));
  // We use ENOENT for all other error code.
  EXPECT_EQ(ENOENT, ConvertPepperErrorToErrno(PP_ERROR_FAILED));
  EXPECT_EQ(ENOENT, ConvertPepperErrorToErrno(PP_ERROR_USERCANCEL));
}

TEST_BACKGROUND_F(PepperFileTest, TestOpenRead) {
  base::AutoLock lock(file_system_->mutex());
  scoped_refptr<FileStream> file(OpenFileWithExpectations(O_RDONLY));
  EXPECT_TRUE(file.get());
  EXPECT_FALSE(IsDirectory(file));
}

TEST_BACKGROUND_F(PepperFileTest, TestOpenCreat) {
  base::AutoLock lock(file_system_->mutex());
  scoped_refptr<FileStream> file(OpenFileWithExpectations(O_RDWR | O_CREAT));
  EXPECT_TRUE(file.get());
  EXPECT_FALSE(IsDirectory(file));
}

TEST_BACKGROUND_F(PepperFileTest, TestOpenCreatExclusive) {
  base::AutoLock lock(file_system_->mutex());
  scoped_refptr<FileStream> file(
      OpenFileWithExpectations(O_RDWR | O_CREAT | O_EXCL));
  EXPECT_TRUE(file.get());
  EXPECT_FALSE(IsDirectory(file));
}

TEST_BACKGROUND_F(PepperFileTest, TestOpenCreatTruncate) {
  base::AutoLock lock(file_system_->mutex());
  scoped_refptr<FileStream> file(
      OpenFileWithExpectations(O_RDWR | O_CREAT | O_TRUNC));
  EXPECT_TRUE(file.get());
  EXPECT_FALSE(IsDirectory(file));
}

TEST_BACKGROUND_F(PepperFileTest, TestOpenCreatWriteOnly) {
  base::AutoLock lock(file_system_->mutex());
  scoped_refptr<FileStream> file(
      OpenFileWithExpectations(O_WRONLY | O_CREAT));
  EXPECT_TRUE(file.get());
  EXPECT_FALSE(IsDirectory(file));
}

TEST_BACKGROUND_F(PepperFileTest, TestOpenAppend) {
  base::AutoLock lock(file_system_->mutex());
  scoped_refptr<FileStream> file(
      OpenFileWithExpectations(O_RDWR | O_APPEND));
  EXPECT_TRUE(file.get());
  EXPECT_FALSE(IsDirectory(file));
}

TEST_BACKGROUND_F(PepperFileTest, TestOpenWithOpenDirectoryFlag) {
  base::AutoLock lock(file_system_->mutex());
  PP_FileInfo file_info = {};
  const int flags = O_RDONLY | O_DIRECTORY;
  SetUpOpenExpectations(kPepperPath, flags,
                        &default_executor_, &default_executor_,
                        &default_executor_, file_info);
  scoped_refptr<FileStream> file(OpenFile(flags));
  EXPECT_FALSE(file.get());
  EXPECT_EQ(ENOTDIR, errno);
}

TEST_BACKGROUND_F(PepperFileTest, TestOpenDirectory) {
  base::AutoLock lock(file_system_->mutex());
  CompletionCallbackExecutor executor(&bg_, PP_ERROR_NOTAFILE);
  PP_FileInfo file_info = {};
  int flags = O_RDONLY;
  SetUpOpenExpectations(kPepperPath, flags,
                        &executor, &default_executor_, &default_executor_,
                        file_info);
  scoped_refptr<FileStream> file(OpenFile(flags));
  EXPECT_TRUE(file.get());
  EXPECT_TRUE(IsDirectory(file));

  flags = O_RDONLY | O_DIRECTORY;
  SetUpOpenExpectations(kPepperPath, flags,
                        &executor, &default_executor_, &default_executor_,
                        file_info);
  scoped_refptr<FileStream> file2(OpenFile(flags));
  EXPECT_TRUE(file2.get());
  EXPECT_TRUE(IsDirectory(file2));
}

TEST_BACKGROUND_F(PepperFileTest, TestOpenDirectoryWithWriteAccess) {
  base::AutoLock lock(file_system_->mutex());
  CompletionCallbackExecutor executor(&bg_, PP_ERROR_NOTAFILE);
  PP_FileInfo file_info = {};
  int flags = O_RDWR;
  SetUpOpenExpectations(kPepperPath, flags,
                        &executor, &default_executor_, &default_executor_,
                        file_info);
  scoped_refptr<FileStream> file(OpenFile(flags));
  EXPECT_FALSE(file.get());
  EXPECT_EQ(EISDIR, errno);

  flags = O_WRONLY;
  SetUpOpenExpectations(kPepperPath, flags,
                        &executor, &default_executor_, &default_executor_,
                        file_info);
  scoped_refptr<FileStream> file2(OpenFile(flags));
  EXPECT_FALSE(file2.get());
  EXPECT_EQ(EISDIR, errno);
}

TEST_BACKGROUND_F(PepperFileTest, TestOpenNoentFail) {
  base::AutoLock lock(file_system_->mutex());
  CompletionCallbackExecutor executor(&bg_, PP_ERROR_FILENOTFOUND);
  int flags = O_RDONLY;
  PP_FileInfo file_info = {};
  SetUpOpenExpectations(kPepperPath, flags,
                        &executor, &default_executor_, &default_executor_,
                        file_info);
  scoped_refptr<FileStream> file(OpenFile(flags));
  EXPECT_FALSE(file.get());
  EXPECT_EQ(ENOENT, errno);
}

TEST_BACKGROUND_F(PepperFileTest, TestOpenPermFail) {
  base::AutoLock lock(file_system_->mutex());
  CompletionCallbackExecutor executor(&bg_, PP_ERROR_NOACCESS);
  int flags = O_RDWR | O_CREAT;
  PP_FileInfo file_info = {};
  SetUpOpenExpectations(kPepperPath, flags,
                        &executor, &default_executor_, &default_executor_,
                        file_info);
  scoped_refptr<FileStream> file2(OpenFile(flags));
  EXPECT_FALSE(file2.get());
  EXPECT_EQ(EPERM, errno);
}

TEST_BACKGROUND_F(PepperFileTest, TestOpenExclusiveFail) {
  base::AutoLock lock(file_system_->mutex());
  CompletionCallbackExecutor executor(&bg_, PP_ERROR_FILEEXISTS);
  int flags = O_WRONLY | O_CREAT | O_EXCL;
  PP_FileInfo file_info = {};
  SetUpOpenExpectations(kPepperPath, flags,
                        &executor, &default_executor_, &default_executor_,
                        file_info);
  scoped_refptr<FileStream> file(OpenFile(flags));
  EXPECT_FALSE(file.get());
  EXPECT_EQ(EEXIST, errno);
}

TEST_BACKGROUND_F(PepperFileTest, TestRequestHandleFail) {
  base::AutoLock lock(file_system_->mutex());
  CompletionCallbackExecutor request_handle_executor(&bg_, PP_ERROR_NOACCESS);
  int flags = O_WRONLY | O_CREAT;
  PP_FileInfo file_info = {};
  SetUpOpenExpectations(kPepperPath, flags,
                        &default_executor_, &request_handle_executor,
                        &default_executor_, file_info);
  scoped_refptr<FileStream> file(OpenFile(flags));
  EXPECT_FALSE(file.get());
  EXPECT_EQ(EPERM, errno);
}

TEST_BACKGROUND_F(PepperFileTest, TestOpenClose) {
  base::AutoLock lock(file_system_->mutex());
  scoped_refptr<FileStream> file(OpenFileWithExpectations(O_RDWR | O_CREAT));
  EXPECT_TRUE(file.get());
}

TEST_BACKGROUND_F(PepperFileTest, TestFstat) {
  base::AutoLock lock(file_system_->mutex());
  scoped_refptr<FileStream> file(OpenFileWithExpectations(O_RDONLY));
  EXPECT_TRUE(file.get());
  struct stat st;
  memset(&st, 1, sizeof(st));
  // Call fstat just to make sure it does not crash.
  EXPECT_EQ(0, file->fstat(&st));
}

TEST_BACKGROUND_F(PepperFileTest, TestRenameDirectoryCached) {
  base::AutoLock lock(file_system_->mutex());
  PP_FileInfo file_info = {};
  SetUpNoFileRefExpectations(&default_executor_, file_info);
  struct stat st;
  EXPECT_EQ(-1, handler_->stat(kPepperPathNewDir, &st));
  EXPECT_EQ(-1, handler_->stat(kPepperPathNewDir, &st));  // cache hit
  SetUpRenameExpectations(kPepperOldDir, kPepperNewDir, &default_executor_);
  EXPECT_EQ(0, handler_->rename(kPepperOldDir, kPepperNewDir));
  // Expect to be called since cache should be invalidated by previous rename.
  SetUpStatExpectations(&default_executor_, file_info);
  EXPECT_EQ(0, handler_->stat(kPepperPathNewDir, &st));
}

TEST_BACKGROUND_F(PepperFileTest, TestStat) {
  base::AutoLock lock(file_system_->mutex());
  PP_FileInfo file_info = {};
  const off64_t kSize = 0xdeadbeef;
  file_info.size = kSize;
  file_info.type = PP_FILETYPE_REGULAR;
  SetUpStatExpectations(&default_executor_, file_info);
  struct stat st;
  memset(&st, 1, sizeof(st));
  EXPECT_EQ(0, handler_->stat(kPepperPath, &st));
  SCOPED_TRACE("TestStat");
  CheckStatStructure(st, S_IFREG, 1, kSize, GetInode(kPepperPath), 0, 0, 0);
}

TEST_BACKGROUND_F(PepperFileTest, TestStatDirectory) {
  base::AutoLock lock(file_system_->mutex());
  PP_FileInfo file_info = {};
  file_info.type = PP_FILETYPE_DIRECTORY;
  SetUpStatExpectations(&default_executor_, file_info);
  struct stat st;
  memset(&st, 1, sizeof(st));
  EXPECT_EQ(0, handler_->stat(kPepperPath, &st));
  SCOPED_TRACE("TestStatDirectory");
  CheckStatStructure(st, S_IFDIR, 32, 4096, GetInode(kPepperPath), 0, 0, 0);
}

TEST_BACKGROUND_F(PepperFileTest, TestMkdir) {
  base::AutoLock lock(file_system_->mutex());
  const mode_t mode = 0777;
  SetUpMkdirExpectations(kPepperPath, &default_executor_);
  EXPECT_EQ(0, handler_->mkdir(kPepperPath, mode));
}

TEST_BACKGROUND_F(PepperFileTest, TestMkdirFail) {
  base::AutoLock lock(file_system_->mutex());
  CompletionCallbackExecutor executor(&bg_, PP_ERROR_FAILED);
  const mode_t mode = 0777;
  SetUpMkdirExpectations(kPepperPath, &executor);
  EXPECT_EQ(-1, handler_->mkdir(kPepperPath, mode));
  EXPECT_EQ(ENOENT, errno);
}

TEST_BACKGROUND_F(PepperFileTest, TestMkdirNoPermission) {
  base::AutoLock lock(file_system_->mutex());
  CompletionCallbackExecutor executor(&bg_, PP_ERROR_NOACCESS);
  const mode_t mode = 0777;
  SetUpMkdirExpectations(kPepperPath, &executor);
  EXPECT_EQ(-1, handler_->mkdir(kPepperPath, mode));
  EXPECT_EQ(EPERM, errno);
}

TEST_BACKGROUND_F(PepperFileTest, TestRename) {
  base::AutoLock lock(file_system_->mutex());
  SetUpRenameExpectations(kPepperPath, kAnotherPepperPath, &default_executor_);
  EXPECT_EQ(0, handler_->rename(kPepperPath, kAnotherPepperPath));
}

TEST_BACKGROUND_F(PepperFileTest, TestRenameInode) {
  base::AutoLock lock(file_system_->mutex());
  static const ino_t zero_ino = 0;
  PP_FileInfo file_info = {};
  file_info.size = 0xdeadbeef;
  file_info.type = PP_FILETYPE_REGULAR;

  // Assign inode for |kPepperPath| by calling stat().
  SetUpStatExpectations(&default_executor_, file_info);
  struct stat st;
  memset(&st, 1, sizeof(st));
  EXPECT_EQ(0, handler_->stat(kPepperPath, &st));
  const ino_t orig_ino = st.st_ino;
  EXPECT_NE(zero_ino, orig_ino);
  // Call rename().
  SetUpRenameExpectations(kPepperPath, kAnotherPepperPath, &default_executor_);
  EXPECT_EQ(0, handler_->rename(kPepperPath, kAnotherPepperPath));
  // Call stat() against |kAnotherPepperPath| to confirm st_ino is the same.
  memset(&st, 1, sizeof(st));
  EXPECT_EQ(0, handler_->stat(kAnotherPepperPath, &st));
  EXPECT_EQ(orig_ino, st.st_ino);
}

TEST_BACKGROUND_F(PepperFileTest, TestRenameInode2) {
  base::AutoLock lock(file_system_->mutex());
  static const ino_t zero_ino = 0;
  PP_FileInfo file_info = {};
  file_info.size = 0xdeadbeef;
  file_info.type = PP_FILETYPE_REGULAR;

  // Assign inode for |kAnotherPepperPath| by calling stat().
  SetUpStatExpectations(&default_executor_, file_info);
  struct stat st;
  memset(&st, 1, sizeof(st));
  EXPECT_EQ(0, handler_->stat(kAnotherPepperPath, &st));
  const ino_t orig_ino = st.st_ino;
  EXPECT_NE(zero_ino, orig_ino);
  // Call rename().
  SetUpRenameExpectations(kPepperPath, kAnotherPepperPath, &default_executor_);
  EXPECT_EQ(0, handler_->rename(kPepperPath, kAnotherPepperPath));
  // Call stat() against |kAnotherPepperPath| again to confirm st_ino is NOT
  // the same.
  SetUpStatExpectations(&default_executor_, file_info);
  memset(&st, 1, sizeof(st));
  EXPECT_EQ(0, handler_->stat(kAnotherPepperPath, &st));
  EXPECT_NE(orig_ino, st.st_ino);
}

TEST_BACKGROUND_F(PepperFileTest, TestRenameEnoentFail) {
  base::AutoLock lock(file_system_->mutex());
  CompletionCallbackExecutor executor(&bg_, PP_ERROR_FILENOTFOUND);
  SetUpRenameExpectations(kPepperPath, kAnotherPepperPath, &executor);
  EXPECT_EQ(-1, handler_->rename(kPepperPath, kAnotherPepperPath));
  EXPECT_EQ(ENOENT, errno);
}

TEST_BACKGROUND_F(PepperFileTest, TestRenameEisdirFail) {
  base::AutoLock lock(file_system_->mutex());
  CompletionCallbackExecutor executor(&bg_, PP_ERROR_NOTAFILE);
  SetUpRenameExpectations(kPepperPath, kAnotherPepperPath, &executor);
  EXPECT_EQ(-1, handler_->rename(kPepperPath, kAnotherPepperPath));
  EXPECT_EQ(EISDIR, errno);
}

TEST_BACKGROUND_F(PepperFileTest, TestUnlink) {
  base::AutoLock lock(file_system_->mutex());
  ino_t inode = GetInode(kPepperPath);
  SetUpUnlinkExpectations(kPepperPath, &default_executor_);
  EXPECT_EQ(0, handler_->unlink(kPepperPath));
  EXPECT_NE(inode, GetInode(kPepperPath));
}

TEST_BACKGROUND_F(PepperFileTest, TestUnlinkFail) {
  base::AutoLock lock(file_system_->mutex());
  {
    CompletionCallbackExecutor executor(&bg_, PP_ERROR_FILENOTFOUND);
    SetUpUnlinkExpectations(kPepperPath, &executor);
    EXPECT_ERROR(handler_->unlink(kPepperPath), ENOENT);
  }
  {
    // If you try to delete a non-empty directory, the API returns with
    // PP_ERROR_FAILED.
    CompletionCallbackExecutor executor(&bg_, PP_ERROR_FAILED);
    SetUpUnlinkExpectations(kPepperPath, &executor);
    EXPECT_ERROR(handler_->unlink(kPepperPath), EISDIR);
  }
}

TEST_BACKGROUND_F(PepperFileTest, TestUTime) {
  base::AutoLock lock(file_system_->mutex());
  SetUpUTimeExpectations(kPepperPath, kTime, &default_executor_);
  {
    struct timeval times[2];
    times[0].tv_sec = kTime;
    times[0].tv_usec = 0;
    times[1].tv_sec = kTime;
    times[1].tv_usec = 0;
    EXPECT_EQ(0, handler_->utimes(kPepperPath, times));
  }
}

TEST_BACKGROUND_F(PepperFileTest, TestUTimeFail) {
  base::AutoLock lock(file_system_->mutex());
  CompletionCallbackExecutor executor(&bg_, PP_ERROR_FILENOTFOUND);
  SetUpUTimeExpectations(kPepperPath, kTime, &executor);
  {
    struct timeval times[2];
    times[0].tv_sec = kTime;
    times[0].tv_usec = 0;
    times[1].tv_sec = kTime;
    times[1].tv_usec = 0;
    EXPECT_ERROR(handler_->utimes(kPepperPath, times), ENOENT);
  }
}

TEST_BACKGROUND_F(PepperFileTest, TestStatCache) {
  base::AutoLock lock(file_system_->mutex());
  PP_FileInfo file_info = {};
  SetUpStatExpectations(&default_executor_, file_info);
  // StatExpectations expect the underlying calls that stat call to be
  // called exactly once.
  struct stat st;
  EXPECT_EQ(0, handler_->stat(kPepperPath, &st));
  EXPECT_EQ(0, handler_->stat(kPepperPath, &st));
}

TEST_BACKGROUND_F(PepperFileTest, TestStatCacheDisabled) {
  base::AutoLock lock(file_system_->mutex());
  DisableCache();
  PP_FileInfo file_info = {};
  struct stat st;
  // Confirm Pepper's stat() is called twice when the cache is disabled.
  SetUpStatExpectations(&default_executor_, file_info);
  EXPECT_EQ(0, handler_->stat(kPepperPath, &st));
  SetUpStatExpectations(&default_executor_, file_info);
  EXPECT_EQ(0, handler_->stat(kPepperPath, &st));
}

TEST_BACKGROUND_F(PepperFileTest, TestStatCacheWithTrailingSlash) {
  base::AutoLock lock(file_system_->mutex());
  PP_FileInfo file_info = {};
  SetUpStatExpectations(&default_executor_, file_info);
  struct stat st;
  EXPECT_EQ(0, handler_->stat("/dir", &st));
  // Check if pepper_file.cc automatically remove the trailing / when
  // accessing the cache.
  EXPECT_EQ(0, handler_->stat("/dir/", &st));
}

TEST_BACKGROUND_F(PepperFileTest, TestStatCacheInvalidation) {
  base::AutoLock lock(file_system_->mutex());
  PP_FileInfo file_info = {};
  SetUpStatExpectations(&default_executor_, file_info);
  struct stat st;
  EXPECT_EQ(0, handler_->stat(kPepperPath, &st));
  // Now call utimes to invalidate the cache.
  SetUpUTimeExpectations(kPepperPath, kTime, &default_executor_);
  {
    struct timeval times[2];
    times[0].tv_sec = kTime;
    times[0].tv_usec = 0;
    times[1].tv_sec = kTime;
    times[1].tv_usec = 0;
    EXPECT_EQ(0, handler_->utimes(kPepperPath, times));
  }
  SetUpStatExpectations(&default_executor_, file_info);
  // StatExpectations expect the underlying calls that stat call to be
  // called exactly once.
  EXPECT_EQ(0, handler_->stat(kPepperPath, &st));
  EXPECT_EQ(0, handler_->stat(kPepperPath, &st));
}

TEST_BACKGROUND_F(PepperFileTest, TestStatWithENOENT) {
  base::AutoLock lock(file_system_->mutex());
  CompletionCallbackExecutor executor(&bg_, PP_ERROR_FILENOTFOUND);  // ENOENT
  PP_FileInfo file_info = {};
  SetUpStatExpectations(&executor, file_info);
  struct stat st;
  EXPECT_EQ(-1, handler_->stat(kPepperPath, &st));
  EXPECT_EQ(ENOENT, errno);

  // The following stat, open, rename, truncate, unlink, and utimes
  // calls should not call into Pepper since the initial stat call above
  // returned ENOENT.
  EXPECT_EQ(ENOENT, errno);
  EXPECT_EQ(-1, handler_->stat(kPepperPath, &st));
  EXPECT_EQ(ENOENT, errno);
  EXPECT_EQ(NULL, handler_->open(-1, kPepperPath, O_RDONLY, 0).get());
  EXPECT_EQ(ENOENT, errno);
  EXPECT_EQ(NULL, handler_->open(-1, kPepperPath, O_WRONLY, 0).get());
  EXPECT_EQ(ENOENT, errno);
  EXPECT_EQ(NULL, handler_->open(-1, kPepperPath, O_RDWR, 0).get());
  EXPECT_EQ(ENOENT, errno);
  EXPECT_EQ(-1, handler_->rename(kPepperPath, "/abc"));
  EXPECT_EQ(ENOENT, errno);
  EXPECT_EQ(-1, handler_->truncate(kPepperPath, 0));
  EXPECT_EQ(ENOENT, errno);
  EXPECT_EQ(-1, handler_->unlink(kPepperPath));
  EXPECT_EQ(ENOENT, errno);
  {
    struct timeval times[2];
    times[0].tv_sec = kTime;
    times[0].tv_usec = 0;
    times[1].tv_sec = kTime;
    times[1].tv_usec = 0;
    EXPECT_EQ(-1, handler_->utimes(kPepperPath, times));
    EXPECT_EQ(ENOENT, errno);
  }

  // However, open() with O_CREAT should ignore the cache.
  scoped_refptr<FileStream> file(OpenFileWithExpectations(O_WRONLY | O_CREAT));
  EXPECT_TRUE(file.get());
}

TEST_BACKGROUND_F(PepperFileTest, TestTruncate) {
  base::AutoLock lock(file_system_->mutex());
  PP_FileInfo file_info = {};
  // Truncate  is just an open, ftruncate, and close.
  SetUpOpenExpectations(kPepperPath, O_WRONLY,
                        &default_executor_, &default_executor_,
                        &default_executor_, file_info);
  off64_t length = 0;
  SetUpFtruncateExpectations(&default_executor_, length);
  EXPECT_EQ(0, handler_->truncate(kPepperPath, length));

  // Do the same with non-zero |length|.
  SetUpOpenExpectations(kPepperPath, O_WRONLY,
                        &default_executor_, &default_executor_,
                        &default_executor_, file_info);
  length = 12345;
  SetUpFtruncateExpectations(&default_executor_, length);
  EXPECT_EQ(0, handler_->truncate(kPepperPath, length));
}

TEST_BACKGROUND_F(PepperFileTest, TestTruncateFail) {
  base::AutoLock lock(file_system_->mutex());
  CompletionCallbackExecutor executor(&bg_, PP_ERROR_FILENOTFOUND);
  PP_FileInfo file_info = {};
  SetUpOpenExpectations(kPepperPath, O_WRONLY,
                        &executor, &default_executor_, &default_executor_,
                        file_info);
  EXPECT_ERROR(handler_->truncate(kPepperPath, 0), ENOENT);

  CompletionCallbackExecutor executor2(&bg_, PP_ERROR_NOTAFILE);
  SetUpOpenExpectations(kPepperPath, O_WRONLY,
                        &executor2, &default_executor_, &default_executor_,
                        file_info);
  EXPECT_ERROR(handler_->truncate(kPepperPath, 0), EISDIR);
}

TEST_BACKGROUND_F(PepperFileTest, TestFtruncateReadonly) {
  base::AutoLock lock(file_system_->mutex());
  // Can not call truncate() against a read-only fd.
  scoped_refptr<FileStream> file(OpenFileWithExpectations(O_RDONLY));
  EXPECT_TRUE(file.get());
  EXPECT_ERROR(file->ftruncate(0), EBADF);
}

TEST_BACKGROUND_F(PepperFileTest, TestPacketCalls) {
  base::AutoLock lock(file_system_->mutex());
  scoped_refptr<FileStream> file(OpenFileWithExpectations(O_RDWR | O_CREAT));

  char buf[1];
  EXPECT_ERROR(file->recv(buf, 1, 0), ENOTSOCK);
  EXPECT_ERROR(file->recvfrom(buf, 1, 0, NULL, NULL), ENOTSOCK);
  EXPECT_ERROR(file->send(buf, 1, 0), ENOTSOCK);
  EXPECT_ERROR(file->sendto(buf, 1, 0, NULL, 0), ENOTSOCK);
}

}  // namespace posix_translation
