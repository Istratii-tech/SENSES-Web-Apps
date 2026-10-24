// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// These classes intend to provide a polling mechanism between producer
// and consumer for when updates are available for consuming.  Note that
// initially there is an update to consume.  Also note that the reading
// and any synchronization requirements for reading are not handled by
// this class.

#ifndef COMMON_UPDATE_TRACKING_H_
#define COMMON_UPDATE_TRACKING_H_

#include "common/private/minimal_base.h"
#include "common/scoped_pthread_mutex_locker.h"

namespace arc {

class UpdateProducer {
 public:
  UpdateProducer() : update_number_(kInitialUpdateNumber) {
    pthread_mutex_init(&mutex_, NULL);
  }

  void ProduceUpdate() {
    ScopedPthreadMutexLocker lock(&mutex_);
    update_number_++;
    if (update_number_ < kInitialUpdateNumber)
      update_number_ = kInitialUpdateNumber;
  }

 private:
  friend class UpdateConsumer;
  friend class UpdateTrackingTest;
  typedef int UpdateNumber;
  enum {
    kInvalidUpdateNumber = -1,
    kInitialUpdateNumber = 0
  };

  UpdateNumber update_number_;
  pthread_mutex_t mutex_;

  COMMON_DISALLOW_COPY_AND_ASSIGN(UpdateProducer);
};

class UpdateConsumer {
 public:
  UpdateConsumer()
      : last_consumed_update_number_(UpdateProducer::kInvalidUpdateNumber) {}

  bool AreThereUpdatesAndConsumeIfSo(UpdateProducer* producer) {
    ScopedPthreadMutexLocker lock(&producer->mutex_);
    if (last_consumed_update_number_ != producer->update_number_) {
      last_consumed_update_number_ = producer->update_number_;
      return true;
    }
    return false;
  }

 private:
  friend class UpdateTrackingTest;
  int last_consumed_update_number_;

  COMMON_DISALLOW_COPY_AND_ASSIGN(UpdateConsumer);
};

}  // namespace arc

#endif  // COMMON_UPDATE_TRACKING_H_
