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

#include <assert.h>
#include <stdio.h>
#include <string>
#include <sstream>

#include "tests/graphics_test.h"
#include "tests/util/texture.h"
#include "common/alog.h"

void glFrustumf(float a, float b, float c, float d, float e, float f) {
  glFrustum(a, b, c, d, e, f);
}

void glOrthof(float a, float b, float c, float d, float e, float f) {
  glOrtho(a, b, c, d, e, f);
}

void glClipPlanef(GLenum name, const float* values) {
  const double arr[] = { values[0], values[1], values[2], values[3] };
  glClipPlane(name, arr);
}

void glClearDepthf(float v) {
  glClearDepth(v);
}

namespace {

Display* g_display = NULL;
Window g_window = 0;
Colormap g_colormap = 0;
GLXFBConfig g_fb_config = 0;
GLXContext g_glx_context = 0;
GLXWindow g_glx_window = 0;
const int kWidth = 360;
const int kHeight = 584;

// Because None and Bool are undef'ed, in graphics_tests.h to avoid name
// confliction with gtest, so we use 0 and int directly, instead.

int /* Bool */ WaitForNotify(Display* dpy, XEvent* event, XPointer arg) {
  return (event->type == MapNotify) && (event->xmap.window == (Window)arg);
}

GLXFBConfig GetFrameBufferConfig() {
  const int attributes[] = {
      GLX_X_RENDERABLE,   True,
      GLX_DRAWABLE_TYPE,  GLX_WINDOW_BIT,
      GLX_RENDER_TYPE,    GLX_RGBA_BIT,
      GLX_X_VISUAL_TYPE,  GLX_TRUE_COLOR,
      GLX_RED_SIZE,       8,
      GLX_GREEN_SIZE,     8,
      GLX_BLUE_SIZE,      8,
      GLX_ALPHA_SIZE,     8,
      GLX_DEPTH_SIZE,     24,
      GLX_STENCIL_SIZE,   8,
      GLX_DOUBLEBUFFER,   True,
      GLX_SAMPLE_BUFFERS, 0,
      GLX_SAMPLES,        0,
      0 /* None */ };

  int count;
  GLXFBConfig* fbc = glXChooseFBConfig(
      g_display, DefaultScreen(g_display), attributes, &count);
  assert(fbc && "Unable to acquire frame buffer configs");

  // Pick the FB config/visual with the least samples per pixel.
  int best = -1;
  int best_samples = -1;
  for (int i = 0; i < count; ++i) {
    XVisualInfo* vi = glXGetVisualFromFBConfig(g_display, fbc[i]);
    if (vi) {
      int samples;
      int sample_buffer;
      glXGetFBConfigAttrib(g_display, fbc[i], GLX_SAMPLES, &samples);
      glXGetFBConfigAttrib(g_display, fbc[i], GLX_SAMPLE_BUFFERS,
                           &sample_buffer);
      if (best < 0 || (sample_buffer && samples < best_samples)) {
        best = i;
        best_samples = samples;
      }
    }
    XFree(vi);
  }

  GLXFBConfig result = fbc[best];
  XFree(fbc);
  return result;
}

void CreateXWindow() {
  g_display = XOpenDisplay(0);
  assert(g_display && "Unable to open display");

  g_fb_config = GetFrameBufferConfig();
  assert(g_fb_config && "Unable to get frame buffer config");

  XVisualInfo* vi = glXGetVisualFromFBConfig(g_display, g_fb_config);
  assert(vi && "Unable to get visuals");

  g_colormap = XCreateColormap(g_display, RootWindow(g_display, vi->screen),
                               vi->visual, AllocNone);
  assert(g_colormap && "Unable to create color map");

  XSetWindowAttributes swa;
  swa.border_pixel = 0;
  swa.event_mask = StructureNotifyMask;
  swa.colormap = g_colormap;
  g_window = XCreateWindow(g_display, RootWindow(g_display, vi->screen),
                           0, 0, kWidth, kHeight, 0, vi->depth,
                           InputOutput, vi->visual,
                           CWBorderPixel | CWColormap | CWEventMask, &swa);
  assert(g_window && "Unable to create window");

  XEvent event;
  XMapWindow(g_display, g_window);
  XIfEvent(g_display, &event, WaitForNotify, (XPointer)g_window);
  XFree(vi);
}

void DestroyXWindow() {
  XFreeColormap(g_display, g_colormap);
  XDestroyWindow(g_display, g_window);
  XCloseDisplay(g_display);
}


class EventListener : public ::testing::EmptyTestEventListener {
 public:
  EventListener() {
  }
  ~EventListener() {
  }

  virtual void OnTestProgramStart(const ::testing::UnitTest& /*unit_test*/) {
    CreateXWindow();
  }
  virtual void OnTestProgramEnd(const ::testing::UnitTest& /*unit_test*/) {
    DestroyXWindow();
  }
};

}  // namespace

// Platform dependent {Create/Destroy}TestContext implementation.

void GraphicsTranslationTestBase::CreateTestContext() {
  typedef GLXContext (*CreateContextFn)(
      Display*, GLXFBConfig, GLXContext, int /* Bool */, const int*);
  CreateContextFn glXCreateContextAttribsARB =
      (CreateContextFn)glXGetProcAddressARB(
          (const GLubyte*)"glXCreateContextAttribsARB");
  const int attribs[] = {GLX_CONTEXT_MAJOR_VERSION_ARB, 3,
                         GLX_CONTEXT_MINOR_VERSION_ARB, 0, 0 /* None */};
  g_glx_context =
      glXCreateContextAttribsARB(g_display, g_fb_config, 0, True, attribs);
  assert(g_glx_context && "Unable to create glx context");

  g_glx_window = glXCreateWindow(g_display, g_fb_config, g_window, NULL);
  assert(g_glx_window && "Unable to create GLX window");

  const bool success = glXMakeContextCurrent(
      g_display, g_glx_window, g_glx_window, g_glx_context);
  assert(success && "Unable to set glx context");

  glDisable(GL_MULTISAMPLE);

  SetViewSize(kWidth, kHeight);
}

void GraphicsTranslationTestBase::DestroyTestContext() {
  glXMakeCurrent(g_display, 0, 0);
  glXDestroyContext(g_display, g_glx_context);
  g_glx_context = 0;
  glXDestroyWindow(g_display, g_glx_window);
  g_glx_window = 0;
}

// On host, generates the referece image.
::testing::AssertionResult ExpectImageWithTolerance(uint64_t tolerance) {
  const ::testing::TestInfo* test_info =
      ::testing::UnitTest::GetInstance()->current_test_info();

  glXSwapBuffers(g_display, g_glx_window);
  Texture img;
  img.Initialize(kWidth, kHeight);
  glReadPixels(0, 0, img.Width(), img.Height(), GL_RGBA, GL_UNSIGNED_BYTE,
               img.GetData());

  std::stringstream filename;
  filename << "out/glx/"
           << test_info->test_case_name() << "." << test_info->name()
           << ".ppm";
  img.WritePPM(filename.str());

  // Return success always.
  return ::testing::AssertionSuccess();
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  ::testing::UnitTest* unit_test = ::testing::UnitTest::GetInstance();
  unit_test->listeners().Append(new EventListener);

  int result = RUN_ALL_TESTS();
  fprintf(stderr, "Finished running %d tests.", unit_test->total_test_count());
  return result;
}
