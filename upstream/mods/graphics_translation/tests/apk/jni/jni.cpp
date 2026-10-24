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

#include <jni.h>
#include <stdio.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <EGL/egl.h>
#include <GLES/gl.h>
#include <GLES2/gl2.h>
#include <sstream>
#include "common/alog.h"
#include "tests/util/texture.h"
#include "tests/graphics_test.h"

#include "jni_test_main.h"  // NOLINT

namespace {

EGLDisplay g_display = 0;
EGLConfig g_config = 0;
EGLSurface g_surface = 0;
EGLContext g_context = 0;
EGLint g_width = 0;
EGLint g_height = 0;
ANativeWindow* g_window = 0;

}  // namespace

// Platform dependent {Create,Destroy}TestContext implementation.

void GraphicsTranslationTestBase::CreateTestContext() {
  bool success = false;
  if (g_display == 0) {
    const EGLint attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_BLUE_SIZE,    8,
        EGL_GREEN_SIZE,   8,
        EGL_RED_SIZE,     8,
        EGL_DEPTH_SIZE,   8,
        EGL_STENCIL_SIZE, 8,
        EGL_NONE };

    g_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    LOG_ALWAYS_FATAL_IF(!g_display);

    success = eglInitialize(g_display, 0, 0);
    LOG_ALWAYS_FATAL_IF(!success);

    EGLint numConfigs;
    success = eglChooseConfig(g_display, attribs, &g_config, 1, &numConfigs);
    LOG_ALWAYS_FATAL_IF(!success);

    EGLint format;
    success = eglGetConfigAttrib(g_display, g_config, EGL_NATIVE_VISUAL_ID,
                                 &format);
    LOG_ALWAYS_FATAL_IF(!success);

    ANativeWindow_setBuffersGeometry(g_window, 0, 0, format);
  }

  g_surface = eglCreateWindowSurface(g_display, g_config, g_window, 0);
  LOG_ALWAYS_FATAL_IF(!g_surface);

  g_context = eglCreateContext(g_display, g_config, 0, 0);
  LOG_ALWAYS_FATAL_IF(!g_context);

  success = eglMakeCurrent(g_display, g_surface, g_surface, g_context);
  LOG_ALWAYS_FATAL_IF(!success);

  eglQuerySurface(g_display, g_surface, EGL_WIDTH, &g_width);
  eglQuerySurface(g_display, g_surface, EGL_HEIGHT, &g_height);

  SetViewSize(g_width, g_height);
}

void GraphicsTranslationTestBase::DestroyTestContext() {
  eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroyContext(g_display, g_context);
  eglDestroySurface(g_display, g_surface);
}

::testing::AssertionResult ExpectImageWithTolerance(uint64_t tolerance) {
  const ::testing::TestInfo* test_info =
      ::testing::UnitTest::GetInstance()->current_test_info();

  Texture img;
  img.Initialize(g_width, g_height);
  glReadPixels(0, 0, img.Width(), img.Height(), GL_RGBA, GL_UNSIGNED_BYTE,
               img.GetData());
  eglSwapBuffers(g_display, g_surface);

  // Load the golden/reference image.
  std::stringstream ss;
  ss << "/vendor/chromium/crx/gold/"
     << test_info->test_case_name() << "." << test_info->name() << ".ppm";
  Texture ref;
  const bool loaded = ref.LoadPPM(ss.str());
  if (!loaded) {
    return ::testing::AssertionSuccess();
  }

  // Allow each pixel in the image to be off by some value per color component.
  static const int kExtraTolerancePerColor = 1;
  const uint64_t diff = Texture::Compare(img, ref, kExtraTolerancePerColor);
  if (diff > tolerance) {
    return ::testing::AssertionFailure()
        << "Measured difference of " << diff << ". "
        << "(Expected: " << tolerance << ")";
  }
  return ::testing::AssertionSuccess();
}

extern "C" jint
Java_org_chromium_graphics_1translation_1tests_GraphicsTranslationTestCases_runTests(
    JNIEnv* env, jobject thiz, jstring gtest_list, jstring gtest_filter) {
  return arc::test::RunAllTests(env, thiz, gtest_list, gtest_filter);
}

extern "C" void
Java_org_chromium_graphics_1translation_1tests_GraphicsTranslationTestCases_setSurface(
    JNIEnv* env, jobject thiz, jobject surface) {
  if (g_window) {
    ANativeWindow_release(g_window);
    g_window = NULL;
  }

  if (surface) {
    g_window = ANativeWindow_fromSurface(env, surface);
  }
}
