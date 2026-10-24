// libbinder 8.1, servicemanager 8.1 (on the in-process /dev/binder of
// shim/binder/dev_binder.cc), libutils 8.1 (Looper on a pipe, threads) and
// the property store, in WebAssembly. Part of make fw8-native-test.
//
// "fw8.remote" is a service in another binder process (pid 4343, uid
// 1000) on its own thread, written with servicemanager's binder.c helpers
// as a native daemon would be, so that calls from libbinder go through the
// driver and a real handle, as between two Android processes.

#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <atomic>
#include <vector>

#include <binder/Binder.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/Parcel.h>
#include <binder/ProcessState.h>
#include <cutils/properties.h>
#include <utils/Looper.h>
#include <utils/String16.h>
#include <utils/String8.h>
#include <utils/SystemClock.h>
#include <utils/Thread.h>
#include <utils/Timers.h>

#include "arc_binder.h"
#include "gtest/gtest.h"
extern "C" {
#include "binder.h"  // frameworks/native/cmds/servicemanager
int binder_write(struct binder_state* bs, void* data, size_t len);
}

using namespace android;

namespace {

TEST(Utils8, Strings) {
  String8 empty;
  ASSERT_TRUE(empty.string() != nullptr);
  EXPECT_EQ(0u, empty.length());
  String8 s("fw8");
  s.appendFormat("-%d", 27);
  EXPECT_STREQ("fw8-27", s.string());
  String16 u(s);
  EXPECT_EQ(6u, u.size());
  EXPECT_EQ(String8("fw8-27"), String8(u));
}

// --- servicemanager -----------------------------------------------------------

sp<IServiceManager> ServiceManager() {
  static bool started = arc_binder_start_servicemanager() == 0;
  EXPECT_TRUE(started);
  return defaultServiceManager();
}

TEST(ServiceManager8, StartsAndAnswers) {
  sp<IServiceManager> sm = ServiceManager();
  ASSERT_TRUE(sm != nullptr);
  // The context manager (handle 0) is in another binder process.
  sp<IBinder> b = ProcessState::self()->getContextObject(nullptr);
  ASSERT_TRUE(b != nullptr);
  EXPECT_EQ(nullptr, b->localBinder());
  // servicemanager answers a ping with an empty reply (svcmgr_handler), so
  // pingBinder() says NOT_ENOUGH_DATA, on a device too; the call itself works.
  Parcel data, reply;
  EXPECT_EQ(NO_ERROR, b->transact(IBinder::PING_TRANSACTION, data, &reply));
  EXPECT_EQ(nullptr, sm->checkService(String16("fw8.no.such.service")).get());
}

class EchoService : public BBinder {
 public:
  static const String16& descriptor() {
    static const String16 d("fw8.IEcho");
    return d;
  }
  const String16& getInterfaceDescriptor() const override { return descriptor(); }

