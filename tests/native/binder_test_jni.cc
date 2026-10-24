// Natives of arcwasm.BinderTest (tests/java/arcwasm/BinderTest.java),
// loaded with System.load("/system/lib/libarcwasm_binder_test.so"), which
// shim/dalvik/static_jni_libs.cc maps to arc_wasm_binder_test_JNI_OnLoad.
//
//  - "arcwasm.native": a libbinder BBinder in this process, registered with
//    defaultServiceManager() and called from Java;
//  - a call from C++ into the Java service "arcwasm.echo";
//  - "arcwasm.remote": a service in another binder process (pid 4242, uid
//    1000) on its own thread, written with servicemanager's binder.c
//    helpers, as a native daemon would be. Java reaches it through a real
//    handle: transactions, one-way calls, calls back into Java and death
//    notices all go through the driver.
//
// Built for four stacks: Android 4.4 on Dalvik (mk/topic-binder.mk),
// Android 6.0.1 on ART (mk/topic-fw6-core.mk), 8.1 on ART 8.1
// (mk/topic-fw8-core.mk, ARC_FW8) and 10 on ART 10 (mk/topic-fw10-core.mk,
// ARC_FW10). Their servicemanager binder.c differ: 6.0's, 8.1's and 10's
// (BINDER_IPC_32BIT builds) pass handles as uint32_t and transactions as
// binder_transaction_data (10's handler gets it inside
// binder_transaction_data_secctx); the aliases below cover them; 8.1's and
// 10's binder_open also take the device's path.

#include <jni.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <vector>

#include <binder/Binder.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/Parcel.h>
#include <utils/Log.h>
#include <utils/String16.h>
#include <utils/String8.h>

#include "arc_binder.h"
extern "C" {
#include "binder.h"  // frameworks/native/cmds/servicemanager
#if defined(BINDER_IPC_32BIT)
int binder_write(struct binder_state* bs, void* data, size_t len);
int binder_parse(struct binder_state* bs, struct binder_io* bio,
                 uintptr_t ptr, size_t size, binder_handler func);
#else
int binder_write(struct binder_state* bs, void* data, unsigned len);
int binder_parse(struct binder_state* bs, struct binder_io* bio,
                 uint32_t* ptr, uint32_t size, binder_handler func);
#endif
}

using namespace android;

namespace {

#if defined(ARC_FW10)
typedef struct binder_transaction_data_secctx Txn;  // what a handler gets
#define TXN(t) (&(t)->transaction_data)               // the transaction in it
typedef struct binder_transaction_data TxnOut;      // what BC_TRANSACTION takes
typedef uint32_t Ref;
typedef size_t StrLen;
#elif defined(BINDER_IPC_32BIT)
typedef struct binder_transaction_data Txn;  // what a handler gets
#define TXN(t) (t)
typedef Txn TxnOut;
typedef uint32_t Ref;                         // a handle
typedef size_t StrLen;
#else
typedef struct binder_txn Txn;
#define TXN(t) (t)
typedef Txn TxnOut;
typedef void* Ref;
typedef uint32_t StrLen;
#endif

// --- a libbinder service in this process ---------------------------------

class NativeService : public BBinder {
 public:
  static const String16& descriptor() {
    static const String16 d("arcwasm.INative");
    return d;
  }
  virtual const String16& getInterfaceDescriptor() const { return descriptor(); }

