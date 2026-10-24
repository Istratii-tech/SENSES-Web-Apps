// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "posix_translation/fence_stream.h"

#include <linux/sync.h>
#include <sched.h>

#include <algorithm>
#include <map>
#include <utility>
#include <vector>

#include "base/compiler_specific.h"
#include "base/memory/scoped_vector.h"
#include "base/strings/string_util.h"
#include "base/synchronization/waitable_event.h"
#include "base/threading/simple_thread.h"
#include "base/time/time.h"
#include "gtest/gtest.h"
#include "posix_translation/test_util/file_system_background_test_common.h"
#include "posix_translation/virtual_file_system.h"
#include "ppapi_mocks/background_test.h"

namespace posix_translation {

namespace {
const char kDriverName[] = "sw_sync";
const char kFenceName[] = "test_fence";
const char kTimelineName[] = "arc";

const int kDefaultTimeoutInMs = 5 * 60 * 1000;  // 5 min

// Helper function to get number of sync_pt_info in sync_fence_info_data.
uint32_t SyncPtInfoCount(sync_fence_info_data* info) {
  uint32_t read_len = sizeof(sync_fence_info_data);
  int i = 0;
  while (read_len < info->len) {
    sync_pt_info* pt_info = reinterpret_cast<sync_pt_info*>(
        reinterpret_cast<uint8_t*>(info) + read_len);
    read_len += pt_info->len;
    i++;
  }
  return i;
}

// Helper function to get sync_pt_info in sync_fence_info_data.
// The |idx| is zero-origin.
const sync_pt_info* GetSyncPtInfo(sync_fence_info_data* info, uint32_t idx) {
  if (idx >= SyncPtInfoCount(info))
    return NULL;

  uint32_t read_len = sizeof(sync_fence_info_data);
  sync_pt_info* result = NULL;
  do {
    result = reinterpret_cast<sync_pt_info*>(
        reinterpret_cast<uint8_t*>(info) + read_len);
    read_len += result->len;
  } while (idx--);
  return result;
}

// Calls Timeline::IncrementCounter with |value_| |times_| times.
class ThreadedIncrementor : public base::DelegateSimpleThread::Delegate {
 public:
  // Caller must free |event|.
  ThreadedIncrementor(scoped_refptr<Timeline> timeline, uint32_t value,
                      uint32_t times, base::WaitableEvent* event)
      : timeline_(timeline), value_(value), times_(times), event_(event),
        thread_(this, "threaded_incrementor") {}

  void Start() {
    thread_.Start();
  }

  void Join() {
    thread_.Join();
  }

 private:
  // base::DelegateSimpleThread::Delegate override.
  virtual void Run() OVERRIDE {
    // Wait until the |event_| is signaled.
    event_->Wait();

    for (uint32_t i = 0; i < times_; ++i) {
      timeline_->IncrementCounter(value_);
    }
  }

  scoped_refptr<Timeline> timeline_;
  const uint32_t value_;
  const uint32_t times_;
  base::WaitableEvent* event_;
  base::DelegateSimpleThread thread_;
};

// Adds and keeps the sync point to |timeline|.
class ThreadedAttacher : public base::DelegateSimpleThread::Delegate {
 public:
  // Caller must free |event|.
  ThreadedAttacher(scoped_refptr<Timeline> timeline,
                   uint32_t origin, uint32_t step, uint32 count,
                   base::WaitableEvent* event)
      : timeline_(timeline), origin_(origin),
        step_(step), count_(count),
        event_(event), thread_(this, "threaded_attacher") {}

  void Start() {
    thread_.Start();
  }

  void Join() {
    thread_.Join();
  }

 private:
  // base::DelegateSimpleThread::Delegate override.
  virtual void Run() OVERRIDE {
    event_->Wait();

    for (uint32_t i = 0; i < count_; ++i) {
      timeline_->CreateFence(kFenceName, origin_ + step_ * i);
    }
  }

  scoped_refptr<Timeline> timeline_;
  const uint32_t origin_;
  const uint32_t step_;
  const uint32_t count_;
  base::WaitableEvent* event_;
  base::DelegateSimpleThread thread_;
  std::vector<scoped_refptr<FenceStream> > created_streams_;
};

// Releases |fences_to_be_removed| on a different thread.
class ThreadedRemover : public base::DelegateSimpleThread::Delegate {
 public:
  // Caller must free |event|.
  ThreadedRemover(std::vector<int> fences_to_be_removed,
                  base::WaitableEvent* event)
      : fences_to_be_removed_(fences_to_be_removed), event_(event),
        thread_(this, "threaded_remover") {}

  void Start() {
    thread_.Start();
  }

  void Join() {
    thread_.Join();
  }

 private:
  // base::DelegateSimpleThread::Delegate override.
  virtual void Run() OVERRIDE {
    event_->Wait();
    VirtualFileSystem* vfs = VirtualFileSystem::GetVirtualFileSystem();
    for (size_t i = 0; i < fences_to_be_removed_.size(); ++i) {
      vfs->close(fences_to_be_removed_[i]);
    }
  }

  std::vector<int> fences_to_be_removed_;
  base::WaitableEvent* event_;
  base::DelegateSimpleThread thread_;
};

// Calls ioctl(SYNC_IOC_WAIT) on a different thread.
class ThreadedWaiter : public base::DelegateSimpleThread::Delegate {
 public:
  ThreadedWaiter(int fd, int ioctl_timeout)
      : fd_(fd), ioctl_timeout_(ioctl_timeout), result_(0),
        is_waiting_(false),
        vfs_(VirtualFileSystem::GetVirtualFileSystem()),
        thread_(this, "threaded_waiter") {}

  void StartAndBlockUntilReady() {
    scoped_refptr<FenceStream> fence = GetFenceStream();

    thread_.Start();

    // Busy-wait until the waiter thread starts waiting on the condition
    // variable.
    while (fence->GetWaitingThreadCountFenceForTesting() == 0) {
      sched_yield();
    }
  }

  void Join() {
    thread_.Join();
  }

  int result() { return result_; }

  bool IsWaiting() const {
    scoped_refptr<FenceStream> fence = GetFenceStream();
    return fence->GetWaitingThreadCountFenceForTesting() != 0;
  }