 protected:
  // (int a, String s) -> (int a + 1, String s + "!", int calling uid)
  status_t onTransact(uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags) override {
    if (code != IBinder::FIRST_CALL_TRANSACTION) return BBinder::onTransact(code, data, reply, flags);
    if (!data.enforceInterface(descriptor())) return PERMISSION_DENIED;
    int32_t a = data.readInt32();
    String8 s(data.readString16());
    s.append("!");
    reply->writeInt32(a + 1);
    reply->writeString16(String16(s));
    reply->writeInt32(IPCThreadState::self()->getCallingUid());
    return NO_ERROR;
  }
};

TEST(ServiceManager8, AddCheckListLocalService) {
  sp<IServiceManager> sm = ServiceManager();
  sp<IBinder> echo = new EchoService;
  ASSERT_EQ(NO_ERROR, sm->addService(String16("fw8.echo"), echo));
  sp<IBinder> found = sm->checkService(String16("fw8.echo"));
  ASSERT_TRUE(found != nullptr);
  // Our own process's node comes back as the local object.
  EXPECT_EQ(echo.get(), found.get());
  EXPECT_EQ(String16("fw8.IEcho"), found->getInterfaceDescriptor());

  Parcel data, reply;
  data.writeInterfaceToken(EchoService::descriptor());
  data.writeInt32(41);
  data.writeString16(String16("hi"));
  ASSERT_EQ(NO_ERROR, found->transact(IBinder::FIRST_CALL_TRANSACTION, data, &reply));
  EXPECT_EQ(42, reply.readInt32());
  EXPECT_EQ(String16("hi!"), reply.readString16());

  Vector<String16> names = sm->listServices();
  bool listed = false;
  for (size_t i = 0; i < names.size(); i++) listed |= names[i] == String16("fw8.echo");
  EXPECT_TRUE(listed);
}

// --- a service in another binder process ------------------------------------------

const int kRemotePid = 4343;
const int kRemoteUid = 1000;  // AID_SYSTEM: servicemanager lets it register
enum {
  kRemoteSum = 1,       // (int n, int[n]) -> (int sum, int calling pid, int calling uid)
  kRemoteRecord = 2,    // oneway (int v)
  kRemoteRecorded = 3,  // () -> (int n, int[n] recorded values)
  kRemoteCallBack = 4,  // (IBinder cb, int v) -> (int status, int reply of cb's FIRST_CALL(v))
};

pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
int g_state;  // 0 not started, 1 starting, 2 serving, -1 failed
std::vector<uint32_t> g_recorded;  // the remote thread's

void SetState(int s) {
  pthread_mutex_lock(&g_lock);
  g_state = s;
  pthread_cond_broadcast(&g_cond);
  pthread_mutex_unlock(&g_lock);
}

int RemoteHandler(struct binder_state* bs, struct binder_transaction_data* txn,
                  struct binder_io* msg, struct binder_io* reply) {
  switch (txn->code) {
    case kRemoteSum: {
      uint32_t n = bio_get_uint32(msg), sum = 0;
      for (uint32_t i = 0; i < n; i++) sum += bio_get_uint32(msg);
      bio_put_uint32(reply, sum);
      bio_put_uint32(reply, txn->sender_pid);
      bio_put_uint32(reply, txn->sender_euid);
      return 0;
    }
    case kRemoteRecord:
      g_recorded.push_back(bio_get_uint32(msg));
      return 0;
    case kRemoteRecorded:
      bio_put_uint32(reply, g_recorded.size());
      for (uint32_t v : g_recorded) bio_put_uint32(reply, v);
      return 0;
    case kRemoteCallBack: {
      uint32_t cb = bio_get_ref(msg);
      uint32_t v = bio_get_uint32(msg);
      unsigned iodata[512 / 4];
      struct binder_io m, r;
      bio_init(&m, iodata, sizeof(iodata), 4);
      bio_put_uint32(&m, v);
      int status = binder_call(bs, &m, &r, cb, IBinder::FIRST_CALL_TRANSACTION);
      bio_put_uint32(reply, status);
      bio_put_uint32(reply, status == 0 ? bio_get_uint32(&r) : 0);
      if (status == 0) binder_done(bs, &m, &r);
      return 0;
    }
  }
  return -1;
}

// servicemanager's IServiceManager.addService(name, binder, allowIsolated).
int Publish(struct binder_state* bs, const char* name, void* ptr) {
  unsigned iodata[512 / 4];
  struct binder_io msg, reply;
  bio_init(&msg, iodata, sizeof(iodata), 4);
  bio_put_uint32(&msg, 0);  // strict mode policy
  bio_put_string16_x(&msg, SVC_MGR_NAME);
  bio_put_string16_x(&msg, name);
  bio_put_obj(&msg, ptr);
  bio_put_uint32(&msg, 0);  // allowIsolated
  if (binder_call(bs, &msg, &reply, BINDER_SERVICE_MANAGER, SVC_MGR_ADD_SERVICE)) return -1;
  int status = bio_get_uint32(&reply);
  binder_done(bs, &msg, &reply);
  return status;
}

void* RemoteThread(void*) {
  static int token;
  arc_binder_set_process_identity(kRemotePid, kRemoteUid);
  struct binder_state* bs = binder_open("/dev/binder", 128 * 1024);
  if (!bs || Publish(bs, "fw8.remote", &token) != 0) {
    SetState(-1);
    return nullptr;
  }
  SetState(2);
  binder_loop(bs, RemoteHandler);
  return nullptr;
}

sp<IBinder> Remote() {
  ServiceManager();
  pthread_mutex_lock(&g_lock);
  if (g_state == 0) {
    g_state = 1;
    pthread_t t;
    pthread_create(&t, nullptr, RemoteThread, nullptr);
    pthread_detach(t);
  }
  while (g_state == 1) pthread_cond_wait(&g_cond, &g_lock);
  pthread_mutex_unlock(&g_lock);
  return defaultServiceManager()->checkService(String16("fw8.remote"));
}

TEST(Binder8, CallAnotherProcess) {
  sp<IBinder> remote = Remote();
  ASSERT_TRUE(remote != nullptr);
  EXPECT_EQ(nullptr, remote->localBinder());  // a handle, not our object
  Parcel data, reply;
  data.writeInt32(4);
  for (int v : {1, 2, 3, 36}) data.writeInt32(v);
  ASSERT_EQ(NO_ERROR, remote->transact(kRemoteSum, data, &reply));
  EXPECT_EQ(42, reply.readInt32());
  EXPECT_EQ(getpid(), reply.readInt32());            // the caller as the driver saw it
  EXPECT_EQ(static_cast<int32_t>(geteuid()), reply.readInt32());
}

TEST(Binder8, OnewayCallsArriveInOrder) {
  sp<IBinder> remote = Remote();
  ASSERT_TRUE(remote != nullptr);
  for (int v = 1; v <= 5; v++) {
    Parcel data;
    data.writeInt32(v * 10);
    ASSERT_EQ(NO_ERROR, remote->transact(kRemoteRecord, data, nullptr, IBinder::FLAG_ONEWAY));
  }
  Parcel data, reply;
  ASSERT_EQ(NO_ERROR, remote->transact(kRemoteRecorded, data, &reply));
  ASSERT_EQ(5, reply.readInt32());
  for (int v = 1; v <= 5; v++) EXPECT_EQ(v * 10, reply.readInt32());
}

class Doubler : public BBinder {
 protected:
  status_t onTransact(uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags) override {
    if (code != IBinder::FIRST_CALL_TRANSACTION) return BBinder::onTransact(code, data, reply, flags);
    calls++;
    reply->writeInt32(data.readInt32() * 2);
    return NO_ERROR;
  }

