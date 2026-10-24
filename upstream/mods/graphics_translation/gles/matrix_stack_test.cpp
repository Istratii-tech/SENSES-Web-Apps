/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "graphics_translation/gles/matrix_stack.h"
#include "common/math_test_helpers.h"
#include "gtest/gtest.h"

TEST(MatrixStack, InitiallyIdentity) {
  MatrixStack s;
  EXPECT_EQ(1, s.GetDepth());
  EXPECT_TRUE(&s.GetTop() != NULL);
  EXPECT_TRUE(AlmostEquals(s.GetTop(), arc::kIdentityMatrix));
}

TEST(MatrixStack, PushAndPopSemantics) {
  MatrixStack s;
  s.GetTop() =
      arc::Matrix::GenerateOrthographic(0.f, 400.f, 0.f, 640.f, 0.f, 1.f);
  EXPECT_TRUE(AlmostEquals(s.GetTop(), arc::kOrthographic400x640Matrix));
  // Outermost is now orthographic projection matrix.

  EXPECT_TRUE(s.Push());

  EXPECT_EQ(2, s.GetDepth());
  EXPECT_TRUE(AlmostEquals(s.GetTop(), arc::kOrthographic400x640Matrix));
  s.GetTop().AssignIdentity();
  EXPECT_TRUE(AlmostEquals(s.GetTop(), arc::kIdentityMatrix));
  // Middle is now identity matrix.

  EXPECT_TRUE(s.Push());

  EXPECT_EQ(3, s.GetDepth());
  EXPECT_TRUE(AlmostEquals(s.GetTop(), arc::kIdentityMatrix));
  s.GetTop() = arc::kFunMatrix;
  EXPECT_TRUE(AlmostEquals(s.GetTop(), arc::kFunMatrix));

  EXPECT_TRUE(s.Pop());

  EXPECT_EQ(2, s.GetDepth());
  EXPECT_TRUE(AlmostEquals(s.GetTop(), arc::kIdentityMatrix));

  EXPECT_TRUE(s.Pop());

  EXPECT_EQ(1, s.GetDepth());
  EXPECT_TRUE(AlmostEquals(s.GetTop(), arc::kOrthographic400x640Matrix));

  // This pop effectively is ignored.
  EXPECT_FALSE(s.Pop());

  EXPECT_EQ(1, s.GetDepth());
  EXPECT_TRUE(AlmostEquals(s.GetTop(), arc::kOrthographic400x640Matrix));
}

TEST(MatrixStack, Overflow) {
  MatrixStack s;
  for (size_t i = 1; i < MatrixStack::kMaxDepth; ++i) {
    EXPECT_EQ(i, static_cast<size_t>(s.GetDepth()));
    EXPECT_TRUE(s.Push());
  }
  EXPECT_FALSE(s.Push());
}