 private:
  // base::DelegateSimpleThread::Delegate override.
  virtual void Run() OVERRIDE {
    base::AutoLock lock(vfs_->mutex());
    result_ = CallIoctlLocked(SYNC_IOC_WAIT, &ioctl_timeout_);
  }

  int CallIoctlLocked(int request, ...) {
    va_list ap;
    va_start(ap, request);
    int ret = vfs_->GetStreamLocked(fd_)->ioctl(request, ap);
    va_end(ap);
    return ret;
  }

  scoped_refptr<FenceStream> GetFenceStream() const {
    base::AutoLock lock(vfs_->mutex());
    return static_cast<FenceStream*>(vfs_->GetStreamLocked(fd_).get());
  }

  int fd_;
  int ioctl_timeout_;
  int result_;
  bool is_waiting_;
  VirtualFileSystem* vfs_;
  base::DelegateSimpleThread thread_;

  DISALLOW_COPY_AND_ASSIGN(ThreadedWaiter);
};

// Calls ioctl(SYNC_IOC_MERGE) on different thread. This thread wait |event|
// just before calling ioctl.
class ThreadedMerger : public base::DelegateSimpleThread::Delegate {
 public:
  // Caller must free |event|.
  ThreadedMerger(const std::vector<int>& fds1, const std::vector<int>& fds2,
                 base::WaitableEvent* event)
      : fds1_(fds1), fds2_(fds2), result_(0),
        event_(event), vfs_(VirtualFileSystem::GetVirtualFileSystem()),
        thread_(this, "threaded_waiter") {
    ALOG_ASSERT(fds1.size() == fds2.size());
    ALOG_ASSERT(!fds1.empty());
    ALOG_ASSERT(!fds2.empty());
  }

  void Start() {
    thread_.Start();
  }

  void Join() {
    thread_.Join();
  }

  int GetMergedFenceFd(size_t index) {
    ALOG_ASSERT(index < merged_fence_fds_.size());
    return merged_fence_fds_[index];
  }

 private:
  // base::DelegateSimpleThread::Delegate override.
  virtual void Run() OVERRIDE {
    event_->Wait();
    merged_fence_fds_.resize(fds1_.size());
    for (size_t i = 0; i < fds1_.size(); ++i) {
      sync_merge_data merge_data = {};
      merge_data.fd2 = fds2_[i];
      base::strlcpy(merge_data.name, kFenceName, sizeof(merge_data.name));

      // Wait until the |event| is signaled.
      result_ &= CallIoctl(fds1_[i], SYNC_IOC_MERGE, &merge_data);
      merged_fence_fds_[i] = merge_data.fence;
      ALOG_ASSERT(merge_data.fence != -1);
    }
  }

  int CallIoctl(int fd, int request, ...) {
    base::AutoLock lock(vfs_->mutex());
    va_list ap;
    va_start(ap, request);
    int ret = vfs_->GetStreamLocked(fd)->ioctl(request, ap);
    va_end(ap);
    return ret;
  }

  std::vector<int> fds1_;
  std::vector<int> fds2_;
  std::vector<int> merged_fence_fds_;
  int result_;
  base::WaitableEvent* event_;
  VirtualFileSystem* vfs_;
  base::DelegateSimpleThread thread_;

  DISALLOW_COPY_AND_ASSIGN(ThreadedMerger);
};

}  // namespace

class TestableTimeline : public Timeline {
 public:
  TestableTimeline() {}

  virtual ~TestableTimeline() {}

  // Returns true if this timeline has at least one sync point at
  // |signaling_time|. Otherwise returns false.
  bool HasSyncPointAt(uint32_t signaling_time) {
    base::AutoLock lock(mutex_);
    std::pair<std::multimap<uint32_t, SyncPoint*>::iterator,
        std::multimap<uint32_t, SyncPoint*>::iterator> range =
            sync_points_.equal_range(signaling_time);
    return range.first != range.second;
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(TestableTimeline);
};

class TimelineTest : public FileSystemBackgroundTestCommon<TimelineTest> {
 public:
  DECLARE_BACKGROUND_TEST(ConstructDestruct);
  DECLARE_BACKGROUND_TEST(CreateFence);
  DECLARE_BACKGROUND_TEST(CreateFence_AtPastPoint);
  DECLARE_BACKGROUND_TEST(IncrementCounterTest);
  DECLARE_BACKGROUND_TEST(Threaded_IncrementCounterTest);
  DECLARE_BACKGROUND_TEST(Threaded_AttachRemoveTest);

 protected:
  virtual void SetUp() OVERRIDE {
    FileSystemBackgroundTestCommon<TimelineTest>::SetUp();
    vfs_ = VirtualFileSystem::GetVirtualFileSystem();
  }

  virtual void TearDown() OVERRIDE {
    vfs_ = NULL;
    FileSystemBackgroundTestCommon<TimelineTest>::TearDown();
  }

  uint32_t GetCounterValue(scoped_refptr<Timeline> timeline) {
    base::AutoLock lock(timeline->mutex_);
    return timeline->counter_;
  }

  bool IsSignaled(int fence_fd) {
    base::AutoLock lock(vfs_->mutex());
    errno = 0;
    int timeout = 0;
    int r = IoctlLocked(GetFenceStreamLocked(fence_fd), SYNC_IOC_WAIT,
                        &timeout);
    ALOG_ASSERT(errno == 0 || errno == ETIME);
    return r == 0;
  }

  size_t GetMapEntryCount(scoped_refptr<Timeline> timeline) {
    base::AutoLock lock(timeline->mutex_);
    return timeline->sync_points_.size();
  }

  base::Lock& GetMutex(scoped_refptr<Timeline> timeline) {
    return timeline->mutex_;
  }

  std::multimap<uint32_t, SyncPoint*>* GetInternalMapLocked(
      scoped_refptr<Timeline> timeline) {
    timeline->mutex_.AssertAcquired();
    return &timeline->sync_points_;
  }

 private:
  int IoctlLocked(scoped_refptr<FileStream> stream, int request, ...) {
    vfs_->mutex().AssertAcquired();
    va_list ap;
    va_start(ap, request);
    int r = stream->ioctl(request, ap);
    va_end(ap);
    return r;
  }

