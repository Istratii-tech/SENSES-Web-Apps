// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// This header provides the same functionalities as the sync driver in Linux
// kernel. Just like the sync driver, this header consists 3 classes: Timeline,
// SyncPoint, and Fence.
//
// Timeline:  A timeline represents a monotonically increasing counter. On
//            Linux, hardware vendor can provide a hardware specific
//            implementation. On destruction, all sync points on the timeline
//            are signaled.
// SyncPoint: A sync point represents a specific value on the attached timeline.
//            Sync point may not belong to any timeline.
// Fence:     A fence is a collection of sync points. This is backed by a file
//            descriptor. A fence may have sync points on different timelines.
//
// An example diagram:
//
// Timeline(TL) and SyncPoint(SP): (*: counter, +: sync points)
//          SP1            SP2
// --*-------+--------------+----------------> TL1
//                                SP3
//         ----*-------------------+---------> TL2
//
// Fence(FE):
// FE1: [SP1]:
// FE2: [SP2, SP3]:
//
// Here, above system works as follows:
// 1. Each timeline increments their counter at any time.
// 2. If TL1 counter reaches SP1, the SP1 sync point is signaled. As the result,
//    the FE1 is signaled since FE1 only has SP1 sync point.
// 3. Then, if TL1 counter reaches SP2, the sync point SP2 is signaled. However
//    FE2 is not signaled since it also has SP3 which is not signalled yet.
// 4. Then, if TL2 counter reaches SP3, the sync point SP3 is signaled and FE2
//    is also signaled since all sync points which FE2 have are signaled.
//
// Note that all sync point must be managed by a fence, and also all sync point
// must be on a timeline.
//
// Also note that creating a new sync points or a new fence stream are timeline
// implementation dependent. For example, there is a reference implementation
// in Android. Its timeline is file descriptor backed implementation, hence
// a new fence can be created by calling ioctl with SW_SYNC_IOC_CREATE_FENCE
// and a timeline file descriptor. For more detail please see
// http://git.kernel.org/cgit/linux/kernel/git/torvalds/linux.git/tree/drivers/staging/android/uapi/sw_sync.h
//
// Fence FD accepts following requests for ioctl(2):
// SYNC_IOC_WAIT:
//   Block until all sync points in the fence are signaled or timeout is
//   reached. The third argument is a pointer to an integer, which is timeout
//   period in millisecond.
// SYNC_IOC_FENCE_INFO:
//   Retrieve fence information including attached sync points.
//   The third argument is a pointer of sync_fence_info_data struct which is
//   used as both input/output. As the input, sync_fence_info_data::len is the
//   total length of the passed buffer. With this function call, the fence
//   information and multiple attached sync point information are written to
//   the buffer. If the size of the buffer is not sufficient for filling, this
//   function fails with ENOMEM.
// SYNC_IOC_MERGE:
//   Create a new "merged" fence which has copied sync points in both passed two
//   fences: the first fence is passed as the first argument, and the other
//   fence is passed in the third argument's struct. Here "merged" means that
//   waiting the merged fence is equal to waiting both passed two fences.
//   The third argument is a pointer to a sync_merge_data struct. |fd2| and
//   |name| members in that sturct are used for input, and |fence| member is
//   used for output. The |fence| will be filled with a new fence FD whose name
//   is |name|. As the result of this process, |fence| has both sync points in
//   passed fences. If two sync points are attached to the same timeline, as the
//   result of this process, only the later one is used. The sync points in a
//   new fence are copied. The passed two fences and their sync points are not
//   affected by this operation.
//   For example, merging FE1 and FE2 in the diagram above results in a new
//   fence FE3 which has two sync points:[SP2, SP3]. Here FE3 doesn't have SP1
//   since there is a later sync point SP2 in FE2. Even after this operation,
//   FE1 and FE2 are still alive.
// Also, if the fence is no longer necessary, the fence can be closed with
// close(2). For more details, please see
// http://git.kernel.org/cgit/linux/kernel/git/torvalds/linux.git/tree/drivers/staging/android/uapi/sync.h
//
// To reduce contention of file system mutex, use different mutex for guarding
// each fence and sync points.
#ifndef POSIX_TRANSLATION_FENCE_STREAM_H_
#define POSIX_TRANSLATION_FENCE_STREAM_H_

