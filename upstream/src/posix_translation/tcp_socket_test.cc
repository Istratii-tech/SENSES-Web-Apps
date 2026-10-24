// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Unit tests for TCP sockets.

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include <algorithm>
#include <vector>

#include "base/compiler_specific.h"
#include "base/memory/scoped_ptr.h"
#include "base/synchronization/waitable_event.h"
#include "gtest/gtest.h"
#include "posix_translation/file_system_handler.h"
#include "posix_translation/socket_util.h"
#include "posix_translation/tcp_socket.h"
#include "posix_translation/test_util/file_system_background_test_common.h"
#include "posix_translation/virtual_file_system.h"
#include "ppapi/utility/completion_callback_factory.h"
#include "ppapi_mocks/background_test.h"
#include "ppapi_mocks/background_thread.h"
#include "ppapi_mocks/ppb_net_address.h"
#include "ppapi_mocks/ppb_tcp_socket.h"

using ::testing::NiceMock;
using ::testing::DoAll;

namespace posix_translation {

namespace {

const char kStreamContents[] = "test";

}  // namespace

// Thin wrapper of base::WaitableEvent::Wait() to block the main thread.
void WaitEvent(void* user_data, int32_t result) {
  base::WaitableEvent* event =
      reinterpret_cast<base::WaitableEvent*>(user_data);
  event->Wait();
}

// Thin Wrapper of base::WaitableEvent::Signal() to run it on the main thread.
void SignalEvent(void* user_data, int32_t result) {
  base::WaitableEvent* event =
      reinterpret_cast<base::WaitableEvent*>(user_data);
  event->Signal();
}

#define EXPECT_ERROR(expected_error, result) do { \
    EXPECT_EQ(-1, result); \
    EXPECT_EQ(expected_error, errno); \
    errno = 0; \
  } while (0)