  scoped_refptr<FenceStream> GetFenceStreamLocked(int fence_fd) const {
    vfs_->mutex().AssertAcquired();
    return static_cast<FenceStream*>(vfs_->GetStreamLocked(fence_fd).get());
  }

  VirtualFileSystem* vfs_;
};

TEST_BACKGROUND_F(TimelineTest, ConstructDestruct) {
  scoped_refptr<Timeline> timeline = new Timeline();
  timeline = NULL;
}

TEST_BACKGROUND_F(TimelineTest, CreateFence) {
  scoped_refptr<Timeline> timeline1 = new Timeline();
  scoped_refptr<Timeline> timeline2 = new Timeline();
  scoped_refptr<Timeline> timeline3 = new Timeline();

  int fence_fd_tl1_1 = timeline1->CreateFence(kFenceName, 1);
  int fence_fd_tl1_2 = timeline1->CreateFence(kFenceName, 2);
  int fence_fd_tl1_3 = timeline1->CreateFence(kFenceName, 3);
  int fence_fd_tl2_1 = timeline2->CreateFence(kFenceName, 1);
  int fence_fd_tl2_2 = timeline2->CreateFence(kFenceName, 2);
  int fence_fd_tl2_3 = timeline2->CreateFence(kFenceName, 3);
  int fence_fd_tl3_1 = timeline3->CreateFence(kFenceName, 1);
  int fence_fd_tl3_2 = timeline3->CreateFence(kFenceName, 2);
  int fence_fd_tl3_3 = timeline3->CreateFence(kFenceName, 3);

  EXPECT_EQ(3U, GetMapEntryCount(timeline1));
  file_system_->close(fence_fd_tl1_1);
  EXPECT_EQ(2U, GetMapEntryCount(timeline1));
  file_system_->close(fence_fd_tl1_2);
  EXPECT_EQ(1U, GetMapEntryCount(timeline1));
  file_system_->close(fence_fd_tl1_3);
  EXPECT_EQ(0U, GetMapEntryCount(timeline1));

  EXPECT_EQ(3U, GetMapEntryCount(timeline2));
  file_system_->close(fence_fd_tl2_1);
  EXPECT_EQ(2U, GetMapEntryCount(timeline2));
  file_system_->close(fence_fd_tl2_2);
  EXPECT_EQ(1U, GetMapEntryCount(timeline2));
  file_system_->close(fence_fd_tl2_3);
  EXPECT_EQ(0U, GetMapEntryCount(timeline2));

  EXPECT_EQ(3U, GetMapEntryCount(timeline3));
  file_system_->close(fence_fd_tl3_1);
  EXPECT_EQ(2U, GetMapEntryCount(timeline3));
  file_system_->close(fence_fd_tl3_2);
  EXPECT_EQ(1U, GetMapEntryCount(timeline3));
  file_system_->close(fence_fd_tl3_3);
  EXPECT_EQ(0U, GetMapEntryCount(timeline3));
}

TEST_BACKGROUND_F(TimelineTest, CreateFence_AtPastPoint) {
  scoped_refptr<Timeline> timeline = new Timeline();
  timeline->IncrementCounter(10);

  int fence_fd = timeline->CreateFence(kFenceName, 5);
  EXPECT_TRUE(IsSignaled(fence_fd));
}

TEST_BACKGROUND_F(TimelineTest, IncrementCounterTest) {
  scoped_refptr<Timeline> timeline = new Timeline();

  int fence_fd1 = timeline->CreateFence(kFenceName, 2);
  int fence_fd2 = timeline->CreateFence(kFenceName, 5);

  EXPECT_EQ(0U, GetCounterValue(timeline));
  EXPECT_FALSE(IsSignaled(fence_fd1));

  timeline->IncrementCounter(1);
  EXPECT_EQ(1U, GetCounterValue(timeline));
  EXPECT_FALSE(IsSignaled(fence_fd1));
  EXPECT_FALSE(IsSignaled(fence_fd2));

  timeline->IncrementCounter(2);
  EXPECT_EQ(3U, GetCounterValue(timeline));
  EXPECT_TRUE(IsSignaled(fence_fd1));
  EXPECT_FALSE(IsSignaled(fence_fd2));

  timeline->IncrementCounter(3);
  EXPECT_EQ(6U, GetCounterValue(timeline));
  EXPECT_TRUE(IsSignaled(fence_fd1));
  EXPECT_TRUE(IsSignaled(fence_fd2));
}

TEST_BACKGROUND_F(TimelineTest, Threaded_AttachRemoveTest) {
  scoped_refptr<Timeline> timeline = new Timeline();

  // Increment counter to 200 for testing of past sync points.
  const size_t kInitialTimelineCounter = 50U;
  timeline->IncrementCounter(kInitialTimelineCounter);

  base::WaitableEvent event(true /* manual reset */, false /* Not signaled */);

  // Increment 100 with 5 threads.
  const size_t kIncrementorCount = 5U;
  ScopedVector<ThreadedIncrementor> incrementor;
  incrementor.resize(kIncrementorCount);
  const size_t kIncrementCountPerThread = 20U;
  const size_t kFinalTimelineCounter = kInitialTimelineCounter +
      kIncrementorCount * kIncrementCountPerThread;
  for (size_t i = 0; i < kIncrementorCount; ++i) {
    incrementor[i] = new ThreadedIncrementor(
        timeline, 1, kIncrementCountPerThread, &event);
    incrementor[i]->Start();
  }

  // The permanent fences won't be removed.
  const size_t kPermanentFenceCount = 200U;
  std::vector<int> permanent_fence_fds(kPermanentFenceCount);
  for (size_t i = 0; i < kPermanentFenceCount; ++i) {
    permanent_fence_fds[i] = timeline->CreateFence(kFenceName, i);
    ASSERT_LE(0, permanent_fence_fds[i]);
  }

  // Merge 200 sync points in 5 threads.
  const size_t kMergerCount = 5U;
  const size_t kMergeSyncPointCountPerThread = 4U;
  ScopedVector<ThreadedMerger> mergers;
  mergers.resize(kMergerCount);
  size_t kMergedNotSignaledSyncPointCount = 0;
  for (size_t i = 0; i < kMergerCount; ++i) {
    std::vector<int> fd1(kMergeSyncPointCountPerThread);
    std::vector<int> fd2(kMergeSyncPointCountPerThread);

    const size_t kChunkMax = (i + 1) * kMergeSyncPointCountPerThread - 1;
    const size_t kChunkMin = i * kMergeSyncPointCountPerThread;
    for (size_t j = 0; j < kMergeSyncPointCountPerThread; ++j) {
      fd1[j] = permanent_fence_fds[kChunkMax - j];
      fd2[j] = permanent_fence_fds[kChunkMin + j];
      if (kChunkMax - j > kFinalTimelineCounter ||
          kChunkMin + j > kFinalTimelineCounter) {
        kMergedNotSignaledSyncPointCount++;
      }
    }

    mergers[i] = new ThreadedMerger(fd1, fd2, &event);
    mergers[i]->Start();
  }

  // Remove 200 sync points in 5 threads.
  const size_t kRemoverCount = 5U;
  ScopedVector<ThreadedRemover> removers;
  removers.resize(kRemoverCount);

  const size_t kRemoveSyncPointCountPerThread = 40U;
  for (size_t i = 0; i < kRemoverCount; ++i) {
    std::vector<int> fences_to_be_removed(kRemoveSyncPointCountPerThread);
    for (size_t j = 0; j < kRemoveSyncPointCountPerThread; ++j) {
      fences_to_be_removed[j] =
          timeline->CreateFence(kFenceName, j * kRemoverCount + i);
      ASSERT_LE(0, fences_to_be_removed[j]);
    }
    removers[i] = new ThreadedRemover(fences_to_be_removed, &event);
    removers[i]->Start();
  }

  // Attach 200 sync points in 5 threads.
  const size_t kAttahcerCount = 5U;
  ScopedVector<ThreadedAttacher> attachers;
  attachers.resize(kAttahcerCount);

  const size_t kAttachSyncPointCountPerThread = 40U;
  for (size_t i = 0; i < kAttahcerCount; ++i) {
    attachers[i] = new ThreadedAttacher(
        timeline, i, kAttahcerCount, kAttachSyncPointCountPerThread, &event);
    attachers[i]->Start();
  }

  EXPECT_EQ(
      kPermanentFenceCount + kRemoverCount * kRemoveSyncPointCountPerThread,
      GetMapEntryCount(timeline));

  event.Signal();  // Wake up all threads.

  // Join all threads.
  for (size_t i = 0; i < kRemoverCount; ++i) {
    removers[i]->Join();
  }
  for (size_t i = 0; i < kAttahcerCount; ++i) {
    attachers[i]->Join();
  }
  for (size_t i = 0; i < kIncrementorCount; ++i) {
    incrementor[i]->Join();
  }
  for (size_t i = 0; i < kMergerCount; ++i) {
    mergers[i]->Join();
  }

  EXPECT_EQ(
      kPermanentFenceCount + kAttahcerCount * kAttachSyncPointCountPerThread +
      kMergerCount * kMergeSyncPointCountPerThread,
      GetMapEntryCount(timeline));

  EXPECT_EQ(kFinalTimelineCounter, GetCounterValue(timeline));

  base::AutoLock lock(GetMutex(timeline));
  std::multimap<uint32_t, SyncPoint*>* sync_points =
      GetInternalMapLocked(timeline);

  // All sync points on [0, 400] must be signaled.
  uint32_t signaled_count = 0;
  std::multimap<uint32_t, SyncPoint*>::iterator signaled_lower =
      sync_points->lower_bound(0);
  std::multimap<uint32_t, SyncPoint*>::iterator signaled_upper =
      sync_points->upper_bound(kFinalTimelineCounter);
  for (std::multimap<uint32_t, SyncPoint*>::iterator it = signaled_lower;
       it != signaled_upper; ++it) {
    EXPECT_TRUE(it->second->IsSignaled());
    signaled_count++;
  }
  // Add 1U because the sync point whose signaling_time is 400 is fired.
  const size_t kPermanentSyncPointSignaledCount = kFinalTimelineCounter + 1U;
  const size_t kAddedSyncPointSignaledCount = kFinalTimelineCounter + 1U;
  const size_t kMergedSyncPoitnSignaledCount =
      kMergerCount * kMergeSyncPointCountPerThread -
      kMergedNotSignaledSyncPointCount;
  EXPECT_EQ(kPermanentSyncPointSignaledCount + kAddedSyncPointSignaledCount +
            kMergedSyncPoitnSignaledCount, signaled_count);

  // All sync points on (400,] must not be signaled.
  uint32_t non_signaled_count = 0;
  std::multimap<uint32_t, SyncPoint*>::iterator not_signaled_lower =
      sync_points->lower_bound(kFinalTimelineCounter + 1);
  for (std::multimap<uint32_t, SyncPoint*>::iterator it = not_signaled_lower;
       it != sync_points->end(); ++it) {
    EXPECT_FALSE(it->second->IsSignaled());
    non_signaled_count++;
  }

  const size_t kPermanentSyncPointNotSignaledCount =
      kPermanentFenceCount - kPermanentSyncPointSignaledCount;
  const size_t kAddedSyncPointNotSignaledCount =
      kAttahcerCount * kAttachSyncPointCountPerThread -
      kAddedSyncPointSignaledCount;

  EXPECT_EQ(
      kPermanentSyncPointNotSignaledCount + kAddedSyncPointNotSignaledCount +
      kMergedNotSignaledSyncPointCount, non_signaled_count);
}

TEST_BACKGROUND_F(TimelineTest, Threaded_IncrementCounterTest) {
  scoped_refptr<Timeline> timeline = new Timeline();
  int fence_fd = timeline->CreateFence(kFenceName, 500);

  EXPECT_EQ(0U, GetCounterValue(timeline));

  base::WaitableEvent event(true /* manual reset */, false /* Not signaled */);

  const size_t kThreadCount = 20U;
  ScopedVector<ThreadedIncrementor> incrementor;
  incrementor.resize(kThreadCount);
  for (size_t i = 0; i < kThreadCount; ++i) {
    // Totally increments 1000 each.
    incrementor[i] = new ThreadedIncrementor(timeline, 10, 100, &event);
    incrementor[i]->Start();
  }

  event.Signal();  // Wake up all incrementor threads.

  for (size_t i = 0; i < kThreadCount; ++i) {
    incrementor[i]->Join();
  }

  EXPECT_EQ(20000U, GetCounterValue(timeline));
  EXPECT_TRUE(IsSignaled(fence_fd));
}

class FenceStreamTest
    : public FileSystemBackgroundTestCommon<FenceStreamTest> {
 public:
  DECLARE_BACKGROUND_TEST(ConstructDestruct);
  DECLARE_BACKGROUND_TEST(Unknown_Ioctl);
  DECLARE_BACKGROUND_TEST(CloseFence);
  DECLARE_BACKGROUND_TEST(CloseFence_DuringWait);
  DECLARE_BACKGROUND_TEST(SpuriousWakeup_ForeverWait);
  DECLARE_BACKGROUND_TEST(SpuriousWakeup_TimedWait);
  DECLARE_BACKGROUND_TEST(SYNC_IOC_WAIT_WithNullTimeout);
  DECLARE_BACKGROUND_TEST(SYNC_IOC_WAIT_Timeout);
  DECLARE_BACKGROUND_TEST(SYNC_IOC_WAIT_Timeout0ms);
  DECLARE_BACKGROUND_TEST(SYNC_IOC_WAIT_Threaded);
  DECLARE_BACKGROUND_TEST(SYNC_IOC_WAIT_Threaded_Forever);
  DECLARE_BACKGROUND_TEST(SYNC_IOC_FENCE_INFO_WithNullArg);
  DECLARE_BACKGROUND_TEST(SYNC_IOC_FENCE_INFO_WithTooSmallSize);
  DECLARE_BACKGROUND_TEST(SYNC_IOC_FENCE_INFO_Normal);
  DECLARE_BACKGROUND_TEST(SYNC_IOC_FENCE_INFO_NoMemory);
  DECLARE_BACKGROUND_TEST(SYNC_IOC_MERGE_SameBackend);
  DECLARE_BACKGROUND_TEST(SYNC_IOC_MERGE_SingleTimeline);
  DECLARE_BACKGROUND_TEST(SYNC_IOC_MERGE_MultiTimeline);
  DECLARE_BACKGROUND_TEST(SYNC_IOC_MERGE_DuringWait);
  DECLARE_BACKGROUND_TEST(SYNC_IOC_MERGE_ThreadedMerge);

 protected:
  virtual void SetUp() OVERRIDE {
    FileSystemBackgroundTestCommon<FenceStreamTest>::SetUp();
    timeline_ = new TestableTimeline();
  }

  virtual void TearDown() OVERRIDE {
    timeline_ = NULL;
    FileSystemBackgroundTestCommon<FenceStreamTest>::TearDown();
  }

  SyncPoint* GetSyncPoint(int fence_fd, uint32_t index) {
    base::AutoLock lock(file_system_->mutex());
    scoped_refptr<FenceStream> fence = GetFenceStreamLocked(fence_fd);
    EXPECT_LT(index, fence->sync_points_.size());
    SyncPoint* result = fence->sync_points_[index]->sync_point.get();
    ALOG_ASSERT(result);
    return result;
  }

  void EmulateSpuriousWakeup(int fence_fd) {
    base::AutoLock lock(file_system_->mutex());
    scoped_refptr<FenceStream> fence = GetFenceStreamLocked(fence_fd);
    fence->fence_cond_.Broadcast();
  }

  int Ioctl(int fd, int request, ...) {
    va_list ap;
    va_start(ap, request);
    int r = file_system_->ioctl(fd, request, ap);
    va_end(ap);
    return r;
  }

  sync_fence_info_data* AllocateFenceInfoDataBuffer(
      std::vector<uint8_t>* buffer, size_t size) {
    buffer->resize(size);
    sync_fence_info_data* info =
        reinterpret_cast<sync_fence_info_data*>(&(*buffer)[0]);
    info->len = size;
    return info;
  }

  scoped_refptr<Timeline> GetTimeline(int fence_fd, SyncPoint* sp) {
    base::AutoLock lock(file_system_->mutex());
    scoped_refptr<FenceStream> fence = GetFenceStreamLocked(fence_fd);
    for (size_t i = 0; i < fence->sync_points_.size(); ++i) {
      if (fence->sync_points_[i]->sync_point == sp)
        return fence->sync_points_[i]->timeline;
    }
    return NULL;
  }

 private:
  scoped_refptr<FenceStream> GetFenceStreamLocked(int fence_fd) {
    return static_cast<FenceStream*>(
        file_system_->GetStreamLocked(fence_fd).get());
  }

  scoped_refptr<FenceStream> GetFenceStream(int fence_fd) {
    base::AutoLock lock(file_system_->mutex());
    return GetFenceStreamLocked(fence_fd);
  }

  scoped_refptr<TestableTimeline> timeline_;
};

TEST_BACKGROUND_F(FenceStreamTest, ConstructDestruct) {
  ScopedVector<FenceStream::SyncPointTimeline> sync_points;
  scoped_refptr<FenceStream> fence =
      new FenceStream(kFenceName, sync_points.Pass());
}

TEST_BACKGROUND_F(FenceStreamTest, Unknown_Ioctl) {
  // Unknown ioctl request to sync driver FD returns ENOTTY on Linux.
  int fence_fd = timeline_->CreateFence(kFenceName, 1);
  errno = 0;
  EXPECT_EQ(-1, Ioctl(fence_fd, FIONREAD));
  EXPECT_EQ(ENOTTY, errno);
}

TEST_BACKGROUND_F(FenceStreamTest, CloseFence) {
  int fence_fd = timeline_->CreateFence(kFenceName, 1);

  EXPECT_TRUE(timeline_->HasSyncPointAt(1));
  file_system_->close(fence_fd);
  // After FenceStream destruction, the attached fence points should be release
  // from timeline.
  EXPECT_FALSE(timeline_->HasSyncPointAt(1));
}

TEST_BACKGROUND_F(FenceStreamTest, CloseFence_DuringWait) {
  int fence_fd = timeline_->CreateFence(kFenceName, 1);

  ThreadedWaiter waiter(fence_fd, kDefaultTimeoutInMs);
  waiter.StartAndBlockUntilReady();

  EXPECT_EQ(0, file_system_->close(fence_fd));

  // Even after close the file descriptor, the already waiting thread keeps
  // waiting and fence stream is still alive. This is compatible with upstream
  // Linux Kernel behavior.
  timeline_->IncrementCounter(1);
  waiter.Join();
  EXPECT_EQ(0, waiter.result());

  // After FenceStream destruction, the attached fence points should be release
  // from timeline.
  EXPECT_FALSE(timeline_->HasSyncPointAt(1));
}

TEST_BACKGROUND_F(FenceStreamTest, SpuriousWakeup_ForeverWait) {
  int fence_fd = timeline_->CreateFence(kFenceName, 1);
  ThreadedWaiter waiter(fence_fd, -1 /* Never timeout. */);
  waiter.StartAndBlockUntilReady();

  EmulateSpuriousWakeup(fence_fd);

  EXPECT_TRUE(waiter.IsWaiting());

  timeline_->IncrementCounter(1);
  waiter.Join();
  EXPECT_EQ(0, waiter.result());
  EXPECT_EQ(0, file_system_->close(fence_fd));
}

TEST_BACKGROUND_F(FenceStreamTest, SpuriousWakeup_TimedWait) {
  int fence_fd = timeline_->CreateFence(kFenceName, 1);
  ThreadedWaiter waiter(fence_fd, kDefaultTimeoutInMs);
  waiter.StartAndBlockUntilReady();

  EmulateSpuriousWakeup(fence_fd);

  EXPECT_TRUE(waiter.IsWaiting());

  timeline_->IncrementCounter(1);
  waiter.Join();
  EXPECT_EQ(0, waiter.result());
  EXPECT_EQ(0, file_system_->close(fence_fd));
}

TEST_BACKGROUND_F(FenceStreamTest, SYNC_IOC_WAIT_WithNullTimeout) {
  int fence_fd = timeline_->CreateFence(kFenceName, 1);
  // If we pass NULL timeout, we always return EFAULT.
  errno = 0;
  EXPECT_EQ(-1, Ioctl(fence_fd, SYNC_IOC_WAIT, NULL));
  EXPECT_EQ(EFAULT, errno);

  EXPECT_EQ(0, file_system_->close(fence_fd));
  fence_fd = timeline_->CreateFence(kFenceName, 1);
  errno = 0;
  EXPECT_EQ(-1, Ioctl(fence_fd, SYNC_IOC_WAIT, NULL));
  EXPECT_EQ(EFAULT, errno);

  // Signaled sync point.
  timeline_->IncrementCounter(1);
  errno = 0;
  EXPECT_EQ(-1, Ioctl(fence_fd, SYNC_IOC_WAIT, NULL));
  EXPECT_EQ(EFAULT, errno);

  errno = 0;
  EXPECT_EQ(0, file_system_->close(fence_fd));
  EXPECT_EQ(0, errno);
}

TEST_BACKGROUND_F(FenceStreamTest, SYNC_IOC_WAIT_Timeout) {
  int fence_fd = timeline_->CreateFence(kFenceName, 1);
  int timeout = 20;  // 20 millisec.
  errno = 0;
  ASSERT_NE(0, Ioctl(fence_fd, SYNC_IOC_WAIT, &timeout));
  EXPECT_EQ(ETIME, errno);
  errno = 0;
  EXPECT_EQ(0, file_system_->close(fence_fd));
  EXPECT_EQ(0, errno);
}

TEST_BACKGROUND_F(FenceStreamTest, SYNC_IOC_WAIT_Timeout0ms) {
  int fence_fd = timeline_->CreateFence(kFenceName, 1);
  int timeout = 0;
  errno = 0;
  ASSERT_NE(0, Ioctl(fence_fd, SYNC_IOC_WAIT, &timeout));
  EXPECT_EQ(ETIME, errno);
  errno = 0;
  EXPECT_EQ(0, file_system_->close(fence_fd));
  EXPECT_EQ(0, errno);
}

TEST_BACKGROUND_F(FenceStreamTest, SYNC_IOC_WAIT_Threaded) {
  // This test verifies signaling sync point will certainly wake up the waiting
  // thread.
  int fence_fd = timeline_->CreateFence(kFenceName, 1);

  ThreadedWaiter waiter(fence_fd, kDefaultTimeoutInMs);
  waiter.StartAndBlockUntilReady();

  EXPECT_TRUE(waiter.IsWaiting());

  timeline_->IncrementCounter(1);
  waiter.Join();
  EXPECT_EQ(0, waiter.result());
  errno = 0;
  EXPECT_EQ(0, file_system_->close(fence_fd));
  EXPECT_EQ(0, errno);
}

TEST_BACKGROUND_F(FenceStreamTest, SYNC_IOC_WAIT_Threaded_Forever) {
  // This test verifies signaling sync point will certainly wake up the waiting
  // thread.
  int fence_fd = timeline_->CreateFence(kFenceName, 1);

  ThreadedWaiter waiter(fence_fd, -2 /* Indefinitely waiting */);
  waiter.StartAndBlockUntilReady();

  EXPECT_TRUE(waiter.IsWaiting());

  timeline_->IncrementCounter(1);
  waiter.Join();
  EXPECT_EQ(0, waiter.result());
  errno = 0;
  EXPECT_EQ(0, file_system_->close(fence_fd));
  EXPECT_EQ(0, errno);
}

TEST_BACKGROUND_F(FenceStreamTest, SYNC_IOC_FENCE_INFO_WithNullArg) {
  int fence_fd = timeline_->CreateFence(kFenceName, 1);
  errno = 0;
  EXPECT_EQ(-1, Ioctl(fence_fd, SYNC_IOC_FENCE_INFO, NULL));
  EXPECT_EQ(EFAULT, errno);
  errno = 0;
  EXPECT_EQ(0, file_system_->close(fence_fd));
  EXPECT_EQ(0, errno);
}

TEST_BACKGROUND_F(FenceStreamTest,
                  SYNC_IOC_FENCE_INFO_WithTooSmallSize) {
  std::vector<uint8_t> buffer;
  sync_fence_info_data* info = AllocateFenceInfoDataBuffer(
      &buffer, sizeof(sync_fence_info_data) - 1);

  int fence_fd = timeline_->CreateFence(kFenceName, 1);
  errno = 0;
  EXPECT_EQ(-1, Ioctl(fence_fd, SYNC_IOC_FENCE_INFO, info));
  EXPECT_EQ(EINVAL, errno);
  errno = 0;
  EXPECT_EQ(0, file_system_->close(fence_fd));
  EXPECT_EQ(0, errno);
}

TEST_BACKGROUND_F(FenceStreamTest, SYNC_IOC_FENCE_INFO_Normal) {
  std::vector<uint8_t> buffer;
  sync_fence_info_data* info = AllocateFenceInfoDataBuffer(
      &buffer, sizeof(sync_fence_info_data) + sizeof(sync_pt_info));

  int fence_fd = timeline_->CreateFence(kFenceName, 1);
  errno = 0;
  EXPECT_EQ(0, Ioctl(fence_fd, SYNC_IOC_FENCE_INFO, info));
  EXPECT_EQ(0, errno);

  EXPECT_EQ(sizeof(sync_fence_info_data) + sizeof(sync_pt_info), info->len);
  EXPECT_EQ(1U, SyncPtInfoCount(info));
  EXPECT_STREQ(kFenceName, info->name);
  EXPECT_EQ(0, info->status);

  const sync_pt_info* pt_info = GetSyncPtInfo(info, 0);

  ASSERT_TRUE(pt_info);
  EXPECT_EQ(sizeof(sync_pt_info), pt_info->len);
  EXPECT_STREQ(kTimelineName, pt_info->obj_name);
  EXPECT_STREQ(kDriverName, pt_info->driver_name);

  errno = 0;
  EXPECT_EQ(0, file_system_->close(fence_fd));
  EXPECT_EQ(0, errno);
}

TEST_BACKGROUND_F(FenceStreamTest, SYNC_IOC_FENCE_INFO_NoMemory) {
  std::vector<uint8_t> buffer;
  sync_fence_info_data* info = AllocateFenceInfoDataBuffer(
      &buffer, sizeof(sync_fence_info_data));

  int fence_fd = timeline_->CreateFence(kFenceName, 1);

  errno = 0;
  EXPECT_EQ(-1, Ioctl(fence_fd, SYNC_IOC_FENCE_INFO, info));
  EXPECT_EQ(ENOMEM, errno);

  errno = 0;
  EXPECT_EQ(0, file_system_->close(fence_fd));
  EXPECT_EQ(0, errno);
}

TEST_BACKGROUND_F(FenceStreamTest, SYNC_IOC_MERGE_SameBackend) {
  int fence_fd1 = timeline_->CreateFence(kFenceName, 1);
  int fence_fd2 = file_system_->dup(fence_fd1);

  EXPECT_NE(fence_fd1, fence_fd2);
  sync_merge_data merge_data = {};
  merge_data.fd2 = fence_fd2;
  base::strlcpy(merge_data.name, kFenceName, sizeof(merge_data.name));
  errno = 0;
  EXPECT_EQ(0, Ioctl(fence_fd1, SYNC_IOC_MERGE, &merge_data));
  EXPECT_EQ(0, errno);
  int merged_fence_fd = merge_data.fence;
  ASSERT_TRUE(merged_fence_fd);
  EXPECT_NE(merged_fence_fd, fence_fd1);
  EXPECT_NE(merged_fence_fd, fence_fd2);

  std::vector<uint8_t> buffer;
  sync_fence_info_data* info = AllocateFenceInfoDataBuffer(
      &buffer, sizeof(sync_fence_info_data) + sizeof(sync_pt_info) * 2);

  errno = 0;
  EXPECT_EQ(0, Ioctl(merged_fence_fd, SYNC_IOC_FENCE_INFO, info));
  EXPECT_EQ(0, errno);
  EXPECT_EQ(1U, SyncPtInfoCount(info));

  SyncPoint* pt = GetSyncPoint(merged_fence_fd, 0);
  EXPECT_EQ(1U, pt->signaling_time());

  errno = 0;
  EXPECT_EQ(0, file_system_->close(fence_fd1));
  EXPECT_EQ(0, errno);
  EXPECT_EQ(0, file_system_->close(fence_fd2));
  EXPECT_EQ(0, errno);
  EXPECT_EQ(0, file_system_->close(merged_fence_fd));
  EXPECT_EQ(0, errno);
}

TEST_BACKGROUND_F(FenceStreamTest, SYNC_IOC_MERGE_SingleTimeline) {
  int fence_fd1 = timeline_->CreateFence(kFenceName, 1);
  int fence_fd2 = timeline_->CreateFence(kFenceName, 2);

  sync_merge_data merge_data = {};
  merge_data.fd2 = fence_fd2;
  base::strlcpy(merge_data.name, kFenceName, sizeof(merge_data.name));
  errno = 0;
  EXPECT_EQ(0, Ioctl(fence_fd1, SYNC_IOC_MERGE, &merge_data));
  EXPECT_EQ(0, errno);
  int merged_fence_fd = merge_data.fence;
  ASSERT_TRUE(merged_fence_fd);

  std::vector<uint8_t> buffer;
  sync_fence_info_data* info = AllocateFenceInfoDataBuffer(
      &buffer, sizeof(sync_fence_info_data) + sizeof(sync_pt_info) * 2);

  errno = 0;
  EXPECT_EQ(0, Ioctl(merged_fence_fd, SYNC_IOC_FENCE_INFO, info));
  EXPECT_EQ(0, errno);
  EXPECT_EQ(1U, SyncPtInfoCount(info));

  // Only future point should be remained.
  SyncPoint* pt = GetSyncPoint(merged_fence_fd, 0);
  EXPECT_EQ(2U, pt->signaling_time());

  errno = 0;
  EXPECT_EQ(0, file_system_->close(fence_fd1));
  EXPECT_EQ(0, errno);
  EXPECT_EQ(0, file_system_->close(fence_fd2));
  EXPECT_EQ(0, errno);
  EXPECT_EQ(0, file_system_->close(merged_fence_fd));
  EXPECT_EQ(0, errno);
}

TEST_BACKGROUND_F(FenceStreamTest, SYNC_IOC_MERGE_MultiTimeline) {
  scoped_refptr<TestableTimeline> timeline2 = new TestableTimeline();

  int fence_fd1 = timeline_->CreateFence(kFenceName, 1);
  int fence_fd2 = timeline2->CreateFence(kFenceName, 2);

  sync_merge_data merge_data = {};
  merge_data.fd2 = fence_fd2;
  base::strlcpy(merge_data.name, kFenceName, sizeof(merge_data.name));
  errno = 0;
  EXPECT_EQ(0, Ioctl(fence_fd1, SYNC_IOC_MERGE, &merge_data));
  EXPECT_EQ(0, errno);
  int merged_fence = merge_data.fence;

  std::vector<uint8_t> buffer;
  sync_fence_info_data* info = AllocateFenceInfoDataBuffer(
      &buffer, sizeof(sync_fence_info_data) + sizeof(sync_pt_info) * 2);

  errno = 0;
  EXPECT_EQ(0, Ioctl(merged_fence, SYNC_IOC_FENCE_INFO, info));
  EXPECT_EQ(0, errno);
  EXPECT_EQ(2U, SyncPtInfoCount(info));

  SyncPoint* tl1_pt = GetSyncPoint(merged_fence, 0);
  SyncPoint* tl2_pt = GetSyncPoint(merged_fence, 1);
  // The order of the sync point is not specified.
  if (GetTimeline(merged_fence, tl1_pt) != timeline_)
    std::swap(tl1_pt, tl2_pt);

  EXPECT_EQ(1U, tl1_pt->signaling_time());
  EXPECT_EQ(2U, tl2_pt->signaling_time());

  errno = 0;
  EXPECT_EQ(0, file_system_->close(fence_fd1));
  EXPECT_EQ(0, errno);
  EXPECT_EQ(0, file_system_->close(fence_fd2));
  EXPECT_EQ(0, errno);
  EXPECT_EQ(0, file_system_->close(merged_fence));
  EXPECT_EQ(0, errno);
}

TEST_BACKGROUND_F(FenceStreamTest, SYNC_IOC_MERGE_DuringWait) {
  int fence_fd1 = timeline_->CreateFence(kFenceName, 1);
  int fence_fd2 = timeline_->CreateFence(kFenceName, 2);

  ThreadedWaiter waiter1(fence_fd1, kDefaultTimeoutInMs);
  ThreadedWaiter waiter2(fence_fd2, kDefaultTimeoutInMs);
  waiter1.StartAndBlockUntilReady();
  waiter2.StartAndBlockUntilReady();

  sync_merge_data merge_data = {};
  merge_data.fd2 = fence_fd2;
  base::strlcpy(merge_data.name, kFenceName, sizeof(merge_data.name));
  int request = SYNC_IOC_MERGE;
  errno = 0;
  EXPECT_EQ(0, Ioctl(fence_fd1, request, &merge_data));
  EXPECT_EQ(0, errno);
  int merged_fence = merge_data.fence;

  std::vector<uint8_t> buffer;
  sync_fence_info_data* info = AllocateFenceInfoDataBuffer(
      &buffer, sizeof(sync_fence_info_data) + sizeof(sync_pt_info) * 2);

  errno = 0;
  EXPECT_EQ(0, Ioctl(merged_fence, SYNC_IOC_FENCE_INFO, info));
  EXPECT_EQ(0, errno);
  EXPECT_EQ(1U, SyncPtInfoCount(info));

  // Only future point should be remained.
  SyncPoint* pt = GetSyncPoint(merged_fence, 0);
  EXPECT_EQ(2U, pt->signaling_time());

  timeline_->IncrementCounter(10000);
  waiter1.Join();
  waiter2.Join();
  EXPECT_EQ(0, waiter1.result());
  EXPECT_EQ(0, waiter2.result());
  errno = 0;
  EXPECT_EQ(0, file_system_->close(fence_fd1));
  EXPECT_EQ(0, errno);
  EXPECT_EQ(0, file_system_->close(fence_fd2));
  EXPECT_EQ(0, errno);
  EXPECT_EQ(0, file_system_->close(merged_fence));
  EXPECT_EQ(0, errno);
}

TEST_BACKGROUND_F(FenceStreamTest, SYNC_IOC_MERGE_ThreadedMerge) {
  scoped_refptr<TestableTimeline> timeline2 = new TestableTimeline();

  int fence_fd1 = timeline_->CreateFence(kFenceName, 1);
  int fence_fd2 = timeline2->CreateFence(kFenceName, 2);

  base::WaitableEvent event(true /* manual reset */, false /* Not signaled */);

  const size_t kThreadCount = 10U;
  ScopedVector<ThreadedMerger> mergers;
  mergers.resize(kThreadCount);
  for (size_t i = 0; i < kThreadCount; ++i) {
    std::vector<int> fd1(1);
    fd1[0] = fence_fd1;
    std::vector<int> fd2(1);
    fd2[0] = fence_fd2;
    mergers[i] = new ThreadedMerger(fd1, fd2, &event);
    mergers[i]->Start();
  }

  event.Signal();  // Wake up all merger threads.

  for (size_t i = 0; i < kThreadCount; ++i) {
    mergers[i]->Join();

    int merged_fence_fd = mergers[i]->GetMergedFenceFd(0);

    std::vector<uint8_t> buffer;
    sync_fence_info_data* info = AllocateFenceInfoDataBuffer(
        &buffer, sizeof(sync_fence_info_data) + sizeof(sync_pt_info) * 2);

    errno = 0;
    EXPECT_EQ(0, Ioctl(merged_fence_fd, SYNC_IOC_FENCE_INFO, info));
    EXPECT_EQ(0, errno);
    EXPECT_EQ(2U, SyncPtInfoCount(info));

    SyncPoint* tl1_pt = GetSyncPoint(merged_fence_fd, 0);
    SyncPoint* tl2_pt = GetSyncPoint(merged_fence_fd, 1);

    // The order of the sync point is not specified.
    if (GetTimeline(merged_fence_fd, tl1_pt) != timeline_)
      std::swap(tl1_pt, tl2_pt);

    EXPECT_EQ(1U, tl1_pt->signaling_time());
    EXPECT_EQ(2U, tl2_pt->signaling_time());

    errno = 0;
    EXPECT_EQ(0, file_system_->close(merged_fence_fd));
    EXPECT_EQ(0, errno);
  }
  errno = 0;
  EXPECT_EQ(0, file_system_->close(fence_fd1));
  EXPECT_EQ(0, errno);
  EXPECT_EQ(0, file_system_->close(fence_fd2));
  EXPECT_EQ(0, errno);
}

}  // namespace posix_translation