#include <map>
#include <string>

#include "base/basictypes.h"
#include "base/compiler_specific.h"
#include "base/memory/ref_counted.h"
#include "base/memory/scoped_ptr.h"
#include "base/memory/scoped_vector.h"
#include "base/synchronization/condition_variable.h"
#include "base/synchronization/lock.h"
#include "base/threading/thread_checker.h"
#include "common/export.h"
#include "posix_translation/device_file.h"
#include "posix_translation/file_system_handler.h"

struct sync_pt_info;

namespace posix_translation {

class FenceStream;
class SyncPoint;

// A software based timeline implementation. Timeline has a monotnically
// increasing counter and it is incremented by IncrementCounter. This timeline
// will signal the SyncPoints which are added by AddNewSyncPoint when this
// internal counter reaches each sync point's signaling time.
//
// This Timeline implementation is compatible with sw_sync in Linux kernel but
// no user-space APIs are provided.
// http://git.kernel.org/cgit/linux/kernel/git/torvalds/linux.git/tree/drivers/staging/android/sw_sync.h
class ARC_EXPORT Timeline : public base::RefCountedThreadSafe<Timeline> {
 public:
  Timeline();

  int CreateFence(const std::string& name, uint32_t signaling_time);

  // Increments the internal counter. This function does nothing even if the
  // internal counter overflows.
  void IncrementCounter(uint32_t amount);

 protected:
  friend class base::RefCountedThreadSafe<Timeline>;
  virtual ~Timeline();

 private:
  friend class TestableTimeline;
  friend class TimelineTest;
  friend class FenceStream;  // For hiding Attach/DetachSyncPoint.

  // Attaches the |pt| to |this| timeline. Each sync point calls this function
  // when it is constructed.
  void AttachSyncPoint(FenceStream* fence, SyncPoint* pt);
  void AttachSyncPointLocked(FenceStream* fence, SyncPoint* pt);

  // Removes |pt| from |this| timeline. Each sync point calls this function when
  // it is destructed.
  void DetachSyncPoint(SyncPoint* pt);

  // A monotonically increasing counter.
  uint32_t counter_;

  // A map from firing counter value to sync point pointer.
  // |this| object does not own |sync_points_|.
  std::multimap<uint32_t, SyncPoint*> sync_points_;

  // A map from a sync point to a fence stream which is the owner of the sync
  // point. |this| object does not own both FenceStream and SyncPoint. The
  // FenceStream will not be released until DetachSyncPoint is called.
  std::map<SyncPoint*, FenceStream*> sync_point_fence_;

  // A lock for protecting all fields of this Timeline.
  base::Lock mutex_;

  DISALLOW_COPY_AND_ASSIGN(Timeline);
};

// A SyncPoint represents a value on timeline. SyncPoint can be only attached on
// one time line and also only attached on one fence. SyncPoints are typically
// destroyed when the attached fence stream is closed.
class SyncPoint {
 public:
  // The caller must not delete the new SyncPoint instance since |fence| takes
  // ownership of the instance to ensure that |fence| always outlives the
  // instance. This constructor must be called with both |timeline| and
  // |fence| locked.
  // If the creating sync point has already been signaled, must pass the
  // signaled timestamp to |timestamp_ns|, otherwise must pass 0ULL.
  SyncPoint(uint32_t signaling_time, uint64_t timestamp_ns);

  ~SyncPoint();

  // Updates the sync point state as signaled.
  void MarkAsSignaled();

  // Returns true if this sync point has already been signaled.
  bool IsSignaled();

  // Fills |info|. Returns written length. Returns 0 on error.
  uint32_t FillSyncPtInfo(struct sync_pt_info* info, uint32_t length);

  uint32_t signaling_time() const { return signaling_time_; }
  uint64_t timestamp_ns();

