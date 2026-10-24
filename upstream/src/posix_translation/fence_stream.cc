// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// You can find the Linux Kernel implementation at:
// http://git.kernel.org/cgit/linux/kernel/git/torvalds/linux.git/tree/drivers/staging/android/sync.c

#include "posix_translation/fence_stream.h"

#include <linux/sync.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

#include <limits>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include "base/strings/string_util.h"
#include "base/synchronization/condition_variable.h"
#include "base/synchronization/lock.h"
#include "base/time/time.h"
#include "common/arc_strace.h"
#include "common/process_emulator.h"
#include "posix_translation/statfs.h"
#include "posix_translation/time_util.h"
#include "posix_translation/virtual_file_system.h"

namespace posix_translation {

// This source code contains four types of locks, file system lock,
// timeline locks, fence stream locks and sync point locks. The hierarchy of
// these locks is
//   file system lock > timeline locks > fence stream locks > sync point locks.
// This ">" means the larger lock will not be newly acquired while the smaller
// lock is acquired. And any two locks in the same layer will not nest. Here,
// file system lock only protects the ref count of fence stream, so it is safe
// to unlock the file system lock if needed only before acquiring other locks.
//
// To keep above hierarchy, any FenceStream instance calls Timeline's member
// methods without FenceStream's lock. On the other hand, any Timelines
// instance calls FenceStream's member methods with Timeline's lock.
// Any SyncPoint locks won't violate above hierarchy since SyncPoint lock is
// private and SyncPoint does not call any Fence or Timeline  member methods.

namespace {

base::Lock& GetFileSystemMutex() {
  return VirtualFileSystem::GetVirtualFileSystem()->mutex();
}

// Increments |counter_| during an instance of this class is alive.
class ScopedCountIncrementer {
 public:
  // Caller must free |counter_| after this instance is deleted.
  explicit ScopedCountIncrementer(uint32_t* counter) : counter_(counter) {
    ALOG_ASSERT(counter_);
    ++(*counter_);
  }

  ~ScopedCountIncrementer() {
    ALOG_ASSERT(*counter_);
    --(*counter_);
  }

