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

#ifndef GRAPHICS_TRANSLATION_TESTS_GRAPHICS_TEST_H_
#define GRAPHICS_TRANSLATION_TESTS_GRAPHICS_TEST_H_

#ifdef GRAPHICS_TRANSLATION_APK
#include "GLES/gl.h"
#include "GLES2/gl2.h"
#else
#include "GL/gl.h"
#include "GL/glx.h"

// The following functions are GLES-specific.
void glFrustumf(float a, float b, float c, float d, float e, float f);
void glOrthof(float a, float b, float c, float d, float e, float f);
void glClipPlanef(GLenum name, const float* values);
void glClearDepthf(float v);

#endif

// In X.h, which is indirectly included by GL/glx.h, None is defined as 0L.
// However, it conflicts with internal::None struct definition in gtest.
// So, here we undef it. Similarly Bool defined in Xlib.h conflicts, so
// undef it, too.
#ifdef None
#undef None
#endif
#ifdef Bool
#undef Bool
#endif
#include "gtest/gtest.h"

// Base fixture of the graphics translation tests.
class GraphicsTranslationTestBase : public ::testing::Test {
 protected:
  GraphicsTranslationTestBase();
  ~GraphicsTranslationTestBase();

  virtual void SetUp();
  virtual void TearDown();

 private:
  // These functions are defined differently for host and target.
  // Please see also exe/ and apk/ directories for more details.
  // Note that, in CreateTestContext(), it is necessary to initialize view size
  // via SetViewSize declared below.
  static void CreateTestContext();
  static void DestroyTestContext();

  // Sets the view size. This must be set in CreateTestContext() properly.
  static void SetViewSize(int width, int height);
  static int width_;
  static int height_;
};

// EXPECT macro for the renderered image.
// This is a kind of golden test. The reference images are generated on host,
// and the renderered images are compared with the corresponding reference
// image on target.
// This macro uses the test method name for the file name, so using this
// macro twice (or more) in a test would cause a problem.
// Please see also exe/ and apk/ directories for more details and the
// implementation.
::testing::AssertionResult ExpectImageWithTolerance(uint64_t tolerance);

#define EXPECT_IMAGE() EXPECT_TRUE(ExpectImageWithTolerance(0))
#define EXPECT_IMAGE_WITH_TOLERANCE(tolerance) \
    EXPECT_TRUE(ExpectImageWithTolerance(tolerance))

#endif  // GRAPHICS_TRANSLATION_TESTS_GRAPHICS_TEST_H_
