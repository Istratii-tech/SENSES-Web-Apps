// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMMON_MATH_TEST_HELPERS_H_
#define COMMON_MATH_TEST_HELPERS_H_

#include "common/matrix.h"
#include "common/vector.h"

namespace arc {

static const Vector kZeroVector(0.f, 0.f, 0.f, 0.f);

static const Matrix kIdentityMatrix(
  1.f, 0.f, 0.f, 0.f,
  0.f, 1.f, 0.f, 0.f,
  0.f, 0.f, 1.f, 0.f,
  0.f, 0.f, 0.f, 1.f
);

static const Matrix kFunMatrix(
  1.f, 2.f, 3.f, 4.f,
  5.f, 6.f, 7.f, 8.f,
  9.f, 10.f, 11.f, 12.f,
  13.f, 14.f, 15.f, 16.f
);

static const Matrix kTransposedFunMatrix(
  1.f, 5.f, 9.f,  13.f,
  2.f, 6.f, 10.f, 14.f,
  3.f, 7.f, 11.f, 15.f,
  4.f, 8.f, 12.f, 16.f
);

static const Matrix kRescaledNormalFunMatrix(
  1.f / sqrt(14.f), 2.f / sqrt(14.f), 3.f / sqrt(14.f), 4.f,
  5.f / sqrt(110.f), 6.f / sqrt(110.f), 7.f / sqrt(110.f), 8.f,
  9.f / sqrt(302.f), 10.f / sqrt(302.f), 11.f / sqrt(302.f), 12.f,
  13.f, 14.f, 15.f, 16.f
);

static const Matrix kFunMatrix2(
  17.f, 18.f, 19.f, 20.f,
  21.f, 22.f, 23.f, 24.f,
  25.f, 26.f, 27.f, 28.f,
  29.f, 30.f, 31.f, 32.f
);

static const Matrix kFunProduct(
  250.f,   260.f,  270.f,  280.f,
  618.f,   644.f,  670.f,  696.f,
  986.f,  1028.f, 1070.f, 1112.f,
  1354.f, 1412.f, 1470.f, 1528.f
);

static const Matrix kOrthographic400x640Matrix(
  2.0f/400.f, 0.f,         0.f,  -1.0f,
  0.f,        2.0f/640.f,  0.f,  -1.0f,
  0.f,        0.f,        -2.f,  -1.0f,
  0.f,        0.f,         0.f,   1.0f
);

static const Matrix kPerspective400x640Matrix(
  2.0f/400.f, 0.f,         1.f,   0.0f,
  0.f,        2.0f/640.f,  1.f,   0.0f,
  0.f,        0.f,        -3.f,  -4.0f,
  0.f,        0.f,        -1.f,   0.0f
);

inline bool AlmostEquals(float v1, float v2) {
  // This is a hack, but it avoids the pain of comparing almost +0.0 and
  // almost -0.0.
  const float kAlmostZero = 0.000000250f;
  if (fabsf(v1) < kAlmostZero && fabsf(v2) < kAlmostZero)
    return true;
  // Check that values are within 1 units of least precision for floats.
  if (nextafterf(v1, v2) != v2)
    return false;
  return true;
}

inline bool AlmostEquals(const Vector& lhs, const Vector& rhs) {
  for (size_t i = 0; i < Vector::kEntries; ++i) {
    if (!AlmostEquals(lhs.Get(i), rhs.Get(i))) {
      return false;
    }
  }
  return true;
}

inline bool AlmostEquals(const Matrix& lhs, const Matrix& rhs) {
  for (int i = 0; i < Matrix::kN; ++i) {
    for (int j = 0; j < Matrix::kN; ++j) {
      if (!AlmostEquals(lhs.Get(i, j), rhs.Get(i, j))) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace arc

#endif  // COMMON_MATH_TEST_HELPERS_H_