 private:
  uint32_t* counter_;
};

}  // namespace

Timeline::Timeline() : counter_(0) {
}

Timeline::~Timeline() {
}

int Timeline::CreateFence(const std::string& name, uint32_t signaling_time) {
  scoped_ptr<SyncPoint> sp(new SyncPoint(signaling_time, 0ULL));
  ScopedVector<FenceStream::SyncPointTimeline> sync_points;
  sync_points.push_back(new FenceStream::SyncPointTimeline(sp.Pass(), this));

  base::AutoLock vfs_lock(GetFileSystemMutex());
  base::AutoLock lock(mutex_);
  const int fd = VirtualFileSystem::GetVirtualFileSystem()->AddFileStreamLocked(
      FenceStream::CreateFenceTimelineLocked(name, sync_points.Pass()));
  ALOG_ASSERT(fd >= 0);
  ARC_STRACE_REGISTER_FD(fd, name.c_str());
  return fd;
}

void Timeline::IncrementCounter(uint32_t amount) {
  base::AutoLock lock(mutex_);

  ALOG_ASSERT(counter_ < (std::numeric_limits<uint32_t>::max() - amount),
              "Timeline counter overflow.");

  // Find sync points which shall signal in (counter_, counter_+amount].
  std::multimap<uint32_t, SyncPoint*>::iterator lower =
      sync_points_.lower_bound(counter_ + 1);
  std::multimap<uint32_t, SyncPoint*>::iterator upper =
      sync_points_.upper_bound(counter_ + amount);

  counter_ += amount;

  for (std::multimap<uint32_t, SyncPoint*>::iterator it = lower;
       it != upper; ++it) {
    FenceStream* fence = sync_point_fence_[it->second];
    it->second->MarkAsSignaled();
    fence->MaybeSignal();
  }
}

void Timeline::AttachSyncPoint(FenceStream* fence, SyncPoint* pt) {
  base::AutoLock lock(mutex_);
  AttachSyncPointLocked(fence, pt);
}

void Timeline::AttachSyncPointLocked(FenceStream* fence, SyncPoint* pt) {
  mutex_.AssertAcquired();
  sync_points_.insert(std::make_pair(pt->signaling_time(), pt));
  sync_point_fence_.insert(std::make_pair(pt, fence));

  if (pt->IsSignaled())
    return;

  if (pt->signaling_time() <= counter_)
    pt->MarkAsSignaled();
}

void Timeline::DetachSyncPoint(SyncPoint* pt) {
  base::AutoLock lock(mutex_);
  size_t erased_count = sync_point_fence_.erase(pt);
  ALOG_ASSERT(1U == erased_count);
  std::pair<std::multimap<uint32_t, SyncPoint*>::iterator,
      std::multimap<uint32_t, SyncPoint*>::iterator> range =
          sync_points_.equal_range(pt->signaling_time());

  for (std::multimap<uint32_t, SyncPoint*>::iterator it = range.first;
       it != range.second; ++it) {
    if (it->second == pt) {
      sync_points_.erase(it);
      // We don't have same syncpoints in a timeline.
      return;
    }
  }
  ALOG_ASSERT(false, "Releasing not managed sync point.");
}

SyncPoint::SyncPoint(uint32_t signaling_time, uint64_t timestamp_ns)
    : timestamp_ns_(timestamp_ns), signaling_time_(signaling_time) {
}

SyncPoint::~SyncPoint() {
}

void SyncPoint::MarkAsSignaled() {
  base::AutoLock lock(mutex_);
  ALOG_ASSERT(timestamp_ns_ == 0ULL,
              "The sync point has already been signaled");
  timespec ts;
  int result = clock_gettime(CLOCK_MONOTONIC, &ts);
  ALOG_ASSERT(result == 0, "clock_gettime failed: errno=%d", errno);
  timestamp_ns_ = ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

bool SyncPoint::IsSignaled() {
  return timestamp_ns_ != 0ULL;
}

uint32_t SyncPoint::FillSyncPtInfo(sync_pt_info* info, uint32_t length) {
  base::AutoLock lock(mutex_);
  if (length < sizeof(sync_pt_info))
    return 0;

  info->len = sizeof(sync_pt_info);

  // On Linux, the timeline name is the command line name who creates this
  // timeline. Use "arc" instead here since Chrome v2 app does not have the
  // concept.
  base::strlcpy(info->obj_name, "arc", sizeof(info->obj_name));

  // The driver name is the same as the original Linux implementation.
  base::strlcpy(info->driver_name, "sw_sync", sizeof(info->driver_name));
  info->timestamp_ns = timestamp_ns_;
  // We fill no driver_data.
  return info->len;
}

uint64_t SyncPoint::timestamp_ns() {
  base::AutoLock lock(mutex_);
  return timestamp_ns_;
}

//------------------------------------------------------------------------------

FenceStream::FenceStream(const std::string& fence_name,
                         ScopedVector<SyncPointTimeline> sync_points)
    : FileStream(O_RDWR, ""), fence_name_(fence_name), status_(FENCE_ACTIVE),
      fence_cond_(&fence_mutex_), sync_points_(sync_points.Pass()),
      waiting_thread_count_for_testing_(0) {
  ALOG_ASSERT(fence_name.size() < sizeof(sync_fence_info_data().name),
              "The length of driver name must be less than %d bytes.",
              sizeof(sync_fence_info_data().name));
  set_permission(PermissionInfo(arc::kRootUid, true));
}

FenceStream::~FenceStream() {
  for (size_t i = 0; i < sync_points_.size(); ++i) {
    sync_points_[i]->timeline->DetachSyncPoint(
        sync_points_[i]->sync_point.get());
  }
}

// static
scoped_refptr<FenceStream> FenceStream::CreateFence(
    const std::string& fence_name,
    ScopedVector<SyncPointTimeline> sync_points) {
  scoped_refptr<FenceStream> fence(
      new FenceStream(fence_name, sync_points.Pass()));
  for (size_t i = 0; i < fence->sync_points_.size(); ++i) {
    fence->sync_points_[i]->timeline->AttachSyncPoint(
        fence, fence->sync_points_[i]->sync_point.get());
  }
  fence->MaybeSignal();
  return fence;
}

// static
scoped_refptr<FenceStream> FenceStream::CreateFenceTimelineLocked(
    const std::string& fence_name,
    ScopedVector<SyncPointTimeline> sync_points) {
  scoped_refptr<FenceStream> fence(
      new FenceStream(fence_name, sync_points.Pass()));
  for (size_t i = 0; i < fence->sync_points_.size(); ++i) {
    fence->sync_points_[i]->timeline->AttachSyncPointLocked(
        fence, fence->sync_points_[i]->sync_point.get());
  }
  fence->MaybeSignal();
  return fence;
}

ssize_t FenceStream::read(void* buf, size_t count) {
  errno = EINVAL;
  return -1;
}

ssize_t FenceStream::write(const void* buf, size_t count) {
  errno = EINVAL;
  return -1;
}

int FenceStream::ioctl(int request, va_list ap) {
  GetFileSystemMutex().AssertAcquired();

  // Unable to write switch-case since bionic ioctl.h enables _IOC_TYPECHECK
  // which is not allowed in constant expression.
  const unsigned int urequest = static_cast<unsigned int>(request);
  if (urequest == SYNC_IOC_WAIT) {
    return SyncIocWait(ap);
  } else if (urequest == SYNC_IOC_MERGE) {
    return SyncIocMerge(ap);
  } else if (urequest == SYNC_IOC_FENCE_INFO) {
    return SyncIocFenceInfo(ap);
  } else {
    errno = ENOTTY;
    return -1;
  }
}

const char* FenceStream::GetStreamType() const {
  return "fence";
}

void FenceStream::MaybeSignal() {
  base::AutoLock lock(fence_mutex_);
  MaybeSignalLocked();
}

void FenceStream::MaybeSignalLocked() {
  fence_mutex_.AssertAcquired();

  if (GetSignaledSyncPointCountLocked() < sync_points_.size())
    return;
  status_ = FENCE_SIGNALED;
  ALOG_ASSERT(IsValidLocked());
  fence_cond_.Broadcast();
}

int FenceStream::SyncIocWait(va_list ap) {
  const base::TimeTicks start(base::TimeTicks::Now());

  // To avoid dead-lock, need to release file system lock before fence lock
  // acquiring.
  base::AutoUnlock unlock(GetFileSystemMutex());
  base::AutoLock lock(fence_mutex_);
  ALOG_ASSERT(IsValidLocked());

  // |waiting_thread_count_for_testing_| must be incremented after the
  // |fence_mutex_| is acquired.
  ScopedCountIncrementer incrementor(&waiting_thread_count_for_testing_);

  int* timeout_pt = va_arg(ap, int*);
  if (!timeout_pt) {
    errno = EFAULT;
    return -1;
  }
  int timeout = *timeout_pt;

  if (sync_points_.empty()) {
    ALOGW("SYNC_IOC_WAIT is called for empty sync points.");
    return 0;
  }

  if (status_ == FENCE_SIGNALED)
    return 0;
  ALOG_ASSERT(status_ == FENCE_ACTIVE);

  // VirtualFileSystem::ioctl added the reference during this function call, so
  // no need to increment reference count here.

  // Negative timeout means the call can block indefinitely.
  const base::TimeTicks time_limit = timeout < 0 ?
      base::TimeTicks() : start + base::TimeDelta::FromMilliseconds(timeout);

  while (true) {
    const bool is_timeout = internal::WaitUntil(&fence_cond_, time_limit);
    ALOG_ASSERT(IsValidLocked());

    if (status_ == FENCE_SIGNALED)
      return 0;

    if (is_timeout) {
      ALOG_ASSERT(timeout >= 0);
      errno = ETIME;
      return -1;
    }
  }
  ALOG_ASSERT(false, "Must not be reached here.");
  return 0;
}

int FenceStream::SyncIocMerge(va_list ap) {
  GetFileSystemMutex().AssertAcquired();

  sync_merge_data* data = va_arg(ap, sync_merge_data*);
  if (!data) {
    errno = EFAULT;
    return -1;
  }

  VirtualFileSystem* vfs = VirtualFileSystem::GetVirtualFileSystem();

  scoped_refptr<FileStream> file_stream = vfs->GetStreamLocked(data->fd2);
  if (!file_stream ||
      strcmp(file_stream->GetStreamType(), GetStreamType()) != 0) {
    // Return ENOENT if the given FD is not a fence stream. This is compatible
    // with upstream implementation.
    errno = ENOENT;
    return -1;
  }

  // This downcast is safe since the stream check is passed above.
  scoped_refptr<FenceStream> other_fence_stream =
      static_cast<FenceStream*>(file_stream.get());
  if (this == other_fence_stream) {
    // Just return duped FD if the sync_ioc_merge is called for same stream.
    data->fence = vfs->DupLocked(data->fd2, -1);
    return 0;
  }

  // If sync points exist in a same timeline, use the latter one.
  std::map<Timeline*, SyncPoint*> timeline_syncpoint;
  for (size_t i = 0; i < sync_points_.size(); ++i) {
    timeline_syncpoint[sync_points_[i]->timeline] =
        sync_points_[i]->sync_point.get();
  }
  for (size_t i = 0; i < other_fence_stream->sync_points_.size(); ++i) {
    SyncPoint* pt = other_fence_stream->sync_points_[i]->sync_point.get();
    Timeline* tm = other_fence_stream->sync_points_[i]->timeline;
    std::pair<std::map<Timeline*, SyncPoint*>::iterator, bool> p =
        timeline_syncpoint.insert(std::make_pair(tm, pt));

    if (!p.second && p.first->second->signaling_time() < pt->signaling_time())
      p.first->second = pt;
  }

  ScopedVector<FenceStream::SyncPointTimeline> new_sync_points;
  for (std::map<Timeline*, SyncPoint*>::iterator it =
       timeline_syncpoint.begin(); it != timeline_syncpoint.end(); ++it) {
    scoped_ptr<SyncPoint> sp(new SyncPoint(it->second->signaling_time(),
                                           it->second->timestamp_ns()));
    new_sync_points.push_back(
        new FenceStream::SyncPointTimeline(sp.Pass(), it->first));
  }

  data->fence = vfs->AddFileStreamLocked(
      FenceStream::CreateFence(data->name, new_sync_points.Pass()));
  if (data->fence == -1) {
    errno = EMFILE;
    return -1;
  }
  ARC_STRACE_REGISTER_FD(data->fence, data->name);
  return 0;
}

int FenceStream::SyncIocFenceInfo(va_list ap) {
  base::AutoUnlock unlock(GetFileSystemMutex());
  base::AutoLock lock(fence_mutex_);
  ALOG_ASSERT(IsValidLocked());

  sync_fence_info_data* info = va_arg(ap, sync_fence_info_data*);
  if (!info) {
    errno = EFAULT;
    return -1;
  }

  if (info->len < sizeof(sync_fence_info_data)) {
    errno = EINVAL;
    return -1;
  }

  base::strlcpy(info->name, fence_name_.c_str(), sizeof(info->name));
  info->status = status_;
  uint32_t written_length = offsetof(sync_fence_info_data, pt_info);

  for (size_t i = 0; i < sync_points_.size(); ++i) {
    sync_pt_info* target =
        reinterpret_cast<sync_pt_info*>(
            reinterpret_cast<uint8_t*>(info) + written_length);
    uint32_t result = sync_points_[i]->sync_point->FillSyncPtInfo(
        target, info->len - written_length);
    if (!result) {
      ALOGW("Failed to write sync point informations.");
      errno = ENOMEM;
      return -1;
    }
    written_length += result;
  }
  info->len = written_length;
  return 0;
}

bool FenceStream::IsValidLocked() const {
  fence_mutex_.AssertAcquired();

  ALOG_ASSERT(!fence_name_.empty());

  // Check all sync points have different timelines.
  std::set<Timeline*> timelines;
  for (size_t i = 0; i < sync_points_.size(); ++i) {
    if (!timelines.insert(sync_points_[i]->timeline).second) {
      ALOGE("Found two sync points which are on the same timeline.");
      return false;
    }
  }

  if (status_ != FENCE_ACTIVE && status_ != FENCE_SIGNALED) {
    ALOGE("Unexpected status value: %d", status_);
    return false;
  }
  return true;
}

uint32_t FenceStream::GetSignaledSyncPointCountLocked() const {
  fence_mutex_.AssertAcquired();
  uint32_t num_signaled = 0;
  for (size_t i = 0; i < sync_points_.size(); ++i) {
    if (sync_points_[i]->sync_point->IsSignaled())
      num_signaled++;
  }
  return num_signaled;
}

uint32_t FenceStream::GetWaitingThreadCountFenceForTesting() {
  base::AutoLock lock(fence_mutex_);
  return waiting_thread_count_for_testing_;
}

FenceStream::SyncPointTimeline::SyncPointTimeline(scoped_ptr<SyncPoint> sp,
                                                  scoped_refptr<Timeline> tm)
        : sync_point(sp.Pass()), timeline(tm) {
}

FenceStream::SyncPointTimeline::~SyncPointTimeline() {
}

}  // namespace posix_translation