 public:
  std::atomic<int> calls{0};
};

TEST(Binder8, CallbackFromAnotherProcess) {
  sp<IBinder> remote = Remote();
  ASSERT_TRUE(remote != nullptr);
  // The remote process calls back into an object of ours while our thread
  // waits for its reply: the driver hands that nested call to this thread.
  sp<Doubler> cb = new Doubler;
  Parcel data, reply;
  data.writeStrongBinder(cb);
  data.writeInt32(21);
  ASSERT_EQ(NO_ERROR, remote->transact(kRemoteCallBack, data, &reply));
  EXPECT_EQ(0, reply.readInt32());
  EXPECT_EQ(42, reply.readInt32());
  EXPECT_EQ(1, cb->calls.load());
}

// --- Parcel ------------------------------------------------------------------------------

TEST(Parcel8, Values) {
  Parcel p;
  p.writeInt32(-7);
  p.writeInt64(1LL << 40);
  p.writeFloat(1.5f);
  p.writeDouble(2.25);
  p.writeBool(true);
  p.writeString16(String16("Привет"));
  p.writeCString("c-string");
  std::vector<int32_t> v = {1, 2, 3};
  p.writeInt32Vector(v);
  p.writeUtf8AsUtf16(std::string("utf8"));
  p.setDataPosition(0);
  EXPECT_EQ(-7, p.readInt32());
  EXPECT_EQ(1LL << 40, p.readInt64());
  EXPECT_EQ(1.5f, p.readFloat());
  EXPECT_EQ(2.25, p.readDouble());
  EXPECT_TRUE(p.readBool());
  EXPECT_EQ(String16("Привет"), p.readString16());
  EXPECT_STREQ("c-string", p.readCString());
  std::vector<int32_t> w;
  EXPECT_EQ(NO_ERROR, p.readInt32Vector(&w));
  EXPECT_EQ(v, w);
  std::string s;
  EXPECT_EQ(NO_ERROR, p.readUtf8FromUtf16(&s));
  EXPECT_EQ("utf8", s);
}

TEST(Parcel8, FileDescriptor) {
  // A pipe's write end is duplicated into the parcel; what is written to
  // the copy read back arrives at the read end.
  int fds[2];
  ASSERT_EQ(0, pipe(fds));
  Parcel p;
  ASSERT_EQ(NO_ERROR, p.writeDupFileDescriptor(fds[1]));
  p.setDataPosition(0);
  int fd = p.readFileDescriptor();
  ASSERT_GE(fd, 0);
  EXPECT_NE(fds[1], fd);
  ASSERT_EQ(3, write(fd, "abc", 3));
  char buf[4] = {};
  ASSERT_EQ(3, read(fds[0], buf, 3));
  EXPECT_STREQ("abc", buf);
  close(fds[0]);
  close(fds[1]);
}

// --- libutils: Looper, threads, clocks -----------------------------------------------------

class CountingHandler : public MessageHandler {
 public:
  void handleMessage(const Message& message) override { seen.push_back(message.what); }
  std::vector<int> seen;
};

TEST(Looper8, MessagesInTimeOrder) {
  sp<Looper> looper = new Looper(true);
  sp<CountingHandler> h = new CountingHandler;
  nsecs_t now = systemTime(SYSTEM_TIME_MONOTONIC);
  looper->sendMessageAtTime(now + ms2ns(30), h, Message(3));
  looper->sendMessageAtTime(now + ms2ns(10), h, Message(1));
  looper->sendMessage(h, Message(0));
  looper->sendMessageAtTime(now + ms2ns(20), h, Message(2));
  for (int i = 0; i < 20 && h->seen.size() < 4; i++) looper->pollOnce(100);
  EXPECT_EQ((std::vector<int>{0, 1, 2, 3}), h->seen);
  EXPECT_GE(systemTime(SYSTEM_TIME_MONOTONIC) - now, ms2ns(30));
}

TEST(Looper8, WakeFromAnotherThread) {
  sp<Looper> looper = new Looper(true);
  struct Waker : Thread {
    sp<Looper> l;
    bool threadLoop() override {
      usleep(20 * 1000);
      l->wake();
      return false;
    }
  };
  sp<Waker> w = new Waker;
  w->l = looper;
  ASSERT_EQ(NO_ERROR, w->run("fw8-waker"));
  nsecs_t start = systemTime(SYSTEM_TIME_MONOTONIC);
  EXPECT_EQ(Looper::POLL_WAKE, looper->pollOnce(5000));
  EXPECT_LT(systemTime(SYSTEM_TIME_MONOTONIC) - start, ms2ns(4000));
  w->join();
}

TEST(Looper8, FdCallback) {
  sp<Looper> looper = new Looper(true);
  int fds[2];
  ASSERT_EQ(0, pipe(fds));
  struct Ctx {
    int calls = 0;
    char got = 0;
  } ctx;
  auto cb = [](int fd, int events, void* data) -> int {
    Ctx* c = static_cast<Ctx*>(data);
    c->calls++;
    if (events & Looper::EVENT_INPUT) read(fd, &c->got, 1);
    return 0;  // unregister
  };
  ASSERT_EQ(1, looper->addFd(fds[0], 0, Looper::EVENT_INPUT, cb, &ctx));
  ASSERT_EQ(1, write(fds[1], "x", 1));
  for (int i = 0; i < 10 && ctx.calls == 0; i++) looper->pollOnce(100);
  EXPECT_EQ(1, ctx.calls);
  EXPECT_EQ('x', ctx.got);
  close(fds[0]);
  close(fds[1]);
}

TEST(Clock8, UptimeAndElapsed) {
  int64_t up = uptimeMillis(), elapsed = elapsedRealtime();
  EXPECT_GT(up, 0);
  EXPECT_GE(elapsed, up - 1000);  // elapsed counts sleep too; here both run together
  usleep(15 * 1000);
  EXPECT_GE(uptimeMillis() - up, 10);
}

// --- properties --------------------------------------------------------------------------

TEST(Properties8, BuildPropAndSet) {
  char value[PROPERTY_VALUE_MAX];
  EXPECT_EQ(2, property_get("ro.build.version.sdk", value, ""));
  EXPECT_STREQ("27", value);
  EXPECT_EQ(5, property_get("ro.build.version.release", value, ""));
  EXPECT_STREQ("8.1.0", value);
  EXPECT_EQ(27, property_get_int32("ro.build.version.sdk", -1));
  EXPECT_EQ(0, property_set("fw8.test.prop", "on"));
  EXPECT_TRUE(property_get_bool("fw8.test.prop", false));
  int listed = 0;
  property_list([](const char* k, const char* v, void* c) {
    if (!strcmp(k, "fw8.test.prop") && !strcmp(v, "on")) ++*static_cast<int*>(c);
  }, &listed);
  EXPECT_EQ(1, listed);
}

}  // namespace
