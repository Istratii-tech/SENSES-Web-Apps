// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// test matrix manipulation.

#include <math.h>
#include "common/math_test_helpers.h"
#include "common/matrix.h"
#include "common/vector.h"
#include "gtest/gtest.h"

namespace arc {

TEST(Matrix, DefaultIdentityConstructor) {
  Matrix m;
  EXPECT_TRUE(AlmostEquals(m, kIdentityMatrix));
}

TEST(Matrix, OperatorMultiplyBy) {
  Matrix a(kFunMatrix);
  Matrix b(kFunMatrix2);
  a *= b;
  EXPECT_TRUE(AlmostEquals(a, kFunProduct));
}

TEST(Matrix, Transpose) {
  Matrix m(kFunMatrix);
  m.Transpose();
  EXPECT_TRUE(AlmostEquals(m, kTransposedFunMatrix));
}

TEST(Matrix, RescaleNormal) {
  Matrix m(kFunMatrix);
  m.RescaleNormal();
  EXPECT_TRUE(AlmostEquals(m, kRescaledNormalFunMatrix));
}

TEST(Matrix, Inverse) {
  Vector translate(1.f, 2.f, 3.f, 1.f);
  Vector axis(1.f, 1.f, 0.f, 0.f);

  Matrix a;
  a.AssignMatrixMultiply(a, Matrix::GenerateTranslation(translate));
  a.AssignMatrixMultiply(a, Matrix::GenerateRotationByDegrees(30.f, axis));

  Matrix b = a;
  b.Inverse();
  a.AssignMatrixMultiply(a, b);
  EXPECT_TRUE(AlmostEquals(a, kIdentityMatrix));
}

TEST(Matrix, GetColumnMajorArray) {
  Matrix m(kFunMatrix);

  float arr[Matrix::kEntries];
  float* result = m.GetColumnMajorArray(arr);

  EXPECT_EQ(result, arr);
  for (int col = 0; col < Matrix::kN; ++col) {
    for (int row = 0; row < Matrix::kN; ++row) {
      EXPECT_EQ(*result, m.Get(row, col));
      ++result;
    }
  }
}

TEST(Matrix, GenerateColumnMajor) {
  float arr[Matrix::kEntries];
  for (int i = 0; i < Matrix::kEntries; ++i) {
    arr[i] = static_cast<float>(i+1);
  }
  Matrix m(arr);
  EXPECT_TRUE(AlmostEquals(m, kTransposedFunMatrix));
}

TEST(Matrix, TransposedMatrixProduct) {
  // Linear algebra says that A*B = (BT*AT)T.  We verify that
  // using our fun matrices and fun product.
  Matrix a(kFunMatrix);
  a.Transpose();
  Matrix b(kFunMatrix2);
  b.Transpose();
  Matrix p;
  p.AssignMatrixMultiply(b, a);
  p.Transpose();
  EXPECT_TRUE(AlmostEquals(p, kFunProduct));
}

TEST(Matrix, GenerateOrthographic) {
  Matrix m = Matrix::GenerateOrthographic(0.f, 400.f, 0.f, 640.f, 0.f, 1.f);
  EXPECT_TRUE(AlmostEquals(m, kOrthographic400x640Matrix));
}

TEST(Matrix, GeneratePerspective) {
  Matrix m = Matrix::GeneratePerspective(0.f, 400.f, 0.f, 640.f, 1.f, 2.f);
  EXPECT_TRUE(AlmostEquals(m, kPerspective400x640Matrix));
}

TEST(Matrix, GenerateScaleMatrix) {
  Vector v(2.f, 3.f, 4.f, 1.f);
  Matrix m = Matrix::GenerateScale(v);
  static const Matrix kScale(
      2.f,  0.f,  0.f,  0.f,
      0.f,  3.f,  0.f,  0.f,
      0.f,  0.f,  4.f,  0.f,
      0.f,  0.f,  0.f,  1.f);
  EXPECT_TRUE(AlmostEquals(m, kScale));
}

TEST(Matrix, GenerateTranslationMatrix) {
  Vector v(2.f, 3.f, 4.f, 1.f);
  Matrix m = Matrix::GenerateTranslation(v);
  static const Matrix kTranslate(
      1.f,  0.f,  0.f,  2.f,
      0.f,  1.f,  0.f,  3.f,
      0.f,  0.f,  1.f,  4.f,
      0.f,  0.f,  0.f,  1.f);
  EXPECT_TRUE(AlmostEquals(m, kTranslate));
}

TEST(Matrix, GenerateRotationMatrix) {
  Matrix m;
  Vector v(0.f, 1.f, 0.f, 0.f);
  // Create a matrix to rotate 90 degrees counterclockwise
  // about the Y axis.  Its columns should describe what the
  // matrix does to the X, Y, and Z axes.
  m = Matrix::GenerateRotationByDegrees(90.f, v);
  static const Matrix k90DegYawLeft(
       0.f,  0.f,  1.f,  0.f,
       0.f,  1.f,  0.f,  0.f,
      -1.f,  0.f,  0.f,  0.f,
       0.f,  0.f,  0.f,  1.f);
  EXPECT_TRUE(AlmostEquals(m, k90DegYawLeft));

  m = Matrix::GenerateRotationByDegrees(-90.f, v);
  static const Matrix k90DegYawRight(
      0.f,  0.f, -1.f,  0.f,
      0.f,  1.f,  0.f,  0.f,
      1.f,  0.f,  0.f,  0.f,
      0.f,  0.f,  0.f,  1.f);
  EXPECT_TRUE(AlmostEquals(m, k90DegYawRight));

  v = Vector(1.f, 0.f, 0.f, 0.f);
  m = Matrix::GenerateRotationByDegrees(90.f, v);
  static const Matrix k90DegPitchUp(
      1.f,  0.f,  0.f,  0.f,
      0.f,  0.f, -1.f,  0.f,
      0.f,  1.f,  0.f,  0.f,
      0.f,  0.f,  0.f,  1.f);
  EXPECT_TRUE(AlmostEquals(m, k90DegPitchUp));
}

}  // namespace arc