class PepperTCPSocketTest
    : public FileSystemBackgroundTestCommon<PepperTCPSocketTest> {
 public:
  DECLARE_BACKGROUND_TEST(ConnectSuccess);
  DECLARE_BACKGROUND_TEST(ConnectFail);
  DECLARE_BACKGROUND_TEST(SetOptionSuccess);
  DECLARE_BACKGROUND_TEST(SetOptionFail);
  DECLARE_BACKGROUND_TEST(ConnectThenSetOption);
  DECLARE_BACKGROUND_TEST(SetOptionThenConnect);
  DECLARE_BACKGROUND_TEST(Recv);
  DECLARE_BACKGROUND_TEST(RecvMsgPeek);
  // TODO(crbug.com/362175): qemu-arm cannot reliably emulate threading
  // functions so run them in a real ARM device.
  DECLARE_BACKGROUND_TEST(QEMU_DISABLED_NonBlockingConnectSuccess);
  DECLARE_BACKGROUND_TEST(QEMU_DISABLED_NonBlockingConnectFail);

 protected:
  static const PP_Resource kTCPSocketResource = 74;

  PepperTCPSocketTest()
      : stream_pos_(0),
        default_executor_(&bg_, PP_OK),
        fail_executor_(&bg_, PP_ERROR_FAILED),
        ppb_tcpsocket_(NULL) {
  }

  virtual void SetUp() OVERRIDE {
    FileSystemBackgroundTestCommon<PepperTCPSocketTest>::SetUp();
    factory_.GetMock(&ppb_tcpsocket_);
    factory_.GetMock(&ppb_netaddress_);

    // We ignore DescribeAsString here, as it is used only for logging.
    EXPECT_CALL(*ppb_netaddress_, DescribeAsString(_, _))
        .WillRepeatedly(Return(ppapi_mocks::VarFromString("")));
    pending_callbacks_.clear();
  }

  virtual void TearDown() OVERRIDE {
    // Run all callbacks with abort error code.
    for (size_t i = 0; i < pending_callbacks_.size(); ++i) {
      PP_RunCompletionCallback(&pending_callbacks_[i], PP_ERROR_ABORTED);
    }
    FileSystemBackgroundTestCommon<PepperTCPSocketTest>::TearDown();
  }

  // Add callback which will be aborted later.
  void AddPendingCallback(const PP_CompletionCallback& callback) {
    pending_callbacks_.push_back(callback);
  }

  int32_t OnRead(PP_Resource tcp_socket, char* buffer, int32_t len,
                 PP_CompletionCallback callback) {
    int32_t bytes_remaining = sizeof(kStreamContents) - stream_pos_;
    int32_t bytes_read = std::min(len, bytes_remaining);
    if (bytes_read > 0) {
      memcpy(buffer, kStreamContents + stream_pos_, bytes_read);
      stream_pos_ += bytes_read;
      return bytes_read;
    }
    return 0;
  }

  void ExpectTCPSocketInstance() {
    // Create and release.
    EXPECT_CALL(*ppb_tcpsocket_, Create(kInstanceNumber)).
        WillOnce(Return(kTCPSocketResource));
  }

  void ExpectConnectSuccess() {
    EXPECT_CALL(*ppb_tcpsocket_, Connect(kTCPSocketResource, _, _)).
        WillOnce(WithArgs<2>(
            Invoke(&default_executor_,
                   &CompletionCallbackExecutor::ExecuteOnMainThread)));

    // On success of TCPSocket::Connect(), pp::TCPSocket::Read() is called
    // on the main thread. Also, keep the callback, which will be aborted in
    // TearDown(), otherwise the resource will be leaked.
    EXPECT_CALL(*ppb_tcpsocket_, Read(kTCPSocketResource, _, _, _)).
        WillOnce(DoAll(
            WithArgs<3>(
                Invoke(this, &PepperTCPSocketTest::AddPendingCallback)),
            Return(static_cast<int32_t>(PP_OK_COMPLETIONPENDING))));
  }

  void ExpectConnectSuccessWithData() {
    EXPECT_CALL(*ppb_tcpsocket_, Connect(kTCPSocketResource, _, _)).
        WillOnce(WithArgs<2>(
            Invoke(&default_executor_,
                   &CompletionCallbackExecutor::ExecuteOnMainThread)));

    ON_CALL(*ppb_tcpsocket_, Read(kTCPSocketResource, _, _, _)).
        WillByDefault(Invoke(this, &PepperTCPSocketTest::OnRead));
  }

  void ExpectConnectFail() {
    EXPECT_CALL(*ppb_tcpsocket_, Connect(kTCPSocketResource, _, _)).
        WillOnce(WithArgs<2>(
            Invoke(&fail_executor_,
                   &CompletionCallbackExecutor::ExecuteOnMainThread)));
  }

  void ExpectSetOptionNoDelaySuccess() {
    EXPECT_CALL(*ppb_tcpsocket_,
                SetOption(kTCPSocketResource,
                          PP_TCPSOCKET_OPTION_NO_DELAY, _, _)).
        WillOnce(WithArgs<3>(
            Invoke(&default_executor_,
                   &CompletionCallbackExecutor::ExecuteOnMainThread)));
  }

  void ExpectSetOptionNoDelayFail() {
    EXPECT_CALL(*ppb_tcpsocket_,
                SetOption(kTCPSocketResource,
                          PP_TCPSOCKET_OPTION_NO_DELAY, _, _)).
        WillOnce(WithArgs<3>(
            Invoke(&fail_executor_,
                   &CompletionCallbackExecutor::ExecuteOnMainThread)));
  }

  int fcntl(int sockfd, int cmd, ...) {
    int result;
    va_list ap;
    va_start(ap, cmd);
    result = file_system_->fcntl(sockfd, cmd, ap);
    va_end(ap);
    return result;
  }

  int connect(int sockfd) {
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_port = htons(2048);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return file_system_->connect(sockfd, reinterpret_cast<sockaddr*>(&addr),
                                 sizeof(addr));
  }

  int set_nodelay_option(int sockfd) {
    int one = 1;
    return file_system_->setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &one,
                                    static_cast<socklen_t>(sizeof(one)));
  }

  int set_non_block(int sockfd) {
    int opts = fcntl(sockfd, F_GETFL);
    if (opts < 0) return opts;
    return fcntl(sockfd, F_SETFL, opts | O_NONBLOCK);
  }

  ssize_t recv(int sockfd, void* buf, size_t len, int flags) {
    return file_system_->recv(sockfd, buf, len, flags);
  }

  void ExpectSoError(int sockfd, int expected) {
    int optval;
    socklen_t optlen = SIZEOF_AS_SOCKLEN(optval);
    EXPECT_EQ(0, file_system_->getsockopt(sockfd, SOL_SOCKET, SO_ERROR,
                                          &optval, &optlen));
    EXPECT_EQ(expected, optval);
  }

  void ExpectPollEvent(int sockfd, int expected_events, int timeout) {
    struct pollfd poller;
    poller.fd = sockfd;
    poller.events = POLLIN | POLLOUT;
    poller.revents = 0;
    EXPECT_EQ(expected_events != 0 ? 1 : 0,
              file_system_->poll(&poller, 1, timeout));
    EXPECT_EQ(expected_events, poller.revents);
  }

  int32_t stream_pos_;
  CompletionCallbackExecutor default_executor_;
  CompletionCallbackExecutor fail_executor_;
  std::vector<PP_CompletionCallback> pending_callbacks_;
  NiceMock<PPB_TCPSocket_Mock>* ppb_tcpsocket_;
  NiceMock<PPB_NetAddress_Mock>* ppb_netaddress_;
};

