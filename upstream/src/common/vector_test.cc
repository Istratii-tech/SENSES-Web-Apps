// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// test vector manipulation.

#include "common/math_test_helpers.h"
#include "common/vector.h"
#include "gtest/gtest.h"

namespace arc {

TEST(Vector, StartZero) {
  Vector v;
  EXPECT_TRUE(AlmostEquals(v, kZeroVector));
}

TEST(Vector, ReadWrite) {
  Vector v;
  static const Vector k1234Vector(1.f, 2.f, 3.f, 4.f);
  v.Set(0, 1.f);
  v.Set(1, 2.f);
  v.Set(2, 3.f);
  v.Set(3, 4.f);
  EXPECT_TRUE(AlmostEquals(v, k1234Vector));
}

TEST(Vector, DotProducts) {
  Vector v1(0.f, 0.f, 1.f, 0.f);
  Vector v2(0.f, 1.f, 0.f, 0.f);
  EXPECT_FLOAT_EQ(0.f, v1.GetDotProduct(v2));
  EXPECT_FLOAT_EQ(0.f, v2.GetDotProduct(v1));

  Vector v3(1.f,  3.f, -5.f, 0.f);
  Vector v4(4.f, -2.f, -1.f, 0.f);
  EXPECT_FLOAT_EQ(3.f, v3.GetDotProduct(v4));
}

TEST(Vector, Length) {
  Vector v;
  EXPECT_FLOAT_EQ(0.f, v.GetLength());
  v = Vector(1.f, 0.f, 0.f, 0.f);
  EXPECT_FLOAT_EQ(1.f, v.GetLength());
  v = Vector(1.f, 1.f, 0.f, 0.f);
  EXPECT_FLOAT_EQ(sqrt(2.f), v.GetLength());
}

TEST(Vector, Normalize) {
  Vector v(1.f, 0.f, 0.f, 0.f);
  v.Normalize();
  static const Vector kOne(1.f, 0.f, 0.f, 0.f);
  EXPECT_TRUE(AlmostEquals(v, kOne));
  v = Vector(1.f, 1.f, 1.f, 1.f);
  v.Normalize();
  static const Vector kUnit(0.5f, 0.5f, 0.5f, 0.5f);
  EXPECT_TRUE(AlmostEquals(v, kUnit));
}

TEST(Vector, MatrixMultiply) {
  Vector v(1.f, 2.f, 3.f, 4.f);
  v.AssignMatrixMultiply(Matrix(kFunMatrix), v);
  EXPECT_FLOAT_EQ(30.f, v.Get(0));
  EXPECT_FLOAT_EQ(70.f, v.Get(1));
  EXPECT_FLOAT_EQ(110.f, v.Get(2));
  EXPECT_FLOAT_EQ(150.f, v.Get(3));
}

TEST(Vector, GetFloatArray) {
  const Vector v(1.f, 2.f, 3.f, 4.f);
  float data[4];
  float* ptr = v.GetFloatArray(data);
  EXPECT_EQ(ptr, data);
  EXPECT_FLOAT_EQ(1.f, data[0]);
  EXPECT_FLOAT_EQ(2.f, data[1]);
  EXPECT_FLOAT_EQ(3.f, data[2]);
  EXPECT_FLOAT_EQ(4.f, data[3]);
}

TEST(Vector, AssignLinearMapping) {
  const float kEpsilon = 0.0001f;
  Vector v;

  const int8_t byte_data[4] = {
    // Narrowing conversions from int to int8_t (signed char) is ill-formed
    // in C++11.
    static_cast<int8_t>(0x80), static_cast<int8_t>(0xff), 0x00, 0x7f
  };
  v.AssignLinearMapping(byte_data, 4);
  EXPECT_NEAR(-1.f, v.Get(0), kEpsilon);
  EXPECT_NEAR(-0.00392f, v.Get(1), kEpsilon);
  EXPECT_NEAR(0.00392f, v.Get(2), kEpsilon);
  EXPECT_NEAR(1.f, v.Get(3), kEpsilon);

  const uint8_t ubyte_data[4] = {0x00, 0x33, 0x66, 0xff};
  v.AssignLinearMapping(ubyte_data, 4);
  EXPECT_FLOAT_EQ(0.0f, v.Get(0));
  EXPECT_FLOAT_EQ(0.2f, v.Get(1));
  EXPECT_FLOAT_EQ(0.4f, v.Get(2));
  EXPECT_FLOAT_EQ(1.0f, v.Get(3));

  const int16_t short_data[4] = {-32767, 0, 32767, 0x3333};
  v.AssignLinearMapping(short_data, 4);
  EXPECT_NEAR(-1.0f, v.Get(0), kEpsilon);
  EXPECT_NEAR(0.0f, v.Get(1), kEpsilon);
  EXPECT_NEAR(1.0f, v.Get(2), kEpsilon);
  EXPECT_NEAR(0.4f, v.Get(3), kEpsilon);

  const float float_data[4] = {-1.f, 0.f, 1.f, 0.4f};
  v.AssignLinearMapping(float_data, 4);
  EXPECT_NEAR(-1.0f, v.Get(0), kEpsilon);
  EXPECT_NEAR(0.0f, v.Get(1), kEpsilon);
  EXPECT_NEAR(1.0f, v.Get(2), kEpsilon);
  EXPECT_NEAR(0.4f, v.Get(3), kEpsilon);

  const float float_data2[4] = {9.f, 9.f, 9.f, 9.f};
  v.AssignLinearMapping(float_data2, 0);
  EXPECT_NEAR(-1.0f, v.Get(0), kEpsilon);
  EXPECT_NEAR(0.0f, v.Get(1), kEpsilon);
  EXPECT_NEAR(1.0f, v.Get(2), kEpsilon);
  EXPECT_NEAR(0.4f, v.Get(3), kEpsilon);
}

TEST(Vector, GetLinearMapping) {
  const float kIntScale = (1u << 31) - 1;
  const float kEpsilon = 1000;
  Vector v(1.f, 0.25f, 0.33333f, 0.f);

  int32_t int_data[4];
  v.GetLinearMapping(int_data, 4);
  EXPECT_NEAR(kIntScale * v.Get(0), int_data[0], kEpsilon);
  EXPECT_NEAR(kIntScale * v.Get(1), int_data[1], kEpsilon);
  EXPECT_NEAR(kIntScale * v.Get(2), int_data[2], kEpsilon);
  EXPECT_NEAR(kIntScale * v.Get(3), int_data[3], kEpsilon);

  float float_data[4];
  v.GetLinearMapping(float_data, 4);
  EXPECT_FLOAT_EQ(v.Get(0), float_data[0]);
  EXPECT_FLOAT_EQ(v.Get(1), float_data[1]);
  EXPECT_FLOAT_EQ(v.Get(2), float_data[2]);
  EXPECT_FLOAT_EQ(v.Get(3), float_data[3]);
}

TEST(Vector, Clamp) {
  Vector v(-1.f, 0.f, 1.f, 2.f);
  v.Clamp(0.f, 1.f);
  EXPECT_FLOAT_EQ(0.f, v.Get(0));
  EXPECT_FLOAT_EQ(0.f, v.Get(1));
  EXPECT_FLOAT_EQ(1.f, v.Get(2));
  EXPECT_FLOAT_EQ(1.f, v.Get(3));
}

}  // namespace arc
