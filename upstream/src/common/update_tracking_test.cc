// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//

#include "base/compiler_specific.h"
#include "common/update_tracking.h"
#include "gtest/gtest.h"

namespace arc {

class UpdateTrackingTest : public testing::Test {
 public:
  UpdateTrackingTest()
      : kInitialUpdateNumber(UpdateProducer::kInitialUpdateNumber),
        kInvalidUpdateNumber(UpdateProducer::kInvalidUpdateNumber) {}
  virtual ~UpdateTrackingTest() {}
  virtual void SetUp() OVERRIDE {}
  virtual void TearDown() OVERRIDE {}

  UpdateProducer::UpdateNumber GetConsumerUpdateNumber() {
    return consumer_.last_consumed_update_number_;
  }

  void SetConsumerUpdateNumber(UpdateProducer::UpdateNumber num) {
    consumer_.last_consumed_update_number_ = num;
  }

 protected:
  const int kInitialUpdateNumber;
  const int kInvalidUpdateNumber;
  UpdateProducer producer_;
  UpdateConsumer consumer_;
};

TEST_F(UpdateTrackingTest, InitiallyConsumeOne) {
  EXPECT_TRUE(consumer_.AreThereUpdatesAndConsumeIfSo(&producer_));
  EXPECT_EQ(kInitialUpdateNumber, GetConsumerUpdateNumber());
  EXPECT_FALSE(consumer_.AreThereUpdatesAndConsumeIfSo(&producer_));
  EXPECT_FALSE(consumer_.AreThereUpdatesAndConsumeIfSo(&producer_));
  EXPECT_FALSE(consumer_.AreThereUpdatesAndConsumeIfSo(&producer_));
}

TEST_F(UpdateTrackingTest, ConsumeOneProducedUpdate) {
  EXPECT_TRUE(consumer_.AreThereUpdatesAndConsumeIfSo(&producer_));
  producer_.ProduceUpdate();
  EXPECT_TRUE(consumer_.AreThereUpdatesAndConsumeIfSo(&producer_));
  EXPECT_NE(kInitialUpdateNumber, GetConsumerUpdateNumber());
  EXPECT_NE(kInvalidUpdateNumber, GetConsumerUpdateNumber());
  EXPECT_FALSE(consumer_.AreThereUpdatesAndConsumeIfSo(&producer_));
}

TEST_F(UpdateTrackingTest, MultipleProducesConsumedOnce) {
  EXPECT_TRUE(consumer_.AreThereUpdatesAndConsumeIfSo(&producer_));
  EXPECT_FALSE(consumer_.AreThereUpdatesAndConsumeIfSo(&producer_));
  producer_.ProduceUpdate();
  producer_.ProduceUpdate();
  producer_.ProduceUpdate();
  EXPECT_TRUE(consumer_.AreThereUpdatesAndConsumeIfSo(&producer_));
  EXPECT_FALSE(consumer_.AreThereUpdatesAndConsumeIfSo(&producer_));
  EXPECT_FALSE(consumer_.AreThereUpdatesAndConsumeIfSo(&producer_));
  EXPECT_FALSE(consumer_.AreThereUpdatesAndConsumeIfSo(&producer_));
}

}  // namespace arc