 protected:
  // multiply(int a, int b, String s) -> (int a*b, String s + " from C++",
  // int calling pid)
  virtual status_t onTransact(uint32_t code, const Parcel& data, Parcel* reply,
                              uint32_t flags) {
    if (code != IBinder::FIRST_CALL_TRANSACTION)
      return BBinder::onTransact(code, data, reply, flags);
    if (!data.enforceInterface(descriptor()))
      return PERMISSION_DENIED;
    const int32_t a = data.readInt32();
    const int32_t b = data.readInt32();
    String8 s(data.readString16());
    s.append(" from C++");
    reply->writeNoException();
    reply->writeInt32(a * b);
    reply->writeString16(String16(s));
    reply->writeInt32(IPCThreadState::self()->getCallingPid());
    return NO_ERROR;
  }
};

jboolean AddNativeService(JNIEnv*, jclass) {
  return defaultServiceManager()->addService(String16("arcwasm.native"),
                                             new NativeService) == NO_ERROR;
}

// IEcho.add(a, b) on the Java service, from C++.
jint CallJavaEcho(JNIEnv*, jclass, jint a, jint b) {
  sp<IBinder> echo = defaultServiceManager()->checkService(String16("arcwasm.echo"));
  if (echo == NULL)
    return -1;
  Parcel data, reply;
  data.writeInterfaceToken(String16("arcwasm.IEcho"));
  data.writeInt32(a);
  data.writeInt32(b);
  if (echo->transact(IBinder::FIRST_CALL_TRANSACTION, data, &reply) != NO_ERROR)
    return -2;
  if (reply.readExceptionCode() != 0)
    return -3;
  return reply.readInt32();
}

// --- a service in another binder process ---------------------------------

const int kRemotePid = 4242;
const int kRemoteUid = 1000;  // AID_SYSTEM: servicemanager lets it register

enum {
  kRemoteSum = 1,         // (int[] values, String s) -> (int sum, String s,
                          //   int calling pid, int calling uid)
  kRemoteRecord = 2,      // oneway (int v)
  kRemoteRecorded = 3,    // () -> int[] recorded values, in order
  kRemoteCallBack = 4,    // (IBinder cb, int v) -> int cb.call(v)
  kRemoteRegister = 5,    // (IBinder cb): keep a reference
  kRemoteNotify = 6,      // (int v): oneway cb.call(v) to the kept one
  kRemoteUnregister = 7,  // (): drop it
  kRemoteDie = 8,         // oneway: close the binder descriptor (exit)
};

pthread_mutex_t g_remote_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_remote_cond = PTHREAD_COND_INITIALIZER;
int g_remote_state;  // 0 not started, 1 starting, 2 serving, -1 failed

// Only the remote thread touches these.
std::vector<uint32_t> g_recorded;
Ref g_callback;
bool g_die;

void SetRemoteState(int s) {
  pthread_mutex_lock(&g_remote_lock);
  g_remote_state = s;
  pthread_cond_broadcast(&g_remote_cond);
  pthread_mutex_unlock(&g_remote_lock);
}

// The header of an interface token before the descriptor: the strict mode
// policy, and since Android 10 the work source (Parcel::writeInterfaceToken).
void PutTokenHeader(struct binder_io* msg) {
  bio_put_uint32(msg, 0);  // strict mode policy
#if defined(ARC_FW10)
  bio_put_uint32(msg, 0xffffffff);  // work source: IPCThreadState::kUnsetWorkSource
#endif
}

bool CheckToken(struct binder_io* msg, const char* expected) {
  bio_get_uint32(msg);  // strict mode policy
#if defined(ARC_FW10)
  bio_get_uint32(msg);  // work source
#endif
  StrLen len;
  uint16_t* s = bio_get_string16(msg, &len);
  if (!s || len != strlen(expected))
    return false;
  for (uint32_t i = 0; i < len; i++) {
    if (s[i] != static_cast<uint8_t>(expected[i]))
      return false;
  }
  return true;
}

// The ICallback message: interface token and an int.
void PutCallbackCall(struct binder_io* msg, uint32_t v) {
  PutTokenHeader(msg);
  bio_put_string16_x(msg, "arcwasm.ICallback");
  bio_put_uint32(msg, v);
}

// binder.c only makes synchronous calls; a one-way one by hand.
void CallOneway(struct binder_state* bs, Ref target, uint32_t code,
                struct binder_io* msg) {
  struct {
    uint32_t cmd;
    TxnOut txn;
  } __attribute__((packed)) w;
  memset(&w, 0, sizeof(w));
  w.cmd = BC_TRANSACTION;
#if defined(BINDER_IPC_32BIT)
  w.txn.target.handle = target;
  w.txn.code = code;
  w.txn.flags = TF_ONE_WAY;
  w.txn.data_size = msg->data - msg->data0;
  w.txn.offsets_size = reinterpret_cast<char*>(msg->offs) -
                       reinterpret_cast<char*>(msg->offs0);
  w.txn.data.ptr.buffer = reinterpret_cast<uintptr_t>(msg->data0);
  w.txn.data.ptr.offsets = reinterpret_cast<uintptr_t>(msg->offs0);
#else
  w.txn.target = target;
  w.txn.cookie = 0;
  w.txn.code = code;
  w.txn.flags = TF_ONE_WAY;
  w.txn.data_size = msg->data - msg->data0;
  w.txn.offs_size = reinterpret_cast<char*>(msg->offs) -
                    reinterpret_cast<char*>(msg->offs0);
  w.txn.data = msg->data0;
  w.txn.offs = msg->offs0;
#endif
  binder_write(bs, &w, sizeof(w));
}

int RemoteHandler(struct binder_state* bs, Txn* txn,
                  struct binder_io* msg, struct binder_io* reply) {
  if (!CheckToken(msg, "arcwasm.IRemote"))
    return -1;
  switch (TXN(txn)->code) {
    case kRemoteSum: {
      const uint32_t n = bio_get_uint32(msg);
      uint32_t sum = 0;
      for (uint32_t i = 0; i < n; i++)
        sum += bio_get_uint32(msg);
      StrLen len;
      uint16_t* s = bio_get_string16(msg, &len);
      bio_put_uint32(reply, 0);  // no exception
      bio_put_uint32(reply, sum);
      bio_put_string16(reply, s);
      bio_put_uint32(reply, TXN(txn)->sender_pid);
      bio_put_uint32(reply, TXN(txn)->sender_euid);
      return 0;
    }
    case kRemoteRecord:
      g_recorded.push_back(bio_get_uint32(msg));
      return 0;
    case kRemoteRecorded:
      bio_put_uint32(reply, 0);
      bio_put_uint32(reply, g_recorded.size());
      for (size_t i = 0; i < g_recorded.size(); i++)
        bio_put_uint32(reply, g_recorded[i]);
      g_recorded.clear();
      return 0;
    case kRemoteCallBack: {
      Ref cb = bio_get_ref(msg);
      const uint32_t v = bio_get_uint32(msg);
      if (!cb)
        return -1;
      unsigned iodata[128];
      struct binder_io cbmsg, cbreply;
      bio_init(&cbmsg, iodata, sizeof(iodata), 4);
      PutCallbackCall(&cbmsg, v);
      // A call back into the process that is waiting for this reply: the
      // driver hands it to the very thread that waits.
      if (binder_call(bs, &cbmsg, &cbreply, cb, IBinder::FIRST_CALL_TRANSACTION))
        return -1;
      const uint32_t exception = bio_get_uint32(&cbreply);
      const uint32_t r = bio_get_uint32(&cbreply);
      binder_done(bs, &cbmsg, &cbreply);
      bio_put_uint32(reply, 0);
      bio_put_uint32(reply, exception ? 0xdead : r);
      return 0;
    }
    case kRemoteRegister: {
      Ref cb = bio_get_ref(msg);
      if (!cb || g_callback)
        return -1;
      // Keep the handle beyond this transaction's buffer.
      binder_acquire(bs, cb);
      g_callback = cb;
      bio_put_uint32(reply, 0);
      return 0;
    }
    case kRemoteNotify: {
      const uint32_t v = bio_get_uint32(msg);
      if (!g_callback)
        return -1;
      unsigned iodata[128];
      struct binder_io cbmsg;
      bio_init(&cbmsg, iodata, sizeof(iodata), 4);
      PutCallbackCall(&cbmsg, v);
      CallOneway(bs, g_callback, IBinder::FIRST_CALL_TRANSACTION, &cbmsg);
      bio_put_uint32(reply, 0);
      return 0;
    }
    case kRemoteUnregister:
      if (g_callback)
        binder_release(bs, g_callback);
      g_callback = 0;
      bio_put_uint32(reply, 0);
      return 0;
    case kRemoteDie:
      g_die = true;
      return 0;
  }
  return -1;
}

void* RemoteProcess(void*) {
  arc_binder_set_process_identity(kRemotePid, kRemoteUid);
#if defined(ARC_FW8) || defined(ARC_FW10)
  struct binder_state* bs = binder_open("/dev/binder", 128 * 1024);
#else
  struct binder_state* bs = binder_open(128 * 1024);
#endif
  if (!bs) {
    SetRemoteState(-1);
    return NULL;
  }
  // servicemanager.addService("arcwasm.remote", this node)
  unsigned iodata[128];
  struct binder_io msg, reply;
  bio_init(&msg, iodata, sizeof(iodata), 4);
  PutTokenHeader(&msg);
  bio_put_string16_x(&msg, SVC_MGR_NAME);
  bio_put_string16_x(&msg, "arcwasm.remote");
  bio_put_obj(&msg, reinterpret_cast<void*>(0x1234));
  bio_put_uint32(&msg, 0);  // allow_isolated
  if (binder_call(bs, &msg, &reply, BINDER_SERVICE_MANAGER, SVC_MGR_ADD_SERVICE)) {
    SetRemoteState(-1);
    return NULL;
  }
  const uint32_t status = bio_get_uint32(&reply);
  binder_done(bs, &msg, &reply);
  if (status != 0) {
    SetRemoteState(-1);
    return NULL;
  }
  SetRemoteState(2);

  // binder_loop(), until told to die.
  uint32_t readbuf[32];
  readbuf[0] = BC_ENTER_LOOPER;
  binder_write(bs, readbuf, sizeof(uint32_t));
  while (!g_die) {
    struct binder_write_read bwr;
    memset(&bwr, 0, sizeof(bwr));
    bwr.read_size = sizeof(readbuf);
    bwr.read_buffer = reinterpret_cast<uintptr_t>(readbuf);
    // struct binder_state (private to binder.c) starts with the descriptor.
    if (ioctl(*reinterpret_cast<int*>(bs), BINDER_WRITE_READ, &bwr) < 0)
      break;
#if defined(BINDER_IPC_32BIT)
    if (binder_parse(bs, 0, reinterpret_cast<uintptr_t>(readbuf), bwr.read_consumed,
                     RemoteHandler) <= 0)
#else
    if (binder_parse(bs, 0, readbuf, bwr.read_consumed, RemoteHandler) <= 0)
#endif
      break;
  }
  // The process exits: the driver tells everyone who holds a handle to it.
  binder_close(bs);
  return NULL;
}

jboolean StartRemoteProcess(JNIEnv*, jclass) {
  pthread_mutex_lock(&g_remote_lock);
  if (g_remote_state == 0) {
    g_remote_state = 1;
    pthread_t thread;
    if (pthread_create(&thread, NULL, RemoteProcess, NULL) == 0)
      pthread_detach(thread);
    else
      g_remote_state = -1;
  }
  while (g_remote_state == 1)
    pthread_cond_wait(&g_remote_cond, &g_remote_lock);
  const bool ok = g_remote_state == 2;
  pthread_mutex_unlock(&g_remote_lock);
  return ok;
}

const JNINativeMethod kMethods[] = {
    {"nativeAddService", "()Z", reinterpret_cast<void*>(AddNativeService)},
    {"nativeCallEcho", "(II)I", reinterpret_cast<void*>(CallJavaEcho)},
    {"nativeStartRemoteProcess", "()Z", reinterpret_cast<void*>(StartRemoteProcess)},
};

}  // namespace

extern "C" int arc_wasm_binder_test_JNI_OnLoad(JavaVM* vm, void* reserved) {
  JNIEnv* env;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_4) != JNI_OK)
    return -1;
  jclass clazz = env->FindClass("arcwasm/BinderTest");
  if (!clazz)
    return -1;
  if (env->RegisterNatives(clazz, kMethods,
                           sizeof(kMethods) / sizeof(kMethods[0])) < 0)
    return -1;
  return JNI_VERSION_1_4;
}
