// Dormant waits of Java threads on ART 10 (topic java-pool-park,
// docs/topics/java-pool-park.md): a thread started by Thread.start that has
// waited longer than a grace period (in LockSupport.park, Object.wait,
// Thread.sleep or an idle Looper) gives its browser worker back. Its Java
// frames are copied to the heap as deoptimized frames, the pthread exits,
// and whatever ends the wait (unpark, notify, interrupt, a ready file
// descriptor, the timeout) starts a new pthread that takes over the same
// art::Thread and continues the frames in the interpreter after the call
// that waited. Included by thread.cc, monitor.cc and interpreter.cc
// (patches/aosp10/zz-pool-park.patch). Part of the arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_ART10_DORMANT_WASM10_H_
#define ARC_WASM_SHIM_ART10_DORMANT_WASM10_H_

#include <stddef.h>
#include <stdint.h>

#include "base/locks.h"
#include "base/macros.h"
#include "thread_state.h"

namespace art {

class ArtMethod;
class Monitor;
class ShadowFrame;
class Thread;
union JValue;

namespace interpreter {
// interpreter.cc: continues |shadow_frame| (a heap frame) at its dex pc, as
// for a deoptimized frame; |result| is the result register (of the call the
// frame made).
JValue ArtWasmResumeFrame(Thread* self, ShadowFrame* shadow_frame, JValue result)
    REQUIRES_SHARED(Locks::mutator_lock_);
}  // namespace interpreter

// A friend of Thread and Monitor (patches/aosp10/zz-pool-park.patch).
class ArtWasmDormant {
 public:
  // Thread::CreateCallback, right before Thread.run: the thread may go
  // dormant (if enabled).
  static void ThreadStart(Thread* self) REQUIRES_SHARED(Locks::mutator_lock_);
  // ~Thread.
  static void ThreadGone(Thread* t);

  // Thread::Park when there was no permit (park state "waiter waiting"):
  // the wait, which may leave the worker after the grace period. False: the
  // thread cannot (the caller waits as usual); true: the wait is over
  // (*timed_out set). Does not return if the thread went dormant: it goes on
  // after the park on another worker.
  static bool ParkWait(Thread* self, bool is_absolute, int64_t time, bool* timed_out)
      NO_THREAD_SAFETY_ANALYSIS;
  // Thread::Unpark, when the park state was "waiter waiting".
  static void Unparked(Thread* t);

  // Monitor::Wait, holding self's wait mutex, suspended, in the wait set:
  // the wait (to a notify, an interrupt or the timeout). False: the caller
  // waits as usual.
  static bool MonitorWait(Thread* self, Monitor* monitor, ThreadState why, int64_t ms,
                          int32_t ns, int prev_lock_count, ArtMethod* saved_method,
                          uintptr_t saved_dex_pc, bool interrupt_should_throw, bool* timed_out)
      NO_THREAD_SAFETY_ANALYSIS;
  // Wakes |waiter| (its wait monitor set; its wait mutex held): instead of
  // waiter->GetWaitConditionVariable()->Signal(self).
  static void SignalWaiter(Thread* self, Thread* waiter) NO_THREAD_SAFETY_ANALYSIS;

  // A native's wait for epoll descriptor |epfd| to have events (level-
  // triggered: an idle Looper), up to |timeout_ms| (< 0: no timeout), in the
  // native state (arc_java_dormant_epoll). 0: the thread cannot go dormant
  // (the caller waits as usual). Otherwise does not return: the thread goes
  // on after the native's call, on another worker, after |on_resume|(|arg|)
  // ran on the new pthread.
  static int EpollDormant(Thread* self, int epfd, int timeout_ms, void (*on_resume)(void*),
                          void* arg) NO_THREAD_SAFETY_ANALYSIS;
  // The file system: epoll descriptor |epfd| has events now.
  static void EpollReady(int epfd);

  // The new pthread of a dormant thread whose wait ended.
  static void* Resume(void* arg) NO_THREAD_SAFETY_ANALYSIS;

  struct Rec;

 private:
  static Rec* RecOf(Thread* t);
  static bool Commit(Thread* self, Rec* r, int kind, double deadline, uint32_t* gen, bool* retry)
      NO_THREAD_SAFETY_ANALYSIS;
  [[noreturn]] static void ExitPthread(Thread* self) NO_THREAD_SAFETY_ANALYSIS;
  static void FinishMonitorWait(Thread* self, Rec* r) NO_THREAD_SAFETY_ANALYSIS;
  static void ContinueFrames(Thread* self, ShadowFrame* frames) NO_THREAD_SAFETY_ANALYSIS;
};

}  // namespace art

#endif  // ARC_WASM_SHIM_ART10_DORMANT_WASM10_H_