TEST_BACKGROUND_F(PepperTCPSocketTest, ConnectSuccess) {
  ExpectTCPSocketInstance();
  ExpectConnectSuccess();
  int sockfd = file_system_->socket(AF_INET, SOCK_STREAM, 0);
  EXPECT_NE(0, sockfd);
  EXPECT_EQ(0, connect(sockfd));
  EXPECT_EQ(0, file_system_->close(sockfd));
}

TEST_BACKGROUND_F(PepperTCPSocketTest, ConnectFail) {
  ExpectTCPSocketInstance();
  ExpectConnectFail();
  int sockfd = file_system_->socket(AF_INET, SOCK_STREAM, 0);
  EXPECT_NE(0, sockfd);
  EXPECT_ERROR(ECONNREFUSED, connect(sockfd));
  EXPECT_EQ(0, file_system_->close(sockfd));
}

TEST_BACKGROUND_F(PepperTCPSocketTest, SetOptionSuccess) {
  ExpectTCPSocketInstance();
  ExpectSetOptionNoDelaySuccess();
  int sockfd = file_system_->socket(AF_INET, SOCK_STREAM, 0);
  EXPECT_NE(0, sockfd);
  EXPECT_EQ(0, set_nodelay_option(sockfd));
  EXPECT_EQ(0, file_system_->close(sockfd));
}

TEST_BACKGROUND_F(PepperTCPSocketTest, SetOptionFail) {
  ExpectTCPSocketInstance();
  ExpectSetOptionNoDelayFail();
  int sockfd = file_system_->socket(AF_INET, SOCK_STREAM, 0);
  EXPECT_NE(0, sockfd);
  EXPECT_ERROR(ENOPROTOOPT, set_nodelay_option(sockfd));
  EXPECT_EQ(0, file_system_->close(sockfd));
}

TEST_BACKGROUND_F(PepperTCPSocketTest, ConnectThenSetOption) {
  ExpectTCPSocketInstance();
  ExpectConnectSuccess();
  ExpectSetOptionNoDelaySuccess();

  int sockfd = file_system_->socket(AF_INET, SOCK_STREAM, 0);
  EXPECT_NE(0, sockfd);
  EXPECT_EQ(0, connect(sockfd));
  EXPECT_EQ(0, set_nodelay_option(sockfd));
  EXPECT_EQ(0, file_system_->close(sockfd));
}

TEST_BACKGROUND_F(PepperTCPSocketTest, SetOptionThenConnect) {
  ExpectTCPSocketInstance();
  ExpectConnectSuccess();
  ExpectSetOptionNoDelaySuccess();

  int sockfd = file_system_->socket(AF_INET, SOCK_STREAM, 0);
  EXPECT_NE(0, sockfd);
  EXPECT_EQ(0, set_nodelay_option(sockfd));
  EXPECT_EQ(0, connect(sockfd));
  EXPECT_EQ(0, file_system_->close(sockfd));
}

TEST_BACKGROUND_F(PepperTCPSocketTest, Recv) {
  ExpectTCPSocketInstance();
  ExpectConnectSuccessWithData();

  int sockfd = file_system_->socket(AF_INET, SOCK_STREAM, 0);
  char buffer[256];
  EXPECT_NE(0, sockfd);
  EXPECT_EQ(0, connect(sockfd));

  memset(buffer, 0, sizeof(buffer));
  EXPECT_EQ(sizeof(kStreamContents), recv(sockfd, buffer, sizeof(buffer), 0));
  EXPECT_STREQ(kStreamContents, buffer);

  // Verify that the stream is closed.
  EXPECT_EQ(0, recv(sockfd, buffer, sizeof(buffer), 0));
  EXPECT_EQ(0, file_system_->close(sockfd));
}