 private:
  // A timestamp when this sync point was signaled. This is a monotonic time
  // from the boot time and 0 if not signaled yet.
  uint64_t timestamp_ns_;

  // If the internal counter of |timeline_| reaches |signaling_time_|, this sync
  // point is signaled.
  const uint32_t signaling_time_;

  // |mutex_| protects all fields in SyncPoint.
  base::Lock mutex_;

  DISALLOW_COPY_AND_ASSIGN(SyncPoint);
};

// A Fence is a collection of sync points. Fence is backed by a file descriptor
// and the file descriptor can be passed to userspace. The application in
// userspace can call ioctl(2) and close(2).
class FenceStream : public FileStream {
 public:
  enum FenceStatus {
    FENCE_ACTIVE = 0,  // The fence is not signaled. This is initial state.
    FENCE_SIGNALED = 1,  // The fence is signaled.
  };

  struct SyncPointTimeline {
    SyncPointTimeline(scoped_ptr<SyncPoint> sp, scoped_refptr<Timeline> tm);
    ~SyncPointTimeline();

    scoped_ptr<SyncPoint> sync_point;
    scoped_refptr<Timeline> timeline;
  };

  // The |fence_name| is used to fill sync_fence_info_data::name when
  // SYNC_IOC_FENCE_INFO is requested. To create fence stream, filesystem lock
  // needs to be acquired.
  static scoped_refptr<FenceStream> CreateFence(
      const std::string& fence_name,
      ScopedVector<SyncPointTimeline> sync_points);
  static scoped_refptr<FenceStream> CreateFenceTimelineLocked(
      const std::string& fence_name,
      ScopedVector<SyncPointTimeline> sync_points);

  // FileStream overrides.
  virtual int ioctl(int request, va_list ap) OVERRIDE;
  virtual ssize_t read(void* buf, size_t count) OVERRIDE;
  virtual ssize_t write(const void* buf, size_t count) OVERRIDE;
  virtual const char* GetStreamType() const OVERRIDE;

  // Looks all the |sync_points_| and signals |fence_cond_| if all of them are
  // signaled state.
  void MaybeSignal();

  // Returns the number of waiting threads on |fence_cond_|. This function
  // acquires |fence_mutex_|.
  uint32_t GetWaitingThreadCountFenceForTesting();

 protected:
  virtual ~FenceStream();

 private:
  friend class FenceStreamTest;

  // Uses CreateFence/CreateFenceTimelineLocked instead.
  FenceStream(const std::string& fence_name,
              ScopedVector<SyncPointTimeline> sync_points);

  // SYNC_IOC_WAIT ioctl request handler.
  int SyncIocWait(va_list ap);

  // SYNC_IOC_MERGE ioctl request handler.
  int SyncIocMerge(va_list ap);

  // SYNC_IOC_FENCE_INFO ioctl request handler.
  int SyncIocFenceInfo(va_list ap);

  // Returns true if the fence stream is valid. The validity of fence stream is
  // that 1)the internal state is consistent with sync points and 2)each sync
  // point is on a different timeline.
  bool IsValidLocked() const;

  // Returns the number of signaled sync points.
  uint32_t GetSignaledSyncPointCountLocked() const;

  // Same as MaybeSignal but this function can be called with |fence_mutex_| is
  // acquired.
  void MaybeSignalLocked();

  const std::string fence_name_;
  FenceStatus status_;

  // |fence_mutex_| must be declared before |fence_cond_|.
  // |fence_mutex_| protects all fields in this fence stream except for
  // |sync_points_|.
  base::Lock fence_mutex_;
  base::ConditionVariable fence_cond_;

  const ScopedVector<SyncPointTimeline> sync_points_;

  // The number of waiting threads on |fence_cond_|.
  uint32_t waiting_thread_count_for_testing_;

  DISALLOW_COPY_AND_ASSIGN(FenceStream);
};

}  // namespace posix_translation
#endif  // POSIX_TRANSLATION_FENCE_STREAM_H_