TEST_BACKGROUND_F(PepperTCPSocketTest, RecvMsgPeek) {
  ExpectTCPSocketInstance();
  ExpectConnectSuccessWithData();

  int sockfd = file_system_->socket(AF_INET, SOCK_STREAM, 0);
  char buffer[256];
  EXPECT_NE(0, sockfd);
  EXPECT_EQ(0, connect(sockfd));

  memset(buffer, 0, sizeof(buffer));
  EXPECT_EQ(sizeof(kStreamContents), recv(sockfd, buffer, sizeof(buffer),
                                          MSG_PEEK));
  EXPECT_STREQ(kStreamContents, buffer);

  memset(buffer, 0, sizeof(buffer));
  EXPECT_EQ(sizeof(kStreamContents), recv(sockfd, buffer, sizeof(buffer), 0));
  EXPECT_STREQ(kStreamContents, buffer);

  // Verify that the stream is closed.
  EXPECT_EQ(0, recv(sockfd, buffer, sizeof(buffer), 0));
  EXPECT_EQ(0, file_system_->close(sockfd));
}

// TODO(crbug.com/362175): qemu-arm cannot reliably emulate threading
// functions so run them in a real ARM device.
TEST_BACKGROUND_F(PepperTCPSocketTest,
                  QEMU_DISABLED_NonBlockingConnectSuccess) {
  ExpectTCPSocketInstance();
  ExpectConnectSuccess();

  int sockfd = file_system_->socket(AF_INET, SOCK_STREAM, 0);
  EXPECT_NE(0, sockfd);
  EXPECT_EQ(0, set_non_block(sockfd));

  // Block the main thread so this code can run tests on the state of |sockfd|
  // before the Pepper main thread callbacks have executed.
  base::WaitableEvent event1(true, false);
  bg_.CallOnMainThread(
      0, PP_MakeCompletionCallback(&WaitEvent, &event1), 0);

  // First time, no background connect() task runs so EINPROGRESS should be
  // raised.
  EXPECT_ERROR(EINPROGRESS, connect(sockfd));

  // Second time, there is a background connect() (initiated by above connect),
  // so EALREADY should be raised.
  EXPECT_ERROR(EALREADY, connect(sockfd));

  // Make sure no poll flag is on.
  ExpectPollEvent(sockfd, 0, 0);

  // Here a task to run TCPSocket::Connect is enqueued to the main thread.
  // So, we unblock the thread and wait the pending task's completion.
  base::WaitableEvent event2(true, false);
  bg_.CallOnMainThread(
      0, PP_MakeCompletionCallback(&SignalEvent, &event2), 0);
  event1.Signal();
  event2.Wait();

  // Here, the connection is established. So, now it should be writable.
  ExpectPollEvent(sockfd, POLLOUT, 0);

  ExpectSoError(sockfd, 0);

  EXPECT_EQ(0, file_system_->close(sockfd));
}

// TODO(crbug.com/362175): qemu-arm cannot reliably emulate threading
// functions so run them in a real ARM device.
TEST_BACKGROUND_F(PepperTCPSocketTest,
                  QEMU_DISABLED_NonBlockingConnectFail) {
  ExpectTCPSocketInstance();
  ExpectConnectFail();

  int sockfd = file_system_->socket(AF_INET, SOCK_STREAM, 0);
  EXPECT_NE(0, sockfd);
  EXPECT_EQ(0, set_non_block(sockfd));

  // Block the main thread.
  base::WaitableEvent event1(true, false);
  bg_.CallOnMainThread(
      0, PP_MakeCompletionCallback(&WaitEvent, &event1), 0);

  // First time, no background connect() task runs so EINPROGRESS should be
  // raised.
  EXPECT_ERROR(EINPROGRESS, connect(sockfd));

  // Second time, there is a background connect() (initiated by above connect),
  // so EALREADY should be raised.
  EXPECT_ERROR(EALREADY, connect(sockfd));

  // Make sure no poll flag is on.
  ExpectPollEvent(sockfd, 0, 0);

  // Here a task to run TCPSocket::Connect is enqueued to the main thread.
  // So, we unblock the thread and wait the pending task's completion.
  base::WaitableEvent event2(true, false);
  bg_.CallOnMainThread(
      0, PP_MakeCompletionCallback(&SignalEvent, &event2), 0);
  event1.Signal();
  event2.Wait();

  // On error, all POLLIN, POLLOUT and POLLERR are raised.
  ExpectPollEvent(sockfd, POLLIN | POLLOUT | POLLERR, 0);

  ExpectSoError(sockfd, ECONNREFUSED);

  EXPECT_EQ(0, file_system_->close(sockfd));
}

}  // namespace posix_translation
